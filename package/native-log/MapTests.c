// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "NativeMap.h"
static JL64 tables[5][512];
static unsigned used;
static JL64 *resolve(JL64 pa) {
  if (pa<4096 || pa>=6*4096 || pa%4096) return 0;
  return tables[pa/4096-1];
}
static JL64 *allocate(JL64 *pa) {
  if (used>=5) return 0;
  *pa=(JL64)(++used)*4096; return tables[used-1];
}
static const JLMapOps ops={resolve,allocate};
static const JL64 attributes=0x006000000000060fULL;
static JL64 translate(JL64 va) {
  JL64 *l2=resolve(tables[0][(va>>30)&511]&JL_PAGE_MASK), e=l2[(va>>21)&511];
  if ((e&3)==1) return (e&JL_BLOCK_MASK)+(va&0x1fffff);
  return resolve(e&JL_PAGE_MASK)[(va>>12)&511]&JL_PAGE_MASK;
}
int main(void) {
  JL64 va=0xfffffff0071f0000ULL, original=0xd01f0000ULL;
  unsigned i;
  memset(tables,0,sizeof(tables)); used=2;
  tables[0][(va>>30)&511]=0x2003;
  tables[1][(va>>21)&511]=0xd0000001ULL|JL_HINT;
  tables[1][((va>>21)&511)+1]=0xd0200001ULL|JL_HINT;
  assert(jl_map_window(tables[0],va,original,&ops,attributes));
  assert(used==4);
  for(i=0;i<64;++i) assert(translate(va+i*4096ULL)==JL_CONSOLE_PA+i*4096ULL);
  assert(translate(va-4096)==original-4096);
  assert(translate(va+JL_CONSOLE_BYTES)==original+JL_CONSOLE_BYTES);
  for(i=0;i<512;++i) assert(!(tables[2][i]&JL_HINT) && !(tables[3][i]&JL_HINT));
  assert(!jl_map_window(tables[0],va,original,&ops,attributes)); // no second rebind
  assert(!jl_map_window(tables[0],va+1,original,&ops,attributes));
  memset(tables,0,sizeof(tables)); used=2;
  assert(!jl_map_window(tables[0],va,original,&ops,attributes));
  tables[0][(va>>30)&511]=0x2003;
  tables[1][(va>>21)&511]=0xd0000001;
  used=5; assert(!jl_map_window(tables[0],va,original,&ops,attributes));
  puts("PASS native mapping: exact window, crossing L2, neighbour preservation, HINT and refusal gates");
  return 0;
}
