#!/usr/bin/env python3
"""Frozen r22 firmware/probe provenance gate; no runtime retention claim.

The expectation pins the separately reviewed actual build/module report and
source/test evidence. Byte identities and PE headers are checked here; those
checks alone are not described as verification of a working firmware path.
"""
import hashlib
import json
from pathlib import Path
import re
import struct

HEADER_SHA = 'd871825f16e72b3923420b3c0aa54e5d5a2291e5c0272d582ce05f9a22a86d16'
OLD_R9_FD_SHA = '4a04f13e77067732533821c39ba575a61babd46d5791ce801c67e9c0b8386dcb'
REVIEW_KEYS = ('actual_base_matches_r9', 'source_overlay_verified',
              'protocol_and_firmware_host_tests_passed', 'actual_fv_modules_reviewed',
              'recovery_before_jlog_initialization_reviewed',
              'ram_ownership_mapping_reviewed', 'probe_load_options_reviewed')


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(raw):
    return hashlib.sha256(raw).hexdigest()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_json(path):
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique_object)


def relative(value):
    require(isinstance(value, str) and value and not value.startswith('/') and
            '\\' not in value and value not in ('.', '..'), 'Non-canonical evidence path')
    path = Path(value)
    require(not path.is_absolute() and path.as_posix() == value and
            all(part not in ('', '.', '..') and ':' not in part for part in path.parts),
            'Evidence path escapes its root')
    return path


def pin(spec):
    require(isinstance(spec, dict) and set(spec) == {'sha256', 'bytes'} and
            type(spec['bytes']) is int and spec['bytes'] > 0 and
            isinstance(spec['sha256'], str) and re.fullmatch('[0-9a-f]{64}', spec['sha256']) and
            len(set(spec['sha256'])) > 1, 'Missing/invalid independent artifact pin')
    return spec


def validate_expectation(doc):
    require(isinstance(doc, dict) and doc.get('schema_version') == 1 and
            type(doc.get('schema_version')) is int and doc.get('status') == 'independently-frozen',
            'New firmware/probe expectation remains pending or unreviewed')
    require(doc.get('protocol_header_sha256') == HEADER_SHA,
            'Firmware and XNU must use the reviewed identical protocol header')
    require(doc.get('hardware_tested') is False and doc.get('retention_qualified') is False,
            'Host firmware gate cannot claim physical retention')
    require(isinstance(doc.get('artifacts'), dict) and
            set(doc['artifacts']) == {'SILICIUM_UEFI.fd', 'J716fEarlyLogProbe.efi'},
            'Expected exactly the new FD and independent RAM-log probe')
    for spec in doc['artifacts'].values():
        pin(spec)
    require(doc['artifacts']['SILICIUM_UEFI.fd']['bytes'] == 7340032,
            'Unreviewed FD size/layout change')
    require(doc['artifacts']['SILICIUM_UEFI.fd']['sha256'] != OLD_R9_FD_SHA,
            'Old r9 firmware has no early-log recovery path')
    require(isinstance(doc.get('review'), dict) and
            all(doc['review'].get(key) is True for key in REVIEW_KEYS),
            'Independent actual firmware/source/module review incomplete')
    evidence = doc.get('review_evidence')
    require(isinstance(evidence, dict) and len(evidence) >= 3,
            'Need separately pinned build/module, source and host-test evidence')
    for name, spec in evidence.items():
        relative(name)
        pin(spec)
    require(doc.get('actual_build_review') in evidence,
            'Actual firmware build/module report is not independently pinned')
    require(doc.get('source_overlay_manifest') in evidence and doc.get('host_test_report') in evidence,
            'Actual overlay and firmware test reports are not independently pinned')
    require(len({doc['actual_build_review'], doc['source_overlay_manifest'], doc['host_test_report']}) == 3,
            'Build, source and test evidence must be distinct files')
    return doc


def load_expectation(path):
    return validate_expectation(read_json(path))


def check_bytes(raw, spec, label):
    pin(spec)
    require(len(raw) == spec['bytes'] and sha(raw) == spec['sha256'], label + ' differs from independent pin')


def check_probe(raw):
    require(len(raw) >= 0x100 and raw[:2] == b'MZ', 'Probe lacks PE DOS signature')
    offset = struct.unpack_from('<I', raw, 0x3c)[0]
    require(offset >= 0x40 and offset + 24 + 70 <= len(raw) and raw[offset:offset + 4] == b'PE\0\0',
            'Probe lacks bounded PE header')
    machine, sections = struct.unpack_from('<HH', raw, offset + 4)
    opt_size = struct.unpack_from('<H', raw, offset + 20)[0]
    require(machine == 0xaa64 and 0 < sections <= 96 and opt_size >= 112 and
            offset + 24 + opt_size + sections * 40 <= len(raw), 'Probe is not a bounded AArch64 PE image')
    opt = offset + 24
    require(struct.unpack_from('<H', raw, opt)[0] == 0x20b and
            struct.unpack_from('<H', raw, opt + 68)[0] == 10,
            'Probe must be PE32+ EFI application')
    entry = struct.unpack_from('<I', raw, opt + 16)[0]
    image_size = struct.unpack_from('<I', raw, opt + 56)[0]
    require(0 < entry < image_size, 'Probe entry lies outside image')
    return {'machine': 'AARCH64', 'subsystem': 'EFI_APPLICATION', 'entry_rva': hex(entry),
            'sha256': sha(raw), 'bytes': len(raw), 'runtime_verified': False}


def verify_evidence(base, doc):
    for name, spec in doc['review_evidence'].items():
        path = (base / relative(name)).resolve(strict=True)
        require(path.is_relative_to(base.resolve()), 'Evidence symlink escapes root')
        check_bytes(path.read_bytes(), spec, name)
    review = read_json(base / relative(doc['actual_build_review']))
    require(review.get('status') == 'reviewed-host-firmware-and-retention-app' and
            review.get('artifacts') == doc['artifacts'] and
            review.get('hardware_tested') is False and review.get('retention_qualified') is False,
            'Pinned actual build review does not describe these FD/probe identities or overstates hardware evidence')


def validate_fd(root, fd, provenance, expectation_path):
    root = root.resolve(strict=True)
    doc = load_expectation(expectation_path)
    require(read_json(provenance) == doc, 'Archive firmware provenance differs from independent expectation')
    check_bytes(fd.read_bytes(), doc['artifacts']['SILICIUM_UEFI.fd'], 'New FD')
    probe = fd.with_name('J716fEarlyLogProbe.efi')
    check_bytes(probe.read_bytes(), doc['artifacts'][probe.name], 'New RAM-log probe')
    check_probe(probe.read_bytes())
    verify_evidence(root, doc)
    return doc


def verify_fd_provenance(artifact, fd, expected_fd_sha):
    doc = load_expectation(artifact / 'firmware-expectation.json')
    require(doc['artifacts']['SILICIUM_UEFI.fd']['sha256'] == expected_fd_sha,
            'Explicit independent FD pin differs from reviewed new firmware')
    require(read_json(artifact / 'fd-provenance.json') == doc,
            'Archived provenance differs from independent firmware expectation')
    check_bytes(fd, doc['artifacts']['SILICIUM_UEFI.fd'], 'Packaged FD')
    probe = (artifact / 'J716fEarlyLogProbe.efi').read_bytes()
    check_bytes(probe, doc['artifacts']['J716fEarlyLogProbe.efi'], 'Packaged RAM-log probe')
    probe_report = check_probe(probe)
    verify_evidence(artifact / 'fd-evidence', doc)
    return {'status': 'passed-host-identity-and-pinned-review', 'fd_sha256': sha(fd),
            'probe': probe_report, 'evidence_count': len(doc['review_evidence']),
            'actual_build_review': doc['actual_build_review'],
            'protocol_header_sha256': HEADER_SHA, 'hardware_tested': False,
            'retention_qualified': False}
