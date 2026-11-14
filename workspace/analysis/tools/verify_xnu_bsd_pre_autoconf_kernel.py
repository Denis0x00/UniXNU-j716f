#!/usr/bin/env python3
"""r28 grouped BSD setup checkpoint, after sysctl_mib_init and before autoconf.

Only the new call/return boundary is changed. Frozen r27 source/early-log/CPU0
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
BASE_MODULE_SHA256 = 'f29d9846d093044e95eec346bcfb3a97d2a3ad488103ac2496ca7c019e768051'
BASE_CONTRACT_SHA256 = '7ad304ec32d95b54687693a6d5e3193f4413f636cbac45ccaffb58e43aab2ccd'
BASE_PATH = Path(__file__).with_name('verify_xnu_cpu_wait_kernel.py').resolve()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


require(sha(BASE_PATH.read_bytes()) == BASE_MODULE_SHA256, 'Frozen r27 verifier changed')
import verify_xnu_cpu_wait_kernel as previous
require(Path(previous.__file__).resolve() == BASE_PATH, 'Wrong frozen r27 import')
base = previous.base
MachO, branch, target, address = previous.MachO, previous.branch, previous.target, previous.address
CALLER_NAME, CALLEE_NAME = '_bsd_init', '_sysctl_mib_init'
RESET_SETTINGS = dict(previous.RESET_SETTINGS, entry_probe_reset_after=CALLEE_NAME, entry_probe_reset_in=CALLER_NAME)
DELAY_TICKS, CAVE_BYTES = previous.DELAY_TICKS, previous.CAVE_BYTES
PARSER_SOURCE_SHA256 = previous.PARSER_SOURCE_SHA256
PROTOCOL_HEADER_SHA256 = previous.PROTOCOL_HEADER_SHA256
SOURCE_PATCH_SHA256 = previous.SOURCE_PATCH_SHA256
REVIEW_DIR = 'analysis/xnu-bsd-pre-autoconf-r28-20261114/'
REVIEW_FLAGS = {'unique_bsd_call_and_return_reviewed': True, 'previous_kperf_hook_restored': True,
                'pre_autoconf_root_boundary_reviewed': True, 'actual_native_probe_gates_reviewed': True,
                'independent_expected_bytes_reviewed': True}
FUNCTIONS = {
    'caller': (CALLER_NAME, '6c976d0b87055ce3de88987a40888e668dec077c9c7ce5a2de2062132966c65f'),
    'callee': (CALLEE_NAME, '2f45108ea830b13d0a32579d7bb0cf27bb5189d6df871084b2afa795624c3ce5'),
    'autoconf': ('_bsd_autoconf', '0dd0d5ac384472dde0d71959a4499c8b1338d98521bee4ee7c84ec648f47c3dc'),
}


def validate_expectation(value):
    require(isinstance(value,dict) and type(value.get('schema_version')) is int and value['schema_version'] == 1,
            'Unsupported r28 expectation')
    require(value.get('status') == 'independently-frozen', 'Unreviewed r28 contract')
    baseline = value.get('baseline_contract')
    require(sha(canonical(baseline)) == BASE_CONTRACT_SHA256, 'Frozen r27 contract changed')
    previous.validate_expectation(baseline)
    require(value.get('base_module_sha256') == BASE_MODULE_SHA256 and value.get('baseline_contract_sha256') == BASE_CONTRACT_SHA256,
            'Missing frozen r27 provenance')
    for key in ('original','parser_source_sha256','protocol_header_sha256','source_patch_sha256'):
        require(value.get(key) == baseline[key], 'Changed inherited field: '+key)
    require(value.get('reset_settings') == RESET_SETTINGS and
            all(type(value['reset_settings'][key]) is type(want) for key,want in RESET_SETTINGS.items()),
            'Expected unique bsd_init/sysctl return and six-second reset')
    require(value.get('review') == REVIEW_FLAGS, 'Missing independent r28 review')
    expected = value.get('expected')
    base._exact_keys(expected,('sha256','changed_bytes'),'expected')
    base._digest(expected['sha256'],'expected.sha256')
    require(type(expected['changed_bytes']) is int and 0 < expected['changed_bytes'] <= 300,'Bad changed byte count')
    evidence = value.get('review_evidence')
    require(isinstance(evidence,dict) and set(baseline['review_evidence']) <= set(evidence),'Missing inherited provenance')
    for path,spec in baseline['review_evidence'].items():
        require(evidence[path] == spec,'Changed inherited provenance: '+path)
    for name in ('machine-review.json','machine-code.json','review_machine.py'):
        require(REVIEW_DIR+name in evidence,'Missing r28 review: '+name)
        spec = evidence[REVIEW_DIR+name]
        base._exact_keys(spec,('sha256','bytes'),name)
        base._digest(spec['sha256'],name)
        require(type(spec['bytes']) is int and spec['bytes']>0,'Bad evidence size')
    require(isinstance(value.get('layout'),dict),'Missing r28 layout')
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
        require(functions[key]['sha256']==digest,'Reviewed BSD function changed: '+name)
    caller,callee = functions['caller'],functions['callee']
    dest=address(callee['start'])
    calls=[va for va in range(address(caller['start']),address(caller['end']),4)
           if image.word(va)&0xfc000000 == 0x94000000 and target(image.word(va),va,0x94000000)==dest]
    require(calls==[0xfffffff0076e87c0] and image.word(calls[0])==0x9401abfd,'Unique sysctl_mib_init BL inside bsd_init')
    require(image.body(dest,16)==struct.pack('<4I',0xa9bd57f6,0xa9014ff4,0xa9027bfd,0x910083fd),'Sysctl 48-byte aligned LR/SP frame')
    require([va for va in range(dest,address(callee['end']),4) if image.word(va)==0xd65f03c0]==[0xfffffff0077538c8],
            'Unique sysctl normal RET')
    require(image.body(0xfffffff0077538bc,16)==struct.pack('<4I',0xa9427bfd,0xa9414ff4,0xa8c357f6,0xd65f03c0),
            'Sysctl normal path must restore LR/SP and callee-saved registers')
    boundary_calls={}
    for site,name in ((0xfffffff0076e87cc,'_kprintf'),(0xfffffff0076e87d0,'_kminit'),
                      (0xfffffff0076e8804,'_IOKitBSDInit'),(0xfffffff0076e8898,'_IOFindBSDRoot'),
                      (0xfffffff0076e88dc,'_vfs_mountroot')):
        require(target(image.word(site),site,0x94000000)==image.names[name],'Later BSD boundary changed')
        require(site>calls[0],'Candidate reached later root/autoconf boundary')
        boundary_calls[name]={'va':hex(site),'target_va':hex(image.names[name]),'word':hex(image.word(site))}
    require(target(image.word(0xfffffff007753860),0xfffffff007753860,0x94000000)==image.names['_ml_wait_max_cpus'],
            'Sysctl CPU publication dependency must remain')
    old_calls=copy.deepcopy(old['old_calls'])
    old_calls['kperf_init_early']={'va':old['call_site'],'word':old['original_call_word'],'target_va':old['callee']['start']}
    layout={key:copy.deepcopy(old[key]) for key in ('entry','first_cpu','mmu','post_mmu','caves','console','mapping')}
    layout.update({'caller':caller,'callee':callee,'call_site':hex(calls[0]),'original_call_word':'0x9401abfd',
                   'old_calls':old_calls,'argument_semantics':'void(void); original SP and no argument registers altered before BL',
                   'normal_return':{'function':CALLEE_NAME,'va':'0xfffffff0077538c8','word':'0xd65f03c0',
                                    'restore_va':'0xfffffff0077538bc','restore_words':['0xa9427bfd','0xa9414ff4','0xa8c357f6']},
                   'bsd_boundary':{'autoconf_function':functions['autoconf'],'later_calls':boundary_calls,
                                   'stops_before_inlined_bsd_autoconf':True,'stops_before_IOBSD_publication':True,
                                   'stops_before_root_discovery_and_mount':True,'required_boot_args':'-v serial=3 j716f-earlylog=1',
                                   'native_probe_gates_present_but_not_requested':True,'sysctl_wait_call':'0xfffffff007753860',
                                   'prior_initialization_can_block':True,'prior_initialization_can_start_workers':True,
                                   'zero_side_effects_proved':False,'timer_irq_delivery_proved':False,
                                   'all_drivers_ready_proved':False,'root_access_exercised':False,'runtime_return_observed':False}})
    return image,layout


def original_layout(original,expectation):
    validate_expectation(expectation)
    image,layout=_discover(original,expectation['baseline_contract'])
    require(layout==expectation['layout'],'Frozen r28 layout differs from actual original')
    return image,layout


def expected_regions(layout):
    return base.expected_regions(layout)


def derive_expected(original,expectation):
    image,layout=original_layout(original,expectation)
    result,changed=base._derive_unreviewed(original,image,layout)
    require(sha(result)==expectation['expected']['sha256'] and changed==expectation['expected']['changed_bytes'],
            'Independent bytes differ from frozen r28 digest/count')
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
        require(low>=cursor and patched[cursor:low]==original[cursor:low],'Mutation outside six r28 intervals')
        count=sum(a!=b for a,b in zip(patched[low:high],original[low:high]))
        changed+=count
        counts.append({'va':hex(va),'file_start':hex(low),'file_end':hex(high),'changed_bytes':count})
        cursor=high
    require(patched[cursor:]==original[cursor:],'Mutation outside r28 intervals in tail')
    require(changed==expectation['expected']['changed_bytes'] and sha(patched)==expectation['expected']['sha256'],
            'Candidate differs from frozen r28 digest/count')
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
            'old_kperf_init_early_call_restored':True,'original_sp_preserved_before_call':True,
            'argument_semantics':layout['argument_semantics'],'normal_return':layout['normal_return'],
            'mapping':layout['mapping'],'bsd_boundary':layout['bsd_boundary'],'hardware_tested':False,
            'source_crypto_seal_changed_by_probe':False,'permitted_regions':counts,
            'scope':'Host bytes only. Grouped BSD setup through sysctl_mib_init; stops before inlined autoconf, IOBSD, '
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
