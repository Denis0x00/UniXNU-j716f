# Historical progress narrative through V23P16

Archived on 2026-10-26. Entries describe their own point in time; later
evidence supersedes earlier pending/next-action statements. Current status
is on the repository home page and in the P16 progress assessment.

# UniXNU-j716f

Lenovo Xiaoxin Pad Pro 2021 (`TB-J716F`, board name `j716f`, Qualcomm
SM8250/Kona) 的 UniXNU 设备仓库。当前仍是 research / bring-up port，不能宣称
已经启动 iPadOS、iOS 或完整真实 XNU。

最新结果：[V23P8 日志有效性与复位保留诊断](docs/V23P8_RETENTION.md)。
独立 UEFI 诊断已完成一轮受控 SYSTEM_RESET：重启后日志头有效，
逻辑/原始扫描均匹配复位前的 PRE_RESET ID=1、phase=3、CRC=E13F0C63。
[设备记录](research/v23p8-retention.json)保存具体证据及限制；此结果不代表 XNU/MMU 进展。
下一步审计并接入内核入口及 MMU 前后记录。构建头文件修复已独立纳入补丁 0004。

最新准备：[V23P9 内核入口与 MMU 直接记录](docs/V23P9_KERNEL_TRACE.md)。
E 先验证入口记录与复位；M 再验证入口、MMU 前后记录。主机检查通过，
真机状态以 [P9 记录](research/v23p9-kernel-trace.json) 为准；只使用 boot_b，先 E 后 M。

## 迁移状态

仓库已按 UniXNU 格式 2 组织：

- `board.toml` 固定 `ipados-17.7.11-21H461` 候选、`j716f_source_defconfig`、DTS
  入口、payload 和 Android boot image 参数。
- `dts/j716f.dts` 与 `socs/sm8250/` 保存从 ZUI DTB0/DTBO8 和 recovery FDT
  交叉核对的 CPU0、GIC、计时器和主连续 DRAM 描述。
- `porting.toml`、`AGENTS.md` 和记录工作流覆盖配置、固件、驱动适配、组装与诊断。
- `socs/sm8250` 是临时的 board-local evidence snapshot。公开 common scaffold 已存在，
  但尚未完成本移植所需的审查和固定依赖；当前不把它当作已验证的 SoC 支持。

## 当前验证范围

截至 2026-10-25：

- 诊断镜像通过 `boot_b` 进入 UniXNU 菜单；GOP、音量键和电源键已有真机观察。
- V14 验证一个 256 MiB DarwinBoot 窗口，7/7 检查通过；两轮读写累计验证 512 MiB。
- V16 观察到 ExitBootServices 与 XNU 早期入口探针；V17 记录 post-MMU 复位检查点。
- V18 改为 WFE 停点后仍复位，具体来源尚未确定。
- V19/V20 增加复位诊断；V21 已构建、校验、刷入 `boot_b` 并进入放大菜单，
  照片确认 `PMIC RESET: 0x02000001`。这是当前启动的历史基线，尚不能据此归因复位。
- V21 录像确认入口三色条及 Lenovo Logo 再现；PMIC 变为 `0x02000041`，复位来源未知。
- V22A／B 各一轮录像，品红至黑屏为 20.245／19.730 秒，未观察到目标 1／3 秒响应；B 最终回到 A 槽 ZUI。
- V23P/C/V/M 已构建并完成主机审计和 crypto reseal。P 已刷入 boot_b 并录像，
  仅见诊断首行，约17.1秒后黑屏并重启回菜单；PSCI 是否执行仍未知。C/V/M 未测。
- 持续 MMU-on、原生平台驱动、物理 UFS/root、trust cache、userdata 和用户态均未验证，未启动 iPadOS。

最新汇总：[V23 独立诊断](docs/V23_INDEPENDENT_PROBES.md)，
[V23 验证摘要](research/v23-independent-probes.json)，[V22 MMU 后检查点](docs/V22_POST_MMU_CHECKPOINT.md)，
[V22 元数据](research/v22-checkpoint-preparation.json)，
[V21 复位诊断](docs/V21_RESET_DIAGNOSTICS.md)，
[公开验证元数据](research/v21-reset-diagnostics.json)，
[配套固件仓库](https://github.com/Denis0x00/uefi-j716f)。

`board.toml` 的 payload 选项仍保留 V17 定时复位方案；它不是 V21/V22/V23 诊断镜像的完整
配方。V21 使用独立 UEFI 诊断配置，内嵌负载与 V20 相同，具体来源与边界见最新汇总。

## 构建与检查

从最新 UniXNU checkout 执行：

```sh
./unixnu defconfig j716f_source_defconfig -D ../UniXNU-j716f
./unixnu fetch -D ../UniXNU-j716f
./unixnu status --json -D ../UniXNU-j716f
./unixnu tree -D ../UniXNU-j716f
python3 scripts/porting-record.py check --device ../UniXNU-j716f
```

DTS 独立检查：

```sh
dtc -I dts -O dtb -i ../UniXNU-j716f/socs/sm8250/dts \
  -o /tmp/j716f.dtb ../UniXNU-j716f/dts/j716f.dts
```

私有路径放在 `local.toml`（由 `local.toml.example` 复制），不要提交厂商二进制、IPSW、
原始 DTB、kernelcache、trust cache、Data seed、payload、照片、日志或密钥。

## 真机边界

当前诊断测试只允许 `boot_b`；不要写 `boot_a`、vbmeta、userdata 或其他分区。
TzDxe 仍不在本轮范围内。

最新细分测试：[V23P2 方块检查点](docs/V23P2_PREFIX_CHECKPOINTS.md)。真机仅亮1/2格，优先排查首行DSB边界；PSCI仍未确认。

最新已构建对照：[V23P3A/B](docs/V23P3_SINGLE_FACTOR.md)。A 仅跳过遗留 BCM 写入，B 仅将标题 DSB 改为 DMB；实际镜像审计和十项受控模拟通过，真机待测，先测 A。

V23P3A 已经通过 fastbootd 仅刷入 boot_b，激活 B 并发送重启；录像和实际内核进度待复核。P3B 尚未刷入。

V23P3B was flashed only to boot_b via fastbootd, B activated and reboot command succeeded. User report for P3A was brief screen then reboot; no checkpoint or PSCI result established. P3B video is pending.

V23P3A 视频已确认菜单身份；约1.7秒后画面变黑，未见诊断方块或EL/SCTLR，PSCI与复位源仍未知。

V23P3B 视频确认标题 mode 5 和方块 1/2；方块 3、EL/SCTLR 未见。约20.9秒后画面消失，随后 Lenovo/Android 启动并回到 UniXNU 菜单。标题 DMB 对照未显示出越过第2格的硬件效果，PSCI/复位源仍未知。

最新诊断：V23P4 遗留 MMIO 前后包络（docs/V23P4_EARLY_MMIO_ENVELOPE.md）。主机审计和受控模拟通过，实机待测；只考虑 boot_b。

最新诊断：V23P4 r1 遗留 MMIO 前后包络（docs/V23P4_EARLY_MMIO_ENVELOPE.md）。主机审计和受控模拟通过，实机待测；只考虑 boot_b。

V23P4 r1 was flashed only to boot_b through fastbootd; B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Menu identity, marker visibility, PSCI and reset observations await video. Retry-count variable is unsupported.

V23P4 r1 video confirms both early MMIO markers and title mode 5 with prefix squares 1/2; square 3 and EL/SCTLR are not observed. Original STR was passed, but external transaction completion is unproven. Diagnostic display disappears about 15.9s after entry, then Lenovo/Android splash and r1 menu return; PMIC 0x02000001 -> 0x02000041, reset cause unknown.

Next-build source preference: UniXNU menu scale changed from 4 to 3 after r1 video; not compiled or flashed. Kernel diagnostic font unchanged. Original r1 library sources were privately snapshotted and match its remote-build hashes; the next build must explicitly audit the changed menu module.

V23P5A/B matched controls passed host image, source/HMAC, pair-delta and emulator checks. A kernel equals P4 r1; B differs only in relocated STR-to-NOP and HMAC. Both use compiled menu scale 3, identical non-payload modules, markers and title DSB. No P5 hardware trial or device write. Test A then B on boot_b. See docs/V23P5_MATCHED_MMIO_CONTROL.md.

V23P5A was flashed only to boot_b in fastbootd; B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. A menu identity, marker progression and reset observations await video. P5B remains unflashed. No PSCI or MMU-on result is claimed.

V23P5A video confirms the compact A menu, title mode 5, prefix squares 1/2 and both early MMIO markers, reproducing P4 r1 visible progress. No square 3, EL/SCTLR or PSCI evidence. Display disappears about 17.47s after entry, followed by Lenovo/Android splash and return to A menu; PMIC 0x02000001 -> 0x02000041. Reset cause and external MMIO completion remain unknown. P5B is the next boot_b control and remains unflashed.

V23P5B was flashed only to boot_b in fastbootd; B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Its menu identity and diagnostic progress await video. Reviewed P5A remains the visible control; no P5B functional, PSCI or MMU-on result is claimed.

V23P5B video confirms the compact B menu, both lower envelope markers and partial title in frame 56; the diagnostic is captured in only one camera frame. Top prefix 1 completion, prefix 2/3, EL/SCTLR and A0/pre-SMC are not established. Entry band to display loss is about 0.067s (camera observation), followed by Lenovo/Android splash and return to B menu; PMIC 0x02000001 -> 0x02000041. Rapid intentional PSCI reset is a hypothesis, not a verified result. Next proposed control retains P5B and replaces only its SMC with an in-place branch, preserving the preceding DSB; not yet built. This checkpoint supersedes earlier pending/unflashed notes; no new device operations or push occurred during video review.

Logging reference checkpoint: Reviewed upstream logging/automatic-test guide at 16e592abee8a550dec8154ed1f38b5ebc4f16213 and same-SoC umi at 3eb72578597d2ca16471b172d45eddc096a34a04. Prioritize board-qualified retained RAM trace plus separate firmware QR logging. Existing J716F Recovery DT/dmesg show overlapping boot_log 0xb0000000+0x80000 and pstore 0xb0000000+0x100000; console file length 262132 does not establish its physical subrange. Do not copy umi 4MiB/2MiB layout or sheng disk/reset constants. Verify layout/format and minimal reset retention before attaching XNU records. Full unattended logging requires a validated reader and return-to-fastboot cycle. Read-only source/evidence review only; no new device test or logging implementation. See docs/LOGGING_AND_SM8250_REFERENCE_REVIEW.md.

## V23P6 RAM 日志保留实验

已完成独立测试镜像的主机验证，尚未刷入。下一步先验证 console 标记能否跨重启被读回，再接入 XNU。见 [V23P6 流程](docs/V23P6_RAMLOG_RETENTION.md)。

V23P6 RAMLOG r1 was flashed only to boot_b through fastbootd with the audited image; B activation, current-slot b and slot-unbootable:b=no were verified, and reboot command succeeded. First live header, marker append, warm-reset retention and Recovery readback await device observation. No other partition write or repository push occurred.

V23P6 first video/photo confirms the correct menu and read-only app: console DBGC 0x43474244, next=size=0x37bd9, capacity=0x3fff4, VALID, marker counts 0 and no append. The inferred console header is now device-readable and consistent, with 33819 bytes of unused capacity. No append, restart retention, Recovery readback or XNU progress is established. See research/v23p6-first-header.json.

V23P6 second video verifies manual RAM marker append and same-run readback: next=size 0x37bd9 -> 0x37c09 (+48 bytes), now=1, appended_this_run=1, APPEND READBACK OK. At about 15s the app reports Warm reset returned; no actual reboot is observed. At about 28s the app exits to parent EFI return diagnosis with Success/code 0, which is normal app return and not reset success. Cross-reset retention and Recovery readback remain unverified. Root cause of the returned firmware reset call is unknown. First-header transcription corrected to 0x37bd9 (33819 free bytes). See research/v23p6-append-reset-return.json.

Latest prepared experiment: [V23P7 RESET r1](docs/V23P7_RESET_DIAGNOSTICS.md), host verified and unflashed. First trial is manual PSCI queries only; P6 append/readback is proven but reset/retention remains unverified. Still early kernel entry bring-up, no platform expert or userspace result.

V23P7 RESET r1 (uncommitted source additions on the recorded repository base; exact source/image hashes in the P7 manifest) was flashed only to boot_b through fastbootd. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Inventory/PSCI-query observations await video. No reset trial, retention result or new XNU checkpoint is established.

V23P7 photos verify EL1, installed reset driver GUID/RVA/prefix match, PSCI_VERSION=0x00010001 (1.1) and FEATURES=0 for SYSTEM_RESET and RESET2 A64. Valid console next=size grows 0x2923f -> 0x2958a (+843 bytes); P7 entry=0/now=6 and Queries complete=1. Query returns and app-displayed same-run records are established. No actual P7 reset, retention, Recovery readback or new XNU checkpoint. Next manual trial uses Direct RESET2 only; preserve initial post-reboot inventory or returned x0..x3. Uninitialized old-driver inputs remain an unproven cause.

V23P7 RESET2 video/photo confirms two direct 0xc4000012 type=0/cookie=0 calls return x0=0xfffffffffffffffe (INVALID_PARAMS -2), x1..x3=0. FEATURES remains 0; valid console next=size 0x2958a -> 0x296a3 -> 0x297bc and P7 now 6 -> 8 -> 10. No reboot is observed in the 10.50s video; retention and Recovery readback remain untested. Old Warm falls back only on FEATURES failure, not on returned RESET2 error; historical P6 SMC returns remain uncaptured. Next manual test is existing Direct SYSTEM_RESET only. No new device commands, flash or source changes during this media review.

V23P10A/B/C terminal prefix controls are host verified and unflashed. See [plan and test procedure](docs/V23P10_PREFIX_CONTROLS_PLAN.md), [text evidence](research/v23p10-prefix-controls.json). Test A first; phase11 is the named prefix stop, with MMU still off.

Next firmware compilation: add Reboot and Power off to the XUI main menu; see [request and validation scope](docs/NEXT_MENU_BUILD.md).

Latest P10A operation: boot_b flash, B active/bootable verification and reboot command succeeded. Initial reader and manual kernel trial remain pending; B/C unflashed.

Latest P10A hardware result: before-BCM terminal phase11 and matching ARM/ENTRY chain survived one observed reboot. Next B, then C if B passes; no MMU-on result. See V23P10 research record.

## V23P11 current diagnostic candidates

A/B/D built and host verified, unflashed; P10B has raw-only terminal progress and C stays gated. See [P11 procedure](docs/V23P11_COMMIT_PUBLICATION.md) and [host evidence](research/v23p11-commit-publication.json). Includes Reboot/Power off; new power-app device behavior is pending. A first, boot_b only; no MMU-on result.

## P11A device operation; kernel result pending

V23P11A was flashed only to boot_b after exact SHA256, fastbootd product/unlock/slot-count/partition-size gates. B active, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Only the untimed menu was requested; menu identity, initial reader, Query/ARM, kernel launch and retained P11 stages remain unobserved. P11B/D remain unflashed; P10C gated. Image SHA256=e19591e47ae931348d1c1e5a8f01ab9a2e397a2108c21cb0a9b75af187bdbea5.
Next: At P11A menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or power actions. Verify menu identity and log validity before fresh Query/ARM and one A launch. B/D unflashed; P10C gated.

Latest P11A device result: matching ARM/ENTRY and logical/raw P11S stage5 retained across one observed reboot. A before-BCM control passes; next P11B. No MMU-on result; see the current P11 research record.

## P11B device operation; observation pending

V23P11B was flashed only to boot_b after observed P11A stage5/ARM/CRC/reset-retention prerequisite and exact image SHA256, fastbootd product/unlock/slot-count/partition-size gates. B activation, current-slot b and slot-unbootable:b=no verified; reboot command succeeded. User confirmed fastboot/recording readiness. Untimed menu only; B menu identity, initial reader, fresh Query/ARM and kernel/stage observations pending. Original BCM STR is present in B, but its execution has not yet been observed in this trial. A result preserved, D/P10C gated. B image SHA256=94c97bacfef84deab3bceaacfee5f1f95b20f4e74a57c01f9beb5bbabc64a40c.
Next: At the P11B menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or menu power actions. Confirm B menu identity and current log validity, then fresh B Query/ARM ID/CRC before one kernel launch. Historical P11A records must remain distinct. D and P10C remain gated.

Latest P11 comparison: A retained published stage5; B retained fresh raw-only stage1 while logical header stayed at ENTRY. P11D first DSB-to-DMB control is next; exact PC, external BCM completion and reset source remain unverified. See P11 research record.

## P11D device operation; observation pending

V23P11D was flashed only to boot_b after reviewed B fresh raw-stage1/ARM/CRC/observed-reboot prerequisite and image SHA256, fastbootd product/unlock/slot-count/partition-size gates. B/D assembled probe difference rechecked: only terminal first DSB SY changed to DMB SY; subsequent barriers retained. B activation, current-slot b and slot-unbootable:b=no verified; reboot command succeeded. User confirmed fastboot/testing readiness following recording request. Untimed menu only; D menu identity, reader, fresh ARM and kernel results pending. A/B observations preserved; P10C gated. D image SHA256=0a0b647be892107591787f05b4709286b20ab49c25125822e81a306bc65c73af.
Next: At P11D menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or power actions. Confirm D menu identity and current log validity, then fresh Query/ARM ID/CRC before one D kernel launch. D differs from B only by terminal first DSB SY to DMB SY; later barriers remain. P10C remains gated.

Latest P11 result: A retained published stage5; B and D each retained matching raw-only stage1. First DSB-to-DMB substitution showed no retained-progress improvement in these trials. Next: independent non-overwriting progress witnesses; exact PC, external BCM completion, reset source and MMU-on remain unverified. See P11 research record.

## V23P12 independent-slot candidates

V23P12A/B built and host verified, unflashed. Six independent pre-staged CRC-bound records use one-byte commits; ENTRY high32 binds ARM CRC and low32 is SCTLR. A skips BCM and B preserves captured original address/value, with exactly one STR/NOP probe delta and identical reader/ADT/nonpayload firmware. 58 actual-assembly and22 linked-reader mocked cases passed; final image/HMAC/source audits passed. Byte-identical P11 power app reuses22 host cases. No P12 hardware/retention/MMU-on result; A first on boot_b only. See [P12 procedure](docs/V23P12_INDEPENDENT_SLOTS.md).

## P12 r2 full-console session handling

P12 r2 A/B built and host verified, unflashed. New reader requires explicit double confirmation before discarding a VALID saturated/exhausted console logical history; frozen entry remains, payload not bulk-cleared, INIT records old next/size, failure locks Query/ARM. 45 actual-linked reader cases passed including full-session-to-both-unchanged-kernels-to-frozen-reader flow, changed-console cancellation, failure and stale-ID gates. A/B kernel/ADT/probes equal r1 byte for byte; 58 prior assembled cases reused after identity check. Power app equals P11, 22 cases reused. Final FD/FAT/EFI/source/HMAC/nonpayload/boot-image checks passed. No r2 hardware, retention or MMU-on result; A first on boot_b only. See [procedure](docs/V23P12R2_SESSION_ROLLOVER.md).

## V23P13 MMU continuation

P13P/M terminal bridge built and host verified: complete BCM routing block excluded, original continuation restored to guarded console mapping and pre/post SCTLR hooks. Six independent P13 ARM-bound commits plus actual terminal SCTLR record; 63 actual-kernel and 34 linked-reader cases passed with privileged/MMU/UEFI/SMC mocks. Final image/HMAC/source/nonpayload/SEC audits passed. P13M boot_b trial now retains a matching ID1 ARM, all six logical/raw witnesses and POST_MMU SCTLR0x3454593d(M1); all nine record CRCs reconstruct. Short hardware MMU-on execution is established. Reset source, sustained kernel continuation and timer/GIC remain unverified. Native timer source profile resolves but has not been compiled; ArmCpu integration remains pending. BCM B is optional; test P13M mainline on boot_b, P13P only if needed.

See [P13 procedure](docs/V23P13_MMU_CONTINUATION.md).

## V23P14 original early initialization

P14M postlaunch reader photo confirms ENTRY/NOW VALID next=size0x2e5a6, consoleCRC5D1C75BD, logical13/raw26, latest ID2/phase12/76EE0045. Fresh ARM6304806B matches prelaunch; ENTRY FCBEA27A value6304806B00C00808(M0). All six phases16..21 are L1/R1 with bound value503134536304806B and CRCs66FE13FA,A1D55ACD,BAF528F7,91424ACD,8CB5CC83,C0FB7ACD. Terminal76EE0045 records actual SCTLR3454593D(M1). All nine CRCs reconstruct exactly; header growth512 matches ENTRY+six slots+terminal. New reader ID3/query0/armed0, no MAP_FAILURE or RESET_RETURN. One original early-init path reaches cpu_data_init return before PE_init_platform, with records present after user-reported automatic reboot. No continuous reset video reviewed; reset source, sustained initialization, timer/GIC and userspace remain unverified.

Next: instrument PE_init_platform entry/return and early dependencies with fresh version-specific ARM binding.

See [P14 procedure](docs/V23P14_EARLY_INIT.md).

## V23P15 platform initialization

P15M continuous13.932s video confirms selected P15M kernel, DARWIN BOOT/green handoff, screen loss, Lenovo/Android splash, P15M menu return and read-only reader. Final video reader matches photo: ENTRY/NOW VALID next=size0x328a4, consoleCRCF498612C, logical13/raw13, latest ID1/phase12/F286B6BF. Fresh ARM B9A219EB matches prelaunch; ENTRY F2FD8F05 valueB9A219EB00C00808(M0). All six phases16..21 are L1/R1 with bound value50313553B9A219EB and CRCs99CE21F7,D58097B9,CEA0E583,E51787B9,F8E001F7,B6E7E7E2. Terminal F286B6BF records actual SCTLR3454593D(M1). All nine CRCs reconstruct exactly; growth512 matches ENTRY+six slots+terminal. New reader ID2/query0/armed0, no MAP_FAILURE/RESET_RETURN. Original PE_init_platform(0, bootArgs) entered and returned before next arm_init call, with records retained over one observed reboot. PMIC02000001->02000041. Reset source, sustained continuation, interrupt/GIC/native timer and userspace remain unverified.

Next: audit subsequent timer/WFE/miscellaneous setup before new original-continuation checkpoints.

See [P15 procedure](docs/V23P15_PLATFORM_INIT.md).

## V23P16 timer/WFE initialization

P16M timer/WFE/misc terminal diagnostic built and host verified: 48 actual-kernel and 31 linked-reader mocked cases passed. Original PE/timer/WFE/misc BL/LR and WFE bodies preserved; independent bound phases18..21 cover timer stub return, WFE configure return, WFE init return and misc completion. Terminal phase12 records actual CNTKCTL, not SCTLR, after M1 guard. Actual parser/calculation executes with pinned ADT in synthetic boot arguments (19.2MHz/default threshold4). Interrupted configure/init, original arithmetic trap/panic, late corruption, consumed ARM and mapping/session gates checked. Final source/FD/FAT/EFI/HMAC/nonpayload/SEC/boot-image audits passed. No P16 hardware result or device write; timer interrupt/WFE wait and ArmCpu/GIC remain separate. boot_b only; reader FIRST, fresh Query/ARM each launch.

See [P16 procedure](docs/V23P16_TIMER_WFE.md).

### P16M device operation

P16M flashed only to boot_b after final image/source hash and fastbootd product/unlock/slot/partition gates. B active and bootable verified; reboot command succeeded. Recording readiness confirmed. Menu appearance, initial reader, fresh Query/ARM and kernel observations remain pending. Phase12 is CNTKCTL, not SCTLR. Open P16 reader FIRST and photograph the full initial page before Query/ARM/session action or kernel launch.

### P16M initial reader

P16 initial reader photo confirms WFE TRACE r1 at EL1: ENTRY/NOW VALID next=size0x28761, consoleCRCECF994E2, logical0/raw13. Frozen ID1 ARM B9A219EB cookie5031354B45524E4C and ENTRY F2FD8F05 valueB9A219EB00C00808 match old P15; raw latest phase12 F286B6BF is not a P16 CNTKCTL terminal. All six P16 slot rows and four terminal rows are zero. THIS RUN ID2/query0/armed0. Unused tail96403 bytes exceeds Query896-byte budget; no new session needed. No P16 launch or controlled reset retention result; parent menu not pictured.

### P16M fresh ARM

P16 fresh Query/ARM photo confirms NOW VALID next=size0x288a1, logical5/raw18, THIS RUN ID2/query1/armed1/CRC93A96F6F, EL1, PSCI1.1 and SYSTEM_RESET FEATURES0. ARM + six invalid templates OK is displayed. Growth320 bytes matches four Query records plus ARM; phase9 CRC reconstructs exactly with P16 cookie5031364B45524E4C and prior0x28861. Frozen initial0x28761/ECF994E2 and old P15 rows remain unchanged. No new-session action or P16 launch observed. One fresh ID2 attempt is ready; predicted complete header0x28aa1 is derived, not observed.

### P16M hardware WFE/misc result

P16M continuous11.998s video confirms selected P16M, DARWIN BOOT/green handoff, screen loss, Lenovo/Android splash, P16M menu return and read-only reader matching final photo. ENTRY/NOW VALID next=size0x28aa1, consoleCRCB9A3B4BA, logical13/raw26, latest ID2/phase12/A684F65B. Fresh ARM93A96F6F matches prelaunch; ENTRY5AF6E0B5 value93A96F6F00C00808(M0). All six phases16..21 are L1/R1 with bound value5031365393A96F6F and CRCsAA287B4B,E666CD05,FD46BF3F,D6F1DD05,CB065B4B,48BB45ED. TerminalA684F65B records actual CNTKCTL_EL1=0x4f after M1 guard. All nine CRCs reconstruct exactly; growth512 matches ENTRY+six slots+terminal. New reader ID3/query0/armed0, no MAP_FAILURE/RESET_RETURN. Original WFE configuration/init and misc returns completed and records retained over one observed reboot. PMIC02000001->02000041. Timer IRQs, real WFE wait/wakeup, sustained initialization, ArmCpu/GIC, reset source and userspace remain unverified.

Next: Preserve this read-only result; next instrument original /chosen SecureDTLookupEntry, xnu_platform_stall property lookup and following control-flow/initialization boundary. Each new attempt requires fresh version-specific Query/ARM; do not directly relaunch consumed P16 ARM.
