"""Create a separate host target; never overwrite the accepted r29/r30 inputs."""
from pathlib import Path
import hashlib
import json
import shutil

ROOT = Path(__file__).resolve().parents[3]
OLD = ROOT / 'work/UniXNU-j716f-host-iokit-cpu0'
NEW = ROOT / 'work/UniXNU-j716f-host-storage-r30'
if NEW.exists():
    raise SystemExit('Refusing to replace an existing target')
before = {p.relative_to(OLD).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
          for p in OLD.rglob('*') if p.is_file() and '__pycache__' not in p.parts}
shutil.copytree(OLD, NEW, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
for name in ('README.md', 'board.toml', 'build-inputs.toml', 'porting.toml', 'scripts/host.py'):
    target = NEW / name
    value = target.read_text(encoding='utf-8')
    value = value.replace('host-iokit-cpu0', 'host-storage-r30')
    value = value.replace('host_iokit_cpu0', 'host_storage_r30')
    target.write_text(value, encoding='utf-8', newline='\n')
(NEW / 'configs/j716f_host_iokit_cpu0_defconfig').rename(
    NEW / 'configs/j716f_host_storage_r30_defconfig')
parent = NEW / 'docs/parent-iokit-cpu0'
parent.mkdir()
for name in ('host-source-manifest.json', 'cpu0-source-delta.json', 'armcpu-cpu0-separators.patch'):
    shutil.move(str(NEW / 'docs' / name), str(parent / name))
(NEW / 'AGENTS.md').write_text('''# Independent r30 storage integration host target

User-authorized extension of the accepted host-iokit-cpu0 target. Preserve its
60 XNU patches, CPU0 asm repairs, early log, text console, DT panic setting,
21H461 prelink ABI and eight archive recipes. Append the reviewed storage
component and explicit initialization recipe only in this independent target.

Use scripts/host.py for the official defconfig/status/tree/build kernel pipeline.
Every host build result is separate from hardware evidence. Do not modify the
parent target, frozen core or r29/r30 images. The storage bootstrap may register
interfaces and report gates; it must not invent a handoff, acquire controller
ownership, access MMIO, issue DMA or block writes, or mount a root filesystem.
No device operations or QEMU. Reparse the new executable before packaging.
''', encoding='utf-8', newline='\n')
(NEW / 'README.md').write_text('''# Independent r30 storage integration host build

This target extends `UniXNU-j716f-host-iokit-cpu0` without changing the accepted
r29 source kernel or the existing r30 diagnostic image. It retains the 60 selected
XNU patches, CPU0 repairs, early RAM log, text console and 21H461 prelink profile.
The storage addition is an explicit registration and state-reporting step. It
does not provide controller handoff evidence, enable native SM8550 UFS, or mount
root/Data. Current hardware progress remains the separately recorded r29 test.

Run `python3 scripts/host.py defconfig`, `fetch`, `status`, `tree`, then `kernel`
on the prepared Linux host. The wrapper invokes the official UniXNU CLI, with
independent output `UniXNU/out-host-storage-r30-20261114`. Source caches can be
reused; archived kernels, old objects and compile records cannot seed this build.

`docs/parent-iokit-cpu0/` records the historical accepted parent, not this target's
current input manifest. New source inputs and build evidence are recorded under
`analysis/r30-storage-inputs-20261114/kernel-target`. Device execution is pending.
''', encoding='utf-8', newline='\n')
assert before == {p.relative_to(OLD).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
                 for p in OLD.rglob('*') if p.is_file() and '__pycache__' not in p.parts}
report = {'source_target': OLD.relative_to(ROOT).as_posix(),
          'new_target': NEW.relative_to(ROOT).as_posix(),
          'parent_unchanged': True, 'parent_files': before,
          'status': 'skeleton-created-storage-integration-pending'}
(Path(__file__).parent / 'creation.json').write_text(json.dumps(report, indent=2) + '\n')
print(report['new_target'])
