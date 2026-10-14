"""Check synthetic settings, consecutive ports and listener isolation."""
from pathlib import Path
import socket
import struct
import unittest
from small_tv_network import seed_clock_network, seed_setup_network
from small_tv_web import reserve_web_ports, user_network_options, web_targets


class NetworkTests(unittest.TestCase):
    def test_separate_consecutive_endpoints(self):
        routes, held = reserve_web_ports('setup')
        try:
            self.assertEqual([row['name'] for row in routes], ['setup', 'station'])
            self.assertEqual(routes[1]['host_port'], routes[0]['host_port'] + 1)
            self.assertEqual([row['guest_address'] for row in routes], ['192.168.43.1', '192.168.43.15'])
            self.assertTrue(all(row['host_address'] == '127.0.0.1' for row in routes))
            other, other_held = reserve_web_ports('setup')
            try:
                self.assertFalse({row['host_port'] for row in routes} & {row['host_port'] for row in other})
                with self.assertRaisesRegex(ValueError, 'busy'):
                    reserve_web_ports('setup', routes[0]['host_port'])
            finally:
                for sock in other_held: sock.close()
        finally:
            for sock in held: sock.close()

    def test_boundaries_and_disabled_forwarding(self):
        for base in (-2, 65536):
            with self.assertRaises(ValueError): reserve_web_ports('clock', base)
        with self.assertRaises(ValueError): reserve_web_ports('setup', 65535)
        self.assertEqual(reserve_web_ports('setup', -1), ([], []))
        self.assertEqual(reserve_web_ports('offline'), ([], []))
        self.assertEqual(web_targets('clock'), [('station', '10.0.2.15')])
        with self.assertRaises(ValueError): web_targets('unknown')

    def test_real_slirp_options(self):
        routes, held = reserve_web_ports('setup')
        try:
            options = user_network_options('setup', routes)
            self.assertIn('restrict=on', options)
            self.assertIn('net=192.168.43.0/24', options)
            for row in routes:
                self.assertIn(f"hostfwd=tcp:127.0.0.1:{row['host_port']}-{row['guest_address']}:80", options)
            self.assertNotIn('0.0.0.0', options)
        finally:
            for sock in held: sock.close()

    def test_opt_in_loopback_weather(self):
        self.assertNotIn('guestfwd', user_network_options('clock', []))
        for network, address in [('clock', '10.0.2.100'), ('setup', '192.168.43.100')]:
            options = user_network_options(network, [], 19000, ['http://fixture.test'])
            self.assertIn(f'guestfwd=tcp:{address}:80-tcp:127.0.0.1:19000', options)
            self.assertIn('restrict=on', options)
        for port in (-1, 65536, '19000'):
            with self.assertRaises(ValueError): user_network_options('clock', [], port)
        with self.assertRaises(ValueError): user_network_options('offline', [], 19000)

    def test_private_state_cleared_and_payload_preserved(self):
        source = bytearray(b'\xa5' * 0x400000)
        struct.pack_into('<5I', source, 0x3e71a8, 100, 200, 300, 400, 500)
        for setup, seed in ((False, seed_clock_network), (True, seed_setup_network)):
            data = seed(source, 'original', Path('.'), Path('.'))
            self.assertEqual(source[0], 0xa5)
            self.assertEqual(data[:0x9000], source[:0x9000])
            self.assertEqual(data[0xf000:0x3e7000], source[0xf000:0x3e7000])
            self.assertEqual(data[0x3e8000:], source[0x3e8000:])
            self.assertEqual(data[0x9000:0xf000], b'\xff' * 0x6000)
            self.assertEqual(struct.unpack_from('<I', data, 0x3e7160)[0], 2 if setup else 1)
            self.assertEqual(data[0x3e7004:0x3e7045].split(b'\0')[0], b'' if setup else b'yoRadio-QEMU')
            self.assertEqual(data[0x3e7045:0x3e7086], bytes(65))
            self.assertEqual(data[0x3e711c:0x3e715d].split(b'\0')[0], b'disabled')
            self.assertEqual(struct.unpack_from('<5I', data, 0x3e71a8), (100,200,300,400,500))
            self.assertEqual(struct.unpack_from('<I', data, 0x3e71a4)[0], 31)


if __name__ == '__main__':
    unittest.main(verbosity=2)
