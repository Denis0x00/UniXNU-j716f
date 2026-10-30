// SPDX-License-Identifier: BSD-2-Clause-Patent
// No ARM action, SMC, ResetSystem, file write or retained-memory store.
#include "Common.h"
#include "NativeMaintenance.h"
STATIC JL64 mFrozen[JL_CONSOLE_BYTES/8];
STATIC UINT32 mPresent[JL_SLOTS],mCount,mPage,mConsoleCrc,mSessionCrc;
STATIC volatile JLHeader *mHeader;
STATIC BOOLEAN mStable;
STATIC UINT32 mOffsets[JL_MAX_SESSIONS],mSessions,mSelected;

STATIC VOID SelectSession(VOID) {
  UINT32 slot,i;CONST JL64 *raw;BOOLEAN present;
  mCount=0;mPage=0;
  mHeader=(volatile JLHeader *)((JL8 *)mFrozen+mOffsets[mSelected]);
  mSessionCrc=jl_crc(mHeader,JL_SESSION_BYTES);
  for(slot=0;slot<JL_SLOTS;++slot) {
    raw=(CONST JL64 *)(UINTN)jl_record(mHeader,slot);present=FALSE;
    for(i=0;i<16;++i) if(raw[i]) present=TRUE;
    if(present) mPresent[mCount++]=slot;
  }
}

STATIC VOID Render(VOID) {
  CHAR16 text[160];UINTN i;CONST JL64 *raw;UINT32 slot=0;
  CONST JL32 *ring=(CONST JL32 *)mFrozen;
  Clear();Line(0,L"J716F NATIVE READ v3 - FROZEN / READ ONLY / NO RESET");
  UnicodeSPrint(text,sizeof(text),L"DBGC %08x next=%08x size=%08x consoleCRC=%08x STABLE=%u",ring[0],ring[1],ring[2],mConsoleCrc,mStable);Line(1,text);
  UnicodeSPrint(text,sizeof(text),L"Expected candidate words %016lx %016lx",mCandidate[0],mCandidate[1]);Line(2,text);
  UnicodeSPrint(text,sizeof(text),L"                         %016lx %016lx",mCandidate[2],mCandidate[3]);Line(3,text);
  if(!mHeader) {
    if(mStable && jl2_record_valid(mFrozen,mCandidate)) {
      CONST JL2InitRecord *init=(CONST JL2InitRecord *)((CONST JL8 *)mFrozen+16);
      CHAR8 digest[65];jl_hex_bytes(digest,init->backup_sha256,32);digest[64]=0;
      Line(5,L"INIT v2 VALID / NOT ARMED. No native kernel result.");
      UnicodeSPrint(text,sizeof(text),L"Transfer %016lx initCRC=%08x class=%u",init->transfer,init->crc,init->classification);Line(6,text);
      UnicodeSPrint(text,sizeof(text),L"Backup SHA256 %a",digest);Line(8,text);
      jl_hex_bytes(digest,init->profile,32);digest[64]=0;
      UnicodeSPrint(text,sizeof(text),L"Maintenance profile %a",digest);Line(10,text);
      Line(12,L"Photograph before separately reviewing fresh Query/ARM. This reader never writes.");
      Line(14,L"Esc / Return: menu. Initialization does not prove retention or kernel execution.");return;
    }
    Line(5,L"NO VALID MATCHING SESSION. No boot result can be inferred.");
    Line(6,L"Photograph this page. Do not ARM, clear the ring, or launch a kernel.");
    Line(7,L"Esc / Return: return to menu. This app never changes the console.");return;
  }
  UnicodeSPrint(text,sizeof(text),L"Session=%016lx offset=%08x state=%08x sessionCRC=%08x",mHeader->session,mHeader->offset,mHeader->state,mSessionCrc);Line(4,text);
  UnicodeSPrint(text,sizeof(text),L"PAGE %u/%u. Header plus ALL %u nonzero record pages must be captured.",mPage+1,mCount+1,mCount);Line(5,text);
  if(!mPage) {raw=(CONST JL64 *)(UINTN)mHeader;Line(6,L"RAW HEADER: sixteen little-endian uint64 words, in memory order.");}
  else {
    slot=mPresent[mPage-1];raw=(CONST JL64 *)(UINTN)jl_record(mHeader,slot);
    UnicodeSPrint(text,sizeof(text),L"RAW RECORD slot=%u VALID=%u (invalid/torn records are preserved)",slot,jl_valid(mHeader,(CONST volatile JLRecord *)raw,slot));Line(6,text);
  }
  for(i=0;i<16;++i) {
    UnicodeSPrint(text,sizeof(text),L"%s%02u %016lx",mPage?L"R":L"H",i,raw[i]);Line(7+i,text);
  }
  Line(24,L"Volume keys: pages/sessions. Power: next. Long power/Esc: menu.");
  UnicodeSPrint(text,sizeof(text),L"RETAINED SESSION %u/%u. Match ID/CRC to ARM photo; not necessarily this boot.",mSelected+1,mSessions);Line(25,text);
  Line(26,L"No refresh/Query/ARM/reset. Photograph all pages before any writes.");
}

EFI_STATUS EFIAPI UefiMain(EFI_HANDLE image,EFI_SYSTEM_TABLE *system) {
  EFI_STATUS s;EFI_INPUT_KEY key;
  (void)image;(void)system;
  s=Panel();if(EFI_ERROR(s)) return s;
  mStable=jl_freeze((CONST volatile VOID *)(UINTN)JL_CONSOLE_PA,mFrozen,Barrier);
  mConsoleCrc=jl_crc(mFrozen,JL_CONSOLE_BYTES);
  mSessions=mStable?jl_recover_sessions(mFrozen,mCandidate,mOffsets):0;
  if(mSessions) {mSelected=mSessions-1;SelectSession();}
  for(;;) {
    Render();s=Key(&key);if(EFI_ERROR(s)) continue;
    if(key.ScanCode==SCAN_ESC || (!mHeader && key.UnicodeChar==L'\r')) return EFI_SUCCESS;
    if(mSessions && (key.ScanCode==SCAN_LEFT || key.ScanCode==SCAN_RIGHT)) {
      mSelected=(mSelected+(key.ScanCode==SCAN_LEFT?mSessions-1:1))%mSessions;
      SelectSession();
    }
    if(key.ScanCode==SCAN_UP) {
      if(!mPage && mSessions>1) {mSelected=(mSelected+mSessions-1)%mSessions;SelectSession();}
      else mPage=(mPage+mCount)%(mCount+1);
    }
    if(key.ScanCode==SCAN_DOWN || key.UnicodeChar==L'\r') {
      if(mPage==mCount && mSessions>1) {mSelected=(mSelected+1)%mSessions;SelectSession();}
      else mPage=(mPage+1)%(mCount+1);
    }
  }
}
