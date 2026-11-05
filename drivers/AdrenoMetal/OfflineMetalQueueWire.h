// SPDX-License-Identifier: MIT
// Offline frontend/backend extension; the kernel broker forwards opaque packets.
#pragma once
#include "OfflineMetalWire.h"
enum { OM_QUEUE_PRIORITY_GET=0x11f,OM_QUEUE_PRIORITY_SET=0x120,OM_COMMAND_ENQUEUE=0x121 };
static_assert(OM_QUEUE_PRIORITY_GET==OM_BLEND_COLOR+1,"Queue extension follows existing operations");
typedef struct {uint64_t queue,background,priority;} OMQueuePriority;
