/* SPDX-License-Identifier: BSD-2-Clause */
/* Synthetic register/device models. They are not hardware observations. */
#include "../J716fGeniBackend.h"
#include "../J716fGeniMmio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
static void ck(int yes,const char *what)
{++checks;if(!yes){fprintf(stderr,"FAIL %u: %s\n",checks,what);exit(1);}}
enum {STATUS=0x40,CLOCK=0x48,IFDIS=0x64,FW=0x68,DMA=0x258,SCL=0x278,
 COMMAND=0x600,CONTROL=0x604,IRQ=0x610,CLEAR=0x618,TX=0x700,RX=0x780,
 TXSTATUS=0x800,RXSTATUS=0x804,TXPARAM=0xe24,RXPARAM=0xe28};
enum {DONE=1,CANCEL=16,ABORT=32,ERROR=1<<9};
#define RX_LAST (1u<<31)
#define TX_WATERMARK (1u<<30)

typedef struct {
 uint32_t regs[0x4000/4];
 uint8_t data[JG_MAX_FRAME];
 uint32_t wire_words[128],commands[128];
 unsigned wire_count,command_count,rx_index,rx_count,read_base;
 unsigned mmio,reads,writes,barriers,terminals,permits,fail_at,fail_barrier;
 uint32_t permit_bits;
 int automatic,cleanup_auto,terminal_ok,late_effect,held,permission;
 int reject_terminal;
} Model;
typedef struct {Model m;JBState bus;JBMMIO mapping;JBIO io;uint64_t now;} Fixture;

static int model_read(void *ctx,uint64_t lease,uint64_t pa,uint32_t *value)
{
 Model *m=ctx;ck(lease==55,"provider sees lease");
 ck(pa>=JB_MMIO_BASE && pa<JB_MMIO_BASE+JB_MMIO_BYTES && !(pa&3),"physical bounded aperture");
 unsigned off=(unsigned)(pa-JB_MMIO_BASE);++m->mmio;++m->reads;
 if(m->fail_at && m->mmio==m->fail_at)return 0;
 if(off==RX){
  ck(m->rx_index<m->rx_count,"FIFO read has a model word");
  uint32_t word=0;unsigned pos=m->read_base+4*m->rx_index;
  for(unsigned k=0;k<4;++k)if(pos+k<JG_MAX_FRAME)word|=(uint32_t)m->data[pos+k]<<(8*k);
  ++m->rx_index;
  m->regs[RXSTATUS/4]=(m->regs[RXSTATUS/4]&~0x01ffffffu)|(m->rx_count-m->rx_index);
  *value=word;return 1;
 }
 *value=m->regs[off/4];return 1;
}
static int model_write(void *ctx,uint64_t lease,uint64_t pa,uint32_t value)
{
 Model *m=ctx;ck(lease==55,"write lease");
 ck(pa>=JB_MMIO_BASE && pa<JB_MMIO_BASE+JB_MMIO_BYTES && !(pa&3),"write aperture");
 unsigned off=(unsigned)(pa-JB_MMIO_BASE);++m->mmio;++m->writes;
 /* A failed write is allowed to have reached hardware. */
 if(off==CLEAR)m->regs[IRQ/4]&=~value;
 else m->regs[off/4]=value;
 if(off==COMMAND){
  ck(m->command_count<128,"bounded command log");m->commands[m->command_count++]=value;
  m->regs[STATUS/4]=1;
  if((value>>27)==1){m->held=(value&4)!=0;
   if(m->automatic)m->regs[IRQ/4]|=TX_WATERMARK;
  }else if(m->automatic){
   m->held=0;m->regs[STATUS/4]=0;m->rx_index=0;m->rx_count=m->regs[0x270/4]/4;
   m->read_base=m->wire_words[m->wire_count-1]==0x5d82?16:0;
   m->regs[RXSTATUS/4]=m->rx_count|RX_LAST;
   m->regs[IRQ/4]|=DONE|(1u<<27);
  }
 }
 if(off==TX){
  ck(m->wire_count<128,"bounded wire log");m->wire_words[m->wire_count++]=value;
  if(m->automatic){m->regs[TXSTATUS/4]=0;m->regs[STATUS/4]=0;m->regs[IRQ/4]|=DONE;}
 }
 if(off==CONTROL && m->cleanup_auto){
  m->regs[STATUS/4]=0;m->regs[TXSTATUS/4]=0;m->held=0;
  m->regs[IRQ/4]|=value==4?CANCEL:ABORT;
 }
 return !(m->fail_at && m->mmio==m->fail_at);
}
static int model_barrier(void *ctx,uint64_t lease)
{Model*m=ctx;ck(lease==55,"barrier lease");++m->barriers;return !m->fail_barrier;}
static int model_permit(void *ctx,uint64_t lease,JBPermit *p)
{
 Model*m=ctx;++m->permits;
 *p=(JBPermit){lease,JB_MMIO_BASE,JB_MMIO_BYTES,m->permit_bits};return m->permission;
}
static int model_terminal(void *ctx,const JBTerminalRecord *r)
{
 Model*m=ctx;++m->terminals;
 ck(r->lease_id==55 && r->serial!=0,"terminal keyed local record");
 ck(!(r->status&0x1001u) && !(r->tx_fifo_status&0xfffffffu) &&
    !(r->rx_fifo_status&0x1ffffffu),"terminal only after locally empty engine");
 if(m->reject_terminal)return -1;
 return m->terminal_ok && !m->late_effect && !m->held;
}
static void init(Fixture *f)
{
 memset(f,0,sizeof(*f));f->now=10;
 Model*m=&f->m;m->automatic=1;m->cleanup_auto=1;m->terminal_ok=1;m->permission=1;
 m->permit_bits=JB_REQUIRED_PERMIT;
 m->regs[FW/4]=3u<<8;m->regs[CLOCK/4]=0x21;
 m->regs[SCL/4]=(5u<<20)|(12u<<10)|24u;
 m->regs[TXPARAM/4]=(32u<<24)|(16u<<16);m->regs[RXPARAM/4]=32u<<16;
 for(unsigned i=0;i<JG_MAX_FRAME;++i)m->data[i]=(uint8_t)(i*3+1);
 JBMProvider provider={m,model_read,model_write,model_barrier,model_permit,model_terminal};
 ck(jbm_bind(&f->mapping,&provider,55,&f->io),"real MMIO wrapper binding");
 JBConfig config={55,5,7};
 ck(jb_init(&f->bus,&f->io,&config,7,f->now)==JB_OK,"fresh backend");
 ck(!m->mmio && !m->writes,"binding/init touches no registers");
}
static JTAction act(JTActionKind kind,unsigned length,uint64_t token,uint64_t now)
{
 JTAction a={0};a.kind=kind;a.epoch=7;a.token=token;a.deadline_ms=now+100;
 a.i2c_address=0x5d;a.reg=kind==JT_READ_TAIL?0x825d:0x824d;
 a.length=(uint16_t)length;return a;
}
static JBResult poll(Fixture*f)
{
 unsigned before=f->m.mmio;JBResult r=jb_poll(&f->bus,++f->now);
 ck(f->m.mmio-before<=JB_MMIO_BUDGET,"each poll has <=32 physical accesses");return r;
}
static JTCompletion run(Fixture*f,JTAction a)
{
 JTCompletion c={0};ck(jb_start(&f->bus,&a,f->now)==JB_OK,"start accepted");
 for(unsigned i=0;i<40;++i){(void)poll(f);if(jb_take_completion(&f->bus,&c))return c;}
 ck(0,"bounded normal transaction completion");return c;
}
static void phase(Fixture*f,JBPhase target)
{
 for(unsigned i=0;i<40 && f->bus.phase!=target;++i)(void)poll(f);
 ck(f->bus.phase==target,"expected transport phase");
}
static void start_to(Fixture*f,JTActionKind kind,unsigned length,JBPhase target)
{JTAction a=act(kind,length,1,f->now);ck(jb_start(&f->bus,&a,f->now)==JB_OK,"manual start");phase(f,target);}
static JTAction fence(Fixture*f,uint64_t token)
{
 JTAction a={0};a.kind=JT_BUS_FENCE;a.epoch=7;a.token=token;a.deadline_ms=f->now+100;
 ck(jb_request_fence(&f->bus,&a,f->now)==JB_PENDING,"fence requested");return a;
}
static JTCompletion proof(JTAction a,uint32_t bits)
{JTCompletion c={a.kind,a.epoch,a.token,1,bits,0,0};return c;}

static void normal(void)
{
 Fixture f;JTCompletion c;
 for(unsigned n=0;n<11;++n){
  init(&f);JTActionKind k=n==0?JT_READ_PREFIX:n==10?JT_ACK:JT_READ_TAIL;
  unsigned length=n==0?16:n==10?2:4+8*(n-1);
  c=run(&f,act(k,length,1,f.now));
  ck(c.success && c.evidence==JT_E_IO_TERMINAL,"qualified terminal completion only");
  ck(f.bus.phase==JB_READY && f.m.terminals==1,"ready after qualified terminal");
  ck(f.m.wire_count==1 && f.m.wire_words[0]==(k==JT_READ_TAIL?0x5d82u:0x4d82u),"16-bit BE register and two-zero ACK");
  ck(f.m.command_count==(k==JT_ACK?1u:2u),"one write or repeated-start pair");
  ck((f.m.commands[0]&4u)==(k==JT_ACK?0u:4u),"STOP_STRETCH only read address phase");
  if(k!=JT_ACK){
   ck(f.m.commands[1]==((2u<<27)|(0x5du<<9)),"read emits STOP without stretch");
   ck(c.length==length && c.bytes!=0,"exact read extent");
   ck(!memcmp(c.bytes,f.m.data+(k==JT_READ_TAIL?16:0),length),"FIFO bytes match fixture");
  }else ck(!c.bytes && !c.length,"ACK carries no read payload");
  ck(!jb_take_completion(&f.bus,&c),"completion delivered exactly once");
 }
}
static void streaming(void)
{
 for(unsigned which=0;which<2;++which){
  Fixture f;init(&f);unsigned words=which?17:4,depth=which?16:2;
  f.m.regs[RXPARAM/4]=depth<<16;
  start_to(&f,which?JT_READ_TAIL:JT_READ_PREFIX,which?68:16,JB_RX_WAIT);
  f.m.regs[RXSTATUS/4]=depth;f.m.regs[IRQ/4]=1u<<26;
  (void)poll(&f);
  ck(f.bus.phase==JB_RX_WAIT && f.bus.received==depth*4,"RX partial FIFO batch remains pending");
  f.m.regs[RXSTATUS/4]=(words-depth)|RX_LAST;f.m.regs[IRQ/4]=DONE|(1u<<27);
  (void)poll(&f);(void)poll(&f);JTCompletion c;
  ck(jb_take_completion(&f.bus,&c) && c.success && c.length==words*4,"multi-poll FIFO read completes exact bytes");
  ck(!memcmp(c.bytes,f.m.data+(which?16:0),c.length),"streamed payload preserves every word");
 }
}
static void rejects(void)
{
 Fixture f;
 for(unsigned i=0;i<8;++i){init(&f);JTAction a=act(JT_ACK,2,1,f.now);
  switch(i){case 0:a.i2c_address=0x49;break;case 1:a.reg=0x8044;break;
   case 2:a.length=1;break;case 3:a.write[0]=1;break;case 4:a.write[1]=1;break;
   case 5:a.kind=JT_READ_PREFIX;a.length=84;break;
   case 6:a.kind=JT_READ_TAIL;a.reg=0x825d;a.length=8;break;default:a.kind=JT_PUBLISH;break;}
  ck(jb_start(&f.bus,&a,f.now)==JB_BAD_ARGUMENT && !f.m.mmio,"whitelist rejects before MMIO");
 }
 for(unsigned bit=0;bit<6;++bit){init(&f);f.m.permit_bits&=~(1u<<bit);
  JTAction a=act(JT_READ_PREFIX,16,1,f.now);
  ck(jb_start(&f.bus,&a,f.now)==JB_PERMISSION && !f.m.mmio,"missing owner condition denies start");
 }
 for(unsigned i=0;i<15;++i){init(&f);
  switch(i){case 0:f.m.regs[IFDIS/4]=1;break;case 1:f.m.regs[IFDIS/4]=2;break;
   case 2:f.m.regs[DMA/4]=1;break;case 3:f.m.regs[FW/4]=1<<8;break;
   case 4:f.m.regs[TXPARAM/4]=32u<<24;break;
   case 5:f.m.regs[TXPARAM/4]=(32u<<24)|(1u<<16);break;
   case 6:f.m.regs[RXPARAM/4]=0;break;case 7:f.m.regs[RXPARAM/4]=64u<<16;break;
   case 8:f.m.regs[TXPARAM/4]=(16u<<24)|(16u<<16);break;
   case 9:f.m.regs[CLOCK/4]=0;break;case 10:f.m.regs[SCL/4]=0;break;
   case 11:f.m.regs[STATUS/4]=1;break;
   case 12:f.m.regs[CLOCK/4]=1;break;
   case 13:f.m.regs[SCL/4]=0x80000000u;break;
   default:f.m.regs[SCL/4]=1;break;}
  JTCompletion c=run(&f,act(JT_READ_PREFIX,16,1,f.now));
  ck(!c.success && !c.evidence && !f.m.writes,"inherited unsupported/busy mode no register writes");
 }
 for(unsigned rate=0;rate<3;++rate){init(&f);
  static const unsigned d[]={7,2,1},h[]={10,5,3},l[]={11,12,9},cy[]={26,24,18};
  f.m.regs[CLOCK/4]=1|(d[rate]<<4);f.m.regs[SCL/4]=(h[rate]<<20)|(l[rate]<<10)|cy[rate];
  JTCompletion c=run(&f,act(JT_ACK,2,1,f.now));ck(c.success,"all three stock inherited timing shapes accepted");
 }
}
static void status_and_lengths(void)
{
 Fixture f;JTCompletion c;
 init(&f);f.m.automatic=0;start_to(&f,JT_READ_PREFIX,16,JB_TX_WAIT);
 f.m.regs[IRQ/4]=DONE|ERROR|TX_WATERMARK;
 unsigned writes=f.m.writes;(void)poll(&f);
 ck(f.bus.phase==JB_CANCEL_ISSUE && f.m.writes==writes,"error wins over DONE and FIFO feed");
 ck(jb_take_completion(&f.bus,&c) && !c.success && !c.evidence,"failure is not terminal evidence");
 for(unsigned i=0;i<5;++i){init(&f);start_to(&f,JT_READ_PREFIX,16,JB_RX_WAIT);
  switch(i){case 0:f.m.regs[RXSTATUS/4]=33|RX_LAST;break;
   case 1:f.m.regs[RXSTATUS/4]=1|RX_LAST;break;
   case 2:f.m.regs[RXSTATUS/4]=4|RX_LAST|(3u<<28);break;
   case 3:f.m.regs[RXSTATUS/4]=4|RX_LAST|(7u<<28);break;
   default:f.m.regs[IRQ/4]=DONE|ERROR;break;}
  unsigned reads=f.m.rx_index;(void)poll(&f);
  ck(f.bus.phase==JB_CANCEL_ISSUE && f.m.rx_index==reads,"bad count/short/last/error rejected before FIFO");
  ck(jb_take_completion(&f.bus,&c)&&!c.success,"malformed RX fails");
 }
 init(&f);f.m.automatic=0;start_to(&f,JT_ACK,2,JB_TX_WAIT);
 f.m.regs[IRQ/4]=TX_WATERMARK;f.m.regs[TXSTATUS/4]=17;
 (void)poll(&f);ck(f.bus.phase==JB_CANCEL_ISSUE && !f.m.wire_count,"TX count bounded by observed depth");
}
static void terminal_and_fence(void)
{
 Fixture f;JTCompletion c;
 init(&f);f.m.terminal_ok=0;start_to(&f,JT_ACK,2,JB_TERMINAL_WAIT);
 for(unsigned i=0;i<3;++i)(void)poll(&f);
 ck(!jb_take_completion(&f.bus,&c),"local inactive plus DONE is not electrical proof");
 f.m.terminal_ok=1;f.m.late_effect=1;(void)poll(&f);
 ck(!jb_take_completion(&f.bus,&c),"possible late target effect blocks success");
 f.m.late_effect=0;(void)poll(&f);
 ck(jb_take_completion(&f.bus,&c) && c.success,"external terminal proof completes");

 init(&f);f.m.automatic=0;start_to(&f,JT_ACK,2,JB_TX_WAIT);
 f.now=f.bus.action.deadline_ms;(void)jb_poll(&f.bus,f.now);
 ck(jb_take_completion(&f.bus,&c) && !c.success && !c.evidence,"deadline failure has no terminal bits");
 JTAction a=fence(&f,2);phase(&f,JB_LOCAL_QUIESCENT);
 ck(jb_fence_ready(&f.bus,0),"cancel acknowledgement plus drain is local only");
 for(unsigned bit=1;bit<=4;++bit){c=proof(a,JT_BUS_FENCE_EVIDENCE&~(1u<<bit));
  ck(jb_finish_fence(&f.bus,&c,++f.now)==JB_NEEDS_EXTERNAL_FENCE,"every external fence bit required");
 }
 c=proof(a,JT_BUS_FENCE_EVIDENCE);c.length=1;
 ck(jb_finish_fence(&f.bus,&c,++f.now)==JB_NEEDS_EXTERNAL_FENCE && f.bus.phase==JB_LOCAL_QUIESCENT,"payload fence cannot release");
 c.length=0;c.token++;
 ck(jb_finish_fence(&f.bus,&c,++f.now)==JB_STALE,"stale fence identity ignored");
 c.token--;ck(jb_finish_fence(&f.bus,&c,++f.now)==JB_OK,"all external evidence and fresh local observation release");
 ck(f.bus.phase==JB_READY && !f.bus.fence_pending,"reuse only after full fence");
 f.m.automatic=1;c=run(&f,act(JT_ACK,2,3,f.now));ck(c.success,"new operation after full fence");

 init(&f);f.m.regs[RXPARAM/4]=63u<<16;f.m.automatic=0;
 start_to(&f,JT_ACK,2,JB_TX_WAIT);f.now=f.bus.action.deadline_ms;
 (void)jb_poll(&f.bus,f.now);(void)jb_take_completion(&f.bus,&c);
 f.m.rx_count=63;f.m.rx_index=0;f.m.regs[RXSTATUS/4]=63|RX_LAST;
 a=fence(&f,2);phase(&f,JB_LOCAL_QUIESCENT);
 ck(f.bus.cleanup_words==63,"cleanup drains bounded chunks across calls");
 c=proof(a,JT_BUS_FENCE_EVIDENCE);ck(jb_finish_fence(&f.bus,&c,++f.now)==JB_OK,"drained FIFO still requires full external fence");
}
static void quarantine(void)
{
 Fixture f;JTCompletion c;
 init(&f);f.m.automatic=0;f.m.cleanup_auto=0;start_to(&f,JT_ACK,2,JB_TX_WAIT);
 f.now=f.bus.action.deadline_ms;(void)jb_poll(&f.bus,f.now);
 (void)jb_take_completion(&f.bus,&c);JTAction a=fence(&f,2);
 phase(&f,JB_CANCEL_WAIT);
 f.m.regs[IRQ/4]=DONE;f.m.regs[STATUS/4]=0;(void)poll(&f);
 ck(f.bus.phase==JB_CANCEL_WAIT,"late DONE does not acknowledge cancel");
 f.now=f.bus.recovery_deadline;(void)jb_poll(&f.bus,f.now);phase(&f,JB_ABORT_WAIT);
 f.now=f.bus.recovery_deadline;(void)jb_poll(&f.bus,f.now);
 ck(f.bus.phase==JB_ISOLATED && !jb_fence_ready(&f.bus,0),"unacknowledged abort permanently isolates even inactive");
 c=proof(a,JT_BUS_FENCE_EVIDENCE);
 ck(jb_finish_fence(&f.bus,&c,++f.now)==JB_QUARANTINED,"external bool cannot override missing local termination");
 unsigned accesses=f.m.mmio;f.m.regs[IRQ/4]=ABORT;(void)poll(&f);
 ck(f.m.mmio==accesses,"late abort status cannot revive isolated lease");

 for(unsigned aborting=0;aborting<2;++aborting){
  init(&f);f.m.automatic=0;f.m.cleanup_auto=0;start_to(&f,JT_ACK,2,JB_TX_WAIT);
  f.now=f.bus.action.deadline_ms;(void)jb_poll(&f.bus,f.now);phase(&f,JB_CANCEL_WAIT);
  if(aborting){f.now=f.bus.recovery_deadline;(void)jb_poll(&f.bus,f.now);phase(&f,JB_ABORT_WAIT);}
  f.m.regs[IRQ/4]=(aborting?ABORT:CANCEL)|ERROR;(void)poll(&f);
  ck(f.bus.phase==(aborting?JB_ISOLATED:JB_ABORT_ISSUE),"new error wins over recovery ACK");
 }
 init(&f);start_to(&f,JT_ACK,2,JB_TERMINAL_WAIT);(void)poll(&f);
 ck(f.bus.completion_ready && f.bus.completion_success,"success pending delivery");
 ck(jb_poll(&f.bus,f.now-1)==JB_CLOCK_ERROR,"clock reversal isolates");
 ck(jb_take_completion(&f.bus,&c) && !c.success && !c.evidence,"isolation invalidates undelivered success");
 a=act(JT_ACK,2,2,f.now);a.epoch=8;
 ck(jb_start(&f.bus,&a,f.now)==JB_STALE,"new epoch cannot silently reset fault");

 init(&f);c=run(&f,act(JT_ACK,2,1,f.now));a=fence(&f,2);
 ck(jb_request_fence(&f.bus,&a,f.now-1)==JB_CLOCK_ERROR,"same fence retry checks time");
}
static void access_failures(void)
{
 Fixture baseline;init(&baseline);JTCompletion c=run(&baseline,act(JT_READ_PREFIX,16,1,baseline.now));
 ck(c.success,"baseline for fault locations");unsigned total=baseline.m.mmio;
 for(unsigned i=1;i<=total;++i){Fixture f;init(&f);f.m.fail_at=i;
  c=run(&f,act(JT_READ_PREFIX,16,1,f.now));
  ck(!c.success && !c.evidence && f.bus.phase==JB_ISOLATED,"every failed MMIO isolates with no success");
  unsigned n=f.m.mmio;(void)poll(&f);ck(f.m.mmio==n,"no access after failed read/write");
 }
 Fixture f;init(&f);f.m.fail_barrier=1;c=run(&f,act(JT_ACK,2,1,f.now));
 ck(!c.success && f.bus.phase==JB_ISOLATED,"failed ordering barrier isolates");
 init(&f);start_to(&f,JT_ACK,2,JB_TX_WAIT);f.m.permission=0;
 unsigned n=f.m.mmio;(void)poll(&f);
 ck(f.bus.phase==JB_ISOLATED && f.m.mmio==n,"revoked permit stops before hardware access");
}
static void make_frame(Model*m,unsigned count,unsigned first)
{
 memset(m->data,0,sizeof(m->data));m->data[0]=0x80;m->data[1]=(uint8_t)count;
 unsigned wire=12+(count>1?(count-1)*8:0);uint8_t sum=0;
 for(unsigned i=0;i<count;++i){uint8_t*p=m->data+2+i*8;
  p[0]=(uint8_t)(first+i);p[1]=10;p[3]=20;p[5]=1;}
 for(unsigned i=1;i<wire;++i)sum=(uint8_t)(sum+m->data[i]);
 m->data[wire-1]=(uint8_t)(m->data[wire-1]-sum);
}
static JTAction bridge_until(Fixture*f,JBBridge*b,JTActionKind kind)
{
 JTAction a={0};
 for(unsigned i=0;i<160;++i){unsigned n=f->m.mmio;
  (void)jb_bridge_step(b,7,++f->now,&a);
  ck(f->m.mmio-n<=JB_MMIO_BUDGET,"bridge call preserves physical MMIO budget");
  if(a.kind==kind)return a;
  ck(a.kind==JT_ACTION_NONE,"no unexpected external action");
 }
 ck(0,"bridge reaches expected external action");return a;
}
static void bridge(void)
{
 Fixture f;JTState owner;JBBridge b;JTConfig config={100,50,100,1000};
 init(&f);ck(jt_init(&owner,7,f.now,&config)==JT_OK,"real transaction owner initialized");
 ck(jb_bridge_init(&b,&f.bus,&owner)==JB_OK,"real bridge initialized");
 make_frame(&f.m,0,0);ck(jt_signal_frame(&owner,7,f.now)==JT_OK,"initial zero signal");
 for(unsigned i=0;i<80 && !owner.core.armed;++i){JTAction a;
  (void)jb_bridge_step(&b,7,++f.now,&a);ck(a.kind==JT_ACTION_NONE,"zero rearm needs no synthetic HID");}
 ck(owner.core.armed && owner.packets_acked==1,"real FIFO ACK arms frozen core");
 make_frame(&f.m,10,1);ck(jt_signal_frame(&owner,7,++f.now)==JT_OK,"ten finger signal");
 JTAction a=bridge_until(&f,&b,JT_PUBLISH);
 ck(a.snapshot.active_mask==1023 && owner.delivered.active_mask==0,"publication passed through before sink success");
 JTCompletion c=proof(a,0);
 ck(jb_bridge_complete_external(&b,++f.now,&c)!=JT_OK,"missing real HID applied evidence rejected");
 ck(owner.delivered.active_mask==0,"no false HID completion");
 a=bridge_until(&f,&b,JT_SINK_FENCE);c=proof(a,JT_SINK_FENCE_EVIDENCE);
 ck(jb_bridge_complete_external(&b,++f.now,&c)==JT_OK,"external sink fence passed through");
 a=bridge_until(&f,&b,JT_PUBLISH);ck(a.snapshot.active_mask==0,"failed report requires explicit all-up");
 c=proof(a,JT_E_SINK_APPLIED_ORDERED);
 ck(jb_bridge_complete_external(&b,++f.now,&c)==JT_OK,"external final release applied");
 ck(owner.delivered.active_mask==0,"no held contacts after final release");

 /* A physically late completion cannot release a new bus fence token. */
 init(&f);f.m.automatic=0;
 ck(jt_init(&owner,7,f.now,&config)==JT_OK && jb_bridge_init(&b,&f.bus,&owner)==JB_OK,"fresh late bridge");
 ck(jt_signal_frame(&owner,7,f.now)==JT_OK,"late frame signal");
 JTAction none;
 for(unsigned i=0;i<8 && f.bus.phase!=JB_TX_WAIT;++i)(void)jb_bridge_step(&b,7,++f.now,&none);
 ck(f.bus.phase==JB_TX_WAIT,"old physical work pending");
 JTAction old=f.bus.action;f.now=old.deadline_ms;
 (void)jb_bridge_step(&b,7,f.now,&none);
 a=bridge_until(&f,&b,JT_BUS_FENCE);
 ck(a.token!=old.token && owner.bus_dirty,"new fence distinct from timed-out read");
 c=proof(old,JT_E_IO_TERMINAL);
 ck(jb_bridge_complete_external(&b,++f.now,&c)==JT_STALE,"old completion cannot impersonate fence");
 c=proof(a,JT_BUS_FENCE_EVIDENCE);c.length=1;
 ck(jb_bridge_complete_external(&b,++f.now,&c)==JT_FENCE_REQUIRED && f.bus.phase==JB_LOCAL_QUIESCENT,"bridge rejects payload fence before physical reuse");
 c.length=0;f.now=a.deadline_ms;
 ck(jb_bridge_complete_external(&b,f.now,&c)==JT_TIMEOUT && f.bus.phase==JB_LOCAL_QUIESCENT,"late external fence cannot clear quarantine");
}
int main(void)
{
 normal();streaming();rejects();status_and_lengths();terminal_and_fence();quarantine();access_failures();bridge();
 printf("GENI backend/real MMIO bridge: %u checks passed\n",checks);return 0;
}
