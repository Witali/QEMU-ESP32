#!/usr/bin/env python3
"""Exercise interleaved guest STA and SoftAP handshakes through real Wi-Fi DMA."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import time
import unittest

import esp32_wifi_services_test as services

GUEST_AP = bytes.fromhex('020000000002')
CLIENT = bytes.fromhex('525400430002')
BROADCAST = bytes([255]) * 6
SSID = b'Small_TV'


class Machine(services.Machine):
    def complete_softap(self):
        """Run the AP client's actual probe/auth/association and DHCP exchange."""
        self.beacon()
        self.management(5, bytes(8) + struct.pack('<HH', 100, 1) + bytes([0, len(SSID)]) + SSID)
        self.management(11, struct.pack('<HHH', 0, 2, 0))
        self.management(1, struct.pack('<HHH', 1, 0, 0xc001))
        bootp = bytearray(240)
        bootp[:3] = bytes([2, 1, 6])
        bootp[16:20] = bytes([192, 168, 43, 2])
        bootp[28:34] = CLIENT
        bootp[236:240] = bytes.fromhex('63825363')
        payload = bytes(bootp) + bytes.fromhex('350102ff')
        ip = struct.pack('!BBHHHBBH4s4s', 0x45, 0, 28 + len(payload), 0, 0, 64, 17, 0,
                         bytes([192, 168, 43, 1]), BROADCAST[:4])
        udp = struct.pack('!HHHH', 67, 68, 8 + len(payload), 0)
        self.send_frame(0x208, GUEST_AP, CLIENT, GUEST_AP, services.LLC + ip + udp + payload)
        self.frames()

    def send_frame(self, control, source, destination, bssid, payload):
        """Transmit a complete guest frame via its unmodified DMA interface."""
        frame = struct.pack('<HH', control, 0) + destination + source + bssid + b'\0\0' + payload + bytes(4)
        self.write(self.tx_data, frame)
        self.write(self.tx_desc, struct.pack('<III', len(frame) | (len(frame) << 12) | (3 << 30), self.tx_data, 0))
        self.command(f'writel {self.wifi + self.tx_register:#x} {0xc0000000 | (self.tx_desc & 0xfffff):#x}')
        self.command('clock_step 1000000')

    def management(self, subtype, payload):
        self.send_frame(subtype << 4, GUEST_AP,
                        BROADCAST if subtype == 8 else CLIENT, GUEST_AP, payload)
        # QTest packets include a chip-specific RX metadata prefix. Locate
        # management frames by all three MACs, rather than guessing that size.
        frames = []
        for packet in self.frames():
            marker = GUEST_AP + CLIENT
            start = packet.find(marker)
            if start >= 4:
                frames.append(packet[start - 4:])
        return frames

    def beacon(self):
        return self.management(8, bytes(8) + struct.pack('<HH', 100, 1) + bytes([0, len(SSID)]) + SSID)


class ApstaTests(unittest.TestCase):
    def test_unselected_or_malformed_beacon_does_not_steal_station(self):
        """Only the configured SSID may start a virtual client on the other role."""
        for c2 in (False, True):
            with self.subTest(c2=c2), Machine(('clock-services=on', 'ntp-hosts=pool.ntp.org',
                                             'softap-client-ssid=Small_TV'), c2=c2) as machine:
                machine.associate()
                for tag in (b'\x00\x05Other', b'\x00\x20tiny'):
                    self.assertEqual(machine.management(8, bytes(8) + struct.pack('<HH', 100, 1) + tag), [])
                    self.assertEqual(machine.dns('pool.ntp.org')[-4:], bytes([10, 0, 2, 2]))
                self.assertTrue(any(frame[0] == 0x40 for frame in machine.beacon()))

    def test_ethernet_reply_and_broadcast_arp_select_correct_interface(self):
        """Incoming packets keep their MAC/direction after traffic on the other role."""
        for c2 in (False, True):
            port = services.free_port()
            with self.subTest(c2=c2), Machine(('clock-services=on', 'ntp-hosts=pool.ntp.org',
                                             'softap-client-ssid=Small_TV'), c2=c2, wire_port=port) as machine:
                with socket.create_connection(('127.0.0.1', port), 2) as wire:
                    machine.associate()
                    machine.complete_softap()
                    self.assertEqual(machine.dns('pool.ntp.org')[-4:], bytes([10, 0, 2, 2]))
                    host = bytes.fromhex('525500000099')

                    def receive(ethernet, macs):
                        wire.sendall(struct.pack('!I', len(ethernet)) + ethernet)
                        deadline = time.monotonic() + 2
                        while time.monotonic() < deadline:
                            machine.command('clock_step 1000000')
                            for packet in machine.frames():
                                start = packet.find(macs)
                                if start >= 4:
                                    return packet[start - 4:]
                            time.sleep(.005)
                        self.fail('No DMA frame for Ethernet target ' + macs.hex())

                    def ip(destination, target):
                        return destination + host + bytes.fromhex('0800') + struct.pack(
                            '!BBHHHBBH4s4s', 0x45, 0, 24, 0, 0, 64, 17, 0,
                            bytes([192, 168, 43, 2]), bytes(target)) + b'APST'

                    ap = receive(ip(GUEST_AP, [192, 168, 43, 1]), GUEST_AP + CLIENT)
                    self.assertEqual(ap[1] & 3, 1, 'AP packet must be sent by the virtual client, ToDS')
                    self.assertEqual(ap[16:22], GUEST_AP)
                    sta = receive(ip(services.STATION, [10, 0, 2, 15]), services.STATION + services.AP)
                    self.assertEqual(sta[1] & 3, 2, 'STA packet must be sent by the external AP, FromDS')
                    self.assertEqual(sta[16:22], services.AP)
                    # The broadcast destination alone cannot identify an interface.
                    arp = struct.pack('!HHBBH', 1, 0x0800, 6, 4, 1) + host + bytes([192, 168, 43, 2])
                    arp += bytes(6) + bytes([192, 168, 43, 1])
                    ap = receive(BROADCAST + host + bytes.fromhex('0806') + arp, GUEST_AP + CLIENT)
                    self.assertEqual(ap[1] & 3, 1)

    def test_external_ap_outage_preserves_queued_softap_frames(self):
        """An external outage drops STA traffic but retains AP association work."""
        for c2 in (False, True):
            with self.subTest(c2=c2), Machine(('clock-services=on', 'ntp-hosts=pool.ntp.org', 'softap-client-ssid=Small_TV',
                                             'latency-ms=20'), c2=c2) as machine:
                machine.associate()
                machine.send_frame(0x80, GUEST_AP, BROADCAST, GUEST_AP,
                                   bytes(8) + struct.pack('<HH', 100, 1) + bytes([0, len(SSID)]) + SSID)
                machine.set('station-ap-online', False)
                machine.command('clock_step 100000000')
                frames = machine.frames()
                ap = [packet[packet.find(GUEST_AP + CLIENT) - 4:]
                      for packet in frames if packet.find(GUEST_AP + CLIENT) >= 4]
                sta = [packet[packet.find(services.STATION + services.AP) - 4:]
                       for packet in frames if packet.find(services.STATION + services.AP) >= 4]
                self.assertTrue(any(frame[0] == 0x40 for frame in ap), 'AP probe lost during STA outage')
                self.assertEqual([frame[0] for frame in sta], [0xc0],
                                 'Only the external AP leaving notice may survive its outage')

    def test_station_association_does_not_block_softap(self):
        """A connected STA must not suppress the guest AP's virtual client."""
        for c2 in (False, True):
            with self.subTest(c2=c2), Machine(('clock-services=on', 'ntp-hosts=pool.ntp.org', 'softap-client-ssid=Small_TV'), c2=c2) as machine:
                machine.associate()
                frames = machine.beacon()
                self.assertTrue(any(frame[0] == 0x40 for frame in frames),
                                'Associated STA suppressed the SoftAP probe request')
                self.assertEqual(machine.dns('pool.ntp.org')[-4:], bytes([10, 0, 2, 2]))
                frames = machine.management(5, bytes(8) + struct.pack('<HH', 100, 1) + bytes([0, len(SSID)]) + SSID)
                self.assertTrue(any(frame[0] == 0xb0 for frame in frames), 'Missing virtual client authentication')
                self.assertEqual(machine.dns('pool.ntp.org')[-4:], bytes([10, 0, 2, 2]))
                frames = machine.management(11, struct.pack('<HHH', 0, 2, 0))
                self.assertTrue(any(frame[0] == 0 for frame in frames), 'Missing virtual client association')
                self.assertEqual(machine.dns('pool.ntp.org')[-4:], bytes([10, 0, 2, 2]))
                machine.management(1, struct.pack('<HHH', 1, 0, 0xc001))
                self.assertEqual(machine.dns('pool.ntp.org')[-4:], bytes([10, 0, 2, 2]))

    def test_softap_authentication_does_not_corrupt_station(self):
        """A partially connected AP client must not swallow a later STA start."""
        for c2 in (False, True):
            with self.subTest(c2=c2), Machine(('clock-services=on', 'ntp-hosts=pool.ntp.org', 'softap-client-ssid=Small_TV'), c2=c2) as machine:
                machine.associate()
                # End the first STA association, then begin AP before STA
                # authenticates again. Do not reset either guest interface.
                machine.send_frame(0xa0, services.STATION, services.AP, services.AP, struct.pack('<H', 8))
                machine.replies()
                self.assertTrue(any(frame[0] == 0x40 for frame in machine.beacon()))
                machine.send(0xb0, struct.pack('<HHH', 0, 1, 0))
                machine.send(0, struct.pack('<HH', 0, 1))
                machine.replies()
                self.assertEqual(machine.dns('pool.ntp.org')[-4:], bytes([10, 0, 2, 2]))
                frames = machine.management(5, bytes(8) + struct.pack('<HH', 100, 1) + bytes([0, len(SSID)]) + SSID)
                self.assertTrue(any(frame[0] == 0xb0 for frame in frames))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', required=True)
    parser.add_argument('--bios', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--temp-root', type=Path, required=True)
    options = parser.parse_args()
    options.expect_debug = None
    services.OPTIONS = options
    options.temp_root.mkdir(parents=True, exist_ok=True)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ApstaTests))
    report = dict(passed=result.wasSuccessful(), tests=result.testsRun,
                  failures=[str(case) for case, _ in result.failures], errors=[str(case) for case, _ in result.errors],
                  qemu_sha256=hashlib.sha256(Path(options.qemu).read_bytes()).hexdigest(),
                  test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  harness_sha256=hashlib.sha256(Path(services.__file__).read_bytes()).hexdigest(),
                  scope=__doc__ + ' C2 and C3 qtest; no firmware execution or host network.')
    options.output.parent.mkdir(parents=True, exist_ok=True)
    options.output.write_text(json.dumps(report, indent=2) + '\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
