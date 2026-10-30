// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host verification for NativeFast.h against exact NativeLog.h kernel admission.
#include <assert.h>
#include <string.h>
#include "NativeFast.h"
#include "NativeMapFixed.h"
#include "NativeVmMapFixed.h"

static void nop_barrier(void) {}

static JL64 gL1[256] __attribute__((aligned(4096)));
static JL64 gL2[512] __attribute__((aligned(4096)));
static JL64 gL3[512] __attribute__((aligned(4096)));
static int gL3Used;
static JL64 *mock_resolve(JL64 pa) {
  if (pa == 0x00000000d2f7f000ULL) return gL2;
  if (pa == 0x00000000d3867000ULL) return gL3;
  return 0;
}
static JL64 *mock_alloc(JL64 *pa) {
  if (gL3Used) return 0;
  gL3Used = 1;
  *pa = 0x00000000d3867000ULL;
  return gL3;
}

static const JL64 kCand[4] = {
  0xcf8d38d551ee191cULL, 0xa561f3dc41fdaebbULL,
  0x6f9f888dd0e3ce4cULL, 0x92d8089af6b64b0eULL
};

int main(void)
{
  static JL64 ram[JL_CONSOLE_BYTES / 8U];
  JLFastArmGate gate;
  JLFastScan scan;
  volatile JLHeader *h;
  JL64 data[10] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
  char frame[JLF_FRAME_BUF];
  JL32 i;

  // 1. Cold-boot uninitialized RAM (exact header observed on 2026-10-29!)
  memset(ram, 0xa5, sizeof(ram));
  {
    JL32 hdr[3] = {0x00b7266bU, 0x8e025595U, 0x2a002821U};
    memcpy(ram, hdr, sizeof(hdr));
  }

  memset(&gate, 0, sizeof(gate));
  assert(jlf_arm_query(&gate, ram, kCand, 4ULL, 0x10001ULL, 0ULL, 0x1111ULL, 0, nop_barrier) == 1);
  assert(gate.mode == JLF_MODE_CLEAN_INIT);
  h = jlf_arm_commit(&gate, ram, kCand, nop_barrier);
  assert(h != 0);
  // Exact kernel j716f_native_begin() check via jl_find()!
  assert(jl_find(ram, kCand) == h);
  assert(h->state == JL_ARMED && h->offset == 16U);

  // Simulate XNU consuming the session and writing slot 0 & slot 17
  h->state = JL_USED;
  assert(jl_write(h, 0U, 0x9999ULL, data, nop_barrier) == 1);
  assert(jl_write(h, 17U, 0xaaaaULL, data, nop_barrier) == 1);

  // 2. Next run appends cleanly at offset 8208 without wiping Session 1!
  memset(&gate, 0, sizeof(gate));
  assert(jlf_arm_query(&gate, ram, kCand, 4ULL, 0x10001ULL, 0ULL, 0x2222ULL, 0, nop_barrier) == 1);
  assert(gate.mode == JLF_MODE_APPEND);
  h = jlf_arm_commit(&gate, ram, kCand, nop_barrier);
  assert(h != 0 && h->offset == 8208U);
  assert(jl_find(ram, kCand) == h);
  jlf_scan_raw(ram, kCand, &scan);
  assert(scan.total == 2U && scan.used_count == 1U && scan.armed_count == 1U);

  // 3. Even if Android fastbootd corrupts/wraps the 12-byte DBGC header after XNU ran,
  // jlf_scan_raw STILL recovers both sessions, and non-rollover query refuses to wipe them!
  h->state = JL_USED;
  {
    JL32 hdr[3] = {0x43474244U, 0x0002a354U, JL_CAPACITY};
    memcpy(ram, hdr, sizeof(hdr));
  }
  jlf_scan_raw(ram, kCand, &scan);
  assert(scan.total == 2U && scan.used_count == 2U);
  memset(&gate, 0, sizeof(gate));
  assert(jlf_arm_query(&gate, ram, kCand, 4ULL, 0x10001ULL, 0ULL, 0x3333ULL, 0, nop_barrier) == 0);
  assert(gate.error == JLF_ERR_NEED_ROLLOVER);

  // Explicit rollover after reading/exporting succeeds cleanly:
  memset(&gate, 0, sizeof(gate));
  assert(jlf_arm_query(&gate, ram, kCand, 4ULL, 0x10001ULL, 0ULL, 0x3333ULL, 1, nop_barrier) == 1);
  assert(gate.mode == JLF_MODE_ROLLOVER);
  h = jlf_arm_commit(&gate, ram, kCand, nop_barrier);
  assert(h != 0 && jl_find(ram, kCand) == h);
  jlf_scan_raw(ram, kCand, &scan);
  assert(scan.total == 1U && scan.armed_count == 1U);

  // 4. 8-frame session QR encoder produces exact 2227-char valid frames
  for (i = 0; i < JLF_SESSION_FRAMES; ++i) {
    assert(jlf_session_frame(frame, (const void *)h, kCand, i) == JLF_FRAME_CHARS);
  }

  // 5. Verify XNU T1SZ=26 (38-bit VA, 256-entry L1 root[192]) page-table rebind
  // using exact hardware Run 1 parameters (va=0xfffffff007aac000, pa=0xd38ac000).
  {
    JLFMapOps ops = {mock_resolve, mock_alloc};
    memset(gL1, 0, sizeof(gL1));
    memset(gL2, 0, sizeof(gL2));
    memset(gL3, 0, sizeof(gL3));
    gL3Used = 0;
    gL1[0xc0] = 0x00000000d2f7f003ULL;
    gL2[0x3d] = 0x00000000d3800601ULL;
    assert(jlf_map_window_t1sz26(gL1, 0xfffffff007aac000ULL, 0x00000000d38ac000ULL,
                                 &ops, 0x006000000000060fULL) == 1);
    assert((gL3[0xac] & JLF_PAGE_MASK) == JL_CONSOLE_PA);
    assert((gL3[0xeb] & JLF_PAGE_MASK) == JL_CONSOLE_PA + 63ULL * 4096ULL);
    assert((gL3[0xab] & JLF_PAGE_MASK) == 0x00000000d38ab000ULL);
  }

  // 6. Verify pre-TTBR1-switch linear bootstrap PA->KVA resolver (Run 2 fix):
  // l2_pa=0xd422a000 inside [gPhysBase=0xd007c000, +0x10000000) maps to
  // 0xfffffff00842a000 under bootstrap_pagetables (not slid +0x200000 ptov KVA).
  assert(jlf_bootstrap_phystokv(0x00000000d422a000ULL, 0x00000000d007c000ULL,
                                0x0000000010000000ULL, 0xfffffff00427c000ULL) ==
         0xfffffff00842a000ULL);
  assert(jlf_bootstrap_phystokv(0x00000000b00be000ULL, 0x00000000d007c000ULL,
                                0x0000000010000000ULL, 0xfffffff00427c000ULL) == 0ULL);

  // 7. Verify FAST-V4 post-vm_done, panic, kdbg, and IOKit slot labels.
  assert(strcmp(jlf_slot_label(33U), "serial_ok") == 0);
  assert(strcmp(jlf_slot_label(38U), "kboot_ent") == 0);
  assert(strcmp(jlf_slot_label(39U), "kdbg_1st") == 0);
  assert(strcmp(jlf_slot_label(45U), "PANIC_CALL") == 0);
  assert(strcmp(jlf_slot_label(46U), "PANIC_STR") == 0);
  assert(strcmp(jlf_slot_label(47U), "KDBG_LAST") == 0);
  assert(strcmp(jlf_slot_label(49U), "LOAD_CTX") == 0);
  assert(strcmp(jlf_slot_label(54U), "kboot_thr") == 0);
  assert(strcmp(jlf_slot_label(55U), "iokit_pre") == 0);
  assert(strcmp(jlf_slot_label(62U), "thread_ok") == 0);
  return 0;
}
