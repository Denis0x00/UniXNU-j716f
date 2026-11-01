# V23P16M: original timer/WFE/misc initialization

## Current hardware result

P16M continuous11.998s video confirms selected P16M, DARWIN BOOT/green handoff, screen loss, Lenovo/Android splash, P16M menu return and read-only reader matching final photo. ENTRY/NOW VALID next=size0x28aa1, consoleCRCB9A3B4BA, logical13/raw26, latest ID2/phase12/A684F65B. Fresh ARM93A96F6F matches prelaunch; ENTRY5AF6E0B5 value93A96F6F00C00808(M0). All six phases16..21 are L1/R1 with bound value5031365393A96F6F and CRCsAA287B4B,E666CD05,FD46BF3F,D6F1DD05,CB065B4B,48BB45ED. TerminalA684F65B records actual CNTKCTL_EL1=0x4f after M1 guard. All nine CRCs reconstruct exactly; growth512 matches ENTRY+six slots+terminal. New reader ID3/query0/armed0, no MAP_FAILURE/RESET_RETURN. Original WFE configuration/init and misc returns completed and records retained over one observed reboot. PMIC02000001->02000041. Timer IRQs, real WFE wait/wakeup, sustained initialization, ArmCpu/GIC, reset source and userspace remain unverified.

Next: Preserve this read-only result; next instrument original /chosen SecureDTLookupEntry, xnu_platform_stall property lookup and following control-flow/initialization boundary. Each new attempt requires fresh version-specific Query/ARM; do not directly relaunch consumed P16 ARM.

Earlier pending notes below are chronological and superseded by this result.

## Fresh ARM observation

P16 fresh Query/ARM photo confirms NOW VALID next=size0x288a1, logical5/raw18, THIS RUN ID2/query1/armed1/CRC93A96F6F, EL1, PSCI1.1 and SYSTEM_RESET FEATURES0. ARM + six invalid templates OK is displayed. Growth320 bytes matches four Query records plus ARM; phase9 CRC reconstructs exactly with P16 cookie5031364B45524E4C and prior0x28861. Frozen initial0x28761/ECF994E2 and old P15 rows remain unchanged. No new-session action or P16 launch observed. One fresh ID2 attempt is ready; predicted complete header0x28aa1 is derived, not observed.

Next: return to menu and launch P16M once. After restart reader FIRST, photograph before writes.

## Initial reader observation

P16 initial reader photo confirms WFE TRACE r1 at EL1: ENTRY/NOW VALID next=size0x28761, consoleCRCECF994E2, logical0/raw13. Frozen ID1 ARM B9A219EB cookie5031354B45524E4C and ENTRY F2FD8F05 valueB9A219EB00C00808 match old P15; raw latest phase12 F286B6BF is not a P16 CNTKCTL terminal. All six P16 slot rows and four terminal rows are zero. THIS RUN ID2/query0/armed0. Unused tail96403 bytes exceeds Query896-byte budget; no new session needed. No P16 launch or controlled reset retention result; parent menu not pictured.

Next: Query then ARM once and photograph the complete page before kernel launch.

## Current device operation

P16M flashed only to boot_b after final image/source hash and fastbootd product/unlock/slot/partition gates. B active and bootable verified; reboot command succeeded. Recording readiness confirmed. Menu appearance, initial reader, fresh Query/ARM and kernel observations remain pending. Phase12 is CNTKCTL, not SCTLR. Open P16 reader FIRST and photograph the full initial page before Query/ARM/session action or kernel launch.

## Status and scope

P15M hardware evidence established the first original
`PE_init_platform(0, bootArgs)` entry and return, with bound M1 records retained
over a video-observed reboot. P16 continues from that boundary. Consult
`research/v23p16-timer-wfe.json` for the current build and hardware status.

This remains a terminal binary bridge on the frozen source-built/prelinked
baseline (`cab8f385a44d21fc6f455cb501227452212ba44d34a9fa03d04fcf7e85324214`).
It is not a newly source-built native timer kernel. ArmCpu/GIC integration,
timer interrupt delivery, an actual WFE wait/wakeup, scheduler progress and
userspace remain separate work.

## Current upstream reference review

Reviewed on 2026-10-26; heads match the preceding review. The source-build pin
is unchanged. P16 does not silently update the kernel or copy SM8550 addresses.

| Repository | Reviewed revision | Applicable finding |
| --- | --- | --- |
| [UniXNU](https://github.com/UniXNU/UniXNU/tree/ae687009348296e97e49ce6e925795044f363ce6) | `ae687009348296e97e49ce6e925795044f363ce6` | Native architecture timer entry excludes the complete BCM routing block; keep that exclusion. Bring-up distinguishes initialization from working interrupts. |
| [sheng](https://github.com/UniXNU/UniXNU-sheng/tree/08cde20edc3254ed6f7df8d0f5384fb887786e8f) | `08cde20edc3254ed6f7df8d0f5384fb887786e8f` | Board selects ArmDTPlatform/ArmCpu and uses its own SoC descriptions. Repository still describes work in progress. |
| [sm8550-common](https://github.com/UniXNU/UniXNU-sm8550-common/tree/23b2049fcd9e30356161def84231334fb55ecca9) | `23b2049fcd9e30356161def84231334fb55ecca9` | Shared SoC layer owns its SM8550 configuration; GIC ranges and SPI IPI policy are not J716F facts. |
| [ArmCpu](https://github.com/UniXNU/ArmCpu/blob/9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b/ArmCpu.cpp) | `9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b` | Enables GIC/timebase and services timer PPI27 through `rtclock_intr`; counter reads and IRQ delivery are separate evidence. |

The upstream native timer checkpoint fixture explicitly does not establish
sheng hardware testing or functional iPadOS boot. Offline HVF execution is
not hardware evidence for either device.

## Audit of this exact baseline

`configure_timer_apple_regs`, `configure_misc_apple_boot_args`, and
`configure_misc_apple_regs` are already single-instruction `RET` stubs.
P16 labels the first result `TIMER_STUB_RETURN`, not timer initialization.

`wfe_timeout_configure` is real original code. It parses `wfe_events_sec`
(default 1,000,000), uses the platform clock frequency, computes an event
threshold and writes the original global. `wfe_timeout_init` checks threshold
below 64, reads `CNTKCTL_EL1`, ORs the threshold shifted by four and `0xf`,
and writes it back. It does not clear the old event-index bits. Therefore
readback preserves firmware-supplied bits; do not assume one hardware value.
These routines do not themselves execute a WFE wait or enable GIC timer IRQs.

The primary host fixture runs the original parser and calculations with the
actual pinned ADT and synthetic boot arguments: frequency 19,200,000 Hz and
default threshold 4. Architectural registers are mocked. For initial CNTKCTL
`0x100`, terminal value is `0x14f`; that is a host result, not a hardware target.
An empty ADT produces the original arithmetic trap in this fixture and is not
accepted as a substitute for the primary test.

## Independent checkpoints

| Phase | Reader name | Placement |
| --- | --- | --- |
| 16 | BCM_ROUTE_SKIPPED | Complete obsolete BCM block excluded |
| 17 | M1_READBACK | Immediately after original SCTLR/ISB |
| 18 | TIMER_STUB_RETURN | Original timer stub RET, after platform return |
| 19 | WFE_CONFIG_RETURN | Original WFE configuration RET, frame restored |
| 20 | WFE_INIT_RETURN | After original CNTKCTL write, at original RET |
| 21 | MISC_SETUP_DONE | After both misc stubs, before chosen DT lookup |
| 12 | WFE_STOP_CNTKCTL | Actual terminal CNTKCTL readback after an M1 guard |

**P16 phase12 is CNTKCTL, not SCTLR.** ENTRY low32 remains original SCTLR;
phase17 and every late hook require M1. Original platform/timer/WFE/misc BL
instructions and return addresses are preserved. The three RET hooks restore
all31 GPRs, SP and NZCV before the original return. The last displaced stack
store is not executed because this is the terminal boundary.

Six invalid CRC templates are bound to fresh ARM CRC, version cookie
`0x5031364b45524e4c`, and slot tag `0x5031365300000000`. Each independent
one-byte commit revalidates ENTRY and exact preceding slot state. The console
header stays at ENTRY until terminal publication; interrupted witnesses can
be raw-only. Missing or consumed ARM stops before original startup.

No new memory reservation, counter-dependent delay or BCM experiment is added.
Console mapping remains exactly 64 noncacheable, nonexecutable pages. The
2784-byte probe fits the existing 2800-byte zero cave. Kernel text HMAC is
resealed and mutation bounds are audited.

## Hardware procedure

1. Flash only the qualified P16M image to `boot_b`, activate B, verify B active
   and bootable, then reboot to the untimed menu. Recording must be ready.
2. Open **P16 reader FIRST**. Photograph the complete initial page before any
   Query, ARM, new-session action or kernel launch. Review header and tail room.
3. Execute **Query PSCI + record**, then **ARM** once. Photograph ID, CRC and
   `ARM + six invalid templates OK` before returning to the menu.
4. Launch **P16M once**, preserving a continuous recording. Every new kernel
   attempt requires a fresh Query/ARM; direct relaunch deliberately halts.
5. After reboot open reader FIRST, photograph all rows and the bottom
   `WFE_STOP_CNTKCTL` row before any write. Match ARM/ENTRY ID, CRC, offsets,
   independent slots, terminal and ring growth. Expected complete growth after
   ARM is 512 bytes; reset-return, if any, is a separate record.

If the screen stops, retain the full screen and recording and report elapsed
time. A missing record is not an exact stopping PC. Manual recovery starts a
separate observation. A fully committed terminal proves the bounded path was
reached; it does not independently identify the reset source.

Reboot and Power off remain in the menu. No boot_a, vbmeta, userdata, TzDxe or
9008 write is part of this diagnostic. Prior P15 evidence must be preserved;
never rerun a build-only recorder over later hardware results.
