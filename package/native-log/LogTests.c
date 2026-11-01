// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "NativeLog.h"
static JL64 *memory;
static const JL64 candidate[4] = {1,2,3,4};
static unsigned barriers;
static void barrier(void) { ++barriers; }
static void reset(void) {
  if (!memory) { memory=malloc(JL_CONSOLE_BYTES+16); assert(memory); }
  memset(memory, 0, JL_CONSOLE_BYTES+16);
  ((JL32 *)memory)[0] = 0x43474244;
  memory[JL_CONSOLE_BYTES/8] = 0x123456789abcdef0ULL;
}
int main(int argc, char **argv) {
  volatile JLHeader *h;
  JL64 data[10] = {27, 19200000, 1, 1};
  JL8 snapshot[128];
  JL32 *ring;
  unsigned slot, byte;
  reset(); ring=(JL32 *)memory; assert(!jl_find(memory,candidate));
  h = jl_reserve(memory,candidate,1,barrier); assert(h && !(h->offset&7));
  assert(!jl_write(h,0,123,data,barrier));
  h->state=JL_USED;
  for(slot=0;slot<JL_SLOTS;++slot) assert(jl_write(h,slot,123+slot,data,barrier));
  assert(!jl_write(h,JL_SLOTS,123,data,barrier));
  assert(!jl_write(h,0,999,data,barrier));
  assert(memory[JL_CONSOLE_BYTES/8] == 0x123456789abcdef0ULL);
  memcpy(snapshot,(const void *)jl_record(h,0),128);
  for(byte=0;byte<128;++byte) {
    ((volatile JL8 *)jl_record(h,0))[byte] ^= 1;
    assert(!jl_valid(h,jl_record(h,0),0));
    ((volatile JL8 *)jl_record(h,0))[byte] ^= 1;
  }
  assert(!memcmp(snapshot,(const void *)jl_record(h,0),128));
  for(byte=0;byte<64;++byte) {
    ((volatile JL8 *)h)[byte] ^= 1; assert(!jl_find(memory,candidate));
    ((volatile JL8 *)h)[byte] ^= 1;
  }
  assert(!jl_find(memory,(const JL64[4]){1,2,3,5}));
  // Header publication tears, stale sessions, partial records, repeated writes.
  ring[1]--; assert(!jl_find(memory,candidate)); ring[1]++;
  jl_record(h,0)->session++; assert(!jl_valid(h,jl_record(h,0),0));
  jl_record(h,0)->session--;
  jl_record(h,0)->commit=0; assert(!jl_valid(h,jl_record(h,0),0));
  assert(!jl_write(h,0,123,data,barrier));
  jl_record(h,0)->commit=JL_COMMIT;
  if(argc==2) {
    FILE *f=fopen(argv[1],"wb"); assert(f);
    assert(fwrite(memory,1,JL_CONSOLE_BYTES,f)==JL_CONSOLE_BYTES); assert(!fclose(f));
  }
  reset(); ring[1]=ring[2]=JL_CAPACITY; assert(!jl_reserve(memory,candidate,1,barrier));
  reset(); ring[1]=ring[2]=JL_CAPACITY+1; assert(!jl_reserve(memory,candidate,1,barrier));
  reset(); ring[1]=7; assert(!jl_reserve(memory,candidate,1,barrier));
  reset(); assert(!jl_reserve(memory,candidate,0,barrier));
  // Exhaustive alignments preserve all previous bytes and keep 64-bit accesses aligned.
  for(byte=0;byte<16;++byte) {
    reset(); memset((JL8*)memory+12,0xa5,byte); ring[1]=ring[2]=byte;
    h=jl_reserve(memory,candidate,byte+1,barrier); assert(h && !(h->offset&7));
    for(slot=0;slot<byte;++slot) assert(((JL8*)memory)[12+slot]==0xa5);
  }
  assert(barriers); puts("PASS native retained ABI: bounds, alignment, stale/torn/CRC, write-once and preservation");
  free(memory); return 0;
}
