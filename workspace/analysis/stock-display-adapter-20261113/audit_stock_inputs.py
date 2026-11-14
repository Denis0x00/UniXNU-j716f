#!/usr/bin/env python3
"""Read captured stock DT/sysfs; emit only display inputs, not private DT data."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import struct
import sys
sys.dont_write_bytecode=True
ROOT=Path(__file__).resolve().parents[2]
D=ROOT/'analysis/device/20261111-231708'
spec=importlib.util.spec_from_file_location('stock_dt_reader',D/'parse_input_display.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out',type=Path,default=Path(__file__).with_name('stock-display-provenance.json'))
    args=parser.parse_args()
    nodes=m.fdt((D/'running.dtb').read_bytes()); live=m.sysfs_tree()
    assert nodes['/reserved-memory']['#address-cells']==struct.pack('>I',2)
    assert nodes['/reserved-memory']['#size-cells']==struct.pack('>I',2)
    records=[]
    for name,base,size in [('cont_splash_region@9c000000',0x9c000000,0x2300000),
                           ('disp_rdump_region@9c000000',0x9c000000,0x800000),
                           ('dfps_data_region@9e300000',0x9e300000,0x100000)]:
        path='/reserved-memory/'+name;n=nodes[path]
        assert struct.unpack('>QQ',n['reg'])==(base,size)
        assert {k:v for k,v in n.items() if k!='name'}=={k:v for k,v in live[path].items() if k!='name'}
        records.append({'node':path,'address':hex(base),'length':hex(size),
                        'end_exclusive':hex(base+size),'no_map':'no-map' in n,
                        'reusable':'reusable' in n,'sysfs_matches_running_dtb':True,
                        'reg_hex':n['reg'].hex()})
    merged=[]
    for r in sorted(records,key=lambda v:int(v['address'],16)):
        begin,end=int(r['address'],16),int(r['end_exclusive'],16)
        if merged and begin<=merged[-1][1]: merged[-1][1]=max(end,merged[-1][1])
        else: merged.append([begin,end])
    assert merged==[[0x9c000000,0x9e400000]]
    panel='/soc/qcom,mdss_mdp@ae00000/qcom,mdss_dsi_rm692e6_edo_amoled_cmd'
    fields=['qcom,mdss-dsi-panel-name','qcom,mdss-dsi-panel-type',
            'qcom,mdss-dsi-panel-physical-type','qcom,mdss-dsi-bpp',
            'qcom,mdss-dsi-bl-pmic-control-type','qcom,mdss-dsi-bl-min-level','qcom,mdss-dsi-bl-max-level']
    props={k:m.readable(nodes[panel][k]) for k in fields if k in nodes[panel]}
    timings={}
    for path,node in nodes.items():
        if path.startswith(panel+'/qcom,mdss-dsi-display-timings/timing@'):
            timings[path.rsplit('/',1)[1]]={k:m.readable(node[k]) for k in [
                'qcom,mdss-dsi-panel-framerate','qcom,mdss-dsi-panel-width',
                'qcom,mdss-dsi-panel-height','qcom,display-topology'] if k in node}
    platform=ROOT/'work/uefi-j716f/Platforms/Lenovo/j716fPkg'
    uefi_memory=platform/'Library/MemoryMapLib/MemoryMapLib.c'
    assert '{"Display Reserved", 0x9C000000, 0x02400000,' in uefi_memory.read_text()
    inputs=[D/name for name in ['running.dtb','device-tree.tar','properties.txt','soc.txt','memory.txt',
            'display.txt','input-display-summary.json','parse_input_display.py']]
    inputs += [uefi_memory,platform/'Drivers/J716fInheritedFbDxe/J716fInheritedFb.c',
               ROOT/'UniXNU/patches/xnu/offline-component-load-probes.patch',
               ROOT/'UniXNU/patches/xnu/offline-legacy-start.patch']
    fingerprint=next(x.split('=',1)[1] for x in (D/'properties.txt').read_text().splitlines()
                     if x.startswith('ro.build.fingerprint='))
    assert fingerprint=='Lenovo/LenovoTB-J716F_PRC/J716F:13/TKQ1.221013.002/15.0.485_240205:user/release-keys'
    record={'scope':'offline original Android capture and local UEFI source cross-check only',
            'android_fingerprint':fingerprint,'original_android_reserved_nodes':records,
            'union':{'start':'0x9c000000','end_exclusive':'0x9e400000','bytes':0x2400000,'deduplicated':True},
            'android_iomem_reserved_line':next(x.strip() for x in (D/'memory.txt').read_text().splitlines()
                                              if '9c000000-9e3fffff' in x),
            'local_uefi_consistent_with_android_union':True,
            'local_uefi_memory_type':'Reserved, WRITE_THROUGH_XN',
            'current_xnu_allocator_exclusion_proven':False,'current_scanout_ownership_proven':False,
            'panel':{'active_boot_selection':re.search(rb'msm_drm\.dsi_display0=[^\x00\s]+',
                nodes['/chosen']['bootargs']).group().decode(),'properties':props,'timings':timings,
                'instantaneous_refresh_proven':False},
            'source_sha256':{p.relative_to(ROOT).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}}
    args.out.write_text(json.dumps(record,indent=2)+'\n')
    print('Stock ZUI 15.0.485: 3 DT/sysfs nodes agree, deduplicated display reservation = 36 MiB; current XNU claim remains unqualified')

if __name__=='__main__':main()
