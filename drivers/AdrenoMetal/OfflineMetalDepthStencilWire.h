// SPDX-License-Identifier: MIT
// Explicit offline depth/stencil resources, state and render attachments.
#pragma once
#include "OfflineMetalTextureWire.h"
enum { OM_DEPTH_STENCIL_STATE=0x123,OM_SET_DEPTH_STENCIL=0x124,
 OM_STENCIL_REFERENCE=0x125,OM_PIPELINE_ATTACHMENTS=0x126,OM_RENDER_ATTACHMENTS=0x127 };
static_assert(OM_DEPTH_STENCIL_STATE==OM_COPY_TEXTURE+1,"Depth/stencil extension follows texture copy");
typedef struct {uint64_t enabled,compare,failure,depthFailure,pass,readMask,writeMask;} OMStencilFace;
typedef struct {uint64_t compare,write;OMStencilFace front,back;} OMDepthStencil;
typedef struct {uint64_t encoder,state;} OMSetDepthStencil;
typedef struct {uint64_t encoder,front,back;} OMStencilReference;
typedef struct {OMBlendPipeline colors;uint64_t depthFormat,stencilFormat;} OMAttachmentPipeline;
typedef struct {uint64_t texture,load,store;double clear;} OMDepthAttachment;
typedef struct {uint64_t texture,load,store,clear;} OMStencilAttachment;
typedef struct {OMRender colors;OMDepthAttachment depth;OMStencilAttachment stencil;} OMAttachmentRender;
static_assert(sizeof(OMDepthStencil)==128 && sizeof(OMAttachmentRender)==520,"Bounded attachment/state ABI");
