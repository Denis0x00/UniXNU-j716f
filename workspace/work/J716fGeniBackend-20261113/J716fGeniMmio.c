/* SPDX-License-Identifier: BSD-2-Clause */
#include "J716fGeniMmio.h"

static int read_allowed(uint32_t offset)
{
 switch(offset) {
 case 0x40: case 0x48: case 0x64: case 0x68: case 0x258: case 0x278:
 case 0x254: case 0x260: case 0x264: case 0x284: case 0x288:
 case 0x80c: case 0x810: case 0x814:
 case 0x610: case 0x614: case 0x780: case 0x800: case 0x804:
 case 0xe1c: case 0xe24: case 0xe28: return 1;
 default: return 0;
 }
}
static int write_allowed(uint32_t offset,uint32_t value)
{
 /* Clock/power/pin/reset/DMA/SMMU/IRQ route writes are intentionally absent.
  * Values are further restricted by the transaction state machine. */
 switch(offset) {
 case 0x254: return value==0;
 case 0x260: case 0x284: return value==0x7f8fe;
 case 0x264: case 0x288: return value==0xffefe;
 case 0x26c: return value==2 || value==4;
 case 0x270: return value==16 || (value>=4 && value<=68 && value%8==4);
 case 0x600: return value==((1u<<27)|(0x5du<<9)) ||
                    value==((1u<<27)|(0x5du<<9)|4u) ||
                    value==((2u<<27)|(0x5du<<9));
 case 0x604: return value==4 || value==2;
 case 0x614: return value==0;
 case 0x618: return 1; /* W1C status only; owner granted polling route. */
 case 0x700: return value==0x4d82 || value==0x5d82;
 case 0x80c: return value<=1;
 case 0x810: return value<=63;
 case 0x814: return value<=63;
 default: return 0;
 }
}
static int enter(JBMMIO *b)
{
 if (!b || !b->initialized || b->revoked) return 0;
 if (b->in_call) {b->revoked=1;return 0;}
 b->in_call=1;return 1;
}
static int leave(JBMMIO *b,int ok)
{
 if (ok!=1) b->revoked=1;
 b->in_call=0;
 return ok==1 && !b->revoked;
}
static int permit_locked(JBMMIO *b,JBPermit *out)
{
 JBPermit p={0};
 if (b->provider.permit(b->provider.context,b->lease_id,&p)!=1 || b->revoked ||
     p.lease_id!=b->lease_id || p.mmio_base!=JB_MMIO_BASE ||
     p.mmio_bytes!=JB_MMIO_BYTES ||
     (p.evidence&JB_REQUIRED_PERMIT)!=JB_REQUIRED_PERMIT) return 0;
 if (out) *out=p;
 return 1;
}
static int read32(void *context,uint32_t offset,uint32_t *value)
{
 JBMMIO *b=context;
 uint32_t local=0;
 if (!enter(b)) return 0;
 if (!value || !read_allowed(offset) || !permit_locked(b,0)) return leave(b,0);
 int ok=b->provider.read32(b->provider.context,b->lease_id,JB_MMIO_BASE+offset,&local);
 if (!leave(b,ok)) return 0;
 *value=local;return 1;
}
static int write32(void *context,uint32_t offset,uint32_t value)
{
 JBMMIO *b=context;
 if (!enter(b)) return 0;
 if (!write_allowed(offset,value) || !permit_locked(b,0)) return leave(b,0);
 return leave(b,b->provider.write32(b->provider.context,b->lease_id,JB_MMIO_BASE+offset,value));
}
static int barrier(void *context)
{
 JBMMIO *b=context;
 if (!enter(b)) return 0;
 if (!permit_locked(b,0)) return leave(b,0);
 return leave(b,b->provider.barrier(b->provider.context,b->lease_id));
}
static int permit(void *context,uint64_t lease,JBPermit *out)
{
 JBMMIO *b=context;
 if (!enter(b)) return 0;
 if (lease!=b->lease_id || !out) return leave(b,0);
 return leave(b,permit_locked(b,out));
}
static int terminal(void *context,const JBTerminalRecord *record)
{
 JBMMIO *b=context;
 if (!enter(b)) return -1;
 if (!record || record->lease_id!=b->lease_id || !permit_locked(b,0)) {
  (void)leave(b,0);return -1;
 }
 int result=b->provider.terminal(b->provider.context,record);
 if (result==0 && !b->revoked) {b->in_call=0;return 0;}
 return leave(b,result)==1?1:-1;
}
int jbm_bind(JBMMIO *b,const JBMProvider *p,uint64_t lease,JBIO *out)
{
 if (!b || !p || !out || !lease || !p->read32 || !p->write32 ||
     !p->barrier || !p->permit || !p->terminal) return 0;
 uintptr_t bb=(uintptr_t)b,oo=(uintptr_t)out;
 if ((bb<=oo && oo-bb<sizeof *b) || (oo<bb && bb-oo<sizeof *out)) return 0;
 JBMProvider saved=*p;
 *b=(JBMMIO){0};b->provider=saved;b->lease_id=lease;b->initialized=1;
 *out=(JBIO){b,read32,write32,barrier,permit,terminal};return 1;
}
void jbm_revoke(JBMMIO *b) {if (b) b->revoked=1;}
