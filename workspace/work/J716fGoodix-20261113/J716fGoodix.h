/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef J716F_GOODIX_H
#define J716F_GOODIX_H
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
#define JG_MAX_CONTACTS 10
#define JG_MAX_FRAME 84
typedef enum {
 JG_OK=0, JG_SHORT, JG_LENGTH, JG_EVENT, JG_COUNT, JG_PEN,
 JG_CHECKSUM, JG_ID, JG_DUPLICATE, JG_COORDINATE, JG_STALE,
 JG_BUSY, JG_ACK_FAILED, JG_CLOCK, JG_DISARMED, JG_STOPPED, JG_TIMEOUT
} JGResult;
typedef struct { uint8_t count; uint16_t checksum_bytes, acquired_bytes, tail_bytes; } JGPlan;
typedef struct { uint8_t raw_id; uint16_t x,y,width; } JGContact;
typedef struct { uint8_t count; JGContact contacts[JG_MAX_CONTACTS]; } JGFrame;
typedef struct { uint8_t active,raw_id; uint16_t x,y,width; } JGSlot;
typedef struct { JGSlot slots[JG_MAX_CONTACTS]; uint16_t active_mask; } JGSnapshot;
/* Reports are ordered: release old slots first, then reuse in a second report.
 * This is a pure state result, not an IOKit/HID delivery or sink-commit API. */
typedef struct { uint8_t reports; JGSnapshot report[2]; JGResult reason; } JGBatch;
typedef struct {
 uint64_t epoch,last_token,pending_token,last_ms;
 uint8_t stopped,armed,pending,clock_set;
 JGFrame frame;
 JGSnapshot current;
} JGState;
/* Stock ROM profile: config SHA 96b8e74d...941ca, Phoenix, coor 0x824d.
 * Identity matching is pure comparison; it never asserts an I2C read occurred. */
int jg_identity_matches(const uint8_t pid[4],const uint8_t vid[4],uint8_t sensor);
JGResult jg_inspect(const uint8_t *bytes,size_t length,JGPlan *plan);
JGResult jg_decode(const uint8_t *bytes,size_t length,JGFrame *frame);
void jg_init(JGState *state,uint64_t epoch);
JGResult jg_submit(JGState *state,uint64_t epoch,uint64_t token,uint64_t now_ms,
                   const uint8_t *bytes,size_t length,JGBatch *batch);
JGResult jg_finish(JGState *state,uint64_t epoch,uint64_t token,int ack_success,JGBatch *batch);
/* Owner must assign a finite monotonic ACK deadline and call expire if it
 * elapses. This core has no scheduler/timer and is not a complete driver.
 * Async completions/timeouts use epoch+token; stale callbacks are harmless. */
JGResult jg_expire(JGState *state,uint64_t epoch,uint64_t token,JGBatch *batch);
/* Synchronous serialized owner only (e.g. known stream loss), never an
 * unvalidated async callback. Stop/drain old callbacks before init/resume. */
JGResult jg_fault(JGState *state,JGResult reason,JGBatch *batch);
void jg_stop(JGState *state,JGBatch *batch);
#ifdef __cplusplus
}
#endif
#endif
