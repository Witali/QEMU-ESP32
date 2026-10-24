"""Replay saved Windows trace-path evidence without starting a guest."""
import argparse
import base64
import gzip
import hashlib
import json
from pathlib import Path
import subprocess


def sha(data):
    """Check full source, binary and raw payload identities."""
    return hashlib.sha256(data).hexdigest()


def verify(path,root,revision=None):
    """Check compiler/runtime filename evidence and both exact baseline executables."""
    proof=json.loads(path.read_text())
    assert proof['passed'] and not proof['all_functionality_verified'] and not proof['board_boot_acceptance']
    raw=(path.parent/proof['raw']['file']).resolve();assert raw.parent==path.parent.resolve()
    packed=raw.read_bytes();assert sha(packed)==proof['raw']['sha256']
    files={}
    for row in json.loads(gzip.decompress(packed))['files']:
        data=base64.b64decode(row['base64'],validate=True)
        assert len(data)==row['bytes'] and sha(data)==row['sha256'] and row['file'] not in files
        files[row['file']]=data
    test=json.loads(files['build/trace-path-test-final/report.json'])
    assert test['passed'] and len(test['cases'])==proof['test_cases']==12
    unsafe=0
    for row in test['cases']:
        assert row['passed']
        matches=row['compiler_status']==0 and row['actual_filenames']==row['expected_filenames']
        assert matches==row['runtime_filename_exact'] and matches!=row['unsafe']
        assert files['build/trace-path-test-final/'+row['name']+'.c'].count(b'puts(__FILE__);')==len(row['expected_filenames'])
        unsafe+=row['unsafe']
    assert unsafe==proof['unsafe_controls']==test['unsafe_variants_rejected']==6
    assert b'fdt disabled' in files['build/baseline-build.log']
    assert b"library 'fdt' not found" in files['build/baseline-build-corrected.log']
    assert b'trace/trace-qapi_commands_authz_trace_events.h' in files['build/baseline-build-internal-fdt.log']
    assert b'unknown escape sequence' in files['build/baseline-build-trace-fix.log']
    log=files['build/baseline-build-escaped-trace.log']
    assert b'FAILED:' not in log and proof['full_build_status']==0
    assert all(('Linking target qemu-system-'+arch+'.exe').encode() in log for arch in ('riscv32','xtensa'))
    for row in proof['binaries'].values():
        file=root/row['file'];assert file.stat().st_size==row['bytes'] and sha(file.read_bytes())==row['sha256']
    commit=None
    if revision:commit=subprocess.check_output(['git','rev-parse','--verify',revision+'^{commit}'],cwd=root,text=True).strip()
    for key in ('sources_sha256','unchanged_models_sha256'):
        for name,digest in proof[key].items():
            assert '\n' not in name and '..' not in Path(name).parts and not Path(name).is_absolute()
            data=subprocess.check_output(['git','show',commit+':'+name],cwd=root) if commit else (root/name).read_bytes()
            if key=='unchanged_models_sha256':data=data.replace(b'\r\n',b'\n')
            assert sha(data)==digest,name
    return dict(passed=True,cases=12,unsafe_controls_rejected=6,baseline_executables_verified=2,board_boot_acceptance=False)


def main():
    """Read archived bytes and optional historical Git source without resetting files."""
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--proof',type=Path,required=True)
    parser.add_argument('--source-revision')
    args=parser.parse_args();print(json.dumps(verify(args.proof,args.root,args.source_revision)))


if __name__=='__main__':main()
