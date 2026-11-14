#!/usr/bin/env python3
"""Focused r29 inline autoconf, publication and callback boundary tests."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode=True
import verify_xnu_bsd_autoconf_kernel as v


def run_tests(original,good,expectation):
    image,layout=v.original_layout(original,expectation)
    actual=v.probe_readback(original,good,expectation)
    v.require(v.derive_expected(original,expectation)==good,'Candidate differs from independent r29 expectation')
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
    mutate('new-IOKitBSDInit-hook-missing',call,image.word(call))
    mutate('wrapper-calls-old-sysctl',cave,v.branch(cave,int(layout['old_calls']['sysctl_mib_init']['target_va'],16),True))
    mutate('wrapper-skips-IOKitBSDInit',cave,0xd503201f)
    mutate('wrapper-tail-B-loses-reset-return',cave,v.branch(cave,int(layout['callee']['start'],16)))
    mutate('six-second-ticks-changed',cave+64,v.DELAY_TICKS+1)
    mutate('unrequested-stage3-code',int(layout['caves']['3'],16),0xd503201f)
    mutate('IOKitBSDInit-normal-return-changed',int(layout['normal_return']['va'],16),0xd503201f)
    mutate('IOKitBSDInit-stack-restore-changed',int(layout['normal_return']['restore_va'],16),0xd503201f)
    mutate('inline-pseudo-loop-skipped',0xfffffff0076e87e0,v.branch(0xfffffff0076e87e0,call))
    mutate('pseudo-count-argument-changed',0xfffffff007a18360,15)
    mutate('pseudo-dtrace-callback-changed',0xfffffff007a183c8,image.names['_mdevinit']&0xffffffff)
    mutate('IOBSD-publication-skipped',0xfffffff00797e7dc,0xd503201f)
    mutate('IOBSD-symbol-failure-gate-bypassed',0xfffffff0078fb824,0xd503201f)
    mutate('corefile-callback-allocation-replaced-with-enqueue',0xfffffff00797e7f4,
           v.branch(0xfffffff00797e7f4,image.names['_thread_call_enter'],True))
    mutate('corefile-global-pointer-store-skipped',0xfffffff00797e7fc,0xd503201f)
    mutate('CPU-publication-wait-bypassed',0xfffffff007458fd0,0xd503201f)
    mutate('root-discovery-call-changed',0xfffffff0076e8898,0xd503201f)
    reject('uninstrumented-original',lambda:v.probe_readback(original,original,expectation))
    wrong=copy.deepcopy(expectation)
    wrong['reset_settings']['entry_probe_reset_after']='_bsd_init'
    reject('contract-after-entire-bsd-init',lambda:v.probe_readback(original,good,wrong))
    wrong=copy.deepcopy(expectation)
    wrong['reset_settings']['entry_probe_reset_before']=True
    reject('contract-before-IOKitBSDInit',lambda:v.probe_readback(original,good,wrong))
    for field in ('IOBSD_publication_success_proved','matching_completion_proved',
                  'polled_corefile_callback_scheduled_here','root_access_exercised','runtime_return_observed'):
        wrong=copy.deepcopy(expectation)
        wrong['layout']['bsd_boundary'][field]=True
        reject('contract-invented-'+field,lambda:v.probe_readback(original,good,wrong))
    stale=copy.deepcopy(expectation)
    stale['baseline_contract']['original']['sha256']='1'+stale['baseline_contract']['original']['sha256'][1:]
    reject('contract-changed-source-baseline',lambda:v.original_layout(original,stale))
    pending=copy.deepcopy(expectation)
    pending['status']='unreviewed-discovery'
    reject('unreviewed-contract',lambda:v.derive_expected(original,pending))
    return {'status':'passed-focused-r29-kernel-tests','positive_cases':1,'rejected_cases':len(cases),'cases':cases,
            'candidate_sha256':actual['sha256'],'hardware_tested':False,
            'scope':'New IOKitBSDInit call/ABI, six old hooks, original pseudo loop/table, publication/allocation gates, '
                    'exact timer/six regions, and bounded publication/root/runtime claims.'}


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
