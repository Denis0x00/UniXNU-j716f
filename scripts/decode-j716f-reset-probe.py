#!/usr/bin/env python3
"""Decode P7 records from a privately collected, chronological Recovery console."""
import argparse
import hashlib
import json
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('console', type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--image', required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    image_sha = hashlib.sha256(args.image.read_bytes()).hexdigest()
    if manifest.get('image_sha256') != image_sha or manifest.get('version') != 'V23P7 RESET r1':
        parser.error('Image / P7 manifest mismatch')
    data = args.console.read_bytes()
    pattern = (rb'\[J716F-V23P7-r1 seq=(\d+) step=(PRE_SMC|RETURN_SMC|PRE_EFI_WARM|RETURN_EFI_WARM) '
               rb'fid=([0-9a-fA-F]{8}) x0=([0-9a-fA-F]{16}) x1=([0-9a-fA-F]{16}) '
               rb'x2=([0-9a-fA-F]{16}) x3=([0-9a-fA-F]{16})\]')
    records = []
    for match in re.finditer(pattern, data):
        records.append({'offset': match.start(), 'sequence': int(match[1]),
                        'step': match[2].decode(), 'fid': '0x'+match[3].decode().lower(),
                        **{f'x{i}': '0x'+match[4+i].decode().lower() for i in range(4)}})
    print(json.dumps({
        'console_sha256': hashlib.sha256(data).hexdigest(), 'tested_image_sha256': image_sha,
        'records': records,
        'interpretation': 'Correlate with filmed invocation/reset/Recovery order. PRE alone does not prove SMC executed; missing RETURN alone does not prove reset. Markers do not embed an image hash; the manifest checks the supplied image, not their provenance.',
        'claims_xnu_progress': False, 'automatically_claims_retention': False
    }, indent=2))


if __name__ == '__main__':
    main()
