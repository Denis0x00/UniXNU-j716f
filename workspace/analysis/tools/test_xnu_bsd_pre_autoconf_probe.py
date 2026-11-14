#!/usr/bin/env python3
"""Focused r28 retained-ADT and manual-menu policy checks; no device access."""
import argparse
import copy
import json
from pathlib import Path
import sys
import tomllib

sys.dont_write_bytecode = True
import verify_xnu_bsd_pre_autoconf_probe as v
from test_xnu_adt_phandle_probe import encode


def run(root, artifact=None):
    target = root / "work/UniXNU-j716f-xnu-bsd-pre-autoconf-probe"
    baseline_files, _ = v.read_fat32((root / "artifacts/j716f-xnu-cpu-wait-probe-20261114-r27/payload.img").read_bytes())
    baseline = baseline_files["efi/unixnu/payloads/kernel.macho.adt"]
    before = v.adt_read(baseline)
    v.require(encode(before) == baseline, "Test encoder does not preserve actual r27 ADT")
    cases = []

    def accept(name, operation):
        operation()
        cases.append({"name": name, "accepted": True})

    def reject(name, operation):
        try:
            operation()
        except (ValueError, AssertionError, KeyError, tomllib.TOMLDecodeError):
            cases.append({"name": name, "rejected": True})
        else:
            raise AssertionError("Accepted invalid input: " + name)

    accept("actual r27 ADT unchanged", lambda: v.compare_r27_adt(baseline, baseline))
    changed = copy.deepcopy(before)
    changed["/chosen"]["random-seed"] = bytes(range(256))
    accept("only generated random seed differs", lambda: v.compare_r27_adt(baseline, encode(changed)))
    for name, node, prop, value in (
        ("removed corrected Apple phandle", "/arm-io/intc", "AAPL,phandle", None),
        ("changed timer interrupts", "/arm-io/timer", "interrupts", b"\0" * 48),
        ("expanded owned memory", "/chosen", "darwinboot-memory-size", (0x20000000).to_bytes(8, "little")),
        ("unapproved added property", "/arm-io/timer", "new-prop", b"x"),
        ("invalid generated seed", "/chosen", "random-seed", bytes(256)),
    ):
        changed = copy.deepcopy(before)
        if value is None:
            del changed[node][prop]
        else:
            changed[node][prop] = value
        reject(name, lambda changed=changed: v.compare_r27_adt(baseline, encode(changed)))
    menu = (target / "payload/unixnu.toml").read_bytes()
    board = tomllib.loads((target / "board.toml").read_text())
    accept("five-entry manual r28 menu", lambda: v.validate_menu(menu))
    accept("bsd_pre_autoconf after-return checkpoint", lambda: v.validate_board(board))
    reject("automatic XNU launch", lambda: v.validate_menu(menu.replace(b"timeout = -1", b"timeout = 0")))
    reject("unrequested startup_debug", lambda: v.validate_menu(menu.replace(b"j716f-earlylog=1", b"j716f-earlylog=1 startup_debug=1")))
    bad_args = copy.deepcopy(board)
    bad_args["payload"]["kernel_args"].append("debug=0x8")
    reject("unrequested debug argument", lambda: v.validate_board(bad_args))
    bad = copy.deepcopy(board)
    bad["payload"]["entry_probe_reset_after"] = "_kperf_init_early"
    bad["payload"]["entry_probe_reset_in"] = "_kernel_bootstrap_thread"
    reject("old kperf_init_early checkpoint", lambda: v.validate_board(bad))
    bad = copy.deepcopy(board)
    bad["payload"]["entry_probe_reset_before"] = True
    reject("before bsd_pre_autoconf checkpoint", lambda: v.validate_board(bad))
    # The source kernel is reused from accepted r27; this image must not
    # claim a new kernel build or silently select the earlier r22 baseline.
    example_expectation = {"layout": {"call_site": "reviewed-later"}}
    build = {
        "status": "host-generation-passed", "hardware_tested": False, "device_actions": False,
        "kernel_checkpoint_target_changed": True, "kernel_baseline_changed": False,
        "firmware_reused_from_revision": "r22", "source_kernel_reused_from_revision": "r27",
        "source_kernel_baseline": "host-iokit-cpu0", "payload_kernel_reused": False,
        "kernel_rebuilt": False, "kernel_rebuilt_by_this_command": False,
        "firmware_rebuilt": False, "firmware_rebuilt_by_this_command": False,
        "menu_default": "xnu", "diagnostic_hold": True,
        "xnu_requires_manual_confirmation": True, "automatic_xnu_boot": False,
        "reset_checkpoint": {
            "stage": 4, "reset_delay_seconds": 6, "timebase_hz": 19200000,
            "delay_ticks": 115200000, "wrapper_entry_probe_reset_delay": 6,
            "reset_after": v.CALLEE_NAME, "reset_in": v.CALLER_NAME,
            "occurrence": 1, "reset_before": False, "call_site": "reviewed-later",
            "stage3": False, "panic_screen": False, "runtime_reached": False,
        },
    }
    accept("accepted r27 CPU0 source kernel reused without rebuild", lambda: v.validate_build_record(build, example_expectation))
    for key, value in (("source_kernel_reused_from_revision", "r22"),
                       ("source_kernel_baseline", "host-early-log"),
                       ("kernel_baseline_changed", True),
                       ("kernel_rebuilt", True),
                       ("kernel_rebuilt_by_this_command", True),
                       ("hardware_tested", True)):
        invalid = copy.deepcopy(build)
        invalid[key] = value
        reject("invalid source-build claim: " + key,
               lambda invalid=invalid: v.validate_build_record(invalid, example_expectation))
    if artifact is not None:
        actual, _ = v.read_fat32((artifact / "payload.img").read_bytes())
        accept("actual generated ADT differs only in seed", lambda: v.compare_r27_adt(baseline, actual["efi/unixnu/payloads/kernel.macho.adt"]))
    return {"status": "passed", "scope": "r28 retained ADT and manual menu policy",
            "positive_checks": sum(c.get("accepted", False) for c in cases),
            "negative_checks": sum(c.get("rejected", False) for c in cases),
            "cases": cases, "actual_payload_checked": artifact is not None,
            "unchanged_firmware_rejection_suite_repeated": False, "hardware_tested": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--artifact", type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    result = run(args.root.resolve(strict=True), args.artifact)
    with args.report.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps({k: val for k, val in result.items() if k != "cases"}, indent=2))
