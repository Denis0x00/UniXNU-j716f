#!/usr/bin/env python3
"""Frozen r24 ConfigureIOKit-return kernel contract, independent of the patcher.

Reuses the unchanged, pinned r22 parser/layout/paint/reset encoder. It does not
reuse the r22 call selection or pretend ConfigureIOKit has a direct RET.
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
CALLER_NAME, CALLEE_NAME = '_PE_init_iokit', '_ConfigureIOKit'
RESET_SETTINGS = dict(base.RESET_SETTINGS, entry_probe_reset_after=CALLEE_NAME)
DELAY_TICKS, CAVE_BYTES = base.DELAY_TICKS, base.CAVE_BYTES
PARSER_SOURCE_SHA256 = base.PARSER_SOURCE_SHA256
PROTOCOL_HEADER_SHA256 = base.PROTOCOL_HEADER_SHA256
SOURCE_PATCH_SHA256 = base.SOURCE_PATCH_SHA256
FUNCTIONS = {
    'callee': ('_ConfigureIOKit', '3eae04a55ebfed94d05237fa1244700af908f8b646da2a5dcbda9fee2cbdde36'),
    'defaults': ('__ZN22IOPlatformExpertDevice17configureDefaultsEv', 'f62c5e43be6c3ad4a2ff1ec19a2c724c9dbca44f746c41f05ea354986b0746ec'),
    'uuid': ('__ZN22IOPlatformExpertDevice20generatePlatformUUIDEv', '38b9ddf7f3d59048a8e63e13b5e8abb6b4ccc067660bb11f1f62e80c80b20da0'),
    'nvram': ('__ZN22IOPlatformExpertDevice11createNVRAMEv', 'b5790441ea41fd23b4760c99e49203e1c9f4c21ad55a787cd5202c4fb6aa93f9'),
    'lockdown': ('_PE_lockdown_iokit', '5769c9699a92d9c5091eef0d932914344e2859943ea560e55cbc49eec50f4daa'),
    'matching': ('_StartIOKitMatching', '0b2e1ae85a660632a9e12db79e6c7922e547e12bcc33176fd62d2b0675d49a12'),
}


def validate_expectation(value):
    require(isinstance(value, dict) and type(value.get('schema_version')) is int and value['schema_version'] == 1,
            'Unsupported r24 expectation')
    require(value.get('status') == 'independently-frozen', 'Unreviewed r24 contract')
    baseline = value.get('baseline_contract')
    require(sha(canonical(baseline)) == BASE_CONTRACT_SHA256, 'Frozen r22 source/layout contract changed')
    base.validate_expectation(baseline)
    require(value.get('base_module_sha256') == BASE_MODULE_SHA256 and
            value.get('baseline_contract_sha256') == BASE_CONTRACT_SHA256, 'Missing pinned baseline provenance')
    for key in ('original', 'parser_source_sha256', 'protocol_header_sha256', 'source_patch_sha256'):
        require(value.get(key) == baseline[key], 'Changed baseline field: ' + key)
    require(value.get('reset_settings') == RESET_SETTINGS and
            all(type(value['reset_settings'][key]) is type(want) for key, want in RESET_SETTINGS.items()),
            'Expected ConfigureIOKit occurrence 1 return, six-second reset')
    require(value.get('review') == {'configure_source_and_abi_reviewed': True,
                                   'two_tail_calls_and_final_ret_reviewed': True,
                                   'independent_expected_bytes_reviewed': True}, 'Missing r24 independent review')
    expected = value.get('expected')
    base._exact_keys(expected, ('sha256', 'changed_bytes'), 'expected')
    base._digest(expected['sha256'], 'expected.sha256')
    require(type(expected['changed_bytes']) is int and 0 < expected['changed_bytes'] <= 300, 'Bad changed byte count')
    evidence = value.get('review_evidence')
    require(isinstance(evidence, dict) and set(baseline['review_evidence']) <= set(evidence), 'Missing inherited review provenance')
    for path, spec in baseline['review_evidence'].items():
        require(evidence[path] == spec, 'Changed inherited review provenance: ' + path)
    for name in ('configure-review.json', 'machine-code.json', 'review_configure.py'):
        path = 'analysis/xnu-configure-iokit-r24-20261114/' + name
        require(path in evidence, 'Missing r24 review: ' + name)
        base._exact_keys(evidence[path], ('sha256', 'bytes'), name)
        base._digest(evidence[path]['sha256'], name)
        require(type(evidence[path]['bytes']) is int and evidence[path]['bytes'] > 0, 'Bad review evidence size')
    require(isinstance(value.get('layout'), dict), 'Missing r24 layout')
    return value


def load_expectation(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'Duplicate JSON expectation key: ' + key)
            result[key] = value
        return result
    return validate_expectation(json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique))


def _discover(original, baseline):
    image, old = base.original_layout(original, baseline)
    functions = {}
    for key, (name, expected_hash) in FUNCTIONS.items():
        functions[key] = base._function(image, name)
        require(functions[key]['sha256'] == expected_hash, 'Reviewed Configure chain body changed: ' + name)
    caller = old['caller']
    destination = address(functions['callee']['start'])
    calls = [va for va in range(address(caller['start']), address(caller['end']), 4)
             if image.word(va) & 0xfc000000 == 0x94000000 and
             target(image.word(va), va, 0x94000000) == destination]
    require(len(calls) == 1, 'ConfigureIOKit call is not unique')
    call = calls[0]
    require(call == address(old['configure_call']['va']) == address(old['call_site']) + 4,
            'New Configure call must immediately follow original InitIOKit call')
    require(image.word(call) == 0x97fd00d5 and image.word(call - 4) == 0x97fd0096,
            'Original Configure/Init call words changed')
    # The source prototype is void(void); the callee loads gRootNub itself.
    require(image.body(destination, 16) == struct.pack('<4I', 0xf0ffc6a8, 0xf9414900, 0xb4000040, 0x14014fcf),
            'Configure root load/null branch/normal tail changed')
    defaults, uuid = address(functions['defaults']['start']), address(functions['uuid']['start'])
    require(target(image.word(destination + 12), destination + 12, 0x14000000) == defaults,
            'Configure tail no longer targets configureDefaults')
    tail = address(functions['defaults']['end']) - 4
    require(target(image.word(tail), tail, 0x14000000) == uuid,
            'configureDefaults tail no longer targets generatePlatformUUID')
    require(image.body(tail - 12, 12) == struct.pack('<3I', 0xaa1303e0, 0xa9417bfd, 0xa8c24ff4),
            'Defaults root/LR/SP restore changed')
    ret = 0xfffffff007939840
    require(image.body(ret - 20, 24) == struct.pack('<6I', 0xa94e7bfd, 0xa94d4ff4, 0xa94c57f6,
                                                  0xa94b5ff8, 0x9103c3ff, 0xd65f03c0),
            'Final UUID normal LR/SP restore or RET changed')
    layout = {key: old[key] for key in ('entry', 'first_cpu', 'mmu', 'post_mmu', 'caves', 'caller', 'console')}
    layout.update({'callee': functions['callee'], 'call_site': hex(call),
                   'original_call_word': f'0x{image.word(call):08x}',
                   'old_init_call': {'va': old['call_site'], 'word': old['original_call_word'],
                                     'target_va': old['callee']['start']},
                   'argument_semantics': 'void(void); callee loads gRootNub into x0',
                   'return_chain': {'functions': functions, 'tail_sites': [hex(destination + 12), hex(tail)],
                                    'final_ret': hex(ret), 'direct_configure_ret': False}})
    return image, layout


def original_layout(original, expectation):
    validate_expectation(expectation)
    image, layout = _discover(original, expectation['baseline_contract'])
    require(layout == expectation['layout'], 'Frozen r24 layout differs from actual original')
    return image, layout


def expected_regions(layout):
    return base.expected_regions(layout)


def derive_expected(original, expectation):
    image, layout = original_layout(original, expectation)
    result, changed = base._derive_unreviewed(original, image, layout)
    require(sha(result) == expectation['expected']['sha256'] and changed == expectation['expected']['changed_bytes'],
            'Independent bytes differ from frozen r24 digest/count')
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
        require(low >= cursor and patched[cursor:low] == original[cursor:low], 'Mutation outside six r24 hook/cave intervals')
        count = sum(a != b for a, b in zip(patched[low:high], original[low:high]))
        changed += count
        counts.append({'va': hex(va), 'file_start': hex(low), 'file_end': hex(high), 'changed_bytes': count})
        cursor = high
    require(patched[cursor:] == original[cursor:], 'Mutation outside r24 intervals in kernel tail')
    require(changed == expectation['expected']['changed_bytes'] and sha(patched) == expectation['expected']['sha256'],
            'Candidate differs from frozen r24 digest/count')
    candidate = MachO(patched)
    require(candidate.word(address(layout['old_init_call']['va'])) == int(layout['old_init_call']['word'], 16),
            'Old InitIOKit hook remains patched')
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
            'old_init_call_restored': True, 'original_sp_preserved_before_call': True,
            'argument_semantics': layout['argument_semantics'], 'return_chain': layout['return_chain'],
            'general_driver_matching_started_by_checkpoint': False,
            'runtime_configure_return_observed': False, 'hardware_tested': False,
            'source_crypto_seal_changed_by_probe': False, 'permitted_regions': counts,
            'scope': 'Host bytes only. Actual ConfigureIOKit return passes through two tail calls and UUID RET. '
                     'NVRAM object init/attach/start is included; general matching, hardware success, root and userland are unproven.'}


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
