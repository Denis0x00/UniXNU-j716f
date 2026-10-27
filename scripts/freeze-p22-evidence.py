#!/usr/bin/env python3
"""Hash the closed P22 experiment without rewriting any historical result.

The output is PRIVATE: it includes local evidence paths, never media contents.
An existing manifest is verified, not refreshed. No hardware access is made.
"""
import argparse
import hashlib
import json
from pathlib import Path


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def freeze(workspace, output):
    workspace, output = Path(workspace).resolve(), Path(output).resolve()
    experiment = workspace/'Firmware/V23P22'
    image = workspace/'Firmware/j716f-mu-zui13-kernel-trace-v23p22m-boot.img'
    if output == experiment or not output.is_relative_to(experiment):
        raise ValueError('Freeze manifest must stay in the private P22 directory')
    if output.exists():
        report = json.loads(output.read_text(encoding='utf-8'))
        for item in report['files']:
            path = (workspace/item['path']).resolve()
            if not path.is_relative_to(workspace) or sha(path) != item['sha256']:
                raise ValueError('Frozen evidence changed: '+item['path'])
        return {'status': 'frozen-files-verified', 'files': len(report['files'])}
    if sha(image) != 'c49fb7542528ca3a8aca005a2f0f4fb304e6a43f379da65555a6525519e3ced2':
        raise ValueError('P22 image does not match the observed experiment')
    result = json.loads((experiment/'post-reboot-observation.json').read_text())
    if not result['full_arm_vm_init_return_observed'] or not result['all_nine_record_crcs_match']:
        raise ValueError('Successful bounded VM-return evidence is missing')
    paths = sorted(p for p in experiment.rglob('*') if p.is_file()) + [image]
    device = workspace/'github-export/UniXNU-j716f'
    paths += [device/'scripts'/name for name in (
        'build-v23p22-kernel.py', 'v23p22-vm-return.S', 'decode-j716f-v23p22.py')]
    report = {
        'schema': 1, 'status': 'closed-historical-experiment',
        'policy': 'Preserve all files; no default successor or repeat launch. Verify, never refresh this manifest.',
        'scope': 'Bounded original arm_vm_init return only; not native platform/IRQ/scheduler qualification.',
        'files': [{'path': p.relative_to(workspace).as_posix(), 'bytes': p.stat().st_size,
                   'sha256': sha(p)} for p in paths],
    }
    with output.open('x', encoding='utf-8') as stream:
        stream.write(json.dumps(report, indent=2)+'\n')
    return {'status': report['status'], 'files': len(paths), 'manifest_sha256': sha(output)}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(freeze(args.workspace, args.output), indent=2))
