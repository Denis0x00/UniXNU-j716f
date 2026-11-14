#!/usr/bin/env python3
"""Focused r21 target and source-build scope rejection checks; never builds or accesses devices."""
import argparse
import copy
import json
from pathlib import Path
import sys
import tomllib

sys.dont_write_bytecode = True
import verify_xnu_text_console_probe as v


def run(target, artifact=None):
    cases = []

    def accept(name, operation):
        operation()
        cases.append({"name": name, "accepted": True})

    def reject(name, operation):
        try:
            operation()
        except (ValueError, AssertionError, KeyError, tomllib.TOMLDecodeError) as error:
            cases.append({"name": name, "rejected": True, "reason": str(error)})
        else:
            raise AssertionError("Accepted invalid input: " + name)

    raw = (target / "payload/unixnu.toml").read_bytes()
    board = tomllib.loads((target / "board.toml").read_text(encoding="utf-8"))
    accept("r21 five-entry manual menu", lambda: v.validate_menu(raw))
    accept("r21 board retains InitIOKit return and memory policy", lambda: v.validate_board(board))
    reject("old r20 title", lambda: v.validate_menu(raw.replace(v.TITLE.encode(), b"J716F XNU IOKit core r20")))
    reject("automatic XNU timeout", lambda: v.validate_menu(raw.replace(b"timeout = -1", b"timeout = 0")))
    reject("changed serial boot argument", lambda: v.validate_menu(raw.replace(b"serial=3", b"serial=0")))
    for label, section, key, value in (
        ("old outer PE checkpoint", "payload", "entry_probe_reset_after", "_PE_init_iokit"),
        ("wrong caller", "payload", "entry_probe_reset_in", "_kernel_bootstrap_thread"),
        ("reset before InitIOKit", "payload", "entry_probe_reset_before", True),
        ("different call occurrence", "payload", "entry_probe_reset_occurrence", 2),
        ("panic screen enabled", "payload", "panic_screen", True),
        ("old target identity", "board", "name", "j716f-xnu-bootstrap-iokit-core-probe"),
        ("oversize advertised boot capacity", "package", "boot_partition_bytes", 100663297),
    ):
        bad = copy.deepcopy(board)
        bad[section][key] = value
        reject(label, lambda bad=bad: v.validate_board(bad))
    bad = copy.deepcopy(board)
    bad["payload"]["adt"]["owned_memory_size"] = 0x20000000
    reject("unapproved memory expansion", lambda: v.validate_board(bad))

    if artifact is not None:
        expectation = v.load_expectation(artifact / "kernel-expectation.json")
        report = v.verify(artifact, v.FD_SHA)
        build = json.loads((artifact / "build-result.json").read_text(encoding="utf-8"))
        accept("actual new artifact and independent kernel", lambda: v.require(report["result"] == "passed", "Container failed"))
        accept("actual r21 source-build record", lambda: v.validate_build_record(build, expectation))
        for key, value in (("kernel_rebuilt", False), ("kernel_rebuilt_by_this_command", True),
                           ("kernel_baseline_changed", False), ("kernel_checkpoint_target_changed", True),
                           ("firmware_rebuilt", True), ("hardware_tested", True),
                           ("automatic_xnu_boot", True)):
            bad = copy.deepcopy(build)
            bad[key] = value
            reject("misstated build scope: " + key, lambda bad=bad: v.validate_build_record(bad, expectation))
        bad = copy.deepcopy(build)
        bad["reset_checkpoint"]["call_site"] = "0x0"
        reject("unreviewed callsite in build metadata", lambda: v.validate_build_record(bad, expectation))
    else:
        report = None

    return {"status": "passed", "scope": "r21 target policy and build-record checks",
            "positive_checks": sum(c.get("accepted", False) for c in cases),
            "negative_checks": sum(c.get("rejected", False) for c in cases), "cases": cases,
            "artifact_verification_run": artifact is not None,
            "artifact_image_sha256": report["image"]["image_sha256"] if report else None,
            "kernel_build_or_runtime_inferred_from_target_checks": False, "hardware_tested": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--target", type=Path, required=True)
    parser.add_argument("--artifact", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        parser.error("Refusing to overwrite report")
    result = run(args.target.resolve(strict=True), args.artifact.resolve(strict=True) if args.artifact else None)
    with args.report.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps({k: val for k, val in result.items() if k != "cases"}, indent=2))
