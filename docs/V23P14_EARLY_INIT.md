# V23P14M: original post-MMU assembly and early C initialization

## Current hardware result

P14M postlaunch reader photo confirms ENTRY/NOW VALID next=size0x2e5a6, consoleCRC5D1C75BD, logical13/raw26, latest ID2/phase12/76EE0045. Fresh ARM6304806B matches prelaunch; ENTRY FCBEA27A value6304806B00C00808(M0). All six phases16..21 are L1/R1 with bound value503134536304806B and CRCs66FE13FA,A1D55ACD,BAF528F7,91424ACD,8CB5CC83,C0FB7ACD. Terminal76EE0045 records actual SCTLR3454593D(M1). All nine CRCs reconstruct exactly; header growth512 matches ENTRY+six slots+terminal. New reader ID3/query0/armed0, no MAP_FAILURE or RESET_RETURN. One original early-init path reaches cpu_data_init return before PE_init_platform, with records present after user-reported automatic reboot. No continuous reset video reviewed; reset source, sustained initialization, timer/GIC and userspace remain unverified.

Next: preserve this baseline and instrument PE_init_platform entry/return and early dependencies in a new diagnostic. Earlier operation notes below are chronological; pending launch instructions are superseded.

## Current device operation

P14M flashed only to boot_b after SHA256 and fastbootd product/unlock/slot/partition gates. B active and bootable verified; reboot command succeeded. Recording readiness confirmed. Menu identity, initial reader, fresh Query/ARM and kernel observations pending.

Open P14 reader FIRST and photograph the complete initial page before Query, ARM, new-session action or kernel launch; review validity and room before fresh Query/ARM and one M launch.

## Initial reader observation

P14 initial reader photo confirms INIT TRACE r1 at EL1: ENTRY/NOW VALID next=size0x2e266, consoleCRC497681EB, logical0/raw13. Historical P13 ARM CCF99510/cookie5031334B45524E4C and ENTRY3CFCD765 remain raw-only; no P14 slot or terminal hits. THIS RUN ID2/query0/armed0. Unused linear tail73102 bytes exceeds Query896-byte budget, so no new-session action is needed. No P14 kernel launch, progress or controlled retention result.

## Fresh ARM observation

P14 Query/ARM photo confirms NOW VALID next=size0x2e3a6, logical5/raw18, THIS RUN ID2/query1/armed1/CRC6304806B, EL1, PSCI1.1 and SYSTEM_RESET FEATURES0. ARM + six invalid templates OK is displayed. Header growth320 bytes matches four Query records plus ARM. Phase9 CRC reconstructs exactly with P14 cookie5031344B45524E4C and prior0x2e366. Frozen initial ENTRY remains0x2e266/CRC497681EB with historical P13 raw rows. No kernel launch or new P14 checkpoint yet.

Next: return to menu and launch P14M once. After restart open reader FIRST and photograph before writes.

## Baseline and scope

P13M retains a CRC-bound M=0 ENTRY, all six witnesses and actual terminal
SCTLR `0x3454593d` (M=1). Its launch/reset chronology is user-uncertain.
A later confirmed launch without fresh Query/ARM stopped with the previous
DarwinBoot display; a subsequent reader photo has the same header, console
CRC and thirteen records. Actual-kernel host replay reproduces admission HALT
without console mutation. The photo does not expose the live stopping PC.
Keep those observations distinct from a controlled SYSTEM_RESET cycle.

P14 is an isolated terminal binary diagnostic on the same frozen source-built,
prelinked baseline, SHA256
`cab8f385a44d21fc6f455cb501227452212ba44d34a9fa03d04fcf7e85324214`.
It preserves source crypto self-tests and reseals the text HMAC. It is not a
new source-native-timer kernel. ArmCpu, GIC, native timer interrupts, platform
expert matching, storage and userspace are not qualified by this image.

## Upstream reference review

Online heads rechecked on 2026-10-26 are unchanged from the P13 review:

- [UniXNU ae68700](https://github.com/UniXNU/UniXNU/tree/ae687009348296e97e49ce6e925795044f363ce6)
- [sheng 08cde20](https://github.com/UniXNU/UniXNU-sheng/tree/08cde20edc3254ed6f7df8d0f5384fb887786e8f)
- [SM8550 common 23b2049](https://github.com/UniXNU/UniXNU-sm8550-common/tree/23b2049fcd9e30356161def84231334fb55ecca9)
- [ArmCpu 9066b53](https://github.com/UniXNU/ArmCpu/blob/9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b/ArmCpu.cpp)

The [bring-up guide](https://github.com/UniXNU/UniXNU/blob/ae687009348296e97e49ce6e925795044f363ce6/docs/zh/porting/bring-up.md)
separates loader handoff from kernel entry and platform expert initialization.
sheng selects ArmCpu/ArmDTPlatform and the shared native timer entry guard.
Its CPU/GIC/timer resources, SMP SPI workaround, memory sizes and storage
policy remain SM8550/board inputs, not J716F inputs. Its porting record
separates implemented support from per-image hardware evidence; do not infer
complete validation for this port from another board's implementation.

J716F remains single CPU. The complete obsolete BCM route stays excluded.
The source driver remains pinned to `42bffe8a56823dad2e448c776b4f94862f3e290a`;
reviewing newer documentation does not change the build pin. Native timer
source qualification prepared in P13 is still separate and not fully built.

## Six independent witnesses

P14 uses a new ARM cookie `0x5031344b45524e4c` and slot binding
`0x5031345300000000 | ARM_CRC`. A P13 ARM cannot admit a P14 kernel.
The six reserved, initially invalid CRC templates retain the previous
one-byte independent commit scheme; no witness overwrites an earlier one.

| Phase | Reader name | Boundary |
| --- | --- | --- |
| 16 | BCM_ROUTE_SKIPPED | Entire nine-instruction BCM route excluded |
| 17 | M1_READBACK | Actual SCTLR M=1 after original MSR/ISB |
| 18 | CPU_SYSREGS_DONE | Original CPACR/TPIDR writes complete, boot-argument VA conversion next |
| 19 | ARM_INIT_ENTRY | Original return reaches `_arm_init`, before its stack prologue |
| 20 | EARLY_FIXUPS_DONE | `_arm_init` reaches its slide/fixup join |
| 21 | CPU_DATA_INIT_DONE | Original `cpu_data_init` call returned, before `PE_init_platform` argument setup |

The four additional hook sites are tied to this exact baseline's symbols and
instruction bytes: `0xfffffff0073fe3a4`, `0xfffffff0079e1448`,
`0xfffffff0079e19b4`, `0xfffffff0079e19f0`. The first three hooks replay
the displaced ADD, SUB and ADRP exactly and resume the following instruction.
The fourth is terminal: phase12 records actual SCTLR and requests SYSTEM_RESET.
It does not call `PE_init_platform` or normal kernel continuation beyond it.

Register state is preserved with a shared integer-only save/restore wrapper:
all31 GPRs, SP and NZCV, with original x30 saved before the wrapper BL.
No handoff context remains below SP across original C code. Every later hook
revalidates the original ARM-bound ENTRY and all six templates, including the
exact expected committed/uncommitted sequence, before committing its witness.

The header covers ENTRY until terminal publication, so interrupted progress
can be raw-only. Terminal records require exact P14 ARM/ENTRY/slot binding.
Phase13 is a guarded console-mapping failure after phase16; phase14 is a
returned reset. Missing records do not identify the precise stopping PC or
reset source. The console mapping, reservation, ADT and nonpayload firmware
remain as previously qualified; only final image audits establish that equality.

## Verification and manual test

The manifest and `research/v23p14-early-init.json` record completed checks.
Host execution runs actual original post-MMU assembly, `_arm_init`, memcpy
and `cpu_data_init`; system registers, translation/cache aliases, UEFI and
SMC are mocked. Host success is not a P14 hardware result.

The untimed firmware menu defaults to the P14 reader and includes P14M,
Reboot, Power off and Exit. It explicitly states that each launch requires
fresh Query/ARM. Reusing a consumed ARM deliberately halts at kernel admission;
the old DarwinBoot display may remain after ExitBootServices.

1. Flash the audited image only to `boot_b` after fresh fastboot/recording
   readiness. Verify B active/bootable and reboot to the untimed menu.
2. Open P14 reader FIRST. Photograph the complete initial page before writes.
   Old P13 records can remain in the console; they are not P14 witnesses.
3. For a VALID header with sufficient unused linear tail, Query once and ARM
   once. Photograph ID/CRC and six-template success. No automatic rollover or
   invalid-header repair. Only use the explicit two-confirmation new-session
   action when the photographed valid console lacks the required room.
4. Return to menu and launch P14M once. Keep continuous recording.
5. After a restart, open reader FIRST and photograph the complete page before
   Query/ARM or another launch. A new reader starts unarmed. If it stalls,
   preserve the last display/video before manual recovery; that recovery is
   a separate event.

Source preparation, host qualification, device commands and device observations
are recorded separately. No P14 partition write is implied by a build result.
