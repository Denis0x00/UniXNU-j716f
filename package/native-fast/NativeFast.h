// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fast bring-up session scanner, 3-second cold/dirty initializer+ARM, and
// 8-frame (8 KiB) session QR exporter. Preserves NativeLog.h kernel ABI 100%.
#ifndef J716F_NATIVE_FAST_H
#define J716F_NATIVE_FAST_H

#include "NativeFirmware.h"
#include "NativeSha256.h"

#define JLF_SESSION_CHUNK   1024U
#define JLF_SESSION_FRAMES  (JL_SESSION_BYTES / JLF_SESSION_CHUNK)
#define JLF_FRAME_CHARS     2227U
#define JLF_FRAME_BUF       2228U

enum {
  JLF_MODE_NONE = 0,
  JLF_MODE_APPEND = 1,
  JLF_MODE_CLEAN_INIT = 2,
  JLF_MODE_ROLLOVER = 3
};

enum {
  JLF_ERR_NONE = 0,
  JLF_ERR_USED = 1,
  JLF_ERR_EL = 2,
  JLF_ERR_PSCI = 3,
  JLF_ERR_UNSTABLE = 4,
  JLF_ERR_PENDING_ARM = 5,
  JLF_ERR_NEED_ROLLOVER = 6
};

typedef struct {
  JL32 total;
  JL32 armed_count;
  JL32 used_count;
  JL32 offsets[JL_MAX_SESSIONS];
} JLFastScan;

typedef struct {
  JL32 ready;
  JL32 used;
  JL32 mode;
  JL32 error;
  JL32 console_crc;
  JL64 session;
} JLFastArmGate;

static inline void jlf_hex(char *out, JL64 value, JL32 digits)
{
  static const char hex[] = "0123456789ABCDEF";
  JL32 i;
  for (i = 0; i < digits; ++i) {
    out[i] = hex[(value >> ((digits - 1U - i) * 4U)) & 15U];
  }
}

static inline void jlf_hex_bytes(char *out, const JL8 *p, JL32 n)
{
  JL32 i;
  for (i = 0; i < n; ++i) {
    jlf_hex(out + 2U * i, p[i], 2U);
  }
}

// Raw physical scan across the entire 256 KiB console window.
// Recovers valid candidate sessions even if the outer 12-byte DBGC ring header
// was wrapped, overwritten by Linux ramoops, or perturbed across reset.
static inline void jlf_scan_raw(const volatile void *console,
    const JL64 candidate[4], JLFastScan *scan)
{
  const volatile JL8 *base = (const volatile JL8 *)console;
  JL32 off, i;
  scan->total = 0;
  scan->armed_count = 0;
  scan->used_count = 0;
  for (off = 16U; off <= JL_CONSOLE_BYTES - JL_SESSION_BYTES; off += 8U) {
    const volatile JLHeader *h = (const volatile JLHeader *)(base + off);
    int match = 1;
    if (h->magic != JL_MAGIC || h->version != 1U || h->bytes != JL_SESSION_BYTES ||
        h->offset != off || !h->session ||
        (h->state != JL_ARMED && h->state != JL_USED) ||
        h->crc != jl_crc(h, 60U)) {
      continue;
    }
    for (i = 0; i < 4U; ++i) {
      if (h->candidate[i] != candidate[i]) { match = 0; break; }
    }
    if (!match) { continue; }
    for (i = 0; i < sizeof(h->reserved); ++i) {
      if (h->reserved[i]) { match = 0; break; }
    }
    if (!match) { continue; }
    if (scan->total < JL_MAX_SESSIONS) {
      scan->offsets[scan->total++] = off;
    }
    if (h->state == JL_ARMED) {
      ++scan->armed_count;
    } else {
      ++scan->used_count;
    }
    off += JL_SESSION_BYTES - 8U;
  }
}

static inline int jlf_linear_space_ok(const volatile void *console)
{
  const volatile JL32 *ring = (const volatile JL32 *)console;
  JL32 offset;
  if (ring[0] != 0x43474244U || ring[1] != ring[2] || ring[1] > JL_CAPACITY) {
    return 0;
  }
  offset = (12U + ring[1] + 7U) & ~7U;
  return offset <= JL_CONSOLE_BYTES - JL_SESSION_BYTES;
}

// Prepare fast ARM:
// - APPEND if valid linear DBGC header already has >= 8 KiB unused tail.
// - CLEAN_INIT if zero JL_MAGIC sessions exist (cold boot RAM or Android logs).
// - ROLLOVER only if allow_rollover != 0 when prior USED sessions exist and
//   either the header is non-linear or < 8 KiB tail remains.
static inline int jlf_arm_query(JLFastArmGate *gate, const volatile void *live,
    const JL64 candidate[4], JL64 el, JL64 psci_ver, JL64 psci_feat,
    JL64 session, int allow_rollover, JLBarrier barrier)
{
  static JL64 scratch[JL_CONSOLE_BYTES / 8U];
  JLFastScan scan;
  JL32 i;
  if (gate->used) { gate->error = JLF_ERR_USED; return 0; }
  gate->ready = 0;
  gate->mode = JLF_MODE_NONE;
  if (el != 4ULL || !session) { gate->error = JLF_ERR_EL; return 0; }
  if ((psci_ver >> 32) || (psci_ver & 0x80000000ULL) || psci_ver < 0x10000ULL || psci_feat) {
    gate->error = JLF_ERR_PSCI;
    return 0;
  }
  if (!jl_freeze(live, scratch, barrier)) {
    gate->error = JLF_ERR_UNSTABLE;
    return 0;
  }
  jlf_scan_raw(scratch, candidate, &scan);
  for (i = 0; i < scan.total; ++i) {
    const JLHeader *old = (const JLHeader *)((const JL8 *)scratch + scan.offsets[i]);
    if (old->session == session) {
      gate->error = JLF_ERR_PENDING_ARM;
      return 0;
    }
  }
  if (scan.armed_count > 0 && !allow_rollover) {
    gate->error = JLF_ERR_PENDING_ARM;
    return 0;
  }
  if (jlf_linear_space_ok(scratch) && scan.armed_count == 0) {
    gate->mode = JLF_MODE_APPEND;
  } else if (scan.total == 0) {
    gate->mode = JLF_MODE_CLEAN_INIT;
  } else if (allow_rollover) {
    gate->mode = JLF_MODE_ROLLOVER;
  } else {
    gate->error = JLF_ERR_NEED_ROLLOVER;
    return 0;
  }
  gate->console_crc = jl_crc(scratch, JL_CONSOLE_BYTES);
  gate->session = session;
  gate->error = JLF_ERR_NONE;
  gate->ready = 1U;
  return 1;
}

static inline volatile JLHeader *jlf_arm_commit(JLFastArmGate *gate,
    volatile void *live, const JL64 candidate[4], JLBarrier barrier)
{
  volatile JL32 *ring = (volatile JL32 *)live;
  volatile JL8 *base = (volatile JL8 *)live;
  JL32 i;
  if (!gate->ready || gate->used) { return 0; }
  gate->ready = 0;
  gate->used = 1U;
  if (jl_crc(live, JL_CONSOLE_BYTES) != gate->console_crc) { return 0; }
  if (gate->mode == JLF_MODE_CLEAN_INIT || gate->mode == JLF_MODE_ROLLOVER) {
    ring[0] = 0;
    barrier();
    // Clear previous session magic words only on explicit rollover so stale
    // armed/used headers beyond the new 8 KiB tail cannot confuse raw scans.
    if (gate->mode == JLF_MODE_ROLLOVER) {
      for (i = 16U; i < JL_CONSOLE_BYTES; i += 8U) {
        if (*(volatile JL64 *)(base + i) == JL_MAGIC) {
          *(volatile JL64 *)(base + i) = 0;
        }
      }
    }
    for (i = 12U; i < 16U; ++i) { base[i] = 0; }
    ring[1] = 4U;
    ring[2] = 4U;
    barrier();
    ring[0] = 0x43474244U;
    barrier();
  }
  return jl_reserve(live, candidate, gate->session, barrier);
}

// Encode one 1024-byte slice of an 8192-byte JLHeader+JLRecord[63] session.
// Format (2227 upper-case QR alphanumeric chars):
// J716FS1/<cand64>/<sess16>/<sha64>/<crc8>/<idx2>/08/<hex2048>/<fcrc8>
static inline JL32 jlf_session_frame(char out[JLF_FRAME_BUF],
    const void *session_bytes, const JL64 candidate[4], JL32 index)
{
  static const char prefix[] = "J716FS1/";
  const JL8 *p = (const JL8 *)session_bytes;
  const JLHeader *h = (const JLHeader *)session_bytes;
  JL8 digest[32];
  JL32 sess_crc, at = 0, i;
  if (index >= JLF_SESSION_FRAMES || h->magic != JL_MAGIC || h->bytes != JL_SESSION_BYTES) {
    return 0;
  }
  jl_sha256(p, JL_SESSION_BYTES, digest);
  sess_crc = jl_crc(p, JL_SESSION_BYTES);
  for (i = 0; i < 8U; ++i) { out[at++] = prefix[i]; }
  for (i = 0; i < 32U; ++i) {
    jlf_hex(out + at + 2U * i, (candidate[i / 8U] >> ((i % 8U) * 8U)) & 255U, 2U);
  }
  at += 64U; out[at++] = '/';
  jlf_hex(out + at, h->session, 16U); at += 16U; out[at++] = '/';
  jlf_hex_bytes(out + at, digest, 32U); at += 64U; out[at++] = '/';
  jlf_hex(out + at, sess_crc, 8U); at += 8U; out[at++] = '/';
  jlf_hex(out + at, index, 2U); at += 2U; out[at++] = '/';
  jlf_hex(out + at, JLF_SESSION_FRAMES, 2U); at += 2U; out[at++] = '/';
  jlf_hex_bytes(out + at, p + index * JLF_SESSION_CHUNK, JLF_SESSION_CHUNK);
  at += 2U * JLF_SESSION_CHUNK; out[at++] = '/';
  jlf_hex(out + at, jl_crc(out, at), 8U); at += 8U;
  out[at] = '\0';
  return at;
}

static inline const char *jlf_slot_label(JL32 slot)
{
  switch (slot) {
  case 0: return "start_begin";
  case 1: return "boot_pt_map";
  case 2: return "arm_init";
  case 3: return "vm_entry";
  case 4: return "vm_perm_map";
  case 5: return "vm_ttbr_sw";
  case 6: return "pmap_boot";
  case 7: return "vm_done";
  case 8: return "ArmDT_cfg";
  case 9: return "ArmDT_iter";
  case 10: return "ArmDT_reg";
  case 11: return "CPU_prepGic";
  case 12: return "CPU_bootSym";
  case 13: return "CPU_reg";
  case 14: return "CPU_gCtrl";
  case 15: return "CPU_gicRdy";
  case 16: return "CPU_timeOn";
  case 17: return "CPU_drained";
  case 18: return "CPU_quiesce";
  case 19: return "CPU_halt";
  case 20: return "iokit_lck";
  case 21: return "bsd_init";
  case 24: return "IRQ1_enter";
  case 25: return "IRQ1_rtclk";
  case 26: return "IRQ1_eoi";
  case 27: return "IRQ2_enter";
  case 28: return "IRQ2_rtclk";
  case 29: return "IRQ2_eoi";
  case 30: return "IRQ8_enter";
  case 31: return "IRQ8_rtclk";
  case 32: return "IRQ8_eoi";
  case 33: return "serial_ok";
  case 34: return "pe_plat";
  case 35: return "cpu_tb";
  case 36: return "pe_fiq";
  case 37: return "early_rng";
  case 38: return "kboot_ent";
  case 39: return "kdbg_1st";
  case 40: return "EARLY_FAULT";
  case 41: return "EL1_SYNC_FLT";
  case 42: return "ANY_SYNC_FLT";
  case 43: return "BOOT_MAP_ERR";
  case 44: return "PERM_MAP_ERR";
  case 45: return "PANIC_CALL";
  case 46: return "PANIC_STR";
  case 47: return "KDBG_LAST";
  case 48: return "SERR_FIQ";
  case 49: return "LOAD_CTX";
  case 50: return "sched_pre1";
  case 51: return "sched_post1";
  case 52: return "sched_pre8";
  case 53: return "sched_post8";
  case 54: return "kboot_thr";
  case 55: return "iokit_pre";
  case 56: return "iokit_post";
  case 57: return "vm_mem_ok";
  case 58: return "con_ok";
  case 59: return "sched_ok";
  case 60: return "mach_ok";
  case 61: return "clock_ok";
  case 62: return "thread_ok";
  default: return "slot";
  }
}

#endif
