#!/usr/bin/env python3
"""Frozen r25 machine_lockdown-return kernel contract, independent of the patcher.

Reuses the unchanged, pinned r22 parser/layout/paint/reset encoder. It does not
reuse the r22 call selection or infer a post-lockdown runtime success.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
BASE_MODULE_SHA256 = '34cbd2a965faa449c05a92ff2ff01012234a14cf81c7c1ecae242924d2220f06'
BASE_CONTRACT_SHA256 = 'ff1455d6c279f47a1fab97ac3f9ac906bb02e71022fe505e350b95d8a1092aa3'
BASE_PATH = Path(__file__).with_name('verify_xnu_early_log_kernel.py').resolve()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


require(sha(BASE_PATH.read_bytes()) == BASE_MODULE_SHA256, 'Frozen base verifier changed')
import verify_xnu_early_log_kernel as base
require(Path(base.__file__).resolve() == BASE_PATH, 'Wrong base verifier import')
MachO, branch, target, address = base.MachO, base.branch, base.target, base.address
CALLER_NAME, CALLEE_NAME = '_kernel_bootstrap_thread', '_machine_lockdown'
RESET_SETTINGS = dict(base.RESET_SETTINGS, entry_probe_reset_after=CALLEE_NAME, entry_probe_reset_in=CALLER_NAME)
DELAY_TICKS, CAVE_BYTES = base.DELAY_TICKS, base.CAVE_BYTES
PARSER_SOURCE_SHA256 = base.PARSER_SOURCE_SHA256
PROTOCOL_HEADER_SHA256 = base.PROTOCOL_HEADER_SHA256
SOURCE_PATCH_SHA256 = base.SOURCE_PATCH_SHA256
FUNCTIONS = {
    'caller': ('_kernel_bootstrap_thread', '1237f72cc1a15a6ebf1437bd59aaa81e66393a353acb0a07187e2785e4c98590'),
    'callee': ('_machine_lockdown', '1c2a9e934d91ffa9536ed159adcbfabde9654fe56c6218fafbfaa90a1f6dc0f2'),
    'prot_init': ('_arm_vm_prot_init', '469bac21af0bf7f433f87ce71e4c3ce81f601e19419540f3b0e7f24d9ccb4cd7'),
    'prot_finalize': ('_arm_vm_prot_finalize', 'e767439b7d5ba674cdc1160d2a90db923ac08728a7edd137c1820ff3ef9a96ea'),
    'replace_bootstack': ('_arm64_replace_bootstack', '3d194475ffb3315a69547d412b22e3b3f88fb20ceac840126a174806e873f8a7'),
}



def validate_expectation(value):
    require(isinstance(value, dict) and type(value.get('schema_version')) is int and value['schema_version'] == 1,
            'Unsupported r25 expectation')
    require(value.get('status') == 'independently-frozen', 'Unreviewed r25 contract')
    baseline = value.get('baseline_contract')
    require(sha(canonical(baseline)) == BASE_CONTRACT_SHA256, 'Frozen r22 source/layout contract changed')
    base.validate_expectation(baseline)
    require(value.get('base_module_sha256') == BASE_MODULE_SHA256 and
            value.get('baseline_contract_sha256') == BASE_CONTRACT_SHA256, 'Missing pinned baseline provenance')
    for key in ('original', 'parser_source_sha256', 'protocol_header_sha256', 'source_patch_sha256'):
        require(value.get(key) == baseline[key], 'Changed baseline field: ' + key)
    require(value.get('reset_settings') == RESET_SETTINGS and
            all(type(value['reset_settings'][key]) is type(want) for key, want in RESET_SETTINGS.items()),
            'Expected machine_lockdown occurrence 1 return, six-second reset')
    require(value.get('review') == {'machine_source_and_abi_reviewed': True,
                                   'post_lockdown_cave_mapping_reviewed': True,
                                   'independent_expected_bytes_reviewed': True}, 'Missing r25 independent review')
    expected = value.get('expected')
    base._exact_keys(expected, ('sha256', 'changed_bytes'), 'expected')
    base._digest(expected['sha256'], 'expected.sha256')
    require(type(expected['changed_bytes']) is int and 0 < expected['changed_bytes'] <= 300, 'Bad changed byte count')
    evidence = value.get('review_evidence')
    require(isinstance(evidence, dict) and set(baseline['review_evidence']) <= set(evidence), 'Missing inherited review provenance')
    for path, spec in baseline['review_evidence'].items():
        require(evidence[path] == spec, 'Changed inherited review provenance: ' + path)
    for name in ('machine-review.json', 'machine-code.json', 'review_machine.py'):
        path = 'analysis/xnu-machine-lockdown-r25-20261114/' + name
        require(path in evidence, 'Missing r25 review: ' + name)
        base._exact_keys(evidence[path], ('sha256', 'bytes'), name)
        base._digest(evidence[path]['sha256'], name)
        require(type(evidence[path]['bytes']) is int and evidence[path]['bytes'] > 0, 'Bad review evidence size')
    require(isinstance(value.get('layout'), dict), 'Missing r25 layout')
    return value


def load_expectation(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'Duplicate JSON expectation key: ' + key)
            result[key] = value
        return result
    return validate_expectation(json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique))


def _segments(raw):
    result = {}
    at = 32
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        command, size = struct.unpack_from('<II', raw, at)
        require(size >= 8 and at + size <= len(raw), 'Malformed load command')
        if command == 0x19:
            name = raw[at + 8:at + 24].split(b'\0', 1)[0].decode('ascii')
            va, length, fileoff, filesize, maxprot, initprot = struct.unpack_from('<QQQQii', raw, at + 24)
            require(name not in result, 'Duplicate segment')
            result[name] = {'start': hex(va), 'end': hex(va + length), 'fileoff': fileoff,
                            'filesize': filesize, 'maxprot': maxprot, 'initprot': initprot}
        at += size
    return result


def _discover(original, baseline):
    image, old = base.original_layout(original, baseline)
    functions = {}
    for key, (name, expected_hash) in FUNCTIONS.items():
        functions[key] = base._function(image, name)
        require(functions[key]['sha256'] == expected_hash, 'Reviewed machine chain body changed: ' + name)
    caller, callee = functions['caller'], functions['callee']
    destination = address(callee['start'])
    calls = [va for va in range(address(caller['start']), address(caller['end']), 4)
             if image.word(va) & 0xfc000000 == 0x94000000 and
             target(image.word(va), va, 0x94000000) == destination]
    require(calls == [0xfffffff0072c87f4] and image.word(calls[0]) == 0x94054d63,
            'Expected unique machine_lockdown BL inside kernel_bootstrap_thread')
    require(image.body(destination, 8) == struct.pack('<2I', 0xa9bf7bfd, 0x910003fd), 'Machine LR/SP save')
    require(image.body(0xfffffff00741bdb8, 8) == struct.pack('<2I', 0xa8c17bfd, 0xd65f03c0), 'Machine LR/SP restore and RET')
    require([va for va in range(destination, address(callee['end']), 4)
             if image.word(va) == 0xd65f03c0] == [0xfffffff00741bdbc], 'Unique machine normal RET')
    require(target(image.word(0xfffffff00741bd94), 0xfffffff00741bd94, 0x94000000)
            == address(functions['prot_finalize']['start']), 'Actual protection finalization call')
    # Bounded actual mapping: ROX(segTEXTEXECB,segSizeTEXTEXEC), including literal reads.
    require(image.body(0xfffffff00740fd50, 36) == struct.pack('<9I',
            0xd0ffeb08, 0xf9410100, 0xd503201f, 0xf9410501, 0xd2800002,
            0x52800003, 0x52800044, 0x52800005, 0x52800066), 'Actual TEXT_EXEC ROX argument materialization')
    require(target(image.word(0xfffffff00740fd74), 0xfffffff00740fd74, 0x94000000)
            == image.names['_arm_vm_page_granular_prot'], 'Actual TEXT_EXEC protection helper')
    # Restore both older selected calls from the original, never chain their probes.
    old_calls = {
        'InitIOKit': {'va': old['call_site'], 'word': old['original_call_word'], 'target_va': old['callee']['start']},
        'ConfigureIOKit': dict(old['configure_call']),
    }
    for item in old_calls.values():
        require(image.word(address(item['va'])) == int(item['word'], 16), 'Old selected call not original')
    segments = _segments(original)
    executable = segments['__TEXT_EXEC']
    require(executable['maxprot'] == executable['initprot'] == 5, 'Kernel text must be readable executable')
    for va in (address(old['caves'][stage]) for stage in ('1', '2', '3', '4')):
        require(address(executable['start']) <= va and va + CAVE_BYTES <= address(executable['end']), 'Cave escaped TEXT_EXEC')
        require([(seg, sec) for seg, sec, low, size in image.sections if low <= va and va + CAVE_BYTES <= low + size]
                == [('__TEXT_EXEC', '__text')], 'Cave escaped ordinary text section')
        require(va >> 12 == address(old['caves']['4']) >> 12, 'Caves not in reviewed common 4KiB page')
    layout = {key: old[key] for key in ('entry', 'first_cpu', 'mmu', 'post_mmu', 'caves', 'console')}
    layout.update({'caller': caller, 'callee': callee, 'call_site': hex(calls[0]),
                   'original_call_word': '0x94054d63', 'old_calls': old_calls,
                   'argument_semantics': 'void(void); callee loads PE_state.bootArgs internally',
                   'normal_return': {'va': '0xfffffff00741bdbc', 'word': '0xd65f03c0',
                                     'restore_va': '0xfffffff00741bdb8', 'restore_word': '0xa8c17bfd'},
                   'mapping': {'functions': functions, 'text_exec': executable,
                               'cave_page': hex(address(old['caves']['4']) & ~0xfff),
                               'text_exec_rox_call': '0xfffffff00740fd74',
                               'runtime_mapping_verified': False}})
    return image, layout


def original_layout(original, expectation):
    validate_expectation(expectation)
    image, layout = _discover(original, expectation['baseline_contract'])
    require(layout == expectation['layout'], 'Frozen r25 layout differs from actual original')
    return image, layout


def expected_regions(layout):
    return base.expected_regions(layout)


def derive_expected(original, expectation):
    image, layout = original_layout(original, expectation)
    result, changed = base._derive_unreviewed(original, image, layout)
    require(sha(result) == expectation['expected']['sha256'] and changed == expectation['expected']['changed_bytes'],
            'Independent bytes differ from frozen r25 digest/count')
    return result


def probe_readback(original, patched, expectation):
    image, layout = original_layout(original, expectation)
    require(len(patched) == len(original), 'Diagnostic kernel length changed')
    regions = []
    for va, code in expected_regions(layout):
        offset = image.offset(va, len(code))
        require(patched[offset:offset + len(code)] == code, 'Wrong exact hook/cave bytes at ' + hex(va))
        regions.append((offset, offset + len(code), va))
    cursor, changed, counts = 0, 0, []
    for low, high, va in sorted(regions):
        require(low >= cursor and patched[cursor:low] == original[cursor:low], 'Mutation outside six r25 hook/cave intervals')
        count = sum(a != b for a, b in zip(patched[low:high], original[low:high]))
        changed += count
        counts.append({'va': hex(va), 'file_start': hex(low), 'file_end': hex(high), 'changed_bytes': count})
        cursor = high
    require(patched[cursor:] == original[cursor:], 'Mutation outside r25 intervals in kernel tail')
    require(changed == expectation['expected']['changed_bytes'] and sha(patched) == expectation['expected']['sha256'],
            'Candidate differs from frozen r25 digest/count')
    candidate = MachO(patched)
    for name, item in layout['old_calls'].items():
        require(candidate.word(address(item['va'])) == int(item['word'], 16), 'Old hook remains patched: ' + name)
    require(candidate.body(address(layout['caves']['3']), CAVE_BYTES) == bytes(CAVE_BYTES), 'Unexpected stage3 code')
    return {'status': 'passed-host-byte-verification', 'sha256': sha(patched), 'bytes': len(patched),
            'original_sha256': expectation['original']['sha256'], 'changed_bytes': changed,
            'caller': CALLER_NAME, 'callee': CALLEE_NAME, 'call_site': layout['call_site'],
            'caller_range': [layout['caller']['start'], layout['caller']['end']],
            'callee_range': [layout['callee']['start'], layout['callee']['end']],
            'occurrence': 1, 'reset_before': False, 'reset_after_callee_returns': True,
            'reset_delay_seconds': 6, 'timebase_hz': 19200000, 'delay_ticks': DELAY_TICKS,
            'counter': 'CNTPCT_EL0', 'reset_function': '0x84000009', 'conduit': 'SMC #0',
            'stage3_disabled': True, 'stage1_stage2_exact_words_verified': True,
            'all_other_bytes_identical': True, 'header_and_symbols_unchanged': True,
            'old_init_call_restored': True, 'old_configure_call_restored': True, 'original_sp_preserved_before_call': True,
            'argument_semantics': layout['argument_semantics'], 'normal_return': layout['normal_return'], 'mapping': layout['mapping'],
            'general_driver_matching_started_by_checkpoint': False,
            'runtime_machine_lockdown_return_observed': False, 'hardware_tested': False,
            'source_crypto_seal_changed_by_probe': False, 'permitted_regions': counts,
            'scope': 'Host bytes only. Machine lockdown normal-return checkpoint after IRQ unmask and VM finalization. '
                     'Static TEXT_EXEC retention supports wrapper/literal execution; runtime page tables, IRQ and post-lockdown SMC remain unproved.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--expectation', type=Path, required=True)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    result = probe_readback(args.original.read_bytes(), args.candidate.read_bytes(), load_expectation(args.expectation))
    if args.report:
        with args.report.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
    print(json.dumps(result, indent=2))
