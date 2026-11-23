/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef J716F_R35_HANDOFF_H
#define J716F_R35_HANDOFF_H
#ifdef R35_EDK2
#include <Base.h>
typedef UINT8 R35U8;
typedef UINT32 R35U32;
typedef UINT64 R35U64;
#else
#include <stdint.h>
typedef uint8_t R35U8;
typedef uint32_t R35U32;
typedef uint64_t R35U64;
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define R35_MAGIC 0x48353352U /* R35H, little endian */
#define R35_VERSION 1U
#define R35_PROFILE 0x8250716fU
#define R35_GROUPS 87U
#define R35_HCI_WORDS 14U
#define R35_CONTEXT_WORDS 11U
#define R35_CONTEXT_ALL 0x7ffU
#define R35_MAX_WRITES 12U
#define R35_RECORD_BYTES 3248U
#define R35_HCI_BASE ((R35U64)0x01d84000)
#define R35_SMMU_BASE ((R35U64)0x15000000)

enum R35Status {
  R35Waiting=0, R35Busy, R35AlreadyAttempted, R35Argument, R35Access,
  R35Clock, R35Deadline, R35TimeReversed, R35Budget, R35Read,
  R35Write, R35Barrier, R35Geometry, R35HostState, R35Routes,
  R35SharedContext, R35Namespace, R35Fault, R35StateChanged,
  R35Readback, R35SyncTimeout, R35Denied, R35Observed, R35Maintained
};
enum R35Phase {
  R35PhaseNone=0, R35CaptureBefore, R35Validate, R35StopHci,
  R35VerifyStopped, R35DisableContexts, R35SyncContexts, R35CaptureAfter,
  R35Done
};
enum R35Mode { R35CaptureOnly=0, R35StopAndMaintain=1 };
enum R35Event { R35Begin=0, R35BeforeCaptured, R35BeforeWrite,
  R35AfterWrite, R35QueueStopped, R35ContextDisabled, R35ContextSynced,
  R35Finalized };
enum R35DetacherPolicy {
  R35BaselineDetacher=0, R35PreserveUfsBanks=1
};
enum R35ContextWord {
  R35Sctlr=0, R35Actlr, R35Cbar, R35Cba2r, R35Tcr, R35Tcr2,
  R35Contextidr, R35Mair0, R35Mair1, R35Fsr, R35Tlbstatus
};
enum R35HciWord {
  R35Cap=0, R35Ver, R35Hce, R35Hcs, R35Is, R35Ie,
  R35TransferBaseLow, R35TransferBaseHigh, R35TransferDoorbell,
  R35TransferRunStop, R35TaskBaseLow, R35TaskBaseHigh,
  R35TaskDoorbell, R35TaskRunStop
};
typedef struct {
  R35U32 smr, s2cr, valid; /* valid bit0=SMR, bit1=S2CR */
} R35Stream;
typedef struct {
  R35U32 bank, valid32, valid64, reserved;
  R35U32 words[R35_CONTEXT_WORDS];
  R35U32 alignment_zero;
  R35U64 ttbr[2];
} R35Context;
typedef struct {
  R35U32 hci[R35_HCI_WORDS], hci_valid;
  R35U32 headers[4], header_valid; /* ID0/ID1/ID2/sCR0 */
  R35U32 stream_count, reserved;
  R35Stream streams[R35_GROUPS];
  R35U32 alignment_zero;
  R35Context context[3]; /* fixed order CB2, CB3, display CB4 */
} R35Snapshot;
typedef struct {
  R35U64 address;
  R35U32 value, phase, completed, reserved;
} R35WriteRecord;
/* Fixed, pointer-free little-endian ABI. All padding/reserved bytes are zero.
 * CRC is corruption detection only, not authenticity, ownership, or TZ grant.
 * Exact byte size is recorded and checked by consumers; never cast unaligned
 * external storage without first copying into a properly aligned object. */
typedef struct {
  R35U32 magic, version, bytes, crc32;
  R35U32 profile, source, boot_sequence, mode;
  R35U64 boot_id;
  R35U32 status, phase, reads32, reads64;
  R35U32 writes_attempted, writes_completed, barriers, clock_queries;
  R35U32 operations, polls, host_stopped, disabled_mask;
  R35U32 synchronized_mask, routes_unchanged, display_config_unchanged, old_namespace_captured;
  R35U32 mutation_possible, smmu_mutation_possible, detacher_policy, preserve_banks;
  R35U32 selected_slots[2], selected_banks[2];
  R35U32 preserve_slots[3], finished;
  R35U32 post_detacher_recorded, post_detacher_preserved, maintenance_record_valid, reserved;
  R35U32 tz_exclusion_proven, dma_ownership_granted, global_dma_drain_proven, root_ready;
  R35U64 started_ticks, last_ticks, failed_address;
  R35Snapshot before, after;
  R35WriteRecord writes[R35_MAX_WRITES];
} R35Record;
typedef char R35RecordSizeMustMatch[(sizeof(R35Record)==R35_RECORD_BYTES)?1:-1];
typedef struct {
  void *context;
  /* Actual pre-detacher local execution/access contract. Never represents TZ
   * exclusion, previous namespace knowledge, or a DMA ownership grant. */
  int (*live)(void *);
  int (*read32)(void *,R35U64,R35U32 *);
  int (*read64)(void *,R35U64,R35U64 *); /* exact native 64-bit TTBR reads */
  int (*write32)(void *,R35U64,R35U32);
  int (*barrier)(void *); /* DSB SY, not merely compiler order */
  int (*ticks)(void *,R35U64 *);
  /* Exact planned write permission. May only narrow the board allowlist.
   * Its true return does NOT populate any owner/TZ/drain output flag. */
  int (*permit)(void *,const R35Record *,R35U64,R35U32);
  void (*note)(void *,enum R35Event,const R35Record *);
} R35Io;
typedef struct {
  R35U64 boot_id, timeout_ticks;
  R35U32 boot_sequence, mode, max_operations, max_polls;
} R35Config;
/* Caller keeps workspace alive until record publication; no allocation,
 * global constructor, dynamic registration, resource release or reset API. */
typedef struct {
  R35U32 busy, used, clock_started, reserved;
  R35Io io;
  R35Config config;
  R35Record record;
  R35Snapshot scratch;
  R35U32 expected_hci[R35_HCI_WORDS];
  R35U32 expected_sctlr[2];
} R35Workspace;

enum R35Status r35_handoff_run(R35Workspace *,const R35Io *,const R35Config *);
R35U32 r35_record_crc(const R35Record *);
int r35_record_validate(const R35Record *);
/* Called after the parent's detacher policy actually completed, before HOB
 * publication. preserved=1 is only permitted if both current route/namespace
 * checks and explicit UFS-bank preservation were performed by that caller.
 * It is still not a kernel owner/grant. A failed record stays unusable. */
void r35_record_post_detacher(R35Record *,int preserved);
const char *r35_status_name(enum R35Status);
R35U64 r35_hci_address(unsigned index);
R35U64 r35_context_address(unsigned bank,unsigned word);

#ifdef __cplusplus
}
#endif
#endif
