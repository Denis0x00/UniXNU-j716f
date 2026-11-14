#!/usr/bin/env python3
"""Independent stdlib decoder for a frozen J716F JXLG v1 RAM snapshot."""
import argparse
import binascii
import json
from pathlib import Path
import struct
import sys

BASE = 0xB0000000
REGION_BYTES = 0x80000
HEADER_BYTES = 128
RECORD_BYTES = 128
RECORD_COUNT = 240
PAYLOAD_BYTES = 104
TOTAL_BYTES = HEADER_BYTES + RECORD_COUNT * RECORD_BYTES
MAGIC = 0x474C584A
COMMIT = 0x58434D54
MAX_SEQUENCE = (1 << 64) - 1


class DecodeError(ValueError):
    pass


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def validate_header(data):
    if len(data) < HEADER_BYTES:
        raise DecodeError("truncated header")
    h = bytes(data[:HEADER_BYTES])
    fields = struct.unpack_from("<IHHIIIIIIQQII", h)
    (magic, version, header_bytes, record_bytes, count, total_bytes,
     boot_sequence, source, reserved, boot_id, base, region_bytes, flags) = fields
    if (magic, version, header_bytes, record_bytes, count, total_bytes) != (
        MAGIC, 1, HEADER_BYTES, RECORD_BYTES, RECORD_COUNT, TOTAL_BYTES
    ):
        raise DecodeError("wrong magic/version/geometry")
    if not boot_sequence or source not in (1, 2) or reserved or not boot_id:
        raise DecodeError("invalid epoch metadata")
    if base != BASE or region_bytes != REGION_BYTES or flags or any(h[88:124]):
        raise DecodeError("invalid region/flags/reserved bytes")
    tag = h[56:88]
    end = tag.find(b"\0")
    if end < 1 or any(tag[end:]) or any(c < 0x20 or c > 0x7E for c in tag[:end]):
        raise DecodeError("invalid build tag")
    if struct.unpack_from("<I", h, 124)[0] != crc32(h[:124]):
        raise DecodeError("header CRC mismatch")
    return {"boot_sequence": boot_sequence, "source": source,
            "boot_id": boot_id, "build_tag": tag[:end].decode("ascii"),
            "base": base, "region_bytes": region_bytes, "total_bytes": total_bytes}


def validate_record(record, slot):
    if len(record) != RECORD_BYTES or not 0 <= slot < RECORD_COUNT:
        raise DecodeError("invalid record geometry")
    sequence, length, flags = struct.unpack_from("<QII", record)
    checksum, commit = struct.unpack_from("<II", record, 120)
    if commit == 0:
        return None
    expected_commit = COMMIT ^ (sequence & 0xFFFFFFFF) ^ (sequence >> 32)
    if (not sequence or (sequence - 1) % RECORD_COUNT != slot or
            not 1 <= length <= PAYLOAD_BYTES or flags or commit != expected_commit):
        raise DecodeError("invalid record metadata/commit/slot")
    if any(record[16 + length:120]):
        raise DecodeError("record padding is nonzero")
    if checksum != crc32(record[:120]):
        raise DecodeError("record CRC mismatch")
    return {"sequence": sequence, "length": length, "payload": bytes(record[16:16 + length])}


def decode(data):
    if len(data) < TOTAL_BYTES:
        raise DecodeError("truncated snapshot")
    header = validate_header(data)
    valid = {}
    invalid_slots = []
    empty_slots = []
    for slot in range(RECORD_COUNT):
        offset = HEADER_BYTES + slot * RECORD_BYTES
        try:
            record = validate_record(data[offset:offset + RECORD_BYTES], slot)
        except DecodeError as error:
            invalid_slots.append({"slot": slot, "reason": str(error)})
            continue
        if record is None:
            empty_slots.append(slot)
        else:
            valid[record["sequence"]] = record
    highest = max(valid, default=0)
    first = max(1, highest - RECORD_COUNT + 1) if highest else 0
    expected = range(first, highest + 1) if highest else ()
    records = [valid[sequence] for sequence in expected if sequence in valid]
    gaps = [sequence for sequence in expected if sequence not in valid]
    return {"header": header, "first_sequence": first, "highest_sequence": highest,
            "valid_records": len(valid), "invalid_records": len(invalid_slots),
            "empty_records": len(empty_slots), "exported_records": len(records),
            "exported_bytes": sum(record["length"] for record in records),
            "gaps": gaps, "invalid_slots": invalid_slots,
            "stale_sequences": sorted(sequence for sequence in valid if sequence < first),
            "records": records}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("snapshot", type=Path)
    parser.add_argument("--text", type=Path, help="write exact payload bytes in chronological order")
    parser.add_argument("--json", type=Path, help="write the structured report")
    args = parser.parse_args()
    result = decode(args.snapshot.read_bytes())
    if args.text:
        args.text.write_bytes(b"".join(record["payload"] for record in result["records"]))
    printable = dict(result)
    printable["records"] = [dict(record, payload_hex=record["payload"].hex(),
                                 text=record["payload"].decode("utf-8", errors="replace"))
                             for record in result["records"]]
    for record in printable["records"]:
        del record["payload"]
    text = json.dumps(printable, ensure_ascii=False, indent=2) + "\n"
    if args.json:
        args.json.write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
