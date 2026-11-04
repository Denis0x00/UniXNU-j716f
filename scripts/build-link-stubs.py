#!/usr/bin/env python3
import sys
from pathlib import Path
import subprocess

ROOT = Path('.').resolve()
def find_src(name):
    for p in [ROOT / f'kits/j716f/{name}', ROOT / f'package/{name}', Path(f'/home/test/tb-j716f-unixnu/sources/UniXNU-j716f/package/{name}')]:
        if p.exists():
            return p
    raise SystemExit(f"Missing {name}")

source = find_src('LinkStubs.c')
out = ROOT / 'build/link-stubs'
out.mkdir(parents=True, exist_ok=True)
obj, archive = out / 'LinkStubs.o', out / 'libLinkStubs.a'
subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios17.0', '-mkernel',
                '-Os', '-Wall', '-Werror', '-c', str(source), '-o', str(obj)], check=True)
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True)
print(archive)
