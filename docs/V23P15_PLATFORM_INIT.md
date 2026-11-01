# V23P15M: PE_init_platform entry and return

## Current hardware result

P15M continuous13.932s video confirms selected P15M kernel, DARWIN BOOT/green handoff, screen loss, Lenovo/Android splash, P15M menu return and read-only reader. Final video reader matches photo: ENTRY/NOW VALID next=size0x328a4, consoleCRCF498612C, logical13/raw13, latest ID1/phase12/F286B6BF. Fresh ARM B9A219EB matches prelaunch; ENTRY F2FD8F05 valueB9A219EB00C00808(M0). All six phases16..21 are L1/R1 with bound value50313553B9A219EB and CRCs99CE21F7,D58097B9,CEA0E583,E51787B9,F8E001F7,B6E7E7E2. Terminal F286B6BF records actual SCTLR3454593D(M1). All nine CRCs reconstruct exactly; growth512 matches ENTRY+six slots+terminal. New reader ID2/query0/armed0, no MAP_FAILURE/RESET_RETURN. Original PE_init_platform(0, bootArgs) entered and returned before next arm_init call, with records retained over one observed reboot. PMIC02000001->02000041. Reset source, sustained continuation, interrupt/GIC/native timer and userspace remain unverified.

Next: preserve this read-only baseline; audit the subsequent configure_timer_apple_regs, WFE and miscellaneous setup for new continuation checkpoints. Earlier pending launch notes below are chronological and superseded.

## Current device operation

P15M flashed only to boot_b after final image SHA256 and fastbootd product/unlock/slot/partition gates. B active and bootable verified; reboot command succeeded. Recording readiness confirmed. Menu appearance, initial P15 reader, fresh Query/ARM and kernel observations pending.

Open P15 reader FIRST and photograph the full initial page before Query, ARM, new-session action or kernel launch; review validity and room before fresh Query/ARM.

## Initial reader observation

P15 initial reader photo confirms PLATFORM TRACE r1 at EL1: ENTRY/NOW VALID next=size0x32564, logical0/raw0, frozen checkpointID0 and all eight logical/raw rows plus four terminal rows zero. THIS RUN ID1/query0/armed0. Unused linear tail55952 bytes exceeds Query896-byte budget; no new-session action needed. Console CRC text is not confidently transcribed; original photo preserved privately. No P15 kernel launch or retention result. Prior P14 retained records remain independent of this intervening fastboot/firmware boot baseline.

## Fresh ARM observation

P15 Query/ARM photo confirms NOW VALID next=size0x326a4, logical5/raw5, THIS RUN ID1/query1/armed1/CRCB9A219EB, EL1, PSCI1.1 and SYSTEM_RESET FEATURES0. ARM + six invalid templates OK is displayed. Header growth320 bytes matches four Query records plus ARM. Phase9 CRC reconstructs exactly with P15 cookie5031354B45524E4C and prior0x32664. Frozen initial header0x32564 and zero rows remain unchanged. No kernel launch or new P15 checkpoint yet.

Next: return to menu and launch P15M once; after restart reader FIRST and photograph before writes. Earlier pending reader instructions are superseded.

## Baseline and boundaries

P14M hardware photos retain a fresh bound ARM/ENTRY, all six original early
initialization witnesses, and actual SCTLR `0x3454593d` (M=1) after
`cpu_data_init` returned. All nine CRCs reconstruct. The user reports an
automatic reboot and then opened the reader before further writes. No
continuous reset video was reviewed and the reset source is not attributed.
See `research/v23p14-early-init.json`; preserve that independent baseline.

P15 continues the same frozen source-built/prelinked kernel, SHA256
`cab8f385a44d21fc6f455cb501227452212ba44d34a9fa03d04fcf7e85324214`.
The complete obsolete BCM block remains excluded. Original post-MMU assembly,
`arm_init`, relocation/fixups, memcpy and `cpu_data_init` execute. The next
call is the first `PE_init_platform(0, bootArgs)` invocation. P15 stops when
that call returns, before the following original `arm_init` call.

This remains a terminal diagnostic on the frozen baseline. ArmCpu, GIC/native
timer integration and the separately prepared source profile are independent
work. Reaching this first platform call does not qualify interrupts, storage,
root mounting, sustained kernel execution or userspace.

The upstream/Pad 6S Pro reference review remains the one recorded for P14:
UniXNU `ae687009348296e97e49ce6e925795044f363ce6`, sheng
`08cde20edc3254ed6f7df8d0f5384fb887786e8f`, SM8550 common
`23b2049fcd9e30356161def84231334fb55ecca9`, ArmCpu
`9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b`. No new upstream-head review is
claimed in P15. Build-driver pin stays `42bffe8a56823dad2e448c776b4f94862f3e290a`.
No SM8550 address, interrupt workaround or memory policy is a J716F input.

## Independent records

New ARM cookie: `0x5031354b45524e4c`.
Slot binding: `0x5031355300000000 | ARM_CRC`.
A P14 ARM cannot admit P15. Each launch needs a fresh Query/ARM.

| Phase | Reader label | Boundary |
| --- | --- | --- |
| 16 | BCM_ROUTE_SKIPPED | Entire obsolete BCM block excluded |
| 17 | M1_READBACK | Actual M=1 readback after original SCTLR MSR/ISB |
| 18 | CPU_DATA_INIT_DONE | cpu_data_init returned; before original w0=0 |
| 19 | PE_CALL_PREP | w0=0 executed; before original x1=bootArgs instruction |
| 20 | PE_INIT_ENTRY | Actual PE_init_platform entry; before its stack prologue |
| 21 | PE_INIT_RETURNED | Original BL returned to its original return address |

The first two retain the qualified MMU path. The later four replace P14's
later witness locations; phase numbers alone must not be read as P14 labels.
Every slot commits independently with its own byte, CRC and ARM binding.
Partial progress can remain raw-only until terminal header publication.

| Hook address | Displaced instruction | Resume |
| --- | --- | --- |
| `0xfffffff0079e19f0` | `mov w0, #0` | Original `0xfffffff0079e19f4` |
| `0xfffffff0079e19f4` | `mov x1, x20` | Original BL at `0xfffffff0079e19f8` |
| `0xfffffff007953998` | `sub sp, sp, #64` | Original `0xfffffff00795399c` |
| `0xfffffff0079e19fc` | Following original BL | Terminal; never executes following call |

The platform BL at `0xfffffff0079e19f8` is byte-identical to the baseline.
Its original LR `0xfffffff0079e19fc` is preserved. Continuation wrappers save
all31 GPRs, SP and NZCV, replay displaced instructions exactly and restore
their input state. No handoff context below SP survives original C code.
Late hooks revalidate the ARM-bound ENTRY and exact committed/uncommitted
template sequence before committing another witness.

Phase12 (`INIT_STOP_SCTLR`) records actual SCTLR after phase21, then requests
SYSTEM_RESET. Phase13 is guarded console-mapping failure after phase16;
phase14 records a returned reset. No such record by itself establishes reset
source. The same reserved console, ADT and nonpayload firmware are retained.

## Qualification

`research/v23p15-platform-init.json` records actual build completion and host
case counts; a document describing the design does not establish completion.
Host execution uses actual emitted kernel code and original platform code,
including SecureDTInit and pe_identify_machine. Synthetic boot arguments
include the pinned ADT. Architectural registers, MMU aliases, barriers and
SMC are mocked. These checks cannot prove real translation or retention.

Kernel checks cover original BL/LR, all31 GPR/SP/NZCV at continuation hooks,
partial entry without false return, template corruption after platform code,
old-cookie/consumed-ARM admission and exact console mappings. Linked EFI
checks cover actual Query/ARM, the new cookie and labels, partial records,
terminal binding, explicit full-ring session rollover and failure gates.
Final packaging audits source hashes, both firmware passes, FD/FAT contents,
kernel HMAC, unchanged nonpayload modules and compact SEC, and boot image.

## Hardware sequence

1. After fastboot and recording readiness, flash only `boot_b`; verify B
   active and bootable, then reboot to the untimed menu.
2. Open **P15 reader FIRST**. Photograph the full initial page before Query,
   ARM, session action or kernel launch. Old P14 raw records are historical.
3. Review valid header and unused linear tail. If enough room, Query once,
   ARM once, then photograph ID/CRC and six-template success. No automatic
   rollover or invalid-header repair. Use the explicit two-confirmation
   new-session action only after reviewing a valid exhausted console.
4. Return to menu and launch P15M once while recording.
5. After restart, reader FIRST and a full photograph before further writes.
   If stalled, preserve the last display and elapsed time before manual
   recovery. Every later launch requires fresh Query/ARM.

The firmware menu keeps Reboot, Power off and Exit. A consumed-ARM relaunch
halts at admission and can leave the previous DarwinBoot display visible.
