# 当前诊断与前序入口准备记录

## 当前：V21 菜单和 PMIC 基线已确认

截至 2026-10-25，最新进展见 [V21_RESET_DIAGNOSTICS.md](V21_RESET_DIAGNOSTICS.md)。
V21 已通过主机构建与镜像审计，刷入 `boot_b` 并重启。照片确认放大的 V21 菜单和
`PMIC RESET: 0x02000001`，尚未观察本版 XNU 启动尝试。

下一步录制第一项 XNU 诊断的启动过程、最终文字与颜色条，区分是否重新出现 Lenovo Logo
和是否只返回菜单。当前 PMIC 值仅作启动历史基线，不作为外部 watchdog 的确诊。
`board.toml` 仍保留 V17 payload 设置；V21 独立固件配置与哈希见最新汇总。

## 前序：V13–V15 准备记录

以下内容描述当时的 V13–V15 准备状态；后续 V16–V21 结果以各版本文档及最新汇总为准。
V15 ADT与真实入口诊断准备见 [V15_ENTRY_DIAGNOSTIC.md](V15_ENTRY_DIAGNOSTIC.md)。

V14 的内存窗口与返回报告已真机验证。最新结果见 [V14_MEMORY_WINDOW.md](V14_MEMORY_WINDOW.md)。
以下 V13/TWRP 部分保留为前序记录；V14 已完成其后续条件中的单窗口分配、读写和释放验证。

## 已完成

- V13 UEFI 内存勘测固件在 Ubuntu 编译完成，启动头 v2、固件卷、APRIORI、FAT
  和外部 ARM64 EFI 的独立产物检查通过。源码位于配套 uefi-j716f 的
  `Platforms/Lenovo/j716fPkg/Applications/J716fMemoryProbe`。
- macOS 源码构建及加密产物保存成功：
  [Actions 40100682659](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40100682659)。
  用户已下载加密 artifact，本地解密及四份产物的大小/SHA256 校验通过。
- 本地 IPSW 已确认为 iPad7,1 / j120ap / 17.7.11 / 21H461。解压的 ARM64
  kernelcache SHA256 与官方 sheng 配置相同；纯元数据在
  `research/ipsw-j120ap-21H461.json`。Apple 二进制只留在本地。
- 已取得官方 UniXNU-sheng `251b291adf941b9953fe3ae66a808b9b626e164d` 的入口
  探针源码供后续移植。没有将 sheng 的 MMIO、ADT 或内存窗口当作 J716F 数据。
- 固定 NativePrelink 工具已生成离线实验候选：30 个可执行组件、1 个无代码组件、
  7 个内核接口，69,294 处指针重绑定，260,779 处 PC 相对编码检查通过。
  组件选择、slide 和 AMFI 初始化地址取自相同 kernelcache 的 sheng 参考配置；
  这不是已验证的 J716F 驱动集合。
- 在 Ubuntu 上为候选副本生成入口色条及 panic 停屏探针（`--panic-screen --no-reset`）；
  实际指令、空洞及修改范围检查通过。完整内核负载和真机启动仍未验证。
  纯元数据报告见 `research/offline-prelink-40100682659.json`；二进制只保存在本地。

## V13 内存勘测结果

2026-10-24，V13 已刷 boot_b 并取得摘要、三页内存图及 EFI_SUCCESS 返回照片。
49 个描述符中仅 2 个 Conventional，最大空闲区从 `0xC0000000` 开始、大小为
`0x0C180000`（193.5 MiB）；另一个仅 `0x6000` 字节。两者被 LoaderCode 隔开。
因此当前内存图不能满足固定 DarwinBoot 的最低窗口。
数字转录及静态容量分析见 `research/v13-memory-survey.json`。

当前板级静态表仅声明 `0xC0000000 + 0x0E000000` 的 DXE 区域（224 MiB）；
固定 DarwinBoot 默认窗口为 256 MiB，ADT 自定义窗口的最小值也是 256 MiB。
实际可分配范围必须以真机 `GetMemoryMap` 为准。尚未指定内核物理窗口。

当前预链接候选的内核基址余量为 `0x7c000`，也必须计入分配要求。
旧 RAM bank / Linux unlabelled span 记录来自 HyperOS；同版本 ZUI 原厂静态 DTB
的 `/memory/reg` 为全零占位，均不能证明本次新窗口可用。下一阶段需取得当前
引导链填充的实际 RAM 分区数据，结合原厂及现有 UEFI 保留范围，再发布更多空闲 RAM。

### TWRP 运行时 RAM 证据已取得

随后在 TWRP 3.7.1_12-0 / Linux 4.19.157-perf+ 的 root ADB 中只读取得 FDT、
live device tree、`/proc/iomem` 和 dmesg。实际 RAM bank 为
`[0x80000000, 0xBCC00000)` 与 `[0xC0000000, 0x200000000)`；live tree 与 FDT
的内存及保留区属性一致。这是新的运行时采集，不是沿用旧 HyperOS 记录。

交叉核对原厂固定 DTB、TWRP 固定保留区、iomem/CMA 和当前 V13 固件范围后，
`[0xD0000000, 0xE8000000)`（384 MiB）可作为下一版 UEFI 的实验候选。
当前内核对齐下的 256 MiB 试分配范围可为 `[0xD007C000, 0xE007C000)`。
纯元数据见 `research/recovery-memory-20261024.json`。

TWRP 使用 DTBO 10，原厂 ZUI 为 DTBO 8；这些证据还需独立 UEFI 分配和读写验证。
本次未修改固件或刷写分区，也没有直接读取 SMEM RAM 分区表。下一步准备独立
V14 内存窗口验证：保留现有固件区域，只新增有依据的有界范围，执行分配/读写/
释放和停屏结果，先完成 UEFI 内存验证。

V13 返回界面的 V12 标题来自沿用的 loader trace 库；该库查询 V12 报告 GUID，
因此独立 V13 内存应用返回后显示 report not found。GetMemoryMap 已由照片验证，
返回状态为 Success；后续版本将同步返回界面与内存探针报告格式。

V13 保持 V12 的固件内存描述、显示和按键基线。第一项启动外部 EFI，显示：

1. conventional memory 总量、最大 2 MiB 对齐范围及其起点。
2. 分页内存图：类型、物理地址、大小、属性。
3. 每页停屏等待拍照；短按电源下一页，末页返回菜单。

对齐容量只是上限估计：还未扣除内核基址低位余量，也没有实际分配内核窗口。
本候选没有真实内核，不调用 DarwinBoot Boot 或 ExitBootServices。

## 内核入口候选的后续条件

- 结合真机内存图与原厂保留区审计，决定可分配窗口；必要时先修正 UEFI 内存发布。
- 依据 J716F 平台支持需求调整实验组件选择；当前源码资格配置仍未启用板级硬件驱动。
- 生成 J716F ADT，并审计启动参数、framebuffer、内存和探针位置。
- 将已静态检查的探针候选与板级数据组合，再生成独立入口诊断镜像。

源码资格配置关闭 CPU/ADT 平台等硬件驱动；编译通过不代表这些驱动已适配。
`boot-progress` 也未编入当前配置，单独添加启动参数无法启用它。
物理 UFS、根卷和用户态仍在后续阶段。真机实验范围继续限于 boot_b。
