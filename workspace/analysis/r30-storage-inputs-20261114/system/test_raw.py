#!/usr/bin/env python3
"""Host-only tests of raw layout validation; no decompressor replacement."""
import hashlib
from pathlib import Path
import struct
import tempfile
import unittest
from unittest import mock
import uuid
import zlib

import verify_raw as raw

CONTAINER = raw.CONTAINER_UUIDS['OS']
VOLUME = '04769D26-2CA0-4825-B443-E2E5F88300F2'
DISK = 'FB0485A6-9C61-40C4-886F-88AA1AEB5810'
PARTITION = '9232A33A-5F8A-4A09-9A43-E7D8BCFCF4BC'


def checksum(block):
    block[:8] = raw.fletcher64(block[8:])
    return block


def container():
    data = bytearray(8 * 4096)
    nx = bytearray(4096)
    nx[32:36] = b'NXSB'
    struct.pack_into('<IQ', nx, 36, 4096, 8)
    nx[72:88] = uuid.UUID(CONTAINER).bytes
    data[:4096] = checksum(nx)
    volume = bytearray(4096)
    volume[32:36] = b'APSB'
    volume[240:256] = uuid.UUID(VOLUME).bytes
    data[4096:8192] = checksum(volume)
    return data


def header(current, backup, table_lba, table_crc):
    result = bytearray(512)
    result[:8] = b'EFI PART'
    struct.pack_into('<IIIIQQQQ', result, 8, 0x10000, 92, 0, 0, current, backup, 34, 97)
    result[56:72] = uuid.UUID(DISK).bytes_le
    struct.pack_into('<QIII', result, 72, table_lba, 128, 128, table_crc)
    fix_header(result)
    return result


def fix_header(sector):
    sector[16:20] = bytes(4)
    struct.pack_into('<I', sector, 16, zlib.crc32(sector[:92]))


def disk():
    # LBA 34 is deliberately not globally aligned to a 4096-byte APFS block.
    table = bytearray(16384)
    table[:16] = uuid.UUID(raw.APFS_TYPE).bytes_le
    table[16:32] = uuid.UUID(PARTITION).bytes_le
    struct.pack_into('<QQQ', table, 32, 34, 97, 0)
    crc = zlib.crc32(table)
    return bytearray(bytes(512) + header(1, 130, 2, crc) + table
                     + container() + table + header(130, 1, 98, crc))


def edit_header(data, lba, offset, fmt, value):
    sector = data[lba * 512:(lba + 1) * 512]
    struct.pack_into(fmt, sector, offset, value)
    fix_header(sector)
    data[lba * 512:(lba + 1) * 512] = sector


def fix_tables(data):
    table = data[1024:17408]
    data[98 * 512:130 * 512] = table
    crc = zlib.crc32(table)
    for lba in (1, 130):
        edit_header(data, lba, 88, '<I', crc)


def scan(data, size=None, step=997, expected_uuid=CONTAINER):
    scanner = raw.RawScanner(len(data) if size is None else size, expected_uuid)
    for at in range(0, len(data), step):
        scanner.feed(data[at:at + step])
    return scanner.result()


class RawTests(unittest.TestCase):
    def test_bare_apfs(self):
        report = scan(container())
        self.assertEqual(report['layout']['kind'], 'bare-apfs')
        self.assertEqual(report['container_offset'], 0)
        self.assertEqual(report['volume_superblocks'], [dict(uuid=VOLUME, offsets=[4096])])
        self.assertFalse(report['mounted'])
        self.assertFalse(report['file_tree_verified'])

    def test_gpt_partition_relative_blocks(self):
        for step in (1, 511, 997, 16384, 8 << 20):
            with self.subTest(step=step):
                report = scan(disk(), step=step)
                self.assertEqual(report['container_offset'], 17408)
                self.assertEqual(report['container_bytes'], 32768)
                self.assertEqual(report['raw_bytes'], 131 * 512)
                self.assertEqual(report['volume_superblocks'], [dict(uuid=VOLUME, offsets=[21504])])
                self.assertTrue(report['layout']['arrays_identical'])
                self.assertEqual(report['container_uuid'], CONTAINER)
                self.assertEqual(report['layout']['primary']['disk_guid'], DISK)
                self.assertEqual(report['layout']['partition']['partition_guid'], PARTITION)

    def test_header_crc_primary_and_backup(self):
        for lba in (1, 130):
            with self.subTest(lba=lba):
                data = disk()
                data[lba * 512 + 56] ^= 1
                with self.assertRaisesRegex(ValueError, 'header CRC'):
                    scan(data)

    def test_array_crc_primary_and_backup(self):
        for lba in (2, 98):
            with self.subTest(lba=lba):
                data = disk()
                data[lba * 512 + 56] ^= 1
                with self.assertRaisesRegex(ValueError, 'array CRC'):
                    scan(data)

    def test_primary_geometry(self):
        cases = [(24, 2), (32, 129), (40, 33), (48, 98), (72, 3)]
        for offset, value in cases:
            with self.subTest(offset=offset):
                data = disk()
                edit_header(data, 1, offset, '<Q', value)
                with self.assertRaisesRegex(ValueError, 'geometry'):
                    scan(data)

    def test_backup_reciprocity_and_metadata(self):
        for offset, value in ((24, 129), (32, 2), (40, 35), (48, 96), (72, 97)):
            with self.subTest(offset=offset):
                data = disk()
                edit_header(data, 130, offset, '<Q', value)
                with self.assertRaisesRegex(ValueError, 'backup metadata'):
                    scan(data)

    def test_bounded_array_geometry(self):
        for offset, value in ((80, 0), (80, 8193), (84, 127), (84, 256)):
            with self.subTest(offset=offset, value=value):
                data = disk()
                edit_header(data, 1, offset, '<I', value)
                with self.assertRaisesRegex(ValueError, 'array geometry'):
                    scan(data)

    def test_gpt_header_format(self):
        for offset, value in ((8, 0x20000), (12, 96), (20, 1)):
            data = disk()
            edit_header(data, 1, offset, '<I', value)
            with self.assertRaisesRegex(ValueError, 'header format'):
                scan(data)

    def test_partition_bounds(self):
        for offset, value in ((32, 33), (32, 98), (40, 98), (40, 33)):
            data = disk()
            struct.pack_into('<Q', data, 1024 + offset, value)
            fix_tables(data)
            with self.assertRaisesRegex(ValueError, 'partition type, GUID or bounds'):
                scan(data)

    def test_multiple_apfs_partitions(self):
        data = disk()
        data[1152:1280] = data[1024:1152]
        fix_tables(data)
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            scan(data)

    def test_non_apfs_partition(self):
        data = disk()
        data[1024:1040] = uuid.UUID(DISK).bytes_le
        fix_tables(data)
        with self.assertRaisesRegex(ValueError, 'partition type'):
            scan(data)

    def test_no_partition(self):
        data = disk()
        data[1024:1152] = bytes(128)
        fix_tables(data)
        with self.assertRaisesRegex(ValueError, 'exactly one'):
            scan(data)

    def test_unused_entry_nonzero(self):
        data = disk()
        data[1152 + 16] = 1
        fix_tables(data)
        with self.assertRaisesRegex(ValueError, 'unused GPT'):
            scan(data)

    def test_zero_partition_guid(self):
        data = disk()
        data[1040:1056] = bytes(16)
        fix_tables(data)
        with self.assertRaisesRegex(ValueError, 'GUID'):
            scan(data)

    def test_zero_disk_guid(self):
        data = disk()
        sector = data[512:1024]
        sector[56:72] = bytes(16)
        fix_header(sector)
        data[512:1024] = sector
        with self.assertRaisesRegex(ValueError, 'disk GUID'):
            scan(data)

    def test_first_sector_policy(self):
        data = disk()
        data[510:512] = b'\x55\xaa'
        with self.assertRaisesRegex(ValueError, 'protective MBR'):
            scan(data)

    def test_protective_mbr(self):
        data = disk()
        data[450] = 0xee
        struct.pack_into('<II', data, 454, 1, 130)
        data[510:512] = b'\x55\xaa'
        self.assertEqual(scan(data)['layout']['first_sector'], 'protective-mbr')
        for offset, value in ((446, 0x80), (450, 7), (454, 2), (458, 129), (462, 1)):
            with self.subTest(offset=offset):
                bad = bytearray(data)
                bad[offset] = value
                with self.assertRaisesRegex(ValueError, 'protective MBR'):
                    scan(bad)

    def test_nx_checksum(self):
        data = disk()
        data[17408 + 72] ^= 1
        with self.assertRaisesRegex(ValueError, 'container superblock checksum'):
            scan(data)

    def test_container_uuid(self):
        with self.assertRaisesRegex(ValueError, 'container UUID'):
            scan(disk(), expected_uuid=DISK)

    def test_container_size(self):
        data = disk()
        nx = data[17408:21504]
        struct.pack_into('<Q', nx, 40, 7)
        data[17408:21504] = checksum(nx)
        with self.assertRaisesRegex(ValueError, 'block count'):
            scan(data)

    def test_no_valid_volume(self):
        data = disk()
        data[21504 + 240] ^= 1
        with self.assertRaisesRegex(ValueError, 'no checksummed volume'):
            scan(data)

    def test_truncated_and_trailing_stream(self):
        data = disk()
        with self.assertRaisesRegex(ValueError, 'incomplete raw'):
            scan(data[:-1], size=len(data))
        with self.assertRaisesRegex(ValueError, 'trailing bytes'):
            scan(data + b'0', size=len(data))

    def test_fresh_partial_retains_failed_file(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / 'OS.raw'
            prior = target.with_name('OS.raw.partial')
            prior.write_bytes(b'failed')
            path, handle = raw.new_temporary(target)
            with handle:
                handle.write(b'new')
            self.assertEqual(prior.read_bytes(), b'failed')
            self.assertEqual(path.name, 'OS.raw.partial-0001')
            self.assertFalse(target.exists())

    def test_bad_backup_never_promotes_partial(self):
        with tempfile.TemporaryDirectory() as directory:
            artifact = Path(directory)
            source = artifact / 'ipsw/21H461/OS.dmg'
            source.parent.mkdir(parents=True)
            source.write_bytes(b'pinned compressed input')
            out = artifact / 'raw'
            out.mkdir()
            (out / 'OS.raw.partial').write_bytes(b'previous failed decode')
            data = disk()
            data[-512 + 56] ^= 1
            fake = mock.Mock(chunks=[object()], size=len(data))
            fake.blocks.return_value = iter([bytes(data)])
            metadata = dict(members=[dict(component='OS', name='OS.dmg', size=source.stat().st_size,
                                          sha256=hashlib.sha256(source.read_bytes()).hexdigest())],
                            expected_root_volume_uuid=VOLUME)
            with mock.patch.object(raw.inputs, 'ARTIFACT', artifact), \
                    mock.patch.object(raw.inputs, 'metadata', return_value=metadata), \
                    mock.patch.object(raw, 'Image', return_value=fake):
                with self.assertRaisesRegex(ValueError, 'header CRC'):
                    raw.prepare('OS')
            self.assertFalse((out / 'OS.raw').exists())
            self.assertFalse((out / 'OS.json').exists())
            self.assertEqual((out / 'OS.raw.partial').read_bytes(), b'previous failed decode')
            self.assertEqual((out / 'OS.raw.partial-0001').read_bytes(), data)

    def test_actual_official_decoded_head_tail(self):
        # This checks actual GPT metadata only, not the missing middle/APFS tree.
        here = Path(__file__).resolve().parent
        paths = [here / 'OS-first-1MiB.bin', here / 'OS-last-33sectors.bin']
        if not all(path.exists() for path in paths):
            self.skipTest('official decoded head/tail not copied to this host')
        head, tail = [path.read_bytes() for path in paths]
        scanner = raw.RawScanner(6077580800, CONTAINER)
        scanner.feed(head)
        self.assertEqual(scanner.start, 17408)
        self.assertEqual(scanner.end - scanner.start, 6077546496)
        self.assertEqual(scanner.primary['disk_guid'], DISK)
        self.assertEqual(scanner.layout['first_sector'], 'protective-mbr')
        self.assertEqual(scanner.apfs.header['container_uuid'], CONTAINER)
        self.assertEqual(scanner.apfs.header['block_count'], 1483776)
        backup = raw.gpt_header(tail[-512:])
        self.assertEqual(backup['current_lba'], 11870274)
        self.assertEqual(backup['backup_lba'], 1)
        self.assertEqual(backup['entries_lba'], 11870242)
        self.assertEqual(tail[:-512], scanner.table)
        self.assertEqual(zlib.crc32(tail[:-512]), backup['entries_crc32'])
        with self.assertRaisesRegex(ValueError, 'incomplete raw'):
            scanner.result()


if __name__ == '__main__':
    unittest.main(verbosity=2)
