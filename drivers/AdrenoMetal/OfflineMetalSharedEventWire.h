// SPDX-License-Identifier: MIT
// Real shared GPU events, values and actual host listener observations.
// Internal export/import/drop operations must be mediated by native Mach rights;
// public raw command submission must never accept a guessed export identifier.
#pragma once
#include "OfflineMetalEventWire.h"
enum { OM_SHARED_EVENT=0x133,OM_SHARED_VALUE_GET,OM_SHARED_VALUE_SET,
 OM_SHARED_NOTIFY,OM_SHARED_NOTIFICATION_POLL,OM_SHARED_LABEL_SET,
 OM_SHARED_EXPORT,OM_SHARED_IMPORT,OM_SHARED_EXPORT_DROP };
static_assert(OM_SHARED_EVENT==OM_EVENT_WAIT+1,"Shared event extension ordering");
typedef struct {uint64_t event,value;} OMSharedEventValue;
typedef struct {uint64_t ready,event,value;} OMSharedNotification;
typedef struct {uint64_t event,present,bytes;} OMSharedLabel;
static_assert(sizeof(OMSharedEventValue)==16 && sizeof(OMSharedNotification)==24,"Bounded shared event ABI");
