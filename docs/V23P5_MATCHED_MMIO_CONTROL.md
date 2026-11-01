# V23P5：保留同一包络的 STR / NOP 对照

## 与上游流程的关系

2026-10-25 通过 `git ls-remote` 核对上游 main 为
`16e592abee8a550dec8154ed1f38b5ebc4f16213`，并读取该版本的指南和补丁。

- [bring-up 顺序](https://github.com/UniXNU/UniXNU/blob/16e592abee8a550dec8154ed1f38b5ebc4f16213/docs/zh/porting/bring-up.md)：分阶段推进、一次隔离一个问题、失败后保存镜像和最后记录。当前是**阶段 5 内核入口的早期部分**；平台专家尚未到达，阶段 5 未完成。
- [移植步骤](https://github.com/UniXNU/UniXNU/blob/16e592abee8a550dec8154ed1f38b5ebc4f16213/docs/zh/porting/steps.md)：板级资源取自本机，组件按硬件和 ABI 选择，结果写入 porting.toml。
- [平台适配](https://github.com/UniXNU/UniXNU/blob/16e592abee8a550dec8154ed1f38b5ebc4f16213/docs/zh/porting/platform.md)：核对 CPU affinity、PSCI、GIC、timer 和内存所有权，再接入对应驱动。
- [日志抓取](https://github.com/UniXNU/UniXNU/blob/16e592abee8a550dec8154ed1f38b5ebc4f16213/docs/zh/porting/log-capture.md)：入口可使用屏幕诊断；固件 Advanced Logger/二维码导出、QEMU 固件/入口 fixture 和更后的持久化日志仍需补齐或独立验证。Unicorn 指令检查不能充当完整 QEMU 启动测试。

V23P5 是 J716F 的临时终止诊断，沿用已测的冻结内核和固件基础；本次没有整体升级
到最新主干，也没有选择尚未验证的 ArmCpu、UFS 或 root。其产物由专用脚本构建，
还需在结论明确后归入标准设备源码构建配方和经过审核的固定 SoC 支持。

## 上游相关实现

上游 [architectural-timer-entry.patch](https://github.com/UniXNU/UniXNU/blob/16e592abee8a550dec8154ed1f38b5ebc4f16213/patches/xnu/architectural-timer-entry.patch)
在 `XNU_NATIVE_ARCH_TIMER` 启用时跳过 BCM2837 的旧 timer interrupt routing。
当前 `configs/j716f_source_defconfig` 为 `CONFIG_XNU_NATIVE_ARCH_TIMER=n`，旧路径
仍保留。本次对照只跳过已定位的一条写入，不能代替完整架构定时器、GIC 或 PSCI
适配，也不能仅凭选项名称就开启整条尚未验证的驱动路径。

## 对照设计

P4 r1 实机录像显示原始 STR 前后方块及标题检查点 1/2；检查点 3 未见。
这证明执行越过 STR，外部事务完成仍未知。P3A 没有这套相同的可见记录。

| 项目 | V23P5A | V23P5B |
| --- | --- | --- |
| 包络中的因子指令 | 原 `str w1, [x0]` | `nop` |
| 与 P4 r1 的内核关系 | SHA256 完全相同 | 只改此 4 字节及 32 字节 crypto HMAC |
| 标题模式与 DSB | mode 5、原 DSB SY | 相同 |
| 清黑旧标记、绘制前后格、clear filter | 保留 | 相同 |
| 菜单缩放 | 3，较原 4 缩小 25% | 相同 |
| 额外驱动、MMU 设置、定时器依赖 | 无 | 无 |

包络中因子地址以 payload.json 的 `matched_factor.site` 为准，原位置
`0xfffffff0073fe32c` 仍为跳入包络的 branch。B 的“后格”代表越过 NOP 的记录。

## 已完成的主机检查

- 两版实际 FD/FV/FAT、PE、Android v2 header/DTB/校验/填充审计通过。
- 每版 crypto HMAC 和源码哈希通过；A 内核精确等于 P4 r1。
- 对整个 A/B 内核逐字节比较，只允许因子指令和 HMAC 变化。
- A/B 的非 payload 固件模块全部相同。相对 P4 r1，显式允许 UniXNU 菜单模块和
  payload 两项变化；菜单源码只改缩放、对应注释及 source-lock 说明。
- Unicorn 分别执行实际 legacy branch、包络、因子和返回，验证寄存器保留与旧像素清除。
- 每版执行整个 pre/setup_fb/clear filter，核对全部 16384000 字节，并继续真实标题渲染
  至 DSB 前：两个包络格保留，顶部仅 1/2 已绘制。
- A 的模拟 MMIO 写入 1 次；B 的 MMIO 页不映射，运行至同一位置且访问为 0。

模拟没有执行实际总线事务；原始返回至 pre 之间的特权指令路径被排除。
标题 DSB 前主动停止，未模拟证明屏障能完成。P5A 已仅刷入 boot_b，激活 B、核验槽位和可启动标志，重启命令成功；A/B 均已刷入并审阅录像，当前结果见末尾各版录像节。

本地命令：

```text
python tools/manage_j716f_v23p5_vm.py start
python tools/manage_j716f_v23p5_vm.py fetch
python tools/package_j716f_v23p5.py
python tools/check_j716f_v23p5_probe.py
```

原始视频、私有输入、VM 凭据、FD、FAT 和 boot 镜像均保存在公共仓库外。

## 实机顺序及判断

1. 仅在 boot_b 测试。先测 A，确认较小菜单和冻结内核仍重现 P4 r1 进度；保存录像。
2. 再用同样条件测试 B。分别拍菜单身份、上下两排方块、EL/SCTLR/A0/RETURN 和后续变化。
3. 若 A 复现、B 能反复越过第 2 格，支持旧写入影响后续进度，继续核对源码 timer routing
   的准确适配范围。单次差异先视作线索，不能直接断定硬件根因。
4. 若两版都停在相同可见位置，继续围绕 DSB 前后的记录和先前内存访问定位。
5. 若 B 没有诊断输出，仅记录实际观察；保留 A 的可见控制，调查路径前置依赖。

任何重启都不能单独证明 PSCI 或 watchdog。持续 MMU-on、ArmCpu、UFS/root 和用户态均未验证。

## V23P5A 录像结果（2026-10-25）

菜单确认 `V23P5A - A: original STR; matched MMIO envelope`，较小菜单可见。
标题 mode 5、顶部检查点 1/2 和下方两个 MMIO 标记均出现，复现 P4 r1 的
可见进度；未见检查点 3、EL/SCTLR 或 PSCI 进度。

- 1.500 秒见入口绿/蓝带，1.567 秒采样确认标题和四个方块。
- 18.932 秒仍有诊断，18.965 秒消失，入口至画面消失约 17.47 秒。
- 后续见 Lenovo/Powered by Android 标志，22.997 秒采样已回 A 菜单。
- PMIC 从 `0x02000001` 变为 `0x02000041`，位含义和复位源仍未知。

相机计时不同于 P4 r1 的约 15.94 秒；本次复现的是可见检查点，不能声称
延时相同或据此判定 watchdog。后标记证明执行越过 STR，外部写事务完成
仍未证实。这是 A 复核时的历史状态；B 后续结果见下一节。

V23P5B was flashed only to boot_b in fastbootd; B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Its menu identity and diagnostic progress await video. Reviewed P5A remains the visible control; no P5B functional, PSCI or MMU-on result is claimed.

## V23P5B 录像结果（2026-10-25，当前结论）

此节取代前文“B 尚未刷写／待审”的历史状态。B 已仅刷 boot_b 并完成录像复核。
菜单身份为 `V23P5B - B: skip STR; matched MMIO envelope`。

- 第 54 帧 1.800 秒仍为菜单；第 55 帧 1.833 秒出现入口带。
- 第 56 帧 1.867 秒清楚出现下方两个包络白格，以及 `V23` 部分标题。
  mode 数值不能可靠辨认；顶部第 1 格区域部分白/蓝可见，不能确认其完整绘制。
  未观察到顶部第 2/3 格、EL/SCTLR、A0 或 SMC 前检查点。
- 第 57 帧 1.900 秒诊断已消失：仅拍到一帧诊断；入口带至消失约 0.067 秒。
  这是相机观测，包含曝光／扫描混合，不能由残留菜单或不完整标题定位最终 PC。
- 随后出现 Lenovo/Powered by Android 标志，5.998 秒采样已回到 B 菜单。
  PMIC 为 `0x02000001 -> 0x02000041`，复位源未分类，不能据此判断 watchdog。

A 的约 17.47 秒与 B 的快速重启存在明显差异，但不能直接判定 B 更早失败。
本诊断后段本来会设置 `x0=0x84000009`、执行 `DSB SY` 后 `SMC #0`
请求 PSCI SYSTEM_RESET。跳过旧 MMIO 后迅速推进到主动复位是一个候选解释；
录像没有捕获 A0/RAW10，故 PSCI、屏障完成和复位来源仍未证实。

下一项建议：保留 P5B，只将 `0xfffffff0073fc798` 的 SMC 替换为原地
`b .`（小端字节 `00 00 00 14`），重算 HMAC 并审计。保留前面的 DSB，
不依赖 counter、不新增 MMIO，也不跳到带额外 DSB 的现有 halt。
若到达调用点，期望保留 A0/RAW10；若仍迅速复位，继续调查其他异常／复位源。
这个对照尚未制作或刷入；本次复核没有设备操作。仍处于阶段 5 早期，未建立持续 MMU-on。
