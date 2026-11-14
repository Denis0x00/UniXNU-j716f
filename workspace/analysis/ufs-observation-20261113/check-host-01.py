#!/usr/bin/env python3
"""Execute the frozen read-only diagnostic core against an instrumented host model."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time


def identity(path):
    return {'bytes': path.stat().st_size, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--root', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    a = p.parse_args()
    if sys.platform != 'linux' or not __debug__:
        p.error('Requires Linux with assertions enabled')
    root = a.root.resolve()
    target = root / 'work/UniXNU-j716f-host-ufs-observation'
    out = a.output.resolve()
    if out.exists():
        p.error('Refusing to overwrite previous test evidence')
    out.mkdir(parents=True)
    manifest_path = target / 'docs/ufs-observation-inputs.json'
    manifest = json.loads(manifest_path.read_text())
    record = {'schema_version': 1, 'passed': False, 'started': time.time(), 'steps': [],
              'scope': 'Read-only diagnostic core with instrumented mappings and register callbacks',
              'device_access': False, 'qemu': False, 'runtime_iokit_host_execution': False,
              'source_manifest_sha256': identity(manifest_path)['sha256'],
              'runner': identity(Path(__file__)), 'inputs_unchanged': False}

    def validate():
        result = {}
        for name, spec in manifest['files'].items():
            source = (target / name).resolve(strict=True)
            assert source.is_relative_to(target) and not (target / name).is_symlink(), name
            result[name] = identity(source)
            assert result[name] == spec, name
        assert identity(manifest_path)['sha256'] == record['source_manifest_sha256']
        return result

    try:
        before = validate()
        platform = target / 'socs/sm8250/kits/Sm8250Platform'
        flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-g', '-I', str(platform), '-I', str(target / 'drivers/ShengUfs')]
        binary = out / 'Sm8250UfsDiagnostics'
        env = dict(os.environ, ASAN_OPTIONS='halt_on_error=1:abort_on_error=1:detect_leaks=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        commands = [
            ('compile', ['clang++', *flags, str(platform / 'tests/Sm8250UfsDiagnostics.cpp'),
                         str(platform / 'Sm8250UfsDiagnostics.cpp'), '-o', str(binary)]),
            ('run', [str(binary)]),
        ]
        for label, command in commands:
            result = subprocess.run(command, cwd=root, env=env, text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
            (out / (label + '.log')).write_text(result.stdout)
            record['steps'].append({'label': label, 'argv': command, 'exit_code': result.returncode})
            if result.returncode:
                raise RuntimeError(label + ' failed: ' + result.stdout[-4000:])
        record['test_output'] = (out / 'run.log').read_text().strip()
        record['inputs_unchanged'] = before == validate()
        assert record['inputs_unchanged']
        record['passed'] = True
    except Exception as exc:
        record['error'] = str(exc)
    finally:
        record['finished'] = time.time()
        (out / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({k: record[k] for k in ('passed', 'inputs_unchanged', 'runtime_iokit_host_execution')}))
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
