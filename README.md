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
| 原生平台与最终镜像 | CI40458123294 的带日志内核已接收、预链接并进入专用 boot.img；反解逐字节核对通过 | 主机结果；没有本候选的实机启动证据 |
| 原生日志与读取器 | 入口/VM/平台/CPU/异常/IRQ/调度已接真实源码；独立 reader/ARM 已编译，14 组真实 EFI 模拟通过 | 实机长期映射、挂死恢复和 RAM 保留尚待验证；不保证所有失败均有日志 |
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

[选定 CI 40458123294](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40458123294)
及六产物接收锁见 `configs/native-log-candidate-40458123294.json`。接收和预链接分别使用
`scripts/receive-native-candidate.py`、`scripts/prelink-native-candidate.py`。
命令及前提见[原生主线](docs/NATIVE_MAINLINE.md)。

接收器校验加密 artifact 摘要、认证解密、源码/组件版本、六产物和实际 Mach-O。
预链接器使用固定 OS 配方，并检查原生可执行字节、provider 符号与 source POST HMAC。
这些工具不访问设备。旧 CI40448429664 的无日志基线单独封存，不替代当前候选。

最终镜像 `j716f-native-log-40458123294-boot.img`：8,359,936 字节，SHA256
`801ddb86b29844e316053f4d061a9fd2cb266e47f1c27ed1d46745b036dbb8e7`。
镜像内原生预链接内核：52,563,968 字节，SHA256
`150c5c5f13a4337eb05218232f3a157429f0974ba49f71b4627789497132d074`。
镜像和原始报告仅留私有工作区；[脱敏主机证据](research/native-image-40458123294.json)。

**固件只能走本轮专用 staging/packaging 路径。** `board.toml` 的普通 firmware DSC
尚未迁移，默认 preflight 已改为直接拒绝；不要用 `--skip-preflight` 绕过。
当前保留 7 MiB FD 与嵌入 FAT，不是标准 initrd；Advanced Logger/二维码也未集成。
实际共用固件依赖包含已记录的本地修改，不能宣称从全部干净固定仓库复现。

主机回归：

```sh
python3 scripts/test-native-candidate.py -v
python3 scripts/test-source-artifact-transfer.py -v
python3 scripts/test-native-image-formats.py -v
python3 scripts/test-native-log.py --cc gcc
# 在固定 UniXNU 中运行：
python3 scripts/porting-record.py check --device /path/to/UniXNU-j716f
```

## 安全与证据

仅在另行核对候选和恢复方案后测试 `boot_b`；不写 `boot_a`、vbmeta、userdata，不处理 TzDxe。
历史 P22 的 Query/ARM 流程只用于该版本，不可代替本轮独立的原生 reader/ARM。
原生候选必须具有独立会话/候选身份和只读取证通道；不可复用已消耗 ARM。
已获用户授权推送脱敏源码并运行 CI；具体运行及结果见原生主线记录。未刷机或重启。
下一步先审查恢复方案并另行授权 reader-only 测试；原生启动、实机日志闭环和 IRQ/调度仍未准入。

公开仓库只保留代码、配置、哈希与脱敏结论，不发布 Apple/Lenovo 输入、
完整 DTB/ADT、镜像、照片视频、原始日志、设备标识、随机种子、凭据或私钥。

## 记录入口

- [权威 porting.toml](porting.toml) · [研究索引](research/README.md)
- [P22 收尾](docs/P22_CLOSED_NATIVE_MAINLINE.md) · [原生主线](docs/NATIVE_MAINLINE.md)
- [原生 IRQ observer 的既有范围](docs/NATIVE_IRQ_OBSERVER.md)
- [历史进度/上游对照](docs/PROGRESS_AFTER_V23P22_20261027.md)
