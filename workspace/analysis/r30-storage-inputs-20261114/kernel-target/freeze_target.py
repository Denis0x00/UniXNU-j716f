"""Pin the new host target and explicit storage source closure."""
from pathlib import Path
import ast
import hashlib
import json
import subprocess

ROOT = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
DEVICE = ROOT / 'work/UniXNU-j716f-host-storage-r30'
COMPONENT = ROOT / 'work/J716fStorageBringup-r30'

def identity(path):
    data = path.read_bytes()
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}

def files(folder):
    return [p for p in folder.rglob('*') if p.is_file()
            and not any(part in ('.git', '__pycache__', '.cache') for part in p.relative_to(folder).parts)
            and p.suffix != '.pyc']

parent = json.loads((HERE / 'creation.json').read_text())
for relative, expected in parent['parent_files'].items():
    assert identity(ROOT / parent['source_target'] / relative)['sha256'] == expected
component_manifest = json.loads((COMPONENT / 'source-manifest.json').read_text())
for relative, expected in component_manifest['files'].items():
    assert identity(ROOT / relative) == expected

platform = 'socs/sm8250/kits/Sm8250Platform'
sources = [platform + '/' + name + '.cpp' for name in (
    'Sm8250UfsRuntime', 'Sm8250UfsHci', 'Sm8250UfsHandoff', 'Sm8250UfsDiagnostics', 'AppsSmmuOwner')]
sources += ['component:J716fStorageBringup/J716fStorageBringup.cpp',
            'component:J716fStorageBringup/J716fStorageKernel.cpp']
device_inputs = [p for p in files(DEVICE) if p.name not in ('storage-inputs.json', 'host-source-manifest.json')]
component_inputs = files(COMPONENT)
storage = {
    'schema_version': 1, 'scope': 'new host target only; runtime storage authorization is absent',
    'translation_units': sources, 'native_ufs_cpp_compiled': False,
    'files': {p.relative_to(DEVICE).as_posix(): identity(p) for p in sorted(device_inputs)},
    'component_files': {p.relative_to(COMPONENT).as_posix(): identity(p) for p in sorted(component_inputs)},
    'component_source_manifest': identity(COMPONENT / 'source-manifest.json'),
    'old_adapter': 'work/UniXNU-j716f-host-ufs-observation',
    'parent_target_unchanged': True,
}
for source in sources[:5]:
    assert (DEVICE / source).read_bytes() == (ROOT / storage['old_adapter'] / source).read_bytes()
(DEVICE / 'docs/storage-inputs.json').write_text(json.dumps(storage, indent=2) + '\n')

status = json.loads((HERE / 'status.json').read_text())
inputs = set(device_inputs + component_inputs + files(ROOT / 'work/ArmCpu-j716f-iokit-cpu0-20261114'))
inputs.add(DEVICE / 'docs/storage-inputs.json')
inputs.update(Path(p) for p in status['patch_order'] + status['configuration_sources'])
inputs.update(files(ROOT / 'UniXNU/scripts'))
inputs.update(files(ROOT / 'UniXNU/profiles/arch/arm64'))
inputs.update(files(ROOT / 'UniXNU/profiles/os/ipados-17.7.11-21H461'))
for folder in ('drivers/ArmDTPlatform', 'drivers/SoftwareAES', 'kits/XnuPrelink', 'drivers/DarwinBoot'):
    inputs.update(files(ROOT / 'UniXNU' / folder))
for name in ('unixnu', 'components.toml', 'feeds.toml'):
    inputs.add(ROOT / 'UniXNU' / name)
revision = subprocess.run(['git', '-C', ROOT / 'UniXNU', 'rev-parse', 'HEAD'], check=True, text=True, capture_output=True).stdout.strip()
host = {
    'origin': 'Independent r30 storage bridge over accepted host-iokit-cpu0 target',
    'upstream_revision': revision, 'source_delta': 'Seven-TU archive, two startup/dependency patches and its explicit linker setting',
    'retained_patch_count': 60, 'new_patch_count': 2, 'archive_recipe_count': 9,
    'fresh_compile_records_required': True,
    'effective_config_delta_from_iokit_cpu0': {'XNU_EXTRA_LD_ARCHIVES': '-Wl,-force_load,{tree}/build/j716f-storage-r30/libJ716fStorageBringup.a'},
    'logging_patch_sha256': identity(DEVICE / 'patches/xnu/j716f-earlylog-console.patch')['sha256'],
    'inputs': {p.relative_to(ROOT).as_posix(): identity(p) for p in sorted(inputs)},
}
(DEVICE / 'docs/host-source-manifest.json').write_text(json.dumps(host, indent=2) + '\n')
sync = dict(host['inputs'])
sync[(DEVICE / 'docs/host-source-manifest.json').relative_to(ROOT).as_posix()] = identity(DEVICE / 'docs/host-source-manifest.json')
report = {'status': 'frozen', 'target': DEVICE.relative_to(ROOT).as_posix(),
          'storage_manifest': identity(DEVICE / 'docs/storage-inputs.json'),
          'host_manifest': identity(DEVICE / 'docs/host-source-manifest.json'),
          'upstream_revision': revision, 'input_count': len(host['inputs']),
          'device_file_count': len(storage['files']), 'component_file_count': len(storage['component_files']),
          'files': sync, 'host_compile_completed': False, 'device_actions': False}
(HERE / 'target-sync-manifest.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({key: value for key, value in report.items() if key != 'files'}, indent=2))
