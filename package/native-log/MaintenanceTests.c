// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "NativeMaintenance.h"
static JL64 live[JL_CONSOLE_BYTES/8],copy[JL_CONSOLE_BYTES/8],candidate[4]={1,2,3,4};
static JL8 profile[32]={1};
static jmp_buf interrupted;
static int cut,steps,mutate;
static void barrier(void) {
  if(mutate) {((JL8 *)live)[50000]^=1;mutate=0;}
  if(cut && ++steps==cut)longjmp(interrupted,1);
}
static void fixture(JL32 next,JL32 size) {
  JL32 i,*r=(JL32 *)live;JL8 *p=(JL8 *)live;
  for(i=0;i<JL_CONSOLE_BYTES;i++)p[i]=(JL8)(i*17+(i>>8));
  r[0]=0x43474244;r[1]=next;r[2]=size;memcpy(copy,live,sizeof(live));
}
static void save(const char *dir,const char *name,const void *data,size_t n) {
  char path[4096];FILE *f;snprintf(path,sizeof(path),"%s/%s",dir,name);f=fopen(path,"wb");
  assert(f && fwrite(data,1,n,f)==n && !fclose(f));
}
int main(int argc,char **argv) {
  JL2Gate gate={0};JLArmGate arm={0};JL32 n,free,kind;JL8 digest[32];JL64 ack;
  char frame[JL2_TEXT],path[4096];FILE *f;volatile int k;JLHeader *h;
  static JL8 classes[JL_CAPACITY+1];
  assert(argc==2);
  for(n=0;n<=JL_CAPACITY;n++) {
    JL32 r[3]={0x43474244,n,n};kind=jl2_classify(r,&free);classes[n]=(JL8)kind;
    assert(kind==(n==JL_CAPACITY?JL2_FULL_LINEAR:n<=0x3dff4?JL2_SPACE:JL2_EXHAUSTED));
    r[2]=JL_CAPACITY;assert(jl2_classify(r,&free)==(n==JL_CAPACITY?JL2_FULL_LINEAR:JL2_FULL_WRAPPED));
  }
  save(argv[1],"classes.bin",classes,sizeof(classes));
  fixture(0x3f56e,0x3f56e);
  assert(jl2_classify(copy,&free)==JL2_EXHAUSTED && free==2688);
  assert(!jl2_prepare(&gate,copy,candidate,profile,1,0) && gate.reason==JL2_UNSTABLE);
  assert(!jl2_prepare(&gate,copy,candidate,profile,0,1));
  for(n=0;n<6;n++) {
    JL32 *r=(JL32 *)copy;fixture(0x3f56e,0x3f56e);gate=(JL2Gate){0};
    if(n==0)r[0]=0;
    if(n==1)r[1]=JL_CAPACITY+1;
    if(n==2)r[2]=JL_CAPACITY+1;
    if(n==3)r[1]--;
    if(n==4)r[1]=r[2]=0;
    if(n==5)r[1]=r[2]=0x3dff4;
    assert(!jl2_prepare(&gate,copy,candidate,profile,123,1));
    assert(!jl2_commit(&gate,live,copy,0,2,barrier));
  }
  fixture(0x3f56e,0x3f56e);gate=(JL2Gate){0};
  assert(jl2_prepare(&gate,copy,candidate,profile,123,1));ack=gate.record.receipt;
  assert(!jl2_commit(&gate,live,copy,ack^1,2,barrier));
  assert(!jl2_commit(&gate,live,copy,ack,1,barrier));
  assert(!memcmp(live,copy,sizeof(live)));
  assert(jl2_receipt(candidate,gate.record.backup_sha256,profile,124)!=ack);
  assert(jl_export_receipt(candidate,gate.record.backup_sha256)!=ack);
  mutate=1;assert(!jl2_commit(&gate,live,copy,ack,2,barrier) && gate.used);
  ((JL8 *)live)[50000]^=1;assert(!memcmp(live,copy,sizeof(live)));
  assert(!jl2_prepare(&gate,copy,candidate,profile,123,1));
  for(n=0;n<3;n++) {
    fixture(n==0?0x3f56e:n==1?JL_CAPACITY:17,n==0?0x3f56e:JL_CAPACITY);
    gate=(JL2Gate){0};assert(jl2_prepare(&gate,copy,candidate,profile,123,1));
    assert(jl2_commit(&gate,live,copy,gate.record.receipt,2,barrier));
    assert(jl2_record_valid(live,candidate));assert(!jl_find(live,candidate));
    assert(!memcmp((JL8 *)live+272,(JL8 *)copy+272,JL_CONSOLE_BYTES-272));
    if(n==0)save(argv[1],"initialized.bin",live,sizeof(live));
  }
  arm=(JLArmGate){0};assert(jl_arm_query(&arm,live,candidate,4,0x10001,0,999));
  h=(JLHeader *)jl_arm_commit(&arm,live,candidate,barrier);
  assert(h && h->offset==272 && h->state==JL_ARMED && jl2_record_valid(live,candidate));
  ((JL32 *)live)[1]=17;((JL32 *)live)[2]=JL_CAPACITY;memcpy(copy,live,sizeof(live));
  gate=(JL2Gate){0};assert(!jl2_prepare(&gate,copy,candidate,profile,123,1) && gate.reason==JL2_PENDING);
  h=(JLHeader *)((JL8 *)copy+272);h->candidate[0]^=1;h->crc=jl_crc(h,60);
  assert(jl2_sessions(copy)==JL2_PENDING);h->state=JL_USED;assert(!jl2_sessions(copy));
  for(n=0;n<6;n++) {
    JLHeader original=*h;
    if(n==0)h->version++;
    if(n==1)h->crc^=1;
    if(n==2)h->state=0;
    if(n==3)h->reserved[0]=1;
    if(n==4)h->offset++;
    if(n==5)h->bytes--;
    assert(jl2_sessions(copy)==JL2_SUSPECT);*h=original;
  }
  *(JL64 *)((JL8 *)copy+JL_CONSOLE_BYTES-8)=JL_MAGIC;assert(jl2_sessions(copy)==JL2_SUSPECT);
  for(k=1;k<=4;k++) {
    fixture(0x3f56e,0x3f56e);gate=(JL2Gate){0};assert(jl2_prepare(&gate,copy,candidate,profile,123,1));
    cut=k;steps=0;if(!setjmp(interrupted)){jl2_commit(&gate,live,copy,ack,2,barrier);assert(0);}
    assert(!jl_find(live,candidate));assert(!memcmp((JL8 *)live+272,(JL8 *)copy+272,JL_CONSOLE_BYTES-272));
    if(k==2 || k==3)assert(((JL32 *)live)[0]==0);
  }
  cut=0;fixture(0x3f56e,0x3f56e);jl_sha256(copy,JL_CONSOLE_BYTES,digest);
  save(argv[1],"raw.bin",copy,sizeof(copy));
  snprintf(path,sizeof(path),"%s/frames.txt",argv[1]);f=fopen(path,"w");assert(f);
  for(n=0;n<256;n++) {assert(jl2_frame(frame,copy,candidate,digest,profile,123,jl_crc(copy,sizeof(copy)),n)==JL2_TEXT-1);assert(fprintf(f,"%s\n",frame)>0);}
  assert(!fclose(f));printf("RECEIPT %016llX\n",jl2_receipt(candidate,digest,profile,123));
  puts("PASS production maintenance: state boundaries, pending/corrupt sessions, write bounds, torn commit, native ARM compatibility");
  return 0;
}
