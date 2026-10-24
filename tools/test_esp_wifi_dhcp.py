#!/usr/bin/env python3
"""Compile the real virtual DHCP protocol and test independently built packets."""
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import random
import struct
import subprocess


class Reply(C.Structure):
    _fields_ = [('type', C.c_uint8), ('mac', C.c_uint8 * 6), ('xid', C.c_uint8 * 4),
                ('address', C.c_uint8 * 4), ('server', C.c_uint8 * 4)]


class Lease(C.Structure):
    _fields_ = [('phase', C.c_uint8), ('address', C.c_uint8 * 4), ('server', C.c_uint8 * 4), ('xid', C.c_uint8 * 4)]


MAC = bytes.fromhex('525400430002')
XID = bytes.fromhex('003d1d00')
ADDRESS = bytes([192, 168, 4, 2])
SERVER = bytes([192, 168, 4, 1])


def packet(kind=2, *, options=None, ihl=5, mac=MAC, xid=XID,
           address=ADDRESS, server=SERVER, overload=0):
    """Serialize RFC wire fields in Python, without using QEMU packet structs."""
    bootp = bytearray(240)
    bootp[:4] = bytes([2, 1, 6, 0]); bootp[4:8] = xid
    bootp[16:20] = address; bootp[28:34] = mac
    bootp[236:240] = bytes.fromhex('63825363')
    fields = b'\x00\x36\x04' + server + b'\x35\x01' + bytes([kind]) + b'\xff'
    if options is None:
        options = fields
        if overload:
            options = bytes([52, 1, overload, 255])
            if overload == 1:
                bootp[108:108+len(fields)] = fields
            elif overload == 2:
                bootp[44:44+len(fields)] = fields
            else:
                bootp[108:116] = b'\x36\x04' + server + b'\xff\x00'
                bootp[44:48] = bytes([53, 1, kind, 255])
    payload = bootp + options
    udp = struct.pack('!HHHH', 67, 68, 8 + len(payload), 0) + payload
    ip = bytearray(ihl * 4)
    ip[0] = 0x40 | ihl; ip[9] = 17
    struct.pack_into('!H', ip, 2, len(ip) + len(udp))
    return bytes.fromhex('aaaa030000000800') + ip + udp


def main():
    """Check bounded parsing, per-peer transaction isolation and ACK-only binding."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(); args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parent.parent
    files = [Path(__file__), root/'hw/net/esp32c3_wifi_dhcp.c',
             root/'hw/net/esp32c3_wifi_dhcp.h', Path(args.cc)]
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    pins = {str(p): sha(p) for p in files}
    dll = args.output.resolve()/('protocol.dll' if os.name == 'nt' else 'protocol.so')
    command = [args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-shared',
               '-g', '-o', str(dll), str(files[1])]
    if os.name != 'nt': command.insert(1, '-fPIC')
    build = subprocess.run(command, capture_output=True, text=True)
    (args.output/'compiler.log').write_text(build.stdout + build.stderr)
    build.check_returncode()
    lib = C.CDLL(str(dll)); parse = lib.esp32_wifi_dhcp_reply
    parse.argtypes = [C.c_void_p, C.c_size_t, C.POINTER(Reply)]; parse.restype = C.c_bool
    accept = lib.esp32_wifi_dhcp_accept
    accept.argtypes = [C.POINTER(Lease), C.POINTER(Reply), C.c_void_p]; accept.restype = C.c_int
    begin = lib.esp32_wifi_dhcp_begin
    begin.argtypes = [C.POINTER(Lease), C.c_void_p]; begin.restype = None
    backoff = lib.esp32_wifi_dhcp_backoff_ms
    backoff.argtypes = [C.POINTER(C.c_uint32), C.c_uint32]; backoff.restype = C.c_uint32
    rows = []

    def decode(data, valid):
        output = Reply(); C.memset(C.byref(output), 0xa5, C.sizeof(output))
        before = bytes(output); result = parse(data, len(data), C.byref(output))
        assert result == valid, (len(data), data.hex(), result)
        if not result: assert bytes(output) == before
        return output

    for ihl in (5, 6, 15):
        for overload in (0, 1, 2, 3):
            for kind in (2, 5, 6):
                data = packet(kind, ihl=ihl, overload=overload)
                reply = decode(data, True)
                assert (reply.type, bytes(reply.mac), bytes(reply.xid), bytes(reply.address),
                        bytes(reply.server)) == (kind, MAC, XID, ADDRESS, SERVER)
                rows.append(dict(case='valid-wire', ihl=ihl, overload=overload, type=kind,
                                 packet_sha256=hashlib.sha256(data).hexdigest()))
    malformed = []
    original = packet()
    for index in range(len(original)):
        malformed.append(('prefix-'+str(index), original[:index]))
    for name, offset, value in (
        ('not-snap', 0, 0), ('not-ipv4', 8, 0x65), ('short-ihl', 8, 0x44),
        ('tcp', 17, 6), ('fragment-offset', 15, 1), ('more-fragments', 14, 0x20),
        ('wrong-source-port', 29, 68), ('wrong-destination-port', 31, 67),
        ('request-not-reply', 36, 1), ('wrong-hardware', 37, 2), ('short-mac', 38, 5),
        ('bad-cookie', 272, 0), ('ip-length-low', 10, 0),
        ('ip-length-high', 10, 0xff), ('udp-length-high', 32, 0xff),
        ('udp-length-low', 32, 0),
    ):
        data = bytearray(original); data[offset] = value
        # Length-low mutations also change the low byte to an actually short size.
        if name == 'ip-length-low': data[11] = 20
        if name == 'udp-length-low': data[33] = 8
        malformed.append((name, bytes(data)))
    for name, options in (
        ('missing-type', b'\x36\x04'+SERVER+b'\xff'),
        ('missing-server', b'\x35\x01\x02\xff'),
        ('missing-end', b'\x35\x01\x02\x36\x04'+SERVER),
        ('missing-option-length', b'\x35'),
        ('short-option', b'\x35\x02\x02'),
        ('duplicate-type', b'\x35\x01\x02\x35\x01\x05\x36\x04'+SERVER+b'\xff'),
        ('duplicate-server', b'\x35\x01\x02'+2*(b'\x36\x04'+SERVER)+b'\xff'),
        ('invalid-overload', b'\x34\x01\x04\xff'),
        ('empty-type', b'\x35\x00\x36\x04'+SERVER+b'\xff'),
    ): malformed.append((name, packet(options=options)))
    for name, data in malformed:
        decode(data, False); rows.append(dict(case=name, rejected=True))

    def step(lease, data, wanted):
        reply = decode(data, True); before = bytes(lease)
        result = accept(C.byref(lease), C.byref(reply), MAC)
        assert result == wanted, (result, wanted)
        if not wanted: assert bytes(lease) == before
        return result

    # An ACK cannot establish a lease without selecting an offer first.
    lease = Lease(); begin(C.byref(lease), XID); step(lease, packet(5), 0)
    step(lease, packet(mac=bytes.fromhex('525400430003')), 0)
    step(lease, packet(xid=b'bad!'), 0)
    step(lease, packet(server=b'\0'*4), 0)
    step(lease, packet(address=b'\0'*4), 0)
    step(lease, packet(), 1)
    assert lease.phase == 1 and bytes(lease.address) == ADDRESS
    step(lease, packet(), 0)
    step(lease, packet(5, server=bytes([192,168,4,9])), 0)
    step(lease, packet(5, address=bytes([192,168,4,3])), 0)
    step(lease, packet(5, xid=b'late'), 0)
    step(lease, packet(5, mac=bytes.fromhex('525400430003')), 0)
    step(lease, packet(5), 2); assert lease.phase == 2
    step(lease, packet(5), 0)
    lease = Lease(); begin(C.byref(lease), XID); step(lease, packet(), 1)
    step(lease, packet(6, server=bytes([192,168,4,9])), 0)
    step(lease, packet(6, address=b'\0'*4), 3)
    assert bytes(lease) == b'\0'*C.sizeof(lease)
    begin(C.byref(lease), XID)
    step(lease, packet(), 1); step(lease, packet(5), 2)
    rows.append(dict(case='transaction-and-state-sequence', passed=True))
    # Different acquisitions must not accept a late packet with the old XID.
    other = Lease(); new_id = b'next'; begin(C.byref(other), new_id)
    step(other, packet(), 0); step(other, packet(xid=new_id), 1)
    step(other, packet(5, xid=XID), 0); step(other, packet(5, xid=new_id), 2)
    begin(C.byref(other), C.addressof(other)+Lease.xid.offset)
    assert other.phase == 0 and bytes(other.xid) == new_id
    assert not any(other.address) and not any(other.server)
    rows.append(dict(case='fresh-xid-and-alias-safe-begin', passed=True))
    jitter_checks = 0
    for previous, nominal in ((0,4),(4,8),(8,16),(16,32),(32,64),(64,64),(0xffffffff,64)):
        for jitter in range(2001):
            value = C.c_uint32(previous)
            delay = backoff(C.byref(value), jitter)
            assert value.value == nominal
            assert delay == min(64000, nominal*1000+jitter-1000)
            jitter_checks += 1
    rows.append(dict(case='exponential-backoff-and-jitter', combinations=jitter_checks, passed=True))


    # Put every truncated packet immediately before a no-access page. An
    # accidental one-byte read past the supplied length fails the process.
    guard_checks = 0
    if os.name == 'nt':
        kernel = C.WinDLL('kernel32', use_last_error=True)
        kernel.VirtualAlloc.argtypes = [C.c_void_p,C.c_size_t,C.c_ulong,C.c_ulong]
        kernel.VirtualAlloc.restype = C.c_void_p
        kernel.VirtualProtect.argtypes = [C.c_void_p,C.c_size_t,C.c_ulong,C.POINTER(C.c_ulong)]
        kernel.VirtualFree.argtypes = [C.c_void_p,C.c_size_t,C.c_ulong]
        allocation = kernel.VirtualAlloc(None, 8192, 0x3000, 4); assert allocation
        old = C.c_ulong(); assert kernel.VirtualProtect(allocation+4096,4096,1,C.byref(old))
        try:
            for ihl in (5,6,15):
                data = packet(ihl=ihl)
                for length in range(len(data)):
                    pointer=allocation+4096-length
                    C.memmove(pointer,data,length)
                    output=Reply(); assert not parse(pointer,length,C.byref(output))
                    guard_checks += 1
        finally: assert kernel.VirtualFree(allocation,0,0x8000)
    randomizer = random.Random(2131)
    for _ in range(10000):
        data = randomizer.randbytes(randomizer.randrange(700))
        output = Reply(); assert not parse(data,len(data),C.byref(output))
    assert {str(p):sha(p) for p in files} == pins
    report = dict(passed=True, inputs_sha256=pins, command=command,
        dll_sha256=sha(dll), cases=rows, guard_page_truncations=guard_checks,
        random_non_dhcp_packets=10000, guest_integration=False,
        rf_acceptance=False, lease_renewal_tested=False)
    (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(passed=True,cases=len(rows),guard_checks=guard_checks,
                         random_non_dhcp_packets=10000)))


if __name__ == '__main__': main()
