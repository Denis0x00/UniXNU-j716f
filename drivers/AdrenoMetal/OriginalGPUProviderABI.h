// SPDX-License-Identifier: MIT
#pragma once
// Unpublished ABI instrument only. The native service prefix and opaque storage
// match the exact original abstract base. Non-pure tail entries are bound to
// their ORIGINAL entry addresses, with no C++ caller supplied for those opaque
// source signatures. Required tail probes capture their actual register frame;
// descriptor slot170 may return NULL only for an explicit missing-resource
// control. No unknown signature, capability or successful
// operational implementation is claimed. This header is not a system driver API.
#include "OriginalAcceleratorABI.h"
class IOMemoryDescriptor;
class IOGPU : public IOAcceleratorES {
 OSDeclareAbstractStructors(IOGPU)
 unsigned char originalPrivateState[600];
protected:
 void free() override;
public:
 IOReturn configureReport(IOReportChannelList *,IOReportConfigureAction,void *,void *) override;
 IOReturn updateReport(IOReportChannelList *,IOReportUpdateAction,void *,void *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 IOWorkLoop *getWorkLoop() const override;
 using IOAcceleratorES::newUserClient;
 IOReturn newUserClient(task_t,void *,UInt32,IOUserClient **) override;
protected:
 virtual void abiSlot168();
 virtual void abiSlot169();
 virtual IOMemoryDescriptor *abiSlot170(UInt32 *) = 0;
 virtual void abiSlot171();
 virtual void abiSlot172();
 virtual void abiSlot173();
 virtual void abiSlot174() = 0;
 virtual void abiSlot175();
 virtual void abiSlot176();
 virtual void abiSlot177();
 virtual void abiSlot178();
 virtual void abiSlot179();
 virtual void abiSlot180();
 virtual void abiSlot181();
 virtual void abiSlot182();
 virtual void abiSlot183();
 virtual void abiSlot184();
 virtual void abiSlot185();
 virtual void abiSlot186();
 virtual void abiSlot187();
 virtual void abiSlot188();
 virtual void abiSlot189() = 0;
 virtual void abiSlot190();
 virtual void abiSlot191();
 virtual void abiSlot192(void *) = 0;
 virtual void abiSlot193();
 virtual void abiSlot194();
 virtual bool abiSlot195(IOService *) = 0;
 virtual void abiSlot196() = 0;
 virtual void abiSlot197() = 0;
 virtual void abiSlot198();
 virtual void abiSlot199();
 virtual void abiSlot200() = 0;
 virtual void abiSlot201();
 virtual void abiSlot202() = 0;
 virtual void abiSlot203();
 virtual void abiSlot204();
 virtual void abiSlot205() = 0;
 virtual void abiSlot206();
 virtual void abiSlot207();
 virtual void abiSlot208();
 virtual void abiSlot209();
 virtual void abiSlot210();
 virtual void abiSlot211();
 virtual void abiSlot212();
 virtual void abiSlot213();
 virtual void abiSlot214();
};
static_assert(sizeof(IOGPU)==736,"Exact original provider storage extent");
