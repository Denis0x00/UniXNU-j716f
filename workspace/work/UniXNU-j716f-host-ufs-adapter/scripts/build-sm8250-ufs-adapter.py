#!/usr/bin/env python3
"""Build the frozen SM8250 adapter using this official tree's fresh XNU ABI."""
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
from kernel_target import ARCH, IOS_VERSION, TRIPLE, check_object

ROOT = Path(__file__).resolve().parents[1]
PLATFORM = 'socs/sm8250/kits/Sm8250Platform'
SOURCES = tuple(PLATFORM + '/' + name + '.cpp' for name in (
    'Sm8250UfsRuntime', 'Sm8250UfsHci', 'Sm8250UfsHandoff', 'AppsSmmuOwner'))
OWNER_EXPORTS = tuple('_apps_smmu_owner_' + name for name in (
    'install_handoff', 'try_acquire', 'validate', 'validate_ready',
    'publication_begin', 'publication_complete', 'queue_started',
    'mark_dma_possible', 'complete_dma', 'confirm_stopped',
    'confirm_unpublished', 'quarantine', 'release'))
RUNTIME_EXPORTS = tuple('_sm8250_ufs_' + name for name in (
    'adapter_init', 'allocate', 'activate', 'read_known', 'stop', 'discard_unused', 'status'))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def frozen_inputs(device):
    path = device / 'docs/ufs-adapter-inputs.json'
    manifest = json.loads(path.read_text())
    if manifest.get('schema_version') != 1 or manifest.get('translation_units') != list(SOURCES):
        raise ValueError('Expected frozen adapter manifest with exactly four translation units')
    entries = manifest['files']
    required = set(SOURCES) | {
        'scripts/build-sm8250-ufs-adapter.py', 'board.toml',
        'configs/j716f_host_ufs_adapter_defconfig',
        'patches/xnu/sm8250-ufs-adapter-init.patch',
        'patches/xnu/sm8250-ufs-adapter-dependency.patch',
        'drivers/ShengUfs/RestrictedUfs.h', 'drivers/ShengUfs/ReadCommandPolicy.h',
        'drivers/ShengUfs/UfsPlatform.h', PLATFORM + '/AppsSmmuOwner.h',
    }
    if not required <= set(entries):
        raise ValueError('Incomplete frozen adapter manifest: ' + repr(sorted(required - set(entries))))
    seen = {}
    for relative, expected in entries.items():
        rel = Path(relative)
        file = device / rel
        if rel.is_absolute() or '..' in rel.parts or not file.resolve().is_relative_to(device):
            raise ValueError('Frozen input escapes this target: ' + relative)
        if file.is_symlink() or not file.is_file():
            raise ValueError('Frozen input must be a regular file: ' + relative)
        actual = {'bytes': file.stat().st_size, 'sha256': digest(file)}
        if actual != expected:
            raise ValueError('Frozen input mismatch: ' + relative)
        seen[relative] = actual
    # Every compile source and project header must be covered, including headers
    # reached by quoted includes. Shared SDK/XNU headers belong to the fresh tree.
    for folder in (device / PLATFORM, device / 'drivers/ShengUfs'):
        for file in folder.rglob('*'):
            if file.is_file() and file.suffix in ('.h', '.hpp', '.c', '.cpp'):
                relative = file.relative_to(device).as_posix()
                if relative not in entries:
                    raise ValueError('Unfrozen local source/header: ' + relative)
    if digest(Path(__file__)) != entries['scripts/build-sm8250-ufs-adapter.py']['sha256']:
        raise ValueError('Resolved-tree recipe differs from the frozen device script')
    return {'manifest_sha256': digest(path), 'files': seen}


def command_for(record, source, output, include_dirs):
    arguments = record['arguments']
    if arguments.count(record['file']) != 1 or arguments.count('-o') != 1 or arguments.count('-c') != 1:
        raise ValueError('Unexpected actual XNU C++ compile record')
    command, changes, index = [], [], 0
    while index < len(arguments):
        value = arguments[index]
        if value == record['file']:
            command.append(str(source))
            changes.append({'kind': 'source', 'before': value, 'after': str(source)})
        elif value == '-o':
            command.extend(['-o', str(output)])
            changes.append({'kind': 'object', 'before': arguments[index + 1], 'after': str(output)})
            index += 1
        elif value in ('-MF', '-MJ', '-MT', '-MQ'):
            # Never overwrite the kernel's own dependency/compile records.
            changes.append({'kind': 'drop_diagnostic_output', 'arguments': arguments[index:index + 2]})
            index += 1
        elif value == '-Xclang' and index + 1 < len(arguments) and arguments[index + 1].startswith('-W'):
            changes.append({'kind': 'replace_warning_policy', 'arguments': arguments[index:index + 2]})
            index += 1
        elif value.startswith('-W'):
            changes.append({'kind': 'replace_warning_policy', 'arguments': [value]})
        else:
            command.append(value)
        index += 1
    extra = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
             '-Wno-c99-designator', '-Wno-reorder-init-list',
             '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
             '-Wno-missing-designated-field-initializers', '-Wframe-larger-than=4096',
             '-mgeneral-regs-only', '-fstack-usage']
    for directory in include_dirs:
        extra += ['-I', str(directory)]
    command += extra
    return command, {'replacements': changes, 'additional_arguments': extra}


def run(command, cwd, log):
    result = subprocess.run([str(arg) for arg in command], cwd=cwd,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    log.write_text(result.stdout)
    if result.returncode:
        raise RuntimeError('Command failed (' + str(result.returncode) + '): ' + str(log) + '\n' + result.stdout[-5000:])
    return result.stdout


def inspect_object(obj):
    """No dynamic global constructor may run before the explicit init caller."""
    data = obj.read_bytes()
    check_object(data, obj.stem)
    count = struct.unpack_from('<I', data, 16)[0]
    offset, sections = 32, []
    for _ in range(count):
        command, size = struct.unpack_from('<II', data, offset)
        if command == 0x19:  # LC_SEGMENT_64
            section_count = struct.unpack_from('<I', data, offset + 64)[0]
            for index in range(section_count):
                at = offset + 72 + index * 80
                name = data[at:at + 16].split(b'\0', 1)[0].decode()
                sections.append(name)
                if name in ('__mod_init_func', '__mod_term_func'):
                    raise ValueError('Dynamic global initialization/termination section: ' + str(obj))
        offset += size
    usage = obj.with_suffix('.su')
    if not usage.is_file():
        raise ValueError('Missing actual compiler stack-usage report: ' + str(usage))
    stack = []
    for line in usage.read_text().splitlines():
        fields = line.rsplit('\t', 2)
        if len(fields) != 3 or not fields[1].isdigit():
            raise ValueError('Malformed stack-usage entry: ' + line)
        stack.append({'function': fields[0], 'bytes': int(fields[1]), 'kind': fields[2]})
    if not stack:
        raise ValueError('Empty stack-usage report: ' + str(usage))
    return {'sha256': digest(obj), 'bytes': obj.stat().st_size,
            'sections': sections, 'dynamic_global_initializers': False,
            'stack_usage': str(usage), 'stack_usage_sha256': digest(usage),
            'max_reported_stack_bytes': max(row['bytes'] for row in stack),
            'functions': stack}


def main():
    if ARCH != 'arm64' or IOS_VERSION != '17.0':
        raise ValueError('This frozen target requires ordinary arm64 / iOS 17.0 deployment')
    if not os.environ.get('UNIXNU_DEVICE'):
        raise ValueError('Run this recipe via the official target build kernel command')
    device = Path(os.environ['UNIXNU_DEVICE']).resolve()
    if device.name != 'UniXNU-j716f-host-ufs-adapter':
        raise ValueError('Wrong independent device target')
    before = frozen_inputs(device)
    record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(record_path.read_text().strip().rstrip(','))
    record_directory = Path(record['directory']).resolve()
    if not record_directory.is_relative_to(ROOT):
        raise ValueError('Compile record is not from this fresh official tree')
    record_sha = digest(record_path)
    out = ROOT / 'build/sm8250-ufs-adapter'
    out.mkdir(parents=True, exist_ok=True)
    report = {'schema_version': 1, 'passed': False, 'target': str(device),
              'triple': TRIPLE, 'tree': str(ROOT), 'fresh_compile_record': str(record_path),
              'compile_record_sha256': record_sha, 'inputs_before': before,
              'translation_units': list(SOURCES), 'unique_owner_translation_unit': True,
              'native_ufs_cpp_compiled': False, 'hardware_qualified': False, 'objects': {}}
    write_json(out / 'result.json', report)
    objects = []
    includes = [device / PLATFORM, device / 'drivers/ShengUfs', ROOT / '.cache/xnu-work/osfmk']
    for relative in SOURCES:
        source = device / relative
        obj = out / (source.stem + '.o')
        command, changes = command_for(record, source, obj, includes)
        write_json(out / (source.stem + '-compile-command.json'), command)
        write_json(out / (source.stem + '-compile-changes.json'), changes)
        run(command, record_directory, out / (source.stem + '-compile.log'))
        report['objects'][obj.name] = inspect_object(obj)
        objects.append(obj)
    archive = out / 'libSm8250UfsAdapter.a'
    temporary = out / 'libSm8250UfsAdapter.new.a'
    temporary.unlink(missing_ok=True)
    run(['xcrun', 'ar', 'rcs', temporary, *objects], ROOT, out / 'archive.log')
    listed = run(['xcrun', 'ar', 't', temporary], ROOT, out / 'archive-members.log').splitlines()
    # Darwin ar includes its linker symbol index in `ar t`. It is archive
    # metadata, not a fifth object. Accept at most one recognized index and
    # still require the exact four ordered object members without duplicates.
    indexes = [name for name in listed if name in ('__.SYMDEF', '__.SYMDEF SORTED')]
    if len(indexes) > 1:
        raise ValueError('Archive contains duplicate symbol indexes')
    members = [name for name in listed if name not in indexes]
    if members != [obj.name for obj in objects]:
        raise ValueError('Archive must contain exactly the four new adapter objects')
    nm = shutil.which('llvm-nm')
    if not nm:
        raise ValueError('llvm-nm is required for unique native-symbol validation')
    counts = Counter()
    for obj in objects:
        all_defined = run([nm, '--defined-only', '--format=posix', obj],
                          ROOT, out / (obj.stem + '-all-defined-symbols.log'))
        if any(marker in all_defined for marker in ('_GLOBAL__sub_I', '__cxx_global_var_init')):
            raise ValueError('Dynamic global initializer symbol: ' + str(obj))
        defined = run([nm, '--defined-only', '--extern-only', '--format=posix', obj],
                      ROOT, out / (obj.stem + '-defined-symbols.log'))
        counts.update(line.split()[0] for line in defined.splitlines() if line.strip())
    for symbol in OWNER_EXPORTS + RUNTIME_EXPORTS:
        if counts[symbol] != 1:
            raise ValueError('Expected one definition of ' + symbol + ', found ' + str(counts[symbol]))
    owner_names = {name for name in counts if name.startswith('_apps_smmu_owner_')}
    if owner_names != set(OWNER_EXPORTS):
        raise ValueError('Unexpected owner C exports')
    report['required_defined_symbols'] = {name: counts[name] for name in OWNER_EXPORTS + RUNTIME_EXPORTS}
    after = frozen_inputs(device)
    if before != after or record_sha != digest(record_path):
        raise ValueError('Source inputs or original compile record changed during the build')
    temporary.replace(archive)
    report.update(passed=True, inputs_after=after, archive=str(archive),
                  archive_listing=listed, archive_symbol_indexes=indexes,
                  archive_sha256=digest(archive), archive_members=members,
                  next_stage='Official native kernel link, then mandatory prelink')
    write_json(out / 'result.json', report)
    print('PASS: frozen SM8250 adapter, four fresh ' + TRIPLE + ' objects, one owner; native link/prelink follow')


if __name__ == '__main__':
    main()
