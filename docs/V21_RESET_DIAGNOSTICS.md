# V19–V21：复位诊断与当前真机基线

更新日期：2026-10-25。当前设备已刷入 V21 的 `boot_b` 并进入 UniXNU 菜单。
最新照片确认放大字体和 PMIC 数值；本版 XNU 启动尝试仍待观察。

## 版本进展

| 版本 | 已记录结果 | 尚未证明 |
| --- | --- | --- |
| V14 | 一个 256 MiB 窗口通过 7 项检查，缓存维护、两轮读回与释放成功 | 全部 RAM 的可用性 |
| V16 | ExitBootServices、XNU MMU-off 入口与 MMU 开启前探针 | 原生平台初始化和用户态 |
| V17 | 记录了 post-MMU 定时 PSCI 复位检查点 | 持续 MMU-on 运行 |
| V18 | 将 stage 3 改为 WFE 后仍复位并回到菜单 | 复位来源和持久化停点 |
| V19 | UniXNU 菜单与 DarwinBoot 交接前文字；reset protocol 显示 Not Found | PMIC 复位原因 |
| V20 | 加入只读 PMIC provider；看到 `inputs validated: exiting boot services`，之后回到菜单 | 当时照片没有可读 PMIC 值；文字本身不证明完成交接 |
| V21 | 主机构建、镜像校验、刷写及重启成功；照片确认 V21 菜单和 `PMIC RESET: 0x02000001` | 本版 XNU 尝试、最终停点与复位原因 |

V20 菜单仍出现 V19 阶段标签，这是旧 UI 字符串，不能据此判断刷错镜像。
V21 已更新标题、返回页和阶段标签。此前版本的入口证据保持在各版本文档中，
没有将它们当作 V21 的新证据。

## 固件源码与构建

配套固件：[uefi-j716f V21 提交](https://github.com/Denis0x00/uefi-j716f/commit/2c13d875f8b3d38414f61443dc44b9b9559dfcb7)。
实际配置：[j716f-entry-v21.dsc](https://github.com/Denis0x00/uefi-j716f/blob/2c13d875f8b3d38414f61443dc44b9b9559dfcb7/Platforms/Lenovo/j716fPkg/j716f-entry-v21.dsc)。
构建方法与依赖边界见该提交的
[V21 固件说明](https://github.com/Denis0x00/uefi-j716f/blob/2c13d875f8b3d38414f61443dc44b9b9559dfcb7/docs/V21_DIAGNOSTICS.md)。

- V21 菜单字号 scale 4，隐藏 Logo，为结果留出空间。
- 当前实际选用的 Silicium framebuffer 日志库字号从 scale 3 增至 5。
- PMIC 摘要出现在显眼状态行；ASCII 的 `Config.Notice` 用 `AsciiSPrint` 写入。
- provider 读取 PM8150 PON 历史寄存器，清除接口返回 `EFI_UNSUPPORTED`。
- 新公开的 V21 DSC 与真机构建时使用的 DSC 内容逐字节一致；当时构建命令沿用
  `j716f-entry-v15.dsc` 名称，FDF 和输出路径也保留 V15 名称。
- 公开 V15 DSC 的额外标准 initrd 构建声明不在该实际配置中。当前使用独立只读
  内嵌 FAT 与空 Android ramdisk，标准 initrd 集成仍是后续事项。

本仓库 `board.toml` 继续保留 V17 定时复位 payload 设置，不是本次 V21 镜像的完整配方。
V21 的内嵌负载与 V20 字节相同；没有证据证明其等同于 V16/V17/V18 的内核变体。
本次只整理现有结果，不改变内核组件、设备树、内存映射或探针策略。

## 主机校验与镜像身份

| 本地产物 | 大小（字节） | SHA256 |
| --- | ---: | --- |
| V21 Android boot image | 8,331,264 | `88e78f74cbe402fbd9bda0c287e16d3422824d5b51a3ea42a1a441316ebd1efc` |
| V21 UEFI FD | 7,340,032 | `05b5486fc9b75d361b17e93a39bca83871b2b1b926630a6863879eebb8cf5d32` |

已核对 FD 下载前后 SHA256、FV/FFS 头校验和、AARCH64 PE、链接产物与 FD 内嵌字节身份、
V21 编译标记及 PMIC provider。模块 GUID 集合与内嵌 FAT 负载均与 V20 一致。
BootShim ARM64 头、gzip 回读、两份原厂 DTB、Android header v2 参数、镜像 SHA1 ID、
填充和总长度均通过检查。

启动镜像使用 pagesize 4096、kernel `0x8000`、tags `0x100`、DTB `0x1f00000`，
ramdisk 为 0 字节。完整的公开摘要见
[v21-reset-diagnostics.json](../research/v21-reset-diagnostics.json)。

## 真机观察

刷写前再次核对镜像 SHA256，仅写入 `boot_b`。刷写前后槽位均为 `b`，flash 与
reboot 指令返回 `OKAY`。照片显示：

```text
J716F V21 - reset reason diagnostic
PMIC RESET: 0x02000001
drv 0/0 kits 0 acpi 0
```

provider 的打包公式为 `Pon | (Warm1 << 8) | (Warm2 << 16) | (Poff << 24)`，
因此当前值拆分为 `pon=0x01`、`warm1=0x00`、`warm2=0x00`、`poff=0x02`。
尚未分类这些 bit 的硬件语义；这是刷写/重启后的历史基线，不能直接归因之前或之后的 XNU 故障。

照片验证了 V21 菜单、放大文字与 PMIC 读取，没有展示 framebuffer 日志字号效果或 V21 的
DarwinBoot/XNU 尝试。原生平台驱动、持续 MMU-on 执行、物理 UFS/root、trust cache、
userdata 和用户态仍未验证，不能宣称 iPadOS 启动。

## 下一步

1. 选择第一项 XNU 诊断，短按电源启动并录像。
2. 记录最后文字和颜色条；若返回菜单，观察是否先重新出现 Lenovo Logo。
3. 保存最终完整屏幕，核对 V21 阶段标签、PMIC 数值和返回状态。
4. 取得本版入口或复位证据后，再决定下一处有界检查点。

后续真机操作仍严格限于 `boot_b`，TzDxe 不在当前范围。
公开仓库保存源码、哈希和文字结论；生成镜像、原始 DTB、照片、完整日志与构建机资料保留在本地。
