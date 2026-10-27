#!/usr/bin/env python3
"""Execute production ABI/map helpers, mutation checks and AArch64 assembly."""
import argparse
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
def load(name):
    spec=importlib.util.spec_from_file_location(name,ROOT/'scripts'/f'{name}.py')
    m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m

STUB='''typedef unsigned long long uint64_t;
typedef unsigned long long vm_offset_t;
#define ARM_PGBYTES 4096
#define XNU_MONITOR 0
#define __ARM_KERNEL_PROTECT__ 0
#define KASAN 0
#define TRUE 1
#define ARM_PTE_TYPE_VALID 3ULL
#define ARM_PTE_AF 0x400ULL
#define ARM_PTE_SH(x) ((x)<<8)
#define SH_OUTER_MEMORY 2ULL
#define ARM_PTE_ATTRINDX(x) ((x)<<2)
#define CACHE_ATTRINDX_DISABLE 3ULL
#define ARM_PTE_NX 0x0040000000000000ULL
#define ARM_PTE_PNX 0x0020000000000000ULL
extern uint64_t segDATAB,segSizeDATA,gPhysBase,gVirtBase,cpu_ttep;
extern uint64_t *cpu_tte;
extern int kva_active;
extern void panic(const char *,...) __attribute__((noreturn));
static vm_offset_t alloc_ptpage(int x) {return (vm_offset_t)(unsigned)x;}
static vm_offset_t kvtophys(vm_offset_t x) {return x;}
static vm_offset_t phystokv(vm_offset_t x) {return x;}
#include "NativeXnu.inc"
void exercise_map(void) {j716f_native_vm_map();}
'''

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cc',default='cc')
    p.add_argument('--clang',default='clang');p.add_argument('--source',type=Path)
    p.add_argument('--armdt',type=Path);a=p.parse_args()
    maker,decoder=load('prepare-native-logger'),load('decode-native-log')
    with tempfile.TemporaryDirectory(prefix='j716f-native-log-') as folder:
        work=Path(folder)
        for name in maker.FILES:shutil.copyfile(ROOT/'package/native-log'/name,work/name)
        candidate,_,header=maker.identity(ROOT);(work/'NativeCandidate.h').write_text(header)
        for name in ('LogTests','MapTests'):
            out=work/name
            subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-O2',
                str(ROOT/'package/native-log'/f'{name}.c'),'-o',str(out)],check=True)
            subprocess.run([str(out),str(work/'fixture.bin')] if name=='LogTests' else [str(out)],check=True)
        fixture=(work/'fixture.bin').read_bytes();fixture_id=struct.pack('<4Q',1,2,3,4).hex()
        result=decoder.decode(fixture,fixture_id)
        assert len(result['records'])==63 and not result['native_timer_chain_consistent']
        assert not result['hardware_validated']
        for byte in (0,4,8,16+16,16+60,16+128+112,16+128+116):
            data=bytearray(fixture);data[byte]^=1
            try:r=decoder.decode(data,fixture_id)
            except ValueError:continue
            assert r['rejected_slots'] and not r['native_timer_chain_consistent']
        (work/'target.c').write_text(STUB)
        subprocess.run([a.clang,'--target=arm64-apple-ios17.0','-ffreestanding',
            '-fno-stack-protector','-fno-stack-check','-mgeneral-regs-only','-std=c11',
            '-Wall','-Wextra','-Werror','-Os','-S',str(work/'target.c'),'-o',str(work/'target.s')],check=True)
        assembly=(work/'target.s').read_text()
        # The inlined mapper must not call an absolute function pointer before MMU.
        body=assembly.split('_j716f_native_bootstrap:',1)[1].split('.cfi_endproc',1)[0]
        body=body.split('.globl',1)[0]
        assert '\tblr\t' not in body and '\tblr ' not in body, 'Indirect pre-MMU call'
        (work/'start.S').write_text('#define EXT(x) _##x\n#define LEXT(x) EXT(x):\n#include "NativeStart.inc"\n')
        subprocess.run([a.clang,'--target=arm64-apple-ios17.0','-c',str(work/'start.S'),'-o',str(work/'start.o')],check=True)
        if a.source:
            report=maker.generate(a.source,ROOT,work/'native.patch')
            subprocess.run(['git','-C',str(a.source),'apply','--check','--ignore-space-change',str(work/'native.patch')],check=True)
            assert report['candidate_sha256']==candidate
        if a.armdt:
            original=a.armdt.read_bytes();changed=maker.platform_source(original)
            assert b'platformStageReset' not in changed and changed.count(b'j716f_native_event(')==3
            source=work/'source';source.mkdir()
            (source/'ArmDTPlatform.cpp').write_bytes(original)
            tree=work/'isolated/tree';(tree/'drivers').mkdir(parents=True)
            (tree/'drivers/ArmDTPlatform').symlink_to(source,target_is_directory=True)
            cpu=tree.parent/'native-observer/ArmCpu';cpu.mkdir(parents=True)
            shutil.copyfile(ROOT/'package/native-observer/J716fArmCpuObserver.inc',cpu/'J716fArmCpuObserver.inc')
            (tree/'drivers/ArmCpu').symlink_to(cpu,target_is_directory=True)
            report=maker.install_drivers(ROOT,tree,source)
            assert report['platform_reset_diagnostics_removed']
            assert (source/'ArmDTPlatform.cpp').read_bytes()==original
            assert 'j716f_native_irq(' in (cpu/'J716fArmCpuObserver.inc').read_text()
            try:maker.install_drivers(ROOT,tree,source)
            except ValueError:pass
            else:raise AssertionError('Repeated driver installation accepted')
        # Test the real linked-code walker without pretending this fixture is
        # a compiled kernel. Branch operands printed by disassemblers are ignored.
        audit=load('check-native-logger')
        class Code:
            symbols=dict(zip(audit.ROOTS,(0x1000,0x1004,0x1008)))
            local_symbols={}
            segments=[(b'__TEXT_EXEC',0x1000,16,0,16,5)]
            data=struct.pack('<4I',*[0xd65f03c0]*4)
            def offset(self,pc,size):return pc-0x1000
        code=Code();decoded={p:('ret','') for p in (0x1000,0x1004,0x1008)}
        assert audit.audit_early(code,decoded)['reachable_instructions']==3
        for bad in [('blr','x8'),('ldr','q0, [x0]'),('smc','#0')]:
            decoded[0x1000]=bad
            try:audit.audit_early(code,decoded)
            except ValueError:pass
            else:raise AssertionError('Unsafe pre-MMU instruction accepted')
    print('PASS: native log ABI/map/decoder refusal and AArch64 production assembly; host-only')

if __name__=='__main__':main()
