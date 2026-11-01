// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "NativeExport.h"
static JL64 live[JL_CONSOLE_BYTES/8],copy[JL_CONSOLE_BYTES/8];
static JL64 candidate[4]={1,2,3,4};
static jmp_buf interrupted;
static int cut,steps;
static void barrier(void) { if(cut && ++steps==cut) longjmp(interrupted,1); }
static void fixture(void) {
  JL32 i;JL8 *p=(JL8 *)live;JL32 *r=(JL32 *)live;
  for(i=0;i<JL_CONSOLE_BYTES;i++) p[i]=(JL8)(i*17+(i>>8));
  r[0]=0x43474244;r[1]=0x2a354;r[2]=JL_CAPACITY;memcpy(copy,live,sizeof(live));
}
int main(int argc,char **argv) {
  JLInitGate gate={0};JLArmGate arm={0};JL8 hash[32];char frame[JL_EXPORT_TEXT],hex[65],path[4096];
  JL32 *r=(JL32 *)live,i;volatile JLHeader *h;FILE *file;JL64 ack;volatile int n;
  assert(argc==2);
  fixture();
  assert(!jl_init_prepare(&gate,copy,candidate,1,0));
  assert(!jl_init_prepare(&gate,copy,candidate,0,1));
  for(i=0;i<4;i++) {
    JL32 *cr=(JL32 *)copy;fixture();
    if(i==0) cr[0]=0;
    if(i==1) cr[2]=JL_CAPACITY-1;
    if(i==2) cr[1]=JL_CAPACITY;
    if(i==3) cr[1]=JL_CAPACITY+1;
    assert(!jl_init_prepare(&gate,copy,candidate,1,1));
    assert(!jl_init_commit(&gate,live,copy,0,2,barrier));
  }
  fixture();assert(jl_init_prepare(&gate,copy,candidate,42,1));ack=gate.record.receipt;
  assert(!jl_init_commit(&gate,live,copy,ack^1,2,barrier));
  assert(!jl_init_commit(&gate,live,copy,ack,1,barrier));assert(!memcmp(live,copy,sizeof(live)));
  ((JL8 *)live)[90000]^=1;
  assert(!jl_init_commit(&gate,live,copy,ack,2,barrier) && gate.used);
  ((JL8 *)live)[90000]^=1;assert(!memcmp(live,copy,sizeof(live)));
  assert(!jl_init_commit(&gate,live,copy,ack,2,barrier));
  gate=(JLInitGate){0};assert(jl_init_prepare(&gate,copy,candidate,42,1));
  ((JL8 *)copy)[17000]^=1;assert(!jl_init_commit(&gate,live,copy,ack,2,barrier));
  fixture();gate=(JLInitGate){0};assert(jl_init_prepare(&gate,copy,candidate,42,1));
  assert(jl_init_commit(&gate,live,copy,ack,2,barrier));
  assert(r[0]==0x43474244 && r[1]==JL_INIT_END && r[2]==JL_INIT_END);
  assert(!memcmp((JL8 *)live+16,&gate.record,256));
  assert(!memcmp((JL8 *)live+272,(JL8 *)copy+272,JL_CONSOLE_BYTES-272));
  assert(!jl_find(live,candidate)); // initialization NEVER arms a kernel
  assert(jl_arm_query(&arm,live,candidate,4,0x10001,0,99));
  h=jl_arm_commit(&arm,live,candidate,barrier);assert(h && h->offset==272 && h->state==JL_ARMED);
  // A pending ARM for any candidate anywhere in the wrapped raw area blocks init.
  r[1]=1;r[2]=JL_CAPACITY;memcpy(copy,live,sizeof(live));gate=(JLInitGate){0};
  assert(jl_raw_armed(copy) && !jl_init_prepare(&gate,copy,candidate,42,1));
  ((JLHeader *)((JL8 *)copy+272))->candidate[0]^=1;
  ((JLHeader *)((JL8 *)copy+272))->crc=jl_crc((JL8 *)copy+272,60);
  assert(jl_raw_armed(copy) && !jl_init_prepare(&gate,copy,candidate,42,1));
  ((JLHeader *)((JL8 *)copy+272))->state=JL_USED;
  assert(jl_init_prepare(&gate,copy,candidate,42,1));
  // Simulated power loss at each persistence barrier, including final commit.
  for(n=1;n<=4;n++) {
    fixture();gate=(JLInitGate){0};assert(jl_init_prepare(&gate,copy,candidate,42,1));
    cut=n;steps=0;
    if(!setjmp(interrupted)) {jl_init_commit(&gate,live,copy,ack,2,barrier);assert(0);}
    assert(!jl_find(live,candidate));
    assert(!memcmp((JL8 *)live+272,(JL8 *)copy+272,JL_CONSOLE_BYTES-272));
    if(n==2 || n==3) assert(r[0]==0);
  }
  cut=0;fixture();jl_sha256(copy,JL_CONSOLE_BYTES,hash);
  assert(!jl_export_frame(frame,copy,candidate,hash,0,0,0));
  assert(!jl_export_frame(frame,copy,candidate,hash,1,0,256));
  snprintf(path,sizeof(path),"%s/raw.bin",argv[1]);file=fopen(path,"wb");assert(file);
  assert(fwrite(copy,1,sizeof(copy),file)==sizeof(copy));assert(!fclose(file));
  snprintf(path,sizeof(path),"%s/frames.txt",argv[1]);file=fopen(path,"w");assert(file);
  for(i=0;i<256;i++) {
    assert(jl_export_frame(frame,copy,candidate,hash,123,jl_crc(copy,JL_CONSOLE_BYTES),i)==JL_EXPORT_TEXT-1);
    assert(fprintf(file,"%s\n",frame)>0);
  }
  assert(!fclose(file));printf("RECEIPT %016llX\n",jl_export_receipt(candidate,hash));
  {
    static const JL32 lengths[]={0,1,55,56,63,64,65,127,128,129,262144,1000000};
    JL8 *data=malloc(1000000);assert(data);
    for(i=0;i<1000000;i++) data[i]=(JL8)(i*31+(i>>8));
    for(i=0;i<sizeof(lengths)/sizeof(lengths[0]);i++) {
      jl_sha256(data,lengths[i],hash);jl_hex_bytes(hex,hash,32);hex[64]=0;
      printf("SHA %u %s\n",lengths[i],hex);
    }
    free(data);
  }
  puts("PASS production export/init: refusal, exact readback, fresh ARM compatibility, four interrupted commits");
  return 0;
}
