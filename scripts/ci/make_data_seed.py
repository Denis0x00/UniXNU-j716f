#!/usr/bin/env python3
"""macOS-only fresh Data creation; no device, private identity or disk install.

Use the fixed official seed/grow/packing tools. Reconstruct and verify the packed
result using actual macOS APFS before publishing it. Only the fresh Data seed,
its provenance, checksums and sanitized logs leave this runner.
"""
import argparse
import contextlib
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import signal
import stat
import struct
import subprocess
import sys
import time
import tomllib
from types import SimpleNamespace
import uuid

from apfs_seed_compat import prepare_overlay

GIB = 1024 ** 3
LOCK = Path(__file__).with_name('data_seed_inputs.json')


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def sha(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for data in iter(lambda: source.read(8 << 20), b''):
            digest.update(data)
    return digest.hexdigest()


def write_json(path, data):
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def below(path, root):
    try:
        Path(path).resolve().relative_to(root.resolve())
        return True
    except (ValueError, OSError):
        return False


class Log:
    def __init__(self, path, roots):
        self.path = path
        self.roots = sorted(((str(p), label) for p, label in roots), key=lambda x: -len(x[0]))

    def clean(self, text):
        for path, label in self.roots:
            text = text.replace(path, label)
        return text

    def write(self, text):
        text = self.clean(str(text))
        with self.path.open('a', encoding='utf-8') as output:
            output.write(text)
        sys.__stdout__.write(text)
        sys.__stdout__.flush()
        return len(text)

    def flush(self):
        pass

    def command(self, *args, timeout=900, check=True, plist=False):
        args = [str(arg) for arg in args]
        self.write('+ ' + ' '.join(args) + '\n')
        # grow.py and diskutil inherit this command session. Stop the group
        # before cleanup on failure, so no descendant keeps writing a mounted
        # image after only its direct Python parent was killed.
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=timeout)
        except BaseException:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                stdout, stderr = process.communicate(timeout=15)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(process.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                stdout, stderr = process.communicate(timeout=15)
            self.write(stdout.decode(errors='replace'))
            self.write(stderr.decode(errors='replace'))
            raise
        result = subprocess.CompletedProcess(args, process.returncode, stdout, stderr)
        if result.stderr:
            self.write(result.stderr.decode(errors='replace'))
        if not plist or result.returncode:
            self.write(result.stdout.decode(errors='replace'))
        if check:
            require(result.returncode == 0, f'{Path(args[0]).name} exited {result.returncode}')
        if plist and result.returncode == 0:
            return plistlib.loads(result.stdout)
        return result


def image_entries(log):
    return log.command('hdiutil', 'info', '-plist', plist=True).get('images', [])


def whole_disk(entry):
    devices = [item.get('dev-entry', '') for item in entry.get('system-entities', [])]
    matches = [d for d in devices if re.fullmatch(r'/dev/disk\d+', d)]
    require(matches, 'No whole image attachment device in hdiutil result')
    return matches[0]


def detach(log, device):
    require(re.fullmatch(r'/dev/disk\d+', device), 'Refuse non-image detach device')
    result = log.command('hdiutil', 'detach', device, timeout=120, check=False)
    if result.returncode:
        log.command('hdiutil', 'detach', device, '-force', timeout=120)


def cleanup(log, roots):
    errors = []
    for entry in image_entries(log):
        path = entry.get('image-path')
        if not path or not any(below(path, root) for root in roots):
            continue
        try:
            detach(log, whole_disk(entry))
        except Exception as error:
            errors.append(log.clean(str(error)))
    remaining = [entry for entry in image_entries(log) if entry.get('image-path') and
                 any(below(entry['image-path'], root) for root in roots)]
    require(not errors and not remaining, 'Owned image detach incomplete: ' + '; '.join(errors))


@contextlib.contextmanager
def mount_readonly(log, image, mount, raw=False):
    mount.mkdir()
    args = ['hdiutil', 'attach', '-readonly', '-nobrowse', '-owners', 'off',
            '-mountpoint', str(mount), '-plist']
    if raw:
        args += ['-imagekey', 'diskimage-class=CRawDiskImage']
    attached = log.command(*args, image, plist=True)
    device = whole_disk(attached)
    try:
        mounts = [entry for entry in attached['system-entities']
                  if entry.get('mount-point') == str(mount)]
        require(len(mounts) == 1, 'Expected one requested mountpoint')
        current = [entry for entry in image_entries(log)
                   if Path(entry.get('image-path', '')).resolve() == image.resolve()]
        require(len(current) == 1 and current[0].get('writeable') is False,
                'Read-only attachment was not confirmed by hdiutil')
        info = log.command('diskutil', 'info', '-plist', mounts[0]['dev-entry'], plist=True)
        fs = str(info.get('FilesystemType', info.get('FileSystemPersonality', ''))).lower()
        require(fs == 'apfs', 'Mounted volume is not APFS')
        yield mounts[0]['dev-entry'], info
    finally:
        detach(log, device)


def fletcher64(data):
    lo = hi = 0
    for word, in struct.iter_unpack('<I', data):
        lo = (lo + word) % 0xffffffff
        hi = (hi + lo) % 0xffffffff
    c0 = 0xffffffff - ((lo + hi) % 0xffffffff)
    c1 = 0xffffffff - ((lo + c0) % 0xffffffff)
    return struct.pack('<II', c0, c1)


def inspect_seed(packed, seed, gib):
    metadata, header = packed.inspect(seed)
    require(metadata.get('packed') and metadata['bytes'] == gib * GIB, 'Wrong packed logical size')
    require(header[32:36] == b'NXSB' and header[:8] == fletcher64(header[8:]),
            'Packed APFS container header/checksum invalid')
    blocks = struct.unpack_from('<Q', header, 40)[0]
    require(struct.unpack_from('<I', header, 36)[0] == 4096 and
            gib * GIB - (64 << 20) <= blocks * 4096 <= gib * GIB, 'Wrong APFS geometry')
    logical, regions = packed.regions(seed.read_bytes())
    identities = set()
    count = 0
    for _, data in regions:
        for at in range(0, len(data), 4096):
            block = bytes(data[at:at + 4096])
            if block[32:36] == b'APSB':
                require(block[:8] == fletcher64(block[8:]), 'APFS volume checksum mismatch')
                identities.add(str(uuid.UUID(bytes=block[240:256])).upper())
                count += 1
    require(len(identities) == 1, 'Require exactly one Data volume UUID')
    metadata['apfs_container_features'] = dict(zip(('features', 'readonly_compatible', 'incompatible'),
                                                 struct.unpack_from('<QQQ', header, 48)))
    return metadata, logical, regions, identities.pop(), count


def source_checks(log, core, pins):
    for repo, expected in ((core, pins['core_commit']),
                           (core / 'drivers/DarwinBoot', pins['darwinboot_commit'])):
        result = log.command('git', '-c', 'safe.directory=' + str(repo), '-C', repo, 'rev-parse', 'HEAD')
        require(result.stdout.decode().strip() == expected, 'Source commit differs from fixed input')
        result = log.command('git', '-c', 'safe.directory=' + str(repo), '-C', repo,
                             'diff', '--quiet', 'HEAD', '--', check=False)
        require(result.returncode == 0, 'Tracked official source is modified')
    profile = tomllib.loads((core / pins['profile']).read_text())
    require(profile['ipsw']['url'] == pins['ipsw_url'], 'Profile IPSW URL changed')
    for field in ('version', 'build', 'product', 'device_class'):
        require(profile['ipsw'][field] == pins[field], 'Profile identity changed: ' + field)
    for name in ('os', 'systemos'):
        require(profile['ipsw']['sha256'][pins[name]['member']] == pins[name]['sha256'], 'Profile image pin changed')
    require(profile['root']['os_volume_uuid'] == pins['root_uuid'], 'Profile root UUID changed')
    return profile


def open_ipsw(log, factory, ctx, url):
    # The pinned upstream retries data blocks, but HEAD/initial probe do not
    # retry. Apple may close an initial TLS connection transiently. Retry only
    # transport failures, before any Data image is created; never pin failures.
    for attempt in range(4):
        try:
            return factory(ctx, spec=url)
        except OSError as error:
            if attempt == 3:
                raise
            log.write(f'IPSW connection retry {attempt + 1}/3: {error}\n')
            time.sleep(2 ** attempt)


def fetch_inputs(log, core, work, pins, profile):
    sys.path.insert(0, str(core / 'scripts'))
    from unixnu.ipsw import Ipsw
    ctx = SimpleNamespace(board=profile, out_root=work / 'download')
    with contextlib.redirect_stdout(log), contextlib.redirect_stderr(log):
        source = open_ipsw(log, Ipsw, ctx, pins['ipsw_url'])
        try:
            manifest = source.zip.read('BuildManifest.plist')
            require(hashlib.sha256(manifest).hexdigest() == pins['build_manifest_sha256'], 'Manifest pin mismatch')
            record = {}
            for component, key in (('OS', 'os'), ('Cryptex1,SystemOS', 'systemos')):
                name, size, compressed = source.info(component)
                require(name == pins[key]['member'] and size == pins[key]['bytes'], 'Component metadata mismatch')
                record[key] = dict(member=name, bytes=size, compressed_bytes=compressed,
                                   expected_sha256=pins[key]['sha256'], body_sha256_verified=False)
            os_image = source.disk_image('OS')
            require(os_image.stat().st_size == pins['os']['bytes'] and sha(os_image) == pins['os']['sha256'],
                    'Downloaded OS pin mismatch')
            record['os']['body_sha256_verified'] = True
            trust = [source.extract(component) for component in ('StaticTrustCache', 'Cryptex1,SystemTrustCache')]
        finally:
            source.zip.close()
            source.stream.close()
    bundle = work / 'trustcache-21H461.bin'
    log.command(sys.executable, core / 'scripts/bundle-ipsw-trust.py', bundle, *trust)
    require(bundle.stat().st_size == pins['trust_bundle']['bytes'] and sha(bundle) == pins['trust_bundle']['sha256'],
            'Original trust bundle pin mismatch')
    record['trust_bundle'] = dict(pins['trust_bundle'], body_sha256_verified=True, modified=False)
    record['build_manifest_sha256'] = pins['build_manifest_sha256']
    return os_image, record


def grow_preflight(log, overlay, work, evidence, gib):
    """Exercise this host's raw APFS discovery before the 5 GB OS download.

    This empty diagnostic filesystem is never used as installation Data.
    """
    root = work / 'apfs-discovery-preflight'
    root.mkdir()
    raw = root / 'empty.cdr'
    log.command('hdiutil', 'create', '-size', '32m', '-layout', 'NONE', '-fs', 'APFS',
                '-volname', 'DataDiscoveryPreflight', '-type', 'UDTO', raw)
    helper = overlay / 'drivers/DarwinBoot/Test/packed_ramdisk.py'
    spec = importlib.util.spec_from_file_location('preflight_packed_ramdisk', helper)
    packed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packed)
    base, grown = root / 'empty-base.dbr', root / 'empty-grown.dbr'
    packed.pack(raw, base)
    log.command(sys.executable, overlay / 'scripts/seed-data-grow.py', '--gib', gib, base, grown,
                timeout=600)
    result, _ = packed.inspect(grown)
    require(result['bytes'] == gib * GIB, 'Preflight grew an unexpected logical capacity')
    write_json(evidence / 'apfs-discovery-preflight.json', {
        'status': 'passed', 'diagnostic_only': True, 'installation_seed': False,
        'empty_filesystem': True, 'logical_gib': gib, 'hardware_tested': False,
        'encoded_sha256': sha(grown),
    })


def create(log, args, pins):
    require(not args.work.exists() and not args.output.exists(), 'Refuse to replace an existing run or seed')
    require(3 <= args.gib <= 64, 'Runner Data size must be 3..64 GiB')
    args.work.mkdir(parents=True)
    require(shutil.disk_usage(args.work).free >= 8 * GIB, 'Need at least 8 GiB free for OS and sparse verification')
    profile = source_checks(log, args.core, pins)
    build = args.core / 'build'
    require(not build.exists(), 'Use a fresh official checkout without build outputs')
    build.mkdir()
    overlay = args.work / 'grow-compat-core'
    compatibility = prepare_overlay(args.core, overlay)
    write_json(args.evidence / 'grow-compatibility.json', compatibility)
    grow_preflight(log, overlay, args.work, args.evidence, args.gib)
    os_image, inputs = fetch_inputs(log, args.core, args.work, pins, profile)
    write_json(args.evidence / 'inputs.json', inputs)
    candidate = args.work / 'data-21H461.dbr'
    os_mount = args.work / 'os-mount'
    with mount_readonly(log, os_image, os_mount) as (_, info):
        require(info.get('VolumeUUID', '').upper() == pins['root_uuid'], 'Mounted OS volume UUID mismatch')
        version = plistlib.loads((os_mount / 'System/Library/CoreServices/SystemVersion.plist').read_bytes())
        require(version.get('ProductVersion') == pins['version'] and
                version.get('ProductBuildVersion') == pins['build'], 'Mounted OS build/version mismatch')
        require((os_mount / 'sbin/launchd').is_file() and (os_mount / 'private/var').is_dir(),
                'Not the OS root expected by make-data-seed.py')
        launchd_hash = sha(os_mount / 'sbin/launchd')
        log.command(sys.executable, overlay / 'scripts/make-data-seed.py', os_mount, candidate,
                    '--gib', args.gib, '--build', pins['build'], timeout=3600)
    require(sha(os_image) == pins['os']['sha256'], 'Read-only original OS changed')
    helper = args.core / 'drivers/DarwinBoot/Test/packed_ramdisk.py'
    spec = importlib.util.spec_from_file_location('fixed_packed_ramdisk', helper)
    packed = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packed)
    metadata, logical, regions, volume_uuid, superblocks = inspect_seed(packed, candidate, args.gib)
    raw = args.work / 'verify-data.raw'
    with raw.open('xb') as output:
        output.truncate(logical)  # APFS-host sparse file; never write zero-filled capacity.
        for offset, data in regions:
            output.seek(offset)
            output.write(data)
    allocated = raw.stat().st_blocks * 512
    require(allocated < logical, 'Verification raw is not sparse')
    data_mount = args.work / 'data-mount'
    with mount_readonly(log, raw, data_mount, raw=True) as (volume, info):
        require(info.get('VolumeUUID', '').upper() == volume_uuid, 'diskutil Data UUID differs from packed metadata')
        require(info.get('VolumeName') == 'Data', 'Unexpected Data volume name')
        for name in ('keybags', 'mobile/tmp', 'tmp', 'run', 'containers/Bundle',
                     'containers/Data/System', 'containers/Shared/SystemGroup', 'containers/Temp'):
            require((data_mount / name).is_dir(), 'Missing initial Data directory: ' + name)
        require(stat.S_IMODE((data_mount / 'tmp').stat().st_mode) == 0o1777, 'Data tmp mode mismatch')
        log.command('diskutil', 'verifyVolume', volume, timeout=900)
        # Record a focused subset; never export host disk inventories or home data.
        verified_info = {key: info[key] for key in ('VolumeUUID', 'VolumeName', 'FilesystemType',
                         'FileSystemPersonality', 'TotalSize', 'VolumeTotalSpace') if key in info}
    require(sha(candidate) == metadata['encoded_sha256'], 'Packed seed changed during verification')
    cleanup(log, [args.work, args.core / 'build'])
    script_hashes = {name: sha(args.core / name) for name in (
        'scripts/make-data-seed.py', 'scripts/seed-data-grow.py', 'scripts/bundle-ipsw-trust.py',
        'scripts/unixnu/ipsw.py', 'drivers/DarwinBoot/Test/packed_ramdisk.py', pins['profile'])}
    report = dict(schema=1, build=pins['build'], version=pins['version'], first_install=True,
                  preserve_for_future_builds=True, logical_gib=args.gib, data_volume_uuid=volume_uuid,
                  core_commit=pins['core_commit'], darwinboot_commit=pins['darwinboot_commit'],
                  grow_discovery_compatibility=compatibility,
                  script_sha256=script_hashes, inputs=inputs, source_launchd_sha256=launchd_hash,
                  encoded_bytes=metadata['encoded_bytes'], encoded_sha256=metadata['encoded_sha256'],
                  logical_bytes=logical, format_seed_identity_sha256=metadata['sha256'],
                  apfs_container_features=metadata['apfs_container_features'],
                  identity_algorithm='SHNGSEED2 (not a whole-raw-file SHA256 for disks above 2 GiB)',
                  checksummed_volume_superblocks=superblocks, verification_raw_allocated_bytes=allocated,
                  diskutil_info=verified_info, diskutil_verify_volume_passed=True,
                  host_macos=platform.mac_ver()[0], skip_setup=False, mounts_detached=True,
                  systemos_body_downloaded=False, private_device_identity_used=False,
                  target_partition_selected=False, payload_memory_and_capacity_qualified=False,
                  target_apfs_mount_verified=False, runtime_tested=False, installed=False)
    args.output.mkdir()
    shutil.copy2(candidate, args.output / candidate.name)
    require(sha(args.output / candidate.name) == metadata['encoded_sha256'], 'Published seed copy mismatch')
    write_json(args.output / 'data-21H461.json', report)
    files = ('data-21H461.dbr', 'data-21H461.json')
    (args.output / 'SHA256SUMS').write_text(''.join(sha(args.output / name) + '  ' + name + '\n'
                                                 for name in files), encoding='ascii')
    log.command(sys.executable, Path(__file__).with_name('verify_data_seed.py'), args.output)
    write_json(args.evidence / 'result.json', dict(status='passed', **report))
    log.write('Verified fresh Data seed ready; preserve its UUID and SHA256 for later builds.\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('core', 'work', 'output', 'evidence'):
        parser.add_argument('--' + name, required=True, type=Path)
    parser.add_argument('--gib', type=int, default=16)
    parser.add_argument('--cleanup-only', action='store_true')
    args = parser.parse_args()
    for name in ('core', 'work', 'output', 'evidence'):
        setattr(args, name, getattr(args, name).resolve())
    require(platform.system() == 'Darwin', 'APFS creation and verification require actual macOS hdiutil/diskutil')
    require(all(shutil.which(tool) for tool in ('hdiutil', 'diskutil', 'git')), 'Missing macOS image tools')
    roots = [args.work, args.core / 'build']
    require(all(not below(a, b) and not below(b, a) for a, b in (
        (args.work, args.core), (args.output, args.work), (args.evidence, args.work),
        (args.output, args.evidence))), 'Working, source and publication directories must be separate')
    args.evidence.mkdir(parents=True, exist_ok=True)
    log = Log(args.evidence / 'creation.log', [(args.core, '<CORE>'), (args.work, '<WORK>'),
              (args.output, '<ARTIFACT>'), (args.evidence, '<EVIDENCE>'), (Path.home(), '<HOME>')])
    if args.cleanup_only:
        cleanup(log, roots)
        return
    pins = json.loads(LOCK.read_text())
    def interrupted(signum, frame):
        raise RuntimeError('Job interrupted; attempting owned image cleanup')
    signal.signal(signal.SIGTERM, interrupted)
    try:
        create(log, args, pins)
    except BaseException as error:
        cleanup_error = None
        try:
            cleanup(log, roots)
        except Exception as cleanup_failure:
            cleanup_error = log.clean(str(cleanup_failure))
        failure = dict(status='failed', error=log.clean(str(error)), cleanup_error=cleanup_error,
                       seed_published=False, hardware_tested=False)
        write_json(args.evidence / 'failure.json', failure)
        log.write(json.dumps(failure) + '\n')
        raise SystemExit(1)


if __name__ == '__main__':
    main()
