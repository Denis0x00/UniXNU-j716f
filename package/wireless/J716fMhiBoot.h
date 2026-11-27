// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure C11 Qualcomm MHI / BHI / BHIe firmware boot state machine for QCA6390 on TB-J716F.
// Drives: PBL -> BHI SBL (512 KiB) -> MHI READY -> MHI contexts -> M0 -> SBL -> BHIe amss20.bin -> AMSS (Mission Mode).
#ifndef J716F_MHI_BOOT_H
#define J716F_MHI_BOOT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define J716F_MHI_REGLEN             0x00u
#define J716F_MHI_BHIOFF             0x28u
#define J716F_MHI_BHIEOFF            0x2Cu
#define J716F_MHI_CTRL               0x38u
#define J716F_MHI_STATUS             0x48u
#define J716F_MHI_CTRL_RESET         (1u << 1)
#define J716F_MHI_STATUS_READY       (1u << 0)
#define J716F_MHI_STATUS_SYSERR      (1u << 2)
#define J716F_MHI_STATE_SHIFT        8u

#define J716F_MHI_BHI_IMGADDR_LOW    0x08u
#define J716F_MHI_BHI_IMGADDR_HIGH   0x0Cu
#define J716F_MHI_BHI_IMGSIZE        0x10u
#define J716F_MHI_BHI_IMGTXDB        0x18u
#define J716F_MHI_BHI_INTVEC         0x20u
#define J716F_MHI_BHI_EXECENV        0x28u
#define J716F_MHI_BHI_STATUS         0x2Cu
#define J716F_MHI_BHI_ERRCODE        0x30u

#define J716F_MHI_BHIE_TXVECADDR_LO  0x2Cu
#define J716F_MHI_BHIE_TXVECADDR_HI  0x30u
#define J716F_MHI_BHIE_TXVECSIZE     0x34u
#define J716F_MHI_BHIE_TXVECDB       0x3Cu
#define J716F_MHI_BHIE_TXVECSTATUS   0x44u
#define J716F_MHI_BHIE_RXVECDB       0x70u
#define J716F_MHI_BHIE_RXVECSTATUS   0x78u

#define J716F_MHI_SEQ_MASK           0x3FFFFFFFu
#define J716F_MHI_XFER_SUCCESS       2u
#define J716F_MHI_XFER_ERROR         3u

enum {
    J716fMhiStateReset  = 0,
    J716fMhiStateReady  = 1,
    J716fMhiStateM0     = 2,
    J716fMhiStateBhi    = 7,
    J716fMhiStateSysErr = 0xFF,
};

enum {
    J716fMhiEePbl   = 0,
    J716fMhiEeSbl   = 1,
    J716fMhiEeAmss  = 2,
    J716fMhiEeRddm  = 3,
    J716fMhiEeWfw   = 4,
    J716fMhiEePthru = 5,
    J716fMhiEeEdl   = 6,
    J716fMhiEeFp    = 7,
};

enum {
    J716fMhiIdle = 0,
    J716fMhiResetting,
    J716fMhiSblLoading,
    J716fMhiWaitReady,
    J716fMhiNeedContexts,
    J716fMhiWaitSbl,
    J716fMhiImageLoading,
    J716fMhiWaitMission,
    J716fMhiDone,
    J716fMhiFailed,
};

enum {
    J716fMhiErrNone = 0,
    J716fMhiErrLayout,
    J716fMhiErrNotPbl,
    J716fMhiErrBhi,
    J716fMhiErrBhie,
    J716fMhiErrSysErr,
    J716fMhiErrTimeout,
    J716fMhiErrState,
    J716fMhiErrRddm,
};

typedef struct J716fMhiIo {
    uint32_t (*read)(void *ctx, uint32_t reg);
    void (*write)(void *ctx, uint32_t reg, uint32_t value);
    void *ctx;
    uint32_t space;
} J716fMhiIo;

typedef struct J716fMhiBoot {
    J716fMhiIo io;
    uint32_t bhi, bhie, reglen;
    uint32_t phase, error, failed_phase, ee, state;
    uint32_t seq, errcode, last_status;
    uint64_t sbl_addr, table_addr;
    uint32_t sbl_size, table_size, msi_vector;
} J716fMhiBoot;

typedef struct J716fMhiBhieEntry {
    uint64_t address;
    uint64_t size;
} J716fMhiBhieEntry;

int j716f_mhi_boot_open(J716fMhiBoot *m, const J716fMhiIo *io);
uint32_t j716f_mhi_boot_ee(J716fMhiBoot *m);
uint32_t j716f_mhi_boot_state(J716fMhiBoot *m);
uint32_t j716f_mhi_boot_begin(J716fMhiBoot *m, uint32_t msi_vector,
                              uint64_t sbl_addr, uint32_t sbl_size,
                              uint64_t table_addr, uint32_t table_size,
                              uint32_t seq);
uint32_t j716f_mhi_boot_step(J716fMhiBoot *m);
uint32_t j716f_mhi_boot_contexts_done(J716fMhiBoot *m);
uint32_t j716f_mhi_boot_timeout(J716fMhiBoot *m);
uint32_t j716f_mhi_bhie_table(J716fMhiBhieEntry *table, uint32_t count,
                              const uint64_t *addresses, uint32_t image_size,
                              uint32_t segment_size);

#ifdef __cplusplus
}
#endif

#endif /* J716F_MHI_BOOT_H */
