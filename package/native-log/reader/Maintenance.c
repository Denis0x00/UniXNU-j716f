// SPDX-License-Identifier: BSD-2-Clause-Patent
// One invocation owns the frozen export and initialization gate. No shared
// protocol/NVRAM ticket, no automatic ARM/launch/reset, no receipt across exits.
#include "Common.h"
#include "NativeMaintenance.h"
#include "NativeMaintenanceProfile.h"
#include <Library/QrEncoderLib.h>
STATIC JL64 mSnapshot[JL_CONSOLE_BYTES/8];
STATIC CONST JL8 mProfile[32]=JL_MAINTENANCE_PROFILE;
STATIC JL8 mDigest[32];
STATIC CHAR8 mFrame[JL2_TEXT],mHash[65],mProfileText[65];
STATIC JL2Gate mInit;
STATIC JL64 mTransfer;
STATIC JL32 mIndex,mCrc,mGear=1;
STATIC BOOLEAN mPlaying;
STATIC CONST UINTN mIntervals[]={500,1000,2000,5000};
STATIC CONST CHAR16 *Reason(JL32 n) {
  switch(n) {
  case JL2_FULL_WRAPPED:return L"FULL_WRAPPED";
  case JL2_FULL_LINEAR:return L"FULL_LINEAR";
  case JL2_EXHAUSTED:return L"LINEAR_EXHAUSTED";
  case JL2_SPACE:return L"SPACE_AVAILABLE: Init unnecessary; preserve bytes";
  case JL2_PENDING:return L"PENDING_ARM: preserve unconsumed session";
  case JL2_SUSPECT:return L"SUSPECT_SESSION: preserve corrupt/unknown record";
  case JL2_UNSTABLE:return L"UNSTABLE: export only, no writes";
  case JL2_IDENTITY:return L"INVALID_IDENTITY";
  default:return L"INVALID_HEADER: export only, no repair";
  }
}
STATIC VOID IdLines(UINTN row) {
  CHAR16 text[100];CHAR8 half[33];UINTN i;
  for(i=0;i<32;i++) half[i]=mHash[i];half[32]=0;
  UnicodeSPrint(text,sizeof(text),L"SHA256  %a",half);Line(row,text);
  UnicodeSPrint(text,sizeof(text),L"        %a",mHash+32);Line(row+1,text);
  for(i=0;i<32;i++) half[i]=mProfileText[i];half[32]=0;
  UnicodeSPrint(text,sizeof(text),L"Profile %a",half);Line(row+2,text);
  UnicodeSPrint(text,sizeof(text),L"        %a",mProfileText+32);Line(row+3,text);
}
STATIC EFI_STATUS Drain(VOID) {
  EFI_INPUT_KEY key;EFI_STATUS s;UINTN i;
  for(i=0;i<64;i++) {s=gST->ConIn->ReadKeyStroke(gST->ConIn,&key);if(s==EFI_NOT_READY)return EFI_SUCCESS;if(EFI_ERROR(s))return s;}
  return EFI_ABORTED;
}
STATIC EFI_STATUS DrawExport(VOID) {
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *bitmap=NULL;EFI_STATUS s;CHAR16 text[128];
  UINTN w=mGop->Mode->Info->HorizontalResolution,h=mGop->Mode->Info->VerticalResolution,region;
  JL32 length=jl2_frame(mFrame,mSnapshot,mCandidate,mDigest,mProfile,mTransfer,mCrc,mIndex);
  if(w<800 || h<1400 || !length) return EFI_UNSUPPORTED;
  region=MIN(MIN(w-80,h-520),1024);
  s=QrEncodeData(QrAutoVersion,QrECLevel_M,QrAlphaNumericMode,0,(UINT8 *)mFrame,(UINT16)length,(INTN)region,&bitmap);
  if(EFI_ERROR(s) || !bitmap) {if(bitmap)FreePool(bitmap);return EFI_OUT_OF_RESOURCES;}
  Clear();Line(0,L"NATIVE MAINT v2 - EXPORT FIRST / NO WRITES WHILE EXPORTING");
  UnicodeSPrint(text,sizeof(text),L"Transfer %016lx frame %u/256 CRC %08x %s %u ms",mTransfer,mIndex+1,mCrc,mPlaying?L"PLAY":L"PAUSE",(UINT32)mIntervals[mGear]);Line(1,text);
  IdLines(2);
  s=mGop->Blt(mGop,bitmap,EfiBltBufferToVideo,0,0,(w-region)/2,380,region,region,0);FreePool(bitmap);
  Line(26,L"Power: play/pause. Volume: frame/speed. Long power: options (default exit).");
  Line(27,L"Keep this app open until host saves ALL 256 frames. Old v1 receipt is refused.");
  return s;
}
STATIC EFI_STATUS ExportOptions(BOOLEAN *initialize) {
  EFI_INPUT_KEY key;EFI_STATUS s;BOOLEAN choose=FALSE;
  *initialize=FALSE;s=Drain();if(EFI_ERROR(s))return s;
  for(;;) {
    Clear();Line(0,L"NATIVE MAINT v2 - SAME-INVOCATION SNAPSHOT");IdLines(2);
    Line(8,choose?L"  Exit to menu (discard this transfer)":L"> Exit to menu (discard this transfer)");
    Line(9,choose?L"> Enter host receipt and review Init":L"  Enter host receipt and review Init");
    Line(12,L"Volume up: review Init. Volume down: exit. Power: select. Long power: exit.");
    Line(14,L"Continue only after the host saved and verified this exact transfer.");
    Line(15,L"A screenshot or a previous capture is not a complete backup.");
    s=Key(&key);if(EFI_ERROR(s))return s;
    if(key.ScanCode==SCAN_ESC)return EFI_SUCCESS;
    if(key.ScanCode==SCAN_UP)choose=TRUE;
    if(key.ScanCode==SCAN_DOWN)choose=FALSE;
    if(key.UnicodeChar==L'\r') {*initialize=choose;return Drain();}
  }
}
STATIC EFI_STATUS Initialize(VOID) {
  EFI_INPUT_KEY key;EFI_STATUS s;CHAR16 text[160];JL64 input=0;JL32 digit=0,pos=0,confirm=0,available;
  BOOLEAN finished=FALSE,consent=FALSE,ok;EFI_TPL oldTpl;
  CONST CHAR16 *message=L"Enter the private host receipt for THIS transfer.";
  const JL32 *ring=(const JL32 *)mSnapshot;
  if(!jl2_prepare(&mInit,mSnapshot,mCandidate,mProfile,mTransfer,
      jl_same((const volatile void *)(UINTN)JL_CONSOLE_PA,mSnapshot,JL_CONSOLE_BYTES))) {
    finished=TRUE;message=L"BLOCKED. No writes. Preserve capture and photograph this page.";
  }
  jl2_classify(mSnapshot,&available);
  for(;;) {
    Clear();Line(0,L"NATIVE INIT v2 - EXPLICIT HEADER REPLACEMENT / NO ARM");
    UnicodeSPrint(text,sizeof(text),L"DBGC %08x next=%08x size=%08x free=%u need=8192",ring[0],ring[1],ring[2],available);Line(1,text);
    IdLines(3);
    UnicodeSPrint(text,sizeof(text),L"Transfer %016lx  %s",mTransfer,Reason(mInit.reason));Line(8,text);
    UnicodeSPrint(text,sizeof(text),L"Receipt %04x %04x %04x %04x  digit %u/16 = %x",(UINT32)(input>>48),(UINT32)((input>>32)&0xffff),(UINT32)((input>>16)&0xffff),(UINT32)(input&0xffff),MIN(pos+1,16),digit);Line(10,text);
    UnicodeSPrint(text,sizeof(text),L"Confirm %u/2 ready=%u used=%u consent=%u",confirm,mInit.ready,mInit.used,consent);Line(11,text);
    Line(13,message);
    Line(16,L"Digits: volume selects hex, Power accepts. Long power exits with no further write.");
    Line(17,L"After receipt: Vol UP then Power for confirm 1; Vol DOWN then Power for confirm 2.");
    Line(18,L"Wrong direction cancels all input. Repeated Power alone can never confirm.");
    Line(20,L"Writes only bytes 0..271; complete prior snapshot must already exist on host.");
    Line(21,L"Success is NOT ARMED. Photograph result, then use Read. No automatic launch/reset.");
    s=Key(&key);if(EFI_ERROR(s))return s;
    if(key.ScanCode==SCAN_ESC)return EFI_SUCCESS;
    if(finished)continue;
    if(key.ScanCode==SCAN_UP || key.ScanCode==SCAN_DOWN) {
      if(pos<16) digit=(digit+(key.ScanCode==SCAN_UP?1:15))%16;
      else if(key.ScanCode==(confirm?SCAN_DOWN:SCAN_UP)) consent=TRUE;
      else {input=0;pos=0;digit=0;confirm=0;consent=FALSE;message=L"Cancelled. Re-enter receipt. No writes.";}
      continue;
    }
    if(key.UnicodeChar!=L'\r')continue;
    if(pos<16) {
      input=(input<<4)|digit;digit=0;pos++;
      if(pos==16) {
        if(input!=mInit.record.receipt) {finished=TRUE;message=L"WRONG RECEIPT. No writes. Exit and start a new complete export.";}
        else message=L"Receipt matches. Tap Volume UP, then Power for confirmation 1.";
        s=Drain();if(EFI_ERROR(s))return s;
      }
      continue;
    }
    if(!consent)continue;
    consent=FALSE;confirm++;s=Drain();if(EFI_ERROR(s))return s;
    if(confirm==1) {message=L"FINAL: tap Volume DOWN, then Power to replace header. Long power cancels.";continue;}
    oldTpl=gBS->RaiseTPL(TPL_HIGH_LEVEL);
    ok=jl2_commit(&mInit,(volatile VOID *)(UINTN)JL_CONSOLE_PA,mSnapshot,input,confirm,Barrier);
    gBS->RestoreTPL(oldTpl);finished=TRUE;
    message=ok?L"INIT READBACK OK: next=size=00000104. NOT ARMED. Photograph then Read.":
      L"INIT FAILED / SNAPSHOT CHANGED. STOP. No retry, ARM or launch.";
  }
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *system) {
  EFI_STATUS s;EFI_INPUT_KEY key;EFI_EVENT timer,events[2];UINTN event;BOOLEAN initialize=FALSE;
  (void)image;(void)system;s=Panel();if(EFI_ERROR(s))return s;
  if(!jl_freeze((const volatile void *)(UINTN)JL_CONSOLE_PA,mSnapshot,Barrier) ||
     !jl_same((const volatile void *)(UINTN)JL_CONSOLE_PA,mSnapshot,JL_CONSOLE_BYTES))return EFI_ABORTED;
  jl_sha256(mSnapshot,JL_CONSOLE_BYTES,mDigest);jl_hex_bytes(mHash,mDigest,32);mHash[64]=0;
  jl_hex_bytes(mProfileText,mProfile,32);mProfileText[64]=0;mCrc=jl_crc(mSnapshot,JL_CONSOLE_BYTES);
  __asm__ volatile("isb; mrs %0, cntvct_el0":"=r"(mTransfer)::"memory");
  if(!mTransfer)return EFI_ABORTED;
  s=Drain();if(EFI_ERROR(s))return s;
  s=gBS->CreateEvent(EVT_TIMER,TPL_APPLICATION,NULL,NULL,&timer);if(EFI_ERROR(s))return s;
  events[0]=gST->ConIn->WaitForKey;events[1]=timer;
  for(;;) {
    s=DrawExport();if(EFI_ERROR(s))break;
    s=gBS->SetTimer(timer,mPlaying?TimerRelative:TimerCancel,mIntervals[mGear]*10000ULL);if(EFI_ERROR(s))break;
    s=gBS->WaitForEvent(mPlaying?2:1,events,&event);if(EFI_ERROR(s))break;
    if(event==1) {mIndex=(mIndex+1)%256;continue;}
    s=gST->ConIn->ReadKeyStroke(gST->ConIn,&key);if(EFI_ERROR(s)) {if(s==EFI_NOT_READY)continue;break;}
    if(key.ScanCode==SCAN_ESC) {s=EFI_SUCCESS;break;}
    if(key.UnicodeChar==L'\r')mPlaying=!mPlaying;
    if(key.ScanCode==SCAN_UP || key.ScanCode==SCAN_DOWN) {
      if(mPlaying)mGear=(mGear+(key.ScanCode==SCAN_UP?3:1))%4;
      else mIndex=(mIndex+(key.ScanCode==SCAN_UP?255:1))%256;
    }
  }
  gBS->SetTimer(timer,TimerCancel,0);gBS->CloseEvent(timer);
  if(EFI_ERROR(s))return s;
  s=ExportOptions(&initialize);if(EFI_ERROR(s) || !initialize)return s;
  return Initialize();
}
