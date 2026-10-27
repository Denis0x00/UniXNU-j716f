# UniXNU-j716f

联想小新 Pad Pro 2021（TB-J716F，SM8250/Kona）的 UniXNU 设备移植仓库。
当前处于早期内核 bring-up，尚未完成 iPadOS 启动。
配套固件：[uefi-j716f](https://github.com/Denis0x00/uefi-j716f)。

## 当前进度（2026-10-27）

| 范围 | 已验证内容 |
| --- | --- |
| 固件与交接 | UEFI 菜单、硬件按键、RAM FAT、DarwinBoot 和早期 XNU 交接 |
| 内存 | 一个 256 MiB 窗口两轮读写通过；不代表全部 RAM |
| 诊断日志 | 版本专用 ARM、CRC、独立检查点、重启后只读读取 |
| P13 | 短程 MMU-on 执行，直接读回 SCTLR M=1 |
| P14/P15 | 原始 cpu_data_init 返回，首次 PE_init_platform(0, bootArgs) 进入并返回 |
| P16 | 原始 WFE configure/init、misc 返回；CNTKCTL_EL1=0x4f，记录经录像观察到的重启后保留 |
| P17 | 原始 chosen/stall/CPU拓扑与boot CPU编号读取返回；终点SCTLR M1，9条CRC匹配并经录像观察到的重启后保留 |
| P18 | boot CPU发布/校验、当前线程设置及rtclock_early_init返回；六槽和终点到达，9条CRC匹配并经一次录像观察到的重启后保留 |
| P19 | 原始debug/startup、timer_call_init、cpu_init和processor_bootstrap返回；9条CRC匹配并经一次录像观察到的重启后保留 |
| P20 | 原始arm_vm_init早期分配、段布局与arm_vm_prot_init返回；切换新TCR/TTBR前终止，9条CRC匹配并经一次录像观察到的重启后保留 |

P18 在rtclock_early_init返回后主动结束；cpu_bootstrap在该基线中只是RET。
真实定时器中断、GIC/ArmCpu 驱动匹配、
持续初始化、调度、物理 UFS/root 和用户态尚未验证。诊断基于冻结的内核构建，
不能视为原生定时器平台集成已经完成。BCM 路由整体排除遵循上游方向，
原始 BCM 故障机理仍未证明。

## P19M 历史实机结果

[startup/timer/CPU/processor分组诊断](docs/V23P19_STARTUP_INIT.md)已仅在boot_b完成测试：
原始debug/startup、timer_call_init、cpu_init和processor_bootstrap返回，六槽及终点均到达。
9条记录CRC独立匹配，终点SCTLR=0x3454593d（M1），日志经一次连续录像观察到的重启后保留。
终点预设请求重启，复位来源未独立证明。P20已进一步验证原始arm_vm_init早期准备，见最新结果；
timer IRQ、GIC、调度及用户态尚未验证。

## P20M 最新实机结果

[arm_vm_init早期VM诊断](docs/V23P20_VM_INIT.md)已仅在boot_b完成测试：
原始VM入口、早期根/上层表分配、内核段布局与vm_set_page_size、arm_vm_prot_init返回均到达。
六槽L1/R1，9条CRC独立匹配；终点SCTLR=0x3454593d（M1），记录经一次连续录像观察到的重启后保留。
终点在新TCR/TTBR切换之前主动请求重启，尚未完成arm_vm_init整体初始化。
下一步审查新表完成与切换顺序，以及探针、栈和日志跨切换的映射/权限。

## P21M 待实机测试

[原始TTBR切换诊断](docs/V23P21_VM_SWITCH.md)已构建，通过26项内核、5项依赖和31项reader主机检查；已仅刷入boot_b，重启命令成功，初始reader、新会话及fresh ID1 Query/ARM已确认，ARM CRC145911AB重算匹配；单次内核测试待进行。
以console专用子表桥接日志，继续原始TTBR1→TTBR0→TLBI/DSB/ISB后终止。
主机walker已修正T1SZ=26的根索引；P20高地址探针/栈缺失误报已更正，原实机结果不变。
原始媒体/二进制仅私有保存；下一轮仍仅boot_b、reader FIRST、fresh Query/ARM。

## 当前文档与证据

- [P16 后的实际进度及上游/sheng 对照](docs/PROGRESS_AFTER_V23P16_20261026.md)
- [P18 历史实机结果](docs/V23P18_BOOTSTRAP_CLOCK.md) · [结构化结果](research/v23p18-bootstrap-clock.json)
- [P16 测试说明](docs/V23P16_TIMER_WFE.md) · [结构化结果](research/v23p16-timer-wfe.json)
- [移植记录](porting.toml) · [证据索引](research/README.md)
- [P13 MMU](docs/V23P13_MMU_CONTINUATION.md) · [P14 early init](docs/V23P14_EARLY_INIT.md)
- [P15 平台初始化](docs/V23P15_PLATFORM_INIT.md)
- [历史进度归档](docs/HISTORY_THROUGH_V23P16.md)

## 构建与仓库边界

`board.toml`/`feeds.toml`/`unixnu.lock` 保存设备格式、依赖与构建方案；
`dts/` 和 `socs/sm8250/` 保存经固件证据交叉核对的板级事实。
SoC 目录仍是本地过渡快照；板级驱动 ABI 和集成未完成。
`board.toml` 中的历史 payload 配方不是最新 P20 的构建入口；
各版本请按独立说明及 `scripts/build-v23p*-kernel.py` 使用。

[原生平台源码候选](docs/NATIVE_PLATFORM_SOURCE_CANDIDATE.md)现已接入独立配置副本：
同时选择 native timer、ArmCpu、ArmDTPlatform 及其 archive recipe。
上游配置解析和组件/构建计划检查已通过；源码编译、链接及实机运行分别验收。
该方案修正了此前 native timer=y、ArmCpu=n 无法满足上游构建条件的问题。
GitHub Actions 的 push 默认构建此源码候选，不执行 prelink 或设备刷写。

不发布 Apple/Lenovo 二进制输入、原始 DTB、镜像、照片、原始日志、用户数据、
随机种子和凭据。公开内容限源码、哈希和脱敏结果摘要。
实机诊断只使用 `boot_b`；每次内核启动必须重新执行该版本 Query → ARM。
TzDxe 不在本阶段范围内。

## P17M 独立历史结果

[chosen/stall/CPU拓扑分组诊断](docs/V23P17_CHOSEN_TOPOLOGY.md)已仅通过`boot_b`
完成一次受控启动：六个独立检查点及终点全到达，实际SCTLR=0x3454593d（M1），
ARM/ENTRY/六槽/终点共9条CRC重算匹配，完整日志在连续录像观察到的重启后保留。
P17终点主动请求重启，未继续boot CPU发布/校验、cpu_bootstrap或rtclock_early_init；
这些后续边界由下面的P18单独验证，保留各版原始证据。

## P18M 历史实机结果

[boot CPU / 当前线程 / 早期时钟分组诊断](docs/V23P18_BOOTSTRAP_CLOCK.md)已构建，
通过50项内核机器码、31项链接reader主机检查及最终镜像审计，并已仅在boot_b测试。
六槽及终点全部到达；新鲜ID1/ARM209C5114绑定的9条CRC独立重算一致，
完整日志在14.277秒连续录像观察到的重启后保留，实际终点SCTLR=0x3454593d（M1）。
P18历史有界结果：在rtclock_early_init返回后主动请求重启，复位来源未独立验证。
P19已推进上述后续函数返回，见当前结果；timer IRQ和调度仍未验证。
