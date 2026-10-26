#!/usr/bin/env python3
"""Find V23P6 markers in a privately collected Recovery console file."""
import argparse
import hashlib
import json
from pathlib import Path
import re

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('console', type=Path)
    p.add_argument('--manifest', required=True, type=Path, help='Exact tested boot image manifest')
    p.add_argument('--image', required=True, type=Path)
    a = p.parse_args()
    manifest = json.loads(a.manifest.read_text())
    image_sha = hashlib.sha256(a.image.read_bytes()).hexdigest()
    if manifest['image_sha256'] != image_sha or manifest['version'] != 'V23P6 RAMLOG r1':
        p.error('Image / P6 manifest mismatch')
    data = a.console.read_bytes()
    records = [{'offset': m.start(), 'sequence': int(m[1]), 'build': m[2].decode()}
               for m in re.finditer(rb'\[J716F-V23P6-r1 seq=(\d+) build=(20261025)\]', data)]
    print(json.dumps({'console_sha256': hashlib.sha256(data).hexdigest(),
                      'tested_image_sha256': image_sha, 'records': records,
                      'interpretation': 'Marker found: correlate with recorded manual append/reset/recovery sequence. Absence alone does not locate the loss stage.',
                      'claims_xnu_progress': False}, indent=2))

if __name__ == '__main__': main()
