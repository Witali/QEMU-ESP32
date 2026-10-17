#!/usr/bin/env python3
"""Test profile-configured DNS and NTP packets through real Wi-Fi DMA, without firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from small_tv_vm import QMP

WIFI = 0x60033000
TX_DESC, TX_DATA, RX_DESC, RX_DATA = 0x3fc80100, 0x3fc80400, 0x3fc81000, 0x3fc82000
STATION, AP = bytes.fromhex('020000000001'), bytes.fromhex('100100c40a56')
LLC = bytes.fromhex('aaaa030000000800')
OPTIONS = None


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


class Machine:
    def __init__(self, properties=(), c2=False, trace=False):
        shift = 0x20000 if c2 else 0
        self.wifi = 0x60047000 if c2 else WIFI
        self.tx_desc, self.tx_data = TX_DESC + shift, TX_DATA + shift
        self.rx_desc, self.rx_data = RX_DESC + shift, RX_DATA + shift
        self.tx_register, self.rx_register = (0xce8, 0x68) if c2 else (0xd08, 0x88)
        self.temp = tempfile.TemporaryDirectory(prefix='esp-wifi-services-', dir=OPTIONS.temp_root)
        self.folder = Path(self.temp.name)
        self.qmp = self.sock = self.stream = self.proc = None
        self.log = (self.folder / 'qemu.log').open('wb')
        flash = self.folder / 'flash.bin'
        flash.write_bytes(b'\xff' * (4 * 1024 * 1024))
        qmp_port, test_port = free_port(), free_port()
        while test_port == qmp_port:
            test_port = free_port()
        if c2:
            # QTest executes no CPU/ROM instructions. A size-correct placeholder
            # avoids requiring a proprietary ROM for register/packet tests.
            (self.folder / 'esp32c2-rev200-rom.bin').write_bytes(bytes(0x90000))
        command = [OPTIONS.qemu, '-M', 'esp32c2,default-peripherals=off' if c2 else 'esp32c3', '-accel', 'qtest',
                   '-display', 'none', '-serial', 'none', '-monitor', 'none',
                   '-nic', 'none', '-L', str(self.folder) if c2 else OPTIONS.bios,
                   '-drive', f'file={flash},if=mtd,format=raw', '-snapshot',
                   '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
                   '-qtest', f'tcp:127.0.0.1:{test_port},server=on,wait=off',
                   '-qtest-log', str(self.folder / 'qtest.log')]
        if trace:
            command += ['-trace', 'enable=esp32c3_wifi_packet_stage',
                        '-trace', 'enable=esp32c3_wifi_rx_state']
        for prop in ('latency-ms=0', 'jitter-ms=0', 'bandwidth-bps=0', *properties):
            command += ['-global', 'esp32c3-wifi.' + prop]
        try:
            self.proc = subprocess.Popen(command, stdout=self.log, stderr=self.log,
                creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
            deadline = time.monotonic() + 15
            while True:
                try:
                    self.sock = socket.create_connection(('127.0.0.1', test_port), .2)
                    break
                except OSError:
                    if self.proc.poll() is not None or time.monotonic() > deadline:
                        raise RuntimeError((self.folder / 'qemu.log').read_text(errors='replace'))
                    time.sleep(.02)
            self.sock.settimeout(5)
            self.stream = self.sock.makefile('rwb', buffering=0)
            self.qmp = QMP(qmp_port)
        except Exception as error:
            self.log.flush()
            details = (self.folder / 'qemu.log').read_text(errors='replace')
            self.close()
            raise RuntimeError(details or str(error)) from error
        self.seen = 0

    def close(self):
        if self.qmp:
            self.qmp.close()
        if self.stream:
            self.stream.close()
        if self.sock:
            self.sock.close()
        if self.proc:
            if self.proc.poll() is None:
                self.proc.terminate()
            self.proc.wait(timeout=5)
        self.log.close()
        # Windows can retain file locks briefly after a failed startup. Only
        # this test's verified temporary child may be recursively removed.
        if self.folder.resolve().parent != OPTIONS.temp_root.resolve():
            raise ValueError('Temporary test directory escaped its workspace')
        for attempt in range(20):
            try:
                self.temp.cleanup()
                break
            except PermissionError:
                if attempt == 19:
                    raise
                time.sleep(.05)

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()

    def command(self, command):
        self.stream.write((command + '\n').encode())
        while True:
            response = self.stream.readline().decode().strip()
            if response.startswith('IRQ '):
                continue
            if not response.startswith('OK'):
                raise RuntimeError((command, response))
            return response.split()[1:]

    def write(self, address, data):
        self.command(f'write {address:#x} {len(data)} 0x{data.hex()}')

    def read(self, address, size):
        return bytes.fromhex(self.command(f'read {address:#x} {size}')[0][2:])

    def set(self, name, value):
        self.qmp.execute('qom-set', path='/machine/wifi', property=name, value=value)

    def get(self, name):
        return self.qmp.execute('qom-get', path='/machine/wifi', property=name)

    def send(self, control, data):
        frame = struct.pack('<HH', control, 0) + AP + STATION + AP + b'\0\0' + data + bytes(4)
        self.write(self.tx_data, frame)
        self.write(self.tx_desc, struct.pack('<III', len(frame) | (len(frame) << 12) | (3 << 30), self.tx_data, 0))
        self.command(f'writel {self.wifi + self.tx_register:#x} {0xc0000000 | (self.tx_desc & 0xfffff):#x}')
        self.command('clock_step 1000000')

    def associate(self):
        self.seen = 0
        for i in range(32):
            next_desc = self.rx_desc + (i + 1) * 12 if i < 31 else 0
            self.write(self.rx_desc + i * 12,
                       struct.pack('<III', 2048 | (1 << 31), self.rx_data + i * 2048, next_desc))
        self.command(f'writel {self.wifi + self.rx_register:#x} {self.rx_desc:#x}')
        self.send(0xb0, struct.pack('<HHH', 0, 1, 0))
        self.send(0, struct.pack('<HH', 0, 1))
        self.replies()

    def frames(self):
        """Read completed DMA buffers, including management frames with metadata."""
        packets = []
        while self.seen < 32:
            flags = struct.unpack('<I', self.read(self.rx_desc + self.seen * 12, 4))[0]
            length = (flags >> 12) & 0xfff
            if not length:
                break
            packets.append(self.read(self.rx_data + self.seen * 2048, length))
            self.seen += 1
        return packets

    def replies(self):
        packets = []
        for packet in self.frames():
            offset = packet.find(LLC)
            if offset >= 0:
                packets.append(packet[offset + len(LLC):])
        return packets

    def udp(self, port, payload, suffix=3, prefix=(10, 0, 2)):
        source, target = bytes([*prefix, 15]), bytes([*prefix, suffix])
        ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 28 + len(payload), 0, 0, 64, 17, 0, source, target)
        udp = struct.pack('!HHHH', 12345, port, 8 + len(payload), 0)
        self.send(0x108, LLC + ip + udp + payload)
        return [packet[28:28 + struct.unpack_from('!H', packet, 24)[0] - 8]
                for packet in self.replies() if packet[9] == 17 and
                struct.unpack_from('!HH', packet, 20) == (port, 12345)]

    def dns(self, host, kind=1, prefix=(10, 0, 2)):
        labels = b''.join(bytes([len(label)]) + label.encode('ascii') for label in host.split('.'))
        question = struct.pack('!6H', 0x1234, 0x100, 1, 0, 0, 0) + labels + b'\0' + struct.pack('!HH', kind, 1)
        replies = self.udp(53, question, prefix=prefix)
        if len(replies) != 1:
            raise AssertionError('Expected one real DNS response, got ' + str(len(replies)))
        return replies[0]


class WifiServicesTests(unittest.TestCase):
    def test_readonly_ntp_reply_timestamp_without_logging(self):
        """Generated wire epoch/counter remain observable with DEBUG compiled out."""
        for c2 in (False, True):
            with self.subTest(c2=c2), Machine(('clock-services=on',), c2=c2) as machine:
                snapshot = lambda: json.loads(machine.get('last-ntp-reply'))
                initial = snapshot()
                self.assertEqual(initial, {'count': 0, 'unix_seconds': 0, 'virtual_us': 0})
                with self.assertRaises(RuntimeError):
                    machine.set('last-ntp-reply', '{}')
                machine.associate()
                machine.set('ntp-unix-seconds', 1781265600)
                request = bytearray(48)
                request[0] = 0x23
                replies = machine.udp(123, request, 2)
                self.assertEqual(len(replies), 1)
                first = snapshot()
                self.assertEqual(first['count'], 1)
                self.assertEqual(first['unix_seconds'], struct.unpack_from('!I', replies[0], 40)[0] - 2208988800)
                self.assertGreater(first['virtual_us'], 0)
                now_ns = int(machine.command('clock_step 0')[0], 0)
                self.assertLessEqual(first['virtual_us'], now_ns // 1000)
                machine.set('ntp-drop-mask', 1)
                self.assertEqual(machine.udp(123, request, 2), [])
                self.assertEqual(snapshot(), first)
                machine.set('ntp-drop-mask', 0)
                self.assertEqual(machine.udp(123, b'\x23', 2), [])
                self.assertEqual(snapshot(), first)
                machine.set('ntp-unix-seconds', 1781265660)
                self.assertEqual(len(machine.udp(123, request, 2)), 1)
                second = snapshot()
                self.assertEqual(second['count'], 2)
                self.assertEqual(second['unix_seconds'], 1781265660)
                self.assertGreater(second['virtual_us'], first['virtual_us'])
                machine.qmp.execute('system_reset')
                self.assertEqual(snapshot(), second)

    def test_compile_time_debug_logging(self):
        """The same DMA traffic works with diagnostic code compiled in or out."""
        if OPTIONS.expect_debug is None:
            self.skipTest('Pass --expect-debug on/off to check the selected build')
        enabled = OPTIONS.expect_debug == 'on'
        with Machine(('clock-services=on', 'ntp-hosts=time.fixture.test'), c2=True, trace=True) as machine:
            self.assertIs(machine.get('debug-logging'), enabled)
            with self.assertRaises(RuntimeError):
                machine.set('debug-logging', not enabled)
            machine.associate()
            self.assertTrue(machine.dns('time.fixture.test'))
            request = bytearray(48)
            request[0] = 0x23
            self.assertEqual(len(machine.udp(123, request, 2)), 1)
            machine.log.flush()
            text = (machine.folder / 'qemu.log').read_text(errors='replace')
            for marker in ('C2 NTP reply', 'C2 DNS answer service=ntp',
                           'esp32c3_wifi_packet_stage', 'esp32c3_wifi_rx_state'):
                self.assertEqual(marker in text, enabled, marker)

    def test_c2_backlog_preserves_finite_receive_bandwidth(self):
        """DMA rearming cannot collapse queued packets into a zero-time burst."""
        with Machine(('clock-services=on', 'bandwidth-bps=8000000'), c2=True) as machine:
            machine.associate()
            machine.command(f'writel {machine.wifi + machine.rx_register:#x} 0')
            machine.seen = 0
            for i in range(32):
                next_desc = machine.rx_desc + (i + 1) * 12 if i < 31 else 0
                machine.write(machine.rx_desc + i * 12,
                    struct.pack('<III', 2048 | (1 << 31), machine.rx_data + i * 2048, next_desc))
            for nonce in (1, 2):
                request = bytearray(48)
                request[0] = 0x23
                request[40:48] = bytes([nonce]) * 8
                self.assertEqual(machine.udp(123, request, 2), [])
            machine.command('clock_step 5000000000')
            machine.command(f'writel {machine.wifi + machine.rx_register:#x} {machine.rx_desc:#x}')
            machine.command('clock_step 1')
            self.assertEqual(machine.replies(), [])
            # 24-byte MAC + 8-byte LLC + 20-byte IPv4 + 8-byte UDP + 48-byte NTP
            # + 4-byte FCS = 112 bytes, or 112 us at the configured 8 Mbit/s.
            machine.command('clock_step 112000')
            first = machine.replies()
            self.assertEqual(len(first), 1)
            self.assertEqual(first[0][52:60], bytes([1]) * 8)
            machine.command('clock_step 112000')
            second = machine.replies()
            self.assertEqual(len(second), 1)
            self.assertEqual(second[0][52:60], bytes([2]) * 8)
            machine.command('clock_step 1000000')
            self.assertEqual(machine.replies(), [])

    def test_data_frames_have_initialized_signal_strength(self):
        """Queued frames cannot inherit RSSI from a previously freed allocation."""
        for c2 in (False, True):
            with self.subTest(c2=c2), Machine(('clock-services=on', 'latency-ms=100'), c2=c2) as machine:
                machine.associate()
                # Keep eight allocations live together: immediate delivery can
                # reuse the valid RSSI left by an earlier management frame.
                machine.command('clock_step 200000000')
                machine.frames()  # Finish the delayed authentication/association replies.
                request = bytearray(48)
                request[0] = 0x23
                for nonce in range(8):
                    request[40:48] = bytes([nonce]) * 8
                    self.assertEqual(machine.udp(123, request, 2), [])
                machine.command('clock_step 200000000')
                packets = [packet for packet in machine.frames() if LLC in packet]
                self.assertEqual(len(packets), 8)
                for packet in packets:
                    raw = packet[0]
                    # C2 stores signed dBm; the older C3 metadata has a 96 bias.
                    rssi = struct.unpack('b', bytes([raw]))[0] if c2 else raw - 96
                    self.assertGreaterEqual(rssi, -25)
                    self.assertLessEqual(rssi, -16)

    def test_station_ap_shutdown_and_recovery(self):
        for c2 in (False, True):
            with self.subTest(c2=c2), Machine(('clock-services=on',), c2=c2) as machine:
                request = bytearray(48)
                request[0] = 0x23
                machine.associate()
                self.assertEqual(len(machine.udp(123, request, 2)), 1)
                machine.set('station-ap-online', False)
                machine.command('clock_step 1000000')
                packets = machine.frames()
                self.assertEqual(len(packets), 1)
                # Metadata precedes the complete 24-byte header, reason and FCS.
                frame = packets[0][-30:]
                self.assertEqual(frame[:2], b'\xc0\x00')
                self.assertEqual(frame[4:10], STATION)
                self.assertEqual(frame[10:16], AP)
                self.assertEqual(frame[16:22], AP)
                self.assertEqual(frame[24:26], b'\x03\x00')
                machine.set('station-ap-online', False)
                machine.command('clock_step 1000000')
                self.assertEqual(machine.frames(), [])
                self.assertEqual(machine.udp(123, request, 2), [])
                machine.set('station-ap-online', True)
                machine.associate()
                self.assertEqual(len(machine.udp(123, request, 2)), 1)
                machine.set('station-ap-online', False)
                machine.qmp.execute('system_reset')
                self.assertFalse(machine.get('station-ap-online'))
                machine.set('station-ap-online', True)
                machine.associate()
                self.assertEqual(len(machine.udp(123, request, 2)), 1)

    def test_c2_receive_waits_for_dma_and_keeps_bounded_fifo(self):
        with Machine(('clock-services=on', 'queue-limit=2'), c2=True) as machine:
            machine.associate()
            # The SDK exhausted its current chain. Prepare fresh buffers in RAM
            # but do not arm the MAC until after three incoming replies.
            machine.command(f'writel {machine.wifi + machine.rx_register:#x} 0')
            machine.seen = 0
            for i in range(32):
                next_desc = machine.rx_desc + (i + 1) * 12 if i < 31 else 0
                machine.write(machine.rx_desc + i * 12,
                    struct.pack('<III', 2048 | (1 << 31), machine.rx_data + i * 2048, next_desc))
            for nonce in (1, 2, 3):
                request = bytearray(48)
                request[0] = 0x23
                request[40:48] = bytes([nonce]) * 8
                self.assertEqual(machine.udp(123, request, 2), [])
            # A long starvation interval must not drain or reorder the queue.
            machine.command('clock_step 5000000000')
            self.assertEqual(machine.replies(), [])
            machine.command(f'writel {machine.wifi + machine.rx_register:#x} {machine.rx_desc:#x}')
            machine.command('clock_step 1000000')
            replies = machine.replies()
            self.assertEqual(len(replies), 2)
            self.assertEqual([reply[28 + 24:28 + 32] for reply in replies],
                             [bytes([1]) * 8, bytes([2]) * 8])
            machine.command('clock_step 1000000000')
            self.assertEqual(machine.replies(), [])

    def test_ntp_controls_and_reset(self):
        with Machine() as machine:
            defaults = {'clock-services': False, 'weather-services': False, 'station-ap-online': True,
                        'service-hosts': '', 'ntp-hosts': '', 'station-ssid': 'QEMU', 'ntp-unix-seconds': -1,
                        'ntp-drop-mask': 0, 'ntp-separate-servers': False}
            for name, value in defaults.items():
                self.assertEqual(machine.get(name), value)
            for name, value in [('ntp-unix-seconds', -2), ('ntp-unix-seconds', 1 << 63),
                                ('ntp-drop-mask', -1), ('ntp-drop-mask', 8),
                                ('ntp-separate-servers', 'invalid'), ('station-ap-online', 'invalid')]:
                with self.subTest(name=name, value=value), self.assertRaises(RuntimeError):
                    machine.set(name, value)
                self.assertEqual(machine.get(name), defaults[name])
            for epoch in (-1, 0, 2147483648, (1 << 63) - 1):
                machine.set('ntp-unix-seconds', epoch)
                self.assertEqual(machine.get('ntp-unix-seconds'), epoch)
            for mask in range(8):
                machine.set('ntp-drop-mask', mask)
                self.assertEqual(machine.get('ntp-drop-mask'), mask)
            machine.set('ntp-separate-servers', True)
            machine.qmp.execute('system_reset')
            self.assertEqual(machine.get('ntp-unix-seconds'), (1 << 63) - 1)
            self.assertEqual(machine.get('ntp-drop-mask'), 7)
            self.assertIs(machine.get('ntp-separate-servers'), True)

    def test_configured_dns_and_unknown_names(self):
        with Machine(('clock-services=on', 'service-hosts=first.service.test;second.service.test')) as machine:
            machine.associate()
            reply = machine.dns('FIRST.SERVICE.TEST')
            self.assertEqual(reply[:2], b'\x12\x34')
            self.assertEqual(struct.unpack_from('!HH', reply, 2), (0x8180, 1))
            self.assertEqual(struct.unpack_from('!H', reply, 6)[0], 1)
            self.assertEqual(reply[-4:], bytes([10, 0, 2, 100]))
            self.assertEqual(machine.dns('second.service.test')[-4:], bytes([10, 0, 2, 101]))
            self.assertEqual(machine.dns('first.service.test', prefix=(192, 168, 43))[-4:],
                             bytes([192, 168, 43, 100]))
            for host in ('unconfigured.service.test', 'unknown.test'):
                reply = machine.dns(host)
                self.assertEqual(struct.unpack_from('!H', reply, 2)[0] & 15, 3)
                self.assertEqual(struct.unpack_from('!H', reply, 6)[0], 0)
            reply = machine.dns('first.service.test', 28)
            self.assertEqual(struct.unpack_from('!H', reply, 6)[0], 0)

    def test_empty_services_and_profile_ntp_dns(self):
        with Machine(('clock-services=on', 'ntp-hosts=first.ntp.test;second.ntp.test;third.ntp.test')) as machine:
            machine.associate()
            reply = machine.dns('unconfigured.service.test')
            self.assertEqual(struct.unpack_from('!H', reply, 2)[0] & 15, 3)
            for host in ('first.ntp.test', 'second.ntp.test', 'third.ntp.test'):
                self.assertEqual(machine.dns(host)[-4:], bytes([10, 0, 2, 2]))
            machine.set('ntp-separate-servers', True)
            for host, suffix in [('first.ntp.test', 2), ('second.ntp.test', 4), ('third.ntp.test', 5)]:
                self.assertEqual(machine.dns(host)[-4:], bytes([10, 0, 2, suffix]))

    def test_ntp_wire_epoch_failure_masks_and_recovery(self):
        with Machine(('clock-services=on',)) as machine:
            machine.associate()
            machine.set('ntp-separate-servers', True)
            request = bytearray(48)
            request[0], request[2] = 0x23, 6
            request[40:48] = b'NONCE123'
            before = int(time.time())
            reply, = machine.udp(123, request, 2)
            seconds = struct.unpack_from('!I', reply, 40)[0]
            self.assertIn(seconds, [(value + 2208988800) & 0xffffffff
                                    for value in range(before, int(time.time()) + 1)])
            for epoch in (0, 1709179420, 2147483648, (1 << 63) - 1):
                machine.set('ntp-unix-seconds', epoch)
                reply, = machine.udp(123, request, 2)
                self.assertEqual(len(reply), 48)
                self.assertEqual(reply[0], 0x24)
                self.assertEqual(reply[24:32], request[40:48])
                for offset in (16, 32, 40):
                    self.assertEqual(struct.unpack_from('!II', reply, offset),
                                     ((epoch + 2208988800) & 0xffffffff, 0))
            for mask in range(8):
                machine.set('ntp-drop-mask', mask)
                for index, suffix in enumerate((2, 4, 5)):
                    self.assertEqual(len(machine.udp(123, request, suffix)), 0 if mask & (1 << index) else 1)
            machine.set('ntp-drop-mask', 0)
            self.assertEqual(len(machine.udp(123, request, 5)), 1)
            self.assertEqual(machine.udp(123, b'\x23', 2), [])

    def test_invalid_service_hosts_fail_startup(self):
        for host in ('http://invalid.test', '-bad.test', 'bad-.test', 'a..test',
                     'a' * 64 + '.test', 'first.test;FIRST.TEST'):
            with self.subTest(host=host), self.assertRaisesRegex(RuntimeError, 'service-hosts must'):
                with Machine(('service-hosts=' + host,)):
                    pass

    def test_disabled_clock_services_are_inert(self):
        with Machine(('service-hosts=first.service.test',)) as machine:
            machine.associate()
            request = bytearray(48)
            request[0] = 0x23
            self.assertEqual(machine.udp(123, request, 2), [])
            self.assertEqual(machine.udp(53, b'\0' * 20), [])


def main():
    global OPTIONS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--expect-debug', choices=('on', 'off'))
    parser.add_argument('--bios', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--temp-root', type=Path, default=Path(__file__).resolve().parents[1] / '.cache/esp-boards/wifi-tests')
    OPTIONS = parser.parse_args()
    OPTIONS.temp_root.mkdir(parents=True, exist_ok=True)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(WifiServicesTests))
    report = {'passed': result.wasSuccessful(), 'tests': result.testsRun,
              'failures': [str(case) for case, _ in result.failures],
              'errors': [str(case) for case, _ in result.errors],
              'qemu_sha256': hashlib.sha256(Path(OPTIONS.qemu).read_bytes()).hexdigest(),
              'expected_debug_logging': OPTIONS.expect_debug,
              'scope': 'QOM validation/reset and real DNS/NTP packets through Wi-Fi DMA; qtest executes no guest instructions, no firmware or Internet.'}
    OPTIONS.output.parent.mkdir(parents=True, exist_ok=True)
    OPTIONS.output.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
