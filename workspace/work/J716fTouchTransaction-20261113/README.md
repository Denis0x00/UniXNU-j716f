# J716F Goodix transaction owner

This is a new, no-allocation C11 transaction layer around the **unchanged**
`../J716fGoodix-20261113` packet/contact core. It produces explicit actions for a
future transport and full-state sink adapter. It performs no I2C, GENI, MMIO,
DMA, IRQ, GPIO, timer scheduling, IOKit, HID, configuration upload, or firmware
upload itself. No placeholder backend reports success and no device target is
silently enabled. Source tests compile the actual `.c` files.

## Stock binding and deliberate differences

Direct stock evidence comes from
`../../analysis/stock-touch-transaction-20261113/stock-ack/README.md` and the
previous frozen `../../analysis/stock-goodix-20261113/` results. The reference is
the actual **TB_J716F_ZUI_15.0.485** boot and vendor images, not a related tablet.

| Action | Stock-bound value |
|---|---|
| Slave | DT I2C address `0x5d` |
| Prefix | Read 16 bytes at `0x824d` |
| Finger tail | Read at `0x825d`; count 2 needs 4 bytes, count 10 needs 68 |
| Maximum acquired finger packet | 84 bytes, including the initial 16 |
| ACK | Write **two zero bytes** at `0x824d` |
| Register wire order | High byte first; ACK message bytes are `82 4d 00 00` |

The original IRQ path normally reports input before writing its ACK and ignores
the ACK result. Some malformed packets return the successful initial-read status.
This layer intentionally uses stricter ordering:

1. Read an exact prefix and only a validated bounded tail.
2. Independently validate with the frozen core and keep a copied pending frame.
3. Receive actual terminal success for the two-byte ACK.
4. Commit the core and offer its snapshots in order to the external sink.

A callback or a nonnegative stock event return is not evidence of a valid frame,
completed I2C transaction, or successful HID delivery. The selected ACK-before-
publication ordering, deadlines, disarming, fencing and release rules are **new
driver policies**, not claims that the original IRQ routine implemented them.

Malformed, unsupported pen/non-touch, and held-contact/disarmed frames receive
one discard ACK after a completed read; no contact frame is committed. Their
previous published contacts are released, and a valid zero-contact frame must
be ACKed before subsequent contacts can publish. Transport errors first require
an actual bus fence; only then may one discard ACK clear an unknown pending
event. A repeated failing discard ACK remains quarantined, rather than granting
read access. This does **not** implement stock request-register `0x8044` servicing,
configuration resend, gesture, or pen support. Such events may require platform
recovery even though the coordinate event has been ACKed.

## Adapter contract

All API calls have one serialized owner. Fresh storage uses `jt_init`; initialization
does not acquire hardware. Before offering frames, an actual adapter must establish
the observed controller identity, supplies/pinctrl/reset, exclusive bus ownership,
IRQ routing, and a usable full-state report sink. No such production adapter is
included. The test timeouts are arbitrary synthetic values, not qualified device
timings.

`jt_signal_frame` coalesces a controller notification or a deliberate poll request.
`jt_next` emits at most one action with an epoch, nonzero strictly increasing token,
and a finite checked absolute deadline. `BUSY` emits nothing. Actions, ACK bytes,
and snapshots are returned by value; the adapter must copy the returned action
into its own work object. Read completion bytes are borrowed only during
`jt_complete` and copied before it returns. No state or stack pointer is exported
for asynchronous work. API argument buffers must not overlap `JTState`, except
`jt_init`'s configuration, which is copied before the state is reset.

The adapter must schedule `jt_tick` for both the current action deadline and the
stream-silence deadline, including when no IRQ arrives. Every `now_ms` is the
monotonic time **when processing the event**, not a saved timer deadline. A stale
timer in the same epoch can call `jt_tick` with current time; it must not replay an
old timestamp. Epoch/token/kind mismatches are rejected before touching the clock
or current work. New epochs are strictly greater, and only a fully quiescent
stopped owner can resume. Epoch/token counters never wrap.

`JT_E_IO_TERMINAL` means that the completed operation can no longer modify the
bus, touch adapter buffers, or perform a delayed side effect. Callback delivery
alone does not meet this contract. Every failure, partial read, missing terminal
evidence or timeout requires a bus fence before new bus work. A bus fence must
cover the entire owner's earlier queued work, DMA, possible device effects, and
callbacks; the four separate evidence bits are mandatory. Rejecting an old token
does **not** cancel that physical operation.

Fence evidence is supplied by a future trusted adapter. This pure layer cannot
measure it, manufacture it, or turn a boolean mock into a hardware result. The
adapter must serialize/coalesce repeated fence requests after a fence timeout and
include the earlier fence work in the drain. It must not overlap resets or bus
commands because another fence action was emitted. The same rule applies to sink
fences. A deadline that cannot be represented, or exhausted token space, emits no
new operation and enters stopping. If cleanup then cannot receive a new token or
deadline, the owner remains nonquiescent and requires independent backend teardown;
software must not pretend cleanup succeeded.

## Publication and final release

`JT_PUBLISH` is a **complete ten-slot state**, not a sparse contact delta. A zero
snapshot must release every slot the sink may previously have exposed. The sink
adapter must implement that meaning in its actual HID/report format. Its ordered
success evidence means this exact snapshot was applied and will not take effect
again later.

The transaction owner tracks acknowledged core state separately from successfully
published sink state. Slot reuse follows the frozen core's two-snapshot ordering:
release old identities, then assign new ones. Failure or timeout of either report
discards the rest of the batch and disarms the stream. Even if the first release
succeeded, a failed second report is potentially visible and must be fenced and
followed by a full all-up. A failed final all-up remains a cleanup obligation;
successful fencing alone cannot clear it. This is logical sink recovery, not proof
that an Apple HID service exists.

Known published contacts can be released while an old bus operation is quarantined;
no new read/ACK uses that bus until its fence is proven. An uncertain sink is fenced
before another report. Packet acquisition and its ACK finish before ordinary sink
publication, so a slow sink does not hold that sensor ACK indefinitely.

## Stop, recovery and remaining hardware work

Silence releases the last published contacts even without an outstanding ACK and
requires clean-zero recovery. A backwards clock enters `STOPPING`; it cannot
silently change the active epoch's clock domain. After fences and all-up, a new
epoch may establish a new monotonic baseline and receive its own clean-zero frame.

`jt_stop` invalidates current work, drains/fences outstanding domains, and requires
all-up. Repeated stop calls preserve cleanup already in flight. `QUIESCENT` means
all generated operations are terminal or fenced and no published contacts remain;
the external adapter still must mask/unregister IRQ and timers and drain their
callbacks before destroying owner storage. Late value-only callbacks against
live/resumed storage are rejected by epoch/token.

Fault handling clears a coalesced software IRQ. A real adapter must mask/resample
the physical IRQ, re-enable it safely, and schedule any necessary recovery polling;
this layer does not assume a new edge will arrive or poll the sensor automatically.
After recovery it may be necessary to explicitly request a clean-zero frame. IRQ
level, pin transitions, electrical bus recovery, controller transfer/DMA limits,
GENI clocks, MMIO ownership, reset/configuration requests, rotation, and HID mapping
remain unimplemented and unqualified. The code has not been run on the tablet.

## Source validation

See `../../analysis/stock-touch-transaction-20261113/README.md`. The source manifest
binds the new implementation/tests and unchanged core to their exact hashes.
Host sanitizer and ordinary ARM64 object/archive results are kept by the parent
integration report; neither establishes physical touch operation.
