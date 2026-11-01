// SPDX-License-Identifier: BSD-2-Clause-Patent
// Explicit read-only Query, then double-confirm ARM. No reset and no ring repair.
#include "Common.h"
VOID J716fSmc(UINT64 *args);
STATIC JLArmGate mGate;
STATIC BOOLEAN mConfirm;
STATIC UINTN mSelected;
STATIC UINT64 mVersion,mFeature;
STATIC volatile JLHeader *mArmed;
STATIC CONST CHAR16 *mResult=L"Read-only on entry. Capture Native READ before doing anything here.";
STATIC CONST CHAR16 *mOptions[]={L"Query PSCI version/features (read only)",L"ARM unused linear tail (confirm twice)",L"Return to menu"};
STATIC volatile VOID *mLive=(volatile VOID *)(UINTN)JL_CONSOLE_PA;
STATIC VOID Query(VOID) {
  UINT64 args[8],counter;
  mConfirm=FALSE;
  if(mGate.used || CurrentEl()!=4) {mResult=L"Blocked: used attempt or wrong EL. No SMC/writes.";return;}
  ZeroMem(args,sizeof(args));args[0]=0x84000000U;J716fSmc(args);mVersion=args[0];
  ZeroMem(args,sizeof(args));args[0]=0x8400000aU;args[1]=0x84000000U;J716fSmc(args);mFeature=args[0];
  __asm__ volatile("isb; mrs %0, cntvct_el0":"=r"(counter)::"memory");
  mResult=jl_arm_query(&mGate,mLive,mCandidate,CurrentEl(),mVersion,mFeature,counter)?
    L"Query OK, no RAM write. Select ARM; two confirmations required.":
    L"Query blocked: version/feature/space/header/stale ARM gate. No repair or clear.";
}
STATIC VOID Arm(VOID) {
  if(!mGate.ready || mGate.used) {mConfirm=FALSE;mResult=L"ARM blocked: fresh Query required. No writes.";return;}
  if(!mConfirm) {mConfirm=TRUE;mResult=L"Confirm ARM again only after photographing READ. This app NEVER resets.";return;}
  mConfirm=FALSE;mArmed=jl_arm_commit(&mGate,mLive,mCandidate,Barrier);
  mResult=mArmed?L"ARM readback OK. Photograph ID/CRC, return to menu, launch matching native candidate ONCE.":
    L"ARM FAILED. Stop; changed console or readback failure. Do not launch.";
}
STATIC VOID Render(VOID) {
  CHAR16 text[160];UINTN i;CONST volatile JL32 *ring=mLive;
  Clear();Line(0,L"J716F NATIVE ARM v2 - manual only / NO RESET / NO CLEAR");
  UnicodeSPrint(text,sizeof(text),L"DBGC %08x next=%08x size=%08x EL%u",ring[0],ring[1],ring[2],(UINT32)(CurrentEl()>>2));Line(1,text);
  UnicodeSPrint(text,sizeof(text),L"Candidate words %016lx %016lx",mCandidate[0],mCandidate[1]);Line(2,text);
  UnicodeSPrint(text,sizeof(text),L"                %016lx %016lx",mCandidate[2],mCandidate[3]);Line(3,text);
  UnicodeSPrint(text,sizeof(text),L"PSCI=%016lx feature(VERSION)=%016lx query=%u used=%u",mVersion,mFeature,mGate.ready,mGate.used);Line(5,text);
  UnicodeSPrint(text,sizeof(text),L"Session=%016lx headerCRC=%08x armed=%u",mGate.session,mArmed?mArmed->crc:0,mArmed!=NULL);Line(6,text);
  UnicodeSPrint(text,sizeof(text),L"Reserved offset=%08x bytes=%08x. Capture together with candidate and session.",mArmed?mArmed->offset:0,mArmed?mArmed->bytes:0);Line(7,text);
  for(i=0;i<ARRAY_SIZE(mOptions);++i) {UnicodeSPrint(text,sizeof(text),L"%s %s",i==mSelected?L">":L" ",mOptions[i]);Line(9+i,text);}
  Line(14,mResult);Line(17,L"Up/Down: select. Enter: execute. Esc: return without further writes.");
  Line(19,L"Native kernel consumes ARM once, then runs without artificial stage resets.");
  Line(20,L"After any restart, Native READ FIRST. Do not open ARM before evidence capture.");
}
EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *system) {
  EFI_STATUS s;EFI_INPUT_KEY key;(void)image;(void)system;
  s=Panel();if(EFI_ERROR(s)) return s;
  for(;;) {
    Render();s=Key(&key);if(EFI_ERROR(s)) continue;
    if(key.ScanCode==SCAN_ESC) return EFI_SUCCESS;
    if(key.ScanCode==SCAN_UP || key.ScanCode==SCAN_DOWN) {mConfirm=FALSE;mSelected=(mSelected+(key.ScanCode==SCAN_UP?2:1))%3;}
    if(key.UnicodeChar!=L'\r') continue;
    if(mSelected==0) Query();else if(mSelected==1) Arm();else return EFI_SUCCESS;
  }
}
