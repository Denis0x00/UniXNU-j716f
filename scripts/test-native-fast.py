#!/usr/bin/env python3
"""Host unit tests for J716F NATIVE FAST-V1..V4 (NativeFast.h, NativeMapFixed.h, NativeVmMapFixed.h, and 8-QR decoder)."""
from __future__ import annotations

import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))

import decode_fast_session as dec  # noqa: E402


class NativeFastTest(unittest.TestCase):
    def test_fast_c_suite_when_cc_available(self) -> None:
        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if not cc:
            return
        with tempfile.TemporaryDirectory(prefix="j716f-fast-test-") as tmp:
            work = Path(tmp)
            for p in (REPO / "package/native-log").iterdir():
                if p.is_file():
                    shutil.copyfile(p, work / p.name)
            for p in (REPO / "package/native-fast").iterdir():
                if p.is_file():
                    shutil.copyfile(p, work / p.name)
            out = work / ("FastTests.exe" if sys.platform == "win32" else "FastTests")
            subprocess.run(
                [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", str(work / "FastTests.c"), "-o", str(out)],
                check=True,
            )
            subprocess.run([str(out)], check=True)

    def test_decode_fast_session_roundtrip_and_v4_slots(self) -> None:
        raw = bytearray(dec.JL_SESSION_BYTES)
        cand = (
            0xCF8D38D551EE191C,
            0xA561F3DC41FDAEBB,
            0x6F9F888DD0E3CE4C,
            0x92D8089AF6B64B0E,
        )
        session_id = 0x000000004A9650EC
        struct.pack_into("<QII", raw, 0, dec.JL_MAGIC, 1, dec.JL_SESSION_BYTES)
        struct.pack_into("<4Q", raw, 16, *cand)
        struct.pack_into("<QIII", raw, 48, session_id, 16, 0, dec.JL_USED)
        hdr_crc = dec.jl_crc(bytes(raw[:60]))
        struct.pack_into("<I", raw, 60, hdr_crc)

        def write_slot(slot: int, counter: int, d0: int = 0, d1: int = 0, d2: int = 0) -> None:
            off = 128 + slot * 128
            struct.pack_into("<IIQQQ", raw, off, dec.JL_RECORD, slot, session_id, slot + 1, counter)
            data = [d0, d1, d2, 0x3454593D, 0xD4229000, 0xFFFFFFF007ABBF00, 0x124F800, 0x80000000, 0xFFFFFFF007AAC000, 0x3C0]
            struct.pack_into("<10Q", raw, off + 32, *data)
            rcrc = dec.jl_crc(bytes(raw[off : off + 112]))
            struct.pack_into("<IIQ", raw, off + 112, rcrc, dec.JL_COMMIT, 0)

        for s in range(8):
            write_slot(s, 0x20800000 + s * 0x1000)
        # FAST-V4 kdbg_1st (slot 39) & KDBG_LAST (slot 47) & PANIC_STR (slot 46)
        kdbg_w0, kdbg_w1 = struct.unpack("<QQ", b"kernel_bootstrap")
        write_slot(39, 0x20810000, 0xFFFFFFF0072C8088, kdbg_w0, kdbg_w1)
        write_slot(47, 0x20820000, 0xFFFFFFF0079C246C, *struct.unpack("<QQ", b"PE_init_iokit\x00\x00\x00"))
        write_slot(46, 0x20830000, *struct.unpack("<QQ", b"assertion failed"), 0x1234)

        cand_hex = b"".join(struct.pack("<Q", w) for w in cand).hex().upper()
        sess_hex = f"{session_id:016X}"
        sha_hex = hashlib.sha256(raw).hexdigest().upper()
        scrc_hex = f"{dec.jl_crc(bytes(raw)):08X}"
        frames = []
        for idx in range(dec.JLF_FRAMES):
            chunk_hex = bytes(raw[idx * 1024 : (idx + 1) * 1024]).hex().upper()
            body = f"J716FS1/{cand_hex}/{sess_hex}/{sha_hex}/{scrc_hex}/{idx:02X}/08/{chunk_hex}/"
            fcrc = f"{dec.jl_crc(body.encode('ascii')):08X}"
            frames.append(body + fcrc)

        reassembled = dec.reassemble_frames(frames)
        self.assertEqual(reassembled, bytes(raw))
        summary = dec.decode_session_bytes(reassembled)
        self.assertTrue(summary["header_crc_ok"])
        self.assertEqual(summary["valid_slots"], 11)
        by_slot = {r["slot"]: r for r in summary["records"]}
        self.assertEqual(by_slot[7]["label"], "vm_done")
        self.assertEqual(by_slot[39]["kdbg_text"], "kernel_bootstrap")
        self.assertEqual(by_slot[47]["kdbg_text"], "PE_init_iokit")
        self.assertEqual(by_slot[46]["panic_text_prefix"], "assertion failed")


if __name__ == "__main__":
    unittest.main()
