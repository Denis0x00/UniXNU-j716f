// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../RestrictedUfs.h"
#include <cassert>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>
#ifdef NDEBUG
#error Assertions must be enabled.
#endif
namespace rp = shengufs::read_policy;
namespace rt = shengufs::restricted;
static void be32(uint8_t *p, uint32_t n) { for (unsigned i=0;i<4;++i) p[i]=uint8_t(n>>(24-8*i)); }
static void le32(uint8_t *p, uint32_t n) { for (unsigned i=0;i<4;++i) p[i]=uint8_t(n>>(8*i)); }

struct Fixture {
  rt::SingleSlot engine;
  rp::Context context = {};
  AppsSmmuClaim claim = {};
  AppsSmmuToken token = {};
  rt::Port port;
  rt::Budget budget{100, 8, 2};
  rp::Request prepared = {};
  rp::Request *mutate_request = nullptr;
  rt::Budget *mutate_budget = nullptr;
  uint64_t ticks = 1;
  unsigned probes=0, prepares=0, acks=0, rings=0, polls=0, collects=0;
  unsigned fault=0, busy_responses=0;
  bool stopped=false, reentry=false, frozen_clock=false, rejected_reentry=false;
  uint32_t power_mode=0x11, block_size=4096, last_lba=1023;
  Fixture(bool authorize=true, bool publish=true) : port{this,probe,prepare,ack,ring,poll,collect}
  {
    const uint64_t base=0x180000000ULL;
    context.selected_lun=0; context.expected_tag=1;
    context.dma={{base,139264},{base,32},{base+4096,32},{base+4608,512},{base+5120,16},{base+8192,131072}};
    context.known_blocks[0]={0,8}; context.known_block_count=1;
    // Even a forged pre-measured capacity must be discarded by configure().
    context.capacity={true,0,4096,1024};
    claim={0x8250,1,0x1d84000,0x15000000,2,2,{0xe0,0x4e0},{6,7},3,base,139264};
    assert(engine.configure(context,claim)==rt::Error::None);
    if (authorize) {
      AppsSmmuHandoff h{1,APPS_SMMU_HANDOFF_REQUIRED,claim};
      assert(apps_smmu_owner_install_handoff(&h)==APPS_SMMU_OK);
      assert(apps_smmu_owner_try_acquire(&claim,&token)==APPS_SMMU_OK);
      if (publish) {
        assert(apps_smmu_owner_publication_begin(&token)==APPS_SMMU_OK);
        assert(apps_smmu_owner_publication_complete(&token)==APPS_SMMU_OK);
        assert(apps_smmu_owner_queue_started(&token)==APPS_SMMU_OK);
      }
    }
  }
  rp::Request request(unsigned kind) const
  {
    rp::Request r={}; r.upiu[3]=context.expected_tag;
    if(kind==1) {r.upiu[0]=0x16;r.upiu[5]=1;r.upiu[12]=3;r.upiu[13]=2;}
    if(kind>=2) {r.upiu[0]=1;r.upiu[2]=context.selected_lun;r.upiu[16]=kind==2?0:kind==3?0x25:0x28;}
    if(kind>=3) {
      r.direction=rp::Direction::FromDevice;r.data_bytes=kind==3?8:4096;r.upiu[1]=0x40;
      be32(r.upiu+12,r.data_bytes);r.prd_count=1;
      const uint64_t base=context.dma.data.address;
      le32(r.prd,uint32_t(base));le32(r.prd+4,uint32_t(base>>32));le32(r.prd+12,r.data_bytes-1);
      if(kind==4) {be32(r.upiu+18,8);r.upiu[24]=1;}
    }
    return r;
  }
  rt::Result run(unsigned kind) {return engine.execute(request(kind),token,port,budget);}
  void prefix(unsigned count) {for(unsigned i=0;i<count;++i) assert(run(i).error==rt::Error::None);}
  static Fixture &self(void *p) {return *static_cast<Fixture*>(p);}
  static bool probe(void *p,rt::Probe *o)
  {
    auto &f=self(p);++f.probes;
    if(f.mutate_request) f.mutate_request->upiu[16]=0x2a;
    if(f.mutate_budget) {f.mutate_budget->max_polls=100000;f.mutate_budget->timeout_ticks=UINT64_MAX;}
    if(f.reentry) {
      f.reentry=false;
      f.rejected_reentry=f.run(0).error==rt::Error::Busy;
    }
    *o={f.ticks,f.context.dma.utrd.address,1,3,0,0,0,0,f.stopped?0U:1U};
    if(f.fault==1)return false;
    if(f.fault==2)o->doorbell=1;
    if(f.fault==3)o->interrupt_status=0x800;
    if(f.fault==4)o->interrupt_enable=1;
    if(f.fault==5)o->request_list+=4096;
    return true;
  }
  static bool prepare(void *p,const rp::Request &r,const rp::Context &c)
  {auto &f=self(p);++f.prepares;assert(c.dma.allocation.address==f.claim.dma_base);f.prepared=r;return f.fault!=6;}
  static bool ack(void *p) {auto &f=self(p);++f.acks;return f.fault!=7;}
  static bool ring(void *p) {auto &f=self(p);++f.rings;return f.fault!=8;}
  static bool poll(void *p,rt::Poll *o)
  {
    auto &f=self(p);++f.polls;
    if(!f.frozen_clock)++f.ticks;
    *o={f.ticks,0,0,0,0};
    if(f.fault==9)return false;
    if(f.fault==10)o->doorbell=1;
    if(f.fault==11)o->ocs=15; // DB=0 cannot prove a completion.
    if(f.fault==12)o->interrupt_status=0x800;
    if(f.fault==13)o->doorbell=2;
    if(f.fault==14)o->task_doorbell=1;
    if(f.fault==15)o->ticks=0;
    if(f.fault==16)o->ocs=7;
    return true;
  }
  static bool collect(void *p,rt::Completion *c)
  {
    auto &f=self(p);++f.collects;
    if(f.fault==17)return false;
    const bool query=f.prepared.upiu[0]==0x16, scsi=f.prepared.upiu[0]==1;
    c->response[0]=scsi?0x21:query?0x36:0x20;
    c->response[2]=f.prepared.upiu[2];c->response[3]=f.prepared.upiu[3];
    c->response_bytes=512;
    if(query) {c->response[5]=1;c->response[12]=3;c->response[13]=2;be32(c->response+20,f.power_mode);}
    if(scsi&&f.busy_responses) {c->response[7]=f.busy_responses%2?8:0x28;--f.busy_responses;}
    if(scsi&&f.prepared.upiu[16]==0x25) {c->capacity_bytes=8;be32(c->capacity,f.last_lba);be32(c->capacity+4,f.block_size);}
    if(f.fault==18)c->response[3]^=1;
    if(f.fault==19)c->response[12]=1;
    if(f.fault==20)c->response_bytes=513;
    if(f.fault==21)c->response[32]=1;
    return true;
  }
  unsigned callbacks() const {return probes+prepares+acks+rings+polls+collects;}
  void close()
  {
    stopped=true;
    assert(engine.confirmStopped(token,port).error==rt::Error::None);
    assert(engine.phase()==rt::Phase::Stopped);
    assert(apps_smmu_owner_confirm_unpublished(&token)==APPS_SMMU_OK);
    assert(apps_smmu_owner_release(&token)==APPS_SMMU_OK);
  }
};

static void scenario(unsigned n)
{
  if(n==0) {
    Fixture f;assert(f.run(4).error==rt::Error::PolicyRejected&&f.callbacks()==0);
    f.prefix(5);assert(f.engine.phase()==rt::Phase::ReadReady);
    assert(f.prepares==5&&f.acks==5&&f.rings==5);
    auto blocked=f.request(4);be32(blocked.upiu+18,9);
    unsigned count=f.callbacks();assert(f.engine.execute(blocked,f.token,f.port,f.budget).error==rt::Error::PolicyRejected);
    assert(f.callbacks()==count);assert(f.run(4).error==rt::Error::None);
    f.close();count=f.callbacks();assert(f.run(4).error==rt::Error::WrongPhase&&f.callbacks()==count);
    assert(f.engine.configure(f.context,f.claim)==rt::Error::WrongPhase);return;
  }
  if(n==1) {
    Fixture f;
    for(unsigned opcode=0;opcode<256;++opcode) if(opcode!=0) {
      auto bad=f.request(2);bad.upiu[16]=uint8_t(opcode);
      auto r=f.engine.execute(bad,f.token,f.port,f.budget);
      assert(r.error!=rt::Error::None&&f.callbacks()==0);
    }
    auto bad=f.request(0);bad.direction=rp::Direction::ToDevice;
    assert(f.engine.execute(bad,f.token,f.port,f.budget).error==rt::Error::PolicyRejected);
    assert(f.callbacks()==0);f.close();return;
  }
  if(n==2) {Fixture f(false);assert(f.run(0).error==rt::Error::Ownership&&f.callbacks()==0);return;}
  if(n==3) {Fixture f(true,false);assert(f.run(0).error==rt::Error::Ownership&&f.callbacks()==0);return;}
  if(n>=4&&n<=24) {
    Fixture f;f.fault=n-3;
    auto result=f.run(0);assert(result.error!=rt::Error::None);
    const bool ordinaryHalt=f.fault==4||f.fault==5;
    assert(f.engine.phase()==(ordinaryHalt?rt::Phase::Halted:rt::Phase::Quarantined));
    const unsigned count=f.callbacks();assert(f.run(0).error==rt::Error::WrongPhase&&f.callbacks()==count);
    assert(apps_smmu_owner_release(&f.token)!=APPS_SMMU_OK);
    if(!ordinaryHalt) {AppsSmmuToken t={};assert(apps_smmu_owner_try_acquire(&f.claim,&t)!=APPS_SMMU_OK);}
    if(f.fault<=5)assert(f.prepares==0&&f.acks==0&&f.rings==0);
    if(f.fault==6)assert(f.acks==0&&f.rings==0);
    if(f.fault==7)assert(f.rings==0);
    return;
  }
  if(n==25) {Fixture f;f.prefix(2);f.busy_responses=2;auto r=f.run(2);assert(r.error==rt::Error::None&&r.attempts==3&&f.engine.phase()==rt::Phase::NeedCapacity);f.close();return;}
  if(n==26) {Fixture f;f.prefix(2);f.busy_responses=3;auto r=f.run(2);assert(r.error==rt::Error::RetryLimit&&r.attempts==3);unsigned c=f.callbacks();assert(f.run(2).error==rt::Error::WrongPhase&&f.callbacks()==c);f.close();return;}
  if(n==27) {Fixture f;f.prefix(1);f.power_mode=0x22;assert(f.run(1).error==rt::Error::PowerNotReady&&f.engine.phase()==rt::Phase::Halted);f.close();return;}
  if(n==28||n==29) {Fixture f;f.prefix(3);if(n==28)f.block_size=512;else f.last_lba=0xffffffff;assert(f.run(3).error==rt::Error::InvalidCapacity);unsigned c=f.callbacks();assert(f.run(4).error==rt::Error::PolicyRejected&&f.callbacks()==c);f.close();return;}
  if(n==30) {Fixture f;f.fault=11;f.frozen_clock=true;f.budget.max_polls=4;auto r=f.run(0);assert(r.error==rt::Error::Timeout&&r.polls==4&&f.collects==0);return;}
  if(n==31) {Fixture f;f.fault=10;f.budget.timeout_ticks=2;auto r=f.run(0);assert(r.error==rt::Error::Timeout&&r.polls==2);return;}
  if(n==32) {Fixture f;f.reentry=true;assert(f.run(0).error==rt::Error::None&&f.rejected_reentry&&f.rings==1);f.close();return;}
  if(n==33) {Fixture f;auto r=f.request(0);f.mutate_request=&r;assert(f.engine.execute(r,f.token,f.port,f.budget).error==rt::Error::None);assert(r.upiu[16]==0x2a&&f.prepared.upiu[16]==0);f.mutate_request=nullptr;f.close();return;}
  if(n==34) {Fixture f;f.budget.max_polls=3;f.mutate_budget=&f.budget;f.fault=11;f.frozen_clock=true;auto r=f.run(0);assert(r.error==rt::Error::Timeout&&r.polls==3);return;}
  if(n==35) {Fixture f;f.budget.retries=3;assert(f.run(0).error==rt::Error::InvalidBudget&&f.callbacks()==0);return;}
  if(n==36) {Fixture f;f.port.collect=nullptr;assert(f.run(0).error==rt::Error::InvalidPort&&f.callbacks()==0);return;}
  if(n==37) {Fixture f;assert(f.engine.confirmStopped(f.token,f.port).error==rt::Error::StopNotProven);assert(apps_smmu_owner_release(&f.token)==APPS_SMMU_NOT_QUIESCENT);f.close();return;}
  if(n==38) {Fixture f;f.prefix(2);f.busy_responses=2;f.budget.max_polls=2;auto r=f.run(2);assert(r.error==rt::Error::Timeout&&r.attempts==2&&r.polls==2);assert(f.engine.phase()==rt::Phase::Halted);f.close();return;}
  if(n==39) {Fixture f;rt::SingleSlot second;auto bad=f.claim;bad.sids[0]=0x60;assert(second.configure(f.context,bad)==rt::Error::InvalidConfiguration);bad=f.claim;bad.dma_bytes+=4096;assert(second.configure(f.context,bad)==rt::Error::InvalidConfiguration);return;}
  assert(false);
}
int main()
{
  // Quarantined claims deliberately have no reset API. Separate OS processes
  // isolate fault scenarios without adding a production-only test reset hook.
  for(unsigned n=0;n<40;++n) {
    pid_t child=fork();assert(child>=0);
    if(child==0) {scenario(n);_exit(0);}
    int status=0;assert(waitpid(child,&status,0)==child);
    if(!WIFEXITED(status)||WEXITSTATUS(status)) {std::fprintf(stderr,"scenario %u failed\n",n);return 1;}
  }
  std::puts("PASS: 40 isolated single-slot scenarios: mandatory packet gate, true owner lease, staged readiness, bounded retry/poll, retained packet, terminal quarantine and explicit stop proof");
}
