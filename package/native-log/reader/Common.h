// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <Uefi.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/ArmLib.h>
#include "NativeFirmware.h"
#include "NativeCandidate.h"
extern CONST UINT8 gCaFontGlyphs[95][19];
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
STATIC EFI_GRAPHICS_OUTPUT_BLT_PIXEL mBg={15,12,11,0},mFg={245,243,242,0};
STATIC CONST JL64 mCandidate[4]=JL_CANDIDATE_WORDS;
STATIC VOID Barrier(VOID) { ArmDataSynchronizationBarrier(); }
STATIC UINT64 CurrentEl(VOID) { UINT64 v; __asm__ volatile("mrs %0, CurrentEL":"=r"(v));return v; }
#include "Panel.h"
STATIC EFI_STATUS Panel(VOID) {
  EFI_STATUS s;
  if(gST->ConIn==NULL || CurrentEl()!=4) return EFI_UNSUPPORTED;
  s=gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid,NULL,(VOID **)&mGop);
  if(EFI_ERROR(s) || !mGop->Mode || !mGop->Mode->Info) return EFI_UNSUPPORTED;
  return gBS->SetWatchdogTimer(0,0,0,NULL);
}
STATIC VOID Clear(VOID) {
  mGop->Blt(mGop,&mBg,EfiBltVideoFill,0,0,0,0,
    mGop->Mode->Info->HorizontalResolution,mGop->Mode->Info->VerticalResolution,0);
}
STATIC EFI_STATUS Key(EFI_INPUT_KEY *key) {
  EFI_STATUS s;UINTN event;
  s=gBS->WaitForEvent(1,&gST->ConIn->WaitForKey,&event);
  if(EFI_ERROR(s)) return s;
  return gST->ConIn->ReadKeyStroke(gST->ConIn,key);
}
