#!/usr/bin/env python3
"""Decode CRC-checked P8/P9 records; input stays private. No device commands."""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import struct

CAPACITY = 0x3fff4
PHASES = {1: 'INIT', 2: 'BASELINE', 3: 'PRE_RESET', 4: 'RESET_RETURN',
          5: 'PRE_VERSION', 6: 'VERSION_RETURN', 7: 'PRE_FEATURE', 8: 'FEATURE_RETURN',
          9: 'ARM_KERNEL', 10: 'KERNEL_ENTRY', 11: 'PRE_MMU', 12: 'POST_MMU',
          13: 'MAP_FAILURE', 14: 'KERNEL_RESET_RETURN'}


def scan(data, circular=False):
    records = []
    view = data + data[:63] if circular else data
    for at in range(len(data) if circular else max(0, len(data) - 63)):
        p = view[at:at + 64]
        if p[:8] != b'J716FP8!' or len(p) != 64 or p[56:] != b'P8DONE!\n':
            continue
        version, size, token, phase = struct.unpack_from('<4I', p, 8)
        crc, inverse = struct.unpack_from('<2I', p, 48)
        if (version != 1 or size != 64 or not token or phase not in PHASES or
                p[44:48] != b'\0'*4 or crc != binascii.crc32(p[:48]) or inverse != crc ^ 0xffffffff):
            continue
        sig, start, length = struct.unpack_from('<3I', p, 32)
        records.append({'offset': at, 'id': token, 'phase': PHASES[phase],
                        'crc32': f'{crc:08x}', 'value': hex(struct.unpack_from('<Q', p, 24)[0]),
                        'before_header': {'signature': hex(sig), 'next': start, 'size': length}})
    return records


def decode(data, kind):
    result = {'input_sha256': hashlib.sha256(data).hexdigest(), 'input_kind': kind}
    if kind == 'raw-console':
        if len(data) != CAPACITY + 12:
            raise ValueError('Raw console must be exactly 0x40000 bytes, including the 12-byte header')
        sig, start, size = struct.unpack_from('<3I', data)
        valid = sig == 0x43474244 and start < CAPACITY and size <= CAPACITY and start <= size
        result['header'] = {'signature': hex(sig), 'next': start, 'size': size, 'valid': valid}
        payload = data[12:]
        logical = payload[start:] + payload[:start] if valid and size == CAPACITY else payload[:size] if valid else b''
        result['logical_records'] = scan(logical)
        result['raw_records_may_be_stale'] = scan(payload, True)
    else:
        result['pstore_records'] = scan(data)
    result['interpretation'] = ('Match the photographed pre-reset ID and CRC in a controlled reboot. '
                                'Raw records alone establish surviving payload bytes, not a valid ring or the reset path. '
                                'Absence does not identify the loss stage.')
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path)
    p.add_argument('--kind', choices=['raw-console', 'pstore'], required=True)
    p.add_argument('--capture-manifest', type=Path, help='Required for pstore; accepted collector entry and SHA must match')
    a = p.parse_args()
    data = a.input.read_bytes()
    if a.kind == 'pstore':
        if not a.capture_manifest:
            p.error('pstore requires --capture-manifest; error text is not a log')
        manifest = json.loads(a.capture_manifest.read_text())
        accepted = [c for c in manifest['captures'] if c['file'] == a.input.name and
                    c.get('accepted') is True and c.get('remote_status') == 0 and
                    c['sha256'] == hashlib.sha256(data).hexdigest()]
        if len(accepted) != 1:
            p.error('No uniquely accepted capture matching this file and hash')
    print(json.dumps(decode(data, a.kind), indent=2))


if __name__ == '__main__':
    main()
