#!/usr/bin/env python3
"""Focused offline misuse checks against the official r8 kernel candidate."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys
import tomllib

sys.dont_write_bytecode = True
import verify_xnu_machine_conf_probe as v
from prepare_r6_followup import MachO


def run(root, candidate_path=None):
    original = (root / "artifacts/j716f-host-dt-panic-20261113/kernel.macho").read_bytes()
    candidate_path = candidate_path or (root / "analysis/xnu-machine-conf-preparation-20261113/candidate-linux/kernel.macho")
    candidate = candidate_path.read_bytes()
    r7 = (root / "analysis/xnu-r6-followup-20261113/candidates-linux/before-machine-startup/kernel.macho").read_bytes()
    r6 = (root / "analysis/xnu-r6-followup-20261113/r6-reference/kernel.macho").read_bytes()
    image = MachO(original)
    cases = []

    def accept(name, operation):
        operation()
        cases.append({"name": name, "result": "accepted"})

    def reject(name, operation):
        try:
            operation()
        except (ValueError, AssertionError, RuntimeError, KeyError, tomllib.TOMLDecodeError) as error:
            cases.append({"name": name, "result": "rejected", "reason": str(error)})
        else:
            raise AssertionError("Invalid case was accepted: " + name)

    def changed(va, raw):
        data = bytearray(candidate)
        offset = image.offset(va, len(raw))
        data[offset:offset + len(raw)] = raw
        return bytes(data)

    accept("actual official r8 candidate matches independently derived instruction requirements",
           lambda: v.probe_readback(original, candidate))
    reject("r7 before-machine boundary substituted", lambda: v.probe_readback(original, r7))
    reject("r6 after-fiq substituted", lambda: v.probe_readback(original, r6))
    reject("instrumented r7 used as original", lambda: v.probe_readback(r7, candidate))
    mutations = (
        ("machine_conf call skipped", v.CAVES[3], struct.pack("<I", 0xd503201f)),
        ("six-second literal changed", v.CAVES[3] + 64, struct.pack("<Q", 19200000)),
        ("timer branch bypasses wait", v.CAVES[3] + 28, struct.pack("<I", 0xd503201f)),
        ("r7 early stop leaked in", v.OLD_R7_SITE, struct.pack("<I", MachO(r7).word(v.OLD_R7_SITE))),
        ("stage3 code leaked in", v.CAVES[2], struct.pack("<I", 0xd503201f)),
        ("preserved machine_conf function changed", v.CALLEE_RET, struct.pack("<I", 0xd503201f)),
    )
    for name, va, raw in mutations:
        bad = changed(va, raw)
        reject(name, lambda bad=bad: v.probe_readback(original, bad))
    old_target = root / "work/UniXNU-j716f-xnu-dt-panic-probe"
    menu = (old_target / "payload/unixnu.toml").read_bytes().replace(
        b"J716F XNU DT panic fix r6", v.TITLE.encode()).replace(
        b"Start XNU (DT panic fix checkpoint)", v.XNU_TITLE.encode())
    accept("r8 menu identity and manual-selection policy", lambda: v.validate_menu(menu))
    bad_menu = menu.replace(v.TITLE.encode(), b"J716F XNU machine boundary r7")
    reject("r7 menu title substituted", lambda: v.validate_menu(bad_menu))
    board = tomllib.loads((old_target / "board.toml").read_text(encoding="utf-8"))
    board["board"]["name"] = v.DEVICE
    board["kernel"]["defconfig"] = v.DEFCONFIG
    board["payload"].update(v.RESET_SETTINGS)
    board["payload"]["title"] = v.XNU_TITLE
    accept("r8 exact after-call board", lambda: v.validate_board(board))
    bad_board = copy.deepcopy(board)
    bad_board["payload"]["entry_probe_reset_before"] = True
    reject("before-call mode mislabelled r8", lambda: v.validate_board(bad_board))
    bad_caller = copy.deepcopy(board)
    bad_caller["payload"]["entry_probe_reset_in"] = "_arm_init"
    reject("old arm_init caller substituted", lambda: v.validate_board(bad_caller))
    build = {"status": "host-generation-passed", "hardware_tested": False, "device_actions": False,
             "reset_checkpoint": {"stage": 4, "reset_delay_seconds": 6, "timebase_hz": 19200000,
                 "delay_ticks": 115200000, "wrapper_entry_probe_reset_delay": 6,
                 "reset_after": v.CALLEE_NAME, "reset_in": v.CALLER_NAME, "occurrence": 1,
                 "reset_before": False, "call_site": hex(v.CALL_SITE), "stage3": False,
                 "panic_screen": False, "runtime_reached": False}}
    accept("r8 metadata records real six-second wait", lambda: v.validate_build_record(build))
    bad_build = copy.deepcopy(build)
    bad_build["reset_checkpoint"]["reset_delay_seconds"] = None
    reject("r7 immediate-delay metadata substituted", lambda: v.validate_build_record(bad_build))
    return {"result": "passed", "scope": "Focused offline r8 exact-kernel and identity misuse checks",
            "candidate_sha256": v.sha256(candidate), "candidate_bytes": len(candidate),
            "case_count": len(cases), "accepted": sum(c["result"] == "accepted" for c in cases),
            "rejected": sum(c["result"] == "rejected" for c in cases), "cases": cases,
            "hardware_tested": False, "container_verification_run": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--candidate-kernel", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        raise SystemExit("Refusing to overwrite report")
    result = run(args.root.resolve(strict=True), args.candidate_kernel)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps({k: value for k, value in result.items() if k != "cases"}, indent=2))
