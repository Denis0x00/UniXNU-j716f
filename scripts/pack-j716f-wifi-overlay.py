#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Merge signed arm64-apple-ios17.0 unixnu-wpa_supplicant + iPadOS 17 launchd.plist
cache overlay into an existing NGPUADD2 gpuaddon.bin and trust.json.

All IPSW-derived inputs (stock /System/Library/xpc/launchd.plist) stay strictly local;
never commit merged launchd.plist or binaries into Git.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import plistlib
import struct
import sys
import zlib
from pathlib import Path
from typing import Any

NGPUADD2_MAGIC = b"NGPUADD2"
SUPPLICANT_JOB_KEY = "/System/Library/LaunchDaemons/org.unixnu.wifi.supplicant.plist"
STOCK_21H461_LAUNCHD_CDHASH = "1dceadc254af12d78ea5ac8e8c6458dfdae5470e"


def pad8(data: bytes) -> bytes:
    return data + bytes(-len(data) % 8)


def unpack_ngpuadd2(blob: bytes) -> list[tuple[str, int, bytes]]:
    if len(blob) < 24 or blob[:8] != NGPUADD2_MAGIC:
        raise ValueError("Invalid NGPUADD2 magic")
    count, _reserved, total = struct.unpack_from("<IIQ", blob, 8)
    if total != len(blob):
        raise ValueError(f"NGPUADD2 size mismatch: header={total} actual={len(blob)}")
    at = 24
    entries: list[tuple[str, int, bytes]] = []
    for _ in range(count):
        name_bytes, mode, size, packed = struct.unpack_from("<IIQQ", blob, at)
        at += 24
        name = blob[at : at + name_bytes - 1].decode("utf-8")
        at += (name_bytes + 7) & ~7
        raw = zlib.decompress(blob[at : at + packed])
        if len(raw) != size:
            raise ValueError(f"Decompressed size mismatch for {name}")
        at += (packed + 7) & ~7
        entries.append((name, mode, raw))
    if at != len(blob):
        raise ValueError("Trailing bytes in NGPUADD2")
    return entries


def pack_ngpuadd2(entries: list[tuple[str, int, bytes]]) -> bytes:
    body = bytearray()
    for name, mode, data in entries:
        enc = name.encode("utf-8") + b"\0"
        if len(enc) > 160 or name.startswith("/") or ".." in name.split("/"):
            raise ValueError(f"Bad NGPUADD2 path: {name}")
        packed = zlib.compress(data, 9)
        body += struct.pack("<IIQQ", len(enc), mode, len(data), len(packed))
        body += pad8(enc)
        body += pad8(packed)
    return NGPUADD2_MAGIC + struct.pack("<IIQ", len(entries), 0, 24 + len(body)) + bytes(body)


def build_detached_codesig(
    data: bytes,
    ident: str,
    *,
    version: int = 0x20400,
    flags: int = 0x2,
    page_shift: int = 12,
    runtime: int = 0,
) -> tuple[bytes, str]:
    """Build an Apple ad-hoc SHA-256 detached SuperBlob signature (4 KiB pages)."""
    req_blob = struct.pack(">III", 0xFADE0C01, 12, 0)
    req_hash = hashlib.sha256(req_blob).digest()
    cms_blob = struct.pack(">II", 0xFADE0B01, 8)

    page_size = 1 << page_shift
    n_code = (len(data) + page_size - 1) // page_size
    n_spec = 2  # -2: requirements, -1: info (zero)
    ident_bytes = ident.encode("utf-8") + b"\0"
    hdr_size = 96 if version >= 0x20500 else 88
    ident_off = hdr_size
    hash_off = ident_off + len(ident_bytes) + n_spec * 32
    code_hashes = b"".join(
        hashlib.sha256(data[i * page_size : (i + 1) * page_size]).digest()
        for i in range(n_code)
    )
    special_hashes = req_hash + b"\x00" * 32
    cd_len = hash_off + len(code_hashes)

    hdr = struct.pack(
        ">IIIIIIIIIBBBBIIIIQQQQ",
        0xFADE0C02,
        cd_len,
        version,
        flags,
        hash_off,
        ident_off,
        n_spec,
        n_code,
        len(data) & 0xFFFFFFFF,
        32,
        2,
        0,
        page_shift,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0,
    )
    if version >= 0x20500:
        hdr += struct.pack(">II", runtime, 0)
    assert len(hdr) == hdr_size
    cd = hdr + ident_bytes + special_hashes + code_hashes
    assert len(cd) == cd_len
    cdhash = hashlib.sha256(cd).hexdigest()[:40]

    blobs = [(0x0, cd), (0x2, req_blob), (0x10000, cms_blob)]
    off = 12 + 8 * len(blobs)
    idx = bytearray()
    body = bytearray()
    for slot, blob in blobs:
        idx += struct.pack(">II", slot, off)
        body += blob
        off += len(blob)
    superblob = struct.pack(">III", 0xFADE0CC0, off, len(blobs)) + bytes(idx) + bytes(body)
    return superblob, cdhash


def merge_launchd_cache_bytes(orig_plist_bytes: bytes) -> tuple[bytes, int]:
    original = plistlib.loads(orig_plist_bytes)
    if original.get("VersionNumber") != 7 or "LaunchDaemons" not in original:
        raise ValueError("Unexpected launchd.plist schema (expected VersionNumber=7)")
    jobs = dict(original["LaunchDaemons"])
    if SUPPLICANT_JOB_KEY in jobs:
        raise ValueError(f"launchd.plist already contains {SUPPLICANT_JOB_KEY}")
    jobs[SUPPLICANT_JOB_KEY] = {
        "Label": "org.unixnu.wifi.supplicant",
        "ProgramArguments": [
            "/usr/libexec/unixnu-wpa_supplicant",
            "-Dsheng",
            "-ien0",
            "-c/private/var/ngpu/etc/wifi/wpa_supplicant.conf",
            "-q",
            "-s",
        ],
        "RunAtLoad": True,
        "KeepAlive": True,
        "ThrottleInterval": 3,
        "UserName": "root",
        "Umask": 0o77,
        "ProcessType": "Interactive",
    }
    merged = dict(original)
    merged["LaunchDaemons"] = jobs
    out_bytes = plistlib.dumps(merged, fmt=plistlib.FMT_BINARY, sort_keys=False)
    verify = plistlib.loads(out_bytes)
    if len(verify["LaunchDaemons"]) != len(original["LaunchDaemons"]) + 1:
        raise ValueError("Merged launchd.plist verification failed")
    return out_bytes, len(original["LaunchDaemons"])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base-addon", type=Path, required=True)
    ap.add_argument("--base-trust-json", type=Path, required=True)
    ap.add_argument("--supplicant", type=Path, required=True)
    ap.add_argument("--supplicant-cdhash", required=True)
    ap.add_argument("--orig-launchd-plist", type=Path, required=True)
    ap.add_argument("--out-addon", type=Path, required=True)
    ap.add_argument("--out-trust-json", type=Path, required=True)
    args = ap.parse_args()

    sup_cdhash = args.supplicant_cdhash.strip().lower()
    if len(sup_cdhash) != 40:
        raise SystemExit("Invalid --supplicant-cdhash (expected 40 hex chars)")
    bytes.fromhex(sup_cdhash)

    orig_launchd = args.orig_launchd_plist.read_bytes()
    # Self-check detached CodeDirectory builder against known stock 21H461 launchd.plist CDHash if stock matches.
    if hashlib.sha256(orig_launchd).hexdigest() == "f1b99c4e3e738cb1c2622ad82f5e78cd2fbd1f88c8ced13c423aabf91b2b22eb":
        _, check_cdh = build_detached_codesig(
            orig_launchd,
            "launchd-b870d2bfc5324009c3e904a4506f78e102d89b2b",
            version=0x20500,
            flags=0x10002,
            page_shift=12,
            runtime=0x000B0000,
        )
        if check_cdh != STOCK_21H461_LAUNCHD_CDHASH:
            raise SystemExit(f"CodeDirectory self-check failed: got {check_cdh}")

    merged_plist, orig_jobs = merge_launchd_cache_bytes(orig_launchd)
    sig_bytes, cache_cdhash = build_detached_codesig(
        merged_plist,
        "org.unixnu.wifi.launchd-cache",
        version=0x20400,
        flags=0x2,
        page_shift=12,
    )

    base_entries = unpack_ngpuadd2(args.base_addon.read_bytes())
    filtered = [
        e for e in base_entries
        if e[0] not in (
            "wifi-bin/unixnu-wpa_supplicant",
            "etc/wifi/wpa_supplicant.conf",
            "xpc/launchd.plist",
            "xpc/launchd.plist.sig",
        )
    ]
    sup_bytes = args.supplicant.read_bytes()
    if sup_bytes[:4] != b"\xcf\xfa\xed\xfe":
        raise SystemExit("Supplicant is not a 64-bit little-endian Mach-O binary")
    conf_bytes = b"# Networks arrive from the native Wi-Fi control channel.\nap_scan=1\n"
    filtered.extend([
        ("wifi-bin/unixnu-wpa_supplicant", 0o755, sup_bytes),
        ("etc/wifi/wpa_supplicant.conf", 0o644, conf_bytes),
        ("xpc/launchd.plist", 0o644, merged_plist),
        ("xpc/launchd.plist.sig", 0o644, sig_bytes),
    ])
    filtered.sort(key=lambda item: item[0])
    merged_addon = pack_ngpuadd2(filtered)
    args.out_addon.parent.mkdir(parents=True, exist_ok=True)
    args.out_addon.write_bytes(merged_addon)

    trust: dict[str, Any] = json.loads(args.base_trust_json.read_text(encoding="utf-8"))
    extras = set(trust.get("extra_cdhashes", []))
    extras.add(sup_cdhash)
    extras.add(cache_cdhash)
    trust["extra_cdhashes"] = sorted(extras)
    trust["wifi_supplicant_cdhash"] = sup_cdhash
    trust["wifi_launchd_cache_cdhash"] = cache_cdhash
    args.out_trust_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_trust_json.write_text(json.dumps(trust, indent=2) + "\n", encoding="utf-8")

    print(f"MERGED_LAUNCHD_JOBS {orig_jobs} -> {orig_jobs + 1} ({len(merged_plist)} B)")
    print(f"LAUNCHD_CACHE_CDHASH {cache_cdhash}")
    print(f"SUPPLICANT_CDHASH {sup_cdhash}")
    print(f"OUT_ADDON {args.out_addon} ({len(merged_addon)} B, {len(filtered)} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
