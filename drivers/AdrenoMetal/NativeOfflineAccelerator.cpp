// SPDX-License-Identifier: MIT
// Unpublished original-base lifecycle qualification. No GPU service or device
// is attached/started/published, and no original client contract is claimed.
#include "OriginalAcceleratorABI.h"
class NativeOfflineAccelerator : public IOAcceleratorES {
 OSDeclareDefaultStructors(NativeOfflineAccelerator)
};
OSDefineMetaClassAndStructors(NativeOfflineAccelerator,IOAcceleratorES)
extern "C" IOService *native_offline_accelerator_allocate(void) {return new NativeOfflineAccelerator;}
extern "C" unsigned native_offline_accelerator_instance_size(void) {return sizeof(NativeOfflineAccelerator);}
extern "C" unsigned native_offline_accelerator_instances(void) {return NativeOfflineAccelerator::gMetaClass.getInstanceCount();}
