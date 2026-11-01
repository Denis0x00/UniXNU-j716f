// SPDX-License-Identifier: BSD-2-Clause-Patent
// Fast Read v4: raw physical session recovery, human-readable stage decoding,
// fault ESR/FAR/ELR spotlight, and instant 8-frame (4-second) session QR export.
#include "Common.h"
#include "NativeFast.h"
#include <Library/QrEncoderLib.h>

STATIC JL64 mFrozen[JL_CONSOLE_BYTES / 8U];
STATIC JLFastScan mScan;
STATIC UINT32 mLinearOffsets[JL_MAX_SESSIONS];
STATIC UINT32 mLinearCount;
STATIC UINT32 mPresent[JL_SLOTS];
STATIC UINT32 mCount, mPage, mSelected, mConsoleCrc, mSessionCrc;
STATIC CONST JLHeader *mHeader;
STATIC BOOLEAN mStable, mQrMode, mQrPlaying;
STATIC UINT32 mQrIndex, mQrGear;
STATIC CHAR8 mFrame[JLF_FRAME_BUF];
STATIC CONST UINTN mIntervals[] = {500, 1000, 2000};

STATIC VOID SelectSession(VOID)
{
  UINT32 slot, i;
  CONST JL64 *raw;
  BOOLEAN present;
  mCount = 0;
  mPage = 0;
  mHeader = (CONST JLHeader *)((CONST JL8 *)mFrozen + mScan.offsets[mSelected]);
  mSessionCrc = jl_crc(mHeader, JL_SESSION_BYTES);
  for (slot = 0; slot < JL_SLOTS; ++slot) {
    raw = (CONST JL64 *)(UINTN)jl_record((volatile JLHeader *)(UINTN)mHeader, slot);
    present = FALSE;
    for (i = 0; i < 16U; ++i) {
      if (raw[i]) { present = TRUE; break; }
    }
    if (present) { mPresent[mCount++] = slot; }
  }
}

STATIC VOID BuildStageSummary(CHAR16 *line1, UINTN size1, CHAR16 *line2, UINTN size2,
    CHAR16 *faultLine, UINTN faultSize)
{
  UINT32 idx, slot;
  CHAR16 item[40];
  line1[0] = L'\0';
  line2[0] = L'\0';
  UnicodeSPrint(faultLine, faultSize, L"Faults: none recorded in slots 40..46,48");
  for (idx = 0; idx < mCount; ++idx) {
    CONST JLRecord *rec;
    slot = mPresent[idx];
    rec = (CONST JLRecord *)((CONST JL8 *)mHeader + 128U + slot * 128U);
    UnicodeSPrint(item, sizeof(item), L"%u:%a ", slot, jlf_slot_label(slot));
    if (StrLen(line1) + StrLen(item) < (size1 / sizeof(CHAR16)) - 2U) {
      StrCatS(line1, size1 / sizeof(CHAR16), item);
    } else if (StrLen(line2) + StrLen(item) < (size2 / sizeof(CHAR16)) - 2U) {
      StrCatS(line2, size2 / sizeof(CHAR16), item);
    }
    if ((slot >= 40U && slot <= 45U) || slot == 48U) {
      UnicodeSPrint(faultLine, faultSize,
        L"ALERT slot=%u(%a) d0=%016lx d1=%016lx d2=%016lx",
        slot, jlf_slot_label(slot), rec->data[0], rec->data[1], rec->data[2]);
    }
  }
  if (mCount == 0) {
    UnicodeSPrint(line1, size1, L"(0 records written - session still ARMED or kernel never entered)");
  }
}

STATIC EFI_STATUS RenderQr(VOID)
{
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bitmap = NULL;
  EFI_STATUS s;
  CHAR16 text[160];
  UINTN w = mGop->Mode->Info->HorizontalResolution;
  UINTN h = mGop->Mode->Info->VerticalResolution;
  UINTN region = MIN(MIN(w - 80U, h - 420U), 1080U);
  JL32 len = jlf_session_frame(mFrame, mHeader, mCandidate, mQrIndex);
  if (!len || w < 800U || h < 1100U) { return EFI_UNSUPPORTED; }
  s = QrEncodeData(QrAutoVersion, QrECLevel_M, QrAlphaNumericMode, 0,
        (UINT8 *)mFrame, (UINT16)len, (INTN)region, &bitmap);
  if (EFI_ERROR(s) || !bitmap) {
    if (bitmap) { FreePool(bitmap); }
    return EFI_OUT_OF_RESOURCES;
  }
  Clear();
  Line(0, L"FAST SESSION QR EXPORT - 8 KiB ONLY (8 FRAMES = 4 SECONDS)");
  UnicodeSPrint(text, sizeof(text), L"Session %016lx  Frame %u/8  CRC %08x  %s %u ms",
    mHeader->session, mQrIndex + 1U, mSessionCrc,
    mQrPlaying ? L"PLAY" : L"PAUSE", (UINT32)mIntervals[mQrGear]);
  Line(1, text);
  s = mGop->Blt(mGop, bitmap, EfiBltBufferToVideo, 0, 0, (w - region) / 2U, 220U, region, region, 0);
  FreePool(bitmap);
  Line(25, L"Power: play/pause. Vol Up/Down: step frame (paused) or speed (playing).");
  Line(26, L"Long Power / Esc: return to session summary.");
  return s;
}

STATIC VOID RenderSummary(VOID)
{
  CHAR16 text[160], s1[140], s2[140], flt[160];
  UINTN i;
  CONST JL32 *ring = (CONST JL32 *)mFrozen;
  CONST JL64 *raw;
  UINT32 slot = 0;

  Clear();
  Line(0, L"J716F FAST READ v4 - RAW SCAN + STAGE DECODER + 8-FRAME QR");
  UnicodeSPrint(text, sizeof(text),
    L"DBGC=%08x next=%08x size=%08x crc=%08x STABLE=%u linear=%u raw=%u",
    ring[0], ring[1], ring[2], mConsoleCrc, mStable, mLinearCount, mScan.total);
  Line(1, text);
  if (!mHeader) {
    UnicodeSPrint(text, sizeof(text), L"Candidate %016lx %016lx", mCandidate[0], mCandidate[1]);
    Line(2, text);
    Line(4, L"NO XNU SESSION IN 256 KiB RAM (0 JL_MAGIC headers found).");
    Line(6, L"-> Go to Menu Item 2 (Fast Init + ARM) to arm in 3 seconds, then Launch XNU!");
    Line(8, L"Power / Esc: return to menu.");
    return;
  }
  UnicodeSPrint(text, sizeof(text),
    L"SESSION %u/%u  ID=%016lx  off=%06x  state=%a  hits=%u/63  crc=%08x",
    mSelected + 1U, mScan.total, mHeader->session, mHeader->offset,
    mHeader->state == JL_ARMED ? "ARMED(unconsumed)" : "USED(ran!)", mCount, mSessionCrc);
  Line(2, text);
  BuildStageSummary(s1, sizeof(s1), s2, sizeof(s2), flt, sizeof(flt));
  UnicodeSPrint(text, sizeof(text), L"Stages: %s", s1); Line(3, text);
  if (s2[0]) { UnicodeSPrint(text, sizeof(text), L"        %s", s2); Line(4, text); }
  Line(5, flt);

  if (mPage == 0) {
    raw = (CONST JL64 *)(UINTN)mHeader;
    UnicodeSPrint(text, sizeof(text), L"PAGE 1/%u: HEADER (Vol Down: next slot | Power: 8-frame QR export)", mCount + 1U);
    Line(6, text);
  } else {
    slot = mPresent[mPage - 1U];
    raw = (CONST JL64 *)((CONST JL8 *)mHeader + 128U + slot * 128U);
    UnicodeSPrint(text, sizeof(text),
      L"PAGE %u/%u: SLOT %u (%a) VALID=%u (Power: 8-frame QR export)",
      mPage + 1U, mCount + 1U, slot, jlf_slot_label(slot),
      jl_valid((CONST volatile JLHeader *)mHeader, (CONST volatile JLRecord *)raw, slot));
    Line(6, text);
  }
  for (i = 0; i < 8U; ++i) {
    UnicodeSPrint(text, sizeof(text), L"%s%02u %016lx   %s%02u %016lx",
      mPage ? L"R" : L"H", (UINT32)(2U * i), raw[2U * i],
      mPage ? L"R" : L"H", (UINT32)(2U * i + 1U), raw[2U * i + 1U]);
    Line(8U + i, text);
  }
  if (mPage > 0) {
    CONST JLRecord *r = (CONST JLRecord *)raw;
    UnicodeSPrint(text, sizeof(text),
      L"Decode: d0=%016lx d1=%016lx d2=%016lx SCTLR=%08lx",
      r->data[0], r->data[1], r->data[2], r->data[3] & 0xffffffffULL);
    Line(17, text);
    UnicodeSPrint(text, sizeof(text),
      L"        TTBR1=%016lx SP=%016lx DAIF=%04lx CNT=%016lx",
      r->data[4], r->data[5], r->data[9] & 0xffffULL, r->counter);
    Line(18, text);
  }
  Line(20, L"Vol Up/Down: flip pages/sessions | Power: open 8-frame QR | Long Power/Esc: menu");
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image, EFI_SYSTEM_TABLE *system)
{
  EFI_STATUS s;
  EFI_INPUT_KEY key;
  EFI_EVENT timer = NULL, events[2];
  UINTN ev;
  (VOID)image;
  (VOID)system;

  s = Panel();
  if (EFI_ERROR(s)) { return s; }
  mStable = jl_freeze((CONST volatile VOID *)(UINTN)JL_CONSOLE_PA, mFrozen, Barrier);
  mConsoleCrc = jl_crc(mFrozen, JL_CONSOLE_BYTES);
  mLinearCount = mStable ? jl_recover_sessions(mFrozen, mCandidate, mLinearOffsets) : 0;
  jlf_scan_raw(mFrozen, mCandidate, &mScan);
  if (mScan.total > 0) {
    mSelected = mScan.total - 1U;
    SelectSession();
  }
  s = gBS->CreateEvent(EVT_TIMER, TPL_APPLICATION, NULL, NULL, &timer);
  if (EFI_ERROR(s)) { return s; }
  events[0] = gST->ConIn->WaitForKey;
  events[1] = timer;

  for (;;) {
    if (mQrMode && mHeader) {
      s = RenderQr();
      if (EFI_ERROR(s)) { mQrMode = FALSE; continue; }
      gBS->SetTimer(timer, mQrPlaying ? TimerRelative : TimerCancel, mIntervals[mQrGear] * 10000ULL);
      s = gBS->WaitForEvent(mQrPlaying ? 2U : 1U, events, &ev);
      if (EFI_ERROR(s)) { break; }
      if (ev == 1U) { mQrIndex = (mQrIndex + 1U) % JLF_SESSION_FRAMES; continue; }
      if (EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) { continue; }
      if (key.ScanCode == SCAN_ESC) {
        gBS->SetTimer(timer, TimerCancel, 0);
        mQrMode = FALSE;
        mQrPlaying = FALSE;
        continue;
      }
      if (key.UnicodeChar == L'\r') { mQrPlaying = !mQrPlaying; continue; }
      if (key.ScanCode == SCAN_UP || key.ScanCode == SCAN_DOWN) {
        if (mQrPlaying) {
          mQrGear = (mQrGear + (key.ScanCode == SCAN_UP ? 2U : 1U)) % 3U;
        } else {
          mQrIndex = (mQrIndex + (key.ScanCode == SCAN_UP ? (JLF_SESSION_FRAMES - 1U) : 1U)) % JLF_SESSION_FRAMES;
        }
      }
      continue;
    }

    RenderSummary();
    s = Key(&key);
    if (EFI_ERROR(s)) { continue; }
    if (key.ScanCode == SCAN_ESC || (!mHeader && key.UnicodeChar == L'\r')) { break; }
    if (!mHeader) { continue; }
    if (key.UnicodeChar == L'\r') {
      mQrMode = TRUE;
      mQrIndex = 0;
      mQrPlaying = TRUE;
      continue;
    }
    if (key.ScanCode == SCAN_UP) {
      if (mPage == 0 && mScan.total > 1U) {
        mSelected = (mSelected + mScan.total - 1U) % mScan.total;
        SelectSession();
      } else {
        mPage = (mPage + mCount) % (mCount + 1U);
      }
    } else if (key.ScanCode == SCAN_DOWN) {
      if (mPage == mCount && mScan.total > 1U) {
        mSelected = (mSelected + 1U) % mScan.total;
        SelectSession();
      } else {
        mPage = (mPage + 1U) % (mCount + 1U);
      }
    }
  }
  gBS->SetTimer(timer, TimerCancel, 0);
  gBS->CloseEvent(timer);
  return EFI_SUCCESS;
}
