#!/usr/bin/env python3
"""Compile frozen resource provider plus current-boot handoff through XNU's recipe."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree', type=Path, required=True)
    p.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[1])
    p.add_argument('--provider', type=Path)
    a = p.parse_args()
    tree, source = a.tree.resolve(), a.source.resolve()
    provider = (a.provider or source.parent/'J716fGopAdapter-20261113').resolve()
    contract = json.loads((source/'package/source-contract.json').read_text())
    for name, expected in contract['resolved_source_sha256'].items():
        actual = tree/'.cache/xnu-work'/name
        if not actual.is_file() or sha(actual) != expected:
            raise ValueError('Unqualified resolved XNU source: '+name)
    # Frozen provider builder copies and compiles the exact accepted source.
    subprocess.run([sys.executable, str(provider/'scripts/build.py'), '--tree', str(tree),
                    '--source', str(provider)], check=True)
    out = tree/'build/native-legacy-display'
    archive, report_path = out/'libNativeLegacyDisplay.late.a', out/'build-report.json'
    old = json.loads(report_path.read_text())
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    attempt = out/'handoff-attempts'/stamp
    inputs = attempt/'source'; inputs.mkdir(parents=True)
    # An interrupted or failed handoff must not advertise a provider-only build
    # as the successful artifact required by this target.
    shutil.copyfile(archive, attempt/'provider-only.a')
    shutil.copyfile(report_path, attempt/'provider-only-report.json')
    archive.unlink(); report_path.unlink()
    original = {}
    hashes = dict(old['source_sha256'])
    for name in ['package/J716fDisplayHandoff.cpp','package/J716fDisplayHandoffCore.h',
                 'package/source-contract.json','scripts/build.py',
                 'patches/j716f-platform-display-handoff.patch']:
        src = source/name; dst = inputs/name; dst.parent.mkdir(parents=True, exist_ok=True)
        original[name] = sha(src); shutil.copyfile(src,dst)
        if sha(dst) != original[name]: raise ValueError('Input changed during snapshot: '+name)
        hashes[str(dst.relative_to(tree))] = sha(dst)
    # Include precisely the frozen video-validation header, without copying or
    # redefining its arithmetic in this package.
    header = inputs/'package/J716fDisplayCore.h'
    shutil.copyfile(provider/'package/J716fDisplayCore.h', header)
    hashes[str(header.relative_to(tree))] = sha(header)
    spec = importlib.util.spec_from_file_location('frozen_provider_build', provider/'scripts/build.py')
    helper = importlib.util.module_from_spec(spec); spec.loader.exec_module(helper)
    recipe = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    obj = attempt/'J716fDisplayHandoff.o'
    command = helper.compile_command(record, inputs/'package/J716fDisplayHandoff.cpp',
                                     obj, inputs/'package', tree)
    (attempt/'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
    result = subprocess.run(command,cwd=record['directory'],text=True,
                            stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
    (attempt/'compile.log').write_text(result.stdout)
    if result.returncode: raise ValueError('Handoff compile failed:\n'+result.stdout[-6000:])
    helper.ENTRY = {'_j716f_display_handoff_platform_ready',
                    '_j716f_display_handoff_scanout_available'}
    audit = helper.audit_object(obj)
    combined = attempt/'libNativeLegacyDisplay.late.a'
    shutil.copyfile(attempt/'provider-only.a', combined)
    subprocess.run([old['archive_tool'],'rcs',str(combined),str(obj)],check=True)
    old.update({'integration_level':'resource-provider-with-current-allocator-exclusion',
                'platform_reservation_caller_present':True,
                'archive_sha256':sha(combined),'source_sha256':hashes,
                'handoff_original_package_sha256':original,
                'handoff_object_sha256':sha(obj),'handoff_object_audit':audit,
                'handoff_attempt':str(attempt.relative_to(tree)),
                'resolved_xnu_source_contract':contract,
                'reservation_authority':'current immutable XNU high-span bounds plus page-by-page pmap checks',
                'limitation':'Conditional metadata reservation only; no scanout, cache alias or display-service grant.'})
    shutil.copyfile(combined,archive)
    report_path.write_text(json.dumps(old,indent=2)+'\n')
    print(json.dumps({'archive':str(archive),'sha256':sha(archive),
                      'integration_level':old['integration_level']}))


if __name__ == '__main__':
    try: main()
    except (ValueError,OSError,subprocess.CalledProcessError) as e:
        raise SystemExit(str(e))
