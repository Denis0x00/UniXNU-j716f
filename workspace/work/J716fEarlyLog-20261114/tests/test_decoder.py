#!/usr/bin/env python3
"""Independent malformed/torn/epoch decoder fixtures; no hardware access."""
import struct
import unittest
import decode_early_log as d


def header(sequence=7, source=2, boot_id=0x1122334455667788, tag=b"r22-test"):
    h = bytearray(128)
    struct.pack_into("<IHHIIIIIIQQII", h, 0, d.MAGIC, 1, 128, 128, 240, d.TOTAL_BYTES,
                     sequence, source, 0, boot_id, d.BASE, d.REGION_BYTES, 0)
    h[56:56 + len(tag)] = tag
    struct.pack_into("<I", h, 124, d.crc32(h[:124]))
    return h


def record(sequence, payload=b"hello\n"):
    r = bytearray(128)
    struct.pack_into("<QII", r, 0, sequence, len(payload), 0)
    r[16:16 + len(payload)] = payload
    struct.pack_into("<II", r, 120, d.crc32(r[:120]),
                     d.COMMIT ^ (sequence & 0xFFFFFFFF) ^ (sequence >> 32))
    return r


def fixture(records=()):
    mem = bytearray(d.TOTAL_BYTES)
    mem[:128] = header()
    for seq, payload in records:
        start = 128 + ((seq - 1) % 240) * 128
        mem[start:start + 128] = record(seq, payload)
    return mem


class DecoderTests(unittest.TestCase):
    def test_ieee_crc(self):
        self.assertEqual(d.crc32(b"123456789"), 0xCBF43926)

    def test_empty_epoch(self):
        r = d.decode(fixture())
        self.assertEqual((r["exported_records"], r["empty_records"], r["gaps"]), (0, 240, []))

    def test_chronological_wrap(self):
        mem = fixture((seq, str(seq).encode()) for seq in range(1, 502))
        r = d.decode(mem)
        self.assertEqual((r["first_sequence"], r["highest_sequence"]), (262, 501))
        self.assertEqual([x["sequence"] for x in r["records"]], list(range(262, 502)))
        self.assertFalse(r["gaps"])

    def test_header_every_byte_corruption(self):
        for offset in range(128):
            mem = fixture()
            mem[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(d.DecodeError):
                d.decode(mem)

    def test_header_malformed_with_recomputed_crc(self):
        edits = [(0, "I", 0), (4, "H", 2), (6, "H", 127), (8, "I", 127),
                 (12, "I", 241), (16, "I", d.TOTAL_BYTES - 1), (20, "I", 0),
                 (24, "I", 0), (24, "I", 3), (28, "I", 1), (32, "Q", 0),
                 (40, "Q", d.BASE + 4096), (48, "I", 0x100000), (52, "I", 1), (88, "B", 1)]
        for offset, fmt, value in edits:
            mem = fixture()
            struct.pack_into("<" + fmt, mem, offset, value)
            struct.pack_into("<I", mem, 124, d.crc32(mem[:124]))
            with self.subTest(offset=offset, value=value), self.assertRaises(d.DecodeError):
                d.decode(mem)

    def test_invalid_tags(self):
        for tag in (b"\0" * 32, b"a" * 32, b"a\0b" + bytes(29), b"a\n" + bytes(30),
                    b"a\xff" + bytes(30)):
            mem = fixture()
            mem[56:88] = tag
            struct.pack_into("<I", mem, 124, d.crc32(mem[:124]))
            with self.subTest(tag=tag), self.assertRaises(d.DecodeError): d.decode(mem)

    def test_record_every_byte_corruption(self):
        for offset in range(128):
            mem = fixture([(1, b"abc"), (2, b"def")])
            mem[128 + offset] ^= 1
            r = d.decode(mem)
            with self.subTest(offset=offset):
                self.assertEqual(r["exported_records"], 1)
                self.assertEqual(r["gaps"], [1])

    def test_record_semantic_rejections(self):
        edits = [(0, "Q", 0), (0, "Q", 2), (8, "I", 0), (8, "I", 105),
                 (12, "I", 1), (20, "B", 1)]
        for offset, fmt, value in edits:
            r = record(1, b"abc")
            struct.pack_into("<" + fmt, r, offset, value)
            struct.pack_into("<I", r, 120, d.crc32(r[:120]))
            sequence = struct.unpack_from("<Q", r)[0]
            struct.pack_into("<I", r, 124, d.COMMIT ^ (sequence & 0xFFFFFFFF) ^ (sequence >> 32))
            with self.subTest(offset=offset, value=value), self.assertRaises(d.DecodeError):
                d.validate_record(r, 0)

    def test_cleared_commit_hides_stale_payload(self):
        mem = fixture((seq, b"old") for seq in range(1, 241))
        for slot in range(240): mem[128 + slot * 128 + 124:128 + slot * 128 + 128] = bytes(4)
        self.assertEqual(d.decode(mem)["exported_records"], 0)

    def test_stale_slot_older_than_window(self):
        mem = fixture([(1, b"old"), (482, b"new")])
        r = d.decode(mem)
        self.assertEqual(r["stale_sequences"], [1])
        self.assertEqual([x["sequence"] for x in r["records"]], [482])
        self.assertEqual(len(r["gaps"]), 239)

    def test_uint64_max_bounded(self):
        mem = fixture([(d.MAX_SEQUENCE, b"last")])
        r = d.decode(mem)
        self.assertEqual(r["highest_sequence"], d.MAX_SEQUENCE)
        self.assertEqual(len(r["gaps"]), 239)
        self.assertEqual(r["exported_records"], 1)

    def test_truncation(self):
        for n in (0, 1, 127, 128, d.TOTAL_BYTES - 1):
            with self.subTest(n=n), self.assertRaises(d.DecodeError): d.decode(bytes(n))

    def test_uncommitted_torn_header(self):
        old = bytearray(d.TOTAL_BYTES)
        new = fixture()
        for n in range(1, 4):
            old[:n] = new[:n]
            with self.assertRaises(d.DecodeError): d.decode(old)


if __name__ == "__main__":
    unittest.main(verbosity=2)
