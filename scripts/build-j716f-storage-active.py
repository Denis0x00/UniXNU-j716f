#!/usr/bin/env python3
"""Build the TB-J716F read-only SHNGROOT window IOBlockStorageDevice archive."""
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tomllib

sys.dont_write_bytecode = True
from kernel_target import ARCH, IOS_VERSION, TRIPLE, check_object

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = 'socs/sm8250/kits/Sm8250Platform'
STORAGE_REL = 'kits/j716f/storage'

PLATFORM_SOURCES = tuple(
    PLATFORM + '/' + name + '.cpp'
    for name in (
        'Sm8250UfsRuntime',
        'Sm8250UfsHci',
        'Sm8250UfsHandoff',
        'Sm8250UfsDiagnostics',
        'AppsSmmuOwner',
    )
)
STORAGE_SOURCES = tuple(
    STORAGE_REL + '/' + name + '.cpp'
    for name in (
        'J716fStorageBringup',
        'J716fStorageKernel',
        'J716fStoragePower',
        'J716fStoragePowerKernel',
    )
)

OWNER_EXPORTS = tuple(
    '_apps_smmu_owner_' + name
    for name in (
        'install_handoff',
        'try_acquire',
        'validate',
        'validate_ready',
        'publication_begin',
        'publication_complete',
        'queue_started',
        'mark_dma_possible',
        'complete_dma',
        'confirm_stopped',
        'confirm_unpublished',
        'quarantine',
        'release',
    )
)
RUNTIME_EXPORTS = tuple(
    '_sm8250_ufs_' + name
    for name in (
        'adapter_init',
        'allocate',
        'activate',
        'read_known',
        'stop',
        'discard_unused',
        'status',
        'observe',
    )
)
BRINGUP_EXPORTS = tuple(
    '_j716f_storage_bringup_' + name
    for name in (
        'init',
        'status',
        'observe',
        'allocate',
        'activate',
        'read_known',
        'stop',
        'discard_unused',
    )
) + (
    '_j716f_storage_power_probe',
    '_j716f_r41_post_root_probe',
    '_j716f_r41_storage_parent',
    '_j716f_r41_base_class_size',
    '_j716f_r41_create_window_device',
    '_j716f_r41_window_stats',
    '_native_verified_block_root_authorized',
    '_native_verified_block_manifest_sha384',
    '_native_verified_block_volume_device',
    '_native_ram_block_volume_seed',
    '_j716f_ufs_rtc_load',
    '_j716f_ufs_rtc_save',
    '_j716f_skip_setup_plist',
    '__GLOBAL__sub_I_IOBlockStorageDevice.cpp',
    '__GLOBAL__sub_I_R41RootWindowDevice.cpp',
    '__GLOBAL__sub_I_NativeRamBlock.cpp',
)


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path: Path, value) -> None:
    path.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')


def command_for(record, source, output, include_dirs, allow_warnings=False):
    arguments = record['arguments']
    command, index = [], 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
        elif value == '-o':
            command.extend(['-o', str(output)])
            index += 1
        elif value in ('-MF', '-MJ', '-MT', '-MQ'):
            index += 1
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            index += 1
        elif value.startswith('-W'):
            pass
        else:
            command.append(value)
        index += 1
    extra = [
        '-Wall',
        '-Wextra',
        '-Wno-unused-parameter',
        '-Wno-c99-designator',
        '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion',
        '-Wno-unnecessary-virtual-specifier',
        '-Wno-missing-designated-field-initializers',
        '-mgeneral-regs-only',
        '-fstack-usage',
    ]
    if not allow_warnings:
        extra += ['-Werror', '-Wframe-larger-than=4096']
    for directory in include_dirs:
        extra += ['-I', str(directory)]
    command += extra
    return command


def run(command, cwd, log):
    result = subprocess.run(
        [str(arg) for arg in command],
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    log.write_text(result.stdout, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(
            'Command failed (' + str(result.returncode) + '): ' + str(log) + '\n' + result.stdout[-5000:]
        )
    return result.stdout


def inspect_object(obj: Path, check_stack=True, allow_mod_init=False):
    data = obj.read_bytes()
    check_object(data, obj.stem)
    count = struct.unpack_from('<I', data, 16)[0]
    offset, sections = 32, []
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:
            section_count = struct.unpack_from('<I', data, offset + 64)[0]
            for index in range(section_count):
                at = offset + 72 + index * 80
                name = data[at : at + 16].split(b'\0', 1)[0].decode()
                sections.append(name)
                if not allow_mod_init and name in ('__mod_init_func', '__mod_term_func'):
                    raise ValueError('Dynamic global initialization/termination section: ' + str(obj))
        offset += size
    max_stack = 0
    if check_stack:
        usage = obj.with_suffix('.su')
        if usage.name.endswith('.late.su'):
            usage = obj.with_name(obj.name.replace('.late.o', '.su'))
        if not usage.is_file():
            raise ValueError('Missing stack-usage report: ' + str(usage))
        stack = []
        for line in usage.read_text(encoding='utf-8').splitlines():
            fields = line.rsplit('\t', 2)
            if len(fields) == 3 and fields[1].isdigit():
                stack.append({'function': fields[0], 'bytes': int(fields[1]), 'kind': fields[2]})
        if not stack:
            raise ValueError('Empty stack-usage report: ' + str(usage))
        max_stack = max(row['bytes'] for row in stack)
    return {
        'sha256': digest(obj),
        'bytes': obj.stat().st_size,
        'sections': sections,
        'dynamic_global_initializers': False,
        'max_reported_stack_bytes': max_stack,
    }


def compile_late_init(record, source: Path, out_dir: Path, include_dirs, allow_warnings=False) -> Path:
    sym = f'__GLOBAL__sub_I_{source.name}'
    raw_s = out_dir / (source.stem + '.s')
    late_s = out_dir / (source.stem + '.late.s')
    late_obj = out_dir / (source.stem + '.late.o')

    cmd_s = command_for(record, source, raw_s, include_dirs, allow_warnings=allow_warnings)
    cmd_s = ['-S' if arg == '-c' else arg for arg in cmd_s]
    run(cmd_s, Path(record['directory']).resolve(), out_dir / (source.stem + '-compile.log'))

    lines = raw_s.read_text(encoding='utf-8').splitlines()
    out_lines = []
    in_mod_init = False
    removed_refs = 0
    globalized = 0
    for line in lines:
        stripped = line.strip()
        if stripped.startswith('.section') and '__mod_init_func' in stripped:
            in_mod_init = True
            continue
        if in_mod_init:
            if stripped.startswith(('.section', '.subsections_via_symbols', '.text', '.data', '.const')):
                in_mod_init = False
            else:
                if sym in stripped:
                    removed_refs += 1
                continue
        if stripped.startswith(f'{sym}:'):
            out_lines.append(f'\t.globl\t{sym}')
            globalized += 1
        out_lines.append(line)

    if globalized != 1 or removed_refs != 1:
        raise ValueError(
            f'Late-init rewrite failed for {source.name}: globalized={globalized} removed_refs={removed_refs}'
        )
    late_s.write_text('\n'.join(out_lines) + '\n', encoding='utf-8')

    asm_cmd = [
        'xcrun',
        'clang',
        '-target',
        TRIPLE,
        '-arch',
        ARCH,
        f'-miphoneos-version-min={IOS_VERSION}',
        '-c',
        str(late_s),
        '-o',
        str(late_obj),
    ]
    run(asm_cmd, Path(record['directory']).resolve(), out_dir / (source.stem + '-assemble.log'))
    return late_obj


def main():
    if ARCH != 'arm64' or IOS_VERSION != '17.0':
        raise ValueError('Requires ordinary arm64 / iOS 17.0')
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    board = tomllib.loads((device / 'board.toml').read_text(encoding='utf-8'))
    if board.get('board', {}).get('name') != 'j716f':
        raise ValueError('Expected board.name == "j716f", got: ' + repr(board.get('board', {}).get('name')))

    record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(record_path.read_text(encoding='utf-8').strip().rstrip(','))
    record_directory = Path(record['directory']).resolve()
    out = ROOT / 'build/j716f-storage-active'
    out.mkdir(parents=True, exist_ok=True)
    storage_inc = ROOT / 'build/storage-abi/include'
    if not (storage_inc / 'IOKit/storage/IOBlockStorageDevice.h').is_file():
        raise ValueError('Missing prepared IOStorageFamily headers at ' + str(storage_inc))

    storage_dir = ROOT / STORAGE_REL
    if not storage_dir.is_dir():
        raise ValueError('Missing assembled device storage package at ' + str(storage_dir))

    nm = shutil.which('llvm-nm')
    if not nm:
        raise ValueError('llvm-nm is required')

    includes = [
        device / PLATFORM,
        storage_dir / 'include',
        ROOT / '.cache/xnu-work/osfmk',
        storage_dir,
        storage_dir / 'r41',
        storage_inc,
    ]
    report = {
        'schema_version': 2,
        'passed': False,
        'target': str(device),
        'triple': TRIPLE,
        'tree': str(ROOT),
        'objects': {},
    }
    objects = []

    for relative in PLATFORM_SOURCES:
        source = device / relative
        obj = out / (source.stem + '.o')
        cmd = command_for(record, source, obj, includes, allow_warnings=False)
        run(cmd, record_directory, out / (source.stem + '-compile.log'))
        report['objects'][obj.name] = inspect_object(obj, check_stack=True, allow_mod_init=False)
        objects.append(obj)

    for relative in STORAGE_SOURCES:
        source = ROOT / relative
        obj = out / (source.stem + '.o')
        cmd = command_for(record, source, obj, includes, allow_warnings=False)
        run(cmd, record_directory, out / (source.stem + '-compile.log'))
        report['objects'][obj.name] = inspect_object(obj, check_stack=True, allow_mod_init=False)
        objects.append(obj)

    orig_base = ROOT / 'refer/IOStorageFamily/IOBlockStorageDevice.cpp'
    base_txt = orig_base.read_text(encoding='utf-8')
    macro = 'OSDefineMetaClassAndAbstractStructors(IOBlockStorageDevice,IOService)'
    if base_txt.count(macro) != 1:
        raise ValueError('Pinned IOBlockStorageDevice registration macro changed')
    patched_base = out / 'IOBlockStorageDevice.cpp'
    patched_base.write_text(
        base_txt.replace(
            macro,
            '''
extern "C" const OSMetaClass *j716f_r41_storage_parent(void);
OSDefineMetaClassWithInit(IOBlockStorageDevice, IOService, )
IOBlockStorageDevice::MetaClass::MetaClass()
    : OSMetaClass("UniXNUBlockStorageDevice", j716f_r41_storage_parent(), sizeof(IOBlockStorageDevice)) {}
OSDefineAbstractStructors(IOBlockStorageDevice, IOService)
OSDefineOperatorMethods(IOBlockStorageDevice)
''',
        ),
        encoding='utf-8',
    )
    base_late_obj = compile_late_init(record, patched_base, out, includes, allow_warnings=True)
    report['objects'][base_late_obj.name] = inspect_object(base_late_obj, check_stack=False, allow_mod_init=False)
    objects.append(base_late_obj)

    blk_src = storage_dir / 'r41/R41RootWindowDevice.cpp'
    blk_late_obj = compile_late_init(record, blk_src, out, includes, allow_warnings=False)
    report['objects'][blk_late_obj.name] = inspect_object(blk_late_obj, check_stack=True, allow_mod_init=False)
    objects.append(blk_late_obj)

    ram_src = ROOT / 'drivers/RamBlock/NativeRamBlock.cpp'
    if not ram_src.is_file():
        raise ValueError('Missing RamBlock component source at ' + str(ram_src))
    # Upstream NativeRamBlock::attachStore() uses a 4240-byte stack frame; match
    # official build-arm-driver.py / build-sheng-source-storage.py warning flags.
    ram_late_obj = compile_late_init(record, ram_src, out, includes + [ram_src.parent], allow_warnings=True)
    report['objects'][ram_late_obj.name] = inspect_object(ram_late_obj, check_stack=False, allow_mod_init=False)
    objects.append(ram_late_obj)

    archive = out / 'libJ716fStorageBringup.a'
    temporary = out / 'libJ716fStorageBringup.new.a'
    temporary.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', temporary, *objects], ROOT, out / 'archive.log')

    counts = Counter()
    for obj in objects:
        defined = run(
            [nm, '--defined-only', '--extern-only', '--format=posix', obj],
            ROOT,
            out / (obj.stem + '-defined-symbols.log'),
        )
        counts.update(line.split()[0] for line in defined.splitlines() if line.strip())
    for symbol in OWNER_EXPORTS + RUNTIME_EXPORTS + BRINGUP_EXPORTS:
        if counts[symbol] != 1:
            raise ValueError('Expected one definition of ' + symbol + ', found ' + str(counts[symbol]))

    temporary.replace(archive)
    report.update(
        passed=True,
        archive=str(archive),
        archive_sha256=digest(archive),
        archive_members=[obj.name for obj in objects],
        required_defined_symbols={n: counts[n] for n in OWNER_EXPORTS + RUNTIME_EXPORTS + BRINGUP_EXPORTS},
    )
    write_json(out / 'result.json', report)
    print('PASS: TB-J716F read-only APFS root window archive (11 objects, zero __mod_init_func)')


if __name__ == '__main__':
    main()
