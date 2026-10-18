#!/usr/bin/env python3
"""Check real ESP32 DPORT modem-clock/reset MMIO against TRM and IDF 5.5.4."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import unittest
import uuid

OPTIONS = None
CLOCK, RESET = 0x3ff000cc, 0x3ff000d0
CLOCK_RESET = 0xfffce030
REQUIRED = 0x8f8f


def port():
    """Choose an endpoint for this test's owned CPU-stopped VM."""
    with socket.socket() as connection:
        connection.bind(('127.0.0.1', 0))
        return connection.getsockname()[1]


class Machine:
    """Own an isolated QTest/QMP pair; execute no application or ROM code."""
    def __init__(self, folder):
        from small_tv_vm import QMP
        folder.mkdir()
        image = folder / 'flash.bin'
        image.write_bytes(b'\xff' * (4 * 1024 * 1024))
        self.socket = self.stream = self.qmp = None
        self.log = (folder / 'qemu.log').open('wb')
        name = 'phy-clock-' + uuid.uuid4().hex[:12]
        test_port, qmp_port = port(), port()
        command = [str(OPTIONS.qemu), '-M', 'esp32', '-accel', 'qtest', '-S',
            '-name', name, '-L', str(OPTIONS.bios), '-display', 'none', '-serial', 'none',
            '-monitor', 'none', '-nic', 'none', '-drive', f'file={image},if=mtd,format=raw',
            '-qtest', f'tcp:127.0.0.1:{test_port},server=on,wait=off',
            '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off',
            '-qtest-log', str(folder / 'qtest.log')]
        self.process = subprocess.Popen(command, stdout=self.log, stderr=self.log,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
        try:
            deadline = time.monotonic() + 15
            while True:
                try:
                    self.socket = socket.create_connection(('127.0.0.1', test_port), .3)
                    break
                except OSError:
                    if self.process.poll() is not None or time.monotonic() >= deadline:
                        raise RuntimeError((folder / 'qemu.log').read_text(errors='replace'))
                    time.sleep(.02)
            self.socket.settimeout(5)
            self.stream = self.socket.makefile('rwb', buffering=0)
            self.qmp = QMP(qmp_port, name)
        except BaseException:
            self.close()
            raise

    def command(self, value):
        """Send one actual MMIO transaction through QTest."""
        self.stream.write((value + '\n').encode())
        while True:
            line = self.stream.readline().decode().strip()
            if line.startswith('IRQ '):
                continue
            assert line.startswith('OK'), (value, line)
            return line[2:].strip()

    def read(self, address):
        """Read a word as the SDK's DPORT_REG_READ does."""
        return int(self.command(f'readl {address:#x}'), 0)

    def write(self, address, value):
        """Write one full DPORT control word."""
        self.command(f'writel {address:#x} {value:#x}')

    def close(self):
        """Close only this VM through its verified QMP or retained process object."""
        if self.qmp is not None:
            try:
                self.qmp.execute('quit')
            except (OSError, ValueError):
                pass
            self.qmp.close()
        if self.stream is not None:
            self.stream.close()
        if self.socket is not None:
            self.socket.close()
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=5)
        self.log.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


class ClockTests(unittest.TestCase):
    """Exercise reset, read-modify-write and the SDK's PHY-required clock mask."""
    def machine(self):
        return Machine(OPTIONS.output / self._testMethodName)

    def test_reset_values(self):
        with self.machine() as machine:
            self.assertEqual(machine.read(CLOCK), CLOCK_RESET)
            self.assertEqual(machine.read(RESET), 0)

    def test_sdk_phy_clock_enable_sequence(self):
        # IDF periph_ctrl.c enables common, RNG, Wi-Fi and BT clocks before PHY.
        with self.machine() as machine:
            machine.write(CLOCK, 0)
            expected = 0
            for mask in (0x3c9, 0x8000, 0x406, 0x30800):
                expected |= mask
                machine.write(CLOCK, machine.read(CLOCK) | mask)
                self.assertEqual(machine.read(CLOCK), expected)
            self.assertEqual(machine.read(CLOCK) & REQUIRED, REQUIRED)
            machine.write(CLOCK, machine.read(CLOCK) & ~0x406)
            self.assertEqual(machine.read(CLOCK), expected & ~0x406)

    def test_reset_latch_and_independent_clock(self):
        # TRM reset bits stay asserted until software clears them.
        with self.machine() as machine:
            machine.write(RESET, 0x7ff)
            machine.write(CLOCK, 0x8f8f)
            self.assertEqual(machine.read(RESET), 0x7ff)
            self.assertEqual(machine.read(CLOCK), 0x8f8f)
            machine.write(RESET, machine.read(RESET) & ~0x618)
            self.assertEqual(machine.read(RESET), 0x1e7)
            self.assertEqual(machine.read(CLOCK), 0x8f8f)

    def test_system_reset_restores_gate_latches(self):
        with self.machine() as machine:
            machine.write(CLOCK, 0)
            machine.write(RESET, 0x7ff)
            machine.qmp.execute('system_reset')
            deadline = time.monotonic() + 5
            while (machine.read(CLOCK), machine.read(RESET)) != (CLOCK_RESET, 0):
                self.assertLess(time.monotonic(), deadline, 'DPORT reset never restored the documented values')
                time.sleep(.02)


def main():
    """Preserve the exact binary, SDK inputs and every failed MMIO control."""
    global OPTIONS
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--bios', type=Path, required=True)
    parser.add_argument('--sdk-root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    OPTIONS = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / 'tools'))
    OPTIONS.qemu, OPTIONS.bios, OPTIONS.output = [path.resolve() for path in (OPTIONS.qemu, OPTIONS.bios, OPTIONS.output)]
    OPTIONS.output.mkdir(parents=True, exist_ok=False)
    inputs = [Path(__file__), OPTIONS.qemu, root / 'tools/small_tv_vm.py', root / 'tools/small_tv_display.py']
    inputs += [OPTIONS.sdk_root / name for name in (
        'components/esp_hw_support/periph_ctrl.c', 'components/hal/esp32/include/hal/clk_gate_ll.h',
        'components/soc/esp32/register/soc/dport_reg.h', 'components/esp_phy/esp32/include/phy_init_deps.h')]
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ClockTests))
    report = dict(passed=result.wasSuccessful(), tests=result.testsRun, all_functionality_verified=False,
        scope=__doc__, limitations='Aligned DPORT MMIO and reset checks, not RF calibration, Wi-Fi packet support or complete CYD acceptance.',
        specification='ESP32 TRM v5.8 sections 12.3.7 and 12.5; IDF 5.5.4 ESP32 clock-gate LL and PHY assertion.',
        failures=[dict(case=str(case),traceback=trace) for case,trace in result.failures],
        errors=[dict(case=str(case),traceback=trace) for case,trace in result.errors], inputs_sha256=hashes)
    report['inputs_unchanged'] = all(hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest for path,digest in hashes.items())
    report['passed'] = report['passed'] and report['inputs_unchanged']
    (OPTIONS.output / 'report.json').write_text(json.dumps(report,indent=2)+'\n',newline='\n')
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
