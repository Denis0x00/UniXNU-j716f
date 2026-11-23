// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host integration of the real handoff, HCI, command-state and owner modules.
// One fake register/cache device supplies only the OS boundary. No fixture call
// marks publication, queue-started or DMA-complete on the owner's behalf.
#include "../Sm8250UfsHandoff.h"
#include "../Sm8250UfsHci.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#ifdef NDEBUG
#error Assertions must remain enabled.
#endif
namespace s = sm8250_ufs;
namespace p = shengufs::read_policy;
namespace r = shengufs::restricted;
constexpr uint64_t H=0x1d84000, S=0x15000000, D=0x180000000ULL;
static void le32(uint8_t *out,uint32_t value){for(unsigned i=0;i<4;++i)out[i]=uint8_t(value>>(8*i));}
static void be32(uint8_t *out,uint32_t value){for(unsigned i=0;i<4;++i)out[i]=uint8_t(value>>(24-8*i));}
static uint8_t knownByte(unsigned offset){return uint8_t((offset*13)^0xa5^7);}
struct Event {char type;uint64_t address;size_t bytes;uint32_t value;};
struct Fixture {
  s::HandoffTransaction transaction;
  s::HciPort hci;
  r::SingleSlot engine;
  s::HandoffGrant grant;
  s::HandoffResult handoff;
  s::Io io;
  alignas(4096) uint8_t cpu[139264];
  alignas(4096) uint8_t device[139264];
  s::DmaView dma{cpu,D,sizeof(cpu)};
  p::Context context{};
  std::map<uint64_t,uint32_t> regs;
  std::vector<Event> events;
  std::vector<unsigned> commands;
  uint64_t clock=1;
  unsigned rings=0, completions=0, pending=0, smmu_writes=0, full_captures=0;
  unsigned capture_registers=0;
  unsigned fail_smmu_write=0, busy_responses=0;
  bool handoff_finished=false, no_completion=false, bad_response=false;
  bool fail_after_ring=false, fail_data_invalidate=false, fail_list_write=false;
  bool frozen_clock=false, capture_active=false;
  Fixture():io{this,read,write,barrier,ticks,cache}
  {
    std::memset(cpu,0xac,sizeof(cpu));std::memset(device,0xdb,sizeof(device));
    const auto *profile=shengufs::platformProfile(shengufs::Platform::Sm8250);
    grant.identity={profile->soc,profile->controller,profile->host,profile->smmu};
    grant.owner={81,APPS_SMMU_HANDOFF_REQUIRED,{0x8250,1,H,S,2,2,{0xe0,0x4e0},{6,7},3,D,sizeof(cpu)}};
    grant.register_access_authorization=82;grant.visibility_authorization=83;
    grant.dma_authorization=84;grant.facts=s::RequiredHandoffFacts;
    regs[profile->gcc+profile->gdsc]=0x80000000;regs[profile->gcc+profile->reset]=0;
    for(const auto &branch:profile->clocks)regs[profile->gcc+branch.offset]=1;
    const unsigned host[]={0,8,0x20,0x24,0x30,0x34,0x50,0x54,0x58,0x60,0x70,0x74,0x78,0x80};
    for(unsigned offset:host)regs[H+offset]=0;
    regs[H]=0x0100001f;regs[H+8]=0x300;regs[H+0x30]=0xf;regs[H+0x34]=1;
    regs[H+0x20]=0x401; // A stale transfer event and unrelated UIC completion.
    regs[H+0x50]=0xffff0000;regs[H+0x54]=0xf0000000;
    regs[H+0x70]=0xffee0000;regs[H+0x74]=0xf0000000;
    regs[S]=0;regs[S+0x20]=0x48001e08;regs[S+0x24]=0x10000004;regs[S+0x28]=0x1111;
    for(unsigned i=0;i<8;++i){regs[S+0x800+4*i]=0;regs[S+0xc00+4*i]=0x20000;}
    for(unsigned i=0;i<4;++i){regs[S+(4+i)*4096]=0;regs[S+0x1000+4*i]=0;regs[S+0x1800+4*i]=0;}
    context.selected_lun=3;context.expected_tag=0x3d;
    context.dma={{D,139264},{D,32},{D+4096,32},{D+4608,512},{D+5120,16},{D+8192,131072}};
    context.known_blocks[0]={3,7};context.known_block_count=1;
    // The state machine must discard this unmeasured caller-supplied capacity.
    context.capacity={true,3,4096,128};
  }
  static Fixture &self(void *v){return *static_cast<Fixture*>(v);}
  void event(char type,uint64_t address=0,size_t bytes=0,uint32_t value=0)
  {events.push_back({type,address,bytes,value});}
  static bool read(void *v,uint64_t address,uint32_t *out)
  {
    auto &f=self(v);assert(f.regs.count(address));f.event('R',address);
    if(address==H+0x58 && f.pending){
      if(f.fail_after_ring)return false;
      if(!f.no_completion && !--f.pending)f.complete();
    }
    // Track the whole register set, so a CBA2R write-readback cannot masquerade
    // as a complete capture of all eight groups and four context banks.
    if(address==S){assert(!f.capture_active);f.capture_active=true;f.capture_registers=0;}
    if(f.capture_active){
      const unsigned at=f.capture_registers;
      uint64_t expected=S;
      if(at>=1&&at<=16){const unsigned stream=(at-1)/2;expected=S+((at-1)%2?0xc00:0x800)+4*stream;}
      if(at>=17){const unsigned bank=(at-17)/3,field=(at-17)%3;
        expected=field==0?S+(4+bank)*4096:field==1?S+0x1000+4*bank:S+0x1800+4*bank;}
      assert(address==expected);
      if(++f.capture_registers==29){f.capture_active=false;++f.full_captures;}
    }
    *out=f.regs[address];return true;
  }
  void mapping() const
  {
    assert(regs.at(S)==0 && regs.at(S+7*4096)==0);
    assert(regs.at(S+0x100c)==0x10000 && regs.at(S+0x180c)==1);
    assert(regs.at(S+0x818)==0x800000e0 && regs.at(S+0x81c)==0x800004e0);
    assert(regs.at(S+0xc18)==3 && regs.at(S+0xc1c)==3);
    // Foreign route/context values remain unchanged through the whole pipeline.
    assert(regs.at(S+0x800)==0 && regs.at(S+0xc00)==0x20000);
    assert(regs.at(S+0x1000)==0 && regs.at(S+0x1800)==0);
  }
  static bool write(void *v,uint64_t address,uint32_t value)
  {
    auto &f=self(v);f.event('W',address,0,value);
    if(address>=S && address<S+0x100000){
      assert(!f.handoff_finished && !f.rings);
      const bool allowed=(address==S+0x180c&&value==1)||(address==S+0x100c&&value==0x10000)||
        (address==S+0xc18&&value==3)||(address==S+0xc1c&&value==3)||
        (address==S+0x818&&value==0x800000e0)||(address==S+0x81c&&value==0x800004e0);
      assert(allowed);f.regs[address]=value;++f.smmu_writes;
      return f.smmu_writes!=f.fail_smmu_write; // Deliberately ambiguous partial write.
    }
    assert(f.handoff_finished && f.full_captures>=3);f.mapping();
    assert(address==H+0x20||address==H+0x50||address==H+0x54||address==H+0x58||address==H+0x60);
    if(address==H+0x20){assert(value==1);f.regs[address]&=~value;}
    else if(address==H+0x58){
      assert(value==1 && f.regs[H+0x60]==1 && !f.regs[address] && !f.pending);
      assert((uint64_t(f.regs[H+0x54])<<32|f.regs[H+0x50])==D);
      assert(apps_smmu_owner_validate_ready(&f.handoff.token,&f.handoff.claim)==APPS_SMMU_NOT_QUIESCENT);
      f.descriptors();f.regs[address]=1;f.pending=2;++f.rings;
    }else if(address==H+0x60){assert(value<=1);f.regs[address]=value;}
    else{
      assert(value==(address==H+0x50?uint32_t(D):uint32_t(D>>32)));
      f.regs[address]=value;if(f.fail_list_write)return false;
    }
    return true;
  }
  static bool barrier(void *v){self(v).event('B');return true;}
  static bool ticks(void *v,uint64_t *out)
  {auto &f=self(v);f.event('T');*out=f.clock;if(!f.frozen_clock)++f.clock;return true;}
  static bool cache(void *v,s::CacheOp op,size_t offset,size_t bytes)
  {
    auto &f=self(v);assert(f.handoff_finished);
    assert(bytes && !(offset&63) && !(bytes&63) && offset<sizeof(f.cpu) && bytes<=sizeof(f.cpu)-offset);
    assert(op==s::CacheOp::CleanInvalidate||op==s::CacheOp::Invalidate);
    f.event(op==s::CacheOp::Invalidate?'I':'C',offset,bytes);
    if(op==s::CacheOp::Invalidate){
      assert(!f.pending); // Controller completion must precede CPU consumption.
      if(f.fail_data_invalidate && offset>=8192)return false;
      std::memcpy(f.cpu+offset,f.device+offset,bytes);
    }else{
      assert(!f.pending);std::memcpy(f.device+offset,f.cpu+offset,bytes);
      std::memset(f.cpu+offset,0xe7,bytes); // Simulate a distinct, invalid CPU view.
    }
    return true;
  }
  void descriptors()
  {
    const uint8_t *q=device+4096;unsigned command=99;
    if(q[0]==0)command=0;
    else if(q[0]==0x16)command=1;
    else if(q[0]==1){assert(q[2]==3);if(q[16]==0)command=2;else if(q[16]==0x25)command=3;else if(q[16]==0x28)command=4;}
    assert(command<5);commands.push_back(command);
    assert(q[3]==0x3d && p::loadLe32(device+8)==15);
    assert(p::loadLe32(device)==(0x10000000U|(command>=3?4U<<24:0)));
    const uint64_t list=uint64_t(p::loadLe32(device+16))|(uint64_t(p::loadLe32(device+20))<<32);
    assert(list==D+4096 && p::loadLe32(device+24)==0x00800080);
    assert(p::loadLe32(device+28)==(0x01000000U|(command>=3?1:0)));
    for(unsigned i=4608;i<5120;++i)assert(!device[i]);
    if(command>=3){
      assert((uint64_t(p::loadLe32(device+5124))<<32|p::loadLe32(device+5120))==D+8192);
      assert(p::loadLe32(device+5132)==(command==3?7:4095));
      if(command==4)assert(p::loadBe32(q+18)==7 && q[24]==1);
    }
  }
  void complete()
  {
    assert(pending==0);++completions;regs[H+0x58]=0;regs[H+0x20]|=1;
    le32(device+8,0);const uint8_t *q=device+4096;uint8_t *rsp=device+4608;
    rsp[0]=q[0]==0?0x20:q[0]==0x16?0x36:0x21;rsp[2]=q[2];rsp[3]=q[3];
    if(q[0]==0x16){rsp[5]=1;rsp[12]=3;rsp[13]=2;rsp[23]=0x11;}
    if(q[0]==1){
      rsp[11]=2;
      if(busy_responses){rsp[7]=8;--busy_responses;}
      else if(q[16]==0x25){be32(device+8192,127);be32(device+8196,4096);}
      else if(q[16]==0x28)for(unsigned i=0;i<4096;++i)device[8192+i]=knownByte(i);
    }
    if(bad_response)rsp[3]^=1;
  }
  bool start()
  {
    handoff=transaction.run(grant,io,dma,{100000,10000,4});
    if(!handoff.published)return false;
    handoff_finished=true;assert(handoff.error==s::HandoffError::None);
    assert(!rings && !regs[H+0x60] && full_captures==3);mapping();
    assert(apps_smmu_owner_validate_ready(&handoff.token,&handoff.claim)==APPS_SMMU_WRONG_STATE);
    assert(engine.configure(context,handoff.claim)==r::Error::None);
    return hci.bind(io,dma,handoff.claim,handoff.token)&&hci.setupQueue();
  }
  p::Request request(unsigned command,uint32_t lba=7) const
  {
    p::Request q{};q.upiu[3]=context.expected_tag;
    if(command==1){q.upiu[0]=0x16;q.upiu[5]=1;q.upiu[12]=3;q.upiu[13]=2;}
    if(command>=2){q.upiu[0]=1;q.upiu[2]=3;q.upiu[16]=command==2?0:command==3?0x25:0x28;}
    if(command>=3){
      q.direction=p::Direction::FromDevice;q.data_bytes=command==3?8:4096;
      q.upiu[1]=0x40;be32(q.upiu+12,q.data_bytes);q.prd_count=1;
      le32(q.prd,uint32_t(D+8192));le32(q.prd+4,uint32_t((D+8192)>>32));le32(q.prd+12,q.data_bytes-1);
      if(command==4){be32(q.upiu+18,lba);q.upiu[24]=1;}
    }
    return q;
  }
  r::Result execute(unsigned command,uint32_t lba=7,unsigned retries=0)
  {return engine.execute(request(command,lba),handoff.token,hci.port(),{1000,8,retries});}
  void prefix(unsigned count)
  {
    for(unsigned command=0;command<count;++command){
      auto result=execute(command);assert(result.error==r::Error::None);
      if(command==3)assert(result.capacity.measured&&result.capacity.blocks==128&&result.capacity.block_bytes==4096&&result.capacity.lun==3);
    }
  }
  void stopAndRetain()
  {
    assert(hci.stopQueue(100,8));assert(engine.confirmStopped(handoff.token,hci.port()).error==r::Error::None);
    assert(engine.phase()==r::Phase::Stopped&&regs[H+0x60]==0);
    const auto old=handoff.token;
    assert(apps_smmu_owner_release(&old)==APPS_SMMU_OK); // No fake unpublish.
    assert(apps_smmu_owner_validate(&old,&handoff.claim)==APPS_SMMU_STALE);mapping();
    // Another client cannot claim the still-published resources after release.
    AppsSmmuHandoff foreign=grant.owner;foreign.authorization_id=91;foreign.claim.client_id=0x8251;
    assert(apps_smmu_owner_install_handoff(&foreign)==APPS_SMMU_OK);
    AppsSmmuToken other{};assert(apps_smmu_owner_try_acquire(&foreign.claim,&other)==APPS_SMMU_CONFLICT);
    // Exact retained claim can reacquire with a fresh lease, still stopped.
    AppsSmmuToken next{};assert(apps_smmu_owner_try_acquire(&handoff.claim,&next)==APPS_SMMU_OK);
    assert(next.lease_id!=old.lease_id&&apps_smmu_owner_validate_ready(&next,&handoff.claim)==APPS_SMMU_WRONG_STATE);
    assert(apps_smmu_owner_release(&next)==APPS_SMMU_OK);mapping();
    // Allocation lifetime ends only with this isolated fake process. Production
    // must retain its mapped DMA allocation even though the CPU lease is free.
  }
  void terminal(unsigned expected_rings)
  {
    assert(engine.phase()==r::Phase::Quarantined&&rings==expected_rings);
    assert(apps_smmu_owner_release(&handoff.token)==APPS_SMMU_QUARANTINED);
    AppsSmmuToken next{};assert(apps_smmu_owner_try_acquire(&handoff.claim,&next)==APPS_SMMU_QUARANTINED);
    const auto count=events.size();const auto saved=device[8192];
    assert(execute(4,7,2).error!=r::Error::None);
    assert(!hci.setupQueue()&&!hci.stopQueue(100,8));
    uint8_t out[4096];assert(!hci.copyReadBlock(out,sizeof(out)));
    assert(events.size()==count&&rings==expected_rings&&device[8192]==saved);
    assert(transaction.run(grant,io,dma,{1000,1000,4}).error==s::HandoffError::AlreadyRun);
    assert(events.size()==count); // No retry, rollback, cache touch or free path.
    mapping();
  }
};

static void scenario(unsigned n)
{
  Fixture f;
  if(n==0){
    assert(f.start());assert(apps_smmu_owner_validate_ready(&f.handoff.token,&f.handoff.claim)==APPS_SMMU_OK);
    f.prefix(5);assert(f.commands==std::vector<unsigned>({0,1,2,3,4}));
    assert(f.rings==5&&f.completions==5&&f.smmu_writes==6);
    uint8_t out[4096]{};assert(f.hci.copyReadBlock(out,sizeof(out)));
    for(unsigned i=0;i<sizeof(out);++i)assert(out[i]==knownByte(i));
    assert(f.regs[H+0x20]&0x400);f.stopAndRetain();return;
  }
  if(n==1){
    assert(f.start());auto count=f.events.size();
    assert(f.execute(4).error==r::Error::PolicyRejected&&f.events.size()==count&&f.rings==0);
    f.prefix(4);count=f.events.size();const uint8_t cpu=f.cpu[8192],device=f.device[8192];
    assert(f.execute(4,8).error==r::Error::PolicyRejected&&f.events.size()==count&&f.rings==4);
    assert(f.cpu[8192]==cpu&&f.device[8192]==device&&f.engine.phase()==r::Phase::ReadReady);
    assert(f.execute(4).error==r::Error::None);f.stopAndRetain();return;
  }
  if(n==2||n==3||n==4){
    if(n==2)f.grant.visibility_authorization=0;
    if(n==3)f.regs[H+0x60]=1;
    if(n==4)f.fail_smmu_write=3;
    assert(!f.start()&&!f.rings&&f.commands.empty());
    for(const auto &e:f.events)assert(e.type!='W'||(e.address>=S&&e.address<S+0x100000));
    if(n==2)assert(f.events.empty()&&f.handoff.error==s::HandoffError::InvalidGrant);
    else assert(f.handoff.quarantined&&apps_smmu_owner_release(&f.handoff.token)==APPS_SMMU_QUARANTINED);
    if(n==4)assert(f.smmu_writes==3);return;
  }
  if(n==5){
    f.fail_list_write=true;assert(!f.start()&&f.handoff.published&&!f.rings&&f.hci.failed());
    assert(apps_smmu_owner_release(&f.handoff.token)==APPS_SMMU_QUARANTINED);
    const auto count=f.events.size();assert(!f.hci.setupQueue()&&f.events.size()==count);return;
  }
  assert(f.start());
  if(n>=6&&n<=9){
    f.prefix(4);
    if(n==6){f.no_completion=true;f.frozen_clock=true;}
    if(n==7)f.bad_response=true;
    if(n==8)f.fail_after_ring=true;
    if(n==9)f.fail_data_invalidate=true;
    auto result=f.execute(4,7,2);assert(result.error!=r::Error::None&&result.attempts==1);
    if(n==6)assert(result.error==r::Error::Timeout&&result.polls==8);
    if(n==7)assert(result.error==r::Error::Protocol);
    f.terminal(5);return;
  }
  if(n==10){
    f.prefix(2);f.busy_responses=1;auto result=f.execute(2,7,2);
    assert(result.error==r::Error::None&&result.attempts==2);
    assert(f.execute(3).error==r::Error::None&&f.execute(4).error==r::Error::None);
    assert(f.commands==std::vector<unsigned>({0,1,2,2,3,4})&&f.completions==6);
    f.stopAndRetain();return;
  }
  assert(false);
}
int main()
{
  for(unsigned n=0;n<11;++n){
    const pid_t pid=fork();assert(pid>=0);if(!pid){scenario(n);_exit(0);}
    int status=0;assert(waitpid(pid,&status,0)==pid);
    if(!WIFEXITED(status)||WEXITSTATUS(status))std::fprintf(stderr,"pipeline scenario %u failed\n",n);
    assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
  }
  std::puts("Sm8250UfsPipeline: 11 integrated grant/capture/publication/HCI/command/cache/stop/retained-claim and terminal-failure scenarios passed; no synthetic owner-ready transitions");
}
