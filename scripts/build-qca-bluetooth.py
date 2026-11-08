#!/usr/bin/env python3
"""Compile the QcaBluetooth module to kernel objects for the board build."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

tree = Path('.').resolve()
if not (tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').exists():
    cand = Path('/home/test/tb-j716f-unixnu/sources/UniXNU/out/j716f/ipados-17.7.11-21H461/j716f/tree')
    if cand.exists():
        tree = cand

if len(sys.argv) > 1:
    module = Path(sys.argv[1]).resolve()
elif len(tree.parents) >= 5 and (tree.parents[4] / 'drivers/QcaBluetooth').exists():
    module = (tree.parents[4] / 'drivers/QcaBluetooth').resolve()
elif (tree / 'drivers/QcaBluetooth').exists():
    module = (tree / 'drivers/QcaBluetooth').resolve()
else:
    module = Path('/home/test/tb-j716f-unixnu/sources/UniXNU/drivers/QcaBluetooth').resolve()

out = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else (tree / 'build/native-legacy-display/qca-bluetooth').resolve()
out.mkdir(parents=True, exist_ok=True)

if not (module / 'out/linux/btqca.c').is_file() or not (module / 'out/linux/btqca.h').is_file():
    fetch_env = {**os.environ, 'GIT_CONFIG_COUNT': '1', 'GIT_CONFIG_KEY_0': 'core.sparseCheckoutCone', 'GIT_CONFIG_VALUE_0': 'false'}
    if not (module / 'deps/linux').exists():
        subprocess.run([sys.executable, str(module / 'tools/fetch-linux.py')], check=True, env=fetch_env)
    subprocess.run([sys.executable, str(module / 'tools/prepare-linux.py')], check=True)

bsd_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json'
bsd_record = json.loads(bsd_path.read_text().strip().rstrip(','))
b_args = bsd_record['arguments']
cwd = Path(bsd_record['directory'])

base_cmd = []
k = 0
while k < len(b_args):
    val = b_args[k]
    if val == bsd_record['file']:
        base_cmd.append('__SOURCE__')
    elif val in ('-o', '-MF', '-MT', '-MJ'):
        if val == '-o':
            base_cmd += ['-o', '__OUTPUT__']
        k += 1
    elif val == '-Xclang' and b_args[k + 1].startswith('-W'):
        k += 1
    elif not val.startswith('-W'):
        base_cmd.append(val)
    k += 1

sources = [
    module / 'src/qbt_core.c',
    module / 'src/qbt_linux.c',
    module / 'src/qbt_geni.c',
    module / 'src/qbt_transport.c',
    module / 'out/linux/btqca.c',
]

objs = []
for src in sources:
    obj = out / (src.stem + '.o')
    cmd = [str(src) if x == '__SOURCE__' else str(obj) if x == '__OUTPUT__' else x for x in base_cmd]
    cmd += [
        '-mgeneral-regs-only',
        '-Wall', '-Wextra', '-Werror',
        '-Wno-unknown-warning-option', '-Wno-unused-parameter', '-Wno-sign-compare',
        '-Wno-unused-but-set-variable', '-Wno-unused-but-set-global',
        '-I' + str(module / 'include'),
        '-I' + str(module / 'src'),
        '-I' + str(module / 'shim'),
        '-I' + str(module / 'out/linux'),
    ]
    if src.name == 'btqca.c':
        cmd.append('-Dqbt_lx_qca_uart_setup=qca_uart_setup_btqca')
    subprocess.run(cmd, cwd=cwd, check=True)
    objs.append(obj)

report = {
    'source_sha256': {str(s.relative_to(module)): hashlib.sha256(s.read_bytes()).hexdigest() for s in sources},
    'object_sha256': {o.name: hashlib.sha256(o.read_bytes()).hexdigest() for o in objs},
}
(out / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n')
for obj in objs:
    print(f'OBJ {obj}')
