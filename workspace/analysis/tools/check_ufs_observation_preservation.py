"""Rehash frozen predecessor targets and deployed r6 inputs after host work."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
report = root / 'analysis/ufs-observation-20261113'


def identity(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return {'bytes': path.stat().st_size, 'sha256': digest.hexdigest()}


protected = json.loads((report / 'retained-inputs.json').read_text())
for name, spec in protected.items():
    assert identity(root / name.replace('\\', '/')) == spec, name
prior = json.loads((root / 'analysis/ufs-adapter-20261113/retained-inputs-check.json').read_text())
for spec in prior['pinned_r6_inputs']:
    assert identity(root / spec['path']) == {k: spec[k] for k in ('bytes', 'sha256')}, spec['path']
r6_path = root / 'artifacts/j716f-xnu-dt-panic-probe-20261113-r6/boot.img'
r6 = identity(r6_path)
assert r6 == prior['r6_boot']
legacy = identity(root / 'work/ShengUfs-sm8250/NativeUfs.cpp')
assert legacy == prior['legacy_native_ufs']
base = root / 'work/UniXNU-j716f-host-ufs-adapter/socs/sm8250/kits/Sm8250Platform/Sm8250UfsRuntime.cpp'
current = root / 'work/UniXNU-j716f/socs/sm8250/kits/Sm8250Platform/Sm8250UfsRuntime.cpp'
assert current.read_text().startswith(base.read_text().rstrip())
result = {'passed': True, 'predecessor_inputs_checked': len(protected),
          'r6_pinned_inputs_checked': len(prior['pinned_r6_inputs']),
          'r6_boot': r6, 'legacy_native_ufs': legacy, 'old_runtime_bodies_unchanged': True,
          'device_access': False, 'qemu': False}
(report / 'retained-inputs-check.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result))
