#!/usr/bin/env python3
"""Boot unchanged CYD and C3 OLED images and check legacy QEMU displays."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import time

from small_tv_vm import QMP, save_json
from esp_board import compile_profile


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def cyd_application_checks(uart, pixels, width, height):
    """Reject ROM-only/console-only startup as evidence of a working CYD app."""
    # FIXED_TEST: the old nonzero count included the UART area below the panel.
    # network.begin is printed before PHY init and does not establish readiness.
    return {
        'application_startup_reached': any(marker in uart for marker in
            ('Running in AP mode', 'netserver.begin')),
        'lcd_panel_has_content': sum(value != 0 for value in pixels[:width * min(height, 240) * 3]) > 1000,
        'framebuffer_size_valid': len(pixels) == width * height * 3,
    }


def run_guest(exe, build, machine, state, rom, seconds, profile=None):
    state.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((build / 'flasher_args.json').read_text())
    flash = bytearray(b'\xff' * (4 * 1024 * 1024))
    sources = {}
    for offset, name in manifest['flash_files'].items():
        data = (build / name).read_bytes()
        start = int(offset, 0)
        if start < 0 or start + len(data) > len(flash):
            raise ValueError('Firmware component is outside the 4 MiB image')
        flash[start:start + len(data)] = data
        sources[name] = sha(build / name)
    image = state / 'flash.bin'
    image.write_bytes(flash)
    before = sha(image)
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    command = [str(exe), '-M', machine, '-L', str(rom), '-drive',
               f'file={str(image).replace(",", ",,")},if=mtd,format=raw',
               '-snapshot', '-nic', 'none', '-display', 'none', '-monitor',
               'none', '-serial', f'file:{state / "uart.log"}', '-qmp',
               f'tcp:127.0.0.1:{port},server=on,wait=off']
    hardware = compile_profile(profile, 'esp32c3') if profile else None
    if hardware:
        command[2] = hardware['machine']
        command += hardware['arguments']
    with (state / 'qemu.log').open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
    connected = False
    try:
        for attempt in range(100):
            if process.poll() is not None:
                raise RuntimeError((state / 'qemu.log').read_text(errors='replace'))
            try:
                with QMP(port) as qmp:
                    qmp.execute('query-status')
                connected = True
                break
            except OSError:
                time.sleep(.1)
        if not connected:
            raise RuntimeError('QMP startup timeout')
        time.sleep(seconds)
        with QMP(port) as qmp:
            status = qmp.execute('query-status')
            qmp.execute('screendump', filename=str(state / 'screen.ppm'))
        parts = (state / 'screen.ppm').read_bytes().split(b'\n', 3)
        width, height = map(int, parts[1].split())
        pixels = parts[3]
        uart = (state / 'uart.log').read_text(errors='replace')
        checks = {
            'running': status['running'],
            'console_has_content': sum(value != 0 for value in pixels) > 1000,
            'display_dimensions': (width, height) == ((320, 284) if machine.startswith('esp32,') else (288, 160)),
            'no_panic_or_watchdog': not any(text in uart for text in ('Guru Meditation', 'abort()', 'watchdog got triggered', 'assert failed:', "panic'ed", 'Interrupt wdt timeout')),
            'source_flash_unchanged': sha(image) == before,
            'source_artifacts_unchanged': all(sha(build / name) == value for name, value in sources.items()),
        }
        if machine.split(',')[0] == 'esp32':
            checks.update(cyd_application_checks(uart, pixels, width, height))
        report = {'passed': all(checks.values()), 'checks': checks,
                  'hardware_profile': hardware,
                  'machine': machine, 'duration_seconds': seconds,
                  'qemu_sha256': sha(exe), 'firmware_sources': sources,
                  'pixel_sha256': hashlib.sha256(pixels).hexdigest(),
                  'nonzero_bytes': sum(value != 0 for value in pixels),
                  'lcd_area_nonzero_bytes': sum(value != 0 for value in pixels[:width * min(height, 240) * 3]),
                  'cyd_boot_scope': 'Bounded application/panel startup and panic checks; RF/networking functionality needs separate evidence' if machine.startswith('esp32,') else None}
        save_json(state / 'report.json', report)
        return report
    finally:
        if process.poll() is None:
            if connected:
                with QMP(port) as qmp:
                    qmp.execute('quit')
            else:
                process.terminate()
            process.wait(timeout=5)


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cyd-build', required=True)
    parser.add_argument('--c3-build', required=True)
    parser.add_argument('--qemu-xtensa', default=str(root / 'bin/qemu-system-xtensa.exe'))
    parser.add_argument('--qemu-riscv', default=str(root / 'bin/qemu-system-riscv32.exe'))
    parser.add_argument('--baseline-xtensa', help='Optional previous executable for CYD comparison')
    parser.add_argument('--c3-profile', help='Also boot the C3 firmware using this modular profile')
    parser.add_argument('--seconds', type=int, default=25)
    parser.add_argument('--state-directory', default=str(root / '.cache/esp-boards/legacy-tests'))
    args = parser.parse_args()
    state = Path(args.state_directory).resolve()
    reports = {}
    if args.baseline_xtensa:
        reports['cyd_before'] = run_guest(Path(args.baseline_xtensa).resolve(), Path(args.cyd_build).resolve(),
            'esp32,st7789=on,sdspi=on', state / 'cyd-before', root / 'pc-bios', args.seconds)
    reports['cyd'] = run_guest(Path(args.qemu_xtensa).resolve(), Path(args.cyd_build).resolve(),
        'esp32,st7789=on,sdspi=on', state / 'cyd', root / 'pc-bios', args.seconds)
    reports['c3_oled'] = run_guest(Path(args.qemu_riscv).resolve(), Path(args.c3_build).resolve(),
        'esp32c3', state / 'c3-oled', root / 'pc-bios', args.seconds)
    if args.c3_profile:
        reports['c3_modular'] = run_guest(Path(args.qemu_riscv).resolve(), Path(args.c3_build).resolve(),
            'esp32c3', state / 'c3-modular', root / 'pc-bios', args.seconds, args.c3_profile)
    if 'cyd_before' in reports:
        reports['cyd_pixels_match_baseline'] = reports['cyd']['pixel_sha256'] == reports['cyd_before']['pixel_sha256']
    reports['passed'] = all(value['passed'] for value in reports.values() if isinstance(value, dict))
    save_json(state / 'legacy-report.json', reports)
    print(json.dumps(reports, indent=2))
    if not reports['passed']:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
