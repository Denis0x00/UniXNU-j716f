/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "R35Handoff.h"
R35U32 r35_record_crc(const R35Record *r)
{
  const R35U8 *p=(const R35U8 *)r;
  R35U32 crc=0xffffffffU;
  unsigned i,bit;
  if(!r) return 0;
  for(i=0;i<R35_RECORD_BYTES;++i) {
    const R35U8 value=(i>=12 && i<16)?0:p[i];
    crc^=value;
    for(bit=0;bit<8;++bit) crc=(crc>>1)^((0U-(crc&1U))&0xedb88320U);
  }
  return ~crc;
}
static int r35_zero(const void *p,unsigned bytes)
{
  const R35U8 *v=(const R35U8 *)p;
  unsigned i;
  for(i=0;i<bytes;++i) if(v[i]) return 0;
  return 1;
}
static int r35_context_valid(const R35Context *c,unsigned bank)
{
  unsigned i;
  if(!c->bank) return r35_zero(c,sizeof(*c)); /* not reached before failure */
  if(c->bank!=bank || c->reserved || c->alignment_zero ||
     (c->valid32&~R35_CONTEXT_ALL) || (c->valid64&~3U)) return 0;
  for(i=0;i<R35_CONTEXT_WORDS;++i) if(!(c->valid32&(1U<<i)) && c->words[i]) return 0;
  for(i=0;i<2;++i) if(!(c->valid64&(1U<<i)) && c->ttbr[i]) return 0;
  return 1;
}
static int r35_snapshot_valid(const R35Snapshot *s)
{
  unsigned i;
  if((s->hci_valid&~((1U<<R35_HCI_WORDS)-1)) || (s->header_valid&~15U) ||
     s->stream_count>R35_GROUPS || s->reserved || s->alignment_zero) return 0;
  for(i=0;i<R35_HCI_WORDS;++i) if(!(s->hci_valid&(1U<<i)) && s->hci[i]) return 0;
  for(i=0;i<4;++i) if(!(s->header_valid&(1U<<i)) && s->headers[i]) return 0;
  for(i=0;i<R35_GROUPS;++i) {
    const R35Stream *v=&s->streams[i];
    if((v->valid&~3U) || (!(v->valid&1U) && v->smr) ||
       (!(v->valid&2U) && v->s2cr)) return 0;
    if(i>=s->stream_count && !r35_zero(v,sizeof(*v))) return 0;
  }
  for(i=0;i<3;++i) if(!r35_context_valid(&s->context[i],i+2)) return 0;
  return 1;
}
int r35_record_validate(const R35Record *r)
{
  unsigned i,completed=0;
  if(!(r && r->magic==R35_MAGIC && r->version==R35_VERSION &&
    r->bytes==R35_RECORD_BYTES && r->profile==R35_PROFILE && r->source==1 &&
    r->boot_id && r->boot_sequence && r->finished==1 &&
    r->status>=R35Argument && r->status<=R35Maintained && r->phase<=R35Done &&
    r->mode<=R35StopAndMaintain && r->writes_attempted<=R35_MAX_WRITES &&
    r->writes_completed<=r->writes_attempted &&
    !r->tz_exclusion_proven && !r->dma_ownership_granted &&
    !r->global_dma_drain_proven && !r->root_ready && !r->reserved)) return 0;
  if(r->host_stopped>1 || r->routes_unchanged>1 || r->display_config_unchanged>1 ||
     r->old_namespace_captured>1 || r->mutation_possible>1 || r->smmu_mutation_possible>1 ||
     r->post_detacher_recorded>1 || r->post_detacher_preserved>1 || r->maintenance_record_valid>1 ||
     r->detacher_policy>R35PreserveUfsBanks || (r->disabled_mask&~0xcU) ||
     (r->synchronized_mask&~0xcU) || (r->preserve_banks&~0x1cU) ||
     (r->preserve_slots[2]&~0x7fffffU)) return 0;
  if((r->synchronized_mask&~r->disabled_mask) ||
     (r->smmu_mutation_possible && !r->mutation_possible) ||
     (r->post_detacher_preserved && !r->post_detacher_recorded)) return 0;
  if((r->status==R35Observed || r->status==R35Maintained) && r->phase!=R35Done) return 0;
  if(r->maintenance_record_valid && (!r->post_detacher_recorded || !r->post_detacher_preserved ||
     r->status!=R35Maintained || !r->host_stopped || r->disabled_mask!=0xc ||
     r->synchronized_mask!=0xc || !r->routes_unchanged || !r->display_config_unchanged ||
     !r->old_namespace_captured)) return 0;
  for(i=0;i<2;++i) {
    if(r->selected_slots[i]==0xffffffffU) {
      if(r->selected_banks[i]!=0xffffffffU) return 0;
    } else if(r->selected_slots[i]>=R35_GROUPS || r->selected_banks[i]!=i+2) return 0;
  }
  if(r->selected_slots[0]!=0xffffffffU && r->selected_slots[0]==r->selected_slots[1]) return 0;
  if(!r35_snapshot_valid(&r->before) || !r35_snapshot_valid(&r->after)) return 0;
  for(i=0;i<R35_MAX_WRITES;++i) {
    const R35WriteRecord *w=&r->writes[i];
    if(i>=r->writes_attempted) { if(!r35_zero(w,sizeof(*w))) return 0; continue; }
    if(!w->address || (w->address&3U) || w->reserved || w->completed>1 ||
       (w->phase!=R35StopHci && w->phase!=R35DisableContexts && w->phase!=R35SyncContexts)) return 0;
    completed+=w->completed;
  }
  return completed==r->writes_completed && r->crc32==r35_record_crc(r);
}
void r35_record_post_detacher(R35Record *r,int preserved)
{
  if(!r35_record_validate(r)) return;
  r->post_detacher_recorded=1;
  r->post_detacher_preserved=preserved?1:0;
  r->maintenance_record_valid=preserved && r->status==R35Maintained &&
    r->host_stopped && r->disabled_mask==0xc && r->synchronized_mask==0xc &&
    r->routes_unchanged && r->display_config_unchanged && r->old_namespace_captured;
  r->crc32=r35_record_crc(r);
}
const char *r35_status_name(enum R35Status s)
{
  switch(s) {
#define NAME(x,v) case x:return v
    NAME(R35Waiting,"waiting"); NAME(R35Busy,"busy"); NAME(R35AlreadyAttempted,"already-attempted");
    NAME(R35Argument,"argument"); NAME(R35Access,"access"); NAME(R35Clock,"clock");
    NAME(R35Deadline,"deadline"); NAME(R35TimeReversed,"time-reversed"); NAME(R35Budget,"budget");
    NAME(R35Read,"read"); NAME(R35Write,"write"); NAME(R35Barrier,"barrier");
    NAME(R35Geometry,"geometry"); NAME(R35HostState,"host-state"); NAME(R35Routes,"routes");
    NAME(R35SharedContext,"shared-context"); NAME(R35Namespace,"namespace"); NAME(R35Fault,"fault");
    NAME(R35StateChanged,"state-changed"); NAME(R35Readback,"readback"); NAME(R35SyncTimeout,"sync-timeout");
    NAME(R35Denied,"denied"); NAME(R35Observed,"observed"); NAME(R35Maintained,"maintained");
#undef NAME
  }
  return "invalid";
}
