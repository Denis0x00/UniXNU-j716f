#!/usr/bin/env python3
"""Focused r23 ADT/reference/menu contract tests; no build or device access."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys
import tomllib

sys.dont_write_bytecode = True
import verify_xnu_adt_phandle_probe as v


def encode(nodes):
    """Test-only independent encoder retaining property and child order."""
    def emit(path):
        props = nodes[path]
        children = [key for key in nodes if key != "/" and
                    (key.rsplit("/", 1)[0] or "/") == path]
        raw = struct.pack("<II", len(props), len(children))
        for key, value in props.items():
            raw += struct.pack("<32sI", key.encode(), len(value)) + value + bytes(-len(value) % 4)
        return raw + b"".join(emit(child) for child in children)
    return emit("/")


def run(root, artifact=None):
    target = root / "work/UniXNU-j716f-xnu-adt-phandle-probe"
    baseline = (root / "analysis/xnu-iokit-panic-r22-20261114/payload.r22.adt").read_bytes()
    v.require(v.sha256(baseline) == v.R22_ADT_SHA, "Test reference is not actual r22 ADT")
    before = v.adt_read(baseline)
    v.require(encode(before) == baseline, "Test encoder does not reproduce actual ADT bytes")
    fixed = copy.deepcopy(before)
    fixed["/arm-io/intc"]["AAPL,phandle"] = fixed["/arm-io/intc"]["phandle"]
    candidate = encode(fixed)
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

    accept("equivalent controller alias only", lambda: v.compare_r22_adt(baseline, candidate))
    accept("four actual timer specifiers resolve through XNU Apple handle table",
           lambda: v.require(v.iokit_interrupt_reference_model(candidate)["mapped_interrupts"] == 4, "Bad model"))
    reject("actual r22 missing alias reproduces FindPHandle NULL",
           lambda: v.iokit_interrupt_reference_model(baseline))
    reject("unchanged r22 ADT is not the correction", lambda: v.compare_r22_adt(baseline, baseline))
    for name, path, key, value in (
        ("wrong Apple alias", "/arm-io/intc", "AAPL,phandle", struct.pack("<I", 2)),
        ("big endian Apple alias", "/arm-io/intc", "AAPL,phandle", struct.pack(">I", 1)),
        ("oversize Apple alias", "/arm-io/intc", "AAPL,phandle", struct.pack("<Q", 1)),
        ("removed timer interrupts workaround", "/arm-io/timer", "interrupts", None),
        ("changed timer PPI", "/arm-io/timer", "interrupts", struct.pack("<12I", 1, 14, 0xff08, 1, 14, 0xff08, 1, 11, 0xff08, 1, 12, 0xff08)),
        ("changed interrupt width", "/arm-io/intc", "#interrupt-cells", struct.pack("<I", 1)),
        ("changed standard handle", "/arm-io/intc", "phandle", struct.pack("<I", 2)),
        ("changed parent", "/arm-io/timer", "interrupt-parent", struct.pack("<I", 2)),
        ("expanded memory", "/chosen", "darwinboot-memory-size", struct.pack("<Q", 0x20000000)),
        ("zero random seed", "/chosen", "random-seed", bytes(256)),
        ("short random seed", "/chosen", "random-seed", b"x"),
        ("unapproved extra property", "/arm-io/timer", "test-extra", b"x"),
    ):
        bad = copy.deepcopy(fixed)
        if value is None:
            del bad[path][key]
        else:
            bad[path][key] = value
        reject(name, lambda bad=bad: v.compare_r22_adt(baseline, encode(bad)))
    bad = copy.deepcopy(fixed)
    bad["/arm-io/duplicate"] = dict(bad["/arm-io/intc"], name=b"duplicate\0")
    reject("duplicate provider handle", lambda: v.iokit_interrupt_reference_model(encode(bad)))
    bad = copy.deepcopy(fixed)
    bad["/arm-io/timer"]["interrupts"] = bad["/arm-io/timer"]["interrupts"][:-4]
    reject("partial last specifier", lambda: v.iokit_interrupt_reference_model(encode(bad)))
    changed_seed = copy.deepcopy(fixed)
    changed_seed["/chosen"]["random-seed"] = bytes(range(256))
    accept("valid generated seed variation", lambda: v.compare_r22_adt(baseline, encode(changed_seed)))
    menu = (target / "payload/unixnu.toml").read_bytes()
    board = tomllib.loads((target / "board.toml").read_text(encoding="utf-8"))
    accept("r23 manual five-entry menu", lambda: v.validate_menu(menu))
    accept("unchanged InitIOKit return checkpoint", lambda: v.validate_board(board))
    reject("automatic XNU boot", lambda: v.validate_menu(menu.replace(b"timeout = -1", b"timeout = 0")))
    reject("wrong main title", lambda: v.validate_menu(menu.replace(v.TITLE.encode(), b"r22")))
    reject("missing early-log opt-in", lambda: v.validate_menu(menu.replace(b" j716f-earlylog=1", b"")))
    bad_board = copy.deepcopy(board)
    bad_board["payload"]["entry_probe_reset_before"] = True
    reject("checkpoint before InitIOKit", lambda: v.validate_board(bad_board))
    if artifact is not None:
        files, _ = v.read_fat32((artifact / "payload.img").read_bytes())
        actual = files["efi/unixnu/payloads/kernel.macho.adt"]
        accept("actual official payload ADT delta", lambda: v.compare_r22_adt(baseline, actual))
        accept("actual official payload XNU handle resolution", lambda: v.iokit_interrupt_reference_model(actual))
    return {"status": "passed", "scope": "r23 ADT difference and actual-XNU reference semantics",
            "positive_checks": sum(c.get("accepted", False) for c in cases),
            "negative_checks": sum(c.get("rejected", False) for c in cases), "cases": cases,
            "actual_payload_checked": artifact is not None,
            "unchanged_kernel_rejection_suite_repeated": False, "hardware_tested": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--artifact", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    v.require(not args.report.exists(), "Refusing to overwrite report")
    result = run(args.root.resolve(strict=True), args.artifact.resolve(strict=True) if args.artifact else None)
    with args.report.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps({k: value for k, value in result.items() if k != "cases"}, indent=2))
