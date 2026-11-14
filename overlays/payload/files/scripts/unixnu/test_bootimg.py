#!/usr/bin/env python3
"""Offline Android boot-image layout tests. All images/FDs/shims are NOT BOOTABLE.

    python3 scripts/unixnu/test_bootimg.py

Set UNIXNU_TEST_MKBOOTIMG to a real Mu-Silicium mkbootimg.py to additionally
exercise build() through that tool. Only BootShim compilation is substituted;
the resulting synthetic images must never be used on a device.
"""
import gzip
import json
import os
from pathlib import Path
import shutil
import struct
import sys
from tempfile import TemporaryDirectory
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from unixnu import bootimg

PAGE = 4096
# Minimal FDT: reservation terminator followed by an empty root node.
TREE = (struct.pack('>10I', 0xd00dfeed, 72, 56, 72, 40, 17, 16, 0, 0, 16)
        + bytes(16) + struct.pack('>4I', 1, 0, 2, 9))
SHIM = b'NOT BOOTABLE: synthetic BootShim\n'
FIRMWARE = b'NOT BOOTABLE: synthetic firmware\n' * 257
MKBOOTIMG = os.environ.get('UNIXNU_TEST_MKBOOTIMG')


def padded(data):
    return data + bytes((-len(data)) % PAGE)


def fixture_image(image, kernel, tree, ramdisk):
    """Encode the documented v0/v2 fields independently of the production writer."""
    header = bytearray(PAGE)
    version = image['header_version']
    struct.pack_into('<8s10I', header, 0, b'ANDROID!', len(kernel),
                     image['base'] + image['kernel_offset'], len(ramdisk),
                     image['base'] + 0x1000000 if ramdisk else 0,
                     0, 0, image['base'] + image['tags_offset'], PAGE, version, 0)
    data = bytes(header) + padded(kernel) + padded(ramdisk)
    if version == 2:
        struct.pack_into('<IQIIQ', header, 1632, 0, 0, 1660, len(tree),
                         image['base'] + image['dtb_offset'])
        data = bytes(header) + data[PAGE:] + padded(tree)
    return data


class BootImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = TemporaryDirectory(prefix='unixnu-NOT-BOOTABLE-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.output = self.root / 'NOT-BOOTABLE.img'
        self.ramdisk_path = self.root / 'ramdisk'
        self.image = {'header_version': 2, 'pagesize': PAGE, 'base': 0x80000000,
                      'kernel_offset': 0x8000, 'tags_offset': 0x1e00000,
                      'dtb_offset': 0x100300000}

    def fixture(self, *, version=2, compression='gzip', ramdisk=b'R' * (PAGE + 17)):
        image = dict(self.image, header_version=version)
        combined = SHIM + FIRMWARE
        kernel = (gzip.compress(combined, mtime=0) if compression == 'gzip' else combined) + TREE
        self.output.write_bytes(fixture_image(image, kernel, TREE, ramdisk))
        self.ramdisk_path.write_bytes(ramdisk)
        return image, PAGE + len(padded(kernel)) + len(padded(ramdisk))

    def verify(self, image, compression='gzip'):
        return bootimg.verify(self.output, SHIM + FIRMWARE, TREE, self.ramdisk_path, image, compression)

    def test_v2_round_trips_both_dtb_copies_and_64_bit_load_address(self):
        for compression in ('gzip', 'none'):
            for ramdisk in (b'', b'R' * (PAGE + 17)):
                with self.subTest(compression=compression, ramdisk=len(ramdisk)):
                    image, dtb_start = self.fixture(compression=compression, ramdisk=ramdisk)
                    report = self.verify(image, compression)
                    self.assertEqual(report['dtb_bytes'], len(TREE))
                    self.assertEqual(report['dtb_address'], '0x180300000')
                    self.assertEqual(report['dtb_file_offset'], dtb_start)

    def test_v2_rejects_corrupted_independent_dtb_with_intact_kernel_suffix(self):
        image, dtb_start = self.fixture()
        data = bytearray(self.output.read_bytes())
        data[dtb_start + len(TREE) - 1] ^= 1
        self.output.write_bytes(data)
        with self.assertRaisesRegex(bootimg.Error, 'device tree bytes differ'):
            self.verify(image)

    def test_v2_rejects_wrong_dtb_size_address_or_extension_layout(self):
        mutations = [(1648, '<I', 0), (1648, '<I', len(TREE) - 1),
                     (1648, '<I', len(TREE) + 1), (1652, '<Q', 0x81f00000),
                     (1644, '<I', 1648), (1632, '<I', 1), (1636, '<Q', PAGE)]
        for offset, encoding, value in mutations:
            with self.subTest(offset=offset, value=value):
                image, _ = self.fixture()
                data = bytearray(self.output.read_bytes())
                struct.pack_into(encoding, data, offset, value)
                self.output.write_bytes(data)
                with self.assertRaises(bootimg.Error):
                    self.verify(image)

    def test_v2_rejects_truncated_header_dtb_and_final_padding(self):
        image, dtb_start = self.fixture()
        data = self.output.read_bytes()
        for end in (0, 8, 43, 1648, 1659, dtb_start, dtb_start + len(TREE) - 1, len(data) - 1):
            with self.subTest(end=end):
                self.output.write_bytes(data[:end])
                with self.assertRaises(bootimg.Error):
                    self.verify(image)

    def test_v0_keeps_only_the_appended_dtb_for_both_compression_modes(self):
        for compression in ('gzip', 'none'):
            with self.subTest(compression=compression):
                image, end = self.fixture(version=0, compression=compression)
                report = self.verify(image, compression)
                self.assertEqual(report['header_version'], 0)
                self.assertNotIn('dtb_address', report)
                self.assertEqual(self.output.stat().st_size, end)

    def test_appended_dtb_is_still_checked_in_v0_and_v2(self):
        for version in (0, 2):
            with self.subTest(version=version):
                image, _ = self.fixture(version=version)
                data = bytearray(self.output.read_bytes())
                kernel_size = struct.unpack_from('<I', data, 8)[0]
                data[PAGE + kernel_size - 1] ^= 1
                self.output.write_bytes(data)
                with self.assertRaisesRegex(bootimg.Error, 'appended device tree'):
                    self.verify(image)

    def prepare_build(self, version):
        uefi = self.root / 'synthetic-uefi'
        scripts, configs = uefi / 'Resources/Scripts', uefi / 'Resources/Configs'
        scripts.mkdir(parents=True)
        configs.mkdir()
        packer = scripts / 'mkbootimg.py'
        packer.write_text('# Synthetic unit-test placeholder; never executed.\n')
        (configs / 'fixture.toml').write_text(
            f'[uefi_fd]\nbase = 268435456\nsize = {len(FIRMWARE)}\n', encoding='utf-8')
        fd, dtb = self.root / 'NOT-BOOTABLE.fd', self.root / 'fixture.dtb'
        fd.write_bytes(FIRMWARE)
        dtb.write_bytes(TREE)
        self.ramdisk_path.write_bytes(b'R' * (PAGE + 17))
        image = dict(self.image, header_version=version)
        ctx = SimpleNamespace(work=self.root / 'out', board={
            'firmware': {'config': 'fixture'}, 'bootimg': {'image': image}})
        return ctx, uefi, fd, dtb, packer

    def test_build_passes_patched_dtb_and_configured_offset_explicitly(self):
        ctx, uefi, fd, dtb, _ = self.prepare_build(2)
        # Change the valid FDT boot_cpuid_phys field to distinguish patched input.
        patched_tree = bytearray(TREE)
        struct.pack_into('>I', patched_tree, 28, 1)
        patched_tree = bytes(patched_tree)

        def pack(command, **kwargs):
            args = dict(zip(command[2::2], command[3::2]))
            self.assertEqual(Path(args['--dtb']).read_bytes(), patched_tree)
            self.assertEqual(int(args['--dtb_offset']), 0x100300000)
            kernel = Path(args['--kernel']).read_bytes()
            self.assertTrue(kernel.endswith(patched_tree))
            Path(args['-o']).write_bytes(fixture_image(
                ctx.board['bootimg']['image'], kernel, patched_tree, self.ramdisk_path.read_bytes()))

        with patch.object(bootimg, 'compile_shim', return_value=SHIM), \
                patch.object(bootimg, 'patch_dtb', return_value=patched_tree), \
                patch.object(bootimg, 'run', side_effect=pack):
            output = bootimg.build(ctx, uefi, fd, dtb, self.ramdisk_path)
        report = json.loads(output.with_name('boot.json').read_text())
        self.assertEqual(report['dtb_address'], '0x180300000')
        self.assertFalse(report['hardware_tested'])

    @unittest.skipUnless(MKBOOTIMG, 'set UNIXNU_TEST_MKBOOTIMG for the real-tool integration test')
    def test_real_mkbootimg_v0_v2_default_and_custom_dtb_offsets(self):
        # Run each case in its own directory so generated files never escape the fixture.
        real_run = bootimg.run
        for version, offset in ((0, None), (2, None), (2, 0x100300000)):
            with self.subTest(version=version, dtb_offset=offset), TemporaryDirectory(
                    prefix='unixnu-NOT-BOOTABLE-packer-') as directory:
                self.root = Path(directory)
                self.ramdisk_path = self.root / 'ramdisk'
                ctx, uefi, fd, dtb, packer = self.prepare_build(version)
                shutil.copyfile(MKBOOTIMG, packer)
                if offset is None:
                    del ctx.board['bootimg']['image']['dtb_offset']
                calls = []

                def run_packer(command, **kwargs):
                    calls.append([str(part) for part in command])
                    return real_run([sys.executable, *command[1:]], **kwargs)

                with patch.object(bootimg, 'compile_shim', return_value=SHIM), \
                        patch.object(bootimg, 'run', side_effect=run_packer):
                    output = bootimg.build(ctx, uefi, fd, dtb, self.ramdisk_path)
                data = output.read_bytes()
                report = json.loads(output.with_name('boot.json').read_text())
                self.assertEqual('--dtb' in calls[0], version == 2)
                self.assertFalse(report['hardware_tested'])
                if version == 2:
                    address = 0x80000000 + (0x1f00000 if offset is None else offset)
                    self.assertEqual(struct.unpack_from('<IQ', data, 1648), (len(TREE), address))
                    start = report['dtb_file_offset']
                    self.assertEqual(data[start:start + len(TREE)], TREE)
                else:
                    self.assertNotIn('dtb_address', report)


if __name__ == '__main__':
    unittest.main()
