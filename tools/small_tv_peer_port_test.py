#!/usr/bin/env python3
"""Check immediate host peer-port reuse after the real guest HTTP LRU closes it."""
import argparse
import datetime as dt
import hashlib
import http.client
import json
from pathlib import Path
import re
import socket
import struct
import sys
import time
from types import SimpleNamespace


def digest(path):
    """Bind a report to the exact executable, fixture and controller bytes."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def request(connection):
    """Read one bounded, real guest response while leaving the TCP socket open."""
    connection.sendall(b'GET /getsettings HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n')
    response = http.client.HTTPResponse(connection)
    try:
        response.begin()
        body = response.read(65537)
        assert response.status == 200 and len(body) <= 65536
        assert 'tempUnit' in json.loads(body)
        return dict(status=response.status, body_sha256=hashlib.sha256(body).hexdigest())
    finally:
        response.close()


def main():
    """Force a reused tuple using owned loopback sockets and unchanged firmware."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--yoradio', type=Path, required=True)
    parser.add_argument('--dependency-root', type=Path, required=True)
    parser.add_argument('--qemu-root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--qemu', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root, qemu, out = args.yoradio.resolve(), args.qemu_root.resolve(), args.output.resolve()
    out.mkdir(exist_ok=False)
    sys.path.insert(0, str(qemu / 'tools'))
    import small_tv_vm as vm
    from small_tv_http_test import wait_http, clean_uart

    fixture = root / 'Small_TV/qemu/profiles/small-tv-c2.json'
    profile = out / 'profile.json'
    profile.write_bytes(fixture.read_bytes())
    ssid = json.loads(profile.read_text())['network']['station_ssid'].encode()
    assert 1 <= len(ssid) <= 32
    executable = args.qemu or qemu / 'bin/qemu-system-riscv32.exe'
    rom = args.dependency_root / '.idf/tools-v6.1/tools/esp-rom-elfs/20260528/esp32c2_rev200_rom.elf'
    options = SimpleNamespace(yoradio=str(root), qemu=str(executable), rom_elf=str(rom),
        hardware_profile=str(profile), network='setup', display='none', persistent=True,
        paused=True, gdb_port=0, http_base_port=0, firmware='reconstructed')
    folder = out / 'vm'
    vm.prepare(options, folder)
    image = bytearray((folder / 'flash.bin').read_bytes())
    image[0x3e7004:0x3e7045] = ssid.ljust(65, b'\0')
    struct.pack_into('<I', image, 0x3e7160, 1)
    (folder / 'flash.bin').write_bytes(image)
    archive = root / 'firmware/development/small-tv-esp32c2-reconstructed'
    inputs = [Path(__file__), profile, executable, rom]
    inputs += [archive / name for name in ('app.bin', 'bootloader.bin', 'partition-table.bin',
               'assets.bin', 'manifest.json', 'source-manifest.json')]
    inputs += [qemu / 'tools' / name for name in ('small_tv_vm.py', 'small_tv_http_test.py',
               'small_tv_network.py', 'small_tv_web.py', 'esp_services.py', 'esp_board.py', 'esp_elf.py')]
    protected = [(0, 0x9000), (0xf000, 0x3e7000), (0x3e8000, 0x400000)]
    before = [hashlib.sha256(image[a:b]).hexdigest() for a, b in protected]
    report = dict(passed=False, finished=False, scope=__doc__,
        tested_utc=dt.datetime.now(dt.timezone.utc).isoformat(),
        inputs_sha256={str(path): digest(path) for path in inputs}, rows=[])
    clients = []
    session = None
    try:
        session, _ = vm.launch(options, folder)
        report['session'] = {key: session[key] for key in ('name', 'pid', 'web_urls')}
        with vm.QMP(session['qmp_port'], session['name']) as qmp:
            network = qmp.execute('human-monitor-command', **{'command-line': 'info network'})
            backends = re.findall(r'(#\S+): index=\d+,type=user', network)
            assert len(backends) == 1
            qmp.execute('object-add', **{'qom-type': 'filter-dump', 'id': 'peertrace',
                'netdev': backends[0], 'file': str(out / 'network.pcap')})
            qmp.execute('cont')
        port = next(row['host_port'] for row in session['web_endpoints'] if row['name'] == 'station')
        address = ('127.0.0.1', port)
        wait_http(session['web_urls']['station'], expected=b'/setclocktype')
        victim = socket.create_connection(address, 5)
        victim.settimeout(5)
        clients.append(victim)
        peer_port = victim.getsockname()[1]
        report['initial_peer_port'] = peer_port
        report['rows'].append(request(victim))
        # The native HTTP server has six sessions. Eight actual sessions make
        # the guest close its oldest one, placing that tuple in guest TIME_WAIT.
        for _ in range(7):
            client = socket.create_connection(address, 5)
            client.settimeout(5)
            clients.append(client)
            report['rows'].append(request(client))
        assert victim.recv(1) == b'', 'Guest did not close the LRU victim'
        report['guest_lru_eof'] = True
        victim.close()
        reused = socket.socket()
        clients.append(reused)
        reused.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        reused.settimeout(3)
        reused.bind(('127.0.0.1', peer_port))
        reused.connect(address)
        assert reused.getsockname()[1] == peer_port
        report['reused_peer_port'] = reused.getsockname()[1]
        started = time.monotonic()
        try:
            report['reused_response'] = request(reused)
            report['passed'] = True
        except TimeoutError:
            report['error'] = 'Same host peer port connected; guest HTTP response timed out'
        report['response_wall_seconds'] = time.monotonic() - started
        with vm.QMP(session['qmp_port'], session['name']) as qmp:
            report['user_network'] = qmp.execute('human-monitor-command', **{'command-line': 'info usernet'})
            qmp.execute('object-del', id='peertrace')
        report['finished'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        for connection in clients:
            connection.close()
        if session:
            vm.control(folder, 'stop')
            vm.ensure_stopped(folder)
        report['uart_clean'] = clean_uart(folder)
        report['rom_boots'] = (folder / 'uart.log').read_bytes().count(b'ESP-ROM:esp32c2')
        after = (folder / 'flash.bin').read_bytes()
        report['protected_flash_unchanged'] = [hashlib.sha256(after[a:b]).hexdigest() for a, b in protected] == before
        report['inputs_unchanged'] = all(digest(Path(path)) == value for path, value in report['inputs_sha256'].items())
        report['passed'] = report['passed'] and report['uart_clean'] and report['rom_boots'] == 1 and report['protected_flash_unchanged'] and report['inputs_unchanged']
        vm.save_json(out / 'report.json', report)
    print(json.dumps({key: value for key, value in report.items() if key not in ('session', 'rows', 'inputs_sha256')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
