#!/usr/bin/env python3
"""Independent r21 text-console kernel / InitIOKit-return probe verifier.

Discovery is deliberately unreviewed. Acceptance needs a separately reviewed,
frozen expectation for the new, uninstrumented source build. No old absolute
addresses, old paint helper, official patcher, compiler, VM or device are used.
Only the six official hook/cave regions may change from that new baseline.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

sys.dont_write_bytecode = True

PARSER_SOURCE_SHA256 = '8ff6044aeeb96bcfbb62857af58e7ad798b50154d5c37ad54d6ddc673c51f030'
PARSER_SOURCE = Path(__file__).resolve().with_name('prepare_r6_followup.py')
LEGACY_ORIGINAL_SHA256 = '7970051f50e280b50e42f5ed372ea095dc8646a36fb92e4716f85349bd6118f2'
CALLER_NAME, CALLEE_NAME = '_PE_init_iokit', '_InitIOKit'
DELAY_TICKS, CAVE_BYTES = 115200000, 96
CAVE_ENTRY_OFFSETS = {'1': 0x78, '2': 0xd8, '3': 0x138, '4': 0x198}
RESET_SETTINGS = {
    'entry_probe_reset_delay': 6,
    'entry_probe_reset_after': CALLEE_NAME,
    'entry_probe_reset_in': CALLER_NAME,
    'entry_probe_reset_occurrence': 1,
    'entry_probe_reset_before': False,
}
REVIEW_KEYS = ('new_uninstrumented_build_verified', 'text_console_source_and_binary_verified',
              'layout_and_return_paths_reviewed', 'independent_expected_bytes_reviewed',
              'source_crypto_seal_not_changed_by_probe')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


require(PARSER_SOURCE.is_file() and sha(PARSER_SOURCE.read_bytes()) == PARSER_SOURCE_SHA256,
        'Independent Mach-O parser source does not match its frozen provenance')
# These three helpers do not access the old module's address/paint constants.
from prepare_r6_followup import MachO, branch, target
require(Path(sys.modules[MachO.__module__].__file__).resolve() == PARSER_SOURCE,
        'Independent Mach-O parser imported from the wrong location')


def address(value):
    require(isinstance(value, str) and re.fullmatch(r'0x[0-9a-f]+', value) is not None,
            'Address must be a canonical lower-case hexadecimal string')
    result = int(value, 16)
    require(0 < result < (1 << 64) and hex(result) == value and result % 4 == 0,
            'Address is zero, unaligned, outside arm64 or non-canonical')
    return result


def _digest(value, field):
    require(isinstance(value, str) and re.fullmatch(r'[0-9a-f]{64}', value) is not None
            and len(set(value)) > 1, 'Missing/invalid frozen SHA256: ' + field)


def _word(value):
    require(isinstance(value, str) and re.fullmatch(r'0x[0-9a-f]{8}', value) is not None,
            'Instruction word must contain exactly eight hexadecimal digits')
    return int(value, 16)


def _exact_keys(value, keys, label):
    require(isinstance(value, dict) and set(value) == set(keys),
            'Missing/unexpected ' + label + ' fields')


def validate_expectation(value):
    """Validate every acceptance entry point, not merely the JSON loader."""
    require(isinstance(value, dict) and value.get('schema_version') == 1 and
            type(value.get('schema_version')) is int, 'Unsupported expectation schema')
    require(value.get('status') == 'independently-frozen',
            'New kernel expectation is not independently frozen; pending/discovery cannot authorize acceptance')
    require(value.get('parser_source_sha256') == PARSER_SOURCE_SHA256,
            'Expectation parser provenance changed')
    require(isinstance(value.get('reset_settings'), dict) and value['reset_settings'] == RESET_SETTINGS and
            all(type(value['reset_settings'][key]) is type(expected) for key, expected in RESET_SETTINGS.items()),
            'Expected only InitIOKit return inside PE_init_iokit, occurrence 1, six seconds')
    require(isinstance(value.get('review'), dict) and
            all(value['review'].get(key) is True for key in REVIEW_KEYS),
            'Independent new-build/layout/console/expected-byte review is incomplete')
    original, expected = value.get('original'), value.get('expected')
    _exact_keys(original, ('sha256', 'bytes'), 'original')
    _exact_keys(expected, ('sha256', 'changed_bytes'), 'expected')
    _digest(original['sha256'], 'original.sha256')
    _digest(expected['sha256'], 'expected.sha256')
    require(original['sha256'] != LEGACY_ORIGINAL_SHA256,
            'r21 must not reuse the old DT-panic source kernel')
    require(type(original['bytes']) is int and 32 < original['bytes'] <= (1 << 30),
            'Missing/invalid frozen original byte size')
    require(type(expected['changed_bytes']) is int and 0 < expected['changed_bytes'] <= 300 and
            expected['sha256'] != original['sha256'], 'Missing/invalid frozen patched-byte expectation')
    layout = value.get('layout')
    _exact_keys(layout, ('entry', 'first_cpu', 'mmu', 'post_mmu', 'caves', 'caller', 'callee',
                       'call_site', 'original_call_word', 'argument', 'direct_return',
                       'optional_tail', 'configure_call', 'console'), 'layout')
    for name in ('entry', 'first_cpu', 'mmu', 'post_mmu', 'call_site'):
        address(layout[name])
    _word(layout['original_call_word'])
    _exact_keys(layout['caves'], CAVE_ENTRY_OFFSETS, 'caves')
    for stage, offset in CAVE_ENTRY_OFFSETS.items():
        require(address(layout['caves'][stage]) == address(layout['entry']) + offset,
                'Cave does not have the reviewed official entry-relative geometry')
    require(address(layout['post_mmu']) == address(layout['mmu']) + 0x20,
            'Post-MMU marker is inconsistent')
    for key, symbol in (('caller', CALLER_NAME), ('callee', CALLEE_NAME), ('console', '_PE_create_console')):
        body = layout[key]
        _exact_keys(body, ('name', 'start', 'end', 'sha256'), key)
        require(body['name'] == symbol and address(body['start']) < address(body['end']),
                'Wrong selected symbol/range: ' + key)
        _digest(body['sha256'], key + '.sha256')
    argument = layout['argument']
    _exact_keys(argument, ('adrp_va', 'adrp_word', 'load_va', 'load_word', 'storage_va',
                           'pe_state_va', 'device_tree_offset', 'saved_x0_va', 'saved_x0_word'), 'argument')
    for key in ('adrp_va', 'load_va', 'storage_va', 'pe_state_va', 'saved_x0_va'):
        address(argument[key])
    for key in ('adrp_word', 'load_word', 'saved_x0_word'):
        _word(argument[key])
    require(type(argument['device_tree_offset']) is int and argument['device_tree_offset'] == 0x98,
            'Unexpected reviewed PE_state.deviceTreeHead offset')
    for key in ('direct_return', 'optional_tail'):
        epilogue = layout[key]
        _exact_keys(epilogue, ('va', 'word', 'restore_words'), key)
        address(epilogue['va'])
        _word(epilogue['word'])
        require(isinstance(epilogue['restore_words'], list) and len(epilogue['restore_words']) == 3,
                'Missing bounded InitIOKit return restoration')
        for word in epilogue['restore_words']:
            _word(word)
    _exact_keys(layout['configure_call'], ('va', 'word', 'target_va'), 'configure_call')
    address(layout['configure_call']['va'])
    address(layout['configure_call']['target_va'])
    _word(layout['configure_call']['word'])
    return value


def load_expectation(path):
    def unique_object(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, 'Duplicate JSON expectation key: ' + key)
            result[key] = value
        return result
    value = json.loads(Path(path).read_text(encoding='utf-8'), object_pairs_hook=unique_object)
    return validate_expectation(value)


def _text_range(image, low, high):
    require(low % 4 == 0 and low < high and high % 4 == 0, 'Invalid instruction range')
    image.offset(low, high - low)
    require([(seg, name) for seg, name, start, size in image.sections
             if start <= low and high <= start + size] == [('__TEXT_EXEC', '__text')],
            'Instruction range is not uniquely inside executable text')


def _symbol(image, name, no_alias=True):
    require(name in image.names and name not in image.ambiguous_names, 'Missing/ambiguous symbol: ' + name)
    start = image.names[name]
    if no_alias:
        require({n for n, va in image.names.items() if va == start} == {name},
                'Selected symbol has an address alias: ' + name)
    return start


def _function(image, name):
    low = _symbol(image, name)
    higher = [va for va in image.addresses if va > low]
    require(bool(higher), 'No bounded next-symbol range for ' + name)
    high = min(higher)
    require(high - low <= 0x10000, 'Unexpectedly large selected symbol interval')
    _text_range(image, low, high)
    return {'name': name, 'start': hex(low), 'end': hex(high), 'sha256': sha(image.body(low, high - low))}


def _discover(raw):
    require(b'_source_corecrypto14_bootstrap\0' not in raw,
            'Source-crypto bootstrap marker requires a separate seal review; this six-region contract cannot accept it')
    image = MachO(raw)
    entry = image.entry
    require(type(entry) is int and entry > 0, 'Missing LC_UNIXTHREAD entry')
    first = _symbol(image, '_start_first_cpu', no_alias=False)
    _text_range(image, entry, entry + 4)
    require(image.word(entry) == branch(entry, first), 'Entry is not the original direct B to first CPU')
    _text_range(image, first, first + 0x1024)
    matches = [va for va in range(first, first + 0x1000, 4)
               if image.word(va) == 0xd5181000 and image.word(va + 4) == 0xd5033fdf and
               image.word(va + 0x20) == 0xd2a00600 and image.word(va + 0x24) == 0xd5181040]
    require(len(matches) == 1, 'Expected one SCTLR/ISB/CPACR sequence in bounded first-CPU window')
    mmu = matches[0]
    caves = {stage: entry + delta for stage, delta in CAVE_ENTRY_OFFSETS.items()}
    for va in caves.values():
        _text_range(image, va, va + CAVE_BYTES)
        require(image.body(va, CAVE_BYTES) == bytes(CAVE_BYTES), 'Original official probe cave is not zero')
        require(not any(va <= sym < va + CAVE_BYTES for sym in image.addresses),
                'A defined symbol occupies a probe cave')
    caller, callee = _function(image, CALLER_NAME), _function(image, CALLEE_NAME)
    cstart, cend = address(caller['start']), address(caller['end'])
    dest, dend = address(callee['start']), address(callee['end'])
    calls = [va for va in range(cstart, cend, 4) if image.word(va) & 0xfc000000 == 0x94000000
             and target(image.word(va), va, 0x94000000) == dest]
    require(len(calls) == 1, 'Expected a unique direct InitIOKit BL inside PE_init_iokit')
    call = calls[0]
    require(call - 8 >= cstart and call + 8 <= cend, 'Selected call has no bounded argument/continuation')
    adrp, load = image.word(call - 8), image.word(call - 4)
    require(adrp & 0x9f000000 == 0x90000000 and load & 0xffc0001f == 0xf9400000 and
            adrp & 31 == (load >> 5) & 31, 'Unsupported x0 ADRP/LDR materialization; independent review required')
    delta = ((adrp >> 5) & 0x7ffff) * 4 + ((adrp >> 29) & 3)
    if delta & (1 << 20):
        delta -= 1 << 21
    pointer = ((call - 8) & ~0xfff) + (delta << 12) + (((load >> 10) & 0xfff) * 8)
    pe_state = _symbol(image, '_PE_state')
    require(pointer == pe_state + 0x98, 'x0 load is not the reviewed PE_state.deviceTreeHead field')
    image.offset(pointer, 8)
    saved = [va for va in range(dest, min(dest + 0x40, dend), 4) if image.word(va) == 0xaa0003f3]
    require(len(saved) == 1, 'Unsupported initial x0 save; independent ABI review required')
    rets = [va for va in range(dest, dend, 4) if image.word(va) == 0xd65f03c0]
    tails = [va for va in range(dest, dend, 4) if image.word(va) & 0xfffffc1f == 0xd61f0000]
    require(len(rets) == 1 and len(tails) == 1 and image.word(tails[0]) == 0xd61f0000,
            'Expected unique InitIOKit direct RET and optional tail BR x0')
    restore = (0xa9427bfd, 0xa9414ff4, 0x9100c3ff)
    for va in rets + tails:
        require(va - 12 >= dest and image.body(va - 12, 12) == struct.pack('<3I', *restore),
                'Unsupported InitIOKit stack/LR restoration; independent ABI review required')
    configure = _symbol(image, '_ConfigureIOKit')
    require(target(image.word(call + 4), call + 4, 0x94000000) == configure,
            'Selected call is not immediately followed by ConfigureIOKit')
    layout = {
        'entry': hex(entry), 'first_cpu': hex(first), 'mmu': hex(mmu), 'post_mmu': hex(mmu + 0x20),
        'caves': {stage: hex(va) for stage, va in caves.items()}, 'caller': caller, 'callee': callee,
        'call_site': hex(call), 'original_call_word': f'0x{image.word(call):08x}',
        'argument': {'adrp_va': hex(call - 8), 'adrp_word': f'0x{adrp:08x}',
                     'load_va': hex(call - 4), 'load_word': f'0x{load:08x}', 'storage_va': hex(pointer),
                     'pe_state_va': hex(pe_state), 'device_tree_offset': 0x98,
                     'saved_x0_va': hex(saved[0]), 'saved_x0_word': '0xaa0003f3'},
        'direct_return': {'va': hex(rets[0]), 'word': '0xd65f03c0',
                          'restore_words': [f'0x{w:08x}' for w in restore]},
        'optional_tail': {'va': hex(tails[0]), 'word': '0xd61f0000',
                          'restore_words': [f'0x{w:08x}' for w in restore]},
        'configure_call': {'va': hex(call + 4), 'word': f'0x{image.word(call + 4):08x}',
                           'target_va': hex(configure)},
        'console': _function(image, '_PE_create_console'),
    }
    return image, layout


def _paint(stage, layout):
    require(stage in (1, 2), 'Only official stage 1/2 paint sequences are allowed')
    words = ([0xf9401401, 0xf9401c02, 0xf9402403] if stage == 1
             else [0xf9401681, 0xf9401e82, 0xf9402683])
    words += [0x9b037c42, 0xd344fc42, 0x927df042,
              0xd2800024 if stage == 1 else 0xd2800044,
              0x9b047c45, 0x8b050021, 0x52801fe6,
              0x72bfe006 if stage == 1 else 0x72bfffe6,
              0x28811826, 0xf1002042, 0x54ffffc8, 0xd5033f9f]
    if stage == 2:
        words.append(0xd5181000)
    cave = address(layout['caves'][str(stage)])
    destination = address(layout['first_cpu']) if stage == 1 else address(layout['mmu']) + 4
    words.append(branch(cave + 4 * len(words), destination))
    return struct.pack('<' + 'I' * len(words), *words).ljust(CAVE_BYTES, b'\0')


def expected_regions(layout):
    cave = address(layout['caves']['4'])
    words = [branch(cave, address(layout['callee']['start']), True),
             0xd53be021, 0x580001c2, 0x8b020022, 0xd5033fdf, 0xd53be021, 0xeb02003f, 0x54ffffa3,
             0x52800120, 0x72b08000, 0xaa1f03e1, 0xaa1f03e2, 0xaa1f03e3,
             0xd4000003, 0xd503205f, 0x17ffffff]
    reset = (struct.pack('<16I', *words) + struct.pack('<Q', DELAY_TICKS)).ljust(CAVE_BYTES, b'\0')
    return [(address(layout['entry']), struct.pack('<I', branch(address(layout['entry']), address(layout['caves']['1'])))),
            (address(layout['mmu']), struct.pack('<I', branch(address(layout['mmu']), address(layout['caves']['2'])))),
            (address(layout['call_site']), struct.pack('<I', branch(address(layout['call_site']), cave))),
            (address(layout['caves']['1']), _paint(1, layout)),
            (address(layout['caves']['2']), _paint(2, layout)), (cave, reset)]


def _derive_unreviewed(raw, image, layout):
    result = bytearray(raw)
    regions = []
    for va, code in expected_regions(layout):
        offset = image.offset(va, len(code))
        regions.append((offset, offset + len(code)))
        result[offset:offset + len(code)] = code
    require(len(regions) == 6, 'Expected exactly six hook/cave intervals')
    end = 0
    for low, high in sorted(regions):
        require(low >= end, 'Probe intervals overlap')
        end = high
    changed = sum(a != b for a, b in zip(raw, result))
    return bytes(result), changed


def discover_layout(raw):
    """Return proposals for review only; this result cannot pass acceptance."""
    image, layout = _discover(raw)
    proposed, changed = _derive_unreviewed(raw, image, layout)
    return {'schema_version': 1, 'status': 'unreviewed-discovery',
            'parser_source_sha256': PARSER_SOURCE_SHA256,
            'original': {'sha256': sha(raw), 'bytes': len(raw)},
            'layout': layout, 'reset_settings': dict(RESET_SETTINGS),
            'expected': {'sha256': sha(proposed), 'changed_bytes': changed},
            'review': {key: False for key in REVIEW_KEYS}}


def original_layout(raw, expectation):
    validate_expectation(expectation)
    require(len(raw) == expectation['original']['bytes'] and sha(raw) == expectation['original']['sha256'],
            'Expected the frozen new uninstrumented text-console kernel')
    image, layout = _discover(raw)
    require(layout == expectation['layout'], 'New original layout differs from independently frozen expectation')
    return image, layout


def derive_expected(original, expectation):
    """In-memory independent reference; never substitute this for official generation."""
    image, layout = original_layout(original, expectation)
    result, changed = _derive_unreviewed(original, image, layout)
    require(sha(result) == expectation['expected']['sha256'] and
            changed == expectation['expected']['changed_bytes'],
            'Independent expected bytes differ from the frozen complete digest/count')
    return result


def probe_readback(original, patched, expectation):
    image, layout = original_layout(original, expectation)
    require(len(patched) == len(original), 'Diagnostic kernel length changed')
    candidate = MachO(patched)
    require(candidate.entry == image.entry and candidate.names == image.names and
            candidate.ambiguous_names == image.ambiguous_names and candidate.segments == image.segments and
            candidate.sections == image.sections and patched[:image.header_end] == original[:image.header_end],
            'Mach-O metadata/entry/symbols changed')
    regions = []
    for va, expected in expected_regions(layout):
        offset = image.offset(va, len(expected))
        require(patched[offset:offset + len(expected)] == expected,
                'Exact official hook/cave sequence differs at ' + hex(va))
        regions.append((offset, offset + len(expected), va))
    cursor, changed, counts = 0, 0, []
    for low, high, va in sorted(regions):
        require(low >= cursor and patched[cursor:low] == original[cursor:low],
                'Bytes changed outside the six approved hook/cave intervals (including any seal)')
        count = sum(a != b for a, b in zip(patched[low:high], original[low:high]))
        changed += count
        counts.append({'va': hex(va), 'file_start': hex(low), 'file_end': hex(high), 'changed_bytes': count})
        cursor = high
    require(patched[cursor:] == original[cursor:], 'Kernel tail changed outside approved intervals (including any seal)')
    require(candidate.body(address(layout['caves']['3']), CAVE_BYTES) == bytes(CAVE_BYTES) and
            candidate.word(address(layout['post_mmu'])) == image.word(address(layout['post_mmu'])),
            'Stage 3 must remain disabled')
    require(changed == expectation['expected']['changed_bytes'] and sha(patched) == expectation['expected']['sha256'],
            'Candidate differs from independently frozen complete digest/count')
    preserved = {}
    for key in ('callee', 'console'):
        low, high = address(layout[key]['start']), address(layout[key]['end'])
        require(candidate.body(low, high - low) == image.body(low, high - low),
                'Original ' + key + ' implementation changed')
        preserved[layout[key]['name']] = {'start': hex(low), 'end': hex(high),
                                          'sha256': sha(candidate.body(low, high - low)),
                                          'identical_to_new_uninstrumented_baseline': True}
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
            'original_sp_and_argument_registers_preserved_before_call': True,
            'argument_semantics': 'PE_state.deviceTreeHead',
            'callee_normal_ret_va': layout['direct_return']['va'],
            'optional_tail_callback_branch_va': layout['optional_tail']['va'],
            'runtime_optional_callback_observed': False, 'configure_iokit_executed': False,
            'console_implementation_preserved_from_new_baseline': True,
            'preserved_functions': preserved,
            'source_crypto_seal_changed_by_probe': False,
            'permitted_regions': counts, 'parser_source_sha256': PARSER_SOURCE_SHA256,
            'hardware_tested': False,
            'scope': 'New text-console baseline; InitIOKit return checkpoint only. No runtime console, callback, '
                     'ConfigureIOKit, matching, IRQ, root or userland success inferred. A missing reset is not callee-localized evidence.'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('candidate', type=Path, nargs='?')
    parser.add_argument('--expectation', type=Path)
    parser.add_argument('--discover', action='store_true', help='Unreviewed in-memory discovery only; never acceptance')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.discover:
        require(args.candidate is None and args.expectation is None, 'Discovery cannot be combined with acceptance inputs')
        result = discover_layout(args.original.read_bytes())
    else:
        require(args.expectation is not None and args.candidate is not None, 'Acceptance needs candidate and --expectation')
        frozen = load_expectation(args.expectation)
        result = probe_readback(args.original.read_bytes(), args.candidate.read_bytes(), frozen)
    if args.report:
        with args.report.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
    print(json.dumps(result, indent=2))
