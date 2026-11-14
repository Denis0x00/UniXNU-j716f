"""Compare one guarded ReadyToBoot CRC32 measurement with an exact host initrd.

CRC32 is a corruption check at the logged phase, not a cryptographic runtime
identity, a measurement of earlier BootShim inputs, or a final EBS ownership test.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import zlib

from analyze_j716f_firmware_log import analyze


def verify(raw, payload):
    audit = analyze(raw)
    errors = list(audit['errors'])
    text = raw.decode('utf-8')
    lines = text.splitlines()
    prefix = 'J716FDIAG INITRD CRC32 '
    records = [(i, line.split(prefix, 1)[1]) for i, line in enumerate(lines, 1) if prefix in line]
    expected = {'bytes': len(payload), 'sha256': hashlib.sha256(payload).hexdigest(),
                'crc32': f'{zlib.crc32(payload) & 0xffffffff:08x}'}
    measurement = None
    common = r'\[(0x[0-9a-fA-F]+),(0x[0-9a-fA-F]+)\) bytes=(0x[0-9a-fA-F]+)'
    begin_re = re.compile(r'begin ' + common + r' phase=ReadyToBoot$')
    ok_re = re.compile(r'ok ' + common + r' crc32=([0-9a-fA-F]{8}) phase=ReadyToBoot$')
    if len(records) != 2:
        errors.append(f'Expected exactly one CRC32 begin and ok pair, found {len(records)} CRC32 records')
    else:
        first, last = begin_re.fullmatch(records[0][1]), ok_re.fullmatch(records[1][1])
        if not first or not last:
            errors.append('CRC32 measurement missing, skipped, failed, malformed or out of order')
        else:
            start, end, size = map(lambda x: int(x, 16), first.groups())
            final_start, final_end, final_size = map(lambda x: int(x, 16), last.groups()[:3])
            checksum = last.group(4).lower()
            measurement = {'start': start, 'end': end, 'bytes': size, 'crc32': checksum,
                           'phase': 'ReadyToBoot', 'begin_line': records[0][0], 'ok_line': records[1][0]}
            if (start, end, size) != (final_start, final_end, final_size):
                errors.append('CRC32 begin and ok ranges disagree')
            if not (0 < size <= 80 * 1024 * 1024 and 0 < start < end < 2**64 and end - start == size):
                errors.append('CRC32 size or range is invalid or exceeds 80 MiB')
            if size != expected['bytes'] or checksum != expected['crc32']:
                errors.append('Runtime size or CRC32 does not match the supplied host initrd')
            markers = audit['markers']
            if len(markers['begin']) == len(markers['end']) == 1:
                if not markers['begin'][0] < records[0][0] < records[1][0] < markers['end'][0]:
                    errors.append('CRC32 measurement is outside the single ReadyToBoot snapshot')
            r = audit['ranges'].get('incoming-initrd')
            if not r or (r['start'], r['end'], r['bytes']) != (start, end, size):
                errors.append('CRC32 range does not match the reported incoming initrd')
            if not r or not r['fully_covered'] or r['types'] != [0]:
                errors.append('CRC32 range lacks full Reserved descriptor coverage')
            for d in audit['map']['descriptors']:
                if max(start, d['start']) < min(end, d['end']) and d['attributes'] & 0x2000:
                    errors.append('CRC32 range intersects an EFI_MEMORY_RP descriptor')
    if audit['conflict_lines'] or audit['reported_range_collisions']:
        errors.append('Snapshot reports a conflict or overlapping diagnostic object ranges')
    return {'schema_version': 1, 'status': 'passed' if not errors else 'not-verified',
            'log': {'bytes': len(raw), 'sha256': audit['sha256']},
            'expected_host_initrd': expected, 'runtime_measurement': measurement,
            'errors': errors, 'diagnostic_structure_valid': audit['diagnostic_structure_valid'],
            'limitations': ['CRC32 is non-cryptographic and applies only at ReadyToBoot.',
                           'Image-to-log association still requires the deployment record.',
                           'No BootShim source-address, final EBS map or XNU execution is inferred.']}


if __name__ == '__main__':
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--log', type=Path, required=True)
    p.add_argument('--payload', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if a.output.exists() or a.output.resolve() in (a.log.resolve(), a.payload.resolve()):
        p.error('Refusing to overwrite an input or an existing result')
    result = verify(a.log.read_bytes(), a.payload.read_bytes())
    a.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['status'] == 'passed' else 1)
