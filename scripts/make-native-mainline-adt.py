#!/usr/bin/env python3
"""Public DTS -> pinned shared ADT generator, with explicit single-CPU PSCI.

The pinned shared generator emits PSCI only for SMP. ArmCpu also consumes its
conduit for the boot CPU, so this device adapter adds the one missing root node.
No opaque historical ADT, device identifier, storage or userland payload input.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import tomllib
from native_candidate import sha, save_new

ROOT=Path(__file__).resolve().parents[1]

def make(upstream, dtb):
    upstream=Path(upstream)
    pin='42bffe8a56823dad2e448c776b4f94862f3e290a'
    if subprocess.check_output(['git','-C',str(upstream),'rev-parse','HEAD'],text=True).strip()!=pin:
        raise ValueError('Wrong shared generator source revision')
    for path in ('scripts/unixnu/devicetree.py','scripts/unixnu/profiles.py'):
        expected=subprocess.check_output(['git','-C',str(upstream),'show','HEAD:'+path])
        if (upstream/path).read_bytes().replace(b'\r\n',b'\n')!=expected.replace(b'\r\n',b'\n'):
            raise ValueError('Unreviewed shared ADT generator modifications')
    sys.path.insert(0,str(upstream/'scripts'))
    from unixnu.devicetree import generate, read_fdt, hardware_properties, node
    from unixnu.profiles import merge
    config=merge(tomllib.loads((upstream/'profiles/os/ipados-17.7.11-21H461/profile.toml').read_text()),
                 tomllib.loads((ROOT/'board.toml').read_text()))
    # Early platform target has no userland text patches or storage sidecars.
    config['payload'].pop('text_fixups',None)
    config['payload']['adt']['chip_id']=f'{config["ipsw"]["chip_id"]:x}'
    seed=secrets.randbelow((1<<64)-1)+1
    result=bytearray(generate(dtb,config,unique_chip_id=f'{seed:x}'))
    root=read_fdt(Path(dtb).read_bytes())
    cpus=next(e for e in root.children if e.name=='cpus')
    if len([e for e in cpus.children if e.enabled()])!=1:raise ValueError('Single CPU only')
    psci=next(e for e in root.children if e.name=='psci')
    if psci.strings('method')!=['smc']:raise ValueError('PSCI differs from J716F evidence')
    count=struct.unpack_from('<I',result,4)[0];struct.pack_into('<I',result,4,count+1)
    result+=node('psci',hardware_properties(psci,{}))
    spec=importlib.util.spec_from_file_location('native_adt',ROOT/'scripts/check-native-platform-adt.py')
    check=importlib.util.module_from_spec(spec);spec.loader.exec_module(check)
    report=check.check(bytes(result),json.loads((ROOT/'scripts/j716f-hardware-evidence.json').read_text()))
    tree=check.decode(bytes(result))
    if '/arm-io/dram' in tree:raise ValueError('DRAM must not be exported as MMIO')
    if (struct.unpack('<Q',tree['/chosen']['dram-base'])[0]!=0xc0000000 or
            struct.unpack('<Q',tree['/chosen']['dram-size'])[0]!=0x140000000 or
            struct.unpack('<Q',tree['/chosen']['darwinboot-memory-size'])[0]!=0x10000000):
        raise ValueError('Changed physical/owned DRAM handoff')
    if tree['/arm-io/timer']['interrupts']!=struct.pack('<12I',1,13,0xff08,1,14,0xff08,1,11,0xff08,1,12,0xff08):
        raise ValueError('Wrong standard architectural timer bindings')
    reservations=[e for e in root.children if e.name=='reserved-memory']
    if len(reservations)!=1:raise ValueError('Missing retained RAM reservation')
    logs=[e for e in reservations[0].children if e.name=='boot-log@b0000000']
    if len(logs)!=1 or logs[0].cells('reg')!=[0,0xb0000000,0,0x100000] or 'no-map' not in logs[0].properties:
        raise ValueError('Retained console reservation differs')
    report.update(shared_generator_commit=pin,hardware_dtb_sha256=sha(Path(dtb).read_bytes()),
                  single_cpu_psci_adapter=True,dram_exported_as_mmio=False,storage_sidecars=False,
                  retained_ram_in_hardware_dts=True,retained_ram_outside_dram_handoff=True)
    return bytes(result),report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--unixnu',type=Path,required=True)
    p.add_argument('--dtb',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    if a.output.exists() or a.output.with_suffix('.json').exists():raise ValueError('Preserve prior ADT')
    data,report=make(a.unixnu,a.dtb)
    a.output.parent.mkdir(parents=True,exist_ok=True)
    with a.output.open('xb') as stream:stream.write(data)
    save_new(a.output.with_suffix('.json'),report)
    print(json.dumps(report,indent=2))
