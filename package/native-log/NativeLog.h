// SPDX-License-Identifier: BSD-2-Clause-Patent
// Single-core, write-once retained records. No allocation, MMIO, reset or I/O.
#ifndef J716F_NATIVE_LOG_H
#define J716F_NATIVE_LOG_H
typedef unsigned char JL8;
typedef unsigned int JL32;
typedef unsigned long long JL64;
#define JL_CONSOLE_PA 0xb00be000ULL
#define JL_CONSOLE_BYTES 0x40000U
#define JL_CAPACITY (JL_CONSOLE_BYTES - 12U)
#define JL_SESSION_BYTES 8192U
#define JL_SLOTS 63U
#define JL_MAGIC 0x31474f4c4e36314aULL
#define JL_RECORD 0x31524c4aU
#define JL_ARMED 0x414c4e4aU
#define JL_USED 0x554c4e4aU
#define JL_COMMIT 0xc04d17edU
typedef struct {
  JL64 magic;
  JL32 version, bytes;
  JL64 candidate[4];
  JL64 session;
  JL32 offset, crc;
  JL32 state;
  JL8 reserved[60];
} JLHeader;
typedef struct {
  JL32 magic, event;
  JL64 session, ordinal, counter;
  JL64 data[10];
  JL32 crc, commit;
  JL64 reserved;
} JLRecord;
typedef char JLHeaderSize[(sizeof(JLHeader) == 128) ? 1 : -1];
typedef char JLRecordSize[(sizeof(JLRecord) == 128) ? 1 : -1];
typedef char JLHeaderCrc[(__builtin_offsetof(JLHeader, crc) == 60) ? 1 : -1];
typedef char JLRecordCrc[(__builtin_offsetof(JLRecord, crc) == 112) ? 1 : -1];
typedef void (*JLBarrier)(void);

static inline JL32 jl_crc(const volatile void *buffer, JL32 bytes)
{
  const volatile JL8 *p = (const volatile JL8 *)buffer;
  JL32 crc = ~0U, i, bit;
  for (i = 0; i < bytes; ++i) {
    crc ^= p[i];
    for (bit = 0; bit < 8; ++bit)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}

// Only the newest completely reserved linear session is eligible. Never scan
// arbitrary stale bytes into a valid boot. All 64-bit fields must be aligned on
// Device-nGnRnE mappings (the enclosing DBGC header itself is only 12 bytes).
static inline volatile JLHeader *jl_find(volatile void *console, const JL64 candidate[4])
{
  volatile JL32 *ring = (volatile JL32 *)console;
  volatile JL8 *base = (volatile JL8 *)console;
  JL32 end, offset, i;
  volatile JLHeader *h;
  if (ring[0] != 0x43474244U || ring[1] != ring[2]) return 0;
  end = ring[1];
  if (end < JL_SESSION_BYTES || end > JL_CAPACITY) return 0;
  offset = 12U + end - JL_SESSION_BYTES;
  if (offset & 7U) return 0;
  h = (volatile JLHeader *)(base + offset);
  if (h->magic != JL_MAGIC || h->version != 1 || h->bytes != JL_SESSION_BYTES ||
      h->offset != offset || !h->session || h->crc != jl_crc(h, 60)) return 0;
  for (i = 0; i < 4; ++i) if (h->candidate[i] != candidate[i]) return 0;
  for (i = 0; i < sizeof(h->reserved); ++i) if (h->reserved[i]) return 0;
  if (h->state != JL_ARMED && h->state != JL_USED) return 0;
  return h;
}

// Firmware-only explicit ARM action. Never called by the reader's refresh or
// kernel. Appends into unused linear space; refuses wrap, repair and overwrite.
static inline volatile JLHeader *jl_reserve(volatile void *console,
    const JL64 candidate[4], JL64 session, JLBarrier barrier)
{
  volatile JL32 *ring = (volatile JL32 *)console;
  volatile JL8 *base = (volatile JL8 *)console;
  volatile JLHeader *h;
  JL32 offset, end, i;
  if (!session || ring[0] != 0x43474244U || ring[1] != ring[2] || ring[1] > JL_CAPACITY)
    return 0;
  offset = (12U + ring[1] + 7U) & ~7U;
  if (offset > JL_CONSOLE_BYTES - JL_SESSION_BYTES) return 0;
  end = offset + JL_SESSION_BYTES - 12U;
  for (i = 0; i < JL_SESSION_BYTES; ++i) base[offset+i] = 0;
  h = (volatile JLHeader *)(base + offset);
  h->magic = JL_MAGIC; h->version = 1; h->bytes = JL_SESSION_BYTES;
  for (i = 0; i < 4; ++i) h->candidate[i] = candidate[i];
  h->session = session; h->offset = offset; h->crc = jl_crc(h, 60);
  h->state = JL_ARMED;
  barrier();
  ring[2] = end; barrier(); ring[1] = end; barrier();
  return jl_find(console, candidate);
}

static inline volatile JLRecord *jl_record(volatile JLHeader *h, JL32 slot)
{
  return (volatile JLRecord *)((volatile JL8 *)h + 128U + slot*128U);
}

static inline int jl_valid(const volatile JLHeader *h, const volatile JLRecord *r, JL32 slot)
{
  return slot < JL_SLOTS && h->state == JL_USED && r->commit == JL_COMMIT &&
    r->magic == JL_RECORD && r->event == slot && r->ordinal == (JL64)slot+1 &&
    r->session == h->session && !r->reserved && r->crc == jl_crc(r, 112);
}

// A record is never rewritten. First claim precedes payload; CRC precedes the
// aligned final commit. On a fault, a different emergency slot remains usable.
static inline int jl_write(volatile JLHeader *h, JL32 slot, JL64 counter,
    const JL64 data[10], JLBarrier barrier)
{
  volatile JLRecord *r;
  JL32 i;
  if (!h || h->state != JL_USED || slot >= JL_SLOTS) return 0;
  r = jl_record(h, slot);
  if (r->magic || r->commit) return 0;
  r->magic = JL_RECORD; barrier();
  r->event = slot; r->session = h->session; r->ordinal = (JL64)slot+1;
  r->counter = counter;
  for (i = 0; i < 10; ++i) r->data[i] = data[i];
  r->reserved = 0; r->crc = jl_crc(r, 112); barrier();
  r->commit = JL_COMMIT; barrier();
  return jl_valid(h, r, slot);
}
#endif
