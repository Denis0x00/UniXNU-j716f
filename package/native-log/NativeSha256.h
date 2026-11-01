// SPDX-License-Identifier: BSD-2-Clause-Patent
// Small, allocation-free SHA-256 for firmware evidence checksums, not signatures.
// FIPS 180-4 section 6.2; tested independently against Python hashlib.
#ifndef J716F_NATIVE_SHA256_H
#define J716F_NATIVE_SHA256_H
#include "NativeLog.h"
static inline JL32 jl_rotr(JL32 x, JL32 n) { return (x >> n) | (x << (32-n)); }
static inline void jl_sha_block(JL32 s[8],const JL8 p[64]) {
  static const JL32 k[64]={
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
  JL32 w[64],a=s[0],b=s[1],c=s[2],d=s[3],e=s[4],f=s[5],g=s[6],h=s[7],i,t,u;
  for(i=0;i<16;i++) w[i]=((JL32)p[4*i]<<24)|((JL32)p[4*i+1]<<16)|((JL32)p[4*i+2]<<8)|p[4*i+3];
  for(i=16;i<64;i++) w[i]=w[i-16]+(jl_rotr(w[i-15],7)^jl_rotr(w[i-15],18)^(w[i-15]>>3))+
    w[i-7]+(jl_rotr(w[i-2],17)^jl_rotr(w[i-2],19)^(w[i-2]>>10));
  for(i=0;i<64;i++) {
    t=h+(jl_rotr(e,6)^jl_rotr(e,11)^jl_rotr(e,25))+((e&f)^(~e&g))+k[i]+w[i];
    u=(jl_rotr(a,2)^jl_rotr(a,13)^jl_rotr(a,22))+((a&b)^(a&c)^(b&c));
    h=g;g=f;f=e;e=d+t;d=c;c=b;b=a;a=t+u;
  }
  s[0]+=a;s[1]+=b;s[2]+=c;s[3]+=d;s[4]+=e;s[5]+=f;s[6]+=g;s[7]+=h;
}
static inline void jl_sha256(const void *data,JL32 bytes,JL8 out[32]) {
  const JL8 *p=(const JL8 *)data;JL8 tail[128];JL32 i,left=bytes%64,n=left<56?64:128;
  JL64 bits=(JL64)bytes*8;
  JL32 s[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  // Volatile initialization prevents freestanding COFF from importing memset.
  for(i=0;i<sizeof(tail);i++) ((volatile JL8 *)tail)[i]=0;
  for(i=0;i<bytes-left;i+=64) jl_sha_block(s,p+i);
  for(i=0;i<left;i++) tail[i]=p[bytes-left+i];
  tail[left]=0x80;
  for(i=0;i<8;i++) tail[n-1-i]=(JL8)(bits>>(i*8));
  jl_sha_block(s,tail);if(n==128) jl_sha_block(s,tail+64);
  for(i=0;i<32;i++) out[i]=(JL8)(s[i/4]>>(24-(i%4)*8));
}
#endif
