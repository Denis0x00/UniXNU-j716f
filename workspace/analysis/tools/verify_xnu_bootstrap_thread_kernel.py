#!/usr/bin/env python3
"""Independent r17 first-thread sched_startup return checkpoint contract.

Standard library only. Never invokes the official patcher, VM or device.
The whole-kernel expectation was derived before official generation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
from prepare_r6_followup import MachO, branch, target, paint, ENTRY, FIRST_CPU, MMU, POST_MMU, CAVES

KERNEL_SHA = '7970051f50e280b50e42f5ed372ea095dc8646a36fb92e4716f85349bd6118f2'
KERNEL_BYTES = 51085312
EXPECTED_KERNEL_SHA = '73ebad1c451b54e6438fa4c0006aef67d4f38404acb47dc64e95a5799011d143'
CALLER_NAME, CALLEE_NAME = '_kernel_bootstrap_thread', '_sched_startup'
CALLER_VA, CALLER_END = 0xfffffff0072c8370, 0xfffffff0072c88cc
CALLEE_VA, CALLEE_END, CALLEE_RET = 0xfffffff0072bd624, 0xfffffff0072bd758, 0xfffffff0072bd6dc
CALL_SITE, ORIGINAL_CALL_WORD = 0xfffffff0072c8414, 0x97ffd484
BOOTSTRAP_VA, BOOTSTRAP_END = 0xfffffff0072c7ae4, 0xfffffff0072c8220
CONTINUATION_ADR, CREATE_SITE, LOAD_SITE = 0xfffffff0072c8174, 0xfffffff0072c8188, 0xfffffff0072c81fc
IDLE_SITE, BLOCK_SITE = 0xfffffff0072c83d8, 0xfffffff0072bd698
START_SITES = (0xfffffff0072bd65c,0xfffffff0072bd6b0)
OLD_R16_SITE, OLD_R15_SITE = 0xfffffff0072c7ed0, 0xfffffff0072c7e58
OLD_R14_SITE, OLD_R13_SITE = 0xfffffff0072c7da8, 0xfffffff0072c7bd8
OLD_R12_SITE, OLD_R11_SITE, OLD_R10_SITE = 0xfffffff0072c7b50, 0xfffffff0072c7b0c, 0xfffffff0072c7b08
OLD_FIQ_SITE, OLD_R7_SITE, OLD_R8_SITE = 0xfffffff0079e09d0, 0xfffffff0079e0a70, 0xfffffff00741bd38
EARLIER_SITES = (OLD_FIQ_SITE, OLD_R7_SITE, OLD_R8_SITE, OLD_R10_SITE,
                 OLD_R11_SITE, OLD_R12_SITE, OLD_R13_SITE, OLD_R14_SITE, OLD_R15_SITE, OLD_R16_SITE)
RESET_SETTINGS = {
    'entry_probe_reset_delay': 6,
    'entry_probe_reset_after': CALLEE_NAME,
    'entry_probe_reset_in': CALLER_NAME,
    'entry_probe_reset_occurrence': 1,
    'entry_probe_reset_before': False,
}

def require(ok, message):
    if not ok:
        raise ValueError(message)

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

def original_layout(raw):
    require(len(raw)==KERNEL_BYTES and sha(raw)==KERNEL_SHA, 'Expected frozen uninstrumented DT-panic kernel')
    image=MachO(raw)
    require(image.entry==ENTRY, 'Wrong Mach-O entry')
    for name,low,high in ((CALLER_NAME,CALLER_VA,CALLER_END),(CALLEE_NAME,CALLEE_VA,CALLEE_END),
                          ('_kernel_bootstrap',BOOTSTRAP_VA,BOOTSTRAP_END)):
        require(name not in image.ambiguous_names and image.names.get(name)==low and image.end(name)==high,
                'Changed or ambiguous selected symbol: '+name)
        require({n for n,v in image.names.items() if v==low}=={name}, 'Selected symbol has an address alias')
    calls=[va for va in range(CALLER_VA,CALLER_END,4)
           if image.word(va)&0xfc000000==0x94000000 and
           target(image.word(va),va,0x94000000)==CALLEE_VA]
    require(calls==[CALL_SITE] and image.word(CALL_SITE)==ORIGINAL_CALL_WORD,
            'Expected unique sched_startup BL in kernel_bootstrap_thread')
    require(image.body(CALLEE_RET-12,16)==struct.pack('<4I',0xa9457bfd,0xa9444ff4,0x910183ff,0xd65f03c0),
            'sched_startup saved-register/SP restoration or RET changed')
    require([va for va in range(CALLEE_VA,CALLEE_END,4) if image.word(va)==0xd65f03c0]==[CALLEE_RET],
            'Expected unique sched_startup RET')
    require(image.word(CONTINUATION_ADR)==0x10000fe0 and
            image.body(CREATE_SITE-12,12)==struct.pack('<3I',0xd10063a3,0xd2800001,0x52800be2) and
            image.word(CREATE_SITE+4)==0x350003a0,
            'Bootstrap continuation, priority/arguments or failure check changed')
    adr=image.word(CONTINUATION_ADR)
    delta=((adr>>5)&0x7ffff)*4+((adr>>29)&3)
    if delta&(1<<20):delta-=1<<21
    require(CONTINUATION_ADR+delta==CALLER_VA, 'Bootstrap continuation ADR does not name selected thread')
    for va,name,word in ((CREATE_SITE,'_kernel_thread_create',0x94005943),
                         (LOAD_SITE,'_load_context',0x940001b4),
                         (0xfffffff0072c89f8,'_machine_load_context',0x94054672),
                         (IDLE_SITE,'_idle_thread_create',0x97ffd44d),
                         (START_SITES[0],'_kernel_thread_start_priority',0x94008610),
                         (BLOCK_SITE,'_thread_block_reason',0x97ffe805),
                         (START_SITES[1],'_kernel_thread_start_priority',0x940085fb)):
        require(image.word(va)==word and target(word,va,0x94000000)==image.names[name],
                'First-thread initialization-chain call changed: '+name)
    require(image.word(START_SITES[0]+4)==0x35000660 and image.word(START_SITES[1]+4)==0x35000440,
            'Scheduler service creation failure branches changed')
    require(image.word(0xfffffff00741a3c0)==0xd518d080 and
            image.word(0xfffffff00741a41c)==0x9100005f and image.word(0xfffffff00741a450)==0xd65f03c0,
            'machine_load_context TPIDR/SP/return mechanism changed')
    require(image.word(ENTRY)==branch(ENTRY,FIRST_CPU) and image.word(MMU)==0xd5181000 and
            image.word(POST_MMU)==0xd2a00600, 'Original entry/MMU markers changed')
    for va in CAVES.values():
        require(image.body(va,96)==bytes(96), 'Original probe cave is not empty')
        require([(s,n) for s,n,start,size in image.sections if start<=va and va+96<=start+size]
                ==[('__TEXT_EXEC','__text')], 'Cave outside expected executable section')
    return image

def expected_regions(image):
    cave=CAVES[4]
    words=[branch(cave,CALLEE_VA,True),
           0xd53be021,0x580001c2,0x8b020022,0xd5033fdf,0xd53be021,0xeb02003f,0x54ffffa3,
           0x52800120,0x72b08000,0xaa1f03e1,0xaa1f03e2,0xaa1f03e3,
           0xd4000003,0xd503205f,0x17ffffff]
    require(words[0]==0x97fa9401 and branch(CALL_SITE,cave)==0x14054083,
            'Independent branch requirement changed')
    reset=(struct.pack('<16I',*words)+struct.pack('<Q',115200000)).ljust(96,b'\0')
    return [(ENTRY,struct.pack('<I',branch(ENTRY,CAVES[1]))),
            (MMU,struct.pack('<I',branch(MMU,CAVES[2]))),
            (CALL_SITE,struct.pack('<I',branch(CALL_SITE,cave))),
            (CAVES[1],paint(1)),(CAVES[2],paint(2)),(cave,reset)]

def derive_expected(original):
    """In-memory reference only; never an official payload input."""
    image=original_layout(original)
    derived=bytearray(original)
    for va,raw in expected_regions(image):
        offset=image.offset(va,len(raw));derived[offset:offset+len(raw)]=raw
    require(sha(derived)==EXPECTED_KERNEL_SHA, 'Independent full digest requirement changed')
    return bytes(derived)

def probe_readback(original,patched):
    image=original_layout(original)
    require(len(patched)==KERNEL_BYTES, 'Diagnostic kernel length changed')
    candidate=MachO(patched)
    require(candidate.entry==image.entry and candidate.names==image.names and
            patched[:image.header_end]==original[:image.header_end], 'Kernel metadata changed')
    regions=[]
    for va,expected in expected_regions(image):
        offset=image.offset(va,len(expected))
        require(patched[offset:offset+len(expected)]==expected, 'Exact hook/cave sequence differs at '+hex(va))
        regions.append((offset,offset+len(expected),va))
    cursor,changed=0,0
    counts=[]
    for low,high,va in sorted(regions):
        require(low>=cursor and patched[cursor:low]==original[cursor:low], 'Bytes changed outside allowed hook/cave regions')
        count=sum(a!=b for a,b in zip(patched[low:high],original[low:high]))
        counts.append({'va':hex(va),'file_start':hex(low),'file_end':hex(high),'changed_bytes':count})
        changed+=count;cursor=high
    require(patched[cursor:]==original[cursor:], 'Kernel tail changed outside allowed regions')
    require(candidate.body(CAVES[3],96)==bytes(96) and candidate.word(POST_MMU)==image.word(POST_MMU),
            'Stage 3 stop must remain disabled')
    for va in EARLIER_SITES:
        require(candidate.word(va)==image.word(va), 'An earlier r6-r16 stop leaked into r17')
    preserved={}
    for name in ('_kernel_bootstrap','_kernel_thread_create','_load_context','_machine_load_context',
                 '_machine_set_current_thread','_current_processor','_platform_stall_panic_or_spin',
                 '_idle_thread_create','_sched_startup','_kernel_thread_start_priority','_thread_block_reason',
                 '_thread_deallocate','_thread_daemon_init','_task_init','_thread_init','_clock_init',
                 '_clock_oldinit','_kernel_startup_initialize_upto','_panic','_panic_trap_to_debugger'):
        low,high=image.names[name],image.end(name)
        require(candidate.body(low,high-low)==image.body(low,high-low), 'Preserved function changed: '+name)
        preserved[name]={'start':hex(low),'end':hex(high),'sha256':sha(candidate.body(low,high-low))}
    require(changed==194 and sha(patched)==EXPECTED_KERNEL_SHA,
            'Kernel differs from independently precomputed complete byte requirements')
    return {'status':'passed-host-byte-verification','sha256':sha(patched),'bytes':len(patched),
            'changed_bytes':changed,'caller':CALLER_NAME,'callee':CALLEE_NAME,
            'caller_range':[hex(CALLER_VA),hex(CALLER_END)],'callee_range':[hex(CALLEE_VA),hex(CALLEE_END)],
            'call_site':hex(CALL_SITE),'original_call_word':hex(ORIGINAL_CALL_WORD),'occurrence':1,
            'caller_target_call_count':1,'hook_word':'0x14054083','cave_first_bl_word':'0x97fa9401',
            'callee_has_direct_ret':True,'callee_normal_ret_va':hex(CALLEE_RET),
            'bootstrap_continuation_va':hex(CALLER_VA),'bootstrap_create_call_va':hex(CREATE_SITE),
            'bootstrap_load_context_call_va':hex(LOAD_SITE),'first_thread_idle_call_va':hex(IDLE_SITE),
            'sched_startup_block_call_va':hex(BLOCK_SITE),'dynamic_context_switch_observed':False,
            'reset_probe':True,'reset_stage':4,'reset_before':False,'reset_after_callee_returns':True,
            'reset_delay_seconds':6,'timebase_hz':19200000,'delay_ticks':115200000,
            'counter':'CNTPCT_EL0','reset_function':'0x84000009','conduit':'SMC #0',
            'stage3_disabled':True,'r6_through_r16_early_hooks_restored':True,
            'stage1_stage2_exact_words_verified':True,'all_other_bytes_identical':True,
            'header_and_symbols_unchanged':True,'preserved_functions':preserved,'permitted_regions':counts,
            'original_sp_and_argument_registers_preserved_before_call':True,
            'return_address_change':'sched_startup saves/restores LR=cave+4 then RETs there; original next thread instruction is not resumed',
            'expected_digest_origin':'Independent instruction derivation before official generation',
            'hardware_tested':False,
            'scope':'First-thread continuation execution and sched_startup actual-path return only; idle creation status not observed; no proof of dynamic context switching, preemption, timer IRQ/EOI, SMP, IOKit, root or userland'}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original',type=Path)
    parser.add_argument('candidate',type=Path)
    parser.add_argument('--report',type=Path)
    args=parser.parse_args()
    result=probe_readback(args.original.read_bytes(),args.candidate.read_bytes())
    if args.report:
        with args.report.open('x',encoding='utf-8',newline='\n') as stream:
            json.dump(result,stream,indent=2);stream.write('\n')
    print(json.dumps(result,indent=2))
