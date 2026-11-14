#!/usr/bin/env python3
"""Compile pure r31 experiment/bridge models; not a kernel or hardware test."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--compiler', default='clang++')
    args = parser.parse_args()
    root, out = args.root.resolve(), args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    component = root / 'work/J716fStorageBringup-r31'
    target = root / 'work/UniXNU-j716f-host-storage-r30'
    platform = target / 'socs/sm8250/kits/Sm8250Platform'
    sheng = target / 'drivers/ShengUfs'
    inputs = {}
    for folder in (component, platform, sheng):
        for path in folder.rglob('*'):
            if path.is_file() and path.suffix in ('.cpp', '.h', '.py'):
                raw = path.read_bytes()
                inputs[path.relative_to(root).as_posix()] = dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())
    tests = {
        'StoragePower': [component / 'tests/StoragePower.cpp', component / 'J716fStoragePower.cpp'],
        'StorageBringup': [component / 'tests/StorageBringup.cpp', component / 'J716fStorageBringup.cpp',
                           platform / 'Sm8250UfsDiagnostics.cpp'],
    }
    flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-g', '-I', str(component), '-I', str(platform), '-I', str(sheng)]
    env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1:abort_on_error=1:detect_leaks=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    report = dict(passed=False, hardware_tested=False, device_access=False, qemu=False,
                  runtime_iokit_execution=False, external_abort_containment=False, inputs=inputs, steps=[])
    try:
        for name, sources in tests.items():
            binary = out / name
            for label, command in [('compile', [args.compiler, *flags, *map(str, sources), '-o', str(binary)]),
                                   ('run', [str(binary)])]:
                result = subprocess.run(command, cwd=root, env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, timeout=180)
                (out / f'{name}-{label}.log').write_text(result.stdout)
                report['steps'].append(dict(name=name, label=label, argv=command, exit_code=result.returncode))
                if result.returncode:
                    raise RuntimeError(f'{name} {label}: {result.stdout[-4000:]}')
        for relative, identity in inputs.items():
            if hashlib.sha256((root / relative).read_bytes()).hexdigest() != identity['sha256']:
                raise RuntimeError('Input changed: ' + relative)
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    (out / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key not in ('inputs', 'steps')}))
    return 0 if report['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
