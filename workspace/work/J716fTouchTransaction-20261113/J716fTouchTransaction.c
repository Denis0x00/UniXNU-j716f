/* SPDX-License-Identifier: BSD-2-Clause */
#include "J716fTouchTransaction.h"

enum { PHASE_IDLE=0, PHASE_PREFIX, PHASE_TAIL, PHASE_ACK, PHASE_DISCARD_ACK };
static int bus_kind(JTActionKind k) {
 return k==JT_READ_PREFIX || k==JT_READ_TAIL || k==JT_ACK || k==JT_BUS_FENCE;
}
static int sink_kind(JTActionKind k) { return k==JT_PUBLISH || k==JT_SINK_FENCE; }
static int duration_ok(uint32_t n) { return n && n<=JT_MAX_TIMEOUT_MS; }
static void count_up(uint32_t *n) { if(*n!=UINT32_MAX)++*n; }
static void settle(JTState *s) {
 if(s->lifecycle==JT_STOPPING && s->current.kind==JT_ACTION_NONE &&
    !s->bus_dirty && !s->sink_dirty && !s->release_needed && !s->sink_unknown &&
    !s->delivered.active_mask && !s->batch.reports && s->phase==PHASE_IDLE)
  s->lifecycle=JT_QUIESCENT;
}
static void fault(JTState *s,JTResult why,JGResult packet,int invalidate) {
 JGBatch ignored;
 if(invalidate && s->current.kind!=JT_ACTION_NONE) {
  if(bus_kind(s->current.kind))s->bus_dirty=1;
  if(sink_kind(s->current.kind)) {s->sink_dirty=1;s->sink_unknown=1;}
  s->current=(JTAction){0};
 }
 (void)jg_fault(&s->core,packet,&ignored);
 s->batch=(JGBatch){0};s->batch_index=0;s->irq_pending=0;
 if(s->delivered.active_mask || s->sink_unknown)s->release_needed=1;
 s->last_fault=why;s->last_packet=packet;
}
/* Monotonic deadlines use checked addition at dispatch. Stale events are
 * rejected before this function, so old callbacks cannot move the clock. */
static JTResult advance(JTState *s,uint64_t now) {
 if(now<s->last_ms) {
  fault(s,JT_CLOCK,JG_CLOCK,1);
  /* A changed clock domain must not silently keep the old epoch running.
   * Cleanup is still driven explicitly, followed by resume with a new epoch. */
  s->lifecycle=JT_STOPPING;s->phase=PHASE_IDLE;
  s->last_ms=now;s->last_progress_ms=now;s->silence_latched=1;
  settle(s);
  return JT_CLOCK;
 }
 s->last_ms=now;
 if(s->current.kind!=JT_ACTION_NONE && now>=s->current.deadline_ms) {
  int was_bus=bus_kind(s->current.kind) || s->phase!=PHASE_IDLE;
  fault(s,JT_TIMEOUT,JG_TIMEOUT,1);
  s->phase=(uint8_t)(was_bus && s->lifecycle==JT_RUNNING ? PHASE_DISCARD_ACK : PHASE_IDLE);
  return JT_TIMEOUT;
 }
 if(s->lifecycle==JT_RUNNING && !s->silence_latched &&
    now-s->last_progress_ms>=s->config.silence_timeout_ms) {
  int was_bus=bus_kind(s->current.kind) || s->phase!=PHASE_IDLE;
  fault(s,JT_TIMEOUT,JG_TIMEOUT,1);
  s->phase=(uint8_t)(was_bus ? PHASE_DISCARD_ACK : PHASE_IDLE);
  s->silence_latched=1;
  return JT_TIMEOUT;
 }
 return JT_OK;
}
JTResult jt_init(JTState *s,uint64_t epoch,uint64_t now,const JTConfig *c) {
 JTConfig saved;
 if(!s || !c || !epoch || !duration_ok(c->bus_timeout_ms) ||
    !duration_ok(c->sink_timeout_ms) || !duration_ok(c->fence_timeout_ms) ||
    !duration_ok(c->silence_timeout_ms))return JT_BAD_ARGUMENT;
 saved=*c;*s=(JTState){0};s->config=saved;s->epoch=epoch;s->last_ms=now;
 s->last_progress_ms=now;s->lifecycle=JT_RUNNING;jg_init(&s->core,epoch);
 return JT_OK;
}
JTResult jt_signal_frame(JTState *s,uint64_t epoch,uint64_t now) {
 JTResult r;
 if(!s)return JT_BAD_ARGUMENT;
 if(epoch!=s->epoch)return JT_STALE;
 if(s->lifecycle!=JT_RUNNING)return JT_STOPPED;
 r=advance(s,now);if(r!=JT_OK)return r;
 s->irq_pending=1;return JT_OK;
}
static JTResult issue(JTState *s,uint64_t now,JTActionKind kind,JTAction *out) {
 uint32_t timeout=kind==JT_BUS_FENCE || kind==JT_SINK_FENCE ? s->config.fence_timeout_ms :
                  kind==JT_PUBLISH ? s->config.sink_timeout_ms : s->config.bus_timeout_ms;
 JTAction a={0};
 if(now>UINT64_MAX-timeout) {
  fault(s,JT_TIME_RANGE,JG_CLOCK,0);s->phase=PHASE_IDLE;s->lifecycle=JT_STOPPING;
  settle(s);return JT_TIME_RANGE;
 }
 if(s->last_token==UINT64_MAX) {
  fault(s,JT_TOKEN_EXHAUSTED,JG_STOPPED,0);s->phase=PHASE_IDLE;s->lifecycle=JT_STOPPING;
  settle(s);return JT_TOKEN_EXHAUSTED;
 }
 a.kind=kind;a.epoch=s->epoch;a.token=++s->last_token;a.deadline_ms=now+timeout;
 if(bus_kind(kind) && kind!=JT_BUS_FENCE)a.i2c_address=JT_I2C_ADDRESS;
 if(kind==JT_READ_PREFIX) {a.reg=JT_COORDINATE_REGISTER;a.length=JT_PREFIX_BYTES;s->frame_token=a.token;}
 else if(kind==JT_READ_TAIL) {a.reg=JT_COORDINATE_REGISTER+JT_PREFIX_BYTES;a.length=(uint16_t)(s->acquired_bytes-JT_PREFIX_BYTES);}
 else if(kind==JT_ACK) {a.reg=JT_COORDINATE_REGISTER;a.length=2;}
 else if(kind==JT_PUBLISH && !s->release_needed)a.snapshot=s->batch.report[s->batch_index];
 s->current=a;*out=a;return JT_OK;
}
JTResult jt_next(JTState *s,uint64_t epoch,uint64_t now,JTAction *out) {
 JTResult r;JTActionKind kind=JT_ACTION_NONE;
 if(!s || !out)return JT_BAD_ARGUMENT;
 *out=(JTAction){0};
 if(epoch!=s->epoch)return JT_STALE;
 r=advance(s,now);if(r!=JT_OK)return r;
 if(s->current.kind!=JT_ACTION_NONE)return JT_BUSY;
 /* Finish acquisition/ACK without waiting on a sink. A quarantined bus can
  * never use this path; its published contacts may be released before fence. */
 if(!s->bus_dirty && s->phase!=PHASE_IDLE) {
  kind=s->phase==PHASE_PREFIX ? JT_READ_PREFIX : s->phase==PHASE_TAIL ? JT_READ_TAIL : JT_ACK;
 } else if(s->sink_dirty)kind=JT_SINK_FENCE;
 else if(s->release_needed)kind=JT_PUBLISH;
 else if(s->batch_index<s->batch.reports)kind=JT_PUBLISH;
 else if(s->bus_dirty)kind=JT_BUS_FENCE;
 else {
  settle(s);
  if(s->lifecycle!=JT_RUNNING)return JT_STOPPED;
  if(s->irq_pending) {s->irq_pending=0;s->phase=PHASE_PREFIX;kind=JT_READ_PREFIX;}
 }
 if(kind==JT_ACTION_NONE)return JT_IDLE;
 return issue(s,now,kind,out);
}
static void discard_packet(JTState *s,JGResult packet) {
 fault(s,JT_PROTOCOL_ERROR,packet,0);
 s->phase=PHASE_DISCARD_ACK;
}
static void submit_packet(JTState *s,uint64_t now) {
 JGBatch ignored;
 JGResult r=jg_submit(&s->core,s->epoch,s->frame_token,now,s->bytes,s->acquired_bytes,&ignored);
 s->last_packet=r;
 if(r==JG_OK)s->phase=PHASE_ACK;
 else discard_packet(s,r);
}
static int payload_empty(const JTCompletion *c) { return c->length==0; }
JTResult jt_complete(JTState *s,uint64_t now,const JTCompletion *c) {
 JTAction a;JTResult r;
 if(!s || !c)return JT_BAD_ARGUMENT;
 if(c->epoch!=s->epoch || !c->token || c->token!=s->current.token ||
    c->kind!=s->current.kind || c->kind==JT_ACTION_NONE)return JT_STALE;
 r=advance(s,now);if(r!=JT_OK)return r;
 a=s->current;s->current=(JTAction){0};
 if(a.kind==JT_BUS_FENCE) {
  if(!c->success || !payload_empty(c) || (c->evidence&JT_BUS_FENCE_EVIDENCE)!=JT_BUS_FENCE_EVIDENCE)
   {s->bus_dirty=1;s->last_fault=JT_FENCE_REQUIRED;return JT_FENCE_REQUIRED;}
  s->bus_dirty=0;settle(s);return JT_OK;
 }
 if(a.kind==JT_SINK_FENCE) {
  if(!c->success || !payload_empty(c) || (c->evidence&JT_SINK_FENCE_EVIDENCE)!=JT_SINK_FENCE_EVIDENCE)
   {s->sink_dirty=1;s->last_fault=JT_FENCE_REQUIRED;return JT_FENCE_REQUIRED;}
  s->sink_dirty=0;s->release_needed=1;return JT_OK;
 }
 if(a.kind==JT_PUBLISH) {
  if(!c->success || !payload_empty(c) || !(c->evidence&JT_E_SINK_APPLIED_ORDERED)) {
   s->sink_dirty=1;s->sink_unknown=1;fault(s,JT_SINK_ERROR,JG_ACK_FAILED,0);
   s->phase=PHASE_IDLE;return JT_SINK_ERROR;
  }
  s->delivered=a.snapshot;
  if(s->release_needed) {s->release_needed=0;s->sink_unknown=0;}
  else if(++s->batch_index>=s->batch.reports) {s->batch=(JGBatch){0};s->batch_index=0;}
  settle(s);return JT_OK;
 }
 if(!c->success || !(c->evidence&JT_E_IO_TERMINAL) ||
    ((a.kind==JT_READ_PREFIX || a.kind==JT_READ_TAIL) && (!c->bytes || c->length!=a.length)) ||
    (a.kind==JT_ACK && !payload_empty(c))) {
  s->bus_dirty=1;fault(s,JT_IO_ERROR,JG_ACK_FAILED,0);
  s->phase=(uint8_t)(s->lifecycle==JT_RUNNING ? PHASE_DISCARD_ACK : PHASE_IDLE);
  return JT_IO_ERROR;
 }
 if(a.kind==JT_READ_PREFIX) {
  JGPlan plan;JGResult packet;
  for(unsigned i=0;i<JT_PREFIX_BYTES;++i)s->bytes[i]=c->bytes[i];
  packet=jg_inspect(s->bytes,JT_PREFIX_BYTES,&plan);
  if(packet!=JG_OK)discard_packet(s,packet);
  else {
   s->acquired_bytes=plan.acquired_bytes;
   if(plan.tail_bytes)s->phase=PHASE_TAIL;else submit_packet(s,now);
  }
 } else if(a.kind==JT_READ_TAIL) {
  for(unsigned i=0;i<a.length;++i)s->bytes[JT_PREFIX_BYTES+i]=c->bytes[i];
  submit_packet(s,now);
 } else if(a.kind==JT_ACK) {
  if(s->phase==PHASE_ACK) {
   JGResult packet=jg_finish(&s->core,s->epoch,s->frame_token,1,&s->batch);
   if(packet!=JG_OK) {discard_packet(s,packet);s->phase=PHASE_IDLE;return JT_PROTOCOL_ERROR;}
   s->batch_index=0;s->last_progress_ms=now;s->silence_latched=0;
   s->last_packet=JG_OK;count_up(&s->packets_acked);
  } else count_up(&s->packets_discarded);
  s->phase=PHASE_IDLE;
 }
 settle(s);return JT_OK;
}
JTResult jt_tick(JTState *s,uint64_t epoch,uint64_t now) {
 if(!s)return JT_BAD_ARGUMENT;
 if(epoch!=s->epoch)return JT_STALE;
 return advance(s,now);
}
JTResult jt_stop(JTState *s,uint64_t epoch,uint64_t now) {
 JTResult r;JGBatch ignored;
 if(!s)return JT_BAD_ARGUMENT;
 if(epoch!=s->epoch)return JT_STALE;
 if(s->lifecycle==JT_QUIESCENT)return JT_OK;
 if(s->lifecycle==JT_STOPPING)return advance(s,now);
 r=advance(s,now);
 s->lifecycle=JT_STOPPING;
 fault(s,JT_STOPPED,JG_STOPPED,1);jg_stop(&s->core,&ignored);
 s->phase=PHASE_IDLE;settle(s);
 return r;
}
JTResult jt_resume(JTState *s,uint64_t epoch,uint64_t now) {
 JTConfig config;
 if(!s || !epoch)return JT_BAD_ARGUMENT;
 if(epoch<=s->epoch)return JT_STALE;
 if(s->lifecycle!=JT_QUIESCENT || s->current.kind!=JT_ACTION_NONE ||
    s->bus_dirty || s->sink_dirty || s->release_needed || s->sink_unknown ||
    s->delivered.active_mask)return JT_NOT_QUIESCENT;
 config=s->config;return jt_init(s,epoch,now,&config);
}
