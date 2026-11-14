#!/usr/bin/env python3
"""Reject wrong storage integration, return ABI and probe bytes on the actual image."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
import verify_storage_kernel as v

def run(original, candidate, expectation):
    image, layout = v.original_layout(original, expectation)
    accepted = v.verify_candidate(original, candidate, expectation)
    v.require(v.derive_expected(original, expectation) == candidate, 'Independent candidate mismatch')
    cases = []
    def reject(name, operation):
        try:
            operation()
        except ValueError as exc:
            cases.append({'name': name, 'rejected': True, 'reason': str(exc)})
        else:
            raise AssertionError('Accepted invalid fixture: ' + name)
    def word(name, address, replacement):
        changed = bytearray(candidate)
        struct.pack_into('<I', changed, image.offset(address, 4), replacement)
        reject(name, lambda: v.verify_candidate(original, bytes(changed), expectation))
    cave = int(layout['caves']['4'], 16)
    call = int(layout['call_site'], 16)
    word('checkpoint-hook-removed', call, image.word(call))
    word('wrapper-skips-callee', cave, 0xd503201f)
    word('wrapper-tail-call-has-no-return', cave, v.branch(cave, int(layout['callee']['start'], 16)))
    word('wrapper-calls-root-discovery', cave,
         v.branch(cave, int(layout['bsd_boundary']['later_calls']['_IOFindBSDRoot']['target_va'], 16), True))
    word('argument-x0-changed', call - 4, 0xd2800020)
    word('timer-wrong', cave + 64, v.DELAY_TICKS + 1)
    word('unexpected-stage3', int(layout['caves']['3'], 16), 0xd503201f)
    word('callee-return-skipped', int(layout['normal_return']['va'], 16), 0xd503201f)
    word('callee-LR-restore-changed', int(layout['normal_return']['restore_va'], 16), 0xd503201f)
    word('callee-SP-restore-changed', int(layout['normal_return']['restore_va'], 16) + 4, 0xd503201f)
    for owner, item in layout['storage']['init_calls'].items():
        word(owner + '-registration-skipped', int(item['va'], 16), 0xd503201f)
    for owner, item in layout['storage']['iobsd_calls'].items():
        word(owner + '-iobsd-skipped', int(item['va'], 16), 0xd503201f)
    word('autoconf-chain-skipped', int(layout['storage']['autoconf_call']['va'], 16), 0xd503201f)
    word('registration-success-gate-skipped', int(layout['storage']['success_gate']['va'], 16), 0xd503201f)
    word('registration-failure-path-falls-through', int(layout['storage']['success_gate']['failure_skip_va'], 16), 0xd503201f)
    word('power-probe-call-skipped', int(layout['storage']['power_call']['va'], 16), 0xd503201f)
    word('power-probe-entry-rewritten', int(layout['storage']['exports'][v.POWER_NAME]['start'], 16), 0xd65f03c0)
    word('pre-root-log-skipped', int(layout['storage']['messages']['pre_root']['printf_va'], 16), 0xd503201f)
    word('storage-read-entry-rewritten', int(layout['storage']['exports']['_j716f_storage_bringup_read_known']['start'], 16), 0xd65f03c0)
    word('early-log-rewritten', int(layout['early_log']['_PE_j716f_earlylog_write']['start'], 16), 0xd65f03c0)
    for name, item in layout['bsd_boundary']['later_calls'].items():
        word('later-root-call-rewritten-' + name, int(item['va'], 16), 0xd503201f)
    reject('uninstrumented-original', lambda: v.verify_candidate(original, original, expectation))
    reject('truncated-candidate', lambda: v.verify_candidate(original, candidate[:-4], expectation))
    for key in ('runtime_return_observed', 'timer_irq_delivery_proved', 'storage_capacity_proved', 'known_block_read_proved', 'root_access_exercised', 'runtime_power_read_observed', 'external_abort_containment_proved'):
        forged = copy.deepcopy(expectation)
        forged['layout']['bsd_boundary'][key] = True
        reject('invented-runtime-' + key, lambda forged=forged: v.derive_expected(original, forged))
    for key, value in (('entry_probe_reset_before', True), ('entry_probe_reset_after', '_IOFindBSDRoot'),
                       ('entry_probe_reset_occurrence', True), ('entry_probe_reset_delay', 5)):
        forged = copy.deepcopy(expectation)
        forged['reset_settings'][key] = value
        reject('wrong-contract-' + key, lambda forged=forged: v.derive_expected(original, forged))
    forged = copy.deepcopy(expectation)
    forged['status'] = 'unreviewed-discovery'
    reject('unreviewed-contract', lambda: v.derive_expected(original, forged))
    for key in v.REVIEW_KEYS:
        forged = copy.deepcopy(expectation)
        forged['review'][key] = False
        reject('unreviewed-' + key, lambda forged=forged: v.derive_expected(original, forged))
    forged = copy.deepcopy(expectation)
    forged['review_evidence'] = {}
    reject('missing-review-evidence', lambda: v.derive_expected(original, forged))
    return {'status': 'passed-actual-storage-kernel-tests', 'positive_cases': 1,
            'rejected_cases': len(cases), 'candidate_sha256': accepted['sha256'],
            'hardware_tested': False, 'cases': cases}

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('original', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('--expectation', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    result = run(args.original.read_bytes(), args.candidate.read_bytes(), v.load_expectation(args.expectation))
    with args.report.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({key: value for key, value in result.items() if key != 'cases'}, indent=2))
