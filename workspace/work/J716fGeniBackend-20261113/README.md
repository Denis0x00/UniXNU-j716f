# J716F bounded GENI FIFO backend

This new C11 package implements register operations and connects them to the
unchanged `J716fTouchTransaction-20261113` owner. It does not enable a tablet
target. Hardware operation still requires a real board resource/ownership and
terminal-evidence provider. No such provider is fabricated by the core, the MMIO
boundary, or the tests. The deployed r6 and earlier accepted sources stay separate.

## Source boundary

The hardware source is **TB_J716F_ZUI_15.0.485**. The stock instruction and constant
audit is `../../analysis/stock-geni-20261113/README.md`; Goodix wire details are
`../../analysis/stock-touch-transaction-20261113/stock-ack/README.md`.
The generic register cross-check uses Linux commit
`adc218676eef25575469234709c2d87185ca223a`. The implementation was also compared
with sheng's `package/GeniI2c.c/.h` at
`1108068537d5396edb2c63b2f8c6f5b97b0a4bbc`; it does not copy its fallback FIFO depth,
unbounded count drain, or timeout-to-software-Idle behavior.

Stock proves the actual controller binding, IF_DISABLE whole-word branch,
protocol extraction, TX width and TX/RX depth helpers, packing, timing table,
cancel/abort writes, and Goodix register framing. It does not prove that today's
firmware handoff leaves FIFO mode, powered hardware, a usable polling route,
idle electrical lines, or no outside DMA. RX_LAST validity decoding and the
GENI inactive bits are generic-source references, not new tablet measurements.
The backend's stricter bounds, status precedence and matching cleanup ACK policy
are new policies, not a claim of identical stock error handling.

## Board-facing API

`J716fGeniBackend.h` defines `JBState`, `JBIO`, `JBConfig` and `JBBridge`.
All calls have one serialized owner. Callbacks are synchronous, bounded and
non-reentrant; they cannot retain pointers into the backend. State and buffers
must remain alive through physical teardown and all outside callback draining.
Arguments must not alias backend/bridge state. `jb_init` is for fresh storage,
not cancellation, resume, or a way to erase a fault.

`JBIO` has fault-aware `read32`, `write32` and ordering `barrier` callbacks. Return
1 means completed; a failed write is treated as possibly delivered. Any failed
access/barrier permanently isolates this binding. No retry silently turns an
unreadable register into zero. Each backend poll/finish call performs at most
32 MMIO accesses; loops are bounded by a checked transfer extent or a drain chunk
of at most24 FIFO words. Callback implementation latency is a board obligation.

The owner must supply a current `JBPermit` matching the exact nonzero lease,
physical aperture `0x00a94000/0x4000`, and all six evidence flags:

- acquired Device-memory resource;
- valid supplies/clocks, including the actual 19.2MHz GENI source/rate;
- established pin/reset state;
- exclusive engine ownership for the entire repeated-start pair;
- all earlier DMA terminated;
- a usable exclusive polling route, including GSI exclusion and no competing
  master IRQ consumer.

`J716fGeniMmio.c` is the board boundary. It passes explicit physical addresses to
the borrowed provider and checks offset/value allowlists plus the permit before
every operation. It does not create a mapping, infer a permit from the constants,
or change the ADT converter to accept a fabricated high address. The provider
owns the mapping and outlives every consumer. Binding revocation does not drain
hardware or free its buffers.

## Allowed transport and register sequence

Only slave `0x5d` and these actions are accepted:

| Action | Register | Extent |
| --- | --- | --- |
| `JT_READ_PREFIX` | `0x824d` | exactly16 bytes |
| `JT_READ_TAIL` | `0x825d` | 4,12,20,28,36,44,52,60,68 bytes |
| `JT_ACK` | `0x824d` | exactly two zero bytes |

Address bytes are high then low. Thus the ACK is `82 4d 00 00` on the wire.
A read issues a two-byte address WRITE with STOP_STRETCH, then a READ without
STOP_STRETCH. Completing the address phase is never whole-operation success.
There is no STOP-then-read fallback and no general register-write API.

Before configuration the backend requires I2C firmware, **IF_DISABLE==0**,
DMA_MODE==0, inactive master/secondary engines, empty FIFOs, and no error status.
Nonzero IF_DISABLE/GPI is unsupported. Existing DMA is never disabled by this
backend. Stock TX width must be32 bits; both depth fields must be2..63, with no
invented depth for zero or a wider-generation field. RX HW_PARAM1 width is not
checked because this audit only establishes its stock depth extraction.

Inherited timing must match one stock tuple `(divider,high,low,cycle)`:
`(7,10,11,26)`, `(2,5,12,24)` or `(1,3,9,18)`. This admits the stock100k/400k/1MHz
table only when the owner has independently qualified the source clock. Matching
fields alone do not establish frequency. No clock, power, reset, GPIO, DMA or
SMMU register is written.

The backend programs and reads back the fixed8-bit/four-byte packing, byte
granularity, FIFO watermarks and master IRQ_EN=0. It polls latched status; it
does not register an interrupt or write the top-level SE/GSI IRQ route.
Polling masked IRQ status has a generic GENI console precedent, but remains an
unqualified board condition until the actual provider supplies evidence.

Status errors take precedence over DONE, including combined error/DONE words.
FIFO counts cannot exceed observed depth, the remaining expected bytes, or the
per-call budget. RX_LAST byte-valid0 means a full four-byte word; values greater
than4 and contradictory last lengths are rejected. Exact completion requires
the requested extent, observed LAST and DONE. A short/malformed completion never
publishes its bytes. The tests include multi-poll reads with a smaller FIFO.

## Terminal proof, recovery and reuse

A local completion candidate requires two ordered snapshots with inactive
engines, empty TX/RX FIFOs, no error status, FIFO mode and the same admitted timing.
The core then calls the mandatory external `terminal` provider with an immutable
value record: lease, epoch/token, sequence number, current time and local reads.
The provider must establish physical/electrical bus termination and no later
effects of this operation under the held lease. It returns pending when it lacks
that proof. The stock IOS register exists at `0x908`, but its line-state bits are
not established here, so the backend does not guess an IOS mask.

Only that complete path produces `JT_E_IO_TERMINAL`. M_ACTIVE=0, software Idle,
DONE, a callback, or a host model's boolean alone is insufficient. A terminal
provider that cannot prove the external condition must remain pending/reject.

Failure first produces an unsuccessful completion **without terminal evidence**.
The original physical work then follows cancel -> matching cancel ACK -> bounded
drain. Cancel timeout or a new cancel-stage error escalates to abort. Abort needs
its own newly observed ACK; late original DONE cannot acknowledge either command.
An abort timeout, access failure, unsupported handoff or unknown cleanup error
isolates the binding. Residual RX words can be discarded only after inactivity,
in bounded chunks with a63-word total ceiling; leftover TX data is never erased
with an invented reset sequence. Local cleanup again needs the external terminal
provider and still only reaches `JB_LOCAL_QUIESCENT`.

`JT_BUS_FENCE` is different. `jb_request_fence` records its exact value token and
coalesces retries without overlapping recovery commands. Even after local
quiescence, **all four** externally supplied `JT_BUS_FENCE_EVIDENCE` bits are
required: old work queue, DMA, late device effects and callbacks must each be
drained. No bit is inferred from the terminal callback or an inactive register.
`jb_finish_fence` rejects stale/expired tokens, payload-bearing completions and
missing evidence; it repeats the local observation and trusted terminal check
before permitting reuse. Test booleans describe synthetic fixtures only.

A backwards clock permanently isolates this lease, including invalidation of an
undelivered successful result. Neither a new epoch nor late DONE/ABORT/proof can
revive it. Independent board teardown/recovery must first stop physical effects
and callbacks, retain or release buffers correctly, and establish a fresh owner
before new storage/lease initialization. This package supplies no reset or opaque
recovery-success override.

## Real transaction bridge

`jb_bridge_step` calls the frozen `jt_tick`, consumes a backend completion through
`jt_complete`, obtains the next `JTAction`, and dispatches only the whitelisted
bus actions. Both the original action and buffers remain owned while cancel/abort
proceeds. An old completion rejected by the transaction does not cancel that
physical cleanup or inherit a later fence token.

The bridge emits `JT_PUBLISH` and `JT_SINK_FENCE` unchanged, once per action.
The actual sink supplies its result through `jb_bridge_complete_external`;
there is no HID implementation or synthetic successful report. Bus fences are
emitted to the outside owner only after local quiescence. Before accepting an
external fence the bridge first advances the transaction clock/deadline, then
checks physical reuse, then forwards the same evidence to the transaction.

The owner still provides scheduling, sensor attention handling and a usable sink.
It must keep calling the bridge at/before action and stream-silence deadlines,
even without new IRQs. This package neither registers/masks the shared TLMM IRQ
nor performs GPIO rearm. It does not upload firmware/configuration, service the
Goodix request register, implement a pen, or claim physical touch operation.

## Verification

`tests/test_backend.c` links the actual core, bridge, MMIO boundary, frozen
transaction and frozen Goodix implementation. Its register/FIFO and electrical
termination conditions are explicitly synthetic. Coverage includes all accepted
extents and wire commands, smaller-FIFO streaming, denied ownership/timing/modes,
error/DONE ordering, malformed counts/last bytes, every successful-path MMIO
access failing, barriers/permission loss, cancel/abort, delayed target effects,
drain budgets, all external fence bits, backwards time, late identities,
full ten-contact parser integration and rejected/missing sink evidence.

Example command on a host with clang:

```sh
clang -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  work/J716fGeniBackend-20261113/J716fGeniBackend.c \
  work/J716fGeniBackend-20261113/J716fGeniBridge.c \
  work/J716fGeniBackend-20261113/J716fGeniMmio.c \
  work/J716fTouchTransaction-20261113/J716fTouchTransaction.c \
  work/J716fGoodix-20261113/J716fGoodix.c \
  work/J716fGeniBackend-20261113/tests/test_backend.c -o test_backend
./test_backend
```

Root-task receipts in `../../analysis/geni-backend-20261113/` own the exact source
freeze, host results and official independent kernel integration. They also retain
failed attempts. A compilation/test result is not a measured GENI handoff, I2C
transaction or iPadOS touch event on TB-J716F.
