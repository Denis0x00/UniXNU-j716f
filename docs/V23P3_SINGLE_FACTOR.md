# V23P3：遗留写入与标题屏障的独立对照

日期：2026-10-25。以真实 V23P2 内核为固定对照，保留 P2 的源码、镜像和实测记录。
两个变体都是单核、MMU 前终止诊断，不继续正常 XNU 启动；硬件结果待测。

## 改动矩阵

| 版本 | 入口 BCM 写入 | 标题行末尾 | 其他文字行末尾 |
| --- | --- | --- | --- |
| P2 对照 | 原 `str w1,[x0]` | DSB SY | DSB SY |
| P3A | 仅该指令替换为 NOP | DSB SY | DSB SY |
| P3B | 保留原写入 | DMB SY | DSB SY |

P3A 修改 `0xfffffff0073fe32c` 的四字节指令：原写入目标是
`0x40000040 + (MPIDR & 0xff)*4`，值为 `0xf0`。
地址和值来自本次封装内核的静态追踪，不表示已确认该访问造成停点。

P3B 的标题与其他行共用 draw_line，所以不能直接将共享 DSB 全部改成 DMB。
将 `0xfffffff0073fca54` 改成无链接分支，进入原探针尾部的 20 字节分派器：

```text
CBNZ X1, non_title   // row 0 is the title; preserves flags and registers
DMB SY
B original_continuation
non_title: DSB SY
B original_continuation
```

分派器不使用栈，不改变通用寄存器、NZCV 或 LR；位置在已验证的原内核代码洞中，
没有请求新内存。P3B 是标题屏障语义对照，不能将绕过 DSB 直接当作平台修复。

两者都在全部代码修改后重做 source crypto seal。审计将改动和 HMAC 还原后，
要求内核 SHA256 与固定 P2 完全相同；P2 本身还通过原 baseline/入口/异常向量审计。
P3A 探针保持 1824 字节；P3B 为 1844/2800 字节。

## 版本识别与读数

- 菜单清楚显示 V23P3A 或 V23P3B，以及对应对照描述。
- 探针首行仍是 `V23 ...0005`，方块位置、字体、启动参数、ADT、PSCI 路径保持 P2 行为。
  不用修改诊断模式值来区分版本，以免额外改变执行代码。
- 方块 1/2 含义与 P2 相同；方块 3 在 A 表示经过原 DSB，在 B 表示经过标题 DMB 分派器。
- 方块 4/5 为读取 CurrentEL 前/后；6/7 为读取 SCTLR 前/后；8/9/10 为后续行返回。
- 方块 10 后仍有原 SMC 前 DSB；方块 11 表示 SMC 返回，12 为异常入口。
- 如果 B 出现方块 5 但缺少 6，应检查保留的 EL 行 DSB 边界及此前访问；
  这与仅两格的 P2 具有不同的诊断意义。
- 方块缺失不等同于架构 PC 轨迹；重启、PMIC 值与持续时间不独立证明 PSCI 或 watchdog。

## 构建

在具备 ARM64 clang/LLVM、mtools 和固定私有输入的 Linux 构建环境中：

```sh
python3 scripts/build-v23p3-payload.py \
  --baseline /private/baseline.macho --adt /private/kernel.macho.adt \
  --sealer /path/to/UniXNU/scripts/seal-source-crypto-kernel.py \
  --output /private/output/v23p3a --variant p3a
```

另用 `--variant p3b` 和独立输出目录构建 B。先重建固定 P2 控制内核并核对哈希，再施加唯一因素。
原始 baseline、ADT、内核、FAT、FD、镜像和日志不提交公开仓库。
固件仍使用独立 V23 DSC/FDF；本次不更改平台模块、内存映射或标准 initrd 集成。
`board.toml` 仍保留历史 V17 配方，不能用它直接复现 P3。

## 验证与硬件顺序

主机应检查真实 FD/FAT 内核差异、HMAC、全部非 payload 固件模块与 P2 的一致性、
Android-v2 header、BootShim、gzip、双 DTB、填充和容量。
模拟执行实际绘字、方块和 B 分派器，并区分标题/非标题屏障；系统寄存器、SMC 和异常仍受控替代。
这些检查不能证明硬件 MMIO 完成、屏障完成或 PSCI 成功。

先测 A，保存从菜单、启动到重启/停留的完整录像，再根据结果测 B。
操作前读取当前槽位、B 的 successful/unbootable/retry 信息（接口不支持时明确记录）。
仅允许 boot_b；boot_a、vbmeta、userdata、GPT 和 TzDxe 不在本轮范围内。
P3A 已通过 fastbootd 仅刷 boot_b，激活 B 并发送重启成功；录像和内核结果待复核。P3B 未刷入，没有 9008 操作。

公开结果索引：`research/v23p3-single-factor.json`。历史依据：
[V23P2](V23P2_PREFIX_CHECKPOINTS.md)、[V23 独立探针](V23_INDEPENDENT_PROBES.md)。

V23P3B was flashed only to boot_b via fastbootd, B activated and reboot command succeeded. User report for P3A was brief screen then reboot; no checkpoint or PSCI result established. P3B video is pending.

V23P3A 视频已确认菜单身份；约1.7秒后画面变黑，未见诊断方块或EL/SCTLR，PSCI与复位源仍未知。

V23P3B 视频确认标题 mode 5 和方块 1/2；方块 3、EL/SCTLR 未见。约20.9秒后画面消失，随后 Lenovo/Android 启动并回到 UniXNU 菜单。标题 DMB 对照未显示出越过第2格的硬件效果，PSCI/复位源仍未知。
