#!/usr/bin/env python3
"""Independent r19 PE_init_iokit actual-return checkpoint byte contract.

Standard library only. Never invokes the official patcher, VM or device.
Whole-kernel expectation was independently derived before official generation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode=True
from prepare_r6_followup import MachO,branch,target,paint,ENTRY,FIRST_CPU,MMU,POST_MMU,CAVES

KERNEL_SHA='7970051f50e280b50e42f5ed372ea095dc8646a36fb92e4716f85349bd6118f2'
KERNEL_BYTES=51085312
EXPECTED_KERNEL_SHA='84eb643a1315caa2ee5696ff82286cb97c2f6a931075aea971d718fdab9cf5f1'
CALLER_NAME,CALLEE_NAME='_kernel_bootstrap_thread','_PE_init_iokit'
CALLER_VA,CALLER_END=0xfffffff0072c8370,0xfffffff0072c88cc
CALLEE_VA,CALLEE_END,CALLEE_RET=0xfffffff0079a5808,0xfffffff0079a5c10,0xfffffff0079a5bb8
CALL_SITE,ORIGINAL_CALL_WORD=0xfffffff0072c8620,0x941b747a
INIT_SITE,CONFIGURE_SITE=0xfffffff0079a5ba0,0xfffffff0079a5ba4
INIT_VA,INIT_END=0xfffffff0078e6a38,0xfffffff0078e6b38
CONFIGURE_VA,CONFIGURE_END=0xfffffff0078e6b38,0xfffffff0078e6b68
DEFAULTS_NAME='__ZN22IOPlatformExpertDevice17configureDefaultsEv'
DEFAULTS_VA,DEFAULTS_END=0xfffffff00793aa80,0xfffffff00793ab04
UUID_NAME='__ZN22IOPlatformExpertDevice20generatePlatformUUIDEv'
UUID_VA,UUID_END,UUID_RET=0xfffffff0079395f4,0xfffffff007939848,0xfffffff007939840
CONFIGURE_TAIL,DEFAULTS_TAIL=0xfffffff0078e6b44,0xfffffff00793ab00
IRQ_CHECK_SITE,IRQ_ENABLE_SITE=0xfffffff0072c8624,0xfffffff0072c8638
OLD_R18_SITE,OLD_R17_SITE=0xfffffff0072c859c,0xfffffff0072c8414
OLD_R16_SITE,OLD_R15_SITE=0xfffffff0072c7ed0,0xfffffff0072c7e58
OLD_R14_SITE,OLD_R13_SITE=0xfffffff0072c7da8,0xfffffff0072c7bd8
OLD_R12_SITE,OLD_R11_SITE,OLD_R10_SITE=0xfffffff0072c7b50,0xfffffff0072c7b0c,0xfffffff0072c7b08
OLD_FIQ_SITE,OLD_R7_SITE,OLD_R8_SITE=0xfffffff0079e09d0,0xfffffff0079e0a70,0xfffffff00741bd38
EARLIER_SITES=(OLD_FIQ_SITE,OLD_R7_SITE,OLD_R8_SITE,OLD_R10_SITE,OLD_R11_SITE,OLD_R12_SITE,
               OLD_R13_SITE,OLD_R14_SITE,OLD_R15_SITE,OLD_R16_SITE,OLD_R17_SITE,OLD_R18_SITE)
RESET_SETTINGS={
    'entry_probe_reset_delay':6,
    'entry_probe_reset_after':CALLEE_NAME,
    'entry_probe_reset_in':CALLER_NAME,
    'entry_probe_reset_occurrence':1,
    'entry_probe_reset_before':False,
}

def require(ok,message):
    if not ok:raise ValueError(message)

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

def original_layout(raw):
    require(len(raw)==KERNEL_BYTES and sha(raw)==KERNEL_SHA,'Expected frozen uninstrumented DT-panic kernel')
    image=MachO(raw)
    require(image.entry==ENTRY,'Wrong Mach-O entry')
    for name,low,high in ((CALLER_NAME,CALLER_VA,CALLER_END),(CALLEE_NAME,CALLEE_VA,CALLEE_END),
                          ('_InitIOKit',INIT_VA,INIT_END),('_ConfigureIOKit',CONFIGURE_VA,CONFIGURE_END),
                          (DEFAULTS_NAME,DEFAULTS_VA,DEFAULTS_END),(UUID_NAME,UUID_VA,UUID_END)):
        require(name not in image.ambiguous_names and image.names.get(name)==low and image.end(name)==high,
                'Changed or ambiguous selected symbol: '+name)
        require({n for n,v in image.names.items() if v==low}=={name},'Selected symbol has an address alias')
    calls=[va for va in range(CALLER_VA,CALLER_END,4)
           if image.word(va)&0xfc000000==0x94000000 and
           target(image.word(va),va,0x94000000)==CALLEE_VA]
    require(calls==[CALL_SITE] and image.word(CALL_SITE)==ORIGINAL_CALL_WORD,
            'Expected unique PE_init_iokit call in bootstrap thread')
    require(image.body(INIT_SITE,28)==struct.pack('<7I',0x97fd03a6,0x97fd03e5,
            0xa9467bfd,0xa9454ff4,0xa94457f6,0x9101c3ff,0xd65f03c0),
            'PE InitIOKit/ConfigureIOKit sequence or normal epilogue changed')
    require([va for va in range(CALLEE_VA,CALLEE_END,4) if image.word(va)==0xd65f03c0]==[CALLEE_RET],
            'Expected unique PE_init_iokit normal RET')
    require(target(image.word(INIT_SITE),INIT_SITE,0x94000000)==INIT_VA and
            target(image.word(CONFIGURE_SITE),CONFIGURE_SITE,0x94000000)==CONFIGURE_VA,
            'PE core IOKit call targets changed')
    require(image.word(CONFIGURE_TAIL)==0x14014fcf and
            target(image.word(CONFIGURE_TAIL),CONFIGURE_TAIL,0x14000000)==DEFAULTS_VA and
            image.word(DEFAULTS_TAIL)==0x17fffabd and
            target(image.word(DEFAULTS_TAIL),DEFAULTS_TAIL,0x14000000)==UUID_VA,
            'ConfigureIOKit/defaults tail-call chain changed')
    require(image.body(DEFAULTS_TAIL-8,8)==struct.pack('<2I',0xa9417bfd,0xa8c24ff4) and
            image.body(UUID_RET-20,24)==struct.pack('<6I',
                0xa94e7bfd,0xa94d4ff4,0xa94c57f6,0xa94b5ff8,0x9103c3ff,0xd65f03c0),
            'Configure defaults/UUID stack restoration or RET changed')
    require(image.word(0xfffffff0078e6ad4)==0xd63f0100 and
            image.word(0xfffffff0078e6ad8)==0x36000220 and
            image.word(0xfffffff0078e6aec)==0xd63f0100 and
            image.word(0xfffffff0078e6b08)==0xd61f0000,
            'InitIOKit virtual initialization/check or optional tail callback changed')
    require(image.word(OLD_R18_SITE)==0x941c41c3 and image.word(OLD_R18_SITE-4)==0x528001e0,
            'Original SYSCTL stage call/argument changed')
    require(image.word(IRQ_CHECK_SITE)==0x94054dac and image.word(IRQ_ENABLE_SITE)==0x94054af0,
            'Later IRQ check/enable instructions changed')
    require(image.word(ENTRY)==branch(ENTRY,FIRST_CPU) and image.word(MMU)==0xd5181000 and
            image.word(POST_MMU)==0xd2a00600,'Original entry/MMU markers changed')
    for va in CAVES.values():
        require(image.body(va,96)==bytes(96),'Original probe cave is not empty')
        require([(s,n) for s,n,start,size in image.sections if start<=va and va+96<=start+size]
                ==[('__TEXT_EXEC','__text')],'Cave outside expected executable section')
    return image

def expected_regions(image):
    cave=CAVES[4]
    words=[branch(cave,CALLEE_VA,True),
           0xd53be021,0x580001c2,0x8b020022,0xd5033fdf,0xd53be021,0xeb02003f,0x54ffffa3,
           0x52800120,0x72b08000,0xaa1f03e1,0xaa1f03e2,0xaa1f03e3,
           0xd4000003,0xd503205f,0x17ffffff]
    require(words[0]==0x9416347a and branch(CALL_SITE,cave)==0x14054000,
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
    require(sha(derived)==EXPECTED_KERNEL_SHA,'Independent full digest requirement changed')
    return bytes(derived)

def probe_readback(original,patched):
    image=original_layout(original)
    require(len(patched)==KERNEL_BYTES,'Diagnostic kernel length changed')
    candidate=MachO(patched)
    require(candidate.entry==image.entry and candidate.names==image.names and
            patched[:image.header_end]==original[:image.header_end],'Kernel metadata changed')
    regions=[]
    for va,expected in expected_regions(image):
        offset=image.offset(va,len(expected))
        require(patched[offset:offset+len(expected)]==expected,'Exact hook/cave sequence differs at '+hex(va))
        regions.append((offset,offset+len(expected),va))
    cursor,changed=0,0
    counts=[]
    for low,high,va in sorted(regions):
        require(low>=cursor and patched[cursor:low]==original[cursor:low],'Bytes changed outside allowed hook/cave regions')
        count=sum(a!=b for a,b in zip(patched[low:high],original[low:high]))
        counts.append({'va':hex(va),'file_start':hex(low),'file_end':hex(high),'changed_bytes':count})
        changed+=count;cursor=high
    require(patched[cursor:]==original[cursor:],'Kernel tail changed outside allowed regions')
    require(candidate.body(CAVES[3],96)==bytes(96) and candidate.word(POST_MMU)==image.word(POST_MMU),
            'Stage 3 stop must remain disabled')
    for va in EARLIER_SITES:
        require(candidate.word(va)==image.word(va),'An earlier r6-r18 stop leaked into r19')
    preserved={}
    for name in ('_kernel_bootstrap','_kernel_startup_initialize_upto','_sched_startup','_early_random',
                 '_PE_init_iokit','_PE_init_printf','_InitIOKit','_ConfigureIOKit',DEFAULTS_NAME,UUID_NAME,
                 '__ZN22IOPlatformExpertDevice11createNVRAMEv','_IOLibInit','_OSlibkernInit',
                 '__Z20IOMachPortInitializev','__Z23interruptAccountingInitv','_PE_lockdown_iokit',
                 '_StartIOKitMatching','_ml_get_interrupts_enabled','_ml_set_interrupts_enabled',
                 '_panic','_panic_trap_to_debugger'):
        low,high=image.names[name],image.end(name)
        require(candidate.body(low,high-low)==image.body(low,high-low),'Preserved function changed: '+name)
        preserved[name]={'start':hex(low),'end':hex(high),'sha256':sha(candidate.body(low,high-low))}
    require(changed==194 and sha(patched)==EXPECTED_KERNEL_SHA,
            'Kernel differs from independently precomputed complete byte requirements')
    return {'status':'passed-host-byte-verification','sha256':sha(patched),'bytes':len(patched),
            'changed_bytes':changed,'caller':CALLER_NAME,'callee':CALLEE_NAME,
            'caller_range':[hex(CALLER_VA),hex(CALLER_END)],'callee_range':[hex(CALLEE_VA),hex(CALLEE_END)],
            'call_site':hex(CALL_SITE),'original_call_word':hex(ORIGINAL_CALL_WORD),'occurrence':1,
            'caller_target_call_count':1,'hook_word':'0x14054000','cave_first_bl_word':'0x9416347a',
            'callee_has_direct_ret':True,'callee_normal_ret_va':hex(CALLEE_RET),
            'init_iokit_call_va':hex(INIT_SITE),'configure_iokit_call_va':hex(CONFIGURE_SITE),
            'configure_tail_chain':['ConfigureIOKit','IOPlatformExpertDevice::configureDefaults','IOPlatformExpertDevice::generatePlatformUUID'],
            'configure_chain_final_ret_va':hex(UUID_RET),
            'later_irq_check_site':hex(IRQ_CHECK_SITE),'later_irq_enable_call_site':hex(IRQ_ENABLE_SITE),
            'reset_probe':True,'reset_stage':4,'reset_before':False,'reset_after_callee_returns':True,
            'reset_delay_seconds':6,'timebase_hz':19200000,'delay_ticks':115200000,
            'counter':'CNTPCT_EL0','reset_function':'0x84000009','conduit':'SMC #0',
            'stage3_disabled':True,'r6_through_r18_early_hooks_restored':True,
            'stage1_stage2_exact_words_verified':True,'all_other_bytes_identical':True,
            'header_and_symbols_unchanged':True,'preserved_functions':preserved,'permitted_regions':counts,
            'original_sp_and_argument_registers_preserved_before_call':True,
            'return_address_change':'PE_init_iokit restores LR=cave+4 and RETs there after actual InitIOKit/ConfigureIOKit paths return; original IRQ-state check is not resumed',
            'expected_digest_origin':'Independent instruction derivation before official generation',
            'hardware_tested':False,
            'scope':'Actual PE_init_iokit initialization/configuration return only; no assertion of successful optional properties/NVRAM/UUID, general driver matching, asynchronous IOKit work, IRQ/preemption, root or userland'}

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
