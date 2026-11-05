// SPDX-License-Identifier: MIT
// Offline GPU broker sessions. The kernel owns session selection; client
// request bytes never select another client's host object namespace.
#pragma once
#include "OfflineMetalWire.h"
enum { OFFLINE_METAL_SESSION_VERSION=3,OM_SESSION_OPEN=0x140,OM_SESSION_CLOSE=0x141 };
typedef struct { OMWireHeader wire;uint64_t session; } OMSessionWireHeader;
static_assert(sizeof(OMSessionWireHeader)==24,"Offline Metal session header");
