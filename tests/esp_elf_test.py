"""Reject wrong ELF/RAM observations before accepting a silent firmware boot."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parents[1]
TEMP_ROOT = ROOT / '.cache/esp-elf-unit'
sys.path.insert(0, str(ROOT / 'tools'))
from esp_elf import object_symbols
from small_tv_vm import application_state, application_state_layout, ntp_reply_observation, wait_for_clock_recovery

OBJECTS = {'connected': (0x3fca1000, 1), 'ap_active': (0x3fca1001, 1),
           'time_synced': (0x3fca1002, 1), 'last_station_address': (0x3fca1004, 4),
           's_microseconds_offset': (0x3fca1008, 8), 's_correction_us': (0x3fca1010, 8)}


def image(objects=None, duplicate=False):
    """Create a minimal real ELF32 symbol/string/BSS layout, not a parser mock."""
    objects = objects or OBJECTS
    names, symbols = bytearray(b'\0'), bytearray(16)
    for name, (address, size) in list(objects.items()) + (list(objects.items())[:1] if duplicate else []):
        at = len(names)
        names += name.encode() + b'\0'
        symbols += struct.pack('<IIIBBH', at, address, size, 1, 0, 3)
    head = bytearray(52)
    head[:7] = b'\x7fELF\x01\x01\x01'
    struct.pack_into('<HHIIIIIHHHHHH', head, 16, 2, 243, 1, 0x42000000, 0, 52, 0, 52, 32, 0, 40, 4, 0)
    tables = bytearray(160)
    struct.pack_into('<10I', tables, 40, 0, 3, 0, 0, 212, len(names), 0, 0, 1, 0)
    struct.pack_into('<10I', tables, 80, 0, 2, 0, 0, 212 + len(names), len(symbols), 1, 0, 4, 16)
    struct.pack_into('<10I', tables, 120, 0, 8, 3, 0x3fca1000, 0, 64, 0, 0, 4, 0)
    return bytes(head + tables + names + symbols)


class ElfTests(unittest.TestCase):
    def test_linked_object_addresses_and_sizes(self):
        self.assertEqual(object_symbols(image(), OBJECTS),
                         {name: {'address': address, 'size': size} for name, (address, size) in OBJECTS.items()})

    def test_wrong_abi_and_short_header(self):
        for candidate in (b'', image()[:51], image()[:5] + b'\x02' + image()[6:]):
            with self.subTest(candidate=candidate[:7]), self.assertRaises(ValueError):
                object_symbols(candidate, OBJECTS)
        candidate = bytearray(image())
        struct.pack_into('<H', candidate, 18, 94)
        with self.assertRaises(ValueError):
            object_symbols(candidate, OBJECTS)

    def test_bounded_section_and_symbol_tables(self):
        for at, fmt, value in ((32, '<I', 0xfffffff0), (46, '<H', 39),
                               (52 + 80 + 36, '<I', 12), (52 + 80 + 24, '<I', 9)):
            candidate = bytearray(image())
            struct.pack_into(fmt, candidate, at, value)
            with self.subTest(at=at), self.assertRaises(ValueError):
                object_symbols(candidate, OBJECTS)
        with self.assertRaises(ValueError):
            object_symbols(image()[:-1], OBJECTS)

    def test_duplicate_missing_and_undefined_objects(self):
        with self.assertRaises(ValueError):
            object_symbols(image(duplicate=True), OBJECTS)
        with self.assertRaises(ValueError):
            object_symbols(image(), {'absent'})
        candidate = bytearray(image())
        symbol_offset = struct.unpack_from('<I', candidate, 52 + 80 + 16)[0]
        struct.pack_into('<H', candidate, symbol_offset + 16 + 14, 0)
        with self.assertRaises(ValueError):
            object_symbols(candidate, OBJECTS)

    def test_unterminated_symbol_name(self):
        candidate = bytearray(image())
        names_size = struct.unpack_from('<I', candidate, 52 + 40 + 20)[0]
        candidate[212 + names_size - 1] = 88
        with self.assertRaises(ValueError):
            object_symbols(candidate, OBJECTS)


class RuntimeStateTests(unittest.TestCase):
    def setUp(self):
        self.parent = TEMP_ROOT.resolve()
        self.parent.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(dir=self.parent)
        self.folder = Path(self.temporary.name).resolve()
        self.assertEqual(self.folder.parent, self.parent)

    def tearDown(self):
        if self.folder.parent != self.parent:
            raise ValueError('Temporary unit-test folder escaped its workspace')
        self.temporary.cleanup()

    def layout(self, data=None, wrong_app=False):
        data = data or image()
        elf = self.folder / 'app.elf'
        elf.write_bytes(data)
        archive = self.folder / 'firmware/development/small-tv-esp32c2-reconstructed'
        archive.mkdir(parents=True, exist_ok=True)
        app = bytearray(208)
        app[176:208] = bytes(32) if wrong_app else hashlib.sha256(data).digest()
        (archive / 'app.bin').write_bytes(app)
        return application_state_layout(SimpleNamespace(yoradio=self.folder, app_elf=elf))

    def test_exact_elf_app_binding(self):
        self.assertEqual(self.layout()['elf_sha256'], hashlib.sha256(image()).hexdigest())
        with self.assertRaises(ValueError):
            self.layout(wrong_app=True)

    def test_reject_wrong_ram_or_scalar_abi(self):
        for address, size in ((0x42000000, 1), (0x3fcdffff, 4), (0x3fca1000, 4)):
            objects = {**OBJECTS, 'connected': (address, size)}
            with self.subTest(address=address, size=size), self.assertRaises(ValueError):
                self.layout(image(objects))

    def test_clock_object_size_and_range(self):
        for address, size in ((0x42000000, 8), (0x3fcdfffc, 8), (0x3fca1008, 4)):
            with self.subTest(address=address, size=size), self.assertRaises(ValueError):
                self.layout(image({**OBJECTS, 's_microseconds_offset': (address, size)}))

    def test_clock_accounts_for_reset_relative_sdk_offsets(self):
        layout = self.layout()
        calls = []
        values = {0x60008050: (100000000, 0), 0x6004d000: (20000000,),
                  0x60023040: (0, 104000000), 0x3fca1008: (15000000, 0),
                  0x3fca1010: (0xfff0bdc0, 0xffffffff)}  # -1,000,000 us
        class Qmp:
            def execute(self, command, **arguments):
                calls.append(command)
                if command == 'qom-get':
                    return json.dumps({'count': 2, 'unix_seconds': 121, 'virtual_us': 17000000})
                if command in ('stop', 'cont'):
                    return {}
                if command != 'human-monitor-command' or not arguments['command-line'].startswith('xp /'):
                    raise AssertionError('Unexpected guest write')
                address = int(arguments['command-line'].split()[-1], 16)
                return f'{address:016x}: ' + ' '.join(f'0x{value:08x}' for value in values[address])
        result = ntp_reply_observation(Qmp(), self.folder, True, False, layout)
        self.assertEqual(result['since_boot_us'], 24000000)
        self.assertEqual(result['actual_utc_seconds'], 124)
        self.assertEqual(result['expected_utc_seconds'], 124)
        self.assertEqual(result['difference_seconds'], 0)
        self.assertEqual((calls.count('stop'), calls.count('cont')), (1, 1))

    def read_state(self, invalid=None, short=False):
        layout = self.layout()
        values = {0x3fca1000: b'\x01', 0x3fca1001: b'\0', 0x3fca1002: b'\x01',
                  0x3fca1004: bytes((10, 0, 2, 15))}
        if invalid is not None:
            values[0x3fca1000] = bytes([invalid])
        calls = []
        class Qmp:
            def execute(self, command, **arguments):
                if command != 'human-monitor-command':
                    raise AssertionError('Only read-only memory commands are permitted')
                text = arguments['command-line']
                if not text.startswith('xp /'):
                    raise AssertionError('Unexpected memory mutation')
                calls.append(text)
                address = int(text.split()[-1], 16)
                return f'{address:016x}: ' + ' '.join('0x%02x' % value for value in values[address][1 if short else 0:]) + '\n'
        result = application_state(Qmp(), layout)
        self.assertEqual(len(calls), 4)
        return result

    def test_flags_and_network_byte_order(self):
        self.assertEqual(self.read_state(), {'connected': True, 'ap_active': False,
            'time_synced': True, 'station_address': '10.0.2.15'})

    def test_corrupt_flag_is_not_readiness(self):
        with self.assertRaises(ValueError):
            self.read_state(invalid=2)

    def test_short_observation_is_not_readiness(self):
        with self.assertRaises(ValueError):
            self.read_state(short=True)


class ClockRecoveryTests(unittest.TestCase):
    def wait(self, samples, flags=None, host_step=4, guest_step=1000000, start=0):
        """Advance only synthetic host/MMIO clocks; keep the real wait predicate."""
        class HostTime:
            value = 0
            def monotonic(self): return self.value
            def sleep(self, ignored): self.value += host_step
        class Qmp:
            count = -1
            def execute(self, command, **arguments):
                if command != 'human-monitor-command' or arguments['command-line'] != 'xp /1wx 0x6004d000':
                    raise AssertionError('Unexpected guest write or call')
                self.count += 1
                value = (start + self.count * guest_step) & 0xffffffff
                return f'000000006004d000: 0x{value:08x}'
        native = {'connected': True, 'time_synced': True, 'ap_active': False, 'station_address': '10.0.2.15'}
        sequence = iter(samples)
        states = iter(flags or [native] * len(samples))
        last_sample, last_flags = samples[-1], (flags or [native])[-1]
        with patch('small_tv_vm.time', HostTime()), \
             patch('small_tv_vm.ntp_reply_observation', side_effect=lambda *args: next(sequence, last_sample)), \
             patch('small_tv_vm.application_state', side_effect=lambda *args: next(states, last_flags)):
            return wait_for_clock_recovery(Qmp(), Path('.'), {'symbols': {'connected': {}}}, 1, True, False)

    def test_late_ready_is_not_an_eight_host_second_failure(self):
        old = {'response_count': 1, 'difference_seconds': 0}
        new = {'response_count': 2, 'difference_seconds': 0}
        wrong = {'response_count': 2, 'difference_seconds': 2}
        bad = {'connected': True, 'time_synced': False, 'ap_active': False, 'station_address': '10.0.2.15'}
        good = {**bad, 'time_synced': True}
        result = self.wait([old, wrong, new, new], [good, good, bad, good])
        self.assertGreater(result['host_seconds'], 8)
        self.assertLess(result['guest_seconds'], 15)
        self.assertEqual(result['polls'], 4)

    def test_old_reply_times_out_on_guest_clock(self):
        with self.assertRaises(TimeoutError):
            self.wait([{'response_count': 1, 'difference_seconds': 0}], host_step=0, guest_step=2000000)

    def test_notification_is_required(self):
        with self.assertRaises(TimeoutError):
            self.wait([{'response_count': 2, 'difference_seconds': 0}],
                      [{'connected': True, 'time_synced': False, 'ap_active': False, 'station_address': '10.0.2.15'}])

    def test_wrong_sdk_clock_is_not_ready(self):
        with self.assertRaises(TimeoutError):
            self.wait([{'response_count': 2, 'difference_seconds': 1.5}])

    def test_stalled_guest_has_a_host_deadline(self):
        with self.assertRaises(TimeoutError):
            self.wait([{'response_count': 1, 'difference_seconds': 0}], guest_step=0)

    def test_ready_after_guest_deadline_is_rejected(self):
        samples = [{'response_count': 1, 'difference_seconds': 0}] * 14
        with self.assertRaises(TimeoutError):
            self.wait(samples + [{'response_count': 2, 'difference_seconds': 0}], host_step=1)

    def test_radio_counter_wrap(self):
        result = self.wait([{'response_count': 2, 'difference_seconds': 0}], guest_step=512, start=0xffffff00)
        self.assertAlmostEqual(result['guest_seconds'], .000512)


def main():
    """Return actual unittest counts and fingerprints for the full VM suite."""
    global TEMP_ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--temp-root', type=Path, default=TEMP_ROOT)
    args = parser.parse_args()
    TEMP_ROOT = args.temp_root
    tests = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(tests)
    digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = {'passed': result.wasSuccessful(), 'tests': result.testsRun,
              'failures': [str(case) for case, _ in result.failures],
              'errors': [str(case) for case, _ in result.errors],
              'source_sha256': digest(Path(__file__)),
              'converter_sha256': digest(ROOT / 'tools/esp_elf.py'),
              'vm_helper_sha256': digest(ROOT / 'tools/small_tv_vm.py')}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
