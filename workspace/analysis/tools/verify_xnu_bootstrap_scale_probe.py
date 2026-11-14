#!/usr/bin/env python3
"""Verify r11 first-bootstrap-scale return checkpoint with retained r9 firmware.

The dedicated r11 kernel verifier checks exact instructions and every permitted
change. The frozen r9 firmware verifier checks display-ownership evidence.
Official ADT generation is retained, with only the existing allowed random-seed
differences. No device access or completed rendering/kernel startup is inferred.
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
from verify_initrd80_probe import checked_relative, check_identity
from verify_menu_ownership_fd_r9 import verify_fd_provenance
from verify_xnu_entry import read_fat32, validate_adt, compare_adt_baseline
from verify_xnu_bootstrap_scale_kernel import (
    probe_readback, EXPECTED_KERNEL_SHA, CALLER_NAME, CALLEE_NAME, CALL_SITE, RESET_SETTINGS,
)
from verify_xnu_machine_conf_probe import (
    KERNEL_SHA, KERNEL_BYTES, DTB_SHA, SHIM_SHA, BASELINE_ADT_SHA, FAT_BYTES,
)
from verify_xnu_menu_ownership_probe import compare_r8_adt

DEVICE = "j716f-xnu-bootstrap-scale-probe"
DEFCONFIG = "j716f_xnu_bootstrap_scale_probe_defconfig"
TITLE = "J716F XNU bootstrap scale r11"
XNU_TITLE = "Start XNU (bootstrap scale checkpoint)"
FD_SHA = "4a04f13e77067732533821c39ba575a61babd46d5791ce801c67e9c0b8386dcb"
R8_ADT_SHA = "55d24c8c3527533e59f2b76955cd9113f5f63ea9249da03e0a3c945ac7ac6975"


def validate_menu(raw):
    cfg = tomllib.loads(raw.decode("utf-8"))
    require(set(cfg) == {"version", "boot", "ui", "kits", "entry"} and cfg["version"] == 1 and
            cfg["boot"] == {"timeout": -1, "default": "xnu", "rotate": 0} and
            cfg["ui"] == {"title": TITLE} and cfg["kits"] == {"autoload": False},
            "Unexpected r11 menu title/default/hold policy")
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
            "Expected only the official first-bootstrap-scale-return stage 4 configuration")
    require(board["board"]["name"] == DEVICE and board["kernel"]["defconfig"] == DEFCONFIG and
            board["board"].get("os_profile") == "ipados-17.7.11-21H461" and
            board["package"]["boot_partition_bytes"] == 100663296,
            "Wrong bootstrap-scale target/profile/capacity")
    require(spec.get("volume") == "J716FENTRY" and spec.get("size_mib") == 80 and
            spec.get("title") == XNU_TITLE and spec.get("kernel_args") == ["-v", "serial=3"] and
            spec.get("timeout") == -1 and spec.get("tools") is True and
            spec.get("adt", {}).get("owned_memory_size") == 0x10000000,
            "Unexpected board payload policy")
    return board


def validate_build_record(build):
    require(build.get("status") == "host-generation-passed" and build.get("hardware_tested") is False and
            build.get("device_actions") is False, "Build result not host-success")
    checkpoint = build.get("reset_checkpoint", {})
    expected = {"stage": 4, "reset_delay_seconds": 6, "timebase_hz": 19200000,
                "delay_ticks": 115200000, "wrapper_entry_probe_reset_delay": 6,
                "reset_after": CALLEE_NAME, "reset_in": CALLER_NAME, "occurrence": 1,
                "reset_before": False, "call_site": hex(CALL_SITE), "stage3": False,
                "panic_screen": False, "runtime_reached": False}
    for key, value in expected.items():
        require(key in checkpoint and checkpoint[key] == value,
                "Build record misstates the first scale_setup-return checkpoint: " + key)
    require(build.get("kernel_checkpoint_target_changed") is True and
            build.get("firmware_reused_from_revision") == "r9" and
            build.get("kernel_rebuilt") is False and build.get("firmware_rebuilt") is False,
            "Wrong r11 build scope")
    require(build.get("menu_default") == "xnu" and build.get("diagnostic_hold") is True and
            build.get("xnu_requires_manual_confirmation") is True and build.get("automatic_xnu_boot") is False,
            "r11 default XNU focus must retain manual confirmation and diagnostic hold")
    return build


def verify(root, expected_fd_sha):
    require(expected_fd_sha == FD_SHA, "Explicit firmware pin must match the reviewed menu-ownership FD")
    root = root.resolve(strict=True)
    manifest = load_json(root / "manifest.json")
    required = {"boot.img", "boot.json", "payload.img", "payload.json", "payload.official.json",
                "unixnu.toml", "original-kernel.macho", "baseline.adt", "SILICIUM_UEFI.fd",
                "firmware.dtb", "shim.bin", "board.toml", "fd-provenance.json", "fd-source.json",
                "fd-archive-manifest.json", "build-result.json", "r8-baseline.adt"}
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
    r8_adt = (root / "r8-baseline.adt").read_bytes()
    r8_adt_comparison = compare_r8_adt(baseline_adt, r8_adt, adt)
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
    require(build.get("adt_baseline_comparison") == adt_comparison and
            build.get("r8_adt_comparison") == r8_adt_comparison,
            "Build ADT comparison metadata differs from actual bytes")
    expected = {"image_sha256": sha256(data), "image_bytes": len(data), "ramdisk_bytes": len(ramdisk),
                "header_version": 2, "kernel_address": hex(ka), "tags_address": hex(tags), "dtb_address": hex(da),
                "dtb_file_offset": dp, "kernel_sha256": sha256(kernel), "fd_sha256": sha256(fd),
                "dtb_sha256": sha256(dtb), "dtb_bytes": len(dtb), "shim_bytes": len(shim), "hardware_tested": False}
    boot = load_json(root / "boot.json")
    for key, value in expected.items():
        require(boot.get(key) == value, "boot.json mismatch: " + key)
    return {"result": "passed", "scope": "Static r11 first-bootstrap-scale-return checkpoint and retained r9 firmware verification",
            "image": expected, "payload_sha256": sha256(ramdisk), "payload_crc32": f"{zlib.crc32(ramdisk):08x}",
            "payload_bytes": len(ramdisk), "menu_title": cfg["ui"]["title"], "menu_default": "xnu",
            "menu_entry_ids": [e["id"] for e in cfg["entry"]], "fat": geometry, "adt": adt_report,
            "adt_baseline_comparison": adt_comparison, "r8_adt_comparison": r8_adt_comparison, "diagnostic_kernel": diagnostic, "firmware": firmware,
            "manifest_entries_verified": len(manifest["outputs"]), "hardware_tested": False, "deployment_ready": False,
            "independently_expected_kernel_sha256": EXPECTED_KERNEL_SHA,
            "files": {name: {"bytes": len(raw), "sha256": sha256(raw)} for name, raw in files.items()},
            "menu_rendering_hardware_tested": False, "kernel_checkpoint_target_changed": True,
            "firmware_reused_from_revision": "r9", "xnu_requires_manual_confirmation": True, "automatic_xnu_boot": False,
            "limits": ["The retained r9 FD has separate runtime observations; this r11 image remains hardware-unqualified.",
                       "Only the XNU diagnostic callsite changes; official ADT generation may vary only permitted random-seed bytes.",
                       "A timed reset supports the first scale_setup return inside kernel_bootstrap, not completion of bootstrap.",
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
