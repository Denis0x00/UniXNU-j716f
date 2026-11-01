# J716F device repository guide

## Latest checkpoint: P16M WFE/misc returns and CNTKCTL terminal observed

P16M continuous11.998s video confirms selected P16M, DARWIN BOOT/green handoff, screen loss, Lenovo/Android splash, P16M menu return and read-only reader matching final photo. ENTRY/NOW VALID next=size0x28aa1, consoleCRCB9A3B4BA, logical13/raw26, latest ID2/phase12/A684F65B. Fresh ARM93A96F6F matches prelaunch; ENTRY5AF6E0B5 value93A96F6F00C00808(M0). All six phases16..21 are L1/R1 with bound value5031365393A96F6F and CRCsAA287B4B,E666CD05,FD46BF3F,D6F1DD05,CB065B4B,48BB45ED. TerminalA684F65B records actual CNTKCTL_EL1=0x4f after M1 guard. All nine CRCs reconstruct exactly; growth512 matches ENTRY+six slots+terminal. New reader ID3/query0/armed0, no MAP_FAILURE/RESET_RETURN. Original WFE configuration/init and misc returns completed and records retained over one observed reboot. PMIC02000001->02000041. Timer IRQs, real WFE wait/wakeup, sustained initialization, ArmCpu/GIC, reset source and userspace remain unverified.
Next: Preserve this read-only result; next instrument original /chosen SecureDTLookupEntry, xnu_platform_stall property lookup and following control-flow/initialization boundary. Each new attempt requires fresh version-specific Query/ARM; do not directly relaunch consumed P16 ARM.
Earlier pending-launch notes below are chronological and superseded. Never rerun build-only recorders over hardware evidence.

P16 fresh Query/ARM photo confirms NOW VALID next=size0x288a1, logical5/raw18, THIS RUN ID2/query1/armed1/CRC93A96F6F, EL1, PSCI1.1 and SYSTEM_RESET FEATURES0. ARM + six invalid templates OK is displayed. Growth320 bytes matches four Query records plus ARM; phase9 CRC reconstructs exactly with P16 cookie5031364B45524E4C and prior0x28861. Frozen initial0x28761/ECF994E2 and old P15 rows remain unchanged. No new-session action or P16 launch observed. One fresh ID2 attempt is ready; predicted complete header0x28aa1 is derived, not observed.
Next: return to menu, one P16M launch with recording, then reader FIRST and full photo before writes. Never directly reuse consumed ARM.

P16 initial reader photo confirms WFE TRACE r1 at EL1: ENTRY/NOW VALID next=size0x28761, consoleCRCECF994E2, logical0/raw13. Frozen ID1 ARM B9A219EB cookie5031354B45524E4C and ENTRY F2FD8F05 valueB9A219EB00C00808 match old P15; raw latest phase12 F286B6BF is not a P16 CNTKCTL terminal. All six P16 slot rows and four terminal rows are zero. THIS RUN ID2/query0/armed0. Unused tail96403 bytes exceeds Query896-byte budget; no new session needed. No P16 launch or controlled reset retention result; parent menu not pictured.
Next: current reader Query then ARM once; photograph full page/ID/CRC before one P16M launch. No new-session action.

P16M flashed only to boot_b after final image/source hash and fastbootd product/unlock/slot/partition gates. B active and bootable verified; reboot command succeeded. Recording readiness confirmed. Menu appearance, initial reader, fresh Query/ARM and kernel observations remain pending. Phase12 is CNTKCTL, not SCTLR. Open P16 reader FIRST and photograph the full initial page before Query/ARM/session action or kernel launch.

P16M timer/WFE/misc terminal diagnostic built and host verified: 48 actual-kernel and 31 linked-reader mocked cases passed. Original PE/timer/WFE/misc BL/LR and WFE bodies preserved; independent bound phases18..21 cover timer stub return, WFE configure return, WFE init return and misc completion. Terminal phase12 records actual CNTKCTL, not SCTLR, after M1 guard. Actual parser/calculation executes with pinned ADT in synthetic boot arguments (19.2MHz/default threshold4). Interrupted configure/init, original arithmetic trap/panic, late corruption, consumed ARM and mapping/session gates checked. Final source/FD/FAT/EFI/HMAC/nonpayload/SEC/boot-image audits passed. No P16 hardware result or device write; timer interrupt/WFE wait and ArmCpu/GIC remain separate. boot_b only; reader FIRST, fresh Query/ARM each launch.
Read `docs/V23P16_TIMER_WFE.md` and `research/v23p16-timer-wfe.json`.
Do not rerun build-only recorders over hardware evidence. This section supersedes historical next actions below; preserve P15 hardware evidence.

## Latest checkpoint: P15M original platform entry and return observed

P15M continuous13.932s video confirms selected P15M kernel, DARWIN BOOT/green handoff, screen loss, Lenovo/Android splash, P15M menu return and read-only reader. Final video reader matches photo: ENTRY/NOW VALID next=size0x328a4, consoleCRCF498612C, logical13/raw13, latest ID1/phase12/F286B6BF. Fresh ARM B9A219EB matches prelaunch; ENTRY F2FD8F05 valueB9A219EB00C00808(M0). All six phases16..21 are L1/R1 with bound value50313553B9A219EB and CRCs99CE21F7,D58097B9,CEA0E583,E51787B9,F8E001F7,B6E7E7E2. Terminal F286B6BF records actual SCTLR3454593D(M1). All nine CRCs reconstruct exactly; growth512 matches ENTRY+six slots+terminal. New reader ID2/query0/armed0, no MAP_FAILURE/RESET_RETURN. Original PE_init_platform(0, bootArgs) entered and returned before next arm_init call, with records retained over one observed reboot. PMIC02000001->02000041. Reset source, sustained continuation, interrupt/GIC/native timer and userspace remain unverified.
Read `docs/V23P15_PLATFORM_INIT.md` and `research/v23p15-platform-init.json`.
Next: preserve read-only baseline; audit subsequent configure_timer_apple_regs/WFE/misc setup before new continuation checkpoints. Each launch needs fresh version-specific Query/ARM; consumed ARM halts at admission.
P15 is a terminal frozen-baseline bridge. Native timer source compilation and ArmCpu/GIC integration remain separate. Host41 kernel/31 linked-reader mocked cases and final source/image audits passed. Hardware scope is the bound entry/return terminal attempt above.
Do not rerun build-only recorders over hardware evidence. boot_b only; no boot_a/vbmeta/userdata/TzDxe writes. This section supersedes historical next actions below; prior evidence is preserved.

## Latest checkpoint: P14M original early init reaches cpu_data_init return

P14M postlaunch reader photo confirms ENTRY/NOW VALID next=size0x2e5a6, consoleCRC5D1C75BD, logical13/raw26, latest ID2/phase12/76EE0045. Fresh ARM6304806B matches prelaunch; ENTRY FCBEA27A value6304806B00C00808(M0). All six phases16..21 are L1/R1 with bound value503134536304806B and CRCs66FE13FA,A1D55ACD,BAF528F7,91424ACD,8CB5CC83,C0FB7ACD. Terminal76EE0045 records actual SCTLR3454593D(M1). All nine CRCs reconstruct exactly; header growth512 matches ENTRY+six slots+terminal. New reader ID3/query0/armed0, no MAP_FAILURE or RESET_RETURN. One original early-init path reaches cpu_data_init return before PE_init_platform, with records present after user-reported automatic reboot. No continuous reset video reviewed; reset source, sustained initialization, timer/GIC and userspace remain unverified.
Read `docs/V23P14_EARLY_INIT.md` and `research/v23p14-early-init.json`.
Next: preserve the read-only baseline; instrument PE_init_platform entry/return and early dependencies in a new diagnostic. Every launch requires fresh version-specific Query/ARM. Never directly reuse consumed ARM.
P14 is a terminal bridge on the frozen source-built/prelinked baseline. Native timer source compilation and ArmCpu/GIC integration remain separate.
Host checks:38 actual-kernel and31 linked-reader mocked cases; final image/source/HMAC/nonpayload/SEC audits passed. Hardware scope is the bound terminal attempt above.
Do not rerun build-only recorders over hardware evidence. boot_b only; no boot_a/vbmeta/userdata/TzDxe writes.
This section supersedes historical next actions below; prior evidence is preserved.

## Latest checkpoint: P13M actual MMU-on terminal record observed

Read `docs/V23P13_MMU_CONTINUATION.md` and
`research/v23p13-mmu-continuation.json`. P13M flashed only boot_b, B active.
Initial read-only and fresh Query/ARM photos precede the result. ARM ID1 CRC
CCF99510 matches the postlaunch snapshot. ENTRY/NOW VALID next=size0x2ffcd,
logical13/raw13, consoleCRC401D38FB. ENTRY3CFCD765 records M0 SCTLR00C00808.
All six bound phases16..21 are L1/R1. POST_MMU D7BBC9E4 records actual
SCTLR3454593D (M1). All nine ARM/ENTRY/slot/terminal CRCs reconstruct exactly;
512-byte growth from ARM matches ENTRY+six slots+terminal. New reader ID2,
query0/armed0. User extra ARM selection is protected by same-invocation guard;
exactly one matching ARM survived. Short hardware MMU-on execution and retained
records at reader reentry are established. Reset video/source, repeatability,
sustained normal kernel execution, timer/GIC and userspace remain unverified.
Next: preserve terminal baseline; continue original post-MMU code with new
independent early-initialization checkpoints. No BCM B or P13P prerequisite.
P13 is a terminal binary bridge on frozen source-built/prelinked baseline,
not a rebuilt native timer kernel. Separate native timer profile resolves but
has not been source-compiled; ArmCpu is not integrated. 63 actual-kernel and34
linked-reader mocked cases and image audits passed. Never rerun build-only
recorders over hardware evidence. Historical next steps below are superseded.

## P13 consumed-ARM relaunch follow-up

User is unsure of the earlier launch sequence, then explicitly confirmed a
new direct P13M launch without fresh Query/ARM hung. Photo ends at DarwinBoot
exiting boot services, not a live kernel PC. Host replay of actual P13M on
reconstructed matching thirteen records halts before original startup with
console unchanged. This matches the required-latest-phase9 admission guard;
live stopping PC remains unobserved. Prior bound M1 records remain valid,
but do not claim a controlled observed automatic-reset sequence. Preserve
hang photo, manually restart to menu and reader FIRST; manual recovery is a
separate event. P14 preparation remains authorized, not yet built/flashed.

## V23P11D raw-only stage1 observed

P11A retained published stage5; P11B and P11D each retained fresh matching
raw-only stage1 over an observed reboot. D changes only the first terminal
DSB SY to DMB SY and showed no retained-progress improvement in these trials.
Read `docs/V23P11_COMMIT_PUBLICATION.md` and its research JSON first. Next:
independent non-overwriting progress witnesses; exact stopping PC, external
BCM completion, reset source and MMU-on remain unresolved. P10C stays gated.
Historical version-specific pending notes below are superseded by the latest
observation for that variant.

Read `docs/V23P8_RETENTION.md` and `research/v23p8-retention.json` before
continuing. P7 SYSTEM_RESET rebooted twice per the user after Query; the
intermediate blocked selection was its per-invocation gate. The later INVALID
photo has unknown timing. Recovery pstore was mounted but empty; its old
65-byte named console capture is missing-file error text, not a log.

P8 is host-verified, with manual invalid-only header initialization, separate
logical/raw CRC record scans, and a two-step SYSTEM_RESET after BASELINE/Query.
Capture frozen ENTRY before any write, and match the photographed PRE_RESET
ID/CRC immediately after a controlled observed reboot. Read the current research
record for flash status; do not infer physical retention from host tests.
The standalone upstream header-restoration backport is build-driver patch 0004;
the pinned upstream revision remains unchanged. One controlled P8 SYSTEM_RESET now preserves a valid header and matching
logical/raw PRE_RESET ID=1/phase=3/CRC=0xe13f0c63 at read-only invocation 2.
Audit writer mappings/cache visibility and registers before attaching MMU checkpoints. Historical notes below remain version-specific.

This repository owns the TB-J716F board description, SoC transition snapshot,
component selection, firmware references and packaging policy. It does not
contain vendor binaries, IPSW members, trust caches, userdata or boot images.

## Read first

Read the UniXNU [porting guide](https://unixnu.com/zh/porting/),
[migration guide](https://unixnu.com/zh/porting/migration), [device repository](https://unixnu.com/zh/porting/device-repository),
[device tree](https://unixnu.com/zh/porting/device-tree), [UEFI integration](https://unixnu.com/zh/porting/uefi-firmware),
and [bring-up order](https://unixnu.com/zh/porting/bring-up) before changing this port.

## Work boundaries

- Keep physical addresses, CPU affinity, GIC ranges, memory reservations and
  firmware paths tied to the committed research evidence.
- Keep the local `socs/sm8250` source marked as a transition snapshot until the
  public common scaffold is reviewed for this port and pinned to a fixed commit.
- Add reusable drivers to their canonical component repository; board wiring and
  package policy belong here.
- Do not add or publish Apple/Lenovo binary inputs, raw DTBs, payload images,
  photos, logs, seeds or VM credentials.
- Current diagnostic hardware testing remains `boot_b` only; do not write `boot_a`, vbmeta or
  userdata. TzDxe is outside the current scope.

## Current checkpoint

See `docs/V22_POST_MMU_CHECKPOINT.md` and `research/v22-checkpoint-preparation.json`.
V21 video confirms its own green/blue/magenta entry bands and a restart through
the Lenovo logo; startup PMIC changes from `0x02000001` to `0x02000041`. The reset
cause and sustained MMU-on execution remain unverified. V22A/B video measured 20.245/19.730 seconds to screen loss, without the
intended 1s/3s response; B ended in A-slot ZUI. V23P/C/V/M host audits and
crypto reseals passed. P was flashed only to boot_b, B activated and rebooted;
video shows only its first diagnostic line and a later restart to its menu.
PSCI execution is unverified. Investigate the common first-line/barrier/return
and register-read prefix before C/V/M hardware tests. See
`docs/V23_INDEPENDENT_PROBES.md` and `research/v23-independent-probes.json`.
Keep version evidence distinct. `board.toml` retains the historical V17 recipe; V22 uses
`scripts/build-v22-payload.py` and the independent firmware V22 DSC/FDF.

## Commands

From the latest UniXNU checkout:

```sh
./unixnu defconfig j716f_source_defconfig -D ../UniXNU-j716f
./unixnu status --json -D ../UniXNU-j716f
./unixnu tree -D ../UniXNU-j716f
python3 scripts/porting-record.py check --device ../UniXNU-j716f
```

Use a local `unixnu.local.toml` for private paths. Build the kernel, package,
firmware, payload and boot image in dependency order only after the required
IPSW, trust-cache, Data seed and firmware DTB inputs are available.

## Porting record

`porting.toml` is authoritative. Update the affected area's findings or
evidence whenever board code, configuration, firmware integration or packaging
changes. Separate host verification from device observations; the current
record does not claim a bootable iPadOS port.

Latest checkpoint: V23P2 passed host audits and was flashed only to boot_b via fastbootd after a bootloader flash failure. Video shows squares 1/2 only, before the title DSB completes visibly. No CurrentEL/SCTLR or PSCI progress is established. See `docs/V23P2_PREFIX_CHECKPOINTS.md`; isolate the barrier and earlier legacy MMIO separately.

Latest prepared controls: V23P3A/B passed actual-image audits and ten mocked renderer/dispatcher cases. A changes only the legacy BCM store; B changes only title-row DSB to DMB, preserving non-title DSB through a dispatcher. Mode 5 and checkpoints stay fixed; identify A/B from the menu. No P3 hardware result exists. Test A first, boot_b only. See `docs/V23P3_SINGLE_FACTOR.md` and `research/v23p3-single-factor.json`.

V23P3A was flashed only to boot_b via fastbootd, B activated and reboot command succeeded. Video, menu identity, prefix progress and PSCI results are pending; P3B remains unflashed. Do not rerun the build-only status recorder over this device-operation evidence.

V23P3B was flashed only to boot_b via fastbootd, B activated and reboot command succeeded. User report for P3A was brief screen then reboot; no checkpoint or PSCI result established. P3B video is pending.

V23P3A 视频已确认菜单身份；约1.7秒后画面变黑，未见诊断方块或EL/SCTLR，PSCI与复位源仍未知。

V23P3B 视频确认标题 mode 5 和方块 1/2；方块 3、EL/SCTLR 未见。约20.9秒后画面消失，随后 Lenovo/Android 启动并回到 UniXNU 菜单。标题 DMB 对照未显示出越过第2格的硬件效果，PSCI/复位源仍未知。

Latest prepared control: V23P4 wraps the legacy BCM store at 0xfffffff0073fe32c with before/after framebuffer markers, preserving the original store and title DSB. Host audit and emulator passed; no hardware result. See docs/V23P4_EARLY_MMIO_ENVELOPE.md; boot_b only.

Latest prepared control: V23P4 r1 initializes both marker regions, preserves them through the subsequent clear, and wraps the legacy BCM store at 0xfffffff0073fe32c with before/after framebuffer markers, preserving the original store and title DSB. Host audit and emulator passed; no hardware result. See docs/V23P4_EARLY_MMIO_ENVELOPE.md; boot_b only.

V23P4 r1 was flashed only to boot_b through fastbootd; B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Menu identity, marker visibility, PSCI and reset observations await video. Retry-count variable is unsupported.

V23P4 r1 video confirms both early MMIO markers and title mode 5 with prefix squares 1/2; square 3 and EL/SCTLR are not observed. Original STR was passed, but external transaction completion is unproven. Diagnostic display disappears about 15.9s after entry, then Lenovo/Android splash and r1 menu return; PMIC 0x02000001 -> 0x02000041, reset cause unknown.

Next-build source preference: UniXNU menu scale changed from 4 to 3 after r1 video; not compiled or flashed. Kernel diagnostic font unchanged. Original r1 library sources were privately snapshotted and match its remote-build hashes; the next build must explicitly audit the changed menu module.

V23P5A/B matched controls passed host image, source/HMAC, pair-delta and emulator checks. A kernel equals P4 r1; B differs only in relocated STR-to-NOP and HMAC. Both use compiled menu scale 3, identical non-payload modules, markers and title DSB. No P5 hardware trial or device write. Test A then B on boot_b. See docs/V23P5_MATCHED_MMIO_CONTROL.md.

V23P5A was flashed only to boot_b in fastbootd; B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. A menu identity, marker progression and reset observations await video. P5B remains unflashed. No PSCI or MMU-on result is claimed.

V23P5A video confirms the compact A menu, title mode 5, prefix squares 1/2 and both early MMIO markers, reproducing P4 r1 visible progress. No square 3, EL/SCTLR or PSCI evidence. Display disappears about 17.47s after entry, followed by Lenovo/Android splash and return to A menu; PMIC 0x02000001 -> 0x02000041. Reset cause and external MMIO completion remain unknown. P5B is the next boot_b control and remains unflashed.

V23P5B was flashed only to boot_b in fastbootd; B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Its menu identity and diagnostic progress await video. Reviewed P5A remains the visible control; no P5B functional, PSCI or MMU-on result is claimed.

V23P5B video confirms the compact B menu, both lower envelope markers and partial title in frame 56; the diagnostic is captured in only one camera frame. Top prefix 1 completion, prefix 2/3, EL/SCTLR and A0/pre-SMC are not established. Entry band to display loss is about 0.067s (camera observation), followed by Lenovo/Android splash and return to B menu; PMIC 0x02000001 -> 0x02000041. Rapid intentional PSCI reset is a hypothesis, not a verified result. Next proposed control retains P5B and replaces only its SMC with an in-place branch, preserving the preceding DSB; not yet built. This checkpoint supersedes earlier pending/unflashed notes; no new device operations or push occurred during video review.

Logging reference checkpoint: Reviewed upstream logging/automatic-test guide at 16e592abee8a550dec8154ed1f38b5ebc4f16213 and same-SoC umi at 3eb72578597d2ca16471b172d45eddc096a34a04. Prioritize board-qualified retained RAM trace plus separate firmware QR logging. Existing J716F Recovery DT/dmesg show overlapping boot_log 0xb0000000+0x80000 and pstore 0xb0000000+0x100000; console file length 262132 does not establish its physical subrange. Do not copy umi 4MiB/2MiB layout or sheng disk/reset constants. Verify layout/format and minimal reset retention before attaching XNU records. Full unattended logging requires a validated reader and return-to-fastboot cycle. Read-only source/evidence review only; no new device test or logging implementation. See docs/LOGGING_AND_SM8250_REFERENCE_REVIEW.md.

V23P6 RAMLOG r1 is host-verified and unflashed: read-only TWRP live parameters plus matching-build Recovery allocator emulation locate console at 0xb00be000+0x40000 (12-byte DBGC, ECC=0). iomem difference is reserved System RAM vmap versus non-RAM request_mem_region. Independent app validates live header before manual single-marker append; default read-only, no automatic start/reset, manual EFI warm request, no XNU. Actual SEC/four DXE nonpayload deltas are only the reservation-size byte; other modules/APRIORI unchanged. Retention and live header remain unverified. See docs/V23P6_RAMLOG_RETENTION.md.

V23P6 RAMLOG r1 was flashed only to boot_b through fastbootd with the audited image; B activation, current-slot b and slot-unbootable:b=no were verified, and reboot command succeeded. First live header, marker append, warm-reset retention and Recovery readback await device observation. No other partition write or repository push occurred.

V23P6 first video/photo confirms the correct menu and read-only app: console DBGC 0x43474244, next=size=0x37bd9, capacity=0x3fff4, VALID, marker counts 0 and no append. The inferred console header is now device-readable and consistent, with 33819 bytes of unused capacity. No append, restart retention, Recovery readback or XNU progress is established. See research/v23p6-first-header.json.

V23P6 second video verifies manual RAM marker append and same-run readback: next=size 0x37bd9 -> 0x37c09 (+48 bytes), now=1, appended_this_run=1, APPEND READBACK OK. At about 15s the app reports Warm reset returned; no actual reboot is observed. At about 28s the app exits to parent EFI return diagnosis with Success/code 0, which is normal app return and not reset success. Cross-reset retention and Recovery readback remain unverified. Root cause of the returned firmware reset call is unknown. First-header transcription corrected to 0x37bd9 (33819 free bytes). See research/v23p6-append-reset-return.json.

V23P7 RESET r1 is host verified and unflashed. Only the P6 embedded FAT config/app changes; all non-payload inner-FV module sections and compact SEC executable match P6 exactly. Actual P6 reset driver uses SMC, queries RESET2 then falls back to SYSTEM_RESET; unused stack inputs are a host finding, not the proven failure cause. P7 defaults to read-only inventory; manual PSCI queries/firmware Warm/direct reset actions log PRE/RETURN to the validated console. Five wrapper cases, two linked record/gate cases and image/source audits passed with mocked SMC responses. First hardware trial must only photograph inventory and Query PSCI results on boot_b; no reset, retention or XNU result yet. See docs/V23P7_RESET_DIAGNOSTICS.md and research/v23p7-reset-probe.json.

V23P7 RESET r1 (uncommitted source additions on the recorded repository base; exact source/image hashes in the P7 manifest) was flashed only to boot_b through fastbootd. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Inventory/PSCI-query observations await video. No reset trial, retention result or new XNU checkpoint is established.

V23P7 photos verify EL1, installed reset driver GUID/RVA/prefix match, PSCI_VERSION=0x00010001 (1.1) and FEATURES=0 for SYSTEM_RESET and RESET2 A64. Valid console next=size grows 0x2923f -> 0x2958a (+843 bytes); P7 entry=0/now=6 and Queries complete=1. Query returns and app-displayed same-run records are established. No actual P7 reset, retention, Recovery readback or new XNU checkpoint. Next manual trial uses Direct RESET2 only; preserve initial post-reboot inventory or returned x0..x3. Uninitialized old-driver inputs remain an unproven cause.

V23P7 RESET2 video/photo confirms two direct 0xc4000012 type=0/cookie=0 calls return x0=0xfffffffffffffffe (INVALID_PARAMS -2), x1..x3=0. FEATURES remains 0; valid console next=size 0x2958a -> 0x296a3 -> 0x297bc and P7 now 6 -> 8 -> 10. No reboot is observed in the 10.50s video; retention and Recovery readback remain untested. Old Warm falls back only on FEATURES failure, not on returned RESET2 error; historical P6 SMC returns remain uncaptured. Next manual test is existing Direct SYSTEM_RESET only. No new device commands, flash or source changes during this media review.

## SYSTEM_RESET user report and reentry gate

User reports the first P7 Direct SYSTEM_RESET rebooted, then selecting it after reentering the app did not reboot. No new SYSTEM_RESET photo/video or post-reset ring counts yet. Fresh app load clears mQueried; without rerunning Query, the action is blocked before SMC. Second-attempt gate status versus actual returned SMC is unresolved. Preserve full page/entry/now before new queries. One reboot is user-reported; repeatability and RAM retention remain unverified.

## SYSTEM_RESET clarification and controlled retention retry

User clarified the second no-reboot selection displayed blocked; rerunning Query then Direct SYSTEM_RESET rebooted again. Two successful SYSTEM_RESET invocations are user-reported; no SYSTEM_RESET error return was reported. User then forced power off and entered ZUI13 before post-reset inventory capture. Earlier reset retention remains unknown; later marker state cannot isolate that path. Repeat a controlled P7 boot_b cycle from a photographed baseline, without forced shutdown or ZUI detour. No new flash/source/device commands performed.

Controlled P7 retention cycle relaunched: fastboot initially reported current-slot a and slot-unbootable:b=yes. set_active b succeeded; current-slot b and slot-unbootable:b=no verified; reboot command succeeded. No partition image was flashed. Prior P7 flash/image hash supplies expected identity, not a new partition readback. Await menu identity, baseline and post-SYSTEM_RESET read-only photo; retention remains unverified.

V23P8 was flashed only to boot_b; B activation and bootable state were verified, and reboot command succeeded. Initial ENTRY/NOW photo confirms a valid header and zero P8 records (research/v23p8-retention.json). Skip initialization; append BASELINE once, Query without reset, and capture the full page. Physical reset retention remains unverified.

P8 BASELINE readback and Query are now device-observed: count=5, ID=1, phase=8, CRC=0xb4d25630; PSCI 1.1 and FEATURES(SYSTEM_RESET)=0. Next press Direct SYSTEM_RESET once only and photograph PRE_RESET phase=3 ID/CRC before the second press. Retention remains unverified; see research/v23p8-retention.json.

P8 PRE_RESET is photographed: ID=1, phase=3, CRC=0xe13f0c63, logical/raw count=6, NOW VALID, next=size=0x3fcbf. The first reset press only armed the app. Next press again without navigation, then photograph reentry ENTRY before any writes/query. No reset or retention observation yet.

Latest result supersedes pending P8 reset notes: P8 controlled SYSTEM_RESET video shows the armed PRE_RESET page, screen loss, Lenovo/Android splash, return to the P8 menu and manual app reentry. At 10.898s (frame 327), frozen ENTRY and NOW retain DBGC 0x43474244, next=size=0x3fcbf, six logical and six raw CRC-valid records, latest ID=1/phase=3/CRC=0xe13f0c63, exactly matching the pre-reset photo. NOW remains VALID. New invocation ID=2 with baseline=0/query=0 and read-only status confirms capture before new app writes/query. One observed immediate UEFI-to-UEFI reset cycle preserves the header and matching PRE_RESET record. This does not establish all payload bytes, power-off/ZUI/Recovery retention, repeated reliability, or XNU/MMU checkpoint progress.

## P9 kernel trace

Read docs/V23P9_KERNEL_TRACE.md and research/v23p9-kernel-trace.json.
V23P9E/M host-verified kernel trace candidates are prepared. E logs the first kernel entry then direct SYSTEM_RESET; M logs entry, pre-MMU and post-MSR/ISB before terminal reset. A manual UEFI Query/ARM cookie and expected prior CRC/phase bind each kernel append to its session. Exactly the audited console is mapped as 64 NC pages after strict TCR/MAIR/TTBR/spare-page guards. Pre/post preserve integer registers, SP and NZCV; pre handles the already-virtual SP with M=0. Original BCM store and vectors remain unchanged. 23 actual-assembly logic cases and 12 linked-reader cases pass with mocked system registers/SMC/UEFI; E/M nonpayload FV sections and compact SEC match P8; crypto HMACs match. No P9 device write or MMU progress yet. Test E before M.
Do not rerun host record generation over later flash/hardware evidence.

Latest P9 device operation: V23P9E was flashed only to boot_b after exact image-hash and fastbootd identity/unlock/size gates. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Await the initial read-only reader ENTRY photo before Query/ARM. No kernel entry/reset-retention observation or MMU progress yet. V23P9M remains unflashed and gated on observed E entry-record retention.

Latest P9 reader photo: First P9 reader photo confirms KERNEL TRACE r1, ENTRY/NOW DBGC 0x43474244, next=size=0x2b0e4, ENTRY whole-console CRC32=0x51d98b35 and NOW VALID. Logical count=0; raw count=6 with old P8 ID=1/phase=3/CRC=0xe13f0c63. All six P9 checkpoint rows have logical/raw hits=0; invocation ID=2, query=0/armed=0 at EL1 and read-only status. Old P8 payload bytes remain outside current logical extent after the intervening fastboot path; this is not a controlled P9 kernel-retention result or evidence locating a header change. Next Query then ARM once, photograph this-run ID/CRC before kernel launch.

Latest P9 Query/ARM checkpoint: P9E Query/ARM photos confirm NOW VALID throughout. Query: next=size=0x2b1e4, logical/raw counts=4/10, ID=2, query=1/armed=0, last CRC=0x3ef428f0, EL1, PSCI_VERSION=0x00010001 and SYSTEM_RESET FEATURES=0. ARM: next=size=0x2b224, logical/raw counts=5/11, ID=2, query=1/armed=1, last CRC=0x47ccacb5 and ARM readback OK. Frozen ENTRY and checkpoint rows remain unchanged from reader entry; no kernel launch or reset is observed in these photos.
Next: Keep recording. Return to menu and select V23P9E: entry only; terminal SYSTEM_RESET, without another Query/ARM. After an observed reboot, open the reader FIRST and photograph frozen ENTRY before Query/ARM; match armed ID=2 and the KERNEL_ENTRY record. E intentionally leaves PRE_MMU/POST_MMU absent. If no reboot, preserve video and last display before forced shutdown. M remains gated on observed E entry-record retention.

Latest P9E result supersedes pending launch notes: P9E controlled entry trial is device-observed. Video starts with armed ID=2/CRC=47CCACB5, shows the E entry-only kernel selected, then screen loss, Lenovo/Android splash and return to the P9E menu. The separate photo, identified by the user as first reader reentry after that reboot, shows frozen ENTRY and NOW VALID: DBGC=0x43474244, next=size=0x2b264, whole-console CRC=0x8b79118f, logical/raw counts=6/12, latest ID=2/phase=10/CRC=0xf17e59fc. ARM has one logical and one raw hit with matching prior CRC=0x47ccacb5; KERNEL_ENTRY has one of each with CRC=0xf17e59fc and SCTLR_EL1=0xc00808 (M=0). Reconstructing phase-10 record bytes from ID/value/prior header reproduces the photographed CRC. New reader invocation ID=3/query=0/armed=0 and read-only status confirm capture before new Query/ARM. PRE_MMU, POST_MMU, MAP_FAILURE and RESET_RETURN have zero hits, expected for E. The valid header and same-session entry record survived one observed reboot; actual reset-source attribution, M execution and sustained MMU-on remain unverified.
Next: P9E entry-record retention prerequisite passed. Prepare fastboot and recording for the already-built V23P9M; flash only boot_b, verify B active/bootable, then open reader FIRST and photograph frozen ENTRY. If NOW VALID, Query then ARM once and photograph the new ID/CRC before launching M. After reset photograph reader ENTRY before any Query/ARM; match ARM/ENTRY/PRE_MMU/POST_MMU or MAP_FAILURE by that new ID. If no reset, preserve last display/video before recovery.

Latest P9M device operation: V23P9M was flashed only to boot_b after exact image SHA256 and fastbootd product/unlock/slot-count/partition-size gates. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Launch targets the untimed menu only; recording readiness for the manual kernel trial was not newly confirmed. Initial M menu identity, reader ENTRY, Query/ARM and kernel/MMU checkpoints await device observations. The observed P9E entry-record retention prerequisite remains preserved.
Next: At the P9M menu open P9 reader FIRST and photograph the complete frozen ENTRY before Query/ARM. If NOW is VALID, prepare recording, then Query once and ARM once; photograph THIS RUN ID/CRC and ARM readback status before launching the M kernel. Stop on INVALID and preserve the page.

Latest P9M initial reader: Initial reader photo after the P9M fastboot launch confirms common J716F V23P9 KERNEL TRACE r1 reader, frozen ENTRY/NOW DBGC=0x43474244, next=size=0x2bf58, ENTRY whole-console CRC=0xbdb07c93 and NOW VALID. Logical count=0; raw count=6 with old P8 ID=1/phase=3/CRC=0xe13f0c63; all six frozen P9 checkpoint rows have zero logical/raw hits. THIS RUN ID=2/query=0/armed=0 at EL1, read-only status. The earlier E ARM/ENTRY chain is not present in this snapshot after the intervening fastboot path; the cause or timing of those changes is not isolated, and this does not alter the separately observed E controlled-reboot result. No M kernel launch or MMU progress is observed.
Next: In this reader invocation prepare recording, select Query PSCI + record (NO RESET), then ARM next kernel attempt once after successful Query. Photograph the full ARM page including NOW VALID, THIS RUN ID/CRC and ARM readback OK before manual M kernel launch. Stop and preserve the page if Query/ARM fails or the header becomes INVALID.

Latest P9M Query/ARM: P9M Query/ARM video and photo show a successful same-invocation preparation, with frozen ENTRY unchanged. At video frame 90, Query OK: NOW VALID next=size=0x2c058, logical/raw counts=4/10, ID=2, query=1/armed=0, CRC=0xbbfaa9d1, EL1, PSCI_VERSION=0x00010001 and SYSTEM_RESET FEATURES=0. ARM photo: NOW VALID next=size=0x2c098, logical/raw counts=5/11, ID=2, query=1/armed=1, CRC=0xc2c22d94 and ARM readback OK. The phase-8/phase-9 CRCs reconstructed from the displayed session/value/prior headers match. All frozen P9 rows remain zero because they describe the earlier reader-entry snapshot. The approximately 11.065-second video ends on the armed reader; it does not show M kernel launch or reset. Match this M attempt by both ID=2 and ARM CRC=C2C22D94, distinct from E ID=2/CRC=47CCACB5.
Next: Continue recording, Return to menu, then select V23P9M: entry / pre-MMU / post-MMU; terminal SYSTEM_RESET. Do not Query/ARM again before launch. After an observed reboot open reader FIRST and photograph the complete frozen ENTRY before Query/ARM; match ARM ID=2/CRC=C2C22D94 and the same-session ENTRY/PRE_MMU/POST_MMU or MAP_FAILURE. If no reboot, preserve the video and last display before forced shutdown or other recovery.

Latest P9M launch observation: P9M launch photo confirms menu identity J716F V23P9M KERNEL TRACE r1 and the M entry/pre-MMU/post-MMU kernel item, with stage DARWIN BOOT, a green top band, and visible DARWINBOOT text: inputs validated; exiting boot services. It is a launch/handoff display, not a reader checkpoint. User subsequently reports an automatic reboot. No video of this launch/reboot or post-reboot reader snapshot has been supplied yet; entry/PRE/POST/MAP_FAILURE record state and actual reset source remain unknown. Do not infer MMU progress or a specific stopping PC from the still image or reboot alone.
Next: After the reported automatic reboot, open P9 reader first and photograph the full frozen ENTRY page before Query/ARM. Match this M attempt by ARM ID=2/CRC=C2C22D94 and inspect same-session KERNEL_ENTRY, PRE_MMU, POST_MMU, MAP_FAILURE and RESET_RETURN. Preserve any available complete launch/reboot video.

P9M second-launch clarification: User clarifies that after the first user-reported automatic M reboot, the M kernel was launched a second time directly, without a fresh Query/ARM or a supplied first post-reboot reader capture. User reports this second launch remains stopped; the supplied photo shows DARWINBOOT inputs validated; exiting boot services. Source stage1 requires an immediately preceding valid phase-9 ARM cookie, and branches to the WFE halt if append validation fails. If the first launch consumed ARM by appending ENTRY or later records, the unarmed second launch would be rejected; this is a source-supported explanation, not a confirmed device stopping PC. Neither first-run MMU progress nor second-run rejection is established without retained reader records. The second launch must not be treated as a freshly armed M trial.
Next: Preserve the stalled-screen photo and available video. Recover to the P9 menu with one hardware restart if necessary, documenting that extra recovery path; open reader FIRST and photograph full frozen ENTRY before Query/ARM or another kernel selection. Treat any recovered records as a later recovery snapshot, not the missing immediate first-reboot capture. For each subsequent trial: reader snapshot, fresh Query/ARM, one kernel launch, then reader snapshot before any repeat.

Latest M recovery reader: M recovery reader photo shows frozen ENTRY and NOW VALID: DBGC=0x43474244, next=size=0x2c0d8, whole-console CRC=0xc0749332, logical/raw counts=6/12, latest ID=2/phase=10/CRC=0xdc7aa441. Frozen ARM has one logical and one raw hit, CRC=0xc2c22d94 and cookie=0x50394b45524e454c, exactly matching the M prelaunch baseline. KERNEL_ENTRY has one logical and one raw hit, CRC=0xdc7aa441, SCTLR_EL1=0xc00808 (M=0); reconstructing the record with prior header 0x2c098 reproduces that CRC. PRE_MMU, POST_MMU, MAP_FAILURE and RESET_RETURN all have zero hits. New reader invocation ID=3/query=0/armed=0 with read-only status confirms no new Query/ARM in the snapshot. This recovery capture follows the user-reported first automatic reboot, an unarmed second launch that stalled, and the requested hardware recovery; it is not the missing immediate first-reboot capture, and recovery method/timing is not independently captured. It establishes a surviving M-session entry record and consumed ARM, with no retained later checkpoints. Source requires ARM immediately before entry: the now-visible consumed chain supports second-launch gate rejection, but does not prove the exact PC of that launch. The missing PRE record spans original firstcpu code, pre-hook stack handling, mapping checks/setup/barriers and PRE append; it does not isolate an SCTLR/MMU failure or a reset source.
Next: Preserve this recovery snapshot. Repeat the existing P9M once with a clean observed sequence: current reader invocation ID=3, Query then ARM once, photograph actual ID/CRC and valid header, launch M exactly once, record the complete cycle, then open reader FIRST and photograph frozen ENTRY before any Query/ARM or second kernel selection. If it stalls, preserve last screen/video before further recovery. If ENTRY-only repeats in the immediate reader capture, add finer prefix and pre-hook-before-mapping checkpoints; do not change SCTLR or disable the legacy BCM store based on the present snapshot.

Fresh M ID=3 repeat: Fresh M session ID=3 photo sequence supplies ARM preparation, M handoff display and a new read-only reader invocation. ARM photo: NOW VALID next=size=0x2c218, logical/raw counts=11/17, ID=3/query=1/armed=1, CRC=0x671d1670, PSCI_VERSION=0x00010001, FEATURES=0. Frozen rows still describe prior ID=2, as designed. M launch photo identifies entry/pre-MMU/post-MMU terminal SYSTEM_RESET and shows the DarwinBoot handoff prompt. Subsequent reader photo: ENTRY/NOW VALID next=size=0x2c258, whole-console CRC=0x8b348f61, logical/raw counts=12/18, latest ID=3/phase=10/CRC=0xd1afe339. Frozen ID=3 ARM and KERNEL_ENTRY each have one logical/raw hit; ARM CRC=0x671d1670 matches prelaunch, ENTRY CRC=0xd1afe339 and SCTLR_EL1=0xc00808 (M=0). Both displayed CRCs reconstruct from the record format and prior headers. PRE_MMU, POST_MMU, MAP_FAILURE and RESET_RETURN again have no hits. New invocation ID=4/query=0/armed=0 confirms a fresh read-only snapshot. Photos were supplied in response to the requested single-launch/immediate-reader sequence; they independently establish the fresh chain and ENTRY-only repeat, while continuous reboot timing, reset source and absence of all intervening operations are not video-verified. Further identical M retries are not required before adding finer probes. Missing PRE spans original firstcpu, pre stack conversion, map_console and PRE append, not a demonstrated MMU-enable failure.
Next: Prepare terminal prefix controls V23P10A/B around the original BCM STR and P10C at the original pre-MMU site before console mapping. Reuse the proven ARM/ENTRY framing and reader, with phase-11 terminal pre-MMU record then direct SYSTEM_RESET; document each hook and the original code it executes. Test A first on boot_b after build/audit and fresh physical readiness; do not infer the legacy store is causal from P9 alone.

## V23P10 prepared controls

V23P10A/B/C are built and host verified, without device writes. Terminal sites are before BCM STR, after STR before ISB, and before original SCTLR. Entry and terminal phase11 use the unchanged P9 CRC writer; phase11 means prefix stop, not P9M mapping success. Eighteen actual full-prefix kernel cases and thirteen linked-reader cases passed with explicit system-register/barrier/MMIO/SMC/UEFI mocks and synthetic boot args. Exact images, sources, probe and reader hashes are in research/v23p10-prefix-controls.json. Actual packed FAT/menu/kernel/HMAC audited; nonpayload FV modules and compact SEC match P9/P8, and reader EFI is byte-identical to P9. No new hardware MMU or retained terminal record is established.
Next: Fresh fastboot/recording readiness, boot_b-only P10A, reader FIRST photograph, Query then ARM once photograph ID/CRC, launch A once, immediate reader FIRST photograph before new writes.

## Next menu build requirement

User requests Reboot and Power off in the XUI main menu on the next compilation. Read docs/NEXT_MENU_BUILD.md. P10 images are already built; preserve their identity.

## Latest P10A device operation

V23P10A was flashed only to boot_b after exact image SHA256, fastbootd product/unlock/slot-count/partition-size gates. Image SHA256=6637e8055846c79d64f09a8115d311cc92c4ac8ba91977ec9dd81f4ef9c98e0b. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. User confirmed fastboot and recording readiness. Only the untimed menu was launched; no manual Query/ARM, kernel execution, terminal record or reset retention is observed yet. B/C remain unflashed.
Next: At the P10A menu open reader FIRST and photograph frozen ENTRY/NOW before Query/ARM. With NOW VALID, Query once then ARM once and photograph ID/CRC; launch A exactly once, then open reader FIRST after reboot before any writes.
Do not rerun build-only record generation over this device evidence.

## P10A initial read-only baseline

P10A initial reader photo shows common J716F V23P9 KERNEL TRACE r1 reader; parent-menu variant is not visible and expected A identity is tied to the recorded flash. Frozen ENTRY/NOW VALID: DBGC=0x43474244, next=size=0x2e137, whole-console CRC=0x5104221a. Logical/raw record counts are both zero; latest ID/phase/CRC and all six frozen checkpoint rows are zero. THIS RUN ID=1/query=0/armed=0/CRC=0 at EL1, read-only status; PSCI/features zeros are unqueried defaults. This is a valid starting baseline after the fastboot path, not a controlled retention result or proof of P10 kernel execution. Fresh Query/ARM and kernel launch remain pending.
Next: In this valid reader invocation ID1 select Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query succeeds. Photograph complete ARM page with NOW VALID, THIS RUN ID/CRC and ARM readback OK before kernel selection. Stop and preserve the page if Query/ARM fails or header becomes INVALID.

## P10A Query/ARM ID1

P10A ARM photo confirms NOW VALID next=size=0x2e277 (+0x140 from baseline), logical/raw counts=5/5, THIS RUN ID=1/query=1/armed=1, CRC=0xde027ae6, EL1, PSCI_VERSION=0x00010001 and SYSTEM_RESET FEATURES=0. ARM readback OK is visible. Frozen ENTRY remains next=size=0x2e137/CRC=0x5104221a and all frozen checkpoint rows are zero, as expected before these writes. Reconstructing phase9 with ID1/cookie=0x50394b45524e454c/prior next=size=0x2e237 reproduces CRC DE027AE6. No kernel launch or reset is observed in this photo; use ID and CRC together to identify this A attempt.
Next: Continue recording. Return to menu and select V23P10A: STOP before BCM STR exactly once, without Query/ARM again. After reboot open reader FIRST and photograph full frozen ENTRY before new writes; match ARM ID1 CRC DE027AE6 and inspect ENTRY/PRE_MMU (P10 prefix stop). If it stalls preserve last display/video before recovery.

## P10A terminal prefix result

P10A continuous video confirms A menu/kernel identity, green handoff band, screen loss, Lenovo/Android splash, return to A menu and read-only reader reentry. At frame240 (~7.999s), frozen ENTRY/NOW VALID next=size=0x2e2f7, whole-console CRC=0x4e5c6046, logical/raw counts=7/7, latest ID1/phase11/CRC=D8F17B33. ARM/ENTRY/PRE_MMU each have logical/raw hits 1/1: ARM DE027AE6 matches prelaunch photo, ENTRY F3461909 and terminal phase11 D8F17B33 both have SCTLR=0xc00808 (M=0). Reconstructed record CRCs match all three. POST/MAP_FAILURE/RESET_RETURN have no hits; new reader ID2/query0/armed0 and read-only status preserve the first snapshot. PMIC changes 0x02000001 to 0x02000041 without independent reset-source attribution. In A phase11 is the terminal prefix stop BEFORE BCM STR; it proves this prefix/physical-stack/logger path and one observed reset retention cycle. A does not execute BCM STR, map_console or MMU enable. Advance to already-built B with a fresh Query/ARM; B/C remain unflashed.
Next: Prepare fastboot and recording for already-built P10B (original STR executed, terminal branch before following ISB). Flash only boot_b; reader FIRST baseline; fresh Query/ARM and photo; one B launch then immediate read-only reader capture. No further A retry required.

## P10B device launch

V23P10B was flashed only to boot_b after exact image SHA256, fastbootd product/unlock/slot-count/partition-size gates and the observed A terminal-record prerequisite. B image SHA256=e77d5b67e8b6a044ef1ca1df10e143f07bc78e060ad371214c5634bd9f690cb4. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. User confirmed fastboot and recording readiness. Only the untimed menu was launched; B menu identity, initial reader, fresh Query/ARM and kernel/BCM terminal result await observation. A retained phase11 result remains preserved; C remains unflashed.
Next: At the P10B menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before any Query/ARM. If NOW VALID, prepare a fresh B Query/ARM once and photograph ID/CRC before launching B exactly once. The A ARM is consumed and must not be reused.

## P10B initial reader baseline

P10B initial reader photo shows common P9 reader, expected B identity tied to recorded flash (parent menu not visible). ENTRY/NOW VALID next=size=0x27fe9, whole-console CRC=0x2394a7e1, logical count0/raw count7. Frozen checkpoint ID1 ARM/ENTRY/PRE_MMU have L0/R1 with raw CRCs DE027AE6/F3461909/D8F17B33 matching historical A; later rows are zero. THIS RUN ID2/query0/armed0/CRC0 at EL1 and read-only status; PSCI/features zeros are unqueried defaults. A bytes are raw-only outside current logical extent, not B progress. Header changes across intervening fastboot/boot are not isolated to a cause; A immediate observed retention remains independently established. Next fresh B Query/ARM; no B kernel launch or retained terminal result yet.
Next: In this NOW VALID reader invocation ID2, Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query success. Photograph full ARM readback page including actual ID/CRC and NOW VALID before B launch. Frozen raw ID1 rows are historical A; identify B using fresh ARM ID and CRC.

## P10B Query/ARM ID2

P10B Query and ARM photos confirm fresh reader session ID2. Query: NOW VALID next=size=0x280e9, logical/raw=4/11, query1/armed0, CRC=DCAE06F5, PSCI1.1/FEATURES0 and Query OK. ARM: NOW VALID next=size=0x28129, logical/raw=5/12, query1/armed1, CRC=A59682B0 and ARM readback OK. Both phase8/phase9 CRCs reconstruct exactly from ID2/value/prior headers. Frozen ENTRY remains 0x27fe9/CRC2394A7E1 and frozen rows still describe old raw A ID1, as designed. No B kernel launch or reset is shown. Match the coming B attempt using both ID2 and ARM A59682B0, distinct from historical A.
Next: Continue recording. Return to menu and select V23P10B: STOP after BCM STR before ISB exactly once. Do not Query/ARM again before launch. After reboot open reader FIRST before writes and photograph full page, matching B ARM ID2 CRC A59682B0 then ENTRY/phase11. If stalled preserve last screen/video before recovery.

## P10B launch video and reported later reboot

P10B 22.648s video starts on NOW VALID armed ID2/CRC A59682B0, then reader returns through EFI diagnosis START IMAGE/Success/code0 before B kernel selection. B menu and STOP after BCM STR before ISB identity are confirmed. Green handoff band and DarwinBoot inputs validated; exiting boot services remain visible through final frame678 (~22.615s). User reports a later automatic reboot into UniXNU, but supplied clip ends before that later cycle and has no post-reboot reader. Earlier EFI Success is reader return, not B terminal reset evidence. B intentionally resets after successful phase11 append, so reboot alone neither proves nor disproves terminal/BCM progress. Need first read-only post-reboot reader matching B ID2/ARM A59682B0; C stays gated.
Next: After the reported B reboot, open reader FIRST and photograph entire frozen ENTRY/NOW before Query/ARM or another kernel launch. Match B ARM ID2/A59682B0 and inspect ENTRY/phase11/RESET_RETURN. Do not mark B passed/failed or advance C based on reboot alone.

## P10B raw terminal only; C remains gated

B post-reported-reboot reader photo matches fresh ID2 ARM A59682B0 logically/raw. ENTRY/NOW VALID next=size=0x28169, whole-console CRC=F03B33AB; logical6 latest phase10 CRC DCD7DF11, raw14 latest phase11 CRC C49C5711. Frozen ENTRY L1/R1 and PRE_MMU L0/R1 both value SCTLR=0xc00808 (M0); reconstructed ID2 phase10/prior28129 and phase11/prior28169 CRCs exactly match. New reader ID3/query0/armed0 is read-only. This is new B raw terminal data, distinct from old A CRC D8F17B33. The audited path crosses original BCM STR into terminal writer, but phase11 is not published in current logical extent: expected next=size after commit would be 0x281a9. Source order is record byte stores -> DSB SY at 0xfffffff0073fc7d0 -> byte readback -> size/next stores at fc828/fc82c -> DSB SY fc830 -> return -> reset DSB/SMC. Raw bytes plus old header do not isolate exact PC; first DSB, readback, publication/visibility and reset-retention differences remain candidates. Direct SYSTEM_RESET and external BCM transaction completion are unverified. Supplied video does not include later reboot; user reports it. Keep B terminal_record_survived_observed_reset false and C gated; isolate writer publication with an independent terminal-only control.
Next: Preserve this B reader without another launch. Diagnose terminal record publication separately: post-body DSB SY, byte readback and header writes/retention. Prepare a single-factor B writer control before C; keep entry writer unchanged, retain CRC/header binding and do not initialize/repair the device log to turn raw-only into a pass. Next firmware compilation must also honor docs/NEXT_MENU_BUILD.md.

## V23P11 prepared; latest next step

V23P11A/B/D built and host verified, unflashed. Terminal phase11 value is P11S stage1..5, not SCTLR or MMU-on. Entry CRC writer and linked reader match P9. 40 kernel, 18 reader and 22 power-app mocked cases passed; actual images/HMAC/sources audited, nonpayload FV and compact SEC match P9/P8. Reboot and Power off are included through direct PSCI queries/actions, without RESET2; device power actions remain untested.
Read docs/V23P11_COMMIT_PUBLICATION.md and research/v23p11-commit-publication.json.
Next: Confirm fastboot/recording readiness, flash only P11A boot_b; reader FIRST photo, fresh Query/ARM ID/CRC photo, one A launch, then immediate read-only reader photo. Review A before B; review B before D. P10C stays gated.

## P11A device operation; kernel result pending

V23P11A was flashed only to boot_b after exact SHA256, fastbootd product/unlock/slot-count/partition-size gates. B active, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Only the untimed menu was requested; menu identity, initial reader, Query/ARM, kernel launch and retained P11 stages remain unobserved. P11B/D remain unflashed; P10C gated. Image SHA256=e19591e47ae931348d1c1e5a8f01ab9a2e397a2108c21cb0a9b75af187bdbea5.
Next: At P11A menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or power actions. Verify menu identity and log validity before fresh Query/ARM and one A launch. B/D unflashed; P10C gated.

## P11A initial read-only reader

P11A initial reader photo shows common P9 reader, ENTRY/NOW VALID next=size=0x2baf2, whole-console CRC32=413A1021, logical0/raw7. Raw ID1 ARM DE027AE6, ENTRY F3461909 and phase11 D8F17B33 (value SCTLR 0xc00808) match historical P10A; no P11S stage code. THIS RUN ID2/query0/armed0/CRC0 at EL1, read-only. Parent P11A menu identity not visible; expected variant comes from recorded flash. No current kernel or retention result.
Next: In this NOW VALID reader invocation ID2, Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query succeeds. Photograph the complete ARM page with actual ID/CRC and ARM readback OK before launching P11A. Historical raw ID1/DE027AE6/F3461909/D8F17B33 is P10A, not P11 progress.

## P11A fresh Query/ARM ID2

P11A Query/ARM photo confirms NOW VALID next=size=0x2bc32 (+0x140), logical5/raw12, THIS RUN ID2/query1/armed1/CRC58E2DBCF at EL1, PSCI1.1 and SYSTEM_RESET FEATURES0. ARM readback OK. Reconstructed phase9 ID2/cookie/prior0x2bbf2 CRC matches exactly. Frozen ENTRY remains 0x2baf2/CRC413A1021 with historical P10A raw ID1 rows; no kernel launch or P11S stage yet.
Next: Continue recording; Return to menu then select V23P11A: STOP before BCM STR once, without another Query/ARM. Record through any reboot; open P9 reader FIRST and photograph complete frozen ENTRY/NOW before new writes. Match ID2 ARM 58E2DBCF then KERNEL_ENTRY and P11S phase11 stage value. If stalled preserve final display/video before recovery. B/D remain gated.

## P11A stage5 retained across observed reboot

P11A 10.565s video confirms A before-BCM menu selection, screen loss, Lenovo/Android splash, return to A menu and manual read-only reader. Frame316 (~10.532s): ENTRY/NOW VALID next=size=0x2bcb2, console CRC94CC810C, logical7/raw14, ID2 phase11 CRC43AA3200. Frozen ARM/ENTRY/PRE_MMU each L1/R1: ARM58E2DBCF matches prelaunch, ENTRY AAC67917 value SCTLR0xc00808 (M0), terminal value5031315300000005 stage5. All three CRCs reconstruct exactly; new invocation ID3/query0/armed0 read-only. One observed reboot preserves the new writer stage5 and logical header before BCM STR. PMIC02000001->02000041; independent reset attribution unknown. No original BCM store, MMU enable, sustained XNU or power-app result. P11B is next; D/P10C remain gated.
Next: Enter fastboot and prepare continuous recording for the already-built P11B only. After boot_b flash, photograph reader FIRST, fresh Query/ARM ID/CRC, then one B launch and immediate read-only reader capture. Compare P11S stage/header visibility with A. Review B before considering D; P10C remains gated.

## P11B device operation; observation pending

V23P11B was flashed only to boot_b after observed P11A stage5/ARM/CRC/reset-retention prerequisite and exact image SHA256, fastbootd product/unlock/slot-count/partition-size gates. B activation, current-slot b and slot-unbootable:b=no verified; reboot command succeeded. User confirmed fastboot/recording readiness. Untimed menu only; B menu identity, initial reader, fresh Query/ARM and kernel/stage observations pending. Original BCM STR is present in B, but its execution has not yet been observed in this trial. A result preserved, D/P10C gated. B image SHA256=94c97bacfef84deab3bceaacfee5f1f95b20f4e74a57c01f9beb5bbabc64a40c.
Next: At the P11B menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or menu power actions. Confirm B menu identity and current log validity, then fresh B Query/ARM ID/CRC before one kernel launch. Historical P11A records must remain distinct. D and P10C remain gated.

## P11B initial read-only reader

P11B initial reader photo confirms ENTRY/NOW VALID next=size=0x2c55a, console CRCAC4189D6, logical0/raw7. Raw ID1 ARM DE027AE6, ENTRY F3461909 and phase11 D8F17B33/value0xc00808 match historical P10A, not B progress. THIS RUN ID2/query0/armed0/CRC0 at EL1 read-only. Parent B menu identity not visible; expected B follows recorded boot_b flash. Fresh B ARM CRC required because ID2 is reused after the intervening fastboot/boot path; earlier A immediate stage5 retention stays independently established. No B kernel launch or P11S stage observed.
Next: In this NOW VALID reader invocation ID2, select Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query succeeds. Photograph full ARM readback page including actual ID/CRC before launching B. Do not reuse earlier P11A ID2/58E2DBCF; B fresh CRC must be recorded.

## P11B fresh Query/ARM ID2

P11B Query/ARM photos confirm Query NOW VALID next=size0x2c65a logical4/raw11 ID2/query1/armed0 CRC9E81CF4A, then ARM NOW VALID next=size0x2c69a logical5/raw12 ID2/query1/armed1 CRCE7B94B0F. PSCI1.1/FEATURES0 and ARM readback OK. Both phase8 and phase9 CRCs reconstruct exactly using prior0x2c61a/0x2c65a. Frozen ENTRY remains0x2c55a/CRCAC4189D6 with historical P10A raw ID1 rows. This B CRC differs from prior A ID2/58E2DBCF; no B launch or terminal stage observed yet.
Next: Continue recording. Return to menu and select V23P11B: STOP after BCM STR before ISB exactly once, without Query/ARM again. After any reboot open P9 reader FIRST and photograph full frozen ENTRY/NOW before writes. Match B ARM ID2/E7B94B0F then ENTRY and P11S phase11 stage/logical/raw/header status. If stalled preserve final display/video before recovery.

## P11B raw-only stage1; D is the next control

P11B 31.562s video confirms B after-STR/before-ISB menu identity, green handoff/DarwinBoot inputs, screen loss, Lenovo/Android splash, B menu return and read-only reader. Frame946 (~31.528s): ENTRY/NOW VALID next=size0x2c6da, CRC D603FA7F, logical6 latest phase10 F901C2DA/raw14 latest phase11 796B1792. Frozen ID2 ARM E7B94B0F and ENTRY F901C2DA(SCTLR0xc00808/M0) are L1/R1; phase11 is L0/R1 value5031315300000001 stage1. All CRCs reconstruct exactly; new reader ID3/query0/armed0 read-only. Header grew only64 from ARM, not expected terminal publication to0x2c71a. Original BCM STR was crossed and terminal body stored; external completion, first DSB completion, stage2+ execution/visibility/retention and reset source unresolved. A stage5 versus B raw-only stage1 motivates P11D single first-DSB-to-DMB control. No terminal SYSTEM_RESET, MMU-on or power-app result.
Next: Preserve the B reader/video. Prepare fastboot and continuous recording for already-built P11D, the B control with only terminal first DSB SY changed to DMB SY. Flash boot_b only; reader FIRST baseline, fresh Query/ARM ID/CRC, one D launch, immediate read-only reader capture. Compare stage and logical header against B; keep P10C gated. No additional B launch required now.

## P11D device operation; observation pending

V23P11D was flashed only to boot_b after reviewed B fresh raw-stage1/ARM/CRC/observed-reboot prerequisite and image SHA256, fastbootd product/unlock/slot-count/partition-size gates. B/D assembled probe difference rechecked: only terminal first DSB SY changed to DMB SY; subsequent barriers retained. B activation, current-slot b and slot-unbootable:b=no verified; reboot command succeeded. User confirmed fastboot/testing readiness following recording request. Untimed menu only; D menu identity, reader, fresh ARM and kernel results pending. A/B observations preserved; P10C gated. D image SHA256=0a0b647be892107591787f05b4709286b20ab49c25125822e81a306bc65c73af.
Next: At P11D menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or power actions. Confirm D menu identity and current log validity, then fresh Query/ARM ID/CRC before one D kernel launch. D differs from B only by terminal first DSB SY to DMB SY; later barriers remain. P10C remains gated.

## P11D initial read-only reader

P11D initial reader photo shows ENTRY/NOW VALID next=size0x2f1fb, whole-console CRC65E3F901, logical0/raw0, frozen checkpoint ID0 and all six rows zero. THIS RUN ID1/query0/armed0/CRC0 at EL1 read-only. Valid new baseline after intervening fastboot/firmware boot; no trace records recognized and no D execution or controlled retention conclusion. Earlier A stage5/B raw-stage1 immediate reboot observations remain independently preserved. Expected D variant tied to recorded flash; parent menu not visible.
Next: In this NOW VALID reader invocation ID1, select Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query success. Photograph full ARM readback page including actual ID/CRC before launching P11D. No initialization, power action or kernel launch before ARM evidence.

## P11D fresh Query/ARM ID1

P11D Query/ARM photos confirm Query NOW VALID next=size0x2f2fb logical4/raw4 ID1/query1/armed0 CRC16B5614F, then ARM NOW VALID next=size0x2f33b logical5/raw5 ID1/query1/armed1 CRC6F8DE50A. PSCI1.1/FEATURES0 and ARM readback OK. Phase8/phase9 CRCs reconstruct exactly with prior0x2f2bb/0x2f2fb. Frozen ENTRY remains0x2f1fb with all rows zero. Full-resolution photo review corrects earlier console-CRC transcription55C3F901 to65E3F901; no image/hash or device log altered. No D kernel/stage result yet.
Next: Continue recording; Return to menu and select V23P11D: after BCM STR, FIRST DMB control exactly once, without Query/ARM again. After reboot open P9 reader FIRST and photograph complete frozen ENTRY/NOW before writes. Match D ID1/ARM6F8DE50A then ENTRY, raw/logical P11S stage and header. If stalled preserve final display/video before recovery.

## P11D raw-only stage1; B/D comparison

P11D 30.83s continuous video confirms D first-DMB menu, handoff, screen loss, Lenovo/Android splash and D menu return. Frame924: frozen ENTRY/NOW VALID next=size0x2f37b, console CRC995CCD03, logical6 latest phase10/16CCB8AB, raw7 latest phase11/96A66DE3. ID1 ARM6F8DE50A and ENTRY16CCB8AB(SCTLR0xc00808/M0) L1/R1; phase11 L0/R1 value5031315300000001 stage1. All three CRCs reconstruct exactly; reader ID2/query0/armed0. Header grew64 from ARM, not terminal publication to0x2f3bb. B and D retain the same stage1-only result; first DSB-to-DMB substitution did not improve observed progress. Same-slot non-atomic updates prevent exact PC/barrier attribution. Original BCM STR crossed, external completion and reset source unknown. No terminal SYSTEM_RESET or MMU-on evidence.
Next: Preserve the D reader/video; no repeat launch needed. Design independent, non-overwriting progress slots around the first terminal barrier and stage2 construction/store so later writes cannot erase earlier witnesses. Qualify reserved storage and reader/CRC/visibility semantics, and retain before-BCM control plus single-factor barrier variants. Build/audit before a new boot_b trial. P10C remains gated; no MMU-on claim.

## Upstream and peer review, 2026-10-26

Reviewed upstream ae687009348296e97e49ce6e925795044f363ce6 and fixed umi/pipa/kebab/enuma/SM-T733 snapshots. Header restore is already backported; cluster-type/catalog changes do not solve the P11 stop. Kebab documents a probe-induced fault after TTBR0 removal, and its architectural-timer patch skips the legacy BCM route. Pipa provides a warm-reset/early-pstore/USB export reference, not J716F-qualified PMIC or retention inputs. P12 should use independent witnesses and a separate matched BCM-STR-versus-skip factor; qualify mapping lifetime before MMU markers. No device operation, new build, pin change or push.
Read `docs/UPSTREAM_REFERENCE_REVIEW_20261026.md` before P12 implementation.

## V23P12 host-verified candidates, unflashed

V23P12A/B built and host verified, unflashed. Six independent pre-staged CRC-bound records use one-byte commits; ENTRY high32 binds ARM CRC and low32 is SCTLR. A skips BCM and B preserves captured original address/value, with exactly one STR/NOP probe delta and identical reader/ADT/nonpayload firmware. 58 actual-assembly and22 linked-reader mocked cases passed; final image/HMAC/source audits passed. Byte-identical P11 power app reuses22 host cases. No P12 hardware/retention/MMU-on result; A first on boot_b only.
Read docs/V23P12_INDEPENDENT_SLOTS.md and research/v23p12-independent-slots.json.
Next: Connect fastboot and prepare recording for P12A BCM SKIP only, boot_b only. Confirm menu; P12 reader FIRST frozen photo, fresh Query/ARM ID/CRC and six-template-OK photo, one A launch, then immediate read-only reader capture. Review six bound slots/header/reboot before B. P10C gated.

## V23P12A device operation

V23P12A BCM SKIP was flashed only to boot_b after image SHA256, matched-probe and fastbootd identity gates. B activation, current-slot b and slot-unbootable:b=no verified; reboot command succeeded. Menu-only launch; recording readiness was not separately confirmed. Initial reader and kernel observations were pending at device operation time; B remains unflashed and gated.

## P12A initial reader: saturated circular console

P12A initial photo confirms V23P12 reader at EL1, frozen ENTRY/NOW VALID next0x26e9 size0x3fff4 consoleCRC50977087, logical0/raw0 and all eight rows zero. THIS RUN ID1/query0/armed0. This is a saturated circular console, not an empty log. P12 linear-tail requirement fails before Query (832 bytes required); no Query block message or kernel launch observed. Parent A menu not pictured; variant follows recorded flash. Preserve baseline; qualify full-ring handling before a new attempt, B/P10C remain gated.
Next: Preserve the saturated P12A initial reader baseline; do not Query/ARM or launch kernel with this ineligible header. Qualify explicit session storage or bounded circular-slot handling before a new P12 attempt. B and P10C remain gated.

## P12 r2 full-console session handling

P12 r2 A/B built and host verified, unflashed. New reader requires explicit double confirmation before discarding a VALID saturated/exhausted console logical history; frozen entry remains, payload not bulk-cleared, INIT records old next/size, failure locks Query/ARM. 45 actual-linked reader cases passed including full-session-to-both-unchanged-kernels-to-frozen-reader flow, changed-console cancellation, failure and stale-ID gates. A/B kernel/ADT/probes equal r1 byte for byte; 58 prior assembled cases reused after identity check. Power app equals P11, 22 cases reused. Final FD/FAT/EFI/source/HMAC/nonpayload/boot-image checks passed. No r2 hardware, retention or MMU-on result; A first on boot_b only.
Read docs/V23P12R2_SESSION_ROLLOVER.md and research/v23p12r2-session-rollover.json.
Next: Flash audited r2A to boot_b only when fastboot ready. Confirm r2A menu, open P12 r2 reader FIRST and photograph complete ENTRY/NOW. If full/exhausted VALID log, manually confirm New linear session twice and photograph successful next=size64; otherwise keep existing eligible tail. Then recorded fresh Query/ARM ID/CRC plus six-template-OK, one A launch, immediate read-only reader after reboot. Review r2A all-six/publication/reboot before B; P10C gated.

## P12 r2A device operation

V23P12R2A BCM SKIP flashed only to boot_b after image SHA256/matched-probe/fastbootd identity gates. B active and bootable verified; reboot command succeeded. Menu-only launch; no independent recording-ready confirmation. Initial r2 reader, manual new-session action, Query/ARM and kernel observations pending. r2B remains unflashed and gated by reviewed r2A six-slot/header/reboot evidence. No new kernel progress, retention or MMU-on result.
Next: Open P12 r2 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or new-session action. Review NOW validity/space; if VALID saturated/exhausted, manually confirm New linear session twice only after saving the old page, photograph new-session success, then fresh recorded Query/ARM and one r2A launch. B and P10C gated.

## P12 r2A initial linear baseline

P12 r2A initial photo confirms r2 reader at EL1; ENTRY/NOW VALID next=size0x38b69, consoleCRC98919B86, logical0/raw0, frozen checkpointID0 and all eight rows zero. THIS RUN ID1/query0/armed0/CRC0. Unused linear tail29835 bytes exceeds Query832-byte budget; no session rollover needed or performed. No kernel launch, r2 retention or hardware rollover qualification; parent A menu not visible, variant follows recorded flash. Earlier r1 saturated baseline stays independent.
Next: In current r2 reader ID1, prepare continuous recording; Query PSCI + record (NO RESET) once, then ARM next kernel attempt once only after successful Query. Photograph full Query and ARM readback including actual ID/CRC and six-template-OK. Skip New linear session; do not launch kernel before fresh ARM evidence is reviewed. B/P10C gated.

## P12 r2A fresh Query/ARM

P12 r2A Query/ARM photo shows NOW VALID next=size0x38ca9 logical5/raw5, THIS RUN ID1/query1/armed1 CRC063FE873 at EL1, PSCI1.1/FEATURES0 and ARM + six invalid templates OK. Frozen initial ENTRY remains0x38b69/CRC98919B86 with zero rows. Header growth320 bytes matches four Query records plus ARM; ARM phase9 CRC reconstructs exactly with cookie5031324b45524e4c and prior0x38c69. No session rollover or kernel launch observed. Fresh ARM binding saved for one A launch; expected terminal header0x38e69 is derived, not observed.
Next: Continue recording; Return to menu and select V23P12R2A BCM SKIP once, without further Query/ARM/session writes. Record through any reboot, then open P12 r2 reader FIRST and photograph full frozen ENTRY/NOW before writes. Match ID1 ARM063FE873, ENTRY binding, six independent slots and header publication (derived terminal next0x38e69). If stalled preserve video/final display before recovery. B/P10C gated.

## P12 r2A six-slot result

P12 r2A postreboot photo confirms ENTRY/NOW VALID next=size0x38e69 consoleCRCC067D0A6, logical12/raw12 latest ID1/phase21/04FFEC87. Fresh ARM063FE873 matches prelaunch; ENTRY1210179B value063FE87300C00808(M0). All six stages16..21 are L1/R1 with CRCsE66CD221,AA22646F,B1021655,9AB5746F,8742F221,04FFEC87 and ARM-bound value50313253063FE873; all eight CRCs reconstruct exactly. New reader ID2/query0/armed0, read-only. User explicitly reports automatic reboot/menu return; no continuous video reviewed. A skipped BCM and reached all six commits/publication, with matching records present after reported reboot. Kernel SMC/reset source, original BCM completion, MMU-on and manual session rollover remain unqualified. r2B original-STR control is next.
Next: Preserve A reader photo; enter fastboot and prepare continuous recording for audited r2B only. Flash boot_b only, verify B active/bootable, then P12 r2 reader FIRST photo, fresh Query/ARM ID/CRC/templates photo, one B launch and immediate read-only reader capture. Compare six slots and header with A. No more A launch needed; P10C gated.
