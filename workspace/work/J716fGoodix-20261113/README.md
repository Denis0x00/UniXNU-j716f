# J716F Goodix: isolated packet and slot core

This is a newly authored, pure C11 protocol/state core for the observed TB-J716F GT7386 profile. It has no I2C, GPIO, power, IRQ, DMA, timer, allocation, IOKit, or HID code. It is not linked into a UniXNU device target. No device or VM operation is part of this directory's implementation work.

The profile is backed by the actual ZUI 15.0.485 vendor configuration and stock boot kernel instruction flow, rather than only a related Linux driver. Evidence, reproducible extraction, source hashes, and synthetic tests are in `../../analysis/stock-goodix-20261113/`.

## Proven facts and deliberate policy

The stock config identifies PID `7386`, sensor `8`, a Phoenix package spelled `phoneix`, and coordinate register **`0x824d`**. Do not substitute the `0x824e` default found in a related source tree. Saved Android observations independently identify VID `00b10023`; the package's VID match field itself is zero. `jg_identity_matches` is only a comparison, not a hardware identity read.

The audited kernel reads a 16-byte prefix. For finger count `n`, the checksum wire extent is `12 + 8 * (n - 1)` for `n >= 1`; the acquired buffer is at least 16 bytes. Counts above 10 and pen packets are outside this core's profile. A nonempty finger packet sums bytes `[1, wire_extent)` modulo 256 to zero; the actual stock path skips this checksum for count zero. Contacts begin at byte 2, use an 8-byte stride, and contain raw ID followed by little-endian X, Y, and width at record offsets 1, 3, and 5. Width zero is a release. Stock remapping skips IDs with the high bit set; this core rejects the whole frame instead. Duplicate IDs are likewise rejected as a new defensive policy.

Original and saved running DT agree on address `0x5d`, panel X/Y values `2560/1600`, reset GPIO 38, IRQ GPIO 39, and swap-axis/x2x properties. This core preserves raw axes, accepts X <= 2560 and Y <= 1600, and performs no rotation or pixel mapping. Inclusive endpoint acceptance is a conservative policy, not a live-qualified statement about coordinate semantics. `panel-max-w=2368` is recorded as evidence but is not imposed as a width validity limit.

Stable slot assignment, release-before-slot-reuse, publish-after-successful-ACK, finite ACK timeout, clean-zero rearm, and fault release are new policies for the prospective driver. They do not claim to reproduce stock IRQ/ACK ordering. Actual stock IRQ-thread ACK machine code has not been audited.

## API ownership

All stateful calls require non-null state and batch pointers, one serialized owner, and no concurrent access. `jg_decode`/`jg_inspect` independently validate supplied byte buffers. A caller must validate its own state/output storage; this is not a hostile-pointer API.

1. Initialize a fresh epoch with `jg_init`. Before reinitializing, stop the old owner, cancel or drain its work, and ensure no old synchronous operation can touch reused state. Epochs must not be reused while callbacks could remain; tokens are strictly increasing, nonzero within an epoch.
2. Inspect a prefix, acquire the exact complete packet length, then call `jg_submit(epoch, token, monotonic_ms, bytes, size, batch)`. `JG_OK` stores an immutable decoded copy and allows one matching ACK completion. Mutation of the caller's original buffer cannot alter that pending frame. `JG_BUSY` must not replace the pending operation.
3. Arrange a **finite monotonic ACK deadline** before waiting for hardware completion. Call `jg_finish` once with the same epoch/token after the actual ACK operation has a definite result, or call `jg_expire` if the deadline elapses. The core has no scheduler. `jg_expire` clears local state; it cannot cancel an already submitted bus transaction or prevent a late hardware ACK from consuming a later controller frame. A transport owner must cancel/drain or safely fence such transactions before continuing.
4. Consume all returned batch snapshots in order. When an old slot must be reused, report 0 releases the old identity and report 1 assigns the replacement. These are logical state snapshots, not proof of delivery to HID. A future HID adapter must track delivery failure and partial two-report delivery and must be able to send a final release of every actually published contact.
5. On a serialized stream-loss or owner error, call `jg_fault`. For asynchronous timeouts and completions, use the epoch/token-checked APIs; never call `jg_fault` from an unchecked late callback. Stop with `jg_stop`, deliver any release batch, and drain outstanding work.

At initialization and after a fault, a decoded zero-contact frame must be ACKed before contacts can be published. A packet whose records all have zero width also decodes to zero active contacts. A held contact is refused with `JG_DISARMED`. Malformed packets, ACK failure, timeout, clock reversal, and stop release active slots once and disarm. Stale epochs/tokens cannot disturb active/pending state. Repeated faults after release emit no repeated release. Clock reversal resets the packet baseline to the observed time and requires clean-zero recovery.

Integration still needs an explicit policy for **device ACK on malformed, unsupported, or `JG_DISARMED` frames**. This core neither issues nor approves that ACK. Without a verified policy, the controller could repeat one pending frame indefinitely. The owner must also handle a missing packet/IRQ and a silent stream separately; no pending ACK is not proof of stream health.

## Validation

`test_goodix.c` uses synthetic byte fixtures derived from the stock instruction evidence. It includes valid counts 0..10, truncation, corruption, pen and ID failures, stable slots, all-slot replacement, release-once, ACK failure/timeout, stale epochs/tokens, resynchronization, clock reversal, immutable pending data, and deterministic arbitrary-byte decoding. No fixture is described as a captured live frame.

Example host check, run from the workspace root:

```sh
clang -std=c11 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  work/J716fGoodix-20261113/J716fGoodix.c \
  analysis/stock-goodix-20261113/test_goodix.c -o goodix-host-test
./goodix-host-test
```

Parent-run validation in `../../analysis/stock-driver-followup-20261113/goodix-host-02/` passed **6293 checks with AddressSanitizer and UndefinedBehaviorSanitizer**. Freestanding `arm64-apple-ios17.0` compilation passed; the ordinary ARM64/subtype-0 Mach-O object is 3568 bytes, exports all nine APIs, and has no undefined symbols. Object SHA256: `d5deba8c06be366aa2a8d1eebaf9347a66fb6c02a8515701175765e0cacb209d`. The manifest binds the result/logs/object to the exact three frozen source hashes; `verify_manifest.py --check` verifies that binding. The earlier `goodix-host-01` complete audit failed because its runner had an incorrect `llvm-nm` path, despite successful tests/compilation; that attempt is preserved.

Passing these checks does not qualify transport, orientation, firmware upload, stylus support, power behavior, or HID publication.

The C/header/test are newly authored and marked `BSD-2-Clause`. Related Goodix Linux source remains separately preserved under its original GPL notices; no Linux driver is linked into this core. Stock config/firmware blobs are local evidence only, carry their original rights, and are neither executed nor uploaded.
