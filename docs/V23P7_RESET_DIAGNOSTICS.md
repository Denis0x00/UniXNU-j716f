# V23P7 RESET r1

## 当前结论

V23P6 已实机验证 console 标记追加和同次运行读回；Warm restart 调用返回，
没有观察到重启，因此跨复位保留仍未验证。P7 是已完成主机校验的独立 UEFI
诊断应用，已仅刷 boot_b；三张实机照片确认 inventory 和三项 PSCI 查询返回，
随后的视频确认两次直接 RESET2 均返回 INVALID_PARAMS（-2），未观察到重启；
跨复位保留未验证，不包含 XNU。

整体仍在上游 bring-up **阶段 5：内核入口的早期诊断**。UEFI、UniXNU 菜单
及早期内核入口已有历史证据；持续 MMU-on、平台专家、存储根卷和用户态未验证。
P7 回到固件环境隔离复位和日志问题，不能作为 XNU 新检查点。

## 从实际 P6 镜像确认的路径

- 实际 FD 中 `ResetSystemRuntimeDxe` 与构建产物逐字节一致，4608 字节，
  SHA-256 `d78eceecd3a9d45a5a39d2b30b32735b4e376dbf51caf4a68c572577ea5908e6`。
- 驱动 GUID：`4b28e4c7-ff36-4e10-93cf-a82159e777c5`；
  `RuntimeServiceResetSystem` 的 RVA 为 `0x10540`。
- `PcdMonitorConduitHvc=0`，实际执行 `SMC #0`。
- Warm 路径先用 `PSCI_FEATURES (0x8400000a)` 查询
  `SYSTEM_RESET2 AArch64 (0xc4000012)`；返回 0 时调用 RESET2 type 0，
  否则回退 `SYSTEM_RESET (0x84000009)`。
- 源码及机器码显示原库没有整体初始化参数结构。主机以 A5 填充栈并模拟两条
  Warm 分支，FEATURES 的 x2..x17 来自栈。**这只是主机发现，尚未证明是
  P6 返回的原因**。运行时 `gRT->ResetSystem` 的实际归属也要实机核对。

## P7 改动与限制

P7 与已刷 P6 相比，只有 embedded FAT 的配置与应用改变。所有 inner-FV
非载荷模块 section、APRIORI 和 compact SEC 可执行代码逐字节一致；仍使用
P6 的 1 MiB Boot Log 保留区、256 KiB console、字体 scale 3 和原复位驱动。
原驱动没有被全局修改，可保留旧路径对照。

默认进入应用仅做只读 inventory：CurrentEL、ResetArch/ResetNotify 协议、
ResetSystem 指针、LoadedImage owner GUID/base/RVA。只在指针属于该 loaded
image 且范围足够时读取 16 字节前缀，与 P6 的 GUID/RVA/前缀比较。
这个匹配用于辨认函数，不是完整完整性校验。

| 手动选项 | 动作 |
| --- | --- |
| Refresh inventory | 只读，无 RAM 写入、无 monitor 调用 |
| Query PSCI + record (NO RESET) | 查询 VERSION、FEATURES(SYSTEM_RESET)、FEATURES(RESET2 A64) |
| EFI Warm restart + record | 调用现有 `gRT->ResetSystem(EfiResetWarm)`，记录返回情况 |
| Direct RESET2 + record | 完整查询已返回且 RESET2 FEATURES 低 32 位为 0 才允许 |
| Direct SYSTEM_RESET + record | 完整查询已返回且 VERSION 低 32 位非负且至少 0.2 才允许 |
| Return | 正常 EFI 应用返回，不表示复位成功 |

直接 SMC 输入 x0..x7 先整体清零，再设置 FID/必要 x1；汇编显式清零
x8..x17，保存本函数使用的 ABI 状态及平台寄存器 x18，捕获 x0..x7 返回值。
无 counter 等待、HVC 回退、PMIC/SCM/vendor 复位路径。

每次调用前及返回后向已验证有效的 console 追加记录并执行 DSB、同次读回。
非法头或读回失败阻止后续调用；不初始化或清空日志。满环时覆盖最旧的记录长度
字节。序号由本次进入时可见记录数派生，只是关联线索，不保证跨回绕全局唯一。
记录含版本、步骤、FID 和 x0..x3；屏幕显示各次查询的 x0 和最后返回的 x0..x3。
PRE 表示准备调用，不能单凭它证明 SMC 已执行；缺失 RETURN 也不能单独证明复位。

## 主机检查

- 两轮 AARCH64 / CLANGPDB RELEASE 编译和最终 FD/FAT/source hash 校验通过。
- GCC ASan/UBSan ring 边界检查通过，Ring.h 与 P6 完全相同。
- 原驱动两条 Warm 分支的栈哨兵模拟完成。
- 新 wrapper 五组模拟调用验证零参数、返回值和 ABI/栈恢复；实际 EFI 中对应
  map 符号的函数体与受测汇编逐字节匹配。
- 执行实际链接的 Call/Record/PrintLib/ring：有效头时 SMC 前后两条记录可读；
  非法头时无写入、无 SMC。仅显示函数与 SMC 响应被桩替换。
- Android header、FD、shim、DTB、FAT 文件清单和无自动启动配置检查通过。

本地复核命令（私有工作区工具）：

```sh
python tools/check_j716f_v23p7.py
python tools/package_j716f_v23p7.py
python tmp-unixnu-upstream-20261025/scripts/porting-record.py check --device github-export/UniXNU-j716f
```

镜像 `j716f-mu-zui13-reset-v23p7-boot.img`，1,699,840 字节，SHA-256：
`919cd695ac28b4eada47b663ee3724575aaefaa5a8778a71f0c2c4581527f23e`。
精确源码/FD/app 哈希保存在私有构建 manifest；脱敏结果见
`research/v23p7-reset-probe.json` 和固件仓库 `docs/v23p7-validation.json`。
当前源文件是记录仓库 base 上的未提交新增，不将 base 提交当作 P7 源码提交。

## 首轮实机：只查询

1. 平板进入 fastboot，录像就绪。核对设备/镜像后仅刷 `boot_b`、激活 B 槽，
   核对当前槽和可启动状态后启动。
2. 选择 P7 reset probe，先拍下完整 inventory 页面及 RAM VALID 状态。
3. 音量键选择 **Query PSCI + record (NO RESET)**，电源键执行。
4. 拍下 VERSION、两项 FEATURES、最后 x0..x3 和状态行，再提供照片或录像。
   本轮停在查询页面，不选择三个复位选项。

后续按真实返回值安排单一复位路径对照。实际重回菜单后先检查旧记录，再进入
TWRP 立即只读采集 console，避免无关启动日志覆盖。P7 解析命令：

```sh
python scripts/decode-j716f-reset-probe.py <private-console> --manifest <private-p7-manifest> --image <private-p7-boot.img>
```

解析器核对所提供镜像与 P7 manifest；记录本身没有嵌入完整镜像哈希，仍需用
录像和操作顺序建立来源。标记缺失不能单独定位哪个启动阶段清除了 RAM。

待 **真实复位、UEFI 读回、Recovery 独立读回** 成立后，再把无栈直接记录
接到 XNU 的 MMU 前后，验证物理地址/虚拟映射和寄存器保存，然后推进平台专家。
自动启动循环继续等待稳定 reader 与返回路径。始终只测试 `boot_b`。

## Flash operations

V23P7 RESET r1 (uncommitted source additions on the recorded repository base; exact source/image hashes in the P7 manifest) was flashed only to boot_b through fastbootd. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Inventory/PSCI-query observations await video. No reset trial, retention result or new XNU checkpoint is established.

## 首轮实机照片：PSCI 查询返回

三张照片确认正确 P7 菜单、初始 inventory 和查询结果：

| 项目 | 屏幕读数 |
| --- | --- |
| CurrentEL | EL1 |
| ResetArch / ResetNotify / ownerFound | 1 / 1 / 1 |
| 驱动 GUID/RVA/16-byte prefix match | 1，RVA=0x10540 |
| 初始 console | VALID，next=size=0x2923f，P6=0，P7 entry=0，now=0 |
| VERSION | x0=0x00010001，即 PSCI 1.1 |
| FEATURES SYSTEM_RESET | x0=0，status32=0 |
| FEATURES RESET2(A64) | x0=0，status32=0 |
| 完成后 console | VALID，next=size=0x2958a，P7 entry=0，now=6 |
| 最后 FID / 返回 x0..x3 | 0x8400000a / 全 0 |
| 状态行 | Queries returned and recorded |

这证明本镜像的 EL1→SMC 查询路径能返回，固件报告两种复位接口可用；六条记录
与三个查询的 PRE/RETURN 数量一致，头增长 843 字节。这里只是应用同次读回
和照片证据，还没有 Recovery 原始日志。版本/返回值编码参照
[Linux PSCI 头文件](https://github.com/torvalds/linux/blob/master/include/uapi/linux/psci.h)。

FEATURES 成功不等于实际复位已完成，也不能证明原库的未初始化输入就是失败
原因。初始 P6=0 经历了中间操作，没有受控的完整保留链路，不据此定位丢失阶段。

下一次在同一 P7 应用中手动选择 **Direct RESET2 + record**，保持录像。
若实际重启并回到菜单，重新打开 P7，先拍只读页的 P7 entry/now、VALID、next/size，
不要先 Query；若调用返回，拍下 RETURNED 状态和 x0..x3。本次先不混用 EFI Warm
或 SYSTEM_RESET。跨复位保留仍需把这次操作与重入和 Recovery 独立读回关联。

## RESET2 实机结果：两次均返回 -2

约 10.50 秒视频与照片确认，直接调用 `0xc4000012`（type=0、cookie=0，
其余未用输入清零）返回 `x0=0xfffffffffffffffe`，x1..x3=0。
末位经放大确认是 **E**，应解码为 **INVALID_PARAMS（-2）**。
两项 FEATURES 仍显示 0；这表示固件报告函数可用，但本次具体请求被拒绝。
不能把 FEATURES=0 当成实际复位成功。

| 画面 | console next=size | P7 now | 观察 |
| --- | --- | --- | --- |
| 视频开始 | 0x2958a | 6 | 上轮查询完成，RESET2 选中 |
| 约 3 秒 | 0x296a3 | 8 | 第一次 RESET2 返回 -2，RETURNED 状态 |
| 约 4 秒至结尾、独立照片 | 0x297bc | 10 | 第二次 RESET2 返回 -2，RETURNED 状态 |

每次增加 281 字节和两条记录。视频持续停留于同一应用，没有观察到画面重启、
Lenovo 启动画面或应用重入。没有实际复位，因此不能从这次试验判断 RAM
跨复位保留。原始 console 尚未由 Recovery 独立读回。

旧 ResetWarm 实现只在 FEATURES 查询失败时回退 SYSTEM_RESET；FEATURES 成功后
RESET2 返回错误时没有再次回退。这提供了 P6 Warm 返回的一个具体机制解释，
但尚未采到 P6 内部那次 SMC 返回值，不能认定历史调用已被逐步复现。
全零参数的直接 RESET2 仍失败，因此仅修旧结构的初始化没有实机成功依据。
也不能据此宣称整个 PSCI 或 SYSTEM_RESET 不可用。

下一次无需重新刷机，在当前应用中只选择 **Direct SYSTEM_RESET + record**。
若重启，返回菜单后打开 P7，先拍只读页面的 entry/now、VALID、next/size，
暂不再次 Query；若返回，拍 RETURNED 和 x0..x3。随后再根据真实复位类型和
读回结果安排 Recovery 验证。此时不修改旧固件的全局复位库。

返回值定义见 [Linux PSCI header](https://github.com/torvalds/linux/blob/master/include/uapi/linux/psci.h)。
[TF-A RESET2 参考实现](https://github.com/ARM-software/arm-trusted-firmware/blob/master/lib/psci/psci_system_off.c)
仅用于区分函数能力查询与具体请求处理，不作为 J716F secure firmware 身份或内部路径证据。

## SYSTEM_RESET user report and reentry gate

User reports the first P7 Direct SYSTEM_RESET rebooted, then selecting it after reentering the app did not reboot. No new SYSTEM_RESET photo/video or post-reset ring counts yet. Fresh app load clears mQueried; without rerunning Query, the action is blocked before SMC. Second-attempt gate status versus actual returned SMC is unresolved. Preserve full page/entry/now before new queries. One reboot is user-reported; repeatability and RAM retention remain unverified.

## SYSTEM_RESET clarification and controlled retention retry

User clarified the second no-reboot selection displayed blocked; rerunning Query then Direct SYSTEM_RESET rebooted again. Two successful SYSTEM_RESET invocations are user-reported; no SYSTEM_RESET error return was reported. User then forced power off and entered ZUI13 before post-reset inventory capture. Earlier reset retention remains unknown; later marker state cannot isolate that path. Repeat a controlled P7 boot_b cycle from a photographed baseline, without forced shutdown or ZUI detour. No new flash/source/device commands performed.

Controlled P7 retention cycle relaunched: fastboot initially reported current-slot a and slot-unbootable:b=yes. set_active b succeeded; current-slot b and slot-unbootable:b=no verified; reboot command succeeded. No partition image was flashed. Prior P7 flash/image hash supplies expected identity, not a new partition readback. Await menu identity, baseline and post-SYSTEM_RESET read-only photo; retention remains unverified.
