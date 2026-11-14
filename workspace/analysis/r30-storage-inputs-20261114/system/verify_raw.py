#!/usr/bin/env python3
"""Decode pinned IPSW images with UniXNU's UDIF reader and inspect APFS UUIDs.

Produces stand-alone raw files, not a userdata layout or installation command.
Run on the existing Linux build VM with /opt/unixnu-linux/bin/lzfse available.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import uuid
import zlib

import prepare_inputs as inputs
from unixnu.udif import Image


def fletcher64(data):
    # Same APFS checksum used by the official make-macos-data-seed.py.
    lo = hi = 0
    for word, in struct.iter_unpack('<I', data):
        lo = (lo + word) % 0xffffffff
        hi = (hi + lo) % 0xffffffff
    c0 = 0xffffffff - ((lo + hi) % 0xffffffff)
    c1 = 0xffffffff - ((lo + c0) % 0xffffffff)
    return struct.pack('<II', c0, c1)


APFS_TYPE = '7C3457EF-0000-11AA-AA11-00306543ECAC'
# Independently inspected from official-decoded image headers, not from GPT GUIDs.
CONTAINER_UUIDS = {
    'OS': '50F8A67F-AFC1-4A1B-BCBB-82162674C67F',
    'Cryptex1,SystemOS': 'E886F82C-EE56-4B52-AF5F-E70BF46320C4',
}


class ApfsScanner:
    def __init__(self, container_offset=0, container_bytes=None, expected_uuid=None):
        self.header = None
        self.pending = b''
        self.offset = 0
        self.volumes = {}
        self.container_offset = container_offset
        self.container_bytes = container_bytes
        self.expected_uuid = expected_uuid

    def feed(self, data):
        data = self.pending + data
        end = len(data) // 4096 * 4096
        for at in range(0, end, 4096):
            if self.offset + at == 0:
                block = data[at:at + 4096]
                if block[32:36] != b'NXSB' or struct.unpack_from('<I', block, 36)[0] != 4096:
                    raise ValueError('expected a 4 KiB APFS container at selected partition start')
                if fletcher64(block[8:]) != block[:8]:
                    raise ValueError('APFS container superblock checksum failed')
                self.header = dict(block_size=4096, block_count=struct.unpack_from('<Q', block, 40)[0],
                                   container_uuid=str(uuid.UUID(bytes=block[72:88])).upper())
                if self.header['container_uuid'] == str(uuid.UUID(int=0)).upper():
                    raise ValueError('zero APFS container UUID')
                if self.expected_uuid and self.header['container_uuid'] != self.expected_uuid:
                    raise ValueError('APFS container UUID differs from independently inspected input')
            if data[at + 32:at + 36] == b'APSB':
                block = data[at:at + 4096]
                if fletcher64(block[8:]) != block[:8]:
                    continue
                identity = str(uuid.UUID(bytes=block[240:256])).upper()
                if identity == str(uuid.UUID(int=0)).upper():
                    raise ValueError('zero APFS volume UUID')
                self.volumes.setdefault(identity, []).append(self.container_offset + self.offset + at)
        self.offset += end
        self.pending = data[end:]

    def result(self):
        if self.pending or self.header is None or not self.volumes:
            raise ValueError('incomplete APFS stream or no checksummed volume superblock')
        if self.header['block_size'] * self.header['block_count'] != self.offset:
            raise ValueError('APFS container block count differs from partition length')
        if self.container_bytes is not None and self.offset != self.container_bytes:
            raise ValueError('APFS stream differs from selected partition length')
        return dict(**self.header, raw_bytes=self.offset, container_offset=self.container_offset,
                    container_bytes=self.offset,
                    volume_superblocks=[dict(uuid=k, offsets=v) for k, v in sorted(self.volumes.items())],
                    inspection='checksummed-container-and-volume-superblocks',
                    mounted=False, file_tree_verified=False)


def gpt_header(sector):
    """Validate the bounded GPT 1.0 / 128-byte-entry format of this Apple image."""
    if len(sector) != 512 or sector[:8] != b'EFI PART':
        raise ValueError('missing GPT header')
    revision, size, crc, reserved = struct.unpack_from('<IIII', sector, 8)
    if revision != 0x10000 or size != 92 or reserved or any(sector[size:]):
        raise ValueError('unsupported GPT header format or reserved bytes')
    checked = bytearray(sector[:size])
    checked[16:20] = bytes(4)
    if zlib.crc32(checked) != crc:
        raise ValueError('GPT header CRC mismatch')
    current, backup, first, last = struct.unpack_from('<QQQQ', sector, 24)
    disk_guid = str(uuid.UUID(bytes_le=sector[56:72])).upper()
    entries_lba, count, entry_size, array_crc = struct.unpack_from('<QIII', sector, 72)
    if disk_guid == str(uuid.UUID(int=0)).upper() or not 1 <= count <= 8192 or entry_size != 128:
        raise ValueError('unsupported GPT array geometry or zero disk GUID')
    return dict(current_lba=current, backup_lba=backup, first_usable_lba=first,
                last_usable_lba=last, disk_guid=disk_guid, entries_lba=entries_lba,
                entry_count=count, entry_size=entry_size, entries_crc32=array_crc,
                header_crc32=crc)


class RawScanner:
    """Stream bare APFS or one GPT-wrapped APFS partition; never mount it.

    APFS alignment is relative to its partition (the OS input starts at byte
    17408). GPT disk, partition, APFS container and APFS volume UUIDs are distinct.
    Accept either a validated protective MBR or an all-zero first sector.
    """
    def __init__(self, raw_bytes, expected_uuid=None):
        if raw_bytes < 4096 or raw_bytes % 512:
            raise ValueError('invalid raw image size')
        self.raw_bytes = raw_bytes
        self.expected_uuid = expected_uuid
        self.received = 0
        self.prefix = b''
        self.apfs = None
        self.primary = None
        self.table = None
        self.tail = b''
        self.layout = None

    def _select(self):
        if len(self.prefix) < 4096:
            return False
        if self.prefix[32:36] == b'NXSB':
            self.start, self.end = 0, self.raw_bytes
            self.layout = dict(kind='bare-apfs')
        else:
            first_sector = 'all-zero'
            mbr = self.prefix[:512]
            if any(mbr):
                first_sector = 'protective-mbr'
                start_lba, length = struct.unpack_from('<II', mbr, 454)
                if (mbr[510:512] != b'\x55\xaa' or any(mbr[:446]) or mbr[446] != 0
                        or mbr[450] != 0xee or any(mbr[462:510]) or start_lba != 1
                        or length != min(self.raw_bytes // 512 - 1, 0xffffffff)):
                    raise ValueError('invalid GPT protective MBR')
            primary = gpt_header(self.prefix[512:1024])
            final_lba = self.raw_bytes // 512 - 1
            array_bytes = primary['entry_count'] * primary['entry_size']
            array_sectors = (array_bytes + 511) // 512
            if (primary['current_lba'] != 1 or primary['backup_lba'] != final_lba
                    or primary['entries_lba'] != 2
                    or not 2 + array_sectors <= primary['first_usable_lba']
                    <= primary['last_usable_lba'] < final_lba - array_sectors):
                raise ValueError('GPT primary geometry or usable bounds invalid')
            table_end = 1024 + array_bytes
            if len(self.prefix) < table_end:
                return False
            table = self.prefix[1024:table_end]
            if zlib.crc32(table) != primary['entries_crc32']:
                raise ValueError('GPT primary array CRC mismatch')
            used = []
            for at in range(0, len(table), 128):
                entry = table[at:at + 128]
                if not any(entry[:16]):
                    if any(entry):
                        raise ValueError('nonzero unused GPT entry')
                    continue
                type_guid = str(uuid.UUID(bytes_le=entry[:16])).upper()
                part_guid = str(uuid.UUID(bytes_le=entry[16:32])).upper()
                first, last, attributes = struct.unpack_from('<QQQ', entry, 32)
                if (type_guid != APFS_TYPE or part_guid == str(uuid.UUID(int=0)).upper()
                        or not primary['first_usable_lba'] <= first <= last <= primary['last_usable_lba']):
                    raise ValueError('GPT partition type, GUID or bounds invalid')
                used.append(dict(type_guid=type_guid, partition_guid=part_guid,
                                 first_lba=first, last_lba=last, attributes=attributes,
                                 name=entry[56:128].decode('utf-16-le').rstrip('\0')))
            if len(used) != 1:
                raise ValueError('expected exactly one used APFS partition')
            partition = used[0]
            self.start = partition['first_lba'] * 512
            self.end = (partition['last_lba'] + 1) * 512
            self.primary, self.table = primary, table
            self.tail_start = (final_lba - array_sectors) * 512
            self.layout = dict(kind='gpt-apfs', sector_size=512, first_sector=first_sector,
                               primary=primary, partition=partition,
                               primary_header_crc_verified=True, primary_array_crc_verified=True)
        if (self.end - self.start) % 4096:
            raise ValueError('APFS partition length is not a multiple of its block size')
        self.apfs = ApfsScanner(self.start, self.end - self.start, self.expected_uuid)
        return True

    def _consume(self, data, at):
        stop = at + len(data)
        begin, end = max(at, self.start), min(stop, self.end)
        if begin < end:
            self.apfs.feed(data[begin - at:end - at])
        if self.primary and stop > self.tail_start:
            self.tail += data[max(at, self.tail_start) - at:]

    def feed(self, data):
        at = self.received
        if at + len(data) > self.raw_bytes:
            raise ValueError('raw image has trailing bytes')
        self.received += len(data)
        if self.apfs is None:
            self.prefix += data
            if self._select():
                self._consume(self.prefix, 0)
                self.prefix = b''
        else:
            self._consume(data, at)

    def result(self):
        if self.received != self.raw_bytes or self.apfs is None:
            raise ValueError('incomplete raw disk stream')
        if self.primary:
            if len(self.tail) != self.raw_bytes - self.tail_start:
                raise ValueError('incomplete GPT backup')
            backup = gpt_header(self.tail[-512:])
            expected = dict(self.primary, current_lba=self.primary['backup_lba'], backup_lba=1,
                            entries_lba=self.tail_start // 512)
            if any(backup[key] != value for key, value in expected.items() if key != 'header_crc32'):
                raise ValueError('GPT backup metadata differs from primary')
            backup_table = self.tail[:len(self.table)]
            if zlib.crc32(backup_table) != backup['entries_crc32'] or backup_table != self.table:
                raise ValueError('GPT backup array CRC or bytes differ from primary')
            self.layout.update(backup=backup, backup_header_crc_verified=True,
                               backup_array_crc_verified=True, arrays_identical=True)
        apfs = self.apfs.result()
        return dict(apfs, raw_bytes=self.raw_bytes, layout=self.layout)


def new_temporary(destination):
    """Retain failed decodes; never truncate an existing partial or final image."""
    for index in range(10000):
        suffix = '.partial' if index == 0 else f'.partial-{index:04d}'
        path = destination.with_name(destination.name + suffix)
        try:
            return path, path.open('xb')
        except FileExistsError:
            continue
    raise ValueError('too many retained partial images')


def raw_chunks(path):
    with path.open('rb') as stream:
        while chunk := stream.read(8 << 20):
            yield chunk


def prepare(component):
    meta = inputs.metadata()
    row, = [r for r in meta['members'] if r['component'] == component]
    source = inputs.ARTIFACT / 'ipsw/21H461' / Path(row['name']).name
    if source.stat().st_size != row['size'] or inputs.sha(source) != row['sha256']:
        raise ValueError('source image differs from OS profile pin')
    image = Image(source)
    label = 'OS' if component == 'OS' else 'SystemOS'
    out = inputs.ARTIFACT / 'raw'
    out.mkdir(parents=True, exist_ok=True)
    destination = out / f'{label}.raw' if image.chunks else source
    report_path = out / f'{label}.json'
    existing = destination.is_file()
    if image.chunks and existing:
        if not report_path.exists():
            raise ValueError('existing raw image has no source identity record; refusing to overwrite')
        previous = json.loads(report_path.read_text())
        if previous['source_sha256'] != row['sha256'] or inputs.sha(destination) != previous['raw_sha256']:
            raise ValueError('existing raw image/source pin differs from prior record')
    scanner = RawScanner(image.size, CONTAINER_UUIDS[component])
    digest = hashlib.sha256()
    temporary, output = new_temporary(destination) if not existing else (None, None)
    try:
        blocks = raw_chunks(destination) if existing else image.blocks()
        for data in blocks:
            scanner.feed(data)
            digest.update(data)
            if output:
                output.write(data)
    finally:
        if output:
            output.close()
    apfs = scanner.result()
    if apfs['raw_bytes'] != image.size:
        raise ValueError('decoded image size mismatch')
    identities = {v['uuid'] for v in apfs['volume_superblocks']}
    expected = meta['expected_root_volume_uuid'] if component == 'OS' else None
    if expected and identities != {expected}:
        raise ValueError(f'OS volume UUIDs {identities} differ from profile root UUID {expected}')
    if not existing:
        if destination.exists():
            raise ValueError('raw destination appeared during decode; preserving partial')
        temporary.replace(destination)
    report = dict(component=component, source=str(source), source_sha256=row['sha256'],
                  raw_path=str(destination), raw_bytes=image.size, raw_sha256=digest.hexdigest(),
                  reused_raw_source=image.chunks is None, apfs=apfs,
                  root_uuid=expected, root_uuid_matches_profile=(expected in identities if expected else None),
                  userdata_geometry_selected=False, installable_layout=False,
                  decoder_sha256=inputs.sha(inputs.CORE / 'scripts/unixnu/udif.py'),
                  inspected_utc=__import__('datetime').datetime.now(__import__('datetime').timezone.utc).isoformat())
    inputs.record(report_path, report)
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', choices=['OS', 'Cryptex1,SystemOS', 'both'], default='both')
    args = parser.parse_args()
    names = ['OS', 'Cryptex1,SystemOS'] if args.component == 'both' else [args.component]
    results = [prepare(name) for name in names]
    inputs.record(inputs.HERE / 'raw-ready.json', dict(images=results, hardware_tested=False))
    print(json.dumps(results, indent=2))
