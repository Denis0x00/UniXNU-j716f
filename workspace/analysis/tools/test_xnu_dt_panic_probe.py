#!/usr/bin/env python3
"""Read-only adversarial checks of an already generated DT panic fix candidate.

Only in-memory copies are mutated. This does not build, patch, overwrite or
qualify an artifact or establish any device execution result.
"""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys
import tomllib

sys.dont_write_bytecode = True
from verify_xnu_dt_panic_probe import (probe_readback, read_fat32, sha256, require,
                                   validate_menu, validate_board, verify,
                                   validate_footer_record, load_json, checked_relative,
                                   OLD_R4_CALL_SITE, OLD_R5_CALL_SITE,
                                   ARM_VM_CALL_SITE, KPRINTF_CALL_SITE,
                                   CALLEE_RET, ORIGINAL_CALL_WORD)
from verify_first_payload import macho_read, file_offset, branch


OLD_KERNEL_SHA = "62215d029ca1b96f5c140abf2fb435e97da8b3ae3574ef10b568a0b8a980f3e8"


def test(artifact, expected_fd_sha, old_r4_artifact, old_r5_artifact):
    baseline = verify(artifact, expected_fd_sha)
    original = (artifact / "original-kernel.macho").read_bytes()
    payload = (artifact / "payload.img").read_bytes()
    files, _ = read_fat32(payload)
    kernel = files["efi/unixnu/payloads/kernel.macho"]
    menu = files["efi/unixnu/unixnu.toml"]
    board = tomllib.loads((artifact / "board.toml").read_text(encoding="utf-8"))
    originals = {"original": sha256(original), "kernel": sha256(kernel), "payload": sha256(payload)}
    passed = baseline["diagnostic_kernel"]
    image = macho_read(original)
    entry = image["entry"]
    cave1, cave2, cave3, cave4 = entry + 0x78, entry + 0xd8, entry + 0x138, entry + 0x198
    mmu, post, call = (int(passed[k], 16) for k in ("mmu_site", "post_mmu_site", "call_site"))
    callee = int(passed["callee_va"], 16)
    failures = []
    old_input_hashes = {}

    def reject(name, action):
        try:
            action()
        except ValueError as error:
            failures.append({"case": name, "rejected": True, "reason": str(error)})
        else:
            raise ValueError("Unsafe mutation was accepted: " + name)

    # Read actual retained predecessors. A fabricated baseline cannot satisfy
    # this cross-generation rejection test, and none of these files is edited.
    for label, old_root in (("r4", old_r4_artifact), ("r5", old_r5_artifact)):
        original_path, payload_path = old_root / "original-kernel.macho", old_root / "payload.img"
        old_original, old_payload = original_path.read_bytes(), payload_path.read_bytes()
        require(sha256(old_original) == OLD_KERNEL_SHA, "Unrecognized retained old-kernel input")
        old_files, _ = read_fat32(old_payload)
        old_kernel = old_files["efi/unixnu/payloads/kernel.macho"]
        require(old_kernel != old_original, "Expected a real previous diagnostic payload")
        old_input_hashes[str(original_path)] = sha256(old_original)
        old_input_hashes[str(payload_path)] = sha256(old_payload)
        reject("old_" + label + "_kernel_used_as_new_baseline", lambda: probe_readback(old_original, old_kernel))
        reject("old_" + label + "_patched_kernel_on_new_baseline", lambda: probe_readback(original, old_kernel))

    # Each mutation targets a distinct possible false positive, especially a
    # stale stage 3 reset that would prevent the selected C call from running.
    cases = [
        ("wrong_stage1_color", cave1 + 40, None),
        ("missing_displaced_sctlr", cave2 + 60, struct.pack("<I", 0xd503201f)),
        ("stale_stage3_hook", post, struct.pack("<I", branch(post, cave3))),
        ("stale_stage3_cave", cave3, struct.pack("<I", 0xd2a00600)),
        ("old_r4_address_hook", OLD_R4_CALL_SITE, struct.pack("<I", branch(OLD_R4_CALL_SITE, cave4))),
        ("old_r5_address_hook", OLD_R5_CALL_SITE, struct.pack("<I", branch(OLD_R5_CALL_SITE, cave4))),
        ("new_arm_vm_init_early_reset_hook", ARM_VM_CALL_SITE, struct.pack("<I", branch(ARM_VM_CALL_SITE, cave4))),
        ("new_kprintf_early_reset_hook", KPRINTF_CALL_SITE, struct.pack("<I", branch(KPRINTF_CALL_SITE, cave4))),
        ("changed_kprintf_argument", KPRINTF_CALL_SITE - 4, struct.pack("<I", 0x528000a0)),
        ("missing_stage4_hook", call, struct.pack("<I", ORIGINAL_CALL_WORD)),
        ("stage4_hook_calls_cave_instead_of_branch", call, struct.pack("<I", branch(call, cave4) | 0x80000000)),
        ("stage4_callee_branch_loses_return_address", cave4, struct.pack("<I", branch(cave4, callee))),
        ("wrong_stage4_callee", cave4, struct.pack("<I", branch(cave4, callee + 4) | 0x80000000)),
        ("changed_fiq_argument", call - 4, struct.pack("<I", 0x52800000)),
        ("callee_normal_ret_removed", CALLEE_RET, struct.pack("<I", 0xd503201f)),
        ("reset_before_skips_callee", cave4, struct.pack("<I", 0xd503201f)),
        ("counter_read_removed", cave4 + 4, struct.pack("<I", 0xd503201f)),
        ("timer_literal_address_changed", cave4 + 8, struct.pack("<I", 0x580001e2)),
        ("delay_loop_bypassed", cave4 + 28, struct.pack("<I", 0xd503201f)),
        ("reset_function_changed", cave4 + 32, struct.pack("<I", 0x52800100)),
        ("reset_conduit_changed", cave4 + 52, struct.pack("<I", 0xd4000002)),
        ("reset_return_does_not_park", cave4 + 60, struct.pack("<I", 0xd65f03c0)),
        ("wrong_delay_literal", cave4 + 64, struct.pack("<Q", 19200000 * 5)),
        ("hidden_cave_tail_code", cave4 + 72, struct.pack("<I", 0xd503201f)),
        ("forbidden_panic_hook", image["symbols"]["_panic"], struct.pack("<I", 0x14000001)),
        ("forbidden_common_panic_hook", image["symbols"]["_panic_trap_to_debugger"], struct.pack("<I", 0x14000001)),
        ("forbidden_extra_padding_injection", cave4 + 96, struct.pack("<I", 0xd503201f)),
        ("extra_hook_outside_allowed_ranges", image["symbols"]["_start_first_cpu"] + 16, None),
        ("changed_isb_outside_hooks", mmu + 4, struct.pack("<I", 0xd503201f)),
        ("changed_caller_after_original_call", call + 4, struct.pack("<I", 0xd503201f)),
    ]
    for name, address, replacement in cases:
        modified = bytearray(kernel)
        offset = file_offset(image, address, len(replacement) if replacement else 1)
        if replacement is None:
            modified[offset] ^= 1
        else:
            require(modified[offset:offset + len(replacement)] != replacement, "Mutation would be a no-op")
            modified[offset:offset + len(replacement)] = replacement
        reject(name, lambda: probe_readback(original, modified))

    modified = bytearray(kernel)
    modified[4] ^= 1  # CPU type in the Mach-O header.
    reject("changed_kernel_metadata", lambda: probe_readback(original, modified))
    reject("truncated_kernel", lambda: probe_readback(original, kernel[:-4]))
    modified = bytearray(kernel)
    modified[-1] ^= 1
    reject("changed_kernel_tail", lambda: probe_readback(original, modified))

    menu_changes = [
        ("stale_r4_menu_title", b"DT panic fix r6", b"next init checkpoint r4"),
        ("stale_r5_menu_title", b"DT panic fix r6", b"mid init checkpoint r5"),
        ("stale_xnu_entry", b"Start XNU (DT panic fix checkpoint)", b"Start XNU (next init checkpoint)"),
        ("automatic_xnu_selection", b'default = "tools"', b'default = "xnu"'),
    ]
    for name, old, new in menu_changes:
        require(menu.count(old) == 1, "Expected exactly one menu mutation target")
        reject(name, lambda: validate_menu(menu.replace(old, new)))

    board_changes = [
        ("wrong_callee_setting", "entry_probe_reset_after", "_PE_init_platform"),
        ("wrong_caller_setting", "entry_probe_reset_in", "_arm_start"),
        ("wrong_occurrence_setting", "entry_probe_reset_occurrence", 2),
        ("wrong_delay_setting", "entry_probe_reset_delay", 5),
        ("reset_before_setting", "entry_probe_reset_before", True),
        ("panic_injection_setting", "panic_screen", True),
    ]
    for name, key, value in board_changes:
        modified_board = copy.deepcopy(board)
        modified_board["payload"][key] = value
        reject(name, lambda: validate_board(modified_board))
    modified_board = copy.deepcopy(board)
    del modified_board["payload"]["entry_probe_reset_after"]
    reject("stage3_fallback_setting", lambda: validate_board(modified_board))

    evidence = load_json(artifact / "fd-provenance.json")["evidence"]
    binary_item = next(item for item in evidence if item["role"] == "binary-verification")
    footer_record = load_json(artifact / "fd-evidence" / checked_relative(binary_item["path"]))
    for name, key, value in (
            ("footer_text_still_present", "select_an_entry_utf16_present", True),
            ("unbounded_menu_source_edit", "menu_source_only_diagnostic_status_line_0_change", False),
            ("diagnostic_status_line_not_skipped", "diagnostic_status_line_0_skipped", False),
            ("diagnostic_line0_call_remains", "diagnostic_line0_status_call_absent", False),
            ("lost_diagnostic_hold", "menu_hold", 0),
            ("qr_libraries_changed", "qr_libraries_preserved", False),
            ("unrelated_firmware_modules_changed", "unrelated_module_hashes_unchanged", False)):
        modified_footer = copy.deepcopy(footer_record)
        modified_footer[key] = value
        reject(name, lambda: validate_footer_record(modified_footer))

    require(sha256(original) == originals["original"] and sha256(kernel) == originals["kernel"] and
            sha256(payload) == originals["payload"], "A retained input changed during mutation testing")
    require(sha256((artifact / "original-kernel.macho").read_bytes()) == originals["original"] and
            sha256((artifact / "payload.img").read_bytes()) == originals["payload"], "Artifact bytes changed")
    for path, digest in old_input_hashes.items():
        require(sha256(Path(path).read_bytes()) == digest, "Retained predecessor input changed")
    return {"result": "passed", "scope": "Real DT panic fix candidate read-only verifier rejection tests",
            "baseline_full_verifier_passed": True, "rejected_mutations": failures, "count": len(failures),
            "original_sha256": originals["original"], "kernel_sha256": originals["kernel"],
            "payload_sha256": originals["payload"], "expected_fd_sha256": expected_fd_sha,
            "retained_predecessor_inputs": old_input_hashes,
            "inputs_modified": False, "hardware_tested": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact", type=Path)
    parser.add_argument("--fd-sha256", required=True, help="Reviewed new firmware pin supplied independently of the artifact")
    parser.add_argument("--old-r4-artifact", type=Path, required=True, help="Retained real r4 artifact for cross-generation rejection")
    parser.add_argument("--old-r5-artifact", type=Path, required=True, help="Retained real r5 artifact for cross-generation rejection")
    args = parser.parse_args()
    print(json.dumps(test(args.artifact.resolve(strict=True), args.fd_sha256,
                          args.old_r4_artifact.resolve(strict=True),
                          args.old_r5_artifact.resolve(strict=True)), indent=2))
