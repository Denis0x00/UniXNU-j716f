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

def prepare(uefi,output,candidate):
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
    for n in ('NativeLog.h','NativeFirmware.h'):
        shutil.copyfile(ROOT/'package/native-log'/n,target/n)
    for n,raw in assets.items():(target/n).write_bytes(raw)
    (target/'NativeCandidate.h').write_text(header,encoding='utf-8',newline='\n')
    (output/'NativeLogApps.dsc').write_text(DSC,encoding='utf-8',newline='\n')
    report={'candidate_sha256':candidate,'scope':'two apps only; no FD or boot image',
            'reused_source_sha256':REUSED,'hardware_validated':False,
            'sources':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in target.iterdir() if p.is_file()}}
    (output/'reader-source.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    return report

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--uefi',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);p.add_argument('--candidate',required=True)
    a=p.parse_args();print(json.dumps(prepare(a.uefi,a.output,a.candidate),indent=2))
