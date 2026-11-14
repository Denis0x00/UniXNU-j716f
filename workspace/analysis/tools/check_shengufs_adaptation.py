#!/usr/bin/env python3
"""Pinned native ASan/UBSan checks; no device, QEMU, kernel link or source edits.

Manifest schema (paths are relative to their named root, hashes are lowercase):
  {"schema_version": 1, "source_revision": "informational revision",
   "roots": {"source": {"files_sha256": {"NativeUfs.cpp": "<sha256>", ...}},
             "verified_root": {"files_sha256": {
               "include/verified_root.h": "<sha256>",
               "src/verified_root.c": "<sha256>"}}}}
The source file set must match exactly, excluding .git metadata only. Other
roots pin selected dependencies. build_shengufs_adaptation.py additionally
requires workspace/prepared roots in the same manifest. No Git executable or
repository metadata is used as evidence. A fresh output directory is required.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import re
import shutil
import subprocess
import sys


def sha(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def utc():
    return datetime.now(timezone.utc).isoformat()


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('Duplicate JSON key: ' + key)
        result[key] = value
    return result


def read_manifest(path):
    result = json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique_object)
    if result.get('schema_version') != 1 or not isinstance(result.get('roots'), dict):
        raise ValueError('Expected schema_version=1 and roots object')
    return result


def relative_file(root, name):
    if not isinstance(name, str) or not name or '\\' in name or ':' in name:
        raise ValueError('Invalid manifest path: ' + repr(name))
    relative = PurePosixPath(name)
    if relative.is_absolute() or any(p in ('.', '..', '.git') for p in name.split('/')):
        raise ValueError('Unsafe manifest path: ' + name)
    if relative.as_posix() != name:
        raise ValueError('Non-canonical manifest path: ' + name)
    path = root.joinpath(*relative.parts)
    if not path.resolve().is_relative_to(root.resolve()):
        raise ValueError('Manifest file escapes root: ' + str(path))
    if not path.is_file():
        raise ValueError('Manifest file missing: ' + str(path))
    return path


def source_files(root):
    return {p.relative_to(root).as_posix() for p in root.rglob('*')
            if p.is_file() and '.git' not in p.relative_to(root).parts}


def verify_manifest(manifest, roots, required):
    result = {}
    for label, root in roots.items():
        entry = manifest['roots'].get(label)
        if not isinstance(entry, dict) or not isinstance(entry.get('files_sha256'), dict):
            raise ValueError('Missing manifest root/files_sha256: ' + label)
        expected = entry['files_sha256']
        if not expected:
            raise ValueError('Empty manifest root: ' + label)
        absent = sorted(set(required.get(label, ())) - set(expected))
        if absent:
            raise ValueError('Required inputs are not pinned in ' + label + ': ' + repr(absent))
        if label == 'source':
            actual_names = source_files(root)
            if actual_names != set(expected):
                raise ValueError('Source file set differs: missing=' +
                                 repr(sorted(set(expected) - actual_names)) + ', extra=' +
                                 repr(sorted(actual_names - set(expected))))
        result[label] = {}
        for name, expected_sha in sorted(expected.items()):
            if not isinstance(expected_sha, str) or not re.fullmatch('[0-9a-f]{64}', expected_sha):
                raise ValueError('Invalid SHA-256 for ' + label + '/' + name)
            path = relative_file(root, name)
            actual_sha = sha(path)
            if actual_sha != expected_sha:
                raise ValueError('Input hash mismatch: ' + label + '/' + name)
            result[label][name] = {'sha256': actual_sha, 'size': path.stat().st_size}
    return result


def fresh_output(path, forbidden):
    # Check the lexical name as well as the resolved name, including dangling links.
    if path.exists() or path.is_symlink():
        raise ValueError('Refusing to overwrite output: ' + str(path))
    out = path.resolve()
    for root in forbidden:
        root = root.resolve()
        if out == root or out.is_relative_to(root) or root.is_relative_to(out):
            raise ValueError('Output overlaps an input tree: ' + str(root))
    out.mkdir(parents=True, exist_ok=False)
    return out


def run_step(report, out, name, command, env, cwd=None, timeout=120):
    command = [str(value) for value in command]
    log = out / (name + '.log')
    step = {'name': name, 'command': command, 'cwd': str(cwd or out),
            'timeout_seconds': timeout, 'log': str(log), 'started_utc': utc()}
    report['steps'].append(step)
    try:
        with log.open('w', encoding='utf-8') as stream:
            step['exit_code'] = subprocess.run(command, cwd=cwd or out, env=env,
                stdout=stream, stderr=subprocess.STDOUT, timeout=timeout).returncode
    except subprocess.TimeoutExpired:
        step['exit_code'] = 'timeout'
    except OSError as error:
        step['exit_code'] = 'launch-error'
        step['error'] = str(error)
    finally:
        step['finished_utc'] = utc()
        if log.is_file():
            step['log_sha256'] = sha(log)
    print(name + ': ' + str(step['exit_code']), flush=True)
    if step['exit_code'] != 0:
        raise RuntimeError(name + ' failed; see ' + str(log))
    return log.read_text(encoding='utf-8', errors='replace')


def finish(report, out):
    report['finished_utc'] = utc()
    report['files'] = {p.relative_to(out).as_posix():
                       {'size': p.stat().st_size, 'sha256': sha(p)}
                       for p in sorted(out.rglob('*')) if p.is_file() and not p.is_symlink()}
    with (out / 'result.json').open('x', encoding='utf-8') as stream:
        json.dump(report, stream, indent=2)
        stream.write('\n')
    return 0 if report['passed'] else 1


HOST_REQUIRED = (
    'NativeUfs.cpp', 'UfsPlatform.h', 'UfsObservation.h', 'SmmuGroupPlan.h',
    'tests/PlatformObservation.cpp', 'tests/SmmuGroupPlan.cpp',
    'tests/TransferPoll.cpp', 'tests/CompletionWait.cpp', 'tests/SmmuTakeover.cpp',
    'tests/RootCommands.cpp', 'tests/check_legacy_descriptors.py',
    'tests/check_root_path_host.py', 'tests/RootLayout.cpp',
    'tests/RootWindowRead.cpp', 'tests/RootDiagnostics.cpp',
    'tests/RootPublication.cpp', 'tests/VerifiedTableCache.cpp',
)
VERIFIED_REQUIRED = ('include/verified_root.h', 'src/verified_root.c')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--verified-root', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    roots = {'source': args.source.resolve(), 'verified_root': args.verified_root.resolve()}
    out = fresh_output(args.output, list(roots.values()))
    report = {'schema_version': 1, 'started_utc': utc(), 'passed': False, 'steps': [],
              'scope': 'Native ASan/UBSan verification of the pure observation/group planner '
                       'plus existing production fixtures; not MMIO, DMA, XNU runtime or device qualification.',
              'roots': {k: str(v) for k, v in roots.items()}, 'qemu_run': False,
              'device_access': False, 'kernel_build': False, 'source_modified_by_runner': False,
              'host': platform.platform(), 'runner_sha256': sha(Path(__file__).resolve()),
              'source_manifest': str(args.manifest.resolve())}
    required = {'source': HOST_REQUIRED, 'verified_root': VERIFIED_REQUIRED}
    before = None
    try:
        report['source_manifest_sha256'] = sha(args.manifest)
        manifest = read_manifest(args.manifest)
        report['source_revision'] = manifest.get('source_revision')
        report['source_changes'] = manifest.get('source_changes', [])
        before = verify_manifest(manifest, roots, required)
        report['inputs_before'] = before
        report['manifest_roots_not_used'] = sorted(set(manifest['roots']) - set(roots))
        env = {'PATH': '/usr/local/bin:/usr/bin:/bin', 'HOME': os.environ.get('HOME', ''),
               'LANG': 'C.UTF-8', 'LC_ALL': 'C', 'PYTHONDONTWRITEBYTECODE': '1',
               'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1:detect_leaks=1',
               'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        report['environment'] = env
        for compiler in ('clang++', 'clang'):
            executable = shutil.which(compiler, path=env['PATH'])
            if not executable:
                raise RuntimeError('Missing host compiler: ' + compiler)
            run_step(report, out, compiler.replace('+', 'p') + '-version',
                     [executable, '--version'], env)
        tests = ('PlatformObservation', 'SmmuGroupPlan', 'TransferPoll',
                 'CompletionWait', 'SmmuTakeover', 'RootCommands')
        report['native_test_names'] = list(tests)
        for name in tests:
            binary = out / name
            run_step(report, out, name + '-compile', ['clang++', '-std=c++17',
                '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                '-fno-omit-frame-pointer', '-g', str(roots['source'] / 'tests' / (name + '.cpp')),
                '-o', str(binary)], env)
            run_step(report, out, name, [binary], env, timeout=60)
        run_step(report, out, 'legacy-descriptors', [sys.executable,
            roots['source'] / 'tests/check_legacy_descriptors.py',
            '--output', out / 'legacy-descriptors'], env)
        descriptor_report = json.loads((out / 'legacy-descriptors/report.json').read_text())
        if descriptor_report.get('passed') is not True or descriptor_report.get('cases') != 14:
            raise RuntimeError('Legacy descriptor report does not confirm 14 passing comparisons')
        report['legacy_descriptor_cases'] = descriptor_report['cases']
        run_step(report, out, 'root-path-host', [sys.executable,
            roots['source'] / 'tests/check_root_path_host.py', '--output', out / 'root-path',
            '--openssl-prefix', '/usr', '--verified-root', roots['verified_root']], env, timeout=240)
        report['root_test_names'] = ['RootLayout', 'RootWindowRead', 'RootDiagnostics',
                                     'RootPublication', 'VerifiedTableCache']
        for name in report['root_test_names']:
            if not (out / 'root-path' / name).is_file() or not (out / 'root-path' / (name + '.log')).is_file():
                raise RuntimeError('Missing completed root fixture evidence: ' + name)
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        if before is not None:
            try:
                report['inputs_after'] = verify_manifest(manifest, roots, required)
                report['inputs_unchanged'] = (before == report['inputs_after'] and
                    sha(args.manifest) == report['source_manifest_sha256'] and
                    sha(Path(__file__).resolve()) == report['runner_sha256'])
            except Exception as error:
                report['inputs_unchanged'] = False
                report['input_recheck_error'] = str(error)
            if not report['inputs_unchanged']:
                report['passed'] = False
    return finish(report, out)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
