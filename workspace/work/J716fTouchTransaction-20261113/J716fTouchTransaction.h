/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef J716F_TOUCH_TRANSACTION_H
#define J716F_TOUCH_TRANSACTION_H
#include "../J716fGoodix-20261113/J716fGoodix.h"
#ifdef __cplusplus
extern "C" {
#endif

#define JT_COORDINATE_REGISTER 0x824du
#define JT_I2C_ADDRESS 0x5du
#define JT_PREFIX_BYTES 16u
#define JT_MAX_TIMEOUT_MS 60000u
typedef enum {
 JT_OK=0, JT_IDLE, JT_BUSY, JT_STALE, JT_BAD_ARGUMENT, JT_STOPPED,
 JT_TIMEOUT, JT_CLOCK, JT_TIME_RANGE, JT_TOKEN_EXHAUSTED, JT_IO_ERROR,
 JT_PROTOCOL_ERROR, JT_SINK_ERROR, JT_FENCE_REQUIRED, JT_NOT_QUIESCENT
} JTResult;
typedef enum {
 JT_ACTION_NONE=0, JT_READ_PREFIX, JT_READ_TAIL, JT_ACK,
 JT_PUBLISH, JT_BUS_FENCE, JT_SINK_FENCE
} JTActionKind;
typedef enum { JT_RUNNING=0, JT_STOPPING, JT_QUIESCENT } JTLifecycle;
/* These bits are assertions by the trusted external adapter, not facts this
 * core can discover. A completion that lacks its required evidence fails.
 * IO_TERMINAL means the operation can no longer affect the bus or access
 * adapter buffers; callback delivery alone is insufficient.
 * BUS_FENCE must exclude old queued/DMA/device effects AND future callbacks.
 * SINK_FENCE must order/drain all earlier reports including timed-out ones. */
enum {
 JT_E_IO_TERMINAL=1u<<0,
 JT_E_BUS_QUEUE_DRAINED=1u<<1,
 JT_E_BUS_DMA_QUIESCENT=1u<<2,
 JT_E_BUS_NO_LATE_EFFECT=1u<<3,
 JT_E_BUS_CALLBACKS_DRAINED=1u<<4,
 JT_E_SINK_APPLIED_ORDERED=1u<<5,
 JT_E_SINK_QUEUE_DRAINED=1u<<6,
 JT_E_SINK_NO_LATE_EFFECT=1u<<7,
 JT_E_SINK_CALLBACKS_DRAINED=1u<<8
};
#define JT_BUS_FENCE_EVIDENCE (JT_E_BUS_QUEUE_DRAINED|JT_E_BUS_DMA_QUIESCENT|JT_E_BUS_NO_LATE_EFFECT|JT_E_BUS_CALLBACKS_DRAINED)
#define JT_SINK_FENCE_EVIDENCE (JT_E_SINK_QUEUE_DRAINED|JT_E_SINK_NO_LATE_EFFECT|JT_E_SINK_CALLBACKS_DRAINED)
typedef struct {
 uint32_t bus_timeout_ms, sink_timeout_ms, fence_timeout_ms, silence_timeout_ms;
} JTConfig;
/* Action is copied by value. The caller must copy it into adapter-owned work
 * storage before jt_next returns to its event loop. No pointer to mutable
 * transaction-owned bytes is exported. PUBLISH is a FULL ten-slot state, not
 * a sparse delta: the all-zero state releases every previously published slot.
 * ACK always carries two zero bytes. No transaction is performed by this API. */
typedef struct {
 JTActionKind kind;
 uint64_t epoch, token, deadline_ms;
 uint16_t reg, length;
 uint8_t i2c_address;
 uint8_t write[2];
 JGSnapshot snapshot;
} JTAction;
typedef struct {
 JTActionKind kind;
 uint64_t epoch, token;
 int success;
 uint32_t evidence;
 const uint8_t *bytes; /* borrowed only during jt_complete */
 size_t length;
} JTCompletion;
/* Storage is public for no-allocation embedding and diagnostics, not mutation.
 * All calls and timer/IRQ/sink callbacks must be serialized by one owner.
 * Every now_ms is captured when handling the event, not an old scheduled
 * timestamp. API action/completion/buffer arguments must not alias state. */
typedef struct {
 JTConfig config;
 JGState core;
 JTAction current;
 JGBatch batch;
 JGSnapshot delivered;
 uint64_t epoch, last_token, frame_token, last_ms, last_progress_ms;
 uint8_t bytes[JG_MAX_FRAME];
 uint16_t acquired_bytes;
 uint8_t phase, batch_index, irq_pending, bus_dirty, sink_dirty;
 uint8_t release_needed, sink_unknown, silence_latched;
 JTLifecycle lifecycle;
 JTResult last_fault;
 JGResult last_packet;
 uint32_t packets_acked, packets_discarded;
} JTState;

/* Fresh storage only. Existing storage may be reused only through resume
 * after QUIESCENT; init is not cancellation or a substitute for draining. */
JTResult jt_init(JTState *state,uint64_t epoch,uint64_t now_ms,const JTConfig *config);
JTResult jt_signal_frame(JTState *state,uint64_t epoch,uint64_t now_ms);
/* Each call emits at most one action and starts its finite deadline. Never
 * execute the same action twice. BUSY means an earlier action is outstanding. */
JTResult jt_next(JTState *state,uint64_t epoch,uint64_t now_ms,JTAction *action);
JTResult jt_complete(JTState *state,uint64_t now_ms,const JTCompletion *completion);
/* The adapter must schedule this at/before the earliest current deadline and
 * stream-silence deadline even when there are no IRQs. Polling is not built in. */
JTResult jt_tick(JTState *state,uint64_t epoch,uint64_t now_ms);
JTResult jt_stop(JTState *state,uint64_t epoch,uint64_t now_ms);
/* Requires stopped, all fences proven, final all-up applied, and no old work
 * capable of effects. The external adapter must also quiesce IRQ/timers before
 * owner destruction; delayed value-only callbacks are safe to reject by epoch.
 * Strictly greater nonzero epoch; timestamp may begin a new monotonic domain. */
JTResult jt_resume(JTState *state,uint64_t new_epoch,uint64_t now_ms);
#ifdef __cplusplus
}
#endif
#endif
