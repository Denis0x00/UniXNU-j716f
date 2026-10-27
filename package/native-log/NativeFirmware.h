// SPDX-License-Identifier: BSD-2-Clause-Patent
// Shared production firmware gates; all policy helpers are host-testable.
#ifndef J716F_NATIVE_FIRMWARE_H
#define J716F_NATIVE_FIRMWARE_H
#include "NativeLog.h"
typedef struct {
  JL32 console_crc, ready, used;
  JL64 session;
} JLArmGate;

// Forensic reader only: accept intact candidate headers inside the committed
// linear console, even if the boot chain appended after them. Never used by
// kernel admission. A recovered session is evidence only after matching its
// ID and CRC to the separately photographed ARM page.
#define JL_MAX_SESSIONS (JL_CONSOLE_BYTES / JL_SESSION_BYTES)
static inline int jl_recover_header(const volatile void *console,JL32 offset,
    const JL64 candidate[4])
{
  const volatile JL32 *ring=(const volatile JL32 *)console;
  const volatile JLHeader *h;
  JL32 i;
  if(ring[0]!=0x43474244U || ring[1]!=ring[2] || ring[1]>JL_CAPACITY ||
      offset<16 || (offset&7U) || offset>JL_CONSOLE_BYTES-JL_SESSION_BYTES ||
      offset+JL_SESSION_BYTES>12U+ring[1]) return 0;
  h=(const volatile JLHeader *)((const volatile JL8 *)console+offset);
  if(h->magic!=JL_MAGIC || h->version!=1 || h->bytes!=JL_SESSION_BYTES ||
      h->offset!=offset || !h->session || h->crc!=jl_crc(h,60) ||
      (h->state!=JL_ARMED && h->state!=JL_USED)) return 0;
  for(i=0;i<4;++i) if(h->candidate[i]!=candidate[i]) return 0;
  for(i=0;i<sizeof(h->reserved);++i) if(h->reserved[i]) return 0;
  return 1;
}

static inline JL32 jl_recover_sessions(const volatile void *console,
    const JL64 candidate[4],JL32 offsets[JL_MAX_SESSIONS])
{
  JL32 count=0,offset;
  for(offset=16;offset<=JL_CONSOLE_BYTES-JL_SESSION_BYTES;offset+=8) {
    if(!jl_recover_header(console,offset,candidate)) continue;
    if(count==JL_MAX_SESSIONS) return 0; // never silently truncate evidence
    offsets[count++]=offset;
    offset+=JL_SESSION_BYTES-8;
  }
  return count;
}

static inline int jl_freeze(const volatile void *live, void *copy, JLBarrier barrier)
{
  const volatile JL8 *source=(const volatile JL8 *)live;
  JL8 *target=(JL8 *)copy;
  JL32 i, before=jl_crc(live,JL_CONSOLE_BYTES);
  barrier();
  for(i=0;i<JL_CONSOLE_BYTES;++i) target[i]=source[i];
  barrier();
  return before==jl_crc(copy,JL_CONSOLE_BYTES) && before==jl_crc(live,JL_CONSOLE_BYTES);
}

// Query is read-only and separate from ARM. No reset FID is ever needed.
static inline int jl_arm_query(JLArmGate *gate,const volatile void *live,
    const JL64 candidate[4],JL64 el,JL64 version,JL64 feature,JL64 session)
{
  const volatile JL32 *ring=(const volatile JL32 *)live;
  const volatile JLHeader *old;
  JL32 offset,offsets[JL_MAX_SESSIONS],count,i;
  if(gate->used) return 0;
  gate->ready=0;
  if(el!=4 || (version>>32) || (version&0x80000000ULL) || version<0x10000 || feature || !session ||
      ring[0]!=0x43474244U || ring[1]!=ring[2] || ring[1]>JL_CAPACITY) return 0;
  offset=(12U+ring[1]+7U)&~7U;
  if(offset>JL_CONSOLE_BYTES-JL_SESSION_BYTES) return 0;
  count=jl_recover_sessions(live,candidate,offsets);
  for(i=0;i<count;++i) {
    old=(const volatile JLHeader *)((const volatile JL8 *)live+offsets[i]);
    if(old->state==JL_ARMED || old->session==session) return 0;
  }
  gate->console_crc=jl_crc(live,JL_CONSOLE_BYTES);
  gate->session=session;gate->ready=1;
  return 1;
}

static inline volatile JLHeader *jl_arm_commit(JLArmGate *gate,volatile void *live,
    const JL64 candidate[4],JLBarrier barrier)
{
  if(!gate->ready || gate->used) return 0;
  gate->ready=0; gate->used=1; // even a failed attempt requires a new app invocation
  if(jl_crc(live,JL_CONSOLE_BYTES)!=gate->console_crc) return 0;
  return jl_reserve(live,candidate,gate->session,barrier);
}
#endif
