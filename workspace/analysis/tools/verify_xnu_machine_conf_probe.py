#!/usr/bin/env python3
"""Independently verify r8 after-machine_conf six-second reset and its boot container.

Exact expected instructions and the full allowed-change boundary are checked
without trusting official patch reports or the generated candidate digest. The
full expected digest was derived independently from the original kernel and
these instruction requirements before running the official patcher. No device
access is performed, and no completed kernel startup is inferred.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tomllib
import zlib

sys.dont_write_bytecode = True
from verify_menu_artifact import require, sha256, load_json
from verify_initrd80_probe import checked_relative, check_identity, verify_fd_provenance
from verify_xnu_entry import read_fat32, validate_adt, compare_adt_baseline
from verify_xnu_dt_panic_probe import (
    baseline_layout, paint_cave, verify_footer_provenance, KERNEL_SHA, KERNEL_BYTES,
    ENTRY_VA, FIRST_CPU_VA, MMU_VA, POST_MMU_VA, CAVES,
    FD_SHA, DTB_SHA, SHIM_SHA, BASELINE_ADT_SHA, FAT_BYTES,
)
from prepare_r6_followup import MachO, target
from verify_first_payload import branch

DEVICE = "j716f-xnu-machine-conf-probe"
DEFCONFIG = "j716f_xnu_machine_conf_probe_defconfig"
TITLE = "J716F XNU machine conf r8"
XNU_TITLE = "Start XNU (machine_conf checkpoint)"
CALLER_NAME, CALLEE_NAME = "_machine_startup", "_machine_conf"
CALLER_VA, CALLER_END = 0xfffffff00741bd30, 0xfffffff00741bd44
CALLEE_VA, CALLEE_END, CALLEE_RET = 0xfffffff00741baac, 0xfffffff00741baf4, 0xfffffff00741baf0
CALL_SITE = 0xfffffff00741bd38
OLD_FIQ_SITE, OLD_R7_SITE = 0xfffffff0079e09d0, 0xfffffff0079e0a70
EXPECTED_KERNEL_SHA = "f7845fda4aea9a30aa32118062e35dc5b208d65714bfb50d670c3fb330838517"
RESET_SETTINGS = {
    "entry_probe_reset_delay": 6,
    "entry_probe_reset_after": CALLEE_NAME,
    "entry_probe_reset_in": CALLER_NAME,
    "entry_probe_reset_occurrence": 1,
    "entry_probe_reset_before": False,
}


def validate_menu(raw):
    cfg = tomllib.loads(raw.decode("utf-8"))
    require(set(cfg) == {"version", "boot", "ui", "kits", "entry"} and cfg["version"] == 1 and
            cfg["boot"] == {"timeout": -1, "default": "tools", "rotate": 0} and
            cfg["ui"] == {"title": TITLE} and cfg["kits"] == {"autoload": False},
            "Unexpected r8 menu title/default/hold policy")
    xnu = {"id": "xnu", "title": XNU_TITLE, "type": "xnu",
           "path": "Payloads/kernel.macho", "args": "-v serial=3"}
    actions = [("tools", "Tools"), ("exit", "Return to firmware"),
               ("reset", "Restart"), ("shutdown", "Power off")]
    require(cfg["entry"] == [xnu] + [{"id": a, "title": t, "type": "action", "action": a} for a, t in actions],
            "Unexpected XNU or action entry; installation and extra boot arguments prohibited")
    return cfg


def validate_board(board):
    spec = board["payload"]
    require({key: value for key, value in spec.items() if key.startswith("entry_probe_reset_")} == RESET_SETTINGS and
            spec.get("panic_screen") is False,
            "Expected only the official after-machine_conf stage 4 configuration")
    require(board["board"]["name"] == DEVICE and board["kernel"]["defconfig"] == DEFCONFIG and
            board["board"].get("os_profile") == "ipados-17.7.11-21H461" and
            board["package"]["boot_partition_bytes"] == 100663296,
            "Wrong machine-conf target/profile/capacity")
    require(spec.get("volume") == "J716FENTRY" and spec.get("size_mib") == 80 and
            spec.get("title") == XNU_TITLE and spec.get("kernel_args") == ["-v", "serial=3"] and
            spec.get("timeout") == -1 and spec.get("tools") is True and
            spec.get("adt", {}).get("owned_memory_size") == 0x10000000,
            "Unexpected board payload policy")
    return board


def validate_build_record(build):
    require(build["status"] == "host-generation-passed" and build["hardware_tested"] is False and
            build["device_actions"] is False, "Build result not host-success")
    checkpoint = build.get("reset_checkpoint", {})
    expected = {"stage": 4, "reset_delay_seconds": 6, "timebase_hz": 19200000,
                "delay_ticks": 115200000, "wrapper_entry_probe_reset_delay": 6,
                "reset_after": CALLEE_NAME, "reset_in": CALLER_NAME, "occurrence": 1,
                "reset_before": False, "call_site": hex(CALL_SITE), "stage3": False,
                "panic_screen": False, "runtime_reached": False}
    for key, value in expected.items():
        require(key in checkpoint and checkpoint[key] == value,
                "Build record misstates the after-call timed checkpoint: " + key)
    return build


def expected_regions(image):
    """AArch64 requirements independent of the upstream patcher's assembler."""
    cave = CAVES[3]
    reset_words = [
        branch(cave, CALLEE_VA) | 0x80000000,  # BL machine_conf: timer starts after RET.
        0xd53be021, 0x580001c2, 0x8b020022,    # CNTPCT; literal at +64; add deadline.
        0xd5033fdf, 0xd53be021, 0xeb02003f, 0x54ffffa3,  # ISB/read/compare/b.lo +16.
        0x52800120, 0x72b08000,               # PSCI SYSTEM_RESET 0x84000009.
        0xaa1f03e1, 0xaa1f03e2, 0xaa1f03e3,  # Zero x1,x2,x3.
        0xd4000003, 0xd503205f, 0x17ffffff,   # SMC; WFE loop if reset returns.
    ]
    reset = (struct.pack("<16I", *reset_words) + struct.pack("<Q", 115200000)).ljust(96, b"\0")
    require(reset_words[0] == 0x94000d23 and branch(CALL_SITE, cave) == 0x17fff23a,
            "Independently derived branch encoding changed")
    return [(ENTRY_VA, struct.pack("<I", branch(ENTRY_VA, CAVES[0]))),
            (MMU_VA, struct.pack("<I", branch(MMU_VA, CAVES[1]))),
            (CALL_SITE, struct.pack("<I", branch(CALL_SITE, cave))),
            (CAVES[0], paint_cave(1, CAVES[0], FIRST_CPU_VA)),
            (CAVES[1], paint_cave(2, CAVES[1], MMU_VA + 4)), (cave, reset)]


def probe_readback(original, patched):
    """Independently derive symbols and check every instruction and unchanged byte."""
    baseline_layout(original)
    require(len(patched) == KERNEL_BYTES, "Wrong extracted diagnostic kernel length")
    image, candidate = MachO(original), MachO(patched)
    require(candidate.entry == image.entry and candidate.names == image.names and
            patched[:image.header_end] == original[:image.header_end], "Kernel metadata changed")
    for name, lo, hi in ((CALLER_NAME, CALLER_VA, CALLER_END), (CALLEE_NAME, CALLEE_VA, CALLEE_END)):
        require(name not in image.ambiguous_names and image.names.get(name) == lo and image.end(name) == hi,
                "Changed/ambiguous selected function: " + name)
        require({key for key, value in image.names.items() if value == lo} == {name},
                "Selected function has an address alias: " + name)
    calls = [va for va in range(CALLER_VA, CALLER_END, 4)
             if image.word(va) & 0xfc000000 == 0x94000000 and
             target(image.word(va), va, 0x94000000) == CALLEE_VA]
    require(calls == [CALL_SITE] and image.word(CALL_SITE) == 0x97ffff5d,
            "machine_conf BL is not unique at the reviewed boundary")
    require(image.word(CALLEE_RET) == 0xd65f03c0, "Original machine_conf normal RET changed")
    require(target(image.word(CALLER_VA + 16), CALLER_VA + 16, 0x14000000) == image.names["_kernel_bootstrap"],
            "machine_startup tail-call changed")
    ranges = []
    for va, expected in expected_regions(image):
        offset = image.offset(va, len(expected))
        require(patched[offset:offset + len(expected)] == expected,
                "Exact hook/cave sequence differs at " + hex(va))
        ranges.append((offset, offset + len(expected), va))
    cursor, changed = 0, 0
    for low, high, va in sorted(ranges):
        require(low >= cursor and patched[cursor:low] == original[cursor:low],
                "Kernel changed outside permitted hook/cave intervals")
        changed += sum(a != b for a, b in zip(patched[low:high], original[low:high]))
        cursor = high
    require(patched[cursor:] == original[cursor:], "Kernel tail changed outside diagnostic intervals")
    require(candidate.body(CAVES[2], 96) == bytes(96) and candidate.word(POST_MMU_VA) == image.word(POST_MMU_VA),
            "Stage 3 stop must remain disabled")
    require(candidate.word(OLD_FIQ_SITE) == image.word(OLD_FIQ_SITE) and
            candidate.word(OLD_R7_SITE) == image.word(OLD_R7_SITE), "r6/r7 early stop leaked into r8")
    preserved = {}
    for name in ("_machine_conf", "_kernel_bootstrap", "_PE_create_console", "_fiq_context_init",
                 "_panic", "_panic_trap_to_debugger"):
        lo, hi = image.names[name], image.end(name)
        require(candidate.body(lo, hi - lo) == image.body(lo, hi - lo), "Preserved function changed: " + name)
        preserved[name] = {"start": hex(lo), "end": hex(hi), "sha256": sha256(candidate.body(lo, hi - lo))}
    require(changed == 192 and sha256(patched) == EXPECTED_KERNEL_SHA,
            "Kernel differs from independently precomputed complete byte requirements")
    return {"status": "passed-host-byte-verification", "sha256": sha256(patched), "bytes": len(patched),
            "changed_bytes": changed, "caller": CALLER_NAME, "callee": CALLEE_NAME,
            "caller_range": [hex(CALLER_VA), hex(CALLER_END)], "callee_range": [hex(CALLEE_VA), hex(CALLEE_END)],
            "call_site": hex(CALL_SITE), "original_call_word": "0x97ffff5d", "occurrence": 1,
            "callee_normal_ret_va": hex(CALLEE_RET), "reset_probe": True, "reset_stage": 4,
            "reset_before": False, "reset_after_callee_returns": True,
            "reset_delay_seconds": 6, "timebase_hz": 19200000, "delay_ticks": 115200000,
            "counter": "CNTPCT_EL0", "reset_function": "0x84000009", "conduit": "SMC #0",
            "stage3_disabled": True, "r6_r7_early_hooks_restored": True,
            "stage1_stage2_exact_words_verified": True, "all_other_bytes_identical": True,
            "header_and_symbols_unchanged": True, "preserved_functions": preserved,
            "permitted_regions": [{"va": hex(va), "file_start": hex(low), "file_end": hex(high)}
                                  for low, high, va in sorted(ranges)],
            "expected_digest_origin": "Independently derived from original bytes and exact instruction requirements before official generation",
            "hardware_tested": False,
            "scope": "After machine_conf return only; kernel_bootstrap, IOKit and official Stage 5 completion remain unproved"}


def verify(root, expected_fd_sha):
    require(expected_fd_sha == FD_SHA, "Explicit firmware pin must match the retained r4/r6 FD")
    root = root.resolve(strict=True)
    manifest = load_json(root / "manifest.json")
    required = {"boot.img", "boot.json", "payload.img", "payload.json", "payload.official.json",
                "unixnu.toml", "original-kernel.macho", "baseline.adt", "SILICIUM_UEFI.fd",
                "firmware.dtb", "shim.bin", "board.toml", "fd-provenance.json", "fd-source.json",
                "fd-archive-manifest.json", "build-result.json"}
    require(required <= manifest["outputs"].keys(), "Incomplete artifact manifest")
    require(manifest.get("hardware_tested") is False and manifest.get("deployment_ready") is False,
            "Host result cannot claim deployment qualification")
    for name, spec in manifest["outputs"].items():
        path = (root / checked_relative(name)).resolve(strict=True)
        require(path.is_relative_to(root), "Manifest escapes artifact")
        check_identity(path, spec)
    data, ramdisk, fd, dtb, shim, original = [(root / name).read_bytes() for name in
        ("boot.img", "payload.img", "SILICIUM_UEFI.fd", "firmware.dtb", "shim.bin", "original-kernel.macho")]
    for raw, digest in ((fd, expected_fd_sha), (dtb, DTB_SHA), (shim, SHIM_SHA), (original, KERNEL_SHA)):
        require(sha256(raw) == digest, "A fixed candidate input changed")
    firmware = verify_fd_provenance(root, fd)
    firmware["footer_provenance"] = verify_footer_provenance(root)
    require(data[:8] == b"ANDROID!" and 1660 < len(data) <= 100663296, "Invalid Android header/capacity")
    ks, ka, rs, ra, ss, sa, tags, page, version, osver = struct.unpack_from("<10I", data, 8)
    recovery, roff, hsize = struct.unpack_from("<IQI", data, 1632)
    ds, da = struct.unpack_from("<IQ", data, 1648)
    require((ka, rs, ra, ss, sa, tags, page, version, recovery, roff, hsize, ds, da) ==
            (0x8000, FAT_BYTES, 0x1000000, 0, 0, 0x100, 4096, 2, 0, 0, 1660, len(dtb), 0x1f00000),
            "Unexpected boot offsets/size")
    aligned = lambda n: (n + page - 1) // page * page
    rp = page + aligned(ks)
    dp = rp + aligned(rs)
    require(ks > 0 and len(data) == dp + aligned(ds), "Invalid boot extent")
    require(not any(data[hsize:page]) and not any(data[page+ks:rp]) and
            not any(data[rp+rs:dp]) and not any(data[dp+ds:]), "Nonzero boot padding")
    kernel = data[page:page+ks]
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    decoded = decoder.decompress(kernel, len(shim) + len(fd) + 1)
    require(decoded == shim + fd and decoder.eof and not decoder.unconsumed_tail and
            decoder.unused_data == dtb, "Invalid shim/FD gzip or DTB tail")
    require(data[rp:rp+rs] == ramdisk and data[dp:dp+ds] == dtb, "Payload/DTB does not match boot image")
    digest = hashlib.sha1()
    for raw in (kernel, ramdisk, b"", b"", dtb):
        digest.update(raw)
        digest.update(struct.pack("<I", len(raw)))
    require(data[576:608] == digest.digest() + b"\0" * 12, "Android image ID mismatch")
    files, geometry = read_fat32(ramdisk)
    require(set(files) == {"efi/unixnu/unixnu.toml", "efi/unixnu/payloads/kernel.macho",
                           "efi/unixnu/payloads/kernel.macho.adt"}, "Unexpected payload files")
    require(files["efi/unixnu/unixnu.toml"] == (root / "unixnu.toml").read_bytes(), "Menu bytes differ")
    cfg = validate_menu(files["efi/unixnu/unixnu.toml"])
    xnu, adt = files["efi/unixnu/payloads/kernel.macho"], files["efi/unixnu/payloads/kernel.macho.adt"]
    diagnostic = probe_readback(original, xnu)
    board = tomllib.loads((root / "board.toml").read_text(encoding="utf-8"))
    validate_board(board)
    adt_report = validate_adt(adt)
    baseline_adt = (root / "baseline.adt").read_bytes()
    require(sha256(baseline_adt) == BASELINE_ADT_SHA, "Retained ADT baseline changed")
    adt_comparison = compare_adt_baseline(baseline_adt, adt)
    payload = load_json(root / "payload.json")
    official = load_json(root / "payload.official.json")
    for name in ("kernel_sha256", "payload_kernel_sha256", "adt_sha256", "kernel_args", "panic_screen",
                 "trust_cache_sha256", "gpu_addon_sha256", "data_ramdisk_sha256", "hardware_tested", "skip_setup"):
        require(payload[name] == official[name], "Menu adaptation changed official payload metadata: " + name)
    require(payload["kernel_sha256"] == KERNEL_SHA and payload["payload_kernel_sha256"] == sha256(xnu) and
            payload["adt_sha256"] == sha256(adt), "Kernel/ADT report mismatch")
    require(payload["payload_sha256"] == sha256(ramdisk) and payload["payload_bytes"] == len(ramdisk) == FAT_BYTES and
            payload["kernel_args"] == "-v serial=3" and payload["panic_screen"] is False and
            payload["skip_setup"] is False and payload["hardware_tested"] is False, "Payload metadata mismatch")
    require(all(payload[n] is None for n in ("trust_cache_sha256", "gpu_addon_sha256", "data_ramdisk_sha256")),
            "Unexpected optional input")
    build = load_json(root / "build-result.json")
    validate_build_record(build)
    expected = {"image_sha256": sha256(data), "image_bytes": len(data), "ramdisk_bytes": len(ramdisk),
                "header_version": 2, "kernel_address": hex(ka), "tags_address": hex(tags), "dtb_address": hex(da),
                "dtb_file_offset": dp, "kernel_sha256": sha256(kernel), "fd_sha256": sha256(fd),
                "dtb_sha256": sha256(dtb), "dtb_bytes": len(dtb), "shim_bytes": len(shim), "hardware_tested": False}
    boot = load_json(root / "boot.json")
    for key, value in expected.items():
        require(boot.get(key) == value, "boot.json mismatch: " + key)
    return {"result": "passed", "scope": "Static r8 after-machine_conf six-second reset verification",
            "image": expected, "payload_sha256": sha256(ramdisk), "payload_crc32": f"{zlib.crc32(ramdisk):08x}",
            "payload_bytes": len(ramdisk), "menu_title": cfg["ui"]["title"], "menu_default": "tools",
            "menu_entry_ids": [e["id"] for e in cfg["entry"]], "fat": geometry, "adt": adt_report,
            "adt_baseline_comparison": adt_comparison, "diagnostic_kernel": diagnostic, "firmware": firmware,
            "manifest_entries_verified": len(manifest["outputs"]), "hardware_tested": False, "deployment_ready": False,
            "independently_expected_kernel_sha256": EXPECTED_KERNEL_SHA,
            "files": {name: {"bytes": len(raw), "sha256": sha256(raw)} for name, raw in files.items()},
            "limits": ["A timed reset supports machine_conf return; the checkpoint stops before kernel_bootstrap.",
                       "The six-second delay assumes the documented 19.2 MHz CNTPCT frequency.",
                       "A missing reset cannot distinguish callee, return path, counter or PSCI failure.",
                       "Probe stage 4 is not official bring-up Stage 4; official Stage 5 completion remains unproved.",
                       "No GIC, timer IRQ, IOKit, root or userland execution is inferred from host bytes.",
                       "Host CRC is optional correlation evidence, not a renewed requirement for full QR export."]}



if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--fd-sha256", required=True)
    args = parser.parse_args()
    require(not args.report.exists(), "Refusing to overwrite a report")
    result = verify(args.artifact, args.fd_sha256)
    with args.report.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps(result, indent=2))
