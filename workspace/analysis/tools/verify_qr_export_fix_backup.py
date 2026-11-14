"""Extract this new archive safely and verify its Windows copy; no device access."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import subprocess
import sys
import tarfile

root = Path(sys.argv[1]).resolve()
reports = root / 'analysis/qr-export-fix-20261112'
spec = json.loads((reports / 'archive.json').read_text())
archive = root / 'artifacts' / spec['archive']
dest = root / 'artifacts/j716f-qr-export-fix-20261112'

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()

assert archive.stat().st_size == spec['bytes'] and digest(archive) == spec['sha256']
assert not dest.exists(), 'Refusing to overwrite existing artifact directory'
with tarfile.open(archive, 'r:gz') as tar:
    members = tar.getmembers()
    seen = set()
    for member in members:
        name = PurePosixPath(member.name)
        assert not name.is_absolute() and '..' not in name.parts
        assert '\\' not in member.name and ':' not in member.name
        assert name.parts and name.parts[0] == dest.name
        assert member.isdir() or member.isfile(), 'Links and special members are forbidden'
        assert member.name not in seen, 'Duplicate archive member'
        seen.add(member.name)
        assert (root / 'artifacts' / member.name).resolve().is_relative_to(dest)
    tar.extractall(root / 'artifacts', filter='data')

manifest = json.loads((dest / 'manifest.json').read_text())
assert len(manifest['outputs']) == spec['manifest_files']
for name, entry in manifest['outputs'].items():
    path = (dest / name).resolve()
    assert path.is_relative_to(dest) and path.is_file()
    assert path.stat().st_size == entry['bytes'] and digest(path) == entry['sha256'], name
actual = {p.relative_to(dest).as_posix() for p in dest.rglob('*') if p.is_file()}
assert actual == set(manifest['outputs']) | {'manifest.json'}

result = subprocess.run(
    [sys.executable, str(root / 'analysis/tools/verify_menu_artifact.py'), str(dest / 'menu')],
    capture_output=True, text=True)
assert result.returncode == 0, result.stdout + result.stderr
verified = json.loads(result.stdout)
assert verified['result'] == 'passed'
(reports / 'menu-verification.windows.json').write_text(json.dumps(verified, indent=2) + '\n')
summary = {'status': 'passed', 'archive_bytes': spec['bytes'],
           'archive_sha256': spec['sha256'], 'archive_members': len(members),
           'verified_manifest_files': len(manifest['outputs']),
           'manifest_sha256': digest(dest / 'manifest.json'),
           'menu_image': manifest['outputs']['menu/boot.img'],
           'independent_windows_menu_verification': 'passed',
           'device_actions': False, 'qemu': False, 'hardware_tested': False}
(reports / 'windows-artifact-checks.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary, indent=2))
