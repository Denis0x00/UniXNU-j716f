// SPDX-License-Identifier: BSD-2-Clause-Patent
// Full RAW optical export. NO retained stores, file writes, ARM, SMC or reset.
#include "Common.h"
#include "NativeExport.h"
#include <Library/QrEncoderLib.h>
STATIC JL64 mRaw[JL_CONSOLE_BYTES/8];
STATIC JL8 mDigest[32];
STATIC CHAR8 mFrame[JL_EXPORT_TEXT],mDigestText[65];
STATIC UINT64 mTransfer;
STATIC UINT32 mIndex,mCrc,mGear=1;
STATIC BOOLEAN mPlaying;
STATIC CONST UINTN mIntervals[]={500,1000,2000,5000};
STATIC EFI_STATUS RenderExport(VOID) {
  EFI_STATUS s;EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bitmap=NULL;CHAR16 text[160];
  UINTN width=mGop->Mode->Info->HorizontalResolution,height=mGop->Mode->Info->VerticalResolution;
  UINTN region;JL32 length;
  if(width<800 || height<1100) return EFI_UNSUPPORTED;
  region=MIN(MIN(width-80,height-400),1152);
  length=jl_export_frame(mFrame,mRaw,mCandidate,mDigest,mTransfer,mCrc,mIndex);
  if(!length) return EFI_INVALID_PARAMETER;
  s=QrEncodeData(QrAutoVersion,QrECLevel_M,QrAlphaNumericMode,0,(UINT8 *)mFrame,(UINT16)length,(INTN)region,&bitmap);
  if(EFI_ERROR(s) || !bitmap) {if(bitmap) FreePool(bitmap);return EFI_OUT_OF_RESOURCES;}
  Clear();Line(0,L"NATIVE RAW EXPORT v1 - 256 KiB / READ ONLY / OFFLINE");
  UnicodeSPrint(text,sizeof(text),L"Transfer %016lx frame %u/%u CRC %08x %s %u ms",mTransfer,mIndex+1,JL_EXPORT_FRAMES,mCrc,mPlaying?L"PLAY":L"PAUSE",(UINT32)mIntervals[mGear]);Line(1,text);
  UnicodeSPrint(text,sizeof(text),L"Snapshot SHA256 %a",mDigestText);Line(2,text);
  s=mGop->Blt(mGop,bitmap,EfiBltBufferToVideo,0,0,(width-region)/2,240,region,region,0);
  FreePool(bitmap);
  Line(26,L"Power: play/pause. Volume: frame (paused), speed (playing). Long power: exit.");
  Line(27,L"Keep all frames PRIVATE. Host must verify/save ALL bytes before Init. No auto-init.");
  return s;
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *system) {
  EFI_STATUS s;EFI_INPUT_KEY key;EFI_EVENT timer,events[2];UINTN event;
  (void)image;(void)system;
  s=Panel();if(EFI_ERROR(s)) return s;
  if(!jl_freeze((CONST volatile VOID *)(UINTN)JL_CONSOLE_PA,mRaw,Barrier) ||
      !jl_same((CONST volatile VOID *)(UINTN)JL_CONSOLE_PA,mRaw,JL_CONSOLE_BYTES)) {
    Clear();Line(0,L"UNSTABLE SNAPSHOT. No export receipt, no initialization. Stop.");
    while(!EFI_ERROR(Key(&key))) if(key.ScanCode==SCAN_ESC || key.UnicodeChar==L'\r') return EFI_ABORTED;
    return EFI_ABORTED;
  }
  jl_sha256(mRaw,JL_CONSOLE_BYTES,mDigest);jl_hex_bytes(mDigestText,mDigest,32);mDigestText[64]=0;
  mCrc=jl_crc(mRaw,JL_CONSOLE_BYTES);
  __asm__ volatile("isb; mrs %0, cntvct_el0":"=r"(mTransfer)::"memory");
  if(!mTransfer) return EFI_ABORTED;
  s=gBS->CreateEvent(EVT_TIMER,TPL_APPLICATION,NULL,NULL,&timer);if(EFI_ERROR(s)) return s;
  events[0]=gST->ConIn->WaitForKey;events[1]=timer;
  for(;;) {
    s=RenderExport();if(EFI_ERROR(s)) break;
    s=gBS->SetTimer(timer,mPlaying?TimerRelative:TimerCancel,mIntervals[mGear]*10000ULL);
    if(EFI_ERROR(s)) break;
    s=gBS->WaitForEvent(mPlaying?2:1,events,&event);if(EFI_ERROR(s)) break;
    if(event==1) {mIndex=(mIndex+1)%JL_EXPORT_FRAMES;continue;}
    s=gST->ConIn->ReadKeyStroke(gST->ConIn,&key);if(EFI_ERROR(s)) continue;
    if(key.ScanCode==SCAN_ESC) {s=EFI_SUCCESS;break;}
    if(key.UnicodeChar==L'\r') mPlaying=!mPlaying;
    if(key.ScanCode==SCAN_UP || key.ScanCode==SCAN_DOWN) {
      if(mPlaying) mGear=(mGear+(key.ScanCode==SCAN_UP?3:1))%4;
      else mIndex=(mIndex+(key.ScanCode==SCAN_UP?JL_EXPORT_FRAMES-1:1))%JL_EXPORT_FRAMES;
    }
  }
  gBS->SetTimer(timer,TimerCancel,0);gBS->CloseEvent(timer);
  // The static copy dies with this app; never claim it is a durable backup.
  return s;
}
