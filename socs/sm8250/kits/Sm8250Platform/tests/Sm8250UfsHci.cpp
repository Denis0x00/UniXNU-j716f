// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../Sm8250UfsHci.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>
#ifdef NDEBUG
#error Assertions must remain enabled.
#endif
namespace p = shengufs::read_policy;
namespace r = shengufs::restricted;
namespace s = sm8250_ufs;
static void le32(uint8_t *p, uint32_t x) { for (unsigned i=0;i<4;++i) p[i]=uint8_t(x>>(8*i)); }
static void be32(uint8_t *p, uint32_t x) { for (unsigned i=0;i<4;++i) p[i]=uint8_t(x>>(24-8*i)); }
struct Event { char type; uint64_t address; size_t bytes; uint32_t value; };
struct Fixture {
  alignas(4096) uint8_t cpu[139264];
  alignas(4096) uint8_t device[139264];
  uint32_t regs[0x80/4] = {};
  std::vector<Event> events;
  uint64_t clock = 1;
  size_t fail_at = 0;
  bool no_complete = false, frozen_clock = false, bad_ocs = false, delayed_stop = false;
  bool skip_dma_invalidate = false;
  unsigned pending = 0, rings = 0;
  AppsSmmuClaim claim{};
  AppsSmmuToken token{};
  p::Context context{};
  s::HciPort hci;
  r::SingleSlot engine;
  s::Io io;
  explicit Fixture(uint64_t physical=0x90000000, bool wide=true) :
    io{this,read,write,barrier,ticks,cache}
  {
    std::memset(cpu, 0xac, sizeof(cpu)); std::memset(device, 0xdb, sizeof(device));
    regs[0] = wide ? 1U<<24 : 0;
    regs[0x34/4] = 1; regs[0x30/4] = 0xf;
    regs[0x20/4] = 0x401; // stale UTRCS plus UIC completion: only bit0 may be ACKed.
    claim = {0x8250,1,0x1d84000,0x15000000,2,2,{0xe0,0x4e0},{6,7},3,physical,sizeof(cpu)};
    const AppsSmmuHandoff handoff{1,APPS_SMMU_HANDOFF_REQUIRED,claim};
    assert(apps_smmu_owner_install_handoff(&handoff)==APPS_SMMU_OK);
    assert(apps_smmu_owner_try_acquire(&claim,&token)==APPS_SMMU_OK);
    assert(apps_smmu_owner_publication_begin(&token)==APPS_SMMU_OK);
    assert(apps_smmu_owner_publication_complete(&token)==APPS_SMMU_OK);
    context.selected_lun=3; context.expected_tag=0x3d;
    context.dma={{physical,139264},{physical,32},{physical+4096,32},{physical+4608,512},
                 {physical+5120,16},{physical+8192,131072}};
    context.capacity={true,3,4096,128};
    context.known_blocks[0]={3,7}; context.known_block_count=1;
    assert(engine.configure(context,claim)==r::Error::None);
  }
  bool event(char type,uint64_t address=0,size_t bytes=0,uint32_t value=0)
  {
    events.push_back({type,address,bytes,value});
    return !fail_at || events.size()!=fail_at;
  }
  static bool read(void *v,uint64_t address,uint32_t *out)
  {
    auto &f=*static_cast<Fixture*>(v);
    assert(address>=0x1d84000 && address<=0x1d84078 && !(address&3));
    const unsigned off=unsigned(address-0x1d84000);
    assert(off==0||off==0x20||off==0x24||off==0x30||off==0x34||off==0x50||off==0x54||off==0x58||off==0x60||off==0x78);
    if(!f.event('R',address)) return false;
    if(off==0x58 && f.pending && !f.no_complete && --f.pending==0) f.complete();
    *out=f.regs[off/4]; return true;
  }
  static bool write(void *v,uint64_t address,uint32_t value)
  {
    auto &f=*static_cast<Fixture*>(v);
    const unsigned off=unsigned(address-0x1d84000);
    assert(address>=0x1d84000 && address<=0x1d84060 && !(address&3));
    assert(off==0x20||off==0x50||off==0x54||off==0x58||off==0x60);
    // A failed callback is deliberately ambiguous: apply the MMIO write first.
    if(off==0x20) { assert(value==1); f.regs[off/4]&=~value; }
    else if(off==0x58) { assert(value==1); f.regs[off/4]|=1; f.pending=2; ++f.rings; }
    else if(off==0x60) { assert(value<=1); if(value || !f.delayed_stop) f.regs[off/4]=value; }
    else f.regs[off/4]=value;
    return f.event('W',address,0,value);
  }
  static bool barrier(void *v) { return static_cast<Fixture*>(v)->event('B'); }
  static bool ticks(void *v,uint64_t *out)
  {
    auto &f=*static_cast<Fixture*>(v);
    if(!f.event('T')) return false;
    *out=f.clock; if(!f.frozen_clock) ++f.clock; return true;
  }
  static bool cache(void *v,s::CacheOp op,size_t offset,size_t bytes)
  {
    auto &f=*static_cast<Fixture*>(v);
    assert(bytes && !(offset&63) && !(bytes&63) && offset<sizeof(f.cpu) && bytes<=sizeof(f.cpu)-offset);
    assert(op!=s::CacheOp::Clean); // implementation uses clean+invalidate before DMA.
    const char type=op==s::CacheOp::Invalidate?'I':'C';
    if(!f.event(type,offset,bytes)) return false;
    if(op==s::CacheOp::Invalidate) {
      if(!f.skip_dma_invalidate) std::memcpy(f.cpu+offset,f.device+offset,bytes);
    } else {
      std::memcpy(f.device+offset,f.cpu+offset,bytes);
      // CPU bytes become deliberately unusable until invalidate-only returns
      // the hardware result. A missing direction/order cannot pass by sharing
      // one fake coherent buffer between CPU and simulated controller.
      std::memset(f.cpu+offset,0xe7,bytes);
    }
    return true;
  }
  bool start()
  { return hci.bind(io,{cpu,claim.dma_base,sizeof(cpu)},claim,token) && hci.setupQueue(); }
  p::Request request(unsigned kind, unsigned data_displacement=0) const
  {
    p::Request q{}; q.upiu[3]=0x3d;
    if(kind==1) {q.upiu[0]=0x16;q.upiu[5]=1;q.upiu[12]=3;q.upiu[13]=2;}
    if(kind>=2) {q.upiu[0]=1;q.upiu[2]=3;q.upiu[16]=kind==2?0:kind==3?0x25:0x28;}
    if(kind>=3) {
      q.direction=p::Direction::FromDevice;q.data_bytes=kind==3?8:4096;
      q.upiu[1]=0x40;be32(q.upiu+12,q.data_bytes);q.prd_count=1;
      const uint64_t addr=claim.dma_base+8192+data_displacement;
      le32(q.prd,uint32_t(addr));le32(q.prd+4,uint32_t(addr>>32));le32(q.prd+12,q.data_bytes-1);
      if(kind==4) {be32(q.upiu+18,7);q.upiu[24]=1;}
    }
    return q;
  }
  void descriptorCheck()
  {
    const uint8_t *q=device+4096;
    uint8_t expected[64]{};
    le32(expected,0x10000000U | ((q[1]&0x40)?4U<<24:0));
    le32(expected+8,15);
    le32(expected+16,uint32_t(claim.dma_base+4096));le32(expected+20,uint32_t((claim.dma_base+4096)>>32));
    le32(expected+24,0x00800080);
    le32(expected+28,0x01000000 | ((q[1]&0x40)?1:0));
    assert(std::memcmp(device,expected,64)==0);
    for(unsigned i=4128;i<4608;++i) assert(device[i]==0);
    for(unsigned i=4608;i<5120;++i) assert(device[i]==0);
    for(unsigned i=5136;i<5184;++i) assert(device[i]==0);
    assert(device[64]==0xdb && device[4095]==0xdb && device[5184]==0xdb);
  }
  void complete()
  {
    descriptorCheck();
    regs[0x58/4]=0; regs[0x20/4]|=1;
    le32(device+8,bad_ocs?0x100:0);
    const uint8_t *q=device+4096;
    uint8_t *rsp=device+4608;
    rsp[0]=q[0]==0?0x20:q[0]==0x16?0x36:0x21;
    rsp[2]=q[2];rsp[3]=q[3];
    if(q[0]==0x16) {rsp[5]=1;rsp[12]=3;rsp[13]=2;rsp[23]=0x11;}
    if(q[0]==1) rsp[11]=2;
    if(q[1]&0x40) {
      const uint64_t address=uint64_t(p::loadLe32(device+5120))|(uint64_t(p::loadLe32(device+5124))<<32);
      const size_t offset=size_t(address-claim.dma_base);
      assert(offset>=8192 && offset+q[15]+(size_t(q[14])<<8)<=sizeof(device));
      if(q[16]==0x25) {be32(device+offset,127);be32(device+offset+4,4096);}
      else for(unsigned i=0;i<4096;++i) device[offset+i]=uint8_t(i^0x5a);
    }
  }
  r::Result execute(unsigned kind,unsigned displacement=0)
  { return engine.execute(request(kind,displacement),token,hci.port(),{1000,8,0}); }
  void drain()
  {
    assert(hci.stopQueue(100,8));
    assert(engine.confirmStopped(token,hci.port()).error==r::Error::None);
    assert(apps_smmu_owner_confirm_unpublished(&token)==APPS_SMMU_OK); // Synthetic fixture only.
    assert(apps_smmu_owner_release(&token)==APPS_SMMU_OK);
  }
};

static std::vector<Event> positive(uint64_t base,bool wide,unsigned displacement,bool finish=true)
{
  Fixture f(base,wide); assert(f.start());
  for(unsigned command=0;command<5;++command) {
    const size_t first=f.events.size();
    const auto result=f.execute(command,displacement);
    assert(result.error==r::Error::None);
    bool rung=false, seenAck=false; unsigned cleans=0, invalidates=0;
    for(size_t i=first;i<f.events.size();++i) {
      const auto &e=f.events[i];
      if(e.type=='W' && e.address==0x1d84020) {seenAck=true;assert(e.value==1);}
      if(e.type=='W' && e.address==0x1d84058) {assert(seenAck);rung=true;assert(e.value==1);}
      if(e.type=='C') {assert(!rung);++cleans;}
      if(e.type=='I') {assert(rung);++invalidates;}
      if(e.type=='W') {assert(i>0 && i+1<f.events.size());assert(f.events[i-1].type=='B' && f.events[i+1].type=='B');}
    }
    assert(cleans==(command>=3?3U:2U) && invalidates==(command>=3?3U:2U));
    if(command==3) assert(result.capacity.measured && result.capacity.blocks==128);
    assert(f.regs[0x20/4]&0x400); // Never indiscriminately ACK the saved IS value.
  }
  uint8_t block[4096]{}; assert(f.hci.copyReadBlock(block,sizeof(block)));
  for(unsigned i=0;i<sizeof(block);++i) assert(block[i]==uint8_t(i^0x5a));
  assert(!f.hci.copyReadBlock(f.cpu+8192,4096));
  assert(!f.hci.copyReadBlock(block,4095));
  if(finish) f.drain();
  return f.events;
}
static void isolated(void (*test)(unsigned),unsigned parameter)
{
  const pid_t child=fork(); assert(child>=0);
  if(!child) {test(parameter);_exit(0);}
  int status=0;assert(waitpid(child,&status,0)==child);
  if(!WIFEXITED(status)||WEXITSTATUS(status)!=0) std::fprintf(stderr,"failed case %u\n",parameter);
  assert(WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static void positiveCase(unsigned n)
{
  if(n==0) (void)positive(0x90000000,false,0);
  if(n==1) (void)positive(0x190000000,true,4);
  if(n==2) (void)positive(0xfffde000,false,131072-4096);
}
static void negativeCase(unsigned n)
{
  Fixture f;
  if(n<7) {
    auto io=f.io; auto dma=s::DmaView{f.cpu,f.claim.dma_base,sizeof(f.cpu)};
    auto claim=f.claim; auto token=f.token;
    if(n==0) io.cache=nullptr;
    if(n==1) ++dma.cpu;
    if(n==2) --dma.bytes;
    if(n==3) dma.physical+=4096;
    if(n==4) ++token.lease_id;
    if(n==5) claim.sids[0]=0x60;
    if(n==6) claim.dma_bytes+=4096;
    assert(!f.hci.bind(io,dma,claim,token));assert(f.events.empty());return;
  }
  if(n==7) { // 32-bit CAP cannot accept a range crossing 4 GiB.
    f.claim.dma_base=0xffffe000; // This altered tuple has no lease: no MMIO.
    assert(!f.hci.bind(f.io,{f.cpu,f.claim.dma_base,sizeof(f.cpu)},f.claim,f.token));return;
  }
  if(n>=8 && n<=13) {
    assert(f.hci.bind(f.io,{f.cpu,f.claim.dma_base,sizeof(f.cpu)},f.claim,f.token));
    if(n==8) f.regs[0x58/4]=1;
    if(n==9) f.regs[0x78/4]=1;
    if(n==10) f.regs[0x20/4]|=0x800;
    if(n==11) f.regs[0x24/4]=1;
    if(n==12) f.regs[0x34/4]=0;
    if(n==13) f.regs[0x60/4]=1;
    assert(!f.hci.setupQueue());
    for(const auto&e:f.events)assert(e.type!='W');return;
  }
  assert(f.start());
  if(n>=14 && n<=17) {
    auto request=f.request(0); auto context=f.context;
    if(n==14) request.upiu[0]=4;
    if(n==15) request.direction=p::Direction::ToDevice;
    if(n==16) context.dma.data.address+=4096;
    const auto before=f.events.size();
    if(n!=17) assert(apps_smmu_owner_mark_dma_possible(&f.token)==APPS_SMMU_OK);
    assert(!f.hci.port().prepare(f.hci.port().context,request,context));
    assert(f.events.size()==before);
    for(auto byte:f.cpu)assert(byte==0xac);
    return;
  }
  if(n==18) {f.no_complete=true;f.frozen_clock=true;}
  if(n==19) f.bad_ocs=true;
  if(n==20) f.skip_dma_invalidate=true;
  if(n>=18 && n<=20) {
    assert(f.execute(0).error!=r::Error::None);
    const auto events=f.events.size();assert(f.execute(0).error!=r::Error::None);
    assert(f.events.size()==events);assert(!f.hci.stopQueue(100,8));return;
  }
  if(n==21) {
    assert(f.execute(0).error==r::Error::None);f.delayed_stop=true;f.frozen_clock=true;
    assert(!f.hci.stopQueue(100,3));assert(f.hci.failed());return;
  }
  if(n==22) {
    const auto before=f.events.size();assert(!f.hci.port().ring_slot_zero(&f.hci));
    assert(f.events.size()==before);return;
  }
}
static void addressCase(unsigned n)
{
  const uint64_t base=n==0?0x190000000:0xffffe000;
  Fixture f(base,false);
  assert(!f.hci.bind(f.io,{f.cpu,base,sizeof(f.cpu)},f.claim,f.token));
  assert(f.hci.failed() && f.events.size()==1 && f.events[0].type=='R');
}
static void faultCase(unsigned point)
{
  Fixture f; f.fail_at=point;
  bool success=f.start();
  if(success) success=f.execute(0).error==r::Error::None;
  assert(!success);
  const size_t after=f.events.size();
  assert(!f.hci.setupQueue());assert(!f.hci.stopQueue(10,2));
  assert(f.execute(0).error!=r::Error::None);
  assert(f.events.size()==after); // No reuse/free/cache/MMIO following failure.
}
int main()
{
  for(unsigned i=0;i<3;++i) isolated(positiveCase,i);
  for(unsigned i=0;i<23;++i) isolated(negativeCase,i);
  for(unsigned i=0;i<2;++i) isolated(addressCase,i);
  // Obtain the exact implementation callback count in an isolated process,
  // then inject one failure at every callback along bind/setup/one command.
  int descriptors[2];assert(pipe(descriptors)==0);
  const pid_t child=fork();assert(child>=0);
  if(!child) {
    close(descriptors[0]);Fixture f;assert(f.start());assert(f.execute(0).error==r::Error::None);
    const unsigned count=unsigned(f.events.size());assert(write(descriptors[1],&count,sizeof(count))==sizeof(count));_exit(0);
  }
  close(descriptors[1]);unsigned count=0;assert(read(descriptors[0],&count,sizeof(count))==sizeof(count));close(descriptors[0]);
  int status=0;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
  for(unsigned i=1;i<=count;++i) isolated(faultCase,i);
  std::printf("Sm8250UfsHci: 28 isolated scope/descriptor/cache/address tests and %u callback failures passed\n",count);
}
