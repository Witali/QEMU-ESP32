#!/usr/bin/env python3
"""ESP32-C3 SPI1 Flash register tests; TCP QTest, no firmware required."""
import argparse
import json
import os
import sys
from pathlib import Path
import socket
import subprocess
import tempfile
import time

SPI = 0x60002000
CMD, ADDR, CTRL, USER, USER1, USER2 = 0, 4, 8, 0x18, 0x1c, 0x20
MOSI_LEN, MISO_LEN, STATUS, W0 = 0x24, 0x28, 0x2c, 0x58
USR, FLASH_READ, WREN, WRSR = 1 << 18, 1 << 31, 1 << 30, 1 << 26
COMMAND, ADDRESS, DUMMY, MISO, MOSI = [1 << n for n in (31, 30, 29, 28, 27)]
MODES = {  # opcode, CTRL flags, dummy clocks (including mode byte for I/O)
    'slow': (0x03, 0, 0),
    'fast': (0x0b, 1 << 13, 8),
    'dout': (0x3b, (1 << 13) | (1 << 14), 8),
    'qout': (0x6b, (1 << 13) | (1 << 20), 8),
    'dio': (0xbb, (1 << 13) | (1 << 23), 4),
    'qio': (0xeb, (1 << 13) | (1 << 24), 6),
}


class Machine:
    def __init__(self, args, directory, family, mib, explicit=True):
        self.image = directory / 'flash.bin'
        self.size = mib * 1024 * 1024
        self.pattern = bytes((i * 37 + 11) % 256 for i in range(64))
        self.contents = {addr: bytes(v ^ salt for v in self.pattern)
                         for addr, salt in [(0, 0x17), (0x12340, 0x5b), (self.size-64, 0xc3)]}
        with self.image.open('wb') as f:
            for _ in range(mib):
                f.write(b'\xff' * (1024 * 1024))
        with self.image.open('r+b') as f:
            for addr in (0, 0x12340, self.size - 64):
                f.seek(addr)
                f.write(self.contents[addr])
        machine = 'esp32c3' if family == 'legacy' else f'esp32c3,flash-model={family}'
        if explicit:
            machine += f',flash-size={mib}M'
        with socket.socket() as listener:
            listener.bind(('127.0.0.1',0))
            port = listener.getsockname()[1]
        self.log = (directory / 'qemu.log').open('w')
        self.proc = subprocess.Popen([
            args.qemu, '-M', machine, '-S', '-display', 'none', '-serial', 'none',
            '-monitor', 'none', '-L', args.bios, '-snapshot',
            '-drive', f'file={self.image},if=mtd,format=raw',
            '-qtest', f'tcp:127.0.0.1:{port},server=on,wait=off', '-qtest-log', os.devnull,
        ], stdout=self.log, stderr=self.log,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform=='win32' else 0)
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.settimeout(5)
        deadline = time.monotonic() + 15
        while True:
            try:
                self.socket.connect(('127.0.0.1',port))
                break
            except (TimeoutError, ConnectionRefusedError):
                if self.proc.poll() is not None or time.monotonic() > deadline:
                    self.close()
                    raise RuntimeError((directory / 'qemu.log').read_text())
                time.sleep(.02)
        self.stream = self.socket.makefile('rwb', buffering=0)

    def close(self):
        if hasattr(self, 'stream'):
            self.stream.close()
        self.socket.close()
        if self.proc.poll() is None:
            self.proc.terminate()
        self.proc.wait(timeout=5)
        self.log.close()

    def command(self, text):
        self.stream.write((text + '\n').encode())
        while True:
            reply = self.stream.readline().decode().strip()
            if reply.startswith('IRQ '):
                continue
            assert reply.startswith('OK'), (text, reply)
            return reply.split()[1:]

    def write(self, reg, value):
        self.command(f'writel {SPI + reg:#x} {value:#x}')

    def read(self, reg):
        return int(self.command(f'readl {SPI + reg:#x}')[0], 0)

    def transfer(self, opcode, tx=b'', rx=0, addr=None, mode='slow', special=False):
        _, flags, dummy = MODES[mode]
        self.write(CTRL, flags)
        user = COMMAND | (MISO if rx else 0) | (MOSI if tx else 0)
        if addr is not None:
            user |= ADDRESS | (DUMMY if dummy else 0)
            # C3 spimem_flash_ll_set_usr_address writes the address unshifted.
            self.write(ADDR, addr)
        self.write(USER, user)
        self.write(USER1, (23 << 26) | max(dummy - 1, 0))
        self.write(USER2, (7 << 28) | opcode)
        self.write(MISO_LEN, max(rx * 8 - 1, 0))
        self.write(MOSI_LEN, max(len(tx) * 8 - 1, 0))
        padded = tx.ljust(64, b'\x00')
        for i in range(0, 64, 4):
            self.write(W0 + i, int.from_bytes(padded[i:i+4], 'little'))
        self.write(CMD, FLASH_READ if special else USR)
        return b''.join(self.read(W0+i).to_bytes(4, 'little') for i in range(0, rx, 4))[:rx]

    def run(self, expected_id, modes):
        assert self.transfer(0x9f, rx=3) == bytes.fromhex(expected_id), 'JEDEC ID'
        count = 1
        assert self.transfer(0x9f, tx=b'\xff', rx=3) == bytes.fromhex(expected_id), 'unequal duplex'
        count += 1
        if modes:
            assert self.transfer(0x35, rx=1)[0] & 2 == 0, 'initial QE'
            assert self.transfer(0xeb, rx=64, addr=0, mode='qio') == b'\xff' * 64, 'QIO requires QE'
            assert self.transfer(0x6b, rx=64, addr=0, mode='qout') == b'\xff' * 64, 'QOUT requires QE'
            self.transfer(0x31, tx=b'\x02')
            assert self.transfer(0x35, rx=1)[0] & 2 == 0, 'WEL required'
            self.transfer(0x06)
            assert self.transfer(0x05, rx=1)[0] & 2, 'WREN'
            self.transfer(0x31, tx=b'\x02')
            assert self.transfer(0x35, rx=1)[0] & 2, 'QE enable'
            assert self.transfer(0x05, rx=1)[0] & 2 == 0, 'WEL cleared'
            count += 7
        for addr in (0, 0x12340, self.size - 64):
            for mode in MODES if modes else ('slow',):
                for special in (False, True):
                    actual = self.transfer(MODES[mode][0], rx=64, addr=addr,
                                           mode=mode, special=special)
                    assert actual == self.contents[addr], (mode, addr, special, actual.hex())
                    count += 1
        assert self.transfer(0x03, rx=64, addr=self.size-16) == (
            self.contents[self.size-64][-16:] + self.contents[0][:48]), 'capacity wrap'
        count += 1
        # Page program and sector erase near the end, protecting the next sector.
        addr = self.size - 8192
        self.transfer(0x06)
        self.transfer(0x02, tx=self.pattern, addr=addr)
        assert self.transfer(0x03, rx=64, addr=addr) == self.pattern, 'program'
        self.transfer(0x06)
        self.transfer(0x20, addr=addr)
        assert self.transfer(0x03, rx=64, addr=addr) == b'\xff' * 64, 'erase'
        assert self.transfer(0x03, rx=64, addr=self.size-64) == self.contents[self.size-64], 'erase boundary'
        assert self.transfer(0x03, rx=64, addr=0) == self.contents[0], 'no address alias'
        count += 4
        if modes:
            # Special two-byte status write must update QE and clear WEL.
            self.write(CMD, WREN)
            self.write(CTRL, 1 << 22)
            self.write(STATUS, 0)
            self.write(CMD, WRSR)
            assert self.transfer(0x35, rx=1)[0] & 2 == 0, 'WRSR_2B clear QE'
            self.write(CMD, WREN)
            self.write(CTRL, 1 << 22)
            self.write(STATUS, 0x200)
            self.write(CMD, WRSR)
            assert self.transfer(0x35, rx=1)[0] & 2, 'WRSR_2B set QE'
            assert self.transfer(0x05, rx=1)[0] & 2 == 0, 'WRSR_2B clear WEL'
            count += 3
        return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--bios', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--temp-root', type=Path, help='Owned temporary test directory')
    parser.add_argument('--baseline', action='store_true', help='Only old default 4 MiB: expose QE failure')
    args = parser.parse_args()
    report = []
    if args.temp_root:
        args.temp_root.mkdir(parents=True,exist_ok=True)
    families = ('legacy',) if args.baseline else ('gd25q', 'w25q', 'legacy')
    for family in families:
        for mib in (4,) if args.baseline else (2, 4, 8, 16):
            expected = f'{"c840" if family == "gd25q" else "ef40"}{20 + mib.bit_length()-1:02x}'
            if family == 'legacy':
                expected = {2:'ef3015', 4:'c84016', 8:'c84017', 16:'9d6018'}[mib]
            with tempfile.TemporaryDirectory(prefix='c3-flash-',dir=args.temp_root) as temp:
                machine = Machine(args, Path(temp), family, mib, explicit=not args.baseline and family != 'legacy')
                try:
                    checks = machine.run(expected, modes=family != 'legacy' or args.baseline)
                    row = dict(family=family, mib=mib, jedec=expected, checks=checks, passed=True)
                except Exception as exc:
                    row = dict(family=family, mib=mib, passed=False, error=str(exc))
                finally:
                    machine.close()
                report.append(row)
                print(json.dumps(row), flush=True)
    if not args.baseline:
        with tempfile.TemporaryDirectory(prefix='c3-invalid-',dir=args.temp_root) as temp:
            image = Path(temp) / 'flash.bin'
            image.write_bytes(b'\xff' * (4 * 1024 * 1024))
            invalid = [
                ('flash-model=unknown', True, 'flash-model must be'),
                ('flash-size=3M', True, 'flash-size must be'),
                ('flash-model=w25q,flash-size=8M', True, 'MTD image size must match'),
                ('flash-model=w25q', False, 'require an MTD drive'),
                ('flash-size=4M', False, 'require an MTD drive'),
            ]
            for options, drive, message in invalid:
                cmd = [args.qemu, '-M', 'esp32c3,' + options, '-S',
                       '-display', 'none', '-serial', 'none', '-monitor', 'none',
                       '-L', args.bios]
                if drive:
                    cmd += ['-drive', f'file={image},if=mtd,format=raw']
                proc = subprocess.run(cmd, capture_output=True, text=True, timeout=15)
                row = dict(invalid=options, drive=drive,
                           passed=proc.returncode != 0 and message in proc.stderr)
                report.append(row)
                print(json.dumps(row), flush=True)
    Path(args.output).write_text(json.dumps(report, indent=2) + '\n')
    return 0 if all(row['passed'] for row in report) else 1


if __name__ == '__main__':
    raise SystemExit(main())
