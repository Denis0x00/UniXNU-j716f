#!/usr/bin/env python3
"""Manifest-pinned Linux adapter tests, with the complete prior host regressions."""
import argparse
import json
import os
from pathlib import Path
import sys

sys.dont_write_bytecode = True
from check_shengufs_adaptation import finish, fresh_output, read_manifest, run_step, sha, utc, verify_manifest
from check_shengufs_restricted import OWNER_REL, RUNNERS, ensure_baseline

ADAPTER_FILES = tuple(name + suffix for name in ('Sm8250UfsHci', 'Sm8250UfsHandoff', 'Sm8250UfsRuntime')
                      for suffix in ('.h', '.cpp')) + ('Sm8250UfsIo.h',) + tuple(
                      'tests/' + name + '.cpp' for name in ('Sm8250UfsHci', 'Sm8250UfsHandoff', 'Sm8250UfsPipeline'))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    root = args.workspace.resolve()
    roots = {'workspace': root, 'source': root / 'work/ShengUfs-sm8250',
             'verified_root': root / 'UniXNU/kits/VerifiedRoot',
             'owner': root / OWNER_REL, 'adapter': root / OWNER_REL}
    out = fresh_output(args.output, [roots['source'], roots['owner'], root / 'analysis/tools'])
    manifest = read_manifest(args.manifest)
    report = {'schema_version': 1, 'started_utc': utc(), 'passed': False, 'steps': [],
              'scope': 'Actual HCI/handoff/core/owner source with modeled MMIO and noncoherent DMA, plus prior regressions',
              'device_access': False, 'qemu': False, 'runtime_iokit_host_execution': False,
              'source_manifest': str(args.manifest.resolve()), 'source_manifest_sha256': sha(args.manifest)}
    required = {'adapter': ADAPTER_FILES, 'workspace': RUNNERS + ('analysis/tools/check_sm8250_ufs_adapter.py',)}
    before = None
    try:
        if sha(Path(__file__)) != manifest['roots']['workspace']['files_sha256']['analysis/tools/check_sm8250_ufs_adapter.py']:
            raise ValueError('Executing runner does not match the manifest')
        before = verify_manifest(manifest, roots, required)
        report['inputs_before'] = before
        env = {'PATH': '/usr/local/bin:/usr/bin:/bin', 'HOME': os.environ.get('HOME', ''),
               'LANG': 'C.UTF-8', 'LC_ALL': 'C', 'PYTHONDONTWRITEBYTECODE': '1',
               'ASAN_OPTIONS': 'halt_on_error=1:abort_on_error=1:detect_leaks=1',
               'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'}
        run_step(report, out, 'prior-restricted-host', [sys.executable, '-B',
                 root / 'analysis/tools/check_shengufs_restricted.py', '--workspace', root,
                 '--manifest', args.manifest.resolve(), '--output', out / 'baseline'], env, timeout=600)
        report['baseline_result'] = ensure_baseline(out / 'baseline/result.json')
        flags = ['-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                 '-fno-omit-frame-pointer', '-g', '-pthread', '-I', str(roots['adapter']), '-I', str(roots['source'])]
        report['compile_flags'] = flags
        for name in ('Sm8250UfsHci', 'Sm8250UfsHandoff', 'Sm8250UfsPipeline'):
            sources = [roots['adapter'] / 'tests' / (name + '.cpp'), roots['owner'] / 'AppsSmmuOwner.cpp']
            for implementation in (('Sm8250UfsHci', 'Sm8250UfsHandoff') if name.endswith('Pipeline') else (name,)):
                sources.append(roots['adapter'] / (implementation + '.cpp'))
            binary = out / name
            run_step(report, out, name + '-compile', ['clang++', *flags, *sources, '-o', binary], env)
            run_step(report, out, name, [binary], env, timeout=90)
        report['passed'] = True
    except Exception as error:
        report['error'] = str(error)
    finally:
        if before is not None:
            try:
                report['inputs_after'] = verify_manifest(manifest, roots, required)
                report['inputs_unchanged'] = before == report['inputs_after'] and sha(args.manifest) == report['source_manifest_sha256']
            except Exception as error:
                report['inputs_unchanged'] = False
                report['recheck_error'] = str(error)
            if not report['inputs_unchanged']:
                report['passed'] = False
    return finish(report, out)


if __name__ == '__main__':
    sys.exit(main())
