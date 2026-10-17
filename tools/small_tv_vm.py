#!/usr/bin/env python3
"""Prepare and control the ESP32-C2 Small TV VM using only Python's stdlib."""
import argparse
import datetime as dt
import hashlib
import json
import os
import re
from small_tv_network import seed_clock_network, seed_setup_network
from small_tv_web import reserve_web_ports, user_network_options
from esp_services import network_profile, service_endpoints
from esp_board import compile_profile
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import time
import threading
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
    if args.network == 'clock':
        network = network_profile(args.hardware_profile or REPO / 'configs/boards/small-tv-c2.json')
        data = seed_clock_network(data, args.firmware, state, args.yoradio,
                                  network.get('station_ssid', 'QEMU'))
    elif args.network == 'setup':
        data = seed_setup_network(data, args.firmware, state, args.yoradio)
    flash_sha = digest(data)
    flash = state / 'flash.bin'
    info_path = state / 'prepared.json'
    old = json.loads(info_path.read_text()) if info_path.exists() else None
    if args.persistent and flash.exists():
        if not old or old['prepared_flash_sha256'] != flash_sha:
            raise ValueError('Persistent flash belongs to another build; choose a new StateDirectory')
    else:
        flash.write_bytes(data)
    info = {'network': args.network, 'firmware': args.firmware, 'prepared_flash_sha256': flash_sha, 'rom': rom_info, 'sources': source_info}
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

_LAUNCH_LOCK = threading.Lock()


def launch(args, state):
    """Serialize listener allocation in-process; retry an auto-port bind race."""
    with _LAUNCH_LOCK:
        for attempt in range(8):
            try:
                return _launch(args, state)
            except RuntimeError as error:
                if (getattr(args, 'http_base_port', 0) != 0 or
                        'host forwarding rule' not in str(error) or attempt == 7):
                    raise
                time.sleep(.05)


def _launch(args, state):
    # Validate the optional loopback fixture before creating any VM state.
    network = network_profile(args.hardware_profile or REPO / 'configs/boards/small-tv-c2.json')
    service_urls = getattr(args, 'service_urls', None)
    if service_urls is None:
        service_urls = network['urls']
    service_port = getattr(args, 'service_port', 0) or getattr(args, 'weather_port', 0) or 0
    user_network_options(args.network, [], service_port, service_urls)
    services = service_endpoints(service_urls)
    if set(network['ntp_hosts']) & {row['hostname'] for row in services}:
        raise ValueError('NTP and HTTP service hostnames must be distinct')
    info = prepare(args, state)
    help_text = subprocess.check_output([args.qemu, '-machine', 'help'], text=True)
    if 'esp32c2 ' not in help_text:
        raise ValueError('QEMU has no esp32c2 machine; run setup-qemu-esp32c2-windows.ps1')
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    hardware = compile_profile(args.hardware_profile, 'esp32c2') if args.hardware_profile else None
    if hardware:
        hardware = dict(hardware, sha256=digest(Path(hardware['profile']).read_bytes()))
    name = 'SmallTV-' + uuid.uuid4().hex[:12]
    flash_path = str(state / 'flash.bin').replace(',', ',,')
    command = [args.qemu, '-M', hardware['machine'] if hardware else 'esp32c2', '-name', name, '-L', str(state),
               '-drive', f'file={flash_path},if=mtd,format=raw',
               '-display', args.display, '-serial', f'file:{state / "uart.log"}',
               '-monitor', 'none', '-icount', 'shift=3,align=off,sleep=on', '-qmp',
               f'tcp:127.0.0.1:{port},server=on,wait=off']
    if hardware:
        command += hardware['arguments']
    web_endpoints, reservations = reserve_web_ports(args.network, getattr(args, 'http_base_port', 0))
    if args.network in ('clock', 'setup'):
        command += ['-global', 'esp32c3-wifi.clock-services=on',
                    '-global', 'esp32c3-wifi.ntp-hosts=' + ';'.join(network['ntp_hosts']),
                    '-global', 'esp32c3-wifi.station-ssid=' + network.get('station_ssid', 'QEMU'),
                    '-nic', user_network_options(args.network, web_endpoints, service_port, service_urls)]
        if service_port:
            hosts = list(dict.fromkeys(row['hostname'] for row in services))
            command += ['-global', 'esp32c3-wifi.service-hosts=' + ';'.join(hosts)]
            if getattr(args, 'weather_port', 0):
                command += ['-global', 'esp32c3-wifi.weather-services=on']
        if args.network == 'setup' and network.get('softap_client_ssid'):
            command += ['-global', 'esp32c3-wifi.softap-client-ssid=' + network['softap_client_ssid']]
    else:
        command += ['-nic', 'none']
    if not args.persistent: command.append('-snapshot')
    if args.gdb_port:
        command += ['-gdb', f'tcp:127.0.0.1:{args.gdb_port}']
    if args.paused: command.append('-S')
    # Release the preflight sockets immediately before QEMU binds its own
    # listeners. A competing bind fails startup visibly, never changes an
    # explicitly requested port or redirects a browser to another VM.
    for reservation in reservations:
        reservation.close()
    with (state / 'qemu.log').open('wb') as log:
        process = subprocess.Popen(command, stdout=log, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
    session = {'name': name, 'pid': process.pid, 'qmp_port': port, 'firmware': args.firmware,
               'hardware_profile': hardware, 'network': args.network, 'persistent': args.persistent, 'qemu': args.qemu, 'started_utc': dt.datetime.now(dt.timezone.utc).isoformat()}
    session['web_endpoints'] = web_endpoints
    session['web_urls'] = {row['name']: row['url'] for row in web_endpoints}
    session['service_urls'] = [row['url'] for row in services] if service_port else []
    prefix = '192.168.43.' if args.network == 'setup' else '10.0.2.'
    session['service_endpoints'] = [dict(row, guest_address=prefix + str(row['suffix']),
                                       host_address='127.0.0.1', host_port=service_port)
                                    for row in services] if service_port else []
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

def read_guest_boot_time(state):
    session = get_session(state)
    with QMP(session['qmp_port'], session['name']) as qmp:
        output = qmp.execute('human-monitor-command', **{'command-line': 'xp /2wx 0x60008050'})
        low, high = [int(value,16) for value in re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
    return ((high << 32) | low) / 1e6

def read_guest_clock(state):
    """Read the RTC epoch offset and elapsed virtual time; never write RAM/PC.

    Both tested IDF versions store the 64-bit boot-time offset in STORE2/3.
    The radio microsecond counter supplies elapsed QEMU virtual time.
    """
    session = get_session(state)
    def words(qmp, address, count):
        output = qmp.execute('human-monitor-command', **{'command-line': f'xp /{count}wx {address:#x}'})
        return [int(value,16) for value in re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
    with QMP(session['qmp_port'], session['name']) as qmp:
        low, high = words(qmp, 0x60008050, 2)
        elapsed, = words(qmp, 0x6004d000, 1)
    return ((high << 32) | low) / 1e6 + elapsed / 1e6

def application_state_layout(args):
    """Bind read-only runtime observations to the exact archived application's ELF."""
    from esp_elf import object_symbols
    path = Path(getattr(args, 'app_elf', '') or
                Path(args.yoradio) / '.build/small-tv-esp32c2/small_tv_reconstructed.elf')
    app = Path(args.yoradio) / 'firmware/development/small-tv-esp32c2-reconstructed/app.bin'
    data = path.read_bytes()
    elf_sha = digest(data)
    if app.read_bytes()[176:208] != bytes.fromhex(elf_sha):
        raise ValueError('Runtime observation ELF does not match archived app.bin')
    sizes = {'connected': 1, 'ap_active': 1, 'last_station_address': 4, 'time_synced': 1,
             's_microseconds_offset': 8, 's_correction_us': 8}
    symbols = object_symbols(data, sizes)
    for name, row in symbols.items():
        if row['size'] != sizes[name] or not 0x3fca0000 <= row['address'] < row['address'] + row['size'] <= 0x3fce0000:
            raise ValueError('Unexpected observed C2 RAM object: ' + name)
    return {'elf_sha256': elf_sha, 'symbols': symbols,
            'converter_sha256': digest(Path(__file__).with_name('esp_elf.py').read_bytes()),
            'vm_helper_sha256': digest(Path(__file__).read_bytes())}


def application_state(qmp, layout):
    """Read actual connection/SNTP flags and DHCP bytes without touching guest state."""
    values = {}
    for name in ('connected', 'ap_active', 'last_station_address', 'time_synced'):
        row = layout['symbols'][name]
        output = qmp.execute('human-monitor-command', **{
            'command-line': f"xp /{row['size']}bx 0x{row['address']:x}"})
        data = b''.join(bytes.fromhex(value) for line in output.splitlines() if ':' in line
                        for value in re.findall(r'0x([0-9a-fA-F]{2})(?=\s|$)', line.split(':', 1)[1]))
        if len(data) != row['size']:
            raise ValueError('Short observed runtime read: ' + name)
        if name == 'last_station_address':
            values['station_address'] = '.'.join(str(value) for value in data)
        else:
            if data[0] not in (0, 1):
                raise ValueError('Invalid observed atomic flag: ' + name)
            values[name] = bool(data[0])
    return values


def factory_clock_layout(args):
    """Bind stock SDK offsets to the immutable dump and verified RV32 instructions."""
    sha = digest((Path(args.yoradio) / 'Small_TV/full-flash.bin').read_bytes())
    if sha != '5321a6b9016a8fc8335958193b4ab182e0d9ea85cfc3363063f120080c1844f4':
        raise ValueError('Factory timekeeping layout needs the verified capture')
    # esp_time_impl_get_time_since_boot at 0x420b288c adds 64-bit
    # [0x3fcc3980]; esp_system_get_time at 0x403802da adds [0x3fcc3888].
    return {'factory_sha256': sha, 'symbols': {
        's_microseconds_offset': {'address': 0x3fcc3980, 'size': 8},
        's_correction_us': {'address': 0x3fcc3888, 'size': 8}}}


def ntp_reply_observation(qmp, state, metadata_available, diagnostics, layout):
    """Compare the real SDK RTC base with the generated packet's virtual timestamp."""
    if metadata_available:
        reply = json.loads(qmp.execute('qom-get', path='/machine/wifi', property='last-ntp-reply'))
        count, epoch, sent_us = (reply[name] for name in ('count', 'unix_seconds', 'virtual_us'))
        if any(type(value) is not int or value < 0 for value in (count, epoch, sent_us)):
            raise ValueError('Malformed NTP service observation')
        if not count:
            return None
        method = 'model_response_timestamp'
    elif diagnostics:
        replies = re.findall(r'C2 NTP reply unix_seconds=(\d+) virtual_us=(\d+)',
                             (state / 'qemu.log').read_text(errors='replace'))
        if not replies:
            return None
        epoch, sent_us = map(int, replies[-1])
        count, method = len(replies), 'diagnostic_wire_timestamp'
    else:
        raise ValueError('Rebuild silent QEMU with read-only NTP timestamp support')
    def words(address, count):
        output = qmp.execute('human-monitor-command', **{'command-line': f'xp /{count}wx {address:#x}'})
        values = [int(value, 16) for value in re.findall(r'0x([0-9a-fA-F]{8})\b', output)]
        if len(values) != count:
            raise ValueError('Short SDK clock observation')
        return values

    def signed_object(name):
        low, high = words(layout['symbols'][name]['address'], 2)
        return int.from_bytes(struct.pack('<II', low, high), 'little', signed=True)

    qmp.execute('stop')
    try:
        low, high = words(0x60008050, 2)
        boot_us = (high << 32) | low
        virtual_us, = words(0x6004d000, 1)
        high, low = words(0x60023040, 2)
        ticks = (high << 32) | low
        offset, correction = signed_object('s_microseconds_offset'), signed_object('s_correction_us')
    finally:
        qmp.execute('cont')
    # C2 / 26 MHz: the SDK's ticks_to_us converter is ticks * 5 / 52.
    # Read its own latched UNIT0 value; do not trigger a hardware snapshot.
    since_boot_us = ticks * 5 // 52 + offset + correction
    actual = (boot_us + since_boot_us) / 1e6
    expected = epoch + (virtual_us - sent_us) / 1e6
    return {'method': method, 'response_count': count, 'server_epoch': epoch, 'sent_virtual_us': sent_us,
            'boot_time_us': boot_us, 'virtual_us': virtual_us, 'systimer_ticks': ticks,
            'rtc_hrt_offset_us': offset, 'system_correction_us': correction,
            'since_boot_us': since_boot_us, 'actual_utc_seconds': actual,
            'expected_utc_seconds': expected, 'difference_seconds': actual - expected}


def test_vm(args, state):
    args.display, args.persistent, args.paused = 'none', False, False
    # Reject a wrong/missing observation ELF before starting a disposable VM.
    layout = application_state_layout(args) if args.firmware == 'reconstructed' else None
    clock_layout = layout or factory_clock_layout(args)
    runtime = after_reset_runtime = None
    diagnostics, reply_metadata = True, False
    ntp_observation = reset_ntp_observation = None
    session, prepared = launch(args, state)
    before = digest((state / 'flash.bin').read_bytes())
    screenshots = []
    clock = None
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
                time.sleep(.5)
            registers = qmp.execute('human-monitor-command', **{'command-line': 'info registers'})
            (state / 'registers.txt').write_text(registers)
            status = qmp.execute('query-status')
            properties = qmp.execute('qom-list', path='/machine/wifi')
            reply_metadata = any(row['name'] == 'last-ntp-reply' for row in properties)
            if any(row['name'] == 'debug-logging' for row in properties):
                diagnostics = qmp.execute('qom-get', path='/machine/wifi', property='debug-logging')
            if layout:
                runtime = application_state(qmp, layout)
        uart = (state / 'uart.log').read_text(errors='replace')
        checks = {
            'machine_still_running': status['running'],
            'rom_and_bootloader': 'ESP-ROM:esp32c2' in uart and 'boot: Loaded app' in uart,
            'lcd_240x240': True,
            'lcd_has_content': max(sum(p != 0 for p in frame) for frame in screenshots) > 1000,
            'no_panic_or_watchdog': not any(x in uart for x in ('Guru Meditation', 'abort() was called', 'watchdog got triggered', 'assert failed')),
        }
        if args.firmware == 'reconstructed':
            # A real rendered application plus SDK-owned network state proves
            # readiness even when INFO and the final app_main message are absent.
            checks['rom_and_bootloader'] = ('ESP-ROM:esp32c2' in uart and
                'load:0x' in uart and 'entry 0x' in uart and checks['lcd_has_content'])
            checks['application_runtime_ready'] = checks['lcd_has_content'] and (
                runtime['connected'] if args.network == 'clock' else runtime['ap_active'])
            if args.network == 'offline':
                checks['ap_started'] = runtime['ap_active']
            # Offline/setup displays a static configuration page. Only the
            # synchronized clock scene must change its animation region.
            if args.network == 'clock':
                region = lambda frame: frame[143*240*3:224*240*3]
                checks['animation_updates'] = len({digest(region(f)) for f in screenshots}) > 1
        clock = None
        if args.network == 'clock':
            clock = read_guest_clock(state)
            with QMP(session['qmp_port'], session['name']) as qmp:
                ntp_observation = ntp_reply_observation(qmp, state, reply_metadata, diagnostics, clock_layout)
            checks['ntp_reply_sent'] = ntp_observation is not None
            checks['guest_clock_set_by_sntp'] = (ntp_observation is not None and
                abs(ntp_observation['difference_seconds']) < 1)
            if args.firmware == 'reconstructed':
                checks['wifi_associated'] = runtime['connected']
                checks['station_dhcp'] = runtime['station_address'] == '10.0.2.15'
                checks['sntp_notification_received'] = runtime['time_synced']
        # Reset the VM and verify the guest can boot and draw a second time.
        with QMP(session['qmp_port'], session['name']) as qmp: qmp.execute('system_reset')
        time.sleep(8)
        with QMP(session['qmp_port'], session['name']) as qmp:
            qmp.execute('screendump', filename=str(state / 'after-reset.ppm'))
            if args.network == 'clock':
                reset_ntp_observation = ntp_reply_observation(qmp, state, reply_metadata, diagnostics, clock_layout)
                checks['clock_recovered_after_reset'] = (reset_ntp_observation is not None and
                    abs(reset_ntp_observation['difference_seconds']) < 1 and
                    ntp_observation is not None and reset_ntp_observation['response_count'] > ntp_observation['response_count'])
            if layout:
                after_reset_runtime = application_state(qmp, layout)
                checks['runtime_ready_after_reset'] = (
                    after_reset_runtime['connected'] and after_reset_runtime['time_synced'] and
                    after_reset_runtime['station_address'] == '10.0.2.15'
                    if args.network == 'clock' else after_reset_runtime['ap_active'])
        uart2 = (state / 'uart.log').read_text(errors='replace')
        checks['no_panic_after_reset'] = 'Guru Meditation' not in uart2 and 'assert failed' not in uart2
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
    hardware = json.loads((state / 'session.json').read_text()).get('hardware_profile') if (state / 'session.json').exists() else None
    report = {'hardware_profile': hardware, 'firmware': args.firmware, 'passed': all(checks.values()), 'checks': checks,
              'duration_seconds': args.seconds, 'network': args.network, 'guest_clock_unix': clock,
              'original_expected_screen': ('Clock with synchronized date/time' if args.network == 'clock' else 'Connecting to WiFi / Ver:0.0.2, then Small_TV / 192.168.43.1 setup instructions'),
              'prepared': prepared, 'error': error, 'qemu_sha256': digest(Path(args.qemu).read_bytes()),
              'runtime_observation': {'layout': layout, 'before_reset': runtime, 'after_reset': after_reset_runtime},
              'clock_layout': clock_layout, 'ntp_observation': ntp_observation, 'reset_ntp_observation': reset_ntp_observation,
              'qemu_diagnostics_enabled': diagnostics,
              'tested_at_utc': dt.datetime.now(dt.timezone.utc).isoformat()}
    save_json(state / 'test-report.json', report)
    return report

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['prepare', 'run', 'status', 'stop', 'reset', 'screenshot', 'test'])
    parser.add_argument('--firmware', choices=['original', 'reconstructed'], default='reconstructed')
    parser.add_argument('--yoradio', default='')
    parser.add_argument('--rom-elf', default='')
    parser.add_argument('--app-elf', default='', help='Matching native application ELF for read-only boot checks')
    parser.add_argument('--qemu', default='')
    parser.add_argument('--state', required=True)
    parser.add_argument('--display', choices=['none', 'sdl'], default='sdl')
    parser.add_argument('--hardware-profile', default='')
    parser.add_argument('--network', choices=['clock','setup','offline'], default='clock')
    parser.add_argument('--http-base-port', type=int, default=0, help='First consecutive localhost HTTP port; 0 auto, -1 disable')
    parser.add_argument('--service-port', type=int, default=0, help='Loopback fixture port for the profile URL list; 0 disables')
    parser.add_argument('--service-url', dest='service_urls', action='append', help='Guest HTTP(S) URL; repeat to override the device profile URL list')
    parser.add_argument('--weather-port', type=int, help=argparse.SUPPRESS)
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
