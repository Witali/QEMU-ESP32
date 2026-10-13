"""Exercise actual Small TV firmware HTTP over QEMU's localhost SLIRP forwards."""
import argparse
import concurrent.futures
import datetime as dt
import hashlib
from http import client as http_client
import json
from pathlib import Path
import struct
import time
from types import SimpleNamespace
from urllib.parse import urlsplit
import uuid
import small_tv_vm as vm


def http(url, path='/', payload=None, timeout=5, origin=None, compact=True):
    """Send direct loopback HTTP, with no environment proxy and bounded response memory."""
    endpoint = urlsplit(url)
    if endpoint.hostname != '127.0.0.1': raise ValueError('Only loopback test endpoints are allowed')
    connection = http_client.HTTPConnection(endpoint.hostname, endpoint.port, timeout=timeout)
    # Factory /configure searches literal JSON.stringify fields rather than
    # parsing JSON; the native handler also accepts ordinary whitespace.
    separators = (',', ':') if compact else (', ', ': ')
    body = json.dumps(payload, separators=separators).encode() if payload is not None else None
    headers = {'Content-Type': 'application/json', 'Connection': 'close'}
    if origin is not None: headers['Origin'] = origin
    try:
        connection.request('POST' if payload is not None else 'GET', path, body, headers)
        response = connection.getresponse()
        data = response.read(262145)
        if len(data) > 262144: raise ValueError('Guest HTTP response exceeds the test limit')
        return response.status, data
    finally:
        connection.close()


def wait_http(url, path='/', expected=None, seconds=90):
    """Wait through guest startup/reboot; never accept a different page or stale value."""
    deadline = time.monotonic() + seconds
    last = None
    while time.monotonic() < deadline:
        try:
            status, body = http(url, path, timeout=2)
            if status == 200 and (expected is None or expected in body): return body
            last = f'HTTP {status}'
        except (OSError, http_client.HTTPException) as error:
            last = type(error).__name__
        time.sleep(.2)
    raise TimeoutError(f'Guest HTTP did not become ready: {url}{path}; {last}')


def wait_for(predicate, message, seconds=90):
    """Bound every asynchronous state transition by wall time."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate(): return
        time.sleep(.2)
    raise TimeoutError(message)


def clean_uart(folder):
    """Check each boot before another launch truncates its UART log."""
    uart = (folder / 'uart.log').read_text(errors='replace')
    return not any(word in uart for word in ('Guru Meditation','assert failed','abort() was called','watchdog got triggered'))


def source_hashes(options):
    """Keep immutable factory/archive input identities separate from writable VM flash."""
    repo = Path(options.yoradio)
    files = [repo / 'Small_TV/full-flash.bin']
    archive = repo / 'firmware/development/small-tv-esp32c2-reconstructed'
    manifest = json.loads((archive / 'manifest.json').read_text(encoding='utf-8-sig'))
    files += [archive / row['file'] for row in manifest['images']]
    return {str(path): vm.digest(path.read_bytes()) for path in files}


def configuration_flow(options, folder):
    """Cold setup -> real scan/POST -> software reboot/STA -> persistent relaunch/reset."""
    options = SimpleNamespace(**{**vars(options), 'network':'setup', 'persistent':True})
    initial_hashes = source_hashes(options)
    session = None
    checks = {}
    result = {'firmware': options.firmware, 'passed': False, 'checks': checks}
    def check(name, value):
        checks[name] = bool(value)
        if not value: raise AssertionError(f'{options.firmware}: {name}')
    try:
        session, prepared = vm.launch(options, folder)
        result['sources'] = prepared['sources']
        result['web_endpoints'] = session['web_endpoints']
        urls = session['web_urls']
        check('consecutive_separate_loopback_ports', session['web_endpoints'][1]['host_port'] == session['web_endpoints'][0]['host_port'] + 1 and len(set(urls.values())) == 2)
        raw = (folder / 'flash.bin').read_bytes()
        check('initial_credentials_empty_setup_mode', raw[0x3e7004] == 0 and struct.unpack_from('<I', raw, 0x3e7160)[0] == 2)
        page = wait_http(urls['setup'], expected=b'/configure')
        check('real_setup_html', len(page) > 1000)
        if options.firmware == 'reconstructed':
            status, body = http(urls['setup'], '/scan', timeout=20)
            records = json.loads(body) if status == 200 else []
            check('real_wifi_scan', status == 200 and any(row.get('ssid') == 'yoRadio-QEMU' for row in records))
        else:
            # The unchanged vendor handler dereferences malloc(0) when scan
            # yields no records (0x4200f484). The AP firmware can still be
            # configured by entering an SSID manually; do not call that bug
            # a passing scan test or modify its executable to hide it.
            result['scan'] = {'status':'not_run_in_factory_setup_flow',
                              'reason':'Confirmed vendor /scan zero-result null dereference at 0x4200f484; native scan is a required check'}
        if options.firmware == 'reconstructed':
            status, _ = http(urls['setup'], '/configure', {'ssid':'yoRadio-QEMU','password':'short'})
            check('invalid_credentials_rejected', status == 400)
            check('invalid_credentials_do_not_restart', b'/configure' in wait_http(urls['setup'], expected=b'/configure'))
            status, _ = http(urls['setup'], '/setclocktype', {'clocktype':1}, origin='http://untrusted.invalid')
            check('foreign_origin_rejected', status == 400)
            status, _ = http(urls['setup'], '/setclocktype', {'clocktype':1}, compact=False)
            check('ordinary_json_whitespace_accepted', status == 200)
        status, _ = http(urls['setup'], '/setclocktype', {'clocktype':'1'}, origin=urls['setup'])
        check('settings_saved_via_real_http', status == 200)
        status, _ = http(urls['setup'], '/configure', {'ssid':'yoRadio-QEMU','password':''}, origin=urls['setup'])
        check('configure_post_accepted', status == 200)
        wait_http(urls['station'], '/getclocktype', expected=b'1')
        page = wait_http(urls['station'], expected=b'/setclocktype')
        check('station_web_interface_after_reboot', len(page) > 1000)
        wait_for(lambda: 'C2 NTP reply' in (folder / 'qemu.log').read_text(errors='replace'), 'No station NTP reply')
        check('station_clock_set', vm.read_guest_clock(folder) > 1735689600)
        raw = (folder / 'flash.bin').read_bytes()
        check('credentials_persisted_in_guest_flash', raw[0x3e7004:0x3e7045].split(b'\0')[0] == b'yoRadio-QEMU' and struct.unpack_from('<I', raw, 0x3e7160)[0] == 1)
        check('real_dhcp_station_address', all(row['guest_address'] == '192.168.43.15' for row in session['web_endpoints'] if row['name'] == 'station'))
        check('no_panic_after_initial_configure', clean_uart(folder))
        vm.control(folder, 'stop'); session = None
        vm.ensure_stopped(folder)
        session, _ = vm.launch(options, folder)
        urls = session['web_urls']
        wait_http(urls['station'], '/getclocktype', expected=b'1')
        check('persistent_cold_boot_skips_setup', b'/setclocktype' in wait_http(urls['station'], expected=b'/setclocktype'))
        check('no_panic_after_persistent_cold_boot', clean_uart(folder))
        before = (folder / 'uart.log').read_text(errors='replace').count('ESP-ROM:esp32c2')
        vm.control(folder, 'reset')
        wait_for(lambda: (folder / 'uart.log').read_text(errors='replace').count('ESP-ROM:esp32c2') > before, 'Reset did not boot ROM')
        wait_http(urls['station'], '/getclocktype', expected=b'1')
        check('reset_preserves_http_settings', True)
        check('no_panic_or_watchdog', clean_uart(folder))
        check('immutable_source_artifacts_unchanged', source_hashes(options) == initial_hashes)
        result['passed'] = True
    except Exception as error:
        result['error'] = str(error)
        raise
    finally:
        if session is not None: vm.control(folder, 'stop')
        folder.mkdir(parents=True, exist_ok=True)
        vm.save_json(folder / 'http-test-report.json', result)
    return result


def station_isolation(options, folder):
    """Simultaneously serve two real guests at consecutive auto ports without cross-talk."""
    sessions = []
    states = [folder / 'first', folder / 'second']
    arguments = SimpleNamespace(**{**vars(options), 'network':'clock', 'persistent':False})
    try:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            sessions = list(pool.map(lambda state: vm.launch(arguments, state)[0], states))
        urls = [session['web_urls']['station'] for session in sessions]
        if urls[0] == urls[1]: raise AssertionError('VMs share a localhost port')
        for url in urls: wait_http(url, '/getclocktype', expected=b'0')
        status, _ = http(urls[0], '/setclocktype', {'clocktype':3})
        if status != 200: raise AssertionError('First station setting failed')
        wait_http(urls[0], '/getclocktype', expected=b'3')
        if http(urls[1], '/getclocktype')[1] != b'0': raise AssertionError('Setting crossed into the second VM')
        return {'passed':True,'separate_ports':True,'real_station_http':True,'settings_isolated':True,
                'web_endpoints':[session['web_endpoints'] for session in sessions]}
    finally:
        # Even a partial concurrent launch must be stopped before propagating its error.
        for state in states:
            if (state / 'session.json').exists():
                try: vm.control(state, 'stop')
                except (OSError, RuntimeError): pass


def run_suite(options, output):
    """Record actual guest network checks and always preserve failed results."""
    output.mkdir(parents=True, exist_ok=True)
    report = {'passed':False,'tested_at_utc':dt.datetime.now(dt.timezone.utc).isoformat(),
              'flows':{},'station_isolation':{},'scope':'Real HTTP/TCP, open synthetic Wi-Fi, DHCP, raw settings persistence, reboot and SNTP; no browser JS, real RF or WPA handshake.'}
    run = output / ('vms-' + uuid.uuid4().hex[:12])
    try:
        for variant in ('original', 'reconstructed'):
            options.firmware = variant
            report['flows'][variant] = configuration_flow(options, run / variant)
            print('PASS real HTTP initial setup/AP->STA/persistence:', variant, flush=True)
        options.firmware = 'reconstructed'
        report['station_isolation'] = station_isolation(options, run / 'isolation')
        report['passed'] = True
    finally:
        report['qemu_sha256'] = vm.digest(Path(options.qemu).read_bytes())
        vm.save_json(output / 'report.json', report)
    return report


def main():
    """Provide a standalone integration runner using the same VM frontend as users."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--yoradio', type=Path, default=Path(__file__).resolve().parents[2] / 'yoRadio')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--qemu', type=Path, default=Path(__file__).resolve().parents[1] / 'bin/qemu-system-riscv32.exe')
    args = parser.parse_args()
    options = SimpleNamespace(yoradio=str(args.yoradio), qemu=str(args.qemu),
                              rom_elf=str(args.yoradio / '.idf/tools-v6.1/tools/esp-rom-elfs/20260528/esp32c2_rev200_rom.elf'),
                              hardware_profile=str(Path(__file__).resolve().parents[1] / 'configs/boards/small-tv-c2.json'),
                              display='none', paused=False, gdb_port=0, http_base_port=0)
    run_suite(options, args.output)


if __name__ == '__main__':
    main()
