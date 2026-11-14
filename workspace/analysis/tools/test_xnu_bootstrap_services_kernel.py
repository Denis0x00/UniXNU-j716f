#!/usr/bin/env python3
"""Focused r18 services/phase/occurrence/return rejection tests."""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode=True
import verify_xnu_bootstrap_services_kernel as v

def run_tests(original,good,r17):
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
    v.require(v.sha(r17)=='73ebad1c451b54e6438fa4c0006aef67d4f38404acb47dc64e95a5799011d143','Wrong retained r17')
    reject('old-r17-first-thread-stop',r17)
    reject('preinstrumented-baseline-input',good,r17)
    mutate('wrong-callee-iokit',v.CAVES[4],v.branch(v.CAVES[4],image.names['_PE_init_iokit'],True))
    mutate('wrong-phase14-argument',v.ARGUMENT_VA,v.ARGUMENT_WORDS[0])
    mutate_words('wrong-first-occurrence',[(v.CALL_SITE,v.ORIGINAL_CALL_WORD),
                 (v.CALL_SITES[0],v.branch(v.CALL_SITES[0],v.CAVES[4]))])
    mutate('before-call-immediate-reset',v.CAVES[4],0x52800120)
    mutate('hook-is-bl-not-b',v.CALL_SITE,v.branch(v.CALL_SITE,v.CAVES[4],True))
    mutate('changed-delay-literal',v.CAVES[4]+64,115200001)
    mutate('timer-bypassed',v.CAVES[4]+4,v.branch(v.CAVES[4]+4,v.CAVES[4]+32))
    mutate('old-r17-hook-left-active',v.OLD_R17_SITE,v.branch(v.OLD_R17_SITE,v.CAVES[4]))
    mutate('stage3-stop-left-active',v.POST_MMU,v.branch(v.POST_MMU,v.CAVES[3]))
    mutate('phase-publication-altered',v.CALLEE_RET-28,0xd503201f)
    mutate('startup-ret-altered',v.CALLEE_RET,0xd503201f)
    v.require(v.probe_readback(original,good)==initial,'Good fixture changed during tests')
    return {'status':'passed','positive_checks':2,'negative_checks':len(tests),'tests':tests,
            'verified_candidate_sha256':v.sha(good),'hardware_tested':False}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--r17',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    result=run_tests(args.original.read_bytes(),args.candidate.read_bytes(),args.r17.read_bytes())
    result['helper_sha256']=v.sha(Path(v.__file__).read_bytes())
    with args.report.open('x',encoding='utf-8',newline='\n') as f:
        json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps({'status':result['status'],'positive_checks':2,'negative_checks':result['negative_checks']},indent=2))
