#!/usr/bin/env python3
"""r22 firmware contract/parser rejection tests; synthetic fixture is not firmware qualification."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys
import tempfile

sys.dont_write_bytecode = True
import verify_xnu_early_log_firmware as v


def run():
    tests = []

    def reject(name, action):
        try:
            action()
        except (ValueError, FileNotFoundError, KeyError):
            tests.append({'name': name, 'rejected': True})
        else:
            raise AssertionError('Accepted invalid fixture: ' + name)

    for status in ('pending-new-artifacts', 'unreviewed-discovery', None):
        reject('pending ' + str(status), lambda status=status: v.validate_expectation({'schema_version': 1, 'status': status}))
    reject('frozen label without review', lambda: v.validate_expectation({'schema_version': 1, 'status': 'independently-frozen'}))
    for path in ('../escape', '/absolute', 'C:/outside', 'a\\b', 'a/../b', './hidden'):
        reject('unsafe evidence ' + path, lambda path=path: v.relative(path))
    raw = bytearray(512)
    raw[:2] = b'MZ'
    struct.pack_into('<I', raw, 0x3c, 0x80)
    raw[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', raw, 0x84, 0xaa64, 1)
    struct.pack_into('<H', raw, 0x94, 112)
    struct.pack_into('<H', raw, 0x98, 0x20b)
    struct.pack_into('<I', raw, 0xa8, 0x1000)
    struct.pack_into('<I', raw, 0xd0, 0x2000)
    struct.pack_into('<H', raw, 0xdc, 10)
    v.check_probe(raw)
    for name, offset, fmt, value in (
        ('wrong machine', 0x84, '<H', 0x8664), ('not app', 0xdc, '<H', 11),
        ('PE32 not PE32+', 0x98, '<H', 0x10b), ('invalid entry', 0xa8, '<I', 0),
        ('entry beyond image', 0xa8, '<I', 0x2000), ('unbounded header', 0x3c, '<I', 0xffffff00),
        ('unbounded sections', 0x86, '<H', 96),
    ):
        bad = bytearray(raw)
        struct.pack_into(fmt, bad, offset, value)
        reject(name, lambda bad=bad: v.check_probe(bad))
    with tempfile.TemporaryDirectory(prefix='j716f-r22-gate-') as temp:
        root = Path(temp)
        artifacts = {'SILICIUM_UEFI.fd': {'sha256': v.sha(b'fixture FD'), 'bytes': 7340032},
                     'J716fEarlyLogProbe.efi': {'sha256': v.sha(raw), 'bytes': len(raw)}}
        report = {'status': 'reviewed-host-firmware-and-retention-app', 'artifacts': artifacts,
                  'hardware_tested': False, 'retention_qualified': False}
        files = {'review.json': json.dumps(report).encode(), 'source.json': b'{"fixture":"source"}',
                 'tests.json': b'{"fixture":"tests"}'}
        for name, data in files.items():
            (root / name).write_bytes(data)
        doc = {'schema_version': 1, 'status': 'independently-frozen', 'artifacts': artifacts,
               'protocol_header_sha256': v.HEADER_SHA, 'hardware_tested': False, 'retention_qualified': False,
               'review': {key: True for key in v.REVIEW_KEYS},
               'review_evidence': {name: {'sha256': v.sha(data), 'bytes': len(data)} for name, data in files.items()},
               'actual_build_review': 'review.json', 'source_overlay_manifest': 'source.json', 'host_test_report': 'tests.json'}
        v.validate_expectation(doc)
        v.verify_evidence(root, doc)
        bad = copy.deepcopy(doc)
        bad['artifacts']['SILICIUM_UEFI.fd']['sha256'] = v.OLD_R9_FD_SHA
        reject('retained r9 FD lacks recovery', lambda: v.validate_expectation(bad))
        for key in v.REVIEW_KEYS:
            bad = copy.deepcopy(doc)
            bad['review'][key] = False
            reject('missing review ' + key, lambda bad=bad: v.validate_expectation(bad))
        for key, value in (('retention_qualified', True), ('hardware_tested', True),
                           ('protocol_header_sha256', '0' * 64), ('actual_build_review', 'missing.json')):
            bad = copy.deepcopy(doc)
            bad[key] = value
            reject('wrong contract ' + key, lambda bad=bad: v.validate_expectation(bad))
        # Even an updated evidence hash must not let a report for another FD pass.
        report['artifacts'] = copy.deepcopy(artifacts)
        report['artifacts']['SILICIUM_UEFI.fd']['sha256'] = v.sha(b'wrong FD')
        changed = json.dumps(report).encode()
        (root / 'review.json').write_bytes(changed)
        reject('changed pinned review', lambda: v.verify_evidence(root, doc))
        doc['review_evidence']['review.json'] = {'sha256': v.sha(changed), 'bytes': len(changed)}
        reject('repinned review for another FD', lambda: v.verify_evidence(root, doc))
    return {'status': 'passed-synthetic-contract-tests', 'positive_checks': 3,
            'negative_checks': len(tests), 'tests': tests, 'hardware_tested': False,
            'firmware_qualified': False, 'retention_qualified': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    result = run()
    with args.report.open('x', encoding='utf-8', newline='\n') as stream:
        json.dump(result, stream, indent=2)
        stream.write('\n')
    print(json.dumps({key: result[key] for key in ('status', 'positive_checks', 'negative_checks')}, indent=2))
