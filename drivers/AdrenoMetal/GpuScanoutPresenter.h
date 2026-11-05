// SPDX-License-Identifier: MIT
// GPU presentation: the GPU copies a finished BGRA frame (the compositor's IOSurface pages) into the firmware scanout memory. Both are
// imported without a copy (newBufferWithBytesNoCopy: pinned by the kernel and mapped into the GPU's 4 KiB context), the copy is a blit on
// the GPU's queue, and the call returns after the GPU retired it. No CPU touches a pixel; there is no staging copy and no software fallback
// here. Used through the public Metal API, on any device (the native frontend on the tablet, the Mac's device for the host check).
#pragma once
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>

extern "C" void sys_dcache_flush(void *start, size_t length);

namespace gpupresent {

// What a present costs, in nanoseconds, by step, and which copy form each frame took: 0 the frame's rows and the scanout's run together (one
// buffer copy), 1 the frame as a linear texture over its pages (one buffer-image copy, or a buffer copy per row where that texture has no image
// of its own), 2 row by row from here, 3 the copy kernel (one dispatch, rows of 16-byte vectors). Reported through
// native_bridge_metal_present_cost (NativeBridgeMetalFrontend.h, its A4 row).
struct Cost {
    uint64_t frames = 0;
    uint64_t importNs = 0, textureNs = 0, encodeNs = 0, submitNs = 0, waitNs = 0;
    uint64_t forms[4] = {};
    uint64_t texturesMade = 0, texturesReused = 0;
    // The last frame's width, height and stride, then the scanout's (its A4 row): the two pitches decide the copy's form.
    uint64_t geometry[6] = {};
};
static inline uint64_t nowNs() {
    static mach_timebase_info_data_t base;
    if (!base.denom) mach_timebase_info(&base);
    return mach_absolute_time() * base.numer / base.denom;
}

struct Scanout {
    void *base;       // writable mapping of the firmware framebuffer (page aligned)
    size_t bytes;     // whole extent, a multiple of the page size
    size_t stride;    // bytes per row
    uint32_t width, height;
};

struct Frame {
    const void *base; // page-aligned mapping of the surface pages
    size_t bytes;     // allocation size, a multiple of the page size
    size_t stride;
    uint32_t width, height;
    // The display driver's mapping cache (NGPSwap.reserved): 0x80 | slot | generation << 8 while that mapping stays the same, 0 for a
    // mapping made for this frame only.
    uint32_t tag = 0;
};

enum Result { kOk = 0, kBadFrame, kBadScanout, kNoResource, kGpuFailed };

class Presenter {
public:
    Presenter(id<MTLDevice> device, const Scanout &scanout) : device_(device), scanout_(scanout) {
        queue_ = [device newCommandQueue];
        if (!validScanout() || !queue_) return;
        target_ = [device newBufferWithBytesNoCopy:scanout.base length:scanout.bytes options:MTLResourceStorageModeShared deallocator:nil];
        if (target_ && scanout.width >= kOverlayX + kOverlayWidth && scanout.height >= kOverlayY + kOverlayHeight &&
            !(kOverlayWidth * 4 % [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm])) {
            overlayBuffer_ = [device newBufferWithLength:kOverlayWidth * 4 * kOverlayHeight options:MTLResourceStorageModeShared];
            MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:kOverlayWidth
                                                                                       height:kOverlayHeight mipmapped:NO];
            d.storageMode = MTLStorageModeShared;
            d.usage = MTLTextureUsageShaderRead;
            if (overlayBuffer_) overlay_ = [overlayBuffer_ newTextureWithDescriptor:d offset:0 bytesPerRow:kOverlayWidth * 4];
            if (overlay_) setOverlay("FPS -");
        }
    }
    bool ready() const { return target_ != nil; }
    // The scale kernel (shaders/scale_bgra.ll), for frames of another size than the scanout's: a board may render below the panel's size
    // (NativeMobileDisplay's render size). Without it such a frame is refused, and the display driver copies it with the CPU.
    // The kernel writes an ordinary buffer of the scanout's layout, which a blit then copies onto the scanout in one top-to-bottom run:
    // written straight into the scanout, in the dispatch's 16x16 tiles, the scale took about 35 ms a frame of uncached stores and the
    // panel showed a tear (tb128fu-084/085).
    void setScaler(id<MTLComputePipelineState> scaler) {
        scaler_ = scaler;
        if (scaler_ && !scaled_) scaled_ = [device_ newBufferWithLength:scanout_.stride * scanout_.height options:MTLResourceStorageModePrivate];
        if (!scaled_) scaler_ = nil;
    }
    bool scaling() const { return scaler_ != nil; }
    // The copy kernel (shaders/copy_rows.ll), for a frame of the scanout's size whose pitch differs from the scanout's: one dispatch writes
    // the scanout row by row from the surface pages, where the blit of the frame's linear texture is one 2D blit per row whenever the
    // scanout's pitch is not a multiple of 64 (pipa: 7200, 2880 blits a frame). Both pitches must be multiples of 16; without the kernel
    // the blit stays.
    void setCopier(id<MTLComputePipelineState> copier) { copier_ = copier; }
    // Whether row y of the scanout lies in the overlay box (a read-back comparison skips it).
    bool overlayRow(uint32_t y) const { return overlay_ && y >= kOverlayY && y < kOverlayY + kOverlayHeight; }
    // A small opaque box near the top left of the panel, copied by the GPU over each frame right after the frame itself, so no CPU store
    // meets the copy (the diagnostic overlay drawn into the scanout flickered against it): the text in a 5x7 font scaled by 3.
    void setOverlay(const char *text) {
        if (!overlay_) return;
        uint32_t *pixels = (uint32_t *)[overlayBuffer_ contents];
        for (uint32_t i = 0; i < kOverlayWidth * kOverlayHeight; ++i) pixels[i] = 0xff000000u;
        uint32_t x = 6;
        for (const char *c = text; *c && x + 15 <= kOverlayWidth; ++c, x += 18) {
            const uint8_t *glyph = overlayGlyph(*c);
            for (uint32_t row = 0; row < 7; ++row)
                for (uint32_t column = 0; column < 5; ++column)
                    if (glyph[row] & (0x10 >> column))
                        for (uint32_t dy = 0; dy < 3; ++dy)
                            for (uint32_t dx = 0; dx < 3; ++dx)
                                pixels[(5 + row * 3 + dy) * kOverlayWidth + x + column * 3 + dx] = 0xff40ff40u;
        }
        sys_dcache_flush(pixels, kOverlayWidth * 4 * kOverlayHeight);
    }
    // One frame: encodes the GPU copy (or the scale onto the whole scanout), commits, waits for the GPU. Rows are copied as whole rows of
    // width*4 bytes.
    Result present(const Frame &frame) {
        if (!ready()) return kBadScanout;
        const uint64_t began = nowNs();
        const size_t page = (size_t)getpagesize(), row = (size_t)frame.width * 4;
        const bool scaled = frame.width != scanout_.width || frame.height != scanout_.height;
        if (!frame.base || ((uintptr_t)frame.base & (page - 1)) || !frame.bytes || (frame.bytes & (page - 1)) ||
            !frame.width || !frame.height || (scaled && (!scaler_ || (frame.stride & 3) || (scanout_.stride & 3))) || frame.stride < row ||
            (size_t)frame.height > frame.bytes / frame.stride || frame.stride * (frame.height - 1) + row > frame.bytes) return kBadFrame;
        // The compositor cycles through a few surfaces and the display driver keeps this task's mapping of each (frame.tag): the import of
        // a cached mapping is made once and reused, instead of pinning and mapping about 2400 pages into the GPU and back every frame.
        id<MTLBuffer> source = nil;
        Cached *cached = (frame.tag & 0x80) ? &cached_[frame.tag & 3] : nullptr;
        if (cached) {
            if (cached->buffer && cached->tag == frame.tag && cached->base == frame.base && cached->bytes == frame.bytes) {
                source = cached->buffer;
                ++imports_[1];
            } else {
                cached->buffer = [device_ newBufferWithBytesNoCopy:(void *)frame.base length:frame.bytes options:MTLResourceStorageModeShared
                                                       deallocator:nil];
                cached->tag = frame.tag; cached->base = frame.base; cached->bytes = frame.bytes;
                cached->texture = nil;   // another mapping: its texture is not this one's
                source = cached->buffer;
                ++imports_[0];
            }
        } else {
            source = [device_ newBufferWithBytesNoCopy:(void *)frame.base length:frame.bytes options:MTLResourceStorageModeShared deallocator:nil];
            ++imports_[0];
        }
        if (!source) return kNoResource;
        id<MTLCommandBuffer> commands = [queue_ commandBuffer];
        if (!commands) return kNoResource;
        if (scaled) {
            // One thread a scanout pixel: the bilinear mix of the four frame texels around it (the kernel's params, in its header).
            id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
            if (!compute) return kNoResource;
            const uint32_t params[8] = {frame.width, frame.height, (uint32_t)(frame.stride / 4), scanout_.width, scanout_.height,
                                        (uint32_t)(scanout_.stride / 4), (uint32_t)(((uint64_t)frame.width << 16) / scanout_.width),
                                        (uint32_t)(((uint64_t)frame.height << 16) / scanout_.height)};
            const NSUInteger side = scaler_.maxTotalThreadsPerThreadgroup >= 256 ? 16 : 8;
            [compute setComputePipelineState:scaler_];
            [compute setBuffer:source offset:0 atIndex:0];
            [compute setBuffer:scaled_ offset:0 atIndex:1];
            [compute setBytes:params length:sizeof params atIndex:2];
            [compute dispatchThreadgroups:MTLSizeMake((scanout_.width + side - 1) / side, (scanout_.height + side - 1) / side, 1)
                    threadsPerThreadgroup:MTLSizeMake(side, side, 1)];
            [compute endEncoding];
        }
        cost_.geometry[0] = frame.width; cost_.geometry[1] = frame.height; cost_.geometry[2] = frame.stride;
        cost_.geometry[3] = scanout_.width; cost_.geometry[4] = scanout_.height; cost_.geometry[5] = scanout_.stride;
        // What the frame's import, its texture and the copy's encoding cost (cost), and which form the copy takes.
        uint64_t step = nowNs();
        cost_.importNs += step - began;
        uint32_t form = 0;
        const bool kernelCopy = !scaled && copier_ && frame.stride != scanout_.stride && !(frame.stride & 15) && !(scanout_.stride & 15) &&
                                !(row & 15);
        if (kernelCopy) {
            // One thread a 16-byte vector of a row; a threadgroup is a run of one row, so the scanout is written front to back.
            id<MTLComputeCommandEncoder> compute = [commands computeCommandEncoder];
            if (!compute) return kNoResource;
            const uint32_t params[4] = {(uint32_t)(row / 16), frame.height, (uint32_t)(frame.stride / 16), (uint32_t)(scanout_.stride / 16)};
            const NSUInteger side = copier_.maxTotalThreadsPerThreadgroup >= 128 ? 128 : 64;
            [compute setComputePipelineState:copier_];
            [compute setBuffer:source offset:0 atIndex:0];
            [compute setBuffer:target_ offset:0 atIndex:1];
            [compute setBytes:params length:sizeof params atIndex:2];
            [compute dispatchThreadgroups:MTLSizeMake((params[0] + side - 1) / side, frame.height, 1) threadsPerThreadgroup:MTLSizeMake(side, 1, 1)];
            [compute endEncoding];
            form = 3;
        }
        // The copy of the frame (or of its scaled image) onto the scanout, and the overlay after it.
        id<MTLBlitCommandEncoder> blit = [commands blitCommandEncoder];
        if (!blit) return kNoResource;
        if (kernelCopy) {
            // The dispatch above wrote the frame; the blit encoder carries only the overlay.
        } else if (scaled) {
            [blit copyFromBuffer:scaled_ sourceOffset:0 toBuffer:target_ destinationOffset:0
                            size:scanout_.stride * (scanout_.height - 1) + (size_t)scanout_.width * 4];
        } else if (frame.stride == scanout_.stride) {
            // Equal pitch: the rows are one contiguous run.
            [blit copyFromBuffer:source sourceOffset:0 toBuffer:target_ destinationOffset:0 size:frame.stride * (frame.height - 1) + row];
        } else {
            // A linear texture over the surface pages reads them with the surface's pitch and writes the scanout's pitch: one buffer-image
            // copy where the runtime's image is that buffer's own bytes, else one buffer copy per row. The texture is the mapping's, made at
            // its first frame and kept while the mapping is (a new one a frame was an image made and thrown away a frame).
            form = 1;
            if (frame.stride % [device_ minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm]) {
                form = 2;
                for (uint32_t y = 0; y < frame.height; ++y)
                    [blit copyFromBuffer:source sourceOffset:y * frame.stride toBuffer:target_ destinationOffset:y * scanout_.stride size:row];
            } else {
                id<MTLTexture> texture = nil;
                const uint64_t textureBegan = nowNs();
                if (cached && cached->texture && cached->stride == frame.stride && cached->width == frame.width &&
                    cached->height == frame.height) {
                    texture = cached->texture;
                    ++cost_.texturesReused;
                } else {
                    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:frame.width
                                                                                               height:frame.height mipmapped:NO];
                    d.storageMode = MTLStorageModeShared;
                    d.usage = MTLTextureUsageShaderRead;
                    texture = [source newTextureWithDescriptor:d offset:0 bytesPerRow:frame.stride];
                    ++cost_.texturesMade;
                    if (texture && cached) {
                        cached->texture = texture; cached->stride = frame.stride; cached->width = frame.width; cached->height = frame.height;
                    }
                }
                if (!texture) {[blit endEncoding]; return kNoResource;}
                {
                    const uint64_t acquired = nowNs();
                    cost_.textureNs += acquired - textureBegan;
                    step = acquired;
                }
                [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                           sourceSize:MTLSizeMake(frame.width, frame.height, 1) toBuffer:target_ destinationOffset:0
                    destinationBytesPerRow:scanout_.stride destinationBytesPerImage:scanout_.stride * frame.height];
            }
        }
        if (overlay_)
            [blit copyFromTexture:overlay_ sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(kOverlayWidth, kOverlayHeight, 1) toBuffer:target_
                destinationOffset:kOverlayY * scanout_.stride + kOverlayX * 4 destinationBytesPerRow:scanout_.stride
                destinationBytesPerImage:scanout_.stride * kOverlayHeight];
        [blit endEncoding];
        {
            const uint64_t encoded = nowNs();
            cost_.encodeNs += encoded - step;
            step = encoded;
        }
        [commands commit];
        {
            const uint64_t submitted = nowNs();
            cost_.submitNs += submitted - step;
            step = submitted;
        }
        [commands waitUntilCompleted];
        cost_.waitNs += nowNs() - step;
        ++cost_.frames;
        ++cost_.forms[form < 4 ? form : 0];
        return commands.status == MTLCommandBufferStatusCompleted ? kOk : kGpuFailed;
    }
    // The cost so far, and the copy forms taken (Cost).
    const Cost &cost() const {return cost_;}
private:
    bool validScanout() const {
        const size_t page = (size_t)getpagesize();
        return scanout_.base && !((uintptr_t)scanout_.base & (page - 1)) && scanout_.bytes && !(scanout_.bytes & (page - 1)) &&
               scanout_.width && scanout_.height && scanout_.stride >= (size_t)scanout_.width * 4 &&
               scanout_.stride * scanout_.height <= scanout_.bytes;
    }
    // Rows of 5 bits, the leftmost pixel in bit 4; anything without a glyph is a blank.
    static const uint8_t *overlayGlyph(char c) {
        static const uint8_t digits[10][7] = {
            {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}, {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e},
            {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}, {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e},
            {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}, {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e},
            {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}, {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08},
            {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}, {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}};
        static const struct {char c; uint8_t rows[7];} letters[] = {
            {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}}, {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
            {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}}, {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
            {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}}, {'m', {0x00, 0x00, 0x1a, 0x15, 0x15, 0x11, 0x11}},
            {'s', {0x00, 0x00, 0x0e, 0x10, 0x0e, 0x01, 0x1e}}, {'-', {0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00}},
            {'.', {0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c}}};
        static const uint8_t blank[7] = {};
        if (c >= '0' && c <= '9') return digits[c - '0'];
        for (const auto &letter : letters) if (letter.c == c) return letter.rows;
        return blank;
    }
    static constexpr uint32_t kOverlayWidth = 320, kOverlayHeight = 32, kOverlayX = 24, kOverlayY = 72;
    id<MTLDevice> device_;
    Scanout scanout_;
    id<MTLCommandQueue> queue_;
    id<MTLBuffer> target_;
    id<MTLBuffer> overlayBuffer_;
    id<MTLTexture> overlay_;
    id<MTLComputePipelineState> scaler_;
    id<MTLComputePipelineState> copier_;
    id<MTLBuffer> scaled_;   // the scaled frame, in the scanout's layout, before the blit onto the scanout
    struct Cached {uint32_t tag = 0; const void *base = nullptr; size_t bytes = 0; size_t stride = 0; uint32_t width = 0, height = 0;
                   id<MTLBuffer> buffer; id<MTLTexture> texture;};
    Cost cost_;
    Cached cached_[4];
public:
    uint64_t imports_[2] = {};   // surfaces imported, frames that reused an import
};

}  // namespace gpupresent
