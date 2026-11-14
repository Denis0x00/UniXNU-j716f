# 本机硬件基线

证据日期：2026-11-11（Asia/Shanghai），批次 `20261111-231708`。
原始证据保存在工作区 `analysis/device/20261111-231708/`，从本目录根相对
路径为 `../../analysis/device/20261111-231708/`。本文件仅转录与移植有关
的事实；`source-provenance.json` 记录输入文件 SHA-256。没有转录序列号、
MAC、磁盘 GUID 或身份 seed。本机 Android 硬件枚举不等于 iPadOS 验证。

## 启动身份、内存和 CPU

- 设备 TB-J716F，SoC SM8250，当前选择 `ro.boot.dtb_idx=0`、`ro.boot.dtbo_idx=1`。
- 6 GiB RAM 级别；FDT 两段为 `[0x80000000,0xbcc00000)`（972 MiB）与 `[0xc0000000,0x200000000)`（5120 MiB），合计 6092 MiB。52 MiB 洞不可补为 RAM。
- 固定 no-map 归并区间为 `[0x80000000,0x80600000)`、`[0x80700000,0x80894000)`、`[0x80900000,0x8681c000)`、`[0x86900000,0x90500000)`、`[0xb0000000,0xb0080000)`；动态 CMA/mailbox、显示保留另见原始审计，不能简单相减获得 XNU 可用内存。
- 高 bank 没有运行 FDT 的固定 `reg` 保留节点，但扣除固定 no-map 后与 Android `iomem` 对照，另有 `[0xffc00000,0xffc20000)` 的 128 KiB 空洞。其大小与 size-only mailbox 请求一致，尚不能证明归属或跨启动固定性；不能据此宣称高 bank 全部可交给 XNU。
- stock boot-log 只有 512 KiB。旧移植的 1 MiB 描述及更高偏移诊断区不能直接继承。
- 上游 `scripts/unixnu/devicetree.py` 不将 `/reserved-memory` 转入 ADT，因此草稿的 `no-map` 不会自动约束 XNU 分配器。还须核对新 UEFI 的 heap/FD/guard、DMA 和 loader 分配，通过实际 EFI memory map 与 DarwinBoot owned-window 交接落实排除范围；单补 DTS 保留节点不构成有效保护。
- CPU affinity 为 `0,0x100,0x200,0x300,0x400,0x500,0x600,0x700`，Android 全在线。硬件/调频分组为 4+3+1；上限分别 1804.8/2419.2/3187.2 MHz。
- generic timer 19.2 MHz，运行 DT 的四组 PPI 为 `<1 13 0xff08> <1 14 0xff08> <1 11 0xff08> <1 12 0xff08>`。它使用系统寄存器；不能把 GICD 的地址当成 timer MMIO。
- GICv3 distributor `0x17a00000/0x10000`，redistributor `0x17a60000/0x100000`，stride `0x20000`。XNU 中断路由和 SMP 未验证。

## UFS 与分区

- KIOXIA **THGJFAT0T44BAILB**，UFS 3.1，manufacturer `0x0198`，6 个普通 LUN；逻辑/物理块为 4096 字节。
- geometry descriptor 容量 127,984,992,256 字节；六个可见逻辑盘合计 127,951,437,824 字节。报告为 128 GB 级，不把 LUN 合计当原始 NAND 容量。
- 六组主备 GPT CRC、entry-array 一致性和边界检查通过；关键分区 GPT 与 sysfs 几何一致。该结果不能证明分区内容或旧镜像正确。
- boot A/B 各 100,663,296 字节，recovery A/B 各 104,857,600 字节，dtbo A/B 各 25,165,824 字节。
- userdata 在 sda18，LBA 2315048..30693370（包含末 LBA），每块 4096 字节，容量 **116,237,611,008 字节**。
- 主机 tools 中旧 userdata 脚本的容量注释写成 116,236,791,808；其 LBA 表达式实际算出上面的正确值，注释少了 819,200 字节。
- 已知 UFS 控制器绑定 `0x01d84000`。DMA/SMMU/PHY/clock 交接和 XNU 读取仍未验证；本草稿没有启用 UFS root 或 Data 写入。

## 显示、触屏和 GPU

- 本机 bootargs 选 **EDO RM692E6 AMOLED command mode**，双 DSI；DRM 合屏 **2560x1600**，公布 90/60 Hz 两种模式。DT 单侧 1280 像素不能误写为整屏宽度。
- **Goodix GT7386**：驱动 PID 7386、VID 00b10023、SensID 08，driver v1.1.1.0。I2C7/0x5d 对应 GENI MMIO `0x00a94000/0x4000`，GPIO36/37 总线、38 reset、39 IRQ，运行驱动绑定和 input 一致。
- 手指 input 10 slots；Android X=0..1600、Y=0..2560。DT 原始 max 2560x1600，swap-axis=1/x2x=1；XNU 四角方向仍需实测。
- 已注册 Goodix 笔接口，pressure 0..4096、tilt +/-90；未测试实际笔/多指能力。
- ST FTS 0x49 仅是 DT 候选，没有 driver symlink。RM69380/BOE 默认 panel 节点也不是当前启动选择。
- Lenovo TouchPad for Tab P11 Pro：独立 I2C HID `04f3:3164`，I2C5/0x60。键盘 I2C2/0x61 已绑定，不能由虚拟 qwerty input 名称宣称实体键盘已测试。
- GPU sysfs 为 **Adreno650v3**，305..670 MHz；DT chipid `0x06050002`。XNU GPU/Metal/电源恢复未验证。

## 无线、音频、传感器、电源

- WLAN endpoint `0000:01:00.0`，PCI `17cb:1101`，cnss_pci 绑定且 wlan0 指向该设备；运行 DT compatible=qcom,cnss-qca6390，确认 **QCA6390**。`17cb:010b` 才是根桥。
- 蓝牙运行 DT 配置 qca,qca6390，Android 属性 hastings；没有独立 HCI ID/版本或收发验证。
- **WCD938x** codec、APR/Q6/Bolero 有实际绑定；两个 SoundWire slave 是 RX/TX 端点，不代表两颗 codec。
- 四个 **CS35L41**，I2C2/0x40..0x43；**ES7210**，I2C4/0x40；均有驱动绑定。已加载 wsa881x 模块不能证明该功放实际存在。
- Sensor HAL 报告 BOSCH **bmi220** 加速度/陀螺、MEMSIC **mmc56x3x** 磁力计、Lite-On **ltr308** 环境光、amsAG **tmf8801** 接近、ROHM **bu52053nvx** 霍尔。名称按 HAL 原文保留；45 h/w sensor handles 包含融合/唤醒变体，不是45颗芯片。
- **BQ27541** I2C6/0x55 -> bq27xxx-battery；**SMB1390** I2C4/0x10、**HL6111R** I2C4/0x25、**FSA4480** I2C4/0x43 均绑定。SMB1390 slave@18 的 DT 状态为 disabled。
- PM8150/PM8150B/PM8150L ADC 已在 IIO 注册。SMB5 DT 为 ok；电池主 power_supply 的准确计量路径尚待追踪。
- Li-ion；驱动设计容量 8,600,000 uAh，charge_full 7,850,000 uAh。不是实测容量或精确健康度。
- 旧 NativeBattery 主读 PM8150B FG；新实现需核清它与本机 BQ27541 的职责，不能继承旧“电量正确”结论。

## 本草稿的实现边界

所有硬件事实作为适配输入；组件仅选模板的 ArmDTPlatform、ArmCpu、
SoftwareAES、XnuPrelink、DarwinBoot。本完整草稿没有本机 iPadOS 启动日志或完整启动镜像、
独立 UFS/GPU/HID/audio/network/power 实现或功能验收。DTS只保留最小基础
节点和单个 RAM bank 候选，完整 reservation/UEFI 所有权问题尚未解决。
独立 `work/UniXNU-j716f-menu` 的空 ramdisk 菜单镜像与实机显示结果见下文；
它不包含 XNU/iPadOS 载荷，也不解除本完整草稿的 guard 或验收缺项。

`board.example=true` 在标准 `build`（含 userdata/dry-run 和短命令）、
`bootimg`、`release` 的主体前拒绝构建。它不阻止 `defconfig/fetch/tree/status`、
独立脚本或 `vendor_boot`；Context 仍可能在拒绝前解析/获取输入及写入 state。
此 guard 不能替代对命令作用范围和完整启动输入的检查。

后续执行以 [主机编译与实机启动条件](build-and-boot-gates.md) 分层：
实际 EFI memory map、owned-window 和运行时 timer/GIC 检验属于实机
交接前的输入审查及交接后的观测，不阻止独立主机目标先生成 SDK、编译
记录和 native kernel。完整 native ABI 验证依赖这些产物，不能提前当作
首次编译门槛。本完整草稿的 guard 保留，主机专用目标尚待建立。

## 早期主机检查快照（2026-11-12，历史记录）

以下是主代理已采集并保存的主机结果，本次文档更新没有访问 VM 或设备：

- VM 为 Ubuntu 24.04.4 LTS、x86_64；此快照采集时官方 Linux 交叉工具链仍在编译，后续完成状态见下节。
- VM 的 `porting-record.py check` 退出码 0，输出 `j716f: 14 structured areas checked`；它检查记录结构和覆盖，不验证驱动或硬件。
- VM 的草稿 DTS 编译退出码 0，仍有 `Warning (simple_bus_reg): /soc/timer: missing or empty reg/ranges property`。generic timer 使用系统寄存器，当前无 MMIO `reg` 是有意的；不能把 GICD 地址填回以压掉警告。
- dtc 通过不证明生成 ADT 的消费者 ABI、timer/PPI、UEFI 或 XNU 运行正确；所有 14 个领域仍为 `planned`。

原始结果在工作区 [device-draft-validation.jsonl](../../../analysis/vm/device-draft-validation.jsonl)，
主机与工具链状态说明见 [VM README](../../../analysis/vm/README.md)。
此快照保留当时的检查边界，后续安装退出码与验收结果已另存。

## 工具链与独立菜单的后续结果（2026-11-12）

官方安装器在 `2026-11-12T01:12:38+08:00` 完成，退出码 0；日志含
`TOOLCHAIN_INSTALL_AND_CHECKS_PASSED`。XNU builtin 与 xcrun 目标选择
检查通过，实际编译的 `arm64-apple-ios17.0` 对象确认为 Mach-O arm64 /
ARM64_ALL / 64-bit relocatable。见 [安装日志](../../../analysis/vm/toolchain-install.log)、
[退出码](../../../analysis/vm/toolchain-install.exit)、
[结束时间](../../../analysis/vm/toolchain-install.finished) 和
[目标对象检查](../../../analysis/vm/toolchain-backend-probe.log)。工具链验收
不能替代完整内核构建或 Apple ABI 输入验证。

后续 VM 官方 fetch 已取得 xnu、libplatform、libdispatch、
AvailabilityVersions、libpthread、corecrypto-2023 和 IOStorageFamily，
七项源的 fetch 命令退出 0；加上先前 edk2，当前 `unixnu.lock` 已锁定
8 项源，实际源仓库 HEAD 均与 lock 一致。
后续 `status --json` 与 `tree` 均退出 0。IPSW info 和分项 fetch 也均
退出 0，已取得 KernelCache、DeviceTree、StaticTrustCache 与
`Cryptex1,SystemTrustCache`；解压后的 KernelCache 为 46,415,872 字节，
SHA-256 `f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7`
与固定 profile 相符。四项输入及三份元数据已备份到 Windows 并再次核对
大小和哈希，没有下载整包 OS 磁盘。
见 [输入准备记录](../../../analysis/offline-input-preparation.md)
与 [输入清单](../../../analysis/vm/offline-inputs-manifest-20261112.json)。
这些是源与原始 Apple 输入准备结果，DriverKit/TrustCache 头恢复、Linux
内核额外输入、内核编译记录、实际 ABI 验证、owned-memory 和完整启动
镜像仍待完成。

DarwinBoot 原始 `test-memory-window.py`、`test-loader-inputs.py` 在
Clang 18 + UBSan 下的非对齐访问失败记录保留；独立副本仅将
Read/WriteUnaligned 测试桩改成匹配生产 API 的 void 指针，保留 UBSan、
非对齐输入和断言后，ADT/window/input 检查与 window-size 编译运行均
通过。生产 Driver 和原 checkout 未修改。此结果不能改写为原版所有
测试通过，更不是设备 EFI 分配或 XNU 交接验证。
见 [内存交接审查](../../../analysis/offline-memory-handoff-review.md)、
[适配副本结果](../../../analysis/vm/darwinboot-host-adapter-20261112.json)、
[原始 window 结果](../../../analysis/vm/darwinboot-host-checks-20261112-r2.json) 和
[原始 loader-inputs 结果](../../../analysis/vm/darwinboot-host-checks-remaining-20261112.json)。

独立菜单目标 `work/UniXNU-j716f-menu` 已完成两版官方构建及
Ubuntu/Windows 产物验收。两版均为 v2/4096 页/空 ramdisk，boot.img 为
1,810,432 字节，FD 为 7,340,032 字节；完整草稿仍无 XNU/载荷/boot 构建。
菜单采用原厂 DTB0+DTBO1 规范化文件，737,306 字节，SHA-256
`6c87f77599a2056befac0080718d826205afed462b9abacac7aa0664e4d3e6e3`；
本完整草稿的 firmware DTB 输入仍未设置。

第一版 boot.img SHA-256 为
`bbd06f9baf0e23004d9e2f0a7fc63a6a0377fde7837a75ff0153e705c57e897a`。
实机照片确认 UniXNU 图形/文字及四项菜单可见，10 秒后默认 exit 返回
EFI_SUCCESS，板级 BDS 按设计显示 `UNIXNU_RETURNED Success - HALTED`。
这是可见菜单与正常返回路径的证据，尚非完整 GOP 参数、帧缓冲所有权或
XNU 显示验收。`input yes` 只说明输入服务存在；实体按键、触摸和输入
稳定性均未验证。空 ramdisk 的 `no boot entry found` 符合本阶段预期；
`drivers 0/0` 是外部配置驱动注入统计，不能据此否定内置固件驱动。
见 [第一版构建报告](../../../analysis/menu-stage-build-report.md) 与
[照片记录](../../../analysis/device/20261112-menu-b/photo-observation.md)。

第二版 boot.img SHA-256 为
`47ed64f01df76a0b6d4f3d329907c3647c2395debeba034dda11fa287a3b7503`。
仅独立菜单目标打开等待按键的诊断配置；有输入服务时不自动超时，无输入
服务时仍使用原 10 秒退化路径。已在 fastbootd 写入 boot_b、选择 B 并
重启成功；尝试记录仍为 `menu_visible=null`、`awaiting_observation=true`，
等待第二版屏幕、实体按键和日志反馈，不能推断第二版已成功显示菜单。
见 [第二版构建报告](../../../analysis/menu-wait-build-report.md) 与
[尝试记录](../../../analysis/device/20261112-menu-wait-b/attempt.json)。

首次写入前已完成 boot/dtbo/vbmeta 双槽备份与哈希核对；两次尝试只改
boot_b 和槽位元数据，没有改 boot_a、DTBO、vbmeta 或 userdata。
Android 恢复路径是 fastboot 选择 A 后重启；B 槽只用于菜单阶段，不代表
其 Android super 内容完整或可启动。实际 EFI memory map、DarwinBoot
owned-window、日志导出与 XNU/iPadOS 交接仍未获得实机验收；菜单显示
不解决 RAM 保留区和内存所有权问题。完整草稿全部 14 个领域保持 `planned`。

## 后续离线源码准备（2026-11-12）

主库同步至 `0e2fb2bcdf11` 后，独立源码检查暴露了原 59 项补丁中的
`zzz-boot-progress.patch` 对未选择 Sheng PMGR 的依赖。当前诊断功能
本来关闭，板级 OS series 因此省略该可选项，保留原有两个 prelink 补丁。
最终 58 项已顺序检查并应用；官方 status/tree 与该顺序一致。

独立 staging 另已恢复 26 个 Apple 公开头文件并核对 Git blob/SHA-256。
原始 kernelcache 的 31 个 roots 闭包、AMFI 可执行范围和 AES 固定地址
完成有限静态核查；原始 AES/AMFI 没有符号名称可作完整方法验证。
这些结果补充上文历史下载状态，不是 SDK/内核编译、native ABI 或硬件
验证。正式 XNU 构建缓存尚未建立，全部 14 个区域仍保持 `planned`。
见 [本轮记录](../../../analysis/offline-readiness-20261112/README.md)。
