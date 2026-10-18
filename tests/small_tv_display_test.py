"""Check panel recovery and prove network/display failures stay independent."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
TEMP_ROOT = ROOT / '.cache/display-unit'
sys.path.insert(0, str(ROOT / 'tools'))
from small_tv_display import wait_for_display_recovery
from small_tv_vm import observe_reset_subsystems, subsystem_check_results


class DisplayRecoveryTests(unittest.TestCase):
    def observe(self, frames, guest_step=100000, host_step=.1, start=0, malformed=False):
        """Read real PPM files; synthesize only the VM transport and time sources."""
        TEMP_ROOT.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=TEMP_ROOT) as directory:
            state = Path(directory)
            class HostTime:
                value = 0
                def monotonic(self): return self.value
                def sleep(self, ignored): self.value += host_step
            class Qmp:
                clocks = -1
                captures = 0
                def virtual_us(self):
                    self.clocks += 1
                    return (start + self.clocks * guest_step) & 0xffffffff
                def execute(self, command, **arguments):
                    if command != 'screendump':
                        raise AssertionError('Display observer accessed non-display state')
                    count = frames[min(self.captures, len(frames) - 1)]
                    image = (b'P6\n240 240\n255\n' + bytes([255]) * count +
                             bytes(240 * 240 * 3 - count))
                    Path(arguments['filename']).write_bytes(image[:-1] if malformed else image)
                    self.captures += 1
            qmp = Qmp()
            with patch('small_tv_display.time', HostTime()):
                result = wait_for_display_recovery(qmp, state, qmp.virtual_us)
            self.assertFalse((state / 'uart.log').exists())
            return result

    def test_partial_frame_then_render(self):
        result = self.observe([0, 624, 11629])
        self.assertTrue(result['passed'])
        self.assertEqual(result['polls'], 3)
        self.assertEqual([row['nonzero_bytes'] for row in result['samples']], [0, 624, 11629])

    def test_black_screen_guest_deadline(self):
        result = self.observe([0], guest_step=1000000, host_step=0)
        self.assertFalse(result['passed'])
        self.assertEqual(result['guest_seconds'], 5)

    def test_stalled_guest_host_deadline(self):
        result = self.observe([0], guest_step=0, host_step=10)
        self.assertFalse(result['passed'])
        self.assertEqual(result['host_seconds'], 30)

    def test_ready_at_guest_deadline_rejected(self):
        self.assertFalse(self.observe([0, 2000], guest_step=2500000)['passed'])

    def test_ready_at_host_deadline_rejected(self):
        self.assertFalse(self.observe([0, 2000], guest_step=0, host_step=30)['passed'])

    def test_radio_clock_wrap(self):
        result = self.observe([2000], guest_step=512, start=0xffffff00)
        self.assertTrue(result['passed'])
        self.assertAlmostEqual(result['guest_seconds'], .000512)

    def test_short_raster_rejected(self):
        with self.assertRaises(AssertionError):
            self.observe([2000], malformed=True)

    def test_original_threshold_retained(self):
        self.assertFalse(self.observe([1000], guest_step=1000000)['passed'])

    def test_display_needs_no_uart_or_network_flags(self):
        self.assertTrue(self.observe([2000])['passed'])


class SubsystemIsolationTests(unittest.TestCase):
    def test_network_failure_does_not_skip_display(self):
        with patch('small_tv_vm.wait_for_clock_recovery', side_effect=TimeoutError('network unavailable')), \
             patch('small_tv_vm.wait_for_display_recovery', return_value={'passed': True}) as display:
            result = observe_reset_subsystems(None, Path('.'), {}, 1, True, False, 'clock')
        display.assert_called_once()
        self.assertFalse(result['network']['passed'])
        self.assertEqual(result['network']['error'], 'network unavailable')
        self.assertTrue(result['display']['passed'])

    def test_display_failure_retains_network_success(self):
        with patch('small_tv_vm.wait_for_clock_recovery', return_value={'passed': True}), \
             patch('small_tv_vm.wait_for_display_recovery', side_effect=ValueError('bad raster')):
            result = observe_reset_subsystems(None, Path('.'), {}, 1, True, False, 'clock')
        self.assertTrue(result['network']['passed'])
        self.assertFalse(result['display']['passed'])
        self.assertEqual(result['display']['error'], 'bad raster')

    def test_boot_network_display_have_separate_verdicts(self):
        result = subsystem_check_results({'reset_reboots': False, 'station_dhcp': True,
                                          'display_ready_after_reset': True})
        self.assertFalse(result['boot']['passed'])
        self.assertTrue(result['network']['passed'])
        self.assertTrue(result['display']['passed'])


def main():
    """Return distinct panel/isolation counts and exact helper identities."""
    global TEMP_ROOT
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--temp-root', type=Path, default=TEMP_ROOT)
    args = parser.parse_args()
    TEMP_ROOT = args.temp_root
    suite = unittest.defaultTestLoader.loadTestsFromModule(sys.modules[__name__])
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sha = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
    report = {'passed': result.wasSuccessful(), 'tests': result.testsRun,
              'display_tests': unittest.defaultTestLoader.loadTestsFromTestCase(DisplayRecoveryTests).countTestCases(),
              'isolation_tests': unittest.defaultTestLoader.loadTestsFromTestCase(SubsystemIsolationTests).countTestCases(),
              'failures': [str(case) for case, _ in result.failures],
              'errors': [str(case) for case, _ in result.errors],
              'source_sha256': sha(Path(__file__)),
              'display_helper_sha256': sha(ROOT / 'tools/small_tv_display.py'),
              'vm_helper_sha256': sha(ROOT / 'tools/small_tv_vm.py')}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
    return not report['passed']


if __name__ == '__main__':
    raise SystemExit(main())
