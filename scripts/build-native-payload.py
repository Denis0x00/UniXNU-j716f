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

def build(stage):
    stage=Path(stage);out=stage/'payload';out.mkdir(exist_ok=False)
    tree=out/'tree'
    (tree/'EFI/UniXNU').mkdir(parents=True);(tree/'EFI/Tools').mkdir();(tree/'Payloads').mkdir()
    (tree/'EFI/UniXNU/unixnu.toml').write_text(CONFIG,newline='\n')
    for name in ('kernel.macho','kernel.macho.adt'):shutil.copyfile(stage/'inputs'/name,tree/'Payloads'/name)
    for name in ('J716fNativeRead.efi','J716fNativeArm.efi'):
        shutil.copyfile(stage/'Build/NativeLogApps/RELEASE_CLANGPDB/AARCH64'/name,tree/'EFI/Tools'/name)
    for path in (tree,*tree.rglob('*')):os.utime(path,(315532800,315532800))
    fat=stage/'Platforms/Lenovo/j716fPkg/Resources/NativePayload.fat'
    if fat.exists():raise ValueError('Preserve prior FAT')
    env=dict(os.environ,MTOOLS_SKIP_CHECK='1')
    subprocess.run(['mformat','-C','-F','-c','1','-T','131072','-v','J716FNATIVE','-N','716f1001','-i',str(fat),'::'],check=True,env=env)
    for folder in ('EFI','Payloads'):
        subprocess.run(['mcopy','-i',str(fat),'-s','-Q','-m',str(tree/folder),'::/'],check=True,env=env)
    digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
    report={'payload_sha256':digest(fat),'payload_bytes':fat.stat().st_size,'automatic_start':False,
            'files':{p.relative_to(tree).as_posix():{'sha256':digest(p),'bytes':p.stat().st_size}
                     for p in tree.rglob('*') if p.is_file()},'hardware_validated':False}
    with (out/'payload.json').open('x') as stream:stream.write(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'payload_sha256':report['payload_sha256'],'file_count':len(report['files'])}))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('stage',type=Path)
    build(p.parse_args().stage)
