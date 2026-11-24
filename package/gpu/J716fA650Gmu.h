/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC, Adreno 650 v2.0 chipid 0x06050002) GMU cold-start, HFI startup,
 * and GX power bring-up engine using upstream UniXNU kits/AdrenoFirmware.
 * Verified against TB-J716F stock DTB:
 *   qcom,kgsl-3d0@3d00000 (chipid 0x06050002, gpu-opp-table_v2 305..670 MHz / levels 64..320)
 *   qcom,gmu@3d6a000 (0x30000), PDC 0xb290000 (0x10000), RSCC 0x3de0000 (0x10000)
 * Host-tested under ASan/UBSan by package/gpu/tests/J716fA650GmuTest.c. */
#ifndef J716F_A650_GMU_H
#define J716F_A650_GMU_H

#include "AdrenoHfiStartup.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { A650GmuCore = 0, A650GmuRscc = 1, A650GmuPdc = 2, A650GmuWindows = 3 };
#define A650_GMU_CORE_BYTES 0x30000u
#define A650_GMU_RSCC_BYTES 0x10000u
#define A650_GMU_PDC_BYTES  0x10000u
#define A650_GMU_CHIP_ID    0x06050002u

typedef enum {
    A650GmuSuccess = 0,
    A650GmuArgument,
    A650GmuFirmware,
    A650GmuBusy,
    A650GmuIoError,
    A650GmuStateMismatch,
    A650GmuTimeout,
    A650GmuClockError,
    A650GmuPoisoned,
    A650GmuFault,
    A650GmuProtocolError,
} A650GmuStatus;

typedef struct {
    void *cookie;
    size_t bytes[A650GmuWindows];
    int (*read32)(void *, unsigned window, size_t byte_offset, uint32_t *);
    int (*write32)(void *, unsigned window, size_t byte_offset, uint32_t);
    int (*barrier)(void *);
} A650GmuIo;

typedef struct {
    void *cookie;
    int (*now_us)(void *, uint64_t *);
    int (*wait_us)(void *, unsigned);
} A650GmuServices;

typedef struct {
    volatile uint32_t *cpu;
    size_t bytes;
    uint64_t iova;
} A650GmuMemory;

typedef struct {
    uint32_t chip_id;
    const uint8_t *firmware;
    size_t firmware_bytes;
    A650GmuMemory icache, dummy, log, hfi;
    unsigned poll_budget;
} A650GmuConfig;

typedef struct {
    A650GmuIo io;
    A650GmuServices services;
    AdrenoHfi hfi;
    uint32_t poisoned, boot_attempted;
    AdrenoHfiStartup startup;
} A650GmuDevice;

typedef struct {
    uint32_t phase, write_attempts, read_attempts, poll_reads;
    uint32_t blocks, memory_changed, reset_verified;
    uint32_t rsc_stop_status, rscc_ack, firmware_version, firmware_init_result, hfi_status;
    uint32_t firmware_ready_observed, hfi_ready_observed;
    uint32_t ao_fault_status, cm3_fault_status;
    uint32_t resources_must_retain;
} A650GmuResult;

typedef struct {
    unsigned polls_per_command, wait_interval_us;
    uint64_t command_timeout_us, total_timeout_us;
} A650GmuStartupLimits;

typedef struct {
    A650GmuResult load;
    AdrenoHfiStartupResult protocol;
    AdrenoHfiStartupStatus protocol_status;
    A650GmuStatus health_status;
} A650GmuStartupResult;

typedef struct {
    uint32_t vote_status, vote_error, oob_polls, oob_info, gx_before, gx_after;
    uint32_t gx_on;
} A650GmuGxResult;

extern unsigned A650GmuDdrVote;

A650GmuStatus A650GmuLoadAndStartHfi(A650GmuDevice *, const A650GmuConfig *, A650GmuResult *);
A650GmuStatus A650GmuColdStart(A650GmuDevice *, const A650GmuConfig *, const AdrenoHfiStartupConfig *,
                               const A650GmuStartupLimits *, A650GmuStartupResult *);
A650GmuStatus A650GmuPowerGx(A650GmuDevice *, unsigned perf_index, A650GmuGxResult *);
AdrenoHfiStartupStatus A650GmuBuildStartup(const uint8_t *gfx, size_t gfx_bytes,
                                           const uint8_t *mx, size_t mx_bytes,
                                           const uint8_t *cx, size_t cx_bytes,
                                           uint32_t mc0, uint32_t sh0, uint32_t acv, uint32_t cn0,
                                           AdrenoHfiStartupConfig *out);

#ifdef __cplusplus
}
#endif
#endif /* J716F_A650_GMU_H */
