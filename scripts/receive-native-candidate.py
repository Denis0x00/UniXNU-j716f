#!/usr/bin/env python3
"""Authenticate/decrypt one selected CI ZIP into a new PRIVATE directory.

Use GitHub's normal signed-in artifact download. No credential discovery,
publication, private-key export, overwrite, prelink or hardware operations.
"""
import argparse
import io
import json
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

from native_candidate import HERE, KERNEL, check_kernel, check_report, check_selected_logger, save_new, sha


def outer(data, lock):
    if len(data) != lock['artifact_bytes'] or sha(data) != lock['artifact_sha256']:
        raise ValueError('GitHub artifact ZIP differs from selected run/digest')
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        if archive.namelist() != ['j716f-source-only.enc']:
            raise ValueError('Unexpected outer artifact entries')
        info = archive.infolist()[0]
        if info.file_size > 64*1024*1024 or info.is_dir():
            raise ValueError('Unreasonable encrypted artifact size')
        return archive.read(info)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--artifact-zip', type=Path, required=True)
    parser.add_argument('--private-key', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--selection', type=Path, default=HERE.parent/'configs/native-log-candidate-40458123294.json')
    args = parser.parse_args()
    lock = json.loads(args.selection.read_text(encoding='utf-8'))
    encrypted = outer(args.artifact_zip.read_bytes(), lock)
    if not args.private_key.is_file():
        raise ValueError('Local decryption key is missing')
    if args.output.exists():
        raise ValueError('Refuse to overwrite received candidate/evidence')
    args.output.mkdir(parents=True)
    with (args.output/'j716f-source-only.enc').open('xb') as stream:
        stream.write(encrypted)
    shutil.copyfile(args.artifact_zip, args.output/'github-artifact.zip')
    # Existing authenticated transfer performs OAEP/AES-GCM and exact manifest checks.
    subprocess.run([sys.executable, str(HERE/'source-artifact-transfer.py'), 'open',
                    '--private-key', str(args.private_key), '--input', str(args.output/'j716f-source-only.enc'),
                    '--output', str(args.output/'source-outputs.zip')], check=True)
    with zipfile.ZipFile(args.output/'source-outputs.zip') as archive:
        report = json.loads(archive.read('source-build-report.json'))
        names, entries = check_report(report, lock)
        if len(archive.namelist()) != 7 or set(archive.namelist()) != set(names) | {'source-build-report.json'}:
            raise ValueError('Unexpected decrypted manifest')
        contents = {}
        for name in names:
            item = archive.getinfo(name)
            if item.file_size > 128*1024*1024:
                raise ValueError('Artifact too large')
            content = archive.read(name)
            if sha(content) != entries[name]['sha256'] or len(content) != entries[name]['bytes']:
                raise ValueError('Decrypted content hash/size mismatch')
            contents[name] = content
        kernel = check_kernel(contents[KERNEL])
    # No extractall: names come solely from the reviewed exact allowlist.
    for name, content in contents.items():
        target = args.output/'tree'/name
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as stream:
            stream.write(content)
    save_new(args.output/'source-build-report.json', report)
    logger = check_selected_logger(args.output/'tree'/KERNEL, report, lock)
    receipt = {'schema': 1, 'stage': 'native-artifact-received', 'selection': lock,
               'selection_sha256': sha(args.selection.read_bytes()), 'kernel': kernel,
               'artifacts': report['artifacts'], 'prelink_performed': False,
               'logger_checks': logger,
               'persistent_early_logger_present': logger is not None, 'hardware_launch_admitted': False,
               'device_writes_performed': False}
    save_new(args.output/'receipt.json', receipt)
    print(json.dumps({'stage': receipt['stage'], 'kernel_sha256': kernel['kernel_sha256'],
                      'artifact_count': len(contents), 'hardware_launch_admitted': False}, indent=2))


if __name__ == '__main__':
    main()
