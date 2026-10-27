#!/usr/bin/env python3
"""Create an isolated source-integration device; never change diagnostic inputs."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import tomllib


def prepare(device, output):
    device, output = Path(device).resolve(), Path(output).resolve()
    if output.exists():
        raise ValueError('Candidate directory already exists; preserve its build/evidence')
    if output == device or device in output.parents:
        raise ValueError('Candidate must be outside the original device repository')
    board = (device/'board.toml').read_text(encoding='utf-8')
    original = tomllib.loads(board)
    native_components = ['XnuPrelink', 'DarwinBoot', 'ArmDTPlatform', 'ArmCpu']
    legacy = original['components']['include'] == ['XnuPrelink', 'DarwinBoot']
    if not legacy and original['components']['include'] != native_components:
        raise ValueError('Unexpected native component selection')
    edits = [
        ('include = ["XnuPrelink", "DarwinBoot"]',
         'include = ["XnuPrelink", "DarwinBoot", "ArmDTPlatform", "ArmCpu"]'),
        ('defconfig = "j716f_source_defconfig"',
         'defconfig = "j716f_arch_timer_source_defconfig"'),
        ('archives = [\n',
         'archives = [\n  "component:ArmDTPlatform",\n  "component:ArmCpu",\n'),
    ] if legacy else []
    for old, new in edits:
        if board.count(old) != 1:
            raise ValueError('Board transformation must match exactly once: '+old)
        board = board.replace(old, new)
    # Historical source candidates silently inherited the V17 post-MMU reset.
    # Fail closed on old packaging policy, even when the caller only compiles.
    native = tomllib.loads(board)
    payload = native['payload']
    if (native['kernel']['defconfig'] != 'j716f_arch_timer_source_defconfig'
            or payload.get('panic_screen') is not False
            or any(k.startswith('entry_probe_') for k in payload)
            or 'V17' in payload.get('title', '')
            or not {'component:ArmCpu', 'component:ArmDTPlatform'} <= set(native['kernel']['archives'])):
        raise ValueError('Native mainline must use native archives and explicitly disable legacy binary/reset patching')
    # Only source configuration is copied; no private local configuration,
    # evidence media, source outputs, Apple inputs or firmware is staged.
    for name in ['configs', 'dts', 'socs', 'patches']:
        shutil.copytree(device/name, output/name)
    for name in ['feeds.toml', 'unixnu.lock']:
        shutil.copyfile(device/name, output/name)
    # Only this board-owned observation package enters the source candidate.
    # Historical diagnostic packages and payloads are intentionally not staged.
    shutil.copytree(device/'package/native-observer', output/'package/native-observer')
    shutil.copytree(device/'package/native-log', output/'package/native-log')
    (output/'board.toml').write_text(board, encoding='utf-8')
    manifest = {
        'phase': 'source-platform-integration-candidate',
        'components': tomllib.loads(board)['components']['include'],
        'defconfig': 'j716f_arch_timer_source_defconfig',
        'original_board_sha256': hashlib.sha256((device/'board.toml').read_bytes()).hexdigest(),
        'candidate_board_sha256': hashlib.sha256((output/'board.toml').read_bytes()).hexdigest(),
        'hardware_validated': False,
        'prelink_performed': False,
        'device_writes_performed': False,
        'legacy_entry_reset_enabled': False,
        'hardware_launch_admitted': False,
    }
    (output/'candidate.json').write_text(json.dumps(manifest, indent=2)+'\n', encoding='utf-8')
    return manifest


if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--device', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args=parser.parse_args()
    print(json.dumps(prepare(args.device,args.output),indent=2))
