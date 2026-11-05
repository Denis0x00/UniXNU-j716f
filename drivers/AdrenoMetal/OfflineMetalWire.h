// SPDX-License-Identifier: MIT
// Offline Metal command transport. No deployment GPU or page-table ABI.
#pragma once
#include <stdint.h>
enum { OM_WIRE_MAGIC=0x5550474e, OM_WIRE_RESPONSE=0x8000 };
typedef struct {uint32_t magic;uint16_t version,operation;uint32_t bytes,request;} OMWireHeader;
static_assert(sizeof(OMWireHeader)==16,"Native Metal wire header");
enum { OFFLINE_METAL_VERSION=2, OFFLINE_METAL_MAX_REPLY=16*1024*1024,
 OM_DEVICE=0x100,OM_QUEUE,OM_LIBRARY,OM_FUNCTION,OM_TEXTURE,OM_PIPELINE,
 OM_COMMAND,OM_RENDER,OM_SET_PIPELINE,OM_SET_CULL,OM_DRAW,OM_END,
 OM_COMMIT,OM_READ_TEXTURE,OM_RELEASE,OM_FINISH,OM_WRITE_TEXTURE,
 OM_BUFFER,OM_WRITE_BUFFER,OM_READ_BUFFER,OM_VERTEX_BUFFER,OM_FRAGMENT_BUFFER,
 OM_DRAW_INDEXED,OM_BLIT,OM_COPY_BUFFER,OM_PIPELINE_BLEND,OM_VERTEX_TEXTURE,OM_FRAGMENT_TEXTURE,
 OM_SCISSOR,OM_VIEWPORT,OM_BLEND_COLOR };
enum { OFFLINE_METAL_MAX_BUFFER=64*1024*1024 };
typedef struct {int32_t error;uint32_t reserved;uint64_t handle;} OMResult;
typedef struct {uint64_t handle;} OMHandle;
typedef struct {uint8_t sha256[32];} OMLibrary;
typedef struct {uint64_t library;char name[128];} OMFunction;
typedef struct {uint64_t type,format,width,height,depth,levels,samples,array,usage,storage,cache;} OMTexture;
typedef struct {uint64_t vertex,fragment,samples,formats[8],masks[8];} OMPipeline;
typedef struct {uint64_t texture,load,store;double clear[4];} OMColor;
typedef struct {uint64_t command;OMColor colors[8];} OMRender;
typedef struct {uint64_t encoder,pipeline;} OMSetPipeline;
typedef struct {uint64_t encoder,mode;} OMSetCull;
typedef struct {uint64_t encoder,primitive,start,count,instances;} OMDraw;
typedef struct {uint64_t status,start_ns,end_ns;} OMCommandResult;
typedef struct {uint64_t texture,x,y,width,height,level;} OMReadTexture;
typedef struct {uint64_t texture,x,y,width,height,level,stride;} OMWriteTexture;
typedef struct {uint64_t bytes,options;} OMBuffer;
typedef struct {uint64_t buffer,offset,bytes;} OMBufferRegion;
typedef struct {uint64_t encoder,buffer,offset,index;} OMSetBuffer;
typedef struct {uint64_t encoder,primitive,count,type,buffer,offset,instances;int64_t baseVertex;uint64_t baseInstance;} OMDrawIndexed;
typedef struct {uint64_t encoder,source,sourceOffset,destination,destinationOffset,bytes;} OMCopyBuffer;
typedef struct {uint64_t enabled,sourceRGB,destinationRGB,rgbOperation,sourceAlpha,destinationAlpha,alphaOperation;} OMBlend;
typedef struct {OMPipeline pipeline;OMBlend colors[8];} OMBlendPipeline;
typedef struct {uint64_t encoder,texture,index;} OMSetTexture;
typedef struct {uint64_t encoder,x,y,width,height;} OMScissor;
typedef struct {uint64_t encoder;double x,y,width,height,nearDepth,farDepth;} OMViewport;
typedef struct {uint64_t encoder;float red,green,blue,alpha;} OMBlendColor;
