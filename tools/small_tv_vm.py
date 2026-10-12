#!/usr/bin/env python3
"""Prepare and control the ESP32-C2 Small TV VM using only Python's stdlib."""
import argparse
import datetime as dt
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
import uuid
import zlib

FLASH_SIZE = 4 * 1024 * 1024
ROM_BASE = 0x40000000
ROM_SIZE = 0x90000
REPO = Path(__file__).resolve().parents[1]

def digest(data):
    return hashlib.sha256(data).hexdigest()

def save_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')

def flatten_rom(path):
    data = path.read_bytes()
    if len(data) < 52 or data[:7] != b'\x7fELF\x01\x01\x01':
        raise ValueError('ROM must be a little-endian ELF32 file')
    fields = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
    if fields[1] != 243 or fields[3] != ROM_BASE:
        raise ValueError('ROM must be the ESP32-C2 RISC-V ROM with entry 0x40000000')
    phoff, phsize, phcount = fields[4], fields[8], fields[9]
    output = bytearray(b'\xff' * ROM_SIZE)
    loaded = []
    for i in range(phcount):
        kind, offset, vaddr, paddr, size, memsize, flags, align = struct.unpack_from('<IIIIIIII', data, phoff + i * phsize)
        if kind != 1 or not size or not ROM_BASE <= paddr < ROM_BASE + ROM_SIZE:
            continue
        start = paddr - ROM_BASE
        if start + size > ROM_SIZE or offset + size > len(data):
            raise ValueError('ROM PT_LOAD segment exceeds its memory/file bounds')
        output[start:start + size] = data[offset:offset + size]
        loaded.append({'physical_address': hex(paddr), 'bytes': size})
    if not loaded or output[:16] == b'\xff' * 16:
        raise ValueError('ELF has no executable C2 ROM payload')
    return output, {'elf_sha256': digest(data), 'binary_sha256': digest(output), 'segments': loaded}

def check_flash(data):
    if len(data) != FLASH_SIZE:
        raise ValueError('Small TV requires a complete 4 MiB flash image')
    for offset in (0, 0x10000):
        if data[offset] != 0xe9 or struct.unpack_from('<H', data, offset + 12)[0] != 12:
            raise ValueError(f'Expected an ESP32-C2 image header at {offset:#x}')
    if data[0x8000:0x8002] != b'\xaa\x50':
        raise ValueError('Partition table is missing at 0x8000')

def prepare(args, state):
    ensure_stopped(state)
    state.mkdir(parents=True, exist_ok=True)
    rom_path = Path(args.rom_elf)
    rom, rom_info = flatten_rom(rom_path)
    (state / 'esp32c2-rev200-rom.bin').write_bytes(rom)
    if args.firmware == 'original':
        source = Path(args.yoradio) / 'Small_TV/full-flash.bin'
        data = source.read_bytes()
        manifest = json.loads((source.parent / 'manifest.json').read_text(encoding='utf-8-sig'))
        if digest(data) != manifest['sha256']:
            raise ValueError('Original flash dump SHA256 differs from its capture manifest')
        source_info = {'full_flash_sha256': digest(data)}
    else:
        folder = Path(args.yoradio) / 'firmware/development/small-tv-esp32c2-reconstructed'
        manifest = json.loads((folder / 'manifest.json').read_text(encoding='utf-8-sig'))
        if manifest['target'] != 'esp32c2' or manifest['flash_bytes'] != FLASH_SIZE:
            raise ValueError('Reconstructed firmware manifest is not a 4 MiB C2 build')
        data = bytearray(b'\xff' * FLASH_SIZE)
        regions = []
        for entry in manifest['images']:
            name = entry['file']
            if Path(name).name != name:
                raise ValueError('Firmware image names must be basenames')
            image = (folder / name).read_bytes()
            start = int(entry['offset'], 0)
            end = start + len(image)
            if digest(image) != entry['sha256'] or len(image) != entry['bytes']:
                raise ValueError(f'Firmware archive checksum/size mismatch: {name}')
            if start < 0 or end > FLASH_SIZE or any(start < b and end > a for a, b in regions):
                raise ValueError(f'Invalid/overlapping flash region: {name}')
            regions.append((start, end))
            data[start:end] = image
        source_info = {'images': manifest['images'], 'source_sha256': manifest['source_sha256']}
    check_flash(data)
    flash_sha = digest(data)
    flash = state / 'flash.bin'
    info_path = state / 'prepared.json'
    old = json.loads(info_path.read_text()) if info_path.exists() else None
    if args.persistent and flash.exists():
        if not old or old['prepared_flash_sha256'] != flash_sha:
            raise ValueError('Persistent flash belongs to another build; choose a new StateDirectory')
    else:
        flash.write_bytes(data)
    info = {'firmware': args.firmware, 'prepared_flash_sha256': flash_sha, 'rom': rom_info, 'sources': source_info}
    save_json(info_path, info)
    return info

class QMP:
    def __init__(self, port, name=None):
        self.sock = socket.create_connection(('127.0.0.1', port), 3)
        self.sock.settimeout(5)
        self.file = self.sock.makefile('rwb')
        greeting = json.loads(self.file.readline())
        if 'QMP' not in greeting:
            raise RuntimeError('Invalid QMP greeting')
        self.execute('qmp_capabilities')
        if name and self.execute('query-name').get('name') != name:
            self.close()
            raise RuntimeError('QMP endpoint belongs to a different VM')

    def execute(self, command, **arguments):
        self.file.write((json.dumps({'execute': command, 'arguments': arguments}) + '\n').encode())
        self.file.flush()
        while True:
            response = json.loads(self.file.readline())
            if 'event' in response:
                continue
            if 'error' in response:
                raise RuntimeError(response['error'])
            return response['return']

    def close(self):
        self.file.close()
        self.sock.close()

    def __enter__(self): return self
    def __exit__(self, *unused): self.close()

def get_session(state):
    return json.loads((state / 'session.json').read_text())

def ensure_stopped(state):
    if (state / 'session.json').exists():
        session = get_session(state)
        try:
            with QMP(session['qmp_port'], session['name']) as qmp:
                qmp.execute('query-status')
        except OSError:
            pass
        else:
            raise RuntimeError('A VM is already using this state directory; stop it first')

def launch(args, state):
    info = prepare(args, state)
    help_text = subprocess.check_output([args.qemu, '-machine', 'help'], text=True)
    if 'esp32c2 ' not in help_text:
        raise ValueError('QEMU has no esp32c2 machine; run setup-qemu-esp32c2-windows.ps1')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    name = 'SmallTV-' + uuid.uuid4().hex[:12]
    flash_path = str(state / 'flash.bin').replace(',', ',,')
    command = [args.qemu, '-M', 'esp32c2', '-name', name, '-L', str(state),
               '-drive', f'file={flash_path},if=mtd,format=raw',
               '-display', args.display, '-serial', f'file:{state / "uart.log"}',
               '-monitor', 'none', '-nic', 'none', '-qmp',
               f'tcp:127.0.0.1:{port},server=on,wait=off']
    if not args.persistent: command.append('-snapshot')
    if args.gdb_port:
        command += ['-gdb', f'tcp:127.0.0.1:{args.gdb_port}']
    if args.paused: command.append('-S')
    with (state / 'qemu.log').open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    session = {'name': name, 'pid': process.pid, 'qmp_port': port, 'firmware': args.firmware,
               'persistent': args.persistent, 'qemu': args.qemu, 'started_utc': dt.datetime.now(dt.timezone.utc).isoformat()}
    save_json(state / 'session.json', session)
    for attempt in range(50):
        if process.poll() is not None:
            raise RuntimeError('QEMU exited during startup: ' + (state / 'qemu.log').read_text(errors='replace')[-2000:])
        try:
            with QMP(port, name) as qmp: qmp.execute('query-status')
            break
        except OSError:
            time.sleep(.1)
    else:
        process.terminate()
        raise RuntimeError('QMP startup timed out')
    return session, info

def control(state, action, screenshot=None):
    session = get_session(state)
    with QMP(session['qmp_port'], session['name']) as qmp:
        if action == 'stop': return qmp.execute('quit')
        if action == 'reset': return qmp.execute('system_reset')
        if action == 'screenshot':
            path = Path(screenshot or state / 'screen.png').resolve()
            capture_png(qmp, path)
            return {'screenshot': str(path)}
        return {'session': session, 'status': qmp.execute('query-status')}

def ppm_pixels(path):
    data = path.read_bytes()
    header, width_height, maximum, pixels = data.split(b'\n', 3)
    if header != b'P6' or maximum != b'255' or width_height != b'240 240' or len(pixels) != 240*240*3:
        raise AssertionError('Unexpected LCD framebuffer dimensions/format')
    return pixels

def capture_png(qmp, path):
    """QEMU's PPM capture works even when the Windows build has no libpng."""
    ppm = path.with_suffix('.ppm')
    qmp.execute('screendump', filename=str(ppm))
    pixels = ppm_pixels(ppm)
    def chunk(kind, payload):
        return struct.pack('>I', len(payload)) + kind + payload + struct.pack('>I', zlib.crc32(kind + payload))
    rows = b''.join(b'\0' + pixels[y*720:(y+1)*720] for y in range(240))
    encoded = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240,240,8,2,0,0,0))
    encoded += chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b'')
    path.write_bytes(encoded)

def test_vm(args, state):
    args.display, args.persistent, args.paused = 'none', False, False
    session, prepared = launch(args, state)
    before = digest((state / 'flash.bin').read_bytes())
    screenshots = []
    error = None
    try:
        with QMP(session['qmp_port'], session['name']) as qmp:
            qmp.execute('screendump', filename=str(state / 'reset.ppm'))
        until = time.monotonic() + args.seconds
        while time.monotonic() < until:
            time.sleep(min(1, max(0, until - time.monotonic())))
        with QMP(session['qmp_port'], session['name']) as qmp:
            for i in range(3):
                ppm = state / f'frame-{i}.ppm'
                png = state / f'frame-{i}.png'
                qmp.execute('screendump', filename=str(ppm))
                capture_png(qmp, png)
                screenshots.append(ppm_pixels(ppm))
                time.sleep(.25)
            registers = qmp.execute('human-monitor-command', **{'command-line': 'info registers'})
            (state / 'registers.txt').write_text(registers)
            status = qmp.execute('query-status')
        uart = (state / 'uart.log').read_text(errors='replace')
        checks = {
            'machine_still_running': status['running'],
            'rom_and_bootloader': 'ESP-ROM:esp32c2' in uart and 'boot: Loaded app' in uart,
            'lcd_240x240': True,
            'lcd_has_content': max(sum(p != 0 for p in frame) for frame in screenshots) > 1000,
            'no_panic_or_watchdog': not any(x in uart for x in ('Guru Meditation', 'abort() was called', 'watchdog got triggered', 'assert failed')),
        }
        if args.firmware == 'reconstructed':
            checks['app_main_completed'] = 'Reconstructed ESP32-C2 clock started' in uart and 'Returned from app_main' in uart
            checks['ap_started'] = 'DHCP server started' in uart
            region = lambda frame: frame[143*240*3:224*240*3]
            checks['animation_updates'] = len({digest(region(f)) for f in screenshots}) > 1
        # Reset the VM and verify the guest can boot and draw a second time.
        with QMP(session['qmp_port'], session['name']) as qmp: qmp.execute('system_reset')
        time.sleep(3)
        with QMP(session['qmp_port'], session['name']) as qmp:
            qmp.execute('screendump', filename=str(state / 'after-reset.ppm'))
        uart2 = (state / 'uart.log').read_text(errors='replace')
        checks['reset_reboots'] = uart2.count('ESP-ROM:esp32c2') >= 2 and sum(p != 0 for p in ppm_pixels(state / 'after-reset.ppm')) > 1000
    except Exception as exc:
        checks = {'test_completed': False}
        error = str(exc)
    finally:
        try:
            control(state, 'stop')
        except (OSError, RuntimeError):
            checks['vm_exited_unexpectedly'] = False
    checks['source_flash_unchanged'] = digest((state / 'flash.bin').read_bytes()) == before
    if args.firmware == 'original':
        checks['source_artifacts_unchanged'] = digest((Path(args.yoradio) / 'Small_TV/full-flash.bin').read_bytes()) == prepared['sources']['full_flash_sha256']
    else:
        folder = Path(args.yoradio) / 'firmware/development/small-tv-esp32c2-reconstructed'
        checks['source_artifacts_unchanged'] = all(digest((folder / e['file']).read_bytes()) == e['sha256'] for e in prepared['sources']['images'])
    report = {'firmware': args.firmware, 'passed': all(checks.values()), 'checks': checks,
              'duration_seconds': args.seconds, 'network': 'offline; C2 Wi-Fi packet transport is not implemented',
              'original_expected_screen': 'Connecting to WiFi; no simulated AP is available',
              'prepared': prepared, 'error': error, 'qemu_sha256': digest(Path(args.qemu).read_bytes()),
              'tested_at_utc': dt.datetime.now(dt.timezone.utc).isoformat()}
    save_json(state / 'test-report.json', report)
    return report

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'run', 'status', 'stop', 'reset', 'screenshot', 'test'])
    parser.add_argument('--firmware', choices=['original', 'reconstructed'], default='reconstructed')
    parser.add_argument('--yoradio', default='')
    parser.add_argument('--rom-elf', default='')
    parser.add_argument('--qemu', default='')
    parser.add_argument('--state', required=True)
    parser.add_argument('--display', choices=['none', 'sdl'], default='sdl')
    parser.add_argument('--persistent', action='store_true')
    parser.add_argument('--paused', action='store_true')
    parser.add_argument('--gdb-port', type=int, default=0)
    parser.add_argument('--seconds', type=int, default=30)
    parser.add_argument('--screenshot')
    args = parser.parse_args()
    if args.action in ('prepare','run','test'):
        if not args.yoradio or not args.rom_elf or not args.qemu:
            parser.error('prepare/run/test require --yoradio, --rom-elf and --qemu')
        args.qemu = str(Path(args.qemu).resolve())
    if not 0 <= args.gdb_port <= 65535 or not 5 <= args.seconds <= 3600:
        parser.error('GDB port must be 0..65535 and test duration 5..3600 seconds')
    state = Path(args.state).resolve()
    if args.action == 'run': result = {'session': launch(args, state)[0], 'state_directory': str(state)}
    elif args.action == 'prepare': result = prepare(args, state)
    elif args.action == 'test': result = test_vm(args, state)
    else: result = control(state, args.action, args.screenshot)
    visible = result
    if args.action == 'test': visible = {k: result[k] for k in ('firmware','passed','checks','error')}
    if args.action in ('test','prepare'):
        visible = dict(visible, report=str(state / ('test-report.json' if args.action == 'test' else 'prepared.json')))
    print(json.dumps(visible, indent=2), flush=True)
    return 1 if args.action == 'test' and not result['passed'] else 0

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, struct.error) as exc:
        raise SystemExit(str(exc))
