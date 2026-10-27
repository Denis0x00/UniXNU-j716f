# P22 准备完成后的实际进度与上游对照

更新日期：2026-10-27。本次核对上游源码与已有证据，并整理文档；未构建新镜像、未刷机。
P22 已按用户要求暂缓测试。P21 是最新实机里程碑，P22 只有构建和主机校验结果。

## 多轮测试获得了什么

测试建立了可复核的执行范围：从早期交接、独立日志和 MMU 开启，逐步推进至原始
CPU/平台初始化、早期时钟、处理器初始化和虚拟内存根表切换。
P13～P21 的终点逐步后移，因此自动重启不能一律解释为又在同一点崩溃。
这些诊断在预设终点请求 SYSTEM_RESET；重启来源仍未独立测定。

| 范围 | 当前证据 | 仍未证明 |
| --- | --- | --- |
| UEFI 至内核交接 | 菜单、按键、RAM FAT、DarwinBoot 与多轮内核入口测试 | 完整 OS 所需全部输入和驱动 |
| MMU 与早期初始化 | P13 M1；P14/P15 原始 cpu_data_init、首次 PE_init_platform 返回 | 完整平台 provider 匹配和运行 |
| CPU 与早期时钟 | P16～P19 的 WFE 配置、拓扑、当前线程设置、rtclock_early_init、cpu_init 等返回 | 定时器持续前进、真实 IRQ、调度器、多核 |
| VM 早期准备 | P20 早期分配、布局、arm_vm_prot_init 返回 | 全 RAM 所有权和完整 VM 稳定性 |
| VM 根表切换 | P21 原始 TTBR1/TTBR0 setters 返回、TLBI/DSB/ISB 后检查点 | 完整 arm_vm_init 返回、后续内存回收 |
| VM 收尾与返回 | P22 主机执行原始 pmap_bootstrap、收尾及 RET，26+8+31 项检查通过 | P22 尚未刷入，无本轮实机结论 |

P21 六槽在逻辑/原始扫描中各命中一次，九条记录 CRC 独立匹配，终点
SCTLR_EL1=0x3454593d（M=1）；日志在一次连续录像观察到的重启后保留。
这提供了执行边界和记录保留证据，不等于长期运行稳定性。

按[上游阶段表](https://github.com/UniXNU/UniXNU/blob/f85b5a60a90847dd1d640ee787b54f6a79333969/docs/zh/porting/bring-up.md)，
当前仍在**阶段 5：早期内核与平台接入**。阶段 6 存储/根卷和阶段 7 用户态尚未完成；
阶段 5 中的完整平台匹配也尚未验证，不能按“完成五个阶段”计算移植百分比。

## 本次上游变化

| 仓库 | 本次核对版本 | 相比上次审查 |
| --- | --- | --- |
| UniXNU | f85b5a60a90847dd1d640ee787b54f6a79333969 | 相比 ae687009 新增一次设备目录/记录刷新 |
| UniXNU-sheng | 08cde20edc3254ed6f7df8d0f5384fb887786e8f | 未变 |
| UniXNU-sm8550-common | 23b2049fcd9e30356161def84231334fb55ecca9 | 未变 |
| ArmCpu | 9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b | 未变 |
| uefi-sheng（sheng 分支） | 20ac23d5e0937106c2129303dfc931b3390ae660 | 本次记录固定版本供固件对照 |

本次新增提交未修改核心 MMU、定时器入口或平台驱动代码，没有发现可直接替代 P22 的修复。
J716F 源码构建继续固定 42bffe8a56823dad2e448c776b4f94862f3e290a；审查最新版不改变构建输入。

## 小米平板 6S Pro 可以借鉴什么

sheng 使用 SM8550，J716F 使用 SM8250。其[设备配置](https://github.com/UniXNU/UniXNU-sheng/blob/08cde20edc3254ed6f7df8d0f5384fb887786e8f/board.toml)
将 ArmDTPlatform、ArmCpu 及存储/OS 服务接入统一归档、预链接和初始化流程。
[ArmCpu](https://github.com/UniXNU/ArmCpu/blob/9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b/ArmCpu.cpp)
提供 CPU 注册、GIC、定时器 PPI27 分发至 rtclock_intr，以及可观测的注册/IRQ 计数。
这些是 J716F 下一阶段需要验证的真实运行链路；只让 rtclock_early_init 返回不足以证明它们可用。

sheng 的[移植记录](https://github.com/UniXNU/UniXNU-sheng/blob/08cde20edc3254ed6f7df8d0f5384fb887786e8f/porting.toml)
已有维护者报告的 GPU 工作负载、Wi-Fi、音频、充电和调频实机结果，显著超出当前 J716F 阶段。
记录也保留 Wi-Fi 性能与密码保存、扬声器调校/保护、低电量关机 panic 等限制。
本次没有独立复测这些结果，不把“已实现”泛化为全部场景稳定。

上游的[原生定时器入口补丁](https://github.com/UniXNU/UniXNU/blob/f85b5a60a90847dd1d640ee787b54f6a79333969/patches/xnu/architectural-timer-entry.patch)
在 native timer 模式下排除整个 BCM 路由段。J716F 当前诊断沿用这一方向，
BCM 原始故障机理仍未证明；本阶段优先推进原生平台，不将恢复 BCM 作为前置条件。

可借鉴构建分层、初始化接线、计数器和日志格式；SM8550 的 MMIO、CPU affinity、
SMP/IPI、UFS/SMMU、电源与分区偏移需要重新按 SM8250 资料核对。
UEFI 方面可参考[sheng 固件](https://github.com/UniXNU/uefi-sheng/blob/20ac23d5e0937106c2129303dfc931b3390ae660/README.md)
的公共负载块设备与板级内存策略分工。J716F 当前诊断仍使用嵌入式 FAT，Android ramdisk 为空；
历史配置中的标准 initrd 声明不等于最新 P22 已切换到该路径。

## 下一步与验收门槛

1. **先完成待测 P22。** 用户恢复测试后，仅刷 boot_b；保存初始 reader，重新 Query→ARM，
   一次启动并录像，重启后先只读 reader，检查 pmap 返回、VM 收尾和调用方返回三组记录。
2. **推进独立原生平台候选。** ArmDTPlatform/ArmCpu/native timer 的源码构建、链接符号与
   归档门槛已有 CI 成功记录；还需匹配预链接 ABI 和单核 ADT，再观察平台匹配、CPU 注册、
   timebase 前进和真实 timer IRQ 计数持续增长。该候选尚未接替冻结诊断内核。
3. **验证调度，再扩展。** 保持单核，先验证线程/超时能持续推进；随后才评估多核、IPI 和 WFE。
   完成 VM/平台基础后，核对 UFS 时钟、PHY、SMMU、DMA，先做受控读取，再处理根卷。
4. **建立持久化日志与自动测试。** 按[上游日志流程](https://github.com/UniXNU/UniXNU/blob/f85b5a60a90847dd1d640ee787b54f6a79333969/docs/zh/porting/log-capture.md)
   先验证板级存储日志范围、写入器、Linux 只读读取器和返回 bootloader；手动完成一次闭环后再自动循环。
   Advanced Logger 的二维码导出可用于固件阶段，不能替代 ExitBootServices 后的内核日志。

P22 借用的静态日志子表生命周期仅在主机模型中审查到立即返回终点；
后续 arm_init、内存回收和线程/用户映射需要重新审查。内核日志的低地址桥接属于临时诊断策略。

## 证据入口

- [P21 实机结果](V23P21_VM_SWITCH.md)
- [P22 待测方案与生命周期审查](V23P22_VM_RETURN.md)
- [本次版本、来源哈希和审查摘要](../research/upstream-progress-review-20261027-p22.json)
- [原生平台源码候选](NATIVE_PLATFORM_SOURCE_CANDIDATE.md)
