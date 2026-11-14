#!/usr/bin/env python3
"""Fresh, manifest-pinned restricted-UFS/owner host checks plus prior suites.

Reuse the preceding runner without modifying it or its historical outputs.
Manifest schema: schema_version=1, roots.<name>.files_sha256. source is an
exact file set; owner is ONLY the explicitly listed new files. The shared
workspace root pins all four runner scripts. No Git or device access is used.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import sys

sys.dont_write_bytecode = True
from check_shengufs_adaptation import (
    HOST_REQUIRED, VERIFIED_REQUIRED, finish, fresh_output, read_manifest,
    run_step, sha, utc, verify_manifest,
)


OWNER_REL = 'work/UniXNU-j716f/socs/sm8250/kits/Sm8250Platform'
RUNNERS = tuple('analysis/tools/' + name for name in (
    'check_shengufs_adaptation.py', 'build_shengufs_adaptation.py',
    'check_shengufs_restricted.py', 'build_shengufs_restricted.py'))
NEW_SOURCE = ('ReadCommandPolicy.h', 'RestrictedUfs.h',
              'tests/ReadCommandPolicy.cpp', 'tests/RestrictedUfs.cpp')


def owner_files(manifest):
    """Only new owner files explicitly listed by the caller, never a tree scan."""
    entry = manifest.get('roots', {}).get('owner', {}).get('files_sha256', {})
    mandatory = {'AppsSmmuOwner.h', 'AppsSmmuOwner.cpp'}
    tests = ['tests/AppsSmmuOwner.cpp', 'tests/AppsSmmuOwnerPeer.cpp']
    if not mandatory.issubset(entry) or not set(tests).issubset(entry):
        raise ValueError('Owner manifest needs header, unique implementation and both named test TUs')
    if set(entry) != mandatory | set(tests):
        raise ValueError('Owner root must contain only new AppsSmmuOwner sources/tests')
    return tuple(sorted(entry)), tests


def runner_hashes(workspace):
    result = {}
    for name in RUNNERS:
        path = workspace / name
        # Prevent a caller from pinning one copy while executing another copy.
        current = Path(__file__).resolve().parent / Path(name).name
        if not path.is_file() or not current.is_file() or sha(path) != sha(current):
            raise ValueError('Runner differs from pinned workspace location: ' + name)
        result[name] = sha(path)
    return result


def ensure_baseline(path):
    report = json.loads(path.read_text(encoding='utf-8'))
    if report.get('passed') is not True or report.get('inputs_unchanged') is not True:
        raise RuntimeError('Delegated baseline did not pass with unchanged inputs')
    return {'path': str(path), 'sha256': sha(path), 'passed': True,
            'inputs_unchanged': True, 'steps': len(report.get('steps', []))}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--source', type=Path)
    parser.add_argument('--verified-root', type=Path)
    parser.add_argument('--owner', type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    workspace = args.workspace.resolve()
    roots = {'workspace': workspace,
             'source': (args.source or workspace / 'work/ShengUfs-sm8250').resolve(),
             'verified_root': (args.verified_root or workspace / 'UniXNU/kits/VerifiedRoot').resolve(),
             'owner': (args.owner or workspace / OWNER_REL).resolve()}
    out = fresh_output(args.output, [root for name, root in roots.items() if name != 'workspace'] +
                       [workspace / 'analysis/tools'])
    report = {'schema_version': 1, 'started_utc': utc(), 'passed': False, 'steps': [],
              'scope': 'ASan/UBSan host-only command policy, single-slot state machine, '
                       'shared owner multi-TU/thread checks and prior regression suites.',
              'roots': {k: str(v) for k, v in roots.items()},
              'host': platform.platform(), 'qemu_run': False, 'device_access': False,
              'kernel_link': False, 'hardware_qualified': False,
              'source_manifest': str(args.manifest.resolve())}
    before = None
    try:
        manifest = read_manifest(args.manifest)
        report['source_manifest_sha256'] = sha(args.manifest)
        owners, owner_tests = owner_files(manifest)
        required = {'source': HOST_REQUIRED + NEW_SOURCE, 'verified_root': VERIFIED_REQUIRED,
                    'workspace': RUNNERS, 'owner': owners}
        report['runner_hashes_before'] = runner_hashes(workspace)
        before = verify_manifest(manifest, roots, required)
        report['inputs_before'] = before
        report['source_revision'] = manifest.get('source_revision')
        report['source_changes'] = manifest.get('source_changes', [])
        report['manifest_roots_not_used'] = sorted(set(manifest['roots']) - set(roots))
        env = {'PATH': '/usr/local/bin:/usr/bin:/bin', 'HOME': os.environ.get('HOME', ''),
               'LANG': 'C.UTF-8', 'LC_ALL': 'C', 'PYTHONDONTWRITEBYTECODE': '1',
               'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1:detect_leaks=1',
               'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        report['environment'] = env
        run_step(report, out, 'prior-host-suites', [sys.executable, '-B',
            workspace / 'analysis/tools/check_shengufs_adaptation.py',
            '--source', roots['source'], '--manifest', args.manifest.resolve(),
            '--verified-root', roots['verified_root'], '--output', out / 'baseline'], env, timeout=600)
        report['baseline_result'] = ensure_baseline(out / 'baseline/result.json')
        flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-g', '-pthread', '-I', str(roots['owner'])]
        report['new_host_compile_flags'] = flags
        for name in ('ReadCommandPolicy', 'RestrictedUfs'):
            binary = out / name
            sources = [roots['source'] / 'tests' / (name + '.cpp')]
            if name == 'RestrictedUfs':
                sources.append(roots['owner'] / 'AppsSmmuOwner.cpp')
            run_step(report, out, name + '-compile', ['clang++', *flags, *sources,
                     '-o', binary], env)
            run_step(report, out, name, [binary], env, timeout=60)
        owner_sources = [roots['owner'] / 'AppsSmmuOwner.cpp'] + [roots['owner'] / name for name in owner_tests]
        report['owner_test_translation_units'] = [str(path) for path in owner_sources]
        owner_binary = out / 'AppsSmmuOwner'
        run_step(report, out, 'AppsSmmuOwner-compile', ['clang++', *flags,
                 *owner_sources, '-o', owner_binary], env)
        run_step(report, out, 'AppsSmmuOwner', [owner_binary], env, timeout=60)
        report['new_test_names'] = ['ReadCommandPolicy', 'RestrictedUfs', 'AppsSmmuOwner']
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        if before is not None:
            try:
                report['inputs_after'] = verify_manifest(manifest, roots, required)
                report['runner_hashes_after'] = runner_hashes(workspace)
                report['inputs_unchanged'] = (before == report['inputs_after'] and
                    report['runner_hashes_before'] == report['runner_hashes_after'] and
                    sha(args.manifest) == report['source_manifest_sha256'])
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
