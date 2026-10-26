# P16 后的实际进度与上游对照

日期：2026-10-26。本次只审查源码和已有证据，没有制作或刷入新镜像。
完整版本与证据索引见
`research/upstream-progress-review-20261026-p16.json`。

## J716F 已经验证的内容

| 里程碑 | 实机证据与边界 |
| --- | --- |
| UEFI、菜单、负载和 DarwinBoot 交接 | 已用于多轮受控内核诊断；不代表完整 OS 镜像所有输入均已验证 |
| 早期日志 | 已建立版本专用 ARM、CRC、独立检查点及重启后只读读取流程 |
| P13：MMU | 直接记录 SCTLR `0x3454593d`，证明短程 M1 执行；尚非持续运行稳定性验证 |
| P14：原始 early init | 已到原始 `cpu_data_init` 返回 |
| P15：平台初始化 | 已到首次 `PE_init_platform(0, bootArgs)` 入口和返回；不等于 ArmDTPlatform 驱动已经匹配 |
| P16：WFE/misc | 原始 WFE configure/init 和 misc 均返回，终点直接读回 CNTKCTL `0x4f`；六个独立检查点与终点在视频观察到的重启后保留 |

P16 九条记录 CRC 全部重算一致；启动前后 ARM ID2/`93A96F6F` 对应，
日志增长512字节。当前已知的原始执行边界是 `/chosen` 查询之前。
这不是未知故障点：诊断在这里主动结束，后续原始路径尚未验证。
视频证明重启、回菜单和读取器的顺序，复位源没有独立测定。

按[上游 bring-up 阶段](https://github.com/UniXNU/UniXNU/blob/ae687009348296e97e49ce6e925795044f363ce6/docs/zh/porting/bring-up.md)，
当前仍处于阶段5的早期内核部分，阶段6存储/根卷和阶段7用户态尚未完成。

## 本次上游核对

主仓库 `ae687009348296e97e49ce6e925795044f363ce6`，sheng
`08cde20edc3254ed6f7df8d0f5384fb887786e8f`，sm8550-common
`23b2049fcd9e30356161def84231334fb55ecca9`，ArmCpu
`9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b`，均与 P16 准备时相同。
没有新的提交需要加入本轮诊断；当前源码构建固定版本也未更改。

上游的[架构定时器入口补丁](https://github.com/UniXNU/UniXNU/blob/ae687009348296e97e49ce6e925795044f363ce6/patches/xnu/architectural-timer-entry.patch)
在 native arch timer 模式下排除整个 BCM 路由段。J716F 的诊断沿用这个方向，
原始 BCM 故障机理没有因此得到证明，也没有恢复该路径的必要条件。

## 小米平板6S Pro 已有后续设备运行记录

本次补查了 sheng 的[完整移植记录](https://github.com/UniXNU/UniXNU-sheng/blob/08cde20edc3254ed6f7df8d0f5384fb887786e8f/porting.toml)。
其中有14条 source、2条 host 和6条 device 证据。维护者报告的设备结果包括
GPU 工作负载测量、Wi-Fi 联网和双向测速、扬声器/采集、应用播放/录音、
充电，以及 CPU/GPU 调频。它已进入明显更后面的系统和外设集成阶段。
这些是对方维护者记录的实机结果，本次没有独立复测。

其记录同时保留了 Wi-Fi 性能/密码保存、音频调校与保护、低电量关机 panic
等限制。因此可以借鉴其已经集成和测试的路径，长期稳定性仍需具体版本的
测试范围支持。主仓库的 native-timer HVF fixture 自述未测 sheng 硬件；
该离线 fixture 与上述设备运行记录是两组不同证据，不能相互替代。

可直接参考的结构：

- [board.toml](https://github.com/UniXNU/UniXNU-sheng/blob/08cde20edc3254ed6f7df8d0f5384fb887786e8f/board.toml)
  把 ArmDTPlatform、ArmCpu 和后续驱动纳入统一构建与预链接。
- [ArmCpu](https://github.com/UniXNU/ArmCpu/blob/9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b/ArmCpu.cpp)
  配置 GIC、启用 timebase，处理定时器 PPI27 并调用 `rtclock_intr`。
- CPU/GIC/timer/PSCI 来自板级和 SoC 描述；SM8550 的物理地址、IPI 策略、
  UFS/SMMU/电源参数不直接用于 SM8250。

## 当前最大的工程差距

P13–P16 是在冻结的源码构建/预链接基线上制作的有界二进制诊断。
它们让原始内核继续执行到逐步后移的终点，但没有把完整平台驱动接起来。
本地 `j716f_arch_timer_source_defconfig` 虽设 native arch timer=y，
仍将 ArmCpu 和 ArmDTPlatform 置为 n；该配置只是源码资格验证方案，
目前只完成解析，没有完成该方案的源码编译或实机运行。
单纯继续增加返回点，无法证明调度器、存储和用户态可用。

下一阶段建议同时准备两条工作线，均尚未在本次实施：

1. 保留 P16 已知基线，下一版一次覆盖 `/chosen`、`xnu_platform_stall`
   查询/分支和下一个原始初始化边界，减少按单条指令拆分版本。
2. 建立独立源码集成候选，将 native arch timer、ArmDTPlatform、ArmCpu 与
   J716F 的真实资源和 ABI 接上。先单核验证驱动匹配、计数器前进和真实
   timer IRQ 计数增长，再推进调度、其他 CPU、存储/根卷及用户态。

已有日志、映射生存期、CRC 和受控重启流程应成为新候选的观测手段。
本次不改变 P16 镜像、固定依赖或设备状态，后续诊断仍遵循 boot_b 范围。
