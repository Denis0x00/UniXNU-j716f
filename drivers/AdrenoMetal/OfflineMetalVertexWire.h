// SPDX-License-Identifier: MIT
// Bounded static vertex input layouts for actual offline GPU pipelines.
#pragma once
#include "OfflineMetalDepthStencilWire.h"
enum { OFFLINE_METAL_VERTEX_SLOTS=31,OFFLINE_METAL_MAX_VERTEX_STRIDE=4096 };
typedef struct {uint64_t format,offset,buffer;} OMVertexAttribute;
typedef struct {uint64_t stride,step,rate;} OMVertexLayout;
// Size-discriminated extension of OM_PIPELINE_ATTACHMENTS. No new operation or
// permission is introduced, including the separately protected shared rights.
typedef struct {OMAttachmentPipeline pipeline;OMVertexAttribute attributes[31];OMVertexLayout layouts[31];} OMVertexPipeline;
static_assert(sizeof(OMVertexPipeline)==2104,"Bounded vertex layout fits native 4080-byte payload");
static inline unsigned OMVertexFormatBytes(uint64_t format) {
 if(format>=1 && format<=12)return 2+(format-1)%3;
 if(format>=13 && format<=24)return 2*(2+(format-13)%3);
 if(format>=25 && format<=27)return 2*(format-23);
 if(format>=28 && format<=39)return 4*(1+(format-28)%4);
 if(format>=40 && format<=42)return 4;
 if(format>=45 && format<=48)return 1;
 if(format>=49 && format<=53)return 2;
 if(format==54 || format==55)return 4;
 return 0;
}
static inline bool OMVertexInputsValid(const OMVertexPipeline &p) {
 bool used[31]={};unsigned count=0;
 for(const auto &a:p.attributes) {
  if(!a.format){if(a.offset || a.buffer)return false;continue;}
  unsigned bytes=OMVertexFormatBytes(a.format);
  if(!bytes || a.buffer>=31 || a.offset>4096-bytes)return false;
  used[a.buffer]=true;count++;
 }
 if(!count)return false;
 for(unsigned i=0;i<31;i++) {
  const auto &l=p.layouts[i];
  if(!used[i]){if(l.stride || l.step || l.rate)return false;continue;}
  if(l.stride>OFFLINE_METAL_MAX_VERTEX_STRIDE || l.step>2 || l.rate>65535 || (l.step && (!l.rate || !l.stride)))return false;
 }
 return true;
}
