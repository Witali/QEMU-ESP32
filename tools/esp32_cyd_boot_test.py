#!/usr/bin/env python3
"""Observe a real CYD image through matching boot stages; reject stalled startup."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import uuid


def main():
    """Require AP/server startup and panel pixels, retaining the first boot failure."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--bios', type=Path, required=True)
    parser.add_argument('--flash', type=Path, required=True)
    parser.add_argument('--max-seconds', type=float, default=180)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / 'tools'))
    from small_tv_vm import QMP
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    image = out / 'flash.bin'
    image.write_bytes(args.flash.read_bytes())
    inputs = [Path(__file__), args.qemu, args.flash, root / 'tools/small_tv_vm.py']
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    with socket.socket() as server:
        server.bind(('127.0.0.1', 0))
        qmp_port = server.getsockname()[1]
    name = 'cyd-stage-' + uuid.uuid4().hex[:12]
    command = [str(args.qemu.resolve()), '-M', 'esp32,st7789=on,sdspi=on', '-L', str(args.bios.resolve()),
        '-drive', f'file={image},if=mtd,format=raw', '-snapshot', '-nic', 'none', '-name', name,
        '-display', 'none', '-monitor', 'none', '-serial', f'file:{out / "uart.log"}',
        '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off']
    report = dict(passed=False, all_functionality_verified=False, scope=__doc__,
        limitations='Bounded AP/server and framebuffer startup checkpoint with NIC disabled, not RF accuracy, station networking or full board acceptance.',
        inputs_sha256=hashes, command=command, milestones={}, outcome='starting')
    monitor = None
    log = (out / 'qemu.log').open('wb')
    process = subprocess.Popen(command, stdout=log, stderr=log,
        creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
    report['pid'] = process.pid
    started = time.monotonic()
    try:
        deadline = started + 15
        while True:
            try:
                monitor = QMP(qmp_port, name)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError((out / 'qemu.log').read_text(errors='replace'))
                time.sleep(.02)
        ready_at = None
        markers = dict(rom_entry=b'entry 0x400805b4', app_banner=b'##[BOOT]#',
            display_init=b'display.init', network_begin=b'network.begin',
            phy_assert=b'phy_init.c:332', panic=b'Guru Meditation',
            ap_started=b'Running in AP mode', server_started=b'netserver.begin')
        while True:
            elapsed = time.monotonic() - started
            uart = (out / 'uart.log').read_bytes() if (out / 'uart.log').exists() else b''
            for key, marker in markers.items():
                if marker in uart and key not in report['milestones']:
                    report['milestones'][key] = round(elapsed, 3)
                    print(key, report['milestones'][key], flush=True)
            if b'assert failed:' in uart or b'Guru Meditation' in uart or b'abort() was called' in uart:
                report['outcome'] = 'boot_failure'
                break
            if ('ap_started' in report['milestones'] or 'server_started' in report['milestones']) and ready_at is None:
                ready_at = elapsed
            if ready_at is not None and elapsed - ready_at >= 10:
                report['outcome'] = 'application_ready_window'
                break
            if elapsed >= args.max_seconds:
                report['outcome'] = 'required_boot_stage_not_reached'
                break
            assert process.poll() is None, 'Owned QEMU exited before reaching the required stage'
            time.sleep(.05)
        monitor.execute('screendump', filename=str(out / 'screen.ppm'))
        pixels = (out / 'screen.ppm').read_bytes().split(b'\n', 3)
        width, height = map(int, pixels[1].split())
        panel = pixels[3][:width * min(240, height) * 3]
        report['panel_nonzero_bytes'] = sum(value != 0 for value in panel)
        report['framebuffer_sha256'] = hashlib.sha256(pixels[3]).hexdigest()
        report['dimensions'] = [width, height]
        report['passed'] = report['outcome'] == 'application_ready_window' and report['panel_nonzero_bytes'] > 1000
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        if monitor is not None and process.poll() is None:
            try:
                monitor.execute('quit')
            except OSError:
                pass
        if process.poll() is None:
            process.terminate()
        process.wait(timeout=5)
        if monitor is not None:
            monitor.close()
        log.close()
        report['wall_seconds'] = round(time.monotonic() - started, 3)
        report['inputs_unchanged'] = all(hashlib.sha256(Path(path).read_bytes()).hexdigest() == digest for path,digest in hashes.items())
        report['backing_flash_unchanged'] = image.read_bytes() == args.flash.read_bytes()
        report['passed'] = report['passed'] and report['inputs_unchanged'] and report['backing_flash_unchanged']
        (out / 'report.json').write_text(json.dumps(report,indent=2)+'\n',newline='\n')
    print(json.dumps({key:value for key,value in report.items() if key not in ('inputs_sha256','command')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
