#!/usr/bin/env python3
"""Read back the existing archive; do not recompile it or link a kernel."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    args = parser.parse_args()
    build = args.build.resolve()
    prior = json.loads((build / 'result.json').read_text())
    assert prior['steps'][0]['name'] == 'official-build'
    assert prior['steps'][0]['exit_code'] == 0
    obj = build / 'build/sheng-ufs/NativeUfs.o'
    archive = build / 'build/sheng-ufs/libShengUfs.a'
    data = obj.read_bytes()
    magic, cpu, subtype, kind, count = struct.unpack_from('<5I', data)
    assert (magic, cpu, subtype, kind) == (0xfeedfacf, 0x100000c, 0, 1)
    at, versions = 32, []
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, at)
        assert length >= 8 and at + length <= len(data)
        if command == 0x32:
            versions.append(struct.unpack_from('<II', data, at + 8))
        at += length
    assert versions == [(2, 17 << 16)]
    command = json.loads((build / 'build/sheng-ufs/compile-command.json').read_text())
    assert command.count('-o') == 1 and command[command.index('-o') + 1] == str(obj)
    outputs = {}
    for name, argv in {
        'object-header': ['llvm-readobj', '--file-headers', str(obj)],
        'undefined-symbols': ['llvm-nm', '--undefined-only', str(archive)],
    }.items():
        text = subprocess.check_output(argv, text=True, timeout=30)
        (build / (name + '-verified.log')).write_text(text)
        outputs[name] = {'command': argv, 'exit_code': 0}
    manifest = json.loads((build.parent / 'source-manifest.json').read_text())
    source = Path(prior['links']['drivers/ShengUfs'])
    production = {n: sha(source / n) for n in manifest['files_sha256'] if not n.startswith('tests/')}
    assert all(digest == manifest['files_sha256'][name] for name, digest in production.items())
    prepared = Path(prior['prepared_abi_tree'])
    assert sha(prepared.parent / 'kernel/kernel.macho') == prior['input_hashes_before']['prior_kernel']
    report = {
        'passed': True, 'build_log': 'official-build.log', 'official_build_exit_code': 0,
        'original_postcheck_error': prior.get('error'),
        'postcheck_recovery': 'Use installed system LLVM tools; original .o/.a not rebuilt.',
        'source_revision': manifest['revision'], 'production_files_match_upstream': production,
        'object': {'format': 'Mach-O', 'architecture': 'arm64', 'subtype': 0,
                   'kind': 'relocatable', 'deployment': 'iOS 17.0'},
        'steps': outputs,
        'artifacts': {p.name: {'size': p.stat().st_size, 'sha256': sha(p)} for p in (obj, archive)},
        'listed_build_inputs_unchanged': prior['inputs_unchanged'],
        'previous_kernel_hash_rechecked': True,
        'linked_into_kernel': False, 'storage_abi_qualified': False, 'device_qualified': False,
    }
    path = build / 'verification.json'
    if path.exists():
        raise SystemExit('Refusing to overwrite existing verification')
    path.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'passed': True, 'artifacts': report['artifacts']}))


if __name__ == '__main__':
    main()
