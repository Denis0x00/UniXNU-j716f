// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fast ARM v4: 3-second cold-boot / dirty-ring clean init + session ARM.
// Preserves NativeLog.h kernel admission ABI 100%; never auto-resets.
#include "Common.h"
#include "NativeFast.h"

VOID J716fSmc(UINT64 *args);

STATIC JLFastArmGate mGate;
STATIC JLFastScan mScan;
STATIC BOOLEAN mConfirm;
STATIC UINTN mSelected;
STATIC UINT64 mVersion, mFeature;
STATIC volatile JLHeader *mArmed;
STATIC CONST CHAR16 *mStatus = L"Select [1] Query & Prepare (or [2] Rollover if prior sessions exist).";
STATIC volatile VOID *mLive = (volatile VOID *)(UINTN)JL_CONSOLE_PA;

STATIC CONST CHAR16 *ModeName(JL32 mode)
{
  switch (mode) {
  case JLF_MODE_APPEND: return L"APPEND (valid linear DBGC tail)";
  case JLF_MODE_CLEAN_INIT: return L"CLEAN_INIT (0 XNU sessions -> init DBGC + ARM)";
  case JLF_MODE_ROLLOVER: return L"ROLLOVER (reset ring & re-arm)";
  default: return L"NONE";
  }
}

STATIC VOID DoQuery(BOOLEAN allowRollover)
{
  UINT64 args[8], counter;
  mConfirm = FALSE;
  if (mGate.used || CurrentEl() != 4ULL) {
    mStatus = L"Already used in this invocation (or wrong EL). Re-open from menu.";
    return;
  }
  ZeroMem(args, sizeof(args));
  args[0] = 0x84000000U;
  J716fSmc(args);
  mVersion = args[0];

  ZeroMem(args, sizeof(args));
  args[0] = 0x8400000aU;
  args[1] = 0x84000000U;
  J716fSmc(args);
  mFeature = args[0];

  __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(counter) :: "memory");
  jlf_scan_raw(mLive, mCandidate, &mScan);
  if (jlf_arm_query(&mGate, mLive, mCandidate, CurrentEl(), mVersion, mFeature,
                    counter, allowRollover ? 1 : 0, Barrier)) {
    mSelected = 2U;
    mStatus = L"READY! Press Power twice on [3] Commit ARM to arm kernel launch.";
  } else if (mGate.error == JLF_ERR_PENDING_ARM) {
    mStatus = L"Existing ARMED session found! Launch XNU directly, or use [2] Rollover.";
  } else if (mGate.error == JLF_ERR_NEED_ROLLOVER) {
    mStatus = L"Prior USED XNU session(s) in non-linear/full RAM. Read first, then [2] Rollover.";
  } else {
    mStatus = L"Query blocked (PSCI / unstable RAM).";
  }
}

STATIC VOID DoCommit(VOID)
{
  EFI_TPL oldTpl;
  if (!mGate.ready || mGate.used) {
    mConfirm = FALSE;
    mStatus = L"Run [1] Query & Prepare (or [2] Rollover) first.";
    return;
  }
  if (!mConfirm) {
    mConfirm = TRUE;
    mStatus = L"CONFIRM 1/2 OK -> Press Power ONCE MORE to write DBGC/ARM!";
    return;
  }
  mConfirm = FALSE;
  oldTpl = gBS->RaiseTPL(TPL_HIGH_LEVEL);
  mArmed = jlf_arm_commit(&mGate, mLive, mCandidate, Barrier);
  gBS->RestoreTPL(oldTpl);
  jlf_scan_raw(mLive, mCandidate, &mScan);
  if (mArmed && jl_find(mLive, mCandidate) == mArmed) {
    mSelected = 3U;
    mStatus = L"ARMED OK! Press Power or Esc -> select [3. Launch Native XNU]!";
  } else {
    mStatus = L"ARM FAILED (RAM changed during commit). Re-open app.";
  }
}

STATIC VOID Render(VOID)
{
  STATIC CONST CHAR16 *opts[] = {
    L"1. Query & Fast Prepare (safe: refuses to overwrite XNU sessions)",
    L"2. Query with Rollover (after saving prior Read/QR sessions)",
    L"3. Commit ARM (press Power twice after Query)",
    L"4. Return to Main Menu",
    L"5. Warm Restart Tablet (PSCI SYSTEM_RESET -> inspect RAM)",
    L"6. Power Off Tablet (PSCI SYSTEM_OFF)"
  };
  CHAR16 text[160];
  UINTN i;
  CONST volatile JL32 *ring = (CONST volatile JL32 *)mLive;

  Clear();
  Line(0, L"J716F FAST ARM v4 - 3-SECOND COLD/DIRTY READY (NO 256-FRAME QR)");
  UnicodeSPrint(text, sizeof(text),
    L"Live DBGC=%08x next=%08x size=%08x  XNU sessions: used=%u armed=%u",
    ring[0], ring[1], ring[2], mScan.used_count, mScan.armed_count);
  Line(1, text);
  UnicodeSPrint(text, sizeof(text), L"Candidate %016lx %016lx", mCandidate[0], mCandidate[1]);
  Line(2, text);
  UnicodeSPrint(text, sizeof(text),
    L"PSCI=%08lx feat=%08lx  Mode: %s",
    mVersion & 0xffffffffULL, mFeature & 0xffffffffULL, ModeName(mGate.mode));
  Line(4, text);
  UnicodeSPrint(text, sizeof(text),
    L"Session=%016lx  off=%06x  hdrCRC=%08x  ARMED=%u",
    mArmed ? mArmed->session : mGate.session,
    mArmed ? mArmed->offset : 0U,
    mArmed ? mArmed->crc : 0U,
    mArmed != NULL);
  Line(5, text);

  for (i = 0; i < 6U; ++i) {
    UnicodeSPrint(text, sizeof(text), L"%s %s", i == mSelected ? L">" : L" ", opts[i]);
    Line(7U + i, text);
  }
  Line(14, mStatus);
  Line(16, L"Zero JL_MAGIC in RAM (cold boot or fastbootd) initializes cleanly in 3s.");
  Line(17, L"Vol Up/Down: select | Power: execute | Long Power/Esc: return to menu");
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
  EFI_STATUS s;
  EFI_INPUT_KEY key;
  (VOID)image;
  (VOID)system;

  s = Panel();
  if (EFI_ERROR(s)) { return s; }
  jlf_scan_raw(mLive, mCandidate, &mScan);
  for (;;) {
    Render();
    s = Key(&key);
    if (EFI_ERROR(s)) { continue; }
    if (key.ScanCode == SCAN_ESC) { return EFI_SUCCESS; }
    if (key.ScanCode == SCAN_UP || key.ScanCode == SCAN_DOWN) {
      mConfirm = FALSE;
      mSelected = (mSelected + (key.ScanCode == SCAN_UP ? 5U : 1U)) % 6U;
      continue;
    }
    if (key.UnicodeChar != L'\r') { continue; }
    if (mSelected == 0U) { DoQuery(FALSE); }
    else if (mSelected == 1U) { DoQuery(TRUE); }
    else if (mSelected == 2U) { DoCommit(); }
    else if (mSelected == 3U) { return EFI_SUCCESS; }
    else if (mSelected == 4U) {
      UINT64 args[8];
      ZeroMem(args, sizeof(args));
      args[0] = 0x84000009U;
      Barrier();
      J716fSmc(args);
    }
    else if (mSelected == 5U) {
      UINT64 args[8];
      ZeroMem(args, sizeof(args));
      args[0] = 0x84000008U;
      Barrier();
      J716fSmc(args);
    }
  }
}
