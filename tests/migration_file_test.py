#!/usr/bin/env python3
"""Verify binary file migration and actual RAM contents on an owned paused VM."""
import argparse
import hashlib
import json
from pathlib import Path
import socket
import subprocess
import sys
import time
import uuid


def sha256(path):
    """Identify the exact executable, helper, test or captured byte stream."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def free_port():
    """Choose a loopback QMP endpoint for this test's own process."""
    with socket.socket() as connection:
        connection.bind(('127.0.0.1', 0))
        return connection.getsockname()[1]


def main():
    """Transfer all byte values through real file migration, then compare RAM."""
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
    sample = bytes(range(256)) * 2 + b'\r\n\n\r\x1a\x00\xff'
    payload = out / 'all-bytes.bin'
    payload.write_bytes(sample)
    inputs = [args.qemu.resolve(), Path(__file__), root / 'tools/small_tv_vm.py', payload]
    report = dict(passed=False, all_functionality_verified=False, scope=__doc__,
                  limitations='Machine none tests file transport and RAM, not ESP peripheral migration.',
                  inputs_sha256={str(path): sha256(path) for path in inputs})
    owned, monitors, logs = [], [], []

    def launch(label):
        """Start a named headless VM; the target has different initial RAM."""
        name = 'migration-file-' + uuid.uuid4().hex[:12]
        port = free_port()
        command = [str(args.qemu), '-M', 'none', '-m', '4M', '-nodefaults', '-S',
                   '-name', name, '-display', 'none', '-serial', 'none', '-monitor', 'none',
                   '-qmp', f'tcp:127.0.0.1:{port},server=on,wait=off']
        if label == 'source':
            command += ['-device', f'loader,addr=0x1000,file={payload.as_posix()},force-raw=on']
        else:
            command += ['-incoming', 'defer']
        log = (out / (label + '.log')).open('wb')
        logs.append(log)
        process = subprocess.Popen(command, stdout=log, stderr=log,
            creationflags=subprocess.CREATE_NO_WINDOW if sys.platform == 'win32' else 0)
        owned.append(process)
        deadline = time.monotonic() + 20
        while True:
            try:
                monitor = QMP(port, name)
                break
            except OSError:
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError((out / (label + '.log')).read_text(errors='replace'))
                time.sleep(.02)
        monitors.append(monitor)
        report[label] = dict(command=command, pid=process.pid, name=name)
        return process, monitor

    def capture(monitor, label):
        """Read the actual RAM block through QEMU, without changing guest memory."""
        path = out / (label + '.bin')
        result = monitor.execute('human-monitor-command', **{
            'command-line': f'pmemsave 0x1000 {len(sample)} "{path.as_posix()}"'})
        assert not result.strip(), result
        return path.read_bytes()

    try:
        source, source_qmp = launch('source')
        assert capture(source_qmp, 'source-ram') == sample
        target, target_qmp = launch('target')
        assert capture(target_qmp, 'target-before') == bytes(len(sample))
        stream = out / 'state.migration'
        source_qmp.execute('migrate', uri='file:' + stream.as_posix())
        deadline = time.monotonic() + 30
        while True:
            state = source_qmp.execute('query-migrate')
            if state.get('status') == 'completed':
                break
            if state.get('status') in ('failed', 'cancelled') or time.monotonic() >= deadline:
                raise AssertionError(state)
            time.sleep(.02)
        report['source']['migration'] = state
        source_qmp.execute('quit')
        source.wait(timeout=10)
        report['stream_sha256'] = sha256(stream)
        report['stream_bytes'] = stream.stat().st_size
        target_qmp.execute('migrate-incoming', uri='file:' + stream.as_posix())
        deadline = time.monotonic() + 30
        while True:
            state = target_qmp.execute('query-status')
            incoming = target_qmp.execute('query-migrate')
            if incoming.get('status') == 'completed':
                assert not state['running'] and state['status'] in ('paused', 'prelaunch'), state
                report['target']['migration'] = incoming
                break
            if target.poll() is not None or time.monotonic() >= deadline:
                raise AssertionError(state)
            time.sleep(.02)
        report['target']['runstate'] = state
        assert capture(target_qmp, 'target-after') == sample
        report['all_byte_values_preserved'] = True
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
        raise
    finally:
        for process in owned:
            if process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        for monitor in monitors:
            monitor.close()
        for log in logs:
            log.close()
        report['inputs_unchanged'] = all(sha256(Path(path)) == value
            for path, value in report['inputs_sha256'].items())
        report['passed'] = report['passed'] and report['inputs_unchanged']
        (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', newline='\n')
    print(json.dumps({key: value for key, value in report.items()
                      if key not in ('source', 'target', 'inputs_sha256')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
