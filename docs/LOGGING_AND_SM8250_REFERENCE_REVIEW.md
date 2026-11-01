# 日志与 SM8250 参考复核（2026-10-25）

## 固定源码与结论

本次通过 `git ls-remote` 核对当前 main，并读取对应本地源码：

| 来源 | 固定提交 | 用途 |
| --- | --- | --- |
| UniXNU/UniXNU | `16e592abee8a550dec8154ed1f38b5ebc4f16213` | 日志分阶段、AdvancedLogDxe、自动测试与 QEMU |
| reAAAq/UniXNU-umi | `3eb72578597d2ca16471b172d45eddc096a34a04` | 同 SM8250、已报告 Hello 的实际设备参考 |
| FFT-infra/UniXNU-sm8250-common | `6bb9b025704764a662728dce1a30557a35e082d8` | 共享 SoC 资源骨架，当前补丁 series 仅注释 |

umi README 报告八核、UFS、Data、CPU 显示和 Hello/Setup；不是 J716F 的设备验证。
它的默认 defconfig 不是完整 Hello 配方。`source-lock.json` 固定旧版 UniXNU
`7b52e7968c76453cdc18b5aa174fc368d5293b91`；不能直接将其所有补丁覆盖到新主干。

## 可以借鉴的具体实现

1. **UEFI 固件二维码日志**：UniXNU 的
   `package/UniXNUPkg/Driver/AdvancedLogDxe/AdvancedLogDxe.inf` 消费 Advanced Logger
   协议，将当前缓冲区快照经 Tools 菜单导出。需要日志收集器、DebugLib 接入、
   AdvancedLoggerAccessLib、QrEncoderLib、DSC/FDF 和 Tools 项共同配置。
   本地 Mu 源码已有日志库和二维码库，但当前 V23 DSC 尚未完成该集成，且
   `PcdUniXNUDebugLevel=0`、DebugPrintErrorLevel 仅 ERROR，需显式审核日志级别。
   二维码导出当前固件日志，不能自动恢复复位前 XNU 的执行记录。
2. **不依赖 UFS 的早期 RAM 日志**：umi 的 `scripts/umi-early-trace.inc` 与
   `patches/umi-xnu/early-entry-trace.patch` 在入口、页表、SCTLR 写入前后、
   arm_init/arm_vm_init 等位置写阶段号与值。无堆栈/分配/固件调用，但占用
   x9..x17/NZCV，须逐点审查活跃寄存器，不能插到任意指令之间。
   它增加 TTBR0 的非缓存映射，arm_vm_init 后改用 ml_io_map；J716F 必须
   独立实现地址、页表和所有权审查。其写入器还含 DSB，接入会改变屏障序列，
   必须与 P5B 的 SMC 单因素对照分开。
3. **复位后解码**：umi 的 `RamoopsSerialPortLib.c` 和
   `bringup/tools/decode_umi_xnu_trace.py` 使用 Linux ramoops 格式，并将日志
   与该次镜像哈希/符号绑定。复位后是否保留需实测。该方法值得优先于继续只靠录像。
4. **架构定时器**：umi 基线启用 ArmCpu、ArmDTPlatform、NATIVE_ARCH_TIMER；
   上游 `architectural-timer-entry.patch` 在该选项启用时绕过旧 BCM 路由。
   当前 J716F 的单条 NOP 只是定位实验，不等同这些组件已适配。
5. **复位**：umi 的 UmiResetSystemLib 有板级 PMIC/SCM/PS_HOLD 流程，工具
   将菜单诊断复位改为 warm 以尝试保留 RAM。可参考流程和错误回滚；不得直接
   复制 PMIC 写入、重启原因或用它宣称 J716F 的 PSCI 已验证。

## J716F 已有本机线索及冲突

以下仅为已有本地 Android/Recovery 采集的脱敏摘要，原始日志和 DT 不入仓库：

- Recovery DT `boot_log_region@0` 为 `0xb0000000 + 0x80000`，
  `pstore_region@b0000000` 为 `0xb0000000 + 0x100000`。
- Recovery dmesg 明确报告这两个保留区重叠，并报告 ramoops
  `attached 0x100000@0xb0000000, ecc: 0/0`。
- boot_log_drv 的 `second_log_offset=0x40000`；不能把重叠区直接当独占日志 RAM。
- Recovery iomem 仅列出 `0xb0000000..0xb007ffff` 的 persistent_ram 页；
  当前 UEFI memory map 的 Boot Log 也只保留 `0x80000`。
- 已有 pstore 列表和文件确认 `console-ramoops-0` 存在，某次长度为 262132，
  即 `0x40000 - 12`。这提示 256 KiB console，但不能仅由长度确定其物理偏移、
  各子区顺序、header 签名或重启后的保留行为。

umi 使用 4 MiB 总区、前 2 MiB console。直接照搬会越过 J716F 已知保留范围。
本次只读分析，没有写 RAM、改页表、改分区或执行设备命令。

## 推荐顺序

1. 保留 P5A/B 镜像与证据；单独做 P5B 的 SMC 原地循环对照，确认快速重启
   是否可能来自诊断自身的 SYSTEM_RESET；该版仍未制作。
2. 优先开展 J716F ramoops 可行性：核对本机内核 ram.c/ram_core.c 配置、console
   的实际物理起止与格式，以及 boot_log_drv 的读写；检查所有固件与 XNU 保留区。
   先在可控固件阶段进行最小启动 ID/标记的 warm-reset 保留实验，再加内核阶段记录。
   未解决范围和格式前不接入写入器。
3. 独立接入并验证 Advanced Logger + Tools QR；先取回一次包含 UniXNU/DarwinBoot
   交接前信息的日志。当前源码具备依赖不表示镜像已支持。
4. RAM 记录链路通过后，将同一启动的镜像哈希、启动 ID、阶段号、EL/SCTLR、
   MMU 前后和 SMC 前记录绑定，复位后只读取回，避免把旧日志当新结果。
5. 对照结论明确后，逐项迁移到源码入口探针、架构定时器和平台组件。
6. UFS 与目标日志存储布局通过后，再参考 sheng 的 BootLog/BootReset、Linux
   读取器、read-sheng-log.py、run-sheng-unattended.sh，先完整手动循环一次，
   最后接自动循环。sheng 的 userdata 尾部 32 MiB 与 PMIC 参数均不是本机配置。

主机侧图像/哈希检查与 USB 返回检测现在可继续使用。完整自动日志循环目前尚不具备。
QEMU 菜单/DarwinBoot/入口 fixture 可验证通用软件路径，不能模拟证明 J716F 的
Qualcomm MMIO、EL3、实际保留内存或复位行为。仍处于阶段 5 早期。

## 来源

- [上游日志抓取与自动测试](https://github.com/UniXNU/UniXNU/blob/16e592abee8a550dec8154ed1f38b5ebc4f16213/docs/zh/porting/log-capture.md)
- [sheng 自动测试示例](https://github.com/UniXNU/UniXNU/blob/16e592abee8a550dec8154ed1f38b5ebc4f16213/docs/zh/devices/sheng-unattended-test.md)
- [umi 设备状态](https://github.com/reAAAq/UniXNU-umi/tree/3eb72578597d2ca16471b172d45eddc096a34a04)
- [umi 入口记录](https://github.com/reAAAq/UniXNU-umi/blob/3eb72578597d2ca16471b172d45eddc096a34a04/scripts/umi-early-trace.inc)
- [umi MMU 前后接入](https://github.com/reAAAq/UniXNU-umi/blob/3eb72578597d2ca16471b172d45eddc096a34a04/patches/umi-xnu/early-entry-trace.patch)
