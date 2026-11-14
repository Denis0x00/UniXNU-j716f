#!/usr/bin/env python3
"""Read-only validation of the independent TB-J716F r31 Power-only host rebuild.

Only --report is written. The official XnuPrelink readers are imported with
bytecode disabled; no planner, emitter, compiler or device command is invoked.
Success establishes the checks listed in the report, not runtime ABI or boot.
"""
import argparse
from bisect import bisect_right
from collections import Counter
import copy
import datetime
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tomllib

sys.dont_write_bytecode = True

MAIN_REVISION = '7b730b231746667d8b7f54746c2c1ce45852611c'
PRELINK_REVISION = '440589b0c31625f17edf363e24b3c82c51c70bb9'
ORIGINAL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
PROFILE = 'ipados-17.7.11-21H461'
SLIDE = -0x1000000
SOURCE_MANIFEST_SHA256 = '111d6e1e7fb61af73bc431a18af8988ced14fd2a94bb10187426abeea93b563f'
PHYSICAL_MANIFEST_SHA256 = 'c79857285fffb06f18a949051ebe702710a7dbef82c6a943ae9b21a1b74a2a62'
PHYSICAL_LINE_ENDING_COUNT = 60
LINE_ENDING_CONTRACT_SHA256 = 'aadc7796e324e42020032d69a278d6bc4093b5499b89bd9b8c5775de6a555ffc'
EXTRA_ARCHIVE = '-Wl,-force_load,{tree}/build/j716f-ufs-power-r31/libJ716fStorageBringup.a'
STORAGE_PATCHES = (('j716f-storage-init.patch', '62b6135a95f434d549feb7897feb6641ccef23e02831b11b396a8030186d5691'), ('j716f-storage-dependency.patch', 'db1caedcab6c9dc5b453d86ad654df872c19d5348daa4b56877f7721be5a24c7'), ('j716f-power-probe-call.patch', '167403de0e4512c2a7f42e776d5cfe99f22a4be21c3c546ba44a2b0fcf9b83f6'))
PLATFORM = 'socs/sm8250/kits/Sm8250Platform'
STORAGE_SOURCES = tuple(PLATFORM + '/' + name + '.cpp' for name in (
    'Sm8250UfsRuntime', 'Sm8250UfsHci', 'Sm8250UfsHandoff', 'Sm8250UfsDiagnostics', 'AppsSmmuOwner')) + (
    'component:J716fStorageBringup/J716fStorageBringup.cpp',
    'component:J716fStorageBringup/J716fStorageKernel.cpp',
    'component:J716fStorageBringup/J716fStoragePower.cpp',
    'component:J716fStorageBringup/J716fStoragePowerKernel.cpp',
)
OWNER_EXPORTS = tuple('_apps_smmu_owner_' + name for name in (
    'install_handoff', 'try_acquire', 'validate', 'validate_ready', 'publication_begin',
    'publication_complete', 'queue_started', 'mark_dma_possible', 'complete_dma',
    'confirm_stopped', 'confirm_unpublished', 'quarantine', 'release'))
RUNTIME_EXPORTS = tuple('_sm8250_ufs_' + name for name in (
    'adapter_init', 'allocate', 'activate', 'read_known', 'stop', 'discard_unused', 'status', 'observe'))
BRINGUP_EXPORTS = tuple('_j716f_storage_bringup_' + name for name in (
    'init', 'status', 'observe', 'allocate', 'activate', 'read_known', 'stop', 'discard_unused')) + ('_j716f_storage_power_probe',)
AMFI = 'com.apple.driver.AppleMobileFileIntegrity'
AMFI_ORIGINAL = 0xfffffff005ab4ae0
FALSE_FLAGS = (
    'images_modified', 'instruction_relocation_complete', 'abi_qualified',
    'loadable', 'register_dataflow_qualified', 'source_inputs_modified',
    'runtime_qualified',
)


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f'Duplicate JSON key: {key}')
            result[key] = value
        return result
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def effective_config(text):
    """Independently parse effective CONFIG values, allowing repeated fragment values."""
    values = {}
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        unset = re.fullmatch(r'#\s*CONFIG_(\w+) is not set', line)
        if unset:
            values[unset.group(1)] = False
        elif not line or line.startswith('#'):
            continue
        else:
            match = re.fullmatch(r'CONFIG_(\w+)=(.*)', line)
            require(match is not None, f'Invalid config line {number}')
            name, value = match.groups()
            if value in ('y', 'n'):
                values[name] = value == 'y'
            elif value.startswith('"') and value.endswith('"'):
                values[name] = value[1:-1]
            elif re.fullmatch(r'-?(0x[0-9a-fA-F]+|\d+)', value):
                values[name] = int(value, 16 if value.lstrip('-').startswith('0x') else 10)
            else:
                raise ValueError(f'Invalid config value on line {number}')
    return values


def compare_configuration(current_text, baseline_text):
    current, baseline = effective_config(current_text), effective_config(baseline_text)
    require(current.get('XNU_DT_PANIC_REGION') is True, 'Effective DT panic-region option is not enabled')
    require(baseline.get('XNU_DT_PANIC_REGION') is True, 'Retained CPU0 baseline lacks DT panic-region')
    require(current.get('XNU_NATIVE_UFS') is False, 'NativeUfs runtime unexpectedly selected')
    current_rest, baseline_rest = dict(current), dict(baseline)
    require(current_rest.pop('XNU_EXTRA_LD_ARCHIVES', None) == EXTRA_ARCHIVE, 'Unexpected explicit storage archive')
    require(baseline_rest.pop('XNU_EXTRA_LD_ARCHIVES', '') == '', 'CPU0 baseline already has extra archives')
    require(current_rest == baseline_rest, 'Effective configuration changed beyond the explicit storage archive')
    return {'only_effective_delta': {'XNU_EXTRA_LD_ARCHIVES': {'before': baseline.get('XNU_EXTRA_LD_ARCHIVES'), 'after': EXTRA_ARCHIVE}},
            'current_effective_keys': len(current), 'baseline_effective_keys': len(baseline)}


def check_source_manifest(root, source_manifest, manifest):
    """Pin the Windows source bytes and the separately reviewed physical LF copies."""
    require(digest(source_manifest.read_bytes()) == SOURCE_MANIFEST_SHA256, 'New target source manifest changed')
    require(manifest['upstream_revision'] == MAIN_REVISION, 'Storage source manifest upstream mismatch')
    physical_path = root/'analysis/r31-ufs-power-20261114/vm-source-identities.json'
    require(digest(physical_path.read_bytes()) == PHYSICAL_MANIFEST_SHA256, 'Pinned VM physical source identities changed')
    physical = read_json(physical_path)
    require(physical['schema_version'] == 1 and physical['source_manifest_sha256'] == SOURCE_MANIFEST_SHA256,
            'Physical identities belong to another source manifest')
    line_path = root/'analysis/r31-ufs-power-20261114/source-line-ending-contract.json'
    require(digest(line_path.read_bytes()) == LINE_ENDING_CONTRACT_SHA256, 'Reviewed Windows/LF source contract changed')
    line_contract = read_json(line_path)
    require(line_contract['source_manifest_sha256'] == SOURCE_MANIFEST_SHA256 and
            set(line_contract['inputs']) == set(manifest['inputs']), 'Line-ending contract belongs to another source closure')
    require(len(manifest['inputs']) == len(physical['inputs']) == 565 and
            set(manifest['inputs']) == set(physical['inputs']), 'Expected the same 565 frozen source inputs')
    require(type(PHYSICAL_LINE_ENDING_COUNT) is int and len(physical['crlf_lf_only']) == PHYSICAL_LINE_ENDING_COUNT, 'Physical line-ending review is pending or changed')
    require(all(isinstance(name, str) for name in physical['crlf_lf_only']), 'Physical differences must be path strings')
    differences = set(physical['crlf_lf_only'])
    require(len(differences) == PHYSICAL_LINE_ENDING_COUNT, 'Duplicate line-ending record')
    actual_differences = {name for name in manifest['inputs'] if manifest['inputs'][name] != physical['inputs'][name]}
    require(set(differences) == actual_differences, 'Unrecorded or spurious physical-source differences')
    for relative, expected in physical['inputs'].items():
        path = (root/relative).resolve(strict=True)
        require(path.is_relative_to(root) and path.is_file(), 'Source input escapes the workspace: ' + relative)
        content = path.read_bytes()
        actual = {'bytes': len(content), 'sha256': digest(content)}
        require(actual == expected, 'Actual VM source input differs: ' + relative)
        item = line_contract['inputs'][relative]
        normalized = content.replace(b'\r\n', b'\n')
        require(item['windows'] == manifest['inputs'][relative] and
                item['normalized_lf'] == {'bytes': len(normalized), 'sha256': digest(normalized)},
                'Actual source does not match independently reviewed LF bytes: ' + relative)
        if relative in differences:
            windows = normalized.replace(b'\n', b'\r\n')
            require(expected == item['normalized_lf'], 'Different physical source is not the exact LF counterpart: ' + relative)
            require({'bytes': len(windows), 'sha256': digest(windows)} == item['windows'],
                    'CRLF reconstruction does not reproduce exact Windows bytes: ' + relative)
    return {'source_manifest': str(source_manifest), 'source_manifest_sha256': SOURCE_MANIFEST_SHA256,
            'physical_manifest': str(physical_path), 'physical_manifest_sha256': PHYSICAL_MANIFEST_SHA256,
            'line_ending_contract_sha256': LINE_ENDING_CONTRACT_SHA256,
            'verified_inputs': 565, 'exact_crlf_lf_differences': PHYSICAL_LINE_ENDING_COUNT}


def defined_symbols(data, image):
    """Read individual N_SECT definitions, retaining repeated same-address names."""
    tables = [(offset, size) for command, offset, size in image['commands'] if command == 2]
    require(len(tables) == 1 and tables[0][1] == 24, 'Expected exactly one LC_SYMTAB')
    offset, count, strings, length = struct.unpack_from('<4I', data, tables[0][0] + 8)
    require(offset + 16 * count <= len(data) and strings + length <= len(data), 'Symbol table exceeds object/image')
    result = Counter()
    for index in range(count):
        name, kind, _, _, _ = struct.unpack_from('<IBBHQ', data, offset + index * 16)
        if kind & 0xe0 or kind & 0x0e != 0x0e:
            continue
        require(name < length, 'Symbol name offset is outside string table')
        end = data.find(b'\0', strings + name, strings + length)
        require(end >= 0, 'Unterminated defined symbol name')
        result[data[strings + name:end].decode()] += 1
    return result


def archive_members(data):
    """Read the actual Darwin/BSD ar members, including the independent index."""
    require(data[:8] == b'!<arch>\n', 'Storage archive has no regular ar header')
    result, cursor = [], 8
    while cursor < len(data):
        require(cursor + 60 <= len(data), 'Truncated ar header')
        header = data[cursor:cursor + 60]
        require(header[58:60] == b'`\n', 'Invalid ar member trailer')
        name = header[:16].decode('ascii').strip()
        size = int(header[48:58].decode('ascii').strip())
        begin, end = cursor + 60, cursor + 60 + size
        require(size >= 0 and end <= len(data), 'ar member exceeds archive')
        if name.startswith('#1/'):
            namesize = int(name[3:])
            require(0 < namesize <= size, 'Invalid BSD ar extended name')
            name = data[begin:begin + namesize].rstrip(b'\0').decode()
            begin += namesize
        elif name.endswith('/'):
            name = name[:-1]
        require(name not in ('', '/', '//') and '/' not in name, 'Unexpected ar member name')
        result.append((name, data[begin:end]))
        cursor = end + (size & 1)
    require(cursor == len(data), 'Invalid final ar member padding')
    return result


def storage_command(original, source, obj, include_dirs):
    """Allow only the explicitly reviewed changes to the fresh kernel command."""
    original = dict(original)
    argv = original['arguments']
    require(isinstance(argv, list) and all(isinstance(x, str) for x in argv), 'Invalid original C++ compile argv')
    require(argv.count(original['file']) == argv.count('-o') == argv.count('-c') == 1, 'Ambiguous kernel C++ compile record')
    expected, index = [], 0
    while index < len(argv):
        arg = argv[index]
        if arg == original['file']:
            expected.append(str(source))
        elif arg == '-o':
            require(index + 1 < len(argv), 'Missing original object operand')
            expected += ['-o', str(obj)]
            index += 1
        elif arg in ('-MF', '-MJ', '-MT', '-MQ'):
            require(index + 1 < len(argv), 'Missing original diagnostic operand')
            index += 1
        elif arg == '-Xclang' and index + 1 < len(argv) and argv[index + 1].startswith('-W'):
            index += 1
        elif not arg.startswith('-W'):
            expected.append(arg)
        index += 1
    expected += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter', '-Wno-c99-designator',
                 '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion', '-Wno-unnecessary-virtual-specifier',
                 '-Wno-missing-designated-field-initializers', '-Wframe-larger-than=4096', '-mgeneral-regs-only', '-fstack-usage']
    for directory in include_dirs:
        expected += ['-I', str(directory)]
    return expected


def check_storage_archive(root, target, tree, np, native, native_image):
    out = tree/'build/j716f-ufs-power-r31'
    report_path, archive = out/'result.json', out/'libJ716fStorageBringup.a'
    result = read_json(report_path)
    require(result['schema_version'] == 1 and result['passed'] is True, 'Storage archive recipe did not pass')
    require(Path(result['target']).resolve() == target.resolve() and Path(result['tree']).resolve() == tree.resolve(),
            'Storage build report belongs to another target/tree')
    require(result['triple'] == 'arm64-apple-ios17.0', 'Storage compile triple differs')
    require(result['translation_units'] == list(STORAGE_SOURCES), 'Storage archive is not the selected nine TUs')
    require(result['unique_owner_translation_unit'] is True and result['native_ufs_cpp_compiled'] is False and
            result['hardware_qualified'] is False, 'Unexpected storage qualification or full UFS driver selection')
    require(result['inputs_before'] == result['inputs_after'], 'Storage recipe source identities changed')
    frozen_path = target/'docs/storage-inputs.json'
    frozen = read_json(frozen_path)
    require(frozen['schema_version'] == 1 and frozen['translation_units'] == list(STORAGE_SOURCES) and
            frozen['native_ufs_cpp_compiled'] is False, 'Unexpected storage frozen-input contract')
    require(result['inputs_before']['manifest_sha256'] == digest(frozen_path.read_bytes()) and
            result['inputs_before']['files'] == frozen['files'] and
            result['inputs_before']['component_files'] == frozen['component_files'], 'Recipe input identities differ from frozen manifest')
    component = root/'work/J716fStorageBringup-r31'
    require((tree/'drivers/J716fStorageBringup').resolve() == component.resolve(), 'Storage component symlink points elsewhere')
    tracked = {}
    def track(path):
        path = Path(path)
        value = digest(path.read_bytes())
        tracked[str(path)] = value
        return value
    for base, rows in ((target, frozen['files']), (component, frozen['component_files'])):
        for name, expected in rows.items():
            path = (base/name).resolve(strict=True)
            require(path.is_relative_to(base.resolve()) and path.is_file(), 'Frozen storage source escapes root')
            require(path.stat().st_size == expected['bytes'] and track(path) == expected['sha256'], 'Frozen storage source differs: ' + name)
    for path in (report_path, frozen_path, archive):
        track(path)
    record_path = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    require(Path(result['fresh_compile_record']).resolve() == record_path.resolve() and
            result['compile_record_sha256'] == track(record_path), 'Storage reference compile record differs')
    original = json.loads(record_path.read_text().strip().rstrip(','))
    directory = Path(original['directory']).resolve()
    require(directory == record_path.parent.resolve(), 'Kernel C++ record directory is not its fresh object directory')
    original_source = Path(original['file'])
    original_source = (original_source if original_source.is_absolute() else directory/original_source).resolve()
    require(original_source == (tree/'.cache/xnu-work/iokit/Kernel/IOPlatformExpert.cpp').resolve(), 'Wrong original C++ source')
    includes = [target/PLATFORM, target/'drivers/ShengUfs', tree/'.cache/xnu-work/osfmk', tree/'drivers/J716fStorageBringup']
    expected_members = [Path(name).stem + '.o' for name in STORAGE_SOURCES]
    actual_members = archive_members(archive.read_bytes())
    indexes = [(name, data) for name, data in actual_members if name in ('__.SYMDEF', '__.SYMDEF SORTED')]
    objects = [(name, data) for name, data in actual_members if name not in ('__.SYMDEF', '__.SYMDEF SORTED')]
    require(len(indexes) <= 1 and [name for name, _ in objects] == expected_members, 'Actual archive members differ from the exact nine ordered objects')
    require(result['archive_listing'] == [name for name, _ in actual_members] and
            result['archive_members'] == expected_members and result['archive_symbol_indexes'] == [name for name, _ in indexes],
            'Reported ar inventory differs from actual archive bytes')
    require(Path(result['archive']).resolve() == archive.resolve() and result['archive_sha256'] == tracked[str(archive)], 'Storage archive report hash/path mismatch')
    require(set(result['objects']) == set(expected_members), 'Storage report object set differs')
    counts, object_reports = Counter(), {}
    for relative, (name, embedded) in zip(STORAGE_SOURCES, objects):
        path = out/name
        raw = path.read_bytes()
        require(raw == embedded, 'Archive member bytes differ from compiled object: ' + name)
        obj_report = result['objects'][name]
        require(len(raw) == obj_report['bytes'] and track(path) == obj_report['sha256'], 'Compiled object report mismatch: ' + name)
        parsed = np.macho(raw)
        require(parsed['kind'] == 1, 'Storage input is not MH_OBJECT: ' + name)
        deployments = [struct.unpack_from('<II', raw, offset + 8) for command, offset, _ in parsed['commands'] if command == 0x32]
        require(deployments == [(2, 17 << 16)], 'Storage object must have one iOS 17 deployment command: ' + name)
        sections = [s['name'] for segment in parsed['segments'] for s in segment['sections']]
        require(sections == obj_report['sections'] and not any(s in sections for s in ('__mod_init_func', '__mod_term_func')) and
                obj_report['dynamic_global_initializers'] is False, 'Dynamic global init or section report mismatch: ' + name)
        symbols = defined_symbols(raw, parsed)
        require(not any('_GLOBAL__sub_I' in symbol or '__cxx_global_var_init' in symbol for symbol in symbols), 'Unexpected dynamic global initializer symbol')
        counts.update(symbols)
        source = tree/'drivers'/relative.removeprefix('component:') if relative.startswith('component:') else target/relative
        command_path = out/(path.stem + '-compile-command.json')
        argv = read_json(command_path)
        require(argv == storage_command(original, source, path, includes), 'Storage command changed beyond reviewed ABI substitutions: ' + name)
        track(command_path)
        stack_path = path.with_suffix('.su')
        stack = []
        for line in stack_path.read_text().splitlines():
            fields = line.rsplit('\t', 2)
            require(len(fields) == 3 and fields[1].isdigit(), 'Malformed storage stack usage')
            stack.append({'function': fields[0], 'bytes': int(fields[1]), 'kind': fields[2]})
        require(bool(stack) and stack == obj_report['functions'] and
                max(row['bytes'] for row in stack) == obj_report['max_reported_stack_bytes'] <= 4096,
                'Storage stack usage is absent, differs, or exceeds 4096 bytes')
        require(Path(obj_report['stack_usage']).resolve() == stack_path.resolve() and
                obj_report['stack_usage_sha256'] == track(stack_path), 'Storage stack record path/hash mismatch')
        object_reports[name] = {'sha256': obj_report['sha256'], 'bytes': len(raw), 'max_reported_stack_bytes': obj_report['max_reported_stack_bytes']}
    required = OWNER_EXPORTS + RUNTIME_EXPORTS + BRINGUP_EXPORTS
    require(all(counts[name] == 1 for name in required), 'Nine objects do not define each required API exactly once')
    require({name for name in counts if name.startswith('_apps_smmu_owner_')} == set(OWNER_EXPORTS), 'Additional owner C exports appear in storage objects')
    require(result['required_defined_symbols'] == {name: 1 for name in required}, 'Recipe export inventory differs')
    owner_symbols = defined_symbols((out/'AppsSmmuOwner.o').read_bytes(), np.macho((out/'AppsSmmuOwner.o').read_bytes()))
    require(all(owner_symbols[name] == 1 for name in OWNER_EXPORTS), 'Shared owner definitions are not all in the one owner TU')
    native_counts = defined_symbols(native, native_image)
    require(all(native_counts[name] == 1 for name in required), 'Native kernel does not define each storage API exactly once')
    require({name for name in native_counts if name.startswith('_apps_smmu_owner_')} == set(OWNER_EXPORTS), 'Native kernel contains an additional owner implementation/API')
    return {'archive': str(archive), 'archive_sha256': result['archive_sha256'], 'translation_units': list(STORAGE_SOURCES),
            'objects': object_reports, 'single_owner_translation_unit': True, 'native_required_definitions': {name: native_counts[name] for name in required},
            'fresh_compile_record_sha256': result['compile_record_sha256'], 'verified_files': tracked,
            'controller_handoff_qualified': False, 'mmio_dma_qualified': False, 'block_device_published': False, 'runtime_verified': False}


def check_panic_compile_record(path, tree):
    """Clang -MJ writes one JSON object followed by a comma; validate that one command."""
    raw = path.read_text(encoding='utf-8').strip()
    if raw.endswith(','):
        raw = raw[:-1].rstrip()
    command = json.loads(raw)
    require(isinstance(command, dict), 'Expected exactly one clang compilation record')
    argv = command.get('arguments')
    require(isinstance(argv, list) and argv and all(isinstance(x, str) for x in argv), 'Compilation record lacks an arguments array')
    directory = Path(command['directory']).resolve()
    source = Path(command['file'])
    source = (source if source.is_absolute() else directory/source).resolve()
    output = Path(command['output'])
    output = (output if output.is_absolute() else directory/output).resolve()
    expected_source = (tree/'.cache/xnu-work/pexpert/arm/pe_init.c').resolve()
    expected_object = path.with_suffix('').resolve()
    require(source == expected_source and source.is_file(), 'pe_init record names a different source')
    require(directory == path.parent.resolve() and output == expected_object and output.is_file(), 'pe_init record names a different or missing object')
    definitions = []
    index = 0
    while index < len(argv):
        value = argv[index]
        if value in ('-D', '-U') and index + 1 < len(argv):
            name = argv[index + 1]
            if name.split('=', 1)[0] == 'XNU_DT_PANIC_REGION':
                require(value == '-D', 'Compilation undefines DT panic-region')
                definitions.append(name.split('=', 1)[1] if '=' in name else '1')
                index += 2
                continue
        if value.startswith('-DXNU_DT_PANIC_REGION'):
            name = value[2:]
            require(name.split('=', 1)[0] == 'XNU_DT_PANIC_REGION', 'Unexpected similarly named DT panic macro')
            definitions.append(name.split('=', 1)[1] if '=' in name else '1')
        elif 'XNU_DT_PANIC_REGION' in value:
            raise ValueError('Unrecognized or conflicting DT panic-region compiler argument')
        index += 1
    require(definitions == ['1'], 'pe_init.o must compile with exactly one DT panic-region definition of 1')
    return {'path': str(path), 'sha256': digest(path.read_bytes()), 'source': str(source),
            'source_sha256': digest(source.read_bytes()), 'object': str(output),
            'object_sha256': digest(output.read_bytes()), 'define': 'XNU_DT_PANIC_REGION=1'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True, help='VM workspace root')
    parser.add_argument('--report', type=Path, required=True, help='New output JSON; never overwrites an earlier report')
    args = parser.parse_args()
    root = args.root.resolve()
    report_path = args.report.resolve()
    main_repo = root / 'UniXNU'
    out = main_repo / 'out-host-ufs-power-r31-20261114/j716f-host-ufs-power-r31' / PROFILE / 'j716f_host_ufs_power_r31'
    tree = out / 'tree'
    status_path = root / 'analysis/r31-ufs-power-20261114/host-build/status.json'
    candidate_path = out / 'kernel/kernel.macho'
    emitted_report_path = candidate_path.with_suffix('.json')
    original_path = main_repo / 'out/ipsw/21H461/kernelcache.decompressed'
    profile_path = main_repo / 'profiles/os' / PROFILE / 'profile.toml'
    require(not report_path.exists(), f'Refusing to overwrite report: {report_path}')
    require(not report_path.is_relative_to(main_repo), 'The verification report must be outside the UniXNU checkout and its build outputs')
    require(report_path != status_path, 'The verification report cannot replace the official status input')
    record = {
        'schema_version': 1, 'started_utc': utc(), 'status': 'running',
        'scope': 'Read-only host candidate verification; no emission, compilation, runtime ABI qualification or device action',
        'root': str(root), 'report': str(report_path), 'checks': [],
        'device_actions': False, 'build_inputs_modified': False,
    }

    def passed(name, **details):
        record['checks'].append({'name': name, 'passed': True, **details})

    def revision(path):
        return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()

    try:
        component = main_repo / 'kits/XnuPrelink'
        require(revision(main_repo) == MAIN_REVISION, 'Unexpected UniXNU revision')
        require(revision(component) == PRELINK_REVISION, 'Unexpected XnuPrelink revision')
        sys.path.insert(0, str(component))
        import native_prelink as np
        require(Path(np.__file__).resolve() == (component / 'native_prelink.py').resolve(), 'Imported an unexpected XnuPrelink reader')
        record['sources'] = {
            'UniXNU': MAIN_REVISION, 'XnuPrelink': PRELINK_REVISION,
            'native_prelink.py_sha256': digest((component / 'native_prelink.py').read_bytes()),
            'emit_prelink.py_sha256': digest((component / 'emit_prelink.py').read_bytes()),
            'verifier_sha256': digest(Path(__file__).read_bytes()),
        }
        status = read_json(status_path)
        require(status['name'] == 'j716f-host-ufs-power-r31', 'Status belongs to another target')
        require(Path(status['output']).resolve() == out.resolve(), 'Status output differs from the isolated host target')
        require(status['board']['board']['example'] is False, 'Host status is still an example')
        require(status['board']['board']['os_profile'] == PROFILE, 'Unexpected target OS profile')
        baseline_status_path = root/'analysis/host-iokit-cpu0-20261114/status.json'
        baseline_status = read_json(baseline_status_path)
        config_path = Path(status['config_path']).resolve()
        require(config_path == (out/'.config').resolve(), 'Effective config is outside the new host output')
        baseline_config = Path(baseline_status['config_path']).resolve()
        require(baseline_config == (main_repo/'out-host-iokit-cpu0-20261114/j716f-host-iokit-cpu0'/PROFILE/'j716f_host_iokit_cpu0/.config').resolve(), 'Unexpected retained host config path')
        config_check = compare_configuration(config_path.read_text(), baseline_config.read_text())
        require(len(status['patch_order']) == 63 and len(baseline_status['patch_order']) == 60 and
                [digest(Path(p).read_bytes()) for p in status['patch_order'][:60]] ==
                [digest(Path(p).read_bytes()) for p in baseline_status['patch_order']], 'First 60 patches differ from accepted CPU0 baseline')
        target = root/'work/UniXNU-j716f-host-ufs-power-r31'
        require(digest(Path(status['patch_order'][58]).read_bytes()) == digest((target/'patches/xnu/j716f-verbose-text-console.patch').read_bytes()), 'Unexpected retained console patch')
        require(digest(Path(status['patch_order'][59]).read_bytes()) == 'b49a5a28d179cbf9dc35432f7f0b98906ea3d0e238f69a6f077bbcca4a548708', 'Unexpected retained early-log patch')
        for index, (name, expected) in enumerate(STORAGE_PATCHES, 60):
            require(digest(Path(status['patch_order'][index]).read_bytes()) == expected and
                    digest((target/'patches/xnu'/name).read_bytes()) == expected, 'Unexpected storage patch: ' + name)
        early_sources = {'osfmk/console/serial_console.c': 'ac84fa14135c1c920a12c4c67d2f607c6d537b737bebb06a1bc642e4863b784a', 'osfmk/kern/printf.c': 'f40cc49f6de5ec58cc94386923ae5ba91dcfdcbae801e11b4ba6bec3aaeed824', 'pexpert/arm/J716fEarlyLog.h': 'd871825f16e72b3923420b3c0aa54e5d5a2291e5c0272d582ce05f9a22a86d16', 'pexpert/arm/j716f_earlylog_guards.h': 'eabea360a6364c7030a336c7082ed0cd28815da0c113379421333f52094f0120', 'pexpert/arm/j716f_earlylog_impl.h': 'be6a873270dd9b8eb97713a9bfcd815529267fa6a72c7534eb4f08759e53d27a', 'pexpert/arm/pe_init.c': '40431ba0aebd1fe08ac3be516e1113655aad47fb2321864c41d40e2603147b84', 'pexpert/gen/pe_gen.c': 'adaa2c7fc77f1affce26aa5b74a2d1ac6d5bcf05145aae389d6394bd376997d2'}
        for name, expected in early_sources.items():
            require(digest((tree/'.cache/xnu-work'/name).read_bytes()) == expected, 'Compiled early-log source mismatch: ' + name)
        require(len(status['components']) == 5 and [c['id'] for c in status['components']] ==
                [c['id'] for c in baseline_status['components']], 'Component selection differs from retained five-component baseline')
        require(len(status['board']['kernel']['archives']) == 9 and status['board']['kernel']['archives'][:8] ==
                baseline_status['board']['kernel']['archives'] and status['board']['kernel']['archives'][8:] == ['build-j716f-ufs-power-r31.py'], 'Unexpected archive recipe extension')

        # Bind the source-only override and the compiler command to this target.
        target = root/'work/UniXNU-j716f-host-ufs-power-r31'
        armcpu = root/'work/ArmCpu-j716f-iokit-cpu0-20261114'
        source_manifest = target/'docs/host-source-manifest.json'
        manifest = read_json(source_manifest)
        record['source_manifest'] = check_source_manifest(root, source_manifest, manifest)
        passed('pinned_565_source_inputs_and_reviewed_line_endings', **record['source_manifest'])
        require((tree/'drivers/ArmCpu').resolve() == armcpu.resolve(), 'Compiled tree does not select isolated ArmCpu')
        armcpu_source = armcpu/'ArmCpu.cpp'
        require(digest(armcpu_source.read_bytes()) == 'f2e4ae0d47fdf838b21004f7e500ac3aa145f4197f2a4ee3bfad9909bdce09e2', 'Six-repair ArmCpu source mismatch')
        require(digest((armcpu/'Info.plist').read_bytes()) == '32fd6a027cc14b497544626f96ae104cd28af8aa9b9fcbde9ef138a4b3d70547', 'ArmCpu bundle identity changed')
        require(sorted(p.name for p in armcpu.iterdir()) == ['AGENTS.md', 'ArmCpu.cpp', 'Info.plist', 'LICENSE', 'README.md'], 'Unexpected extra ArmCpu source or diagnostic files')
        old_status = read_json(root/'analysis/host-early-log-20261114/status.json')
        early_config = effective_config(config_path.read_text())
        early_config.pop('XNU_EXTRA_LD_ARCHIVES', None)
        old_early_config = effective_config(Path(old_status['config_path']).read_text())
        require(old_early_config.pop('XNU_EXTRA_LD_ARCHIVES', '') == '', 'Accepted early-log already has extra archives')
        require(early_config == old_early_config, 'Effective config differs beyond storage archive from accepted earlylog target')
        require([digest(Path(p).read_bytes()) for p in status['patch_order'][:60]] == [digest(Path(p).read_bytes()) for p in old_status['patch_order']], 'XNU patches differ from accepted earlylog target')
        require(status['board']['kernel']['archives'][:8] == old_status['board']['kernel']['archives'], 'Retained archive recipes changed')
        cmd_path = tree/'build/armcpu/compile-command.json'
        argv = read_json(cmd_path)
        require(isinstance(argv, list) and all(isinstance(v, str) for v in argv), 'Expected ArmCpu command argv')
        expected_source = str(tree/'drivers/ArmCpu/ArmCpu.cpp')
        obj = tree/'build/armcpu/ArmCpu.o'
        archive = tree/'build/armcpu/libArmCpu.a'
        require(argv.count(expected_source) == 1 and argv.count('-o') == 1, 'ArmCpu command source/output is not unique')
        require(Path(argv[argv.index('-o')+1]).resolve() == obj.resolve(), 'ArmCpu command output differs')
        require('--target=arm64-apple-ios17.0' in argv and '-c' in argv, 'ArmCpu target ABI command differs')
        require(obj.is_file() and archive.is_file(), 'ArmCpu compiled object/archive missing')
        armcpu_files = [source_manifest, armcpu_source, armcpu/'Info.plist', cmd_path, obj, archive]
        record['cpu0_integration'] = {
            'source_delta': 'Exactly six reviewed CPU0 asm template separator repairs',
            'compiled_source': str(armcpu_source), 'files': {str(p): {'bytes': p.stat().st_size, 'sha256': digest(p.read_bytes())} for p in armcpu_files},
            'retained_config_and_first_60_xnu_patches_equal_earlylog': True,
            'compiler_command_selects_independent_source': True,
            'machine_code_six_templates_verified': False, 'runtime_verified': False,
            'qualification': 'Compile input and structural prelink checks only; independent actual machine-code audit required',
        }
        passed('cpu0_source_override_and_compile_command', **record['cpu0_integration'])

        pe_record = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_init.o.json'
        compilation_check = check_panic_compile_record(pe_record, tree)
        require(compilation_check['source_sha256'] == '40431ba0aebd1fe08ac3be516e1113655aad47fb2321864c41d40e2603147b84',
                'Actual compiled PE_create_console source differs from the independently reviewed early-log delta')
        mode_stamp = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/.dt-panic-region-mode'
        require(mode_stamp.read_text().strip() == '1', 'DT panic-region build mode stamp is not 1')
        config_check.update(current_path=str(config_path), current_sha256=digest(config_path.read_bytes()),
                            baseline_path=str(baseline_config), baseline_sha256=digest(baseline_config.read_bytes()))
        record['dt_panic_region'] = {'effective_config': config_check, 'compilation': compilation_check,
                                    'mode_stamp': {'path': str(mode_stamp), 'sha256': digest(mode_stamp.read_bytes()), 'value': 1},
                                    'machine_code_control_flow_verified': False,
                                    'runtime_verified': False, 'old_probe_addresses_reused': False}
        passed('dt_panic_region_effective_config_and_pe_init_compile', **record['dt_panic_region'])
        spec = status['board']['kernel']['prelink']
        profile = tomllib.loads(profile_path.read_text(encoding='utf-8'))
        require(spec == profile['kernel']['prelink'], 'Resolved prelink inputs differ from the fixed OS profile')
        require(spec['kernelcache_sha256'] == ORIGINAL_SHA256 and int(spec['slide'], 0) == SLIDE, 'Prelink identity or slide differs from the fixed target')
        require(spec['early_security_init'] == f'{AMFI}={AMFI_ORIGINAL:#x}', 'Unexpected early security initializer')
        native_path = (tree / spec['kernel']).resolve()
        require(native_path.is_relative_to(tree.resolve()), 'Native path escapes the host build tree')
        paths = {'native': native_path, 'candidate': candidate_path, 'original': original_path}
        images = {name: path.read_bytes() for name, path in paths.items()}
        hashes = {name: digest(data) for name, data in images.items()}
        require(hashes['original'] == ORIGINAL_SHA256, 'Original kernelcache hash differs from the fixed input')
        emitted = read_json(emitted_report_path)
        for field, name in (('native_sha256', 'native'), ('original_sha256', 'original'), ('candidate_sha256', 'candidate')):
            require(emitted[field] == hashes[name], f'Emitter report {field} does not match actual bytes')
        require(emitted['candidate_bytes'] == len(images['candidate']), 'Emitter candidate size differs from actual bytes')
        require(Path(emitted['output']).resolve() == candidate_path.resolve(), 'Emitter report names a different output')
        require(emitted['slide'] == SLIDE, 'Emitter slide differs from the selected profile')
        require(not any(key in emitted for key in ('missing', 'stubbed', 'collection_slide')), 'Unexpected fileset-style fields in ordinary prelink report')
        record['inputs'] = {name: {'path': str(paths[name]), 'sha256': hashes[name], 'bytes': len(data)} for name, data in images.items()}
        record['inputs']['status'] = {'path': str(status_path), 'sha256': digest(status_path.read_bytes())}
        record['inputs']['emitter_report'] = {'path': str(emitted_report_path), 'sha256': digest(emitted_report_path.read_bytes())}
        passed('input_identity_and_report', ordinary_prelink=True, slide=SLIDE,
               missing_stubbed_fields='Not present in the ordinary ARM64 report; not interpreted as zero counts')

        parsed = {name: np.macho(data) for name, data in images.items()}
        for name, image in parsed.items():
            require(image['kind'] == 2, f'{name} is not MH_EXECUTE')
            for segment in image['segments']:
                require(segment['fileoff'] + segment['filesize'] <= len(images[name]), f'{name}: segment {segment["name"]} exceeds file')
            occupied = sorted((s['va'], s['va'] + s['size'], s['name']) for s in image['segments'] if s['size'])
            for left, right in zip(occupied, occupied[1:]):
                require(left[1] <= right[0], f'{name}: virtual segments overlap: {left[2]}, {right[2]}')
        passed('macho_headers_and_segments', magic='0xfeedfacf', cpu='0x0100000c', subtype=0, file_type='MH_EXECUTE')
        record['storage_integration'] = check_storage_archive(root, target, tree, np, images['native'], parsed['native'])
        passed('actual_nine_tu_archive_and_unique_native_storage_owner', **record['storage_integration'])

        def metadata(name):
            segments = parsed[name]['segments']
            matches = [s for s in segments if s['name'] == '__PRELINK_INFO']
            require(len(matches) == 1, f'{name}: expected one __PRELINK_INFO')
            segment = matches[0]
            at = np.backed_offset(segments, segment['va'], segment['filesize'], len(images[name]))
            value = np.unserialize_xml(images[name][at:at + segment['filesize']])
            entries = {}
            for entry in value['_PrelinkInfoDictionary']:
                identifier = entry['CFBundleIdentifier']
                require(identifier not in entries, f'{name}: duplicate bundle {identifier}')
                entries[identifier] = entry
            return value, entries

        original_meta, original_entries = metadata('original')
        candidate_meta, candidate_entries = metadata('candidate')
        roots = spec['roots']
        require(len(roots) == len(set(roots)) == 31, 'Expected 31 unique selected roots')
        selected, interfaces = set(), set()
        pending = list(roots)
        while pending:
            identifier = pending.pop()
            if identifier in selected or identifier in interfaces:
                continue
            require(identifier in original_entries, f'Missing declared dependency {identifier}')
            entry = original_entries[identifier]
            if entry.get('OSKernelResource') is True:
                interfaces.add(identifier)
            else:
                selected.add(identifier)
                pending.extend(entry.get('OSBundleLibraries') or {})
        original_segments = parsed['original']['segments']
        candidate_segments = parsed['candidate']['segments']
        modules, owned = {}, []
        for identifier, entry in original_entries.items():
            if entry.get('OSKernelResource') is True or not entry.get('_PrelinkExecutableSize'):
                continue
            va = entry.get('_PrelinkExecutableLoadAddr')
            require(va is not None, f'{identifier}: executable size without address')
            at = np.backed_offset(original_segments, va, 32, len(images['original']))
            module = np.macho(images['original'], at)
            require(module['kind'] == 11, f'{identifier}: expected MH_KEXT_BUNDLE')
            modules[identifier] = module
            owned.extend((s['va'], s['va'] + s['size'], identifier, s['name'])
                         for s in module['segments'] if s['size'] and s['name'] != '__LINKEDIT')
        owned.sort()
        for left, right in zip(owned, owned[1:]):
            require(left[1] <= right[0], 'Original component extents overlap')
        starts = [item[0] for item in owned]

        def owner(address):
            index = bisect_right(starts, address) - 1
            return owned[index] if index >= 0 and address < owned[index][1] else None

        codeless = selected - modules.keys()
        executable = selected - codeless
        require(len(executable) == 30 and len(interfaces) == 7 and codeless == {'com.apple.iokit.IOHIDEventDriver'}, 'Selected closure differs from the fixed input audit')
        for field, expected in (('components', executable), ('codeless_components', codeless), ('kernel_interfaces', interfaces)):
            require(emitted[field] == sorted(expected), f'Emitter {field} differs from the actual roots closure')
        require(set(candidate_entries) == selected | interfaces, 'Candidate metadata bundle set differs from selected closure')
        for identifier in codeless:
            entry = original_entries[identifier]
            require(isinstance(entry.get('IOKitPersonalities'), dict) and bool(entry['IOKitPersonalities']), 'Codeless bundle lacks personalities')
            require(not any(key in entry for key in ('CFBundleExecutable', '_PrelinkExecutableLoadAddr', '_PrelinkExecutableSourceAddr', '_PrelinkExecutableSize', '_PrelinkKmodInfo')), 'Codeless bundle has executable fields')
        for identifier in selected | interfaces:
            expected = copy.deepcopy(original_entries[identifier])
            require('_NativePrelinkEarlySecurityInit' not in expected, 'Original input unexpectedly supplies a native initializer')
            if identifier == AMFI:
                expected['_NativePrelinkEarlySecurityInit'] = AMFI_ORIGINAL + SLIDE
            for key in ('_PrelinkExecutableLoadAddr', '_PrelinkExecutableSourceAddr', '_PrelinkKmodInfo'):
                if expected.get(key):
                    expected[key] += SLIDE
            require(candidate_entries[identifier] == expected, f'Candidate bundle metadata differs: {identifier}')
        passed('roots_closure_and_bundle_metadata', roots=roots, executable_components=sorted(executable),
               codeless_components=sorted(codeless), kernel_interfaces=sorted(interfaces))

        native_exports = np.exports(images['native'], parsed['native'])
        original_exports = np.exports(images['original'], parsed['original'])
        native_by_name = {}
        for address, names in native_exports.items():
            for name in names:
                native_by_name.setdefault(name, set()).add(address)
        for marker in ('_kalloc_sparse_prelink_index_version', '_native_prelink_security_init_version'):
            addresses = native_by_name.get(marker, set())
            require(len(addresses) == 1, f'Native ABI marker is not unique: {marker}')
            at = np.backed_offset(parsed['native']['segments'], next(iter(addresses)), 4, len(images['native']))
            require(struct.unpack_from('<I', images['native'], at)[0] == 1, f'Native ABI marker version differs: {marker}')
        packed = original_meta['_PrelinkLinkKASLROffsets']
        count, = struct.unpack_from('<I', packed)
        require(len(packed) == 4 + count * 4, 'Invalid original pointer table extent')
        offsets = struct.unpack_from(f'<{count}I', packed, 4)
        require(len(set(offsets)) == count, 'Original pointer table has duplicate sites')
        base = next(s['va'] for s in original_segments if s['name'] == '__PRELINK_TEXT')
        bindings = emitted['bindings']
        require(len({b['site'] for b in bindings}) == len(bindings), 'Emitter bindings have duplicate sites')
        actual_bindings, kinds, kernel_targets = [], Counter(), set()
        for offset in offsets:
            site = base + offset
            source = owner(site)
            if source is None or source[2] not in selected:
                continue
            require(source == owner(site + 7), f'Pointer spans module extent at {site:#x}')
            old_at = np.backed_offset(original_segments, site, 8, len(images['original']))
            target, = struct.unpack_from('<Q', images['original'], old_at)
            destination = owner(target)
            if destination:
                require(destination[2] in selected, f'Pointer targets an unselected component at {site:#x}')
                replacement, kind = target + SLIDE, 'component'
            else:
                choices = {new for name in original_exports.get(target, []) for new in native_by_name.get(name, set())}
                require(len(choices) == 1, f'Original kernel target {target:#x} lacks a unique native symbol binding')
                replacement, kind = next(iter(choices)), 'kernel'
                require(any(s['va'] <= replacement < s['va'] + s['size'] for s in parsed['native']['segments']), 'Native symbol binding lies outside mapped segments')
                kernel_targets.add(target)
            expected = {'site': site, 'target': target, 'new_site': site + SLIDE,
                        'new_target': replacement, 'kind': kind, 'module': source[2]}
            actual_bindings.append(expected)
            new_at = np.backed_offset(candidate_segments, site + SLIDE, 8, len(images['candidate']))
            require(struct.unpack_from('<Q', images['candidate'], new_at)[0] == replacement, f'Candidate pointer differs at {site + SLIDE:#x}')
            kinds[kind] += 1
        require(bindings == actual_bindings, 'Emitter bindings differ from independently reconstructed original relocations/native exports')
        require(dict(kinds) == {'kernel': 42532, 'component': 26762} and len(kernel_targets) == 1794, 'Pointer inventory differs from the fixed original-input audit')
        require(emitted['pointer_counts'] == dict(kinds), 'Emitter pointer counts differ from actual bindings')
        rebased_offsets = [b['new_site'] - (base + SLIDE) for b in actual_bindings]
        require(candidate_meta['_PrelinkLinkKASLROffsets'] == struct.pack(f'<{len(rebased_offsets) + 1}I', len(rebased_offsets), *rebased_offsets), 'Candidate KASLR pointer table differs from selected actual bindings')
        passed('pointer_table_native_symbol_bindings_and_candidate_readback', pointer_counts=dict(kinds),
               total=len(actual_bindings), unique_original_kernel_targets=len(kernel_targets))

        text_hashes, entrypoints, headers = {}, {}, []
        for identifier in sorted(executable):
            entry = original_entries[identifier]
            old_header = entry['_PrelinkExecutableLoadAddr']
            headers.append(old_header + SLIDE)
            new_at = np.backed_offset(candidate_segments, old_header + SLIDE, 32, len(images['candidate']))
            new_module = np.macho(images['candidate'], new_at)
            require(new_module['kind'] == 11, f'{identifier}: candidate module is not MH_KEXT_BUNDLE')
            old_module = modules[identifier]
            require(len(old_module['segments']) == len(new_module['segments']), f'{identifier}: module segment count differs')
            for old, new in zip(old_module['segments'], new_module['segments']):
                for key in ('name', 'size', 'fileoff', 'filesize', 'initprot', 'maxprot'):
                    require(old[key] == new[key], f'{identifier}: module segment {key} differs')
                require(new['va'] == (old['va'] + SLIDE if old['va'] else 0), f'{identifier}: segment address was not rebased correctly')
                require(len(old['sections']) == len(new['sections']), f'{identifier}: section count differs')
                for old_section, new_section in zip(old['sections'], new['sections']):
                    for key in ('name', 'size', 'fileoff'):
                        require(old_section[key] == new_section[key], f'{identifier}: section {key} differs')
                    require(new_section['va'] == (old_section['va'] + SLIDE if old_section['va'] else 0), f'{identifier}: section address differs')
                if old['name'] == '__TEXT_EXEC' and old['filesize']:
                    before_at = np.backed_offset(original_segments, old['va'], old['filesize'], len(images['original']))
                    after_at = np.backed_offset(candidate_segments, old['va'] + SLIDE, old['filesize'], len(images['candidate']))
                    before = images['original'][before_at:before_at + old['filesize']]
                    require(images['candidate'][after_at:after_at + old['filesize']] == before, f'{identifier}: original executable bytes changed')
                    text_hashes[identifier] = digest(before)
            kmod = entry['_PrelinkKmodInfo']
            old_at = np.backed_offset(original_segments, kmod, 196, len(images['original']))
            new_at = np.backed_offset(candidate_segments, kmod + SLIDE, 196, len(images['candidate']))
            original_address, = struct.unpack_from('<Q', images['original'], old_at + 156)
            require(original_address in (0, old_header), f'{identifier}: original kmod address disagrees with module header')
            require(struct.unpack_from('<Q', images['candidate'], new_at + 156)[0] == (original_address + SLIDE if original_address else 0), f'{identifier}: candidate kmod address differs')
            start, stop = struct.unpack_from('<QQ', images['original'], old_at + 180)
            expected_start, expected_stop = start + SLIDE if start else 0, stop + SLIDE if stop else 0
            require(struct.unpack_from('<QQ', images['candidate'], new_at + 180) == (expected_start, expected_stop), f'{identifier}: candidate module entrypoints differ')
            entrypoints[identifier] = {'kmod_info_va': kmod + SLIDE, 'start_va': expected_start, 'stop_va': expected_stop}
        require(emitted['original_executable_sha256'] == text_hashes, 'Emitter executable hashes differ from actual original/candidate ranges')
        require(emitted['module_entrypoints'] == entrypoints, 'Emitter module entrypoints differ from actual metadata')
        expected_extents = [{'module': identifier, 'segment': name, 'va': start + SLIDE, 'size': end - start}
                            for start, end, identifier, name in owned if identifier in selected]
        require(emitted['extents'] == expected_extents, 'Emitter selected extents differ from original module segments')
        passed('module_headers_entrypoints_and_original_executable_bytes', executable_hashes=text_hashes, modules=len(executable))

        headers.sort()
        prelink_text = next(s for s in candidate_segments if s['name'] == '__PRELINK_TEXT')
        index_sections = [s for s in prelink_text['sections'] if s['name'] == '__macho_list']
        require(len(index_sections) == 1, 'Candidate lacks a unique sparse header index section')
        index_section = index_sections[0]
        require(index_section['size'] == len(headers) * 8, 'Sparse index size differs from executable component count')
        index_at = np.backed_offset(candidate_segments, index_section['va'], index_section['size'], len(images['candidate']))
        require(index_at == index_section['fileoff'], 'Sparse index section file offset differs from mapping')
        require(list(struct.unpack_from(f'<{len(headers)}Q', images['candidate'], index_at)) == headers, 'Sparse index bytes differ from actual selected module headers')
        expected_index = {'va': index_section['va'], 'size': index_section['size'], 'fileoff': index_at, 'headers': headers, 'abi': 1}
        require(emitted['sparse_index'] == expected_index, 'Emitter sparse index differs from candidate section')
        require(emitted['early_security_initializers'] == {AMFI: AMFI_ORIGINAL + SLIDE}, 'Emitter AMFI initializer differs from selected input')
        require(original_entries[AMFI].get('AppleSecurityExtension') is True, 'AMFI is not marked as a security extension')
        require(any(s['name'] == '__TEXT_EXEC' and s['initprot'] == 5 and not s['maxprot'] & 2
                    and s['va'] <= AMFI_ORIGINAL and AMFI_ORIGINAL + 4 <= s['va'] + s['filesize']
                    for s in modules[AMFI]['segments']), 'AMFI initializer does not lie in original readonly executable bytes')
        require(candidate_meta['_PrelinkKCID'] == hashlib.sha256(images['native'] + images['original']).digest()[:16], 'Candidate KCID differs from actual input pair')
        for item in emitted['coalesced_segments']:
            matches = [s for s in candidate_segments if s['name'] == item['name']]
            require(len(matches) == 1, 'Report coalesced segment is not unique in candidate')
            segment = matches[0]
            require(all(item[key] == segment[key] for key in ('name', 'va', 'size', 'filesize', 'fileoff')), 'Reported coalesced segment differs from candidate')
            require(segment['va'] % 4096 == segment['size'] % 4096 == segment['fileoff'] % 4096 == 0, 'Coalesced segment is not 4 KiB aligned')
        require({s['name'] for s in emitted['coalesced_segments']} == {'__PRELINK_TEXT', '__PLK_TEXT_EXEC', '__PLK_DATA_CONST', '__PRELINK_DATA', '__PRELINK_INFO'}, 'Unexpected emitted coalesced segment set')
        passed('sparse_index_security_initializer_kcid_and_coalesced_geometry', sparse_index_abi=1,
               sparse_headers=len(headers), amfi_candidate_address=hex(AMFI_ORIGINAL + SLIDE))

        for item in emitted['known_inline_data']:
            require(item['module'] in executable, 'Inline-data inventory names an unselected executable component')
            before_at = np.backed_offset(original_segments, item['va'], item['size'], len(images['original']))
            after_at = np.backed_offset(candidate_segments, item['va'] + SLIDE, item['size'], len(images['candidate']))
            before = images['original'][before_at:before_at + item['size']]
            require(digest(before) == item['sha256'] and images['candidate'][after_at:after_at + item['size']] == before, 'Recognized inline data differs from original/candidate bytes')
        require(type(emitted['pc_relative_encodings_preserved']) is int and emitted['pc_relative_encodings_preserved'] > 0, 'Missing emitter PC-relative inventory count')
        for flag in FALSE_FLAGS:
            require(emitted[flag] is False, f'Unexpected qualification/modification marker: {flag}')
        record['qualification_markers'] = {flag: emitted[flag] for flag in FALSE_FLAGS}
        record['limitations'] = [
            'DT panic-region effective configuration and pe_init.o compiler arguments are checked; independent new-kernel control-flow disassembly and hardware execution remain required.',
            'No runtime ABI, C++ vtable/structure/call compatibility, register dataflow, hardware handoff or boot qualification.',
            'Storage registration/linkage does not grant controller ownership, prove MMIO/DMA, publish a block device, mount root or qualify persistence.',
            'The emitter PC-relative inventory count is recorded, not independently re-decoded by this verifier.',
            'The ordinary ARM64 format has no missing/stubbed fields; actual selected pointer sites and unique native export bindings were verified instead.',
            'AMFI initializer range and native version markers do not prove runtime policy behavior; code preservation does not prove AES request semantics.',
        ]
        passed('qualification_boundaries_and_inline_data', known_inline_data_ranges=len(emitted['known_inline_data']),
               emitter_pc_relative_encodings_preserved=emitted['pc_relative_encodings_preserved'])
        for name, path in paths.items():
            require(digest(path.read_bytes()) == hashes[name], f'{name} changed while validation was running')
        for name, path in (('status', status_path), ('emitter_report', emitted_report_path)):
            require(digest(path.read_bytes()) == record['inputs'][name]['sha256'], f'{name} changed during validation')
        for path, before in ((config_path, config_check['current_sha256']),
                             (baseline_config, config_check['baseline_sha256']),
                             (pe_record, compilation_check['sha256']),
                             (mode_stamp, record['dt_panic_region']['mode_stamp']['sha256']),
                             (Path(compilation_check['object']), compilation_check['object_sha256']),
                             (Path(compilation_check['source']), compilation_check['source_sha256'])):
            require(digest(path.read_bytes()) == before, f'{path.name} changed during validation')
        for path, spec in record['cpu0_integration']['files'].items():
            require(digest(Path(path).read_bytes()) == spec['sha256'], 'CPU0 compile evidence changed during verification: ' + path)
        require(check_source_manifest(root, source_manifest, manifest) == record['source_manifest'], 'Source/physical manifests changed during verification')
        for path, expected in record['storage_integration']['verified_files'].items():
            require(digest(Path(path).read_bytes()) == expected, 'Storage build evidence changed during verification: ' + path)
        passed('inputs_unchanged_during_verification')
        record['status'] = 'passed'
    except Exception as exc:
        record['status'] = 'failed'
        record['error'] = {'type': type(exc).__name__, 'message': str(exc)}
        raise
    finally:
        record['finished_utc'] = utc()
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(json.dumps(record, indent=2) + '\n', encoding='utf-8')
        print(f'Host candidate verification {record["status"]}: {report_path}', file=sys.stderr)


if __name__ == '__main__':
    main()
