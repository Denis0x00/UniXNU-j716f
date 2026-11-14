#!/usr/bin/env python3
"""Independent r16 clock_init tail-return checkpoint byte contract.

Standard library only. Never invokes the official patcher, VM or device.
Whole-kernel expectation was independently derived before official generation.
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
EXPECTED_KERNEL_SHA = 'ae0b78204335ec3a2851aab31e4503587fe1aa6ba9f51ff85fa3d28cee26aca0'
CALLER_NAME, CALLEE_NAME = '_kernel_bootstrap', '_clock_init'
CALLER_VA, CALLER_END = 0xfffffff0072c7ae4, 0xfffffff0072c8220
CALLEE_VA, CALLEE_END = 0xfffffff0072766a4, 0xfffffff0072766a8
CALL_SITE, ORIGINAL_CALL_WORD = 0xfffffff0072c7ed0, 0x97feb9f5
TAIL_B, TAIL_WORD = CALLEE_VA, 0x14000921
TAIL_NAME = '_clock_oldinit'
TAIL_VA, TAIL_END, CALLEE_RET = 0xfffffff007278b28, 0xfffffff007278b90, 0xfffffff007278b8c
INDIRECT_SITE, RTCLOCK_VA = 0xfffffff007278b74, 0xfffffff00742229c
OLD_R15_SITE, OLD_R14_SITE, OLD_R13_SITE = 0xfffffff0072c7e58, 0xfffffff0072c7da8, 0xfffffff0072c7bd8
OLD_R12_SITE, OLD_R11_SITE, OLD_R10_SITE = 0xfffffff0072c7b50, 0xfffffff0072c7b0c, 0xfffffff0072c7b08
OLD_FIQ_SITE, OLD_R7_SITE, OLD_R8_SITE = 0xfffffff0079e09d0, 0xfffffff0079e0a70, 0xfffffff00741bd38
EARLIER_SITES = (OLD_FIQ_SITE, OLD_R7_SITE, OLD_R8_SITE, OLD_R10_SITE,
                 OLD_R11_SITE, OLD_R12_SITE, OLD_R13_SITE, OLD_R14_SITE, OLD_R15_SITE)
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
    for name,low,high in ((CALLER_NAME,CALLER_VA,CALLER_END),
                          (CALLEE_NAME,CALLEE_VA,CALLEE_END),(TAIL_NAME,TAIL_VA,TAIL_END)):
        require(name not in image.ambiguous_names and image.names.get(name)==low and image.end(name)==high,
                'Changed or ambiguous selected symbol: '+name)
        require({n for n,v in image.names.items() if v==low}=={name}, 'Selected symbol has an address alias')
    calls=[va for va in range(CALLER_VA,CALLER_END,4)
           if image.word(va)&0xfc000000==0x94000000 and
           target(image.word(va),va,0x94000000)==CALLEE_VA]
    require(calls==[CALL_SITE] and image.word(CALL_SITE)==ORIGINAL_CALL_WORD,
            'Expected unique clock_init BL in kernel_bootstrap')
    require(image.word(TAIL_B)==TAIL_WORD and target(TAIL_WORD,TAIL_B,0x14000000)==TAIL_VA,
            'clock_init single tail-B changed')
    require(image.body(CALLEE_RET-12,16)==struct.pack('<4I',0xa9427bfd,0xa9414ff4,0xa8c357f6,0xd65f03c0),
            'clock_oldinit saved-register/SP restoration or RET changed')
    require([va for va in range(TAIL_VA,TAIL_END,4) if image.word(va)==0xd65f03c0]==[CALLEE_RET],
            'Expected unique clock_oldinit RET')
    require(image.body(INDIRECT_SITE-8,12)==struct.pack('<3I',0xf9400529,0xb4ffff49,0xd63f0120),
            'clock_oldinit callback load/null check/BLR changed')
    require(image.names['_rtclock_init']==RTCLOCK_VA and
            image.body(image.names['_sysclk_ops'],16)==struct.pack('<QQ',0,RTCLOCK_VA) and
            image.body(image.names['_calend_ops'],16)==bytes(16) and
            image.body(image.names['_clock_list'],32)==struct.pack('<4Q',image.names['_sysclk_ops'],0,image.names['_calend_ops'],0) and
            image.word(image.names['_clock_count'])==2,
            'Original static clock list or callback table changed')
    for va,name,word in ((OLD_R15_SITE,'_machine_init',0x94054f27),
                         (0xfffffff0072c7e94,'_thread_machine_init_template',0x9400527a),
                         (RTCLOCK_VA+12,'_clock_timebase_init',0x97f95100),
                         (RTCLOCK_VA+36,'_timer_resync_deadlines',0x97ffb02a)):
        require(image.word(va)==word and target(word,va,0x94000000)==image.names[name],
                'Selected initialization-chain call changed: '+name)
    require(image.word(OLD_R14_SITE)==0x941c43c0 and image.word(OLD_R14_SITE-4)==0x528001a0 and
            image.word(OLD_R13_SITE)==0x941c4434 and image.word(OLD_R13_SITE-4)==0x52800180,
            'Original phase12/phase13 calls and arguments changed')
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
    require(words[0]==0x97f97821 and branch(CALL_SITE,cave)==0x140541d4,
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
        require(candidate.word(va)==image.word(va), 'An earlier r6-r15 stop leaked into r16')
    preserved={}
    for name in ('_machine_init','_ml_lockdown_init','_mapping_free_prime','_clock_config','_clock_oldconfig',
                 '_thread_machine_init_template','_machine_thread_template_init','_clock_init','_clock_oldinit',
                 '_rtclock_init','_clock_timebase_init','_sched_timebase_init','_timer_resync_deadlines',
                 '_setPop','_ml_set_decrementer','_ml_set_interrupts_enabled','_clock_list','_clock_count',
                 '_sysclk_ops','_calend_ops','_kernel_startup_initialize_upto','_panic','_panic_trap_to_debugger'):
        low,high=image.names[name],image.end(name)
        require(candidate.body(low,high-low)==image.body(low,high-low), 'Preserved function/data changed: '+name)
        preserved[name]={'start':hex(low),'end':hex(high),'sha256':sha(candidate.body(low,high-low))}
    require(changed==194 and sha(patched)==EXPECTED_KERNEL_SHA,
            'Kernel differs from independently precomputed complete byte requirements')
    return {'status':'passed-host-byte-verification','sha256':sha(patched),'bytes':len(patched),
            'changed_bytes':changed,'caller':CALLER_NAME,'callee':CALLEE_NAME,
            'caller_range':[hex(CALLER_VA),hex(CALLER_END)],'callee_range':[hex(CALLEE_VA),hex(CALLEE_END)],
            'call_site':hex(CALL_SITE),'original_call_word':hex(ORIGINAL_CALL_WORD),'occurrence':1,
            'caller_target_call_count':1,'hook_word':'0x140541d4','cave_first_bl_word':'0x97f97821',
            'callee_has_direct_ret':False,'callee_tail_b_va':hex(TAIL_B),'callee_tail_b_word':hex(TAIL_WORD),
            'tail_return_symbol':TAIL_NAME,'tail_return_range':[hex(TAIL_VA),hex(TAIL_END)],
            'callee_normal_ret_va':hex(CALLEE_RET),'tail_preserves_cave_return_address':True,
            'static_sysclk_init_callback':'_rtclock_init','static_clock_list_count':2,
            'callback_indirect_blr_va':hex(INDIRECT_SITE),'runtime_callback_execution_observed':False,
            'reset_probe':True,'reset_stage':4,'reset_before':False,'reset_after_callee_returns':True,
            'reset_delay_seconds':6,'timebase_hz':19200000,'delay_ticks':115200000,
            'counter':'CNTPCT_EL0','reset_function':'0x84000009','conduit':'SMC #0',
            'stage3_disabled':True,'r6_through_r15_early_hooks_restored':True,
            'stage1_stage2_exact_words_verified':True,'all_other_bytes_identical':True,
            'header_and_symbols_unchanged':True,'preserved_functions':preserved,'permitted_regions':counts,
            'original_sp_and_argument_registers_preserved_before_call':True,
            'return_address_change':'clock_init tail-B preserves LR=cave+4; clock_oldinit saves/restores it and RETs there; original next instruction is not resumed',
            'expected_digest_origin':'Independent instruction derivation before official generation',
            'hardware_tested':False,
            'scope':'clock_init actual-path return through clock_oldinit and eligible callbacks only; static callback tables do not prove runtime callback execution; timer IRQ/EOI/scheduling/IOKit/root/userland unproved'}

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
