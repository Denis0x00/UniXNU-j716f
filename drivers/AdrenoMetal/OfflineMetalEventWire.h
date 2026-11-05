// SPDX-License-Identifier: MIT
// Actual GPU timeline events for cross-queue dependencies; CPU bookkeeping
// never substitutes for executing a GPU signal/wait or reports a signaled value.
#pragma once
#include "OfflineMetalCommandWire.h"
enum { OM_EVENT=0x130,OM_EVENT_SIGNAL,OM_EVENT_WAIT };
static_assert(OM_EVENT==OM_COMMAND_STATUS+1,"Event extension ordering");
typedef struct {uint64_t command,event,value;} OMEventDependency;
static_assert(sizeof(OMEventDependency)==24,"Bounded real GPU event dependency");
