#!/usr/bin/env python3
"""Record the complete source snapshot after the parent has copied final inputs."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
platform = 'socs/sm8250/kits/Sm8250Platform'
units = [platform + '/' + name + '.cpp' for name in (
    'Sm8250UfsRuntime', 'Sm8250UfsHci', 'Sm8250UfsHandoff', 'Sm8250UfsDiagnostics', 'AppsSmmuOwner')]
required = units + [platform + '/' + name + '.h' for name in (
    'Sm8250UfsRuntime', 'Sm8250UfsHci', 'Sm8250UfsHandoff', 'Sm8250UfsIo', 'Sm8250UfsDiagnostics', 'AppsSmmuOwner')]
required += ['drivers/ShengUfs/' + name + '.h' for name in (
    'RestrictedUfs', 'ReadCommandPolicy', 'UfsPlatform')]
for name in required:
    if not (root / name).is_file():
        raise SystemExit('Cannot freeze incomplete source snapshot: ' + name)
destination = root / 'docs/ufs-observation-inputs.json'
files = {}
for path in sorted(root.rglob('*')):
    relative = path.relative_to(root)
    if path == destination or not path.is_file():
        continue
    if path.is_symlink() or any(part in ('.git', '__pycache__', 'build', 'out', 'SDK') for part in relative.parts):
        raise SystemExit('Source snapshot contains non-source input: ' + str(relative))
    if path.suffix in ('.o', '.a', '.pyc'):
        raise SystemExit('Source snapshot contains compiled output: ' + str(relative))
    files[relative.as_posix()] = {'bytes': path.stat().st_size,
                                'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
manifest = {'schema_version': 1, 'base_target': 'UniXNU-j716f-host-ufs-adapter',
            'upstream_revision': 'dff276535db3ac394f628cec58a718cd46c20ac2',
            'shengufs_base_revision': 'b6fe07b307eeddc5e258c9d5d4387b968a55e4e9',
            'translation_units': units, 'files': files}
destination.write_text(json.dumps(manifest, indent=2) + '\n', newline='\n')
print('Frozen', len(files), 'source files:', destination)
