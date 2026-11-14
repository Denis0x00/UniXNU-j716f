#!/usr/bin/env python3
"""Official archive wrapper; the CLI copies this script into its resolved tree."""
from pathlib import Path
import os
import subprocess
import sys
device = Path(os.environ['UNIXNU_DEVICE']).resolve(strict=True)
source = device.parent / 'J716fDisplayHandoff-20261113'
raise SystemExit(subprocess.call([sys.executable, str(source / 'scripts/build.py'),
    '--tree', str(Path.cwd().resolve()), '--source', str(source)]))
