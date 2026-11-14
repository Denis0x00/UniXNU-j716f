#!/usr/bin/env python3
"""Independent r20 InitIOKit-return split of the unresolved r19 interval.

Standard library only; never invokes the official patcher, VM or device.
The whole-kernel expectation was derived before official generation.
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
EXPECTED_KERNEL_SHA='fb7453f96ff26ae936fe6c22ff714093f1b1420f2e049f3d7a6d3e87e3473cba'
CALLER_NAME,CALLEE_NAME='_PE_init_iokit','_InitIOKit'
CALLER_VA,CALLER_END=0xfffffff0079a5808,0xfffffff0079a5c10
CALLEE_VA,CALLEE_END,CALLEE_RET=0xfffffff0078e6a38,0xfffffff0078e6b38,0xfffffff0078e6b18
CALL_SITE,ORIGINAL_CALL_WORD=0xfffffff0079a5ba0,0x97fd03a6
ARGUMENT_LOAD_VA,ARGUMENT_LOAD_WORD=0xfffffff0079a5b9c,0xf9466d00
ARGUMENT_POINTER_ADDRESS=0xfffffff007201cd8
OPTIONAL_TAIL_BR,OPTIONAL_TAIL_WORD=0xfffffff0078e6b08,0xd61f0000
INIT_STATUS_CHECK=0xfffffff0078e6ad8
CONFIGURE_SITE,PE_RET=0xfffffff0079a5ba4,0xfffffff0079a5bb8
OLD_R19_SITE,OLD_R18_SITE,OLD_R17_SITE=0xfffffff0072c8620,0xfffffff0072c859c,0xfffffff0072c8414
OLD_R16_SITE,OLD_R15_SITE=0xfffffff0072c7ed0,0xfffffff0072c7e58
OLD_R14_SITE,OLD_R13_SITE=0xfffffff0072c7da8,0xfffffff0072c7bd8
OLD_R12_SITE,OLD_R11_SITE,OLD_R10_SITE=0xfffffff0072c7b50,0xfffffff0072c7b0c,0xfffffff0072c7b08
OLD_FIQ_SITE,OLD_R7_SITE,OLD_R8_SITE=0xfffffff0079e09d0,0xfffffff0079e0a70,0xfffffff00741bd38
EARLIER_SITES=(OLD_FIQ_SITE,OLD_R7_SITE,OLD_R8_SITE,OLD_R10_SITE,OLD_R11_SITE,OLD_R12_SITE,
               OLD_R13_SITE,OLD_R14_SITE,OLD_R15_SITE,OLD_R16_SITE,OLD_R17_SITE,OLD_R18_SITE,OLD_R19_SITE)
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
    for name,low,high in ((CALLER_NAME,CALLER_VA,CALLER_END),(CALLEE_NAME,CALLEE_VA,CALLEE_END)):
        require(name not in image.ambiguous_names and image.names.get(name)==low and image.end(name)==high,
                'Changed or ambiguous selected symbol: '+name)
        require({n for n,v in image.names.items() if v==low}=={name},'Selected symbol has an address alias')
    calls=[va for va in range(CALLER_VA,CALLER_END,4)
           if image.word(va)&0xfc000000==0x94000000 and
           target(image.word(va),va,0x94000000)==CALLEE_VA]
    require(calls==[CALL_SITE] and image.word(CALL_SITE)==ORIGINAL_CALL_WORD,
            'Expected unique InitIOKit call in PE_init_iokit')
    require(image.body(CALL_SITE-8,8)==struct.pack('<2I',0x90ffc2e8,ARGUMENT_LOAD_WORD) and
            image.names['_PE_state']==0xfffffff007201c40 and
            image.word(CALLEE_VA+16)==0xaa0003f3,
            'Device-tree pointer argument materialization/saved x0 changed')
    adrp=image.word(CALL_SITE-8)
    page_delta=((adrp>>5)&0x7ffff)*4+((adrp>>29)&3)
    if page_delta&(1<<20):page_delta-=1<<21
    base=((CALL_SITE-8)&~0xfff)+(page_delta<<12)
    require(base+(((ARGUMENT_LOAD_WORD>>10)&0xfff)*8)==ARGUMENT_POINTER_ADDRESS and
            ARGUMENT_POINTER_ADDRESS==image.names['_PE_state']+0x98,
            'Argument pointer load does not resolve to PE_state.deviceTreeHead storage')
    restore=(0xa9427bfd,0xa9414ff4,0x9100c3ff)
    require(image.body(CALLEE_RET-12,16)==struct.pack('<4I',*restore,0xd65f03c0) and
            image.body(OPTIONAL_TAIL_BR-12,16)==struct.pack('<4I',*restore,OPTIONAL_TAIL_WORD),
            'InitIOKit direct/tail return restoration changed')
    require([va for va in range(CALLEE_VA,CALLEE_END,4) if image.word(va)==0xd65f03c0]==[CALLEE_RET],
            'Expected unique direct InitIOKit RET')
    require(image.word(0xfffffff0078e6af8)==0xb40000a0 and
            image.word(0xfffffff0078e6ad4)==0xd63f0100 and
            image.word(INIT_STATUS_CHECK)==0x36000220 and image.word(0xfffffff0078e6aec)==0xd63f0100,
            'Optional callback selector or platform virtual-init path changed')
    require(image.word(CONFIGURE_SITE)==0x97fd03e5 and
            target(image.word(CONFIGURE_SITE),CONFIGURE_SITE,0x94000000)==image.names['_ConfigureIOKit'] and
            image.word(PE_RET)==0xd65f03c0,
            'Following ConfigureIOKit call/PE return changed')
    require(image.word(OLD_R19_SITE)==0x941b747a and
            target(image.word(OLD_R19_SITE),OLD_R19_SITE,0x94000000)==CALLER_VA and
            image.word(OLD_R18_SITE)==0x941c41c3 and image.word(OLD_R18_SITE-4)==0x528001e0,
            'Original r18/r19 sites changed')
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
    require(words[0]==0x94133906 and branch(CALL_SITE,cave)==0x17e9caa0,
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
        require(candidate.word(va)==image.word(va),'An earlier r6-r19 stop leaked into r20')
    preserved={}
    for name in ('_kernel_bootstrap','_kernel_bootstrap_thread','_kernel_startup_initialize_upto','_early_random',
                 '_PE_init_printf','_InitIOKit','_ConfigureIOKit','_PE_state',
                 '__ZN22IOPlatformExpertDevice17configureDefaultsEv',
                 '__ZN22IOPlatformExpertDevice20generatePlatformUUIDEv','_IOLibInit','_OSlibkernInit',
                 '__Z20IOMachPortInitializev','__Z23interruptAccountingInitv','_PE_lockdown_iokit',
                 '_StartIOKitMatching','_ml_get_interrupts_enabled','_ml_set_interrupts_enabled',
                 '_panic','_panic_trap_to_debugger'):
        low,high=image.names[name],image.end(name)
        require(candidate.body(low,high-low)==image.body(low,high-low),'Preserved function/data changed: '+name)
        preserved[name]={'start':hex(low),'end':hex(high),'sha256':sha(candidate.body(low,high-low))}
    require(changed==194 and sha(patched)==EXPECTED_KERNEL_SHA,
            'Kernel differs from independently precomputed complete byte requirements')
    return {'status':'passed-host-byte-verification','sha256':sha(patched),'bytes':len(patched),
            'changed_bytes':changed,'caller':CALLER_NAME,'callee':CALLEE_NAME,
            'caller_range':[hex(CALLER_VA),hex(CALLER_END)],'callee_range':[hex(CALLEE_VA),hex(CALLEE_END)],
            'call_site':hex(CALL_SITE),'original_call_word':hex(ORIGINAL_CALL_WORD),'occurrence':1,
            'caller_target_call_count':1,'hook_word':'0x17e9caa0','cave_first_bl_word':'0x94133906',
            'callee_has_direct_ret':True,'callee_normal_ret_va':hex(CALLEE_RET),
            'optional_tail_callback_branch_va':hex(OPTIONAL_TAIL_BR),'optional_tail_callback_word':hex(OPTIONAL_TAIL_WORD),
            'optional_tail_restores_sp_and_cave_lr':True,'runtime_optional_callback_observed':False,
            'argument_register':'x0','argument_load_va':hex(ARGUMENT_LOAD_VA),
            'argument_pointer_storage':hex(ARGUMENT_POINTER_ADDRESS),'argument_semantics':'PE_state.deviceTreeHead',
            'following_configure_iokit_call_va':hex(CONFIGURE_SITE),'configure_iokit_executed':False,
            'reset_probe':True,'reset_stage':4,'reset_before':False,'reset_after_callee_returns':True,
            'reset_delay_seconds':6,'timebase_hz':19200000,'delay_ticks':115200000,
            'counter':'CNTPCT_EL0','reset_function':'0x84000009','conduit':'SMC #0',
            'stage3_disabled':True,'r6_through_r19_early_hooks_restored':True,
            'stage1_stage2_exact_words_verified':True,'all_other_bytes_identical':True,
            'header_and_symbols_unchanged':True,'preserved_functions':preserved,'permitted_regions':counts,
            'original_sp_and_argument_registers_preserved_before_call':True,
            'return_address_change':'InitIOKit direct RET or returning optional tail callback uses cave+4; following ConfigureIOKit is not resumed',
            'expected_digest_origin':'Independent instruction derivation before official generation',
            'hardware_tested':False,
            'scope':'Midpoint after actual InitIOKit return, including chosen callback path; no ConfigureIOKit/PE completion, general driver matching, async completion, IRQ/preemption, root or userland claim; missing reset is not callee-localized failure evidence'}

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
