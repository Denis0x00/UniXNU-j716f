#!/usr/bin/env python3
"""Prepare/verify two UNDEPLOYED r6 follow-up kernel copies with the official patcher.

This script never constructs a payload/boot image or accesses a device. It uses
only the frozen DT-panic baseline, not r6's instrumented kernel. Python 3.10+;
prepare needs clang, ld.lld, llvm-nm and llvm-objcopy on PATH. The verifier is
independent of the official patcher and uses Python's standard library. Optional
Capstone adds a second disassembly listing without changing acceptance.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
KERNEL_SHA = "7970051f50e280b50e42f5ed372ea095dc8646a36fb92e4716f85349bd6118f2"
NATIVE_SHA = "2675a264b350ce41053d00be244ef531b307e0eaec4114b8b57df8f4a289a237"
PATCHER_SHA = "159eb1ea8d6c1da30f26913a281f46e21d940a1ae7886656529907ec3edf96ad"
R6_SHA = "e535b9ba62974ec22cfcbf4a553f73e138353b0b662cabe09c396c60b3ac629f"
KERNEL_BYTES, NATIVE_BYTES = 51085312, 18242752
ENTRY = 0xfffffff007418488
FIRST_CPU = 0xfffffff00741a05c
MMU = 0xfffffff00741a350
POST_MMU = 0xfffffff00741a370
CAVES = {1: 0xfffffff007418500, 2: 0xfffffff007418560,
         3: 0xfffffff0074185c0, 4: 0xfffffff007418620}
ARM_INIT, ARM_INIT_END = 0xfffffff0079e00c4, 0xfffffff0079e0b94
FIQ_SITE = 0xfffffff0079e09d0
CONSOLE, CONSOLE_END = 0xfffffff0079a6000, 0xfffffff0079a61a0
BRANCHES = {
    "after-console-return": {
        "site": 0xfffffff0079e0984, "callee": CONSOLE,
        "symbol": "_PE_create_console", "before": False,
        "selection": "Only if the actual r6 after-fiq checkpoint is not confirmed",
        "behavior": "Call _PE_create_console; after it returns wait 115200000 CNTPCT ticks, then SMC SYSTEM_RESET",
        "delay_seconds": 6, "timebase_hz": 19200000, "delay_ticks": 115200000,
    },
    "before-machine-startup": {
        "site": 0xfffffff0079e0a70, "callee": 0xfffffff00741bd30,
        "symbol": "_machine_startup", "before": True,
        "selection": "Only after the actual r6 after-fiq checkpoint is confirmed",
        "behavior": "At the _machine_startup call site immediately SMC SYSTEM_RESET; do not call _machine_startup or read CNTPCT",
        "delay_seconds": None, "timebase_hz": None, "delay_ticks": None,
    },
}


def require(ok, message):
    if not ok:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def stamp():
    return datetime.now(timezone.utc).isoformat()


def write_json(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(value, stream, ensure_ascii=False, indent=2)
        stream.write("\n")


def write_text(path, value):
    with path.open("x", encoding="utf-8", newline="\n") as stream:
        stream.write(value)


def identity(path):
    data = path.read_bytes()
    return {"path": str(path.resolve()), "bytes": len(data), "sha256": sha(data)}


class MachO:
    """Independent bounded LC_SEGMENT_64/LC_SYMTAB/LC_UNIXTHREAD reader."""
    def __init__(self, data):
        self.data, self.segments, self.sections, self.names = data, [], [], {}
        self.addresses, self.ambiguous_names = set(), set()
        require(len(data) >= 32, "Mach-O header truncated")
        magic, cpu, _, _, ncmds, cmdbytes, _, _ = struct.unpack_from("<8I", data)
        require(magic == 0xfeedfacf and cpu == 0x100000c, "Expected ARM64 Mach-O")
        self.header_end = 32 + cmdbytes
        require(self.header_end <= len(data), "Load-command extent invalid")
        cursor, symtab, pc = 32, None, None
        for _ in range(ncmds):
            require(cursor + 8 <= self.header_end, "Truncated load command")
            cmd, size = struct.unpack_from("<II", data, cursor)
            require(size >= 8 and size % 4 == 0 and cursor + size <= self.header_end,
                    "Invalid load-command size")
            if cmd == 0x19:
                require(size >= 72, "Truncated segment")
                name, vm, vmsize, off, count, _, _, nsects, _ = struct.unpack_from("<16s4Q4I", data, cursor + 8)
                require(size == 72 + nsects * 80 and off + count <= len(data), "Invalid segment extent")
                self.segments.append((vm, min(vmsize, count), off))
                for index in range(nsects):
                    sname, seg, addr, length, *_ = struct.unpack_from("<16s16s2Q8I", data, cursor + 72 + index * 80)
                    self.sections.append((seg.rstrip(b"\0").decode(), sname.rstrip(b"\0").decode(), addr, length))
            elif cmd == 2:
                require(size == 24 and symtab is None, "Invalid/duplicate LC_SYMTAB")
                symtab = struct.unpack_from("<4I", data, cursor + 8)
            elif cmd == 5:
                require(size >= 280 and pc is None, "Invalid/duplicate LC_UNIXTHREAD")
                flavor, count = struct.unpack_from("<II", data, cursor + 8)
                require(flavor == 6 and count >= 66, "Unexpected ARM64 thread flavor/count")
                pc = struct.unpack_from("<Q", data, cursor + 16 + 32 * 8)[0]
            cursor += size
        require(cursor == self.header_end and symtab is not None, "Missing symtab/incorrect command end")
        self.entry = pc
        off, count, strings, length = symtab
        require(off + 16 * count <= len(data) and strings + length <= len(data), "Invalid symbol/string extent")
        for index in range(count):
            at, kind, _, _, value = struct.unpack_from("<IBBHQ", data, off + 16 * index)
            require(at < length, "Symbol string out of bounds")
            end = data.find(b"\0", strings + at, strings + length)
            require(end >= 0, "Unterminated symbol")
            name = data[strings + at:end].decode("utf-8")
            if name and kind & 0xe == 0xe and not kind & 0xe0:
                if name in self.names and self.names[name] != value:
                    self.ambiguous_names.add(name)
                self.names.setdefault(name, value)
                self.addresses.add(value)

    def offset(self, address, size=1):
        hits = [off + address - vm for vm, count, off in self.segments
                if vm <= address and address + size <= vm + count]
        require(len(hits) == 1, "Address is not uniquely file-backed: " + hex(address))
        return hits[0]

    def body(self, address, size):
        at = self.offset(address, size)
        return self.data[at:at + size]

    def word(self, address):
        return struct.unpack("<I", self.body(address, 4))[0]

    def end(self, name):
        require(name not in self.ambiguous_names, "Ambiguous selected symbol: " + name)
        start = self.names[name]
        return min(value for value in self.addresses if value > start)


def target(word, address, opcode):
    require(word & 0xfc000000 == opcode, "Unexpected branch instruction at " + hex(address))
    imm = word & 0x3ffffff
    if imm & 0x2000000:
        imm -= 0x4000000
    return address + 4 * imm


def branch(address, dest, link=False):
    delta = dest - address
    require(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), "Branch outside signed imm26 range")
    return (0x94000000 if link else 0x14000000) | ((delta // 4) & 0x3ffffff)


def packed(words):
    return struct.pack("<" + "I" * len(words), *words)


def paint(stage):
    words = ([0xf9401401, 0xf9401c02, 0xf9402403] if stage == 1
             else [0xf9401681, 0xf9401e82, 0xf9402683])
    words += [0x9b037c42, 0xd344fc42, 0x927df042,
              0xd2800024 if stage == 1 else 0xd2800044,
              0x9b047c45, 0x8b050021, 0x52801fe6,
              0x72bfe006 if stage == 1 else 0x72bfffe6,
              0x28811826, 0xf1002042, 0x54ffffc8, 0xd5033f9f]
    if stage == 2:
        words.append(0xd5181000)
    words.append(branch(CAVES[stage] + 4 * len(words), FIRST_CPU if stage == 1 else MMU + 4))
    return packed(words).ljust(96, b"\0")


def reset_sequence(spec):
    reset = [0x52800120, 0x72b08000, 0xaa1f03e1, 0xaa1f03e2,
             0xaa1f03e3, 0xd4000003, 0xd503205f, 0x17ffffff]
    if spec["before"]:
        return packed(reset).ljust(96, b"\0")
    timer = [branch(CAVES[4], spec["callee"], True), 0xd53be021,
             0x580001c2, 0x8b020022, 0xd5033fdf, 0xd53be021,
             0xeb02003f, 0x54ffffa3]
    return (packed(timer + reset) + struct.pack("<Q", 115200000)).ljust(96, b"\0")


def inputs(kernel, native, patcher):
    original, nat, tool = kernel.read_bytes(), native.read_bytes(), patcher.read_bytes()
    require(len(original) == KERNEL_BYTES and sha(original) == KERNEL_SHA, "Wrong frozen original kernel")
    require(len(nat) == NATIVE_BYTES and sha(nat) == NATIVE_SHA, "Wrong frozen original native kernel")
    require(sha(tool.replace(b"\r\n", b"\n")) == PATCHER_SHA, "Official patcher source changed")
    image, native_image = MachO(original), MachO(nat)
    require(image.entry == ENTRY, "Original entry changed")
    expected = {"_start_first_cpu": FIRST_CPU, "_arm_init": ARM_INIT,
                "_PE_create_console": CONSOLE, "_machine_startup": 0xfffffff00741bd30,
                "_fiq_context_init": 0xfffffff00741e364}
    for name, address in expected.items():
        require(name not in image.ambiguous_names and name not in native_image.ambiguous_names,
                "Ambiguous selected baseline symbol: " + name)
        require(image.names.get(name) == address, "Original symbol changed: " + name)
        require(native_image.names.get(name) == address, "Native symbol changed: " + name)
    require(image.end("_arm_init") == ARM_INIT_END and image.end("_PE_create_console") == CONSOLE_END,
            "Original function boundary changed")
    require(target(image.word(ENTRY), ENTRY, 0x14000000) == FIRST_CPU, "Original entry does not branch to first CPU")
    require(image.word(MMU) == 0xd5181000 and image.word(POST_MMU) == 0xd2a00600,
            "Original MMU/stage3 site changed")
    mmu_matches = [va for va in range(FIRST_CPU, FIRST_CPU + 0x1000, 4)
                   if image.word(va) == 0xd5181000 and image.word(va + 4) == 0xd5033fdf
                   and image.word(va + 0x20) == 0xd2a00600 and image.word(va + 0x24) == 0xd5181040]
    require(mmu_matches == [MMU], "MMU marker is not unique")
    for va in CAVES.values():
        require(image.body(va, 96) == bytes(96), "Original cave is not zero")
        sections = [(seg, name) for seg, name, start, length in image.sections
                    if start <= va and va + 96 <= start + length]
        require(sections == [("__TEXT_EXEC", "__text")], "Cave is outside expected executable section")
    for spec in BRANCHES.values():
        matches = [va for va in range(ARM_INIT, ARM_INIT_END, 4)
                   if image.word(va) & 0xfc000000 == 0x94000000
                   and target(image.word(va), va, 0x94000000) == spec["callee"]]
        require(matches == [spec["site"]], "Call occurrence/boundary changed: " + spec["symbol"])
    require(image.word(0xfffffff0079a6038) == 0x540009e1, "Missing-pram guarded branch changed")
    for name in ["_PE_create_console", "_panic", "_panic_trap_to_debugger", "_machine_startup"]:
        start, end = image.names[name], image.end(name)
        require(native_image.end(name) == end and image.body(start, end - start) == native_image.body(start, end - start),
                "Native/prelinked function differs: " + name)
    return image, {"kernel": identity(kernel), "native": identity(native), "official_patcher": identity(patcher)}


def configuration(name):
    spec = BRANCHES[name]
    return {"id": name, "status": "undeployed-host-kernel-candidate", "selection_rule": spec["selection"],
            "kernel_input_sha256": KERNEL_SHA, "native_input_sha256": NATIVE_SHA,
            "official_patcher_normalized_sha256": PATCHER_SHA,
            "caller": "_arm_init", "callee": spec["symbol"], "occurrence": 1,
            "site_va": hex(spec["site"]), "callee_va": hex(spec["callee"]),
            "behavior": spec["behavior"], "reset_before": spec["before"],
            "nominal_runtime_delay_seconds": spec["delay_seconds"], "counter_frequency_assumed_hz": spec["timebase_hz"],
            "delay_ticks": spec["delay_ticks"], "wrapper_entry_probe_reset_delay": 6,
            "wrapper_delay_semantics": "Nonzero enables official stage4 wrapper; ignored by RESET_BEFORE" if spec["before"] else "Six seconds at assumed 19.2 MHz",
            "panic_screen": False, "stages": [1, 2, 4], "stage3_enabled": False,
            "payload_built": False, "boot_image_built": False, "deployed": False, "hardware_tested": False}


def overlay(name):
    spec = BRANCHES[name]
    note = ("# Nonzero delay enables official payload wrapper stage4; RESET_BEFORE ignores it.\n"
            "# Actual behavior is IMMEDIATE reset: no timer and no call to _machine_startup.\n"
            if spec["before"] else "# Runtime wait begins only after _PE_create_console returns.\n")
    return ("# Proposal fragment only: not a full board and not applied to any target.\n"
            "# Use the ORIGINAL kernel, never this already-patched kernel as payload input.\n"
            "[payload]\npanic_screen = false\n" + note + "entry_probe_reset_delay = 6\n"
            + 'entry_probe_reset_after = "' + spec["symbol"] + '"\n'
            + 'entry_probe_reset_in = "_arm_init"\nentry_probe_reset_occurrence = 1\n'
            + "entry_probe_reset_before = " + str(spec["before"]).lower() + "\n")


def verify_candidate(image, data, name, r6=None):
    spec = BRANCHES[name]
    require(len(data) == len(image.data), "Candidate size changed")
    candidate = MachO(data)
    require(data[:image.header_end] == image.data[:image.header_end], "Mach-O header/load commands changed")
    require(candidate.entry == image.entry and candidate.names == image.names, "Entry/symbol metadata changed")
    regions = []
    for va, expected in [(ENTRY, packed([branch(ENTRY, CAVES[1])])),
                         (MMU, packed([branch(MMU, CAVES[2])])),
                         (spec["site"], packed([branch(spec["site"], CAVES[4])])),
                         (CAVES[1], paint(1)), (CAVES[2], paint(2)),
                         (CAVES[4], reset_sequence(spec))]:
        offset = image.offset(va, len(expected))
        require(data[offset:offset + len(expected)] == expected, "Unexpected exact hook/cave bytes at " + hex(va))
        regions.append((offset, offset + len(expected), va))
    regions.sort()
    cursor, changed, changes = 0, 0, []
    for low, high, va in regions:
        require(low >= cursor and data[cursor:low] == image.data[cursor:low], "Mutation outside allowed hooks/caves")
        count = sum(a != b for a, b in zip(data[low:high], image.data[low:high]))
        changed += count
        changes.append({"va": hex(va), "file_start": hex(low), "file_end": hex(high), "changed_bytes": count})
        cursor = high
    require(data[cursor:] == image.data[cursor:], "Mutation after last allowed region")
    require(candidate.body(CAVES[3], 96) == bytes(96) and candidate.word(POST_MMU) == 0xd2a00600,
            "Stage3 is not disabled")
    require(candidate.word(FIQ_SITE) == image.word(FIQ_SITE), "r6 fiq hook leaked into candidate")
    preserved = {}
    for func in ["_PE_create_console", "_panic", "_panic_trap_to_debugger", "_machine_startup", "_fiq_context_init"]:
        start, end = image.names[func], image.end(func)
        body = candidate.body(start, end - start)
        require(body == image.body(start, end - start), "Changed preserved function: " + func)
        preserved[func] = {"start": hex(start), "end": hex(end), "sha256": sha(body)}
    if r6 is not None:
        require(len(r6) == KERNEL_BYTES and sha(r6) == R6_SHA, "Wrong frozen instrumented r6 reference")
        for va, length in [(ENTRY, 4), (MMU, 4), (CAVES[1], 96), (CAVES[2], 96)]:
            at = image.offset(va, length)
            require(data[at:at + length] == r6[at:at + length], "Stage1/2 do not match retained r6")
    annotations = (["mov w0,#9", "movk w0,#0x8400,lsl#16", "mov x1,xzr", "mov x2,xzr",
                    "mov x3,xzr", "smc #0 (PSCI 0x84000009)", "wfe", "b previous wfe"])
    if not spec["before"]:
        annotations = ["bl " + spec["symbol"] + " at " + hex(spec["callee"]),
                       "mrs x1,CNTPCT_EL0 (only reached after callee RET)",
                       "ldr x2,[pc,#56] = literal at cave+64", "add x2,x1,x2",
                       "isb (delay loop)", "mrs x1,CNTPCT_EL0", "cmp x1,x2", "b.lo cave+16"] + annotations
    words = struct.unpack("<" + "I" * len(annotations), candidate.body(CAVES[4], len(annotations) * 4))
    code = [{"va": hex(CAVES[4] + 4 * index), "word": hex(word), "decoded_requirement": text}
            for index, (word, text) in enumerate(zip(words, annotations))]
    capstone_listing = None
    try:
        import capstone
    except ImportError:
        pass
    else:
        decoder = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        ranges = [("stage1", CAVES[1], 64), ("stage2", CAVES[2], 68),
                  ("stage4", CAVES[4], 32 if spec["before"] else 64),
                  ("selected-call-site", spec["site"] - 8, 20),
                  ("PE_create_console", CONSOLE, CONSOLE_END - CONSOLE)]
        capstone_listing = {"version": capstone.__version__, "ranges": {}}
        for label, va, size in ranges:
            decoded = list(decoder.disasm(candidate.body(va, size), va))
            require(sum(i.size for i in decoded) == size, "Capstone could not decode complete range: " + label)
            capstone_listing["ranges"][label] = [
                {"va": hex(i.address), "bytes": i.bytes.hex(), "mnemonic": i.mnemonic, "operands": i.op_str}
                for i in decoded]
    return {"id": name, "status": "passed-host-byte-verification", "bytes": len(data), "sha256": sha(data),
            "changed_bytes": changed, "permitted_regions": changes, "all_other_bytes_identical": True,
            "header_and_symbols_unchanged": True, "stage3_disabled": True, "fiq_call_original": True,
            "stage1_stage2_exact_words_verified": True, "r6_stage1_stage2_comparison": r6 is not None,
            "preserved_functions": preserved, "stage4_control_flow": code,
            "behavior": spec["behavior"], "runtime_delay_seconds": spec["delay_seconds"],
            "delay_ticks": spec["delay_ticks"], "timebase_hz": spec["timebase_hz"],
            "capstone_disassembly": capstone_listing, "payload_built": False, "boot_image_built": False,
            "deployed": False, "hardware_tested": False}


def tool_identity(name):
    path = shutil.which(name)
    require(path is not None, "Required official tool absent from PATH: " + name)
    result = subprocess.run([path, "--version"], capture_output=True, text=True, check=True)
    return {**identity(Path(path)), "version": (result.stdout + result.stderr).strip()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=["specs", "prepare", "verify"])
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--native", required=True, type=Path)
    parser.add_argument("--patcher", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path, help="New directory for specs/prepare; candidate directory for verify")
    parser.add_argument("--report", type=Path, help="New JSON file for verify (required for verify)")
    parser.add_argument("--r6-kernel", type=Path, help="Optional exact instrumented r6 kernel for stage1/2 comparison, read only")
    args = parser.parse_args()
    if args.mode == "verify":
        require(args.report is not None and not args.report.exists(), "verify needs a new --report path")
    else:
        require(args.report is None and not args.output.exists(), "specs/prepare requires a new output directory; no --report")
    image, baseline = inputs(args.kernel, args.native, args.patcher)
    r6 = args.r6_kernel.read_bytes() if args.r6_kernel else None
    r6_identity = identity(args.r6_kernel) if args.r6_kernel else None
    report = {"schema_version": 1, "time_utc": stamp(), "mode": args.mode,
              "scope": "Host-only follow-up kernel preparation; r6 hardware outcome remains unobserved",
              "inputs_before": baseline, "script": identity(Path(__file__)), "r6_reference": r6_identity,
              "payload_built": False, "boot_image_built": False, "device_accessed": False,
              "deployed": False, "hardware_tested": False}
    if args.mode != "verify":
        args.output.mkdir(parents=True, exist_ok=False)
        for name in BRANCHES:
            folder = args.output / name
            folder.mkdir()
            write_json(folder / "configuration.json", configuration(name))
            write_text(folder / "payload-fragment.toml", overlay(name))
    try:
        if args.mode == "prepare":
            report["toolchain"] = {name: tool_identity(name) for name in ["clang", "ld.lld", "llvm-nm", "llvm-objcopy"]}
            report["commands"] = []
            for name, spec in BRANCHES.items():
                candidate = (args.output / name / "kernel.macho").resolve()
                with candidate.open("xb") as stream:
                    stream.write(image.data)
                command = [sys.executable, str(args.patcher.resolve()), str(candidate), "--reset-delay", "6",
                           "--reset-after", spec["symbol"], "--in", "_arm_init", "--occurrence", "1"]
                if spec["before"]:
                    command.append("--reset-before")
                result = subprocess.run(command, capture_output=True, text=True)
                command_record = {"argv": command, "returncode": result.returncode,
                                  "stdout": result.stdout, "stderr": result.stderr}
                report["commands"].append(command_record)
                write_json(candidate.parent / "official-command.json", command_record)
                require(result.returncode == 0, "Official patcher failed; candidate retained for inspection: " + name)
        if args.mode != "specs":
            report["candidates"] = {}
            for name in BRANCHES:
                candidate = args.output / name / "kernel.macho"
                report["candidates"][name] = verify_candidate(image, candidate.read_bytes(), name, r6)
            report["status"] = "passed-host-only-two-kernel-verification"
        else:
            report["status"] = "specifications-only-no-kernels-generated"
        report["inputs_after"] = {"kernel": identity(args.kernel), "native": identity(args.native),
                                  "official_patcher": identity(args.patcher)}
        require(report["inputs_after"] == baseline, "Frozen input/official patcher changed during run")
        if args.r6_kernel:
            require(identity(args.r6_kernel) == r6_identity, "r6 read-only reference changed")
        report["frozen_inputs_unchanged"] = True
    except Exception as error:
        report["status"] = "failed-do-not-use-candidates"
        report["error"] = str(error)
        if args.mode != "verify":
            write_json(args.output / "failure.json", report)
        else:
            write_json(args.report, report)
        raise
    write_json(args.report if args.mode == "verify" else args.output / "manifest.json", report)
    print(json.dumps({"status": report["status"], "output": str(args.output),
                      "candidates": {key: {"sha256": value["sha256"], "changed_bytes": value["changed_bytes"]}
                                     for key, value in report.get("candidates", {}).items()}}, indent=2))


if __name__ == "__main__":
    main()
