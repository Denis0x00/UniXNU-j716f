#!/usr/bin/env python3
"""Focused r30 grouped pre-root boundary rejection tests."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode=True
import verify_xnu_bsd_pre_root_kernel as v


def run_tests(original,good,expectation):
    image,layout=v.original_layout(original,expectation)
    actual=v.probe_readback(original,good,expectation)
    v.require(v.derive_expected(original,expectation)==good,'Candidate differs from independent r30 expectation')
    cases=[]

    def reject(name,action):
        try:
            action()
        except ValueError as error:
            cases.append({'name':name,'rejected':True,'reason':str(error)})
        else:
            raise AssertionError('Accepted invalid fixture: '+name)

    def mutate(name,va,word):
        raw=bytearray(good)
        struct.pack_into('<I',raw,image.offset(va,4),word)
        reject(name,lambda:v.probe_readback(original,bytes(raw),expectation))

    cave,call=int(layout['caves']['4'],16),int(layout['call_site'],16)
    for name,item in layout['old_calls'].items():
        old=int(item['va'],16)
        mutate('old-'+name+'-hook-still-patched',old,v.branch(old,cave))
    mutate('new-inittodr-hook-missing',call,image.word(call))
    mutate('wrapper-calls-old-IOKitBSDInit',cave,v.branch(cave,int(layout['old_calls']['IOKitBSDInit']['target_va'],16),True))
    mutate('wrapper-skips-inittodr',cave,0xd503201f)
    mutate('wrapper-tail-B-loses-reset-return',cave,v.branch(cave,int(layout['callee']['start'],16)))
    mutate('inittodr-original-zero-argument-changed',call-4,0xd2800020)
    mutate('six-second-ticks-changed',cave+64,v.DELAY_TICKS+1)
    mutate('unrequested-stage3-code',int(layout['caves']['3'],16),0xd503201f)
    mutate('inittodr-normal-return-changed',int(layout['normal_return']['va'],16),0xd503201f)
    mutate('inittodr-LR-restore-changed',int(layout['normal_return']['restore_va'],16),0xd503201f)
    mutate('inittodr-SP-restore-changed',int(layout['normal_return']['restore_va'],16)+4,0xd503201f)
    mutate('negative-seconds-gate-bypassed',0xfffffff007756e10,0xd503201f)
    mutate('negative-microseconds-gate-bypassed',0xfffffff007756e18,0xd503201f)
    mutate('calendar-correction-changed-to-positive-epoch',0xfffffff007756e28,0xd2800020)
    mutate('DTrace-postinit-skipped',0xfffffff0076e8808,0xd503201f)
    mutate('network-callback-execution-skipped',0xfffffff0074fb7b8,0xd503201f)
    mutate('network-entry-free-skipped',0xfffffff0074fb7c4,0xd503201f)
    mutate('root-discovery-call-changed',0xfffffff0076e8898,0xd503201f)
    mutate('root-mount-call-changed',0xfffffff0076e88dc,0xd503201f)
    reject('uninstrumented-original',lambda:v.probe_readback(original,original,expectation))
    wrong=copy.deepcopy(expectation)
    wrong['reset_settings']['entry_probe_reset_after']='_IOFindBSDRoot'
    reject('contract-crosses-root-discovery',lambda:v.probe_readback(original,good,wrong))
    wrong=copy.deepcopy(expectation)
    wrong['reset_settings']['entry_probe_reset_before']=True
    reject('contract-before-inittodr',lambda:v.probe_readback(original,good,wrong))
    for field in ('live_callback_list_proved','calendar_branch_observed','network_hardware_ready_proved',
                  'RTC_persistence_proved','root_access_exercised','runtime_return_observed'):
        wrong=copy.deepcopy(expectation)
        wrong['layout']['bsd_boundary'][field]=True
        reject('contract-invented-'+field,lambda:v.probe_readback(original,good,wrong))
    stale=copy.deepcopy(expectation)
    stale['baseline_contract']['original']['sha256']='1'+stale['baseline_contract']['original']['sha256'][1:]
    reject('contract-changed-source-baseline',lambda:v.original_layout(original,stale))
    pending=copy.deepcopy(expectation)
    pending['status']='unreviewed-discovery'
    reject('unreviewed-contract',lambda:v.derive_expected(original,pending))
    return {'status':'passed-focused-r30-kernel-tests','positive_cases':1,'rejected_cases':len(cases),'cases':cases,
            'candidate_sha256':actual['sha256'],'hardware_tested':False,
            'scope':'New inittodr call/ABI, seven old hooks, original argument/calendar conditions, grouped callback work, '
                    'exact timer/six regions, and bounded callback/RTC/network/root/runtime claims.'}


def run(root,artifact):
    from verify_xnu_entry import read_fat32
    artifact=Path(artifact)
    files,_=read_fat32((artifact/'payload.img').read_bytes())
    return run_tests((artifact/'original-kernel.macho').read_bytes(),files['efi/unixnu/payloads/kernel.macho'],
                     v.load_expectation(artifact/'kernel-expectation.json'))


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('artifact',type=Path)
    p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
    p.add_argument('--report',type=Path)
    a=p.parse_args()
    result=run(a.root,a.artifact)
    if a.report:
        with a.report.open('x',encoding='utf-8',newline='\n') as f:
            json.dump(result,f,indent=2)
            f.write('\n')
    print(json.dumps(result,indent=2))
