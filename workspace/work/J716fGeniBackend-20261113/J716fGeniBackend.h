/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef J716F_GENI_BACKEND_H
#define J716F_GENI_BACKEND_H
#include "../J716fTouchTransaction-20261113/J716fTouchTransaction.h"
#ifdef __cplusplus
extern "C" {
#endif

#define JB_MMIO_BASE UINT64_C(0x00a94000)
#define JB_MMIO_BYTES 0x4000u
#define JB_MMIO_BUDGET 32u
#define JB_MAX_FIFO_WORDS 63u
#define JB_MAX_RECOVERY_MS 1000u

/* These are board-owner attestations, never synthesized by this backend.
 * The production provider must bind them to real resource/clock/power/pin,
 * exclusive SE ownership, old DMA termination and polling-route evidence.
 * POWER_CLOCK includes the actual 19.2MHz source/rate, not just clock enable;
 * POLLING_ROUTE includes GSI exclusion and no other M IRQ consumer. */
enum {
 JB_P_RESOURCE=1u<<0, JB_P_POWER_CLOCK=1u<<1, JB_P_PINS_RESET=1u<<2,
 JB_P_EXCLUSIVE=1u<<3, JB_P_OLD_DMA_STOPPED=1u<<4,
 JB_P_POLLING_ROUTE=1u<<5
};
#define JB_REQUIRED_PERMIT (JB_P_RESOURCE|JB_P_POWER_CLOCK|JB_P_PINS_RESET|JB_P_EXCLUSIVE|JB_P_OLD_DMA_STOPPED|JB_P_POLLING_ROUTE)
typedef struct {
 uint64_t lease_id, mmio_base;
 uint32_t mmio_bytes, evidence;
} JBPermit;

/* A local observation, not a complete bus fence. No presumed IOS bit layout is
 * used. terminal() must establish physical/electrical bus termination and no
 * late operation effects against the still-held lease. A mock is host evidence
 * only. Missing terminal provider makes success impossible. */
typedef struct {
 uint64_t lease_id, epoch, token, serial, observed_ms;
 JTActionKind kind;
 uint32_t status, irq_status, tx_fifo_status, rx_fifo_status;
 uint16_t requested, received;
 uint8_t cancelled, aborted;
} JBTerminalRecord;

typedef struct {
 void *context;
 /* Return 1 only for a completed access. A failed write may have reached HW;
  * failure immediately isolates this binding. Calls must be ordered, bounded,
  * synchronous, non-reentrant and must never retain backend pointers. */
 int (*read32)(void *,uint32_t offset,uint32_t *value);
 int (*write32)(void *,uint32_t offset,uint32_t value);
 int (*barrier)(void *);
 /* Return 1 with a fresh matching permit; 0/-1 denies access. */
 int (*permit)(void *,uint64_t lease_id,JBPermit *observed);
 /* 1=independently proven terminal; 0=not yet proven; -1=lost/invalid proof.
  * It does not supply any of JT_BUS_FENCE_EVIDENCE. */
 int (*terminal)(void *,const JBTerminalRecord *);
} JBIO;

typedef struct {
 uint64_t lease_id;
 uint32_t cancel_timeout_ms, abort_timeout_ms;
} JBConfig;
typedef enum {
 JB_OK=0, JB_PENDING, JB_COMPLETE, JB_BUSY, JB_BAD_ARGUMENT, JB_STALE,
 JB_PERMISSION, JB_UNSUPPORTED, JB_MMIO_ERROR, JB_STATUS_ERROR,
 JB_FIFO_ERROR, JB_LENGTH_ERROR, JB_TIMEOUT, JB_CLOCK_ERROR,
 JB_QUARANTINED, JB_NEEDS_EXTERNAL_FENCE, JB_TIME_RANGE
} JBResult;
typedef enum {
 JB_READY=0, JB_PRECHECK, JB_CONFIGURE, JB_TX_ISSUE, JB_TX_WAIT,
 JB_RX_ISSUE, JB_RX_WAIT, JB_TERMINAL_WAIT, JB_CANCEL_ISSUE,
 JB_CANCEL_WAIT, JB_ABORT_ISSUE, JB_ABORT_WAIT, JB_CLEANUP_WAIT,
 JB_LOCAL_QUIESCENT, JB_ISOLATED, JB_FENCE_CHECK
} JBPhase;

/* Public for allocation-free embedding/diagnostics, not caller mutation.
 * One serial owner only; caller arguments may not alias this object.
 * Storage/buffers survive failures until owner teardown/fence has drained all
 * outside work. No automatic reset, clock write, IRQ registration or DMA. */
typedef struct {
 JBIO io;
 JBConfig config;
 JTAction action, fence;
 JBTerminalRecord terminal_record;
 uint64_t epoch, last_token, last_ms, recovery_deadline, serial;
 uint8_t bytes[JG_MAX_FRAME], tx[4];
 uint32_t tx_depth, rx_depth, last_irq, mmio_used, barrier_used;
 uint32_t cleanup_words;
 uint16_t received;
 uint8_t tx_length, tx_sent, done_seen, last_seen, cleanup_ack;
 uint8_t completion_ready, completion_taken, completion_success;
 uint8_t local_proven, clock_fault, initialized, fence_pending;
 JBPhase phase;
 JBResult fault;
} JBState;

/* Fresh storage only. init does not acquire resources or access registers. */
JBResult jb_init(JBState *,const JBIO *,const JBConfig *,uint64_t epoch,uint64_t now_ms);
/* Only prefix16@824d, tail{4,12,...,68}@825d and ACK824d/00,00. */
JBResult jb_start(JBState *,const JTAction *,uint64_t now_ms);
/* At most 32 MMIO accesses and one bounded phase per call. Poll continuously
 * while active and at/before all deadlines. Completion time is now, not the
 * scheduled timestamp. Any backwards clock permanently isolates this lease. */
JBResult jb_poll(JBState *,uint64_t now_ms);
/* Exactly once, borrowed read buffer valid only until next start/recovery. */
int jb_take_completion(JBState *,JTCompletion *);
/* Requests/replaces only a value fence token, never overlaps cancel/abort.
 * Local evidence is necessary but insufficient: caller must provide the four
 * independent JT bus-fence bits, keyed to this exact action, before reuse. */
JBResult jb_request_fence(JBState *,const JTAction *,uint64_t now_ms);
int jb_fence_ready(const JBState *,JBTerminalRecord *);
JBResult jb_finish_fence(JBState *,const JTCompletion *,uint64_t now_ms);

typedef struct {
 JBState *bus;
 JTState *transaction;
 JTAction external;
 uint8_t external_pending, external_emitted;
 JTResult last_transaction_result;
} JBBridge;
/* Does not initialize or modify the frozen transaction owner. */
JBResult jb_bridge_init(JBBridge *,JBState *,JTState *);
/* Calls jt_tick/next/complete and polls the real backend. Publishes an external
 * action once: PUBLISH/SINK_FENCE unchanged; BUS_FENCE only after local proof.
 * An all-zero output means no action. Never reports synthetic HID success. */
JBResult jb_bridge_step(JBBridge *,uint64_t epoch,uint64_t now_ms,JTAction *external);
JTResult jb_bridge_complete_external(JBBridge *,uint64_t now_ms,const JTCompletion *);

#ifdef __cplusplus
}
#endif
#endif
