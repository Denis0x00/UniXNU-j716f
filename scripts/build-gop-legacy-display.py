#!/usr/bin/env python3
"""Build NativeLegacyDisplay, NativeGOPPresenter, AppleKeyStoreState, ShengHidInput, and Buttons for J716F."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import shutil

ROOT = Path('.').resolve()
OUT = ROOT / 'build/native-legacy-display'
OUT.mkdir(parents=True, exist_ok=True)

for p in [ROOT / 'kits/XnuPrelink', Path('/home/test/tb-j716f-unixnu/sources/UniXNU/kits/XnuPrelink')]:
    if p.exists():
        sys.path.insert(0, str(p))
        break
from native_prelink import macho, backed_offset, exports

def run(cmd, **kw):
    return subprocess.check_output([str(x) for x in cmd], timeout=90, **kw)

def require(v, why):
    if not v:
        raise SystemExit(why)

def sha(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()

def macho_globalize_symbol(path: Path, sym_name: str):
    data = bytearray(path.read_bytes())
    magic, cputype, cpusubtype, filetype, ncmds, sizeofcmds, flags, reserved = struct.unpack_from('<8I', data, 0)
    at = 32
    symoff = None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from('<2I', data, at)
        if cmd == 2:
            _, _, symoff, nsyms, stroff, strsize = struct.unpack_from('<6I', data, at)
            break
        at += cmdsize
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

board_kernel = ROOT.parent / 'kernel/kernel.macho'
require(board_kernel.exists(), f"Missing board kernel {board_kernel}")

# Ensure prelink candidate and audit inputs exist and match current kernel
candidate_dir = ROOT / 'build/xnu-prelink'
candidate_dir.mkdir(parents=True, exist_ok=True)
candidate = candidate_dir / 'native-owner-ufs-cpu-desktop-j716f-candidate.macho'
shutil.copyfile(board_kernel, candidate)
shutil.copyfile(board_kernel.with_suffix('.json'), candidate.with_suffix('.json'))

disp_analysis = ROOT / 'build/display-analysis'
disp_analysis.mkdir(parents=True, exist_ok=True)
abi_file = disp_analysis / 'mobile-graphics-legacy-abi.json'
if not abi_file.exists():
    abi_file.write_text(json.dumps({'runtime_metadata_checkpoint_sha256': '3044c2beff552911605994c0dc7aa73182066cbb5aed0b396999ceeb5a96da2b'}))

print(run(['python3', ROOT / 'scripts/audit-mobile-graphics-legacy.py'], text=True), end='')
audit = json.loads(abi_file.read_text())
pure = audit['abstract_slots']
methods = audit['methods']

header = '''// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <IOKit/IOService.h>
#include <libkern/libkern.h>
struct OriginalLegacySwapRequest;
struct OriginalLegacyStateRequest {uint32_t skipRequested,requested,skipPowered,powered;uint8_t changed;uint8_t pad[11];};
static constexpr unsigned kLegacyVsyncClients=5592;
static constexpr unsigned kRequestRect0=17,kRequestRect1=81,kRequestMasks=792,kRequestOptions=804,kRequestDims=816,kRequestSurfaces=2624;
#define NATIVE_LEGACY_LEAVES(LEAF) LEAF(211) LEAF(245) LEAF(246) LEAF(306) LEAF(307) LEAF(338) LEAF(345)
class IOMobileFramebufferLegacy : public IOService {
 OSDeclareAbstractStructors(IOMobileFramebufferLegacy)
 unsigned char originalPrivate[0x18a0-sizeof(IOService)];
public:
 void installBootDimensions(uint32_t width,uint32_t height) {
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5808)=width;
  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this)+5812)=height;
 }
 const OriginalLegacySwapRequest *pendingSwapRequest() const {
  const OriginalLegacySwapRequest *request=nullptr;
  memcpy(&request,reinterpret_cast<const unsigned char *>(this)+5480,sizeof(request));return request;
 }
 bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {
  auto self=reinterpret_cast<unsigned char *>(this);
  auto bytes=reinterpret_cast<const unsigned char *>(request);
  const void *current=nullptr,*head=nullptr,*tail=nullptr,*next=nullptr,*previous=nullptr;
  memcpy(&current,self+5472,8);memcpy(&head,self+5480,8);memcpy(&tail,self+5488,8);
  if(!request || current || head!=request)return false;
  memcpy(&next,bytes+2960,8);memcpy(&previous,bytes+2968,8);
  if(previous!=self+5480)return false;
  if(next) {
   const void *link=self+5480;
   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next))+2968,&link,8);
  } else if(tail!=bytes+2960)return false;
  memcpy(self+5472,&request,8);
  if(next) {memcpy(self+5480,&next,8);}
  else {const void *empty=nullptr,*emptyTail=self+5480;memcpy(self+5480,&empty,8);memcpy(self+5488,&emptyTail,8);}
  return true;
 }
 void free() override;
 bool init(OSDictionary *properties=nullptr) override;
 IOReturn setProperties(OSObject *) override;
 void systemWillShutdown(IOOptionBits) override;
 IOService *probe(IOService *,SInt32 *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
 IOReturn setPowerState(unsigned long,IOService *) override;
'''

for i in range(168, 358):
    if i == 184:
        header += ' virtual bool isMainDisplay();\n'
    elif i == 188:
        header += ' virtual IOReturn getDotPitch(uint32_t *);\n'
    elif i == 213:
        header += ' virtual IOReturn supportedFrameInfo(uint32_t,uint32_t *,char *,uint32_t *)=0;\n'
    elif i == 249:
        header += ' virtual void getDisplayDimensions(uint32_t *,uint32_t *);\n'
    elif i == 290:
        header += ' virtual void kickSwapQueue();\n'
    elif i == 296:
        header += ' virtual bool validateSwap(const OriginalLegacySwapRequest *);\n'
    elif i == 301:
        header += ' virtual bool getRefreshAttributes(uint64_t *,bool);\n'
    elif i == 295:
        header += ' virtual void completeCurrentSwap();\n'
    elif i == 337:
        header += ' virtual IOReturn collectCompletionReports(uint32_t,void *,uint32_t *,uint32_t)=0;\n'
    elif i == 288:
        header += ' virtual bool startHardware();\n'
    elif i == 354:
        header += ' virtual int applyStateRequest(OriginalLegacyStateRequest *,bool);\n'
    elif i in pure:
        header += f' virtual __attribute__((noreturn)) void opaque{i}()=0;\n'
    else:
        header += f' virtual __attribute__((deprecated("Unclassified ABI: never call from source"))) void opaque{i}();\n'

header += '};\nstatic_assert(sizeof(IOMobileFramebufferLegacy)==0x18a0,"Actual original instance extent");\n'
(OUT / 'OriginalLegacyFramebufferABI.h').write_text(header)

print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeLegacyDisplay'], text=True), end='')
obj = OUT / 'NativeLegacyDisplay.o'
raw = obj.read_bytes()
image = macho(raw)
st = next(p for c, p, _ in image['commands'] if c == 2)
sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
symbols = []
for i in range(count):
    ix, kind, section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), va, kind))

vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
relocations = {}
for segment in image['segments']:
    for sec in segment['sections']:
        if sec['va'] <= vtable < sec['va'] + sec['size']:
            ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
            for i in range(rc):
                offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[sec['va'] + offset] = symbols[bits & 0xffffff][0]

compiled = [relocations.get(vtable + 16 + i * 8) for i in range(358)]
require(all(compiled) and vtable + 16 + 358 * 8 not in relocations, 'Exact owned 358-entry vtable')

old = candidate.read_bytes()
old_image = macho(old)
names = {n: v for v, group in exports(old, old_image).items() for n in group}

bindings = {
    '__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': 0xfffffff0057865cc,
    '__ZN25IOMobileFramebufferLegacyD2Ev': 0xfffffff005786618,
    '__ZN25IOMobileFramebufferLegacy10gMetaClassE': 0xfffffff006b35940,
    '__ZTV25IOMobileFramebufferLegacy': int(audit['legacy_vptr'], 16) - 16
}

owned = []
native = []
for i, name in enumerate(compiled):
    expected = int(methods[i]['address'], 16)
    if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
        owned.append(i)
        require(i in (0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301) or i in pure, 'Only qualified own methods and guarded leaves override')
    elif '25IOMobileFramebufferLegacy' in name:
        require(i not in pure, 'No unresolved original abstract backend target')
        bindings[name] = expected
    else:
        require(i < 168 and names.get(name) == expected, 'Actual inherited native base symbol ' + str(i))
        native.append(i)

require(owned == sorted([0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301] + pure), 'Exact owned method set')

for name, address, field, wide in [
    ('descriptor', 0xfffffff004b9ead0, 48, True), ('width', 0xfffffff004b9ebbc, 88, True),
    ('height', 0xfffffff004b9ebc4, 96, True), ('stride', 0xfffffff004b9ebd4, 104, True),
    ('allocation', 0xfffffff004b9ebfc, 144, True), ('format', 0xfffffff004b9ebac, 128, False),
    ('planes', 0xfffffff004b9ebf4, 152, False)]:
    bindings['_native_legacy_surface_' + name] = address

bindings['_native_legacy_vsync_notify'] = 0xfffffff005796720
bindings['_native_legacy_set_refresh_properties'] = 0xfffffff00579ebb8

asm = OUT / 'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(bindings.items())))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', asm, '-o', OUT / 'original-bindings.o'])

late = OUT / 'NativeLegacyDisplay.late.o'
run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', obj, late])
require(macho_globalize_symbol(late, '__GLOBAL__sub_I_NativeLegacyDisplay.cpp'), 'Globalized NativeLegacyDisplay initializer')

def code(data, info):
    sec = next(s for g in info['segments'] for s in g['sections'] if s['name'] == '__text')
    return data[sec['fileoff']:sec['fileoff'] + sec['size']]

require(code(raw, image) == code(late.read_bytes(), macho(late.read_bytes())), 'Only owned initializer section registration changes')

r = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
cmd = []
args = r['arguments']
i = 0
source = next(ROOT.glob('kits/*/LegacyDisplayProbe.cpp'))
while i < len(args):
    value = args[i]
    if value == r['file']:
        cmd.append(str(source))
    elif value == '-o':
        cmd += ['-o', str(OUT / 'LegacyDisplayProbe.o')]
        i += 1
    elif value == '-Xclang' and args[i + 1].startswith('-W'):
        i += 1
    elif not value.startswith('-W'):
        cmd.append(value)
    i += 1

cmd += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion', '-Wno-missing-designated-field-initializers', '-Wno-unused-but-set-variable', '-Wno-unused-but-set-global', '-I', str(ROOT / '.cache/xnu-work/osfmk')]
run(cmd, cwd=r['directory'])

keystore_source = next(ROOT.glob('kits/*/KeyStoreProbe.cpp'))
keystore_obj = OUT / 'KeyStoreProbe.o'
run([str(keystore_source) if str(arg) == str(source) else str(keystore_obj) if str(arg) == str(OUT / 'LegacyDisplayProbe.o') else arg for arg in cmd], cwd=r['directory'])

hid_source = next(ROOT.glob('kits/*/HidInputProbe.cpp'))
hid_obj = OUT / 'HidInputProbe.o'
run([str(hid_source) if str(arg) == str(source) else str(hid_obj) if str(arg) == str(OUT / 'LegacyDisplayProbe.o') else arg for arg in cmd], cwd=r['directory'])

touch_source = next(ROOT.glob('kits/*/TouchState.cpp'))
touch_obj = OUT / 'TouchState.o'
run([str(touch_source) if str(arg) == str(source) else str(touch_obj) if str(arg) == str(OUT / 'LegacyDisplayProbe.o') else arg for arg in cmd], cwd=r['directory'])

kit_dir = touch_source.parent
pkg_repo = next(
    (
        p for p in (
            ([Path(os.environ['UNIXNU_DEVICE'])] if os.environ.get('UNIXNU_DEVICE') else [])
            + [Path(__file__).resolve().parents[1], Path('/home/test/tb-j716f-unixnu/sources/UniXNU-j716f')]
        )
        if (p / 'board.toml').is_file()
    ),
    kit_dir.parent,
)
pkg_fallback = pkg_repo / 'package' if (pkg_repo / 'package').exists() else kit_dir
unixnu_repo = next((p for p in [ROOT, Path('/home/test/tb-j716f-unixnu/sources/UniXNU')] if (p / 'kits/AdrenoPM4').exists()), ROOT)

for extra_name in (
    'TouchBoard.h', 'TouchProbe.cpp', 'QcomRpmh.h', 'QcomRpmh.c', 'SmmuBypass.h',
    'UsbDebugCdc.h', 'UsbDebugWire.h', 'UsbDebugCommands.h', 'UsbDebugHw.h', 'UsbDebugCdc.cpp',
    'GpuPowerBoard.h', 'A650Gmu.h', 'A650Gmu.c', 'GpuProbe.cpp', 'MdssProbe.cpp',
    'NativeBatteryBoard.h', 'NativeBattery.cpp',
):
    dst_f = kit_dir / extra_name
    src_f = pkg_fallback / extra_name
    if src_f.exists() and src_f.resolve() != dst_f.resolve():
        shutil.copy2(src_f, dst_f)

touch_probe_source = kit_dir / 'TouchProbe.cpp'
touch_probe_obj = OUT / 'TouchProbe.o'
run([str(touch_probe_source) if str(arg) == str(source) else str(touch_probe_obj) if str(arg) == str(OUT / 'LegacyDisplayProbe.o') else arg for arg in cmd] + ['-I', str(kit_dir)], cwd=r['directory'])

usb_debug_source = kit_dir / 'UsbDebugCdc.cpp'
usb_debug_obj = OUT / 'UsbDebugCdc.o'
run([str(usb_debug_source) if str(arg) == str(source) else str(usb_debug_obj) if str(arg) == str(OUT / 'LegacyDisplayProbe.o') else arg for arg in cmd] + ['-I', str(kit_dir)], cwd=r['directory'])

smmu_dir = pkg_repo / 'kits/ArmSmmu4K' if (pkg_repo / 'kits/ArmSmmu4K').exists() else (unixnu_repo / 'kits/ArmSmmu4K')
adreno_dir = pkg_repo / 'drivers/Adreno650' if (pkg_repo / 'drivers/Adreno650').exists() else (ROOT / 'drivers/Adreno650')
gpu_includes = [
    '-I', str(kit_dir),
    '-I', str(adreno_dir),
    '-I', str(smmu_dir),
    '-I', str(unixnu_repo / 'kits/AdrenoFirmware'),
    '-I', str(unixnu_repo / 'kits/AdrenoPM4'),
]
gpu_source = kit_dir / 'GpuProbe.cpp'
gpu_obj = OUT / 'GpuProbe.o'
run([str(gpu_source) if str(arg) == str(source) else str(gpu_obj) if str(arg) == str(OUT / 'LegacyDisplayProbe.o') else arg for arg in cmd] + gpu_includes, cwd=r['directory'])

mdss_source = kit_dir / 'MdssProbe.cpp'
mdss_obj = OUT / 'MdssProbe.o'
run([str(mdss_source) if str(arg) == str(source) else str(mdss_obj) if str(arg) == str(OUT / 'LegacyDisplayProbe.o') else arg for arg in cmd] + ['-I', str(kit_dir)], cwd=r['directory'])

battery_source = kit_dir / 'NativeBattery.cpp'
battery_obj = OUT / 'NativeBattery.o'
battery_late = OUT / 'NativeBattery.late.o'
run([str(battery_source) if str(arg) == str(source) else str(battery_obj) if str(arg) == str(OUT / 'LegacyDisplayProbe.o') else arg for arg in cmd] + ['-I', str(kit_dir)], cwd=r['directory'])
run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', battery_obj, battery_late])
require(macho_globalize_symbol(battery_late, '__GLOBAL__sub_I_NativeBattery.cpp'), 'Globalized NativeBattery initializer')

def find_fw(env_key, rel_paths):
    if os.environ.get(env_key):
        return Path(os.environ[env_key])
    for base in (ROOT, pkg_repo):
        for rel in rel_paths:
            cand = base / rel
            if cand.is_file():
                return cand
    return ROOT / rel_paths[0]

gmu_firmware = find_fw('PIPA_GMU_FIRMWARE', ('.cache/firmware/gpu/a650_gmu.bin', 'firmware/gpu/a650_gmu.bin'))
require(gmu_firmware.is_file(), f'Missing GMU firmware {gmu_firmware}')
gmu_blob = gmu_firmware.read_bytes()
require(len(gmu_blob) >= 16 and len(gmu_blob) % 4 == 0, f'{gmu_firmware} is not a GMU block image')
gmu_c = OUT / 'GmuFirmware.c'
gmu_c.write_text(
    f'// {gmu_firmware.name}, sha256 {hashlib.sha256(gmu_blob).hexdigest()}\n#include <stdint.h>\n'
    'const uint8_t native_gpu_gmu_firmware[] __attribute__((aligned(16))) = {' + ','.join(map(str, gmu_blob)) + '};\n'
    f'const uint32_t native_gpu_gmu_firmware_size = {len(gmu_blob)};\n'
)

sqe_firmware = find_fw('PIPA_SQE_FIRMWARE', ('.cache/firmware/gpu/a650_sqe.fw', '.cache/firmware/linux-firmware/a650_sqe.fw', 'firmware/gpu/a650_sqe.fw'))
require(sqe_firmware.is_file(), f'Missing SQE firmware {sqe_firmware}')
sqe_blob = sqe_firmware.read_bytes()
require(len(sqe_blob) >= 8 and len(sqe_blob) % 4 == 0, f'{sqe_firmware} is not an SQE image')
sqe_c = OUT / 'SqeFirmware.c'
sqe_c.write_text(
    f'// {sqe_firmware.name}, sha256 {hashlib.sha256(sqe_blob).hexdigest()}\n#include <stdint.h>\n'
    'const uint8_t native_gpu_sqe_firmware[] __attribute__((aligned(16))) = {' + ','.join(map(str, sqe_blob)) + '};\n'
    f'const uint32_t native_gpu_sqe_firmware_size = {len(sqe_blob)};\n'
)

zap_firmware = find_fw('PIPA_ZAP_FIRMWARE', ('.cache/firmware/gpu/a650_zap.elf', 'firmware/gpu/a650_zap.elf'))
require(zap_firmware.is_file(), f'Missing zap shader {zap_firmware}')
zap_blob = zap_firmware.read_bytes()
require(len(zap_blob) >= 52 and zap_blob[:5] == b'\x7fELF\x01', f'{zap_firmware} is not a 32-bit ELF')
zap_c = OUT / 'ZapFirmware.c'
zap_c.write_text(
    f'// {zap_firmware.name}, sha256 {hashlib.sha256(zap_blob).hexdigest()}\n#include <stdint.h>\n'
    'const uint8_t native_gpu_zap_firmware[] __attribute__((aligned(16))) = {' + ','.join(map(str, zap_blob)) + '};\n'
    f'const uint32_t native_gpu_zap_firmware_size = {len(zap_blob)};\n'
)

c_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
buttons_source = next(ROOT.glob('kits/*/Buttons.c'))
buttons_obj = OUT / 'Buttons.o'
c_out = []
c_args = c_record['arguments']
j = 0
while j < len(c_args):
    val = c_args[j]
    if val == c_record['file']:
        c_out.append(str(buttons_source))
    elif val in ('-o', '-MF', '-MT', '-MJ'):
        if val == '-o':
            c_out += ['-o', str(buttons_obj)]
        j += 1
    elif val == '-Xclang' and c_args[j + 1].startswith('-W'):
        j += 1
    elif not val.startswith('-W'):
        c_out.append(val)
    j += 1
run(c_out + ['-Wall', '-Wextra', '-Werror', '-I', str(buttons_source.parent)], cwd=c_record['directory'])

def compile_c_obj(src_c: Path, dst_o: Path):
    run([str(src_c) if str(arg) == str(buttons_source) else str(dst_o) if str(arg) == str(buttons_obj) else arg for arg in c_out] + ['-Wall', '-Wextra', '-Werror', '-I', str(kit_dir)], cwd=c_record['directory'])

rpmh_source = kit_dir / 'QcomRpmh.c'
rpmh_obj = OUT / 'QcomRpmh.o'
compile_c_obj(rpmh_source, rpmh_obj)

gmu_fw_obj = OUT / 'GmuFirmware.o'
sqe_fw_obj = OUT / 'SqeFirmware.o'
zap_fw_obj = OUT / 'ZapFirmware.o'
compile_c_obj(gmu_c, gmu_fw_obj)
compile_c_obj(sqe_c, sqe_fw_obj)
compile_c_obj(zap_c, zap_fw_obj)

print(run(['python3', ROOT / 'scripts/build-sheng-hid-input.py'], text=True), end='')
hid_dir = ROOT / 'build/sheng-hid-input'

print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'AppleKeyStoreState'], text=True), end='')
keyevents = ROOT / 'build/apple-key-store-state'
keyevents_late = keyevents / 'NativeKeyStoreEvents.late.o'
run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', keyevents / 'NativeKeyStoreEvents.o', keyevents_late])
require(macho_globalize_symbol(keyevents_late, '__GLOBAL__sub_I_NativeKeyStoreEvents.cpp'), 'Globalized NativeKeyStoreEvents initializer')

print(run(['python3', ROOT / 'scripts/build-arm-driver.py', 'NativeGOPPresenter'], text=True), end='')
gop = ROOT / 'build/native-gop-presenter'
gop_late = gop / 'NativeGOPPresenter.late.o'
run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', gop / 'NativeGOPPresenter.o', gop_late])
require(macho_globalize_symbol(gop_late, '__GLOBAL__sub_I_NativeGOPPresenter.cpp'), 'Globalized NativeGOPPresenter initializer')

archive = OUT / 'libNativeLegacyDisplay.late.a'
archive.unlink(missing_ok=True)

run([
    'xcrun', 'ar', 'rcs', archive,
    late,
    OUT / 'original-bindings.o',
    OUT / 'LegacyDisplayProbe.o',
    keystore_obj,
    keyevents_late,
    hid_obj,
    touch_obj,
    touch_probe_obj,
    usb_debug_obj,
    gpu_obj,
    mdss_obj,
    battery_late,
    rpmh_obj,
    buttons_obj,
    gmu_fw_obj,
    sqe_fw_obj,
    zap_fw_obj,
    hid_dir / 'NativeHidInput.late.o',
    hid_dir / 'hid-original-bindings.o',
    gop_late,
])

sources = [
    ROOT / 'drivers/GopFramebuffer/NativeLegacyDisplay.cpp',
    ROOT / 'drivers/GopFramebuffer/NativeLegacyRequest.h',
    source,
    keystore_source,
    ROOT / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp',
    hid_source,
    touch_source,
    touch_probe_source,
    usb_debug_source,
    kit_dir / 'UsbDebugCdc.h',
    kit_dir / 'UsbDebugWire.h',
    kit_dir / 'UsbDebugCommands.h',
    kit_dir / 'UsbDebugHw.h',
    kit_dir / 'TouchBoard.h',
    kit_dir / 'QcomRpmh.h',
    rpmh_source,
    kit_dir / 'SmmuBypass.h',
    gpu_source,
    kit_dir / 'GpuPowerBoard.h',
    kit_dir / 'A650Gmu.h',
    mdss_source,
    battery_source,
    kit_dir / 'NativeBatteryBoard.h',
    buttons_source,
    buttons_source.parent / 'ButtonsCore.h',
    ROOT / 'drivers/ShengHidInput/NativeHidInput.cpp',
    ROOT / 'scripts/build-sheng-hid-input.py',
    ROOT / 'drivers/GopFramebuffer/NativeGOPPresenter.cpp',
    ROOT / 'drivers/GopFramebuffer/NativeGOPWire.h',
]
report = {
    'source_sha256': {str(p.relative_to(ROOT)): sha(p) for p in sources},
    'archive_sha256': sha(archive),
    'generated_abi_sha256': sha(OUT / 'OriginalLegacyFramebufferABI.h'),
    'original_candidate_sha256': sha(candidate),
    'original_audit_sha256': sha(ROOT / 'build/display-analysis/mobile-graphics-legacy-abi.json'),
    'compiled_object_sha256': sha(obj),
    'late_object_sha256': sha(late),
    'bindings': {n: hex(v) for n, v in bindings.items()},
    'virtual_entries': 358,
    'owned_entries': owned,
    'inherited_native_entries': native,
}
(OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')

print(f"\nPASS: Built {archive} and build-report.json successfully! Size: {archive.stat().st_size} bytes")
