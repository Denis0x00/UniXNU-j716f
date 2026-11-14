// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../Sm8250UfsHandoff.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#ifdef NDEBUG
#error Assertions must be enabled.
#endif
using namespace sm8250_ufs;
namespace sg = shengufs::smmu_group;
constexpr uint64_t S = 0x15000000, H = 0x1d84000, D = 0x180000000ULL;
struct Event { char type; uint64_t address; uint32_t value; };
struct Fixture {
  HandoffTransaction transaction;
  HandoffGrant grant;
  Io io;
  alignas(4096) uint8_t buffer[139264] = {};
  DmaView dma{buffer, D, sizeof(buffer)};
  HandoffBudget budget{100000, 10000, 4};
  std::map<uint64_t,uint32_t> regs;
  std::vector<Event> events;
  unsigned callbacks = 0, fault_at = 0, scr_reads = 0, writes = 0;
  unsigned mutation = 0, delay = 0, remaining_delay = 0;
  uint64_t ticks_value = 1, pending_address = 0;
  uint32_t pending_value = 0;
  bool frozen = false, reverse = false, ignore_writes = false;
  bool mutate_inputs = false, reenter = false, reentry_rejected = false;
  explicit Fixture(unsigned groups=8, unsigned banks=4, unsigned pages=4)
    : io{this, read, write, barrier, ticks, cache}
  {
    const auto *p = shengufs::platformProfile(shengufs::Platform::Sm8250);
    grant.identity = {p->soc,p->controller,p->host,p->smmu};
    grant.owner = {41, APPS_SMMU_HANDOFF_REQUIRED,
                  {0x8250,1,H,S,2,2,{0xe0,0x4e0},{6,7},3,D,sizeof(buffer)}};
    grant.register_access_authorization=42; grant.visibility_authorization=43;
    grant.dma_authorization=44; grant.facts=RequiredHandoffFacts;
    regs[p->gcc+p->gdsc]=0x80000000U; regs[p->gcc+p->reset]=0;
    for(const auto &clock:p->clocks) regs[p->gcc+clock.offset]=1;
    const unsigned offsets[]={0,8,0x20,0x24,0x30,0x34,0x50,0x54,0x58,0x60,0x70,0x74,0x78,0x80};
    for(unsigned offset:offsets)regs[H+offset]=0;
    regs[H]=0x0100001f;regs[H+8]=0x300;regs[H+0x30]=3;regs[H+0x34]=1;
    // These firmware pointer values are intentionally not readable addresses.
    regs[H+0x50]=0xffff0000;regs[H+0x54]=0xf0000000;
    regs[H+0x70]=0xffee0000;regs[H+0x74]=0xf0000000;
    unsigned encoded=0;while((1U<<(encoded+1))!=pages)++encoded;
    regs[S]=0;regs[S+0x20]=0x48001e00|groups;
    regs[S+0x24]=(encoded<<28)|banks;regs[S+0x28]=0x1111;
    for(unsigned i=0;i<groups;++i){regs[S+0x800+4*i]=0;regs[S+0xc00+4*i]=0x20000;}
    for(unsigned i=0;i<banks;++i){regs[S+(pages+i)*4096]=0;regs[S+0x1000+4*i]=0;regs[S+0x1800+4*i]=0;}
  }
  HandoffResult run(){return transaction.run(grant,io,dma,budget);}
  static Fixture &self(void *context){return *static_cast<Fixture*>(context);}
  bool event(char type,uint64_t address=0,uint32_t value=0)
  {
    events.push_back({type,address,value});++callbacks;
    if(mutate_inputs){
      mutate_inputs=false;grant.owner.claim.slots[0]=100;grant.facts=0;
      budget.max_operations=1;dma.physical=0;io.write32=nullptr;
    }
    if(reenter){
      reenter=false;reentry_rejected=run().error==HandoffError::Busy;
    }
    return callbacks!=fault_at;
  }
  static bool read(void *context,uint64_t address,uint32_t *value)
  {
    auto &f=self(context);assert(f.regs.count(address));
    if(!f.event('r',address))return false;
    if(address==S){
      ++f.scr_reads;
      if(f.scr_reads==2 && f.mutation==1)f.regs[S+0xc00]^=1;
      if(f.scr_reads==2 && f.mutation==2)f.regs[S+0x1000]^=1;
      if(f.scr_reads==3 && f.mutation==3)f.regs[S+0xc00]^=1;
      if(f.scr_reads==3 && f.mutation==4)f.regs[H+0x58]=1;
    }
    if(address==f.pending_address && f.remaining_delay && !--f.remaining_delay){
      f.regs[address]=f.pending_value;f.pending_address=0;
    }
    *value=f.regs[address];return true;
  }
  static bool write(void *context,uint64_t address,uint32_t value)
  {
    auto &f=self(context);++f.writes;
    const bool allowed=(address==S+0x180c&&value==1)||(address==S+0x100c&&value==0x10000)||
      (address==S+0xc18&&value==3)||(address==S+0xc1c&&value==3)||
      (address==S+0x818&&value==0x800000e0U)||(address==S+0x81c&&value==0x800004e0U);
    assert(allowed); // No globals, foreign CB, HCI, PHY, UIC or firmware pointers.
    // Deliberately apply a write BEFORE returning an injected false: partial
    // execution must quarantine and must never cause a rollback write.
    if(!f.ignore_writes){
      if(f.delay){f.pending_address=address;f.pending_value=value;f.remaining_delay=f.delay;}
      else f.regs[address]=value;
    }
    return f.event('w',address,value);
  }
  static bool barrier(void *context){return self(context).event('b');}
  static bool ticks(void *context,uint64_t *value)
  {
    auto &f=self(context);if(!f.event('t'))return false;
    if(f.reverse && f.callbacks>30)*value=0;
    else {*value=f.ticks_value;if(!f.frozen)++f.ticks_value;}
    return true;
  }
  static bool cache(void *,CacheOp,size_t,size_t){assert(false);return false;}
  void reuse(bool wide=true)
  {
    regs[S+0x818]=0x800000e0;regs[S+0xc18]=3;
    regs[S+0x81c]=0x800004e0;regs[S+0xc1c]=3;
    regs[S+0x100c]=0x10000;regs[S+0x180c]=wide?1:0;
  }
  void checkFailure(const HandoffResult &r,bool expect_quarantine=true)
  {
    assert(r.error!=HandoffError::None && !r.published);
    if(expect_quarantine){
      assert(r.quarantined && !r.lease_retained && r.cleanup_error==APPS_SMMU_OK);
      assert(apps_smmu_owner_release(&r.token)==APPS_SMMU_QUARANTINED);
      AppsSmmuToken next={};assert(apps_smmu_owner_try_acquire(&r.claim,&next)==APPS_SMMU_QUARANTINED);
    }
    const unsigned count=callbacks;
    assert(run().error==HandoffError::AlreadyRun && callbacks==count);
  }
  void checkSuccess(const HandoffResult &r,unsigned expected_writes)
  {
    assert(r.error==HandoffError::None && r.published && !r.quarantined && r.lease_retained);
    assert(r.writes==expected_writes && writes==expected_writes);
    assert(apps_smmu_owner_validate(&r.token,&r.claim)==APPS_SMMU_OK);
    // Publication alone is deliberately not command readiness.
    assert(apps_smmu_owner_validate_ready(&r.token,&r.claim)==APPS_SMMU_WRONG_STATE);
    assert(regs[H+0x60]==0 && regs[H+0x80]==0 && regs[H+0x58]==0 && regs[H+0x78]==0);
    assert(regs[H+0x50]==0xffff0000 && regs[H+0x54]==0xf0000000);
    assert(regs[S+0x818]==0x800000e0 && regs[S+0x81c]==0x800004e0);
    assert(regs[S+0xc18]==3 && regs[S+0xc1c]==3 && regs[S+0x100c]==0x10000 && regs[S+0x180c]==1);
    const unsigned count=callbacks;assert(run().error==HandoffError::AlreadyRun&&callbacks==count);
  }
};

static void scenario(unsigned n)
{
  Fixture f;
  if(n==0){auto r=f.run();f.checkSuccess(r,6);return;}
  if(n==1){f.reuse();auto r=f.run();f.checkSuccess(r,0);return;}
  if(n==2){f.reuse(false);auto r=f.run();f.checkSuccess(r,1);return;}
  if(n==3){f.delay=3;auto r=f.run();f.checkSuccess(r,6);assert(r.readback_polls==18);return;}
  if(n==4){f.frozen=true;auto r=f.run();f.checkSuccess(r,6);return;}
  if(n==5){f.reenter=true;auto r=f.run();f.checkSuccess(r,6);assert(f.reentry_rejected);return;}
  if(n==6){f.mutate_inputs=true;auto r=f.run();f.checkSuccess(r,6);assert(r.claim.slots[0]==6&&r.claim.dma_base==D);return;}
  if(n>=7&&n<=20){
    switch(n){
      case 7:f.grant.owner.authorization_id=0;break;
      case 8:f.grant.register_access_authorization=0;break;
      case 9:f.grant.visibility_authorization=0;break;
      case 10:f.grant.dma_authorization=0;break;
      case 11:case 12:case 13:case 14:f.grant.facts^=1U<<(n-11);break;
      case 15:f.grant.owner.flags^=APPS_SMMU_HANDOFF_DMA_IDLE;break;
      case 16:f.grant.identity.soc=shengufs::text("qcom,sm8550");break;
      case 17:f.grant.owner.claim.sids[0]=0x60;break;
      case 18:f.grant.owner.claim.slots[0]=128;break;
      case 19:f.grant.owner.claim.bank=128;break;
      case 20:f.grant.owner.claim.slots[0]=7;break;
    }
    auto r=f.run();assert(r.error==HandoffError::InvalidGrant&&f.callbacks==0);f.checkFailure(r,false);return;
  }
  if(n>=21&&n<=26){
    switch(n){case 21:f.dma.cpu=nullptr;break;case 22:++f.dma.cpu;break;
      case 23:--f.dma.bytes;break;case 24:f.dma.physical+=4096;break;
      case 25:f.dma.physical=S;f.grant.owner.claim.dma_base=S;break;
      case 26:f.dma.physical=UINT64_MAX-4095;f.grant.owner.claim.dma_base=f.dma.physical;break;}
    auto r=f.run();assert(r.error==HandoffError::InvalidDma&&f.callbacks==0);f.checkFailure(r,false);return;
  }
  if(n>=27&&n<=30){
    if(n==27)f.io.read32=nullptr;if(n==28)f.budget.timeout_ticks=0;
    if(n==29)f.budget.max_operations=100001;if(n==30)f.budget.max_readback_polls=1025;
    auto r=f.run();assert(f.callbacks==0);f.checkFailure(r,false);return;
  }
  if(n>=31&&n<=40){
    const uint64_t addresses[]={H+0x34,H+0x30,H+0x24,H+0x20,H+0x58,H+0x78,H+0x60,H+0x80,H,H+8};
    const uint32_t values[]={0,0,1,4,1,1,1,1,31,0x400};
    f.regs[addresses[n-31]]=values[n-31];auto r=f.run();f.checkFailure(r);assert(f.writes==0);return;
  }
  if(n>=41&&n<=49){
    switch(n){case 41:f.regs[S+0x20]|=1U<<8;break;case 42:f.regs[S+0x20]=0x48001e81;break;
      case 43:f.regs[S+0x24]=0x80000004;break;case 44:f.regs[S+0x28]=0x1666;break;
      case 45:f.regs[S]=1;break;case 46:f.regs[S]=8;break;
      case 47:f.regs[S+0x20]&=~(15U<<9);break;
      case 48:f.regs[S+0x24]|=4U<<16;break;case 49:f.regs[S+0x24]=0x10000005;break;}
    auto r=f.run();f.checkFailure(r);assert(f.writes==0);return;
  }
  if(n>=50&&n<=56){
    switch(n){
      case 50:f.regs[S+0x800]=0x800000f0;f.regs[S+0xc00]=3;break;
      case 51:f.regs[S+0x800]=0x800100e0;f.regs[S+0xc00]=3;break;
      case 52:f.regs[S+7*4096]=1;break;
      case 53:f.regs[S+0x100c]=0x20000;break;
      case 54:f.regs[S+0x180c]=2;break;
      case 55:f.regs[S+0x818]=0x800000e0;f.regs[S+0xc18]=2;break;
      case 56:f.grant.owner.claim.slots[0]=7;f.grant.owner.claim.slots[1]=6;break;
    }
    auto r=f.run();f.checkFailure(r);assert(f.writes==0);return;
  }
  if(n>=57&&n<=60){f.mutation=n-56;auto r=f.run();f.checkFailure(r);assert((n<59)==(f.writes==0));return;}
  if(n==61){f.ignore_writes=true;f.frozen=true;auto r=f.run();assert(r.error==HandoffError::Readback&&r.readback_polls==4&&r.writes==1);f.checkFailure(r);return;}
  if(n==62){f.reverse=true;auto r=f.run();assert(r.error==HandoffError::TimeReversed);f.checkFailure(r);assert(f.writes==0);return;}
  if(n==63){f.budget.timeout_ticks=20;auto r=f.run();assert(r.error==HandoffError::Timeout);f.checkFailure(r);assert(f.writes==0);return;}
  if(n==64){f.frozen=true;f.budget.max_operations=20;auto r=f.run();assert(r.error==HandoffError::OperationLimit&&r.operations==20);f.checkFailure(r);return;}
  if(n==65){
    Fixture maximum(128,128,128);auto r=maximum.run();maximum.checkSuccess(r,6);assert(r.reads>1900);return;
  }
  if(n==66){
    f.regs[S+0x818]=0x800000e0;f.regs[S+0xc18]=3;f.regs[S+0x100c]=0x10000;f.regs[S+0x180c]=1;
    auto r=f.run();f.checkSuccess(r,2);return;
  }
  assert(false);
}
static void child(unsigned mode,unsigned argument)
{
  if(mode==0){scenario(argument);return;}
  Fixture f;f.fault_at=argument;
  auto r=f.run();assert(f.callbacks==argument);f.checkFailure(r);
  // The first failed callback terminates access. Even a write that occurred
  // before failure receives no readback, retry or rollback.
  assert(r.operations<=f.budget.max_operations);
}
static void isolated(unsigned mode,unsigned argument)
{
  const pid_t pid=fork();assert(pid>=0);
  if(pid==0){child(mode,argument);_exit(0);}
  int status=0;assert(waitpid(pid,&status,0)==pid);
  if(!WIFEXITED(status)||WEXITSTATUS(status)){
    std::fprintf(stderr,"handoff mode %u scenario %u failed\n",mode,argument);std::abort();
  }
}
int main()
{
  // Real owner state is process-global and quarantine has no reset hook.
  // A pipe obtains baseline callback count without polluting the parent's owner.
  int pipes[2];assert(pipe(pipes)==0);const pid_t pid=fork();assert(pid>=0);
  if(pid==0){close(pipes[0]);Fixture f;auto r=f.run();f.checkSuccess(r,6);const unsigned count=f.callbacks;
    assert(write(pipes[1],&count,sizeof(count))==static_cast<ssize_t>(sizeof(count)));close(pipes[1]);_exit(0);}
  close(pipes[1]);unsigned callbacks=0;assert(read(pipes[0],&callbacks,sizeof(callbacks))==static_cast<ssize_t>(sizeof(callbacks)));
  close(pipes[0]);int status=0;assert(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
  assert(callbacks>300&&callbacks<1000);
  for(unsigned n=0;n<67;++n)isolated(0,n);
  for(unsigned n=1;n<=callbacks;++n)isolated(1,n);
  std::printf("PASS: 67 handoff scenarios and %u individual callback failures; bounded complete capture, exact dual-SID publication, no global/HCI writes, quarantine and retained owner lease\n",callbacks);
}
