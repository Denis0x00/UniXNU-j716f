#!/usr/bin/env python3
"""Restricted entry to the unchanged official firmware CLI."""
import argparse, os, subprocess, sys, tomllib
from pathlib import Path
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('action', choices=('defconfig', 'status', 'tree', 'firmware'))
a = p.parse_args()
d = Path(__file__).resolve().parents[1]
r = d.parents[1] / 'UniXNU'
b = tomllib.loads((d/'board.toml').read_text())
if b['board']['name'] != 'j716f-early-log-firmware':
    p.error('Unexpected target')
actions = {'defconfig':['defconfig','j716f_early_log_firmware_defconfig'],
           'status':['status','--json'], 'tree':['tree'], 'firmware':['build','firmware']}
env = {k:v for k,v in os.environ.items() if not k.startswith(('XNU_','UNIXNU_')) and k != 'JOBS'}
env.update(UNIXNU_TOOLCHAIN='/opt/unixnu-linux', PYTHONUNBUFFERED='1')
argv = [sys.executable, str(r/'unixnu'), *actions[a.action], '--config', str(d/'build-inputs.toml'),
        '-D', str(d), '--out', str(r/'out-early-log-firmware-20261114'), '--os', 'ipados-17.7.11-21H461',
        '--uefi', str(d.parent/'uefi-j716f-early-log'), '--target', 'DEBUG', '-j', '2']
sys.exit(subprocess.call(argv, cwd=r, env=env))
