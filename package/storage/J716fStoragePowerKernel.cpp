// SPDX-License-Identifier: BSD-2-Clause-Patent
// Read-only SHNGROOT manifest + 512B window IOBlockStorageDevice + APFS root mount probe.
#include "J716fStoragePower.h"
#include "J716fStorageBringup.h"
#include "r41/bootstrap-domain/J716fStorageDomain.cpp"
#include "r41/R35Record.c"
#include "r41/KnownBlockReference.cpp"
#include "r41/R41UfsTransport.cpp"
#include "r41/R41Entry.cpp"
