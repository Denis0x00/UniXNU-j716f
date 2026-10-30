#!/usr/bin/env python3
"""100% git-free builder & roundtrip auditor for J716F NATIVE FAST-V12.

FAST-V12: Phase 1 RAMDisk Rootfs Bringup & PID 1 Launchd Transition
  - Overcomes Line 984 root mount panic:
      Incorporates official Apple 10MB APFS ramdisk (044-75188-038.dmg) with NXSB superblock.
      Supplies updated ADT (kernel.macho.adt.v12) containing /chosen/memory-map/RAMDisk (16 bytes).
      DarwinBoot reads ramdisk, copies into window, and registers physical address & size in ADT.
      Passes boot-arg "rd=md0", directing XNU _IOFindBSDRoot to bind md0 as root device.
  - Expands Flash Volume (FD) to 11MB (0xb00000):
      Allows kernel.macho + 10MB ramdisk to compress into FVMAIN_COMPACT without overflow.
      Uses 11MB BootShim.bin (FD_BASE=0xce000000, FD_SIZE=0xb00000).
      Strictly remains within Qualcomm ABL's 13.97MB DTB collision ceiling, eliminating boot hang.
      Updates UEFI memory descriptors (MemoryMapLib) for 11MB FD.
  - Adds milestone hooks for root mount and launchd transition:
      Slot 17: mountroot_ok (post _vfs_mountroot at 0xfffffff0076c1b18)
      Slot 16: pid1_create  (post _bsd_utaskbootstrap at 0xfffffff0076c1d88)
      Slot 15: load_init    (post load_init_program at 0xfffffff0076c1f2c)
      Slot 14: exec_launchd (post load_init_program_at_path /sbin/launchd at 0xfffffff0076ec480)
  - Preserves all Patches 1-42 + safe_cckprng_generate from V11.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import hmac
import json
import os
os.environ.setdefault("J716F_SSH_PASSWORD", "test@123")
import posixpath
import shlex
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

EXPECTED_ADT_V12_SHA = "5a0c7fe4efd5d6210bff5373e160edc86faa780996858601042400f70174558e"
EXPECTED_RAMDISK_SHA = "52e568e1eaaede5fde4684ad10afc60f8fb236bfb5a23e0869bb0ed8a1d275d7"
EXPECTED_SHIM_11MB_SHA = "ec1cef37c683f80b931eda772287bcd3589bc74a00e8157a9b3c36f2f40d7d6c"


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


def apply_v12_kernel_fixes(kernel_bytes: bytes) -> tuple[bytes, dict]:
    if v1.sha(kernel_bytes) != v1.EXPECTED_KERNEL_SHA:
        raise RuntimeError("Input kernel.macho does not match expected baseline SHA256")
    img = native_candidate.MachO(kernel_bytes)
    buf = bytearray(kernel_bytes)

    cave_off = img.offset(CAVE_BASE, CAVE_LIMIT - CAVE_BASE)
    if any(buf[cave_off : cave_off + (CAVE_LIMIT - CAVE_BASE)]):
        raise RuntimeError("Cave region 0xfffffff0073fd9c0..0xfffffff0073fe000 is not empty!")

    cave_words: list[int] = []

    def cur_va() -> int:
        return CAVE_BASE + len(cave_words) * 4

    def emit(w: int) -> None:
        cave_words.append(w & 0xFFFFFFFF)

    # 1. Emit common_post_logger helper
    common_logger_va = cur_va()
    emit(0xF9400BE1)  # ldr x1, [sp, #0x10] (arg1 = return value)
    emit(0xF94007E2)  # ldr x2, [sp, #0x08] (arg2 = caller LR)
    emit(0xD2800003)  # mov x3, #0          (arg3 = 0)
    emit(asm_bl(cur_va(), EVENT_VA))        # bl EVENT_VA
    emit(0xF9400BE0)  # ldr x0, [sp, #0x10] (restore original return value!)
    emit(0xA8C27BFD)  # ldp x29, x30, [sp], #0x20
    emit(0xD65F03C0)  # ret

    def emit_post_stub(orig_callee: int, slot: int) -> int:
        entry = cur_va()
        emit(0xA9BE7BFD)  # stp x29, x30, [sp, #-0x20]!
        emit(0x910003FD)  # mov x29, sp
        emit(asm_bl(cur_va(), orig_callee))
        emit(0xF9000BE0)  # str x0, [sp, #0x10] (CRITICAL: preserve callee return value!)
        emit(movz_w0(slot))  # w0 = slot
        emit(asm_b(cur_va(), common_logger_va))
        return entry

    # 2. Build post-call milestone stubs
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
        # FAST-V11 milestone hooks in _kernel_bootstrap_thread:
        (0xFFFFFFF0079C247C, 0xFFFFFFF0079C1FC0, 22, "_kernel_bootstrap_thread post STARTUP_SUB_KEXT (22:kext_sub_ok)"),
        (0xFFFFFFF0079C2490, 0xFFFFFFF0073D0BE8, 23, "_kernel_bootstrap_thread post _initialize_screen (23:screen_ok)"),
        (0xFFFFFFF0079C2494, 0xFFFFFFF0073B134C, 24, "_kernel_bootstrap_thread post _vm_commpage_init (24:comm_init_ok)"),
        (0xFFFFFFF0079C2498, 0xFFFFFFF0073B12F4, 25, "_kernel_bootstrap_thread post _vm_commpage_text_init (25:comm_txt_ok)"),
        (0xFFFFFFF0079C24C4, 0xFFFFFFF00795BB24, 26, "_kernel_bootstrap_thread post _mac_policy_initmach (26:mac_ok)"),
        (0xFFFFFFF0079C254C, 0xFFFFFFF00772C858, 27, "_kernel_bootstrap_thread post _trust_cache_runtime_init (27:trust_ok)"),
        (0xFFFFFFF0079C2558, 0xFFFFFFF0079C1FC0, 28, "_kernel_bootstrap_thread post STARTUP_SUB_ALL (28:all_sub_ok)"),
        (0xFFFFFFF0079C2614, 0xFFFFFFF007959D70, 20, "_kernel_bootstrap_thread post _PE_lockdown_iokit (20:iokit_lck)"),
        (0xFFFFFFF0076C14C4, 0xFFFFFFF007701C14, 18, "_bsd_init post _proc_ro_alloc (18:proc_ro_ok)"),
        (0xFFFFFFF0076C1854, 0xFFFFFFF00747D148, 19, "_bsd_init post _vfsinit (19:vfs_ok)"),
        # FAST-V12 milestone hooks for RAMDisk rootfs & PID 1 launchd transition:
        (0xFFFFFFF0076C1B18, 0xFFFFFFF00748112C, 17, "_bsd_init post _vfs_mountroot (17:mountroot_ok)"),
        (0xFFFFFFF0076C1D88, 0xFFFFFFF007708488, 16, "_bsd_init post _bsd_utaskbootstrap (16:pid1_create)"),
        (0xFFFFFFF0076C1F2C, 0xFFFFFFF0076EC2A4, 15, "bsd_init_task post load_init_program (15:load_init)"),
        (0xFFFFFFF0076EC480, 0xFFFFFFF0076EC4D8, 14, "load_init_program post load_init_program_at_path /sbin/launchd (14:exec_launchd)"),
    ]

    stub_map: dict[int, tuple[int, int, str]] = {}
    for callsite, orig_callee, slot, desc in post_hooks:
        stub_va = emit_post_stub(orig_callee, slot)
        stub_map[callsite] = (asm_bl(callsite, orig_callee), asm_bl(callsite, stub_va), desc)

    # 3. Pre+Post stub for _PE_init_iokit (slot 55 pre, slot 56 post) at 0xfffffff0079c246c
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

    # 4. Pre-call stub for _bsd_init (slot 21) at 0xfffffff0079c262c
    bsd_stub_va = cur_va()
    emit(0xA9BE7BFD)  # stp x29, x30, [sp, #-0x20]!
    emit(0x910003FD)  # mov x29, sp
    emit(movz_w0(21))  # slot 21: bsd_init
    emit(0xD2800001)  # mov x1, #0
    emit(0xD2800002)  # mov x2, #0
    emit(0xD2800003)  # mov x3, #0
    emit(asm_bl(cur_va(), EVENT_VA))
    emit(0xA8C27BFD)  # ldp x29, x30, [sp], #0x20
    emit(asm_b(cur_va(), 0xFFFFFFF0076C13F8))  # b _bsd_init
    stub_map[0xFFFFFFF0079C262C] = (
        asm_bl(0xFFFFFFF0079C262C, 0xFFFFFFF0076C13F8),
        asm_bl(0xFFFFFFF0079C262C, bsd_stub_va),
        "_kernel_bootstrap_thread pre _bsd_init (21:bsd_init)",
    )

    # 5. Pre-call stub for _load_context (slot 49) at 0xfffffff0072c8870
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

    # 6. _kernel_debug_string_early hook (slot 39 first + slot 47 rolling latest) at 0xfffffff0076c4248
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

    # 7. _ml_panic_trap_to_debugger hook (slot 45 PANIC_CALL + slot 46 PANIC_STR) at 0xfffffff0073ffa84
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

    # 8. FIQ & SError hooks (slot 48 SERR_FIQ)
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

    # 9. safe_cckprng_generate (FAST-V11 PRNG fix)
    safe_prng_va = cur_va()
    emit(0xB40001E3)  # cbz x3, +60
    emit(0xB40001C2)  # cbz x2, +56
    emit(0xD53BE048)  # mrs x8, cntpct_el0
    emit(0xB5000048)  # cbnz x8, +8
    emit(0x528266E8)  # movz x8, #0x1337
    emit(0xD28FE5A9)  # movz x9, #0x7f2d
    emit(0xF2A992A9)  # movk x9, #0x4c95, lsl #16
    emit(0xF2CE85A9)  # movk x9, #0xf42d, lsl #32
    emit(0xF2EB0A29)  # movk x9, #0x5851, lsl #48
    # Loop start (pc = safe_prng_va + 0x24):
    emit(0x9B097D08)  # mul x8, x8, x9
    emit(0x91000508)  # add x8, x8, #1
    emit(0xD360FC0A)  # lsr x10, x8, #32
    emit(0x3800146A)  # strb w10, [x3], #1
    emit(0xF1000442)  # subs x2, x2, #1
    emit(0x54FFFF61)  # b.ne loop_start
    # exit_0:
    emit(0x52800000)  # mov w0, #0
    emit(0xD65F03C0)  # ret

    stub_map[0xFFFFFFF007992A1C] = (
        0xA9BF7BFD,  # stp x29, x30, [sp, #-16]!
        asm_b(0xFFFFFFF007992A1C, safe_prng_va),
        "_cckprng_generate -> safe_cckprng_generate (FAST-V11 Root Cause Fix)",
    )

    if cur_va() > CAVE_LIMIT:
        raise RuntimeError(f"Cave overflow: {cur_va():#x} > {CAVE_LIMIT:#x}")

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
            0x35000D6A,
            0x7100BC3F,
        ),
        (
            0xFFFFFFF0073F9AC8,
            "jl_write allow slot 47 overwrite (csel w10, wzr, w10, eq)",
            0xB940750A,
            0x1A8A03EA,
        ),
        # FAST-V11 41: AMFI waitForMatchingService(ApplePMGRNub) bypass missing PMGR panic
        (
            0xFFFFFFF004AAEB38,
            "AMFI waitForMatchingService(ApplePMGRNub) bypass missing PMGR panic",
            0xB4000E20,
            0xB40001E0,
        ),
        # FAST-V11 42: Disable _pmap_phys_write_disable to preserve Physical Aperture RW
        (
            0xFFFFFFF0073E5170,
            "disable _pmap_phys_write_disable (ret)",
            0xA9BE4FF4,
            0xD65F03C0,
        ),
        # FAST-V5 8-9: Redirect _PE_create_console PLATFORM_PANIC_LOG_PADDR to 0xb00b8000
        (
            0xFFFFFFF00795A158,
            "_PE_create_console panic_log_paddr low16 (0xc000 -> 0x8000)",
            0x52980000,
            0x52900000,
        ),
        (
            0xFFFFFFF00795A15C,
            "_PE_create_console panic_log_paddr high16 (0x3c0f -> 0xb00b => PA 0xb00b8000)",
            0x72A781E0,
            0x72B60160,
        ),
        # FAST-V6 10: IODT AAPL,phandle -> phandle
        (
            0xFFFFFFF0078A0F6C,
            "IODeviceTreeAlloc gIODTPHandleKey ('AAPL,phandle' -> 'phandle')",
            0x9122E000,
            0x9122F400,
        ),
        # FAST-V7 11: register_and_init_prng double-registration bypass
        (
            0xFFFFFFF0073D8D94,
            "register_and_init_prng double-registration bypass (cbnz w8, ret)",
            0x350005A8,
            0x350004C8,
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
                f"Byte mismatch at {va:#x} ({sym}): expected {old_w:#010x}, found {actual_w:#010x}"
            )
        struct.pack_into("<I", buf, foff, new_w)
        applied.append(
            {
                "va": f"{va:#x}",
                "description": sym,
                "orig": f"{old_w:#010x}",
                "patched": f"{new_w:#010x}",
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

    info = {
        "cave_base": f"{CAVE_BASE:#x}",
        "cave_used_bytes": len(cave_words) * 4,
        "cave_limit_bytes": CAVE_LIMIT - CAVE_BASE,
        "safe_prng_va": f"{safe_prng_va:#x}",
        "patches": applied,
        "fips_post_hmac_sha256": checks["source_crypto_hmac"]["code_sha256"],
    }
    return out, info


def remote_build_v12(stage: Path, host: str, user: str) -> None:
    client = v1.connect_ssh(host, user)

    def run_ssh(cmd: str) -> tuple[int, str, str]:
        _, out, err = client.exec_command(cmd)
        stdout = out.read().decode("utf-8", errors="replace")
        stderr = err.read().decode("utf-8", errors="replace")
        return out.channel.recv_exit_status(), stdout, stderr

    try:
        code, remote_raw, err = run_ssh("mktemp -d /home/test/tb-j716f-unixnu/native-fast-v12-XXXXXXXX")
        remote = remote_raw.strip()
        if code != 0 or not remote.startswith("/home/test/tb-j716f-unixnu/native-fast-v12-"):
            raise RuntimeError(f"Cannot create remote workspace: {err}")
        print(f"Remote build workspace: {remote}", flush=True)

        with client.open_sftp() as sftp:
            created_dirs = set()
            for path in stage.rglob("*"):
                if not path.is_file() or "__pycache__" in path.parts or path.relative_to(stage).parts[0] == "bootwrap":
                    continue
                rel = path.relative_to(stage).as_posix()
                rpath = f"{remote}/{rel}"
                cur = remote
                for part in posixpath.dirname(rel).split("/"):
                    if not part or part == ".":
                        continue
                    cur = f"{cur}/{part}"
                    if cur not in created_dirs:
                        sftp.mkdir(cur)
                        created_dirs.add(cur)
                sftp.put(str(path), rpath)

        build_sh = f"""set -eo pipefail
source /home/test/tb-j716f-unixnu/.venv/bin/activate
cd {shlex.quote(remote)}
gcc -O2 -fno-strict-aliasing -Wall -Wextra -Werror -IJ716fNativeLog J716fNativeLog/FastTests.c -o fast-tests
./fast-tests > fast-tests.log 2>&1
export WORKSPACE="$PWD" CONF_PATH="$PWD/Conf"
export PACKAGES_PATH="$PWD:$PWD/Platforms/Lenovo:$PWD/package:$PWD/DarwinBoot:{v1.REMOTE_UEFI}:{v1.REMOTE_UEFI}/Common/Mu:{v1.REMOTE_UEFI}/Common/Mu_OEM_Sample:{v1.REMOTE_UEFI}/Mu_Basecore:{v1.REMOTE_UEFI}/Silicon/Qualcomm:{v1.REMOTE_UEFI}/Silicon/Silicium:/home/test/tb-j716f-unixnu/ports/j716f/edk2-overlay"
mkdir -p "$CONF_PATH"
cp {v1.REMOTE_UEFI}/Conf/target.txt {v1.REMOTE_UEFI}/Conf/tools_def.txt {v1.REMOTE_UEFI}/Conf/build_rule.txt "$CONF_PATH/"
source {v1.REMOTE_UEFI}/Mu_Basecore/BaseTools/BuildEnv
build -a AARCH64 -t CLANGPDB -b RELEASE -p NativeLogApps.dsc > reader-build.log 2>&1
python3 build-native-payload.py "$PWD" > payload-build.log 2>&1
build -a AARCH64 -t CLANGPDB -b RELEASE -p Platforms/Lenovo/j716fPkg/j716f-native.dsc \\
  -D FD_BASE=0xce000000 -D FD_SIZE=0xb00000 -D FD_BLOCKS=0xb00 -D ENABLE_SECUREBOOT=0 \\
  -y firmware-build-report.txt -Y LIBRARY -Y DEPEX > firmware-build.log 2>&1
"""
        print("Running FastTests.c + EDK II 11MB build on 10.1.2.110...", flush=True)
        code, out, err = run_ssh("bash -c " + shlex.quote(build_sh))

        built = stage / "built"
        built.mkdir(exist_ok=True)
        wanted = {
            "fast-tests.log": "fast-tests.log",
            "reader-build.log": "reader-build.log",
            "payload-build.log": "payload-build.log",
            "firmware-build.log": "firmware-build.log",
            "firmware-build-report.txt": "firmware-build-report.txt",
            "payload/payload.json": "payload.json",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd": "SILICIUM_UEFI.fd",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/FV/FVMAIN.Fv": "FVMAIN.Fv",
            "Platforms/Lenovo/j716fPkg/Resources/NativePayload.fat": "NativePayload.fat",
            "Build/NativeLogApps/RELEASE_CLANGPDB/AARCH64/J716fNativeRead.efi": "J716fNativeRead.efi",
            "Build/NativeLogApps/RELEASE_CLANGPDB/AARCH64/J716fNativeArm.efi": "J716fNativeArm.efi",
            "Build/NativeLogApps/RELEASE_CLANGPDB/AARCH64/J716fPowerAction.efi": "J716fPowerAction.efi",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/AARCH64/UniXNU.efi": "UniXNU.efi",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/AARCH64/DarwinBoot.efi": "DarwinBoot.efi",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/AARCH64/J716fNativeFatDxe.efi": "J716fNativeFatDxe.efi",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/AARCH64/J716fKeysDxe.efi": "J716fKeysDxe.efi",
        }
        with client.open_sftp() as sftp:
            for rrel, lname in wanted.items():
                try:
                    sftp.stat(f"{remote}/{rrel}")
                except FileNotFoundError:
                    continue
                sftp.get(f"{remote}/{rrel}", str(built / lname))

        if code != 0:
            fw_log = built / "firmware-build.log"
            tail = fw_log.read_text(encoding="utf-8", errors="replace")[-2000:] if fw_log.exists() else err
            raise RuntimeError(f"Remote build failed (exit code {code}):\n{tail}")
    finally:
        client.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stage",
        type=Path,
        default=ROOT / "debug/native-fast-stage-v12",
        help="Local staging directory for FAST-V12",
    )
    parser.add_argument(
        "--out-dir",
        type=Path,
        default=ROOT / "Firmware/NATIVE-40458123294-FAST-V12",
        help="Firmware output directory",
    )
    parser.add_argument("--ssh-host", default="10.1.2.110")
    parser.add_argument("--ssh-user", default="test")
    parser.add_argument("--stage-only", action="store_true")
    parser.add_argument("--skip-remote-build", action="store_true", help="Reuse built/ artifacts")
    args = parser.parse_args()

    stage: Path = args.stage
    out_dir: Path = args.out_dir
    base_stage = ROOT / "debug/native-fast-stage-v7"

    if stage.exists() and not args.skip_remote_build:
        shutil.rmtree(stage)
    if not stage.exists():
        shutil.copytree(base_stage, stage)

    manifest_path = stage / "firmware-source.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    # Update DSC title
    dsc = (stage / "Platforms/Lenovo/j716fPkg/j716f-native.dsc").read_text(encoding="utf-8")
    dsc = dsc.replace("J716F NATIVE FAST-V7 40458123294", "J716F NATIVE FAST-V12 40458123294")
    (stage / "Platforms/Lenovo/j716fPkg/j716f-native.dsc").write_text(dsc, encoding="utf-8", newline="\n")

    # Update MemoryMapLib for 11MB FD (0xb00000)
    for mmap_rel in [
        "Platforms/Lenovo/j716fPkg/Library/J716fRamLogMemoryMapLib/MemoryMapLib.c",
        "Platforms/Lenovo/j716fPkg/Library/J716fWindowMemoryMapLib/MemoryMapLib.c",
        "Platforms/Lenovo/j716fPkg/Library/MemoryMapLib/MemoryMapLib.c",
    ]:
        mmap_path = stage / mmap_rel
        if mmap_path.exists():
            txt = mmap_path.read_text(encoding="utf-8")
            txt = txt.replace(
                '{"UEFI FD", 0xCE000000, 0x00700000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},',
                '{"UEFI FD", 0xCE000000, 0x00B00000, AddMem, SYS_MEM, SYS_MEM_CAP, BsData, WRITE_BACK},',
            )
            txt = txt.replace(
                '{"FD Guard", 0xCE700000, 0x01900000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},',
                '{"FD Guard", 0xCEB00000, 0x01500000, AddMem, MEM_RES, UNCACHEABLE, Reserv, UNCACHED_UNBUFFERED_XN},',
            )
            mmap_path.write_text(txt, encoding="utf-8", newline="\n")

    # Update build-native-payload.py to include ramdisk.dmg
    bnp = (stage / "build-native-payload.py").read_text(encoding="utf-8")
    bnp = bnp.replace(
        'for name in ("kernel.macho", "kernel.macho.adt"):',
        'for name in ("kernel.macho", "kernel.macho.adt", "ramdisk.dmg"):',
    )
    (stage / "build-native-payload.py").write_text(bnp, encoding="utf-8", newline="\n")

    # Copy updated ADT v12 and APFS ramdisk into inputs/
    adt_v12_path = ROOT / "Firmware/kernel.macho.adt.v12"
    ramdisk_path = ROOT / "Firmware/ramdisk_appos_10mb.dmg"
    shim_11mb_path = ROOT / "Firmware/BootShim_11mb.bin"

    if v1.sha(adt_v12_path.read_bytes()) != EXPECTED_ADT_V12_SHA:
        raise RuntimeError("kernel.macho.adt.v12 SHA256 mismatch")
    if v1.sha(ramdisk_path.read_bytes()) != EXPECTED_RAMDISK_SHA:
        raise RuntimeError("ramdisk_appos_10mb.dmg SHA256 mismatch")
    if v1.sha(shim_11mb_path.read_bytes()) != EXPECTED_SHIM_11MB_SHA:
        raise RuntimeError("BootShim_11mb.bin SHA256 mismatch")

    shutil.copyfile(adt_v12_path, stage / "inputs/kernel.macho.adt")
    shutil.copyfile(ramdisk_path, stage / "inputs/ramdisk.dmg")
    shutil.copyfile(shim_11mb_path, stage / "bootwrap/BootShim.bin")

    # Update unixnu.toml
    toml_txt = (stage / "unixnu.toml").read_text(encoding="utf-8")
    toml_txt = toml_txt.replace("NATIVE FAST-V7", "NATIVE FAST-V12")
    for num in range(1, 10):
        toml_txt = toml_txt.replace(f'title = "{num}. ', 'title = "')
    # Update native entry with rd=md0 and initrd
    toml_txt = toml_txt.replace(
        'args = "-v serial=0 debug=0x14e cpus=1 -no_slto_panic"',
        'args = "-v serial=0 debug=0x14e cpus=1 -no_slto_panic rd=md0"\ninitrd = "\\\\Payloads\\\\ramdisk.dmg"',
    )
    (stage / "unixnu.toml").write_text(toml_txt, encoding="utf-8", newline="\n")

    # Apply kernel fixes
    orig_kernel = (v1.BASE_STAGE / "inputs/kernel.macho").read_bytes()
    fixed_kernel, patch_info = apply_v12_kernel_fixes(orig_kernel)
    fixed_kernel_sha = v1.sha(fixed_kernel)
    (stage / "inputs/kernel.macho").write_bytes(fixed_kernel)

    manifest["profile"] = "FAST-V12"
    manifest["baseline_kernel_sha256"] = v1.EXPECTED_KERNEL_SHA
    manifest["prelinked_kernel_sha256"] = fixed_kernel_sha
    manifest["v12_fixes"] = patch_info
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

    if not args.skip_remote_build:
        remote_build_v12(stage, args.ssh_host, args.ssh_user)

    built = stage / "built"
    fd = (built / "SILICIUM_UEFI.fd").read_bytes()
    if len(fd) != 0xb00000:
        raise RuntimeError(f"Expected 11MB SILICIUM_UEFI.fd, got {len(fd):#x} bytes")
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
        for pair in ((0xB0000000, 0x100000), (0xD0000000, 0x18000000), (0xCE000000, 0xb00000)):
            if binary.count(struct.pack("<QQ", *pair)) != 1:
                raise RuntimeError(f"Missing linked memory reservation {pair}")

    fat = v1.one_section(modules[v1.FAT_GUID], 0x19)
    content = read_fat32(fat)
    if v1.sha(content["/Payloads/kernel.macho"]) != fixed_kernel_sha:
        raise RuntimeError("Embedded kernel.macho SHA256 mismatch!")
    if v1.sha(content["/Payloads/kernel.macho.adt"]) != EXPECTED_ADT_V12_SHA:
        raise RuntimeError("Embedded ADT SHA256 mismatch!")
    ramdisk_entry = content.get("/Payloads/ramdisk.dmg") or content.get("/Payloads/RAMDISK.DMG")
    if not ramdisk_entry or v1.sha(ramdisk_entry) != EXPECTED_RAMDISK_SHA:
        raise RuntimeError("Embedded ramdisk.dmg SHA256 mismatch!")
    native_candidate.check_kernel(content["/Payloads/kernel.macho"])

    shim = (stage / "bootwrap/BootShim.bin").read_bytes()
    dtb = (stage / "bootwrap/dtb.bin").read_bytes()
    out_dir.mkdir(parents=True, exist_ok=False)
    raw = shim + fd
    packed = gzip.compress(raw, mtime=0) + dtb
    (out_dir / "Image.gz-dtb").write_bytes(packed)
    (out_dir / "empty-ramdisk").write_bytes(b"")
    (out_dir / "dtb.bin").write_bytes(dtb)

    target = out_dir / "j716f-native-log-40458123294-fast-v12-boot.img"
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
        "stage": "native-fast-v12-host-audited",
        "image": target.name,
        "image_bytes": len(final),
        "image_sha256": v1.sha(final),
        "fd_sha256": v1.sha(fd),
        "fv_sha256": v1.sha(fv),
        "payload_sha256": v1.sha(fat),
        "baseline_kernel_sha256": v1.EXPECTED_KERNEL_SHA,
        "kernel_sha256": fixed_kernel_sha,
        "adt_sha256": EXPECTED_ADT_V12_SHA,
        "ramdisk_sha256": EXPECTED_RAMDISK_SHA,
        "logger_candidate_sha256": v1.EXPECTED_CANDIDATE_SHA,
        "bootshim_sha256": EXPECTED_SHIM_11MB_SHA,
        "firmware_dtb_sha256": v1.EXPECTED_DTB_SHA,
        "cave_base": patch_info["cave_base"],
        "cave_used_bytes": patch_info["cave_used_bytes"],
        "safe_prng_va": patch_info["safe_prng_va"],
        "patch_count": len(patch_info["patches"]),
        "reservation_descriptors_linked": True,
        "automatic_start": False,
    }
    (out_dir / "host-audit.json").write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(audit, indent=2))


if __name__ == "__main__":
    main()
