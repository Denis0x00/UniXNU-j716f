// SPDX-License-Identifier: BSD-2-Clause-Patent
// Firmware-only maintenance ABI. NativeLog.h and the selected kernel stay fixed.
#ifndef J716F_NATIVE_MAINTENANCE_H
#define J716F_NATIVE_MAINTENANCE_H
#include "NativeExport.h"
#define JL2_TEXT (JL_EXPORT_TEXT+65U)
#define JL2_INIT_MAGIC 0x3254494e4936314aULL
enum { JL2_INVALID=0,JL2_FULL_WRAPPED,JL2_FULL_LINEAR,JL2_EXHAUSTED,JL2_SPACE,
       JL2_PENDING,JL2_SUSPECT,JL2_UNSTABLE,JL2_IDENTITY,JL2_USED };
typedef struct {
  JL64 magic;JL32 version,bytes;JL64 candidate[4];JL8 backup_sha256[32],profile[32];
  JL64 transfer;JL32 old_header[3],backup_crc;JL64 receipt;JL32 classification;
  JL8 reserved[100];JL32 crc,commit;
} JL2InitRecord;
typedef char JL2Size[(sizeof(JL2InitRecord)==256)?1:-1];
typedef char JL2CrcOffset[(__builtin_offsetof(JL2InitRecord,crc)==248)?1:-1];
typedef struct { JL2InitRecord record;JL32 ready,used,reason; } JL2Gate;
static inline int jl2_nonzero(const JL8 *p,JL32 n) {
  JL32 i;for(i=0;i<n;i++) if(p[i]) return 1;return 0;
}
static inline JL32 jl2_classify(const void *snapshot,JL32 *available) {
  const JL32 *r=snapshot;JL32 offset;
  *available=0;
  if(r[0]!=0x43474244U || r[2]>JL_CAPACITY || r[1]>JL_CAPACITY) return JL2_INVALID;
  if(r[2]==JL_CAPACITY) return r[1]==JL_CAPACITY?JL2_FULL_LINEAR:JL2_FULL_WRAPPED;
  if(r[1]!=r[2]) return JL2_INVALID;
  offset=(12U+r[1]+7U)&~7U;*available=JL_CONSOLE_BYTES-offset;
  return *available<JL_SESSION_BYTES?JL2_EXHAUSTED:JL2_SPACE;
}
// Inspect ALL aligned physical positions, including beyond logical length and
// incomplete headers near the end. Recognizable corrupt/unknown sessions block.
static inline JL32 jl2_sessions(const void *snapshot) {
  const JL8 *p=snapshot;const JLHeader *h;JL32 off;
  for(off=16;off<=JL_CONSOLE_BYTES-8;off+=8) {
    if(*(const JL64 *)(p+off)!=JL_MAGIC) continue;
    if(off>JL_CONSOLE_BYTES-JL_SESSION_BYTES) return JL2_SUSPECT;
    h=(const JLHeader *)(p+off);
    if(h->version!=1 || h->bytes!=JL_SESSION_BYTES || h->offset!=off || !h->session ||
       !jl2_nonzero((const JL8 *)h->candidate,32) || h->crc!=jl_crc(h,60) ||
       jl2_nonzero(h->reserved,sizeof(h->reserved)) ||
       (h->state!=JL_ARMED && h->state!=JL_USED)) return JL2_SUSPECT;
    if(h->state==JL_ARMED) return JL2_PENDING;
  }
  return 0;
}
static inline JL64 jl2_receipt(const JL64 candidate[4],const JL8 digest[32],
    const JL8 profile[32],JL64 transfer) {
  static const JL8 domain[]="J716F-INIT-RECEIPT-V2";
  JL8 input[sizeof(domain)-1+104],hash[32];JL32 i,at=sizeof(domain)-1;JL64 value=0;
  for(i=0;i<at;i++) input[i]=domain[i];
  for(i=0;i<32;i++) input[at++]=profile[i];
  for(i=0;i<32;i++) input[at++]=(JL8)(candidate[i/8]>>((i%8)*8));
  for(i=0;i<32;i++) input[at++]=digest[i];
  for(i=0;i<8;i++) input[at++]=(JL8)(transfer>>(8*i));
  jl_sha256(input,sizeof(input),hash);
  for(i=0;i<8;i++) value|=(JL64)hash[i]<<(8*i);
  return value;
}
// J716F2/profile/candidate/SHA256/transfer/consoleCRC/index/count/raw/frameCRC.
static inline JL32 jl2_frame(char out[JL2_TEXT],const void *snapshot,
    const JL64 candidate[4],const JL8 digest[32],const JL8 profile[32],
    JL64 transfer,JL32 crc,JL32 index) {
  JL32 at=0,i;const JL8 *p=snapshot;static const char prefix[]="J716F2/";
  if(!transfer || index>=JL_EXPORT_FRAMES || !jl2_nonzero(profile,32)) return 0;
  for(i=0;i<7;i++) out[at++]=prefix[i];
  jl_hex_bytes(out+at,profile,32);at+=64;out[at++]='/';
  for(i=0;i<32;i++) jl_hex(out+at+2*i,(candidate[i/8]>>((i%8)*8))&255,2);
  at+=64;out[at++]='/';jl_hex_bytes(out+at,digest,32);at+=64;out[at++]='/';
  jl_hex(out+at,transfer,16);at+=16;out[at++]='/';jl_hex(out+at,crc,8);at+=8;out[at++]='/';
  jl_hex(out+at,index,4);at+=4;out[at++]='/';jl_hex(out+at,JL_EXPORT_FRAMES,4);at+=4;out[at++]='/';
  jl_hex_bytes(out+at,p+index*JL_EXPORT_CHUNK,JL_EXPORT_CHUNK);at+=2*JL_EXPORT_CHUNK;out[at++]='/';
  jl_hex(out+at,jl_crc(out,at),8);at+=8;out[at]=0;return at;
}
static inline int jl2_prepare(JL2Gate *gate,const void *snapshot,
    const JL64 candidate[4],const JL8 profile[32],JL64 transfer,int stable) {
  JL32 i,available,kind;JL8 *p=(JL8 *)&gate->record;const JL32 *r=snapshot;
  gate->ready=0;
  if(gate->used) {gate->reason=JL2_USED;return 0;}
  if(!stable) {gate->reason=JL2_UNSTABLE;return 0;}
  if(!transfer || !jl2_nonzero(profile,32) || !jl2_nonzero((const JL8 *)candidate,32)) {
    gate->reason=JL2_IDENTITY;return 0;
  }
  kind=jl2_classify(snapshot,&available);gate->reason=kind;
  if(kind==JL2_INVALID || kind==JL2_SPACE) return 0;
  i=jl2_sessions(snapshot);if(i) {gate->reason=i;return 0;}
  for(i=0;i<sizeof(gate->record);i++) p[i]=0;
  gate->record.magic=JL2_INIT_MAGIC;gate->record.version=2;gate->record.bytes=256;
  for(i=0;i<4;i++) gate->record.candidate[i]=candidate[i];
  for(i=0;i<32;i++) gate->record.profile[i]=profile[i];
  jl_sha256(snapshot,JL_CONSOLE_BYTES,gate->record.backup_sha256);
  gate->record.transfer=transfer;
  for(i=0;i<3;i++) gate->record.old_header[i]=r[i];
  gate->record.backup_crc=jl_crc(snapshot,JL_CONSOLE_BYTES);
  gate->record.receipt=jl2_receipt(candidate,gate->record.backup_sha256,profile,transfer);
  if(!gate->record.receipt) {gate->reason=JL2_IDENTITY;return 0;}
  gate->record.classification=kind;gate->record.crc=jl_crc(&gate->record,248);
  gate->record.commit=JL_COMMIT;gate->ready=1;return 1;
}
static inline int jl2_record_valid(const void *snapshot,const JL64 candidate[4]) {
  const JL32 *r=snapshot;const JL2InitRecord *h=(const JL2InitRecord *)((const JL8 *)snapshot+16);
  JL32 unused;
  if(r[0]!=0x43474244U || r[1]!=r[2] || r[2]<JL_INIT_END || r[2]>JL_CAPACITY ||
     h->magic!=JL2_INIT_MAGIC || h->version!=2 || h->bytes!=256 || !h->transfer ||
     !jl_same(candidate,h->candidate,32) || !jl2_nonzero(h->profile,32) ||
     jl2_nonzero(h->reserved,sizeof(h->reserved)) || h->crc!=jl_crc(h,248) || h->commit!=JL_COMMIT ||
     h->classification<JL2_FULL_WRAPPED || h->classification>JL2_EXHAUSTED ||
     jl2_classify(h->old_header,&unused)!=h->classification) return 0;
  return h->receipt && h->receipt==jl2_receipt(h->candidate,h->backup_sha256,h->profile,h->transfer);
}
// No allocation/I/O/automatic ARM. Signature is committed last; only bytes 0..271
// may change. Caller masks firmware events; DMA and reset retention remain unproven.
static inline int jl2_commit(JL2Gate *gate,volatile void *live,const void *snapshot,
    JL64 receipt,JL32 confirmations,JLBarrier barrier) {
  volatile JL32 *ring=live;volatile JL8 *p=live;const JL8 *record=(const JL8 *)&gate->record;
  JL8 digest[32];JL32 i,available;
  if(!gate->ready || gate->used || confirmations!=2 || receipt!=gate->record.receipt) return 0;
  gate->used=1;gate->ready=0;
  jl_sha256(snapshot,JL_CONSOLE_BYTES,digest);
  if(!jl_same(digest,gate->record.backup_sha256,32) || gate->record.crc!=jl_crc(record,248) ||
     gate->record.commit!=JL_COMMIT || jl2_sessions(snapshot) ||
     jl2_classify(snapshot,&available)!=gate->record.classification ||
     !jl_same(live,snapshot,JL_CONSOLE_BYTES)) return 0;
  barrier();if(!jl_same(live,snapshot,JL_CONSOLE_BYTES)) return 0;
  ring[0]=0;barrier();
  for(i=12;i<16;i++) p[i]=0;
  for(i=0;i<256;i++) p[16+i]=record[i];
  ring[1]=JL_INIT_END;ring[2]=JL_INIT_END;barrier();ring[0]=0x43474244U;barrier();
  return ring[0]==0x43474244U && ring[1]==JL_INIT_END && ring[2]==JL_INIT_END &&
    jl_same(p+16,record,256) && jl_same(p+272,(const JL8 *)snapshot+272,JL_CONSOLE_BYTES-272);
}
#endif
