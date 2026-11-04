#!/usr/bin/env python3
import json
from pathlib import Path
import subprocess

ROOT = Path('.').resolve()
def find_src(name):
    for p in [ROOT / f'kits/j716f/{name}', ROOT / f'package/{name}', Path(f'/home/test/tb-j716f-unixnu/sources/UniXNU-j716f/package/{name}')]:
        if p.exists():
            return p
    raise SystemExit(f"Missing {name}")

source = find_src('BootRamoops.c')
out = ROOT / 'build/boot-progress'
out.mkdir(parents=True, exist_ok=True)
obj, archive = out / 'BootRamoops.o', out / 'libBootProgress.a'
subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-target', 'arm64-apple-ios17.0', '-mkernel',
                '-mgeneral-regs-only', '-Os', '-Wall', '-Wextra', '-Werror',
                '-Wno-missing-field-initializers', '-c', str(source), '-o', str(obj)], check=True)

def compile_like(object_record, src, target, extra):
    record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / object_record).read_text().strip().rstrip(','))
    command, args, i = [], record['arguments'], 0
    while i < len(args):
        arg = args[i]
        if arg == record['file']:
            command.append(str(src))
        elif arg in ('-o', '-MF', '-MT', '-MJ'):
            if arg == '-o':
                command += ['-o', str(target)]
            i += 1
        elif arg == '-Xclang' and args[i + 1].startswith('-W'):
            i += 1
        elif not arg.startswith('-W'):
            command.append(arg)
        i += 1
    subprocess.run(command + ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
                              '-Wno-missing-field-initializers',
                              '-Wno-unused-but-set-variable', '-Wno-unused-but-set-global', *extra],
                   cwd=record['directory'], check=True, timeout=90)

policy_obj = out / 'BootWatchdogPolicy.o'
compile_like('bsd/DEVELOPMENT/kern_exec.o.json', find_src('BootWatchdogPolicy.c'), policy_obj, [])

fault_obj = out / 'BootFaultProbe.o'
compile_like('osfmk/DEVELOPMENT/startup.o.json', find_src('BootFaultProbe.c'), fault_obj,
             ['-I', str(ROOT / '.cache/xnu-work/osfmk')])

probe_objs = []
for probe in ('BootProcessProbe', 'BootUiTraceProbe'):
    p_obj = out / f'{probe}.o'
    compile_like('osfmk/DEVELOPMENT/startup.o.json', find_src(f'{probe}.c'), p_obj,
                 ['-I', str(ROOT / '.cache/xnu-work/osfmk')])
    probe_objs.append(p_obj)

archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(policy_obj), str(fault_obj), *[str(p) for p in probe_objs]], check=True)
print(archive)
