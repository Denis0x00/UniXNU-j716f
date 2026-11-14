#!/usr/bin/env python3
"""Read-only, explicit-input display ABI audit for ordinary arm64 / 21H461.

This audits retained Apple code and dispatch rebinding, not a compiled display
adapter, runtime object, GPU, or hardware. It never imports an upstream script,
searches for candidates, creates a checkpoint, or writes any input image.
"""
import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

PROFILE = "ipados-17.7.11-21H461"
ORIGINAL_SHA = "f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7"
SLIDE = -0x1000000
FAMILY = "com.apple.iokit.IOMobileGraphicsFamily"
SURFACE = "com.apple.iokit.IOSurface"
# These virtual ranges and hashes belong to the exact original kernelcache above,
# after the fixed profile slide. They are not trusted merely because JSON says so.
TEXT = {
    FAMILY: (0xfffffff00576c000, 225140,
             "2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b"),
    SURFACE: (0xfffffff004b96000, 141468,
              "6f30c4fccca429b53281ceb298de8cdd4d4dd67deabbe800d436a9d27e1e9dc9"),
}
CONSTRUCTORS = {
    "IOMobileFramebuffer": (0xfffffff00577b324, 262),
    "IOMobileFramebufferService": (0xfffffff00577b51c, 262),
    "IOMobileFramebufferLegacy": (0xfffffff0057865d8, 358),
}
PURE = [211, 213, 245, 246, 306, 307, 337, 338, 345]
GETTERS = [
    ("descriptor", 0xfffffff004b9ead0, 48, True),
    ("width", 0xfffffff004b9ebbc, 88, True),
    ("height", 0xfffffff004b9ebc4, 96, True),
    ("stride", 0xfffffff004b9ebd4, 104, True),
    ("allocation", 0xfffffff004b9ebfc, 144, True),
    ("format", 0xfffffff004b9ebac, 128, False),
    ("planes", 0xfffffff004b9ebf4, 152, False),
]


class AuditError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise AuditError(message)


def digest(blob):
    return hashlib.sha256(blob).hexdigest()


def integer(value, label):
    require(type(value) is int, label + " must be an integer")
    return value


def strict_json(blob):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, "Duplicate JSON key: " + key)
            result[key] = value
        return result
    try:
        result = json.loads(blob, object_pairs_hook=pairs,
                            parse_constant=lambda value: (_ for _ in ()).throw(AuditError("Nonfinite JSON value")))
    except (ValueError, UnicodeError) as exc:
        raise AuditError("Invalid metadata JSON: " + str(exc)) from exc
    require(isinstance(result, dict), "Metadata must be a JSON object")
    return result


class MachO:
    """Small bounded LC_SEGMENT_64/LC_SYMTAB reader; no code execution."""
    def __init__(self, blob):
        self.blob = blob
        require(len(blob) >= 32, "Truncated Mach-O header")
        magic, cpu, subtype, kind, count, size = struct.unpack_from("<6I", blob)
        require((magic, cpu, subtype) == (0xfeedfacf, 0x100000c, 0),
                "Expected ordinary little-endian arm64, not arm64e or another architecture")
        require(kind == 2, "Expected MH_EXECUTE kernel image")
        end = 32 + size
        require(end <= len(blob) and count <= size // 8, "Invalid load-command bounds")
        self.segments, self.symtabs = [], []
        offset = 32
        for _ in range(count):
            require(offset + 8 <= end, "Truncated load command")
            cmd, length = struct.unpack_from("<II", blob, offset)
            require(length >= 8 and length % 8 == 0 and offset + length <= end,
                    "Invalid load-command extent")
            if cmd == 0x19:
                require(length >= 72, "Truncated segment command")
                raw, va, extent, fileoff, filesize, _, _, sections, _ = struct.unpack_from("<16s4Q4I", blob, offset + 8)
                require(length == 72 + 80 * sections and filesize <= extent and
                        va + extent <= 1 << 64 and fileoff + filesize <= len(blob), "Invalid segment bounds")
                name = raw.split(b"\0", 1)[0].decode("ascii")
                self.segments.append((name, va, extent, fileoff, filesize))
            elif cmd == 2:
                require(length == 24, "Invalid LC_SYMTAB length")
                self.symtabs.append(struct.unpack_from("<4I", blob, offset + 8))
            offset += length
        require(offset == end and self.segments, "Inconsistent load-command size or no segments")
        require(len(self.symtabs) == 1, "Expected exactly one symbol table")
        # File-backed ranges may not alias. This also prevents ambiguous VA reads.
        for index, a in enumerate(self.segments):
            for b in self.segments[index + 1:]:
                if a[4] and b[4]:
                    require(not (a[1] < b[1] + b[4] and b[1] < a[1] + a[4]), "Overlapping backed virtual segments")
                    require(not (a[3] < b[3] + b[4] and b[3] < a[3] + a[4]), "Overlapping backed file segments")

    def offset(self, address, length):
        require(type(address) is int and type(length) is int and address >= 0 and length > 0 and
                address + length <= 1 << 64, "Invalid virtual read")
        matches = [fo + address - va for _, va, _, fo, fs in self.segments
                   if va <= address and address + length <= va + fs]
        require(len(matches) == 1, f"Ambiguous or unbacked virtual read {address:#x}+{length:#x}")
        return matches[0]

    def read(self, address, length):
        offset = self.offset(address, length)
        return self.blob[offset:offset + length]

    def exports(self):
        symoff, count, stroff, size = self.symtabs[0]
        require(symoff + count * 16 <= len(self.blob) and stroff + size <= len(self.blob),
                "Symbol table outside image")
        strings = self.blob[stroff:stroff + size]
        by_address, by_name = defaultdict(set), defaultdict(set)
        for i in range(count):
            strx, kind, _, _, address = struct.unpack_from("<IBBHQ", self.blob, symoff + 16 * i)
            if kind & 0xe0 or not kind & 1 or not kind & 0x0e:
                continue
            finish = strings.find(b"\0", strx)
            require(strx < size and finish >= strx, "Invalid symbol string")
            name = strings[strx:finish].decode("utf-8")
            require(name, "Empty exported symbol")
            by_address[address].add(name)
            by_name[name].add(address)
        return by_address, by_name


def unique(names, name):
    matches = names.get(name, set())
    require(len(matches) == 1, "Missing or ambiguous native export: " + name)
    return next(iter(matches))


def vptr(image, pc):
    a, b, c = struct.unpack("<3I", image.read(pc, 12))
    require(a & 0x9f00001f == 0x90000008 and b & 0xffc003ff == 0x91000108 and c == 0x91004108,
            "Unrecognized constructor ADRP/ADD/vptr sequence")
    imm = ((a >> 5) & 0x7ffff) * 4 + ((a >> 29) & 3)
    if imm & (1 << 20):
        imm -= 1 << 21
    return (pc & ~4095) + (imm << 12) + ((b >> 10) & 4095) + 16


def branch(image, pc):
    word, = struct.unpack("<I", image.read(pc, 4))
    require(word & 0xfc000000 == 0x94000000, "Expected direct BL constructor/destructor call")
    imm = word & 0x3ffffff
    if imm & (1 << 25):
        imm -= 1 << 26
    return pc + imm * 4


def audit_getters(image):
    result = []
    for name, address, field, wide in GETTERS:
        opcode = (0xf9400000 if wide else 0xb9400000) | ((field // (8 if wide else 4)) << 10)
        code = image.read(address, 8)
        require(code == struct.pack("<2I", opcode, 0xd65f03c0), "IOSurface getter mismatch: " + name)
        result.append({"name": name, "address": hex(address), "field_offset": field,
                       "field_bytes": 8 if wide else 4, "code": code.hex()})
    return result


def audit_dispatch(image, original):
    _, names = image.exports()
    original_exports, _ = original.exports()
    pure = unique(names, "___cxa_pure_virtual")
    new_client = unique(names, "__ZN9IOService13newUserClientEP4taskPvjPP12IOUserClient")
    tables, values = {}, {}
    code_start, code_size, _ = TEXT[FAMILY]
    for name, (pc, count) in CONSTRUCTORS.items():
        address = vptr(image, pc)
        old_address = vptr(original, pc - SLIDE)
        require(address == old_address + SLIDE, "Constructor vptr slide mismatch: " + name)
        current = struct.unpack(f"<{count}Q", image.read(address, count * 8))
        before = struct.unpack(f"<{count}Q", original.read(old_address, count * 8))
        require(all(current), "Null vtable entry: " + name)
        methods = []
        for slot, (old, actual) in enumerate(zip(before, current)):
            native_names = []
            if code_start - SLIDE <= old < code_start - SLIDE + code_size:
                expected, owner = old + SLIDE, FAMILY
            else:
                source_names = original_exports.get(old, set())
                native_names = sorted(n for n in source_names if n in names)
                addresses = {a for n in native_names for a in names[n]}
                require(len(addresses) == 1, f"Original/native symbol binding not unique: {name} slot {slot}")
                expected, owner = next(iter(addresses)), "native-kernel"
            require(expected == actual, f"Vtable rebinding mismatch: {name} slot {slot}")
            methods.append({"slot": slot, "original_address": hex(old), "address": hex(actual),
                            "owner": owner, "native_export_names": native_names,
                            "pure": actual == pure, "prototype_qualified": False})
        tables[name] = {"address": hex(address), "original_address": hex(old_address),
                        "entries": count, "methods": methods}
        values[name] = current
    base = values["IOMobileFramebuffer"]
    service = values["IOMobileFramebufferService"]
    legacy = values["IOMobileFramebufferLegacy"]
    require([i for i, (a, b) in enumerate(zip(base, service)) if a != b] == [0, 1, 7],
            "Service operational override set changed")
    require([i for i, x in enumerate(legacy) if x == pure] == PURE, "Legacy abstract slot set changed")
    changed = [i for i, (a, b) in enumerate(zip(base, legacy)) if a != b]
    require(len(changed) == 103 and [i for i in changed if i < 168] == [0, 1, 7, 14, 21, 44, 79, 85, 86, 87, 111, 141, 155],
            "Legacy inherited override set changed")
    require(base[141] == new_client, "IOService newUserClient four-argument dispatch changed")
    legacy_address = int(tables["IOMobileFramebufferLegacy"]["address"], 16)
    require(image.read(legacy_address + 358 * 8, 24) == bytes(24), "Legacy vtable terminal boundary changed")
    for slot, target in enumerate(legacy):
        if slot >= 168 and slot not in PURE:
            require(code_start <= target < code_start + code_size, "Generic method outside original family code")
    return {"tables": tables, "legacy_abstract_slots": PURE,
            "legacy_inherited_override_slots": changed, "entries_rebound_and_compared": 882,
            "runtime_object_created": False}


def audit_method_bytes(image, dispatch):
    checks = []
    def exact(label, address, expected):
        raw = bytes.fromhex(expected)
        require(image.read(address, len(raw)) == raw, "Method bytes mismatch: " + label)
        checks.append({"label": label, "address": hex(address), "code": expected})
    for label, pc, target in [
        ("Legacy C2 to Service C2", 0xfffffff0057865fc, 0xfffffff00577b510),
        ("Service C2 to Framebuffer C2", 0xfffffff00577b540, 0xfffffff00577b318),
        ("Legacy D2 to Service D2", 0xfffffff005786630, 0xfffffff00577b55c),
    ]:
        require(branch(image, pc) == target, "Original call chain mismatch: " + label)
        checks.append({"label": label, "address": hex(pc), "target": hex(target)})
    exact("startHardware bool dispatch", 0xfffffff005787ef8, "080040f9088144f900013fd6e0000037")
    exact("completeCurrentSwap original true path", 0xfffffff005795b98, "e00740f9280080520101001272d3ff97")
    exact("optional report slot337 null outputs", 0xfffffff00578aa7c, "080040f9084545f9030080d2e20303aa0400805200013fd6")
    exact("isMainDisplay bool dispatch", 0xfffffff005777e9c, "080040f908e142f900013fd6c0000036")
    exact("vsync sender prologue", 0xfffffff005796720, "ff0302d1fd7b07a9fdc30191a0831ff8")
    exact("vsync client queue field", 0xfffffff005796830, "08ed4af9")
    exact("vsync six-word async sender", 0xfffffff00579685c, "e2230091c3008052240080521684ff97")
    exact("vsync registration offset", 0xfffffff005795fbc, "0ab182d2")
    exact("refresh 1/60s minimum", 0xfffffff00579ea68, "888888d28888a0f2")
    exact("refresh virtual dispatch", 0xfffffff00579ec54, "08b544f9")
    exact("refresh publisher prologue", 0xfffffff00579ebb8, "ff0302d1fd7b07a9fdc30191a0031ff8")
    exact("refresh publisher setProperty", 0xfffffff00579ed54, "086540f90173ff90218c0c9100013fd6")
    methods = dispatch["tables"]["IOMobileFramebufferLegacy"]["methods"]
    require(int(methods[295]["address"], 16) == 0xfffffff005795b88 and
            int(methods[301]["address"], 16) == 0xfffffff00579ea5c, "Qualified Legacy slot binding changed")
    getter = int(methods[249]["address"], 16)
    for offset, field in [(0x30, 5808), (0x58, 5812)]:
        expected = 0xb9400000 | ((field // 4) << 10) | (8 << 5) | 8
        require(struct.unpack("<I", image.read(getter + offset, 4))[0] == expected,
                "Display dimension getter field changed")
        checks.append({"label": "display dimension field", "address": hex(getter + offset), "field_offset": field})
    return checks


def audit(kernel, metadata_blob, original_blob, expected_sha, profile=PROFILE):
    require(profile == PROFILE, "Only the pinned 21H461 profile is supported")
    require(bool(re.fullmatch("[0-9a-f]{64}", expected_sha)), "Expected kernel SHA-256 must be 64 lowercase hex digits")
    require(digest(kernel) == expected_sha, "Kernel does not match the explicit expected SHA-256")
    require(digest(original_blob) == ORIGINAL_SHA, "Original kernelcache does not match pinned 21H461 SHA-256")
    plan = strict_json(metadata_blob)
    require(plan.get("candidate_sha256") == expected_sha, "Metadata candidate SHA mismatch")
    require(plan.get("original_sha256") == ORIGINAL_SHA, "Metadata original SHA mismatch")
    require(integer(plan.get("candidate_bytes"), "candidate_bytes") == len(kernel), "Metadata candidate size mismatch")
    require(integer(plan.get("slide"), "slide") == SLIDE, "Unsupported prelink slide")
    require(isinstance(plan.get("components"), list) and all(name in plan["components"] for name in TEXT),
            "Display family or IOSurface missing from retained components")
    require(isinstance(plan.get("extents"), list) and isinstance(plan.get("original_executable_sha256"), dict),
            "Missing prelink extent/hash records")
    image, original = MachO(kernel), MachO(original_blob)
    family_records = {}
    for name, (address, size, expected) in TEXT.items():
        extents = [e for e in plan["extents"] if isinstance(e, dict) and e.get("module") == name and e.get("segment") == "__TEXT_EXEC"]
        require(len(extents) == 1 and type(extents[0].get("va")) is int and type(extents[0].get("size")) is int and
                (extents[0]["va"], extents[0]["size"]) == (address, size), "Pinned family extent mismatch: " + name)
        require(plan["original_executable_sha256"].get(name) == expected, "Metadata family hash mismatch: " + name)
        before, after = original.read(address - SLIDE, size), image.read(address, size)
        require(digest(before) == expected and digest(after) == expected and before == after,
                "Retained Apple executable changed: " + name)
        family_records[name] = {"address": hex(address), "original_address": hex(address - SLIDE),
                                "bytes": size, "sha256": expected, "byte_identical": True}
    getters = audit_getters(image)
    dispatch = audit_dispatch(image, original)
    method_bytes = audit_method_bytes(image, dispatch)
    return {
        "schema": 1, "profile": PROFILE,
        "scope": "Static retained-family code, native dispatch rebinding and selected method instructions only",
        "kernel_sha256": expected_sha, "kernel_bytes": len(kernel), "metadata_sha256": digest(metadata_blob),
        "original_sha256": ORIGINAL_SHA, "slide": SLIDE, "retained_executables": family_records,
        "iosurface_getters": getters, "dispatch": dispatch, "selected_method_instruction_checks": method_bytes,
        "qualification": {
            "retained_family_code": True, "seven_iosurface_getters": True,
            "three_vtable_rebinding_checks": True, "selected_method_instruction_checks": True,
            "complete_display_abi": False, "compiled_adapter": False,
            "runtime_metadata": False, "runtime_qualified": False, "device_qualified": False,
        },
        "remaining": [
            "Confirm exact metaclass identity/ancestry and instance size without inventing runtime metadata",
            "Audit swap request fields and queue mutation/retirement semantics before generating ABI storage",
            "Compile adapter against this kernel's actual headers/records and validate native inheritance/overrides",
            "Supply and audit board provider, strong callbacks, late constructors, archive recipe and callers",
            "Validate workloop/PM, IOSurface lifetime, cancellation, cache/scanout ownership and physical output",
        ],
        "inputs_modified": False, "runtime_checkpoint_supplied_or_created": False,
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--original-kernelcache", type=Path, required=True)
    parser.add_argument("--kernel-sha256", required=True)
    parser.add_argument("--profile", default=PROFILE)
    parser.add_argument("--output", type=Path, help="New JSON file only; omitted means stdout")
    args = parser.parse_args(argv)
    inputs = [args.kernel, args.metadata, args.original_kernelcache]
    try:
        require(all(path.is_file() for path in inputs), "Every explicit input must be an existing file")
        if args.output:
            require(args.output.resolve() not in {p.resolve() for p in inputs}, "Output aliases an input")
            require(not args.output.exists(), "Refusing to overwrite an existing output")
            require(args.output.parent.is_dir(), "Output parent must already exist")
        blobs = [p.read_bytes() for p in inputs]
        result = audit(*blobs, args.kernel_sha256, args.profile)
        require(all(digest(p.read_bytes()) == digest(b) for p, b in zip(inputs, blobs)), "An input changed during audit")
        result["inputs"] = {name: {"path": p.as_posix(), "bytes": len(b), "sha256": digest(b)}
                            for name, p, b in zip(("kernel", "metadata", "original_kernelcache"), inputs, blobs)}
        result["auditor_sha256"] = digest(Path(__file__).read_bytes())
        rendered = json.dumps(result, ensure_ascii=False, indent=2, sort_keys=True) + "\n"
        if args.output:
            with args.output.open("x", encoding="utf-8", newline="\n") as stream:
                stream.write(rendered)
            print("PASS: static display subset; complete display ABI/runtime remain unqualified: " + str(args.output))
        else:
            print(rendered, end="")
        return 0
    except (AuditError, OSError, UnicodeError, struct.error) as exc:
        print("FAIL: " + str(exc), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
