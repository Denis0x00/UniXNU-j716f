#!/usr/bin/env python3
"""Host unit tests for scripts/j716f-usb-debug.py GSC1 demuxer & PNG encoder."""
from __future__ import annotations

import importlib.util
import struct
import sys
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MOD_PATH = ROOT / "scripts/j716f-usb-debug.py"

spec = importlib.util.spec_from_file_location("j716f_usb_debug", MOD_PATH)
assert spec and spec.loader
usb_dbg = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = usb_dbg
spec.loader.exec_module(usb_dbg)


def make_gsc1_packet(
    frame_id: int,
    flags: int,
    width: int,
    height: int,
    total: int,
    offset: int,
    payload: bytes,
    frame_crc: int,
) -> bytes:
    chunk_crc = usb_dbg.crc32_ieee(payload) if payload else 0
    body = struct.pack(
        "<IIHHIIIIII",
        frame_id,
        flags,
        width,
        height,
        total,
        offset,
        len(payload),
        chunk_crc,
        frame_crc,
        0,
    )
    hdr_crc = usb_dbg.crc32_ieee(body[:32])
    hdr = struct.pack(
        usb_dbg.GSC1_HEADER_FMT,
        usb_dbg.GSC1_MAGIC,
        frame_id,
        flags,
        width,
        height,
        total,
        offset,
        len(payload),
        chunk_crc,
        frame_crc,
        hdr_crc,
    )
    return hdr + payload


class TestUsbDebugWire(unittest.TestCase):
    def test_crc32_standard_vector(self) -> None:
        self.assertEqual(usb_dbg.crc32_ieee(b"123456789"), 0xCBF43926)

    def test_gsc1_interleaved_reassembly_and_png(self) -> None:
        w, h = 40, 20
        rgb565 = bytearray()
        for y in range(h):
            for x in range(w):
                r5 = (x * 31) // (w - 1)
                g6 = (y * 63) // (h - 1)
                b5 = 0x1F - r5
                px = (r5 << 11) | (g6 << 5) | b5
                rgb565.extend(struct.pack("<H", px))
        raw = bytes(rgb565)
        frame_crc = usb_dbg.crc32_ieee(raw)

        stream = bytearray()
        stream.extend(b"[J716F cpufreq] init d0=1804MHz d1=2419MHz d2=3187MHz\r\n")
        for off in range(0, len(raw), 1024):
            chunk = raw[off : off + 1024]
            stream.extend(
                make_gsc1_packet(1, usb_dbg.GSC1_FLAG_SNAPSHOT, w, h, len(raw), off, chunk, frame_crc)
            )
            if off == 0:
                stream.extend(b"[J716F touch] frame 1\r\n")
        stream.extend(b"[usb] SCREEN: snapshot id=1 40x20 bytes=1600\r\n")

        texts = bytearray()
        frames: list = []
        demux = usb_dbg.Gsc1StreamDemuxer(on_text=texts.extend, on_frame=frames.append)

        # Feed in awkward 13-byte slices to stress magic/header boundary splits
        for i in range(0, len(stream), 13):
            demux.feed(bytes(stream[i : i + 13]))

        self.assertEqual(len(frames), 1)
        self.assertEqual(demux.frames_ok, 1)
        self.assertEqual(demux.bad_headers, 0)
        self.assertEqual(demux.bad_chunks, 0)
        self.assertEqual(demux.bad_frames, 0)
        self.assertEqual(frames[0].width, w)
        self.assertEqual(frames[0].height, h)
        self.assertEqual(frames[0].rgb565, raw)
        self.assertIn(b"[J716F cpufreq] init", texts)
        self.assertIn(b"[J716F touch] frame 1", texts)
        self.assertIn(b"[usb] SCREEN: snapshot id=1", texts)

        png = usb_dbg.rgb565_to_png(frames[0].rgb565, w, h)
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        # Validate every PNG chunk CRC and decompressed scanline length
        pos = 8
        idat = bytearray()
        seen_end = False
        while pos < len(png):
            length = struct.unpack_from(">I", png, pos)[0]
            tag = png[pos + 4 : pos + 8]
            payload = png[pos + 8 : pos + 8 + length]
            crc_stored = struct.unpack_from(">I", png, pos + 8 + length)[0]
            self.assertEqual(usb_dbg.crc32_ieee(tag + payload), crc_stored)
            if tag == b"IHDR":
                pw, ph, depth, ctype, comp, flt, inter = struct.unpack(">IIBBBBB", payload)
                self.assertEqual((pw, ph, depth, ctype, comp, flt, inter), (w, h, 8, 2, 0, 0, 0))
            elif tag == b"IDAT":
                idat.extend(payload)
            elif tag == b"IEND":
                seen_end = True
            pos += 12 + length
        self.assertTrue(seen_end)
        self.assertEqual(len(zlib.decompress(bytes(idat))), h * (1 + w * 3))

    def test_gsc1_rejects_corrupt_chunk_and_handles_stop(self) -> None:
        raw = b"\x00\xF8" * 16  # 4x4 pure red RGB565
        frame_crc = usb_dbg.crc32_ieee(raw)
        pkt = bytearray(make_gsc1_packet(7, usb_dbg.GSC1_FLAG_SNAPSHOT, 4, 4, len(raw), 0, raw, frame_crc))
        pkt[-1] ^= 0xFF  # corrupt payload byte

        stops: list[int] = []
        frames: list = []
        demux = usb_dbg.Gsc1StreamDemuxer(on_frame=frames.append, on_stop=stops.append)
        demux.feed(bytes(pkt))
        self.assertEqual(len(frames), 0)
        self.assertEqual(demux.bad_chunks, 1)

        stop_pkt = make_gsc1_packet(8, usb_dbg.GSC1_FLAG_STOP, 0, 0, 0, 0, b"", 0)
        demux.feed(stop_pkt)
        self.assertEqual(stops, [8])


if __name__ == "__main__":
    unittest.main()
