// SPDX-License-Identifier: MIT
// Explicit offline actual GPU fence dependencies; no emulated completion.
#pragma once
#include "OfflineMetalDepthStencilWire.h"
enum { OM_FENCE=0x128,OM_RENDER_FENCE_UPDATE,OM_RENDER_FENCE_WAIT,
 OM_BLIT_FENCE_UPDATE,OM_BLIT_FENCE_WAIT,OM_TEXTURE_HAZARD };
static_assert(OM_FENCE==OM_RENDER_ATTACHMENTS+1,"Fence extension ordering");
typedef struct {uint64_t encoder,fence,stages;} OMFenceDependency;
typedef struct {OMTexture texture;uint64_t hazard;} OMHazardTexture;
static_assert(sizeof(OMHazardTexture)==96,"Bounded actual texture hazard mode");
static_assert(sizeof(OMFenceDependency)==24,"Bounded real fence dependency");
