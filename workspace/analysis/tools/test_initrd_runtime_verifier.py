"""Synthetic parser fault checks; none of these fixtures are runtime CRC evidence."""
import json
from pathlib import Path
import sys
import zlib

from verify_initrd_runtime import verify

root = Path(sys.argv[1]).resolve()
original = (root / 'analysis/device/20261112-initrd8m-b-200646/firmware.log').read_bytes()
payload = (root / 'artifacts/j716f-initrd-probe-8m-20261112/probe.img').read_bytes()
crc = f'{zlib.crc32(payload) & 0xffffffff:08x}'
start = b'J716FDIAG INITRD CRC32 begin [0xAF7FF000,0xAFFFF000) bytes=0x800000 phase=ReadyToBoot\n'
end = f'J716FDIAG INITRD CRC32 ok [0xAF7FF000,0xAFFFF000) bytes=0x800000 crc32={crc} phase=ReadyToBoot\n'.encode()
boundary = original.index(b'J716FDIAG END snapshot;')
fixture = original[:boundary] + start + end + original[boundary:]
cases = [
    ('synthetic-valid', fixture, payload, True),
    ('actual-old-log-no-crc', original, payload, False),
    ('truncated-pair', original[:boundary] + start + original[boundary:], payload, False),
    ('duplicate-success', fixture.replace(end, end + end), payload, False),
    ('range-disagreement', fixture.replace(end, end.replace(b'AF7FF000', b'AF7FE000')), payload, False),
    ('same-shift-both-records', fixture.replace(b'CRC32 begin [0xAF7FF000', b'CRC32 begin [0xAF7FE000').replace(b'CRC32 ok [0xAF7FF000', b'CRC32 ok [0xAF7FE000'), payload, False),
    ('wrong-host-content', fixture, payload[:-1] + bytes([payload[-1] ^ 1]), False),
    ('wrong-host-size', fixture, payload[:-1], False),
    ('outside-snapshot', original + start + end, payload, False),
    ('reverse-order', original[:boundary] + end + start + original[boundary:], payload, False),
    ('firmware-refusal', original[:boundary] + b'J716FDIAG INITRD CRC32 skipped base=0xAF7FF000 bytes=0x800000 reason=bounds\n' + original[boundary:], payload, False),
    ('malformed-checksum', fixture.replace(b'crc32=' + crc.encode(), b'crc32=123'), payload, False),
]
results = []
for name, raw, expected_payload, should_pass in cases:
    result = verify(raw, expected_payload)
    passed = result['status'] == 'passed'
    assert passed == should_pass, (name, result)
    results.append({'case': name, 'accepted': passed, 'expected_acceptance': should_pass})
print(json.dumps({'status': 'passed', 'scope': 'Synthetic verifier tests, not device results', 'cases': results}, indent=2))
