"""Create the separate r31 Power-diagnostic host target without changing r30."""
from pathlib import Path
import hashlib
import json
import shutil

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
OLD = ROOT / 'work/UniXNU-j716f-host-storage-r30'
NEW = ROOT / 'work/UniXNU-j716f-host-ufs-power-r31'
if NEW.exists():
    raise SystemExit('Refusing to overwrite an existing r31 target')
def hashes(folder):
    return {p.relative_to(folder).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in folder.rglob('*') if p.is_file() and '__pycache__' not in p.parts}
before = hashes(OLD)
shutil.copytree(OLD, NEW, ignore=shutil.ignore_patterns('__pycache__', '*.pyc'))
parent = NEW / 'docs/parent-storage-r30'
parent.mkdir()
for name in ('host-source-manifest.json', 'storage-inputs.json'):
    shutil.move(str(NEW / 'docs' / name), str(parent / name))
renames = (
    ('configs/j716f_host_storage_r30_defconfig', 'configs/j716f_host_ufs_power_r31_defconfig'),
    ('scripts/build-j716f-storage-r30.py', 'scripts/build-j716f-ufs-power-r31.py'))
for old, new in renames:
    (NEW / old).rename(NEW / new)
for relative in ('board.toml','build-inputs.toml','porting.toml','scripts/host.py',
                 'scripts/build-j716f-ufs-power-r31.py','feeds.toml',
                 'configs/j716f_host_ufs_power_r31_defconfig',
                 'patches/xnu/j716f-storage-dependency.patch'):
    path = NEW / relative
    value = path.read_text().replace('host-storage-r30','host-ufs-power-r31').replace('host_storage_r30','host_ufs_power_r31')
    value = value.replace('J716fStorageBringup-r30','J716fStorageBringup-r31').replace('j716f-storage-r30','j716f-ufs-power-r31')
    value = value.replace('Independent r30 storage integration','Independent r31 Power-only diagnostics')
    value = value.replace('independent r30 storage integration','independent r31 Power-only diagnostics')
    value = value.replace('r30 storage integration, seven fresh','r31 Power-only diagnostics, seven fresh')
    value = value.replace('two storage integration patches','three storage integration/Power-call patches')
    path.write_text(value, encoding='utf-8', newline='\n')
recipe = NEW / 'scripts/build-j716f-ufs-power-r31.py'
value = recipe.read_text().replace("'init', 'status', 'observe', 'allocate', 'activate', 'read_known', 'stop', 'discard_unused'))",
    "'init', 'status', 'observe', 'allocate', 'activate', 'read_known', 'stop', 'discard_unused')) + ('_j716f_storage_power_probe',)")
value = value.replace("'patches/xnu/j716f-storage-init.patch',", "'patches/xnu/j716f-storage-init.patch',\n        'patches/xnu/j716f-power-probe-call.patch',")
recipe.write_text(value, encoding='utf-8', newline='\n')
(NEW / 'patches/xnu/j716f-power-probe-call.patch').write_text('''--- a/bsd/kern/bsd_init.c
+++ b/bsd/kern/bsd_init.c
@@ -1310,6 +1310,8 @@ bsd_autoconf(void)
 \tif (result == KERN_SUCCESS) {
 \t\textern void j716f_storage_bringup_init(void);
+\t\textern unsigned int j716f_storage_power_probe(void);
 \t\tj716f_storage_bringup_init();
+\t\t(void)j716f_storage_power_probe();
 \t} else {
 \t\tprintf("[J716F storage] registration skipped; IOKitBSDInit failed\\n");
 \t}
''', encoding='utf-8', newline='\n')
with (NEW / 'patches/xnu/series').open('a', encoding='utf-8', newline='\n') as stream:
    stream.write('j716f-power-probe-call.patch\n')
(NEW / 'AGENTS.md').write_text('''# Independent r31 Power-diagnostic host target

User-authorized extension of the accepted r30 storage target. Preserve the first
60 XNU patches, first eight archive recipes, CPU0 repairs, early-log/text-console,
DT panic setting and 21H461 prelink ABI. Use the independent r31 component and
output tree. Do not change frozen r30 targets, manifests, kernels or images.

After IOKitBSDInit returns zero, register the storage interface and call the
explicit Power-only board entry. That entry must enforce its own reviewed live
read/abort-containment gates; a blocked gate is not hardware success. Do not
invent authority, enter Host/Geometry stages, transfer firmware ownership, issue
DMA/storage writes or mount root. Preserve the after-inittodr six-second boundary.

Use scripts/host.py for the official CLI pipeline. Fresh native/prelink outputs
need a new dynamic machine review before packaging. No VM/device/QEMU action is
part of this preparation task. Host evidence stays separate from tablet results.
''', encoding='utf-8', newline='\n')
(NEW / 'README.md').write_text('''# Independent r31 Power-only diagnostic host target

This target extends the accepted r30 storage target in a separate directory.
It preserves the original 60 patches/eight archive recipes, CPU0 repair, early
RAM log, text console, DT panic setting and iPadOS 17.7.11/21H461 ABI. The storage
archive now resolves the explicit `J716fStorageBringup-r31` local component.

After IOKitBSDInit returns zero, startup registers the storage interface and
calls `j716f_storage_power_probe()`. The component alone decides whether its
reviewed Power read/abort-containment gates permit a read. A logged blocked gate
is a valid diagnostic outcome, not evidence of a working UFS controller. This
target does not advance to Host/Geometry, transfer ownership, issue DMA/storage
writes, publish a block device or mount root.

Run `python3 scripts/host.py defconfig`, `fetch`, `status`, `tree`, then `kernel`
on the prepared Linux host. This wrapper invokes the official UniXNU CLI with
independent output `UniXNU/out-host-ufs-power-r31-20261114`. Source caches may be
reused; r30 objects and compile records must not substitute for the fresh build.

The new executable must be reparsed before retaining the diagnostic boundary
after inittodr returns, with six-second reset before root lookup. Host tests,
tablet execution, storage reads and userland startup remain separate evidence.
Historical manifests in `docs/parent-storage-r30/` describe the frozen parent;
the current manifest is generated only after the new component is frozen.
''', encoding='utf-8', newline='\n')
v = NEW / 'porting.toml'
t = v.read_text().replace('IOKitBSDInit-success registration and pre-inittodr milestone logging',
    'IOKitBSDInit-success registration, explicitly gated Power-only probe and pre-inittodr milestone logging')
t = t.replace('register the adapter and emit bounded stage/error/gate logs only.',
    'register the adapter, invoke only the gated Power diagnostic and emit bounded stage/error/gate logs.')
t = t.replace('No live controller ownership, MMIO, DMA, capacity, block read, root device or filesystem mount is established by this target.',
    'Power reads require independently reviewed current access and abort-containment facts; no host result establishes live MMIO, ownership, DMA, capacity, block read, root device or mount.')
v.write_text(t, encoding='utf-8', newline='\n')
assert hashes(OLD) == before
report = {'source_target': OLD.relative_to(ROOT).as_posix(), 'new_target': NEW.relative_to(ROOT).as_posix(),
          'parent_unchanged': True, 'parent_files': before,
          'status': 'skeleton-created-r31-component-freeze-pending', 'host_build': False, 'device_actions': False}
with (HERE / 'creation.json').open('x', encoding='utf-8', newline='\n') as stream:
    json.dump(report, stream, indent=2); stream.write('\n')
print(report['new_target'])
