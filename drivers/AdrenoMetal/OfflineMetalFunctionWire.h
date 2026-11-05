// SPDX-License-Identifier: MIT
// Bounded original function constants for explicit offline specialization.
#pragma once
#include "OfflineMetalWire.h"
enum { OFFLINE_METAL_MAX_CONSTANTS=16 };
typedef struct {uint64_t type,index,bytes,named;char name[128];uint8_t data[32];} OMFunctionConstant;
typedef struct {OMFunction function;uint64_t count;} OMSpecializedFunction;
static_assert(sizeof(OMFunctionConstant)==192 && sizeof(OMSpecializedFunction)==144,"Function constant wire ABI");
static_assert(sizeof(OMSpecializedFunction)+OFFLINE_METAL_MAX_CONSTANTS*sizeof(OMFunctionConstant)<=4080,"Bounded specialization transaction");
// Only qualified scalar and 2/4-component value sizes. Matrices, 3-component
// alignment profiles, aggregate and pointer types refuse explicitly.
static inline uint64_t OMFunctionConstantBytes(uint64_t type) {
 switch(type) {
  case 3:case 29:case 33:return 4; // float/int/uint
  case 4:case 30:case 34:return 8;
  case 6:case 32:case 36:return 16;
  case 16:case 37:case 41:return 2; // half/short/ushort
  case 17:case 38:case 42:return 4;
  case 19:case 40:case 44:return 8;
  case 45:case 49:case 53:return 1; // char/uchar/bool
  case 46:case 50:case 54:return 2;
  case 48:case 52:case 56:return 4;
  case 81:case 85:return 8; // long/ulong
  case 82:case 86:return 16;
  case 84:case 88:return 32;
  default:return 0;
 }
}
