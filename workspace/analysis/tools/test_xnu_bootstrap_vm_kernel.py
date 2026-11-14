#!/usr/bin/env python3
"""Focused r12 VM-return byte-contract tests; no device or disk mutations."""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode=True
import verify_xnu_bootstrap_vm_kernel as v

def run_tests(original,good,r11):
    image=v.original_layout(original)
    initial=v.probe_readback(original,good)
    tests=[]
    def reject(name,candidate,base=original):
        try:v.probe_readback(base,candidate)
        except ValueError as exc:tests.append({'name':name,'rejected':True,'reason':str(exc)})
        else:raise AssertionError('Accepted invalid candidate: '+name)
    def mutate_word(name,va,word):
        candidate=bytearray(good);struct.pack_into('<I',candidate,image.offset(va,4),word)
        reject(name,bytes(candidate))
    reject('unpatched-original',original)
    v.require(v.sha(r11)=='4fc2c247215fdb64b5aa2f07082e297300e12fe9c105200a9e16df12b9342af5','Wrong retained r11')
    reject('old-r11-scale-stop',r11)
    reject('preinstrumented-baseline-input',good,r11)
    mutate_word('wrong-callee-scale',v.CAVES[4],v.branch(v.CAVES[4],image.names['_scale_setup'],True))
    mutate_word('before-call-immediate-reset',v.CAVES[4],0x52800120)
    mutate_word('hook-is-bl-not-b',v.CALL_SITE,v.branch(v.CALL_SITE,v.CAVES[4],True))
    mutate_word('changed-delay-literal',v.CAVES[4]+64,115200001)
    mutate_word('timer-bypassed',v.CAVES[4]+4,v.branch(v.CAVES[4]+4,v.CAVES[4]+32))
    mutate_word('old-r10-hook-left-active',v.OLD_R10_SITE,v.branch(v.OLD_R10_SITE,v.CAVES[4]))
    mutate_word('old-r11-hook-left-active',v.OLD_R11_SITE,v.branch(v.OLD_R11_SITE,v.CAVES[4]))
    mutate_word('stage3-stop-left-active',v.POST_MMU,v.branch(v.POST_MMU,v.CAVES[3]))
    mutate_word('vm-page-bootstrap-call-altered',0xfffffff0079dcb20,0xd503201f)
    mutate_word('vm-mem-bootstrap-ret-altered',v.CALLEE_RET,0xd503201f)
    mutate_word('later-bootstrap-code-altered',v.CALL_SITE+4,0xd503201f)
    v.require(v.probe_readback(original,good)==initial,'Good fixture changed during tests')
    return {'status':'passed','positive_checks':2,'negative_checks':len(tests),'tests':tests,
            'verified_candidate_sha256':v.sha(good),'hardware_tested':False}

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original',type=Path,required=True)
    parser.add_argument('--candidate',type=Path,required=True)
    parser.add_argument('--r11',type=Path,required=True)
    parser.add_argument('--report',type=Path,required=True)
    args=parser.parse_args()
    result=run_tests(args.original.read_bytes(),args.candidate.read_bytes(),args.r11.read_bytes())
    result['helper_sha256']=v.sha(Path(v.__file__).read_bytes())
    with args.report.open('x',encoding='utf-8',newline='\n') as f:
        json.dump(result,f,indent=2);f.write('\n')
    print(json.dumps({'status':result['status'],'positive_checks':2,'negative_checks':result['negative_checks']},indent=2))
