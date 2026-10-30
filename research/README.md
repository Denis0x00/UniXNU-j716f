# 研究与证据索引

## 当前入口（2026-10-29）

- 最新实机突破：[`NATIVE FAST-V1..V3` 实机 100% 完成 `arm_vm_init` / `TTBR1_EL1` 切换与 `pmap_bootstrap`](native-fast-vm-done-20261029.json)，详见 [`docs/NATIVE_FAST_VM_BRINGUP_20261029.md`](../docs/NATIVE_FAST_VM_BRINGUP_20261029.md)。通过 `package/native-fast/`（3 秒冷/脏启动 ARM + 4 秒 8 帧 QR 会话导出 + 物理全区扫描）连续完成三轮 `boot_b` 实机测试：
  1. Run 1（`FAST-V1`，`eca22352`）：捕获 `0:start_begin -> 43:BOOT_MAP_ERR`，定位并修复 `TCR_EL1.T1SZ=26`（38-bit VA，256 项 L1 根表）索引掩码缺陷（[`NativeMapFixed.h`](../package/native-fast/NativeMapFixed.h)）；
  2. Run 2（`FAST-V2`，`28463fde`）：捕获 `0..3 -> 44:PERM_MAP_ERR -> 41:EL1_SYNC_FLT`，定位并修复 `TTBR1_EL1` 切换前 `_phystokv` 返回永久表 KVA 的缺陷（[`NativeVmMapFixed.h`](../package/native-fast/NativeVmMapFixed.h)）；
  3. Run 3（`FAST-V3`，`d367d4ee`）：实机恢复会话 `0x000000004a9650ec`（8/8 槽位 CRC 全过，**`Faults: none`**），完整跑通 `0:start_begin -> 1:boot_pt_map -> 2:arm_init -> 3:vm_entry -> 4:vm_perm_map -> 5:vm_ttbr_sw (TTBR1_EL1=0xd4229000) -> 6:pmap_boot (dynamic_memory_begin=0xfffffff029400000) -> 7:vm_done (first_avail=0xd4320000)`。

- 下一轮就绪镜像：[`NATIVE FAST-V4` 主机审计构建清单](native-fast-v4-build-20261029.json)（镜像 `8ac33597`，内核 `2a45d7d0`）。在 `FAST-V3` 基础上利用 `__TEXT_EXEC` 末尾 988 字节空闲区新增 21 个槽位（`33..39`、`45..49`、`54..62`），覆盖 `_arm_init` 尾声、全部 55 处 `_kernel_debug_string_early`（首次 + 槽位 47 滚动末次）、`_kernel_bootstrap`、`_load_context`、`_kernel_bootstrap_thread` / `_PE_init_iokit` 及 `_ml_panic_trap_to_debugger`。

- 前一现场：[MAINT v2 完整录像导出](native-maint-complete-20261029.json)。视频独立恢复256/256帧、262144字节；整区 SHA5583ff6e/CRC B6D19E53重算通过，落盘读回及同盘第二目录副本已核对。不需再补录这份快照。头仍INVALID_HEADER，全区无当前原生日志精确标记；没有Init令牌/原生启动结果。保持暂停，下一步审查冷启动会话建立与区域所有权，不自动修复/放宽门槛/重刷。

- 前一首帧：[MAINT v2 首帧解码与私有保存](native-maint-first-frame-20261029.json)。保留当时仅1/256帧、整区SHA/CRC未验证的事实及头字段转录更正；后续录像首帧文本逐字匹配，未改写旧记录。

- 前一现场：[完全关机后 Read v3 / 无效 DBGC 头](native-read-coldboot-20261029.json)。照片和用户冷启动说明已核对；STABLE=1 不等于格式有效或跨重启保留。MAINT-v2/ARM 会拒绝当前头，只允许原样导出；不进入 Init/ARM/Native，不重刷。尚无新原生内核运行证据。

- [sheng/pipa/umi 分层复用与已有镜像只读复核](upstream-mainline-reference-20261029.json)，[具体缺项和下一步](../docs/UPSTREAM_REFERENCE_20261029.md)。实际最终镜像、原生符号、ADT、日志身份复核通过；确认标准 initrd/AdvancedLogDxe 未集成，实际专用菜单无 Tools。没有新构建或设备命令；用户报告已开机进入 Read，待当前照片，不提升硬件等级。

- 最新实施：[MAINT v2 主机验证及 boot_b 部署](native-maintenance-v2-20261027.json)，[操作流程](../docs/NATIVE_MAINTENANCE_V2.md)。固定内核/ADT；同一应用新Export→Init，实际C/EFI验证及最终镜像反解通过。刷写/B槽确认/重启命令成功；新照片、完整导出、物理Init/ARM/XNU均未取得。旧方案/实机检查点保留。
- 最新方案：[RAW/Init v2 设计审查](native-console-v2-plan-20261027.json)与[执行方案](../docs/NATIVE_CONSOLE_V2_PLAN.md)。7组纯主机设计测试覆盖524265个合法线性/满环头状态；不等于生产实现、固件或硬件验证。现有v1未改。
- 最新实机：[Init v1 拒绝变化后的线性快照](native-init-blocked-20261027.json)。用户确认中途重启/fastboot；新头next=size0x3f56e、SHA变化，ready0/used0且未写入。旧备份保持不变；需新快照只读导出，不能复用回执或绕过策略。
- 前一分析与授权：[Android 遗留日志分类 / Init-only](native-console-analysis-init-20261027.json)。满环主要是 Keymaster 重试、电池和 fastbootd 日志；保留当时已授权 Init、尚待现场核对的检查点，不扩大为复位或启动授权。
- 最新实机：[RAW-v1 完整光学导出](native-console-complete-20261027.json)，视频独立恢复 256/256 帧、完整 262144 字节；整区 SHA256/CRC 重算及落盘读回通过，另存私有目录副本。保留该检查点当时 Init 未授权的事实。
- 前一光学门槛：[首帧验证](native-console-optical-20261027.json)，保留当时 1/256 帧的事实，不改写为完整备份；视频属于另一 transfer，未与照片混用。
- 前次原生观察：[首次菜单/只读 reader](native-reader-40458123294-20261027.json)，STABLE=1；满环形阻塞 ARM，作为历史现场完整保留。
- 历史内核边界：[P22 原始 VM 返回并封存](p22-closed-20261027.json)，六槽、九 CRC、M=1 与一次重启保留；不重复测试。
- 当前主线：[原生候选与可恢复日志](../docs/NATIVE_MAINLINE.md)；[P22 历史构建/会话](v23p22-vm-return.json)仅保留历史状态。
- 当前原生主机里程碑：[CI40458123294 → 预链接 → reader/ARM → 最终镜像反解](native-image-40458123294.json)。保留当时 host-only 报告，后续硬件观察另记；旧无日志 CI40448429664 不是当前候选。
- [最新上游与 sheng 对照](upstream-progress-review-20261027-p22.json)及[进度说明](../docs/PROGRESS_AFTER_V23P22_20261027.md)。
- [权威移植记录](../porting.toml)。下文保留各时间点的历史记录，“最新准备”等措辞仅适用于当时。
- [原生 IRQ 观测实现/主机摘要](native-irq-observer-20261027.json)：同 SM8250 pipa/umi 对照，
  保留原始 rtclock/EOI，尚未预链接或实机验证。

## 历史记录（按原时间顺序保留）

最新准备：[V23P8 retention](v23p8-retention.json)，测试步骤见
[V23P8](../docs/V23P8_RETENTION.md)。它区分无效日志头与仍可识别的载荷字节，
主机验证不代表真机保留成功。此前 Recovery 的 65 字节命名 console 采集为
缺少文件的错误文本；pstore 当时已挂载但为空。

公开仓库只保留可复核的板级结论，不放设备专属备份、照片、序列号、ADB 输出、Ubuntu 日志、完整 DTB、厂商二进制或 IPSW。

截至 2026-10-25，已确认的公开结论：

- 板级代号：`j716f`；SoC：SM8250；设备：Lenovo Xiaoxin Pad Pro 2021 / TB-J716F。
- UEFI GOP 已观察到 2560×1600、10240 字节行跨度和 32 位像素；原生显示驱动、
  刷新率控制与背光仍未完成。
- V14 的一个 256 MiB 内存窗口通过 7 项检查，两轮读写累计验证 512 MiB；不代表全部 RAM。
- V16/V17 记录早期 XNU 入口检查点；V18 WFE 后复位的来源仍未知。
- V21 录像确认入口三色条及 Lenovo Logo 再现；PMIC 变为 `0x02000041`，原因未分类。
- V22A／B 录像约20.245／19.730秒黑屏，未见目标1／3秒响应；B最终进入A槽ZUI。
- [V23 摘要](v23-independent-probes.json)：四变体主机通过；P真机仅见诊断首行后重启，PSCI尚未确认。
- 物理 UFS/root、原生平台驱动和用户态仍未验证。
- TzDxe 暂不处理。

最新纯元数据：[v22-checkpoint-preparation.json](v22-checkpoint-preparation.json)。
测试方法：[V22 检查点](../docs/V22_POST_MMU_CHECKPOINT.md)。
此前基线：[v21-reset-diagnostics.json](v21-reset-diagnostics.json)。
版本链、源码引用、主机审计与真机观察分别记录在
[V21 诊断汇总](../docs/V21_RESET_DIAGNOSTICS.md)。

完整原始测量资料保存在本地工作区，不属于公开发布内容。

- [V23P2 摘要](v23p2-prefix-checkpoints.json)：只亮1/2格，首行DSB附近为下一步排查重点。

- [V23P3A/B 单因素摘要](v23p3-single-factor.json)：两镜像构建与主机校验完成，硬件待测；先测 A，保留 P2 对照。

- [V23P4 摘要](v23p4-single-factor.json)：遗留 BCM 写入前后包络，主机通过，实机待测。

- [V23P5 对照](v23p5-matched-mmio.json)：原始 STR/NOP，相同标记与较小菜单；主机通过，实机待测。

- `v23p6-ramoops-layout.json`: matching-build Recovery allocator emulation with live parameters; physical header access and retention still unverified.

- `v23p7-reset-probe.json`: P6 actual reset-route host audit and unflashed P7 inventory/PSCI-query control; no hardware/retention claim.

- `v23p7-query-photos.json`: device inventory and PSCI 1.1 / FEATURES photo evidence; no reset/retention claim.

- `v23p7-reset2-return.json`: two direct RESET2 calls return INVALID_PARAMS (-2); no actual reboot or retention result.

- [V23P8 retention](v23p8-retention.json): one observed immediate UEFI-to-UEFI SYSTEM_RESET preserves the valid header and matching logical/raw PRE_RESET ID=1/phase=3/CRC=E13F0C63. No XNU/MMU progress is implied.

- [V23P9 kernel trace](v23p9-kernel-trace.json): E entry control then M entry/pre/post; 23 assembly and 12 linked-reader cases pass with mocked hardware. Actual flash and hardware status are recorded independently.

V23P10A/B/C terminal prefix controls are host verified and unflashed. See [v23p10-prefix-controls.json](v23p10-prefix-controls.json). Test A first; phase11 is the named prefix stop, with MMU still off.

Latest P10A hardware result: before-BCM terminal phase11 and matching ARM/ENTRY chain survived one observed reboot. Next B, then C if B passes; no MMU-on result. See V23P10 research record.

- [V23P11 publication stages](v23p11-commit-publication.json): A/B/D built, host verified and unflashed. Five CRC-valid progress values; 40 kernel + 18 reader + 22 power-app cases. P10B raw-only finding preserved; C gated.

Latest P11A device result: matching ARM/ENTRY and logical/raw P11S stage5 retained across one observed reboot. A before-BCM control passes; next P11B. No MMU-on result; see the current P11 research record.

Latest P11 comparison: A retained published stage5; B retained fresh raw-only stage1 while logical header stayed at ENTRY. P11D first DSB-to-DMB control is next; exact PC, external BCM completion and reset source remain unverified. See P11 research record.

Latest P11 result: A retained published stage5; B and D each retained matching raw-only stage1. First DSB-to-DMB substitution showed no retained-progress improvement in these trials. Next: independent non-overwriting progress witnesses; exact PC, external BCM completion, reset source and MMU-on remain unverified. See P11 research record.

- [P12 independent slots](v23p12-independent-slots.json): A/B host verified and unflashed; six distinct CRC-bound one-byte commits; A BCM-skip first, boot_b only.

- [P12 r2 session rollover](v23p12r2-session-rollover.json): explicit valid-full-log rollover; unchanged A/B kernels, new linked-reader checks.

- `v23p13-mmu-continuation.json`: P13 bridge images and host checks; native timer source preparation, with hardware MMU/timer results explicitly pending.

- [P16 timer/WFE qualification](v23p16-timer-wfe.json): original WFE configure/init and misc returns observed; all six witnesses and CNTKCTL_EL1=0x4f terminal retained over one video-observed reboot. Timer IRQ, actual WFE wait/wakeup and sustained initialization remain unverified.
- [P16 progress/upstream review](upstream-progress-review-20261026-p16.json): current upstream pins, sheng maintainer device evidence and the remaining native timer/ArmCpu/GIC integration gap. See [progress assessment](../docs/PROGRESS_AFTER_V23P16_20261026.md).

- [P17 grouped continuation](v23p17-chosen-topology.json): boot_b hardware reaches original chosen/stall/topology and boot CPU number returns, terminal SCTLR M1. All nine CRCs match fresh ARM and survive one video-observed reboot; stops before boot CPU publication/max-CPU validation.

- [P18 bootstrap/early clock](v23p18-bootstrap-clock.json): historical device result, original boot CPU validation/current-thread/early-clock returns; nine CRCs matched and records retained over one video-observed reboot.

- [P19 startup/timer/CPU/processor](v23p19-startup-init.json): bounded original four-group returns device verified; all nine CRCs match, terminal SCTLR M1 and records retained over one continuous-video-observed reboot. Next audit arm_init remainder and arm_vm_init mapping/stack lifetime.

- [P20 VM preparation](v23p20-vm-init.json): bounded original early allocation/layout and arm_vm_prot_init return device verified before TCR/TTBR activation; all nine CRCs match and retained over one video-observed reboot. Full arm_vm_init remains incomplete; next audit completed mappings and switch lifetimes.

- [P21 translation switch](v23p21-vm-switch.json): bounded original TTBR1/TTBR0 setter returns and TLBI/DSB/ISB device verified; six slots L1/R1, nine CRCs match and retained over one reboot. Full arm_vm_init return and later borrowed-console lifetime remain unqualified.

- [P22 VM return](v23p22-vm-return.json): built/host qualified, boot_b flashed; initial VALID full-ring reader ID1/query0/armed0 confirmed; explicit new session confirmed at next=size0x40, INIT CRC independently matches; fresh Query/ARM pending; static console subtree reviewed through original pmap_bootstrap/VM RET caller; P21 remains latest device result.
