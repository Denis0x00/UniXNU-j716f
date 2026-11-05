/* SPDX-License-Identifier: MIT */
#ifndef NATIVE_ADRENO_GPU_CONTEXT_H
#define NATIVE_ADRENO_GPU_CONTEXT_H
#include "NativeAdrenoGmuContext.h"

/* The GPU's own SMMU context, built like the GMU's (NativeAdrenoGmuContext): a native4K lower-region (TTBR0) table
 * pool for a 0..2 GiB GPU space, one context bank, the GPU's two streams routed to it, then the kernel buffers the
 * command processor needs at fixed GPU addresses below the A740 GMEM window (UCHE_GMEM_RANGE_MIN 16 MiB):
 *   sqe      the SQE microcode (a740_sqe.fw without its first dword, as adreno_fw_create_bo copies it), R+X
 *   ring     ring buffer 0, 32 KiB, aligned to its size, R+W
 *   memptrs  msm_rbmemptrs layout: +0 rptr, +4 fence, +8 bv_rptr, +12 bv_fence, R+W
 *   pwrup    the a7xx power-up register list (cpu_gpu_lock header + pairs), R+W
 *   scratch  one page the first submissions may write, R+W
 * All mappings are Normal non-cacheable (Linux maps kernel BOs write-combined) and outer shareable; the CPU must use
 * a matching non-cacheable view. Bank, ASID, SMR slots and stream IDs are caller inputs. Nothing is torn down.
 * The buffers lie at 32 MiB and up, as Linux keeps every GPU address below 16 MiB free (msm's address space starts at SZ_16M):
 * the a650 family's GMEM is reached through the UCHE at 0 (Linux reports GMEM base 0 and leaves UCHE_GMEM_RANGE alone), and the
 * SQE buffer at 1 MiB would lie inside that window. */
enum : uint64_t {
    GpuSqeIova=0x2000000, GpuRingIova=0x2100000, GpuMemptrsIova=0x2200000, GpuPwrupIova=0x2201000,
    GpuScratchIova=0x2202000, GpuApertureBytes=0x80000000
};
enum : size_t {GpuSqeBytes=0x13000, GpuRingBytes=0x8000, GpuPageBytes=0x1000, GpuBufferCount=5};
struct NativeAdrenoGpuBuffer {uint64_t physical,bytes,iova;};
class NativeAdrenoGpuContext final {
    NativeAdrenoMemory tables_,buffers_[GpuBufferCount];
    uint64_t handles_[GpuBufferCount]{};
    NativeAdrenoSmmuContext smmu_;
    NativeAdrenoVm vm_;
    /* The alias checks' sort scratch: one entry per range of the smaller compared side, at most half the aperture's leaves. */
    uint32_t alias_scratch_[NativeAdrenoSmmuContext::MaxLeaves/2];
    bool ready_=false,used_=false;
public:
    enum Step : uint32_t {Allocate=1,Prepare,Program,Attach,VmAttach,Map,Ready};
    enum : size_t {TablePages=1027};
    NativeAdrenoGpuContext()=default;
    NativeAdrenoGpuContext(const NativeAdrenoGpuContext &)=delete;
    NativeAdrenoGpuContext &operator=(const NativeAdrenoGpuContext &)=delete;
    /* The report is the GMU context's: route counts cover every stream. Routes (SMR slot and exact stream ID, 1..8) as the
     * GMU context's: pipa's GPU iommus <0 0x401> is four streams, 0, 1, 0x400 and 0x401. */
    IOReturn bringUp(const ArmSmmu4KIo *io,unsigned bank,unsigned asid,const ArmSmmu4KStream *routes,size_t route_count,
        void (*progress)(uint32_t step),NativeAdrenoGmuContextReport *report);
    bool ready() const {return ready_;}
    const NativeAdrenoMemory &buffer(unsigned index) const {return buffers_[index];}
    NativeAdrenoVm *vm() {return ready_?&vm_:nullptr;}
};
extern "C" IOReturn NativeAdrenoGpuContextBringup(volatile uint8_t *smmu,size_t bytes,unsigned bank,unsigned asid,
    unsigned slot0,unsigned sid0,unsigned slot1,unsigned sid1,void (*progress)(uint32_t),
    NativeAdrenoGmuContextReport *report);
/* The same with several routes (slots[i] matches sids[i]), count 1..8. */
extern "C" IOReturn NativeAdrenoGpuContextBringupRoutes(volatile uint8_t *smmu,size_t bytes,unsigned bank,unsigned asid,
    const unsigned *slots,const unsigned *sids,unsigned count,void (*progress)(uint32_t),NativeAdrenoGmuContextReport *report);
/* sqe, ring, memptrs, pwrup, scratch: physical base, size and GPU address, only after Ready. */
extern "C" IOReturn NativeAdrenoGpuContextBuffers(NativeAdrenoGpuBuffer out[5]);
/* The live VM of the ready GPU context (null before Ready), for the user-client core. */
extern "C" NativeAdrenoVm *NativeAdrenoGpuContextVm();
#endif
