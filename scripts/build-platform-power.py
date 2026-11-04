#!/usr/bin/env python3
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path('.').resolve()
OUT = ROOT / 'build/sheng-platform-power'
OUT.mkdir(parents=True, exist_ok=True)

for p in [ROOT / 'kits/XnuPrelink', Path('/home/test/tb-j716f-unixnu/sources/UniXNU/kits/XnuPrelink')]:
    if p.exists():
        sys.path.insert(0, str(p))
        break
from native_prelink import macho, backed_offset, exports

ORIGINAL_KERNELCACHE_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'

def run(command, **kwargs):
    return subprocess.check_output([str(v) for v in command], timeout=90, **kwargs)

def require(value, message):
    if not value:
        raise SystemExit(message)

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

reference = ROOT.parent / 'kernel/kernel.macho'
if not reference.is_file():
    stub = OUT / 'FirstPassStub.c'
    stub.write_text('// No reference kernel yet: the bridge needs one, so the first kernel links without it.\n'
                    'void sheng_pmgr_run(void);\nvoid sheng_pmgr_run(void)\n{\n}\n')
    run(['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64', '-mkernel', '-miphoneos-version-min=17.0', '-Os',
         '-c', stub, '-o', OUT / 'FirstPassStub.o'])
    (OUT / 'libShengPlatformPower.a').unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', OUT / 'libShengPlatformPower.a', OUT / 'FirstPassStub.o'])
    print('PMGR bridge: no reference kernel yet; linked a no-op sheng_pmgr_run. Build the kernel again to link the bridge.')
    raise SystemExit(0)

original = ROOT / '.cache/firmware/kernelcache.macho/kernelcache.release.ipad7.decompressed'
if not original.exists():
    cached = [path for path in (list(ROOT.parents[1].glob('ipsw/*/kernelcache.decompressed')) + list(Path('/home/test/tb-j716f-unixnu/sources/UniXNU/out/ipsw').glob('*/kernelcache.decompressed')))
              if hashlib.sha256(path.read_bytes()).hexdigest() == ORIGINAL_KERNELCACHE_SHA256]
    require(cached, 'The original kernelcache is not in the IPSW cache; run "unixnu ipsw fetch"')
    original.parent.mkdir(parents=True, exist_ok=True)
    original.symlink_to(cached[0])
print(run(['python3', ROOT / 'scripts/audit-amfi-pmgr-contract.py', '--candidate', reference], text=True), end='')
header = '''// Generated exact storage/slot ABI; private fields stay opaque.
#pragma once
#include <IOKit/IOPMGR.h>
class ApplePMGR : public IOPMGR {
 OSDeclareAbstractStructors(ApplePMGR)
 unsigned char originalPrivate[0x4b8a8-sizeof(IOPMGR)];
public:
'''
guards = ''
for i in range(177, 340):
    if i == 320:
        header += ' virtual uint32_t readAon();\n'
    elif i == 321:
        header += ' virtual void writeAon(uint64_t);\n'
    else:
        header += f' virtual void opaque{i}() = 0;\n'
        guards += f' void opaque{i}() override {{ unavailable({i}); }}\n'
header += '};\nstatic_assert(sizeof(ApplePMGR)==0x4b8a8,"Original provider storage");\n'
(OUT / 'OriginalPMGRABI.h').write_text(header)
(OUT / 'GuardedPMGRSlots.inc').write_text(guards)

sources = [p for p in [ROOT / 'kits/j716f/ShengPlatformPower.cpp', ROOT / 'package/ShengPlatformPower.cpp', Path('/home/test/tb-j716f-unixnu/sources/UniXNU-j716f/package/ShengPlatformPower.cpp')] if p.exists()]
require(len(sources) >= 1, 'Expected ShengPlatformPower.cpp')
source = sources[0]
obj = OUT / 'ShengPlatformPower.o'
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
command, i = [], 0
arguments = record['arguments']
while i < len(arguments):
    arg = arguments[i]
    if arg == record['file']:
        command.append(source)
    elif arg == '-o':
        command += ['-o', obj]
        i += 1
    elif arg == '-Xclang' and arguments[i + 1].startswith('-W'):
        i += 1
    elif not arg.startswith('-W'):
        command.append(arg)
    i += 1
command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
            '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
            '-Wno-unnecessary-virtual-specifier', '-Wno-missing-designated-field-initializers',
            '-Wno-unused-but-set-variable', '-Wno-unused-but-set-global',
            '-I', OUT, '-I', ROOT / '.cache/xnu-work/osfmk']
run(command, cwd=record['directory'])
raw = obj.read_bytes()
info = macho(raw)
require(info['kind'] == 1, 'Require actual ARM64 subtype-0 Mach-O object')
sym_at = next(at for cmd, at, _ in info['commands'] if cmd == 2)
at, count, strings, _ = struct.unpack_from('<4I', raw, sym_at + 8)
symbols = []
for i in range(count):
    ix, kind, _, _, address = struct.unpack_from('<IBBHQ', raw, at + i * 16)
    symbols.append((raw[strings + ix:raw.index(b'\0', strings + ix)].decode(), address, kind))
vtable = next(va for name, va, _ in symbols if name == '__ZTV18ShengPlatformPower')
relocations = {}
for segment in info['segments']:
    for section in segment['sections']:
        if section['va'] <= vtable < section['va'] + section['size']:
            at, count = struct.unpack_from('<2I', raw, section['command_offset'] + 56)
            for i in range(count):
                offset, bits = struct.unpack_from('<2I', raw, at + i * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[section['va'] + offset] = symbols[bits & 0xffffff][0]
entries = [relocations.get(vtable + 16 + i * 8) for i in range(340)]
require(all(entries) and vtable + 16 + 340 * 8 not in relocations, 'Exact 340-entry provider table')
candidate = reference.read_bytes()
meta = macho(candidate)
native_names = {n: va for va, names in exports(candidate, meta).items() for n in names}

def ptr(va):
    return struct.unpack_from('<Q', candidate, backed_offset(meta['segments'], va, 8, len(candidate)))[0]

bindings = {'__ZN9ApplePMGRC2EPK11OSMetaClass': 0xfffffff0049f003c,
            '__ZN9ApplePMGRD2Ev': 0xfffffff0049f019c,
            '__ZN9ApplePMGR10gMetaClassE': 0xfffffff006adf490}
for slot, name in enumerate(entries):
    if slot in (0, 1, 7) or 168 <= slot < 340 and slot not in (320, 321):
        require('18ShengPlatformPower' in name, f'Own guard/lifecycle required at slot {slot}')
    elif slot in (320, 321):
        require('9ApplePMGR' in name, 'Only actual original AON implementations inherited')
        bindings[name] = ptr(0xfffffff005ce26d8 + slot * 8)
    else:
        require(native_names.get(name) == ptr(native_names['__ZTV6IOPMGR'] + 16 + slot * 8),
                f'Native IOPMGR/IOService entry differs at {slot}')

undefined = {n for n, _, k in symbols if k == 1 and 'ApplePMGR' in n}
require(undefined == set(bindings), 'All and only audited original base imports')
bindings.update({'_sheng_original_pmgr_nub_create': 0xfffffff004a22568,
                 '_sheng_original_pmgr_nub_read': 0xfffffff004a24adc,
                 '_sheng_original_pmgr_nub_write': 0xfffffff004a230ec})
asm = OUT / 'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in bindings.items()))
sdk = run(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk, '-c', asm, '-o', OUT / 'original-bindings.o'])
late = OUT / 'ShengPlatformPower.late.o'
run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func', obj, late])
macho_globalize_symbol(late, '__GLOBAL__sub_I_ShengPlatformPower.cpp')

archive = OUT / 'libShengPlatformPower.a'
archive.unlink(missing_ok=True)
run(['xcrun', 'ar', 'rcs', archive, late, OUT / 'original-bindings.o'])
print('PASS: libShengPlatformPower.a built successfully!')
