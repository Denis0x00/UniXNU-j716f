#!/usr/bin/env python3
"""Build P22 original VM completion and caller-return diagnostic; source timer integration remains separate."""
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
    0xfffffff0073f5de4:('switch_ready',bytes.fromhex('00bd4092')),
    0xfffffff0073f61d0:('pmap_return',bytes.fromhex('162b41f9')),
    0xfffffff0073f64e0:('vm_final',bytes.fromhex('fd7b55a9')),
    0xfffffff0079e1bbc:('vm_return',bytes.fromhex('28bcfff0')),
}
CALLS={
    0xfffffff0079e19f8:'e8c7fd97',0xfffffff0079e19fc:'1284e897',
    0xfffffff0079e1a00:'1981e897',0xfffffff0079e1a04:'0281e897',
    0xfffffff0079e1a08:'0d84e897',0xfffffff0079e1a10:'0c84e897',
    0xfffffff0079e1a28:'01bdfd97',0xfffffff0079e1a48:'e1bdfd97',
    0xfffffff0079e1a6c:'c281e297',0xfffffff0079e1a90:'6e79e897',
    0xfffffff0079e1a94:'327ce897',
    0xfffffff0079e1aac:'737ce897',
    0xfffffff0079e1afc:'7161ff97',
    0xfffffff0079e1b08:'1273e897',
    0xfffffff0079e1b10:'dc3be897',
    0xfffffff0079e1b14:'3090e897',
    0xfffffff007405bec:'93381594',
    0xfffffff007405bfc:'21f1f997',
    0xfffffff007405c1c:'7e2e1594',

    0xfffffff0079e1b20:'6272f397',
    0xfffffff0079e1b24:'ed58ff97',
    0xfffffff0079e1b28:'ff0ee497',
    0xfffffff0079e1b2c:'3a3de897',
    0xfffffff0079e1b30:'a223e397',
    0xfffffff0079e1b44:'b4befd97',
    0xfffffff0079e1b68:'a5c1fd97',
    0xfffffff0079e1b90:'7c91e897',
    0xfffffff0079e1ba8:'9bbefd97',
    0xfffffff0079e1bb8:'b84de897',
    0xfffffff0073f5370:'3cfcff97',
    0xfffffff0073f537c:'9c7dff97',
    0xfffffff0073f53ec:'abb1ff97',
    0xfffffff0073f5448:'06fcff97',
    0xfffffff0073f5464:'8db1ff97',
    0xfffffff0073f547c:'31451994',
    0xfffffff0073f549c:'f1fbff97',
    0xfffffff0073f54b8:'78b1ff97',
    0xfffffff0073f54d0:'1c451994',
    0xfffffff0073f555c:'c1fbff97',
    0xfffffff0073f5570:'4ab1ff97',
    0xfffffff0073f5588:'ee441994',
    0xfffffff0073f5b24:'c390fe97',
    0xfffffff0073f5c00:'92fbff97',
    0xfffffff0073f5c18:'12faff97',
    0xfffffff0073f5c44:'07faff97',
    0xfffffff0073f5c58:'90afff97',
    0xfffffff0073f5c70:'34431994',
    0xfffffff0073f5c84:'e6270094',
    0xfffffff0073f5c98:'72310094',
    0xfffffff0073f5ce0:'0b040094',
    0xfffffff0073f5cf0:'dcf9ff97',
    0xfffffff0073f5d14:'64020094',
    0xfffffff0073f5d40:'f3030094',
    0xfffffff0073f5d88:'c6030e94',
    0xfffffff0073f5de8:'13310094',
    0xfffffff0073f5dfc:'12310094',
    0xfffffff0073f5370:'3cfcff97',
    0xfffffff0073f537c:'9c7dff97',
    0xfffffff0073f53ec:'abb1ff97',
    0xfffffff0073f5448:'06fcff97',
    0xfffffff0073f5464:'8db1ff97',
    0xfffffff0073f547c:'31451994',
    0xfffffff0073f549c:'f1fbff97',
    0xfffffff0073f54b8:'78b1ff97',
    0xfffffff0073f54d0:'1c451994',
    0xfffffff0073f555c:'c1fbff97',
    0xfffffff0073f5570:'4ab1ff97',
    0xfffffff0073f5588:'ee441994',
    0xfffffff0073f55d8:'5a071294',
    0xfffffff0073f55f8:'52071294',
    0xfffffff0073f5618:'4a071294',
    0xfffffff0073f5638:'42071294',
    0xfffffff0073f565c:'39071294',
    0xfffffff0073f5680:'30071294',
    0xfffffff0073f56a0:'28071294',
    0xfffffff0073f56c0:'20071294',
    0xfffffff0073f56e0:'18071294',
    0xfffffff0073f5700:'10071294',
    0xfffffff0073f5720:'08071294',
    0xfffffff0073f5740:'00071294',
    0xfffffff0073f5760:'f8061294',
    0xfffffff0073f5780:'f0061294',
    0xfffffff0073f57a0:'e8061294',
    0xfffffff0073f57c0:'e0061294',
    0xfffffff0073f57e0:'d8061294',
    0xfffffff0073f5804:'7e061294',
    0xfffffff0073f5828:'75061294',
    0xfffffff0073f5848:'be061294',
    0xfffffff0073f586c:'66741594',
    0xfffffff0073f5884:'af061294',
    0xfffffff0073f58ec:'95061294',
    0xfffffff0073f5964:'77061294',
    0xfffffff0073f59dc:'59061294',
    0xfffffff0073f5a10:'4c061294',
    0xfffffff0073f5a30:'44061294',
    0xfffffff0073f5a54:'f06e1594',
    0xfffffff0073f5ad8:'97051294',
    0xfffffff0073f5b24:'c390fe97',
    0xfffffff0073f5c00:'92fbff97',
    0xfffffff0073f5c18:'12faff97',
    0xfffffff0073f5c44:'07faff97',
    0xfffffff0073f5c58:'90afff97',
    0xfffffff0073f5c70:'34431994',
    0xfffffff0073f5c84:'e6270094',
    0xfffffff0073f5c98:'72310094',
    0xfffffff0073f5ce0:'0b040094',
    0xfffffff0073f5cf0:'dcf9ff97',
    0xfffffff0073f5d14:'64020094',
    0xfffffff0073f5d40:'f3030094',
    0xfffffff0073f5d88:'c6030e94',
    0xfffffff0073f5de8:'13310094',
    0xfffffff0073f5dfc:'12310094',
    0xfffffff0073f5e20:'90f9ff97',
    0xfffffff0073f5e34:'8bf9ff97',
    0xfffffff0073f5eb4:'80f6ff97',
    0xfffffff0073f5f68:'cf010094',
    0xfffffff0073f5f90:'c5010094',
    0xfffffff0073f5fbc:'ba010094',
    0xfffffff0073f6084:'f7f8ff97',
    0xfffffff0073f6098:'80aeff97',
    0xfffffff0073f60b0:'24421994',
    0xfffffff0073f61c8:'db8eff97',
    0xfffffff0073f61cc:'23320094',
    0xfffffff0073f62a4:'6ff8ff97',
    0xfffffff0073f62c0:'68f8ff97',
    0xfffffff0073f62d4:'f1adff97',
    0xfffffff0073f62e4:'97411994',
    0xfffffff0073f62fc:'17aeff97',
    0xfffffff0073f63e0:'20f8ff97',
    0xfffffff0073f63fc:'19f8ff97',
    0xfffffff0073f6410:'a2adff97',
    0xfffffff0073f6420:'48411994',
    0xfffffff0073f6438:'c8adff97',
    0xfffffff0073f64c0:'78110094',
    0xfffffff0073f64c4:'75310094',
    0xfffffff0073f6500:'a4af1794',
    0xfffffff0073f6504:'1bb01794',
    0xfffffff0073f6508:'deaf1794',
    0xfffffff0073f650c:'e5af1794',
    0xfffffff0073f6510:'a8af1794',
    0xfffffff0073f6514:'c3af1794',
    0xfffffff0073f6518:'0eb01794',
    0xfffffff0073f651c:'c9af1794',
    0xfffffff0073f6520:'04b01794',
    0xfffffff0073f6524:'fbaf1794',
    0xfffffff0073f6528:'f2af1794',
    0xfffffff0073f652c:'e9af1794',
    0xfffffff0073f6530:'ccaf1794',
    0xfffffff0073f6534:'2faf1794',
    0xfffffff0073f6538:'8eaf1794',
    0xfffffff0073f653c:'85af1794',
    0xfffffff0073f6540:'7caf1794',
    0xfffffff0073f6544:'33af1794',
    0xfffffff0073f6548:'3aaf1794',
    0xfffffff0073f654c:'71af1794',
    0xfffffff0073f6550:'68af1794',
    0xfffffff0073f6554:'3faf1794',
    0xfffffff0073f6558:'5eaf1794',
    0xfffffff0073f655c:'55af1794',
    0xfffffff0073f6560:'44af1794',
    0xfffffff0073f6564:'4baf1794',
    0xfffffff0073f656c:'c02a1994',
    0xfffffff0073f657c:'1ab01794',
    0xfffffff0073f6580:'bb2a1994',
    0xfffffff0073f6588:'10b01794',
    0xfffffff0073f658c:'05b01794',
    0xfffffff0073f6590:'b72a1994',
    0xfffffff0073f659c:'0daf1794',
    0xfffffff0073f65a4:'b22a1994',
    0xfffffff0073f65c4:'aa2a1994',
    0xfffffff0073f65cc:'f1af1794',
    0xfffffff0073f65d0:'a72a1994',
    0xfffffff0073f65e8:'a12a1994',
    0xfffffff0073f65f0:'b4af1794',
    0xfffffff0073f65f4:'9e2a1994',
    0xfffffff0073f65fc:'9c2a1994',
    0xfffffff0073f6618:'952a1994',
    0xfffffff0073f6620:'44ff1894',
    0xfffffff0073f6624:'6baf1794',
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
    for site,word in CALLS.items():
        at=m.offset(site)
        assert original[at:at+4]==bytes.fromhex(word)
        assert candidate[at:at+4]==original[at:at+4], 'Original BL/LR must stay intact'
    for start,size in [(0xfffffff007401e0c,32),(0xfffffff007401e64,168)]:
        at=m.offset(start,size)
        assert candidate[at:at+size]==original[at:at+size], 'Original WFE body must stay intact'
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
    return dict(version='V23P22'+mode.upper(),kernel_sha256=base.sha(candidate),
        baseline_sha256=base.BASELINE_SHA,source_native_arch_timer_kernel=False,
        native_timer_driver_integrated=False,whole_bcm_routing_block_removed=True,
        changes_confined=True,probe_bytes=len(code),probe_capacity=base.CAVE_END-base.CAVE,
        source_crypto_hmac=seal,normal_kernel_continuation=False,original_post_mmu_and_arm_init_continuation=True,early_init_hooks={hex(k):v[0] for k,v in HOOKS.items()},
        original_calls_preserved={hex(k):v for k,v in CALLS.items()},original_wfe_bodies_preserved=True,terminal_register="SCTLR_EL1",platform_argument=0,original_continuation_to_pre_mmu=True,mmu_enable_attempted=mode=='m',
        terminal_control='Original pmap_bootstrap, dynamic/copy-window tables, static-region patch, preemption enable and arm_vm_init RET complete; stop at caller0xfffffff0079e1bbc; actual SCTLR_EL1 terminal M1',
        console_base='0xb00be000',console_bytes='0x40000',counter_used=False,original_path_may_read_counter=True,
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
    subprocess.run(['clang','--target=aarch64-none-elf','-DMODE='+('1' if a.mode=='p' else '2'),'-c',str(HERE/'v23p22-vm-return.S'),'-o',str(out/'probe.o')],check=True)
    subprocess.run(['ld.lld','-T',str(out/'probe.ld'),'--defsym=original_first='+hex(base.FIRST),
        '--defsym=original_stack='+hex(STACK),'--defsym=original_after_bcm='+hex(BCM_END),
        '--defsym=original_post_mmu=0xfffffff0073fe37c',
        '--defsym=original_switch_next=0xfffffff0073f5de8',
        '--defsym=original_pmap_next=0xfffffff0073f61d4',
        '--defsym=original_final_next=0xfffffff0073f64e4',
        '--defsym=vm_globals=0xfffffff007176000','-e','stage1',str(out/'probe.o'),'-o',str(out/'probe.elf')],check=True)
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
    report.update(symbols=symbols,assembly_sha256=base.sha((HERE/'v23p22-vm-return.S').read_bytes()),
        builder_sha256=base.sha(Path(__file__).read_bytes()),adt_sha256=base.sha(adt),
        sealer_sha256=base.sha(a.sealer.read_bytes()))
    (out/'kernel.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ['version','probe_bytes','kernel_sha256']}))

if __name__=='__main__':main()
