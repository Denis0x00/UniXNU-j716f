#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Host unit tests for scripts/native_ios17.py (no Apple binaries required)."""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPTS_DIR = Path(__file__).resolve().parents[1]
if str(SCRIPTS_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPTS_DIR))

import native_ios17 as n


class NativeIos17Tests(unittest.TestCase):
    def test_branch_and_cbz_encodings(self) -> None:
        self.assertEqual(n.branch(0x100003000, 0x100003010), 0x14000004)
        self.assertEqual(n.branch(0x100003000, 0x100003010, link=True), 0x94000004)
        self.assertEqual(n.branch(0x100003010, 0x100003000), 0x17fffffc)
        self.assertEqual(n.cbnz(0x100003000, 0, 0x100003008), 0x35000040)
        self.assertEqual(n.cbz(0x100003000, 8, 0x100003020), 0x34000108)
        with self.assertRaises(ValueError):
            n.relative(0x1000, 0x1002, 26)

    def test_adrp_add_memory_mov(self) -> None:
        self.assertEqual(n.adrp(0x100003000, 8, 0x100947f00), 0x90004a28)
        self.assertEqual(n.add(8, 8, 0xf00), 0x913c0108)
        self.assertEqual(n.memory(0xf9400000, 16, 16, 0xf08, 8), 0xf9478610)
        self.assertEqual(n.memory(0xb9400000, 0, 8, 0xf1c, 4), 0xb94f1d00)
        self.assertEqual(n.memory(0x39400000, 0, 8, 0xf10, 1), 0x397c4100)
        self.assertEqual(n.mov_w(1, 0), 0x52800001)
        self.assertEqual(n.mov_w(1, 15), 0x528001e1)
        with self.assertRaises(ValueError):
            n.memory(0xf9400000, 0, 8, 4, 8)

    def test_decode_signed_imm19(self) -> None:
        # Forward b.ne +0x20
        word_fwd = 0x54000001 | (8 << 5)
        self.assertEqual(n.decode_signed_imm19_target(0x10062e0ac, word_fwd), 0x10062e0cc)
        # Backward cbz w0, -0x10
        imm = (-4) & 0x7ffff
        word_bwd = 0x34000000 | (imm << 5)
        self.assertEqual(n.decode_signed_imm19_target(0x10062e114, word_bwd), 0x10062e104)

    def test_layout_header_abi_constants(self) -> None:
        hdr = n.layout_header()
        self.assertIn('#ifndef QBT_IOS17_LAYOUT_H', hdr)
        self.assertIn(
            '#define QBT_NATIVE_UUID {0x0f, 0x83, 0x63, 0x81, 0xd0, 0x7b, 0x36, 0xe4, '
            '0x9d, 0xbb, 0x8a, 0x44, 0x78, 0x2b, 0x9c, 0x14}',
            hdr,
        )
        self.assertIn('#define QBT_ADV_CAPACITY 0x5ULL', hdr)
        self.assertIn('#define QBT_ADV_MODE 0x88ULL', hdr)
        self.assertIn('#define QBT_DESCRIPTOR_COUNT 0x109ULL', hdr)
        self.assertIn('#define QBT_STATE 0x100947f00ULL', hdr)
        self.assertIn('offsetof(struct qbt_native_state, adv_sets) == 28', hdr)
        self.assertIn('offsetof(struct qbt_native_state, status) == 64', hdr)
        self.assertIn('offsetof(struct qbt_native_state, parameters) == 72', hdr)
        self.assertEqual(len(n.MODEL_CAPABILITIES), 11)
        self.assertEqual(len(n.CAP_OFFSETS), 11)
        # Ensure NATIVE_STATE + 128 stays within a single 4 KiB page.
        self.assertLessEqual((n.NATIVE_STATE & 0xfff) + n.NATIVE_STATE_BYTES, 0x1000)

    def test_rejects_non_macho_and_wrong_sha(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / 'bad'
            p.write_bytes(b'not-a-macho' * 8)
            with self.assertRaisesRegex(ValueError, 'Mach-O'):
                n.NativeImage(p)

            # Minimal thin Mach-O header with __TEXT segment fails locked SHA-256 check.
            seg = struct.pack(
                '<II16sQQQQIIII',
                0x19, 72, b'__TEXT'.ljust(16, b'\0'),
                0x100000000, 0x4000, 0, 0x4000, 5, 5, 0, 0,
            )
            hdr = struct.pack('<8I', 0xfeedfacf, 0x100000c, 0, 2, 1, len(seg), 0, 0)
            p.write_bytes((hdr + seg).ljust(0x4000, b'\0'))
            with self.assertRaisesRegex(ValueError, 'SHA-256'):
                n.NativeImage(p)


if __name__ == '__main__':
    unittest.main()
