#!/usr/bin/env python3
"""Focused r24 retained-ADT and manual-menu policy checks; no device access."""
import argparse
import copy
import json
from pathlib import Path
import sys
import tomllib

sys.dont_write_bytecode = True
import verify_xnu_configure_iokit_probe as v
from test_xnu_adt_phandle_probe import encode


def run(root, artifact=None):
    target = root / "work/UniXNU-j716f-xnu-configure-iokit-probe"
    baseline_files, _ = v.read_fat32((root / "artifacts/j716f-xnu-adt-phandle-probe-20261114-r23/payload.img").read_bytes())
    baseline = baseline_files["efi/unixnu/payloads/kernel.macho.adt"]
    before = v.adt_read(baseline)
    v.require(encode(before) == baseline, "Test encoder does not preserve actual r23 ADT")
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

    accept("actual r23 ADT unchanged", lambda: v.compare_r23_adt(baseline, baseline))
    changed = copy.deepcopy(before)
    changed["/chosen"]["random-seed"] = bytes(range(256))
    accept("only generated random seed differs", lambda: v.compare_r23_adt(baseline, encode(changed)))
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
        reject(name, lambda changed=changed: v.compare_r23_adt(baseline, encode(changed)))
    menu = (target / "payload/unixnu.toml").read_bytes()
    board = tomllib.loads((target / "board.toml").read_text())
    accept("five-entry manual r24 menu", lambda: v.validate_menu(menu))
    accept("ConfigureIOKit after-return checkpoint", lambda: v.validate_board(board))
    reject("automatic XNU launch", lambda: v.validate_menu(menu.replace(b"timeout = -1", b"timeout = 0")))
    bad = copy.deepcopy(board)
    bad["payload"]["entry_probe_reset_after"] = "_InitIOKit"
    reject("old InitIOKit checkpoint", lambda: v.validate_board(bad))
    bad = copy.deepcopy(board)
    bad["payload"]["entry_probe_reset_before"] = True
    reject("before ConfigureIOKit checkpoint", lambda: v.validate_board(bad))
    if artifact is not None:
        actual, _ = v.read_fat32((artifact / "payload.img").read_bytes())
        accept("actual generated ADT differs only in seed", lambda: v.compare_r23_adt(baseline, actual["efi/unixnu/payloads/kernel.macho.adt"]))
    return {"status": "passed", "scope": "r24 retained ADT and manual menu policy",
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
