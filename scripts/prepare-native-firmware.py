#!/usr/bin/env python3
"""Stage a dedicated native firmware build from immutable source archives.

No old workspace, P22 image, device or shared dependency checkout is modified.
This explicitly keeps the bounded embedded-FAT transport until initrd physical
ownership is proven. The FD remains 0xce000000 + 0x700000.
"""
import argparse
import io
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
from native_candidate import load_module, sha, save_new

ROOT=Path(__file__).resolve().parents[1]
UEFI_PIN='21ac8e3477daf001ea6046283698165cbf3fd1d8'
PKG='Platforms/Lenovo/j716fPkg'
FAT_GUID='90F40472-1975-40E2-8028-C76D530D15A7'

def archive(repo,pin,out,paths=()):
    if subprocess.check_output(['git','-C',str(repo),'rev-parse','HEAD'],text=True).strip()!=pin:
        raise ValueError('Unexpected source checkout: '+str(repo))
    raw=subprocess.check_output(['git','-C',str(repo),'archive','--format=tar',pin,*paths])
    with tarfile.open(fileobj=io.BytesIO(raw)) as tar:
        for item in tar:
            if item.isdir():continue
            if not item.isfile() or Path(item.name).is_absolute() or '..' in Path(item.name).parts:
                raise ValueError('Non-regular source archive member')
            target=out/item.name;target.parent.mkdir(parents=True,exist_ok=True)
            with target.open('xb') as stream:stream.write(tar.extractfile(item).read())

def rewrite(path,old,new):
    text=path.read_text()
    if text.count(old)!=1:raise ValueError('Expected one reviewed firmware preimage: '+old)
    path.write_text(text.replace(old,new),encoding='utf-8',newline='\n')

def prepare(a):
    lock=json.loads(a.selection.read_text());prelink=json.loads((a.prelinked/'native-prelink.json').read_text())
    if prelink['selection']!=lock or not prelink['persistent_early_logger_present']:
        raise ValueError('Dedicated firmware requires the selected prelinked logger')
    kernel=(a.prelinked/'kernel.macho').read_bytes()
    if sha(kernel)!=prelink['kernel_sha256'] or prelink['logger_checks']['candidate_sha256']!=lock['logger_candidate_sha256']:
        raise ValueError('Prelinked source identity mismatch')
    if a.output.exists():raise ValueError('Never overwrite a native staging/build attempt')
    a.output.mkdir(parents=True)
    archive(a.uefi,UEFI_PIN,a.output,(PKG,))
    archive(a.unixnu,lock['unixnu_commit'],a.output,('package',))
    archive(a.unixnu/'drivers/DarwinBoot',lock['components']['DarwinBoot'],a.output/'DarwinBoot')
    reader=load_module('prepare-native-reader').prepare(a.uefi,a.output/'reader-stage',lock['logger_candidate_sha256'])
    shutil.copytree(a.output/'reader-stage/J716fNativeLog',a.output/'J716fNativeLog')
    shutil.copyfile(a.output/'reader-stage/NativeLogApps.dsc',a.output/'NativeLogApps.dsc')
    menu=a.output/'NativeMenu'
    shutil.copytree(a.output/'package/UniXNUPkg/Library/UniXNULib',menu)
    text=(menu/'Menu.c').read_text()
    start=text.index('STATIC\nVOID\nDrawCountdown (');end=text.index('//\n// The Tools submenu',start)
    text=text[:start]+(ROOT/'package/native-log/ManualMenu.inc').read_text()+'\n'+text[end:]
    text=text.replace('Menu loop: input, countdown and idle rotation.','Pinned upstream menu with J716F manual-only loop.')
    (menu/'Menu.c').write_text(text,encoding='utf-8',newline='\n')
    rewrite(menu/'UniXNULib.inf','BASE_NAME      = UniXNULib','BASE_NAME      = J716fNativeUniXNULib')
    guard=r'''  UniXNUConfigLoad (ImageHandle, &Config);
  // Fail closed before injection or boot if the dedicated payload is absent.
  if (Config.Error[0] || Config.EntryCount != 4 || Config.DefaultIndex != 0 ||
      Config.Timeout != -1 || Config.DriverCount || Config.PatchCount || Config.AcpiCount ||
      Config.SliceCount || Config.KitsLoaded) { return EFI_SECURITY_VIOLATION; }
  {
    STATIC CONST CHAR8 *Ids[] = {"reader", "arm", "native", "exit"};
    for (Index = 0; Index < 4; Index++) {
      if (!Config.Entries[Index].Id || AsciiStrCmp (Config.Entries[Index].Id, Ids[Index])) {
        return EFI_SECURITY_VIOLATION;
      }
    }
    if (Config.Entries[0].Type != UniXNUEntryEfi || Config.Entries[1].Type != UniXNUEntryEfi ||
        Config.Entries[2].Type != UniXNUEntryXnu || Config.Entries[3].Type != UniXNUEntryAction ||
        Config.Entries[3].Action != UniXNUActionExit ||
        !Config.Entries[0].Path || StrCmp (Config.Entries[0].Path, L"\EFI\Tools\J716fNativeRead.efi") ||
        !Config.Entries[1].Path || StrCmp (Config.Entries[1].Path, L"\EFI\Tools\J716fNativeArm.efi") ||
        !Config.Entries[2].Path || StrCmp (Config.Entries[2].Path, L"\Payloads\kernel.macho") ||
        !Config.Entries[2].Args || StrCmp (Config.Entries[2].Args, L"-v serial=0 debug=0x14e cpus=1 -no_slto_panic")) {
      return EFI_SECURITY_VIOLATION;
    }
  }'''
    # Raw Python strings above contain C escaped directory separators.
    guard=guard.replace('L"\\EFI','L"\\\\EFI').replace('\\Tools','\\\\Tools').replace('\\J716f','\\\\J716f')
    guard=guard.replace('L"\\Payloads','L"\\\\Payloads').replace('\\kernel','\\\\kernel')
    rewrite(menu/'Main.c','  UniXNUConfigLoad (ImageHandle, &Config);',guard)
    board=a.output/PKG
    dsc=(board/'j716f-vm-return-trace-v23p22.dsc').read_text()
    dsc=dsc.replace('Build/j716fMmuTraceV23P22Pkg','Build/j716fNativePkg')
    dsc=dsc.replace('j716f-vm-return-trace-v23p22.fdf','j716f-native.fdf')
    dsc=dsc.replace('J716F V23P9 KERNEL TRACE r1','J716F NATIVE LOG 40458123294')
    dsc=dsc.replace('j716fPkg/Library/J716fV23TraceUniXNULib/UniXNULib.inf','NativeMenu/UniXNULib.inf')
    dsc=dsc.replace('  j716fPkg/Application/J716fVmReturnTrace/J716fVmReturnTrace.inf\n','')
    dsc=dsc.replace('  j716fPkg/Application/J716fPowerAction/J716fPowerAction.inf\n','')
    dsc=dsc.replace('J716fEntryFatDxe','J716fNativeFatDxe')
    (board/'j716f-native.dsc').write_text(dsc,encoding='utf-8',newline='\n')
    fdf=(board/'j716f-vm-return-trace-v23p22.fdf').read_text()
    fdf=fdf.replace('ENTRY_V23_','NATIVE_').replace('A4934A8E-547B-4E44-B2B2-E044EBFA19EB',FAT_GUID)
    fdf=fdf.replace('J716fVmReturnTraceV23P22.fat','NativePayload.fat')
    (board/'j716f-native.fdf').write_text(fdf,encoding='utf-8',newline='\n')
    for name in ('APRIORI','DXE'):
        text=(board/f'Include/ENTRY_V23_{name}.inc').read_text()
        text=text.replace('# V22 freezes the actual V21 embedded-FAT module selection; no standard initrd migration.',
                          '# Native bounded embedded-FAT transport; standard initrd ownership remains unproven.')
        text=text.replace('J716fEntryFatDxe','J716fNativeFatDxe')
        (board/f'Include/NATIVE_{name}.inc').write_text(text,encoding='utf-8',newline='\n')
    fat=board/'Drivers/J716fNativeFatDxe';fat.mkdir()
    text=(board/'Drivers/J716fEntryFatDxe/J716fEntryFatDxe.inf').read_text().replace('J716fEntryFat','J716fNativeFat')
    text=text.replace('715EBA15-6296-4C97-AE22-A85B64D7E515','067750AD-8F6D-4306-BD22-2C4F1AE21627')
    (fat/'J716fNativeFatDxe.inf').write_text(text,encoding='utf-8',newline='\n')
    text=(board/'Drivers/J716fEntryFatDxe/J716fEntryFat.c').read_text().replace('J716fEntryFat','J716fNativeFat')
    text=text.replace('{0xa4934a8e,0x547b,0x4e44,{0xb2,0xb2,0xe0,0x44,0xeb,0xfa,0x19,0xeb}}',
                      '{0x90f40472,0x1975,0x40e2,{0x80,0x28,0xc7,0x6d,0x53,0x0d,0x15,0xa7}}')
    text=text.replace('J716F V15 ENTRYFAT','J716F NATIVE FAT').replace('0x716f0015','0x716f1001')
    (fat/'J716fNativeFat.c').write_text(text,encoding='utf-8',newline='\n')
    inputs=a.output/'inputs';inputs.mkdir()
    (inputs/'kernel.macho').write_bytes(kernel)
    for name in ('dts/j716f.dts','socs/sm8250/dts/sm8250.dtsi'):
        shutil.copyfile(ROOT/name,inputs/Path(name).name)
    shutil.copyfile(ROOT/'scripts/build-native-payload.py',a.output/'build-native-payload.py')
    report={'schema':1,'selection':lock,'uefi_commit':UEFI_PIN,'reader':reader,
            'prelinked_kernel_sha256':sha(kernel),'prelink_manifest_sha256':sha((a.prelinked/'native-prelink.json').read_bytes()),
            'transport':'read-only embedded FAT; NOT standard initrd',
            'fd_base':'0xce000000','fd_bytes':0x700000,'fat_guid':FAT_GUID.lower(),
            'old_trace_menu_selected':False,'manual_only_menu':True,'device_operations':False,
            'source_files':{p.relative_to(a.output).as_posix():sha(p.read_bytes()) for p in a.output.rglob('*')
                            if p.is_file() and 'inputs' not in p.relative_to(a.output).parts}}
    save_new(a.output/'firmware-source.json',report)
    print(json.dumps({k:report[k] for k in ('prelinked_kernel_sha256','fd_bytes','transport')},indent=2))

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for n in ('selection','prelinked','uefi','unixnu','output'):p.add_argument('--'+n,type=Path,required=True)
    prepare(p.parse_args())
