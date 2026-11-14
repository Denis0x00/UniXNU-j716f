#!/usr/bin/env python3
"""Independent r14 bootstrap-Mach-IPC return checkpoint byte contract.

Standard library only; never calls the upstream patcher, VM or a device.
The expected digest was derived from original bytes and these instructions
before official generation. It is not a runtime or complete-boot assertion.
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
EXPECTED_KERNEL_SHA = '6ec24a01d1323263361ee611b1123be3c47c6eaddeb25f1959df3385e6a7f728'
CALLER_NAME, CALLEE_NAME = '_kernel_bootstrap', '_kernel_startup_initialize_upto'
CALLER_VA, CALLER_END = 0xfffffff0072c7ae4, 0xfffffff0072c8220
CALLEE_VA, CALLEE_END, CALLEE_RET = 0xfffffff0079d8ca8, 0xfffffff0079d8dd8, 0xfffffff0079d8dbc
CALL_SITE, ORIGINAL_CALL_WORD = 0xfffffff0072c7da8, 0x941c43c0
FIRST_CALL_SITE = 0xfffffff0072c7bd8
OLD_R13_SITE = FIRST_CALL_SITE
ARGUMENT_VA, ARGUMENT_WORD, STARTUP_PHASE = CALL_SITE-4, 0x528001a0, 13
OLD_R12_SITE = 0xfffffff0072c7b50
OLD_R10_SITE, OLD_R11_SITE = 0xfffffff0072c7b08, 0xfffffff0072c7b0c
OLD_FIQ_SITE, OLD_R7_SITE, OLD_R8_SITE = 0xfffffff0079e09d0, 0xfffffff0079e0a70, 0xfffffff00741bd38
MACHINE_TAIL_B = 0xfffffff00741bd40
RESET_SETTINGS = {
    'entry_probe_reset_delay': 6,
    'entry_probe_reset_after': CALLEE_NAME,
    'entry_probe_reset_in': CALLER_NAME,
    'entry_probe_reset_occurrence': 2,
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
    for name,low,high in ((CALLER_NAME,CALLER_VA,CALLER_END),(CALLEE_NAME,CALLEE_VA,CALLEE_END)):
        require(name not in image.ambiguous_names and image.names.get(name)==low and image.end(name)==high,
                'Changed or ambiguous selected symbol: '+name)
        require({n for n,v in image.names.items() if v==low}=={name}, 'Selected symbol has an address alias')
    calls=[va for va in range(CALLER_VA,CALLER_END,4)
           if image.word(va)&0xfc000000==0x94000000 and
           target(image.word(va),va,0x94000000)==CALLEE_VA]
    require(calls==[FIRST_CALL_SITE,CALL_SITE] and image.word(CALL_SITE)==ORIGINAL_CALL_WORD,
            'Expected second of exactly two startup-init BL calls in kernel_bootstrap')
    require(image.word(ARGUMENT_VA)==ARGUMENT_WORD and
            image.word(FIRST_CALL_SITE-4)==0x52800180 and image.word(FIRST_CALL_SITE)==0x941c4434,
            'OSLOG phase12 first call / MACH_IPC phase13 second call changed')
    require(image.body(CALLEE_RET-28,32)==struct.pack('<8I',
            0xb90282b3,0xa9477bfd,0xa9464ff4,0xa94557f6,
            0xa9445ff8,0xa94367fa,0x910203ff,0xd65f03c0),
            'startup phase publication / FP/LR/SP restoration / normal RET changed')
    require(image.word(0xfffffff0079d8cd4)==0xaa0003f3 and
            image.names['_startup_phase']==0xfffffff007169280,
            'Callee saved upto argument or phase storage identity changed')
    require(image.word(MACHINE_TAIL_B)==0x17faaf69 and
            target(image.word(MACHINE_TAIL_B),MACHINE_TAIL_B,0x14000000)==CALLER_VA,
            'machine_startup tail-B to kernel_bootstrap changed')
    require(image.word(ENTRY)==branch(ENTRY,FIRST_CPU) and image.word(MMU)==0xd5181000 and
            image.word(POST_MMU)==0xd2a00600, 'Original entry/MMU markers changed')
    for va in CAVES.values():
        require(image.body(va,96)==bytes(96), 'Original probe cave is not empty')
        require([(s,n) for s,n,start,size in image.sections if start<=va and va+96<=start+size]
                ==[('__TEXT_EXEC','__text')], 'Cave outside expected executable section')
    for va,name,word in ((OLD_R10_SITE,'_printf',0x97ff834a),
                         (OLD_R11_SITE,'_scale_setup',0x940001c5),
                         (OLD_R12_SITE,'_vm_mem_bootstrap',0x941c53eb),
                         (FIRST_CALL_SITE-8,'_atm_init',0x94018c98),
                         (CALL_SITE-8,'_mac_policy_init',0x941b7f82)):
        require(image.word(va)==word and target(image.word(va),va,0x94000000)==image.names[name],
                'Preceding initialization/logging boundary changed: '+name)
    return image

def expected_regions(image):
    """Expected AArch64 bytes derived independently of upstream assembly."""
    cave=CAVES[4]
    words=[branch(cave,CALLEE_VA,True),
           0xd53be021,0x580001c2,0x8b020022,
           0xd5033fdf,0xd53be021,0xeb02003f,0x54ffffa3,
           0x52800120,0x72b08000,0xaa1f03e1,0xaa1f03e2,0xaa1f03e3,
           0xd4000003,0xd503205f,0x17ffffff]
    require(words[0]==0x941701a2 and branch(CALL_SITE,cave)==0x1405421e,
            'Independent branch requirement changed')
    reset=(struct.pack('<16I',*words)+struct.pack('<Q',115200000)).ljust(96,b'\0')
    return [(ENTRY,struct.pack('<I',branch(ENTRY,CAVES[1]))),
            (MMU,struct.pack('<I',branch(MMU,CAVES[2]))),
            (CALL_SITE,struct.pack('<I',branch(CALL_SITE,cave))),
            (CAVES[1],paint(1)),(CAVES[2],paint(2)),(cave,reset)]

def derive_expected(original):
    """In-memory reference only; never a payload input or official build output."""
    image=original_layout(original)
    derived=bytearray(original)
    for va,raw in expected_regions(image):
        offset=image.offset(va,len(raw))
        derived[offset:offset+len(raw)]=raw
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
        require(patched[offset:offset+len(expected)]==expected,
                'Exact hook/cave sequence differs at '+hex(va))
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
    for va in (OLD_FIQ_SITE,OLD_R7_SITE,OLD_R8_SITE,OLD_R10_SITE,OLD_R11_SITE,OLD_R12_SITE,OLD_R13_SITE):
        require(candidate.word(va)==image.word(va), 'An earlier r6/r7/r8/r10/r11/r12/r13 stop leaked into r14')
    preserved={}
    for name in ('_printf','_vprintf_internal','_machine_startup','_machine_conf','_scale_setup',
                 '_bsd_scale_setup','_bsd_exec_setup','_vm_mem_bootstrap','_vm_page_bootstrap',
                 '_atm_init','_kernel_startup_initialize_upto','_kernel_startup_log',
                 '_console_init','_stackshot_init','_sched_init','_mac_policy_init','_PE_create_console','_fiq_context_init','_panic','_panic_trap_to_debugger'):
        low,high=image.names[name],image.end(name)
        require(candidate.body(low,high-low)==image.body(low,high-low), 'Preserved function changed: '+name)
        preserved[name]={'start':hex(low),'end':hex(high),'sha256':sha(candidate.body(low,high-low))}
    require(changed==194 and sha(patched)==EXPECTED_KERNEL_SHA,
            'Kernel differs from independently precomputed complete byte requirements')
    return {'status':'passed-host-byte-verification','sha256':sha(patched),'bytes':len(patched),
            'changed_bytes':changed,'caller':CALLER_NAME,'callee':CALLEE_NAME,
            'caller_range':[hex(CALLER_VA),hex(CALLER_END)],'callee_range':[hex(CALLEE_VA),hex(CALLEE_END)],
            'call_site':hex(CALL_SITE),'original_call_word':hex(ORIGINAL_CALL_WORD),'occurrence':2,
            'caller_target_call_count':2,'argument_word':hex(ARGUMENT_WORD),'argument_va':hex(ARGUMENT_VA),
            'argument_upto':STARTUP_PHASE,'argument_name':'STARTUP_SUB_MACH_IPC',
            'common_return_publishes_startup_phase':True,
            'hook_word':'0x1405421e','cave_first_bl_word':'0x941701a2','callee_normal_ret_va':hex(CALLEE_RET),
            'reset_probe':True,'reset_stage':4,'reset_before':False,'reset_after_callee_returns':True,
            'reset_delay_seconds':6,'timebase_hz':19200000,'delay_ticks':115200000,
            'counter':'CNTPCT_EL0','reset_function':'0x84000009','conduit':'SMC #0',
            'stage3_disabled':True,'r6_r7_r8_r10_r11_r12_r13_early_hooks_restored':True,
            'stage1_stage2_exact_words_verified':True,'all_other_bytes_identical':True,
            'header_and_symbols_unchanged':True,'preserved_functions':preserved,
            'permitted_regions':counts,'original_sp_and_argument_registers_preserved_before_call':True,
            'return_address_change':'kernel_startup_initialize_upto(13) returns to cave+4; original next instruction is not resumed',
            'expected_digest_origin':'Independent instruction derivation before official generation',
            'hardware_tested':False,
            'scope':'After startup initialization through Mach IPC phase 13 only; later bootstrap/IOKit/root/userland and callback-specific hardware success remain unproved'}

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
