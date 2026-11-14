#!/usr/bin/env python3
"""Verify local delivery and retained baselines without contacting a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path


def disk_path(path):
    value = str(path.resolve())
    return Path('\\\\?\\' + value) if os.name == 'nt' and not value.startswith('\\\\?\\') else Path(value)


def identity(path):
    file = disk_path(path)
    digest = hashlib.sha256()
    with file.open('rb') as stream:
        while block := stream.read(8 << 20):
            digest.update(block)
    return dict(bytes=file.stat().st_size, sha256=digest.hexdigest())


def verify_manifest(root, manifest):
    mismatches = []
    for relative, expected in manifest.items():
        file = root / relative
        if not file.resolve().is_relative_to(root.resolve()):
            raise ValueError('Path escapes manifest root')
        try:
            observed = identity(file)
        except OSError as error:
            mismatches.append(dict(path=relative, error=str(error)))
            continue
        if observed != expected:
            mismatches.append(dict(path=relative, expected=expected, observed=observed))
    return dict(passed=not mismatches, files_checked=len(manifest), mismatches=mismatches)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve(strict=True)
    phase = root / 'analysis/r30-storage-inputs-20261114'
    image = root / 'artifacts/j716f-xnu-storage-inputs-20261114-r30s'
    raw = root / 'artifacts/j716f-ipados-root-inputs-20261114-r30/raw'
    expected_raw = json.loads((raw / 'OS.json').read_text())
    groups = {
        'image': verify_manifest(image, json.loads((image / 'manifest.json').read_text())['outputs']),
        'original_baselines': verify_manifest(root, json.loads((phase / 'baseline-protected.json').read_text())),
        'os_raw_transfer': verify_manifest(raw, {'OS.raw': dict(bytes=expected_raw['raw_bytes'], sha256=expected_raw['raw_sha256'])}),
    }
    report = dict(passed=all(item['passed'] for item in groups.values()), checks=groups,
                  method='streamed-sha256-with-absolute-Windows-namespaced-paths',
                  device_actions=False, hardware_tested=False)
    with (phase / 'delivery-verification.json').open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    print(json.dumps(report, indent=2))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
