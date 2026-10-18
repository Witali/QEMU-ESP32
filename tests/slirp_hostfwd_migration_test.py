#!/usr/bin/env python3
"""Check real QEMU migration of host-forward port reservations, without firmware."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
import uuid


def free_port():
    """Choose a loopback endpoint for this test's isolated QMP or host forward."""
    with socket.socket() as connection:
        connection.bind(('127.0.0.1', 0))
        return connection.getsockname()[1]


def lease_payload(path):
    """Decode the uniquely tagged extension from an actual uncompressed VM stream."""
    data = path.read_bytes()
    positions = [index for index in range(len(data)) if data.startswith(b'HPF1', index)]
    assert len(positions) == 1, 'Missing or ambiguous lease migration extension'
    start = positions[0]
    count, cursor, flags = struct.unpack_from('!IHH', data, start + 4)
    assert count <= 65535 and cursor >= 49152 and flags == 0
    rows = []
    for index in range(count):
        port, milliseconds = struct.unpack_from('!HI', data, start + 12 + index * 6)
        assert port and 0 < milliseconds <= 240001
        rows.append(dict(port=port, remaining_guest_ms=milliseconds))
    assert len({row['port'] for row in rows}) == count
    return dict(file_sha256=hashlib.sha256(data).hexdigest(), offset=start, cursor=cursor, leases=rows)


def main():
    """Migrate a paused backend, rebind the peer port, and verify a distinct alias."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.qemu = args.qemu.resolve()
    root = Path(__file__).resolve().parents[1]
    sys.path.insert(0, str(root / 'tools'))
    from small_tv_vm import QMP
    out = args.output.resolve()
    out.mkdir(exist_ok=False)
    report = dict(passed=False, all_functionality_verified=False, scope=__doc__,
        limitations='Machine none validates the backend state format and port reservations; it does not prove migration of ESP peripherals or live host TCP streams.',
        qemu_sha256=hashlib.sha256(args.qemu.read_bytes()).hexdigest(),
        test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        helper_sha256=hashlib.sha256((root / 'tools/small_tv_vm.py').read_bytes()).hexdigest())
    processes, monitors, clients, logs = [], [], [], []
    host_port = free_port()

    def launch(label, incoming=None):
        """Launch only a named, owned headless backend; execute no guest CPU code."""
        name = 'hostfwd-migration-' + uuid.uuid4().hex[:12]
        qmp_port = free_port()
        command = [str(args.qemu), '-M', 'none', '-m', '4M', '-nodefaults', '-S',
            '-name', name, '-display', 'none', '-serial', 'none', '-monitor', 'none',
            '-netdev', f'user,id=porttest,restrict=on,ipv6=off,hostfwd=tcp:127.0.0.1:{host_port}-10.0.2.15:80',
            '-qmp', f'tcp:127.0.0.1:{qmp_port},server=on,wait=off']
        if incoming:
            command += ['-incoming', 'file:' + incoming.as_posix()]
        log = (out / (label + '.log')).open('wb')
        logs.append(log)
        process = subprocess.Popen(command, stdout=log, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
        processes.append(process)
        deadline = time.monotonic() + 20
        while True:
            try:
                monitor = QMP(qmp_port, name)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError((out / (label + '.log')).read_text(errors='replace'))
                time.sleep(.02)
        monitors.append(monitor)
        report[label] = dict(name=name, pid=process.pid, command=command)
        return process, monitor

    def accepted(monitor):
        """Observe that the real libslirp accept path has allocated its TCP state."""
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            table = monitor.execute('human-monitor-command', **{'command-line': 'info usernet'})
            if 'TCP[SYN_SENT]' in table:
                return table
            time.sleep(.02)
        raise AssertionError('Backend never accepted the owned host connection')

    def migrate(monitor, path):
        """Use QEMU's file transport and require a terminal completed migration."""
        monitor.execute('migrate', uri='file:' + path.as_posix())
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            status = monitor.execute('query-migrate')
            if status.get('status') == 'completed':
                return status
            if status.get('status') in ('failed', 'cancelled'):
                raise AssertionError(status)
            time.sleep(.02)
        raise TimeoutError('Owned migration did not complete')

    try:
        source, source_qmp = launch('source')
        first = socket.create_connection(('127.0.0.1', host_port), 3)
        clients.append(first)
        peer_port = first.getsockname()[1]
        report['peer_port'] = peer_port
        report['source']['usernet'] = accepted(source_qmp)
        first_file = out / 'first.migration'
        report['source']['migration'] = migrate(source_qmp, first_file)
        report['first'] = lease_payload(first_file)
        assert [row['port'] for row in report['first']['leases']] == [peer_port]
        source_qmp.execute('quit')
        source.wait(timeout=10)
        first.close()

        target, target_qmp = launch('target', first_file)
        report['target']['runstate'] = target_qmp.execute('query-status')
        second = socket.socket()
        clients.append(second)
        second.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        second.settimeout(3)
        second.bind(('127.0.0.1', peer_port))
        second.connect(('127.0.0.1', host_port))
        assert second.getsockname()[1] == peer_port
        report['target']['usernet'] = accepted(target_qmp)
        second_file = out / 'second.migration'
        report['target']['migration'] = migrate(target_qmp, second_file)
        report['second'] = lease_payload(second_file)
        ports = {row['port'] for row in report['second']['leases']}
        assert len(ports) == 2 and peer_port in ports
        alias = next(port for port in ports if port != peer_port)
        assert 49152 <= alias <= 65535
        report['guest_alias'] = alias
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        for client in clients:
            client.close()
        for process in processes:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        for monitor in monitors:
            monitor.close()
        for log in logs:
            log.close()
        report['qemu_unchanged'] = hashlib.sha256(args.qemu.read_bytes()).hexdigest() == report['qemu_sha256']
        report['test_unchanged'] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest() == report['test_sha256']
        report['helper_unchanged'] = hashlib.sha256((root / 'tools/small_tv_vm.py').read_bytes()).hexdigest() == report['helper_sha256']
        report['passed'] = report['passed'] and all(report[key] for key in ('qemu_unchanged', 'test_unchanged', 'helper_unchanged'))
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', newline='\n')
    print(json.dumps({key: value for key, value in report.items() if key not in ('source', 'target')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
