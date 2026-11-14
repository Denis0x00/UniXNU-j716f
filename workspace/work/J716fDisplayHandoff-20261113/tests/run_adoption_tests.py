#!/usr/bin/env python3
"""Compile the exact patched vm_page_create with narrow page-accounting stubs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

def sha(data): return hashlib.sha256(data).hexdigest()

def apply_vm_hunk(data,patch):
    section=patch.split('diff --git a/osfmk/vm/vm_resident.c b/osfmk/vm/vm_resident.c\n')
    if len(section)!=2: raise ValueError('Expected one vm_resident patch')
    rows=section[1].splitlines(True)
    at=next(i for i,v in enumerate(rows) if v.startswith('@@'))
    old=[]; new=[]
    for line in rows[at+1:]:
        if line.startswith('diff ') or line.startswith('@@'): raise ValueError('Unexpected extra hunk')
        if line.startswith(' '): old.append(line[1:]); new.append(line[1:])
        elif line.startswith('-'): old.append(line[1:])
        elif line.startswith('+'): new.append(line[1:])
        else: raise ValueError('Invalid hunk line')
    old,new=''.join(old),''.join(new)
    if data.count(old)!=1: raise ValueError('Hunk context does not match exactly once')
    return data.replace(old,new,1)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--cc',default='clang'); p.add_argument('--out',type=Path,required=True)
    p.add_argument('--xnu-source',type=Path,help='Actual patched resolved vm_resident.c')
    a=p.parse_args(); source=Path(__file__).resolve().parents[1]; root=source.parents[1]
    contract=json.loads((source/'package/source-contract.json').read_text())
    expected=contract['resolved_source_sha256']['osfmk/vm/vm_resident.c']
    if a.xnu_source:
        data=a.xnu_source.read_bytes()
    else:
        origin=root/'analysis/display-handoff-chain-20261113/sources/xnu/osfmk/vm/vm_resident.c'
        data=apply_vm_hunk(origin.read_text(),(source/'patches/j716f-platform-display-handoff.patch').read_text()).encode()
    if sha(data)!=expected: raise ValueError('Patched source hash differs from fixed source contract')
    text=data.decode(); mark='void\nvm_page_create(\n'
    if text.count(mark)!=1: raise ValueError('Expected one actual vm_page_create')
    start=text.index(mark); end=text.index('\n}\n',start)+3
    body=text[start:end]
    out=a.out.resolve(); out.mkdir(parents=True,exist_ok=True)
    (out/'vm_page_create_exact.inc').write_text(body)
    cc=shutil.which(a.cc)
    if not cc: raise ValueError('Missing host compiler')
    cmd=[cc,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(out),
         str(source/'tests/vm_adoption_test.c'),'-o',str(out/'vm-adoption-tests'),
         '-fsanitize=address,undefined','-fno-omit-frame-pointer']
    built=subprocess.run(cmd,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (out/'compile.log').write_text(built.stdout)
    if built.returncode: raise ValueError(built.stdout)
    tested=subprocess.run([str(out/'vm-adoption-tests')],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (out/'test.log').write_text(tested.stdout)
    report={'status':'pass' if tested.returncode==0 else 'fail','command':cmd,
            'scope':'exact patched XNU function with host allocation/accounting stubs',
            'sanitizers':['address','undefined'],'output':tested.stdout,
            'page_shift_model':'runtime int PAGE_SHIFT_CONST; all 0..64 except 12 rejected before shifts or release',
            'xnu_source_sha256':sha(data),'function_sha256':sha(body.encode()),
            'function_first_line':text[:start].count('\n')+1,
            'test_sha256':sha((source/'tests/vm_adoption_test.c').read_bytes())}
    (out/'host-test-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(tested.stdout,end=''); raise SystemExit(tested.returncode)
if __name__=='__main__': main()
