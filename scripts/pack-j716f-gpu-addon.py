#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Assemble a TB-J716F Stage 7+ / Phase 4b boot.img with AdrenoMetal GPU addon.

Builds a DarwinBoot-compatible 3-module owner-trust.bundle (prepending the
signed NativeAdrenoMetal / daemon / Turnip CDHashes to the stock 21H461 trust
cache bundle), validates the NGPUADD2 gpuaddon.bin container, computes the
required FAT payload size_mib, and invokes official `./unixnu build payload`
and `./unixnu bootimg`.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path
from typing import Any

STOCK_21H461_TRUST_SHA256 = (
    "7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7"
)
NGPUADD2_MAGIC = b"NGPUADD2"
TRUST_NAMESPACE_PREFIX = "org.blu-sharky.adreno-metal:"


def validate_trust_bundle(data: bytes) -> bool:
    """Validate a trust cache bundle using the same checks as DarwinBoot.c."""
    if len(data) < 8:
        return False
    count = struct.unpack_from("<I", data, 0)[0]
    if not (0 < count <= 128):
        return False
    header_bytes = 4 + 4 * count
    if len(data) < header_bytes:
        return False
    for idx in range(count):
        start = struct.unpack_from("<I", data, 4 + 4 * idx)[0]
        end = (
            struct.unpack_from("<I", data, 8 + 4 * idx)[0]
            if idx + 1 < count
            else len(data)
        )
        if start < header_bytes or (start & 3) != 0 or end > len(data) or end - start < 24:
            return False
        version = struct.unpack_from("<I", data, start)[0]
        if version != 2:
            return False
        entries = struct.unpack_from("<I", data, start + 20)[0]
        if end - start != 24 + entries * 24:
            return False
        for entry in range(entries):
            at = start + 24 + entry * 24
            hash_type = data[at + 20]
            if not (1 <= hash_type <= 4):
                return False
            if entry > 0 and data[at - 24 : at - 4] >= data[at : at + 20]:
                return False
    return True


def build_owner_trust_bundle(
    original_trust: bytes,
    trust_meta: dict[str, Any],
    *,
    expected_orig_sha256: str | None = STOCK_21H461_TRUST_SHA256,
) -> bytes:
    """Prepend the 3 sorted GPU addon CDHashes to the stock trust cache bundle."""
    if expected_orig_sha256 is not None:
        actual = hashlib.sha256(original_trust).hexdigest()
        if actual != expected_orig_sha256:
            raise ValueError(
                f"Original trust cache SHA-256 mismatch: got {actual}, expected {expected_orig_sha256}"
            )
    if not validate_trust_bundle(original_trust):
        raise ValueError("Original trust cache failed structural validation")

    orig_count = struct.unpack_from("<I", original_trust, 0)[0]
    offsets = struct.unpack_from(f"<{orig_count}I", original_trust, 4)
    originals = [
        original_trust[
            offsets[i] : (offsets[i + 1] if i + 1 < orig_count else len(original_trust))
        ]
        for i in range(orig_count)
    ]

    required_keys = ("cdhash", "daemon_cdhash", "turnip_cdhash")
    parsed_hashes: set[bytes] = set()
    for key in required_keys:
        raw_hex = trust_meta.get(key)
        if not isinstance(raw_hex, str) or len(raw_hex) != 40:
            raise ValueError(f"trust.json missing valid 20-byte hex {key!r}")
        parsed_hashes.add(bytes.fromhex(raw_hex))
    hashes = sorted(parsed_hashes)
    if len(hashes) != 3:
        raise ValueError(f"Expected 3 unique CDHashes, got {len(hashes)}")

    identifier = uuid.uuid5(
        uuid.NAMESPACE_URL,
        TRUST_NAMESPACE_PREFIX + "".join(h.hex() for h in hashes),
    )
    own_module = struct.pack("<I16sI", 2, identifier.bytes, len(hashes)) + b"".join(
        h + bytes([2, 0, 0, 0]) for h in hashes
    )
    modules = [own_module, *originals]
    total_count = len(modules)
    cursor = 4 + 4 * total_count
    starts: list[int] = []
    for mod in modules:
        starts.append(cursor)
        cursor += len(mod)
    bundle = struct.pack(f"<{1 + total_count}I", total_count, *starts) + b"".join(modules)
    if not validate_trust_bundle(bundle):
        raise ValueError("Constructed owner-trust.bundle failed DarwinBoot validation")
    return bundle


def validate_gpu_addon(addon_bytes: bytes) -> dict[str, int]:
    """Validate NGPUADD2 container header and total size."""
    if len(addon_bytes) < 24:
        raise ValueError(f"GPU addon too short ({len(addon_bytes)} bytes)")
    if addon_bytes[:8] != NGPUADD2_MAGIC:
        raise ValueError(f"Invalid GPU addon magic: {addon_bytes[:8]!r}")
    version = struct.unpack_from("<I", addon_bytes, 8)[0]
    header_len = struct.unpack_from("<I", addon_bytes, 12)[0]
    total_len = struct.unpack_from("<Q", addon_bytes, 16)[0]
    if total_len != len(addon_bytes):
        raise ValueError(
            f"NGPUADD2 total length mismatch: header={total_len}, actual={len(addon_bytes)}"
        )
    return {"version": version, "header_len": header_len, "total_len": total_len}


def compute_payload_size_mib(
    kernel_bytes: int,
    data_bytes: int,
    addon_bytes: int,
    trust_bytes: int,
    *,
    overhead_mib: int = 8,
    min_mib: int = 80,
    max_mib: int = 88,
) -> int:
    """Compute bounded FAT payload size_mib fitting kernel, data seed, addon, and trust cache."""
    if min(kernel_bytes, data_bytes, addon_bytes, trust_bytes) < 0:
        raise ValueError("Input sizes must be non-negative")
    need_mib = (
        kernel_bytes + data_bytes + addon_bytes + trust_bytes + overhead_mib * 1024 * 1024
    ) // (1024 * 1024)
    return max(min_mib, min(max_mib, need_mib))


def default_phase4b_kernel_args(
    extra_args: list[str] | None = None,
    *,
    fb_gpu: bool = False,
    data_persist: int = 0,
    data_reset: bool = False,
) -> list[str]:
    """Build standard TB-J716F Stage 7+ Phase 4b kernel boot arguments."""
    if data_persist not in (0, 1, 2):
        raise ValueError(f"Invalid data_persist mode: {data_persist} (expected 0, 1, or 2)")

    args = [
        "serial=3",
        "j716f-earlylog=1",
        "j716f-r41-continue=1",
        "j716f-skip-setup=1",
        "j716f-usb=1",
        "j716f-battery=1",
        "j716f-gpu=11",
        "j716f-gpu-metal=1",
        "gpu-addon=1",
        "smp=1",
        "armcpu-spi-ipi=1",
        "armcpu-self-ipi=1",
        "armcpu-edge=1",
        "cpufreq=4",
    ]
    if data_persist == 0:
        args.extend(["data-reset=1", "data-fua=1"])
    else:
        args.append(f"j716f-data-persist={data_persist}")
        if data_reset:
            args.append("data-reset=1")

    args.extend(
        [
            "-apfs_shared_datavolume",
            "-no_slto_panic",
            "offline-mobile-graphics-load=1",
            "offline-legacy-register=1",
            "offline-legacy-gop=1",
            "offline-legacy-unit-scale=2",
            "offline-legacy-pm=1",
            "offline-legacy-validate=1",
            "offline-legacy-transfer=1",
            "offline-legacy-cpu-present=1",
            "offline-legacy-phase=4",
            "offline-key-events=1",
            "aks=1",
            "offline-hid-input=1",
            "offline-hid-home=1",
            "touch-hid=1",
            "j716f-touch=1",
            "j716f-buttons=1",
        ]
    )
    if fb_gpu:
        args.append("fb-gpu=1")
    if extra_args:
        args.extend(extra_args)
    return args


def _replace_payload_size_mib(board_text: str, new_size_mib: int) -> tuple[str, int]:
    pattern = re.compile(r"(?m)^(\s*size_mib\s*=\s*)(\d+)(\s*)$")
    match = pattern.search(board_text)
    if not match:
        raise ValueError("Could not find payload size_mib in board.toml")
    old_size = int(match.group(2))
    if old_size == new_size_mib:
        return board_text, old_size
    updated = pattern.sub(rf"\g<1>{new_size_mib}\g<3>", board_text, count=1)
    return updated, old_size


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    default_device = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(
        description="Pack a TB-J716F Stage 7+ / Phase 4b boot.img with gpuaddon.bin."
    )
    parser.add_argument("--unixnu-core", type=Path, required=True, help="Path to UniXNU core checkout")
    parser.add_argument("--device-dir", type=Path, default=default_device, help="Path to UniXNU-j716f checkout")
    parser.add_argument("--config", type=Path, required=True, help="Path to local Stage 7 build-inputs TOML")
    parser.add_argument("--addon", type=Path, required=True, help="Path to NGPUADD2 gpuaddon.bin")
    parser.add_argument("--trust-json", type=Path, required=True, help="Path to gpuaddon trust.json")
    parser.add_argument("--orig-trust", type=Path, required=True, help="Path to stock 21H461 trustcache bundle")
    parser.add_argument("--data-ramdisk", type=Path, required=True, help="Path to packed data-21H461.dbr")
    parser.add_argument("--dtb", type=Path, required=True, help="Path to normalized TB-J716F stock DTB")
    parser.add_argument(
        "--out-trust",
        type=Path,
        default=None,
        help="Optional path to write constructed owner-trust.bundle (defaults to temp file)",
    )
    parser.add_argument(
        "--chip-id-file",
        type=Path,
        default=None,
        help="Optional file containing 16-hex UNIXNU_UNIQUE_CHIP_ID (else use environment)",
    )
    parser.add_argument(
        "--toolchain",
        type=Path,
        default=None,
        help="Optional UNIXNU_TOOLCHAIN root (prepends llvm/bin and bin to PATH)",
    )
    parser.add_argument(
        "--skip-orig-sha-check",
        action="store_true",
        help="Skip pinned stock 21H461 trust cache SHA-256 check",
    )
    parser.add_argument("--fb-gpu", action="store_true", help="Append opt-in fb-gpu=1 GPU presenter handoff")
    parser.add_argument(
        "--data-persist",
        type=int,
        choices=(0, 1, 2),
        default=0,
        help="0=volatile RAM Data store (default), 1=persistent UFS RO probe, 2=persistent UFS RW direct",
    )
    parser.add_argument(
        "--data-reset",
        action="store_true",
        help="Include data-reset=1 when --data-persist is 1 or 2 (re-initializes persistent Data overlay)",
    )
    parser.add_argument(
        "--extra-arg",
        action="append",
        default=[],
        help="Additional kernel boot-arg (may be passed multiple times)",
    )
    parser.add_argument(
        "--config-name",
        default=None,
        help="Optional UniXNU output config name (defaults to auto-detecting j716f or j716f_stage7)",
    )
    parser.add_argument("--dry-run", action="store_true", help="Validate inputs and print commands without running")
    return parser.parse_args(argv)


def resolve_stage7_out_dir(core: Path, config_name: str | None = None) -> Path:
    """Resolve the built Stage 7+ kernel output directory under UniXNU out/."""
    base = core / "out/j716f/ipados-17.7.11-21H461"
    if config_name:
        return base / config_name
    for candidate in ("j716f", "j716f_stage7"):
        cand_dir = base / candidate
        if (cand_dir / "kernel/kernel.macho").is_file():
            return cand_dir
    return base / "j716f"


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    core = args.unixnu_core.resolve()
    dev = args.device_dir.resolve()
    out_dir = resolve_stage7_out_dir(core, args.config_name)
    kernel_path = out_dir / "kernel/kernel.macho"
    if not kernel_path.is_file():
        raise SystemExit(f"Missing built Stage 7+ kernel: {kernel_path}")

    trust_meta = json.loads(args.trust_json.read_text(encoding="utf-8"))
    bundle = build_owner_trust_bundle(
        args.orig_trust.read_bytes(),
        trust_meta,
        expected_orig_sha256=None if args.skip_orig_sha_check else STOCK_21H461_TRUST_SHA256,
    )
    addon_bytes = args.addon.read_bytes()
    addon_info = validate_gpu_addon(addon_bytes)

    kernel_bytes = kernel_path.stat().st_size
    data_bytes = args.data_ramdisk.stat().st_size
    size_mib = compute_payload_size_mib(kernel_bytes, data_bytes, len(addon_bytes), len(bundle))

    kernel_sha = hashlib.sha256(kernel_path.read_bytes()).hexdigest()
    bundle_sha = hashlib.sha256(bundle).hexdigest()
    print(f"OWNER_TRUST_BYTES {len(bundle)} sha256={bundle_sha}")
    print(f"GPU_ADDON_VERSION {addon_info['version']} bytes={len(addon_bytes)}")
    print(f"KERNEL_SHA256 {kernel_sha}")
    print(
        f"PAYLOAD_SIZE_MIB {size_mib} "
        f"(kernel={kernel_bytes / 1048576:.1f}M data={data_bytes / 1048576:.1f}M "
        f"addon={len(addon_bytes) / 1048576:.1f}M)"
    )

    kernel_args = default_phase4b_kernel_args(
        args.extra_arg,
        fb_gpu=args.fb_gpu,
        data_persist=args.data_persist,
        data_reset=args.data_reset,
    )

    env = dict(os.environ)
    if args.toolchain is not None:
        tc = args.toolchain.resolve()
        env["UNIXNU_TOOLCHAIN"] = str(tc)
        env["PATH"] = f"{tc / 'llvm/bin'}:{tc / 'bin'}:{env.get('PATH', '')}"
    if args.chip_id_file is not None:
        env["UNIXNU_UNIQUE_CHIP_ID"] = args.chip_id_file.read_text(encoding="utf-8").strip()
    if not args.dry_run and not env.get("UNIXNU_UNIQUE_CHIP_ID"):
        raise SystemExit("UNIXNU_UNIQUE_CHIP_ID not set (pass --chip-id-file or set env var)")

    tmp_dir: tempfile.TemporaryDirectory[str] | None = None
    if args.out_trust is not None:
        owner_trust_path = args.out_trust.resolve()
        owner_trust_path.parent.mkdir(parents=True, exist_ok=True)
    else:
        tmp_dir = tempfile.TemporaryDirectory(prefix="j716f-pack-")
        owner_trust_path = Path(tmp_dir.name) / "owner-trust.bundle"

    board_toml = dev / "board.toml"
    orig_board = board_toml.read_text(encoding="utf-8")
    try:
        owner_trust_path.write_bytes(bundle)
        cmd_payload = [
            "./unixnu",
            "build",
            "payload",
            "--config",
            str(args.config.resolve()),
            "--owner-trust-cache",
            str(owner_trust_path),
            "--gpu-addon",
            str(args.addon.resolve()),
            "--data-ramdisk",
            str(args.data_ramdisk.resolve()),
            *[f"--kernel-arg={arg}" for arg in kernel_args],
        ]
        cmd_boot = [
            "./unixnu",
            "bootimg",
            "--config",
            str(args.config.resolve()),
            "--dtb",
            str(args.dtb.resolve()),
        ]
        print("+", " ".join(cmd_payload))
        print("+", " ".join(cmd_boot))
        if args.dry_run:
            return 0

        updated_board, old_size = _replace_payload_size_mib(orig_board, size_mib)
        if old_size != size_mib:
            board_toml.write_text(updated_board, encoding="utf-8")

        subprocess.run(cmd_payload, cwd=core, env=env, check=True)
        subprocess.run(cmd_boot, cwd=core, env=env, check=True)

        boot_img = out_dir / "boot/boot.img"
        print(f"BOOT_IMG {boot_img}")
        print(f"BOOT_SIZE {boot_img.stat().st_size}")
        print(f"BOOT_SHA256 {hashlib.sha256(boot_img.read_bytes()).hexdigest()}")
        return 0
    finally:
        if board_toml.read_text(encoding="utf-8") != orig_board:
            board_toml.write_text(orig_board, encoding="utf-8")
        if tmp_dir is not None:
            tmp_dir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())
