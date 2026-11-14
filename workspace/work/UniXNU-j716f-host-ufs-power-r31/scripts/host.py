#!/usr/bin/env python3
"""Fixed, host-only entry points to the unmodified official UniXNU CLI."""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import sys
import tomllib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('defconfig', 'fetch', 'status', 'tree', 'kernel', 'prelink'))
    args = parser.parse_args()
    device = Path(__file__).resolve().parents[1]
    root = device.parents[1] / 'UniXNU'
    board = tomllib.loads((device / 'board.toml').read_text())
    if board['board']['name'] != 'j716f-host-ufs-power-r31' or board['board']['example'] is not False:
        parser.error('expected the concrete j716f-host-ufs-power-r31 target')
    cache = root / 'out/ipsw/21H461/kernelcache.decompressed'
    if args.action in ('kernel', 'prelink'):
        if not cache.is_file() or hashlib.sha256(cache.read_bytes()).hexdigest() != (
                'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'):
            parser.error('missing or mismatched original 21H461 kernelcache')
    actions = {
        'defconfig': ['defconfig', 'j716f_host_ufs_power_r31_defconfig'],
        'fetch': ['fetch', 'xnu', 'libplatform', 'libdispatch', 'AvailabilityVersions',
                  'libpthread', 'corecrypto-2023', 'IOStorageFamily'],
        'status': ['status', '--json'],
        'tree': ['tree'],
        'kernel': ['build', 'kernel'],
        'prelink': ['build', 'prelink'],
    }
    command = [sys.executable, str(root / 'unixnu'), *actions[args.action],
               '--config', str(device / 'build-inputs.toml'), '-D', str(device),
               '--local-module', 'driver:J716fStorageBringup=' + str(device.parent / 'J716fStorageBringup-r31'),
               '--out', str(root / 'out-host-ufs-power-r31-20261114'), '--os', 'ipados-17.7.11-21H461',
               '--kernelcache', str(cache), '-j', '1']
    # Keep session network settings, but do not inherit unrelated target flags.
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('XNU_', 'UNIXNU_')) and k != 'JOBS'}
    env['UNIXNU_TOOLCHAIN'] = '/opt/unixnu-linux'
    env['PYTHONUNBUFFERED'] = '1'
    print('Host action: ' + args.action, file=sys.stderr, flush=True)
    return subprocess.call(command, cwd=root, env=env)


if __name__ == '__main__':
    sys.exit(main())
