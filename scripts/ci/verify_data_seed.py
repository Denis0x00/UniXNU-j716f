#!/usr/bin/env python3
"""Verify a downloaded Data artifact's bytes and recorded provenance on any host.

This cannot rerun diskutil on Windows/Linux. A successful byte check is separate
from the macOS creation report and never establishes a working tablet driver.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import uuid

PAGE = 4096
MAX_ENCODED = 256 << 20
GIB = 1024 ** 3


def require(condition, message):
    if not condition:
        raise ValueError(message)


def fletcher64(data):
    lo = hi = 0
    for word, in struct.iter_unpack('<I', data):
        lo = (lo + word) % 0xffffffff
        hi = (hi + lo) % 0xffffffff
    c0 = 0xffffffff - ((lo + hi) % 0xffffffff)
    return struct.pack('<II', c0, 0xffffffff - ((lo + c0) % 0xffffffff))


def inspect_bytes(blob):
    require(32 <= len(blob) <= MAX_ENCODED, 'Packed size outside format bounds')
    magic, version, page, logical, count = struct.unpack_from('<8sIIQQ', blob)
    require(magic == b'DBRDISK1' and version == 1 and page == PAGE, 'Wrong packed format')
    require(3 * GIB <= logical <= 64 * GIB and logical % GIB == 0, 'Wrong runner logical size')
    require(0 < count <= 65536 and count <= (len(blob) - 32) // 16, 'Invalid extent count')
    cursor = 32 + 16 * count
    end = 0
    digest = hashlib.sha256(b'SHNGSEED2' + struct.pack('<Q', logical))
    uuids = set()
    primary = None
    volume_superblocks = 0
    for index in range(count):
        offset, size = struct.unpack_from('<QQ', blob, 32 + index * 16)
        require(size and not ((offset | size) % PAGE) and offset >= end and
                offset <= logical and size <= logical - offset and size <= len(blob) - cursor,
                'Invalid packed extent')
        for at in range(0, size, PAGE):
            block = blob[cursor + at:cursor + at + PAGE]
            absolute = offset + at
            if absolute == 0:
                primary = block
            if any(block):
                digest.update(struct.pack('<Q', absolute // PAGE) + block)
            if block[32:36] == b'APSB':
                require(block[:8] == fletcher64(block[8:]), 'APFS volume checksum mismatch')
                uuids.add(str(uuid.UUID(bytes=bytes(block[240:256]))).upper())
                volume_superblocks += 1
        end = offset + size
        cursor += size
    require(cursor == len(blob), 'Trailing packed bytes')
    require(primary is not None and primary[32:36] == b'NXSB' and
            primary[:8] == fletcher64(primary[8:]), 'APFS container checksum/magic mismatch')
    block_size = struct.unpack_from('<I', primary, 36)[0]
    blocks = struct.unpack_from('<Q', primary, 40)[0]
    require(block_size == PAGE and logical - (64 << 20) <= blocks * PAGE <= logical,
            'APFS container geometry mismatch')
    require(len(uuids) == 1, 'Expected exactly one APFS Data volume UUID')
    return dict(logical_bytes=logical, encoded_bytes=len(blob),
                encoded_sha256=hashlib.sha256(blob).hexdigest(),
                format_seed_identity_sha256=digest.hexdigest(), data_volume_uuid=uuids.pop(),
                apfs_container_features=dict(zip(('features', 'readonly_compatible', 'incompatible'),
                                                 struct.unpack_from('<QQQ', primary, 48))),
                checksummed_volume_superblocks=volume_superblocks)


def verify(directory, pins):
    directory = Path(directory)
    paths = ('data-21H461.dbr', 'data-21H461.json')
    lines = (directory / 'SHA256SUMS').read_text(encoding='ascii').splitlines()
    require(len(lines) == len(paths), 'Unexpected SHA256SUMS entries')
    expected = {}
    for line in lines:
        match = re.fullmatch(r'([0-9a-f]{64})  (data-21H461\.(?:dbr|json))', line)
        require(match is not None and match[2] not in expected, 'Malformed/duplicate checksum entry')
        expected[match[2]] = match[1]
    require(set(expected) == set(paths), 'Missing artifact checksum')
    seed = directory / paths[0]
    require(seed.stat().st_size <= MAX_ENCODED, 'Packed seed exceeds format bound')
    data = seed.read_bytes()
    metadata_bytes = (directory / paths[1]).read_bytes()
    for name, content in zip(paths, (data, metadata_bytes)):
        require(hashlib.sha256(content).hexdigest() == expected[name], 'Artifact checksum mismatch: ' + name)
    report = json.loads(metadata_bytes)
    for key in ('core_commit', 'darwinboot_commit', 'build', 'version'):
        require(report[key] == pins[key], 'Recorded source/OS pin mismatch: ' + key)
    actual = inspect_bytes(data)
    for key, value in actual.items():
        require(report[key] == value, 'Packed data differs from report: ' + key)
    require(report['logical_gib'] * GIB == actual['logical_bytes'], 'Logical GiB differs')
    require(report['diskutil_info']['VolumeUUID'].upper() == actual['data_volume_uuid'], 'diskutil UUID differs')
    require(report['inputs']['os']['expected_sha256'] == pins['os']['sha256'] and
            report['inputs']['os']['body_sha256_verified'] is True, 'Recorded OS provenance differs')
    require(report['inputs']['trust_bundle']['sha256'] == pins['trust_bundle']['sha256'] and
            report['inputs']['trust_bundle']['body_sha256_verified'] is True, 'Recorded trust provenance differs')
    for key in ('first_install', 'preserve_for_future_builds', 'diskutil_verify_volume_passed', 'mounts_detached'):
        require(report[key] is True, 'Missing creation evidence: ' + key)
    for key in ('private_device_identity_used', 'installed', 'runtime_tested', 'skip_setup', 'target_apfs_mount_verified'):
        require(report[key] is False, 'Unexpected artifact scope: ' + key)
    return dict(status='artifact-bytes-and-recorded-provenance-verified', **actual,
                macos_apfs_validation_reported=report['diskutil_verify_volume_passed'],
                apfs_validation_executed_here=False, hardware_validated=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    pins = json.loads(Path(__file__).with_name('data_seed_inputs.json').read_text())
    print(json.dumps(verify(args.directory, pins), indent=2))
