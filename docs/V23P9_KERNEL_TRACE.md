# V23P9: retained records at kernel entry and the MMU transition

## Current device status

P9E entry-record retention is device-observed. P9M has now been flashed only to boot_b; B active/bootable state was verified and reboot command succeeded. The M recovery reader retains matching ARM (ID=2/CRC=C2C22D94) and KERNEL_ENTRY (CRC=DC7AA441), with no PRE/POST/MAP_FAILURE records. This follows an automatic reboot report, an unarmed second launch and additional recovery; the immediate first-reboot snapshot is missing. M entry is established, MMU transition and exact stopping PC remain unverified.

## First reader observation

First P9 reader photo confirms KERNEL TRACE r1, ENTRY/NOW DBGC 0x43474244, next=size=0x2b0e4, ENTRY whole-console CRC32=0x51d98b35 and NOW VALID. Logical count=0; raw count=6 with old P8 ID=1/phase=3/CRC=0xe13f0c63. All six P9 checkpoint rows have logical/raw hits=0; invocation ID=2, query=0/armed=0 at EL1 and read-only status. Old P8 payload bytes remain outside current logical extent after the intervening fastboot path; this is not a controlled P9 kernel-retention result or evidence locating a header change. Next Query then ARM once, photograph this-run ID/CRC before kernel launch.

## Query and ARM observations

P9E Query/ARM photos confirm NOW VALID throughout. Query: next=size=0x2b1e4, logical/raw counts=4/10, ID=2, query=1/armed=0, last CRC=0x3ef428f0, EL1, PSCI_VERSION=0x00010001 and SYSTEM_RESET FEATURES=0. ARM: next=size=0x2b224, logical/raw counts=5/11, ID=2, query=1/armed=1, last CRC=0x47ccacb5 and ARM readback OK. Frozen ENTRY and checkpoint rows remain unchanged from reader entry; no kernel launch or reset is observed in these photos.

Keep recording. Return to menu and select V23P9E: entry only; terminal SYSTEM_RESET, without another Query/ARM. After an observed reboot, open the reader FIRST and photograph frozen ENTRY before Query/ARM; match armed ID=2 and the KERNEL_ENTRY record. E intentionally leaves PRE_MMU/POST_MMU absent. If no reboot, preserve video and last display before forced shutdown. M remains gated on observed E entry-record retention.

## Observed E reboot and retained kernel entry

P9E controlled entry trial is device-observed. Video starts with armed ID=2/CRC=47CCACB5, shows the E entry-only kernel selected, then screen loss, Lenovo/Android splash and return to the P9E menu. The separate photo, identified by the user as first reader reentry after that reboot, shows frozen ENTRY and NOW VALID: DBGC=0x43474244, next=size=0x2b264, whole-console CRC=0x8b79118f, logical/raw counts=6/12, latest ID=2/phase=10/CRC=0xf17e59fc. ARM has one logical and one raw hit with matching prior CRC=0x47ccacb5; KERNEL_ENTRY has one of each with CRC=0xf17e59fc and SCTLR_EL1=0xc00808 (M=0). Reconstructing phase-10 record bytes from ID/value/prior header reproduces the photographed CRC. New reader invocation ID=3/query=0/armed=0 and read-only status confirm capture before new Query/ARM. PRE_MMU, POST_MMU, MAP_FAILURE and RESET_RETURN have zero hits, expected for E. The valid header and same-session entry record survived one observed reboot; actual reset-source attribution, M execution and sustained MMU-on remain unverified.

The video ends at the returned menu; the separate reader photograph supplies the post-reboot record evidence. Raw media are private; hashes and observed fields are in the research JSON.

Next: P9E entry-record retention prerequisite passed. Prepare fastboot and recording for the already-built V23P9M; flash only boot_b, verify B active/bootable, then open reader FIRST and photograph frozen ENTRY. If NOW VALID, Query then ARM once and photograph the new ID/CRC before launching M. After reset photograph reader ENTRY before any Query/ARM; match ARM/ENTRY/PRE_MMU/POST_MMU or MAP_FAILURE by that new ID. If no reset, preserve last display/video before recovery.

## Scope

P8 established one controlled UEFI-to-UEFI SYSTEM_RESET cycle: the valid header
and logical/raw PRE_RESET ID=1, phase=3, CRC=E13F0C63 survived immediate read-only
reentry. P9 attaches records to the pinned experimental kernel copy. It remains
a terminal single-CPU diagnostic at bring-up stage 5, not a bootable XNU or iPadOS.

The baseline SHA256 remains
`cab8f385a44d21fc6f455cb501227452212ba44d34a9fa03d04fcf7e85324214`.
Only approved branch sites, verified zero text padding and the source corecrypto
HMAC change. The original vectors and legacy BCM store at
`0xfffffff0073fe32c` remain unchanged. There is no counter, diagnostic framebuffer
write or crypto bypass in the kernel probe. Existing P8 artifacts are preserved.

## Two variants

| Variant | Kernel behavior | First hardware use |
| --- | --- | --- |
| P9E | Record KERNEL_ENTRY, then direct SYSTEM_RESET | Validate entry writer and return path |
| P9M | Record KERNEL_ENTRY; run original prefix; map console; record PRE_MMU; execute displaced SCTLR+ISB; record POST_MMU; direct SYSTEM_RESET | Only after the E control returns an entry record |

Both menus have no timeout, default to the read-only reader, and identify the
variant in the kernel entry title. Opening the reader does not write records,
query PSCI or reset. A successful manual PSCI Query is required before ARM.
ARM writes a new session ID and a CRC-protected cookie. Return to the menu and
manually start the kernel without further Query/ARM operations. No other
record may intervene between ARM and the kernel's entry writer.

## Checkpoints and interpretation

The P8 64-byte framing and CRC/complement remain unchanged. The P9 reader and
decoder accept these additional phases:

| Phase | Name | Value |
| --- | --- | --- |
| 9 | ARM_KERNEL | `0x50394b45524e454c` |
| 10 | KERNEL_ENTRY | Actual SCTLR_EL1, before the original kernel prefix |
| 11 | PRE_MMU | Actual SCTLR_EL1 after successful mapping checks, before MSR |
| 12 | POST_MMU | Actual SCTLR_EL1 after MSR+ISB; M must be set |
| 13 | MAP_FAILURE | Guard code below; no MMU enable attempted |
| 14 | KERNEL_RESET_RETURN | x0 returned by the terminal SYSTEM_RESET call |

Every kernel append validates the immediately previous CRC-checked logical
record, ID, expected phase and corresponding current ring header. Entry also
requires the ARM cookie. It writes only the audited `0xb00be000+0x40000`
console and refuses bad headers. It never initializes a ring. Bytes are
read back before publishing size/next, with DSB SY ordering. ID remains fixed
through the ARM/ENTRY/PRE/POST chain. The reader freezes per-phase logical and
raw counts/CRCs/values at entry, separately from live ring statistics.

An ENTRY-only result in E establishes that the first logger ran; E intentionally
does not attempt PRE/POST. In M, ENTRY without PRE locates the missing evidence
between the original prefix and the pre-MMU append, but does not identify a PC.
MAP_FAILURE separates a deliberate mapping refusal from a missing checkpoint.
PRE without POST does not distinguish the MSR/ISB, instruction translation,
post stack access or post writer. POST with M=1 establishes execution through
that logger; sustained MMU-on/platform-expert execution is still untested.
Raw-only matches show surviving payload, without establishing ring validity.

## Stack, registers and mapping audit

At entry, DarwinEnter supplies the live XNU argument in x0 with M/C/I cleared.
The probe temporarily uses the same PC-relative physical stack top
(`0xfffffff007be8000` at link time) that the pinned start_first_cpu later uses
for SP_EL0. It preserves x0, incoming SP and NZCV. x19/x20/x21/x30 are temporary;
the pinned original prefix overwrites them before any original read. Other
callee-saved registers remain untouched. This is the actual boot ABI, not a
claim to preserve every unspecified incoming register.

Before MSR, XNU's SP is already high virtual while M=0. The wrapper converts it
with `SP - x22 + x23` (virtBase/physBase), saves x0..x18/x30 and NZCV, then restores
them and the exact original virtual SP before MSR. x19..x29 stay untouched.
After MSR+ISB, the original high virtual stack is used. The wrappers preserve
all 31 integer registers, SP and NZCV up to the deliberate terminal reset path.
The helper's 128-byte frame plus wrapper's 176 bytes stay within the existing
owned kernel stack, without firmware calls.

The original TTBR0 maps the owned kernel physical window; TTBR1 maps its high
virtual window. The console is outside that owned range. P9 requires:

- TCR_EL1 exactly `0x22a51a251a` (pinned 4 KiB/L1-root layout).
- MAIR index 1 exactly Normal non-cacheable (`0x44`).
- TTBR0 root equal to x25; TTBR1 root equal to x25+0x1000.
- TTBR0 L1 slot 2 empty.
- The last two pages of the original 64 KiB bootstrap table reservation zero
  and unreferenced by table descriptors in the other pages.

Only then it uses pages +0xe000/+0xf000 for L2/L3 and maps exactly 64 console
pages with descriptor attributes `0x0060000000000607`: AF, outer shareable,
Normal NC, PXN/UXN. Neighboring reserved pages are not mapped. The existing
owned-memory mappings and both TTBR values remain unchanged. Table writes occur
with M/C off; DSB SY / TLBI / DSB SY / ISB precede the original SCTLR write.
Console writes after MMU use the explicit NC mapping, so no data-cache clean
of that console is required. This mapping still needs hardware observation.

MAP_FAILURE codes: 1=TCR, 2=MAIR, 3=TTBR roots, 4=occupied L1 slot,
5=nonzero spare pages, 6=spare page already referenced. No page-table changes
occur on a refused guard. Failed logging halts without making a reset call.
The original platform's external reset behavior remains outside this probe.

## Host verification and hardware sequence

See `research/v23p9-kernel-trace.json` and the firmware validation metadata for
the final build identity, exact check counts and device-operation status.
Instruction tests mock system-register/SMC behavior; software page-table walking
does not emulate real MMU/TLB/cache silicon. Linked reader tests mock UEFI input,
display and SMC while executing its record code and gates. Keep these separate
from future hardware evidence.

For each variant, boot_b only:

1. Open the reader first; photograph the complete frozen ENTRY page.
2. If NOW is INVALID, stop and preserve the page. Do not launch the kernel;
   P9 has no initialization action. Use the preserved P8 recovery procedure as
   a separately recorded path if needed.
3. Query PSCI. ARM once and photograph THIS RUN ID/CRC and ARM readback status.
4. Return to menu and select the matching kernel. Record the entire cycle.
5. After an observed reset, open the reader first and photograph the full page
   before Query/ARM. Match the armed ID and per-checkpoint CRC/values.
6. If there is no reset, save the last display and elapsed time before deciding
   on recovery. A forced shutdown/ZUI/Recovery detour is not equivalent to the
   immediate reset-retention path.

Finish E before M. Do not mark MMU progress from a restart or an ARM-only record.

## M menu launch

V23P9M was flashed only to boot_b after exact image SHA256 and fastbootd product/unlock/slot-count/partition-size gates. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Launch targets the untimed menu only; recording readiness for the manual kernel trial was not newly confirmed. Initial M menu identity, reader ENTRY, Query/ARM and kernel/MMU checkpoints await device observations. The observed P9E entry-record retention prerequisite remains preserved.

Next: At the P9M menu open P9 reader FIRST and photograph the complete frozen ENTRY before Query/ARM. If NOW is VALID, prepare recording, then Query once and ARM once; photograph THIS RUN ID/CRC and ARM readback status before launching the M kernel. Stop on INVALID and preserve the page.

## M initial reader observation

Initial reader photo after the P9M fastboot launch confirms common J716F V23P9 KERNEL TRACE r1 reader, frozen ENTRY/NOW DBGC=0x43474244, next=size=0x2bf58, ENTRY whole-console CRC=0xbdb07c93 and NOW VALID. Logical count=0; raw count=6 with old P8 ID=1/phase=3/CRC=0xe13f0c63; all six frozen P9 checkpoint rows have zero logical/raw hits. THIS RUN ID=2/query=0/armed=0 at EL1, read-only status. The earlier E ARM/ENTRY chain is not present in this snapshot after the intervening fastboot path; the cause or timing of those changes is not isolated, and this does not alter the separately observed E controlled-reboot result. No M kernel launch or MMU progress is observed.

Next: In this reader invocation prepare recording, select Query PSCI + record (NO RESET), then ARM next kernel attempt once after successful Query. Photograph the full ARM page including NOW VALID, THIS RUN ID/CRC and ARM readback OK before manual M kernel launch. Stop and preserve the page if Query/ARM fails or the header becomes INVALID.

## M Query and ARM observations

P9M Query/ARM video and photo show a successful same-invocation preparation, with frozen ENTRY unchanged. At video frame 90, Query OK: NOW VALID next=size=0x2c058, logical/raw counts=4/10, ID=2, query=1/armed=0, CRC=0xbbfaa9d1, EL1, PSCI_VERSION=0x00010001 and SYSTEM_RESET FEATURES=0. ARM photo: NOW VALID next=size=0x2c098, logical/raw counts=5/11, ID=2, query=1/armed=1, CRC=0xc2c22d94 and ARM readback OK. The phase-8/phase-9 CRCs reconstructed from the displayed session/value/prior headers match. All frozen P9 rows remain zero because they describe the earlier reader-entry snapshot. The approximately 11.065-second video ends on the armed reader; it does not show M kernel launch or reset. Match this M attempt by both ID=2 and ARM CRC=C2C22D94, distinct from E ID=2/CRC=47CCACB5.

Next: Continue recording, Return to menu, then select V23P9M: entry / pre-MMU / post-MMU; terminal SYSTEM_RESET. Do not Query/ARM again before launch. After an observed reboot open reader FIRST and photograph the complete frozen ENTRY before Query/ARM; match ARM ID=2/CRC=C2C22D94 and the same-session ENTRY/PRE_MMU/POST_MMU or MAP_FAILURE. If no reboot, preserve the video and last display before forced shutdown or other recovery.

## M launch display and reported reboot

P9M launch photo confirms menu identity J716F V23P9M KERNEL TRACE r1 and the M entry/pre-MMU/post-MMU kernel item, with stage DARWIN BOOT, a green top band, and visible DARWINBOOT text: inputs validated; exiting boot services. It is a launch/handoff display, not a reader checkpoint. User subsequently reports an automatic reboot. No video of this launch/reboot or post-reboot reader snapshot has been supplied yet; entry/PRE/POST/MAP_FAILURE record state and actual reset source remain unknown. Do not infer MMU progress or a specific stopping PC from the still image or reboot alone.

Next: After the reported automatic reboot, open P9 reader first and photograph the full frozen ENTRY page before Query/ARM. Match this M attempt by ARM ID=2/CRC=C2C22D94 and inspect same-session KERNEL_ENTRY, PRE_MMU, POST_MMU, MAP_FAILURE and RESET_RETURN. Preserve any available complete launch/reboot video.

## Unarmed second M launch

User clarifies that after the first user-reported automatic M reboot, the M kernel was launched a second time directly, without a fresh Query/ARM or a supplied first post-reboot reader capture. User reports this second launch remains stopped; the supplied photo shows DARWINBOOT inputs validated; exiting boot services. Source stage1 requires an immediately preceding valid phase-9 ARM cookie, and branches to the WFE halt if append validation fails. If the first launch consumed ARM by appending ENTRY or later records, the unarmed second launch would be rejected; this is a source-supported explanation, not a confirmed device stopping PC. Neither first-run MMU progress nor second-run rejection is established without retained reader records. The second launch must not be treated as a freshly armed M trial.

Next: Preserve the stalled-screen photo and available video. Recover to the P9 menu with one hardware restart if necessary, documenting that extra recovery path; open reader FIRST and photograph full frozen ENTRY before Query/ARM or another kernel selection. Treat any recovered records as a later recovery snapshot, not the missing immediate first-reboot capture. For each subsequent trial: reader snapshot, fresh Query/ARM, one kernel launch, then reader snapshot before any repeat.

## M recovery reader: matching entry, later checkpoints absent

M recovery reader photo shows frozen ENTRY and NOW VALID: DBGC=0x43474244, next=size=0x2c0d8, whole-console CRC=0xc0749332, logical/raw counts=6/12, latest ID=2/phase=10/CRC=0xdc7aa441. Frozen ARM has one logical and one raw hit, CRC=0xc2c22d94 and cookie=0x50394b45524e454c, exactly matching the M prelaunch baseline. KERNEL_ENTRY has one logical and one raw hit, CRC=0xdc7aa441, SCTLR_EL1=0xc00808 (M=0); reconstructing the record with prior header 0x2c098 reproduces that CRC. PRE_MMU, POST_MMU, MAP_FAILURE and RESET_RETURN all have zero hits. New reader invocation ID=3/query=0/armed=0 with read-only status confirms no new Query/ARM in the snapshot. This recovery capture follows the user-reported first automatic reboot, an unarmed second launch that stalled, and the requested hardware recovery; it is not the missing immediate first-reboot capture, and recovery method/timing is not independently captured. It establishes a surviving M-session entry record and consumed ARM, with no retained later checkpoints. Source requires ARM immediately before entry: the now-visible consumed chain supports second-launch gate rejection, but does not prove the exact PC of that launch. The missing PRE record spans original firstcpu code, pre-hook stack handling, mapping checks/setup/barriers and PRE append; it does not isolate an SCTLR/MMU failure or a reset source.

Next: Preserve this recovery snapshot. Repeat the existing P9M once with a clean observed sequence: current reader invocation ID=3, Query then ARM once, photograph actual ID/CRC and valid header, launch M exactly once, record the complete cycle, then open reader FIRST and photograph frozen ENTRY before any Query/ARM or second kernel selection. If it stalls, preserve last screen/video before further recovery. If ENTRY-only repeats in the immediate reader capture, add finer prefix and pre-hook-before-mapping checkpoints; do not change SCTLR or disable the legacy BCM store based on the present snapshot.

## Fresh M ID=3 photo sequence

Fresh M session ID=3 photo sequence supplies ARM preparation, M handoff display and a new read-only reader invocation. ARM photo: NOW VALID next=size=0x2c218, logical/raw counts=11/17, ID=3/query=1/armed=1, CRC=0x671d1670, PSCI_VERSION=0x00010001, FEATURES=0. Frozen rows still describe prior ID=2, as designed. M launch photo identifies entry/pre-MMU/post-MMU terminal SYSTEM_RESET and shows the DarwinBoot handoff prompt. Subsequent reader photo: ENTRY/NOW VALID next=size=0x2c258, whole-console CRC=0x8b348f61, logical/raw counts=12/18, latest ID=3/phase=10/CRC=0xd1afe339. Frozen ID=3 ARM and KERNEL_ENTRY each have one logical/raw hit; ARM CRC=0x671d1670 matches prelaunch, ENTRY CRC=0xd1afe339 and SCTLR_EL1=0xc00808 (M=0). Both displayed CRCs reconstruct from the record format and prior headers. PRE_MMU, POST_MMU, MAP_FAILURE and RESET_RETURN again have no hits. New invocation ID=4/query=0/armed=0 confirms a fresh read-only snapshot. Photos were supplied in response to the requested single-launch/immediate-reader sequence; they independently establish the fresh chain and ENTRY-only repeat, while continuous reboot timing, reset source and absence of all intervening operations are not video-verified. Further identical M retries are not required before adding finer probes. Missing PRE spans original firstcpu, pre stack conversion, map_console and PRE append, not a demonstrated MMU-enable failure.

Next: Prepare terminal prefix controls V23P10A/B around the original BCM STR and P10C at the original pre-MMU site before console mapping. Reuse the proven ARM/ENTRY framing and reader, with phase-11 terminal pre-MMU record then direct SYSTEM_RESET; document each hook and the original code it executes. Test A first on boot_b after build/audit and fresh physical readiness; do not infer the legacy store is causal from P9 alone.
