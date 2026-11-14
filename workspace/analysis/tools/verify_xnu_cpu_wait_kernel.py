#!/usr/bin/env python3
"""Independent r27 kperf-return/CPU-publication checkpoint byte contract.

The normal kperf path unconditionally traverses ml_wait_max_cpus. A return
supports publication of SET, not proof that sleep, IRQ delivery or SMP ran.
The unchanged early-log/CPU0 baseline is pinned to the frozen r26 review.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
BASE_MODULE_SHA256 = '40cf8a14cb913ad1c62c9f6511db9b8c887fb2f33997aa09215cfd3bf727cd04'
BASE_CONTRACT_SHA256 = '7d3d71f89af7173cff0d0ebadc41a274ab4e45cf82815a3422b830d02e940bb0'
BASE_PATH = Path(__file__).with_name('verify_xnu_iokit_matching_kernel.py').resolve()


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()


require(sha(BASE_PATH.read_bytes()) == BASE_MODULE_SHA256, 'Frozen r26 verifier changed')
import verify_xnu_iokit_matching_kernel as previous
require(Path(previous.__file__).resolve() == BASE_PATH, 'Wrong frozen r26 module imported')
base = previous.base
MachO, branch, target, address = previous.MachO, previous.branch, previous.target, previous.address
CALLER_NAME, CALLEE_NAME = '_kernel_bootstrap_thread', '_kperf_init_early'
RESET_SETTINGS = dict(previous.RESET_SETTINGS, entry_probe_reset_after=CALLEE_NAME)
DELAY_TICKS, CAVE_BYTES = previous.DELAY_TICKS, previous.CAVE_BYTES
PARSER_SOURCE_SHA256 = previous.PARSER_SOURCE_SHA256
PROTOCOL_HEADER_SHA256 = previous.PROTOCOL_HEADER_SHA256
SOURCE_PATCH_SHA256 = previous.SOURCE_PATCH_SHA256
FUNCTIONS = {
    'caller': (CALLER_NAME, 'deb92769543f8bc19efc93cb42bac63f922f1ab7b7db1a72dc30a2446b2f460a'),
    'callee': (CALLEE_NAME, '3ff956446386774aff4e585000a7dca7b3df361ac4c516e9a81228f087758f03'),
    'cpu_wait': ('_ml_wait_max_cpus', '73246279fdc33923c457ca0586a65bd3916c928ccd513ae74ff01a25d7d73fec'),
    'cpu_set': ('_ml_set_max_cpus', '6be9e6016fcb4bb93ab1b5dfa9e050fdbb345d439700cf6b3e63a847bd72ee28'),
    'mutex_sleep': ('_lck_mtx_sleep', 'afc8a616f2acfa06fe2a52fab885d26eacc788514091e76ceb41e59cc7cc1004'),
}
REVIEW_DIR = 'analysis/xnu-cpu-wait-r27-20261114/'
REVIEW_FLAGS = {'new_call_and_normal_return_reviewed': True,
                'previous_PE_hook_restored': True,
                'conditional_cpu_wait_and_publication_limits_reviewed': True,
                'independent_expected_bytes_reviewed': True}


def validate_expectation(value):
    require(isinstance(value, dict) and type(value.get('schema_version')) is int and value['schema_version'] == 1,
            'Unsupported r27 expectation')
    require(value.get('status') == 'independently-frozen', 'Unreviewed r27 contract')
    baseline = value.get('baseline_contract')
    require(sha(canonical(baseline)) == BASE_CONTRACT_SHA256, 'Frozen r26 contract changed')
    previous.validate_expectation(baseline)
    require(value.get('base_module_sha256') == BASE_MODULE_SHA256 and
            value.get('baseline_contract_sha256') == BASE_CONTRACT_SHA256, 'Missing frozen r26 provenance')
    for key in ('original', 'parser_source_sha256', 'protocol_header_sha256', 'source_patch_sha256'):
        require(value.get(key) == baseline[key], 'Changed inherited baseline field: ' + key)
    require(value.get('reset_settings') == RESET_SETTINGS and
            all(type(value['reset_settings'][key]) is type(want) for key, want in RESET_SETTINGS.items()),
            'Expected unique kperf return, six-second reset')
    require(value.get('review') == REVIEW_FLAGS, 'Missing independent r27 review')
    expected = value.get('expected')
    base._exact_keys(expected, ('sha256', 'changed_bytes'), 'expected')
    base._digest(expected['sha256'], 'expected.sha256')
    require(type(expected['changed_bytes']) is int and 0 < expected['changed_bytes'] <= 300, 'Bad changed byte count')
    evidence = value.get('review_evidence')
    require(isinstance(evidence, dict) and set(baseline['review_evidence']) <= set(evidence), 'Missing inherited provenance')
    for path, spec in baseline['review_evidence'].items():
        require(evidence[path] == spec, 'Changed inherited provenance: ' + path)
    for name in ('machine-review.json', 'machine-code.json', 'review_machine.py'):
        require(REVIEW_DIR + name in evidence, 'Missing r27 review: ' + name)
        spec = evidence[REVIEW_DIR + name]
        base._exact_keys(spec, ('sha256', 'bytes'), name)
        base._digest(spec['sha256'], name)
        require(type(spec['bytes']) is int and spec['bytes'] > 0, 'Bad evidence size')
    require(isinstance(value.get('layout'), dict), 'Missing r27 layout')
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
    image, old = previous.original_layout(original, baseline)
    functions = {}
    for key, (name, digest) in FUNCTIONS.items():
        functions[key] = base._function(image, name)
        require(functions[key]['sha256'] == digest, 'New reviewed CPU-wait body changed: ' + name)
    caller, callee = functions['caller'], functions['callee']
    destination = address(callee['start'])
    calls = [va for va in range(address(caller['start']), address(caller['end']), 4)
             if image.word(va) & 0xfc000000 == 0x94000000 and target(image.word(va), va, 0x94000000) == destination]
    require(calls == [0xfffffff0072c8888] and image.word(calls[0]) == 0x9404436c,
            'Expected unique kperf_init_early BL inside kernel_bootstrap_thread')
    require(image.body(destination, 12) == struct.pack('<3I', 0xd10183ff, 0xa9057bfd, 0x910143fd),
            'kperf must save caller LR on aligned 96-byte stack frame')
    require([va for va in range(destination, address(callee['end']), 4) if image.word(va) == 0xd65f03c0]
            == [0xfffffff0073d96b0], 'Unique kperf normal RET')
    require(image.body(0xfffffff0073d96a8, 12) == struct.pack('<3I', 0xa9457bfd, 0x910183ff, 0xd65f03c0),
            'kperf normal epilogue must restore original SP/LR')
    require(target(image.word(0xfffffff0073d9654), 0xfffffff0073d9654, 0x94000000)
            == address(functions['cpu_wait']['start']), 'Unconditional max-cpus gate before kperf bootarg')
    require(image.body(0xfffffff0073d9658, 12) == struct.pack('<3I', 0x4f00e400, 0xad0103e0, 0xad0003e0),
            'Normal fallthrough after wait')
    require(image.body(0xfffffff007077f77, 6) == b'kperf\0', 'Expected kperf bootarg string')
    for site, name in ((0xfffffff0072c8884, '_vm_set_restrictions'), (0xfffffff0072c888c, '_bsd_init')):
        require(target(image.word(site), site, 0x94000000) == image.names[name], 'Normal neighboring bootstrap call changed')
    old_calls = copy.deepcopy(old['old_calls'])
    old_calls['PE_lockdown_iokit'] = {'va': old['call_site'], 'word': old['original_call_word'], 'target_va': old['callee']['start']}
    layout = {key: copy.deepcopy(old[key]) for key in ('entry', 'first_cpu', 'mmu', 'post_mmu', 'caves', 'console', 'mapping')}
    layout.update({'caller': caller, 'callee': callee, 'call_site': hex(calls[0]), 'original_call_word': '0x9404436c',
                   'old_calls': old_calls, 'argument_semantics': 'void(void); no arguments added or modified',
                   'normal_return': {'function': CALLEE_NAME, 'va': '0xfffffff0073d96b0', 'word': '0xd65f03c0',
                                     'restore_va': '0xfffffff0073d96a8', 'restore_words': ['0xa9457bfd', '0x910183ff']},
                   'cpu_gate': {'functions': {key: functions[key] for key in ('cpu_wait', 'cpu_set', 'mutex_sleep')},
                                'wait_callsite': '0xfffffff0073d9654', 'state_va': '0xfffffff007afa86c',
                                'mutex_va': '0xfffffff007a90200', 'initial_state': 0, 'set_state': 1, 'wait_state': 2,
                                'sleep_only_if_not_set': True, 'sleep_has_deadline': False,
                                'return_supports_state_publication': True, 'return_proves_actual_sleep': False,
                                'return_proves_timer_irq': False, 'return_proves_processor_registration_complete': False,
                                'return_proves_matching_complete': False, 'return_proves_kperf_sampling': False,
                                'runtime_return_observed': False, 'next_unexecuted_bootstrap_call': '_bsd_init'}})
    return image, layout


def original_layout(original, expectation):
    validate_expectation(expectation)
    image, layout = _discover(original, expectation['baseline_contract'])
    require(layout == expectation['layout'], 'Frozen r27 layout differs from actual original')
    return image, layout


def expected_regions(layout):
    return base.expected_regions(layout)


def derive_expected(original, expectation):
    image, layout = original_layout(original, expectation)
    result, changed = base._derive_unreviewed(original, image, layout)
    require(sha(result) == expectation['expected']['sha256'] and changed == expectation['expected']['changed_bytes'],
            'Independent bytes differ from frozen r27 digest/count')
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
        require(low >= cursor and patched[cursor:low] == original[cursor:low], 'Mutation outside six r27 hook/cave intervals')
        count = sum(a != b for a, b in zip(patched[low:high], original[low:high]))
        changed += count
        counts.append({'va': hex(va), 'file_start': hex(low), 'file_end': hex(high), 'changed_bytes': count})
        cursor = high
    require(patched[cursor:] == original[cursor:], 'Mutation outside r27 intervals in kernel tail')
    require(changed == expectation['expected']['changed_bytes'] and sha(patched) == expectation['expected']['sha256'],
            'Candidate differs from frozen r27 digest/count')
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
            'old_init_call_restored': True, 'old_configure_call_restored': True,
            'old_machine_lockdown_call_restored': True, 'old_PE_lockdown_iokit_call_restored': True,
            'original_sp_preserved_before_call': True, 'argument_semantics': layout['argument_semantics'],
            'normal_return': layout['normal_return'], 'mapping': layout['mapping'], 'cpu_gate': layout['cpu_gate'],
            'hardware_tested': False, 'source_crypto_seal_changed_by_probe': False, 'permitted_regions': counts,
            'scope': 'Host bytes only. Return after unconditional ml_wait_max_cpus supports state publication, '
                     'but not actual sleep, complete matching, processor registration, timer IRQ or kperf sampling. '
                     'If gate never completes the six-second reset wait does not start. Stops before bsd_init.'}


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
