#!/usr/bin/env python3
"""Single-CPU native platform ADT candidate using J716F stock/recovery evidence.

This does not replace the immutable ADT used by P13-P17 diagnostics. It adds the
explicit PSCI conduit required by the reviewed ArmCpu ABI; no SMP is selected.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import struct

HERE=Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('entry_adt',HERE/'make-j716f-entry-adt.py')
entry=importlib.util.module_from_spec(spec)
spec.loader.exec_module(entry)


def make(evidence):
    if evidence.get('psci_method') != 'smc':
        raise ValueError('Candidate requires the stock/recovery-qualified SMC conduit')
    data=bytearray(entry.make(evidence))
    count=struct.unpack_from('<I',data,4)[0]
    struct.pack_into('<I',data,4,count+1)
    return bytes(data)+entry.node('psci',[
        ('compatible',b'arm,psci-1.0\0'),('method',b'smc\0')])


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hardware-evidence',type=Path,default=HERE/'j716f-hardware-evidence.json')
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args()
    evidence=json.loads(args.hardware_evidence.read_text(encoding='utf-8'))
    data=make(evidence)
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with args.output.open('xb') as stream:stream.write(data)
    print(f'Native platform single-CPU ADT candidate: {len(data)} bytes; fresh private seed; no storage or SMP')
