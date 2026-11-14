#!/usr/bin/env python3
"""Narrow APFS discovery compatibility for the pinned official grow helper.

Never change seed construction, sizing, resizing, packing, or input identities.
Only an exact physical-store match owned by this newly attached raw image may
reach the upstream resize command. Host-wide inventories are never printed.
"""
import contextlib
import hashlib
import json
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import time


SOURCE_SHA256 = {
    'scripts/make-data-seed.py': 'a10a818ae19e2853b8a2e04ac56f9af2ea555c47ef6abaf85e0ee4da2865df0b',
    'scripts/seed-data-grow.py': '2c4b6da3f87fc4e148f0eb9321372bab42149b56eae0c4b1f405a900d81f1e44',
    'drivers/DarwinBoot/Test/packed_ramdisk.py': 'efb5fac21ae94dcb1aecd47da2b73a7112b487e43692ac6396858552d27a0e48',
}
BEFORE = """        attached = run('hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage', '-nomount', '-nobrowse', str(raw))
        store = re.match(r'(/dev/disk\\d+)', attached).group(1).split('/')[-1]
        try:
            listing = plistlib.loads(subprocess.check_output(['diskutil', 'apfs', 'list', '-plist']))
            container = None
            for entry in listing['Containers']:
                if any(s['DeviceIdentifier'] == store for s in entry.get('PhysicalStores', [])):
                    container = entry['ContainerReference']
            if not container:
                sys.exit('No APFS container found on the attached seed')
            run('diskutil', 'apfs', 'resizeContainer', container, '0')
        finally:
            subprocess.run(['hdiutil', 'detach', store, '-force'], capture_output=True)
"""
AFTER = """        with attached_seed_container(raw) as container:
            run('diskutil', 'apfs', 'resizeContainer', container, '0')
"""


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def prepare_overlay(core, destination):
    """Copy three hash-locked upstream files; change only grow discovery."""
    require(not destination.exists(), 'Refuse to replace a compatibility overlay')
    sources = {name: (core / name).read_bytes() for name in SOURCE_SHA256}
    for name, data in sources.items():
        require(digest(data) == SOURCE_SHA256[name], 'Compatibility source pin mismatch: ' + name)
    grow = sources['scripts/seed-data-grow.py'].decode('utf-8')
    require(grow.count(BEFORE) == 1 and grow.count('import tempfile\n') == 1,
            'Pinned grow discovery region differs')
    patched = grow.replace('import tempfile\n',
                           'import tempfile\nfrom apfs_seed_compat import attached_seed_container\n')
    patched = patched.replace(BEFORE, AFTER).encode('utf-8')
    destination.mkdir(parents=True)
    for name, data in sources.items():
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(patched if name == 'scripts/seed-data-grow.py' else data)
    helper = destination / 'scripts/apfs_seed_compat.py'
    shutil.copyfile(Path(__file__), helper)
    (destination / 'build').mkdir()
    return {
        'kind': 'exact-store-discovery-with-bounded-wait',
        'official_checkout_modified': False,
        'official_source_sha256': SOURCE_SHA256,
        'patched_grow_sha256': digest(patched),
        'compat_helper_sha256': digest(helper.read_bytes()),
        'seed_algorithm_modified': False,
        'physical_store_match_widened': False,
        'maximum_discovery_attempts': 11,
        'discovery_retry_seconds': 1,
        'discovery_deadline_seconds': 15,
    }


def event(name, **facts):
    print(json.dumps({'apfs_seed_discovery': name, **facts}, sort_keys=True), flush=True)


def plist_command(*args, timeout=30):
    result = subprocess.run(args, capture_output=True, check=True, timeout=timeout)
    return plistlib.loads(result.stdout)


def attachment_store(attached):
    entities = attached.get('system-entities', [])
    require(entities and isinstance(entities[0], dict), 'No first image attachment entity')
    device = entities[0].get('dev-entry', '')
    # Preserve the old first-whole-disk rule. Do not infer a parent by prefix or
    # select a partition/synthesized device to make an unmatched result pass.
    require(re.fullmatch(r'/dev/disk\d+', device), 'First image entity is not a whole disk')
    return device.removeprefix('/dev/')


def owned_image(raw, store, timeout=30):
    images = plist_command('hdiutil', 'info', '-plist', timeout=timeout).get('images', [])
    matching = [entry for entry in images if entry.get('image-path') and
                Path(entry['image-path']).resolve() == raw.resolve()]
    require(len(matching) == 1, 'Raw image attachment ownership is absent or ambiguous')
    require(attachment_store(matching[0]) == store, 'Raw image attachment changed its first whole disk')
    return matching[0]


def container_is_attached(owned, container):
    return any(entry.get('dev-entry') == '/dev/' + container
               for entry in owned.get('system-entities', []))


def exact_container(listing, store):
    matches = [entry for entry in listing.get('Containers', [])
               if any(s.get('DeviceIdentifier') == store for s in entry.get('PhysicalStores', []))]
    require(len(matches) <= 1, 'Ambiguous APFS containers for the exact image store')
    if not matches:
        return None
    match = matches[0]
    stores = [s.get('DeviceIdentifier') for s in match.get('PhysicalStores', [])]
    require(stores == [store], 'Refuse APFS container spanning additional physical stores')
    container = match.get('ContainerReference', '')
    require(re.fullmatch(r'disk\d+', container), 'Invalid APFS container reference')
    require(container != store, 'APFS synthesized container equals its physical store')
    return container


def scoped_diagnostics(raw, store, attached, listing):
    owned = owned_image(raw, store)
    devices = {e.get('dev-entry', '').removeprefix('/dev/')
               for e in attached.get('system-entities', []) + owned.get('system-entities', [])}
    devices = {d for d in devices if re.fullmatch(r'disk\d+(s\d+)*', d)}
    # Each diskutil info argument came from this image's own attachment plist.
    keys = ('DeviceIdentifier', 'ParentWholeDisk', 'PartOfWhole', 'Whole', 'Content',
            'FilesystemType', 'APFSContainerReference', 'APFSPhysicalStores', 'APFSContainerUUID')
    for device in sorted(devices):
        try:
            info = plist_command('diskutil', 'info', '-plist', '/dev/' + device)
            event('owned-device-info', device=device, info={k: info[k] for k in keys if k in info})
        except (OSError, ValueError, subprocess.SubprocessError) as error:
            event('owned-device-info-failed', device=device, error=type(error).__name__)
    relevant = [entry for entry in listing.get('Containers', [])
                if entry.get('ContainerReference') in devices or
                any(s.get('DeviceIdentifier') in devices for s in entry.get('PhysicalStores', []))]
    event('owned-topology', containers=[{
        k: entry[k] for k in ('ContainerReference', 'PhysicalStores') if k in entry
    } for entry in relevant])


@contextlib.contextmanager
def attached_seed_container(raw):
    raw = Path(raw).resolve()
    require(raw.is_file(), 'Seed raw image is missing')
    attached = plist_command('hdiutil', 'attach', '-imagekey', 'diskimage-class=CRawDiskImage',
                             '-nomount', '-nobrowse', '-plist', str(raw))
    # This plist is scoped to the image just attached; never print hdiutil info
    # or the global APFS listing. The outer runner redacts work directory paths.
    event('attach', entities=attached.get('system-entities', []))
    store = attachment_store(attached)
    try:
        listing = {}
        deadline = time.monotonic() + 15
        def remaining():
            seconds = deadline - time.monotonic()
            if seconds <= 0:
                raise TimeoutError('APFS discovery deadline expired')
            return seconds
        for attempt in range(11):
            try:
                owned_image(raw, store, timeout=remaining())
                listing = plist_command('diskutil', 'apfs', 'list', '-plist', timeout=remaining())
            except (TimeoutError, subprocess.TimeoutExpired):
                break
            container = exact_container(listing, store)
            if container:
                owned = owned_image(raw, store, timeout=remaining())
                if not container_is_attached(owned, container):
                    scoped_diagnostics(raw, store, attached, listing)
                    raise RuntimeError('Selected APFS container is absent from this image attachment')
                event('selected', physical_store=store, container=container, attempt=attempt + 1)
                yield container
                return
            if attempt != 10:
                time.sleep(1)
        scoped_diagnostics(raw, store, attached, listing)
        raise RuntimeError('No APFS container found for the exact attached seed store after bounded wait')
    finally:
        # A missing/reassigned device must not turn cleanup into host-disk work.
        owned_image(raw, store)
        result = subprocess.run(['hdiutil', 'detach', '/dev/' + store, '-force'],
                                capture_output=True, timeout=30)
        require(result.returncode == 0, 'Owned APFS seed detach failed')
        event('detached', physical_store=store)
