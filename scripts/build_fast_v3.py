#!/usr/bin/env python3
"""100% git-free builder & roundtrip auditor for J716F NATIVE FAST-V3.

Fixes BOTH cold-boot page-table rebind bugs in candidate 40458123294:
  1. [Run 1 -> Run 2 fix] XNU ARM64 T1SZ=26 (38-bit VA, 256-entry L1 root[192]):
     ubfx #30, #9 -> ubfx #30, #8 at _j716f_native_bootstrap (0xfffffff0073f972c)
     and j716f_native_vm_map (0xfffffff0073fadd4).
  2. [Run 2 -> Run 3 fix] Pre-TTBR1-switch PA->KVA resolution in j716f_native_vm_map:
     j716f_native_vm_map runs AFTER arm_vm_physmap_init(args) updates _ptov_table
     with +0x200000 2 MiB-slid KVA mappings for [topOfKernelData, real_avail_end),
     BEFORE set_mmu_ttb_alternate(cpu_ttep) switches TTBR1_EL1 off bootstrap_pagetables.
     Redirect the 3 bl _phystokv (0xfffffff0073f83e0) calls in j716f_native_vm_map
     (0xfffffff0073fadec, 0xfffffff0073fae40, 0xfffffff0073faebc) to _phystokv+0x168
     (0xfffffff0073f8548: linear bootstrap gVirtBase + (pa - gPhysBase)), then
     recompute the 32-byte corecrypto FIPS POST SHA-256 HMAC in __fips_hmacs.

Zero git invocations.
"""
from __future__ import annotations

import argparse
import gzip
import hmac
import json
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT / "github-export/UniXNU-j716f"
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(ROOT / "tools"))

import native_candidate  # noqa: E402
from native_image_formats import extract_fd, read_fat32  # noqa: E402
import build_fast_v1 as v1  # noqa: E402


def apply_v3_kernel_fixes(kernel_bytes: bytes) -> tuple[bytes, dict]:
    if v1.sha(kernel_bytes) != v1.EXPECTED_KERNEL_SHA:
        raise RuntimeError("Input kernel.macho does not match expected baseline SHA256")
    img = native_candidate.MachO(kernel_bytes)
    buf = bytearray(kernel_bytes)

    patches = [
        # 1-2: T1SZ=26 256-entry L1 root index mask (#9 -> #8)
        (
            0xFFFFFFF0073F972C,
            "_j716f_native_bootstrap (T1SZ=26 L1 mask)",
            0xD35E9A30,  # ubfx x16, x17, #30, #9
            0xD35E9630,  # ubfx x16, x17, #30, #8
        ),
        (
            0xFFFFFFF0073FADD4,
            "j716f_native_vm_map (T1SZ=26 L1 mask)",
            0xD35E9B08,  # ubfx x8, x24, #30, #9
            0xD35E9708,  # ubfx x8, x24, #30, #8
        ),
        # 3-5: Pre-TTBR1-switch linear bootstrap PA->KVA (_phystokv -> _phystokv+0x168)
        (
            0xFFFFFFF0073FADEC,
            "j716f_native_vm_map L2 resolve -> bootstrap linear PA->KVA",
            0x97FFF57D,  # bl 0xfffffff0073f83e0
            0x97FFF5D7,  # bl 0xfffffff0073f8548
        ),
        (
            0xFFFFFFF0073FAE40,
            "j716f_native_vm_map L3 alloc -> bootstrap linear PA->KVA",
            0x97FFF568,  # bl 0xfffffff0073f83e0
            0x97FFF5C2,  # bl 0xfffffff0073f8548
        ),
        (
            0xFFFFFFF0073FAEBC,
            "j716f_native_vm_map L3 resolve -> bootstrap linear PA->KVA",
            0x97FFF549,  # bl 0xfffffff0073f83e0
            0x97FFF5A3,  # bl 0xfffffff0073f8548
        ),
    ]
    applied = []
    for va, sym, old_w, new_w in patches:
        foff = img.offset(va, 4)
        actual_w = struct.unpack_from("<I", buf, foff)[0]
        if actual_w != old_w:
            raise RuntimeError(
                f"Unexpected instruction at {va:#x} ({sym}): got {actual_w:#010x}, expected {old_w:#010x}"
            )
        struct.pack_into("<I", buf, foff, new_w)
        applied.append(
            {
                "va": f"0x{va:016x}",
                "file_offset": foff,
                "symbol": sym,
                "old": f"0x{old_w:08x}",
                "new": f"0x{new_w:08x}",
            }
        )

    # Recompute corecrypto FIPS POST SHA-256 HMAC over __TEXT_EXEC,__text -> __fips_hmacs
    text_sec = [s for s in img.sections if s[0] == b"__text" and s[1] in (b"__TEXT", b"__TEXT_EXEC")][0]
    hmac_sec = [s for s in img.sections if s[0] == b"__fips_hmacs"][0]
    text_off = img.offset(text_sec[2], text_sec[3])
    hmac_off = img.offset(hmac_sec[2], 32)
    new_hmac = hmac.digest(b"\0", bytes(buf[text_off : text_off + text_sec[3]]), "sha256")
    buf[hmac_off : hmac_off + 32] = new_hmac

    out = bytes(buf)
    checks = native_candidate.check_kernel(out)
    return out, {"patches": applied, "checks": checks}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, default=ROOT / "debug/native-fast-stage-v3")
    parser.add_argument("--output", type=Path, default=ROOT / "Firmware/NATIVE-40458123294-FAST-V3")
    parser.add_argument("--ssh-host", default="10.1.2.110")
    parser.add_argument("--ssh-user", default="test")
    parser.add_argument("--stage-only", action="store_true")
    args = parser.parse_args()

    stage = args.stage.resolve()
    out_dir = args.output.resolve()
    manifest = v1.stage_fast_v1(stage)

    # Copy both NativeMapFixed.h and NativeVmMapFixed.h so FastTests.c steps 5 & 6 run!
    shutil.copyfile(v1.FAST_SRC / "NativeMapFixed.h", stage / "J716fNativeLog/NativeMapFixed.h")
    shutil.copyfile(v1.FAST_SRC / "NativeVmMapFixed.h", stage / "J716fNativeLog/NativeVmMapFixed.h")

    # Update UI & firmware banner to FAST-V3
    dsc = (stage / "Platforms/Lenovo/j716fPkg/j716f-native.dsc").read_text(encoding="utf-8")
    dsc = dsc.replace("J716F NATIVE FAST-V1 40458123294", "J716F NATIVE FAST-V3 40458123294")
    (stage / "Platforms/Lenovo/j716fPkg/j716f-native.dsc").write_text(dsc, encoding="utf-8", newline="\n")

    toml_txt = (stage / "unixnu.toml").read_text(encoding="utf-8")
    toml_txt = toml_txt.replace("NATIVE FAST-V1", "NATIVE FAST-V3")
    toml_txt = toml_txt.replace("J716F NATIVE FAST-V1", "J716F NATIVE FAST-V3 (VM+PTOV FIX)")
    (stage / "unixnu.toml").write_text(toml_txt, encoding="utf-8", newline="\n")

    # Apply all 5 kernel fixes + corecrypto POST HMAC update
    orig_kernel = (stage / "inputs/kernel.macho").read_bytes()
    fixed_kernel, patch_info = apply_v3_kernel_fixes(orig_kernel)
    fixed_kernel_sha = v1.sha(fixed_kernel)
    (stage / "inputs/kernel.macho").write_bytes(fixed_kernel)

    manifest["profile"] = "FAST-V3"
    manifest["baseline_kernel_sha256"] = v1.EXPECTED_KERNEL_SHA
    manifest["prelinked_kernel_sha256"] = fixed_kernel_sha
    manifest["v3_fixes"] = patch_info
    manifest["source_files"] = {
        p.relative_to(stage).as_posix(): v1.sha(p.read_bytes())
        for p in sorted(stage.rglob("*"))
        if p.is_file() and p.relative_to(stage).parts[0] not in ("inputs", "bootwrap")
    }
    (stage / "firmware-source.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    if args.stage_only:
        print(json.dumps({"staged": True, "fixed_kernel_sha256": fixed_kernel_sha, "patches": patch_info["patches"]}, indent=2))
        return

    v1.remote_build(stage, args.ssh_host, args.ssh_user)

    # Package & roundtrip-audit FAST-V3 boot.img
    built = stage / "built"
    fd = (built / "SILICIUM_UEFI.fd").read_bytes()
    fv, modules, sec = extract_fd(fd)
    if fv != (built / "FVMAIN.Fv").read_bytes():
        raise RuntimeError("Compressed FV differs from FVMAIN.Fv")

    map_users = [sec] + [
        v1.one_section(modules[g], 0x10)
        for g in (
            "d6a2cb7f-6a18-4e2f-b43b-9920a733700a",
            "6c2004ef-4e0e-4be4-b14c-340eb4aa5891",
            "b8d9777e-d72a-451f-9bdb-bafb52a68415",
            "40d49ba8-448a-40e5-bf92-75fdc4a760d4",
        )
    ]
    for binary in map_users:
        for pair in ((0xB0000000, 0x100000), (0xD0000000, 0x18000000), (0xCE000000, 0x700000)):
            if binary.count(struct.pack("<QQ", *pair)) != 1:
                raise RuntimeError(f"Missing linked memory reservation {pair}")

    fat = v1.one_section(modules[v1.FAT_GUID], 0x19)
    content = read_fat32(fat)
    if v1.sha(content["/Payloads/kernel.macho"]) != fixed_kernel_sha:
        raise RuntimeError("Embedded kernel.macho SHA256 mismatch!")
    if v1.sha(content["/Payloads/kernel.macho.adt"]) != v1.EXPECTED_ADT_SHA:
        raise RuntimeError("Embedded ADT SHA256 mismatch!")
    native_candidate.check_kernel(content["/Payloads/kernel.macho"])

    shim = (stage / "bootwrap/BootShim.bin").read_bytes()
    dtb = (stage / "bootwrap/dtb.bin").read_bytes()
    out_dir.mkdir(parents=True, exist_ok=False)
    raw = shim + fd
    packed = gzip.compress(raw, mtime=0) + dtb
    (out_dir / "Image.gz-dtb").write_bytes(packed)
    (out_dir / "empty-ramdisk").write_bytes(b"")
    (out_dir / "dtb.bin").write_bytes(dtb)

    target = out_dir / "j716f-native-log-40458123294-fast-v3-boot.img"
    subprocess.run(
        [
            sys.executable,
            str(v1.MKBOOTIMG),
            "--kernel",
            str(out_dir / "Image.gz-dtb"),
            "--ramdisk",
            str(out_dir / "empty-ramdisk"),
            "--dtb",
            str(out_dir / "dtb.bin"),
            "--output",
            str(target),
            "--header_version",
            "2",
            "--base",
            "0",
            "--kernel_offset",
            "32768",
            "--tags_offset",
            "256",
            "--pagesize",
            "4096",
            "--dtb_offset",
            "32505856",
        ],
        check=True,
    )

    final = target.read_bytes()
    ks, ka, rs, _ra, ss, _sa, tags, page, version, _ = struct.unpack_from("<10I", final, 8)
    if final[:8] != b"ANDROID!" or (ka, rs, ss, tags, page, version) != (0x8000, 0, 0, 0x100, 4096, 2):
        raise RuntimeError("Invalid Android boot image v2 header")
    dec = zlib.decompressobj(31)
    actual = dec.decompress(final[page : page + ks])
    if not dec.eof or dec.unused_data != dtb or actual != raw:
        raise RuntimeError("Android boot image gzip roundtrip failed")
    _, again, _ = extract_fd(actual[len(shim) :])
    if read_fat32(v1.one_section(again[v1.FAT_GUID], 0x19)) != content:
        raise RuntimeError("Android boot image FAT32 roundtrip failed")

    audit = {
        "schema": 1,
        "stage": "native-fast-v3-host-audited",
        "image": target.name,
        "image_bytes": len(final),
        "image_sha256": v1.sha(final),
        "fd_sha256": v1.sha(fd),
        "fv_sha256": v1.sha(fv),
        "payload_sha256": v1.sha(fat),
        "baseline_kernel_sha256": v1.EXPECTED_KERNEL_SHA,
        "kernel_sha256": fixed_kernel_sha,
        "adt_sha256": v1.EXPECTED_ADT_SHA,
        "logger_candidate_sha256": v1.EXPECTED_CANDIDATE_SHA,
        "bootshim_sha256": v1.EXPECTED_SHIM_SHA,
        "firmware_dtb_sha256": v1.EXPECTED_DTB_SHA,
        "v3_patches": patch_info["patches"],
        "reservation_descriptors_linked": True,
        "automatic_start": False,
    }
    (out_dir / "host-audit.json").write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(audit, indent=2))


if __name__ == "__main__":
    main()
