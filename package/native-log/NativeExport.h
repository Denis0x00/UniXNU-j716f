// SPDX-License-Identifier: BSD-2-Clause-Patent
// RAW export and separate explicit initialization; does not change kernel ABI.
#ifndef J716F_NATIVE_EXPORT_H
#define J716F_NATIVE_EXPORT_H
#include "NativeFirmware.h"
#include "NativeSha256.h"
#define JL_EXPORT_CHUNK 1024U
#define JL_EXPORT_FRAMES (JL_CONSOLE_BYTES/JL_EXPORT_CHUNK)
#define JL_EXPORT_TEXT 2231U
#define JL_INIT_MAGIC 0x3154494e4936314aULL // J16INIT1, little endian
#define JL_INIT_OFFSET 16U
#define JL_INIT_BYTES 256U
#define JL_INIT_END (JL_INIT_OFFSET+JL_INIT_BYTES-12U)
static inline void jl_hex(char *out,JL64 value,JL32 digits) {
  static const char hex[]="0123456789ABCDEF";JL32 i;
  for(i=0;i<digits;i++) out[i]=hex[(value>>((digits-1-i)*4))&15U];
}
static inline void jl_hex_bytes(char *out,const JL8 *p,JL32 n) {
  JL32 i;for(i=0;i<n;i++) jl_hex(out+2*i,p[i],2);
}
static inline int jl_same(const volatile void *a,const volatile void *b,JL32 n) {
  const volatile JL8 *x=a,*y=b;JL32 i;
  for(i=0;i<n;i++) if(x[i]!=y[i]) return 0;
  return 1;
}
// J716F1/candidate/SHA256/transfer/consoleCRC/index/count/RAW-hex/frameCRC
// Every frame is fixed length, upper-case QR alphanumeric. Includes ALL bytes,
// regardless of DBGC validity, wrap, stale sessions or otherwise uninterpreted data.
static inline JL32 jl_export_frame(char out[JL_EXPORT_TEXT],const void *snapshot,
    const JL64 candidate[4],const JL8 digest[32],JL64 transfer,JL32 crc,JL32 index) {
  JL32 at=0,i;const JL8 *p=snapshot;static const char prefix[]="J716F1/";
  if(index>=JL_EXPORT_FRAMES || !transfer) return 0;
  for(i=0;i<7;i++) out[at++]=prefix[i];
  for(i=0;i<32;i++) jl_hex(out+at+2*i,(candidate[i/8]>>((i%8)*8))&255,2);
  at+=64;out[at++]='/';jl_hex_bytes(out+at,digest,32);at+=64;out[at++]='/';
  jl_hex(out+at,transfer,16);at+=16;out[at++]='/';
  jl_hex(out+at,crc,8);at+=8;out[at++]='/';
  jl_hex(out+at,index,4);at+=4;out[at++]='/';
  jl_hex(out+at,JL_EXPORT_FRAMES,4);at+=4;out[at++]='/';
  jl_hex_bytes(out+at,p+index*JL_EXPORT_CHUNK,JL_EXPORT_CHUNK);
  at+=2*JL_EXPORT_CHUNK;out[at++]='/';
  jl_hex(out+at,jl_crc(out,at),8);at+=8;out[at]=0;return at;
}
// A human receipt for this EXACT byte snapshot and candidate. Deliberately not
// authentication: cannot prove an external disk's durability or resist forgery.
// Same bytes yield the same receipt; a receipt for changed bytes is not reusable.
static inline JL64 jl_export_receipt(const JL64 candidate[4],const JL8 digest[32]) {
  static const JL8 domain[]="J716F-INIT-RECEIPT-V1";
  JL8 input[sizeof(domain)-1+64],hash[32];JL32 i;JL64 result=0;
  for(i=0;i<sizeof(domain)-1;i++) input[i]=domain[i];
  for(i=0;i<32;i++) input[sizeof(domain)-1+i]=(JL8)(candidate[i/8]>>((i%8)*8));
  for(i=0;i<32;i++) input[sizeof(domain)-1+32+i]=digest[i];
  jl_sha256(input,sizeof(input),hash);
  for(i=0;i<8;i++) result|=(JL64)hash[i]<<(8*i);
  return result;
}
typedef struct {
  JL64 magic;JL32 version,bytes;JL64 candidate[4];JL8 backup_sha256[32];
  JL32 old_header[3],backup_crc;JL64 receipt,counter;JL32 flags,reserved0,crc,commit;
  JL8 reserved[128];
} JLInitRecord;
typedef char JLInitSize[(sizeof(JLInitRecord)==JL_INIT_BYTES)?1:-1];
typedef char JLInitCrc[(__builtin_offsetof(JLInitRecord,crc)==120)?1:-1];
typedef struct { JLInitRecord record;JL32 ready,used; } JLInitGate;
// Refuse a recognizable unconsumed ARM even outside the current linear range,
// for ANY candidate. Export remains available; initialization is not an override.
static inline int jl_raw_armed(const void *snapshot) {
  const JL8 *p=snapshot;const JLHeader *h;JL32 off,i;
  for(off=16;off<=JL_CONSOLE_BYTES-JL_SESSION_BYTES;off+=8) {
    h=(const JLHeader *)(p+off);
    if(h->magic!=JL_MAGIC || h->version!=1 || h->bytes!=JL_SESSION_BYTES ||
       h->offset!=off || !h->session || h->state!=JL_ARMED || h->crc!=jl_crc(h,60)) continue;
    for(i=0;i<sizeof(h->reserved);i++) if(h->reserved[i]) break;
    if(i==sizeof(h->reserved)) return 1;
  }
  return 0;
}
static inline int jl_init_prepare(JLInitGate *gate,const void *snapshot,
    const JL64 candidate[4],JL64 counter,int stable) {
  const JL32 *ring=snapshot;JL32 i;JL8 *p=(JL8 *)&gate->record;
  if(gate->used) return 0;
  gate->ready=0;
  // Narrow first implementation: only a well-formed, FULL wrapped DBGC ring.
  // Invalid headers and even full linear logs are NOT silently repaired.
  if(!stable || !counter || ring[0]!=0x43474244U || ring[2]!=JL_CAPACITY ||
      ring[1]>=JL_CAPACITY || jl_raw_armed(snapshot)) return 0;
  for(i=0;i<sizeof(gate->record);i++) p[i]=0;
  gate->record.magic=JL_INIT_MAGIC;gate->record.version=1;gate->record.bytes=JL_INIT_BYTES;
  for(i=0;i<4;i++) gate->record.candidate[i]=candidate[i];
  jl_sha256(snapshot,JL_CONSOLE_BYTES,gate->record.backup_sha256);
  for(i=0;i<3;i++) gate->record.old_header[i]=ring[i];
  gate->record.backup_crc=jl_crc(snapshot,JL_CONSOLE_BYTES);
  gate->record.receipt=jl_export_receipt(candidate,gate->record.backup_sha256);
  if(!gate->record.receipt) return 0;
  gate->record.counter=counter;gate->record.flags=1; // host receipt required
  gate->record.crc=jl_crc(&gate->record,120);gate->record.commit=JL_COMMIT;
  gate->ready=1;return 1;
}
// Caller masks firmware events around commit (RaiseTPL) and performs no I/O in
// this critical section. Barriers are not protection from DMA/secure writers;
// physical ownership and actual recovery still need board qualification.
static inline int jl_init_commit(JLInitGate *gate,volatile void *live,const void *snapshot,
    JL64 receipt,JL32 confirmations,JLBarrier barrier) {
  volatile JL32 *ring=live;volatile JL8 *p=live;const JL8 *r=(const JL8 *)&gate->record;
  JL8 digest[32];JL32 i;
  if(!gate->ready || gate->used || confirmations!=2 || receipt!=gate->record.receipt) return 0;
  gate->used=1;gate->ready=0; // failure is terminal for this invocation, no retry
  jl_sha256(snapshot,JL_CONSOLE_BYTES,digest);
  if(!jl_same(digest,gate->record.backup_sha256,32) ||
      gate->record.crc!=jl_crc(&gate->record,120) ||
      !jl_same(live,snapshot,JL_CONSOLE_BYTES)) return 0;
  barrier();
  if(!jl_same(live,snapshot,JL_CONSOLE_BYTES)) return 0;
  ring[0]=0;barrier(); // invalidate before ANY payload/header replacement
  for(i=12;i<16;i++) p[i]=0;
  for(i=0;i<JL_INIT_BYTES;i++) p[JL_INIT_OFFSET+i]=r[i];
  ring[1]=JL_INIT_END;ring[2]=JL_INIT_END;barrier();
  // Commit signature LAST. Interruption before this cannot admit a kernel.
  ring[0]=0x43474244U;barrier();
  return ring[0]==0x43474244U && ring[1]==JL_INIT_END && ring[2]==JL_INIT_END &&
    jl_same(p+JL_INIT_OFFSET,r,JL_INIT_BYTES) &&
    jl_same(p+JL_INIT_OFFSET+JL_INIT_BYTES,(const JL8 *)snapshot+JL_INIT_OFFSET+JL_INIT_BYTES,
      JL_CONSOLE_BYTES-JL_INIT_OFFSET-JL_INIT_BYTES);
}
#endif
