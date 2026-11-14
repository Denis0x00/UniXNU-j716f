#!/usr/bin/env python3
"""Independent r18 pre-IOKit services/SYSCTL return checkpoint contract.

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
EXPECTED_KERNEL_SHA = '042666ef61099602b7c26a217cef110da2f54831a989e886e8d6d25c4f3f6654'
CALLER_NAME, CALLEE_NAME = '_kernel_bootstrap_thread', '_kernel_startup_initialize_upto'
CALLER_VA, CALLER_END = 0xfffffff0072c8370, 0xfffffff0072c88cc
CALLEE_VA, CALLEE_END, CALLEE_RET = 0xfffffff0079d8ca8, 0xfffffff0079d8dd8, 0xfffffff0079d8dbc
CALL_SITE, ORIGINAL_CALL_WORD = 0xfffffff0072c859c, 0x941c41c3
CALL_SITES = (0xfffffff0072c8458,CALL_SITE,0xfffffff0072c8630,0xfffffff0072c874c)
CALL_WORDS = (0x941c4214,ORIGINAL_CALL_WORD,0x941c419e,0x941c4157)
ARGUMENT_WORDS = (0x528001c0,0x528001e0,0x52800200,0x12800000)
ARGUMENT_VA, ARGUMENT_WORD, STARTUP_PHASE = CALL_SITE-4,0x528001e0,15
IOKIT_SITE, IRQ_ENABLE_SITE = 0xfffffff0072c8620,0xfffffff0072c8638
OLD_R17_SITE, OLD_R16_SITE, OLD_R15_SITE = 0xfffffff0072c8414,0xfffffff0072c7ed0,0xfffffff0072c7e58
OLD_R14_SITE, OLD_R13_SITE = 0xfffffff0072c7da8,0xfffffff0072c7bd8
OLD_R12_SITE, OLD_R11_SITE, OLD_R10_SITE = 0xfffffff0072c7b50,0xfffffff0072c7b0c,0xfffffff0072c7b08
OLD_FIQ_SITE, OLD_R7_SITE, OLD_R8_SITE = 0xfffffff0079e09d0,0xfffffff0079e0a70,0xfffffff00741bd38
EARLIER_SITES = (OLD_FIQ_SITE,OLD_R7_SITE,OLD_R8_SITE,OLD_R10_SITE,OLD_R11_SITE,
                 OLD_R12_SITE,OLD_R13_SITE,OLD_R14_SITE,OLD_R15_SITE,OLD_R16_SITE,OLD_R17_SITE)
RESET_SETTINGS = {
    'entry_probe_reset_delay':6,
    'entry_probe_reset_after':CALLEE_NAME,
    'entry_probe_reset_in':CALLER_NAME,
    'entry_probe_reset_occurrence':2,
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
    for name,low,high in ((CALLER_NAME,CALLER_VA,CALLER_END),(CALLEE_NAME,CALLEE_VA,CALLEE_END)):
        require(name not in image.ambiguous_names and image.names.get(name)==low and image.end(name)==high,
                'Changed or ambiguous selected symbol: '+name)
        require({n for n,v in image.names.items() if v==low}=={name},'Selected symbol has an address alias')
    calls=[va for va in range(CALLER_VA,CALLER_END,4)
           if image.word(va)&0xfc000000==0x94000000 and
           target(image.word(va),va,0x94000000)==CALLEE_VA]
    require(calls==list(CALL_SITES),'Expected exactly four startup-init calls in bootstrap thread')
    require(tuple(image.word(va) for va in CALL_SITES)==CALL_WORDS and
            tuple(image.word(va-4) for va in CALL_SITES)==ARGUMENT_WORDS,
            'Startup-init call occurrences/phase arguments changed')
    require(image.body(CALLEE_RET-28,32)==struct.pack('<8I',
            0xb90282b3,0xa9477bfd,0xa9464ff4,0xa94557f6,
            0xa9445ff8,0xa94367fa,0x910203ff,0xd65f03c0),
            'startup phase publication / FP/LR/SP restoration / normal RET changed')
    require(image.word(0xfffffff0079d8cd4)==0xaa0003f3 and
            image.names['_startup_phase']==0xfffffff007169280,
            'Callee saved upto argument or phase storage identity changed')
    for va,name,word in ((OLD_R17_SITE,'_sched_startup',0x97ffd484),
                         (0xfffffff0072c8450,'_thread_daemon_init',0x9400553c),
                         (0xfffffff0072c8498,'_thread_bind',0x97ffb681),
                         (0xfffffff0072c84d4,'_mapping_adjust',0x9404fafb),
                         (0xfffffff0072c8510,'_clock_service_create',0x97fec1a0),
                         (0xfffffff0072c8514,'_device_service_create',0x94043fa7),
                         (0xfffffff0072c8518,'_phys_carveout_init',0x97fed5ce),
                         (0xfffffff0072c8594,'_bootprofile_init',0x94048003),
                         (IOKIT_SITE,'_PE_init_iokit',0x941b747a),
                         (IRQ_ENABLE_SITE,'_ml_set_interrupts_enabled',0x94054af0)):
        require(image.word(va)==word and target(word,va,0x94000000)==image.names[name],
                'Service-stage adjacent call changed: '+name)
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
    require(words[0]==0x941701a2 and branch(CALL_SITE,cave)==0x14054021,
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
        require(candidate.word(va)==image.word(va),'An earlier r6-r17 stop leaked into r18')
    for va in CALL_SITES:
        require(candidate.word(va-4)==image.word(va-4),'A phase argument changed')
        if va!=CALL_SITE:require(candidate.word(va)==image.word(va),'An unselected startup occurrence changed')
    preserved={}
    for name in ('_kernel_bootstrap','_kernel_startup_initialize_upto','_kernel_startup_log','_sched_startup',
                 '_thread_daemon_init','_thread_bind','_mapping_adjust','_clock_service_create',
                 '_device_service_create','_phys_carveout_init','_kdp_init','_alternate_debugger_init',
                 '_bootprofile_init','_early_random','_PE_init_iokit','_InitIOKit','_ConfigureIOKit',
                 '_ml_get_interrupts_enabled','_ml_set_interrupts_enabled','_panic','_panic_trap_to_debugger'):
        low,high=image.names[name],image.end(name)
        require(candidate.body(low,high-low)==image.body(low,high-low),'Preserved function changed: '+name)
        preserved[name]={'start':hex(low),'end':hex(high),'sha256':sha(candidate.body(low,high-low))}
    require(changed==194 and sha(patched)==EXPECTED_KERNEL_SHA,
            'Kernel differs from independently precomputed complete byte requirements')
    return {'status':'passed-host-byte-verification','sha256':sha(patched),'bytes':len(patched),
            'changed_bytes':changed,'caller':CALLER_NAME,'callee':CALLEE_NAME,
            'caller_range':[hex(CALLER_VA),hex(CALLER_END)],'callee_range':[hex(CALLEE_VA),hex(CALLEE_END)],
            'call_site':hex(CALL_SITE),'original_call_word':hex(ORIGINAL_CALL_WORD),'occurrence':2,
            'caller_target_call_count':4,'argument_va':hex(ARGUMENT_VA),'argument_word':hex(ARGUMENT_WORD),
            'argument_upto':STARTUP_PHASE,'argument_name':'STARTUP_SUB_SYSCTL',
            'hook_word':'0x14054021','cave_first_bl_word':'0x941701a2',
            'callee_has_direct_ret':True,'callee_normal_ret_va':hex(CALLEE_RET),
            'common_return_publishes_startup_phase':True,
            'other_startup_occurrences_unchanged':True,'later_iokit_call_site':hex(IOKIT_SITE),
            'later_irq_enable_call_site':hex(IRQ_ENABLE_SITE),'iokit_reached_or_executed':False,
            'reset_probe':True,'reset_stage':4,'reset_before':False,'reset_after_callee_returns':True,
            'reset_delay_seconds':6,'timebase_hz':19200000,'delay_ticks':115200000,
            'counter':'CNTPCT_EL0','reset_function':'0x84000009','conduit':'SMC #0',
            'stage3_disabled':True,'r6_through_r17_early_hooks_restored':True,
            'stage1_stage2_exact_words_verified':True,'all_other_bytes_identical':True,
            'header_and_symbols_unchanged':True,'preserved_functions':preserved,'permitted_regions':counts,
            'original_sp_and_argument_registers_preserved_before_call':True,
            'return_address_change':'kernel_startup_initialize_upto(15) returns to cave+4; following random initialization and IOKit call are not resumed',
            'expected_digest_origin':'Independent instruction derivation before official generation',
            'hardware_tested':False,
            'scope':'Bootstrap-thread core-service actual path and eligible startup callbacks through SYSCTL phase15 only; later address-randomization/IOKit/EARLY_BOOT/IRQ enable unexecuted; no proof of ongoing services, scheduling, timer IRQ, root or userland'}

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
