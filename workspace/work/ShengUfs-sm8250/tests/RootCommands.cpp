// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real production commands with a deterministic DMA/HC fault fixture.
#include "RootCommandsBridge.cpp"
#include <array>
#include <algorithm>
#include <string>
#include <vector>
enum class Fault { None, Empty, Type, Tag, Lun, Query, Residual, Underflow,
  Oversize, SenseLength, CheckCondition, Timeout, Fatal, Stop, Start, HighBase, Busy, TaskSetFull };
static unsigned busyLeft;      // Busy/TaskSetFull: commands answered with that status before the device accepts again
static unsigned lastSlot;      // the slot of the last command rung
static std::vector<uint8_t> written;   // the data of the last WRITE(10)
static Fault fault;
static std::array<uint8_t,kDmaBytes> device;
static std::array<uint32_t,64> registers;
static uint64_t now;
static unsigned writes, releases;
static bool dataAcquired;     // a data area was taken back from the device (the descriptor and the request list line are smaller)
static bool released;

static uint64_t io(unsigned op,uint64_t address,uint64_t value,void *bytes)
{
  if (op==0) return registers.at(address/4);
  if (op==1) {
    ++writes;
    if (address==0x20) registers[address/4]&=~uint32_t(value);
    else if (!(address==0x60 && ((fault==Fault::Stop && !value) || (fault==Fault::Start && value))) &&
             !(address==0x54 && fault==Fault::HighBase)) registers.at(address/4)=uint32_t(value);
    if (address==0x58) {
      assert(released); released=false;
      // Exactly one doorbell bit, of an even request list entry (RootTransportLayout.h).
      assert(value && !(value&(value-1)));
      const unsigned entry=unsigned(__builtin_ctz(uint32_t(value)));
      assert(entry%2==0 && entry/2<kRegionSlots);
      const unsigned slot=entry/2; lastSlot=slot;
      // The entry points at this slot's command descriptor.
      const uint8_t *utrd=device.data()+ufsSlotUtrdOffset(slot);
      uint64_t ucd=*reinterpret_cast<const uint32_t *>(utrd+16)|uint64_t(*reinterpret_cast<const uint32_t *>(utrd+20))<<32;
      assert(ucd==gHost.phys+ufsSlotUcdOffset(slot));
      if (fault==Fault::Fatal) { registers[0x20/4]=0x20000; return 0; }
      registers[0x58/4]=0;
      if (fault==Fault::Timeout) return 0;
      uint8_t *cmd=device.data()+ufsSlotUcdOffset(slot), *rsp=cmd+kRspOffset;
      if (cmd[0]==1) assert(cmd[3]==entry);            // SCSI task tag: the request list entry
      assert(gSlotOpcode[slot]==cmd[cmd[0]==1?16:12]);   // the log's record of the command in flight
      std::fill(rsp,rsp+512,0);
      if (cmd[0]==1 && cmd[16]==0x2a) {                 // WRITE(10): keep what the data area held
        const uint32_t n=uint32_t(cmd[23])<<8|cmd[24];
        written.assign(device.data()+ufsSlotDataOffset(slot),device.data()+ufsSlotDataOffset(slot)+n*4096);
      }
      if (!(cmd[0]==1 && cmd[16]==0x2a)) memset(device.data()+ufsSlotDataOffset(slot),0x7b+slot,kDataMax);   // a write leaves its buffer
      *reinterpret_cast<uint32_t *>(device.data()+ufsSlotUtrdOffset(slot)+8)=0;
      if (fault==Fault::Empty) return 0;
      memcpy(rsp,cmd,32); rsp[0]=cmd[0]+0x20; rsp[1]=0;
      if (cmd[0]==1) putBe32(rsp+12,0);
      if (cmd[0]==0x16) putBe32(rsp+20,0x11);
      if (fault==Fault::Type) rsp[0]=0x3f;
      if (fault==Fault::Tag) ++rsp[3];
      if (fault==Fault::Lun) ++rsp[2];
      if (fault==Fault::Query) ++rsp[13];
      if (fault==Fault::Residual) putBe32(rsp+12,4096);
      if (fault==Fault::Underflow) rsp[1]=0x20;
      if (fault==Fault::Oversize) rsp[10]=0xff;
      if (fault==Fault::SenseLength) { rsp[11]=2; rsp[33]=18; }
      if ((fault==Fault::Busy || fault==Fault::TaskSetFull) && busyLeft) { --busyLeft; rsp[7]=fault==Fault::Busy?0x08:0x28; }
      if (fault==Fault::CheckCondition) {
        rsp[7]=2; rsp[11]=20; rsp[33]=18; rsp[34]=0x70;
        rsp[36]=6; rsp[46]=0x29; rsp[47]=0;
      }
    }
  }
  if (op==2 || op==3) {
    assert(address>=gHost.phys && address-gHost.phys+value<=device.size());
    auto at=device.data()+(address-gHost.phys);
    if (op==2) { ++releases; memcpy(at,bytes,value); released=true; }
    else { memcpy(bytes,at,value); if (value>=4096) dataAcquired=true; }
  }
  if (op==4) return now;
  if (op==5) now+=value;
  return 0;
}
static void reset(Fault f=Fault::None)
{
  fault=f; device.fill(0); registers.fill(0); now=0; writes=releases=0; released=false; dataAcquired=false;
  registers[0x34/4]=1; registers[0x30/4]=15; registers[0x60/4]=1;
  ufs_test_reset(io,0x8cb5e9000);
}
static void assertQuarantined()
{
  assert(ufs_test_poisoned());
  std::array<uint8_t,kDmaBytes> old; memcpy(old.data(),memory,sizeof(memory));
  unsigned oldWrites=writes, oldReleases=releases;
  uint8_t cdb[6]={}; uint32_t scsi=0, value=0xabcdef;
  assert(ufs_test_nop()!=0);
  assert(ufs_test_query(3,2,&value)!=0 && value==0xabcdef);
  assert(ufs_test_scsi(0,cdb,6,false,0,&scsi)!=0);
  assert(writes==oldWrites && releases==oldReleases);
  assert(!memcmp(old.data(),memory,sizeof(memory)));
}
int main()
{
  reset(); assert(ufs_test_nop()==0);
  uint32_t value=0; assert(ufs_test_query(3,2,&value)==0 && value==0x11);
  std::array<uint8_t,131072> out;
  assert(ufs_test_read(0,1,32,out.data())==0);
  assert(std::all_of(out.begin(),out.end(),[](uint8_t x){return x==0x7b;}));
  assert(dataAcquired && gStagedCommands==1 && gDirectCommands==0);   // the host model has no zero copy
  assert(gSlotOpcode[lastSlot]==0x28 && std::all_of(std::begin(gSlotIssuedAt),std::end(gSlotIssuedAt),[](uint64_t t){return t==0;}));
  assert(*reinterpret_cast<uint32_t *>(device.data()+20)==8);
  assert(*reinterpret_cast<uint32_t *>(device.data()+4096+1024+4)==8);
  assert(ufs_test_read(0,0xffffffffULL,1,out.data())==0); // Last READ(10) LBA is valid.
  auto oldWrites=writes;
  assert(ufs_test_read(0,0xffffffffULL,2,out.data())==kIOReturnBadArgument && writes==oldWrites);
  for (Fault f: {Fault::Empty,Fault::Type,Fault::Tag,Fault::Lun,Fault::Residual,
                Fault::Underflow,Fault::Oversize,Fault::SenseLength,Fault::Timeout,Fault::Fatal}) {
    reset(f); out.fill(0xa5);
    assert(ufs_test_read(0,1,1,out.data())!=0);
    assert(std::all_of(out.begin(),out.end(),[](uint8_t x){return x==0xa5;}));
    assertQuarantined();
    assert(std::all_of(std::begin(gSlotIssuedAt),std::end(gSlotIssuedAt),[](uint64_t t){return t==0;}));   // an ended wait clears it
  }
  reset(Fault::Query); value=0xabcdef;
  assert(ufs_test_query(3,2,&value)!=0 && value==0xabcdef); assertQuarantined();
  reset(Fault::CheckCondition); out.fill(0xa5);
  assert(ufs_test_read(0,1,1,out.data())==kIOReturnNotReady);
  assert(!gHost.poisoned && gRootDiagnostic.sense==0x062900);
  assert(out[0]==0xa5);
  fault=Fault::None; assert(ufs_test_read(0,1,1,out.data())==0);
  reset(); uint8_t cdb[17]={}; uint32_t scsi;
  assert(ufs_test_scsi(0,cdb,17,false,0,&scsi)==kIOReturnBadArgument);
  assert(ufs_test_scsi(0,cdb,6,true,kDataMax+4,&scsi)==kIOReturnBadArgument);
  assert(ufs_test_scsi(0,cdb,6,false,4096,&scsi)==kIOReturnBadArgument); // No writes to media.
  assert(writes==0 && releases==0);
  for (unsigned offset: {0x58U,0x78U}) {
    reset(); registers[offset/4]=1;
    assert(!ufs_test_stop_queue() && writes==0);
  }
  reset(); registers[0x34/4]=0; assert(!ufs_test_stop_queue() && writes==0);
  reset(); registers[0x20/4]=0x20000;
  assert(!ufs_test_stop_queue() && writes==0);
  assert(ufs_test_nop()!=0 && writes==0 && releases==0); assertQuarantined();
  reset(Fault::Stop); assert(!ufs_test_stop_queue() && now>=100000);
  reset(Fault::Start); assert(ufs_test_stop_queue()); assert(!ufs_test_start_queue());
  reset(Fault::HighBase); assert(ufs_test_stop_queue()); assert(!ufs_test_start_queue());
  reset(); assert(ufs_test_stop_queue() && ufs_test_start_queue());
  assert(registers[0x50/4]==0xcb5e9000 && registers[0x54/4]==8);
  // Command slots: each slot's entry, descriptor, tag and data area; the pool; busy retries; the panic slot; one-slot fallback.
  for (unsigned slot=0;slot<kRegionSlots;++slot) {
    reset(); uint8_t read10[10]={0x28,0,0,0,0,1,0,0,1,0}; uint32_t status=0xffff;
    assert(ufs_test_scsi_slot(slot,0,read10,10,4096,&status)==0 && status==0 && lastSlot==slot);
    assert(memory[ufsSlotDataOffset(slot)]==uint8_t(0x7b+slot) && memory[ufsSlotDataOffset(slot)+4095]==uint8_t(0x7b+slot));
  }
  reset();
  assert(ufs_test_slot_count()==kRegionSlots && ufs_test_free_slots()==(1U<<(kRegionSlots-1))-1);   // the last slot is the panic's
  assert(ufs_test_slot_take()==0 && ufs_test_slot_take()==1);
  assert(ufs_test_read(0,7,1,out.data())==0 && lastSlot==2 && out[0]==uint8_t(0x7b+2));   // the first free slot
  assert(ufs_test_free_slots()==(1U<<(kRegionSlots-1))-1-3);                                    // given back after the command
  ufs_test_slot_give(0); ufs_test_slot_give(1);
  assert(ufs_test_store_open(100,64)==1);
  // A write leaves its data to the CPU: only what the device wrote (the response) is taken back after the command.
  dataAcquired=false;
  { std::vector<uint8_t> data(8192,0x55); assert(ufs_test_write(0,110,2,data.data())==0 && written==data); }
  assert(!dataAcquired);
  assert(ufs_test_sync()==0 && ufs_test_free_slots()==(1U<<(kRegionSlots-1))-1);
  reset(Fault::Busy); busyLeft=3; assert(ufs_test_read(0,1,1,out.data())==0 && !gHost.poisoned && gBusyRetries==3);
  reset(Fault::TaskSetFull); busyLeft=5; assert(ufs_test_store_open(100,64)==1);
  { std::vector<uint8_t> data(4096,0x66); assert(ufs_test_write(0,100,1,data.data())==0 && written==data); }
  reset(Fault::Busy); busyLeft=1000; assert(ufs_test_read(0,1,1,out.data())==kIOReturnNotReady && !gHost.poisoned);
  assert(ufs_test_free_slots()==(1U<<(kRegionSlots-1))-1);                                      // a failed command gives its slot back
  reset(); ufs_test_panic(); assert(ufs_test_read(0,1,1,out.data())==0 && lastSlot==kRegionSlots-1);
  assert(ufs_test_free_slots()==(1U<<(kRegionSlots-1))-1);
  reset(); ufs_test_slots(1); assert(ufs_test_slot_count()==1 && ufs_test_free_slots()==1);
  assert(ufs_test_read(0,1,1,out.data())==0 && lastSlot==0 && ufs_test_free_slots()==1);
  ufs_test_panic(); assert(ufs_test_read(0,1,1,out.data())==0 && lastSlot==0);
  puts("PASS: command slots: per-slot entry, descriptor, tag and data; pool; busy and task-set-full retries; panic slot; one-slot fallback");
  puts("PASS: actual command/queue path; malformed DMA responses, bounds, short reads, sense, quarantine, queue refusal and full-width DMA");
}
