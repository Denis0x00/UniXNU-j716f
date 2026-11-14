"""Validate independent official configuration and source integration, without building."""
from pathlib import Path
import ast
import hashlib
import json
import os
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
DEVICE = ROOT / 'work/UniXNU-j716f-host-storage-r30'
PARENT = ROOT / 'work/UniXNU-j716f-host-iokit-cpu0'
env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def run(args, label, cwd=ROOT):
    result = subprocess.run([str(x) for x in args], cwd=cwd, env=env,
                            text=True, encoding='utf-8', capture_output=True)
    (HERE / (label + '.stdout.log')).write_text(result.stdout, encoding='utf-8')
    (HERE / (label + '.stderr.log')).write_text(result.stderr, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(label + ': ' + result.stderr)
    return result.stdout

cli = [sys.executable, ROOT / 'UniXNU/unixnu']
common = ['--os', 'ipados-17.7.11-21H461', '--out', HERE / 'official-status-out']
local_module = ['--local-module', 'driver:J716fStorageBringup=' + str(ROOT / 'work/J716fStorageBringup-r30')]
run([*cli, 'defconfig', 'j716f_host_storage_r30_defconfig', '-D', DEVICE, *common, *local_module], 'defconfig')
status = json.loads(run([*cli, 'status', '--json', '-D', DEVICE, *common, *local_module], 'status'))
baseline = json.loads(run([*cli, 'status', '--json', '-D', PARENT, *common], 'parent-status'))
(HERE / 'status.json').write_text(json.dumps(status, indent=2) + '\n')
(HERE / 'parent-status.json').write_text(json.dumps(baseline, indent=2) + '\n')
assert status['name'] == 'j716f-host-storage-r30' and status['configured']
assert len(baseline['patch_order']) == 60 and len(status['patch_order']) == 62
assert [sha(Path(p)) for p in baseline['patch_order']] == [sha(Path(p)) for p in status['patch_order'][:60]]
assert status['board']['kernel']['archives'][:8] == baseline['board']['kernel']['archives']
assert len(status['board']['kernel']['archives']) == 9
assert status['board']['kernel']['prelink'] == baseline['board']['kernel']['prelink']
assert [c['id'] for c in status['components']] == [c['id'] for c in baseline['components']]
bridge = [m for m in status['modules'] if m['kind'] == 'driver' and m['name'] == 'J716fStorageBringup']
assert len(bridge) == 1 and bridge[0]['origin'] == 'local (--local-module)'
assert Path(bridge[0]['path']).resolve() == (ROOT / 'work/J716fStorageBringup-r30').resolve()
for rel in ('dts/j716f.dts', 'socs/sm8250/dts/sm8250.dtsi'):
    assert (DEVICE / rel).read_bytes() == (PARENT / rel).read_bytes()
original = (PARENT / 'configs/j716f_host_iokit_cpu0_defconfig').read_text()
extended = (DEVICE / 'configs/j716f_host_storage_r30_defconfig').read_text()
assert extended.startswith(original)
assert extended[len(original):].strip() == (
    '# Five frozen SM8250 TUs plus two board transaction/logging TUs; one owner.\n'
    'CONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,{tree}/build/j716f-storage-r30/libJ716fStorageBringup.a"')

scratch = HERE / 'patch-check'
source = ROOT / 'analysis/xnu-r30-pre-root-planning-20261114/sources/xnu/bsd/kern/bsd_init.c'
target = scratch / 'bsd/kern/bsd_init.c'
target.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(source, target)
patch = DEVICE / 'patches/xnu/j716f-storage-init.patch'
run(['git', 'apply', '--check', patch], 'init-patch-check', cwd=scratch)
run(['git', 'apply', patch], 'init-patch-apply', cwd=scratch)
patched = target.read_text()
assert patched.count('j716f_storage_bringup_init();') == 1
assert patched.count('inittodr(0);') == 1
assert patched.index('[J716F boot] BSD pre-root reached;') < patched.index('inittodr(0);')
run(['git', 'apply', '--numstat', DEVICE / 'patches/xnu/j716f-storage-dependency.patch'], 'dependency-patch-format')
for script in (DEVICE / 'scripts').glob('*.py'):
    ast.parse(script.read_text(), filename=str(script))
record = run([sys.executable, ROOT / 'UniXNU/scripts/porting-record.py', 'check', '--device', DEVICE], 'porting-record')
creation = json.loads((HERE / 'creation.json').read_text())
for rel, expected in creation['parent_files'].items():
    assert sha(PARENT / rel) == expected, 'Frozen parent changed: ' + rel
report = {
    'status': 'passed', 'host_compile_executed': False, 'device_actions': False,
    'parent_unchanged': True, 'retained_patches': 60, 'appended_patches': 2,
    'retained_archive_recipes': 8, 'total_archive_recipes': 9,
    'retained_component_ids': [c['id'] for c in status['components']],
    'retained_prelink_abi': True, 'retained_dts': True,
    'explicit_storage_module_resolved': True,
    'storage_translation_units': 7, 'storage_owner_translation_units': 1,
    'init_patch_applies_to_actual_r30_compiled_bsd_source': True,
    'pre_root_log_before_inittodr': True, 'inittodr_body_modified': False,
    'porting_record': record.strip(),
}
(HERE / 'static-verification.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
