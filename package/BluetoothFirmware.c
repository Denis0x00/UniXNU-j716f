// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "BluetoothFirmware.h"
#include "FatVolume.h"
#include <IOKit/IOLib.h>
#include <string.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/param.h>
#include <sys/uio.h>
#include <sys/vnode.h>

enum { kMaxFirmwareBytes = 2 * 1024 * 1024 };

extern int native_ufs_part_find(const char *name, uint32_t *lun, uint64_t *first_lba, uint64_t *blocks) __attribute__((weak_import));
extern int native_ufs_part_read(uint32_t lun, uint64_t lba, uint32_t blocks, void *dst) __attribute__((weak_import));

struct bt_ufs_ctx {
    uint32_t lun;
    uint64_t first_lba;
    uint64_t blocks;
    uint64_t cached;
    uint8_t block[4096];
};

static int bt_ufs_read(void *ctx, uint64_t offset, void *buf, size_t size) {
    struct bt_ufs_ctx *c = (struct bt_ufs_ctx *)ctx;
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

static bool valid_component(const char *s, size_t max_len) {
    if (!s || !s[0]) return false;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok || (c == '.' && s[i + 1] == '.') || i + 1 >= max_len) return false;
    }
    return true;
}

static const char *allowed_leaf(const char *name) {
    if (!name || strncmp(name, "qca/", 4) != 0) return NULL;
    const char *leaf = name + 4;
    if (!valid_component(leaf, 48)) return NULL;
    if (strncmp(leaf, "htbtfw", 6) == 0 || strncmp(leaf, "htnv", 4) == 0 ||
        strncmp(leaf, "hmtbtfw", 7) == 0 || strncmp(leaf, "hmtnv", 5) == 0 ||
        strncmp(leaf, "msbtfw", 6) == 0 || strncmp(leaf, "msnv", 4) == 0)
        return leaf;
    return NULL;
}

static int read_from_ufs_partition(const char *leaf, uint8_t **data, size_t *size) {
    if (!native_ufs_part_find || !native_ufs_part_read)
        return ENOENT;

    const char *fat_path = NULL;
    if (strncmp(leaf, "htbtfw", 6) == 0 || strncmp(leaf, "hmtbtfw", 7) == 0 || strncmp(leaf, "msbtfw", 6) == 0)
        fat_path = "image/htbtfw20.tlv";
    else if (strncmp(leaf, "htnv", 4) == 0 || strncmp(leaf, "hmtnv", 5) == 0 || strncmp(leaf, "msnv", 4) == 0)
        fat_path = "image/htnv20.bin";
    else
        return ENOENT;

    struct bt_ufs_ctx *uctx = IOMallocType(struct bt_ufs_ctx);
    FatVolume *vol = IOMallocType(FatVolume);
    if (!uctx || !vol) {
        if (uctx) IOFreeType(uctx, struct bt_ufs_ctx);
        if (vol) IOFreeType(vol, FatVolume);
        return ENOMEM;
    }

    static const char *const kBtParts[2] = {"bluetooth_a", "bluetooth_b"};
    FatFile file = {0, 0, 0};
    int found = 0;
    for (size_t p = 0; p < 2 && !found; ++p) {
        uctx->cached = UINT64_MAX;
        if (!native_ufs_part_find(kBtParts[p], &uctx->lun, &uctx->first_lba, &uctx->blocks))
            continue;
        if (fat_mount(vol, uctx->blocks * 4096ull, bt_ufs_read, uctx) != 0)
            continue;
        if (fat_open(vol, fat_path, &file) == 0 && !file.directory && file.size > 0 && file.size <= kMaxFirmwareBytes) {
            found = 1;
            break;
        }
    }
    if (!found) {
        IOFreeType(uctx, struct bt_ufs_ctx);
        IOFreeType(vol, FatVolume);
        return ENOENT;
    }

    uint8_t *buf = (uint8_t *)IOMallocData(file.size);
    if (!buf) {
        IOFreeType(uctx, struct bt_ufs_ctx);
        IOFreeType(vol, FatVolume);
        return ENOMEM;
    }

    if (fat_read(vol, &file, 0, buf, file.size) != 0) {
        IOFreeData(buf, file.size);
        IOFreeType(uctx, struct bt_ufs_ctx);
        IOFreeType(vol, FatVolume);
        return EIO;
    }

    IOFreeType(uctx, struct bt_ufs_ctx);
    IOFreeType(vol, FatVolume);
    *data = buf;
    *size = file.size;
    return 0;
}

int sheng_bt_firmware_read(const char *name, uint8_t **data, size_t *size) {
    if (!data || !size) return EINVAL;
    *data = NULL;
    *size = 0;
    const char *leaf = allowed_leaf(name);
    if (!leaf) return ENOENT;

    /* Prefer UFS bluetooth_a/bluetooth_b partition (image/htbtfw20.tlv, image/htnv20.bin) */
    if (read_from_ufs_partition(leaf, data, size) == 0)
        return 0;

    char path[160];
    int n = snprintf(path, sizeof(path), "%s/%s", SHENG_BT_FIRMWARE_ROOT, leaf);
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

void sheng_bt_firmware_release(uint8_t *data, size_t size) {
    if (data && size) IOFreeData(data, size);
}

/* Route qbt_linux.c's qca_uart_setup call to QCA_QCA6390 (enum value 8 in btqca.h) on TB-J716F. */
struct hci_dev;
struct qca_btsoc_version {
    uint32_t product_id;
    uint16_t patch_ver;
    uint16_t rom_ver;
    uint32_t soc_id;
} __attribute__((packed));

extern int qca_uart_setup_btqca(struct hci_dev *hdev, uint8_t baudrate,
                                int soc_type, struct qca_btsoc_version ver,
                                const char *firmware_name, const char *rampatch_name);

int qbt_lx_qca_uart_setup(struct hci_dev *hdev, uint8_t baudrate,
                          int soc_type, struct qca_btsoc_version ver,
                          const char *firmware_name, const char *rampatch_name) {
    (void)soc_type;
    /* QCA_QCA6390 == 8 in enum qca_btsoc_type */
    return qca_uart_setup_btqca(hdev, baudrate, 8, ver, firmware_name, rampatch_name);
}
