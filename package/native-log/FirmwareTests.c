// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "NativeFirmware.h"
static void barrier(void) {}
int main(void) {
  JL32 *live=calloc(1,JL_CONSOLE_BYTES),*copy=calloc(1,JL_CONSOLE_BYTES);
  JL64 id[4]={1,2,3,4},wrong[4]={2,2,3,4};JLArmGate gate={0};volatile JLHeader *h;
  JL32 offsets[JL_MAX_SESSIONS];
  live[0]=0x43474244;
  assert(jl_freeze(live,copy,barrier) && !memcmp(live,copy,JL_CONSOLE_BYTES));
  assert(!jl_arm_commit(&gate,live,id,barrier));
  assert(!jl_arm_query(&gate,live,id,8,0x10001,0,42));
  assert(!jl_arm_query(&gate,live,id,4,0xffffffff,0,42));
  assert(!jl_arm_query(&gate,live,id,4,0x10001,~0ULL,42));
  assert(jl_arm_query(&gate,live,id,4,0x10001,0,42));
  assert(!memcmp(live,copy,JL_CONSOLE_BYTES)); // Query has no retained writes
  ((unsigned char *)live)[100]^=1;
  assert(!jl_arm_commit(&gate,live,id,barrier) && gate.used && !gate.ready);
  assert(!jl_arm_query(&gate,live,id,4,0x10001,0,43));
  gate=(JLArmGate){0};
  assert(jl_arm_query(&gate,live,id,4,0x10001,0,43));
  h=jl_arm_commit(&gate,live,id,barrier);assert(h && h->state==JL_ARMED);
  assert(!jl_arm_commit(&gate,live,id,barrier));
  gate=(JLArmGate){0};assert(!jl_arm_query(&gate,live,id,4,0x10001,0,44));
  live[1]+=64;live[2]+=64; // Firmware appended after reset: kernel stays strict.
  assert(!jl_find(live,id));
  assert(jl_recover_sessions(live,id,offsets)==1 && offsets[0]==h->offset);
  assert(!jl_recover_sessions(live,wrong,offsets));
  assert(!jl_arm_query(&gate,live,id,4,0x10001,0,44));
  h->state=JL_USED;
  assert(!jl_arm_query(&gate,live,id,4,0x10001,0,43));
  assert(jl_arm_query(&gate,live,id,4,0x10001,0,44));
  assert(jl_arm_commit(&gate,live,id,barrier));
  assert(jl_recover_sessions(live,id,offsets)==2);
  h->crc^=1;assert(jl_recover_sessions(live,id,offsets)==1);h->crc^=1;
  assert(jl_freeze(live,copy,barrier));
  assert(!memcmp(live,copy,JL_CONSOLE_BYTES));
  live[1]=1;live[2]=JL_CAPACITY;
  gate=(JLArmGate){0};assert(!jl_arm_query(&gate,live,id,4,0x10001,0,45));
  live[1]=live[2]=JL_CAPACITY;
  assert(!jl_arm_query(&gate,live,id,4,0x10001,0,45));
  free(live);free(copy);
  puts("PASS firmware gates: frozen read-only copy, query/no-write, changed-console, stale/used/no-space refusal");
  return 0;
}
