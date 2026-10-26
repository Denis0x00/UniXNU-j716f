# UniXNU-j716f

联想小新 Pad Pro 2021（TB-J716F，SM8250/Kona）的 UniXNU 设备移植仓库。
当前处于早期内核 bring-up，尚未完成 iPadOS 启动。
配套固件：[uefi-j716f](https://github.com/Denis0x00/uefi-j716f)。

## 当前进度（2026-10-26）

| 范围 | 已验证内容 |
| --- | --- |
| 固件与交接 | UEFI 菜单、硬件按键、RAM FAT、DarwinBoot 和早期 XNU 交接 |
| 内存 | 一个 256 MiB 窗口两轮读写通过；不代表全部 RAM |
| 诊断日志 | 版本专用 ARM、CRC、独立检查点、重启后只读读取 |
| P13 | 短程 MMU-on 执行，直接读回 SCTLR M=1 |
| P14/P15 | 原始 cpu_data_init 返回，首次 PE_init_platform(0, bootArgs) 进入并返回 |
| P16 | 原始 WFE configure/init、misc 返回；CNTKCTL_EL1=0x4f，记录经录像观察到的重启后保留 |

P16 在 `/chosen` 查询之前主动结束。真实定时器中断、GIC/ArmCpu 驱动匹配、
持续初始化、调度、物理 UFS/root 和用户态尚未验证。诊断基于冻结的内核构建，
不能视为原生定时器平台集成已经完成。BCM 路由整体排除遵循上游方向，
原始 BCM 故障机理仍未证明。

## 当前文档与证据

- [P16 后的实际进度及上游/sheng 对照](docs/PROGRESS_AFTER_V23P16_20261026.md)
- [P16 测试说明](docs/V23P16_TIMER_WFE.md) · [结构化结果](research/v23p16-timer-wfe.json)
- [移植记录](porting.toml) · [证据索引](research/README.md)
- [P13 MMU](docs/V23P13_MMU_CONTINUATION.md) · [P14 early init](docs/V23P14_EARLY_INIT.md)
- [P15 平台初始化](docs/V23P15_PLATFORM_INIT.md)
- [历史进度归档](docs/HISTORY_THROUGH_V23P16.md)

## 构建与仓库边界

`board.toml`/`feeds.toml`/`unixnu.lock` 保存设备格式、依赖与构建方案；
`dts/` 和 `socs/sm8250/` 保存经固件证据交叉核对的板级事实。
SoC 目录仍是本地过渡快照；板级驱动 ABI 和集成未完成。
`board.toml` 中的历史 payload 配方不是最新 P16 的构建入口；
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

## P17M 当前测试

[chosen/stall/CPU 拓扑分组诊断](docs/V23P17_CHOSEN_TOPOLOGY.md)已构建并完成
45项实际内核机器码、31项实际链接读取器的主机模拟检查及最终镜像校验。
已仅刷入 `boot_b`，验证 B 槽激活且可启动，重启命令成功；实机照片确认
P17 reader 初始页有效、日志为满环形状态，尚未 Query/ARM 或观察到 P17 内核结果。
显式新线性会话已成功，INIT CRC重算匹配；接下来本次 Query → ARM → 单次 P17M。
P16 仍是最后已验证的内核里程碑。
