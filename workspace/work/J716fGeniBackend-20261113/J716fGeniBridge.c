/* SPDX-License-Identifier: BSD-2-Clause */
#include "J716fGeniBackend.h"

static int same_current(const JTState *s,const JTAction *a)
{
 return s->current.kind==a->kind && s->current.epoch==a->epoch &&
        s->current.token==a->token;
}
static int same_completion(const JTAction *a,const JTCompletion *c)
{
 return a->kind==c->kind && a->epoch==c->epoch && a->token==c->token;
}
JBResult jb_bridge_init(JBBridge *b,JBState *bus,JTState *transaction)
{
 if(!b || !bus || !bus->initialized || !transaction ||
    bus->epoch!=transaction->epoch)return JB_BAD_ARGUMENT;
 *b=(JBBridge){0};b->bus=bus;b->transaction=transaction;return JB_OK;
}
JBResult jb_bridge_step(JBBridge *b,uint64_t epoch,uint64_t now,JTAction *out)
{
 if(!b || !b->bus || !b->transaction || !out)return JB_BAD_ARGUMENT;
 *out=(JTAction){0};
 if(epoch!=b->transaction->epoch || epoch!=b->bus->epoch)return JB_STALE;
 b->last_transaction_result=jt_tick(b->transaction,epoch,now);
 JBResult result=jb_poll(b->bus,now);
 JTCompletion completion;
 if(jb_take_completion(b->bus,&completion))
  b->last_transaction_result=jt_complete(b->transaction,now,&completion);
 /* A core timeout invalidates the old value token, not the physical work.
  * The latter continues in bus cancel/abort/quarantine without a new command. */
 if(b->external_pending && !same_current(b->transaction,&b->external)){
  b->external_pending=0;b->external_emitted=0;
 }
 if(b->external_pending){
  if(!b->external_emitted &&
     (b->external.kind!=JT_BUS_FENCE || jb_fence_ready(b->bus,0))){
   *out=b->external;b->external_emitted=1;
  }
  return result;
 }
 JTAction action;
 JTResult next=jt_next(b->transaction,epoch,now,&action);
 b->last_transaction_result=next;
 if(next!=JT_OK)return result;
 switch(action.kind){
 case JT_READ_PREFIX:case JT_READ_TAIL:case JT_ACK:{
  JBResult started=jb_start(b->bus,&action,now);
  if(started!=JB_OK){
   /* No false IO_TERMINAL even when rejection occurred before a new write. */
   JTCompletion failed={action.kind,action.epoch,action.token,0,0,0,0};
   b->last_transaction_result=jt_complete(b->transaction,now,&failed);
  }
  return started;
 }
 case JT_BUS_FENCE:
  result=jb_request_fence(b->bus,&action,now);
  b->external=action;b->external_pending=1;b->external_emitted=0;
  if(jb_fence_ready(b->bus,0)){*out=action;b->external_emitted=1;}
  return result;
 case JT_PUBLISH:case JT_SINK_FENCE:
  b->external=action;b->external_pending=1;b->external_emitted=1;
  *out=action;return JB_NEEDS_EXTERNAL_FENCE;
 default:return JB_BAD_ARGUMENT;
 }
}
JTResult jb_bridge_complete_external(JBBridge *b,uint64_t now,const JTCompletion *c)
{
 if(!b || !b->bus || !b->transaction || !c)return JT_BAD_ARGUMENT;
 if(!b->external_pending || !b->external_emitted ||
    !same_completion(&b->external,c) || !same_current(b->transaction,&b->external))return JT_STALE;
 /* Expiry/clock checks precede the physical release. A late external proof
  * must not make the bus reusable after its transaction token expired. */
 JTResult tick=jt_tick(b->transaction,b->transaction->epoch,now);
 if(tick!=JT_OK || !same_current(b->transaction,&b->external)){
  b->last_transaction_result=tick;
  return tick==JT_OK?JT_STALE:tick;
 }
 if(c->kind==JT_BUS_FENCE){
  if(c->success){
   JBResult release=jb_finish_fence(b->bus,c,now);
   if(release!=JB_OK){
    /* Keep the exact external token pending. Caller may correct missing
     * evidence; normal ticking still enforces the original deadline. */
    if(release==JB_STALE)return JT_STALE;
    if(release==JB_CLOCK_ERROR)return JT_CLOCK;
    if(release==JB_TIMEOUT)return JT_TIMEOUT;
    return JT_FENCE_REQUIRED;
   }
  }
 }else if(c->kind!=JT_PUBLISH && c->kind!=JT_SINK_FENCE)return JT_BAD_ARGUMENT;
 /* Forward the actual external result and its evidence unchanged. */
 JTResult r=jt_complete(b->transaction,now,c);
 b->last_transaction_result=r;
 b->external_pending=0;b->external_emitted=0;
 return r;
}
