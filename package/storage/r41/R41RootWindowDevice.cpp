// SPDX-License-Identifier: BSD-2-Clause-Patent
// Strictly read-only 512-byte logical IOBlockStorageDevice backed by a verified SHNGROOT window on UFS LUN 0.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSString.h>

#include "R41RootWindowDevice.h"
#include "R41UfsTransport.h"

namespace {
struct WindowCounters {
  volatile uint64_t reads;
  volatile uint64_t bytes;
  volatile uint64_t writes_refused;
  volatile uint64_t errors;
};

constexpr unsigned kTrackedWindows = 2;  // 0 = SystemOS, 1 = OS
WindowCounters g_win_stats[kTrackedWindows] = {};

struct DescriptorSink {
  IOMemoryDescriptor *buffer;
  uint64_t copied;
};

bool writeDescriptorSink(void *ctx, uint64_t dst_off, const uint8_t *src, size_t bytes) {
  auto &s = *static_cast<DescriptorSink *>(ctx);
  const IOByteCount written = s.buffer->writeBytes(dst_off, src, bytes);
  if (written != bytes) return false;
  s.copied += written;
  return true;
}
}  // namespace

class J716fRootWindowDevice : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(J716fRootWindowDevice)

 private:
  j716f_r41::Transport *transport_ = nullptr;
  unsigned window_id_ = 0;
  uint64_t abs_offset_ = 0;
  uint64_t window_bytes_ = 0;
  uint64_t blocks512_ = 0;
  bool online_ = false;
  char product_[40] = {};

 public:
  bool configure(j716f_r41::Transport *transport, unsigned window_id,
                 const char *label, uint64_t abs_offset_bytes, uint64_t window_bytes);
  bool start(IOService *provider) override;
  void free() override;

  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("Lenovo"); }
  char *getProductString() override { return product_; }
  char *getRevisionString() override { return const_cast<char *>("r41"); }
  char *getAdditionalDeviceInfoString() override {
    return const_cast<char *>("SM8250 UFS 3.1 read-only SHNGROOT window");
  }
  IOReturn reportBlockSize(UInt64 *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = j716f_r41::kLogicalBlockBytes;
    return kIOReturnSuccess;
  }
  IOReturn reportEjectability(bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportMaxValidBlock(UInt64 *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = blocks512_ ? (blocks512_ - 1) : 0;
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    if (present) *present = online_;
    if (changed) *changed = false;
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportWriteProtection(bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = true;
    return kIOReturnSuccess;
  }
  IOReturn doSynchronizeCache() override {
    return online_ ? kIOReturnSuccess : kIOReturnNotReady;
  }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override {
    return doSynchronizeCache();
  }
  IOReturn getWriteCacheState(bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                            IOStorageAttributes *attributes,
                            IOStorageCompletion *completion) override;
};

OSDefineMetaClassAndStructors(J716fRootWindowDevice, IOBlockStorageDevice)

bool J716fRootWindowDevice::configure(j716f_r41::Transport *transport, unsigned window_id,
                                      const char *label, uint64_t abs_offset_bytes,
                                      uint64_t window_bytes) {
  if (!transport || window_id >= kTrackedWindows || !label || !label[0] ||
      (abs_offset_bytes & 4095ULL) != 0 || window_bytes == 0 ||
      (window_bytes & 511ULL) != 0) {
    return false;
  }
  transport_ = transport;
  window_id_ = window_id;
  abs_offset_ = abs_offset_bytes;
  window_bytes_ = window_bytes;
  blocks512_ = window_bytes / j716f_r41::kLogicalBlockBytes;
  snprintf(product_, sizeof(product_), "TB-J716F UFS RO %.20s", label);
  return true;
}

bool J716fRootWindowDevice::start(IOService *provider) {
  if (!transport_ || !blocks512_ || !IOBlockStorageDevice::start(provider)) return false;

  OSDictionary *protocol = OSDictionary::withCapacity(2);
  OSString *location = OSString::withCString(kIOPropertyInternalKey);
  OSString *interconnect = OSString::withCString(kIOPropertyPhysicalInterconnectTypeVirtual);
  const bool proto_ok =
      protocol && location && interconnect &&
      protocol->setObject(kIOPropertyPhysicalInterconnectLocationKey, location) &&
      protocol->setObject(kIOPropertyPhysicalInterconnectTypeKey, interconnect) &&
      setProperty(kIOPropertyProtocolCharacteristicsKey, protocol);
  OSSafeReleaseNULL(interconnect);
  OSSafeReleaseNULL(location);
  OSSafeReleaseNULL(protocol);
  if (!proto_ok) return false;

  OSDictionary *chars = OSDictionary::withCapacity(1);
  OSString *medium = OSString::withCString(kIOPropertyMediumTypeSolidStateKey);
  if (chars && medium && chars->setObject(kIOPropertyMediumTypeKey, medium)) {
    setProperty(kIOPropertyDeviceCharacteristicsKey, chars);
  }
  OSSafeReleaseNULL(medium);
  OSSafeReleaseNULL(chars);

  online_ = true;
  registerService();
  return true;
}

void J716fRootWindowDevice::free() {
  online_ = false;
  transport_ = nullptr;
  IOBlockStorageDevice::free();
}

IOReturn J716fRootWindowDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block,
                                                 UInt64 count, IOStorageAttributes *,
                                                 IOStorageCompletion *completion) {
  auto &st = g_win_stats[window_id_ < kTrackedWindows ? window_id_ : 0];
  if (!buffer || !completion || !completion->action || !count || block >= blocks512_ ||
      count > blocks512_ - block ||
      buffer->getLength() < count * static_cast<UInt64>(j716f_r41::kLogicalBlockBytes)) {
    __atomic_fetch_add(&st.errors, 1ULL, __ATOMIC_RELAXED);
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection() != kIODirectionIn) {
    __atomic_fetch_add(&st.writes_refused, 1ULL, __ATOMIC_RELAXED);
    return kIOReturnNotWritable;
  }
  if (!online_ || !transport_ || !transport_->online()) {
    __atomic_fetch_add(&st.errors, 1ULL, __ATOMIC_RELAXED);
    return kIOReturnNotReady;
  }

  DescriptorSink sink{buffer, 0};
  uint64_t transferred = 0;
  const bool ok = transport_->readWindow512WithSink(
      abs_offset_, window_bytes_, block, count, writeDescriptorSink, &sink, &transferred);
  const uint64_t expected_bytes = count * static_cast<uint64_t>(j716f_r41::kLogicalBlockBytes);
  const IOReturn status =
      (ok && sink.copied == expected_bytes && transferred == expected_bytes)
          ? kIOReturnSuccess
          : kIOReturnIOError;
  if (status == kIOReturnSuccess) {
    __atomic_fetch_add(&st.reads, 1ULL, __ATOMIC_RELAXED);
    __atomic_fetch_add(&st.bytes, expected_bytes, __ATOMIC_RELAXED);
  } else {
    __atomic_fetch_add(&st.errors, 1ULL, __ATOMIC_RELAXED);
  }
  completion->action(completion->target, completion->parameter, status, sink.copied);
  return kIOReturnSuccess;
}

extern "C" size_t j716f_r41_base_class_size(void) {
  return sizeof(IOBlockStorageDevice);
}

extern "C" IOService *j716f_r41_create_window_device(j716f_r41::Transport *transport,
                                                     unsigned window_id,
                                                     const char *label,
                                                     uint64_t abs_offset_bytes,
                                                     uint64_t window_bytes) {
  J716fRootWindowDevice *dev = new J716fRootWindowDevice;
  if (!dev) return nullptr;
  if (!dev->init(nullptr) ||
      !dev->configure(transport, window_id, label, abs_offset_bytes, window_bytes)) {
    dev->release();
    return nullptr;
  }
  return dev;
}

extern "C" void j716f_r41_window_stats(unsigned window_id,
                                       uint64_t *reads, uint64_t *bytes_read,
                                       uint64_t *writes_refused, uint64_t *errors) {
  if (window_id >= kTrackedWindows) return;
  const auto &st = g_win_stats[window_id];
  if (reads) *reads = __atomic_load_n(&st.reads, __ATOMIC_RELAXED);
  if (bytes_read) *bytes_read = __atomic_load_n(&st.bytes, __ATOMIC_RELAXED);
  if (writes_refused) *writes_refused = __atomic_load_n(&st.writes_refused, __ATOMIC_RELAXED);
  if (errors) *errors = __atomic_load_n(&st.errors, __ATOMIC_RELAXED);
}
