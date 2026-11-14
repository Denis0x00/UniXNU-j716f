#!/usr/bin/env python3
"""Verify r7's container and exact before-machine_startup kernel, without device access.

The frozen r6 container/FAT/ADT readers are reused, but r6's after-call probe is
never accepted here. The extracted kernel must equal the separately accepted
before-machine-startup candidate and independently satisfy its exact hook/cave
contract. This is an immediate reset boundary, not a six-second reset probe.
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
    baseline_layout, verify_footer_provenance, KERNEL_SHA, KERNEL_BYTES,
    FD_SHA, DTB_SHA, SHIM_SHA, BASELINE_ADT_SHA, FAT_BYTES,
)
from prepare_r6_followup import MachO, verify_candidate, target

DEVICE = "j716f-xnu-machine-boundary-probe"
DEFCONFIG = "j716f_xnu_machine_boundary_probe_defconfig"
TITLE = "J716F XNU machine boundary r7"
XNU_TITLE = "Start XNU (before machine_startup)"
CALLER_NAME, CALLEE_NAME = "_arm_init", "_machine_startup"
CALLER_VA, CALLER_END = 0xfffffff0079e00c4, 0xfffffff0079e0b94
CALLEE_VA, CALL_SITE = 0xfffffff00741bd30, 0xfffffff0079e0a70
CANDIDATE_SHA = "9f608afa24693deb6b84dcf6a29e5ae0374f2cf89829bbe22c7dd53dc9632b47"
DEFAULT_CANDIDATE = (Path(__file__).resolve().parents[2] /
    "analysis/xnu-r6-followup-20261113/candidates-linux/before-machine-startup/kernel.macho")
RESET_SETTINGS = {
    "entry_probe_reset_delay": 6,
    "entry_probe_reset_after": CALLEE_NAME,
    "entry_probe_reset_in": CALLER_NAME,
    "entry_probe_reset_occurrence": 1,
    "entry_probe_reset_before": True,
}


def validate_menu(raw):
    cfg = tomllib.loads(raw.decode("utf-8"))
    require(set(cfg) == {"version", "boot", "ui", "kits", "entry"} and cfg["version"] == 1 and
            cfg["boot"] == {"timeout": -1, "default": "tools", "rotate": 0} and
            cfg["ui"] == {"title": TITLE} and cfg["kits"] == {"autoload": False},
            "Unexpected r7 menu title/default/hold policy")
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
            "Expected only the official before-machine_startup stage 4 configuration")
    require(board["board"]["name"] == DEVICE and board["kernel"]["defconfig"] == DEFCONFIG and
            board["board"].get("os_profile") == "ipados-17.7.11-21H461" and
            board["package"]["boot_partition_bytes"] == 100663296,
            "Wrong machine-boundary target/profile/capacity")
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
    expected = {"stage": 4, "reset_delay_seconds": None, "timebase_hz": None,
                "delay_ticks": None, "wrapper_entry_probe_reset_delay": 6,
                "reset_before": True, "call_site": hex(CALL_SITE), "stage3": False,
                "panic_screen": False, "runtime_reached": False}
    for key, value in expected.items():
        require(key in checkpoint and checkpoint[key] == value,
                "Build record misstates the immediate before-call checkpoint: " + key)
    return build


def probe_readback(original, patched, candidate_raw=None):
    """Check actual instructions, permitted changes, and full candidate equality."""
    baseline_layout(original)  # Pins original bytes and independently checks r6-era layout.
    candidate_raw = DEFAULT_CANDIDATE.read_bytes() if candidate_raw is None else candidate_raw
    require(len(candidate_raw) == KERNEL_BYTES and sha256(candidate_raw) == CANDIDATE_SHA,
            "Wrong independently accepted before-machine-startup reference")
    image = MachO(original)
    require(image.names.get(CALLER_NAME) == CALLER_VA and image.end(CALLER_NAME) == CALLER_END and
            image.names.get(CALLEE_NAME) == CALLEE_VA and
            CALLER_NAME not in image.ambiguous_names and CALLEE_NAME not in image.ambiguous_names,
            "Machine boundary symbols or caller extent changed")
    calls = [va for va in range(CALLER_VA, CALLER_END, 4)
             if image.word(va) & 0xfc000000 == 0x94000000 and
             target(image.word(va), va, 0x94000000) == CALLEE_VA]
    require(calls == [CALL_SITE], "Machine startup call is not unique at the reviewed boundary")
    # This verifier checks exact branches, the eight immediate-reset instructions,
    # all cave padding, preserved functions, and every byte outside allowed spans.
    result = verify_candidate(image, patched, "before-machine-startup")
    require(patched == candidate_raw,
            "Extracted payload kernel differs byte-for-byte from the accepted candidate")
    require(result["changed_bytes"] == 161 and result["runtime_delay_seconds"] is None and
            result["delay_ticks"] is None and result["timebase_hz"] is None,
            "Unexpected before-machine boundary diagnostic semantics")
    # These legacy preparation fields describe a kernel-only helper. They would
    # misdescribe the enclosing payload verification, so do not propagate them.
    for key in ("payload_built", "boot_image_built", "deployed"):
        result.pop(key, None)
    result.update(
        reference_sha256=CANDIDATE_SHA, reference_bytes=KERNEL_BYTES,
        reference_full_bytes_equal=True, caller=CALLER_NAME, callee=CALLEE_NAME,
        call_site=hex(CALL_SITE), callee_va=hex(CALLEE_VA), occurrence=1,
        reset_probe=True, reset_stage=4, reset_before=True,
        machine_startup_called=False, timer_read=False, actual_delay_seconds=0,
        wrapper_entry_probe_reset_delay=6,
        wrapper_delay_semantics="Nonzero stage-4 switch only; RESET_BEFORE ignores the delay",
        scope="Exact before-call reset boundary; no machine_startup execution or official Stage 5 completion inferred",
    )
    return result


def verify(root, expected_fd_sha, candidate_kernel=None):
    require(expected_fd_sha == FD_SHA, "Explicit firmware pin must match the retained r4/r6 FD")
    root = root.resolve(strict=True)
    candidate_path = Path(candidate_kernel) if candidate_kernel is not None else DEFAULT_CANDIDATE
    candidate_path = candidate_path.resolve(strict=True)
    candidate_raw = candidate_path.read_bytes()
    require(len(candidate_raw) == KERNEL_BYTES and sha256(candidate_raw) == CANDIDATE_SHA,
            "Wrong independently accepted candidate file")
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
    diagnostic = probe_readback(original, xnu, candidate_raw)
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
    return {"result": "passed", "scope": "Static r7 before-machine_startup immediate-reset boundary verification",
            "image": expected, "payload_sha256": sha256(ramdisk), "payload_crc32": f"{zlib.crc32(ramdisk):08x}",
            "payload_bytes": len(ramdisk), "menu_title": cfg["ui"]["title"], "menu_default": "tools",
            "menu_entry_ids": [e["id"] for e in cfg["entry"]], "fat": geometry, "adt": adt_report,
            "adt_baseline_comparison": adt_comparison, "diagnostic_kernel": diagnostic, "firmware": firmware,
            "manifest_entries_verified": len(manifest["outputs"]), "hardware_tested": False, "deployment_ready": False,
            "candidate_reference": {"sha256": CANDIDATE_SHA, "bytes": KERNEL_BYTES, "full_bytes_equal": True},
            "files": {name: {"bytes": len(raw), "sha256": sha256(raw)} for name, raw in files.items()},
            "limits": ["The checkpoint resets before calling machine_startup; no machine_startup execution is claimed.",
                       "There is no six-second runtime wait: wrapper delay=6 only enables stage 4.",
                       "An arbitrary quick reboot is not independently sufficient to prove the checkpoint was reached.",
                       "Probe stage 4 is not official bring-up Stage 4; official Stage 5 completion remains unproved.",
                       "No GIC, timer IRQ, IOKit, root or userland execution is inferred from host bytes.",
                       "Host CRC is optional correlation evidence, not a renewed requirement for full QR export."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--fd-sha256", required=True)
    parser.add_argument("--candidate-kernel", type=Path, default=DEFAULT_CANDIDATE)
    args = parser.parse_args()
    require(not args.report.exists(), "Refusing to overwrite a report")
    result = verify(args.artifact, args.fd_sha256, args.candidate_kernel)
    with args.report.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(json.dumps(result, indent=2))
