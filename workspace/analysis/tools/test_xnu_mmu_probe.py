#!/usr/bin/env python3
"""Read-only adversarial checks of a real, already generated r2 candidate.

Mutations are made only to in-memory copies. This cannot build a candidate,
change an input artifact or establish any device-execution result.
"""
import argparse
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
from verify_xnu_mmu_probe import probe_readback, read_fat32, sha256, require
from verify_first_payload import macho_read, file_offset


def test(artifact):
    original = (artifact / "original-kernel.macho").read_bytes()
    payload = (artifact / "payload.img").read_bytes()
    kernel = read_fat32(payload)[0]["efi/unixnu/payloads/kernel.macho"]
    originals = {"original": sha256(original), "kernel": sha256(kernel), "payload": sha256(payload)}
    passed = probe_readback(original, kernel)
    image = macho_read(original)
    entry = image["entry"]
    cave1, cave2, cave3 = entry + 0x78, entry + 0xd8, entry + 0x138
    mmu, post = int(passed["mmu_site"], 16), int(passed["post_mmu_site"], 16)
    panic = image["symbols"]["_panic"]
    common = image["symbols"]["_panic_trap_to_debugger"]
    failures = []

    # Each corruption addresses a different false-positive risk: wrong marker,
    # bad handoff, skipped timer/reset, hidden extra code, or an unbounded edit.
    cases = [
        ("wrong_stage1_color", cave1 + 40, None),
        ("missing_displaced_sctlr", cave2 + 60, struct.pack("<I", 0xd503201f)),
        ("missing_post_mmu_hook", post, struct.pack("<I", 0xd2a00600)),
        ("wrong_displaced_mov", cave3, struct.pack("<I", 0xd2800000)),
        ("counter_read_removed", cave3 + 4, struct.pack("<I", 0xd503201f)),
        ("delay_loop_bypassed", cave3 + 28, struct.pack("<I", 0xd503201f)),
        ("reset_conduit_changed", cave3 + 52, struct.pack("<I", 0xd4000002)),
        ("wrong_delay_literal", cave3 + 64, struct.pack("<Q", 19200000 * 5)),
        ("hidden_cave_tail_code", cave3 + 72, struct.pack("<I", 0xd503201f)),
        ("forbidden_panic_hook", panic, struct.pack("<I", 0x14000001)),
        ("forbidden_common_panic_hook", common, struct.pack("<I", 0x14000001)),
        ("forbidden_extra_padding_injection", cave3 + 96, struct.pack("<I", 0xd503201f)),
        ("extra_hook_outside_allowed_ranges", image["symbols"]["_start_first_cpu"] + 16, None),
        ("changed_isb_outside_hooks", mmu + 4, struct.pack("<I", 0xd503201f)),
    ]
    for name, address, replacement in cases:
        modified = bytearray(kernel)
        offset = file_offset(image, address, len(replacement) if replacement else 1)
        if replacement is None:
            modified[offset] ^= 1
        else:
            require(modified[offset:offset + len(replacement)] != replacement, "Mutation would be a no-op")
            modified[offset:offset + len(replacement)] = replacement
        try:
            probe_readback(original, modified)
        except ValueError as error:
            failures.append({"case": name, "rejected": True, "reason": str(error)})
        else:
            raise ValueError("Unsafe mutation was accepted: " + name)
    require(sha256(original) == originals["original"] and sha256(kernel) == originals["kernel"] and
            sha256(payload) == originals["payload"], "A retained input changed during mutation testing")
    require(sha256((artifact / "original-kernel.macho").read_bytes()) == originals["original"] and
            sha256((artifact / "payload.img").read_bytes()) == originals["payload"], "Artifact bytes changed")
    return {"result": "passed", "scope": "Real candidate read-only verifier rejection tests",
            "baseline_passed": True, "rejected_mutations": failures, "count": len(failures),
            "original_sha256": originals["original"], "kernel_sha256": originals["kernel"],
            "payload_sha256": originals["payload"], "inputs_modified": False, "hardware_tested": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    print(json.dumps(test(args.artifact.resolve(strict=True)), indent=2))
