#!/usr/bin/env python3
"""Frozen r26 PE_lockdown_iokit-return kernel contract, independent of patcher.

The selected callee tail-calls StartIOKitMatching. Its return is not a matching
completion barrier and does not certify worker execution or device readiness.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
BASE_MODULE_SHA256 = '34cbd2a965faa449c05a92ff2ff01012234a14cf81c7c1ecae242924d2220f06'
BASE_CONTRACT_SHA256 = '9b2230e304e9f279d83e64e9a87fb42ff859256540f78932e5919cad6cdb79e7'
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
CALLER_NAME, CALLEE_NAME = '_kernel_bootstrap_thread', '_PE_lockdown_iokit'
RESET_SETTINGS = dict(base.RESET_SETTINGS, entry_probe_reset_after=CALLEE_NAME, entry_probe_reset_in=CALLER_NAME)
DELAY_TICKS, CAVE_BYTES = base.DELAY_TICKS, base.CAVE_BYTES
PARSER_SOURCE_SHA256 = base.PARSER_SOURCE_SHA256
PROTOCOL_HEADER_SHA256 = base.PROTOCOL_HEADER_SHA256
SOURCE_PATCH_SHA256 = base.SOURCE_PATCH_SHA256
FUNCTIONS = {
    'caller': (CALLER_NAME, 'deb92769543f8bc19efc93cb42bac63f922f1ab7b7db1a72dc30a2446b2f460a'),
    'callee': (CALLEE_NAME, '5769c9699a92d9c5091eef0d932914344e2859943ea560e55cbc49eec50f4daa'),
    'start_matching': ('_StartIOKitMatching', '4aca6fd4bbfaed4e1cd8fb4530d33a7114327bc14a7e07b001bf5afb6489d309'),
    'start_service_matching': ('__ZN22IOPlatformExpertDevice22startIOServiceMatchingEv', '97c7e439d991a438063505e08a163b87937f5d4f3a4a7fcd67d5e735756e72b1'),
    'register_service': ('__ZN9IOService15registerServiceEj', 'cbd3d1f535b3663da689592f756ce81f4f666bf55e762f581d5dd9a4d76d4076'),
    'service_start_matching': ('__ZN9IOService13startMatchingEj', '997f8dde647c185eba0d11e038332ec9468203873bd01a1e366ee162f4b13a90'),
    'start_job': ('__ZN13_IOServiceJob8startJobEP9IOServiceij', '2c8cb976d007fc08098fde43f01da41de07622034bfa266028cbb380cae3926c'),
    'get_provider': ('__ZNK9IOService11getProviderEv', 'b55a98dafbd5966d4f3beb91248ad80d7e7ffda2d0bb62f58ec3a87a514e2ecb'),
    'machine_lockdown': ('_machine_lockdown', '1c2a9e934d91ffa9536ed159adcbfabde9654fe56c6218fafbfaa90a1f6dc0f2'),
    'prot_init': ('_arm_vm_prot_init', 'c6a0ceb1b4a1023fa88e118fe3ecfe47994bf9e76893355732cb9f6c4e3cb588'),
    'prot_finalize': ('_arm_vm_prot_finalize', 'e767439b7d5ba674cdc1160d2a90db923ac08728a7edd137c1820ff3ef9a96ea'),
    'replace_bootstack': ('_arm64_replace_bootstack', '3d194475ffb3315a69547d412b22e3b3f88fb20ceac840126a174806e873f8a7'),
}
REVIEW_DIR = 'analysis/xnu-iokit-matching-r26-20261114/'
REVIEW_FLAGS = {'machine_source_and_abi_reviewed': True,
                'post_lockdown_cave_mapping_reviewed': True,
                'asynchronous_matching_limits_reviewed': True,
                'independent_expected_bytes_reviewed': True}


def validate_expectation(value):
    require(isinstance(value, dict) and type(value.get('schema_version')) is int and value['schema_version'] == 1,
            'Unsupported r26 expectation')
    require(value.get('status') == 'independently-frozen', 'Unreviewed r26 contract')
    baseline = value.get('baseline_contract')
    require(sha(canonical(baseline)) == BASE_CONTRACT_SHA256, 'Frozen new CPU0 source/layout contract changed')
    base.validate_expectation(baseline)
    require(value.get('base_module_sha256') == BASE_MODULE_SHA256 and
            value.get('baseline_contract_sha256') == BASE_CONTRACT_SHA256, 'Missing pinned baseline provenance')
    for key in ('original', 'parser_source_sha256', 'protocol_header_sha256', 'source_patch_sha256'):
        require(value.get(key) == baseline[key], 'Changed baseline field: ' + key)
    require(value.get('reset_settings') == RESET_SETTINGS and
            all(type(value['reset_settings'][key]) is type(want) for key, want in RESET_SETTINGS.items()),
            'Expected PE_lockdown_iokit occurrence 1 return, six-second reset')
    require(value.get('review') == REVIEW_FLAGS, 'Missing r26 independent review')
    expected = value.get('expected')
    base._exact_keys(expected, ('sha256', 'changed_bytes'), 'expected')
    base._digest(expected['sha256'], 'expected.sha256')
    require(type(expected['changed_bytes']) is int and 0 < expected['changed_bytes'] <= 300, 'Bad changed byte count')
    evidence = value.get('review_evidence')
    require(isinstance(evidence, dict) and set(baseline['review_evidence']) <= set(evidence), 'Missing inherited review provenance')
    for path, spec in baseline['review_evidence'].items():
        require(evidence[path] == spec, 'Changed inherited review provenance: ' + path)
    for name in ('machine-review.json', 'machine-code.json', 'review_machine.py'):
        require(REVIEW_DIR + name in evidence, 'Missing r26 review: ' + name)
        spec = evidence[REVIEW_DIR + name]
        base._exact_keys(spec, ('sha256', 'bytes'), name)
        base._digest(spec['sha256'], name)
        require(type(spec['bytes']) is int and spec['bytes'] > 0, 'Bad review evidence size')
    require(isinstance(value.get('layout'), dict), 'Missing r26 layout')
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
        require(functions[key]['sha256'] == expected_hash, 'Reviewed matching chain body changed: ' + name)
    caller, callee = functions['caller'], functions['callee']
    destination = address(callee['start'])
    calls = [va for va in range(address(caller['start']), address(caller['end']), 4)
             if image.word(va) & 0xfc000000 == 0x94000000 and
             target(image.word(va), va, 0x94000000) == destination]
    require(calls == [0xfffffff0072c8874] and image.word(calls[0]) == 0x941b77f7,
            'Expected unique outer PE_lockdown_iokit BL inside kernel_bootstrap_thread')
    require(image.body(destination, 8) == struct.pack('<2I', 0xa9bf7bfd, 0x910003fd), 'PE LR/SP save')
    require(target(image.word(0xfffffff0079a6858), 0xfffffff0079a6858, 0x94000000)
            == image.names['_zalloc_iokit_lockdown'], 'PE zalloc lockdown call')
    require(image.word(0xfffffff0079a685c) == 0xa8c17bfd, 'PE LR/SP restore before tail call')
    require(target(image.word(0xfffffff0079a6860), 0xfffffff0079a6860, 0x14000000)
            == address(functions['start_matching']['start']), 'PE must tail B to StartIOKitMatching')
    require(not any(image.word(va) == 0xd65f03c0 for va in range(destination, address(callee['end']), 4)),
            'PE normal return belongs to tail callee')
    start, end = map(address, (functions['start_matching']['start'], functions['start_matching']['end']))
    require([va for va in range(start, end, 4) if image.word(va) == 0xd65f03c0] == [0xfffffff0078e6bac],
            'Unique StartIOKitMatching normal RET')
    require(image.body(0xfffffff0078e6ba4, 12) == struct.pack('<3I', 0xa9417bfd, 0x910083ff, 0xd65f03c0),
            'Tail callee restores outer wrapper LR and original SP')
    require(image.body(0xfffffff0078e6b90, 12) == struct.pack('<3I', 0xf0ffc6a8, 0xf9414900, 0xb40000c0)
            and image.names['_gRootNub'] == 0xfffffff0071bd290, 'gRootNub load and null assertion branch')
    require(target(image.word(0xfffffff0078e6b9c), 0xfffffff0078e6b9c, 0x94000000)
            == address(functions['start_service_matching']['start']), 'Actual startIOServiceMatching call')
    require(image.word(0xfffffff0078e6ba0) == 0x36000140, 'False workloop result enters panic')
    require(image.body(0xfffffff00793a948, 8) == struct.pack('<2I', 0x52800001, 0xd63f0100),
            'registerService virtual call gets options zero')
    vtable_slot = image.names['__ZTV22IOPlatformExpertDevice'] + 16 + 0x2a0
    require(struct.unpack('<Q', image.body(vtable_slot, 8))[0] == address(functions['register_service']['start']),
            'Actual root-class registerService vtable target')
    old_calls = {'InitIOKit': {'va': old['call_site'], 'word': old['original_call_word'], 'target_va': old['callee']['start']},
                 'ConfigureIOKit': dict(old['configure_call']),
                 'machine_lockdown': {'va': '0xfffffff0072c87f4', 'word': '0x94054d63',
                                      'target_va': functions['machine_lockdown']['start']}}
    for item in old_calls.values():
        require(image.word(address(item['va'])) == int(item['word'], 16), 'Old selected call not original')
    # Retained static mapping reviewed for r25; same original bytes and cave page.
    require(image.body(0xfffffff00740fd50, 36) == struct.pack('<9I',
            0xd0ffeb08, 0xf9410100, 0xd503201f, 0xf9410501, 0xd2800002,
            0x52800003, 0x52800044, 0x52800005, 0x52800066), 'Actual TEXT_EXEC ROX arguments')
    require(target(image.word(0xfffffff00740fd74), 0xfffffff00740fd74, 0x94000000)
            == image.names['_arm_vm_page_granular_prot'], 'Actual TEXT_EXEC protection helper')
    executable = _segments(original)['__TEXT_EXEC']
    require(executable['maxprot'] == executable['initprot'] == 5, 'Kernel text readable and executable')
    for va in (address(old['caves'][stage]) for stage in ('1', '2', '3', '4')):
        require(address(executable['start']) <= va and va + CAVE_BYTES <= address(executable['end']), 'Cave escaped TEXT_EXEC')
        require([(seg, sec) for seg, sec, low, size in image.sections if low <= va and va + CAVE_BYTES <= low + size]
                == [('__TEXT_EXEC', '__text')], 'Cave escaped ordinary text section')
        require(va >> 12 == address(old['caves']['4']) >> 12, 'Caves not in reviewed common 4KiB page')
    layout = {key: old[key] for key in ('entry', 'first_cpu', 'mmu', 'post_mmu', 'caves', 'console')}
    layout.update({'caller': caller, 'callee': callee, 'call_site': hex(calls[0]),
                   'original_call_word': '0x941b77f7', 'old_calls': old_calls,
                   'argument_semantics': 'void(void); PE tail callee loads gRootNub internally',
                   'normal_return': {'function': '_StartIOKitMatching', 'va': '0xfffffff0078e6bac',
                                     'word': '0xd65f03c0', 'restore_va': '0xfffffff0078e6ba4',
                                     'restore_words': ['0xa9417bfd', '0x910083ff'],
                                     'tail_branch_va': '0xfffffff0079a6860'},
                   'matching': {'functions': {k: functions[k] for k in ('start_matching', 'start_service_matching',
                                  'register_service', 'service_start_matching', 'start_job', 'get_provider')},
                                'register_service_options': 0, 'root_class_register_service_vtable_slot': hex(vtable_slot),
                                'root_job_path': 'asynchronous if ordinary registry/provider conditions reach startJob',
                                'return_proves_matching_complete': False, 'return_proves_job_submitted': False,
                                'worker_mmio_may_run_before_reset': True, 'offline_board_drivers_added': False},
                   'mapping': {'functions': {k: functions[k] for k in ('machine_lockdown', 'prot_init', 'prot_finalize', 'replace_bootstack')},
                               'text_exec': executable, 'cave_page': hex(address(old['caves']['4']) & ~0xfff),
                               'text_exec_rox_call': '0xfffffff00740fd74', 'runtime_mapping_verified': False}})
    return image, layout


def original_layout(original, expectation):
    validate_expectation(expectation)
    image, layout = _discover(original, expectation['baseline_contract'])
    require(layout == expectation['layout'], 'Frozen r26 layout differs from actual original')
    return image, layout


def expected_regions(layout):
    return base.expected_regions(layout)


def derive_expected(original, expectation):
    image, layout = original_layout(original, expectation)
    result, changed = base._derive_unreviewed(original, image, layout)
    require(sha(result) == expectation['expected']['sha256'] and changed == expectation['expected']['changed_bytes'],
            'Independent bytes differ from frozen r26 digest/count')
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
        require(low >= cursor and patched[cursor:low] == original[cursor:low], 'Mutation outside six r26 hook/cave intervals')
        count = sum(a != b for a, b in zip(patched[low:high], original[low:high]))
        changed += count
        counts.append({'va': hex(va), 'file_start': hex(low), 'file_end': hex(high), 'changed_bytes': count})
        cursor = high
    require(patched[cursor:] == original[cursor:], 'Mutation outside r26 intervals in kernel tail')
    require(changed == expectation['expected']['changed_bytes'] and sha(patched) == expectation['expected']['sha256'],
            'Candidate differs from frozen r26 digest/count')
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
            'old_init_call_restored': True, 'old_configure_call_restored': True, 'old_machine_lockdown_call_restored': True,
            'original_sp_preserved_before_call': True, 'argument_semantics': layout['argument_semantics'],
            'normal_return': layout['normal_return'], 'mapping': layout['mapping'], 'matching': layout['matching'],
            'general_driver_matching_requested_by_checkpoint': True,
            'general_driver_matching_completion_proved': False,
            'runtime_pe_lockdown_iokit_return_observed': False, 'hardware_tested': False,
            'source_crypto_seal_changed_by_probe': False, 'permitted_regions': counts,
            'scope': 'Host bytes only. Outer PE_lockdown_iokit normal-return checkpoint after its tail StartIOKitMatching. '
                     'Return implies successful root workloop creation and registerService return, not job submission or matching completion. '
                     'Workers and MMIO may run during the six-second wait; the checkpoint is not a driver/IRQ/device readiness test.'}


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
