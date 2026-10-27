#!/usr/bin/env python3
"""Prelink an authenticated native receipt with the pinned upstream recipe.

Writes a NEW private directory. Never modifies the received source kernel,
historical P22, POST policy or any device. No binary entry/reset patching.
"""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tomllib

from native_candidate import HERE, KERNEL, MachO, check_kernel, check_report, check_selected_logger, save_new, sha

PINS = {
    'kits/XnuPrelink/emit_prelink.py': 'fdbfecbb1dbd25d2cc9c693111e07b6081f17bd3a6b0a0934d4191bc08162311',
    'kits/XnuPrelink/native_prelink.py': 'a2da7ea8a4431f6c26418ecab9be718c0fbd2f0aa2334ba8d32c2270d2ac81dc',
    'profiles/os/ipados-17.7.11-21H461/profile.toml': '93661d78eda49c4076f1ec2ced391e6cc418ff72f30dcf17f5f30d92b6141ac1',
}


def audit_native_preserved(source, candidate):
    before, after = MachO(source), MachO(candidate)
    checked = []
    for name, va, _vmsize, off, size, prot in before.segments:
        if size and prot & 4:
            at = after.offset(va, size)
            if candidate[at:at+size] != source[off:off+size]:
                raise ValueError('Native executable segment changed during prelink')
            checked.append({'segment': name.decode(), 'bytes': size, 'sha256': sha(source[off:off+size])})
    if not checked:
        raise ValueError('No native executable segment audited')
    for name in before.symbols:
        if name.startswith(('_armcpu_', '_armdt_', '_j716f_')) and after.symbols.get(name) != before.symbols[name]:
            raise ValueError('Native provider symbol moved/disappeared')
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--received', type=Path, required=True)
    parser.add_argument('--unixnu', type=Path, required=True)
    parser.add_argument('--kernelcache', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--selection', type=Path, default=HERE.parent/'configs/native-log-candidate-40458123294.json')
    args = parser.parse_args()
    lock = json.loads(args.selection.read_text(encoding='utf-8'))
    receipt = json.loads((args.received/'receipt.json').read_text(encoding='utf-8'))
    if receipt.get('stage') != 'native-artifact-received' or receipt.get('selection_sha256') != sha(args.selection.read_bytes()) or receipt.get('selection') != lock:
        raise ValueError('Receipt not bound to selected CI artifact')
    source_report = json.loads((args.received/'source-build-report.json').read_text(encoding='utf-8'))
    names, entries = check_report(source_report, lock)
    for name in names:
        content = (args.received/'tree'/name).read_bytes()
        if sha(content) != entries[name]['sha256'] or len(content) != entries[name]['bytes']:
            raise ValueError('Received artifact changed: '+name)
    source = (args.received/'tree'/KERNEL).read_bytes()
    source_checks = check_kernel(source)
    if receipt.get('artifacts') != source_report['artifacts'] or receipt.get('kernel') != source_checks:
        raise ValueError('Receipt/content/report disagreement')
    source_logger = check_selected_logger(args.received/'tree'/KERNEL, source_report, lock)
    if (receipt.get('logger_checks') != source_logger or
            receipt.get('persistent_early_logger_present') != (source_logger is not None)):
        raise ValueError('Receipt logger/content disagreement')
    for path, digest in PINS.items():
        if sha((args.unixnu/path).read_bytes()) != digest:
            raise ValueError('Prelink tool/profile bytes differ from pin: '+path)
    for path, commit in [(args.unixnu, lock['unixnu_commit']),
                         (args.unixnu/'kits/XnuPrelink', lock['components']['XnuPrelink'])]:
        actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
        if actual != commit:
            raise ValueError('Wrong upstream/component checkout')
    if sha(args.kernelcache.read_bytes()) != lock['kernelcache_sha256']:
        raise ValueError('Wrong Apple kernelcache/OS ABI')
    profile = tomllib.loads((args.unixnu/'profiles/os'/lock['os_profile']/'profile.toml').read_text())
    spec = profile['kernel']['prelink']
    if spec['kernelcache_sha256'] != lock['kernelcache_sha256'] or spec['kernel'] != KERNEL:
        raise ValueError('Selected OS prelink recipe mismatch')
    if args.output.exists():
        raise ValueError('Refuse to overwrite a prelink attempt')
    args.output.mkdir(parents=True)
    target = args.output/'kernel.macho'
    command = [sys.executable, str((args.unixnu/spec['tool']).resolve()),
               '--kernel', str((args.received/'tree'/KERNEL).resolve()),
               '--prelinked', str(args.kernelcache.resolve()), '--slide='+spec['slide'],
               '--output', str(target.resolve())]
    for root in spec['roots']:
        command += ['--root', root]
    command += ['--early-security-init', spec['early_security_init']]
    with (args.output/'prelink.log').open('x', encoding='utf-8') as stream:
        completed = subprocess.run(command, stdout=stream, stderr=subprocess.STDOUT, text=True)
    if completed.returncode:
        raise ValueError('XnuPrelink failed; preserved prelink.log for diagnosis')
    output = target.read_bytes()
    text = audit_native_preserved(source, output)
    checks = check_kernel(output)
    logger = check_selected_logger(target, source_report, lock)
    report = json.loads(target.with_suffix('.json').read_text())
    if report['candidate_sha256'] != sha(output) or report['candidate_bytes'] != len(output) or report['source_inputs_modified']:
        raise ValueError('XnuPrelink output/report disagreement')
    if sha((args.received/'tree'/KERNEL).read_bytes()) != source_checks['kernel_sha256']:
        raise ValueError('Source input was modified')
    manifest = {'schema': 1, 'stage': 'native-prelinked-host-only', 'selection': lock,
                'receipt_sha256': sha((args.received/'receipt.json').read_bytes()),
                'tool_and_profile_sha256': PINS, 'recipe': spec,
                'source_kernel_sha256': sha(source), 'kernel_sha256': sha(output),
                'prelink_report_sha256': sha(target.with_suffix('.json').read_bytes()),
                'native_executable_segments_unchanged': text, 'checks': checks,
                'logger_checks': logger,
                'persistent_early_logger_present': logger is not None, 'hardware_launch_admitted': False,
                'device_writes_performed': False, 'runtime_qualified': False}
    save_new(args.output/'native-prelink.json', manifest)
    print(json.dumps({'stage': manifest['stage'], 'kernel_sha256': sha(output),
                      'native_text_preserved': True, 'hardware_launch_admitted': False}, indent=2))


if __name__ == '__main__':
    main()
