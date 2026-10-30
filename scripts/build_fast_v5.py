#!/usr/bin/env python3
"""100% git-free builder & roundtrip auditor for J716F NATIVE FAST-V5.

Includes all FAST-V3 VM/MMU fixes + FAST-V4 post-vm_done/panic/kdbg instrumentation
PLUS the Run 4 root-cause fix in _PE_create_console (check_for_panic_log):
  - Redirects PLATFORM_PANIC_LOG_PADDR from Raspberry Pi 3's 0x3c0fc000
    (which sits in unmapped/unclocked SM8250 MMIO space and caused an immediate
    SoC bus hang on `ldr w8, [x0]` / `bzero` right after `47:KDBG_LAST = "kprintf"`)
    to 0xb00b8000..0xb00bc000 (16 KiB inside TB-J716F's reserved 0xb0000000..0xb0100000
    boot-log DRAM region, below the 0xb00be000..0xb00fe000 native-log window).

Zero git invocations.
"""
from __future__ import annotations

import argparse
import gzip
import hmac
import json
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT / "github-export/UniXNU-j716f"
sys.path.insert(0, str(REPO / "scripts"))
sys.path.insert(0, str(ROOT / "tools"))

import native_candidate  # noqa: E402
from native_image_formats import extract_fd, read_fat32  # noqa: E402
import build_fast_v1 as v1  # noqa: E402

EVENT_VA = 0xFFFFFFF0073F9C84
CAVE_BASE = 0xFFFFFFF0073FD9C0
CAVE_LIMIT = 0xFFFFFFF0073FE000


def asm_b(pc: int, target: int) -> int:
    diff = (target - pc) >> 2
    if not (-(1 << 25) <= diff < (1 << 25)):
        raise ValueError(f"B target out of range: {pc:#x} -> {target:#x}")
    return 0x14000000 | (diff & 0x3FFFFFF)


def asm_bl(pc: int, target: int) -> int:
    diff = (target - pc) >> 2
    if not (-(1 << 25) <= diff < (1 << 25)):
        raise ValueError(f"BL target out of range: {pc:#x} -> {target:#x}")
    return 0x94000000 | (diff & 0x3FFFFFF)


def movz_w0(imm16: int) -> int:
    return 0x52800000 | ((imm16 & 0xFFFF) << 5)


def movz_x1(imm16: int) -> int:
    return 0xD2800001 | ((imm16 & 0xFFFF) << 5)


def apply_v5_kernel_fixes(kernel_bytes: bytes) -> tuple[bytes, dict]:
    if v1.sha(kernel_bytes) != v1.EXPECTED_KERNEL_SHA:
        raise RuntimeError("Input kernel.macho does not match expected baseline SHA256")
    img = native_candidate.MachO(kernel_bytes)
    buf = bytearray(kernel_bytes)

    # Verify the cave region 0xfffffff0073fd9c0..0xfffffff0073fe000 is all zeros
    cave_off = img.offset(CAVE_BASE, CAVE_LIMIT - CAVE_BASE)
    if any(buf[cave_off : cave_off + (CAVE_LIMIT - CAVE_BASE)]):
        raise RuntimeError("Cave region 0xfffffff0073fd9c0..0xfffffff0073fe000 is not empty!")

    cave_words: list[int] = []

    def cur_va() -> int:
        return CAVE_BASE + len(cave_words) * 4

    def emit(w: int) -> None:
        cave_words.append(w & 0xFFFFFFFF)

    def emit_post_stub(orig_callee: int, slot: int) -> int:
        entry = cur_va()
        emit(0xA9BE7BFD)  # stp x29, x30, [sp, #-0x20]!
        emit(0x910003FD)  # mov x29, sp
        emit(asm_bl(cur_va(), orig_callee))
        emit(0xF9000BE0)  # str x0, [sp, #0x10]
        emit(movz_w0(slot))
        emit(0xF9400BE1)  # ldr x1, [sp, #0x10] (return value)
        emit(0xF94007E2)  # ldr x2, [sp, #0x08] (caller LR)
        emit(0xD2800003)  # mov x3, #0
        emit(asm_bl(cur_va(), EVENT_VA))
        emit(0xF9400BE0)  # ldr x0, [sp, #0x10]
        emit(0xA8C27BFD)  # ldp x29, x30, [sp], #0x20
        emit(0xD65F03C0)  # ret
        return entry

    # 1. Build post-call milestone stubs in cave
    post_hooks = [
        # (callsite_va, orig_callee_va, slot, name)
        (0xFFFFFFF0079EBD78, 0xFFFFFFF007958BBC, 33, "_arm_init post _PE_init_printf (33:serial_ok)"),
        (0xFFFFFFF0079EBD90, 0xFFFFFFF007959DB8, 34, "_arm_init post _PE_init_platform (34:pe_plat)"),
        (0xFFFFFFF0079EBDAC, 0xFFFFFFF0073F552C, 35, "_arm_init post _cpu_timebase_init (35:cpu_tb)"),
        (0xFFFFFFF0079EBDBC, 0xFFFFFFF007407A64, 36, "_arm_init post _fiq_context_init (36:pe_fiq)"),
        (0xFFFFFFF0079EBE40, 0xFFFFFFF0073D8984, 37, "_arm_init post _early_random (37:early_rng)"),
        (0xFFFFFFF007405908, 0xFFFFFFF007405690, 38, "_machine_startup post _machine_conf (38:kboot_ent)"),
        (0xFFFFFFF0079C2260, 0xFFFFFFF0073F6390, 54, "_kernel_bootstrap_thread entry (54:kboot_thr)"),
        (0xFFFFFFF0072C81C4, 0xFFFFFFF0079D2F84, 57, "_kernel_bootstrap post _vm_mem_bootstrap (57:vm_mem_ok)"),
        (0xFFFFFFF0072C8360, 0xFFFFFFF0073CD65C, 58, "_kernel_bootstrap post _console_init (58:con_ok)"),
        (0xFFFFFFF0072C83D8, 0xFFFFFFF0072B58FC, 59, "_kernel_bootstrap post _sched_init (59:sched_ok)"),
        (0xFFFFFFF0072C84CC, 0xFFFFFFF0074056D8, 60, "_kernel_bootstrap post _machine_init (60:mach_ok)"),
        (0xFFFFFFF0072C8544, 0xFFFFFFF00727D660, 61, "_kernel_bootstrap post _clock_init (61:clock_ok)"),
        (0xFFFFFFF0072C867C, 0xFFFFFFF0072DB89C, 62, "_kernel_bootstrap post _thread_init (62:thread_ok)"),
    ]

    stub_map: dict[int, tuple[int, int, str]] = {}
    for callsite, orig_callee, slot, desc in post_hooks:
        stub_va = emit_post_stub(orig_callee, slot)
        stub_map[callsite] = (asm_bl(callsite, orig_callee), asm_bl(callsite, stub_va), desc)

    # 2. Pre+Post stub for _PE_init_iokit (slot 55 pre, slot 56 post) at 0xfffffff0079c246c
    iokit_stub_va = cur_va()
    emit(0xA9BE7BFD)  # stp x29, x30, [sp, #-0x20]!
    emit(0x910003FD)  # mov x29, sp
    emit(movz_w0(55))
    emit(0xAA1E03E1)  # mov x1, x30
    emit(0xD2800002)  # mov x2, #0
    emit(0xD2800003)  # mov x3, #0
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(asm_bl(cur_va(), 0xFFFFFFF007959990))  # bl _PE_init_iokit
    emit(0xF9000BE0)  # str x0, [sp, #0x10]
    emit(movz_w0(56))
    emit(0xF9400BE1)  # ldr x1, [sp, #0x10]
    emit(0xD2800002)  # mov x2, #0
    emit(0xD2800003)  # mov x3, #0
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(0xF9400BE0)  # ldr x0, [sp, #0x10]
    emit(0xA8C27BFD)  # ldp x29, x30, [sp], #0x20
    emit(0xD65F03C0)  # ret
    stub_map[0xFFFFFFF0079C246C] = (
        asm_bl(0xFFFFFFF0079C246C, 0xFFFFFFF007959990),
        asm_bl(0xFFFFFFF0079C246C, iokit_stub_va),
        "_kernel_bootstrap_thread _PE_init_iokit (55:iokit_pre, 56:iokit_post)",
    )

    # 3. Pre-call stub for _load_context (slot 49) at 0xfffffff0072c8870
    load_ctx_stub_va = cur_va()
    emit(0xA9BE7BFD)  # stp x29, x30, [sp, #-0x20]!
    emit(0x910003FD)  # mov x29, sp
    emit(0xF9000BE0)  # str x0, [sp, #0x10]
    emit(0xAA0003E1)  # mov x1, x0 (thread)
    emit(0xAA1E03E2)  # mov x2, x30 (caller LR)
    emit(0xD2800003)  # mov x3, #0
    emit(movz_w0(49))
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(0xF9400BE0)  # ldr x0, [sp, #0x10]
    emit(0xA8C27BFD)  # ldp x29, x30, [sp], #0x20
    emit(asm_b(cur_va(), 0xFFFFFFF0072C89E4))  # b _load_context
    stub_map[0xFFFFFFF0072C8870] = (
        asm_bl(0xFFFFFFF0072C8870, 0xFFFFFFF0072C89E4),
        asm_bl(0xFFFFFFF0072C8870, load_ctx_stub_va),
        "_kernel_bootstrap pre _load_context (49:LOAD_CTX)",
    )

    # 4. _kernel_debug_string_early hook (slot 39 first + slot 47 rolling latest) at 0xfffffff0076c4248
    kdbg_stub_va = cur_va()
    emit(0xD10103FF)  # sub sp, sp, #0x40
    emit(0xA9037BFD)  # stp x29, x30, [sp, #0x30]
    emit(0x9100C3FD)  # add x29, sp, #0x30
    emit(0xA9017BE0)  # stp x0, x30, [sp, #0x10]
    emit(0xA9400C02)  # ldp x2, x3, [x0]
    emit(0xAA1E03E1)  # mov x1, x30
    emit(movz_w0(39))
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(0xF9400BE0)  # ldr x0, [sp, #0x10]
    emit(0xA9400C02)  # ldp x2, x3, [x0]
    emit(0xF9400FE1)  # ldr x1, [sp, #0x18]
    emit(movz_w0(47))
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(0xA9417BE0)  # ldp x0, x30, [sp, #0x10]
    emit(asm_b(cur_va(), 0xFFFFFFF0076C4254))
    stub_map[0xFFFFFFF0076C4248] = (
        0xD10103FF,  # sub sp, sp, #0x40
        asm_b(0xFFFFFFF0076C4248, kdbg_stub_va),
        "_kernel_debug_string_early entry (39:kdbg_1st + 47:KDBG_LAST)",
    )

    # 5. _ml_panic_trap_to_debugger hook (slot 45 PANIC_CALL + slot 46 PANIC_STR) at 0xfffffff0073ffa84
    panic_stub_va = cur_va()
    emit(0xA9BF7BFD)  # stp x29, x30, [sp, #-0x10]!
    emit(0x910003FD)  # mov x29, sp
    emit(0xD2800003)  # mov x3, #0
    emit(0xB4000096)  # cbz x22, +16 (skip deref if x22 == 0)
    emit(0xF94002C8)  # ldr x8, [x22] (x8 = va_list)
    emit(0xB4000048)  # cbz x8, +8
    emit(0xF9400103)  # ldr x3, [x8] (x3 = vararg0)
    emit(movz_w0(45))
    emit(0xAA1303E1)  # mov x1, x19 (panic_caller LR)
    emit(0xAA1703E2)  # mov x2, x23 (panic_str VA)
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(0xD2800001)  # mov x1, #0
    emit(0xB4000057)  # cbz x23, +8
    emit(0xF94002E1)  # ldr x1, [x23] (panic_str[0..7])
    emit(0xD2800002)  # mov x2, #0
    emit(0xD2800003)  # mov x3, #0
    emit(0xB4000096)  # cbz x22, +16
    emit(0xF94002C8)  # ldr x8, [x22]
    emit(0xB4000048)  # cbz x8, +8
    emit(0xA9408D02)  # ldp x2, x3, [x8, #8] (x2 = vararg1, x3 = vararg2)
    emit(movz_w0(46))
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(0xA8C17BFD)  # ldp x29, x30, [sp], #0x10
    emit(0xD65F03C0)  # ret
    stub_map[0xFFFFFFF0073FFA84] = (
        0xD65F03C0,  # ret
        asm_b(0xFFFFFFF0073FFA84, panic_stub_va),
        "_ml_panic_trap_to_debugger (45:PANIC_CALL + 46:PANIC_STR)",
    )

    # 6. FIQ & SError hooks (slot 48 SERR_FIQ)
    fiq_stub_va = cur_va()
    emit(0xA9BE7BFD)  # stp x29, x30, [sp, #-0x20]!
    emit(0x910003FD)  # mov x29, sp
    emit(0xF9000BE0)  # str x0, [sp, #0x10]
    emit(movz_w0(48))
    emit(movz_x1(0xF10))
    emit(0xD5385202)  # mrs x2, esr_el1
    emit(0xD5384023)  # mrs x3, elr_el1
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(0xF9400BE0)  # ldr x0, [sp, #0x10]
    emit(0xA8C27BFD)  # ldp x29, x30, [sp], #0x20
    emit(asm_b(cur_va(), 0xFFFFFFF0079EDA10))  # b _sleh_fiq
    stub_map[0xFFFFFFF0073FF6A0] = (
        asm_bl(0xFFFFFFF0073FF6A0, 0xFFFFFFF0079EDA10),
        asm_bl(0xFFFFFFF0073FF6A0, fiq_stub_va),
        "_fleh_fiq -> _sleh_fiq (48:SERR_FIQ)",
    )

    serr_stub_va = cur_va()
    emit(0xA9BD7BFD)  # stp x29, x30, [sp, #-0x30]!
    emit(0x910003FD)  # mov x29, sp
    emit(0xA90107E0)  # stp x0, x1, [sp, #0x10]
    emit(0xF90013E2)  # str x2, [sp, #0x20]
    emit(movz_w0(48))
    emit(movz_x1(0x5E0))
    emit(0xD5385202)  # mrs x2, esr_el1
    emit(0xD5384023)  # mrs x3, elr_el1
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(0xA94107E0)  # ldp x0, x1, [sp, #0x10]
    emit(0xF94013E2)  # ldr x2, [sp, #0x20]
    emit(0xA8C37BFD)  # ldp x29, x30, [sp], #0x30
    emit(asm_b(cur_va(), 0xFFFFFFF0079EDB08))  # b _sleh_serror
    stub_map[0xFFFFFFF0073FF6EC] = (
        asm_bl(0xFFFFFFF0073FF6EC, 0xFFFFFFF0079EDB08),
        asm_bl(0xFFFFFFF0073FF6EC, serr_stub_va),
        "_fleh_serror -> _sleh_serror (48:SERR_FIQ)",
    )

    if cur_va() > CAVE_LIMIT:
        raise RuntimeError(f"Cave overflow: {cur_va():#x} > {CAVE_LIMIT:#x}")

    # Write cave words into buf
    for idx, w in enumerate(cave_words):
        struct.pack_into("<I", buf, cave_off + idx * 4, w)

    patches = [
        # FAST-V3 1-5: T1SZ=26 L1 root mask + pre-TTBR1-switch linear PA->KVA
        (
            0xFFFFFFF0073F972C,
            "_j716f_native_bootstrap (T1SZ=26 L1 mask)",
            0xD35E9A30,
            0xD35E9630,
        ),
        (
            0xFFFFFFF0073FADD4,
            "j716f_native_vm_map (T1SZ=26 L1 mask)",
            0xD35E9B08,
            0xD35E9708,
        ),
        (
            0xFFFFFFF0073FADEC,
            "j716f_native_vm_map L2 resolve -> bootstrap linear PA->KVA",
            0x97FFF57D,
            0x97FFF5D7,
        ),
        (
            0xFFFFFFF0073FAE40,
            "j716f_native_vm_map L3 alloc -> bootstrap linear PA->KVA",
            0x97FFF568,
            0x97FFF5C2,
        ),
        (
            0xFFFFFFF0073FAEBC,
            "j716f_native_vm_map L3 resolve -> bootstrap linear PA->KVA",
            0x97FFF549,
            0x97FFF5A3,
        ),
        # FAST-V4 6-7: Allow slot 47 (KDBG_LAST) rolling overwrite in jl_write
        (
            0xFFFFFFF0073F9AC4,
            "jl_write allow slot 47 overwrite (cmp w1, #47)",
            0x35000D6A,  # cbnz w10, #0xfffffff0073f9c70
            0x7100BC3F,  # cmp w1, #47
        ),
        (
            0xFFFFFFF0073F9AC8,
            "jl_write allow slot 47 overwrite (csel w10, wzr, w10, eq)",
            0xB940750A,  # ldr w10, [x8, #0x74]
            0x1A8A03EA,  # csel w10, wzr, w10, eq
        ),
        # FAST-V5 8-9: Redirect _PE_create_console PLATFORM_PANIC_LOG_PADDR
        # from Raspberry Pi 3 0x3c0fc000 (SM8250 unmapped MMIO) to 0xb00b8000 (reserved DRAM)
        (
            0xFFFFFFF00795A158,
            "_PE_create_console panic_log_paddr low16 (0xc000 -> 0x8000)",
            0x52980000,  # mov w0, #0xc000
            0x52900000,  # mov w0, #0x8000
        ),
        (
            0xFFFFFFF00795A15C,
            "_PE_create_console panic_log_paddr high16 (0x3c0f -> 0xb00b => PA 0xb00b8000)",
            0x72A781E0,  # movk w0, #0x3c0f, lsl #16
            0x72B60160,  # movk w0, #0xb00b, lsl #16
        ),
    ]
    for va, (old_w, new_w, desc) in sorted(stub_map.items()):
        patches.append((va, desc, old_w, new_w))

    applied = []
    for va, sym, old_w, new_w in patches:
        foff = img.offset(va, 4)
        actual_w = struct.unpack_from("<I", buf, foff)[0]
        if actual_w != old_w:
            raise RuntimeError(
                f"Unexpected instruction at {va:#x} ({sym}): got {actual_w:#010x}, expected {old_w:#010x}"
            )
        struct.pack_into("<I", buf, foff, new_w)
        applied.append(
            {
                "va": f"0x{va:016x}",
                "file_offset": foff,
                "symbol": sym,
                "old": f"0x{old_w:08x}",
                "new": f"0x{new_w:08x}",
            }
        )

    # Recompute corecrypto FIPS POST SHA-256 HMAC over __TEXT_EXEC,__text -> __fips_hmacs
    text_sec = [s for s in img.sections if s[0] == b"__text" and s[1] in (b"__TEXT", b"__TEXT_EXEC")][0]
    hmac_sec = [s for s in img.sections if s[0] == b"__fips_hmacs"][0]
    text_off = img.offset(text_sec[2], text_sec[3])
    hmac_off = img.offset(hmac_sec[2], 32)
    new_hmac = hmac.digest(b"\0", bytes(buf[text_off : text_off + text_sec[3]]), "sha256")
    buf[hmac_off : hmac_off + 32] = new_hmac

    out = bytes(buf)
    checks = native_candidate.check_kernel(out)
    return out, {
        "cave_base": f"0x{CAVE_BASE:016x}",
        "cave_used_bytes": len(cave_words) * 4,
        "panic_log_paddr_redirect": "0x3c0fc000 -> 0xb00b8000 (16 KiB reserved DRAM)",
        "patches": applied,
        "checks": checks,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, default=ROOT / "debug/native-fast-stage-v5")
    parser.add_argument("--output", type=Path, default=ROOT / "Firmware/NATIVE-40458123294-FAST-V5")
    parser.add_argument("--ssh-host", default="10.1.2.110")
    parser.add_argument("--ssh-user", default="test")
    parser.add_argument("--stage-only", action="store_true")
    args = parser.parse_args()

    stage = args.stage.resolve()
    out_dir = args.output.resolve()
    manifest = v1.stage_fast_v1(stage)

    shutil.copyfile(v1.FAST_SRC / "NativeMapFixed.h", stage / "J716fNativeLog/NativeMapFixed.h")
    shutil.copyfile(v1.FAST_SRC / "NativeVmMapFixed.h", stage / "J716fNativeLog/NativeVmMapFixed.h")

    dsc = (stage / "Platforms/Lenovo/j716fPkg/j716f-native.dsc").read_text(encoding="utf-8")
    dsc = dsc.replace("J716F NATIVE FAST-V1 40458123294", "J716F NATIVE FAST-V5 40458123294")
    (stage / "Platforms/Lenovo/j716fPkg/j716f-native.dsc").write_text(dsc, encoding="utf-8", newline="\n")

    toml_txt = (stage / "unixnu.toml").read_text(encoding="utf-8")
    toml_txt = toml_txt.replace("NATIVE FAST-V1", "NATIVE FAST-V5")
    toml_txt = toml_txt.replace("J716F NATIVE FAST-V1", "J716F NATIVE FAST-V5 (PE_CONSOLE FIX)")
    (stage / "unixnu.toml").write_text(toml_txt, encoding="utf-8", newline="\n")

    orig_kernel = (stage / "inputs/kernel.macho").read_bytes()
    fixed_kernel, patch_info = apply_v5_kernel_fixes(orig_kernel)
    fixed_kernel_sha = v1.sha(fixed_kernel)
    (stage / "inputs/kernel.macho").write_bytes(fixed_kernel)

    manifest["profile"] = "FAST-V5"
    manifest["baseline_kernel_sha256"] = v1.EXPECTED_KERNEL_SHA
    manifest["prelinked_kernel_sha256"] = fixed_kernel_sha
    manifest["v5_fixes"] = patch_info
    manifest["source_files"] = {
        p.relative_to(stage).as_posix(): v1.sha(p.read_bytes())
        for p in sorted(stage.rglob("*"))
        if p.is_file() and p.relative_to(stage).parts[0] not in ("inputs", "bootwrap")
    }
    (stage / "firmware-source.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    if args.stage_only:
        print(
            json.dumps(
                {
                    "staged": True,
                    "fixed_kernel_sha256": fixed_kernel_sha,
                    "cave_used_bytes": patch_info["cave_used_bytes"],
                    "patch_count": len(patch_info["patches"]),
                },
                indent=2,
            )
        )
        return

    v1.remote_build(stage, args.ssh_host, args.ssh_user)

    built = stage / "built"
    fd = (built / "SILICIUM_UEFI.fd").read_bytes()
    fv, modules, sec = extract_fd(fd)
    if fv != (built / "FVMAIN.Fv").read_bytes():
        raise RuntimeError("Compressed FV differs from FVMAIN.Fv")

    map_users = [sec] + [
        v1.one_section(modules[g], 0x10)
        for g in (
            "d6a2cb7f-6a18-4e2f-b43b-9920a733700a",
            "6c2004ef-4e0e-4be4-b14c-340eb4aa5891",
            "b8d9777e-d72a-451f-9bdb-bafb52a68415",
            "40d49ba8-448a-40e5-bf92-75fdc4a760d4",
        )
    ]
    for binary in map_users:
        for pair in ((0xB0000000, 0x100000), (0xD0000000, 0x18000000), (0xCE000000, 0x700000)):
            if binary.count(struct.pack("<QQ", *pair)) != 1:
                raise RuntimeError(f"Missing linked memory reservation {pair}")

    fat = v1.one_section(modules[v1.FAT_GUID], 0x19)
    content = read_fat32(fat)
    if v1.sha(content["/Payloads/kernel.macho"]) != fixed_kernel_sha:
        raise RuntimeError("Embedded kernel.macho SHA256 mismatch!")
    if v1.sha(content["/Payloads/kernel.macho.adt"]) != v1.EXPECTED_ADT_SHA:
        raise RuntimeError("Embedded ADT SHA256 mismatch!")
    native_candidate.check_kernel(content["/Payloads/kernel.macho"])

    shim = (stage / "bootwrap/BootShim.bin").read_bytes()
    dtb = (stage / "bootwrap/dtb.bin").read_bytes()
    out_dir.mkdir(parents=True, exist_ok=False)
    raw = shim + fd
    packed = gzip.compress(raw, mtime=0) + dtb
    (out_dir / "Image.gz-dtb").write_bytes(packed)
    (out_dir / "empty-ramdisk").write_bytes(b"")
    (out_dir / "dtb.bin").write_bytes(dtb)

    target = out_dir / "j716f-native-log-40458123294-fast-v5-boot.img"
    subprocess.run(
        [
            sys.executable,
            str(v1.MKBOOTIMG),
            "--kernel",
            str(out_dir / "Image.gz-dtb"),
            "--ramdisk",
            str(out_dir / "empty-ramdisk"),
            "--dtb",
            str(out_dir / "dtb.bin"),
            "--output",
            str(target),
            "--header_version",
            "2",
            "--base",
            "0",
            "--kernel_offset",
            "32768",
            "--tags_offset",
            "256",
            "--pagesize",
            "4096",
            "--dtb_offset",
            "32505856",
        ],
        check=True,
    )

    final = target.read_bytes()
    ks, ka, rs, _ra, ss, _sa, tags, page, version, _ = struct.unpack_from("<10I", final, 8)
    if final[:8] != b"ANDROID!" or (ka, rs, ss, tags, page, version) != (0x8000, 0, 0, 0x100, 4096, 2):
        raise RuntimeError("Invalid Android boot image v2 header")
    dec = zlib.decompressobj(31)
    actual = dec.decompress(final[page : page + ks])
    if not dec.eof or dec.unused_data != dtb or actual != raw:
        raise RuntimeError("Android boot image gzip roundtrip failed")
    _, again, _ = extract_fd(actual[len(shim) :])
    if read_fat32(v1.one_section(again[v1.FAT_GUID], 0x19)) != content:
        raise RuntimeError("Android boot image FAT32 roundtrip failed")

    audit = {
        "schema": 1,
        "stage": "native-fast-v5-host-audited",
        "image": target.name,
        "image_bytes": len(final),
        "image_sha256": v1.sha(final),
        "fd_sha256": v1.sha(fd),
        "fv_sha256": v1.sha(fv),
        "payload_sha256": v1.sha(fat),
        "baseline_kernel_sha256": v1.EXPECTED_KERNEL_SHA,
        "kernel_sha256": fixed_kernel_sha,
        "adt_sha256": v1.EXPECTED_ADT_SHA,
        "logger_candidate_sha256": v1.EXPECTED_CANDIDATE_SHA,
        "bootshim_sha256": v1.EXPECTED_SHIM_SHA,
        "firmware_dtb_sha256": v1.EXPECTED_DTB_SHA,
        "cave_base": patch_info["cave_base"],
        "cave_used_bytes": patch_info["cave_used_bytes"],
        "panic_log_paddr_redirect": patch_info["panic_log_paddr_redirect"],
        "v5_patches": patch_info["patches"],
        "reservation_descriptors_linked": True,
        "automatic_start": False,
    }
    (out_dir / "host-audit.json").write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(audit, indent=2))


if __name__ == "__main__":
    main()
