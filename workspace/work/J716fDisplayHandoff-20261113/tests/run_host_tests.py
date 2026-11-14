#!/usr/bin/env python3
"""Test the actual caller together with the frozen resource provider."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cxx',default='clang++'); p.add_argument('--out',type=Path,required=True)
    a=p.parse_args(); source=Path(__file__).resolve().parents[1]
    provider=source.parent/'J716fGopAdapter-20261113'
    out=a.out.resolve(); out.mkdir(parents=True,exist_ok=True); include=out/'shim'
    for name in ['IOKit/IOService.h','IOKit/IODeviceMemory.h','IOKit/IOLib.h',
                 'libkern/c++/OSArray.h','pexpert/pexpert.h','mach/vm_param.h','arm/pmap_public.h']:
        f=include/name; f.parent.mkdir(parents=True,exist_ok=True)
        f.write_text('#include "IOKitShim.h"\n')
    cxx=shutil.which(a.cxx)
    if not cxx: raise SystemExit('Missing compiler '+a.cxx)
    cmd=[cxx,'-std=c++20','-Wall','-Wextra','-Werror','-pthread',
         '-Wno-deprecated-volatile','-I'+str(include),'-I'+str(provider/'tests/shim'),
         '-I'+str(provider/'package'),str(source/'tests/handoff_test.cpp'),
         '-o',str(out/'handoff-tests'),'-fsanitize=address,undefined','-fno-omit-frame-pointer']
    built=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (out/'compile.log').write_text(built.stdout)
    if built.returncode: raise SystemExit(built.stdout)
    tested=subprocess.run([str(out/'handoff-tests')],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (out/'test.log').write_text(tested.stdout)
    files=[p for base in ['package','tests'] for p in (source/base).rglob('*') if p.is_file()]
    files += [provider/'package/J716fGopProvider.cpp',provider/'package/J716fDisplayCore.h',provider/'tests/shim/IOKitShim.h']
    report={'status':'pass' if tested.returncode==0 else 'fail','command':cmd,
            'scope':'actual source with host IOKit model; no target ABI or hardware proof',
            'sanitizers':['address','undefined'],'output':tested.stdout,
            'source_sha256':{str(p.relative_to(source.parent)):hashlib.sha256(p.read_bytes()).hexdigest() for p in files}}
    (out/'host-test-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(tested.stdout,end=''); raise SystemExit(tested.returncode)
if __name__=='__main__': main()
