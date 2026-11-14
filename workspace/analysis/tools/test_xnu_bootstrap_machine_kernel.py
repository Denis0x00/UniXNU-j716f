#!/usr/bin/env python3
"""Focused r15 call/tail-return/old-hook rejection tests; no device access."""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode=True
import verify_xnu_bootstrap_machine_kernel as v

def run_tests(original,good,r14):
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
    v.require(v.sha(r14)=='6ec24a01d1323263361ee611b1123be3c47c6eaddeb25f1959df3385e6a7f728','Wrong retained r14')
    reject('old-r14-ipc-stop',r14)
    reject('preinstrumented-baseline-input',good,r14)
    mutate('wrong-callee-clock-init',v.CAVES[4],v.branch(v.CAVES[4],image.names['_clock_init'],True))
    mutate_words('wrong-earlier-mapping-stop',[(v.CALL_SITE,v.ORIGINAL_CALL_WORD),
                 (v.MAPPING_SITE,v.branch(v.MAPPING_SITE,v.CAVES[4]))])
    mutate('before-call-immediate-reset',v.CAVES[4],0x52800120)
    mutate('hook-is-bl-not-b',v.CALL_SITE,v.branch(v.CALL_SITE,v.CAVES[4],True))
    mutate('changed-delay-literal',v.CAVES[4]+64,115200001)
    mutate('timer-bypassed',v.CAVES[4]+4,v.branch(v.CAVES[4]+4,v.CAVES[4]+32))
    mutate('old-r14-hook-left-active',v.OLD_R14_SITE,v.branch(v.OLD_R14_SITE,v.CAVES[4]))
    mutate('stage3-stop-left-active',v.POST_MMU,v.branch(v.POST_MMU,v.CAVES[3]))
    mutate('machine-stack-restore-altered',v.TAIL_B-4,0xd503201f)
    mutate('machine-tail-b-changed-to-bl',v.TAIL_B,v.branch(v.TAIL_B,v.CALLEE_RET,True))
    mutate('lockdown-ret-altered',v.CALLEE_RET,0xd503201f)
    v.require(v.probe_readback(original,good)==initial,'Good fixture changed during tests')
    return {'status':'passed','positive_checks':2,'negative_checks':len(tests),'tests':tests,
            'verified_candidate_sha256':v.sha(good),'hardware_tested':False}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--r14',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    result=run_tests(args.original.read_bytes(),args.candidate.read_bytes(),args.r14.read_bytes())
    result['helper_sha256']=v.sha(Path(v.__file__).read_bytes())
    with args.report.open('x',encoding='utf-8',newline='\n') as f:
        json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps({'status':result['status'],'positive_checks':2,'negative_checks':result['negative_checks']},indent=2))
