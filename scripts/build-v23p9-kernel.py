#!/usr/bin/env python3
"""Build pinned terminal P9 E/M kernel copies. No device access or crypto bypass."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('base',HERE/'build-v23-payload.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
STACK=0xfffffff007be8000

def audit(original,candidate,code,symbols,mode):
    assert base.sha(original)==base.BASELINE_SHA and len(original)==len(candidate)
    m=base.MachO(original); start=m.offset(base.CAVE,len(code))
    assert len(code)<=base.CAVE_END-base.CAVE and not any(original[start:start+len(code)])
    assert candidate[start:start+len(code)]==code
    masked=bytearray(candidate);masked[start:start+len(code)]=original[start:start+len(code)]
    sites=[(base.ENTRY,symbols['stage1'])]
    if mode=='m': sites.append((base.MMU,symbols['pre']))
    for site,target in sites:
        at=m.offset(site);assert candidate[at:at+4]==base.branch(site,target)
        masked[at:at+4]=original[at:at+4]
    seal=base.MachO(candidate).hmac_info();assert seal['matches']
    off=seal['offset'];masked[off:off+32]=original[off:off+32]
    assert masked==original,'Change outside cave, approved branches or crypto HMAC'
    assert original[m.offset(base.ENTRY):m.offset(base.ENTRY)+4]==base.branch(base.ENTRY,base.FIRST)
    assert original[m.offset(base.MMU):m.offset(base.MMU)+8]==bytes.fromhex('001018d5df3f03d5')
    assert original[m.offset(0xfffffff0073fe0ac):m.offset(0xfffffff0073fe0ac)+8]==bytes.fromhex('403f00d000000091')
    # Legacy BCM STR and original vectors remain byte-identical to the pinned source.
    assert candidate[m.offset(0xfffffff0073fe32c):m.offset(0xfffffff0073fe32c)+4]==original[m.offset(0xfffffff0073fe32c):m.offset(0xfffffff0073fe32c)+4]
    return dict(version='V23P9'+mode.upper(),kernel_sha256=base.sha(candidate),baseline_sha256=base.BASELINE_SHA,
        probe_bytes=len(code),probe_capacity=base.CAVE_END-base.CAVE,changes_confined=True,
        source_crypto_hmac=seal,entry_site=hex(base.ENTRY),pre_mmu_site=hex(base.MMU),
        post_mmu_site='local continuation immediately after displaced MSR and ISB',
        console_base='0xb00be000',console_bytes='0x40000',record_bytes=64,
        normal_kernel_continuation=False,counter_used=False,legacy_store_unchanged=True,
        automatic_terminal_system_reset=True,hardware_verified=False)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ['baseline','adt','sealer','output']:p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--mode',choices=['e','m'],required=True);a=p.parse_args()
    original=a.baseline.read_bytes();adt=a.adt.read_bytes()
    assert base.sha(original)==base.BASELINE_SHA and base.sha(adt)==base.ADT_SHA
    assert base.MachO(original).hmac_info()['matches']
    out=a.output;out.mkdir(parents=True,exist_ok=False)
    (out/'probe.ld').write_text('SECTIONS { . = %#x; .text : { *(.text*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n'%base.CAVE)
    subprocess.run(['clang','--target=aarch64-none-elf','-DMODE='+('1' if a.mode=='e' else '2'),'-c',str(HERE/'v23p9-trace.S'),'-o',str(out/'probe.o')],check=True)
    subprocess.run(['ld.lld','-T',str(out/'probe.ld'),'--defsym=original_first='+hex(base.FIRST),'--defsym=original_stack='+hex(STACK),'-e','stage1',str(out/'probe.o'),'-o',str(out/'probe.elf')],check=True)
    subprocess.run(['llvm-objcopy','-O','binary','-j','.text',str(out/'probe.elf'),str(out/'probe.bin')],check=True)
    symbols={v[2]:int(v[0],16) for l in subprocess.check_output(['llvm-nm','--defined-only','-n',str(out/'probe.elf')],text=True).splitlines() if len(v:=l.split())==3}
    code=(out/'probe.bin').read_bytes();assert symbols['stage1']==base.CAVE
    assert len(code)<=base.CAVE_END-base.CAVE
    data=bytearray(original);m=base.MachO(data);at=m.offset(base.CAVE,len(code));assert not any(data[at:at+len(code)])
    data[at:at+len(code)]=code
    for site,target in [(base.ENTRY,symbols['stage1'])]+([(base.MMU,symbols['pre'])] if a.mode=='m' else []):
        at=m.offset(site);data[at:at+4]=base.branch(site,target)
    kernel=out/'kernel.macho';kernel.write_bytes(data)
    r=subprocess.run([sys.executable,str(a.sealer),str(kernel)],check=True,capture_output=True,text=True)
    (out/'crypto-seal.log').write_text(r.stdout+r.stderr)
    kernel.with_suffix('.macho.crypto-hmac.json').rename(out/'crypto-hmac.json')
    (out/'kernel.macho.adt').write_bytes(adt)
    report=audit(original,kernel.read_bytes(),code,symbols,a.mode)
    report.update(symbols=symbols,assembly_sha256=base.sha((HERE/'v23p9-trace.S').read_bytes()),builder_sha256=base.sha(Path(__file__).read_bytes()),adt_sha256=base.sha(adt),sealer_sha256=base.sha(a.sealer.read_bytes()))
    (out/'kernel.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ['version','probe_bytes','kernel_sha256']},indent=2))

if __name__=='__main__':main()
