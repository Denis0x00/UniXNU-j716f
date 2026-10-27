# Research notes

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

- [P21 translation switch](v23p21-vm-switch.json): built/host verified, flashed only to boot_b; reboot command succeeded, initial VALID full-ring reader ID1/query0/armed0 observed; explicit new session now confirmed at next=size0x40, ID1/query0/armed0; INIT CRC independently matched; fresh Query/ARM pending; console-only bridge and original TTBR1/TTBR0/TLB sync; TCR-aware P20 walker correction, hardware result preserved.
