#!/usr/bin/env python3
"""Build libAdreno650.a (NativeAdreno for SM8250 Adreno 650 + GMU + staged AGXAccelerator) for TB-J716F."""
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path('.').resolve()
PKG_REPO = next(
    (
        p for p in (
            ([Path(os.environ['UNIXNU_DEVICE'])] if os.environ.get('UNIXNU_DEVICE') else [])
            + [Path(__file__).resolve().parents[1], Path('/home/test/tb-j716f-unixnu/sources/UniXNU-j716f')]
        )
        if (p / 'board.toml').is_file()
    ),
    ROOT,
)
UNIXNU_REPO = next(
    (p for p in [ROOT, Path('/home/test/tb-j716f-unixnu/sources/UniXNU')] if (p / 'kits/AdrenoPM4').exists()),
    ROOT,
)

for p in [ROOT / 'kits/XnuPrelink', UNIXNU_REPO / 'kits/XnuPrelink']:
    if p.exists():
        sys.path.insert(0, str(p))
        break
from native_prelink import macho

def macho_globalize_symbol(path: Path, sym_name: str) -> bool:
    data = bytearray(path.read_bytes())
    _, _, _, _, ncmds, _, _, _ = struct.unpack_from('<8I', data, 0)
    at = 32
    symoff = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<2I', data, at)
        if cmd == 2:
            _, _, symoff, nsyms, stroff, strsize = struct.unpack_from('<6I', data, at)
            break
        at += cmdsize
    if symoff is None:
        return False
    strings = data[stroff:stroff + strsize]
    for i in range(nsyms):
        offset = symoff + i * 16
        n_strx, n_type = struct.unpack_from('<IB', data, offset)
        if n_strx < len(strings):
            end = strings.find(b'\0', n_strx)
            name = strings[n_strx:end].decode('latin1', errors='ignore')
            if name == sym_name:
                data[offset + 4] = n_type | 0x01
                path.write_bytes(data)
                return True
    return False

# Ensure symlinks/copies in build tree for kits and drivers
smmu_dir = PKG_REPO / 'kits/ArmSmmu4K' if (PKG_REPO / 'kits/ArmSmmu4K').exists() else (UNIXNU_REPO / 'kits/ArmSmmu4K')
pm4_dir = UNIXNU_REPO / 'kits/AdrenoPM4'
fw_dir = UNIXNU_REPO / 'kits/AdrenoFirmware'
kit_dir = ROOT / 'kits/j716f' if (ROOT / 'kits/j716f').exists() else (PKG_REPO / 'package')
driver_dir = PKG_REPO / 'drivers/Adreno650' if (PKG_REPO / 'drivers/Adreno650').exists() else (ROOT / 'drivers/Adreno650')
metal_dir = PKG_REPO / 'drivers/AdrenoMetal' if (PKG_REPO / 'drivers/AdrenoMetal').exists() else (UNIXNU_REPO / 'drivers/AdrenoMetal')

for extra in ('A650Gmu.c', 'A650Gmu.h', 'GpuPowerBoard.h'):
    src_f = (PKG_REPO / 'package' / extra)
    dst_f = (kit_dir / extra)
    if src_f.exists() and src_f.resolve() != dst_f.resolve():
        shutil.copy2(src_f, dst_f)

c_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
cxx_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))

out = ROOT / 'build/adreno650'
out.mkdir(parents=True, exist_ok=True)

sources = sorted(smmu_dir.glob('ArmSmmu4K*.c')) + [pm4_dir / 'AdrenoPM4.c']
sources += [fw_dir / f'{name}.c' for name in ('AdrenoFirmware', 'AdrenoHfi', 'AdrenoHfiStartup', 'AdrenoHfiVotes')]
sources += [kit_dir / 'A650Gmu.c']
sources += [driver_dir / f'{name}.cpp' for name in (
    'NativeAdrenoGpuContext', 'NativeAdrenoSmmuContext', 'NativeAdrenoSmmuMemory',
    'NativeAdrenoMemory', 'NativeAdrenoCpu', 'NativeAdrenoVm', 'NativeAdrenoRing',
    'NativeAdrenoSubmit', 'NativeAdrenoTimer', 'NativeAdrenoUserClient',
    'NativeAdrenoPublish', 'NativeAdrenoCpInit', 'NativeAdrenoIrq',
    'NativeAdrenoGmuContext', 'NativeAdrenoGmuMemory',
)]

includes = [
    '-I', str(pm4_dir),
    '-I', str(smmu_dir),
    '-I', str(fw_dir),
    '-I', str(kit_dir),
    '-I', str(ROOT / '.cache/xnu-work/osfmk'),
    '-I', str(driver_dir),
    '-I', str(metal_dir),
]

objects = []
for source in sources:
    recipe = c_recipe if source.suffix == '.c' else cxx_recipe
    obj = out / (source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
    command, i = [], 0
    while i < len(recipe['arguments']):
        value = recipe['arguments'][i]
        if value == recipe['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ'):
            i += 1
        elif value == '-Xclang' and recipe['arguments'][i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += includes + [
        '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-Wno-unnecessary-virtual-specifier',
    ]
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    if result.returncode:
        sys.exit(str(source) + ':\n' + result.stderr[-2500:])
    objects.append(obj)

# Build staged AGXAccelerator (NativeAdrenoAccelerator.late.o + original-bindings.o) for GpuProbe mode 12
plan_path = ROOT / 'build/gpu-analysis/ioaccelerator-base-abi.json'
recorded = next((p for p in [ROOT / 'scripts/data/ioaccelerator-base-abi.json', UNIXNU_REPO / 'scripts/data/ioaccelerator-base-abi.json'] if p.exists()), None)
if not plan_path.exists() and recorded is not None:
    plan_path.parent.mkdir(parents=True, exist_ok=True)
    plan_path.write_bytes(recorded.read_bytes())
if not plan_path.exists():
    sys.exit(f'Missing {plan_path}')
plan = json.loads(plan_path.read_text())

accel_src = driver_dir / 'NativeAdrenoAccelerator.cpp'
accel_obj = out / 'NativeAdrenoAccelerator.o'
accel_cmd, i = [], 0
while i < len(cxx_recipe['arguments']):
    v = cxx_recipe['arguments'][i]
    if v == cxx_recipe['file']:
        accel_cmd.append(str(accel_src))
    elif v == '-o':
        accel_cmd += ['-o', str(accel_obj)]
        i += 1
    elif v in ('-MF', '-MJ'):
        i += 1
    elif v == '-Xclang' and cxx_recipe['arguments'][i + 1].startswith('-W'):
        i += 1
    elif not v.startswith('-W') and v not in ('-MD', '-MP'):
        accel_cmd.append(v)
    i += 1
accel_cmd += includes + [
    '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier',
    '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-overloaded-virtual',
]
subprocess.run(accel_cmd, cwd=cxx_recipe['directory'], check=True)

bindings = {
    '__ZN15IOAcceleratorESC2EPK11OSMetaClass': int(plan['protected_compatible_constructor_aliases'][0], 16),
    '__ZN15IOAcceleratorESD2Ev': int(plan['nondeleting_destructor'], 16),
    '__ZN15IOAcceleratorES10gMetaClassE': int(plan['metaclass'], 16),
}
asm = out / 'original-bindings.S'
asm.write_text(''.join(f'.globl {n}\n.set {n}, {a:#x}\n' for n, a in sorted(bindings.items())))
sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
bindings_obj = out / 'original-bindings.o'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', str(asm), '-o', str(bindings_obj)], check=True)

accel_late = out / 'NativeAdrenoAccelerator.late.o'
subprocess.run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', str(accel_obj), str(accel_late)], check=True)
if not macho_globalize_symbol(accel_late, '__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp'):
    sys.exit('Failed to globalize __GLOBAL__sub_I_NativeAdrenoAccelerator.cpp')

objects.extend([accel_late, bindings_obj])

archive = out / 'libAdreno650.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *[str(o) for o in objects]], check=True)

compat_dir = ROOT / 'build/adreno740'
compat_dir.mkdir(parents=True, exist_ok=True)
shutil.copy2(archive, compat_dir / 'libAdreno740.a')

nm = subprocess.run(['xcrun', 'nm', '-gP', str(archive)], capture_output=True, text=True, check=True).stdout.splitlines()
defined = {f.split()[0] for f in nm if len(f.split()) > 1 and f.split()[1] in 'TSDBC'}
undefined = {f.split()[0] for f in nm if len(f.split()) > 1 and f.split()[1] == 'U'}
missing = sorted(s for s in undefined - defined if re.search(r'NativeAdreno|native_adreno|ArmSmmu4K|AdrenoPM4|AdrenoHfi|AdrenoFirmware|A650Gmu|Sheng', s))
if missing:
    sys.exit('NativeAdreno symbols used but not built here: ' + ', '.join(missing))
required = (
    '_NativeAdrenoGpuContextBringup', '_NativeAdrenoGpuContextBuffers', '_NativeAdrenoGpuContextVm',
    '_native_adreno_publish', '_ArmSmmu4KAttachStreams', '_AdrenoPM4Type7',
    '_NativeAdrenoGmuContextBringupRoutes', '_NativeAdrenoGmuContextPayloads',
    '_A650GmuColdStart', '_A650GmuBuildStartup',
    '__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp', '_native_adreno_accelerator_allocate',
)
absent = [s for s in required if s not in defined]
if absent:
    sys.exit('Required definitions absent: ' + ', '.join(absent))
print(f'PASS: {len(objects)} NativeAdreno a650 objects archived into {archive} ({archive.stat().st_size} bytes)')
