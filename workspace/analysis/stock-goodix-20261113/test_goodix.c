/* Synthetic byte fixtures derived from the stock ROM handler's audited layout.
 * No captured touch traffic is available; these are not device observations. */
#include "../../work/J716fGoodix-20261113/J716fGoodix.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static void check(int ok,const char *name) { ++checks;if(!ok){fprintf(stderr,"FAIL %s\n",name);exit(1);} }
static size_t make_frame(uint8_t *b,unsigned n,unsigned first) {
 memset(b,0,JG_MAX_FRAME);b[0]=0x80;b[1]=(uint8_t)n;
 for(unsigned i=0;i<n;++i) {
  uint8_t *p=b+2+8*i;p[0]=(uint8_t)(first+i);
  p[1]=0x00;p[2]=0x0a;p[3]=0x40;p[4]=0x06;p[5]=1;
 }
 unsigned wire=12+(n>1 ? 8*(n-1) : 0);
 uint8_t sum=0;
 for(unsigned i=1;i<wire;++i)sum=(uint8_t)(sum+b[i]);
 b[wire-1]=(uint8_t)(b[wire-1]-sum);
 return wire<16?16:wire;
}
static void checksum(uint8_t *b,unsigned n) {
 unsigned wire=12+(n>1?8*(n-1):0);b[wire-1]=0;
 uint8_t sum=0;for(unsigned i=1;i<wire;++i)sum=(uint8_t)(sum+b[i]);
 b[wire-1]=(uint8_t)(0-sum);
}
static JGResult exchange(JGState *s,uint64_t token,uint8_t *b,size_t n,JGBatch *batch) {
 JGResult r=jg_submit(s,s->epoch,token,token,b,n,batch);
 return r==JG_OK?jg_finish(s,s->epoch,token,1,batch):r;
}
int main(void) {
 uint8_t b[JG_MAX_FRAME],copy[JG_MAX_FRAME],pid[4]={'7','3','8','6'},vid[4]={0,0xb1,0,0x23};
 JGFrame f;JGPlan p;JGBatch batch;JGState s;size_t n;
 check(jg_identity_matches(pid,vid,8),"matching observed identity");
 check(!jg_identity_matches(pid,vid,9),"wrong sensor");
 vid[3]^=1;check(!jg_identity_matches(pid,vid,8),"wrong firmware");vid[3]^=1;
 for(unsigned count=0;count<=10;++count) {
  n=make_frame(b,count,1);
  check(jg_inspect(b,2,&p)==JG_OK && p.acquired_bytes==n && p.tail_bytes==n-16,"header plan bounded");
  check(jg_decode(b,n,&f)==JG_OK && f.count==count,"all valid contact counts");
  for(size_t cut=0;cut<n;++cut)check(jg_decode(b,cut,&f)!=JG_OK && !f.count,"every truncation rejected atomically");
  if(count)for(unsigned i=2;i<p.checksum_bytes;++i) {
   memcpy(copy,b,n);copy[i]^=1;
   check(jg_decode(copy,n,&f)==JG_CHECKSUM && !f.count,"every covered data byte corruption");
  }
 }
 n=make_frame(b,1,127);check(jg_decode(b,n,&f)==JG_OK && f.contacts[0].raw_id==127,"raw ID127 retained");
 for(unsigned id=128;id<256;++id) {
  b[2]=(uint8_t)id;checksum(b,1);
  check(jg_decode(b,n,&f)==JG_ID && !f.count,"high-bit IDs rejected");
 }
 n=make_frame(b,2,1);b[10]=1;checksum(b,2);check(jg_decode(b,n,&f)==JG_DUPLICATE,"duplicate IDs");
 n=make_frame(b,1,1);b[3]=1;checksum(b,1);check(jg_decode(b,n,&f)==JG_COORDINATE,"X beyond raw limit");
 n=make_frame(b,1,1);b[5]=0x41;checksum(b,1);check(jg_decode(b,n,&f)==JG_COORDINATE,"Y beyond raw limit");
 n=make_frame(b,1,1);b[7]=b[8]=0;checksum(b,1);check(jg_decode(b,n,&f)==JG_OK && !f.count,"width zero releases");
 n=make_frame(b,1,1);b[12]=0xff;b[15]=0xab;check(jg_decode(b,n,&f)==JG_OK,"unused prefix tail outside checksum");
 n=make_frame(b,0,1);b[3]=0xff;check(jg_decode(b,n,&f)==JG_OK && !f.count,"stock zero count skips checksum");
 for(unsigned count=11;count<=15;++count){b[1]=(uint8_t)count;check(jg_inspect(b,2,&p)==JG_COUNT && !p.acquired_bytes,"invalid count no tail plan");}
 b[1]=0x21;check(jg_decode(b,n,&f)==JG_PEN,"pen explicitly unsupported");
 b[1]=1;for(unsigned status=0;status<128;++status){b[0]=(uint8_t)status;check(jg_decode(b,n,&f)==JG_EVENT,"non-touch events not decoded");}
 check(jg_decode(NULL,16,&f)==JG_SHORT,"null bytes");
 n=make_frame(b,1,1);check(jg_decode(b,n+1,&f)==JG_LENGTH,"oversized frame");
 jg_init(&s,7);
 check(exchange(&s,1,b,n,&batch)==JG_DISARMED && !batch.reports,"held contact at start ignored");
 n=make_frame(b,0,0);check(exchange(&s,2,b,n,&batch)==JG_OK && s.armed && !batch.reports,"ACKed clean release rearms");
 n=make_frame(b,10,1);check(exchange(&s,3,b,n,&batch)==JG_OK && batch.reports==1 && s.current.active_mask==1023,"ten slots active");
 check(jg_finish(&s,7,3,1,&batch)==JG_STALE && !batch.reports,"duplicate completion cannot republish");
 n=make_frame(b,10,20);check(exchange(&s,4,b,n,&batch)==JG_OK && batch.reports==2 && !batch.report[0].active_mask &&
       batch.report[1].active_mask==1023,"full replacement releases before slot reuse");
 n=make_frame(b,10,20);
 for(unsigned i=0;i<5;++i){uint8_t tmp[8];memcpy(tmp,b+2+8*i,8);memcpy(b+2+8*i,b+2+8*(9-i),8);memcpy(b+2+8*(9-i),tmp,8);}
 checksum(b,10);
 check(exchange(&s,5,b,n,&batch)==JG_OK && batch.reports==1 && s.current.slots[0].raw_id==20,"wire order does not change stable slots");
 n=make_frame(b,1,50);check(jg_submit(&s,7,6,6,b,n,&batch)==JG_OK,"stage immutable frame");
 memset(b,0,n);check(jg_submit(&s,7,7,7,b,n,&batch)==JG_BUSY && s.pending_token==6,"busy does not replace pending");
 check(jg_finish(&s,6,6,1,&batch)==JG_STALE && s.current.active_mask==1023,"old epoch leaves active/pending state");
 check(jg_finish(&s,7,6,0,&batch)==JG_ACK_FAILED && batch.reports==1 && !s.current.active_mask && !s.armed,"ACK failure releases once");
 check(jg_finish(&s,7,6,0,&batch)==JG_STALE && !batch.reports,"late ACK failure no repeat release");
 n=make_frame(b,1,60);check(exchange(&s,7,b,n,&batch)==JG_DISARMED,"held after failure cannot rearm");
 n=make_frame(b,0,0);check(exchange(&s,8,b,n,&batch)==JG_OK && s.armed,"clean release recovery");
 n=make_frame(b,1,60);check(exchange(&s,9,b,n,&batch)==JG_OK && s.current.active_mask,"contact after recovery");
 b[3]^=1;check(exchange(&s,10,b,n,&batch)==JG_CHECKSUM && batch.reports==1 && !s.armed,"real corrupted bytes release all");
 check(exchange(&s,11,b,n,&batch)==JG_CHECKSUM && !batch.reports,"repeated corruption no repeated release");
 n=make_frame(b,0,0);check(exchange(&s,12,b,n,&batch)==JG_OK,"second clean recovery");
 n=make_frame(b,1,62);check(exchange(&s,13,b,n,&batch)==JG_OK,"active before clock reversal");
 check(jg_submit(&s,7,14,1,b,n,&batch)==JG_CLOCK && batch.reports==1 && !s.armed,"clock reversal releases");
 n=make_frame(b,0,0);check(exchange(&s,15,b,n,&batch)==JG_OK,"recover after clock catches up");
 n=make_frame(b,1,63);check(exchange(&s,16,b,n,&batch)==JG_OK,"active before stop");
 jg_stop(&s,&batch);check(batch.reports==1 && !batch.report[0].active_mask,"stop release");
 jg_stop(&s,&batch);check(!batch.reports,"stop idempotent");
 check(exchange(&s,17,b,n,&batch)==JG_STOPPED,"stopped rejects input");
 jg_init(&s,8);check(jg_finish(&s,7,16,1,&batch)==JG_STALE,"old callback after new epoch");
 n=make_frame(b,0,0);check(exchange(&s,1,b,n,&batch)==JG_OK,"timeout scenario rearm");
 n=make_frame(b,1,42);check(exchange(&s,2,b,n,&batch)==JG_OK,"timeout scenario contact");
 n=make_frame(b,1,43);check(jg_submit(&s,8,3,3,b,n,&batch)==JG_OK,"pending without ACK");
 check(jg_expire(&s,7,3,&batch)==JG_STALE && s.pending && s.current.active_mask,"old generation timeout harmless");
 check(jg_expire(&s,8,2,&batch)==JG_STALE && s.pending && s.current.active_mask,"old token timeout harmless");
 check(jg_expire(&s,8,3,&batch)==JG_TIMEOUT && batch.reports==1 && !s.armed && !s.pending,"bounded owner timeout releases");
 check(jg_expire(&s,8,3,&batch)==JG_STALE && !batch.reports,"duplicate timeout no second release");
 check(jg_finish(&s,8,3,1,&batch)==JG_STALE && !batch.reports,"late ACK after timeout rejected");
 check(exchange(&s,4,b,n,&batch)==JG_DISARMED,"held after timeout not rearmed");
 n=make_frame(b,0,0);check(exchange(&s,5,b,n,&batch)==JG_OK && s.armed,"clean zero after timeout rearms");
 n=make_frame(b,1,44);check(jg_submit(&s,8,6,6,b,n,&batch)==JG_OK,"immutable success stage");
 memset(b,0,n);
 check(jg_finish(&s,8,6,1,&batch)==JG_OK && s.current.slots[0].raw_id==44,"caller byte buffer cannot alter pending frame");
 /* Deterministic arbitrary-byte coverage with guarded source bounds. */
 uint32_t random=1;
 for(unsigned iteration=0;iteration<5000;++iteration) {
  size_t length=iteration%(JG_MAX_FRAME+1);
  for(size_t i=0;i<length;++i){random=random*1664525u+1013904223u;b[i]=(uint8_t)(random>>24);}
  JGResult r=jg_decode(b,length,&f);
  check(r==JG_OK?f.count<=10:f.count==0,"random decode remains bounded and atomic");
 }
 printf("PASS %u stock Goodix core checks\n",checks);
 return 0;
}
