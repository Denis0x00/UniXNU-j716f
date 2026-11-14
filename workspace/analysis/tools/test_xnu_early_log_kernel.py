#!/usr/bin/env python3
"""Focused r22 frozen-contract and diagnostic-byte rejection tests.

Pending-only checks do not qualify a kernel. Full fixtures require the new
original, official candidate, retained r20 candidate and frozen expectation.
"""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
import verify_xnu_early_log_kernel as v

R20_SHA256 = 'fb7453f96ff26ae936fe6c22ff714093f1b1420f2e049f3d7a6d3e87e3473cba'


def _reject(tests, name, action):
    try:
        action()
    except ValueError as exc:
        tests.append({'name': name, 'rejected': True, 'reason': str(exc)})
    else:
        raise AssertionError('Accepted invalid fixture: ' + name)


def pending_checks():
    tests = []
    for status in ('pending', 'pending-new-kernel', 'unreviewed-discovery', None):
        value = {'schema_version': 1, 'status': status}
        _reject(tests, 'reject-status-' + str(status), lambda value=value: v.validate_expectation(value))
    _reject(tests, 'frozen-label-without-pins',
            lambda: v.validate_expectation({'schema_version': 1, 'status': 'independently-frozen'}))
    _reject(tests, 'probe-api-cannot-bypass-pending-loader',
            lambda: v.probe_readback(b'', b'', {'schema_version': 1, 'status': 'pending'}))
    _reject(tests, 'derive-api-cannot-bypass-pending-loader',
            lambda: v.derive_expected(b'', {'schema_version': 1, 'status': 'pending'}))
    _reject(tests, 'layout-api-cannot-bypass-pending-loader',
            lambda: v.original_layout(b'', {'schema_version': 1, 'status': 'pending'}))
    return tests


def run_tests(original, good, r20, expectation):
    tests = pending_checks()
    image, layout = v.original_layout(original, expectation)
    initial = v.probe_readback(original, good, expectation)
    v.require(v.derive_expected(original, expectation) == good,
              'Official fixture is not byte-identical to independent expectation')
    v.require(v.sha(r20) == R20_SHA256, 'Wrong retained r20 negative fixture')

    def reject(name, candidate, base=original, contract=expectation):
        _reject(tests, name, lambda: v.probe_readback(base, candidate, contract))

    def mutate(name, va, word):
        candidate = bytearray(good)
        struct.pack_into('<I', candidate, image.offset(va, 4), word)
        reject(name, bytes(candidate))

    reject('unpatched-new-original', original)
    reject('old-r20-candidate', r20)
    reject('old-r20-as-baseline', good, r20)
    reject('preinstrumented-new-baseline', good, good)
    changed_base = bytearray(original)
    changed_base[-1] ^= 1
    reject('wrong-baseline-single-byte', good, bytes(changed_base))
    unreviewed = v.discover_layout(original)
    reject('discovery-cannot-self-authorize', good, contract=unreviewed)
    missing_pin = copy.deepcopy(expectation)
    missing_pin['expected']['sha256'] = None
    reject('frozen-contract-missing-digest', good, contract=missing_pin)
    wrong_layout = copy.deepcopy(expectation)
    wrong_layout['layout']['call_site'] = hex(v.address(layout['call_site']) + 4)
    reject('frozen-contract-wrong-callsite', good, contract=wrong_layout)
    wrong_count = copy.deepcopy(expectation)
    wrong_count['expected']['changed_bytes'] += 1
    reject('frozen-contract-wrong-byte-count', good, contract=wrong_count)
    unreviewed_console = copy.deepcopy(expectation)
    unreviewed_console['review']['early_log_source_and_binary_verified'] = False
    reject('frozen-contract-unreviewed-console', good, contract=unreviewed_console)

    cave, call = v.address(layout['caves']['4']), v.address(layout['call_site'])
    mutate('wrong-callee-configure', cave, v.branch(cave, v.address(layout['configure_call']['target_va']), True))
    mutate('before-call-immediate-reset', cave, 0x52800120)
    mutate('hook-is-bl-not-b', call, v.branch(call, cave, True))
    mutate('changed-delay-literal', cave + 64, v.DELAY_TICKS + 1)
    mutate('timer-bypassed', cave + 4, v.branch(cave + 4, cave + 32))
    mutate('smc-replaced', cave + 52, 0xd503201f)
    mutate('stage3-stop-enabled', v.address(layout['post_mmu']),
           v.branch(v.address(layout['post_mmu']), v.address(layout['caves']['3'])))
    mutate('device-tree-pointer-load-zeroed', v.address(layout['argument']['load_va']), 0xd2800000)
    mutate('init-direct-ret-altered', v.address(layout['direct_return']['va']), 0xd503201f)
    mutate('optional-tail-br-changed-to-blr', v.address(layout['optional_tail']['va']), 0xd63f0000)
    mutate('configure-call-altered', v.address(layout['configure_call']['va']), 0xd503201f)
    console = v.address(layout['console']['start'])
    mutate('new-console-implementation-altered', console, image.word(console) ^ 1)
    mutate('stage1-paint-altered', v.address(layout['caves']['1']), 0xd503201f)
    metadata = bytearray(good)
    metadata[24] ^= 1
    reject('header-changed', bytes(metadata))
    tail = bytearray(good)
    tail[-1] ^= 1
    reject('outside-regions-tail-byte-changed', bytes(tail))
    reject('candidate-appended-byte', good + b'\0')
    reject('candidate-truncated', good[:-1])
    v.require(v.probe_readback(original, good, expectation) == initial, 'Good fixture changed during rejection checks')
    return {'status': 'passed', 'positive_checks': 3, 'negative_checks': len(tests), 'tests': tests,
            'original_sha256': v.sha(original), 'verified_candidate_sha256': v.sha(good),
            'hardware_tested': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--original', type=Path)
    parser.add_argument('--candidate', type=Path)
    parser.add_argument('--r20', type=Path)
    parser.add_argument('--expectation', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--pending-only', action='store_true')
    args = parser.parse_args()
    if args.pending_only:
        v.require(args.original is None and args.candidate is None and args.r20 is None,
                  'Pending-only checks cannot consume positive kernel fixtures')
        tests = pending_checks()
        _reject(tests, 'pending-file-rejected-by-loader', lambda: v.load_expectation(args.expectation))
        result = {'status': 'passed-pending-rejections-only', 'positive_checks': 0,
                  'negative_checks': len(tests), 'tests': tests, 'kernel_qualified': False, 'hardware_tested': False}
    else:
        v.require(all(path is not None for path in (args.original, args.candidate, args.r20)),
                  'Full tests require --original, --candidate and --r20')
        expectation = v.load_expectation(args.expectation)
        result = run_tests(args.original.read_bytes(), args.candidate.read_bytes(), args.r20.read_bytes(), expectation)
    result['helper_sha256'] = v.sha(Path(v.__file__).read_bytes())
    result['test_sha256'] = v.sha(Path(__file__).read_bytes())
    with args.report.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({key: result[key] for key in ('status', 'positive_checks', 'negative_checks')}, indent=2))
