// SPDX-License-Identifier: MIT
// Bounded real texture blits for the explicit offline transport.
#pragma once
#include "OfflineMetalQueueWire.h"
enum { OM_COPY_TEXTURE=0x122 };
static_assert(OM_COPY_TEXTURE==OM_COMMAND_ENQUEUE+1,"Texture copy follows existing queue operations");
typedef struct {
 uint64_t encoder,source,destination,sourceSlice,sourceLevel;
 uint64_t sourceOrigin[3],sourceSize[3],destinationSlice,destinationLevel,destinationOrigin[3];
} OMCopyTexture;
static_assert(sizeof(OMCopyTexture)==128,"Bounded texture-copy request ABI");
