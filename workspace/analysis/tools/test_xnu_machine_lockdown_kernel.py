#!/usr/bin/env python3
"""Focused r25 transition regressions, not a repeat of unchanged early-log tests."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
import verify_xnu_machine_lockdown_kernel as v


def run_tests(original, good, expectation):
    image, layout = v.original_layout(original, expectation)
    actual = v.probe_readback(original, good, expectation)
    v.require(v.derive_expected(original, expectation) == good, 'Candidate is not independent expected r25 kernel')
    cases = []

    def reject(name, action):
        try:
            action()
        except ValueError as error:
            cases.append({'name': name, 'rejected': True, 'reason': str(error)})
        else:
            raise AssertionError('Accepted invalid fixture: ' + name)

    def mutate(name, va, word):
        raw = bytearray(good)
        struct.pack_into('<I', raw, image.offset(va, 4), word)
        reject(name, lambda: v.probe_readback(original, bytes(raw), expectation))

    cave, call = int(layout['caves']['4'], 16), int(layout['call_site'], 16)
    old = int(layout['old_calls']['InitIOKit']['va'], 16)
    configure = int(layout['old_calls']['ConfigureIOKit']['va'], 16)
    mutate('old-InitIOKit-hook-still-patched', old, v.branch(old, cave))
    mutate('old-ConfigureIOKit-hook-still-patched', configure, v.branch(configure, cave))
    mutate('new-machine-lockdown-hook-missing', call, image.word(call))
    mutate('wrapper-calls-old-ConfigureIOKit', cave, v.branch(cave, int(layout['old_calls']['ConfigureIOKit']['target_va'], 16), True))
    mutate('wrapper-skips-actual-machine-lockdown-call', cave, 0xd503201f)
    mutate('wrapper-uses-tail-B-and-loses-return-to-timer', cave, v.branch(cave, int(layout['callee']['start'], 16)))
    mutate('six-second-ticks-changed', cave + 64, v.DELAY_TICKS + 1)
    mutate('unrequested-stage3-code', int(layout['caves']['3'], 16), 0xd503201f)
    mutate('machine-lockdown-body-modified', int(layout['callee']['start'], 16), 0xd503201f)
    mutate('machine-normal-return-modified', int(layout['normal_return']['va'], 16), 0xd503201f)
    reject('uninstrumented-original', lambda: v.probe_readback(original, original, expectation))
    wrong = copy.deepcopy(expectation)
    wrong['reset_settings']['entry_probe_reset_after'] = '_InitIOKit'
    reject('contract-old-InitIOKit-selection', lambda: v.probe_readback(original, good, wrong))
    wrong_layout = copy.deepcopy(expectation)
    wrong_layout['layout']['mapping']['runtime_mapping_verified'] = True
    reject('contract-invented-runtime-mapping-success', lambda: v.probe_readback(original, good, wrong_layout))
    stale = copy.deepcopy(expectation)
    stale['baseline_contract']['original']['sha256'] = '1' + stale['baseline_contract']['original']['sha256'][1:]
    reject('contract-changed-inherited-original', lambda: v.original_layout(original, stale))
    pending = copy.deepcopy(expectation)
    pending['status'] = 'unreviewed-discovery'
    reject('unreviewed-contract', lambda: v.derive_expected(original, pending))
    return {'status': 'passed-focused-r25-kernel-tests', 'positive_cases': 1, 'rejected_cases': len(cases),
            'cases': cases, 'candidate_sha256': actual['sha256'],
            'hardware_tested': False, 'scope': 'Machine callsite, both old-hook restorations, normal return, static retained text mapping, exact timer and six allowed regions.'}


def run(root, artifact):
    from verify_xnu_entry import read_fat32
    artifact = Path(artifact)
    files, _ = read_fat32((artifact / 'payload.img').read_bytes())
    return run_tests((artifact / 'original-kernel.macho').read_bytes(),
                     files['efi/unixnu/payloads/kernel.macho'],
                     v.load_expectation(artifact / 'kernel-expectation.json'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact', type=Path)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    result = run(args.root, args.artifact)
    if args.report:
        with args.report.open('x', encoding='utf-8', newline='\n') as stream:
            json.dump(result, stream, indent=2)
            stream.write('\n')
    print(json.dumps(result, indent=2))
