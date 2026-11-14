"""Check r31 official configuration and layered source patches without compiling."""
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
DEVICE = ROOT / 'work/UniXNU-j716f-host-ufs-power-r31'
PARENT = ROOT / 'work/UniXNU-j716f-host-storage-r30'
BASE = ROOT / 'work/UniXNU-j716f-host-iokit-cpu0'
COMPONENT = ROOT / 'work/J716fStorageBringup-r31'
env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1')

def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def run(args, label, cwd=ROOT):
    result = subprocess.run([str(x) for x in args], cwd=cwd, env=env,
                            text=True, encoding='utf-8', capture_output=True)
    (HERE / (label + '.stdout.log')).write_text(result.stdout, encoding='utf-8')
    (HERE / (label + '.stderr.log')).write_text(result.stderr, encoding='utf-8')
    if result.returncode: raise RuntimeError(label + ': ' + result.stderr)
    return result.stdout

cli = [sys.executable, ROOT / 'UniXNU/unixnu']
common = ['--os', 'ipados-17.7.11-21H461', '--out', HERE / 'official-status-out']
local = ['--local-module', 'driver:J716fStorageBringup=' + str(COMPONENT)]
run([*cli, 'defconfig', 'j716f_host_ufs_power_r31_defconfig', '-D', DEVICE, *common, *local], 'defconfig')
status = json.loads(run([*cli, 'status', '--json', '-D', DEVICE, *common, *local], 'status'))
base = json.loads(run([*cli, 'status', '--json', '-D', BASE, *common], 'base-status'))
for name, value in (('status',status),('base-status',base)):
    (HERE / (name+'.json')).write_text(json.dumps(value, indent=2)+'\n', encoding='utf-8')
assert status['name'] == 'j716f-host-ufs-power-r31' and status['configured']
assert len(base['patch_order']) == 60 and len(status['patch_order']) == 63
assert [sha(Path(p)) for p in base['patch_order']] == [sha(Path(p)) for p in status['patch_order'][:60]]
assert status['board']['kernel']['archives'][:8] == base['board']['kernel']['archives']
assert status['board']['kernel']['archives'][8:] == ['build-j716f-ufs-power-r31.py']
assert status['board']['kernel']['prelink'] == base['board']['kernel']['prelink']
assert [c['id'] for c in status['components']] == [c['id'] for c in base['components']]
bridge = [m for m in status['modules'] if m['kind'] == 'driver' and m['name'] == 'J716fStorageBringup']
assert len(bridge) == 1 and bridge[0]['origin'] == 'local (--local-module)'
assert Path(bridge[0]['path']).resolve() == COMPONENT.resolve()
for rel in ('dts/j716f.dts','socs/sm8250/dts/sm8250.dtsi','patches/xnu/j716f-storage-init.patch',
            'patches/xnu/j716f-earlylog-console.patch','patches/xnu/j716f-verbose-text-console.patch'):
    assert (DEVICE / rel).read_bytes() == (PARENT / rel).read_bytes()
original = (BASE / 'configs/j716f_host_iokit_cpu0_defconfig').read_text()
extended = (DEVICE / 'configs/j716f_host_ufs_power_r31_defconfig').read_text()
assert extended.startswith(original)
assert extended[len(original):].strip() == (
    '# Five frozen SM8250 TUs, two retained bridge TUs and two Power TUs; one owner.\n'
    'CONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,{tree}/build/j716f-ufs-power-r31/libJ716fStorageBringup.a"')

scratch = HERE / 'patch-check'
source = ROOT / 'analysis/xnu-r30-pre-root-planning-20261114/sources/xnu/bsd/kern/bsd_init.c'
target = scratch / 'bsd/kern/bsd_init.c'
target.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(source, target)
for name in ('j716f-storage-init.patch','j716f-power-probe-call.patch'):
    patch = DEVICE / 'patches/xnu' / name
    run(['git','apply','--check',patch], name+'-check', cwd=scratch)
    run(['git','apply',patch], name+'-apply', cwd=scratch)
patched = target.read_text()
assert patched.count('j716f_storage_bringup_init();') == patched.count('j716f_storage_power_probe();') == 1
assert patched.count('inittodr(0);') == 1
assert patched.index('[J716F boot] BSD pre-root reached;') < patched.index('inittodr(0);')
gate = patched.index('if (result == KERN_SUCCESS)')
assert gate < patched.index('j716f_storage_bringup_init();',gate) < patched.index('j716f_storage_power_probe();',gate) < patched.index('} else {',gate)
run(['git','apply','--numstat',DEVICE/'patches/xnu/j716f-storage-dependency.patch'],'dependency-patch-format')
for script in (DEVICE/'scripts').glob('*.py'): ast.parse(script.read_text(), filename=str(script))
record = run([sys.executable,ROOT/'UniXNU/scripts/porting-record.py','check','--device',DEVICE],'porting-record')
creation = json.loads((HERE/'creation.json').read_text())
for rel, expected in creation['parent_files'].items(): assert sha(PARENT/rel) == expected, rel
report = {'status':'passed','host_compile_executed':False,'device_actions':False,'parent_unchanged':True,
          'retained_patches':60,'appended_patches':3,'retained_archive_recipes':8,'total_archive_recipes':9,
          'retained_component_ids':[c['id'] for c in status['components']],
          'retained_prelink_abi':True,'retained_dts':True,'explicit_r31_component_resolved':True,
          'storage_translation_units':9,'storage_owner_translation_units':1,
          'layered_patches_apply_to_actual_r30_base_bsd_source':True,
          'power_call_after_registration_inside_zero_success_gate':True,
          'inittodr_body_modified':False,'pre_root_log_before_inittodr':True,
          'porting_record':record.strip(),'power_hardware_success_inferred':False}
(HERE/'static-verification.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
print(json.dumps(report,indent=2))
