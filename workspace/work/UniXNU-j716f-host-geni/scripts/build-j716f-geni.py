#!/usr/bin/env python3
"""Invoke the frozen backend builder from the official archive stage."""
from pathlib import Path
import os
import subprocess
import sys
device = Path(os.environ['UNIXNU_DEVICE']).resolve(strict=True)
assert device.name == 'UniXNU-j716f-host-geni'
source = device.parent/'J716fGeniBackend-20261113'
raise SystemExit(subprocess.call([sys.executable, str(source/'scripts/build.py'),
                                 '--tree', str(Path.cwd()), '--source', str(source)]))
