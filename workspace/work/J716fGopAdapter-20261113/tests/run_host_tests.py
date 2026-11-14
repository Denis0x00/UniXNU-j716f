#!/usr/bin/env python3
"""Run the actual provider with host-only IOKit stubs, optionally sanitized."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx', default='clang++')
    p.add_argument('--out', type=Path, required=True)
    p.add_argument('--no-sanitize', action='store_true')
    a = p.parse_args()
    source = Path(__file__).resolve().parents[1]
    out = a.out.resolve(); out.mkdir(parents=True, exist_ok=True)
    include = out/'shim'
    for name in ['IOKit/IOService.h','IOKit/IODeviceMemory.h','IOKit/IOLib.h',
                 'libkern/c++/OSArray.h','pexpert/pexpert.h']:
        path = include/name; path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text('#include "IOKitShim.h"\n')
    compiler = shutil.which(a.cxx)
    if not compiler: raise SystemExit('Missing host C++ compiler: '+a.cxx)
    cmd = [compiler,'-std=c++20','-Wall','-Wextra','-Werror','-pthread',
           '-I'+str(include),'-I'+str(source/'tests/shim'),
           str(source/'tests/provider_test.cpp'),'-o',str(out/'provider-tests')]
    if not a.no_sanitize: cmd += ['-fsanitize=address,undefined','-fno-omit-frame-pointer']
    built = subprocess.run(cmd, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out/'compile.log').write_text(built.stdout)
    if built.returncode: raise SystemExit(built.stdout)
    tested = subprocess.run([str(out/'provider-tests')], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    (out/'test.log').write_text(tested.stdout)
    record = {'status':'pass' if not tested.returncode else 'fail',
              'scope':'actual provider source with modeled host IOKit APIs; not target ABI or hardware',
              'command':cmd,'sanitizers':[] if a.no_sanitize else ['address','undefined'],
              'output':tested.stdout,'source_sha256':{str(p.relative_to(source)):
                 hashlib.sha256(p.read_bytes()).hexdigest() for base in ['package','tests']
                 for p in sorted((source/base).rglob('*')) if p.is_file()}}
    (out/'host-test-report.json').write_text(json.dumps(record,indent=2)+'\n')
    print(tested.stdout, end='')
    raise SystemExit(tested.returncode)

if __name__ == '__main__': main()
