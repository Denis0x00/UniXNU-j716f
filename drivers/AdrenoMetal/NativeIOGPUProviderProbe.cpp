// SPDX-License-Identifier: MIT
// Unpublished original-provider ABI instrument. Required methods never return
// fabricated success. An explicit missing-resource control returns NULL from
// descriptor slot170; all other required calls remain non-returning captures.
// Original C2/D2 and every inherited operational address
// remain intact; registration is intercepted only on this own probe class.
#include "OriginalGPUProviderABI.h"
extern "C" __attribute__((noreturn)) void native_original_provider_required(unsigned,const unsigned long long *);
extern "C" IOMemoryDescriptor *native_original_provider_descriptor(const unsigned long long *);
class NativeIOGPUProviderProbe : public IOGPU {
 OSDeclareDefaultStructors(NativeIOGPUProviderProbe)
public:
 __attribute__((noreturn)) void registerService(IOOptionBits = 0) override;
 IOMemoryDescriptor *abiSlot170(UInt32 *) override;
 __attribute__((noreturn)) void abiSlot174() override;
 __attribute__((noreturn)) void abiSlot189() override;
 __attribute__((noreturn)) void abiSlot192(void *) override;
 __attribute__((noreturn)) bool abiSlot195(IOService *) override;
 __attribute__((noreturn)) void abiSlot196() override;
 __attribute__((noreturn)) void abiSlot197() override;
 __attribute__((noreturn)) void abiSlot200() override;
 __attribute__((noreturn)) void abiSlot202() override;
 __attribute__((noreturn)) void abiSlot205() override;
};
OSDefineMetaClassAndStructors(NativeIOGPUProviderProbe,IOGPU)
// Save actual GPR arguments, caller LR, live CPU translation registers and SP.
// No FP instruction is executed in an unqualified EL1 FP context. These
// non-returning hooks do not reinterpret or mutate unknown pointer arguments.
#define SAVE_REQUIRED_FRAME \
 "sub sp, sp, #128\n" \
 "stp x0, x1, [sp, #0]\n" "stp x2, x3, [sp, #16]\n" \
 "stp x4, x5, [sp, #32]\n" "stp x6, x7, [sp, #48]\n" \
 "stp x8, x30, [sp, #64]\n" \
 "mrs x9, tcr_el1\n" "mrs x10, sctlr_el1\n" "stp x9, x10, [sp, #80]\n" \
 "mrs x9, ttbr0_el1\n" "mrs x10, ttbr1_el1\n" "stp x9, x10, [sp, #96]\n" \
 "add x9, sp, #128\n" "mrs x10, daif\n" "stp x9, x10, [sp, #112]\n"
#define CAPTURE_REQUIRED(number) __asm__ volatile(SAVE_REQUIRED_FRAME \
 "mov w0, #" #number "\n" "mov x1, sp\n" \
 "bl _native_original_provider_required\n" "brk #1\n")
__attribute__((naked,noreturn)) void NativeIOGPUProviderProbe::registerService(IOOptionBits) {CAPTURE_REQUIRED(84);}
__attribute__((naked)) IOMemoryDescriptor *NativeIOGPUProviderProbe::abiSlot170(UInt32 *) {
 __asm__ volatile(SAVE_REQUIRED_FRAME
  "mov x0, sp\n" "bl _native_original_provider_descriptor\n"
  "ldr x30, [sp, #72]\n" "add sp, sp, #128\n" "ret\n");
}
__attribute__((naked,noreturn)) void NativeIOGPUProviderProbe::abiSlot174() {CAPTURE_REQUIRED(174);}
__attribute__((naked,noreturn)) void NativeIOGPUProviderProbe::abiSlot189() {CAPTURE_REQUIRED(189);}
__attribute__((naked,noreturn)) void NativeIOGPUProviderProbe::abiSlot192(void *) {CAPTURE_REQUIRED(192);}
__attribute__((naked,noreturn)) bool NativeIOGPUProviderProbe::abiSlot195(IOService *) {CAPTURE_REQUIRED(195);}
__attribute__((naked,noreturn)) void NativeIOGPUProviderProbe::abiSlot196() {CAPTURE_REQUIRED(196);}
__attribute__((naked,noreturn)) void NativeIOGPUProviderProbe::abiSlot197() {CAPTURE_REQUIRED(197);}
__attribute__((naked,noreturn)) void NativeIOGPUProviderProbe::abiSlot200() {CAPTURE_REQUIRED(200);}
__attribute__((naked,noreturn)) void NativeIOGPUProviderProbe::abiSlot202() {CAPTURE_REQUIRED(202);}
__attribute__((naked,noreturn)) void NativeIOGPUProviderProbe::abiSlot205() {CAPTURE_REQUIRED(205);}
extern "C" IOService *native_original_provider_allocate(void) {return new NativeIOGPUProviderProbe;}
extern "C" unsigned native_original_provider_instances(void) {return NativeIOGPUProviderProbe::gMetaClass.getInstanceCount();}
