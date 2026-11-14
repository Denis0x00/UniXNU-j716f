#!/usr/bin/env python3
"""Offline rejection checks for r7's exact kernel, menu and board contract."""
import argparse
import copy
import json
from pathlib import Path
import struct
import sys
import tomllib

sys.dont_write_bytecode = True
import verify_xnu_machine_boundary_probe as v
from prepare_r6_followup import MachO, CAVES, FIQ_SITE, POST_MMU


def run(root):
    original = (root / "artifacts/j716f-host-dt-panic-20261113/kernel.macho").read_bytes()
    candidate = (root / "analysis/xnu-r6-followup-20261113/candidates-linux/before-machine-startup/kernel.macho").read_bytes()
    after_console = (root / "analysis/xnu-r6-followup-20261113/candidates-linux/after-console-return/kernel.macho").read_bytes()
    r6 = (root / "analysis/xnu-r6-followup-20261113/r6-reference/kernel.macho").read_bytes()
    image = MachO(original)
    results = []

    def accept(name, operation):
        operation()
        results.append({"name": name, "result": "accepted"})

    def reject(name, operation):
        try:
            operation()
        except (ValueError, AssertionError, RuntimeError, KeyError, tomllib.TOMLDecodeError) as error:
            results.append({"name": name, "result": "rejected", "reason": str(error)})
        else:
            raise AssertionError("Verifier accepted invalid case: " + name)

    def replace_word(va, word):
        changed = bytearray(candidate)
        struct.pack_into("<I", changed, image.offset(va, 4), word)
        return bytes(changed)

    accept("exact accepted before-machine-startup candidate",
           lambda: v.probe_readback(original, candidate, candidate))
    reject("after-console branch cannot substitute for before-machine", 
           lambda: v.probe_readback(original, after_console, candidate))
    reject("old r6 after-fiq cannot substitute for r7", lambda: v.probe_readback(original, r6, candidate))
    reject("unpatched kernel cannot substitute for r7", lambda: v.probe_readback(original, original, candidate))
    reject("r6 cannot be used as the original input", lambda: v.probe_readback(r6, candidate, candidate))
    reject("wrong branch cannot be trusted as reference", lambda: v.probe_readback(original, candidate, after_console))
    reject("truncated reference is rejected", lambda: v.probe_readback(original, candidate, candidate[:-4]))
    reject("truncated extracted kernel is rejected", lambda: v.probe_readback(original, candidate[:-4], candidate))
    for index, label in enumerate(("reset low function ID", "reset high function ID", "reset x1", "reset x2",
                                   "reset x3", "SMC", "fallback WFE", "fallback branch")):
        bad = replace_word(CAVES[4] + index * 4, 0xd503201f)
        reject("mutated " + label, lambda bad=bad: v.probe_readback(original, bad, candidate))
    for label, va, word in (
        ("original machine call restored", v.CALL_SITE, image.word(v.CALL_SITE)),
        ("r6 fiq stop leaked into r7", FIQ_SITE, MachO(r6).word(FIQ_SITE)),
        ("stage3 hook re-enabled", POST_MMU, 0x14000000),
        ("stage4 padding contains code", CAVES[4] + 32, 0xd503201f),
        ("preserved callee modified", v.CALLEE_VA, 0xd503201f),
    ):
        bad = replace_word(va, word)
        reject(label, lambda bad=bad: v.probe_readback(original, bad, candidate))

    old_target = root / "work/UniXNU-j716f-xnu-dt-panic-probe"
    menu = (old_target / "payload/unixnu.toml").read_bytes().replace(
        b"J716F XNU DT panic fix r6", v.TITLE.encode()).replace(
        b"Start XNU (DT panic fix checkpoint)", v.XNU_TITLE.encode())
    accept("reviewed r7 hold menu", lambda: v.validate_menu(menu))
    for label, old, new in (
        ("stale r6 title", v.TITLE.encode(), b"J716F XNU DT panic fix r6"),
        ("autoboot timeout", b"timeout = -1", b"timeout = 0"),
        ("default XNU instead of Tools", b'default = "tools"', b'default = "xnu"'),
        ("extra root argument", b"-v serial=3", b"-v serial=3 rd=disk0s1"),
        ("wrong XNU label", v.XNU_TITLE.encode(), b"Start XNU (DT panic fix checkpoint)"),
    ):
        bad = menu.replace(old, new)
        reject(label, lambda bad=bad: v.validate_menu(bad))
    board = tomllib.loads((old_target / "board.toml").read_text(encoding="utf-8"))
    board["board"]["name"] = v.DEVICE
    board["kernel"]["defconfig"] = v.DEFCONFIG
    board["payload"].update(v.RESET_SETTINGS)
    board["payload"]["title"] = v.XNU_TITLE
    accept("reviewed r7 before-call board", lambda: v.validate_board(board))
    for key, value in (("entry_probe_reset_before", False), ("entry_probe_reset_delay", 0),
                       ("entry_probe_reset_after", "_fiq_context_init"), ("entry_probe_reset_occurrence", 2),
                       ("panic_screen", True)):
        bad = copy.deepcopy(board)
        bad["payload"][key] = value
        reject("board " + key, lambda bad=bad: v.validate_board(bad))
    bad = copy.deepcopy(board)
    del bad["payload"]["entry_probe_reset_before"]
    reject("board omits explicit before mode", lambda: v.validate_board(bad))
    build = {"status": "host-generation-passed", "hardware_tested": False, "device_actions": False,
             "reset_checkpoint": {"stage": 4, "reset_delay_seconds": None, "timebase_hz": None,
                 "delay_ticks": None, "wrapper_entry_probe_reset_delay": 6, "reset_before": True,
                 "call_site": hex(v.CALL_SITE), "stage3": False, "panic_screen": False, "runtime_reached": False}}
    accept("build metadata describes immediate reset", lambda: v.validate_build_record(build))
    bad = copy.deepcopy(build)
    bad["reset_checkpoint"]["reset_delay_seconds"] = 6
    reject("six-second runtime delay incorrectly claimed", lambda: v.validate_build_record(bad))
    bad = copy.deepcopy(build)
    bad["reset_checkpoint"]["timebase_hz"] = 19200000
    reject("timer dependency incorrectly claimed", lambda: v.validate_build_record(bad))
    return {"result": "passed", "scope": "Offline exact-kernel/menu/board rejection checks",
            "case_count": len(results), "accepted": sum(item["result"] == "accepted" for item in results),
            "rejected": sum(item["result"] == "rejected" for item in results), "cases": results,
            "hardware_tested": False, "container_verification_run": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.report.exists():
        raise SystemExit("Refusing to overwrite report")
    result = run(args.root.resolve(strict=True))
    args.report.parent.mkdir(parents=True, exist_ok=True)
    with args.report.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    print(json.dumps({key: value for key, value in result.items() if key != "cases"}, indent=2))
