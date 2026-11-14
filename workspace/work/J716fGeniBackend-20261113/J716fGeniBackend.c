/* SPDX-License-Identifier: BSD-2-Clause */
#include "J716fGeniBackend.h"
enum {
 R_STATUS=0x40, R_CLOCK=0x48, R_IF_DISABLE=0x64, R_FW=0x68,
 R_BYTE_GRAN=0x254, R_DMA=0x258, R_TX_PACK0=0x260, R_TX_PACK1=0x264,
 R_TX_LEN=0x26c, R_RX_LEN=0x270, R_SCL=0x278,
 R_RX_PACK0=0x284, R_RX_PACK1=0x288, R_COMMAND=0x600, R_CONTROL=0x604,
 R_IRQ=0x610, R_IRQ_ENABLE=0x614, R_IRQ_CLEAR=0x618,
 R_TX_FIFO=0x700, R_RX_FIFO=0x780, R_TX_STATUS=0x800, R_RX_STATUS=0x804,
 R_TX_WATERMARK=0x80c, R_RX_WATERMARK=0x810, R_RX_RFR=0x814,
 R_TX_PARAM=0xe24, R_RX_PARAM=0xe28
};
#define M_ACTIVE ((1u<<0)|(1u<<12))
#define IRQ_DONE (1u<<0)
#define IRQ_CANCEL (1u<<4)
#define IRQ_ABORT (1u<<5)
#define IRQ_RX_WATERMARK (1u<<26)
#define IRQ_RX_LAST (1u<<27)
#define IRQ_TX_WATERMARK (1u<<30)
#define IRQ_NORMAL (IRQ_DONE|IRQ_RX_WATERMARK|IRQ_RX_LAST|IRQ_TX_WATERMARK)
#define IRQ_ERRORS (~IRQ_NORMAL)
#define TX_COUNT(v) ((v)&0x0fffffffu)
#define RX_COUNT(v) ((v)&0x01ffffffu)
#define RX_LAST (1u<<31)

static int io_phase(JBPhase p)
{
 return p>=JB_PRECHECK && p<=JB_TERMINAL_WAIT;
}
static void failure_completion(JBState *s)
{
 if (s->completion_ready) s->completion_success=0;
 if (!s->completion_taken && !s->completion_ready && s->action.token) {
  s->completion_ready=1; s->completion_success=0;
 }
}
static JBResult isolate(JBState *s,JBResult reason)
{
 s->fault=reason; s->phase=JB_ISOLATED; s->local_proven=0;
 failure_completion(s);
 return reason;
}
static int access(JBState *s)
{
 JBPermit p={0};
 if (s->io.permit(s->io.context,s->config.lease_id,&p)!=1 ||
     p.lease_id!=s->config.lease_id || p.mmio_base!=JB_MMIO_BASE ||
     p.mmio_bytes!=JB_MMIO_BYTES ||
     (p.evidence&JB_REQUIRED_PERMIT)!=JB_REQUIRED_PERMIT) {
  (void)isolate(s,JB_PERMISSION); return 0;
 }
 return 1;
}
static int rd(JBState *s,uint32_t offset,uint32_t *v)
{
 if (s->mmio_used>=JB_MMIO_BUDGET || offset>=JB_MMIO_BYTES || (offset&3u)) {
  (void)isolate(s,JB_MMIO_ERROR); return 0;
 }
 ++s->mmio_used;
 if (s->io.read32(s->io.context,offset,v)!=1) {
  (void)isolate(s,JB_MMIO_ERROR); return 0;
 }
 return 1;
}
static int wr(JBState *s,uint32_t offset,uint32_t v)
{
 if (s->mmio_used>=JB_MMIO_BUDGET || offset>=JB_MMIO_BYTES || (offset&3u)) {
  (void)isolate(s,JB_MMIO_ERROR); return 0;
 }
 ++s->mmio_used;
 if (s->io.write32(s->io.context,offset,v)!=1) {
  (void)isolate(s,JB_MMIO_ERROR); return 0;
 }
 return 1;
}
static int barrier(JBState *s)
{
 if (++s->barrier_used>4u || s->io.barrier(s->io.context)!=1) {
  (void)isolate(s,JB_MMIO_ERROR); return 0;
 }
 return 1;
}
static JBResult time_check(JBState *s,uint64_t now)
{
 s->mmio_used=0; s->barrier_used=0;
 if (s->clock_fault) return JB_CLOCK_ERROR;
 if (now<s->last_ms) {
  s->clock_fault=1; return isolate(s,JB_CLOCK_ERROR);
 }
 s->last_ms=now; return JB_OK;
}
static int deadline(JBState *s,uint64_t now,uint32_t duration)
{
 if (UINT64_MAX-now<duration) {
  (void)isolate(s,JB_TIME_RANGE); return 0;
 }
 s->recovery_deadline=now+duration; return 1;
}
static int action_identity(const JTAction *a,const JTCompletion *c)
{
 return a->kind==c->kind && a->epoch==c->epoch && a->token==c->token;
}
static int same_action(const JTAction *a,const JTAction *b)
{
 return a->kind==b->kind && a->epoch==b->epoch && a->token==b->token &&
        a->deadline_ms==b->deadline_ms;
}
static int whitelisted(const JTAction *a)
{
 if (a->i2c_address!=JT_I2C_ADDRESS) return 0;
 switch(a->kind) {
 case JT_READ_PREFIX: return a->reg==JT_COORDINATE_REGISTER && a->length==16;
 case JT_READ_TAIL:
  return a->reg==JT_COORDINATE_REGISTER+16 && a->length>=4 &&
         a->length<=68 && a->length%8==4;
 case JT_ACK:
  return a->reg==JT_COORDINATE_REGISTER && a->length==2 &&
         a->write[0]==0 && a->write[1]==0;
 default: return 0;
 }
}

JBResult jb_init(JBState *s,const JBIO *io,const JBConfig *config,
                 uint64_t epoch,uint64_t now)
{
 if (!s || !io || !config || !epoch || !config->lease_id ||
     !io->read32 || !io->write32 || !io->barrier || !io->permit || !io->terminal ||
     !config->cancel_timeout_ms || !config->abort_timeout_ms ||
     config->cancel_timeout_ms>JB_MAX_RECOVERY_MS ||
     config->abort_timeout_ms>JB_MAX_RECOVERY_MS) return JB_BAD_ARGUMENT;
 /* Copy inputs first; callers still must obey the documented no-alias rule. */
 JBIO saved_io=*io; JBConfig saved_config=*config;
 *s=(JBState){0}; s->io=saved_io; s->config=saved_config;
 s->epoch=epoch; s->last_ms=now; s->initialized=1; return JB_OK;
}

JBResult jb_start(JBState *s,const JTAction *a,uint64_t now)
{
 if (!s || !s->initialized || !a || !whitelisted(a) || !a->token)
  return JB_BAD_ARGUMENT;
 if (a->epoch!=s->epoch || a->token<=s->last_token) return JB_STALE;
 if (s->phase!=JB_READY || s->fence_pending || s->completion_ready) return JB_BUSY;
 JBResult t=time_check(s,now); if(t!=JB_OK)return t;
 if (a->deadline_ms<=now || a->deadline_ms-now>JT_MAX_TIMEOUT_MS) return JB_BAD_ARGUMENT;
 if (!access(s)) return s->fault;
 s->action=*a; s->last_token=a->token; s->phase=JB_PRECHECK;
 s->received=0; s->tx_length=a->kind==JT_ACK?4:2;
 s->tx[0]=(uint8_t)(a->reg>>8); s->tx[1]=(uint8_t)a->reg;
 s->tx[2]=0; s->tx[3]=0;
 s->tx_sent=0; s->done_seen=0; s->last_seen=0; s->cleanup_ack=0;
 s->completion_ready=0; s->completion_taken=0; s->completion_success=0;
 s->local_proven=0; s->cleanup_words=0; s->fault=JB_OK;
 for(unsigned i=0;i<JG_MAX_FRAME;++i)s->bytes[i]=0;
 return JB_OK;
}

static JBResult cancel(JBState *s,JBResult reason)
{
 s->fault=reason; s->local_proven=0; failure_completion(s);
 s->phase=JB_CANCEL_ISSUE; return reason;
}
static int fifo_shape(uint32_t param,uint32_t *depth,int require_width)
{
 /* The stock driver uses six depth bits. Reject wider-generation fields;
  * never guess sixteen when hardware reads zero. Only 32-bit FIFO words. */
 uint32_t d=(param>>16)&0xffu,w=(param>>24)&0x3fu;
 if (d<2 || d>JB_MAX_FIFO_WORDS || (require_width && w!=32u)) return 0;
 *depth=d; return 1;
}
static int timing_shape(uint32_t clock,uint32_t scl)
{
 /* Only the stock 19.2MHz-source table. Owner POWER_CLOCK proof must still
  * establish that source/rate; matching divider/counters alone cannot. */
 if(!(clock&1u) || (scl&0xc0000000u))return 0;
 uint32_t divider=(clock>>4)&0xfffu;
 uint32_t high=(scl>>20)&0x3ffu,low=(scl>>10)&0x3ffu,cycle=scl&0x3ffu;
 return (divider==7 && high==10 && low==11 && cycle==26) ||
        (divider==2 && high==5 && low==12 && cycle==24) ||
        (divider==1 && high==3 && low==9 && cycle==18);
}
static JBResult precheck(JBState *s)
{
 uint32_t fw,disabled,clock,scl,txp,rxp,status,dma,irq,tx,rx;
 if (!rd(s,R_FW,&fw) || !rd(s,R_IF_DISABLE,&disabled) ||
     !rd(s,R_CLOCK,&clock) || !rd(s,R_SCL,&scl) ||
     !rd(s,R_TX_PARAM,&txp) || !rd(s,R_RX_PARAM,&rxp) ||
     !rd(s,R_STATUS,&status) || !rd(s,R_DMA,&dma) || !rd(s,R_IRQ,&irq) ||
     !rd(s,R_TX_STATUS,&tx) || !rd(s,R_RX_STATUS,&rx)) return s->fault;
 if (((fw>>8)&255u)!=3 || disabled!=0 || dma!=0 || !timing_shape(clock,scl) ||
     !fifo_shape(txp,&s->tx_depth,1) || !fifo_shape(rxp,&s->rx_depth,0))
  return isolate(s,JB_UNSUPPORTED);
 if ((status&M_ACTIVE) || TX_COUNT(tx) || RX_COUNT(rx)) return isolate(s,JB_QUARANTINED);
 if (irq&IRQ_ERRORS) return isolate(s,JB_STATUS_ERROR);
 s->last_irq=irq; s->phase=JB_CONFIGURE; return JB_PENDING;
}
static JBResult configure(JBState *s)
{
 /* No DMA or top-level interrupt routing write. Owner must already have
  * proven the polling route. Status polling with master IRQ_EN=0 follows
  * the generic GENI console precedent, not a J716F hardware qualification. */
 const uint32_t regs[]={R_IRQ_ENABLE,R_BYTE_GRAN,R_TX_PACK0,R_TX_PACK1,
                        R_RX_PACK0,R_RX_PACK1,R_TX_WATERMARK,R_RX_WATERMARK,R_RX_RFR};
 const uint32_t values[]={0,0,0x7f8fe,0xffefe,0x7f8fe,0xffefe,1,
                          s->rx_depth>1?s->rx_depth-1:1,s->rx_depth};
 for(unsigned i=0;i<9;++i)if(!wr(s,regs[i],values[i]))return s->fault;
 if(!barrier(s))return s->fault;
 for(unsigned i=0;i<9;++i){uint32_t v;if(!rd(s,regs[i],&v))return s->fault;
  if(v!=values[i])return isolate(s,JB_STATUS_ERROR);}
 uint32_t irq;
 if(!rd(s,R_IRQ,&irq))return s->fault;
 if(irq&IRQ_ERRORS)return isolate(s,JB_STATUS_ERROR);
 if(irq && !wr(s,R_IRQ_CLEAR,irq))return s->fault;
 if(!barrier(s) || !rd(s,R_IRQ,&irq))return s->fault;
 if(irq&~(IRQ_TX_WATERMARK|IRQ_RX_WATERMARK))return isolate(s,JB_STATUS_ERROR);
 s->phase=JB_TX_ISSUE; return JB_PENDING;
}
static JBResult tx_issue(JBState *s)
{
 uint32_t status,tx,rx,irq;
 if(!rd(s,R_STATUS,&status)||!rd(s,R_TX_STATUS,&tx)||!rd(s,R_RX_STATUS,&rx)||
    !rd(s,R_IRQ,&irq))return s->fault;
 if((status&M_ACTIVE)||TX_COUNT(tx)||RX_COUNT(rx)||
    (irq&~IRQ_TX_WATERMARK))return isolate(s,JB_STATUS_ERROR);
 uint32_t command=(1u<<27)|(JT_I2C_ADDRESS<<9);
 if(s->action.kind!=JT_ACK)command|=4u; /* STOP_STRETCH: repeated start */
 if(!wr(s,R_TX_LEN,s->tx_length)||!wr(s,R_COMMAND,command)||!barrier(s))return s->fault;
 s->phase=JB_TX_WAIT;return JB_PENDING;
}
static JBResult tx_wait(JBState *s)
{
 uint32_t irq,tx;
 if(!rd(s,R_IRQ,&irq))return s->fault;
 s->last_irq=irq;
 if(irq&IRQ_ERRORS)return cancel(s,JB_STATUS_ERROR);
 if((irq&(IRQ_RX_LAST|IRQ_RX_WATERMARK)) || ((irq&IRQ_DONE)&&!s->tx_sent))
  return cancel(s,JB_STATUS_ERROR);
 if((irq&IRQ_TX_WATERMARK)&&!s->tx_sent){
  if(!rd(s,R_TX_STATUS,&tx))return s->fault;
  if(TX_COUNT(tx)>s->tx_depth || TX_COUNT(tx)!=0)return cancel(s,JB_FIFO_ERROR);
  uint32_t word=(uint32_t)s->tx[0]|((uint32_t)s->tx[1]<<8)|
                ((uint32_t)s->tx[2]<<16)|((uint32_t)s->tx[3]<<24);
  if(!wr(s,R_TX_FIFO,word)||!wr(s,R_TX_WATERMARK,0)||!barrier(s))return s->fault;
  s->tx_sent=1;
 }
 if(irq && !wr(s,R_IRQ_CLEAR,irq))return s->fault;
 if(irq&IRQ_DONE){
  if(!rd(s,R_TX_STATUS,&tx))return s->fault;
  if(TX_COUNT(tx))return cancel(s,JB_FIFO_ERROR);
  if(s->action.kind==JT_ACK){s->done_seen=1;s->phase=JB_TERMINAL_WAIT;}
  else s->phase=JB_RX_ISSUE;
 }
 return JB_PENDING;
}
static JBResult rx_issue(JBState *s)
{
 uint32_t irq;
 if(!rd(s,R_IRQ,&irq))return s->fault;
 if(irq&~IRQ_TX_WATERMARK)return cancel(s,JB_STATUS_ERROR);
 if(irq && !wr(s,R_IRQ_CLEAR,irq))return s->fault;
 /* No STOP_STRETCH on READ: the pair ends with STOP, not STOP + a new read. */
 if(!wr(s,R_RX_LEN,s->action.length)||
    !wr(s,R_COMMAND,(2u<<27)|(JT_I2C_ADDRESS<<9))||!barrier(s))return s->fault;
 s->phase=JB_RX_WAIT;return JB_PENDING;
}
static JBResult rx_wait(JBState *s)
{
 uint32_t irq,rx;
 if(!rd(s,R_IRQ,&irq))return s->fault;
 s->last_irq=irq;
 if(irq&IRQ_ERRORS)return cancel(s,JB_STATUS_ERROR);
 if(irq&IRQ_DONE)s->done_seen=1;
 if(!rd(s,R_RX_STATUS,&rx))return s->fault;
 uint32_t words=RX_COUNT(rx),last=(rx>>28)&7u;
 if(words>s->rx_depth || last>4 ||
    (words && !(rx&RX_LAST) && last))return cancel(s,JB_FIFO_ERROR);
 if(words){
  uint32_t valid=(rx&RX_LAST)&&last?last:4;
  uint32_t bytes=words*4u-4u+valid;
  if(bytes>(uint32_t)s->action.length-s->received ||
     ((rx&RX_LAST) && bytes!=(uint32_t)s->action.length-s->received))
   return cancel(s,JB_LENGTH_ERROR);
  /* All allowed reads are <=68B; at most17 FIFO reads, independently of an
   * arbitrary RX count. The checked word count cannot exceed that limit. */
  for(uint32_t i=0;i<words;++i){uint32_t word;
   if(!rd(s,R_RX_FIFO,&word))return s->fault;
   uint32_t n=i+1==words?valid:4;
   for(uint32_t k=0;k<n;++k)s->bytes[s->received++]=(uint8_t)(word>>(k*8));
  }
  if(rx&RX_LAST)s->last_seen=1;
 }
 if(irq && !wr(s,R_IRQ_CLEAR,irq))return s->fault;
 if(s->done_seen){
  if(s->received!=s->action.length || !s->last_seen)return cancel(s,JB_LENGTH_ERROR);
  s->phase=JB_TERMINAL_WAIT;
 }
 return JB_PENDING;
}

/* Read two fresh, ordered snapshots. This only proves local controller state;
 * target/electrical termination remains the independent provider's obligation. */
static int local_idle(JBState *s,uint64_t now)
{
 uint32_t status=0,irq=0,tx=0,rx=0,mode=0,disabled=0,clock=0,scl=0;
 for(unsigned pass=0;pass<2;++pass){
  if(!rd(s,R_STATUS,&status)||!rd(s,R_IRQ,&irq)||!rd(s,R_TX_STATUS,&tx)||
     !rd(s,R_RX_STATUS,&rx)||!rd(s,R_DMA,&mode)||!rd(s,R_IF_DISABLE,&disabled)||
     !rd(s,R_CLOCK,&clock)||!rd(s,R_SCL,&scl))return -1;
  if(mode!=0 || disabled!=0 || !timing_shape(clock,scl)){
   (void)isolate(s,JB_UNSUPPORTED);return -1;}
  if(TX_COUNT(tx)>s->tx_depth || RX_COUNT(rx)>s->rx_depth){
   (void)isolate(s,JB_FIFO_ERROR);return -1;}
  if(irq&IRQ_ERRORS){(void)isolate(s,JB_STATUS_ERROR);return -1;}
  if((status&M_ACTIVE)||TX_COUNT(tx)||RX_COUNT(rx))return 0;
  if(!barrier(s))return -1;
 }
 if(s->serial==UINT64_MAX){(void)isolate(s,JB_TIME_RANGE);return -1;}
 s->terminal_record=(JBTerminalRecord){s->config.lease_id,s->action.epoch,
  s->action.token,++s->serial,now,s->action.kind,status,irq,tx,rx,
  s->action.length,s->received,s->cleanup_ack==1,s->cleanup_ack==2};
 int terminal=s->io.terminal(s->io.context,&s->terminal_record);
 if(terminal<0){(void)isolate(s,JB_QUARANTINED);return -1;}
 if(terminal!=0 && terminal!=1){(void)isolate(s,JB_QUARANTINED);return -1;}
 return terminal;
}
static JBResult normal_terminal(JBState *s,uint64_t now)
{
 int idle=local_idle(s,now);
 if(idle<0)return s->fault;
 if(!idle)return JB_PENDING;
 if(!s->done_seen)return isolate(s,JB_STATUS_ERROR);
 s->completion_ready=1;s->completion_success=1;s->local_proven=1;
 s->phase=JB_READY;return JB_COMPLETE;
}
static JBResult issue_cleanup(JBState *s,uint64_t now,int aborting)
{
 uint32_t irq;
 if(!deadline(s,now,aborting?s->config.abort_timeout_ms:s->config.cancel_timeout_ms))return s->fault;
 /* Drop prior DONE/CANCEL/ABORT before issuing the new recovery command.
  * In particular a late original DONE cannot acknowledge CANCEL. */
 if(!rd(s,R_IRQ,&irq)||!wr(s,R_IRQ_ENABLE,0)||
    (irq&&!wr(s,R_IRQ_CLEAR,irq))||!barrier(s)||!rd(s,R_IRQ,&irq))return s->fault;
 if(irq&(IRQ_DONE|IRQ_CANCEL|IRQ_ABORT))return isolate(s,JB_STATUS_ERROR);
 if(!wr(s,R_CONTROL,aborting?2:4)||!barrier(s))return s->fault;
 s->phase=aborting?JB_ABORT_WAIT:JB_CANCEL_WAIT;return JB_PENDING;
}
static JBResult wait_cleanup(JBState *s,uint64_t now,int aborting)
{
 if(now>=s->recovery_deadline){
  if(aborting)return isolate(s,JB_TIMEOUT);
  s->phase=JB_ABORT_ISSUE;return JB_PENDING;
 }
 uint32_t irq;
 if(!rd(s,R_IRQ,&irq))return s->fault;
 s->last_irq=irq;
 if(irq&~(IRQ_NORMAL|(aborting?IRQ_ABORT:IRQ_CANCEL))){
  if(aborting)return isolate(s,JB_STATUS_ERROR);
  s->phase=JB_ABORT_ISSUE;return JB_PENDING;
 }
 if(irq&&!wr(s,R_IRQ_CLEAR,irq))return s->fault;
 if(irq&(aborting?IRQ_ABORT:IRQ_CANCEL)){
  s->cleanup_ack=aborting?2:1;
  if(!deadline(s,now,s->config.abort_timeout_ms))return s->fault;
  s->phase=JB_CLEANUP_WAIT;
 }
 return JB_PENDING;
}
static JBResult clean_fifo(JBState *s,uint64_t now)
{
 if(now>=s->recovery_deadline){
  if(s->cleanup_ack==1){s->phase=JB_ABORT_ISSUE;return JB_PENDING;}
  return isolate(s,JB_TIMEOUT);
 }
 uint32_t status,rx,tx;
 if(!rd(s,R_STATUS,&status)||!rd(s,R_TX_STATUS,&tx)||!rd(s,R_RX_STATUS,&rx))return s->fault;
 if(TX_COUNT(tx)>s->tx_depth || RX_COUNT(rx)>s->rx_depth)return isolate(s,JB_FIFO_ERROR);
 if(status&M_ACTIVE)return JB_PENDING;
 uint32_t count=RX_COUNT(rx);
 if(count){
  if(count>JB_MAX_FIFO_WORDS-s->cleanup_words)return isolate(s,JB_FIFO_ERROR);
  /* Drain a bounded chunk only after engine inactivity; never abandon a
   * borrowed buffer because software has reached its deadline. */
  uint32_t n=count>24?24:count;
  for(uint32_t i=0;i<n;++i){uint32_t discarded;
   if(!rd(s,R_RX_FIFO,&discarded))return s->fault;
   ++s->cleanup_words;
  }
  return JB_PENDING;
 }
 if(TX_COUNT(tx))return JB_PENDING; /* No guessed FIFO reset sequence. */
 int idle=local_idle(s,now);
 if(idle<0)return s->fault;
 if(!idle)return JB_PENDING;
 s->local_proven=1;s->phase=JB_LOCAL_QUIESCENT;
 return JB_NEEDS_EXTERNAL_FENCE;
}

JBResult jb_poll(JBState *s,uint64_t now)
{
 if(!s || !s->initialized)return JB_BAD_ARGUMENT;
 JBResult t=time_check(s,now);if(t!=JB_OK)return t;
 if(s->phase==JB_ISOLATED)return JB_QUARANTINED;
 if(s->phase==JB_READY)return s->completion_ready?JB_COMPLETE:JB_OK;
 if(!access(s))return s->fault;
 if(io_phase(s->phase) && now>=s->action.deadline_ms){
  if(s->phase<=JB_TX_ISSUE)return isolate(s,JB_TIMEOUT);
  return cancel(s,JB_TIMEOUT);
 }
 switch(s->phase){
 case JB_PRECHECK:return precheck(s);
 case JB_CONFIGURE:return configure(s);
 case JB_TX_ISSUE:return tx_issue(s);
 case JB_TX_WAIT:return tx_wait(s);
 case JB_RX_ISSUE:return rx_issue(s);
 case JB_RX_WAIT:return rx_wait(s);
 case JB_TERMINAL_WAIT:return normal_terminal(s,now);
 case JB_CANCEL_ISSUE:return issue_cleanup(s,now,0);
 case JB_CANCEL_WAIT:return wait_cleanup(s,now,0);
 case JB_ABORT_ISSUE:return issue_cleanup(s,now,1);
 case JB_ABORT_WAIT:return wait_cleanup(s,now,1);
 case JB_CLEANUP_WAIT:return clean_fifo(s,now);
 case JB_FENCE_CHECK:{
  int idle=local_idle(s,now);if(idle<0)return s->fault;
  if(!idle)return JB_PENDING;
  s->local_proven=1;s->phase=JB_LOCAL_QUIESCENT;
  return JB_NEEDS_EXTERNAL_FENCE;
 }
 case JB_LOCAL_QUIESCENT:return JB_NEEDS_EXTERNAL_FENCE;
 default:return JB_BAD_ARGUMENT;
 }
}
int jb_take_completion(JBState *s,JTCompletion *c)
{
 if(!s || !s->initialized || !c || !s->completion_ready)return 0;
 *c=(JTCompletion){s->action.kind,s->action.epoch,s->action.token,
  s->completion_success,s->completion_success?JT_E_IO_TERMINAL:0,0,0};
 if(s->completion_success && s->action.kind!=JT_ACK){
  c->bytes=s->bytes;c->length=s->received;
 }
 s->completion_ready=0;s->completion_taken=1;return 1;
}
JBResult jb_request_fence(JBState *s,const JTAction *a,uint64_t now)
{
 if(!s || !s->initialized || !a || a->kind!=JT_BUS_FENCE || !a->token)
  return JB_BAD_ARGUMENT;
 if(a->epoch!=s->epoch)return JB_STALE;
 if(s->fence_pending && same_action(a,&s->fence))return time_check(s,now)==JB_OK?JB_PENDING:s->fault;
 if(a->token<=s->last_token)return JB_STALE;
 JBResult t=time_check(s,now);if(t!=JB_OK)return t;
 if(a->deadline_ms<=now || a->deadline_ms-now>JT_MAX_TIMEOUT_MS)return JB_BAD_ARGUMENT;
 s->last_token=a->token;s->fence=*a;s->fence_pending=1;
 if(s->phase==JB_ISOLATED)return JB_QUARANTINED;
 if(s->phase==JB_READY){s->local_proven=0;s->phase=JB_FENCE_CHECK;}
 else if(io_phase(s->phase)){
  if(s->phase<=JB_TX_ISSUE)return isolate(s,JB_QUARANTINED);
  (void)cancel(s,JB_TIMEOUT);
 }
 return JB_PENDING;
}
int jb_fence_ready(const JBState *s,JBTerminalRecord *record)
{
 if(!s || !s->initialized || !s->fence_pending || !s->local_proven ||
    s->clock_fault || s->phase!=JB_LOCAL_QUIESCENT)return 0;
 if(record)*record=s->terminal_record;
 return 1;
}
JBResult jb_finish_fence(JBState *s,const JTCompletion *c,uint64_t now)
{
 if(!s || !s->initialized || !c)return JB_BAD_ARGUMENT;
 if(!s->fence_pending || !action_identity(&s->fence,c))return JB_STALE;
 JBResult t=time_check(s,now);if(t!=JB_OK)return t;
 if(now>=s->fence.deadline_ms)return JB_TIMEOUT;
 if(c->length || !c->success || (c->evidence&JT_BUS_FENCE_EVIDENCE)!=JT_BUS_FENCE_EVIDENCE)
  return JB_NEEDS_EXTERNAL_FENCE;
 if(!jb_fence_ready(s,0))return JB_QUARANTINED;
 if(!access(s))return s->fault;
 int idle=local_idle(s,now);if(idle<0)return s->fault;
 if(!idle){s->local_proven=0;s->phase=JB_FENCE_CHECK;return JB_PENDING;}
 /* The four external claims belong to this fence token. No bit was inferred
  * from M_ACTIVE, cancellation, status polling or the terminal callback. */
 s->phase=JB_READY;s->fence_pending=0;s->local_proven=0;
 s->completion_ready=0;s->completion_taken=1;s->action=(JTAction){0};
 return JB_OK;
}
