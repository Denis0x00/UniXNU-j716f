#!/usr/bin/env python3
"""Official host-only payload/boot image build for the independent r30s target."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

sys.dont_write_bytecode=True
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from verify_xnu_early_log_firmware import validate_fd
from verify_xnu_entry import read_fat32
from verify_storage_kernel import derive_expected, verify_candidate
from verify_storage_image import verify, identity, sha, require, menu_check, FD, TRUST, DTB, SHIM, BASE_ADT

def save(p,v):
    p.parent.mkdir(parents=True,exist_ok=True)
    p.write_text(json.dumps(v,indent=2)+'\n')

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root',type=Path,required=True)
    p.add_argument('--expectation',type=Path,required=True)
    p.add_argument('--attempt',type=int,default=1)
    a=p.parse_args()
    require(sys.platform=='linux' and a.attempt>0,'Prepared Linux host required')
    root=a.root.resolve(strict=True)
    target=root/'work/UniXNU-j716f-xnu-storage-r30'
    core=root/'work/UniXNU-core-adt-phandle-r23'
    uefi=root/'work/uefi-j716f'
    kernel=root/'artifacts/j716f-host-storage-20261114-r30/kernel.macho'
    trust=root/'artifacts/j716f-ipados-root-inputs-20261114-r30/trustcache-21H461.bin'
    firmware=root/'artifacts/j716f-early-log-firmware-20261114'
    fd=firmware/'SILICIUM_UEFI.fd'
    dtb=target/'firmware/j716f-stock-base00-overlay01-normalized.dtb'
    shim=root/'artifacts/j716f-qr-export-fix-20261112/menu/shim.bin'
    expectation_path=(root/a.expectation).resolve(strict=True)
    expectation=json.loads(expectation_path.read_text())
    proposed=derive_expected(kernel.read_bytes(),expectation)
    for relative,spec in expectation['review_evidence'].items():
        evidence=(root/relative).resolve(strict=True)
        require(evidence.is_relative_to(root) and identity(evidence)==spec,'Review evidence differs: '+relative)
    before=verify_candidate(kernel.read_bytes(),proposed,expectation)
    del proposed
    for f,h in ((fd,FD),(dtb,DTB),(shim,SHIM),(trust,TRUST)):
        require(identity(f)['sha256']==h,'Input pin differs: '+f.name)
    require(trust.stat().st_size==61356,'Unexpected original trust-cache length')
    firmware_review=validate_fd(root,fd,firmware/'fd-provenance.json',target/'firmware-expectation.json')
    host_build=json.loads((root/'artifacts/j716f-host-storage-20261114-r30/build-result.json').read_text())
    require(host_build['status']=='passed','Official source build did not pass')
    require(any(x['sha256']==identity(kernel)['sha256'] for x in host_build['artifacts']),'Source kernel not in official build report')
    menu=(target/'payload/unixnu.toml').read_bytes()
    menu_check(menu)
    baseline_payload=root/'artifacts/j716f-xnu-bsd-autoconf-probe-20261114-r29/payload.img'
    files,_=read_fat32(baseline_payload.read_bytes())
    baseline_adt=files['efi/unixnu/payloads/kernel.macho.adt']
    require(sha(baseline_adt)==BASE_ADT,'Original r30 hardware ADT changed')
    suffix='' if a.attempt==1 else '-attempt'+str(a.attempt)
    name='j716f-xnu-storage-inputs-20261114-r30s'+suffix
    out=core/('out-'+name)
    artifact=root/'artifacts'/name
    report_dir=root/'analysis/r30-storage-inputs-20261114'/('image-'+str(a.attempt).zfill(2))
    for d in (out,artifact,report_dir):
        require(not d.exists(),'Refusing to overwrite output: '+str(d))
    artifact.mkdir(parents=True);report_dir.mkdir(parents=True)
    seed=(root/'private-inputs/j716f/platform-seed.hex').read_text().strip()
    require(re.fullmatch('[0-9a-fA-F]{16}',seed) and int(seed,16),'Invalid private platform seed')
    def redact(data):
        for s in {seed,seed.lower(),seed.upper()}:
            data=data.replace(s.encode(),b'<private-platform-seed>')
        return data
    def scrub_logs():
        if not out.exists():return
        for f in out.rglob('*'):
            if f.suffix in ('.log','.stdout','.stderr') and f.is_file() and f.resolve().is_relative_to(out):
                b=f.read_bytes()
                if b'\0' not in b:
                    c=redact(b)
                    if c!=b:f.write_bytes(c)
    env={k:v for k,v in os.environ.items() if not k.startswith(('UNIXNU_','XNU_','MTOOLS_')) and k!='JOBS'}
    env.update(UNIXNU_UNIQUE_CHIP_ID=seed,UNIXNU_TOOLCHAIN='/opt/unixnu-linux',PYTHONDONTWRITEBYTECODE='1',
               PYTHONUNBUFFERED='1',MTOOLSRC='/dev/null',SOURCE_DATE_EPOCH='315532800',TZ='UTC',LC_ALL='C')
    record=dict(status='running',hardware_tested=False,device_actions=False,qemu=False,steps=[],
                scope='new storage registration kernel and original trust-cache; same pre-root checkpoint',
                kernel=identity(kernel),trust=identity(trust),source_build=host_build,
                independent_before_generation=before,firmware_reused=True,storage_runtime_qualified=False,
                root_ready=False,data_seed_present=False)
    pinned={}
    def pin(f):
        relative=f.relative_to(root).as_posix()
        pinned[relative]=identity(f)
    def copy(f,name):
        destination=artifact/name
        destination.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(f,destination)
    for f in (kernel,trust,fd,dtb,shim,firmware/'J716fEarlyLogProbe.efi',expectation_path,
              firmware/'fd-provenance.json',core/'scripts/unixnu/devicetree.py',core/'scripts/patch-xnu-entry-probe.py'):
        pin(f)
    for relative in expectation['review_evidence']:pin(root/relative)
    for folder in (target,root/'work/UniXNU-j716f-host-storage-r30',root/'work/J716fStorageBringup-r30'):
        for f in folder.rglob('*'):
            if f.is_file() and not {'.git','__pycache__'}.intersection(f.parts):pin(f)
    def run(label,argv):
        result=subprocess.run(list(map(str,argv)),cwd=root,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        stem=f'{len(record["steps"])+1:02d}-{label}'
        (report_dir/(stem+'.stdout')).write_bytes(redact(result.stdout))
        (report_dir/(stem+'.stderr')).write_bytes(redact(result.stderr))
        scrub_logs()
        record['steps'].append(dict(label=label,argv=list(map(str,argv)),exit_code=result.returncode))
        save(report_dir/'build-result.json',record)
        require(result.returncode==0,'Official step failed: '+label)
        return redact(result.stdout)
    try:
        for f,name in ((kernel,'original-kernel.macho'),(trust,'trustcache-21H461.bin'),(fd,'SILICIUM_UEFI.fd'),
                       (dtb,'firmware.dtb'),(shim,'shim.bin'),(target/'board.toml','board.toml'),
                       (expectation_path,'kernel-expectation.json'),(firmware/'J716fEarlyLogProbe.efi','J716fEarlyLogProbe.efi'),
                       (firmware/'fd-provenance.json','fd-provenance.json')):copy(f,name)
        (artifact/'baseline.adt').write_bytes(baseline_adt)
        (artifact/'unixnu.toml').write_bytes(menu)
        save(artifact/'source-inputs.json',pinned)
        for relative in expectation['review_evidence']:copy(root/relative,'review-evidence/'+relative)
        for rel in pinned:
            if rel.startswith('work/') and not rel.endswith('.dtb'):copy(root/rel,'sources/'+rel)
        common=['--config',target/'build-inputs.toml','-D',target,'--out',out,'--os','ipados-17.7.11-21H461',
                '--uefi',uefi,'--target','DEBUG','-j','2']
        cli=[sys.executable,core/'unixnu']
        run('defconfig',cli+['defconfig','j716f_xnu_storage_r30_defconfig']+common)
        status=json.loads(run('status',cli+['status','--json']+common))
        require(status['board']['payload']['trust_cache_sha256']==
                [TRUST,'8900d6a93d7a59ce1a872a5c081e559f4240a218f97e12d44d22dda46644d68f'],
                'Official profile trust allow-list differs')
        save(artifact/'status.json',status)
        run('payload',cli+['build','payload','--kernel',kernel,'--trust-cache',trust]+common)
        found=list(out.rglob('payload/payload.json'))
        require(len(found)==1,'Ambiguous payload output')
        copy(found[0],'payload.official.json');copy(found[0].parent/'payload.img','payload.img')
        payload=artifact/'payload.img'
        for f in (artifact/'unixnu.toml',artifact/'J716fEarlyLogProbe.efi'):os.utime(f,(315532800,315532800))
        run('menu', ['mcopy','-o','-m','-i',payload,artifact/'unixnu.toml','::/EFI/UniXNU/unixnu.toml'])
        run('tools-dir',['mmd','-i',payload,'::/EFI/UniXNU/Tools'])
        run('logger',['mcopy','-m','-i',payload,artifact/'J716fEarlyLogProbe.efi','::/EFI/UniXNU/Tools/J716fEarlyLogProbe.efi'])
        run('fat-check',['fsck.fat','-n',payload])
        run('bootimg',cli+['bootimg','--fd',fd,'--dtb',dtb,'--ramdisk',payload,'--output',artifact/'boot.img']+common)
        result=verify(artifact)
        require(result['kernel']==before,'Before/after independent kernel verification differs')
        for rel,spec in pinned.items():require(identity(root/rel)==spec,'Input changed during build: '+rel)
        record.update(status='host-generation-passed',verification=result,pinned_inputs=pinned,
                      finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
        save(artifact/'acceptance-linux.json',result)
    except Exception as exc:
        scrub_logs()
        record.update(status='failed',error=redact(str(exc).encode()).decode())
        save(report_dir/'build-result.json',record)
        print(json.dumps(dict(status=record['status'],error=record['error'])))
        return 1
    save(report_dir/'build-result.json',record);save(artifact/'build-result.json',record)
    shutil.copytree(report_dir,artifact/'logs')
    save(artifact/'manifest.json',dict(outputs={f.relative_to(artifact).as_posix():identity(f) for f in artifact.rglob('*') if f.is_file()},hardware_tested=False))
    print(json.dumps(dict(status=record['status'],artifact=str(artifact),boot=identity(artifact/'boot.img'))))
    return 0

if __name__=='__main__':sys.exit(main())
