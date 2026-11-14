#!/usr/bin/env python3
"""Focused r17 first-thread/return/old-checkpoint rejection tests."""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode=True
import verify_xnu_bootstrap_thread_kernel as v

def run_tests(original,good,r16):
    image=v.original_layout(original)
    initial=v.probe_readback(original,good)
    tests=[]
    def reject(name,candidate,base=original):
        try:v.probe_readback(base,candidate)
        except ValueError as exc:tests.append({'name':name,'rejected':True,'reason':str(exc)})
        else:raise AssertionError('Accepted invalid candidate: '+name)
    def mutate_words(name,changes):
        candidate=bytearray(good)
        for va,word in changes:struct.pack_into('<I',candidate,image.offset(va,4),word)
        reject(name,bytes(candidate))
    def mutate(name,va,word):mutate_words(name,[(va,word)])
    reject('unpatched-original',original)
    v.require(v.sha(r16)=='ae0b78204335ec3a2851aab31e4503587fe1aa6ba9f51ff85fa3d28cee26aca0','Wrong retained r16')
    reject('old-r16-clock-stop',r16)
    reject('preinstrumented-baseline-input',good,r16)
    mutate('wrong-callee-idle-create',v.CAVES[4],v.branch(v.CAVES[4],image.names['_idle_thread_create'],True))
    mutate('before-call-immediate-reset',v.CAVES[4],0x52800120)
    mutate('hook-is-bl-not-b',v.CALL_SITE,v.branch(v.CALL_SITE,v.CAVES[4],True))
    mutate('changed-delay-literal',v.CAVES[4]+64,115200001)
    mutate('timer-bypassed',v.CAVES[4]+4,v.branch(v.CAVES[4]+4,v.CAVES[4]+32))
    mutate('old-r16-hook-left-active',v.OLD_R16_SITE,v.branch(v.OLD_R16_SITE,v.CAVES[4]))
    mutate('stage3-stop-left-active',v.POST_MMU,v.branch(v.POST_MMU,v.CAVES[3]))
    mutate_words('creation-return-instead-of-thread-execution',[(v.CALL_SITE,v.ORIGINAL_CALL_WORD),
                 (v.CREATE_SITE,v.branch(v.CREATE_SITE,v.CAVES[4]))])
    mutate('bootstrap-load-context-skipped',v.LOAD_SITE,0xd503201f)
    mutate('sched-ret-altered',v.CALLEE_RET,0xd503201f)
    mutate('scheduler-service-failure-check-skipped',v.START_SITES[0]+4,0xd503201f)
    v.require(v.probe_readback(original,good)==initial,'Good fixture changed during tests')
    return {'status':'passed','positive_checks':2,'negative_checks':len(tests),'tests':tests,
            'verified_candidate_sha256':v.sha(good),'hardware_tested':False}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--r16',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    result=run_tests(args.original.read_bytes(),args.candidate.read_bytes(),args.r16.read_bytes())
    result['helper_sha256']=v.sha(Path(v.__file__).read_bytes())
    with args.report.open('x',encoding='utf-8',newline='\n') as f:
        json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps({'status':result['status'],'positive_checks':2,'negative_checks':result['negative_checks']},indent=2))
