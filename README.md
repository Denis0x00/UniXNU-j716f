# UniXNU-j716f

联想小新 Pad Pro 2021（TB-J716F，SM8250/Kona）的 UniXNU 设备配置与移植记录。
配套固件：[uefi-j716f](https://github.com/Denis0x00/uefi-j716f)；
规范：[上游移植指南](https://unixnu.com/zh/porting/)。

## 当前状态（2026-10-27）

仍处于内核/平台接入阶段，尚未启动完整 iPadOS。OS profile 为 iPadOS 17.7.11 / 21H461。

| 范围 | 当前证据 | 边界 |
| --- | --- | --- |
| UEFI/GOP/按键/加载器 | 多轮实机诊断可用 | 不代表 XNU 显示、输入或存储驱动 |
| 内存 | V14 的 256 MiB 窗口读写；P22 原始 arm_vm_init 返回 | 不代表全部 RAM、回收、长期映射或 DMA |
| P22M | 已完成并封存；六槽 L1/R1，九条重建 CRC，终点 M=1，重启后首次 reader 保留 | 临时日志桥接，有界返回即请求复位；实际复位源未独立证明 |
| 原生平台 | 固定 CI40448429664 已认证接收并通过 XnuPrelink；52,248,576 字节 | 无日志的接收基线不准上机；尚无最终镜像 |
| 可恢复原生日志 | 已接入入口、VM、平台、同步/SP1 异常、真实 IRQ 与调度源代码；双编译器主机检查通过 | 新完整 XNU CI、只读 reader、实机长期映射/保留均待验证 |
| IRQ/调度/UFS/根卷/用户态 | 无本板完成证据 | 不得以链接通过或旧内核函数返回代替 |

[P22 收尾](docs/P22_CLOSED_NATIVE_MAINLINE.md)与[脱敏实机结果](research/p22-closed-20261027.json)
取代旧的“P22 待 Query/ARM”当前状态。历史版本、结果和构建快照全部保留，无需重复 P22。

## 唯一推进主线

固定原生产物 → XnuPrelink → 专用负载/最终镜像核验 + 可恢复早期日志 →
同一候选上的平台匹配、CPU0 注册、GIC、真实 timer IRQ/rtclock/EOI、单核调度。
之后才进入 UFS、根卷与用户态。

[执行与上游差距清单](docs/NATIVE_MAINLINE.md)列出每道门槛和未完成项。
不再默认制造逐函数复位的冻结内核后继版本。

## 构建与候选

默认 `board.toml` 现选择 ArmDTPlatform、ArmCpu 与 native timer，关闭倒计时与旧入口/复位补丁。
旧 V17 配方归档于 `docs/archive/board-v17.toml`；它也不是 P22 的生成配方。
SoC 仍是明确标识的板内过渡快照；未静默升级依赖或选用未资格验证的公共 SM8250 实现。

源码构建仍固定 UniXNU `42bffe8a56823dad2e448c776b4f94862f3e290a`，需要 macOS/Xcode：

```sh
python3 scripts/build-j716f-source.py --unixnu /path/to/pinned-UniXNU --device /path/to/UniXNU-j716f --native-arch-timer --jobs 3
```

[选定 CI 40448429664](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40448429664)
及六产物接收锁见 `configs/native-candidate-lock.json`。接收和预链接分别使用
`scripts/receive-native-candidate.py`、`scripts/prelink-native-candidate.py`。
命令及前提见[原生主线](docs/NATIVE_MAINLINE.md)。

接收器校验加密 artifact 摘要、认证解密、源码/组件版本、六产物和实际 Mach-O。
预链接器使用固定 OS 配方，并检查原生可执行字节、provider 符号与 source POST HMAC。
这些工具不访问设备；选定的旧 CI 原生基线没有可恢复日志，不能仅因预链接通过就上机。

主机回归：

```sh
python3 scripts/test-native-candidate.py -v
python3 scripts/test-source-artifact-transfer.py -v
python3 scripts/test-native-log.py --cc gcc
# 在固定 UniXNU 中运行：
python3 scripts/porting-record.py check --device /path/to/UniXNU-j716f
```

## 安全与证据

仅在另行核对候选和恢复方案后测试 `boot_b`；不写 `boot_a`、vbmeta、userdata，不处理 TzDxe。
历史 P22 的 Query/ARM 流程只用于该版本，不是未实现的原生日志流程。
原生候选必须具有独立会话/候选身份和只读取证通道；不可复用已消耗 ARM。
已获用户授权推送脱敏源码并运行 CI；具体运行及结果见原生主线记录。未刷机或重启。

公开仓库只保留代码、配置、哈希与脱敏结论，不发布 Apple/Lenovo 输入、
完整 DTB/ADT、镜像、照片视频、原始日志、设备标识、随机种子、凭据或私钥。

## 记录入口

- [权威 porting.toml](porting.toml) · [研究索引](research/README.md)
- [P22 收尾](docs/P22_CLOSED_NATIVE_MAINLINE.md) · [原生主线](docs/NATIVE_MAINLINE.md)
- [原生 IRQ observer 的既有范围](docs/NATIVE_IRQ_OBSERVER.md)
- [历史进度/上游对照](docs/PROGRESS_AFTER_V23P22_20261027.md)
