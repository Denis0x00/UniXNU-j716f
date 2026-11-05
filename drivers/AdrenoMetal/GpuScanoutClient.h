// SPDX-License-Identifier: MIT
// Userspace side of the GPU hand-off (boot-arg sheng-fb-gpu=1): serves the display driver's frames with the GPU. For every offered frame the
// GPU copies the compositor's surface pages into the scanout mapping (GpuScanoutPresenter.h), and the swap is completed with the GPU's
// verdict. A failed copy is reported as failed; the display driver then falls back to its own path and counts it.
// The calls reach the kernel through a Transport. In the plugin that is the Adreno accelerator's connection (the original sandbox profiles
// admit that class, not the presenter's own service): methods 0x7000 + NGP_METHOD_* through the tunnel selector, scanout as memory type 0x5343.
#pragma once
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include "GpuScanoutPresenter.h"
#include "ScaleKernelAir.h"
#include "CopyRowsAir.h"
#include "../GopFramebuffer/NativeGOPWire.h"

// The render thread's frames and CPU time (NativeBridgeMetalFrontend.mm); absent where the native frontend is not linked (host checks).
extern "C" void native_bridge_metal_frame_cost(uint64_t *frames, uint64_t *cpu_us) __attribute__((weak_import));
// A library from AIR bitcode, which only the native frontend loads (the scale kernel).
extern "C" id native_bridge_metal_library_with_air(const void *bytes, size_t length) __attribute__((weak_import));
// What the scanout copy costs, by step (NativeBridgeMetalFrontend.h's A4 row); absent where the frontend is not linked.
extern "C" void native_bridge_metal_present_cost(const uint64_t *values) __attribute__((weak_import));
extern "C" void NBWaitInFlight(uint64_t beforeUs) __attribute__((weak_import));
extern "C" void NBTraceSwap(uint64_t offered,uint64_t copied,uint64_t completed,uint32_t sequence,uint32_t result) __attribute__((weak_import));

namespace gpuscanout {

struct Transport {
    void *context;
    // NGP_METHOD_MODE / WAIT_SWAP / COMPLETE_SWAP with the wire records; returns the IOReturn (0 = success).
    int (*call)(void *context, uint32_t method, const void *in, size_t inSize, void *out, size_t outSize);
    // The scanout, writable, mapped into this task.
    int (*mapScanout)(void *context, uint64_t *address, uint64_t *length);
    // Progress marker (the plugin shows the latest ones on the boot page, which survives a panic that parks the calling core).
    void (*mark)(void *context, uint32_t value);
};

struct Stats {
    uint64_t frames, failures, timeouts;
    int lastResult;     // gpupresent::Result of the last frame
    int step;           // 0 idle, 2 mode refused, 3 scanout not mappable/importable, 4 serving
    int error;          // the failing call's return, when there is one
    uint64_t verified;  // frames whose scanout was read back and compared with the surface
    uint64_t mismatchRows;   // rows of those frames that differ (CPU reads only: nothing is rendered or copied by the CPU)
    uint32_t lastMs;    // the GPU copy's time for the last frame
    uint64_t totalMs;   // and for all frames (the mean is totalMs / (frames + failures))
    uint64_t imported, reused;   // surface imports made, and frames that reused one (Presenter's cache of the driver's mappings)
    int scaler;         // the scale kernel: 0 not loaded (no AIR loader), 1 ready, 2 library, 3 function, 4 pipeline refused
    uint64_t scaled;    // frames scaled onto the scanout
};

class Client {
public:
    // Reads the mode and maps the scanout; false with stats.step telling where it stopped.
    bool open(id<MTLDevice> device, const Transport &transport, Stats *stats) {
        transport_ = transport;
        stats_ = stats;
        NGPMode mode = {};
        note(0x6710);
        int status = transport_.call(transport_.context, NGP_METHOD_MODE, nullptr, 0, &mode, sizeof mode);
        note(0x6711);
        if (status || mode.version != NGP_VERSION || mode.format != NGP_BGRX8888 || !mode.width || !mode.height ||
            mode.stride < mode.width * 4 || mode.bytes < mode.stride * mode.height) return fail(2, status);
        const size_t page = (size_t)getpagesize();
        uint64_t address = 0, length = 0;
        note(0x6712);
        status = transport_.mapScanout(transport_.context, &address, &length);
        note(0x6713);
        if (status || !address || length < mode.bytes) return fail(3, status);
        // The mapping covers whole pages; the framebuffer allocation ends on a page boundary, the copy never writes past the last row.
        gpupresent::Scanout scanout = {(void *)(uintptr_t)address, (size_t)((mode.bytes + page - 1) & ~(page - 1)), (size_t)mode.stride,
                                       (uint32_t)mode.width, (uint32_t)mode.height};
        note(0x6714);
        scanout_ = scanout;
        presenter_ = new gpupresent::Presenter(device, scanout);
        note(0x6715);
        if (!presenter_->ready()) return fail(3, 0);
        // The scale kernel, for a board that renders below the panel's size: without it such frames take the display driver's CPU copy.
        if (native_bridge_metal_library_with_air) {
            id<MTLLibrary> library = native_bridge_metal_library_with_air(kScaleKernelAir, kScaleKernelAirLength);
            id<MTLFunction> function = library ? [library newFunctionWithName:@"scale_bgra"] : nil;
            NSError *error = nil;
            id<MTLComputePipelineState> scaler = function ? [device newComputePipelineStateWithFunction:function error:&error] : nil;
            presenter_->setScaler(scaler);
            stats_->scaler = scaler ? 1 : !library ? 2 : !function ? 3 : 4;
            note(scaler ? 0x6716 : 0x67f0 | (uint32_t)stats_->scaler);
            // The copy kernel, for a frame whose pitch is not the scanout's: without it the copy stays the blit of the frame's texture.
            id<MTLLibrary> copyLibrary = native_bridge_metal_library_with_air(kCopyRowsAir, kCopyRowsAirLength);
            id<MTLFunction> copyFunction = copyLibrary ? [copyLibrary newFunctionWithName:@"copy_rows"] : nil;
            NSError *copyError = nil;
            id<MTLComputePipelineState> copier = copyFunction ? [device newComputePipelineStateWithFunction:copyFunction error:&copyError] : nil;
            presenter_->setCopier(copier);
            note(copier ? 0x6717 : 0x67f8 | (!copyLibrary ? 1u : !copyFunction ? 2u : 3u));
        }
        stats_->step = 4;
        return true;
    }
    // One wait (at most timeoutMs) and, when the display driver offered a frame, one GPU copy and its completion. Returns false on a
    // protocol error that makes further serving pointless.
    bool serveOne(uint64_t timeoutMs) {
        NGPWait wait = {timeoutMs};
        NGPSwap swap = {};
        if (!stats_->timeouts && !stats_->frames) note(0x6720);   // first wait
        const int status = transport_.call(transport_.context, NGP_METHOD_WAIT_SWAP, &wait, sizeof wait, &swap, sizeof swap);
        if (status) {
            if ((unsigned)status == 0xe00002d6u /* kIOReturnTimeout */) {++stats_->timeouts; return true;}
            ++stats_->failures;
            stats_->error = status;
            return false;
        }
        const size_t page = (size_t)getpagesize();
        note(0x6730);   // a frame was offered
        const uint64_t offeredUs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
        if (NBWaitInFlight) NBWaitInFlight(offeredUs);
        const uint64_t began = mach_absolute_time();
        const gpupresent::Result r = presenter_->present({(const void *)(uintptr_t)swap.address, (size_t)((swap.bytes + page - 1) & ~(page - 1)),
                                                          (size_t)swap.stride, swap.width, swap.height, swap.reserved});
        {
            static mach_timebase_info_data_t base;
            if (!base.denom) mach_timebase_info(&base);
            stats_->lastMs = (uint32_t)(((mach_absolute_time() - began) * base.numer / base.denom) / 1000000);
            stats_->totalMs += stats_->lastMs;
            stats_->imported = presenter_->imports_[0];
            stats_->reused = presenter_->imports_[1];
        }
        stats_->lastResult = (int)r;
        // The copy's cost so far, for the frontend's telemetry (A4, A5): frames, the five steps' ns, the copy forms, the textures made and
        // reused, and the geometry.
        if (native_bridge_metal_present_cost) {
            const gpupresent::Cost &cost = presenter_->cost();
            const uint64_t values[18] = {cost.frames, cost.importNs, cost.textureNs, cost.encodeNs, cost.submitNs, cost.waitNs,
                                         cost.forms[0], cost.forms[1], cost.forms[2], cost.texturesMade, cost.texturesReused,
                                         cost.geometry[0], cost.geometry[1], cost.geometry[2], cost.geometry[3], cost.geometry[4],
                                         cost.geometry[5], cost.forms[3]};
            native_bridge_metal_present_cost(values);
        }
        const bool scaled = swap.width != scanout_.width || swap.height != scanout_.height;
        if (r == gpupresent::kOk && scaled) ++stats_->scaled;
        // The read-back compares rows one to one; a scaled frame has no such rows.
        if (r == gpupresent::kOk && !scaled && stats_->verified < 4) verify(swap);
        if (r == gpupresent::kOk) updateOverlay();
        note(0x6731);   // the GPU copy returned
        const uint64_t copiedUs = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000;
        NGPComplete done = {swap.sequence, r == gpupresent::kOk ? 0u : 0xe00002bcu /* kIOReturnError */, 0};
        const int completed = transport_.call(transport_.context, NGP_METHOD_COMPLETE_SWAP, &done, sizeof done, nullptr, 0);
        if (NBTraceSwap) NBTraceSwap(offeredUs, copiedUs, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000, (uint32_t)swap.sequence, (uint32_t)r);
        if (completed) {++stats_->failures; stats_->error = completed; return false;}
        if (r == gpupresent::kOk) ++stats_->frames; else ++stats_->failures;
        return true;
    }
private:
    // Reads the scanout back with aligned 32-bit loads (the framebuffer is device memory; wider loads can fault) and compares the top rows with the
    // surface the GPU copied. The lower part of the panel is skipped: the diagnostic renderer paints there. Evidence only; nothing is written.
    void verify(const NGPSwap &swap) {
        const uint32_t rows = swap.height < 400 ? swap.height : 400, words = swap.width;
        const volatile uint32_t *shown = (const volatile uint32_t *)scanout_.base;
        const volatile uint32_t *surface = (const volatile uint32_t *)(uintptr_t)swap.address;
        for (uint32_t y = 0; y < rows; ++y) {
            if (presenter_->overlayRow(y)) continue;
            const volatile uint32_t *a = shown + (size_t)y * (scanout_.stride / 4), *b = surface + (size_t)y * (swap.stride / 4);
            for (uint32_t x = 0; x < words; ++x)
                if (a[x] != b[x]) {++stats_->mismatchRows; break;}
        }
        ++stats_->verified;
    }
    // The overlay's text, every half second of frames: the frames served in it per second and the render thread's CPU time per frame
    // over the same span. A pause of a second or more starts a new span, so an idle screen does not count as a low rate.
    void updateOverlay() {
        static mach_timebase_info_data_t base;
        if (!base.denom) mach_timebase_info(&base);
        const uint64_t now = mach_absolute_time() * base.numer / base.denom / 1000;
        uint64_t cost[2] = {};
        if (native_bridge_metal_frame_cost) native_bridge_metal_frame_cost(&cost[0], &cost[1]);
        if (!spanStart_ || now - lastFrame_ >= 1000000) {
            spanStart_ = lastFrame_ = now; spanFrames_ = 0; spanCost_[0] = cost[0]; spanCost_[1] = cost[1];
            return;
        }
        lastFrame_ = now;
        ++spanFrames_;
        if (now - spanStart_ < 500000) return;
        char text[32];
        const unsigned long long fps = (spanFrames_ * 1000000ull + (now - spanStart_) / 2) / (now - spanStart_);
        const uint64_t frames = cost[0] - spanCost_[0];
        if (frames) snprintf(text, sizeof text, "FPS %llu CPU %llums", fps, (unsigned long long)((cost[1] - spanCost_[1]) / frames / 1000));
        else snprintf(text, sizeof text, "FPS %llu", fps);
        presenter_->setOverlay(text);
        spanStart_ = now; spanFrames_ = 0; spanCost_[0] = cost[0]; spanCost_[1] = cost[1];
    }
    uint64_t spanStart_ = 0, lastFrame_ = 0, spanFrames_ = 0, spanCost_[2] = {};
    void note(uint32_t value) {if (transport_.mark) transport_.mark(transport_.context, value);}
    bool fail(int step, int error) {stats_->step = step; stats_->error = error; return false;}
    Transport transport_ = {};
    gpupresent::Presenter *presenter_ = nullptr;
    gpupresent::Scanout scanout_ = {};
    Stats *stats_ = nullptr;
};

}  // namespace gpuscanout
