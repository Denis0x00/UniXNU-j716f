#!/usr/bin/env python3
"""Stage separate reader/ARM apps in a new EDK II workspace, never old firmware."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil

ROOT=Path(__file__).resolve().parents[1]
REUSED={
    'Font.c':'588d0968cc8565aa882f755356bb7ac9440670a973265218b72639bbdc099f81',
    'Panel.h':'8822cd37fd125ad06fbc442850174dd655b373689a39e656de0d144a559a800d',
    'Smc.S':'44e792bc917de7ca4e940f8029217bff53cb28681f9d4c6c851d4f367db8942f',
}
DSC='''[Defines]
  PLATFORM_NAME = J716fNativeLogApps
  PLATFORM_GUID = da593c7b-de34-47cc-b8a3-0d991d607984
  PLATFORM_VERSION = 1.0
  DSC_SPECIFICATION = 0x00010005
  OUTPUT_DIRECTORY = Build/NativeLogApps
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS = RELEASE|DEBUG
  SKUID_IDENTIFIER = DEFAULT
[LibraryClasses]
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0
[Components]
  J716fNativeLog/Read.inf
  J716fNativeLog/Arm.inf
'''

QR_INPUTS = {
    'Library/QrEncoderLib/QrEncoderLib.c': '7b1781aba7728d31965c6da15eb16b920b9c98f610360f6608755fac88967d30',
    'Library/QrEncoderLib/QrEncoderTables.c': 'f341049e054b1a6ff2c8b17025f4d752ec26ab3a3915404df559f6135cb931b8',
    'Library/QrEncoderLib/QrEncoderTables.h': '348e12d952d169d8122b03b5bb301218a048ed892599d729e210ac70f9ec051b',
    'Library/QrEncoderLib/QrEncoderLib.inf': '9edb2316a28fa31608d37f8f759c4b27304e8f78fcc149a9b502d6d6eb07e97e',
    'Include/Library/QrEncoderLib.h': '86a14aa523088a35ebd3d145c2fed959275c32f5c51d8abd10e2edca7803be81',
}

def prepare(uefi,output,candidate,export_init=False):
    spec=importlib.util.spec_from_file_location('logger',ROOT/'scripts/prepare-native-logger.py')
    logger=importlib.util.module_from_spec(spec);spec.loader.exec_module(logger)
    actual,_,header=logger.identity(ROOT)
    if candidate!=actual:raise ValueError('Reader must match the selected logger source identity')
    output=Path(output).resolve()
    if output.exists():raise ValueError('Preserve existing reader build workspace')
    source=Path(uefi)/'Platforms/Lenovo/j716fPkg/Application/J716fVmReturnTrace'
    # Compare LF source identity across Windows and Linux checkouts.
    assets={n:(source/n).read_bytes().replace(b'\r\n',b'\n') for n in REUSED}
    for n,raw in assets.items():
        if hashlib.sha256(raw).hexdigest()!=REUSED[n]:
            raise ValueError('Unreviewed historical firmware helper: '+n)
    target=output/'J716fNativeLog'
    shutil.copytree(ROOT/'package/native-log/reader',target)
    for n in ('NativeLog.h','NativeFirmware.h','NativeExport.h','NativeSha256.h','NativeMaintenance.h'):
        shutil.copyfile(ROOT/'package/native-log'/n,target/n)
    for n,raw in assets.items():(target/n).write_bytes(raw)
    (target/'NativeCandidate.h').write_text(header,encoding='utf-8',newline='\n')
    dsc=DSC
    if export_init:
        dsc=dsc.replace('[LibraryClasses]\n','[LibraryClasses]\n  QrEncoderLib|MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.inf\n')
        dsc+='  J716fNativeLog/Export.inf\n  J716fNativeLog/Init.inf\n'
    # Independent firmware maintenance identity; never changes logger identity.
    profile_inputs={
        'format':'J716F2-single-invocation', 'candidate':candidate,
        'kernel_sha256':'150c5c5f13a4337eb05218232f3a157429f0974ba49f71b4627789497132d074',
        'adt_sha256':'806ad48c85881b51f6acbeeea7ff4ec065f9edbe32d90f4aa304b5046faa64a3',
        'sources':{p.name:hashlib.sha256(p.read_bytes().replace(b'\r\n',b'\n')).hexdigest()
                   for p in sorted(target.iterdir()) if p.is_file()},
        'host_sources':{n:hashlib.sha256((ROOT/'scripts'/n).read_bytes().replace(b'\r\n',b'\n')).hexdigest()
                        for n in ('native_maintenance.py','receive-native-console.py','build-native-payload.py')},
        'qr_inputs':QR_INPUTS,'dsc_sha256':hashlib.sha256(dsc.encode()).hexdigest()}
    profile=hashlib.sha256(json.dumps(profile_inputs,sort_keys=True,separators=(',',':')).encode()).hexdigest()
    (target/'NativeMaintenanceProfile.h').write_text(
        '#define JL_MAINTENANCE_PROFILE {'+','.join('0x'+profile[i:i+2] for i in range(0,64,2))+'}\n',
        encoding='ascii',newline='\n')
    (output/'NativeLogApps.dsc').write_text(dsc,encoding='utf-8',newline='\n')
    report={'candidate_sha256':candidate,'scope':'standalone apps only; no FD or boot image',
            'export_init':export_init,'apps':['Read','Arm']+(['Export','Init'] if export_init else []),
            'maintenance_profile':profile if export_init else None,
            'maintenance_profile_inputs':profile_inputs if export_init else None,
            'qr_inputs':QR_INPUTS if export_init else {},
            'qr_mu_commit':'b8d46c71610318e36a661819e1e3c0b8bdda413a' if export_init else None,
            'reused_source_sha256':REUSED,'hardware_validated':False,
            'sources':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in target.iterdir() if p.is_file()}}
    (output/'reader-source.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--uefi',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--candidate',required=True)
    p.add_argument('--export-init',action='store_true',help='Explicitly add RAW exporter and receipt-gated initializer')
    a=p.parse_args();print(json.dumps(prepare(a.uefi,a.output,a.candidate,a.export_init),indent=2))
