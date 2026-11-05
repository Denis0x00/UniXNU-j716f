// SPDX-License-Identifier: MIT
// Actual asynchronous host submission and status observation. Native integration
// must separately qualify resource visibility, lifetime and callback delivery.
#pragma once
#include "OfflineMetalFenceWire.h"
enum { OM_COMMAND_SUBMIT=0x12e,OM_COMMAND_STATUS=0x12f };
static_assert(OM_COMMAND_SUBMIT==OM_TEXTURE_HAZARD+1,"Command extension ordering");
// Reply is OMCommandResult: non-completed states have unavailable (zero) times.
// Successful completion requires actual status 4 and genuine nonzero GPU times.
