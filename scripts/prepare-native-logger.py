#!/usr/bin/env python3
"""Generate a reviewed device patch from exact post-upstream XNU inputs.

Only isolated candidate/driver copies change. The result enters the ordinary
device patch series, so the shared resolver replays it on clean pinned XNU.
No binaries, devices, credentials or reset implementation are involved.
"""
import argparse
import difflib
import hashlib
import json
from pathlib import Path
import shutil

PREIMAGES = {
 'osfmk/arm64/start.s': '837c29dbd72fbbbcf5e142821a3de773d727ef8072eccb6a980b84cb7e99091f',
 'osfmk/arm64/arm_vm_init.c': '372d0449a312dc4391aa1e6b5adee35dd2a87b4c7beb0000954a3d84808493e3',
 'osfmk/arm/arm_init.c': '02b768806f3b56e2db6ae57b8e6e9ad6783a7686d1ebc50ec170f543be369a46',
 'osfmk/arm64/sleh.c': 'c190c36f36a0c799b7b13af8d485fd6bfce2d604b51bf3a4f0499eaf33377bfe',
 'osfmk/kern/sched_prim.c': '7a8a87dd5ee0d04b4284a3c16765b063e0ea328d147722cbc035d9880cb84a8c',
 'osfmk/conf/Makefile.arm64': '83e43ad7ccd513e87fc3594b5242e8ba5aa465bac4d51fb8bdfc722fd502155b',
}
ARM_DT_SHA = '07f96a42bdd638022960f5fcaf9e3a07dd73a82c869a70815781cac6d4873b3b'
FILES = ('NativeLog.h','NativeMap.h','NativeHooks.h','NativeXnu.inc','NativeStart.inc')

def sha(data): return hashlib.sha256(data).hexdigest()

def replace(text, old, new):
    if text.count(old) != 1: raise ValueError('Native hook must match once: '+old[:100])
    return text.replace(old,new)

def identity(device):
    folder=Path(device)/'package/native-log'
    # Git checkout on Windows may use CRLF. Identity is over reviewed source,
    # normalized identically on the host, macOS CI and firmware build host.
    inputs={n:sha((folder/n).read_bytes().replace(b'\r\n',b'\n')) for n in FILES}
    for n in ('board.toml', 'configs/j716f_arch_timer_source_defconfig',
              'scripts/prepare-native-observer.py', 'scripts/j716f-native-timer-route.json',
              'package/native-observer/J716fArmCpuState.h',
              'package/native-observer/J716fArmCpuObserver.inc'):
        inputs[n]=sha((Path(device)/n).read_bytes().replace(b'\r\n',b'\n'))
    inputs['prepare-native-logger.py']=sha(Path(__file__).read_bytes().replace(b'\r\n',b'\n'))
    digest=sha(json.dumps({'inputs':inputs,'xnu':PREIMAGES,'armdt':ARM_DT_SHA},sort_keys=True).encode())
    words=[int.from_bytes(bytes.fromhex(digest)[i:i+8],'little') for i in range(0,32,8)]
    header='// Generated candidate identity, not a hardware qualification.\n#define JL_CANDIDATE_WORDS {'+', '.join(hex(v)+'ULL' for v in words)+'}\n'
    return digest,inputs,header

def xnu_changes(source, device):
    source,device=Path(source),Path(device)
    before={p:(source/p).read_bytes().replace(b'\r\n',b'\n') for p in PREIMAGES}
    for p,digest in PREIMAGES.items():
        if sha(before[p]) != digest: raise ValueError('Unreviewed XNU preimage: '+p)
    after={p:data.decode() for p,data in before.items()}
    p='osfmk/arm64/start.s'; t=after[p]
    start=t.index('LEXT(start_first_cpu)')
    prefix,t=t[:start],t[start:]
    t=replace(t,'EXT(LowExceptionVectorBase)@pageoff','EXT(j716f_native_early_vectors)@pageoff')
    t=replace(t,'EXT(LowExceptionVectorBase)@page','EXT(j716f_native_early_vectors)@page')
    anchor='\t// Get the kernel memory parameters from the boot args'
    entry='''\t// Native session must be armed before entering C or changing mappings.
\tmsr spsel, #0
\tadrp x0, EXT(intstack_top)@page
\tadd x0, x0, EXT(intstack_top)@pageoff
\tmov sp, x0
\tmov x0, x20
\tbl EXT(j716f_native_begin)
\tcbz x0, Lj716f_native_halt

'''
    t=replace(t,anchor,entry+anchor)
    t=replace(t,'Lbootstrap_tables_ready:\n','''Lbootstrap_tables_ready:
\t// Original SP/LR are virtual. C runs on a temporary physical stack here.
\tmov x27, sp
\tmov x28, lr
\tadrp x0, EXT(intstack_top)@page
\tadd x0, x0, EXT(intstack_top)@pageoff
\tmov sp, x0
\tadd x0, x25, PGBYTES
\tsub x1, x22, x23
\tbl EXT(j716f_native_bootstrap)
\tmov sp, x27
\tmov lr, x28
\tcbz x0, Lj716f_native_halt
''')
    after[p]=prefix+t+'\n#include "NativeStart.inc"\n'
    p='osfmk/arm64/arm_vm_init.c';t=after[p]
    t=replace(t,'void\narm_vm_init(uint64_t memory_size, boot_args * args)',
              '#include "NativeXnu.inc"\n\nvoid\narm_vm_init(uint64_t memory_size, boot_args * args)')
    t=replace(t,'\tgVirtBase = args->virtBase;','\tj716f_native_event(3, args->virtBase, args->physBase, args->memSize);\n\tgVirtBase = args->virtBase;')
    t=replace(t,'\tarm_vm_physmap_init(args);','\tarm_vm_physmap_init(args);\n\tj716f_native_vm_map();')
    t=replace(t,'\t// From here on out, we\'re off the bootstrap translation tables.',
              '\tj716f_native_event(5, (uint64_t)cpu_tte, cpu_ttep, invalid_ttep);\n\t// From here on out, we\'re off the bootstrap translation tables.')
    t=replace(t,'\tpmap_bootstrap(dynamic_memory_begin);','\tpmap_bootstrap(dynamic_memory_begin);\n\tj716f_native_event(6, dynamic_memory_begin, 0, 0);')
    t=replace(t,'\tpatch_low_glo_static_region(args->topOfKernelData, avail_start - args->topOfKernelData);',
              '\tpatch_low_glo_static_region(args->topOfKernelData, avail_start - args->topOfKernelData);\n\tj716f_native_event(7, first_avail, (uint64_t)j716f_native_window, 0);')
    after[p]=t
    p='osfmk/arm/arm_init.c'; t='#include <arm64/NativeHooks.h>\n'+after[p]
    t=replace(t,'\tarm_setup_pre_sign();','\tj716f_native_event(2, (uint64_t)args, 0, 0);\n\tarm_setup_pre_sign();')
    after[p]=t
    p='osfmk/arm64/sleh.c';t='#include "NativeHooks.h"\n'+after[p]
    anchor='\tarm_saved_state_t    * state   = &context->ss;'
    t=replace(t,anchor,anchor+'\n\tif (PSR64_IS_KERNEL(get_saved_state_cpsr(state))) {\n\t\tj716f_native_event(41, esr, far, get_saved_state_pc(state));\n\t}')
    anchor='\tarm_saved_state_t    * state = &context->ss;'
    t=replace(t,anchor,anchor+'\n\tj716f_native_event(42, esr, far, get_saved_state_pc(state));')
    after[p]=t
    p='osfmk/kern/sched_prim.c';t='#include <arm64/NativeHooks.h>\n'+after[p]
    t=replace(t,'\tthread = machine_switch_context(self, continuation, thread);',
              '\tj716f_native_sched(0, (uintptr_t)self, (uintptr_t)thread);\n\tthread = machine_switch_context(self, continuation, thread);\n\tj716f_native_sched(1, (uintptr_t)thread, (uintptr_t)self);')
    after[p]=t
    after['osfmk/conf/Makefile.arm64'] += '\n# Cold-boot logger executes before SIMD/stack-check runtime initialization.\narm_vm_init.o_CFLAGS_ADD += -fno-stack-protector -fno-stack-check -mgeneral-regs-only\n'
    candidate,inputs,header=identity(device)
    for n in FILES: after['osfmk/arm64/'+n]=(device/'package/native-log'/n).read_text()
    after['osfmk/arm64/NativeCandidate.h']=header
    return before,after,{'candidate_sha256':candidate,'implementation_sha256':inputs,
                        'preimages':PREIMAGES,'hardware_validated':False}

def generate(source,device,output):
    before,after,report=xnu_changes(source,device)
    output=Path(output)
    if output.exists(): raise ValueError('Do not overwrite generated native patch')
    text=''
    for p,new in after.items():
        old=before.get(p,b'').decode()
        text+=''.join(difflib.unified_diff(old.splitlines(True),new.splitlines(True),
                    fromfile='a/'+p if p in before else '/dev/null',tofile='b/'+p))
    output.parent.mkdir(parents=True,exist_ok=True)
    with output.open('x',encoding='utf-8',newline='\n') as f:f.write(text)
    report['patch_sha256']=sha(output.read_bytes())
    return report

def platform_source(data):
    data=data.replace(b'\r\n',b'\n')
    if sha(data)!=ARM_DT_SHA: raise ValueError('Unreviewed ArmDTPlatform bytes')
    t=data.decode()
    start=t.index('static void platformStageReset (void)')
    end=t.index('bool ArmDTPlatform::configure',start)
    t=t[:start]+t[end:]
    t=replace(t,'  unsigned platformStage = 0;\n  PE_parse_boot_argn ("sheng-platform-stage", &platformStage, sizeof (platformStage));\n','')
    for n in (1,2,3,5):
        t=replace(t,f'  if (platformStage == {n}) {{\n    platformStageReset ();\n  }}\n','')
    start=t.index('  // Explicit hardware A/B diagnostic:')
    end=t.index('  registerService ();',start)
    t=t[:start]+t[end:]
    t='#include "NativeHooks.h"\n'+t
    t=replace(t,'bool ArmDTPlatform::configure (IOService *provider)\n{',
        'bool ArmDTPlatform::configure (IOService *provider)\n{\n  j716f_native_event(8, 0, 0, 0);')
    t=replace(t,'  OSIterator *iterator = getChildIterator (gIOServicePlane);',
        '  j716f_native_event(9, 0, 0, 0);\n  OSIterator *iterator = getChildIterator (gIOServicePlane);')
    t=replace(t,'  registerService ();\n  return true;',
        '  registerService ();\n  j716f_native_event(10, cpuCount, busCount, count);\n  return true;')
    if any(s in t for s in ('platformStageReset','sheng-platform-stage','sheng-platform-panic','smc #0')):
        raise ValueError('Legacy reset diagnostics remain in platform candidate')
    return t.encode()

def install_drivers(device,tree,armdt_source):
    device,tree,source=Path(device),Path(tree),Path(armdt_source)
    link=tree/'drivers/ArmDTPlatform'
    if not link.is_symlink() or link.resolve()!=source.resolve():
        raise ValueError('Expected pristine ArmDTPlatform link')
    cpu=tree/'drivers/ArmCpu'; inc=cpu/'J716fArmCpuObserver.inc'
    # Never write through a component/cache link. Only the already isolated
    # observer copy may receive this bridge, and only once.
    expected_cpu=tree.parent/'native-observer/ArmCpu'
    if not cpu.is_symlink() or cpu.resolve()!=expected_cpu.resolve():
        raise ValueError('Expected isolated ArmCpu observer link')
    original=(device/'package/native-observer/J716fArmCpuObserver.inc').read_bytes().replace(b'\r\n',b'\n')
    if inc.read_bytes().replace(b'\r\n',b'\n')!=original:
        raise ValueError('Unexpected/repeated observer bridge')
    destination=tree.parent/'native-logger/ArmDTPlatform'
    if destination.exists(): raise ValueError('Refuse source overwrite')
    data=(source/'ArmDTPlatform.cpp').read_bytes(); patched=platform_source(data)
    shutil.copytree(source,destination,ignore=shutil.ignore_patterns('.git'))
    (destination/'ArmDTPlatform.cpp').write_bytes(patched)
    shutil.copyfile(device/'package/native-log/NativeHooks.h',destination/'NativeHooks.h')
    link.unlink(); link.symlink_to(destination.resolve(),target_is_directory=True)
    t=original.decode()
    t='#include "NativeHooks.h"\n'+t
    t=replace(t,'  j716fArmCpuRead(r, daif, armcpu_irq_id);',
        '  j716fArmCpuRead(r, daif, armcpu_irq_id);\n  j716f_native_event(10+phase, detail, armcpu_registered, armcpu_gic_ready);')
    t=replace(t,'  if (ready) { j716f_armcpu_timer_checkpoint(); }',
        '  j716f_native_irq(phase, logical, (const volatile unsigned long long *)j716f_armcpu_trace, (const unsigned long long *)r);\n  if (ready) { j716f_armcpu_timer_checkpoint(); }')
    inc.write_text(t); shutil.copyfile(device/'package/native-log/NativeHooks.h',cpu/'NativeHooks.h')
    return {'armdt_original_sha256':sha(data),'armdt_modified_sha256':sha(patched),
            'observer_original_sha256':sha(original),'observer_bridged_sha256':sha(t.encode()),
            'platform_reset_diagnostics_removed':True,'hardware_validated':False}

def install_xnu(device, original_device, source):
    """Append to the isolated device series; the official resolver replays it."""
    device,original_device=Path(device).resolve(),Path(original_device).resolve()
    if device==original_device or original_device in device.parents:
        raise ValueError('Logger patch generation requires an isolated device')
    series=device/'patches/xnu/series'
    previous=series.read_text(encoding='utf-8')
    if 'j716f-native-log.patch' in previous:
        raise ValueError('Native logger already in series')
    report=generate(source,original_device,series.parent/'j716f-native-log.patch')
    series.write_text(previous.rstrip()+'\nj716f-native-log.patch\n',encoding='utf-8',newline='\n')
    report['integration']='ordinary-device-patch-series'
    report['launch_admitted']=False
    return report

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--device',type=Path,default=Path(__file__).resolve().parents[1])
    parser.add_argument('--output',type=Path,required=True)
    a=parser.parse_args();print(json.dumps(generate(a.source,a.device,a.output),indent=2))
