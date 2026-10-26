# V22：MMU 后 1 秒／3 秒复位对照

更新：2026-10-25。两份镜像均已刷入 boot_b 并取得录像。A/B 品红至黑屏为20.245／19.730秒，各一轮，没有目标1／3秒响应；B 最终进入A槽ZUI。MMU后执行与复位来源未确认。后续见 [V23 独立诊断](V23_INDEPENDENT_PROBES.md)。下面保留原设计与准备步骤。

## 为什么做这组测试

V21 连续录像补充了本版独立证据：约 24.5 秒出现绿、蓝、品红色条，约 44 秒黑屏，
46 秒 Lenovo Logo 再现，48 秒返回新菜单。PMIC 从尝试前的 `0x02000001` 变为
重启后的 `0x02000041`。这里只确认重启时序，不分类复位来源。

从 V21 FD 提取的实际内核带入口／MMU 前探针与 panic 渲染器，没有原位置的
post-MMU 定时复位跳转。不能使用历史 `board.toml` 的 V17 六秒设置解释这次重启。

## 两份独立镜像

| 变体 | 菜单标题 | post-MMU 行为 | boot image SHA256 |
| --- | --- | --- | --- |
| V22A | `J716F V22A - post-MMU reset 1s` | 等待 1 秒后请求 PSCI SYSTEM_RESET | `7c2cce9a188da50f3b060e0ce5b5efb966fa97d5e754e5d8ca88016589f5360a` |
| V22B | `J716F V22B - post-MMU reset 3s` | 等待 3 秒后请求 PSCI SYSTEM_RESET | `d3d8d1a7fed7a954b5506c71c02ca1f11b2605885084de3e1f08a699dbac59d1` |

它们分别包含一份 64 MiB 只读 FAT。内核唯一的字节差异位于计时常量；ADT、启动参数、
固件模块选择、内存表、PMIC provider 和已链接的 UEFI PE 相同。
1 秒／3 秒是探针开始执行后的目标等待时间，不含厂商 Logo 和重新进入菜单的时间。

原始实验预链接内核 SHA256：
`cab8f385a44d21fc6f455cb501227452212ba44d34a9fa03d04fcf7e85324214`。
它与 V21 的原始预链接输入相同。V22 从该未安装入口探针的输入重新制作，未把 V17／V18
或已安装 panic 探针的内核当作新基线。

ADT 保持 V21 实际值：
`b68fc49b2d9bb6b0a387e59a7816fd4848dffc3bde706a439cbbff3cbebb1aa7`。
参数保持 `-v serial=0 debug=0x14e cpus=1 -no_slto_panic`。

## 探针和边界

- 绿色：DarwinBoot 退出 Boot Services 后的交接条。
- 蓝色：XNU MMU-off 入口探针。
- 品红：开启 MMU 前的探针。
- post-MMU 位置 `0xfffffff0073fe394` 跳转到定时复位代码。
- 定时器使用已记录的 19.2 MHz 时基，随后执行 PSCI `0x84000009`、`smc #0`。
- PSCI 返回时停在 WFE 循环；这也需要与外部复位区分。
- 入口保留区不能同时容纳当前 panic 渲染器与定时探针，所以 V22 不安装 panic 渲染器。
  本版在检查点主动终止，不用于验证后续平台初始化；V21 保留作 panic／无定时探针基线。

## 源码和构建配方

使用 UniXNU `42bffe8a56823dad2e448c776b4f94862f3e290a` 的共享
`scripts/patch-xnu-entry-probe.py`，固定工具 SHA256 为
`159eb1ea8d6c1da30f26913a281f46e21d940a1ae7886656529907ec3edf96ad`。

在具备 LLVM、mtools 和匹配私有输入的主机上，从本设备仓库运行：

```sh
python3 scripts/build-v22-payload.py --unixnu /path/to/pinned-UniXNU \
  --baseline /private/kernel.prelinked.experimental \
  --adt /private/v21-kernel.macho.adt --output /private/v22a --variant a
python3 scripts/build-v22-payload.py --unixnu /path/to/pinned-UniXNU \
  --baseline /private/kernel.prelinked.experimental \
  --adt /private/v21-kernel.macho.adt --output /private/v22b --variant b
```

每个输出目录必须尚无 FAT，脚本拒绝覆盖已有 FAT。生成 payload、探针构建日志和哈希记录。
固件使用配套仓库新增的 `j716f-entry-v22.dsc`、`j716f-entry-v22.fdf`、冻结的
`ENTRY_V22_*` 清单和独立 `J716fV22TraceUniXNULib`。每次将对应 FAT 放到
`Platforms/Lenovo/j716fPkg/Resources/J716fEntryV22.fat` 后构建并立即保存该变体 FD。
详细命令见配套固件仓库的 `docs/V22_DIAGNOSTICS.md`。

`board.toml` 保留历史 V17 配方；本文件、构建脚本和
[v22-checkpoint-preparation.json](../research/v22-checkpoint-preparation.json) 明确描述 V22。
标准 Android initrd 集成、CPU／平台驱动适配与本次检查点实验分别验证。

## 主机验证

两次固件构建成功；独立检查探针精确指令、计时常量、修改边界、内核布局；重新解析最终 FD
中的 FAT、簇链与文件身份；核对远端 FD、FV／FFS 校验和、AARCH64 PE、链接文件身份和
APRIORI 顺序。模块 GUID 集合与 V21 相同。Android v2 header、BootShim、gzip、两份
固件 DTB、SHA1 ID、填充、长度及 `boot_b` 容量边界均通过主机检查。

## 真机测试

1. 严格只刷 `boot_b`。先测 A，确认菜单标题含 `V22A` 和 `1s`。
2. 从菜单开始连续录像，选择第一项启动；之后松开按键。
3. 记录品红条首次出现至黑屏的间隔，以及是否出现 Lenovo Logo、最终菜单标题和 PMIC。
4. 再用相同过程测试 B，确认菜单标题含 `V22B` 和 `3s`。
5. 只有结果可重复且复位时间随 1 秒／3 秒设置变化，才将其记为本版 post-MMU 探针证据。
   若仍约 20 秒或其他固定时间复位，不能认定到达了该探针；继续调查异常／外部复位。

当前没有 V22 真机结论。持续 MMU-on、原生平台驱动、UFS／root 和用户态仍未验证。
