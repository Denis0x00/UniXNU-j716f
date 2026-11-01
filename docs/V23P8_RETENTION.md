# V23P8: controlled console validity and reboot retention

## Current result

V23P8 RETENTION r1 is built and host verified. It was flashed only to boot_b;
B activation, current-slot b and slot-unbootable:b=no were verified, and the
reboot command succeeded. The first photo confirms the initial valid header;
BASELINE append/readback and PSCI query are observed. One controlled SYSTEM_RESET
cycle now preserves a valid header and the matching PRE_RESET record at P8 reentry.
It is an independent
UEFI application; it contains no XNU. J716F remains in bring-up stage 5: early
kernel entry, without sustained MMU-on or platform-expert proof.

The candidate is 1,699,840 bytes, SHA256
`b8d776e19de9d51feb88d20843a5bafc812c360c78ae0f52f2adbe0a9b11fba6`.
Its non-payload inner-FV sections and compact SEC executable are byte-identical
to the P7 baseline. Existing P6/P7 firmware images and sources remain preserved.

## First device observation

The first P8 photo confirms RETENTION r1, ENTRY/NOW DBGC 0x43474244, next=size=0x3fb3f, ENTRY whole-console CRC32=0x9e93c89c, NOW VALID, zero logical/raw P8 records, run ID=1, baseline=0 and query=0 at EL1. This establishes the initial readable header, not append or reboot retention. Skip initialization; append BASELINE once, run Query PSCI + record (NO RESET), and photograph the full page before reset.

Photo SHA256 (raw photo kept private):
`1dba5641eb114b42da682c8616acaccc48cfdecd2b5c2fd8682886f131383a59`.

## BASELINE and Query device observation

P8 BASELINE photo confirms readback OK: NOW VALID, next=size=0x3fb7f (+64 bytes), logical/raw count=1, ID=1, phase=2, CRC=0x668a6fef, baseline=1/query=0. Query photo confirms NOW VALID, next=size=0x3fc7f (+256 bytes), logical/raw count=5, ID=1, phase=8, CRC=0xb4d25630, baseline=1/query=1, EL1, PSCI_VERSION=0x10001 (1.1), FEATURES(SYSTEM_RESET)=0 and last-x0=0. Frozen ENTRY remains unchanged. Same-invocation append/readback and query are observed; PRE_RESET and cross-reset retention remain untested.

PRE_RESET has now been photographed: ID=1, phase=3, CRC=`E13F0C63`;
logical/raw count=6, NOW VALID and next=size=`0x3fcbf`. The following video completes the controlled reset cycle described below.

## Controlled reset result

P8 controlled SYSTEM_RESET video shows the armed PRE_RESET page, screen loss, Lenovo/Android splash, return to the P8 menu and manual app reentry. At 10.898s (frame 327), frozen ENTRY and NOW retain DBGC 0x43474244, next=size=0x3fcbf, six logical and six raw CRC-valid records, latest ID=1/phase=3/CRC=0xe13f0c63, exactly matching the pre-reset photo. NOW remains VALID. New invocation ID=2 with baseline=0/query=0 and read-only status confirms capture before new app writes/query. One observed immediate UEFI-to-UEFI reset cycle preserves the header and matching PRE_RESET record. This does not establish all payload bytes, power-off/ZUI/Recovery retention, repeated reliability, or XNU/MMU checkpoint progress.

| Field | Before reset | Read-only reentry |
| --- | --- | --- |
| Header | VALID | VALID |
| next / size | `0x3fcbf` / `0x3fcbf` | `0x3fcbf` / `0x3fcbf` |
| Logical / raw records | 6 / 6 | 6 / 6 |
| PRE_RESET ID / phase | 1 / 3 | 1 / 3 |
| PRE_RESET CRC32 | `E13F0C63` | `E13F0C63` |
| Invocation ID | 1 | 2 |
| baseline / query | 1 / 1 | 0 / 0 |

Video SHA256 (raw media private):
`d96d4e0186c69107690057b0dd7651e04f28fb48e5bdbd4af8a0fe90487440ac`.

## Why a new baseline is necessary

P6 established valid-header reads and same-run append/readback. P7 established
PSCI 1.1 query returns and two RESET2 returns of -2 (INVALID_PARAMS). The user
reported two SYSTEM_RESET reboots after Query. The blocked action on reentry
was the fresh-invocation Query gate, not a failing SYSTEM_RESET call.

A later P7 photo displayed INVALID, but its timing and intervening actions are
unknown. It does not identify when the header became invalid. The subsequent
read-only Recovery capture had the same DTB, kernel config and ramoops parameters
as the earlier capture. Pstore was mounted and empty. The 65-byte file saved as
`console-ramoops-0` was a missing-file error, not a log. A readback of the first
1,699,840 boot_b bytes matched P7; partition tail was not checked. Recovery itself
changes RAM state, and the later launch was interrupted by the user's pause.
None of those observations establishes a controlled retention result.

## Memory and format

Only the audited console zone is writable: `0xb00be000 + 0x40000`, including
the 12-byte DBGC header; payload capacity is `0x3fff4`. The existing P7 mapping
reserves the full 1 MiB and maps it uncached. Dump, ftrace and pmsg zones are
not written by P8.

Each record is 64 bytes, little endian:

| Offset | Field |
| --- | --- |
| 0 | 8 bytes `J716FP8!` |
| 8 / 12 | version 1 / record bytes 64 |
| 16 / 20 | nonzero record ID / phase |
| 24 | 64-bit value |
| 32 / 36 / 40 | signature / next / size before this record |
| 44 | reserved zero |
| 48 / 52 | IEEE CRC32 over bytes 0..47 / its complement |
| 56 | 8 bytes `P8DONE!` plus newline (8 bytes total) |

Phases: INIT=1, BASELINE=2, PRE_RESET=3, RESET_RETURN=4, PRE_VERSION=5,
VERSION_RETURN=6, PRE_FEATURE=7, FEATURE_RETURN=8.

The frozen ENTRY snapshot contains the raw header, whole-console CRC and
CRC-checked records. Logical scanning respects a valid header; separate raw
scanning can find stale payload records even if the header is invalid. Raw
records do not establish a valid ring. Raw summary selects the highest ID and
prefers PRE_RESET within that ID, since physical offset cannot establish time
after header loss. Logical summary follows ring chronology. The session ID advances beyond existing
CRC-valid logical/raw records and refuses wrap to zero. Entry and refresh do
not write console memory or invoke SMC.

## Manual hardware sequence: boot_b only

1. Enter the P8 app and photograph the entire initial page, including ENTRY,
   NOW and the bottom result line. Do not initialize before saving this baseline.
2. If NOW is INVALID, select Initialize and press Power once to arm it. Save the
   page, then press again to initialize. It rechecks the full-console CRC and
   header before replacing the invalid 12-byte header, publishing DBGC last.
   An INIT record stores the prior header and CRC. Payload is not bulk-cleared;
   appending records naturally replaces their span. A valid header is never
   reinitialized. Navigation cancels confirmation.
3. Append BASELINE once and save its ID/CRC. Query PSCI; version >=0.2 and
   FEATURES(SYSTEM_RESET)=0 are required in this invocation.
4. Select Direct SYSTEM_RESET and press once. This only commits PRE_RESET.
   Photograph its ID, phase=3 and CRC while the screen is stable.
5. Press again without navigating. The app rechecks the PRE_RESET record and
   makes one SMC SYSTEM_RESET with zero unused inputs. If it returns, capture
   last-x0 and the RESET_RETURN record; do not claim successful reset.
6. After an observed reboot, reopen P8 and photograph the full initial page
   BEFORE append, initialization or Query. Do not detour through ZUI, Recovery
   or power-off during this immediate UEFI-to-UEFI trial. Those are separate
   later paths requiring their own controlled baseline.

| Post-reboot observation | Supported conclusion |
| --- | --- |
| Valid logical PRE_RESET with the exact photographed ID/CRC | That record survived the observed reboot into P8 |
| Invalid header but matching raw PRE_RESET | Its payload bytes survived; header/ring validity failed |
| Neither logical nor raw record | Loss occurred somewhere on this path; the stage is not located |
| SYSTEM_RESET returned | A reset-retention trial did not complete |

## Host checks

- Two complete firmware passes and actual FD/FV/FAT/PE/Android-v2 packaging.
- All non-payload inner-FV sections and compact SEC executable match P7.
- Original ring tests plus ASan/UBSan record tests: standard CRC vector, all
  512 single-bit corruptions, invalid-only initialization, payload preservation,
  physical wrap, raw/logical distinction and guard bytes.
- Three mocked SMC wrapper cases; the final EFI contains the tested wrapper.
- Nine linked-EFI UI cases execute entry, key handling, initialization, append,
  query and reset gates. UEFI services/input/display/CurrentEL/SMC are mocked;
  large-region CRC uses Python CRC32. Record CRC, writer and scanner execute
  the linked code. Mocked reset/reentry is not physical retention evidence.
- The collector now frames remote shell status and rejects failed/unframed
  captures even when exec-out exits zero. Five regression cases pass. The P8
  decoder requires an accepted, hash-matching collector manifest for pstore.

Private tests/artifacts remain outside the public repositories. The only new
partition write is boot_b. No counter or kernel MMU result is claimed. Physical retention evidence is
limited to the one controlled cycle above.

## Independent build fix

`patches/build-driver/0004-restore-xnu-headers-after-reclone.patch` backports
upstream `018c8f3439f1093b7ccd9965db0894516aace274` without changing the pinned
`42bffe8` build driver. The source builder and macOS workflow include it.
Host tests verify exact upstream file equivalence, patch/reverse applicability,
fresh-clone restoration, unchanged-input reuse, re-clone restoration and failure
without a success stamp. Full macOS XNU compilation has not been rerun.

## Following stage

With one controlled retention cycle verified, prepare direct RAM records to the existing XNU
entry and MMU transition, using fixed checkpoint IDs. First audit the physical
address, TTBR0 attributes and post-MMU virtual mapping plus register/NZCV
preservation. Confirm each checkpoint with a returned record before advancing;
do not infer MMU progress from a generic restart. If P8 loses its record, first
capture the same zone earlier in UEFI to isolate header reset versus payload loss.
