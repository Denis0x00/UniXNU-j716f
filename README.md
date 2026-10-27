# UniXNU-j716f

联想小新 Pad Pro 2021（TB-J716F，代号 j716f，SM8250/Kona）的 UniXNU 设备移植仓库。
本仓库维护设备配置、板级硬件描述、XNU 构建方案、诊断探针与移植证据。
配套固件：[uefi-j716f](https://github.com/Denis0x00/uefi-j716f)；
上游：[UniXNU](https://github.com/UniXNU/UniXNU)。

**截至 2026-10-27：仍处于早期内核与平台接入阶段，尚未启动完整 iPadOS。**
当前选择的 OS 配置为 iPadOS 17.7.11（21H461）；配置选择不代表已经兼容或启动该系统。

## 当前进展

| 范围 | 已确认的结果 | 验证边界 |
| --- | --- | --- |
| 固件与交接 | UEFI 菜单、硬件按键、RAM FAT、DarwinBoot 与 XNU 入口已用于多轮实机诊断 | 不代表完整 OS 驱动和所有输入均可用 |
| 内存窗口 | 一个 256 MiB 窗口完成两轮读写 | 不代表全部 RAM 或外设 DMA 已验证 |
| 诊断日志 | 版本专用 ARM、CRC、独立检查点及重启后只读读取 | 保留结果受具体启动/复位流程约束 |
| MMU | P13 已直接读回 SCTLR_EL1 的 M=1 | 只证明该有界执行范围 |
| 早期 CPU/平台 | P14～P19 推进原始 CPU、平台、拓扑、早期时钟与处理器初始化函数返回 | 尚未证明 IOKit 平台匹配、真实 IRQ 和调度 |
| 虚拟内存 | P20/P21 推进早期 VM 准备、原始 TTBR1/TTBR0 切换及 TLB 同步 | 完整 arm_vm_init 返回尚待实机验证 |
| P22 | 剩余 VM 流程与返回诊断已构建并通过主机校验 | 尚未刷入；用户已暂缓本轮测试 |
| 原生平台候选 | 独立 ArmDTPlatform/ArmCpu/native timer 源码方案已有编译、链接门槛和导出 CI 成功记录 | 尚未预链接成设备验证候选，未验证平台匹配或 timer IRQ |

按上游流程，当前仍在**阶段 5：内核入口与平台接入**；阶段 6 存储/根卷、阶段 7 用户态尚未完成。
当前最深的已验证实机边界是 P21；P22 的主机结果单独记录。
完整说明见[最新进度与上游/sheng 对照](docs/PROGRESS_AFTER_V23P22_20261027.md)。

本轮参照同 SM8250 的 pipa/umi 新增[原生 IRQ 观测候选](docs/NATIVE_IRQ_OBSERVER.md)：
已接入真实 IRQ 进入、rtclock 返回、EOI 后的九个有界快照，确认原厂/Recovery 输入均描述
virtual timer INTID27。Linux 双编译器及 AArch64 汇编门槛通过；完整内核编译待 CI，
尚无预链接、持久化采集通道或实机 IRQ 结果。P22 继续待测。

### P21：最新实机里程碑

原始 TTBR1/TTBR0 设置函数均已返回，执行到原始 TLBI/DSB/ISB 之后。
六个独立检查点在逻辑/原始扫描中各命中一次，九条记录 CRC 独立重算匹配；
终点 SCTLR_EL1=0x3454593d（M=1），日志在一次连续录像观察到的重启后保留。

诊断终点主动请求 SYSTEM_RESET，复位来源尚未独立测定；自动重启本身不表示又在相同位置崩溃。
尚未证明 arm_vm_init 完整返回、全部内存映射或长期运行稳定性。
详见 [P21 测试结果](docs/V23P21_VM_SWITCH.md)和[结构化证据](research/v23p21-vm-switch.json)。

### P22：已构建，等待实机测试

保留原始调用、失败分支与 RET，新增以下检查点：

1. `PMAP_BOOTSTRAP_RETURN`：pmap_bootstrap 及紧随其后的 disable_preemption 返回。
2. `VM_FINAL_READY`：动态/复制窗口页表、静态区域补丁、启用抢占与栈保护检查完成。
3. `ARM_VM_RETURN`：原始 arm_vm_init 已返回调用方，立即记录终点并请求复位。

26 项内核、8 项依赖、31 项实际链接读取器检查，以及两轮 UEFI 构建和最终镜像审计通过。
主机模型执行了原始 VM 流程，但系统寄存器、TLB、缓存与 SMC 等效果仍为模型。
日志借用的静态 bootstrap 子表已审查至本轮返回终点，后续回收和线程切换不在结论内。
详见 [P22 方案](docs/V23P22_VM_RETURN.md)和[构建摘要](research/v23p22-vm-return.json)。

## 多轮测试的里程碑

| 版本 | 实机验证到的边界 | 说明 |
| --- | --- | --- |
| P13 | MMU 开启后直接读回 M=1 | [MMU 诊断](docs/V23P13_MMU_CONTINUATION.md) |
| P14/P15 | 原始 cpu_data_init 返回；首次 PE_init_platform(0, bootArgs) 进入并返回 | [P14](docs/V23P14_EARLY_INIT.md) / [P15](docs/V23P15_PLATFORM_INIT.md) |
| P16 | 原始 WFE configure/init、misc 返回；CNTKCTL_EL1=0x4f | [定时器/WFE](docs/V23P16_TIMER_WFE.md)；尚非真实 WFE 唤醒验证 |
| P17 | chosen/stall 查询、CPU 拓扑与 boot CPU 编号读取返回 | [拓扑诊断](docs/V23P17_CHOSEN_TOPOLOGY.md) |
| P18 | boot CPU 发布/校验、当前线程设置、rtclock_early_init 返回 | [CPU/早期时钟](docs/V23P18_BOOTSTRAP_CLOCK.md)；本基线 cpu_bootstrap 只是 RET |
| P19 | debug/startup、timer_call_init、cpu_init、processor_bootstrap 返回 | [启动初始化](docs/V23P19_STARTUP_INIT.md) |
| P20 | arm_vm_init 早期分配、段布局、arm_vm_prot_init 返回 | [VM 准备](docs/V23P20_VM_INIT.md)；新 TCR/TTBR 切换前停止 |
| P21 | 原始 TTBR1/TTBR0 返回及 TLB 同步后 | [VM 切换](docs/V23P21_VM_SWITCH.md)；完整 VM 返回待测 |

P22 为待测版本，不计入上表实机成果。各版本原始证据独立保留；
较早过程见[历史归档](docs/HISTORY_THROUGH_V23P16.md)和[研究索引](research/README.md)。

## 与上游和小米平板 6S Pro 的对照

本次核对 UniXNU `f85b5a60`、sheng `08cde20e`、SM8550 公共层 `23b2049f`、
ArmCpu `9066b53b` 与 uefi-sheng `20ac23d5`。
上游主仓库相比上次审查新增一次设备目录/记录刷新，没有新增当前 MMU/VM 路径的代码修复；
sheng 与 ArmCpu 版本未变。审查版本与本项目固定构建版本分别记录。

sheng 已有维护者报告的 GPU、联网、音频、充电与调频实机记录，也保留性能、音频调校和
低电量关机等限制。本项目没有独立复测这些结果。
可参考其平台驱动接线、DTS/ADT 资源描述、真实定时器中断计数与持久化日志流程；
SM8550 的寄存器地址、CPU 拓扑、UFS/SMMU、电源和分区常数需按 SM8250 重新核对。

BCM 路由整体排除遵循上游 native timer 入口方向，原始 BCM 故障机理仍未证明。
下一步重点是原生平台接入和实际 IRQ，而不是把恢复 BCM 作为前置条件。
参考版本、链接和适用范围见[本次对照](docs/PROGRESS_AFTER_V23P22_20261027.md)。

## 接下来的验收顺序

1. 用户恢复测试后完成 P22，验证剩余 VM 流程与原始返回。
2. 审查独立原生平台候选的预链接 ABI、单核 ADT、内存与日志生命周期。
3. 实机记录 ArmDTPlatform 匹配、CPU 注册、timebase 前进和 timer IRQ 计数持续增长。
4. 验证单核调度和超时，再评估多核/IPI；核对 UFS 时钟、PHY、SMMU、DMA 后先做受控读取。
5. 完成根卷与用户态；存储日志范围、只读读取器和手动抓取闭环验证后，再接自动启动测试。

原生显示、背光、触摸、GPU、Wi-Fi、音频、电源管理也需要各自的板级适配与设备证据。
UEFI 有画面和菜单按键，不代表 XNU 对应驱动已经工作。

## 仓库结构

| 路径 | 用途 |
| --- | --- |
| `porting.toml` | 权威移植记录：子系统输入、发现、证据、限制与下一步 |
| `board.toml`、`feeds.toml`、`unixnu.lock` | 设备身份、构建方案、依赖与固定来源 |
| `configs/` | 基线和独立原生平台源码配置 |
| `dts/`、`socs/sm8250/` | 板级硬件描述与当前 SoC 过渡快照 |
| `patches/`、`package/` | 构建兼容补丁与设备集成代码 |
| `scripts/` | 源码构建、ADT 检查、版本专用探针构建及解码 |
| `docs/`、`research/` | 测试说明、脱敏结果与来源摘要 |

SM8250 公共源尚未完成本项目资格验证；当前 SoC 目录仍是过渡快照。

## 构建入口与持续集成

### 冻结诊断与原生平台候选

- P13～P22 使用冻结的源码构建/预链接内核制作有界诊断，按各版文档及
  `scripts/build-v23p*-kernel.py` 选用；P22 入口为 `scripts/build-v23p22-kernel.py`。
- `board.toml` 的历史负载配方不能直接生成最新 P22；版本专属内核、ADT、固件配置与哈希必须一致。
- 原生平台候选单独选择 ArmDTPlatform、ArmCpu 与 native timer；源码构建固定
  UniXNU `42bffe8a56823dad2e448c776b4f94862f3e290a`，不能直接换成最新主分支。

在 macOS/Xcode 构建机上，使用已检出的固定上游版本执行源码候选构建：

```sh
python3 scripts/build-j716f-source.py --unixnu /path/to/pinned-UniXNU   --device /path/to/UniXNU-j716f --native-arch-timer --jobs 3
```

该命令处理源码与归档构建，不执行预链接或设备刷写。
完整要求见[源码构建](docs/XNU_SOURCE_BUILD.md)和[原生平台候选](docs/NATIVE_PLATFORM_SOURCE_CANDIDATE.md)。

### GitHub Actions 的验证范围

| 工作流 | 验证内容 | 不包含 |
| --- | --- | --- |
| [移植记录检查](.github/workflows/porting-record.yml) | 结构、组件覆盖和变更证据 | 实机启动 |
| [XNU 源码构建](.github/workflows/xnu-source-macos.yml) | 源码编译、链接符号/归档门槛、加密导出及上传；符合路径条件的推送默认构建原生候选 | 预链接、P22 镜像组装、刷机、IRQ 验证 |

源码候选已有[完整成功记录](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40351062303)，
此前导出清单问题的修复见 [CI 记录](docs/CI_SOURCE_TRANSFER.md)。
当前运行状态请查看 [Actions](https://github.com/Denis0x00/UniXNU-j716f/actions)。

## 实机测试约定

- 仅使用 `boot_b`，保持 `boot_a`、vbmeta、userdata 不变；TzDxe 不在本阶段范围内。
- 每轮先核对版本和镜像哈希，进入菜单后先保存该版本 reader 初始页。
- 每次内核启动都要重新执行该版本 `Query → ARM`，核对照片后仅启动一次。
- 重启后先只读 reader 并保存页面，再进行后续操作；已消耗的 ARM 不能复用。
- P22 当前暂缓测试，构建通过不等于设备验证通过。

公开仓库提供源码、配置、哈希和脱敏摘要；完整复现仍需匹配的本地厂商/系统输入。
Apple/Lenovo 二进制、完整 DTB/ADT、生成镜像、照片视频、原始日志、设备标识、随机种子和凭据不公开。
配置本地输入时参考 `local.toml.example`，不要提交本地路径或私有输入。

## 文档入口

- [当前进度与上游对照](docs/PROGRESS_AFTER_V23P22_20261027.md)
- [P21 实机证据](research/v23p21-vm-switch.json) · [P22 待测摘要](research/v23p22-vm-return.json)
- [移植记录](porting.toml) · [研究索引](research/README.md)
- [上游移植指南](https://unixnu.com/zh/porting/) · [小米平板 6S Pro](https://github.com/UniXNU/UniXNU-sheng)
