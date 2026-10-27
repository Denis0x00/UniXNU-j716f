#!/usr/bin/env python3
"""Decode a frozen console snapshot; never read a live device or infer a boot."""
import argparse
import json
from pathlib import Path
import re
import struct
import zlib

EVENTS={0:'entry-mmu-off',1:'bootstrap-mapping-ready',2:'arm-init-mmu-on',3:'vm-enter',
 4:'permanent-map-before-switch',5:'vm-after-ttbr-switch',6:'pmap-bootstrap-return',7:'vm-final',
 8:'platform-configure-enter',9:'inherited-platform-configure-return',10:'platform-service-published',
 11:'cpu-start',12:'cpu-prepare-return',13:'cpu-registered',14:'controller-registered',15:'gic-ready',
 16:'timebase-enabled',17:'cpu-start-return',18:'cpu-quiesce',19:'cpu-halt',
 40:'early-exception',41:'kernel-synchronous-exception',42:'sp1-exception',43:'bootstrap-map-failed',
 44:'permanent-map-failed',50:'switch-context-before',51:'switch-context-return',
 52:'switch-context-before-after-irq8',53:'switch-context-return-after-irq8'}
for sample,n in enumerate((1,2,8)):
    for phase,name in enumerate(('enter','rtclock-return','eoi')):EVENTS[24+sample*3+phase]=f'irq{n}-{name}'

def decode(data,candidate,expected_session=None):
    if len(data)!=0x40000:raise ValueError('Expected exact frozen console size')
    sig,end,size=struct.unpack_from('<3I',data)
    if sig!=0x43474244 or end!=size or not 8192<=end<=0x3fff4:
        raise ValueError('Invalid/nonlinear/short DBGC console')
    def header_at(offset):
        if offset<16 or offset%8 or offset+8192>12+end:return None
        h=data[offset:offset+128]
        if (struct.unpack_from('<QII',h)!=(0x31474f4c4e36314a,1,8192) or
            struct.unpack_from('<I',h,56)[0]!=offset or not struct.unpack_from('<Q',h,48)[0] or
            struct.unpack_from('<I',h,60)[0]!=zlib.crc32(h[:60]) or any(h[68:]) or
            h[16:48].hex()!=candidate or struct.unpack_from('<I',h,64)[0] not in (0x414c4e4a,0x554c4e4a)):
            return None
        return h
    offset=12+end-8192
    if expected_session is not None:
        if not re.fullmatch('[0-9a-fA-F]{16}',expected_session):
            raise ValueError('Expected full session ID from independent ARM photo')
        matches=[]
        for at in range(16,12+end-8192+1,8):
            h=header_at(at)
            if h and struct.unpack_from('<Q',h,48)[0]==int(expected_session,16):matches.append(at)
        if len(matches)!=1:raise ValueError('Expected exactly one matching retained session')
        offset=matches[0]
    h=header_at(offset)
    if h is None:raise ValueError('Wrong candidate/header/session CRC/state')
    magic,version,length=struct.unpack_from('<QII',h)
    session,recorded_offset,crc,state=struct.unpack_from('<QIII',h,48)
    if (magic!=0x31474f4c4e36314a or version!=1 or length!=8192 or not session or
        recorded_offset!=offset or crc!=zlib.crc32(h[:60]) or any(h[68:]) or
        h[16:48].hex()!=candidate or state not in (0x414c4e4a,0x554c4e4a)):
        raise ValueError('Wrong candidate/header/session CRC/state')
    records=[];rejected=[]
    for slot in range(63):
        r=data[offset+128+slot*128:offset+256+slot*128]
        if not any(r):continue
        magic,event,sid,ordinal,counter=struct.unpack_from('<II3Q',r)
        fields=list(struct.unpack_from('<10Q',r,32))
        crc,commit,reserved=struct.unpack_from('<IIQ',r,112)
        if (state!=0x554c4e4a or magic!=0x31524c4a or event!=slot or sid!=session or
            ordinal!=slot+1 or reserved or commit!=0xc04d17ed or crc!=zlib.crc32(r[:112])):
            rejected.append(slot);continue
        records.append({'slot':slot,'name':EVENTS.get(slot,'reserved'),
                        'counter':counter,'crc32':f'{crc:08x}','data':fields})
    by={r['slot']:r for r in records}
    chain=all(i in by for i in (0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,*range(24,33)))
    chain=chain and not rejected and not any(i in by for i in (18,19,40,41,42,43,44))
    for sample,n in enumerate((1,2,8)):
        for phase in range(3):
            r=by.get(24+sample*3+phase)
            if not r:continue
            f=r['data']
            if (f[0]&0xffffff)!=27 or f[1:4]!=[n,n if phase>=1 else n-1,n if phase==2 else n-1] or f[4]!=19200000 or f[8]!=0x100000001 or (f[9]&0xff00ffffff):chain=False
    if all(i in by for i in range(24,33)):
        counters=[by[i]['counter'] for i in range(24,33)]
        if any(a>b for a,b in zip(counters,counters[1:])) or counters[-1]<=counters[0]:chain=False
    scheduling=bool(chain and 52 in by and 53 in by and
        by[52]['data'][2]>=8 and by[53]['data'][2]>=8 and
        by[52]['counter']>=by[32]['counter'] and by[53]['counter']>by[52]['counter'])
    return {'schema':1,'candidate':candidate,'session':f'{session:016x}','offset':offset,
            'recovered_before_console_tail':offset+8192!=12+end,
            'state':'armed' if state==0x414c4e4a else 'consumed','records':records,
            'rejected_slots':rejected,'native_timer_chain_consistent':bool(chain),
            'post_irq_context_switch_observed':scheduling,'hardware_validated':False,
            'qualification_note':'Offline consistency only; bind to frozen reader acquisition and final image identity.'}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('snapshot',type=Path)
    p.add_argument('--candidate',required=True);p.add_argument('--session');a=p.parse_args()
    print(json.dumps(decode(a.snapshot.read_bytes(),a.candidate,a.session),indent=2))
