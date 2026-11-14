#!/usr/bin/env python3
"""Focused r27 call transition/CPU gate contracts; no old audit re-execution."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
import verify_xnu_cpu_wait_kernel as v


def run_tests(original, good, expectation):
    image,layout = v.original_layout(original,expectation)
    actual = v.probe_readback(original,good,expectation)
    v.require(v.derive_expected(original,expectation) == good,'Candidate differs from independent r27 expectation')
    cases = []

    def reject(name,action):
        try:
            action()
        except ValueError as error:
            cases.append({'name':name,'rejected':True,'reason':str(error)})
        else:
            raise AssertionError('Accepted invalid fixture: '+name)

    def mutate(name,va,word):
        raw = bytearray(good)
        struct.pack_into('<I',raw,image.offset(va,4),word)
        reject(name,lambda:v.probe_readback(original,bytes(raw),expectation))

    cave,call = int(layout['caves']['4'],16),int(layout['call_site'],16)
    for name,item in layout['old_calls'].items():
        old = int(item['va'],16)
        mutate('old-'+name+'-hook-still-patched',old,v.branch(old,cave))
    mutate('new-kperf-hook-missing',call,image.word(call))
    mutate('wrapper-calls-old-PE-lockdown',cave,v.branch(cave,int(layout['old_calls']['PE_lockdown_iokit']['target_va'],16),True))
    mutate('wrapper-skips-actual-kperf-call',cave,0xd503201f)
    mutate('wrapper-tail-B-loses-timer-return',cave,v.branch(cave,int(layout['callee']['start'],16)))
    mutate('six-second-ticks-changed',cave+64,v.DELAY_TICKS+1)
    mutate('unrequested-stage3-code',int(layout['caves']['3'],16),0xd503201f)
    mutate('kperf-normal-return-modified',int(layout['normal_return']['va'],16),0xd503201f)
    mutate('max-cpus-gate-bypassed',int(layout['cpu_gate']['wait_callsite'],16),0xd503201f)
    mutate('kperf-stack-frame-modified',int(layout['callee']['start'],16),0xd503201f)
    mutate('CPU-wait-sleep-bypassed',0xfffffff00741b828,0xd503201f)
    reject('uninstrumented-original',lambda:v.probe_readback(original,original,expectation))
    wrong = copy.deepcopy(expectation)
    wrong['reset_settings']['entry_probe_reset_after'] = '_PE_lockdown_iokit'
    reject('contract-old-PE-selection',lambda:v.probe_readback(original,good,wrong))
    wrong_layout = copy.deepcopy(expectation)
    wrong_layout['layout']['cpu_gate']['runtime_return_observed'] = True
    reject('contract-fabricated-runtime-gate-success',lambda:v.probe_readback(original,good,wrong_layout))
    false_irq = copy.deepcopy(expectation)
    false_irq['layout']['cpu_gate']['return_proves_timer_irq'] = True
    reject('contract-fabricated-timer-IRQ-success',lambda:v.probe_readback(original,good,false_irq))
    stale = copy.deepcopy(expectation)
    stale['baseline_contract']['original']['sha256'] = '1'+stale['baseline_contract']['original']['sha256'][1:]
    reject('contract-changed-frozen-baseline',lambda:v.original_layout(original,stale))
    pending = copy.deepcopy(expectation)
    pending['status'] = 'unreviewed-discovery'
    reject('unreviewed-contract',lambda:v.derive_expected(original,pending))
    return {'status':'passed-focused-r27-kernel-tests','positive_cases':1,'rejected_cases':len(cases),'cases':cases,
            'candidate_sha256':actual['sha256'],'hardware_tested':False,
            'scope':'Unique new call/RET, four old hooks restored, conditional gate preserved, exact timer and six mutation intervals.'}


def run(root,artifact):
    from verify_xnu_entry import read_fat32
    artifact = Path(artifact)
    files,_ = read_fat32((artifact/'payload.img').read_bytes())
    return run_tests((artifact/'original-kernel.macho').read_bytes(),files['efi/unixnu/payloads/kernel.macho'],
                     v.load_expectation(artifact/'kernel-expectation.json'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact',type=Path)
    parser.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
    parser.add_argument('--report',type=Path)
    args = parser.parse_args()
    result = run(args.root,args.artifact)
    if args.report:
        with args.report.open('x',encoding='utf-8',newline='\n') as stream:
            json.dump(result,stream,indent=2)
            stream.write('\n')
    print(json.dumps(result,indent=2))
