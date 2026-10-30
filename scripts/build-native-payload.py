#!/usr/bin/env python3
"""Build a NEW private native FAT from explicitly staged inputs; no devices."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

CONFIG=r'''version = 1
[boot]
timeout = -1
fallback_timeout = -1
rotate = 0
connect_all = false
default = "reader"
[kits]
autoload = false
[ui]
title = "J716F NATIVE LOG 40458123294 - manual only / no stage reset"
scale = 3
[[entry]]
id = "reader"
title = "1. Read frozen retained evidence FIRST (no RAM writes)"
type = "efi"
path = "\\EFI\\Tools\\J716fNativeRead.efi"
[[entry]]
id = "arm"
title = "2. Query then double-confirm ARM (no reset / no clear)"
type = "efi"
path = "\\EFI\\Tools\\J716fNativeArm.efi"
[[entry]]
id = "native"
title = "3. Native platform + CPU + timer: launch armed candidate ONCE"
type = "xnu"
path = "\\Payloads\\kernel.macho"
args = "-v serial=0 debug=0x14e cpus=1 -no_slto_panic"
confirm = "Photographed fresh ARM for candidate 1c19ee51? Launch once only."
[[entry]]
id = "exit"
title = "Exit and halt firmware (no restart)"
type = "action"
action = "exit"
'''

def config(export_init=False):
    if not export_init:return CONFIG
    extra=r'''[[entry]]
id = "export"
title = "2. Export ALL raw RAM as offline QR (read only)"
type = "efi"
path = "\\EFI\\Tools\\J716fNativeExport.efi"
[[entry]]
id = "init"
title = "3. MAINT v2: new Export then Init in SAME app"
type = "efi"
path = "\\EFI\\Tools\\J716fNativeInit.efi"
'''
    return CONFIG.replace('manual only / no stage reset','MAINT v2 / manual only').replace(
        '[[entry]]\nid = "arm"',extra+'[[entry]]\nid = "arm"').replace(
        '2. Query then','4. Query then').replace('3. Native platform','5. Native platform')

def build(stage):
    stage=Path(stage);out=stage/'payload';out.mkdir(exist_ok=False)
    spec=json.loads((stage/'firmware-source.json').read_text())
    export_init=spec.get('export_init',False)
    if type(export_init) is not bool:raise ValueError('Invalid explicit export/init profile')
    tree=out/'tree'
    (tree/'EFI/UniXNU').mkdir(parents=True);(tree/'EFI/Tools').mkdir();(tree/'Payloads').mkdir()
    (tree/'EFI/UniXNU/unixnu.toml').write_text(config(export_init),newline='\n')
    for name in ('kernel.macho','kernel.macho.adt'):shutil.copyfile(stage/'inputs'/name,tree/'Payloads'/name)
    names=['J716fNativeRead.efi','J716fNativeArm.efi']
    if export_init:names+=['J716fNativeExport.efi','J716fNativeInit.efi']
    for name in names:
        shutil.copyfile(stage/'Build/NativeLogApps/RELEASE_CLANGPDB/AARCH64'/name,tree/'EFI/Tools'/name)
    for path in (tree,*tree.rglob('*')):os.utime(path,(315532800,315532800))
    fat=stage/'Platforms/Lenovo/j716fPkg/Resources/NativePayload.fat'
    if fat.exists():raise ValueError('Preserve prior FAT')
    env=dict(os.environ,MTOOLS_SKIP_CHECK='1')
    subprocess.run(['mformat','-C','-F','-c','1','-T','131072','-v','J716FNATIVE','-N','716f1001','-i',str(fat),'::'],check=True,env=env)
    for folder in ('EFI','Payloads'):
        subprocess.run(['mcopy','-i',str(fat),'-s','-Q','-m',str(tree/folder),'::/'],check=True,env=env)
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    report={'payload_sha256':digest(fat),'payload_bytes':fat.stat().st_size,'automatic_start':False,'export_init':export_init,
            'files':{p.relative_to(tree).as_posix():{'sha256':digest(p),'bytes':p.stat().st_size}
                     for p in tree.rglob('*') if p.is_file()},'hardware_validated':False}
    with (out/'payload.json').open('x') as stream:stream.write(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'payload_sha256':report['payload_sha256'],'file_count':len(report['files'])}))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('stage',type=Path)
    build(p.parse_args().stage)
