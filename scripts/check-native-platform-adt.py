#!/usr/bin/env python3
"""Check actual ADT bytes against reviewed ArmCpu/ArmDTPlatform input ABI.

No device access. Passing describes resource encoding only, not MMIO ownership,
driver matching, GIC system-register access, timer delivery or hardware validity.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct


def decode(data):
    offset=0
    result={}
    def node(parent=None):
        nonlocal offset
        props,children=struct.unpack_from('<II',data,offset);offset+=8
        if props>10000 or children>10000:raise ValueError('Unreasonable ADT node')
        values={}
        for _ in range(props):
            name,size=struct.unpack_from('<32sI',data,offset);offset+=36
            name=name.split(b'\0',1)[0].decode('ascii')
            if name in values or offset+size>len(data):raise ValueError('Invalid ADT property')
            values[name]=data[offset:offset+size];offset+=(size+3)&~3
        name=values['name'].rstrip(b'\0').decode('ascii')
        path='/' if parent is None else parent.rstrip('/')+'/'+name
        if path in result:raise ValueError('Duplicate ADT path')
        result[path]=values
        for _ in range(children):node(path)
    node()
    if offset!=len(data):raise ValueError('Trailing or truncated ADT bytes')
    return result


def check(data,evidence):
    tree=decode(data)
    for path in ['/','/chosen','/cpus','/arm-io','/arm-io/intc','/defaults','/options','/psci']:
        if path not in tree:raise ValueError('Missing required platform node: '+path)
    if tree['/']['name']!=b'device-tree\0':raise ValueError('Platform expert root mismatch')
    chosen=tree['/chosen']
    if len(chosen['nvram-bank-size'])!=4 or len(chosen['nvram-proxy-data'])!=8192:
        raise ValueError('Missing CHRP NVRAM proxy input')
    if chosen['nvram-proxy-data'][:1]!=b'\x70':raise ValueError('Unexpected CHRP signature')
    cpus=[v for p,v in tree.items() if p.startswith('/cpus/')]
    if len(cpus)!=1:raise ValueError('Single boot CPU candidate only')
    cpu=cpus[0]
    if cpu['device_type']!=b'cpu\0' or cpu['reg']!=struct.pack('<I',evidence['cpu0']['mpidr_affinity']):
        raise ValueError('CPU0 nub does not meet ArmCpu probe input ABI')
    if cpu['timebase-frequency']!=struct.pack('<I',evidence['timer_hz']):
        raise ValueError('Timebase differs from board evidence')
    if tree['/psci']['method']!=evidence['psci_method'].encode()+b'\0':
        raise ValueError('PSCI conduit differs from board evidence')
    bus=tree['/arm-io']['ranges'];reg=tree['/arm-io/intc']['reg']
    if len(bus)!=24 or len(reg)!=32 or tree['/arm-io/intc']['compatible']!=b'arm,gic-v3\0':
        raise ValueError('GIC ABI needs three u64 ranges and four u64 register cells')
    child,parent,length=struct.unpack('<3Q',bus);regions=struct.unpack('<4Q',reg)
    g=evidence['gic']
    expected=[(g['distributor_base'],g['distributor_bytes']),
              (g['redistributor_base'],g['redistributor_bytes'])]
    for index,(address,size) in enumerate(expected):
        offset,count=regions[index*2:index*2+2]
        if offset<child or offset-child+count>length or parent+offset-child!=address or count!=size:
            raise ValueError('Resolved GIC resource differs from independent board evidence')
    if tree['/arm-io/intc']['redistributor-stride']!=struct.pack('<Q',g['redistributor_stride']):
        raise ValueError('Redistributor stride mismatch')
    return {'passed':True,'single_cpu':True,'timer_hz':evidence['timer_hz'],
            'gic_resources_from_stock_and_recovery':True,'psci_method':evidence['psci_method'],
            'adt_sha256':hashlib.sha256(data).hexdigest(),'hardware_validated':False,
            'scope':'ADT input ABI/resource encoding only; no driver matching, MMIO, timer IRQ or prelink validation'}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adt',type=Path,required=True)
    parser.add_argument('--hardware-evidence',type=Path,default=Path(__file__).with_name('j716f-hardware-evidence.json'))
    args=parser.parse_args()
    print(json.dumps(check(args.adt.read_bytes(),json.loads(args.hardware_evidence.read_text(encoding='utf-8'))),indent=2))
