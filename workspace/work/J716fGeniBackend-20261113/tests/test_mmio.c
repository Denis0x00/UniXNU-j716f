#include "../J716fGeniMmio.h"
#include <assert.h>
#include <stdio.h>

typedef struct {
 JBMMIO binding; JBIO io; JBMProvider provider;
 unsigned reads,writes,permits,barriers,terminals;
 uint64_t physical; JBPermit grant;
 int failure,reenter,terminal_result;
} Fake;
static unsigned checks;
#define CHECK(x) do {++checks;assert(x);} while(0)
static int permit_cb(void *p,uint64_t lease,JBPermit *out) {
 Fake *f=p;++f->permits;CHECK(lease==7);
 if(f->reenter) {uint32_t value=123;CHECK(!f->io.read32(f->io.context,0x40,&value));CHECK(value==123);}
 *out=f->grant;return f->failure==1?0:1;
}
static int read_cb(void *p,uint64_t lease,uint64_t physical,uint32_t *out) {
 Fake *f=p;++f->reads;CHECK(lease==7);f->physical=physical;*out=0x12345678;return f->failure==2?0:1;
}
static int write_cb(void *p,uint64_t lease,uint64_t physical,uint32_t value) {
 Fake *f=p;++f->writes;CHECK(lease==7);(void)value;f->physical=physical;return f->failure==3?0:1;
}
static int barrier_cb(void *p,uint64_t lease) {Fake *f=p;++f->barriers;CHECK(lease==7);return f->failure==4?0:1;}
static int terminal_cb(void *p,const JBTerminalRecord *r) {Fake *f=p;++f->terminals;CHECK(r->lease_id==7);return f->terminal_result;}
static void setup(Fake *f) {
 *f=(Fake){0};f->grant=(JBPermit){7,JB_MMIO_BASE,JB_MMIO_BYTES,JB_REQUIRED_PERMIT};
 f->provider=(JBMProvider){f,read_cb,write_cb,barrier_cb,permit_cb,terminal_cb};
 CHECK(jbm_bind(&f->binding,&f->provider,7,&f->io));CHECK(!f->permits&&!f->reads&&!f->writes);
}
int main(void) {
 Fake f;uint32_t value;JBPermit grant;JBTerminalRecord terminal_record={0};terminal_record.lease_id=7;
 setup(&f);value=0;CHECK(f.io.read32(f.io.context,0x40,&value));CHECK(value==0x12345678);
 CHECK(f.physical==UINT64_C(0x00a94040));CHECK(f.permits==1 && f.reads==1);
 CHECK(f.io.write32(f.io.context,0x700,0x4d82));CHECK(f.physical==UINT64_C(0x00a94700));
 CHECK(f.io.barrier(f.io.context));CHECK(f.io.permit(f.io.context,7,&grant));CHECK(grant.evidence==JB_REQUIRED_PERMIT);
 CHECK(f.io.terminal(f.io.context,&terminal_record)==0);CHECK(!f.binding.revoked);
 f.terminal_result=1;CHECK(f.io.terminal(f.io.context,&terminal_record)==1);
 f.terminal_result=-1;CHECK(f.io.terminal(f.io.context,&terminal_record)==-1);CHECK(f.binding.revoked);
 unsigned before=f.permits;CHECK(!f.io.barrier(f.io.context));CHECK(f.permits==before);
 /* Exhaustive unaligned offsets across and just beyond the aperture. */
 for(uint32_t offset=1;offset<JB_MMIO_BYTES+4;offset+=4) {
  setup(&f);value=99;CHECK(!f.io.read32(f.io.context,offset,&value));CHECK(value==99);CHECK(!f.reads&&!f.permits);CHECK(f.binding.revoked);
 }
 /* No clock, protocol/mode, power, reset, wrapper, IRQ routing or arbitrary command writes. */
 const uint32_t forbidden[]={0x40,0x48,0x64,0x68,0x258,0x278,0x908,0xe1c,0xe24,0x4000,0xffffffff};
 for(unsigned i=0;i<sizeof forbidden/sizeof *forbidden;++i) {
  setup(&f);CHECK(!f.io.write32(f.io.context,forbidden[i],0));CHECK(!f.writes&&!f.permits);CHECK(f.binding.revoked);
 }
 const uint32_t bad_commands[]={0,1,0xffffffff,(1u<<27)|(0x5cu<<9),(3u<<27)|(0x5du<<9)};
 for(unsigned i=0;i<sizeof bad_commands/sizeof *bad_commands;++i) {
  setup(&f);CHECK(!f.io.write32(f.io.context,0x600,bad_commands[i]));CHECK(!f.writes&&!f.permits);
 }
 for(unsigned bit=0;bit<6;++bit) {
  setup(&f);f.grant.evidence&=~(1u<<bit);value=77;
  CHECK(!f.io.read32(f.io.context,0x40,&value));CHECK(value==77&&!f.reads&&f.binding.revoked);
 }
 for(unsigned which=0;which<3;++which) {
  setup(&f);if(which==0)++f.grant.lease_id;else if(which==1)++f.grant.mmio_base;else --f.grant.mmio_bytes;
  CHECK(!f.io.write32(f.io.context,0x604,4));CHECK(!f.writes&&f.binding.revoked);
 }
 for(int fail=1;fail<=4;++fail) {
  setup(&f);f.failure=fail;value=91;
  if(fail<=2) {CHECK(!f.io.read32(f.io.context,0x40,&value));CHECK(value==91);}
  else if(fail==3) CHECK(!f.io.write32(f.io.context,0x604,2));
  else CHECK(!f.io.barrier(f.io.context));
  CHECK(f.binding.revoked);before=f.permits;CHECK(!f.io.write32(f.io.context,0x604,2));CHECK(before==f.permits);
 }
 setup(&f);f.reenter=1;value=55;CHECK(!f.io.read32(f.io.context,0x40,&value));CHECK(value==55&&!f.reads&&f.binding.revoked);
 setup(&f);jbm_revoke(&f.binding);CHECK(!f.io.permit(f.io.context,7,&grant));CHECK(!f.permits);
 setup(&f);CHECK(!f.io.permit(f.io.context,8,&grant));CHECK(!f.permits&&f.binding.revoked);
 setup(&f);f.provider.terminal=0;CHECK(!jbm_bind(&f.binding,&f.provider,7,&f.io));
 setup(&f);CHECK(jbm_bind(&f.binding,&f.binding.provider,7,&f.io));
 CHECK(f.io.read32(f.io.context,0x40,&value));CHECK(value==0x12345678);
 setup(&f);CHECK(!jbm_bind(&f.binding,&f.provider,7,(JBIO *)(void *)&f.binding));
 CHECK(!f.binding.revoked);CHECK(f.io.read32(f.io.context,0x40,&value));
 printf("MMIO boundary: %u checks passed\n",checks);return 0;
}
