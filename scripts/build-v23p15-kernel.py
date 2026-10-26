#!/usr/bin/env python3
"""Build P15 original platform-entry terminal diagnostic; source timer integration remains separate."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('base',HERE/'build-v23-payload.py')
base=importlib.util.module_from_spec(spec);spec.loader.exec_module(base)
STACK=0xfffffff007be8000
BCM_START=0xfffffff0073fe310
BCM_END=0xfffffff0073fe334
HOOKS={
    0xfffffff0079e19f0:('cpu_done',bytes.fromhex('00008052')),
    0xfffffff0079e19f4:('pe_prep',bytes.fromhex('e10314aa')),
    0xfffffff007953998:('pe_entry',bytes.fromhex('ff0301d1')),
    0xfffffff0079e19fc:('pe_return',bytes.fromhex('1284e897')),
}
BCM_BYTES=bytes.fromhex('af0038d5ef1d4092000088d200bc70d30000019100080f8b011e8052010000b9df3f03d5')

def audit(original,candidate,code,symbols,mode):
    assert mode in ['m'] and base.sha(original)==base.BASELINE_SHA
    assert len(candidate)==len(original) and len(code)<=base.CAVE_END-base.CAVE
    m=base.MachO(original);at=m.offset(base.CAVE,base.CAVE_END-base.CAVE)
    assert not any(original[at:at+base.CAVE_END-base.CAVE])
    assert symbols['stage1']==base.CAVE and candidate[at:at+len(code)]==code
    masked=bytearray(candidate);masked[at:at+len(code)]=original[at:at+len(code)]
    start=m.offset(BCM_START,36)
    assert original[start:start+36]==BCM_BYTES
    expected=base.branch(BCM_START,symbols['after_bcm'])+bytes.fromhex('1f2003d5')*8
    assert candidate[start:start+36]==expected and BCM_BYTES not in candidate
    masked[start:start+36]=BCM_BYTES
    assert original[m.offset(base.ENTRY):m.offset(base.ENTRY)+4]==base.branch(base.ENTRY,base.FIRST)
    assert original[m.offset(base.MMU):m.offset(base.MMU)+8]==bytes.fromhex('001018d5df3f03d5')
    call=m.offset(0xfffffff0079e19f8)
    assert original[call:call+4]==bytes.fromhex('e8c7fd97')
    assert candidate[call:call+4]==original[call:call+4], 'Original platform BL/LR must stay intact'
    for site,target in [(base.ENTRY,symbols['stage1']),(base.MMU,symbols['pre'])]:
        off=m.offset(site);assert candidate[off:off+4]==base.branch(site,target)
        masked[off:off+4]=original[off:off+4]
    for site,(name,expected) in HOOKS.items():
        off=m.offset(site);assert original[off:off+4]==expected
        assert candidate[off:off+4]==base.branch(site,symbols[name])
        masked[off:off+4]=expected
    seal=base.MachO(candidate).hmac_info();assert seal['matches']
    off=seal['offset'];masked[off:off+32]=original[off:off+32]
    assert masked==original,'Unapproved mutation outside entry, BCM block, MMU hook, cave and HMAC'
    return dict(version='V23P15'+mode.upper(),kernel_sha256=base.sha(candidate),
        baseline_sha256=base.BASELINE_SHA,source_native_arch_timer_kernel=False,
        native_timer_driver_integrated=False,whole_bcm_routing_block_removed=True,
        changes_confined=True,probe_bytes=len(code),probe_capacity=base.CAVE_END-base.CAVE,
        source_crypto_hmac=seal,normal_kernel_continuation=False,original_post_mmu_and_arm_init_continuation=True,early_init_hooks={hex(k):v[0] for k,v in HOOKS.items()},
        original_pe_call_preserved=True,platform_argument=0,original_continuation_to_pre_mmu=True,mmu_enable_attempted=mode=='m',
        terminal_control='PE_init_platform(0, bootArgs) returned; before next arm_init call; SCTLR M1',
        console_base='0xb00be000',console_bytes='0x40000',counter_used=False,
        slot_phases={str(i):16+i for i in range(6)},reserved_bytes_after_arm=576,
        independent_one_byte_witnesses=True,hardware_verified=False)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    for n in ['baseline','adt','sealer','output']:p.add_argument('--'+n,type=Path,required=True)
    p.add_argument('--mode',choices=['m'],required=True);a=p.parse_args()
    original=a.baseline.read_bytes();adt=a.adt.read_bytes()
    assert base.sha(original)==base.BASELINE_SHA and base.sha(adt)==base.ADT_SHA
    assert base.MachO(original).hmac_info()['matches']
    out=a.output;out.mkdir(parents=True,exist_ok=False)
    (out/'probe.ld').write_text('SECTIONS { . = %#x; .text : { *(.text*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n'%base.CAVE)
    subprocess.run(['clang','--target=aarch64-none-elf','-DMODE='+('1' if a.mode=='p' else '2'),'-c',str(HERE/'v23p15-platform.S'),'-o',str(out/'probe.o')],check=True)
    subprocess.run(['ld.lld','-T',str(out/'probe.ld'),'--defsym=original_first='+hex(base.FIRST),
        '--defsym=original_stack='+hex(STACK),'--defsym=original_after_bcm='+hex(BCM_END),
        '--defsym=original_post_mmu=0xfffffff0073fe37c','--defsym=original_cpu_done_next=0xfffffff0079e19f4',
        '--defsym=original_pe_call=0xfffffff0079e19f8','--defsym=original_pe_entry_next=0xfffffff00795399c',
        '-e','stage1',str(out/'probe.o'),'-o',str(out/'probe.elf')],check=True)
    subprocess.run(['llvm-objcopy','-O','binary','-j','.text',str(out/'probe.elf'),str(out/'probe.bin')],check=True)
    symbols={v[2]:int(v[0],16) for l in subprocess.check_output(['llvm-nm','--defined-only','-n',str(out/'probe.elf')],text=True).splitlines() if len(v:=l.split())==3}
    code=(out/'probe.bin').read_bytes();assert len(code)<=base.CAVE_END-base.CAVE
    data=bytearray(original);m=base.MachO(data);at=m.offset(base.CAVE,len(code));data[at:at+len(code)]=code
    at=m.offset(BCM_START,36);assert data[at:at+36]==BCM_BYTES
    data[at:at+36]=base.branch(BCM_START,symbols['after_bcm'])+bytes.fromhex('1f2003d5')*8
    for site,target in [(base.ENTRY,symbols['stage1']),(base.MMU,symbols['pre'])]:
        at=m.offset(site);data[at:at+4]=base.branch(site,target)
    for site,(name,expected) in HOOKS.items():
        at=m.offset(site);assert data[at:at+4]==expected
        data[at:at+4]=base.branch(site,symbols[name])
    kernel=out/'kernel.macho';kernel.write_bytes(data)
    r=subprocess.run([sys.executable,str(a.sealer),str(kernel)],check=True,capture_output=True,text=True)
    (out/'crypto-seal.log').write_text(r.stdout+r.stderr)
    kernel.with_suffix('.macho.crypto-hmac.json').rename(out/'crypto-hmac.json')
    (out/'kernel.macho.adt').write_bytes(adt)
    report=audit(original,kernel.read_bytes(),code,symbols,a.mode)
    report.update(symbols=symbols,assembly_sha256=base.sha((HERE/'v23p15-platform.S').read_bytes()),
        builder_sha256=base.sha(Path(__file__).read_bytes()),adt_sha256=base.sha(adt),
        sealer_sha256=base.sha(a.sealer.read_bytes()))
    (out/'kernel.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ['version','probe_bytes','kernel_sha256']}))

if __name__=='__main__':main()
