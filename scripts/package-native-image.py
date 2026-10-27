#!/usr/bin/env python3
"""Reverse-audit actual native FD/FAT, wrap Android v2, then unpack it again.

Host-only. Never calls adb/fastboot, writes a device or upgrades qualification.
"""
import argparse
import gzip
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tomllib
import zlib
from native_candidate import check_kernel, load_module, sha, save_new
from native_image_formats import extract_fd, read_fat32

def one(parts,kind):
    items=[body for k,body in parts if k==kind]
    if len(items)!=1:raise ValueError('Expected one EFI section of type '+str(kind))
    return items[0]

def package(a):
    if sys.flags.optimize:raise ValueError('Do not disable binary parser assertions')
    stage=a.stage;built=stage/'built'
    spec=json.loads((stage/'firmware-source.json').read_text())
    build=json.loads((built/'build-result.json').read_text())
    pre=json.loads((a.prelinked/'native-prelink.json').read_text())
    if build['exit_code'] or pre['selection']!=spec['selection']:raise ValueError('Not a successful selected build')
    for path,digest in spec['source_files'].items():
        if sha((stage/path).read_bytes())!=digest:raise ValueError('Source snapshot changed: '+path)
    for path,digest in build['artifacts'].items():
        if sha((built/path).read_bytes())!=digest:raise ValueError('Build artifact changed: '+path)
    fd=(built/'SILICIUM_UEFI.fd').read_bytes();fv,modules,sec=extract_fd(fd)
    if fv!=(built/'FVMAIN.Fv').read_bytes():raise ValueError('Compressed FV differs from linked FV')
    meta=(built/'firmware-build-report.txt').read_text()
    declared={}
    for block in meta.split('Module Summary')[1:]:
        inf=re.search(r'Module INF Path:\s+(\S+)',block)
        guid=re.search(r'File GUID:\s+(\S+)',block)
        if inf and guid:declared[inf[1]]=guid[1].lower()
    include=(stage/'Platforms/Lenovo/j716fPkg/Include/NATIVE_DXE.inc').read_text()
    selected=re.findall(r'^\s*INF\s+(\S+)',include,re.M)
    expected={declared[p] for p in selected}|{spec['fat_guid'],'fc510ee7-ffdc-11d4-bd41-0080c73c8881'}
    if set(modules)!=expected:raise ValueError('FD module set differs from dedicated FDF')
    if 'J716fV23TraceUniXNULib' in meta or '/NativeMenu/UniXNULib.inf' not in meta:
        raise ValueError('Wrong linked menu library')
    linked={}
    for name,guid in (('UniXNU','9b665204-a133-47a2-9396-9e2b370a4e25'),
                      ('DarwinBoot','1b64af87-8415-4b57-9b16-680d70913b9f'),
                      ('J716fNativeFatDxe','067750ad-8f6d-4306-bd22-2c4f1ae21627'),
                      ('J716fKeysDxe','0e917852-129a-44cb-bc69-fa6444ba1447')):
        binary=one(modules[guid],0x10)
        if binary!=(built/(name+'.efi')).read_bytes():raise ValueError('Wrong final linked '+name)
        linked[name]=sha(binary)
    # The map belongs to SEC and DXE users, not only a host source declaration.
    map_users=[sec]+[one(modules[g],0x10) for g in (
        'd6a2cb7f-6a18-4e2f-b43b-9920a733700a','6c2004ef-4e0e-4be4-b14c-340eb4aa5891',
        'b8d9777e-d72a-451f-9bdb-bafb52a68415','40d49ba8-448a-40e5-bf92-75fdc4a760d4')]
    for binary in map_users:
        for pair in ((0xb0000000,0x100000),(0xd0000000,0x18000000),(0xce000000,0x700000)):
            if binary.count(struct.pack('<QQ',*pair))!=1:raise ValueError('Missing/ambiguous linked memory reservation')
    fat=one(modules[spec['fat_guid']],0x19)
    if fat!=(built/'NativePayload.fat').read_bytes():raise ValueError('Wrong embedded FAT')
    content=read_fat32(fat);payload=json.loads((built/'payload.json').read_text())
    wanted={'/EFI/UniXNU/unixnu.toml','/EFI/Tools/J716fNativeRead.efi','/EFI/Tools/J716fNativeArm.efi',
            '/Payloads/kernel.macho','/Payloads/kernel.macho.adt'}
    if set(content)!=wanted or set(payload['files'])!={p[1:] for p in wanted}:raise ValueError('Unexpected FAT contents')
    for path,raw in content.items():
        if payload['files'][path[1:]]!={'sha256':sha(raw),'bytes':len(raw)}:raise ValueError('FAT manifest mismatch')
    kernel=content['/Payloads/kernel.macho'];adt=content['/Payloads/kernel.macho.adt']
    if kernel!=(a.prelinked/'kernel.macho').read_bytes() or sha(kernel)!=pre['kernel_sha256']:
        raise ValueError('Final image does not contain the exact selected native kernel')
    kernel_checks=check_kernel(kernel)
    if adt!=(stage/'inputs/kernel.macho.adt').read_bytes():raise ValueError('Wrong generated ADT')
    adt_checks=json.loads((stage/'inputs/kernel.macho.json').read_text())
    if sha(adt)!=adt_checks['adt_sha256']:raise ValueError('ADT provenance mismatch')
    for name in ('J716fNativeRead','J716fNativeArm'):
        if content['/EFI/Tools/'+name+'.efi']!=(built/(name+'.efi')).read_bytes():raise ValueError('Wrong reader/ARM')
    menu=tomllib.loads(content['/EFI/UniXNU/unixnu.toml'].decode())
    if (menu['boot']!={'timeout':-1,'fallback_timeout':-1,'rotate':0,'connect_all':False,'default':'reader'} or
        menu['kits']['autoload'] is not False or [e['id'] for e in menu['entry']]!=['reader','arm','native','exit'] or
        [e['type'] for e in menu['entry']]!=['efi','efi','xnu','action'] or
        menu['entry'][2]['path']!='\\Payloads\\kernel.macho' or
        menu['entry'][2]['args']!='-v serial=0 debug=0x14e cpus=1 -no_slto_panic' or
        menu['entry'][3]['action']!='exit' or any(e.get('action') in ('reset','shutdown') for e in menu['entry'])):
        raise ValueError('Unexpected native manual policy')
    if 'Native manual-only:'.encode('utf-16le') not in (built/'UniXNU.efi').read_bytes():
        raise ValueError('Manual-only loop not linked')
    for marker in ('V23 stage:','PMIC RESET:','V23P22 VM RETURN TRACE'):
        if marker.encode('utf-16le') in (built/'UniXNU.efi').read_bytes():raise ValueError('Old trace menu linked')
    shim=a.shim.read_bytes();dtb=a.dtb.read_bytes()
    if sha(shim)!='72bfc9e9137a0e3ab9f7264162968c546ba63533b94d6813814fe84c831d03b4':raise ValueError('Wrong qualified BootShim')
    if sha(dtb)!='2c0d57deb7f99a081fe4994535c51ca0362bf44b6ad5422a7300f01496f6b3ec':raise ValueError('Wrong firmware DTB')
    if shim[56:60]!=b'ARM\x64' or struct.unpack_from('<QQ',shim,8)!=(0xce000000,len(shim)+len(fd)):
        raise ValueError('BootShim/FD geometry mismatch')
    if a.output.exists():raise ValueError('Never overwrite a final image attempt')
    a.output.mkdir(parents=True)
    raw=shim+fd;packed=gzip.compress(raw,mtime=0)+dtb
    (a.output/'Image.gz-dtb').write_bytes(packed);(a.output/'empty-ramdisk').write_bytes(b'')
    mk=a.uefi/'Resources/Scripts/mkbootimg.py'
    source=subprocess.check_output(['git','-C',str(a.uefi),'show',spec['uefi_commit']+':Resources/Scripts/mkbootimg.py'])
    if mk.read_bytes().replace(b'\r\n',b'\n')!=source.replace(b'\r\n',b'\n'):raise ValueError('Changed image wrapper')
    target=a.output/'j716f-native-log-40458123294-boot.img'
    subprocess.run([sys.executable,str(mk),'--kernel',str(a.output/'Image.gz-dtb'),
       '--ramdisk',str(a.output/'empty-ramdisk'),'--dtb',str(a.dtb),'--output',str(target),
       '--header_version','2','--base','0','--kernel_offset','32768','--tags_offset','256',
       '--pagesize','4096','--dtb_offset','32505856'],check=True)
    final=target.read_bytes();ks,ka,rs,ra,ss,sa,tags,page,version,_=struct.unpack_from('<10I',final,8)
    if final[:8]!=b'ANDROID!' or (ka,rs,ss,tags,page,version)!=(0x8000,0,0,0x100,4096,2):raise ValueError('Wrong boot header')
    dec=zlib.decompressobj(31);actual=dec.decompress(final[page:page+ks])
    if not dec.eof or dec.unused_data!=dtb or actual!=raw:raise ValueError('Final boot/FD roundtrip failed')
    _,again,_=extract_fd(actual[len(shim):])
    final_files=read_fat32(one(again[spec['fat_guid']],0x19))
    if final_files!=content:raise ValueError('Final boot/FAT/file roundtrip failed')
    dtbat=page*(1+(ks+page-1)//page)
    if final[dtbat:dtbat+len(dtb)]!=dtb or any(final[page+ks:dtbat]) or any(final[dtbat+len(dtb):]):raise ValueError('Boot DTB/padding mismatch')
    if len(final)!=(dtbat+len(dtb)+page-1)//page*page or len(final)>=0x6000000:raise ValueError('Boot partition/length bounds')
    import hashlib
    digest=hashlib.sha1()
    for part in (packed,b'',b'',b'',dtb):digest.update(part);digest.update(struct.pack('<I',len(part)))
    if final[576:608]!=digest.digest()+bytes(12):raise ValueError('Android v2 image ID mismatch')
    report={'schema':1,'stage':'native-final-image-host-audited','selection':spec['selection'],
      'image':target.name,'image_bytes':len(final),'image_sha256':sha(final),'fd_sha256':sha(fd),
      'fd_bytes':len(fd),'fv_sha256':sha(fv),'payload_sha256':sha(fat),'kernel_sha256':sha(kernel),
      'kernel_bytes':len(kernel),'source_kernel_sha256':pre['source_kernel_sha256'],
      'logger_candidate_sha256':spec['selection']['logger_candidate_sha256'],'logger_checks':pre['logger_checks'],
      'kernel_checks':kernel_checks,'adt_checks':adt_checks,'files':payload['files'],'linked_modules':linked,
      'source_manifest_sha256':sha((stage/'firmware-source.json').read_bytes()),
      'build_manifest_sha256':sha((built/'build-result.json').read_bytes()),
      'bootshim_sha256':sha(shim),'firmware_dtb_sha256':sha(dtb),'module_guids':sorted(modules),
      'reservation_descriptors_linked':True,'transport':spec['transport'],'automatic_start':False,
      'legacy_code_cave_patching':False,'stage_reset_policy':False,'hardware_launch_admitted':False,
      'device_writes_performed':False,'hardware_validated':False,
      'legacy_reference_adt_in_artifact_lock_used':False,
      'limits':['Host provenance/roundtrip only; no native device launch.',
                'Retention, permanent map lifetime, real IRQ and scheduler require same-image hardware evidence.',
                'Standard initrd and Advanced Logger are not integrated.']}
    save_new(a.output/'host-audit.json',report)
    print(json.dumps({k:report[k] for k in ('stage','image','image_bytes','image_sha256','kernel_sha256','hardware_validated')},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('stage','prelinked','shim','dtb','uefi','output'):p.add_argument('--'+n,type=Path,required=True)
    package(p.parse_args())
