// SPDX-License-Identifier: BSD-2-Clause-Patent
// Reads QCA6390 firmware files from UFS modem_a/modem_b (image/qca6390/*) first, falling back to VFS (/private/var/ngpu/firmware/ath11k/QCA6390/hw2.0/*).
#include "WifiFirmware.h"
#include "FatVolume.h"
#include <IOKit/IOLib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <sys/vnode.h>

enum { kMaxFirmwareBytes = 16 * 1024 * 1024 };

extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first_lba, uint64_t *blocks) __attribute__((weak_import));
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst) __attribute__((weak_import));

struct wifi_ufs_ctx {
    uint32_t lun;
    uint64_t first_lba;
    uint64_t blocks;
    uint64_t cached;
    uint8_t block[4096];
};

static int wifi_ufs_read(void *ctx, uint64_t offset, void *buf, size_t size) {
    struct wifi_ufs_ctx *c = (struct wifi_ufs_ctx *)ctx;
    uint8_t *out = (uint8_t *)buf;
    if (!native_ufs_part_read)
        return EIO;
    while (size) {
        const uint64_t lba = offset / 4096u;
        const size_t within = (size_t)(offset % 4096u);
        if (!within && size >= 4096u) {
            const uint32_t n = size / 4096u > 256u ? 256u : (uint32_t)(size / 4096u);
            if (!native_ufs_part_read(c->lun, c->first_lba + lba, n, out))
                return EIO;
            out += (size_t)n * 4096u;
            offset += (uint64_t)n * 4096u;
            size -= (size_t)n * 4096u;
            continue;
        }
        if (c->cached != lba) {
            c->cached = UINT64_MAX;
            if (!native_ufs_part_read(c->lun, c->first_lba + lba, 1u, c->block))
                return EIO;
            c->cached = lba;
        }
        const size_t take = 4096u - within < size ? 4096u - within : size;
        memcpy(out, c->block + within, take);
        out += take;
        offset += take;
        size -= take;
    }
    return 0;
}

static int valid_name(const char *name) {
    if (!name || !name[0]) return 0;
    for (size_t i = 0; name[i]; ++i) {
        char c = name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok || (c == '.' && name[i + 1] == '.') || i >= 48) return 0;
    }
    return 1;
}

static int read_from_modem_partition(const char *name, uint8_t **data, size_t *size) {
    if (!native_ufs_part_find || !native_ufs_part_read)
        return ENOENT;

    char dynamic_cand0[48];
    char dynamic_cand1[48];
    const char *candidates[3] = {NULL, NULL, NULL};
    if (strcmp(name, "amss.bin") == 0 || strcmp(name, "amss20.bin") == 0) {
        candidates[0] = "image/qca6390/amss20.bin";
        candidates[1] = "image/amss20.bin";
    } else if (strcmp(name, "m3.bin") == 0) {
        candidates[0] = "image/qca6390/m3.bin";
        candidates[1] = "image/m3.bin";
    } else if (strcmp(name, "board.bin") == 0 || strcmp(name, "bdwlan.elf") == 0) {
        candidates[0] = "image/qca6390/bdwlan.elf";
        candidates[1] = "image/bdwlan.elf";
    } else if (strncmp(name, "bdwlan.", 7) == 0) {
        snprintf(dynamic_cand0, sizeof(dynamic_cand0), "image/qca6390/%s", name);
        snprintf(dynamic_cand1, sizeof(dynamic_cand1), "image/%s", name);
        candidates[0] = dynamic_cand0;
        candidates[1] = dynamic_cand1;
    } else {
        return ENOENT;
    }

    struct wifi_ufs_ctx *uctx = IOMallocType(struct wifi_ufs_ctx);
    FatVolume *vol = IOMallocType(FatVolume);
    if (!uctx || !vol) {
        if (uctx) IOFreeType(uctx, struct wifi_ufs_ctx);
        if (vol) IOFreeType(vol, FatVolume);
        return ENOMEM;
    }

    static const char *const kModemParts[2] = {"modem_a", "modem_b"};
    FatFile file = {0, 0, 0};
    int found = 0;
    for (size_t p = 0; p < 2 && !found; ++p) {
        uctx->cached = UINT64_MAX;
        if (!native_ufs_part_find(kModemParts[p], &uctx->lun, &uctx->first_lba, &uctx->blocks))
            continue;
        if (fat_mount(vol, uctx->blocks * 4096ull, wifi_ufs_read, uctx) != 0)
            continue;
        for (size_t i = 0; i < 2 && candidates[i]; ++i) {
            if (fat_open(vol, candidates[i], &file) == 0 && !file.directory && file.size > 0 && file.size <= kMaxFirmwareBytes) {
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        IOFreeType(uctx, struct wifi_ufs_ctx);
        IOFreeType(vol, FatVolume);
        return ENOENT;
    }

    uint8_t *buf = (uint8_t *)IOMallocData(file.size);
    if (!buf) {
        IOFreeType(uctx, struct wifi_ufs_ctx);
        IOFreeType(vol, FatVolume);
        return ENOMEM;
    }

    if (fat_read(vol, &file, 0, buf, file.size) != 0) {
        IOFreeData(buf, file.size);
        IOFreeType(uctx, struct wifi_ufs_ctx);
        IOFreeType(vol, FatVolume);
        return EIO;
    }

    IOFreeType(uctx, struct wifi_ufs_ctx);
    IOFreeType(vol, FatVolume);
    *data = buf;
    *size = file.size;
    return 0;
}

int sheng_wifi_firmware_read(const char *name, uint8_t **data, size_t *size) {
    if (!data || !size || !valid_name(name)) return EINVAL;
    *data = NULL;
    *size = 0;

    if (read_from_modem_partition(name, data, size) == 0)
        return 0;

    char path[160];
    int n = snprintf(path, sizeof(path), "%s/%s", SHENG_WIFI_FIRMWARE_ROOT, name);
    if (n <= 0 || (size_t)n >= sizeof(path)) return ENAMETOOLONG;
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int err = vnode_open(path, FREAD, 0, 0, &vp, ctx);
    if (err) return err;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    err = vnode_getattr(vp, &va, ctx);
    uint64_t length = (!err && VATTR_IS_SUPPORTED(&va, va_data_size)) ? va.va_data_size : 0;
    if (!err && (!length || length > kMaxFirmwareBytes)) err = EFBIG;
    uint8_t *buf = (!err) ? (uint8_t *)IOMallocData((size_t)length) : NULL;
    if (!err && !buf) err = ENOMEM;
    size_t offset = 0;
    while (!err && offset < length) {
        size_t chunk = (size_t)length - offset;
        if (chunk > 128 * 1024) chunk = 128 * 1024;
        int resid = 0;
        err = vn_rdwr(UIO_READ, vp, (caddr_t)(buf + offset), (int)chunk, (off_t)offset,
                      UIO_SYSSPACE, IO_NODELOCKED, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!err && resid == (int)chunk) err = EIO;
        if (!err) offset += chunk - (size_t)resid;
    }
    vnode_close(vp, FREAD, ctx);
    if (err) {
        if (buf) IOFreeData(buf, (size_t)length);
        return err;
    }
    *data = buf;
    *size = (size_t)length;
    return 0;
}
