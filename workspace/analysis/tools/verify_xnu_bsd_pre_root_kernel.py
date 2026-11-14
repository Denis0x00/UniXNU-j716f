#!/usr/bin/env python3
"""r30 grouped BSD setup checkpoint, after inittodr and before root discovery.

Only the new call/return boundary is changed. Frozen r29 source/early-log/CPU0
provenance is inherited. This is not a root-filesystem or hardware-ready test.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
BASE_MODULE_SHA256 = 'e719ba486a124329278f20ab66de23ff728c1ee9c2b88bf91f43fddf4c5016a1'
BASE_CONTRACT_SHA256 = 'a9cc41748ae620a2d1a3158fcf2b132c9fee1ab88aed2b36ddc1bb4af2d279a8'
BASE_PATH = Path(__file__).with_name('verify_xnu_bsd_autoconf_kernel.py').resolve()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


require(sha(BASE_PATH.read_bytes()) == BASE_MODULE_SHA256, 'Frozen r29 verifier changed')
import verify_xnu_bsd_autoconf_kernel as previous
require(Path(previous.__file__).resolve() == BASE_PATH, 'Wrong frozen r29 import')
base = previous.base
MachO, branch, target, address = previous.MachO, previous.branch, previous.target, previous.address
CALLER_NAME, CALLEE_NAME = '_bsd_init', '_inittodr'
RESET_SETTINGS = dict(previous.RESET_SETTINGS, entry_probe_reset_after=CALLEE_NAME, entry_probe_reset_in=CALLER_NAME)
DELAY_TICKS, CAVE_BYTES = previous.DELAY_TICKS, previous.CAVE_BYTES
PARSER_SOURCE_SHA256 = previous.PARSER_SOURCE_SHA256
PROTOCOL_HEADER_SHA256 = previous.PROTOCOL_HEADER_SHA256
SOURCE_PATCH_SHA256 = previous.SOURCE_PATCH_SHA256
REVIEW_DIR = 'analysis/xnu-bsd-pre-root-r30-20261114/'
REVIEW_FLAGS = {'unique_bsd_call_and_return_reviewed': True, 'previous_iobsd_hook_restored': True,
                'grouped_network_and_root_boundary_reviewed': True, 'calendar_negative_branch_reviewed': True,
                'independent_expected_bytes_reviewed': True}
FUNCTIONS = {
    'caller': (CALLER_NAME, '6c976d0b87055ce3de88987a40888e668dec077c9c7ce5a2de2062132966c65f'),
    'callee': (CALLEE_NAME, '62acc79b4b0a3273dad316170597e8ea213047673242fae12d8f9ae24d978ed0'),
}


def validate_expectation(value):
    require(isinstance(value,dict) and type(value.get('schema_version')) is int and value['schema_version'] == 1,
            'Unsupported r30 expectation')
    require(value.get('status') == 'independently-frozen', 'Unreviewed r30 contract')
    baseline = value.get('baseline_contract')
    require(sha(canonical(baseline)) == BASE_CONTRACT_SHA256, 'Frozen r29 contract changed')
    previous.validate_expectation(baseline)
    require(value.get('base_module_sha256') == BASE_MODULE_SHA256 and value.get('baseline_contract_sha256') == BASE_CONTRACT_SHA256,
            'Missing frozen r29 provenance')
    for key in ('original','parser_source_sha256','protocol_header_sha256','source_patch_sha256'):
        require(value.get(key) == baseline[key], 'Changed inherited field: '+key)
    require(value.get('reset_settings') == RESET_SETTINGS and
            all(type(value['reset_settings'][key]) is type(want) for key,want in RESET_SETTINGS.items()),
            'Expected unique bsd_init/inittodr return and six-second reset')
    require(value.get('review') == REVIEW_FLAGS, 'Missing independent r30 review')
    expected = value.get('expected')
    base._exact_keys(expected,('sha256','changed_bytes'),'expected')
    base._digest(expected['sha256'],'expected.sha256')
    require(type(expected['changed_bytes']) is int and 0 < expected['changed_bytes'] <= 300,'Bad changed byte count')
    evidence = value.get('review_evidence')
    require(isinstance(evidence,dict) and set(baseline['review_evidence']) <= set(evidence),'Missing inherited provenance')
    for path,spec in baseline['review_evidence'].items():
        require(evidence[path] == spec,'Changed inherited provenance: '+path)
    for name in ('machine-review.json','machine-code.json','review_machine.py'):
        require(REVIEW_DIR+name in evidence,'Missing r30 review: '+name)
        spec = evidence[REVIEW_DIR+name]
        base._exact_keys(spec,('sha256','bytes'),name)
        base._digest(spec['sha256'],name)
        require(type(spec['bytes']) is int and spec['bytes']>0,'Bad evidence size')
    require(isinstance(value.get('layout'),dict),'Missing r30 layout')
    return value


def load_expectation(path):
    def unique(pairs):
        result={}
        for key,value in pairs:
            require(key not in result,'Duplicate JSON expectation key: '+key)
            result[key]=value
        return result
    return validate_expectation(json.loads(Path(path).read_text(encoding='utf-8'),object_pairs_hook=unique))


def _discover(original, baseline):
    image,old = previous.original_layout(original,baseline)
    functions={}
    for key,(name,digest) in FUNCTIONS.items():
        functions[key]=base._function(image,name)
        require(functions[key]['sha256']==digest,'Reviewed BSD pre-root function changed: '+name)
    caller,callee=functions['caller'],functions['callee']
    dest=address(callee['start'])
    calls=[va for va in range(address(caller['start']),address(caller['end']),4)
           if image.word(va)&0xfc000000==0x94000000 and target(image.word(va),va,0x94000000)==dest]
    require(calls==[0xfffffff0076e8840] and image.word(calls[0])==0x9401b96b,'Unique inittodr BL inside bsd_init')
    require(image.word(calls[0]-4)==0xd2800000,'Original inittodr argument x0=0')
    require(image.body(dest,12)==struct.pack('<3I',0xd10083ff,0xa9017bfd,0x910043fd),'inittodr aligned 32-byte LR/SP frame')
    require([va for va in range(dest,address(callee['end']),4) if image.word(va)==0xd65f03c0]==[0xfffffff007756e48],
            'Unique inittodr normal RET')
    require(image.body(0xfffffff007756e40,12)==struct.pack('<3I',0xa9417bfd,0x910083ff,0xd65f03c0),
            'inittodr restores original LR/SP')
    for va,name in ((0xfffffff007756e08,'_clock_get_calendar_microtime'),(0xfffffff007756e30,'_clock_set_calendar_microtime')):
        require(target(image.word(va),va,0x94000000)==image.names[name],'Actual calendar call changed')
    require(image.word(0xfffffff007756e10)==0xb7f80068 and image.word(0xfffffff007756e18)==0x36f80148,
            'Calendar correction must remain conditional on a negative time component')
    group_names=['_dtrace_postinit','_loopattach','_ether_family_init','_cfil_init','_pkt_mnglr_init',
                 '_utun_register_control','_ipsec_init','_netsrc_init','_nstat_init','_mptcp_control_register',
                 '_if_redirect_init','_kctl_test_init','_net_init_run']
    grouped=[]
    for index,name in enumerate(group_names):
        va=0xfffffff0076e8808+4*index
        require(target(image.word(va),va,0x94000000)==image.names[name],'Grouped pre-root call changed: '+name)
        grouped.append({'va':hex(va),'target_va':hex(image.names[name]),'symbol':name,'word':hex(image.word(va))})
    later={}
    for va,name in ((0xfffffff0076e8898,'_IOFindBSDRoot'),(0xfffffff0076e88dc,'_vfs_mountroot')):
        require(target(image.word(va),va,0x94000000)==image.names[name],'Later root boundary changed')
        later[name]={'va':hex(va),'target_va':hex(image.names[name]),'word':hex(image.word(va))}
    require(not any(image.word(va)&0xfc000000==0x94000000 for va in range(calls[0]+4,0xfffffff0076e8898,4)),
            'Selected call is not the final BL before root discovery')
    old_calls=copy.deepcopy(old['old_calls'])
    old_calls['IOKitBSDInit']={'va':old['call_site'],'word':old['original_call_word'],'target_va':old['callee']['start']}
    layout={key:copy.deepcopy(old[key]) for key in ('entry','first_cpu','mmu','post_mmu','caves','console','mapping')}
    layout.update({'caller':caller,'callee':callee,'call_site':hex(calls[0]),'original_call_word':'0x9401b96b',
                   'old_calls':old_calls,'argument_semantics':'void inittodr(time_t base); original x0=0 and original SP preserved; base is unused by actual callee',
                   'normal_return':{'function':CALLEE_NAME,'va':'0xfffffff007756e48','word':'0xd65f03c0',
                                    'restore_va':'0xfffffff007756e40','restore_words':['0xa9417bfd','0x910083ff'],'return_value':'void'},
                   'bsd_boundary':{'grouped_calls':grouped,'later_calls':later,
                                   'last_BL_before_IOFindBSDRoot':True,'stops_before_root_discovery_and_mount':True,
                                   'net_init_run_invokes_registered_callbacks':True,'live_callback_list_proved':False,
                                   'calendar_correction_only_if_negative':True,'calendar_branch_observed':False,
                                   'required_boot_args':'-v serial=3 j716f-earlylog=1',
                                   'prior_initialization_can_block':True,'prior_initialization_can_start_workers':True,
                                   'matching_completion_proved':False,'network_hardware_ready_proved':False,
                                   'zero_side_effects_proved':False,'timer_irq_delivery_proved':False,
                                   'RTC_persistence_proved':False,'all_drivers_ready_proved':False,
                                   'root_access_exercised':False,'runtime_return_observed':False}})
    return image,layout


def original_layout(original,expectation):
    validate_expectation(expectation)
    image,layout=_discover(original,expectation['baseline_contract'])
    require(layout==expectation['layout'],'Frozen r30 layout differs from actual original')
    return image,layout


def expected_regions(layout):
    return base.expected_regions(layout)


def derive_expected(original,expectation):
    image,layout=original_layout(original,expectation)
    result,changed=base._derive_unreviewed(original,image,layout)
    require(sha(result)==expectation['expected']['sha256'] and changed==expectation['expected']['changed_bytes'],
            'Independent bytes differ from frozen r30 digest/count')
    return result


def probe_readback(original,patched,expectation):
    image,layout=original_layout(original,expectation)
    require(len(patched)==len(original),'Diagnostic kernel length changed')
    regions=[]
    for va,code in expected_regions(layout):
        offset=image.offset(va,len(code))
        require(patched[offset:offset+len(code)]==code,'Wrong exact hook/cave bytes at '+hex(va))
        regions.append((offset,offset+len(code),va))
    cursor,changed,counts=0,0,[]
    for low,high,va in sorted(regions):
        require(low>=cursor and patched[cursor:low]==original[cursor:low],'Mutation outside six r30 intervals')
        count=sum(a!=b for a,b in zip(patched[low:high],original[low:high]))
        changed+=count
        counts.append({'va':hex(va),'file_start':hex(low),'file_end':hex(high),'changed_bytes':count})
        cursor=high
    require(patched[cursor:]==original[cursor:],'Mutation outside r30 intervals in tail')
    require(changed==expectation['expected']['changed_bytes'] and sha(patched)==expectation['expected']['sha256'],
            'Candidate differs from frozen r30 digest/count')
    candidate=MachO(patched)
    for name,item in layout['old_calls'].items():
        require(candidate.word(address(item['va']))==int(item['word'],16),'Old hook remains patched: '+name)
    require(candidate.body(address(layout['caves']['3']),CAVE_BYTES)==bytes(CAVE_BYTES),'Unexpected stage3 code')
    return {'status':'passed-host-byte-verification','sha256':sha(patched),'bytes':len(patched),
            'original_sha256':expectation['original']['sha256'],'changed_bytes':changed,
            'caller':CALLER_NAME,'callee':CALLEE_NAME,'call_site':layout['call_site'],
            'caller_range':[layout['caller']['start'],layout['caller']['end']],
            'callee_range':[layout['callee']['start'],layout['callee']['end']],
            'occurrence':1,'reset_before':False,'reset_after_callee_returns':True,
            'reset_delay_seconds':6,'timebase_hz':19200000,'delay_ticks':DELAY_TICKS,
            'counter':'CNTPCT_EL0','reset_function':'0x84000009','conduit':'SMC #0',
            'stage3_disabled':True,'stage1_stage2_exact_words_verified':True,
            'all_other_bytes_identical':True,'header_and_symbols_unchanged':True,
            'old_init_call_restored':True,'old_configure_call_restored':True,
            'old_machine_lockdown_call_restored':True,'old_PE_lockdown_iokit_call_restored':True,
            'old_kperf_init_early_call_restored':True,'old_sysctl_mib_init_call_restored':True,'old_IOKitBSDInit_call_restored':True,'original_sp_preserved_before_call':True,
            'argument_semantics':layout['argument_semantics'],'normal_return':layout['normal_return'],
            'mapping':layout['mapping'],'bsd_boundary':layout['bsd_boundary'],'hardware_tested':False,
            'source_crypto_seal_changed_by_probe':False,'permitted_regions':counts,
            'scope':'Host bytes only. Grouped DTrace/network/calendar initialization through inittodr return. Stops before '
                    'root discovery and mounting. Earlier initialization may allocate, start workers or block. '
                    'Six-second reset starts only after normal return; no IRQ, driver-ready, zero-side-effect or userland proof.'}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('original',type=Path)
    p.add_argument('candidate',type=Path)
    p.add_argument('--expectation',type=Path,required=True)
    p.add_argument('--report',type=Path)
    a=p.parse_args()
    result=probe_readback(a.original.read_bytes(),a.candidate.read_bytes(),load_expectation(a.expectation))
    if a.report:
        with a.report.open('x',encoding='utf-8',newline='\n') as f:
            json.dump(result,f,indent=2)
            f.write('\n')
    print(json.dumps(result,indent=2))
