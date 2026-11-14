"""Create a separate r31 image target; never rewrite frozen r30 inputs."""
import hashlib
import json
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[2]
phase = Path(__file__).resolve().parent
old = root / 'work/UniXNU-j716f-xnu-storage-r30'
new = root / 'work/UniXNU-j716f-xnu-ufs-power-r31'
assert not new.exists()
protected = {}
for folder in ('work/UniXNU-j716f-xnu-storage-r30', 'work/UniXNU-j716f-host-storage-r30',
               'work/J716fStorageBringup-r30', 'artifacts/j716f-xnu-storage-inputs-20261114-r30s',
               'artifacts/j716f-host-storage-20261114-r30'):
    for p in (root / folder).rglob('*'):
        if p.is_file() and '.git' not in p.parts and '__pycache__' not in p.parts:
            protected[p.relative_to(root).as_posix()] = {
                'bytes': p.stat().st_size, 'sha256': hashlib.sha256(p.read_bytes()).hexdigest()}
(phase / 'r30-protected-before.json').write_text(json.dumps(protected, indent=2) + '\n')
shutil.copytree(old, new)
replacements = {
    'j716f-xnu-storage-r30': 'j716f-xnu-ufs-power-r31',
    'j716f_xnu_storage_r30': 'j716f_xnu_ufs_power_r31',
    'j716f-host-storage-20261114-r30': 'j716f-host-ufs-power-20261114-r31',
    'j716f-host-storage-r30': 'j716f-host-ufs-power-r31',
    'J716fStorageBringup-r30': 'J716fStorageBringup-r31',
    'r30-storage-inputs-20261114': 'r31-ufs-power-20261114',
    'j716f-xnu-storage-inputs-20261114-r30s': 'j716f-xnu-ufs-power-20261114-r31',
    'J716F XNU storage inputs r30s': 'J716F XNU UFS power r31',
    'Start XNU (storage pre-root checkpoint)': 'Start XNU (UFS power diagnostics)',
}
def translated(s):
    for a,b in replacements.items(): s=s.replace(a,b)
    return s
for p in new.rglob('*'):
    if p.is_file() and p.suffix != '.dtb':
        p.write_text(translated(p.read_text()), encoding='utf-8', newline='\n')
(new/'configs/j716f_xnu_storage_r30_defconfig').rename(new/'configs/j716f_xnu_ufs_power_r31_defconfig')
(new/'AGENTS.md').write_text('''# Independent r31 UFS Power diagnostic image

Keep the frozen r30/r30s targets and artifacts unchanged. Package the independent
r31 official host kernel and original 21H461 trust cache. Retain five manual
entries, default XNU, timeout=-1, CPU0/4KiB, 80MiB payload, 256MiB window, r22 FD
and hardware ADT. Verify the actual rebuilt executable, never old call addresses.
The only new hardware scope is a bounded read-only GCC UFS Power diagnostic;
no HCI/SMMU access, DMA, UFS commands, root mount, disk writes or handoff grants.
Keep the inittodr after-return six-second reset for recovery. Runtime observations
remain separate from host acceptance. Device deployment is the established
boot_b-only workflow when the image is verified and the user is ready.
''', encoding='utf-8')
(new/'README.md').write_text('''# J716F r31: UFS Power diagnostic

Independent successor to accepted r30s. Uses the new official host kernel with
the board Power-only probe, original 21H461 trust cache and unchanged early RAM
log / menu drawing fixes. Diagnostic records, not the six-second reset alone,
determine whether GCC reads completed and whether UFS power/clocks are ready.
The checkpoint remains after inittodr and before root lookup. Root storage,
APFS mounting, writable Data and iPadOS userspace are not yet qualified.
''', encoding='utf-8')
board = new/'board.toml'
board.write_text(board.read_text().replace(
    'TB-J716F independent r30 storage bridge and original trust cache, before root lookup; host preparation only',
    'TB-J716F r31 bounded read-only UFS GCC power diagnostics, before root lookup'), encoding='utf-8')
for name in ('build_storage_image.py', 'verify_storage_image.py', 'test_storage_image.py'):
    p=phase/name
    assert not p.exists()
    s=translated((root/'analysis/r30-storage-inputs-20261114'/name).read_text())
    s=s.replace('r30s image verification','r31 image verification').replace('independent r30s target','independent r31 target')
    s=s.replace('new storage registration kernel and original trust-cache; same pre-root checkpoint',
                'bounded GCC power-only diagnostic and original trust-cache; same pre-root checkpoint')
    p.write_text(s, encoding='utf-8', newline='\n')
print(json.dumps({'target':str(new),'protected_files':len(protected)}))
