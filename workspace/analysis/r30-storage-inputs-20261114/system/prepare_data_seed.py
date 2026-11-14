#!/usr/bin/env python3
"""Create or inspect an immutable first-install 21H461 Data seed on the host.

Creation is explicitly macOS-only and requires a chosen logical size. It mounts
the pinned Apple OS image read-only, runs the official seed tool, and preserves
the resulting installation identity. No target disk/partition is accessed.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import platform
import plistlib
import struct
import subprocess
import sys
import tempfile

import prepare_inputs as inputs
from unixnu.runtime import seed_uuid
from verify_raw import fletcher64


def inspect(seed):
    seed = Path(seed).resolve()
    spec = importlib.util.spec_from_file_location('r30_packed_ramdisk', inputs.CORE / 'drivers/DarwinBoot/Test/packed_ramdisk.py')
    packed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packed)
    metadata, header = packed.inspect(seed)
    if not metadata.get('packed') or header[32:36] != b'NXSB' or fletcher64(header[8:]) != header[:8]:
        raise ValueError('require a packed Data seed with a checksummed APFS container')
    logical, extents = packed.regions(seed.read_bytes())
    if struct.unpack_from('<I', header, 36)[0] != 4096 or struct.unpack_from('<Q', header, 40)[0] * 4096 != logical:
        raise ValueError('APFS container size/block geometry differs from packed logical image')
    count = 0
    for _, data in extents:
        for at in range(0, len(data), 4096):
            block = bytes(data[at:at + 4096])
            if block[32:36] == b'APSB':
                if fletcher64(block[8:]) != block[:8]:
                    raise ValueError('Data volume superblock checksum failed')
                count += 1
    if not count:
        raise ValueError('no checksummed Data volume superblock')
    identity = seed_uuid(inputs.CORE, seed)
    return dict(path=str(seed), **metadata, data_volume_uuid=identity,
                checksummed_volume_superblocks=count, installation_identity_must_be_preserved=True,
                payload_memory_and_capacity_qualified=False, on_device_persistence_qualified=False,
                content_build_not_inferred_from_packed_header=True)


def create(gib, skip_setup):
    if platform.system() != 'Darwin':
        raise ValueError('official APFS Data creation requires macOS hdiutil/diskutil; no Linux/Windows substitute is fabricated')
    if not 3 <= gib <= 1024:
        raise ValueError('official seed size must be 3..1024 GiB')
    profile = inputs.profile()
    source = inputs.ARTIFACT / 'ipsw/21H461/044-76040-038.dmg'
    expected = profile['ipsw']['sha256'][source.name]
    if inputs.sha(source) != expected:
        raise ValueError('OS image differs from official 21H461 pin')
    folder = inputs.ARTIFACT / 'data'
    folder.mkdir(parents=True, exist_ok=True)
    output, report_path = folder / 'data-21H461.dbr', folder / 'data-21H461.json'
    if output.exists():
        if not report_path.exists():
            raise ValueError('an existing seed has no provenance; preserve it and inspect it separately')
        prior = json.loads(report_path.read_text())
        if prior['source_os_sha256'] != expected or prior['logical_gib'] != gib or prior['skip_setup'] != skip_setup:
            raise ValueError('existing first-install parameters differ; automatic regeneration is forbidden')
        current = inspect(output)
        if current['encoded_sha256'] != prior['seed']['encoded_sha256']:
            raise ValueError('preserved installation seed changed')
        return prior
    with tempfile.TemporaryDirectory(prefix='r30-readonly-os-', dir=folder) as temp:
        mount = Path(temp) / 'OS'
        mount.mkdir()
        result = subprocess.check_output(['hdiutil', 'attach', '-readonly', '-nobrowse', '-owners', 'off',
                                          '-mountpoint', str(mount), '-plist', str(source)])
        attached = plistlib.loads(result)
        device = attached['system-entities'][0]['dev-entry']
        try:
            system = plistlib.loads((mount / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
            if system.get('ProductBuildVersion') != '21H461' or system.get('ProductVersion') != '17.7.11':
                raise ValueError('mounted SystemVersion differs from selected target')
            command = [sys.executable, str(inputs.CORE / 'scripts/make-data-seed.py'), str(mount), str(output),
                       '--gib', str(gib), '--build', '21H461']
            if skip_setup:
                command.append('--skip-setup')
            subprocess.run(command, check=True)
        finally:
            subprocess.run(['hdiutil', 'detach', device], check=True)
    report = dict(build='21H461', version='17.7.11', first_install=True, logical_gib=gib,
                  skip_setup=skip_setup, source_os_sha256=expected, source_os=str(source),
                  official_seed_tool_sha256=inputs.sha(inputs.CORE / 'scripts/make-data-seed.py'),
                  seed=inspect(output), target_partition_selected=False, installed=False)
    inputs.record(report_path, report)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='action', required=True)
    check = sub.add_parser('inspect')
    check.add_argument('seed', type=Path)
    init = sub.add_parser('create')
    init.add_argument('--gib', type=int, required=True)
    init.add_argument('--skip-setup', action='store_true')
    args = parser.parse_args()
    result = inspect(args.seed) if args.action == 'inspect' else create(args.gib, args.skip_setup)
    print(json.dumps(result, indent=2))
