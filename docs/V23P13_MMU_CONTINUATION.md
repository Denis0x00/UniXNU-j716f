# V23P13: restore original continuation, exclude the complete BCM route

## Direction

User approved advancing toward upstream native architectural timer integration
after P12 r2A retained all six bound witnesses. P12 r2B is optional historical
root-cause work, not a prerequisite for P13. Do not require another BCM-write
trial before advancing the MMU investigation.

Upstream review: UniXNU `ae687009348296e97e49ce6e925795044f363ce6`,
sheng `08cde20edc3254ed6f7df8d0f5384fb887786e8f`, ArmCpu
`9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b`. The upstream
`architectural-timer-entry.patch` excludes the entire BCM2837 timer interrupt
routing block when `XNU_NATIVE_ARCH_TIMER=1`. sheng selects that configuration
and ArmCpu/ArmDTPlatform. Its SM8550 resources and IPI workaround are not J716F
hardware inputs.

## Two separate outputs

1. `configs/j716f_arch_timer_source_defconfig` and
   `build-j716f-source.py --native-arch-timer` prepare a separate source
   qualification. It enables the upstream architectural timer feature while
   retaining disabled ArmCpu/ArmDTPlatform and other unqualified drivers. It
   resolves with the existing pinned `42bffe8` driver. The optional workflow
   dispatch input selects this profile and checks the linked BCM route sequence
   is absent. **No full source build was executed for this profile in P13.**
2. P13P/M are **terminal binary bridge diagnostics on the existing frozen,
   source-built/prelinked baseline**, SHA256
   `cab8f385a44d21fc6f455cb501227452212ba44d34a9fa03d04fcf7e85324214`.
   They do not claim a rebuilt native timer kernel or a working timer driver.
   The entire nine-instruction BCM route at `0xfffffff0073fe310..330`
   is replaced by a diagnostic branch plus unreachable NOPs, returning to the
   unchanged original continuation at `0xfffffff0073fe334`.

The old source qualification profile and dependency pin remain available.
Do not relabel the bridge as complete source timer integration.

## Diagnostic path

Both variants keep the validated console at `0xb00be000+0x40000`, 12-byte
DBGC header, CRC record framing, single CPU, source crypto self-test code,
original exception vectors and original ADT. Modified kernel text is resealed
with the existing corecrypto HMAC tool; there is no crypto bypass.

Fresh Query/ARM uses P13-only cookie `0x5031334b45524e4c`; ENTRY binds the
ARM CRC in high32 and SCTLR in low32. Six invalid, pre-staged records use tag
`0x5031335300000000 | ARM_CRC`. Before continuation and again before mapping,
the kernel validates ENTRY, all six exact templates, offsets, ID and CRCs.

| Phase | Independent one-byte commit follows |
| --- | --- |
| 16 | Reaching the whole-BCM-block exclusion path |
| 17 | Reaching the pre-MMU hook before console mapping |
| 18 | Console mapping checks, descriptor writes and barriers returning success |
| 19 | Reading SCTLR with M=0 before the displaced original MSR |
| 20 | Falling through the original MSR SCTLR / ISB |
| 21 | Reading SCTLR and checking M=1 |

P13P publishes only phases16..19, appends phase11 with actual SCTLR M=0,
then requests SYSTEM_RESET. P13M publishes phases16..21 and appends phase12
with the actual complete SCTLR value, then requests SYSTEM_RESET. Mapping
failure publishes phases16/17 and appends phase13 with the failure code.
A returned SMC appends phase14 when its chain remains valid. Surviving stage21
is a lower bound consistent with the readback branch; the separately framed
phase12 value gives the directly recorded SCTLR. Missing records never identify
the exact stopping PC or reset source.

The publication header remains after ENTRY while independent slots commit.
The reader therefore searches logical and raw records separately and binds
them to the photographed ARM. Full terminal rows also require the P13 ARM,
bound preceding slot and exact record offset; older generic phase11/12 records
are not accepted as P13 results.

## MMU and ABI checks

- Original x0..x30, SP and NZCV are saved/restored around the continuation hook
  and MMU hook. The entry trampoline retains the original entry ABI.
- Physical stack conversion uses the original x22/x23 boot argument bases.
- Pre/post context occupies 32 bytes in the same 288-byte stack frame across
  the original MSR/ISB; no kernel text or new reserved region stores context.
- Console-only NC, PXN/UXN mappings reuse the P9 guarded two-page setup. It
  rejects unexpected TCR/MAIR/TTBR, occupied root, nonzero spare or existing
  references before any table write. It adds exactly 64 console pages.
- The actual prefix is emulated using DarwinBoot's real PA/VA block-offset
  congruence (synthetic example PA `0xd0004000`, VA `0xfffffff007004000`).
  Original generated TTBR1 descriptors are walked with T1SZ=26 to verify the
  high stack aliases the exact physical handoff frame. Early legacy fixtures
  with an aligned PA are not evidence for that post-MMU mapping.
- System registers, MMU switch, stack alias/cache behavior and SMC are mocked
  in host tests. Neither those tests nor the page walk proves hardware MMU-on.

## Firmware and manual test

P and M are separate images using the existing 64 MiB payload / 7 MiB FD
layout. Each has an untimed reader-first menu, one diagnostic kernel, Reboot,
Power off and Exit. The same new reader handles both. It retains r2's explicit
two-press valid-full-log session rollover and adds terminal SCTLR/failure rows.
It does not initialize an invalid header automatically. Query needs 896 bytes
of eligible linear tail; no forced wrapping or bulk erase is performed.

**Mainline test is P13M. P13P is an optional pre-SCTLR fallback, not a gate.**

1. Prepare fastboot and recording. Flash only `boot_b`, then verify B active
   and bootable and boot the untimed menu. No kernel auto-start.
2. Open P13 reader FIRST; photograph frozen ENTRY/NOW before any write.
3. If NOW is VALID with sufficient linear room, Query once, ARM once, and
   photograph ID/CRC and successful six-template preparation. For VALID
   saturated/exhausted logs, photograph old state before explicitly confirming
   New linear session twice. INVALID headers stop this trial.
4. Return to menu and launch the selected P13 kernel once. Do not reuse a
   consumed ARM or launch again after reboot without a fresh Query/ARM.
5. After automatic reboot, open reader FIRST and photograph the complete page,
   including all six slot rows and PRE_MMU/POST_MMU/MAP_FAILURE/RESET_RETURN.
   Do not Query/ARM again before preserving that snapshot. A stall requires
   saving the last display/video before recovery.

Host manifests give exact images and checks. P13M was flashed only to boot_b, B verified active/bootable, and reboot
command succeeded. The subsequent hardware observation below supersedes the pending
reader/MMU status; real timer interrupts and sustained normal kernel
continuation remain unverified. The later ArmCpu/GIC/timer integration uses
J716F's measured SM8250 resources and a new source/prelink qualification.

## P13M hardware checkpoint observed

A full reader photo after the user selected P13M retains ID1 ARM CRC
`CCF99510`, ENTRY CRC `3CFCD765` with SCTLR `0x00c00808` (M=0),
and all six phases16..21 as logical1/raw1 with the same ARM binding.
The terminal POST_MMU record CRC `D7BBC9E4` contains actual SCTLR
`0x3454593d` (M=1). All nine ARM/ENTRY/witness/terminal CRCs independently
reconstruct. ENTRY/NOW are VALID next=size `0x2ffcd`, logical13/raw13,
consoleCRC `401D38FB`; growth512 matches eight records after ARM.
The new reader is ID2/query0/armed0. No MAP_FAILURE or RESET_RETURN record
is present; absence does not identify the reset source. No continuous reset
video has been reviewed for this attempt.

The user reports selecting ARM once extra before P13M. The same-invocation
`mArmed` guard returns before writing; one matching ARM remains and the
binding/counts are intact. The trial remains usable.

This establishes short hardware execution across the original SCTLR MSR/ISB,
M=1 readback and post-MMU logging, with records present at reader reentry.
It does not establish sustained normal kernel initialization, real timer/GIC,
userspace or reset-source attribution. Next: preserve this terminal baseline
and add independent checkpoints while restoring original post-MMU continuation.
P13P and original BCM B controls are optional, not prerequisites.

### Consumed-ARM relaunch clarification

User subsequently expressed uncertainty about the earlier launch order and confirmed a new direct P13M selection without Query/ARM hung. Photo ends at DarwinBoot exiting boot services. Actual-kernel host replay on reconstructed matching thirteen records ends at the admission HALT with no console mutation; the live PC is not observed. Prior bound M1 records remain evidence, while reset chronology/source remain unqualified. Preserve photo, manually restart and read reader FIRST; manual recovery is a separate event.
