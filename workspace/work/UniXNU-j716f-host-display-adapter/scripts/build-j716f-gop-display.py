#!/usr/bin/env python3
"""Run the independent board recipe in the official resolved build tree."""
from pathlib import Path
import os
import subprocess
import sys
if not os.environ.get('UNIXNU_DEVICE'):
    raise SystemExit('Run this recipe through the official device build kernel command')
device=Path(os.environ['UNIXNU_DEVICE']).resolve(strict=True)
if device.name != 'UniXNU-j716f-host-display-adapter':
    raise SystemExit('Wrong independent display target')
tree=Path(__file__).resolve().parents[1]
if tree != Path.cwd().resolve():
    raise SystemExit('Recipe must run from its official resolved tree')
source=device.parent/'J716fGopAdapter-20261113'
raise SystemExit(subprocess.call([sys.executable,str(source/'scripts/build.py'),
    '--tree',str(tree),'--source',str(source)]))
