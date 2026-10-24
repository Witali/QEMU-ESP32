"""Compile trace #line directives and check the runtime value of __FILE__."""
import argparse
import importlib
import json
import os
from pathlib import Path
import subprocess
import sys


def main():
    """Check input/output filenames for all backends that emit C line directives."""
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc',default='cc')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();args.output.mkdir(parents=True,exist_ok=False)
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'scripts'))
    import tracetool
    import tracetool.backend
    import tracetool.format
    modules=[importlib.import_module('tracetool.backend.'+name) for name in ('log','syslog','ftrace')]
    original=tracetool.c_filename;cases=[]
    for unsafe in (False,True):
        encoder=(lambda value:value) if unsafe else original
        tracetool.c_filename=encoder
        for module in modules:module.c_filename=encoder
        for backend in ('log','syslog','ftrace'):
            for index,virtual in enumerate((r'qapi\trace-events',r'qapi\trace-"quoted".events')):
                label=f'{backend}-{index}-'+('unsafe' if unsafe else 'checked')
                header=(args.output/(label+'.h')).resolve()
                event=tracetool.Event.build('path_fixture(int value) "value %d"',17,virtual)
                tracetool.out_lineno=1;tracetool.out_open(str(header))
                tracetool.generate([event],'path_fixture','h',[backend])
                tracetool.out_fobj.close()
                directives=[line for line in header.read_text().splitlines() if line.startswith('#line ')]
                count=2 if backend=='log' else 1
                assert len(directives)==2*count
                expected=[item for _ in range(count) for item in (os.path.relpath(virtual),str(header))]
                source=args.output/(label+'.c');executable=(args.output/(label+'.exe')).resolve()
                source.write_text('#include <stdio.h>\nint main(void) {\n'+
                    '\n'.join(line+'\nputs(__FILE__);' for line in directives)+'\nreturn 0;\n}\n',encoding='utf-8')
                compiled=subprocess.run([args.cc,'-std=c11','-Werror',str(source.resolve()),'-o',str(executable)],
                    capture_output=True,text=True,encoding='utf-8',errors='replace')
                (args.output/(label+'-compiler.log')).write_text(compiled.stdout+compiled.stderr,encoding='utf-8')
                actual=None
                if compiled.returncode==0:
                    actual=subprocess.run([str(executable)],capture_output=True,text=True,encoding='utf-8',check=True).stdout.splitlines()
                matches=compiled.returncode==0 and actual==expected
                assert matches != unsafe,(label,compiled.returncode,actual,expected)
                cases.append(dict(name=label,unsafe=unsafe,compiler_status=compiled.returncode,
                    runtime_filename_exact=matches,expected_filenames=expected,actual_filenames=actual,passed=True))
        tracetool.c_filename=original
        for module in modules:module.c_filename=original
    report=dict(passed=True,cases=cases,unsafe_variants_rejected=sum(row['unsafe'] for row in cases))
    (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8',newline='\n')
    print('PASS actual C #line compile/runtime:',len(cases),'cases,',report['unsafe_variants_rejected'],'unsafe controls')


if __name__=='__main__':main()
