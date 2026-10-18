#!/usr/bin/env python3
"""Compile and execute port-lease boundary, exhaustion and migration controls."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess


def main():
    """Use the selected QEMU toolchain and preserve compiler and test diagnostics."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, required=True)
    parser.add_argument('--cc', default='cc')
    parser.add_argument('--pkg-config', default='pkg-config')
    parser.add_argument('--core-only', action='store_true')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    source = root / 'subprojects/slirp-win32/src'
    build = args.build.resolve()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    compiler = Path(shutil.which(args.cc) or args.cc).resolve()
    env = {**os.environ, 'PATH': str(compiler.parent) + os.pathsep + os.environ.get('PATH', '')}

    def flags(*options):
        """Ask pkg-config directly, without shell interpolation of compiler flags."""
        return shlex.split(subprocess.check_output([args.pkg_config, *options, 'glib-2.0'], env=env, text=True))

    executable = out / ('port-tests.exe' if os.name == 'nt' else 'port-tests')
    inputs = [Path(__file__), Path(__file__).with_name('slirp_hostfwd_port_test.c'), compiler]
    inputs += [source / 'hostfwd_port.c', build / 'subprojects/slirp-win32/libslirp-version.h']
    inputs += sorted(source.glob('*.h'))
    command = [str(compiler), '-std=gnu11', '-O2', '-g', '-Wall', '-Wextra',
        '-DGLIB_STATIC_COMPILATION', '-DLIBSLIRP_STATIC', '-I' + str(source),
        '-I' + str(build / 'subprojects/slirp-win32'), *flags('--cflags'),
        str(Path(__file__).with_name('slirp_hostfwd_port_test.c'))]
    if args.core_only:
        inputs.append(source / 'stream.c')
        command += [str(source / 'hostfwd_port.c'), str(source / 'stream.c')]
    else:
        library = build / 'subprojects/slirp-win32/libslirp.a'
        inputs.append(library)
        # Meson uses thin archives: their own bytes do not contain object code.
        # Pin every referenced object as well as the library's source files.
        inputs += sorted(source.glob('*.c'))
        if library.read_bytes().startswith(b'!<thin>\n'):
            archiver = compiler.with_name('ar' + compiler.suffix)
            inputs.append(archiver)
            members = subprocess.check_output([str(archiver), 't', str(library)], env=env, text=True)
            inputs += [Path(member).resolve() for member in members.splitlines() if member]
        command += ['-DHOSTFWD_FULL_STATE_TEST', str(library)]
    command += ['-static', '-o', str(executable), *flags('--libs', '--static')]
    if os.name == 'nt':
        command += ['-liconv', '-lws2_32', '-liphlpapi']
    hashes = {str(path): hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs}
    report = dict(passed=False, scope=__doc__, core_only=args.core_only,
                  compile_command=command, inputs_sha256=hashes)
    with (out / 'compile.log').open('wb') as log:
        compile_result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    report['compile_exit_code'] = compile_result.returncode
    if compile_result.returncode == 0:
        with (out / 'tests.log').open('wb') as log:
            result = subprocess.run([str(executable)], env=env, stdout=log, stderr=subprocess.STDOUT)
        report['test_exit_code'] = result.returncode
        output = (out / 'tests.log').read_text(errors='replace')
        report['tests'] = len(re.findall(r'^ok \d+ ', output, re.MULTILINE))
        report['passed'] = result.returncode == 0 and report['tests'] == (9 if args.core_only else 10)
        report['executable_sha256'] = hashlib.sha256(executable.read_bytes()).hexdigest()
    report['inputs_unchanged'] = all(hashlib.sha256(Path(path).read_bytes()).hexdigest() == value
                                     for path, value in hashes.items())
    report['passed'] = report['passed'] and report['inputs_unchanged']
    (out / 'report.json').write_text(json.dumps(report, indent=2) + '\n', newline='\n')
    print(json.dumps({key: value for key, value in report.items() if key not in ('compile_command', 'inputs_sha256')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
