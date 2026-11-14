#!/usr/bin/env python3
"""Offline TB-J716F Stage 1/2 artifact verification; Python 3.11+, stdlib only.

Usage: python verify_menu_artifact.py ARTIFACT_DIRECTORY
Required files: boot.img, boot.json, SILICIUM_UEFI.fd, firmware.dtb, shim.bin,
uefi-config.toml, board.toml, manifest.json. The manifest uses the upstream
``outputs: {relative_name: {bytes: integer, sha256: hex}}`` shape and must cover
all seven input/output files (not itself). Extra listed files are also checked.

shim.bin must be compiled independently from the recorded BootShim.S and the
same FD parameters, e.g. with upstream compile_shim; do not extract it from the
image under test. SILICIUM_UEFI.fd is the complete Build/.../FV output, not an
intermediate FVMAIN_COMPACT.Fv. firmware.dtb is the selected normalized DTB.

No upstream verifier is imported: header offsets, alignment, Android image ID,
gzip framing and byte comparisons are independently checked here. A manifest
is an integrity inventory, not authenticated provenance. This does not verify
machine code correctness, embedded FV modules, ABL handoff or hardware boot.
The script only reads inputs and prints JSON; it never changes an artifact.
"""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import sys
import tomllib
import zlib

REQUIRED = {"boot.img", "boot.json", "SILICIUM_UEFI.fd", "firmware.dtb",
            "shim.bin", "uefi-config.toml", "board.toml"}
DTB_SHA256 = "6c87f77599a2056befac0080718d826205afed462b9abacac7aa0664e4d3e6e3"
BOOT_CAPACITY = 100663296
FD_BYTES = 0x700000


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def load_json(path):
    # Duplicate keys must not silently hide a manifest entry or a report value.
    def unique(pairs):
        result = {}
        for key, value in pairs:
            require(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result
    return json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)


def verify(directory):
    root = directory.resolve(strict=True)
    require(root.is_dir(), "artifact path must be a directory")
    manifest = load_json(root / "manifest.json")
    entries = manifest.get("outputs")
    require(isinstance(entries, dict) and REQUIRED <= entries.keys(),
            "manifest.outputs must cover: " + ", ".join(sorted(REQUIRED)))
    checked = {}
    for name, spec in entries.items():
        relative = PurePosixPath(name)
        require(name and "\\" not in name and ":" not in name and
                not relative.is_absolute() and ".." not in relative.parts,
                f"manifest path must be artifact-relative: {name}")
        path = (root / name).resolve(strict=True)
        require(path.is_relative_to(root) and path.is_file(), f"invalid artifact path: {name}")
        require(isinstance(spec, dict) and type(spec.get("bytes")) is int and
                isinstance(spec.get("sha256"), str) and
                re.fullmatch(r"[0-9a-fA-F]{64}", spec["sha256"]), f"invalid manifest entry: {name}")
        require(path.stat().st_size == spec["bytes"], f"manifest size mismatch: {name}")
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        require(digest == spec["sha256"].lower(), f"manifest hash mismatch: {name}")
        checked[name] = {"bytes": spec["bytes"], "sha256": digest}

    require(1660 <= checked["boot.img"]["bytes"] <= BOOT_CAPACITY,
            "boot.img is truncated or exceeds the 96 MiB boot capacity")
    require(checked["SILICIUM_UEFI.fd"]["bytes"] == FD_BYTES, "FD must be exactly 0x700000 bytes")
    require(64 <= checked["shim.bin"]["bytes"] <= 65536, "unexpected shim size")
    require(checked["firmware.dtb"]["sha256"] == DTB_SHA256,
            "firmware.dtb differs from the reviewed normalized DTB0+DTBO1 input")
    data = (root / "boot.img").read_bytes()
    fd = (root / "SILICIUM_UEFI.fd").read_bytes()
    shim = (root / "shim.bin").read_bytes()
    dtb = (root / "firmware.dtb").read_bytes()
    report = load_json(root / "boot.json")
    config = tomllib.loads((root / "uefi-config.toml").read_text(encoding="utf-8"))
    board = tomllib.loads((root / "board.toml").read_text(encoding="utf-8"))
    image = {"base": 0, "kernel_offset": 0x8000, "tags_offset": 0x1e00000,
             "dtb_offset": 0x1f00000, "pagesize": 4096, "header_version": 0}
    image.update(config.get("boot_image", {}))
    image.update(board.get("bootimg", {}).get("image", {}))
    require(image["header_version"] == 2 and image["pagesize"] == 4096,
            "this verifier requires the selected 4096-byte-page boot v2 profile")
    require(str(image.get("kernel_compression", "gzip")).lower() == "gzip",
            "this verifier requires the gzip menu build")
    require(not board.get("bootimg", {}).get("dtb_node"),
            "DTB node transformations require a separately reviewed verifier input")
    require(config["uefi_fd"]["size"] == FD_BYTES and
            config["boot_shim"]["requires_kernel_header"] is True,
            "unexpected FD/shim configuration")
    require(board["package"]["boot_partition_bytes"] == BOOT_CAPACITY,
            "board boot capacity differs from the recorded device geometry")

    require(data[:8] == b"ANDROID!", "Android image magic missing")
    kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr, tags, page, version, _ = struct.unpack_from("<10I", data, 8)
    recovery_size, recovery_offset, header_size = struct.unpack_from("<IQI", data, 1632)
    dtb_size, dtb_addr = struct.unpack_from("<IQ", data, 1648)
    require(version == 2 and page == 4096 and header_size == 1660, "invalid v2 header/page size")
    require((ramdisk_size, ramdisk_addr, second_size, second_addr, recovery_size, recovery_offset) == (0, 0, 0, 0, 0, 0),
            "menu image must have no ramdisk, second stage or recovery DTBO")
    require(kernel_size > 0 and kernel_addr == image["base"] + image["kernel_offset"] and
            tags == image["base"] + image["tags_offset"] and
            dtb_addr == image["base"] + image["dtb_offset"], "image load addresses differ from inputs")
    aligned = lambda size: (size + page - 1) // page * page
    dtb_offset = page + aligned(kernel_size)
    require(dtb_size == len(dtb) and len(data) == dtb_offset + aligned(dtb_size),
            "DTB size, image end or page alignment differs from the expected layout")
    require(not any(data[header_size:page]) and not any(data[page + kernel_size:dtb_offset]) and
            not any(data[dtb_offset + dtb_size:]), "nonzero boot section padding")
    require(len(dtb) >= 40 and dtb[:4] == b"\xd0\x0d\xfe\xed" and
            struct.unpack_from(">I", dtb, 4)[0] == len(dtb), "invalid input FDT header")
    require(data[dtb_offset:dtb_offset + dtb_size] == dtb, "independent v2 DTB differs from input")

    kernel = data[page:page + kernel_size]
    combined = shim + fd
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    unpacked = decoder.decompress(kernel, len(combined) + 1)
    require(decoder.eof and not decoder.unconsumed_tail and unpacked == combined,
            "gzip does not decode to the independently supplied shim + FD")
    require(decoder.unused_data == dtb, "kernel-tail DTB differs from independent DTB/input")
    require(shim[56:60] == b"ARM\x64" and len(shim) % 16 == 0 and
            struct.unpack_from("<QQ", shim, 8) == (config["uefi_fd"]["base"], len(combined)),
            "shim ARM64 header disagrees with configured FD base/combined size")
    # Android v2 image ID: each section's bytes followed by its uint32 size.
    image_id = hashlib.sha1()
    for section in (kernel, b"", b"", b"", dtb):
        image_id.update(section)
        image_id.update(struct.pack("<I", len(section)))
    require(data[576:608] == image_id.digest() + b"\0" * 12, "Android v2 image ID mismatch")

    expected = {"image_sha256": sha256(data), "image_bytes": len(data),
                "fd_sha256": sha256(fd), "dtb_sha256": sha256(dtb), "shim_bytes": len(shim),
                "kernel_sha256": sha256(kernel), "ramdisk_bytes": 0, "header_version": 2,
                "kernel_address": hex(kernel_addr), "tags_address": hex(tags),
                "dtb_bytes": dtb_size, "dtb_address": hex(dtb_addr), "dtb_file_offset": dtb_offset}
    for key, value in expected.items():
        require(report.get(key) == value, f"boot.json mismatch: {key}")
    require(report.get("hardware_tested") is False, "boot.json must not claim hardware testing")
    return {"result": "passed", "scope": "offline menu artifact integrity/layout only",
            "hardware_tested": False, "image": expected, "manifest_verified": checked,
            "limits": ["Manifest hashes do not authenticate source provenance.",
                       "FD module contents, machine code correctness and hardware handoff are not verified."]}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("artifact_directory", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(verify(args.artifact_directory), indent=2))
    except (OSError, ValueError, KeyError, TypeError, struct.error, zlib.error) as error:
        print(json.dumps({"result": "failed", "error": str(error), "hardware_tested": False}), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
