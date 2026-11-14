#!/usr/bin/env python3
"""Independent byte verification of the fixed next-init after-call six-second reset candidate."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tomllib
import zlib

sys.dont_write_bytecode = True
from verify_menu_artifact import require, sha256, load_json
from verify_initrd80_probe import checked_relative, check_identity, verify_fd_provenance
from verify_first_payload import macho_read, file_offset, branch, branch_target
from verify_xnu_entry import read_fat32, validate_adt, compare_adt_baseline

KERNEL_SHA = "62215d029ca1b96f5c140abf2fb435e97da8b3ae3574ef10b568a0b8a980f3e8"
FD_SHA = "babe4f28e73d29503a561d53c9b909f0a5561afd930c078ac582eda3ad6bf1c3"
DTB_SHA = "6c87f77599a2056befac0080718d826205afed462b9abacac7aa0664e4d3e6e3"
SHIM_SHA = "72bfc9e9137a0e3ab9f7264162968c546ba63533b94d6813814fe84c831d03b4"
BASELINE_ADT_SHA = "97e98f4911923d5b4c9c2b27af582ad74c40c3db7870b35839bea9e1b091acc2"
FAT_BYTES = 83886080
MENU_BEFORE_SHA = "bac9ccbd4c060ad1525296f9741dde9fe6f3334f6da8419283c48cd87f29b5c2"
MENU_AFTER_SHA = "c354ed4ac95d3ab8f82ff7af0dd317ba10e53cdc55f3fae0c44f75162fa93518"
UNIXNU_EFI_SHA = "cbc65f8aa9a062bfc8f1ddb98ef1c1f58afa90f1efc3cf11be03d344759d439a"
CALLER_NAME = "_arm_init"
CALLEE_NAME = "_fiq_context_init"
CALLER_VA = 0xfffffff0079dbffc
CALLEE_VA = 0xfffffff00741a364
CALLER_END = 0xfffffff0079dcacc
EXPECTED_CALL_SITES = (0xfffffff0079dc908,)
OCCURRENCE = 1
ORIGINAL_CALL_WORD = 0x97e8f697
R3_CALL_SITE = 0xfffffff0079dc774
R3_CALL_WORD = 0x97e8be90


def validate_menu(raw):
    cfg = tomllib.loads(raw.decode("utf-8"))
    require(set(cfg) == {"version", "boot", "ui", "kits", "entry"} and cfg["version"] == 1 and
            cfg["boot"] == {"timeout": -1, "default": "tools", "rotate": 0} and
            cfg["ui"] == {"title": "J716F XNU next init checkpoint r4"} and cfg["kits"] == {"autoload": False},
            "Unexpected candidate menu title/default/hold policy")
    xnu = {"id": "xnu", "title": "Start XNU (next init checkpoint)", "type": "xnu",
           "path": "Payloads/kernel.macho", "args": "-v serial=3"}
    actions = [("tools", "Tools"), ("exit", "Return to firmware"), ("reset", "Restart"), ("shutdown", "Power off")]
    require(cfg["entry"] == [xnu] + [{"id": a, "title": t, "type": "action", "action": a} for a, t in actions],
            "Unexpected XNU or action entry; installation and extra boot arguments prohibited")
    return cfg


def paint_cave(stage, start, target):
    """Expected AArch64 instructions, independent of the official assembler."""
    require(stage in (1, 2), "Only the two framebuffer probes are supported")
    words = [0xf9401401, 0xf9401c02, 0xf9402403] if stage == 1 else [0xf9401681, 0xf9401e82, 0xf9402683]
    words += [
        0x9b037c42,  # mul x2,x2,x3 (row bytes * height)
        0xd344fc42,  # lsr x2,x2,#4 (one sixteenth of framebuffer)
        0x927df042,  # bic x2,x2,#7
        0xd2800024 if stage == 1 else 0xd2800044,  # band 1 or 2
        0x9b047c45, 0x8b050021, 0x52801fe6,
        0x72bfe006 if stage == 1 else 0x72bfffe6,  # blue or magenta
        0x28811826, 0xf1002042, 0x54ffffc8, 0xd5033f9f,
    ]
    if stage == 2:
        words.append(0xd5181000)  # displaced msr SCTLR_EL1,x0
    words.append(branch(start + 4 * len(words), target))
    return struct.pack("<" + "I" * len(words), *words).ljust(96, b"\0")


def verify_reset_cave(body, cave, callee):
    """Require every instruction, delay literal and byte of the 96-byte cave.

    The timer loop is intentionally checked byte-for-byte: merely finding a
    counter read and an SMC somewhere would not prove that the delay precedes
    the reset, nor exclude a branch bypassing it.
    """
    require(len(body) == 96, "Stage 4 must occupy exactly one 96-byte cave")
    words = [
        branch(cave, callee) | 0x80000000,  # BL original callee before timer starts
        0xd53be021,  # mrs x1,CNTPCT_EL0
        0x580001c2,  # ldr x2,[pc,#56] -> 64-bit ticks at cave+64
        0x8b020022,  # add x2,x1,x2
        0xd5033fdf,  # loop: isb
        0xd53be021,  # mrs x1,CNTPCT_EL0
        0xeb02003f,  # cmp x1,x2
        0x54ffffa3,  # b.lo loop (cave+16)
        0x52800120,  # movz w0,#9
        0x72b08000,  # movk w0,#0x8400,lsl#16 -> PSCI_SYSTEM_RESET
        0xaa1f03e1, 0xaa1f03e2, 0xaa1f03e3,  # x1/x2/x3 = 0
        0xd4000003,  # smc #0
        0xd503205f, 0x17ffffff,  # wfe; b back to wfe if SMC returns
    ]
    require(body[:64] == struct.pack("<16I", *words), "Stage 4 instruction sequence differs")
    require(struct.unpack_from("<Q", body, 64)[0] == 115200000,
            "Stage 4 delay is not six seconds at 19.2 MHz")
    require(not any(body[72:]), "Nonzero bytes after the stage 4 literal")
    return {"cave_bytes": 96, "instruction_bytes": 64, "literal_offset": 64,
            "timebase_hz": 19200000, "delay_seconds": 6, "delay_ticks": 115200000,
            "counter": "CNTPCT_EL0", "reset_function": "0x84000009", "conduit": "SMC #0",
            "displaced_instruction": "bl " + CALLEE_NAME, "callee": hex(callee),
            "call_return_va": hex(cave + 4), "reset_after_callee_returns": True,
            "fourth_framebuffer_bar": False}


def defined_symbols(data):
    """Read all defined LC_SYMTAB names to identify the caller boundary independently."""
    count, commands_bytes = struct.unpack_from("<II", data, 16)
    cursor, limit, symtab = 32, 32 + commands_bytes, None
    require(limit <= len(data), "Invalid symbol command extent")
    for _ in range(count):
        require(cursor + 8 <= limit, "Truncated symbol load command")
        cmd, length = struct.unpack_from("<II", data, cursor)
        require(length >= 8 and cursor + length <= limit, "Invalid symbol load command")
        if cmd == 2:
            require(length == 24 and symtab is None, "Missing/duplicate LC_SYMTAB")
            symtab = struct.unpack_from("<4I", data, cursor + 8)
        cursor += length
    require(cursor == limit and symtab is not None, "Missing/invalid symbol table")
    offset, count, strings, string_bytes = symtab
    require(offset + count * 16 <= len(data) and strings + string_bytes <= len(data), "Symbol extent exceeds image")
    result = []
    for index in range(count):
        at, kind, _, _, value = struct.unpack_from("<IBBHQ", data, offset + index * 16)
        require(at < string_bytes, "Symbol name is outside string table")
        end = data.find(b"\0", strings + at, strings + string_bytes)
        require(end >= 0, "Unterminated symbol name")
        name = data[strings + at:end].decode("utf-8")
        if name and kind & 0xe == 0xe and not kind & 0xe0:
            result.append((name, value))
    return result


def bl_target(word, address):
    require(word & 0xfc000000 == 0x94000000, "Expected an AArch64 BL")
    return branch_target(word & ~0x80000000, address)


def probe_readback(original, patched):
    """Allow only hooks/caves 1, 2 and 4; the former stage 3 remains original."""
    require(sha256(original) == KERNEL_SHA, "Unrecognized unpatched kernel")
    require(len(original) == len(patched) and original != patched,
            "Expected a same-size diagnostic kernel copy")
    require(b"_source_corecrypto14_bootstrap\0" not in original,
            "This fixed candidate unexpectedly requires POST resealing")
    image = macho_read(original)
    require(macho_read(patched) == image, "Diagnostic patch changed kernel metadata")
    require(CALLER_NAME is not None and CALLEE_NAME is not None and EXPECTED_CALL_SITES and
            isinstance(OCCURRENCE, int) and 1 <= OCCURRENCE <= len(EXPECTED_CALL_SITES),
            "Next checkpoint has not been reviewed and pinned")
    entry, symbols = image["entry"], defined_symbols(original)
    names = {}
    for key in ("_start_first_cpu", "_panic", "_panic_trap_to_debugger", "_vinfo", CALLER_NAME, CALLEE_NAME):
        matches = {value for name, value in symbols if name == key}
        require(len(matches) == 1, "Missing or ambiguous probe symbol " + key)
        names[key] = next(iter(matches))
    first, caller, callee = names["_start_first_cpu"], names[CALLER_NAME], names[CALLEE_NAME]
    require((caller, callee) == (CALLER_VA, CALLEE_VA), "Unexpected fixed caller/callee address")
    require({key for key, value in symbols if value == caller} == {CALLER_NAME} and
            {key for key, value in symbols if value == callee} == {CALLEE_NAME}, "Caller/callee has a same-address named alias")
    boundary = min(value for _, value in symbols if value > caller)
    require(boundary == CALLER_END, "Caller function boundary changed")

    def word(data, address):
        return struct.unpack_from("<I", data, file_offset(image, address))[0]

    def span(data, start, length):
        at = file_offset(image, start, length)
        return data[at:at + length]

    matches = [va for va in range(first, first + 0x1000, 4)
               if word(original, va) == 0xd5181000 and word(original, va + 4) == 0xd5033fdf
               and word(original, va + 0x20) == 0xd2a00600 and word(original, va + 0x24) == 0xd5181040]
    require(len(matches) == 1, "MMU diagnostic site is not unique")
    mmu, post_mmu = matches[0], matches[0] + 0x20
    calls = [va for va in range(caller, boundary, 4)
             if word(original, va) & 0xfc000000 == 0x94000000 and bl_target(word(original, va), va) == callee]
    require(tuple(calls) == EXPECTED_CALL_SITES, "Caller BL occurrences differ from the reviewed next checkpoint")
    call = calls[OCCURRENCE - 1]
    require(call != R3_CALL_SITE and word(original, call) == ORIGINAL_CALL_WORD, "Original next-call encoding/site changed")
    require(word(original, R3_CALL_SITE) == R3_CALL_WORD and word(patched, R3_CALL_SITE) == R3_CALL_WORD,
            "Old r3 arm_vm_init hook must be restored; early reset would prevent reaching next init")
    cave1, cave2, cave3, cave4 = entry + 0x78, entry + 0xd8, entry + 0x138, entry + 0x198
    injection_limit = cave4 + 96
    text_va, text_bytes, _ = image["sections"][(b"__TEXT_EXEC", b"__text")]
    require(text_va <= cave1 < injection_limit <= text_va + text_bytes, "Probe cave is not in persistent executable text")
    require(word(original, entry) == branch(entry, first), "Unexpected original entry instruction")
    for name, site, cave in (("stage1", entry, cave1), ("stage2", mmu, cave2), ("stage4", call, cave4)):
        require(word(patched, site) == branch(site, cave), name + " patch branch differs")
        require(branch_target(word(patched, site), site) == cave, name + " branch does not reach its cave")
    require(word(original, post_mmu) == 0xd2a00600 and word(patched, post_mmu) == word(original, post_mmu),
            "Stage 3 hook must remain original; early reset would prevent reaching C init")
    require(not any(span(original, cave1, injection_limit - cave1)), "Probe injection overwrites original non-padding bytes")
    require(span(patched, cave3, 96) == span(original, cave3, 96), "Unused stage 3 cave changed")
    require(span(patched, cave1, 96) == paint_cave(1, cave1, first), "Stage 1 exact framebuffer probe differs")
    require(span(patched, cave2, 96) == paint_cave(2, cave2, mmu + 4), "Stage 2 exact framebuffer probe differs")
    require(bl_target(word(patched, cave4), cave4) == callee, "Stage 4 BL does not call the reviewed original callee")
    reset = verify_reset_cave(span(patched, cave4, 96), cave4, callee)
    for name in ("_panic", "_panic_trap_to_debugger"):
        require(word(patched, names[name]) == word(original, names[name]), "The reset-only candidate must not patch " + name)
    ranges = [(file_offset(image, va), file_offset(image, va) + size)
              for va, size in ((entry, 4), (mmu, 4), (call, 4), (cave1, 96), (cave2, 96), (cave4, 96))]
    ranges.sort()
    cursor, changed = 0, 0
    for start, end in ranges:
        require(start >= cursor, "Diagnostic patch ranges overlap")
        require(original[cursor:start] == patched[cursor:start], "Kernel changed outside permitted diagnostic ranges (including unused stage 3)")
        changed += sum(a != b for a, b in zip(original[start:end], patched[start:end]))
        cursor = end
    require(original[cursor:] == patched[cursor:], "Kernel tail changed outside diagnostics")
    return {"changed_bytes": changed, "entry": hex(entry), "mmu_site": hex(mmu), "post_mmu_site": hex(post_mmu),
            "call_site": hex(call), "original_call_word": hex(word(original, call)),
            "caller": CALLER_NAME, "caller_range": [hex(caller), hex(boundary)], "callee": CALLEE_NAME, "callee_va": hex(callee), "occurrence": OCCURRENCE,
            "all_callee_calls_in_caller": [hex(va) for va in calls],
            "old_r3_call_site": hex(R3_CALL_SITE), "old_r3_hook_unchanged": True,
            "stage_caves": {"1": hex(cave1), "2": hex(cave2), "4": hex(cave4)},
            "unused_stage3_cave": [hex(cave3), hex(cave3 + 96)], "stage3_hook_and_cave_unchanged": True,
            "panic_screen": False, "panic_hooks_unchanged": True,
            "original_panic_sites": {name: hex(names[name]) for name in ("_panic", "_panic_trap_to_debugger")},
            "patch_file_ranges": [[hex(a), hex(b)] for a, b in ranges],
            "cave_sha256": {str(stage): sha256(span(patched, cave, 96)) for stage, cave in ((1,cave1),(2,cave2),(4,cave4))},
            "reset_probe": True, "reset_stage": 4, "reset_after": CALLEE_NAME, "reset_before": False, "reset": reset,
            "post_reseal_required": False,
            "scope": "Exact stage 1/2/4 bytes and branches; only three hooks and three 96-byte caves may differ. Stage 3, panic functions and every other byte remain original. Execution requires hardware."}


def validate_board(board):
    spec = board["payload"]
    require({key: value for key, value in spec.items() if key.startswith("entry_probe_reset_")} == {
                "entry_probe_reset_delay": 6, "entry_probe_reset_after": CALLEE_NAME,
                "entry_probe_reset_in": CALLER_NAME, "entry_probe_reset_occurrence": OCCURRENCE} and
            spec.get("panic_screen") is False,
            "Expected only the official six-second after-call stage 4 configuration without panic injection")
    require(board["board"]["name"] == "j716f-xnu-next-init-probe" and
            board["kernel"]["defconfig"] == "j716f_xnu_next_init_probe_defconfig" and
            board["package"]["boot_partition_bytes"] == 100663296, "Wrong next-init target/capacity")
    require(spec.get("volume") == "J716FENTRY" and spec.get("size_mib") == 80 and
            spec.get("kernel_args") == ["-v", "serial=3"] and spec.get("timeout") == -1 and
            spec.get("tools") is True, "Unexpected board payload policy")
    return board


def validate_footer_record(binary):
    require(binary.get("status") == "passed" and binary.get("menu_hold") == 1 and
            binary.get("max_initrd_crc_bytes") == FAT_BYTES and
            binary.get("select_an_entry_utf16_present") is False and
            binary.get("diagnostic_status_line_0_skipped") is True and
            binary.get("diagnostic_line0_status_call_absent") is True and
            binary.get("menu_source_only_diagnostic_status_line_0_change") is True and
            binary.get("allowed_changed_module") == "UniXNU.efi" and
            set(binary.get("unchanged_modules", [])) == {
                "J716fHandoffDiagnosticsDxe.efi", "Sec.efi", "AdvancedLogDxe.efi"} and
            binary.get("unrelated_module_hashes_unchanged") is True and
            binary.get("early_protection_linked") is True and
            binary.get("qr_libraries_preserved") is True and
            binary.get("menu_host_case_count") == 39 and
            binary.get("rendering_root_cause_established") is False and
            binary.get("hardware_rendering_fix_verified") is False and
            binary.get("hardware_tested") is False,
            "New firmware lacks the reviewed diagnostic footer/CRC/hold/QR provenance")
    require(binary["modules"]["UniXNU.efi"]["sha256"] == UNIXNU_EFI_SHA and
            binary["baseline_fd"]["sha256"] == "f9d07d7c5b449b966f1a7cdbad567c59dec00f0900650bd36685adb11e03b3d7" and
            binary["menu_source_before"]["sha256"] == MENU_BEFORE_SHA and
            binary["menu_source_after"]["sha256"] == MENU_AFTER_SHA,
            "Footer provenance source/module identities differ from reviewed values")
    return binary


def verify_footer_provenance(root):
    evidence = load_json(root / "fd-provenance.json")["evidence"]
    selected = {}
    for role in ("binary-verification", "menu-source", "menu-source-before", "menu-host-tests"):
        matches = [item for item in evidence if item["role"] == role]
        require(len(matches) == 1, "Missing or ambiguous footer provenance role " + role)
        selected[role] = matches[0]
    paths = {role: root / "fd-evidence" / checked_relative(item["path"])
             for role, item in selected.items()}
    # verify_fd_provenance already checked each evidence file's path and hash.
    binary = validate_footer_record(load_json(paths["binary-verification"]))
    for role, label, digest in (("menu-source", "menu_source_after", MENU_AFTER_SHA),
                                ("menu-source-before", "menu_source_before", MENU_BEFORE_SHA)):
        raw = paths[role].read_bytes()
        require(sha256(raw) == digest and binary[label] == {"bytes": len(raw), "sha256": digest},
                "Footer source evidence differs from binary verification record")
    return {"status": "passed", "select_an_entry_footer_suppressed": True, "diagnostic_status_line_0_skipped": True,
            "menu_hold": 1, "allowed_changed_module": "UniXNU.efi", "unixnu_efi_sha256": UNIXNU_EFI_SHA,
            "menu_source_sha256": MENU_AFTER_SHA,
            "scope": "Pinned firmware plus archived source/build assertions; no screen or execution result inferred"}


def verify(root, expected_fd_sha):
    require(FD_SHA is not None and MENU_AFTER_SHA is not None and UNIXNU_EFI_SHA is not None and
            expected_fd_sha == FD_SHA, "Explicit firmware pin must match the reviewed next-init candidate FD")
    root = root.resolve(strict=True)
    manifest = load_json(root/"manifest.json")
    required = {"boot.img","boot.json","payload.img","payload.json","payload.official.json","unixnu.toml","original-kernel.macho","baseline.adt",
                "SILICIUM_UEFI.fd","firmware.dtb","shim.bin","board.toml","fd-provenance.json","fd-source.json","fd-archive-manifest.json","build-result.json"}
    require(required <= manifest["outputs"].keys(), "Incomplete artifact manifest")
    require(manifest.get("hardware_tested") is False and manifest.get("deployment_ready") is False, "Host result cannot claim deployment qualification")
    for name,spec in manifest["outputs"].items():
        path = (root/checked_relative(name)).resolve(strict=True)
        require(path.is_relative_to(root), "Manifest escapes artifact")
        check_identity(path,spec)
    data, ramdisk, fd, dtb, shim, original = [(root/name).read_bytes() for name in ("boot.img","payload.img","SILICIUM_UEFI.fd","firmware.dtb","shim.bin","original-kernel.macho")]
    for raw,digest in ((fd,expected_fd_sha),(dtb,DTB_SHA),(shim,SHIM_SHA),(original,KERNEL_SHA)):
        require(sha256(raw) == digest, "A fixed candidate input changed")
    firmware = verify_fd_provenance(root,fd)
    firmware["footer_provenance"] = verify_footer_provenance(root)
    require(data[:8] == b"ANDROID!" and 1660 < len(data) <= 100663296, "Invalid Android header/capacity")
    ks,ka,rs,ra,ss,sa,tags,page,version,osver = struct.unpack_from("<10I",data,8)
    recovery,roff,hsize = struct.unpack_from("<IQI",data,1632)
    ds,da = struct.unpack_from("<IQ",data,1648)
    require((ka,rs,ra,ss,sa,tags,page,version,recovery,roff,hsize,ds,da) == (0x8000,FAT_BYTES,0x1000000,0,0,0x100,4096,2,0,0,1660,len(dtb),0x1f00000), "Unexpected boot offsets/size")
    aligned = lambda n:(n+page-1)//page*page
    rp = page+aligned(ks)
    dp = rp+aligned(rs)
    require(ks > 0 and len(data) == dp+aligned(ds), "Invalid boot extent")
    require(not any(data[hsize:page]) and not any(data[page+ks:rp]) and not any(data[rp+rs:dp]) and not any(data[dp+ds:]), "Nonzero boot padding")
    kernel = data[page:page+ks]
    decoder = zlib.decompressobj(16+zlib.MAX_WBITS)
    decoded = decoder.decompress(kernel,len(shim)+len(fd)+1)
    require(decoded == shim+fd and decoder.eof and not decoder.unconsumed_tail and decoder.unused_data == dtb, "Invalid shim/FD gzip or DTB tail")
    require(data[rp:rp+rs] == ramdisk and data[dp:dp+ds] == dtb, "Payload/DTB does not match boot image")
    digest = hashlib.sha1()
    for raw in (kernel,ramdisk,b"",b"",dtb):
        digest.update(raw);digest.update(struct.pack("<I",len(raw)))
    require(data[576:608] == digest.digest()+b"\0"*12,"Android image ID mismatch")
    files,geometry = read_fat32(ramdisk)
    require(set(files) == {"efi/unixnu/unixnu.toml","efi/unixnu/payloads/kernel.macho","efi/unixnu/payloads/kernel.macho.adt"}, "Unexpected payload files")
    require(files["efi/unixnu/unixnu.toml"] == (root/"unixnu.toml").read_bytes(), "Menu bytes differ")
    cfg = validate_menu(files["efi/unixnu/unixnu.toml"])
    xnu,adt = files["efi/unixnu/payloads/kernel.macho"],files["efi/unixnu/payloads/kernel.macho.adt"]
    diagnostic = probe_readback(original,xnu)
    board = tomllib.loads((root/"board.toml").read_text(encoding="utf-8"))
    validate_board(board)
    adt_report = validate_adt(adt)
    adt_comparison = compare_adt_baseline((root/"baseline.adt").read_bytes(), adt)
    payload = load_json(root/"payload.json")
    official = load_json(root/"payload.official.json")
    for name in ("kernel_sha256","payload_kernel_sha256","adt_sha256","kernel_args","panic_screen","trust_cache_sha256","gpu_addon_sha256","data_ramdisk_sha256","hardware_tested","skip_setup"):
        require(payload[name] == official[name], "Menu adaptation changed official payload metadata: "+name)
    require(payload["kernel_sha256"] == KERNEL_SHA and payload["payload_kernel_sha256"] == sha256(xnu) and payload["adt_sha256"] == sha256(adt), "Kernel/ADT report mismatch")
    require(payload["payload_sha256"] == sha256(ramdisk) and payload["payload_bytes"] == len(ramdisk) == FAT_BYTES and payload["kernel_args"] == "-v serial=3" and payload["panic_screen"] is False and payload["skip_setup"] is False and payload["hardware_tested"] is False, "Payload metadata mismatch")
    require(all(payload[n] is None for n in ("trust_cache_sha256","gpu_addon_sha256","data_ramdisk_sha256")), "Unexpected optional input")
    build = load_json(root/"build-result.json")
    require(build["status"] == "host-generation-passed" and build["hardware_tested"] is False and build["device_actions"] is False, "Build result not host-success")
    expected = {"image_sha256":sha256(data),"image_bytes":len(data),"ramdisk_bytes":len(ramdisk),"header_version":2,"kernel_address":hex(ka),"tags_address":hex(tags),"dtb_address":hex(da),"dtb_file_offset":dp,"kernel_sha256":sha256(kernel),"fd_sha256":sha256(fd),"dtb_sha256":sha256(dtb),"dtb_bytes":len(dtb),"shim_bytes":len(shim),"hardware_tested":False}
    boot = load_json(root/"boot.json")
    for key,value in expected.items():
        require(boot.get(key) == value,"boot.json mismatch: "+key)
    return {"result":"passed","scope":"Static next-init six-second after-call reset candidate verification", "image":expected,
            "payload_sha256":sha256(ramdisk),"payload_crc32":f"{zlib.crc32(ramdisk):08x}","payload_bytes":len(ramdisk),
            "menu_title":cfg["ui"]["title"],"menu_default":"tools","menu_entry_ids":[e["id"] for e in cfg["entry"]],
            "fat":geometry,"adt":adt_report,"adt_baseline_comparison":adt_comparison,"diagnostic_kernel":diagnostic,"firmware":firmware,
            "manifest_entries_verified":len(manifest["outputs"]),"hardware_tested":False,"deployment_ready":False,
            "files":{name:{"bytes":len(raw),"sha256":sha256(raw)} for name,raw in files.items()},
            "limits":["Runtime CRC must match this exact reset-probe payload; prior r1/r2/r3 integrity does not qualify it.","256 MiB is a loader allocation request, not an allocated/owned range.","No DarwinBoot, EBS, XNU, root or userland execution is inferred from host bytes.","A missing timed reset cannot distinguish callee failure, failed return mapping, counter failure or PSCI failure."]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("artifact",type=Path)
    parser.add_argument("--report",type=Path,required=True)
    parser.add_argument("--fd-sha256", required=True, help="Reviewed new firmware pin supplied independently of the artifact")
    args = parser.parse_args()
    require(not args.report.exists(),"Refusing to overwrite a report")
    result = verify(args.artifact, args.fd_sha256)
    args.report.write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    print(json.dumps(result,indent=2))
