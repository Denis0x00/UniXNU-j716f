/* SPDX-License-Identifier: BSD-2-Clause */
/* Synthetic protocol fixtures, not captured hardware packets. This compiles
 * the actual transaction and frozen packet-core sources without a fake bus. */
#include "../../work/J716fTouchTransaction-20261113/J716fTouchTransaction.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned checks;
static uint64_t now;
static void check(int yes,const char *name) {
 ++checks;if(!yes) {fprintf(stderr,"FAIL %u: %s\n",checks,name);exit(1);}
}
static void init(JTState *s) {
 JTConfig c={100,80,100,1000};now=100;
 check(jt_init(s,7,now,&c)==JT_OK,"initialize owner");
}
static size_t frame(uint8_t *b,unsigned count,unsigned first) {
 unsigned wire=12+(count>1 ? (count-1)*8 : 0);uint8_t sum=0;
 memset(b,0,JG_MAX_FRAME);b[0]=0x80;b[1]=(uint8_t)count;
 for(unsigned i=0;i<count;++i) {
  uint8_t *p=b+2+8*i;p[0]=(uint8_t)(first+i);p[1]=10;p[3]=20;p[5]=1;
 }
 for(unsigned i=1;i<wire;++i)sum=(uint8_t)(sum+b[i]);
 b[wire-1]=(uint8_t)(b[wire-1]-sum);return wire<16?16:wire;
}
static JTAction next(JTState *s,JTActionKind kind) {
 JTAction a;
 check(jt_next(s,s->epoch,++now,&a)==JT_OK,"next action emitted");
 check(a.kind==kind && a.epoch==s->epoch && a.token && a.deadline_ms>now,"expected finite action");
 return a;
}
static JTResult complete(JTState *s,JTAction a,int success,uint32_t proof,const uint8_t *b,size_t n) {
 JTCompletion c={a.kind,a.epoch,a.token,success,proof,b,n};
 return jt_complete(s,++now,&c);
}
static void io_done(JTState *s,JTAction a,const uint8_t *b,size_t n) {
 check(complete(s,a,1,JT_E_IO_TERMINAL,b,n)==JT_OK,"terminal I2C completion");
}
static void publish_done(JTState *s,JTAction a) {
 check(complete(s,a,1,JT_E_SINK_APPLIED_ORDERED,NULL,0)==JT_OK,"ordered full-state publication");
}
static void fence_done(JTState *s,JTActionKind kind) {
 JTAction a=next(s,kind);
 check(complete(s,a,1,kind==JT_BUS_FENCE?JT_BUS_FENCE_EVIDENCE:JT_SINK_FENCE_EVIDENCE,NULL,0)==JT_OK,"explicit fence proof");
}
static void ack_done(JTState *s) {
 JTAction a=next(s,JT_ACK);
 check(a.i2c_address==0x5d && a.reg==0x824d && a.length==2 && !a.write[0] && !a.write[1],"stock two-byte zero ACK");
 io_done(s,a,NULL,0);
}
static void feed(JTState *s,unsigned count,unsigned first) {
 uint8_t b[JG_MAX_FRAME];size_t n=frame(b,count,first);JTAction a;
 check(jt_signal_frame(s,s->epoch,++now)==JT_OK,"frame signal");
 a=next(s,JT_READ_PREFIX);
 check(a.reg==0x824d && a.length==16 && a.i2c_address==0x5d,"stock prefix address and length");
 io_done(s,a,b,16);
 if(n>16) {
  a=next(s,JT_READ_TAIL);
  check(a.reg==0x825d && a.length==n-16,"bounded stock tail extent");
  io_done(s,a,b+16,n-16);
 }
 ack_done(s);
}
static unsigned publish_all(JTState *s) {
 unsigned n=0;JTAction a;
 while(s->batch.reports || s->release_needed) {
  check(++n<=3,"bounded publication batch");a=next(s,JT_PUBLISH);publish_done(s,a);
 }
 check(jt_next(s,s->epoch,++now,&a)==JT_IDLE,"idle after publication");return n;
}
static void arm(JTState *s) {feed(s,0,0);check(s->core.armed,"ACKed zero arms");check(publish_all(s)==0,"initial zero not published");}
static void active(JTState *s,unsigned count,unsigned first) {feed(s,count,first);check(publish_all(s)==1,"single normal report");}
static void release(JTState *s) {JTAction a=next(s,JT_PUBLISH);check(!a.snapshot.active_mask,"full all-up release");publish_done(s,a);}
static void test_normal(void) {
 JTState s;JTAction a,unchanged;JTCompletion c;uint8_t b[JG_MAX_FRAME];size_t n;
 init(&s);check(jt_next(&s,7,++now,&a)==JT_IDLE,"fresh owner waits for signal");
 feed(&s,1,1);check(!s.core.armed && s.packets_discarded==1,"held startup frame ACKed and discarded");
 check(publish_all(&s)==0,"held startup does not publish");arm(&s);
 for(unsigned count=1;count<=10;++count) {
  feed(&s,count,1);check(s.core.current.active_mask==(1u<<count)-1,"all ten counts committed after ACK");
  check(publish_all(&s)==1,"count growth has one report");
 }
 feed(&s,10,30);a=next(&s,JT_PUBLISH);
 check(!a.snapshot.active_mask,"replacement reports old release first");publish_done(&s,a);
 a=next(&s,JT_PUBLISH);check(a.snapshot.active_mask==1023 && a.snapshot.slots[0].raw_id==30,"replacement second report reuses slots");
 publish_done(&s,a);check(s.delivered.active_mask==1023,"publication tracked independently");
 n=frame(b,1,50);check(n==16,"single frame extent");
 check(jt_signal_frame(&s,7,++now)==JT_OK,"signal before immutable payload test");a=next(&s,JT_READ_PREFIX);
 check(jt_next(&s,7,++now,&unchanged)==JT_BUSY && unchanged.kind==JT_ACTION_NONE,"never emits action twice");
 c=(JTCompletion){a.kind,6,a.token,1,JT_E_IO_TERMINAL,b,16};
 check(jt_complete(&s,0,&c)==JT_STALE && s.current.token==a.token,"stale epoch cannot rewind clock");
 c.epoch=7;c.token++;check(jt_complete(&s,0,&c)==JT_STALE,"future token rejected");
 c.token=a.token;c.kind=JT_ACK;check(jt_complete(&s,0,&c)==JT_STALE,"wrong action kind rejected");
 io_done(&s,a,b,16);memset(b,0,sizeof b);
 check(s.core.current.slots[0].raw_id==30 && s.core.pending,"sensor not committed before ACK");
 ack_done(&s);check(s.core.current.slots[0].raw_id==50,"payload copied before callback return");
 check(publish_all(&s)==2,"reuse sequence preserved after copied input");
 c=(JTCompletion){a.kind,7,a.token,1,JT_E_IO_TERMINAL,NULL,0};
 check(jt_complete(&s,0,&c)==JT_STALE && s.delivered.slots[0].raw_id==50,"duplicate completion cannot fault current stream");
}
static void test_bad_packets(void) {
 JTState s;JTAction a;uint8_t b[JG_MAX_FRAME];
 for(unsigned variant=0;variant<7;++variant) {
  init(&s);arm(&s);active(&s,1,1);(void)frame(b,1,2);
  if(variant==0)b[3]^=1; /* checksum */
  if(variant==1)b[1]=11; /* count */
  if(variant==2)b[1]=0x21; /* pen */
  if(variant==3)b[0]=0x20; /* non-touch event */
  if(variant==4) {b[2]=0x80;b[11]=(uint8_t)(b[11]-0x7e);} /* ID + fixed checksum */
  if(variant==5) {b[3]=0xff;b[4]=0xff;b[11]=(uint8_t)(b[11]-0xf4);} /* coordinate */
  if(variant==6) {b[1]=0x80;/* low-nibble zero, actual clean-zero accepted */}
  check(jt_signal_frame(&s,7,++now)==JT_OK,"bad frame signal");a=next(&s,JT_READ_PREFIX);io_done(&s,a,b,16);
  ack_done(&s);
  if(variant==6)check(s.core.armed,"zero count follows stock no checksum rule");
  else check(!s.core.armed && s.packets_discarded==1,"bad/unsupported frame ACKed without commit");
  release(&s);check(!s.delivered.active_mask,"bad-frame final release delivered");
  if(variant!=6) {
   feed(&s,1,9);check(!s.core.armed && !s.batch.reports,"held after fault discarded");
   feed(&s,0,0);check(s.core.armed,"clean-zero ACK rearms after malformed frame");
  }
 }
 /* Invalid length is a transport error, not a packet ACK permission. */
 for(unsigned n=0;n<18;++n)if(n!=16) {
  init(&s);check(jt_signal_frame(&s,7,++now)==JT_OK,"short transfer signal");a=next(&s,JT_READ_PREFIX);
  check(complete(&s,a,1,JT_E_IO_TERMINAL,b,n)==JT_IO_ERROR,"nonexact prefix length quarantines bus");
  a=next(&s,JT_BUS_FENCE);
  check(complete(&s,a,1,JT_BUS_FENCE_EVIDENCE,NULL,0)==JT_OK,"short transfer drain proof");ack_done(&s);
 }
}
static void test_bus_failure(void) {
 JTState s;JTAction a,stale;uint8_t b[JG_MAX_FRAME];JTCompletion c;
 for(unsigned step=0;step<3;++step)for(unsigned mode=0;mode<3;++mode) {
  init(&s);arm(&s);active(&s,1,1);(void)frame(b,2,10);
  check(jt_signal_frame(&s,7,++now)==JT_OK,"bus failure frame signal");a=next(&s,JT_READ_PREFIX);
  if(step>=1) {io_done(&s,a,b,16);a=next(&s,JT_READ_TAIL);}
  if(step>=2) {io_done(&s,a,b+16,4);a=next(&s,JT_ACK);}
  stale=a;
  if(mode==0)check(complete(&s,a,0,0,NULL,0)==JT_IO_ERROR,"failed I2C operation quarantined");
  if(mode==1) {
   now=a.deadline_ms;check(jt_tick(&s,7,now)==JT_TIMEOUT,"finite deadline expires");
  }
  if(mode==2)check(complete(&s,a,1,0,b,a.length)==JT_IO_ERROR,"success without terminal evidence rejected");
  check(s.bus_dirty && !s.core.armed && s.release_needed,"bus fault disarms and queues release");
  c=(JTCompletion){stale.kind,7,stale.token,1,JT_E_IO_TERMINAL,b,stale.length};
  check(jt_complete(&s,0,&c)==JT_STALE && s.bus_dirty,"late success cannot clear bus quarantine");
  release(&s);a=next(&s,JT_BUS_FENCE);
  check(complete(&s,a,1,JT_E_BUS_QUEUE_DRAINED,NULL,0)==JT_FENCE_REQUIRED,"partial bus proof cannot permit reuse");
  check(s.bus_dirty,"insufficient bus fence remains quarantined");fence_done(&s,JT_BUS_FENCE);
  ack_done(&s);check(s.packets_discarded==1,"one recovered discard ACK after actual fence");
  feed(&s,1,40);check(!s.core.armed,"postfault held frame remains disarmed");feed(&s,0,0);check(s.core.armed,"clean-zero after quarantine rearms");
 }
 /* Every individual bus-fence evidence bit is mandatory. */
 for(unsigned bit=1;bit<=4;++bit) {
  init(&s);check(jt_signal_frame(&s,7,++now)==JT_OK,"fence bit signal");a=next(&s,JT_READ_PREFIX);
  check(complete(&s,a,0,0,NULL,0)==JT_IO_ERROR,"fence bit failure");a=next(&s,JT_BUS_FENCE);
  check(complete(&s,a,1,JT_BUS_FENCE_EVIDENCE&~(1u<<bit),NULL,0)==JT_FENCE_REQUIRED,"each omitted bus proof rejected");
 }
}
static void test_sink_failure(void) {
 JTState s;JTAction a,stale;JTCompletion c;
 for(unsigned which=0;which<2;++which)for(unsigned mode=0;mode<3;++mode) {
  init(&s);arm(&s);active(&s,10,1);feed(&s,10,30);a=next(&s,JT_PUBLISH);
  if(which) {check(!a.snapshot.active_mask,"first release in partial delivery");publish_done(&s,a);a=next(&s,JT_PUBLISH);}
  stale=a;
  if(mode==0)check(complete(&s,a,0,0,NULL,0)==JT_SINK_ERROR,"sink failure enters uncertainty");
  if(mode==1) {now=a.deadline_ms;check(jt_tick(&s,7,now)==JT_TIMEOUT,"sink timeout enters uncertainty");}
  if(mode==2)check(complete(&s,a,1,0,NULL,0)==JT_SINK_ERROR,"sink success needs ordered application proof");
  check(s.sink_dirty && s.sink_unknown && s.release_needed && !s.core.armed && !s.batch.reports,"partial batch cannot continue after failure");
  c=(JTCompletion){stale.kind,7,stale.token,1,JT_E_SINK_APPLIED_ORDERED,NULL,0};
  check(jt_complete(&s,0,&c)==JT_STALE && s.sink_dirty,"late report cannot clear sink uncertainty");
  a=next(&s,JT_SINK_FENCE);
  check(complete(&s,a,1,JT_E_SINK_QUEUE_DRAINED,NULL,0)==JT_FENCE_REQUIRED,"sink queue-only proof insufficient");
  fence_done(&s,JT_SINK_FENCE);release(&s);
  check(!s.sink_dirty && !s.sink_unknown && !s.delivered.active_mask,"full release after sink fence clears delivered state");
  feed(&s,1,50);check(!s.core.armed,"sink-fault held frame discarded");feed(&s,0,0);check(s.core.armed,"sink-fault recovery requires clean-zero");
 }
 /* A failed final release remains a cleanup obligation even if previous known
  * state was already empty; the failed report might have been applied late. */
 init(&s);arm(&s);active(&s,1,1);check(jt_stop(&s,7,++now)==JT_OK,"stop active stream");
 a=next(&s,JT_PUBLISH);check(complete(&s,a,0,0,NULL,0)==JT_SINK_ERROR,"failed stop all-up retained");
 check(jt_resume(&s,8,++now)==JT_NOT_QUIESCENT,"resume cannot bypass release failure");
 fence_done(&s,JT_SINK_FENCE);release(&s);check(s.lifecycle==JT_QUIESCENT,"final release completes stopped lifecycle");
}
static void test_lifecycle(void) {
 JTState s;JTAction a,stale;JTCompletion c;uint8_t b[JG_MAX_FRAME];
 for(unsigned pending=0;pending<4;++pending) {
  init(&s);arm(&s);active(&s,1,1);(void)frame(b,2,20);
  if(pending<3) {
   check(jt_signal_frame(&s,7,++now)==JT_OK,"stop pending signal");a=next(&s,JT_READ_PREFIX);
   if(pending>0) {io_done(&s,a,b,16);a=next(&s,JT_READ_TAIL);}
   if(pending>1) {io_done(&s,a,b+16,4);a=next(&s,JT_ACK);}
  } else {feed(&s,1,1);a=next(&s,JT_PUBLISH);}
  stale=a;check(jt_stop(&s,7,++now)==JT_OK,"stop invalidates pending operation");
  check(jt_resume(&s,8,now)==JT_NOT_QUIESCENT,"resume denied with cleanup pending");
  c=(JTCompletion){stale.kind,7,stale.token,1,JT_E_IO_TERMINAL,b,stale.length};
  check(jt_complete(&s,0,&c)==JT_STALE,"stopped old completion rejected");
  if(pending==3)fence_done(&s,JT_SINK_FENCE);
  a=next(&s,JT_PUBLISH);
  check(jt_stop(&s,7,++now)==JT_OK && s.current.token==a.token,"repeated stop preserves cleanup action");
  publish_done(&s,a);
  if(pending<3)fence_done(&s,JT_BUS_FENCE);
  check(s.lifecycle==JT_QUIESCENT && !s.delivered.active_mask,"quiescent means fenced and all-up");
  check(jt_signal_frame(&s,7,++now)==JT_STOPPED,"stopped IRQ ignored");
  check(jt_next(&s,7,++now,&a)==JT_STOPPED,"no work after quiescence");
  check(jt_resume(&s,7,now)==JT_STALE,"epoch reuse refused");
  check(jt_resume(&s,8,10)==JT_OK && s.epoch==8 && !s.core.armed,"new epoch may have new clock domain");
  now=10;check(jt_complete(&s,0,&c)==JT_STALE,"old epoch cannot affect resumed owner");
  feed(&s,1,50);check(!s.core.armed,"resume requires ACKed clean-zero");feed(&s,0,0);check(s.core.armed,"resume zero arms");
 }
 init(&s);check(jt_stop(&s,7,++now)==JT_OK && s.lifecycle==JT_QUIESCENT,"idle stop is immediately quiescent");
 check(jt_stop(&s,6,0)==JT_STALE,"stale stop harmless");
}
static void test_silence_and_clock(void) {
 JTState s;JTAction a,stale;uint8_t b[JG_MAX_FRAME];
 init(&s);arm(&s);active(&s,1,1);
 now=s.last_progress_ms+s.config.silence_timeout_ms;
 check(jt_tick(&s,7,now)==JT_TIMEOUT && s.release_needed && !s.core.armed,"silent stream releases contacts without IRQ");
 release(&s);check(jt_tick(&s,7,++now)==JT_OK,"silence episode releases once");
 check(jt_next(&s,7,++now,&a)==JT_IDLE,"no repeated all-up after silent release");
 feed(&s,1,30);check(!s.core.armed,"silence held frame discarded");feed(&s,0,0);check(s.core.armed,"silence clean-zero rearm");
 for(unsigned kind=0;kind<3;++kind) {
  init(&s);arm(&s);active(&s,1,1);
  if(kind==1) {check(jt_signal_frame(&s,7,++now)==JT_OK,"clock bus signal");a=next(&s,JT_READ_PREFIX);}
  if(kind==2) {feed(&s,1,1);a=next(&s,JT_PUBLISH);}
  now=1;check(jt_tick(&s,7,now)==JT_CLOCK && !s.core.armed,"large clock reversal faults owner");
  if(kind==2)fence_done(&s,JT_SINK_FENCE);
  release(&s);
  if(kind==1)fence_done(&s,JT_BUS_FENCE);
  check(s.lifecycle==JT_QUIESCENT,"clock reversal requires stopped cleanup before new epoch");
  check(jt_resume(&s,8,10)==JT_OK,"clock reversal recovers only through new epoch");
  now=10;feed(&s,0,0);check(s.core.armed,"new-clock clean-zero ACK rearms without old clock catch-up");
 }
 /* Stream deadline can precede the operation deadline; no outstanding
  * operation is implicitly cancelled by the stream timeout. */
 init(&s);s.config.silence_timeout_ms=20;
 check(jt_signal_frame(&s,7,++now)==JT_OK,"short stream signal");stale=next(&s,JT_READ_PREFIX);
 now=120;check(jt_tick(&s,7,now)==JT_TIMEOUT && s.bus_dirty,"silence quarantines live bus");
 fence_done(&s,JT_BUS_FENCE);ack_done(&s);
 (void)stale;
 /* Acquisition between callbacks still owns a sensor event. */
 init(&s);s.config.silence_timeout_ms=20;(void)frame(b,2,1);
 check(jt_signal_frame(&s,7,++now)==JT_OK,"between-callback signal");a=next(&s,JT_READ_PREFIX);io_done(&s,a,b,16);
 now=120;check(jt_tick(&s,7,now)==JT_TIMEOUT && !s.bus_dirty,"between-callback stream fault has no live I2C");
 ack_done(&s);check(s.packets_discarded==1,"between-callback pending sensor event discarded");
}
static void test_arguments_and_overflow(void) {
 JTState s,saved;JTAction a;JTConfig c={100,80,100,1000};JTCompletion done={0};
 check(jt_init(NULL,1,0,&c)==JT_BAD_ARGUMENT,"null init");
 check(jt_init(&s,0,0,&c)==JT_BAD_ARGUMENT,"nonzero epoch required");
 check(jt_init(&s,1,0,NULL)==JT_BAD_ARGUMENT,"configuration required");
 c.bus_timeout_ms=0;check(jt_init(&s,1,0,&c)==JT_BAD_ARGUMENT,"zero infinite deadline forbidden");
 c.bus_timeout_ms=60001;check(jt_init(&s,1,0,&c)==JT_BAD_ARGUMENT,"unbounded deadline forbidden");
 init(&s);saved=s;
 check(jt_signal_frame(&s,6,0)==JT_STALE && !memcmp(&s,&saved,sizeof s),"old signal has no effect");
 check(jt_tick(&s,6,UINT64_MAX)==JT_STALE && !memcmp(&s,&saved,sizeof s),"old timer has no effect");
 check(jt_next(&s,6,0,&a)==JT_STALE && !memcmp(&s,&saved,sizeof s),"old dispatch has no effect");
 check(jt_complete(NULL,0,&done)==JT_BAD_ARGUMENT && jt_complete(&s,0,NULL)==JT_BAD_ARGUMENT,"null completion rejected");
 check(jt_next(&s,7,now,NULL)==JT_BAD_ARGUMENT,"null action rejected");
 check(jt_init(&s,8,now,&s.config)==JT_OK && s.config.bus_timeout_ms==100,"init safely copies embedded config before resetting state");
 now=UINT64_MAX-50;c=(JTConfig){100,80,100,1000};
 check(jt_init(&s,7,now,&c)==JT_OK,"large monotonic baseline accepted");
 check(jt_signal_frame(&s,7,now)==JT_OK,"large timestamp signal");
 check(jt_next(&s,7,now,&a)==JT_TIME_RANGE && a.kind==JT_ACTION_NONE,"deadline overflow emits no operation");
 init(&s);s.last_token=UINT64_MAX;check(jt_signal_frame(&s,7,++now)==JT_OK,"exhaustion signal");
 check(jt_next(&s,7,++now,&a)==JT_TOKEN_EXHAUSTED && a.kind==JT_ACTION_NONE,"tokens never wrap or repeat");
}
int main(void) {
 test_normal();test_bad_packets();test_bus_failure();test_sink_failure();
 test_lifecycle();test_silence_and_clock();test_arguments_and_overflow();
 printf("PASS %u transaction checks\n",checks);return 0;
}
