#!/usr/bin/env python3
"""Discover/review a NEW r31 Power-diagnostic kernel; independently verify its after-inittodr return probe.

No old kernel address or digest is accepted. Discovery is unreviewed and cannot
authorize a candidate. The six permitted diagnostic regions are derived from
the new executable and checked against a separately reviewed frozen contract.
"""
import argparse
from collections import Counter
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

sys.dont_write_bytecode = True
TOOLS = Path(__file__).resolve().parents[1] / 'tools'
HELPER = TOOLS / 'verify_xnu_early_log_kernel.py'
HELPER_SHA256 = '34cbd2a965faa449c05a92ff2ff01012234a14cf81c7c1ecae242924d2220f06'

def require(condition, message):
    if not condition:
        raise ValueError(message)

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':')).encode()

require(sha(HELPER.read_bytes()) == HELPER_SHA256, 'Independent discovery/encoder helper changed')
sys.path.insert(0, str(TOOLS))
import verify_xnu_early_log_kernel as base
require(Path(base.__file__).resolve() == HELPER, 'Wrong helper import')
MachO, branch, target, address = base.MachO, base.branch, base.target, base.address
DELAY_TICKS, CAVE_BYTES = base.DELAY_TICKS, base.CAVE_BYTES
CALLER_NAME, CALLEE_NAME = '_bsd_init', '_inittodr'
RESET_SETTINGS = dict(base.RESET_SETTINGS, entry_probe_reset_in=CALLER_NAME,
                      entry_probe_reset_after=CALLEE_NAME)
REVIEW_KEYS = ('new_source_build_verified', 'storage_init_and_access_gates_reviewed',
               'early_log_source_and_binary_verified', 'layout_and_return_abi_reviewed',
               'root_boundary_reviewed', 'independent_expected_bytes_reviewed',
               'power_read_scope_and_fault_model_reviewed')
POWER_NAME = '_j716f_storage_power_probe'
BRINGUP_NAMES = tuple('_j716f_storage_bringup_' + name for name in
                      ('init', 'status', 'observe', 'allocate', 'activate', 'read_known', 'stop', 'discard_unused'))
OWNER_NAMES = tuple('_apps_smmu_owner_' + name for name in
                   ('install_handoff', 'try_acquire', 'validate', 'validate_ready',
                    'publication_begin', 'publication_complete', 'queue_started',
                    'mark_dma_possible', 'complete_dma', 'confirm_stopped',
                    'confirm_unpublished', 'quarantine', 'release'))
RUNTIME_NAMES = tuple('_sm8250_ufs_' + name for name in
                     ('adapter_init', 'allocate', 'activate', 'read_known', 'stop', 'discard_unused', 'status', 'observe'))
MARKERS = {
    'pre_root': b'[J716F boot] BSD pre-root reached; inittodr next; root lookup not reached\n\0',
    'iobsd_result': b'[J716F boot] IOKitBSDInit returned result=0x%x\n\0',
    'registration_skipped': b'[J716F storage] registration skipped; IOKitBSDInit failed\n\0',
}

def _defined_counts(raw):
    """Count individual N_SECT records, also rejecting repeated same-address names."""
    cursor, symtab = 32, None
    for _ in range(struct.unpack_from('<I', raw, 16)[0]):
        command, size = struct.unpack_from('<II', raw, cursor)
        if command == 2:
            require(symtab is None, 'Duplicate symbol table')
            symtab = struct.unpack_from('<4I', raw, cursor + 8)
        cursor += size
    require(symtab is not None, 'Missing symbol table')
    offset, count, strings, length = symtab
    result = Counter()
    for index in range(count):
        string, kind, _, _, _ = struct.unpack_from('<IBBHQ', raw, offset + index * 16)
        if kind & 0x0e != 0x0e or kind & 0xe0:
            continue
        end = raw.find(b'\0', strings + string, strings + length)
        require(end >= 0, 'Unterminated symbol name')
        result[raw[strings + string:end].decode()] += 1
    return result

def _calls(image, function, name):
    destination = base._symbol(image, name)
    low, high = address(function['start']), address(function['end'])
    return [va for va in range(low, high, 4)
            if image.word(va) & 0xfc000000 == 0x94000000
            and target(image.word(va), va, 0x94000000) == destination]

def _one_call(image, function, name):
    hits = _calls(image, function, name)
    require(len(hits) == 1, 'Expected one direct call in ' + function['name'] + ': ' + name)
    va = hits[0]
    return {'va': hex(va), 'word': f'0x{image.word(va):08x}',
            'target_va': hex(base._symbol(image, name)), 'symbol': name}

def _signed21(word):
    value = ((word >> 5) & 0x7ffff) * 4 + ((word >> 29) & 3)
    return value - (1 << 21) if value & (1 << 20) else value

def _constant_x0(image, start, end, value):
    """Small independent ADR / adjacent ADRP+ADD decoder, not text disassembly."""
    found = []
    for va in range(start, end, 4):
        word = image.word(va)
        if word & 0x9f00001f == 0x10000000 and va + _signed21(word) == value:
            found.append({'va': hex(va), 'words': [f'0x{word:08x}'], 'kind': 'ADR x0'})
        elif word & 0x9f00001f == 0x90000000 and va + 8 <= end:
            add = image.word(va + 4)
            if add & 0xffc003ff == 0x91000000:
                materialized = (va & ~0xfff) + (_signed21(word) << 12) + ((add >> 10) & 0xfff)
                if materialized == value:
                    found.append({'va': hex(va), 'words': [f'0x{word:08x}', f'0x{add:08x}'], 'kind': 'ADRP/ADD x0'})
    return found

def _string(image, raw, marker):
    hits, pos = [], 0
    while True:
        pos = raw.find(marker, pos)
        if pos < 0:
            break
        hits.append(pos)
        pos += 1
    require(len(hits) == 1, 'Missing/duplicate exact startup message')
    offset = hits[0]
    values = [vm + offset - off for vm, count, off in image.segments
              if off <= offset and offset + len(marker) <= off + count]
    require(len(values) == 1, 'Startup message is not uniquely file-backed')
    return values[0]

def _discover(raw):
    image, entry_layout = base._discover(raw)
    functions = {key: base._function(image, name) for key, name in
                 (('caller', CALLER_NAME), ('callee', CALLEE_NAME), ('autoconf', '_bsd_autoconf'))}
    caller, callee = functions['caller'], functions['callee']
    call = _one_call(image, caller, CALLEE_NAME)
    call_va, callee_va = address(call['va']), address(callee['start'])
    require(image.word(call_va - 4) == 0xd2800000, 'inittodr argument is not original MOV x0,#0')
    require(image.body(callee_va, 12) == struct.pack('<3I', 0xd10083ff, 0xa9017bfd, 0x910043fd),
            'Review new inittodr prologue: expected aligned 32-byte frame saving original LR')
    rets = [va for va in range(callee_va, address(callee['end']), 4) if image.word(va) == 0xd65f03c0]
    require(len(rets) == 1, 'Review non-unique inittodr return')
    ret = rets[0]
    require(image.body(ret - 8, 12) == struct.pack('<3I', 0xa9417bfd, 0x910083ff, 0xd65f03c0),
            'inittodr does not restore original LR/SP on normal return')
    require(not any(image.word(va) & 0xfffffc1f == 0xd61f0000
                    for va in range(callee_va, address(callee['end']), 4)),
            'Unexpected indirect tail from inittodr; independent ABI review required')
    calendar = [_one_call(image, callee, name) for name in
                ('_clock_get_calendar_microtime', '_clock_set_calendar_microtime')]
    root = {name: _one_call(image, caller, name) for name in ('_IOFindBSDRoot', '_vfs_mountroot')}
    require(call_va < address(root['_IOFindBSDRoot']['va']) < address(root['_vfs_mountroot']['va']),
            'Selected checkpoint is not before root discovery and mounting')
    require(not any(image.word(va) & 0xfc000000 == 0x94000000
                    for va in range(call_va + 4, address(root['_IOFindBSDRoot']['va']), 4)),
            'New call appears between inittodr and first root discovery')
    counts = _defined_counts(raw)
    exports = {}
    for name in BRINGUP_NAMES + OWNER_NAMES + RUNTIME_NAMES + (POWER_NAME,):
        require(counts[name] == 1, 'Expected one actual symbol-table definition: ' + name)
        exports[name] = base._function(image, name)
    require({name for name in counts if name.startswith('_apps_smmu_owner_')} == set(OWNER_NAMES),
            'Unexpected owner API definitions')
    autoconf_call = _one_call(image, caller, '_bsd_autoconf')
    require(address(autoconf_call['va']) < call_va, 'Autoconf is not before the checkpoint')
    require(not _calls(image, caller, BRINGUP_NAMES[0]) and not _calls(image, caller, POWER_NAME) and not _calls(image, caller, '_IOKitBSDInit'),
            'Review changed compiler layout: autoconf is expected to be out of line')
    init_calls = {'autoconf': _one_call(image, functions['autoconf'], BRINGUP_NAMES[0])}
    iobsd_calls = {'autoconf': _one_call(image, functions['autoconf'], '_IOKitBSDInit')}
    power_call = _one_call(image, functions['autoconf'], POWER_NAME)
    iobsd_va, init_va = address(iobsd_calls['autoconf']['va']), address(init_calls['autoconf']['va'])
    power_va = address(power_call['va'])
    require(power_va == init_va + 4, 'Fixed Power probe must immediately follow registration')
    require(image.word(iobsd_va + 4) == 0xaa0003f3, 'IOKitBSDInit result is not retained in x19')
    gate_va = init_va - 20
    gate_word = image.word(gate_va)
    require(gate_word & 0xff00001f == 0x34000013, 'Storage registration needs CBZ w19 success gate')
    imm19 = (gate_word >> 5) & 0x7ffff
    imm19 -= (1 << 19) if imm19 & (1 << 18) else 0
    require(iobsd_va < gate_va and gate_va + imm19 * 4 == init_va,
            'Only zero IOKitBSDInit result may branch to storage registration')
    require(image.word(init_va - 4) & 0xfc000000 == 0x14000000 and
            target(image.word(init_va - 4), init_va - 4, 0x14000000) == power_va + 4,
            'Nonzero IOKitBSDInit path does not skip storage registration')
    require(image.word(power_va + 4) == 0xaa1303e0, 'Autoconf must return saved IOKitBSDInit result')
    success_gate = {'va': hex(gate_va), 'word': f'0x{gate_word:08x}',
                    'zero_target': hex(init_va), 'failure_skip_va': hex(init_va - 4),
                    'failure_target': hex(power_va + 4), 'saved_result_register': 'x19'}
    messages = {name: {'string_va': hex(_string(image, raw, marker)),
                       'bytes_sha256': sha(marker)} for name, marker in MARKERS.items()}
    print_calls = _calls(image, caller, '_printf')
    references = _constant_x0(image, address(caller['start']), call_va,
                             int(messages['pre_root']['string_va'], 16))
    require(len(references) == 1, 'Expected one pre-inittodr x0 startup message materialization')
    ref = references[0]
    end = address(ref['va']) + 4 * len(ref['words'])
    candidates = [va for va in print_calls if end <= va <= end + 24 and va < call_va]
    require(len(candidates) == 1, 'Expected bounded pre-root printf immediately after its message')
    messages['pre_root'].update(materialization=ref, printf_va=hex(candidates[0]),
                                 before_inittodr=True)
    group = [_one_call(image, caller, name) for name in (
        '_dtrace_postinit', '_loopattach', '_ether_family_init', '_cfil_init', '_pkt_mnglr_init',
        '_utun_register_control', '_ipsec_init', '_netsrc_init', '_nstat_init',
        '_mptcp_control_register', '_if_redirect_init', '_kctl_test_init', '_net_init_run')]
    group_addresses = [address(x['va']) for x in group]
    require(group_addresses == sorted(group_addresses) and group_addresses[-1] < candidates[0] < call_va,
            'Grouped DTrace/network/pre-root message order changed')
    earlylog = {name: base._function(image, name) for name in
                ('_PE_j716f_earlylog_init', '_PE_j716f_earlylog_write')}
    layout = {key: copy.deepcopy(entry_layout[key]) for key in
              ('entry', 'first_cpu', 'mmu', 'post_mmu', 'caves', 'console')}
    layout.update(caller=caller, callee=callee, call_site=call['va'], original_call_word=call['word'],
                  argument_semantics='void inittodr(time_t base), original x0=0 and original SP retained',
                  normal_return={'va': hex(ret), 'restore_va': hex(ret - 8),
                                 'restore_words': ['0xa9417bfd', '0x910083ff'], 'word': '0xd65f03c0'},
                  original_iokit_audit={key: entry_layout[key] for key in
                      ('caller', 'callee', 'call_site', 'original_call_word', 'argument', 'direct_return', 'optional_tail', 'configure_call')},
                  early_log=earlylog,
                  storage={'exports': exports, 'init_calls': init_calls, 'iobsd_calls': iobsd_calls,
                           'autoconf': functions['autoconf'], 'autoconf_call': autoconf_call,
                           'success_gate': success_gate, 'power_call': power_call,
                           'power_probe_symbol': POWER_NAME, 'power_read_scope_requires_review': True,
                           'messages': messages, 'unique_owner_export_definitions': True,
                           'automatic_hardware_access_proved': False, 'runtime_registration_observed': False},
                  bsd_boundary={'calendar_calls': calendar, 'grouped_calls': group, 'later_calls': root,
                                'stops_before_root_discovery_and_mount': True,
                                'prior_initialization_can_block_or_start_workers': True,
                                'runtime_return_observed': False, 'timer_irq_delivery_proved': False,
                                'storage_capacity_proved': False, 'known_block_read_proved': False,
                                'root_access_exercised': False, 'runtime_power_read_observed': False,
                                'external_abort_containment_proved': False})
    return image, layout

def expected_regions(layout):
    return base.expected_regions(layout)

def discover_layout(raw):
    image, layout = _discover(raw)
    patched, changed = base._derive_unreviewed(raw, image, layout)
    return {'schema_version': 1, 'status': 'unreviewed-discovery',
            'helper_sha256': HELPER_SHA256, 'parser_source_sha256': base.PARSER_SOURCE_SHA256,
            'original': {'sha256': sha(raw), 'bytes': len(raw)}, 'layout': layout,
            'reset_settings': dict(RESET_SETTINGS),
            'expected': {'sha256': sha(patched), 'changed_bytes': changed},
            'review': {key: False for key in REVIEW_KEYS}, 'review_evidence': {}}

def validate_expectation(value):
    require(isinstance(value, dict) and type(value.get('schema_version')) is int and value['schema_version'] == 1,
            'Unsupported storage-kernel expectation')
    require(value.get('status') == 'independently-frozen', 'Unreviewed discovery cannot accept an image')
    require(value.get('helper_sha256') == HELPER_SHA256 and
            value.get('parser_source_sha256') == base.PARSER_SOURCE_SHA256, 'Wrong helper/parser provenance')
    require(value.get('reset_settings') == RESET_SETTINGS and
            all(type(value['reset_settings'][key]) is type(want) for key, want in RESET_SETTINGS.items()),
            'Only after-inittodr return, occurrence one, six seconds is permitted')
    require(value.get('review') == {key: True for key in REVIEW_KEYS} and
            all(value['review'][key] is True for key in REVIEW_KEYS), 'Independent source/machine review incomplete')
    for name, fields in (('original', ('sha256', 'bytes')), ('expected', ('sha256', 'changed_bytes'))):
        base._exact_keys(value.get(name), fields, name)
        base._digest(value[name]['sha256'], name)
    require(type(value['original']['bytes']) is int and 32 < value['original']['bytes'] < 1 << 30,
            'Invalid source kernel length')
    require(type(value['expected']['changed_bytes']) is int and 0 < value['expected']['changed_bytes'] <= 300 and
            value['original']['sha256'] != value['expected']['sha256'], 'Invalid expected modification')
    evidence = value.get('review_evidence')
    require(isinstance(evidence, dict) and len(evidence) >= 3, 'Pin source, build and actual machine review evidence')
    for name, item in evidence.items():
        require(isinstance(name, str) and name and not name.startswith('/') and ':' not in name and '\\' not in name
                and all(p not in ('', '.', '..') for p in name.split('/')), 'Unsafe review evidence path')
        base._exact_keys(item, ('bytes', 'sha256'), name)
        base._digest(item['sha256'], name)
        require(type(item['bytes']) is int and item['bytes'] > 0, 'Invalid evidence length')
    require(isinstance(value.get('layout'), dict), 'Missing actual executable layout')
    return value

def load_expectation(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'Duplicate expectation key: ' + key)
            result[key] = value
        return result
    return validate_expectation(json.loads(Path(path).read_text(), object_pairs_hook=unique))

def original_layout(raw, expectation):
    validate_expectation(expectation)
    require(len(raw) == expectation['original']['bytes'] and sha(raw) == expectation['original']['sha256'],
            'Source kernel is not the newly reviewed uninstrumented executable')
    image, layout = _discover(raw)
    require(canonical(layout) == canonical(expectation['layout']), 'Actual source layout differs from frozen review')
    return image, layout

def derive_expected(raw, expectation):
    image, layout = original_layout(raw, expectation)
    proposed, changed = base._derive_unreviewed(raw, image, layout)
    require(sha(proposed) == expectation['expected']['sha256'] and changed == expectation['expected']['changed_bytes'],
            'Independent encoding disagrees with frozen expected bytes')
    return proposed

def probe_readback(original, candidate, expectation):
    image, layout = original_layout(original, expectation)
    require(len(candidate) == len(original), 'Kernel length changed')
    spans = []
    for va, expected in expected_regions(layout):
        offset = image.offset(va, len(expected))
        require(candidate[offset:offset + len(expected)] == expected, 'Wrong exact probe bytes at ' + hex(va))
        spans.append((offset, offset + len(expected), va))
    cursor, changed, regions = 0, 0, []
    for low, high, va in sorted(spans):
        require(low >= cursor and candidate[cursor:low] == original[cursor:low],
                'Mutation outside six approved probe regions')
        count = sum(a != b for a, b in zip(candidate[low:high], original[low:high]))
        changed += count
        regions.append({'va': hex(va), 'file_start': low, 'file_end': high, 'changed_bytes': count})
        cursor = high
    require(candidate[cursor:] == original[cursor:], 'Mutation in unapproved kernel tail')
    require(sha(candidate) == expectation['expected']['sha256'] and changed == expectation['expected']['changed_bytes'],
            'Candidate digest/count differs from reviewed reference')
    parsed = MachO(candidate)
    require(parsed.names == image.names and parsed.entry == image.entry and parsed.segments == image.segments and
            parsed.sections == image.sections and candidate[:image.header_end] == original[:image.header_end],
            'Executable metadata changed')
    require(parsed.body(address(layout['caves']['3']), CAVE_BYTES) == bytes(CAVE_BYTES), 'Stage 3 must remain disabled')
    return {'status': 'passed-host-byte-verification', 'sha256': sha(candidate), 'bytes': len(candidate),
            'original_sha256': sha(original), 'changed_bytes': changed, 'permitted_regions': regions,
            'caller': CALLER_NAME, 'callee': CALLEE_NAME, 'call_site': layout['call_site'],
            'reset_after_callee_returns': True, 'reset_delay_seconds': 6, 'delay_ticks': DELAY_TICKS,
            'timebase_hz': 19200000, 'stage3_disabled': True, 'all_other_bytes_identical': True,
            'header_and_symbols_unchanged': True, 'source_crypto_seal_changed_by_probe': False,
            'storage': layout['storage'], 'bsd_boundary': layout['bsd_boundary'], 'hardware_tested': False,
            'scope': 'New source kernel includes explicit bounded Power experiment after registration. Exact six-region return probe only. '
                     'No successful Power read, abort containment, ownership, DMA, capacity, block-read, IRQ, root or userland success inferred.'}

discover = discover_layout
verify_candidate = probe_readback

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('candidate', type=Path, nargs='?')
    parser.add_argument('--discover', action='store_true')
    parser.add_argument('--expectation', type=Path)
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.discover:
        require(args.candidate is None and args.expectation is None, 'Discovery cannot accept an image')
        result = discover_layout(args.original.read_bytes())
    else:
        require(args.candidate is not None and args.expectation is not None, 'Candidate and frozen expectation required')
        result = probe_readback(args.original.read_bytes(), args.candidate.read_bytes(), load_expectation(args.expectation))
    if args.report:
        with args.report.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
    print(json.dumps(result, indent=2))
