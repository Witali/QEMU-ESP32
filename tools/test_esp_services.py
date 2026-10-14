"""Check profile-driven service URLs, isolated routing and invalid configurations."""
import json
from pathlib import Path
import tempfile
import unittest

from esp_board import compile_profile
from esp_services import hostname, network_profile, service_endpoints
from small_tv_web import user_network_options


class ServiceProfileTests(unittest.TestCase):
    def test_multiple_hosts_ports_and_paths(self):
        urls = ['http://first.test/a?key=synthetic', 'http://second.test:8081/b',
                'http://FIRST.TEST/other', 'https://first.test/secure']
        endpoints = service_endpoints(urls)
        self.assertEqual([row['suffix'] for row in endpoints], [100, 101, 100, 100])
        self.assertNotIn('key=', str(endpoints))
        for network, subnet in [('clock', '10.0.2'), ('setup', '192.168.43')]:
            options = user_network_options(network, [], 19000, urls)
            self.assertEqual(options.count(f'guestfwd=tcp:{subnet}.100:80-'), 1)
            self.assertIn(f'guestfwd=tcp:{subnet}.101:8081-tcp:127.0.0.1:19000', options)
            self.assertIn(f'guestfwd=tcp:{subnet}.100:443-tcp:127.0.0.1:19000', options)
            self.assertIn('restrict=on', options)
        self.assertNotIn('guestfwd', user_network_options('clock', [], 0, urls))

    def test_idna_and_url_boundaries(self):
        self.assertEqual(hostname('météo.test.'), 'xn--mto-bmab.test')
        self.assertEqual(service_endpoints(['https://fixture.test'])[0]['port'], 443)
        self.assertEqual(service_endpoints(['http://fixture.test:65535'])[0]['port'], 65535)
        for url in ('', 'fixture.test', 'ftp://fixture.test', 'http://127.0.0.1',
                    'http://[::1]', 'http://fixture.test:0', 'http://fixture.test:65536',
                    'http://fixture.test:invalid', 'http://a..test', 'http://-bad.test',
                    'http://bad-.test', 'http://' + 'a' * 64 + '.test',
                    'http://user:secret@fixture.test', 'http://fixture.test/#fragment',
                    'http://fixture_test', 'http://fixture.test..'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                service_endpoints([url])
        with self.assertRaises(ValueError):
            service_endpoints(['http://host%d.test' % i for i in range(156)])

    def test_device_profile_compiles_services(self):
        profile = Path(__file__).resolve().parents[1] / 'configs/boards/small-tv-c2.json'
        config = compile_profile(profile, 'esp32c2', 'clock', 19000)
        self.assertIn('esp32c3-wifi.ntp-hosts=' + ';'.join(config['network']['ntp_hosts']), config['network_arguments'])
        hosts = ';'.join(dict.fromkeys(row['hostname'] for row in service_endpoints(config['network']['urls'])))
        self.assertIn('esp32c3-wifi.service-hosts=' + hosts, config['network_arguments'])
        self.assertEqual(config['service_urls'], config['network']['urls'])
        self.assertEqual(compile_profile(profile)['network_arguments'], [])
        overridden = compile_profile(profile, 'esp32c2', 'setup', 19000, ['http://custom.test:8081'])
        self.assertIn('esp32c3-wifi.service-hosts=custom.test', overridden['network_arguments'])
        self.assertIn('esp32c3-wifi.softap-client-ssid=' + config['network']['softap_client_ssid'], overridden['network_arguments'])
        with self.assertRaises(ValueError):
            compile_profile(profile, service_port=19000)
        with self.assertRaises(ValueError):
            compile_profile(profile, 'esp32c2', 'clock', 19000,
                            ['http://' + config['network']['ntp_hosts'][0]])

    def test_invalid_and_legacy_device_profiles(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'profile.json'
            profile = {'schema': 1, 'soc': 'esp32c3', 'devices': []}
            path.write_text(json.dumps(profile))
            self.assertEqual(network_profile(path), {'urls': [], 'ntp_hosts': []})
            self.assertEqual(compile_profile(path)['network_arguments'], [])
            profile['network'] = {'urls': ['http://fixture.test/a?key=synthetic']}
            path.write_text(json.dumps(profile))
            self.assertNotIn('key=', json.dumps(compile_profile(path)))
            for network in ({'urls': 'http://fixture.test'}, {'urls': [42]},
                            {'ntp_hosts': ['a.test'] * 2}, {'ntp_hosts': ['x.test'] * 4},
                            {'ntp_hosts': ['bad..test']}, {'station_ssid': 'x' * 33},
                            {'softap_client_ssid': ''}, {'urls': ['http://a.test'], 'ntp_hosts': ['a.test']}):
                profile['network'] = network
                path.write_text(json.dumps(profile))
                with self.subTest(network=network), self.assertRaises(ValueError):
                    network_profile(path)


if __name__ == '__main__':
    unittest.main(verbosity=2)
