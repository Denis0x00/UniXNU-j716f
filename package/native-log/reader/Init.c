// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit, receipt-gated initializer. Separate from Read/Export and Query/ARM.
#include "Common.h"
#include "NativeExport.h"
STATIC JL64 mSnapshot[JL_CONSOLE_BYTES/8];
STATIC JLInitGate mInit;
STATIC UINT64 mInput;
STATIC UINT32 mDigit,mPosition,mConfirmations;
STATIC BOOLEAN mFinished;
STATIC CHAR8 mHash[65];
STATIC CONST CHAR16 *mMessage=L"Enter host receipt ONLY after complete private dump was saved and verified.";
STATIC VOID RenderInit(VOID) {
  CHAR16 text[160];CONST JL32 *ring=(CONST JL32 *)mSnapshot;
  Clear();Line(0,L"NATIVE INIT v1 - DESTRUCTIVE HEADER CHANGE / NO ARM / NO RESET");
  UnicodeSPrint(text,sizeof(text),L"FROZEN DBGC %08x next=%08x size=%08x",ring[0],ring[1],ring[2]);Line(1,text);
  UnicodeSPrint(text,sizeof(text),L"Candidate %016lx %016lx",mCandidate[0],mCandidate[1]);Line(2,text);
  UnicodeSPrint(text,sizeof(text),L"          %016lx %016lx",mCandidate[2],mCandidate[3]);Line(3,text);
  UnicodeSPrint(text,sizeof(text),L"Snapshot SHA256 %a",mHash);Line(5,text);
  UnicodeSPrint(text,sizeof(text),L"Typed receipt %016lx. Digit %u/16, selected hex %x",mInput,MIN(mPosition+1,16),mDigit);Line(7,text);
  UnicodeSPrint(text,sizeof(text),L"Confirmations %u/2  ready=%u used=%u",mConfirmations,mInit.ready,mInit.used);Line(8,text);
  Line(10,mMessage);
  Line(13,L"Only full wrapped DBGC accepted. Intact unconsumed ARM anywhere blocks init.");
  Line(14,L"Changes first 272 bytes only; remaining raw bytes are NOT erased.");
  Line(15,L"Old complete snapshot must exist on host. Receipt is not cryptographic authorization.");
  Line(17,L"Volume: choose hex digit. Power: accept digit, then two separate confirmations.");
  Line(18,L"Volume during confirmation cancels it. Long power/Esc: leave without more writes.");
  Line(20,L"Success creates an init receipt, NOT an ARM. Photograph result; return to READ.");
  Line(21,L"No automatic Query, native launch, retry, file write or reset.");
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *system) {
  EFI_STATUS s;EFI_INPUT_KEY key;UINT64 counter;BOOLEAN stable;EFI_TPL oldTpl;JL8 digest[32];
  (void)image;(void)system;
  s=Panel();if(EFI_ERROR(s)) return s;
  stable=jl_freeze((CONST volatile VOID *)(UINTN)JL_CONSOLE_PA,mSnapshot,Barrier);
  jl_sha256(mSnapshot,JL_CONSOLE_BYTES,digest);jl_hex_bytes(mHash,digest,32);mHash[64]=0;
  __asm__ volatile("isb; mrs %0, cntvct_el0":"=r"(counter)::"memory");
  if(!jl_init_prepare(&mInit,mSnapshot,mCandidate,counter,stable)) {
    mFinished=TRUE;mMessage=L"BLOCKED: unstable/non-full/non-wrapped/corrupt header or pending ARM. No writes.";
  }
  for(;;) {
    RenderInit();s=Key(&key);if(EFI_ERROR(s)) continue;
    if(key.ScanCode==SCAN_ESC) return EFI_SUCCESS;
    if(mFinished) continue;
    if(key.ScanCode==SCAN_UP || key.ScanCode==SCAN_DOWN) {
      if(mPosition==16) {mInput=0;mPosition=0;mConfirmations=0;mDigit=0;mMessage=L"Cancelled. Re-enter complete receipt; no writes.";}
      else mDigit=(mDigit+(key.ScanCode==SCAN_UP?1:15))%16;
    }
    if(key.UnicodeChar!=L'\r') continue;
    if(mPosition<16) {
      mInput=(mInput<<4)|mDigit;mDigit=0;mPosition++;
      if(mPosition==16) {
        if(mInput!=mInit.record.receipt) {mFinished=TRUE;mMessage=L"WRONG RECEIPT. No writes. Do not initialize without a complete matching backup.";}
        else mMessage=L"Receipt matches exact snapshot. Power: first confirmation to replace DBGC header.";
      }
      continue;
    }
    if(++mConfirmations==1) {mMessage=L"FINAL CONFIRMATION: Power again changes header. Volume cancels; long power exits.";continue;}
    oldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
    stable=jl_init_commit(&mInit,(volatile VOID *)(UINTN)JL_CONSOLE_PA,mSnapshot,mInput,mConfirmations,Barrier);
    gBS->RestoreTPL(oldTpl);
    mFinished=TRUE;
    mMessage=stable?L"INIT READBACK OK: next=size=00000104. No ARM. Photograph, then READ before further action.":
      L"INIT FAILED/CHANGED CONSOLE. Stop; no retry, ARM or launch. Export current bytes separately.";
  }
}
