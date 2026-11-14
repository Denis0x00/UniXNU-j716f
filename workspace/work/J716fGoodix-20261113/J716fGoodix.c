/* SPDX-License-Identifier: BSD-2-Clause */
#include "J716fGoodix.h"

static void clear_batch(JGBatch *b,JGResult reason) {
 *b=(JGBatch){0}; b->reason=reason;
}
static uint16_t le16(const uint8_t *b) { return (uint16_t)((uint16_t)b[0]|((uint16_t)b[1]<<8)); }
int jg_identity_matches(const uint8_t p[4],const uint8_t v[4],uint8_t s) {
 return p && v && p[0]=='7' && p[1]=='3' && p[2]=='8' && p[3]=='6' &&
        v[0]==0 && v[1]==0xb1 && v[2]==0 && v[3]==0x23 && s==8;
}
JGResult jg_inspect(const uint8_t *b,size_t n,JGPlan *out) {
 JGPlan p={0};
 if (!out) return JG_SHORT;
 *out=p;
 if (!b || n<2) return JG_SHORT;
 if (!(b[0]&0x80)) return JG_EVENT;
 p.count=b[1]&15;
 if (p.count>JG_MAX_CONTACTS) return JG_COUNT;
 if (b[1]&0x20) return JG_PEN;
 p.checksum_bytes=(uint16_t)(12+(p.count>1 ? 8*(p.count-1) : 0));
 p.acquired_bytes=p.checksum_bytes<16 ? 16 : p.checksum_bytes;
 p.tail_bytes=(uint16_t)(p.acquired_bytes-16);
 *out=p;
 return JG_OK;
}
JGResult jg_decode(const uint8_t *b,size_t n,JGFrame *out) {
 JGPlan p; JGFrame f={0}; JGResult r;
 if (!out) return JG_SHORT;
 *out=f;
 r=jg_inspect(b,n,&p); if(r!=JG_OK)return r;
 if(n<p.acquired_bytes)return JG_SHORT;
 if(n!=p.acquired_bytes)return JG_LENGTH;
 if(p.count) {
  uint8_t sum=0;
  for(size_t i=1;i<p.checksum_bytes;++i)sum=(uint8_t)(sum+b[i]);
  if(sum)return JG_CHECKSUM;
 }
 for(unsigned i=0;i<p.count;++i) {
  const uint8_t *q=b+2+8*i;
  JGContact c={q[0],le16(q+1),le16(q+3),le16(q+5)};
  if(c.raw_id>=0x80)return JG_ID;
  for(unsigned j=0;j<i;++j)if(b[2+8*j]==c.raw_id)return JG_DUPLICATE;
  /* Preserve raw axes. Inclusive endpoint accepted; no pixel transform/HID. */
  if(c.x>2560 || c.y>1600)return JG_COORDINATE;
  if(c.width)f.contacts[f.count++]=c;
 }
 *out=f;
 return JG_OK;
}
void jg_init(JGState *s,uint64_t epoch) {
 *s=(JGState){0}; s->epoch=epoch;
}
JGResult jg_fault(JGState *s,JGResult reason,JGBatch *b) {
 clear_batch(b,reason);
 s->pending=0; s->pending_token=0; s->frame=(JGFrame){0}; s->armed=0;
 if(s->current.active_mask) { b->reports=1; s->current=(JGSnapshot){0}; }
 return reason;
}
JGResult jg_submit(JGState *s,uint64_t epoch,uint64_t token,uint64_t now,
                  const uint8_t *bytes,size_t n,JGBatch *b) {
 JGFrame f; JGResult r;
 clear_batch(b,JG_OK);
 if(s->stopped) {b->reason=JG_STOPPED;return JG_STOPPED;}
 /* Stale callbacks never release current contacts or disturb an in-flight frame. */
 if(epoch!=s->epoch || !token || token<=s->last_token) {b->reason=JG_STALE;return JG_STALE;}
 if(s->pending) {b->reason=JG_BUSY;return JG_BUSY;}
 s->last_token=token;
 if(s->clock_set && now<s->last_ms)return jg_fault(s,JG_CLOCK,b);
 s->last_ms=now; s->clock_set=1;
 r=jg_decode(bytes,n,&f);
 if(r!=JG_OK)return jg_fault(s,r,b);
 /* A clean decoded zero frame must be ACKed before startup/fault rearming. */
 if(!s->armed && f.count) {b->reason=JG_DISARMED;return JG_DISARMED;}
 s->frame=f; s->pending_token=token; s->pending=1;
 return JG_OK;
}
JGResult jg_finish(JGState *s,uint64_t epoch,uint64_t token,int ack,JGBatch *b) {
 JGSnapshot retained={0},next={0};
 uint8_t used[JG_MAX_CONTACTS]={0};
 uint16_t removed;
 clear_batch(b,JG_OK);
 if(s->stopped) {b->reason=JG_STOPPED;return JG_STOPPED;}
 if(epoch!=s->epoch || !s->pending || token!=s->pending_token) {b->reason=JG_STALE;return JG_STALE;}
 s->pending=0; s->pending_token=0;
 if(!ack)return jg_fault(s,JG_ACK_FAILED,b);
 if(!s->armed) {s->armed=1;return JG_OK;}
 for(unsigned i=0;i<JG_MAX_CONTACTS;++i)if(s->current.slots[i].active) {
  for(unsigned j=0;j<s->frame.count;++j)if(s->current.slots[i].raw_id==s->frame.contacts[j].raw_id) {
   JGContact c=s->frame.contacts[j];
   retained.slots[i]=(JGSlot){1,c.raw_id,c.x,c.y,c.width};
   retained.active_mask|=(uint16_t)(1u<<i); used[j]=1;break;
  }
 }
 next=retained;
 for(unsigned j=0;j<s->frame.count;++j)if(!used[j]) {
  for(unsigned i=0;i<JG_MAX_CONTACTS;++i)if(!next.slots[i].active) {
   JGContact c=s->frame.contacts[j];
   next.slots[i]=(JGSlot){1,c.raw_id,c.x,c.y,c.width};
   next.active_mask|=(uint16_t)(1u<<i);break;
  }
 }
 removed=(uint16_t)(s->current.active_mask&~retained.active_mask);
 if(removed & next.active_mask) b->report[b->reports++]=retained;
 b->report[b->reports++]=next;
 s->current=next; s->frame=(JGFrame){0};
 return JG_OK;
}
void jg_stop(JGState *s,JGBatch *b) {
 (void)jg_fault(s,JG_STOPPED,b); s->stopped=1;
}
JGResult jg_expire(JGState *s,uint64_t epoch,uint64_t token,JGBatch *b) {
 clear_batch(b,JG_STALE);
 if(s->stopped) {b->reason=JG_STOPPED;return JG_STOPPED;}
 if(epoch!=s->epoch || !s->pending || token!=s->pending_token)return JG_STALE;
 return jg_fault(s,JG_TIMEOUT,b);
}
