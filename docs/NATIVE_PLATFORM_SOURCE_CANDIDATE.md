# 原生定时器平台源码候选

2026-10-27 新增独立 [IRQ 观测候选](NATIVE_IRQ_OBSERVER.md)：保留固定组件原始路径，
增加 CPU/GIC/timebase 和真实 IRQ enter/rtclock 返回/EOI 的有界快照。
Linux 双编译器和 AArch64 观测封装检查通过，本轮完整内核编译及新增链接门槛已在
[CI 40445461837](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40445461837)
通过（受测源码 `9ae5d75`）；无预链接或新实机结论。

此候选与 P16/P17 的冻结内核诊断分别构建。上游构建脚本明确要求
`XNU_NATIVE_ARCH_TIMER=1` 同时具有真实 `ArmCpu` 中断提供者。
此前 native timer=y、ArmCpu=n 的方案仅解析过配置，不能通过该上游构建门槛。

## 实际接线

- `scripts/prepare-native-platform.py` 在原设备仓库之外建立源码配置副本。
- 副本选择 `ArmDTPlatform`、`ArmCpu`，加入两者 archive recipe。
- `j716f_arch_timer_source_defconfig` 同时启用 native timer、ArmCpu、ArmDTPlatform。
- 普通源码构建与诊断 `board.toml` 保持各自入口。上游版本仍固定
  `42bffe8a56823dad2e448c776b4f94862f3e290a`。
- 组件由该版本 catalog 固定：ArmCpu `9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b`、
  ArmDTPlatform `417ff95d6a6e8607ddea9309ec45c3bfd4a67c60`。

```sh
python3 scripts/build-j716f-source.py --unixnu /path/to/pinned-unixnu \
  --device /path/to/UniXNU-j716f --native-arch-timer --jobs 3
```

需要 macOS/Xcode 构建机；GitHub Actions 的 push 默认构建该候选，手动工作流
可通过 `native_arch_timer` 选择。生成目录已存在时拒绝覆盖。
构建结束必须检查真实链接 Mach-O 的已定义符号：`armcpu_registered`、
`armcpu_timer_interrupts`、`armdt_platform_configured`、`rtclock_intr`，
并核对两份 archive、原生入口中 BCM 段消失及输出哈希。

## 后续设备门槛

源码编译/链接不等于设备驱动运行。本阶段不执行 prelink、刷写或用户态启动。
后续还须核对单核 ADT、CPU 拓扑、GIC ranges/reg/stride、PSCI、内存映射生命周期
以及预链接 ABI，再在真实设备上记录平台匹配、CPU 注册、timebase 前进和
timer IRQ 计数连续增长。J716F GIC 资源来自已有 stock/recovery 交叉证据，
不复制 SM8550 地址或 IPI/SMP 策略。当前先保持单核。

## 独立单核 ADT 候选

`make-j716f-native-platform-adt.py` 从既有板级证据生成独立输入，
显式添加 `/psci/method=smc`；`check-native-platform-adt.py` 独立解码核对
CPU0 affinity、timebase、NVRAM 输入以及 GIC ranges/reg/stride 的编码和映射。
实际候选10540字节，SHA256见结构化研究摘要；检查通过。
GICD=0x17a00000/0x10000、GICR=0x17a60000/0x100000、stride=0x20000
均来自已有 stock/recovery 交叉证据。保持单核，不复制 SM8550 的 SMP/IPI 常数。

该候选包含新随机种子，仅留本地；没有替换 P13～P17 的固定 ADT，未装入 P17。
输入检查不访问 MMIO，不证明驱动匹配、IRQ、prelink 或硬件运行。
## 源码构建 CI 结果（2026-10-26）

[运行40351062303](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40351062303)
在设备源码 `816e3c554af49ffb50c82a9eece504cf76e1f0b8` 全部通过：
原生候选源码编译、链接符号/archive 门槛、加密导出及上传成功。
此前源码编译步骤已成功，整个 workflow 的失败来自导出脚本仍只接受四个产物；
现已同时支持原生配置的六个产物和基线四个产物，详见
[CI 修复记录](CI_SOURCE_TRANSFER.md)。

这是 CI 步骤结果及驱动门槛的证据；本次没有下载并独立解密远端实际产物，
没有执行 prelink 或装入 P18，也不证明实机平台匹配、GIC 或 timer IRQ。
前面的单核 ADT 输入检查与该源码编译分别成立，尚未组成实机验证结果。
