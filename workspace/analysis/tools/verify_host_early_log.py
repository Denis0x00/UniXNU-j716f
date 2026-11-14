#!/usr/bin/env python3
"""Read-only validation of the independent TB-J716F early-log host rebuild.

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

MAIN_REVISION = '7d85ede80092050f4d87f72ac7229bb605ce07fb'
PRELINK_REVISION = '440589b0c31625f17edf363e24b3c82c51c70bb9'
ORIGINAL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
PROFILE = 'ipados-17.7.11-21H461'
SLIDE = -0x1000000
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
    require(baseline.get('XNU_DT_PANIC_REGION', False) is False, 'Retained baseline already enabled DT panic-region')
    current_rest, baseline_rest = dict(current), dict(baseline)
    current_rest.pop('XNU_DT_PANIC_REGION', None)
    baseline_rest.pop('XNU_DT_PANIC_REGION', None)
    require(current_rest == baseline_rest, 'Effective configuration changed beyond DT panic-region')
    return {'only_effective_delta': {'XNU_DT_PANIC_REGION': {'before': baseline.get('XNU_DT_PANIC_REGION'), 'after': True}},
            'current_effective_keys': len(current), 'baseline_effective_keys': len(baseline)}


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
    out = main_repo / 'out-host-early-log-20261114/j716f-host-early-log' / PROFILE / 'j716f_host_early_log'
    tree = out / 'tree'
    status_path = root / 'analysis/host-early-log-20261114/status.json'
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
        require(status['name'] == 'j716f-host-early-log', 'Status belongs to another target')
        require(Path(status['output']).resolve() == out.resolve(), 'Status output differs from the isolated host target')
        require(status['board']['board']['example'] is False, 'Host status is still an example')
        require(status['board']['board']['os_profile'] == PROFILE, 'Unexpected target OS profile')
        baseline_status_path = root/'analysis/host-kernel-20261112/status.json'
        baseline_status = read_json(baseline_status_path)
        config_path = Path(status['config_path']).resolve()
        require(config_path == (out/'.config').resolve(), 'Effective config is outside the new host output')
        baseline_config = Path(baseline_status['config_path']).resolve()
        require(baseline_config == (main_repo/'out-host/j716f-host'/PROFILE/'j716f_host/.config').resolve(), 'Unexpected retained host config path')
        config_check = compare_configuration(config_path.read_text(), baseline_config.read_text())
        require(len(status['patch_order']) == 60 and
                [digest(Path(p).read_bytes()) for p in status['patch_order'][:-2]] ==
                [digest(Path(p).read_bytes()) for p in baseline_status['patch_order']], 'First 58 patches differ from retained baseline')
        require(digest(Path(status['patch_order'][-2]).read_bytes()) == digest((root/'work/UniXNU-j716f-host-early-log/patches/xnu/j716f-verbose-text-console.patch').read_bytes()), 'Unexpected console patch')
        require(digest(Path(status['patch_order'][-1]).read_bytes()) == 'b49a5a28d179cbf9dc35432f7f0b98906ea3d0e238f69a6f077bbcca4a548708', 'Unexpected early-log patch')
        early_sources = {'osfmk/console/serial_console.c': 'ac84fa14135c1c920a12c4c67d2f607c6d537b737bebb06a1bc642e4863b784a', 'osfmk/kern/printf.c': 'f40cc49f6de5ec58cc94386923ae5ba91dcfdcbae801e11b4ba6bec3aaeed824', 'pexpert/arm/J716fEarlyLog.h': 'd871825f16e72b3923420b3c0aa54e5d5a2291e5c0272d582ce05f9a22a86d16', 'pexpert/arm/j716f_earlylog_guards.h': 'eabea360a6364c7030a336c7082ed0cd28815da0c113379421333f52094f0120', 'pexpert/arm/j716f_earlylog_impl.h': 'be6a873270dd9b8eb97713a9bfcd815529267fa6a72c7534eb4f08759e53d27a', 'pexpert/arm/pe_init.c': '40431ba0aebd1fe08ac3be516e1113655aad47fb2321864c41d40e2603147b84', 'pexpert/gen/pe_gen.c': 'adaa2c7fc77f1affce26aa5b74a2d1ac6d5bcf05145aae389d6394bd376997d2'}
        for name, expected in early_sources.items():
            require(digest((tree/'.cache/xnu-work'/name).read_bytes()) == expected, 'Compiled early-log source mismatch: ' + name)
        require(len(status['components']) == 5 and [c['id'] for c in status['components']] ==
                [c['id'] for c in baseline_status['components']], 'Component selection differs from retained five-component baseline')
        require(len(status['board']['kernel']['archives']) == 8 and status['board']['kernel']['archives'] ==
                baseline_status['board']['kernel']['archives'], 'Kernel archive recipes differ from retained eight-recipe baseline')
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
