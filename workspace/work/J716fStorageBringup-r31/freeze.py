#!/usr/bin/env python3
"""Pin independent r31 experiment and unchanged r30 bridge/adapter inputs."""
import argparse
import hashlib
import json
from pathlib import Path


def identity(path):
    data = path.read_bytes()
    return dict(bytes=len(data), sha256=hashlib.sha256(data).hexdigest())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    component = Path(__file__).resolve().parent
    root = component.parents[1]
    old = root / 'work/J716fStorageBringup-r30/source-manifest.json'
    previous = json.loads(old.read_text())
    for relative, expected in previous['files'].items():
        if identity(root / relative) != expected:
            raise ValueError('Frozen r30 input changed: ' + relative)
    for name in ('J716fStorageBringup.h', 'J716fStorageBringup.cpp', 'J716fStorageKernel.cpp'):
        if identity(component / name) != identity(old.parent / name):
            raise ValueError('Original bridge contract changed: ' + name)
    target = root / 'work/UniXNU-j716f-host-storage-r30'
    platform = target / 'socs/sm8250/kits/Sm8250Platform'
    files = {}
    for folder in (component, platform, target / 'drivers/ShengUfs'):
        for path in sorted(folder.rglob('*')):
            if path.is_file() and path.suffix in ('.cpp', '.h', '.py', '.md'):
                if path.is_symlink():
                    raise ValueError('Symlink source: ' + str(path))
                files[path.relative_to(root).as_posix()] = identity(path)
    tus = [component / (name + '.cpp') for name in ('J716fStorageBringup', 'J716fStorageKernel',
                                                  'J716fStoragePower', 'J716fStoragePowerKernel')]
    tus += [platform / (name + '.cpp') for name in
            ('Sm8250UfsRuntime', 'Sm8250UfsHci', 'Sm8250UfsHandoff', 'Sm8250UfsDiagnostics', 'AppsSmmuOwner')]
    result = dict(schema_version=1, component='J716fStorageBringup-r31',
                  target='TB-J716F / SM8250 / iPadOS 17.7.11 21H461 / ordinary arm64',
                  previous_component_manifest=identity(old),
                  automatic_behavior='registration then experimental one-shot fixed GCC reads with no abort containment',
                  translation_units=[path.relative_to(root).as_posix() for path in tus], files=files,
                  hardware_qualified=False, root_device_published=False, external_abort_containment=False)
    destination = component / 'source-manifest.json'
    if args.check:
        if json.loads(destination.read_text()) != result:
            raise ValueError('r31 source manifest differs')
    else:
        destination.write_text(json.dumps(result, indent=2) + '\n')
    print(f"PASS: {len(files)} pinned files, nine TUs, one owner; r30 untouched")


if __name__ == '__main__':
    main()
