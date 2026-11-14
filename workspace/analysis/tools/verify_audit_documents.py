"""Back up reviewed documentation before upload, or verify uploaded hashes/schema."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('backup', 'verify'))
    parser.add_argument('--workspace', type=Path, required=True)
    args = parser.parse_args()
    root = args.workspace.resolve()
    out = root / 'analysis/progress-upstream-audit-20261112'
    plan = json.loads((out / 'document-upload-plan.json').read_text(encoding='utf-8'))
    files = []
    for entry in plan['files']:
        path = root / entry['path']
        path.resolve().relative_to(root)
        current = hashlib.sha256(path.read_bytes()).hexdigest() if path.is_file() else None
        if args.action == 'backup':
            if entry['remote_before_sha256'] and current != entry['remote_before_sha256']:
                raise RuntimeError('Remote document changed after review: ' + entry['path'])
            if path.is_file():
                dest = root / plan['backup'] / entry['path']
                dest.parent.mkdir(parents=True, exist_ok=True)
                if dest.exists():
                    raise RuntimeError('Backup already exists: ' + str(dest))
                shutil.copy2(path, dest)
            path.parent.mkdir(parents=True, exist_ok=True)
        else:
            if current != entry['sha256']:
                raise RuntimeError('Uploaded document hash mismatch: ' + entry['path'])
        files.append({'path': entry['path'], 'sha256': current})
    result = {'action': args.action, 'files': files, 'success': True}
    if args.action == 'verify':
        result['records'] = []
        for name in ('UniXNU-j716f', 'UniXNU-j716f-host', 'UniXNU-j716f-menu'):
            command = [sys.executable, str(root / 'UniXNU/scripts/porting-record.py'),
                       'check', '--device', str(root / 'work' / name)]
            p = subprocess.run(command, capture_output=True, text=True, timeout=30)
            result['records'].append({'target': name, 'exit_code': p.returncode,
                                      'stdout': p.stdout, 'stderr': p.stderr})
        result['success'] = all(x['exit_code'] == 0 for x in result['records'])
    (out / ('vm-document-' + args.action + '.json')).write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({'success': result['success'], 'files': len(files), 'action': args.action}))
    return 0 if result['success'] else 1


if __name__ == '__main__':
    sys.exit(main())
