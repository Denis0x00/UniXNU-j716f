# UniXNU-j716f

联想小新 Pad Pro 2021（TB-J716F，SM8250/Kona）的 UniXNU 设备配置与移植记录。
配套固件：[uefi-j716f](https://github.com/Denis0x00/uefi-j716f)；
规范：[上游移植指南](https://unixnu.com/zh/porting/)。

## 当前状态（2026-10-30）

原生候选 `40458123294`（含 `ArmDTPlatform` + `ArmCpu` + native timer）配合 `uefi-j716f` 的 `J716fNativeFast` 已完成 11 轮 `boot_b` 实机迭代（`FAST-V1 -> ... -> FAST-V11`）：

| 范围 | 当前证据 | 边界 |
| --- | --- | --- |
| UEFI/GOP/按键/加载器 | 多轮实机诊断可用；新增 `package/native-fast/`（3 秒冷/脏启动 ARM + 4 秒 8 帧 QR 导出 + 确认式热重启/关机） | 不代表 XNU 显示、输入或存储驱动 |
| 内存与原生 XNU VM | **原生候选 `40458123294` 已在实机 100% 跑通 `0:start_begin -> ... -> 7:vm_done`，8/8 槽位 CRC 全过且零异常** | 已修复 `T1SZ=26` L1 根表索引掩码与切换前 `_phystokv` 两个实机缺陷 |
| Mach 微内核与多核 SMP | **8 核 Kryo 585 拓扑、GICv3 中断控制器、ARM Generic Timer 定时器、Mach 任务/线程与调度器全面跑通** | 零异常，时钟中断正常触发与 EOI |
| IOKit 与平台安全驱动 | **通过 `kext_sub_ok`、`commpage`、`MAC policy`、`TrustCache`，成功跳过苹果专有 PMGR nub 依赖** | 不依赖苹果专有电源芯片硬件 |
| BSD 子系统与 VFS | **`FAST-V11` 彻底攻克 `_proc_ro_alloc` 物理只读页限制，`18:proc_ro_ok` 与 `19:vfs_ok` 全部绿灯通过** | 完整 Mach + BSD 内核初始化全面走通 |
| **根文件系统挂载（当前位置）** | **实机 58/58 槽位全部有效，成功进入 `IOKitBSDInit.cpp` 并抵达 RootFS 挂载点：`panic("Failed to mount root device @%s:%d", "IOKitBSDInit.cpp", 984)`** | 内核等待挂载根镜像；待下一步接入 Ramdisk / UFS Root 卷 |

- **重大突破**：原生 XNU 内核已完整执行全部 Mach 微内核与 BSD 子系统初始化，正式抵达根文件系统挂载点！
- **最新实机与技术报告**：[NATIVE FAST-V1..V11：实机 `arm_vm_init` / BSD / RootFS 进展](docs/NATIVE_FAST_VM_BRINGUP_20261029.md)
- [P22 收尾](docs/P22_CLOSED_NATIVE_MAINLINE.md)与[脱敏实机结果](research/p22-closed-20261027.json)作为历史记录保留。

## 唯一推进主线

固定原生产物 → XnuPrelink → 专用负载/最终镜像核验 + 可恢复早期日志 →
同一候选上的平台匹配、CPU0 注册、GIC、真实 timer IRQ/rtclock/EOI、单核调度。
之后才进入 UFS、根卷与用户态。

[执行与上游差距清单](docs/NATIVE_MAINLINE.md)列出每道门槛和未完成项。
不再默认制造逐函数复位的冻结内核后继版本。

[MAINT v2 已实施](docs/NATIVE_MAINTENANCE_V2.md)：实际C/EFI验证和镜像反解通过，
新镜像627dc31c已执行boot_b-only刷写/选槽/重启；仍待新菜单/Read和完整导出。
内核/ADT不变，第3项在同一应用内完成新Export→Init；不复用旧回执。尚无实机Init/ARM/XNU结果。
为保持本次最小主线，原计划的主菜单缩字/电源项暂缓。

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

原始原生镜像 `j716f-native-log-40458123294-boot.img`：8,359,936 字节，SHA256
`801ddb86b29844e316053f4d061a9fd2cb266e47f1c27ed1d46745b036dbb8e7`。
镜像内原生预链接内核：52,563,968 字节，SHA256
`150c5c5f13a4337eb05218232f3a157429f0974ba49f71b4627789497132d074`。
镜像和原始报告仅留私有工作区；[脱敏主机证据](research/native-image-40458123294.json)。
后续[首次菜单/reader 实机观察](research/native-reader-40458123294-20261027.json)独立记录，
不覆盖主机报告，也不把只读页面当作内核启动或跨复位日志保留成功。

新增 RAW-v1 镜像为 8,372,224 字节，SHA256
`63399ed5260c5291c25a9a5b55c5214aa0099799b6cc040d9b7ceef20eb5addc`，内核/ADT 不变。
[只读光学导出流程](docs/NATIVE_CONSOLE_EXPORT.md)、[首帧证据](research/native-console-optical-20261027.json)
及[完整导出验收](research/native-console-complete-20261027.json)分开保存。
视频使用另一 transfer；独立接收后恢复全区 SHA256 `0ebb2cdc...25382fa` / CRC `1CC58AC7`。
完整 dump 和回执只留本机。用户随后已授权 Init-only，但中途重启/fastboot 后现场已变；
[首次 Init 页面](research/native-init-blocked-20261027.json)为 BLOCKED、ready0/used0，未写入。
当前有效线性日志只剩2688字节对齐空间；既不满足 Init v1 满环策略，也不够8192字节原生会话。
[日志分析](research/native-console-analysis-init-20261027.json)确认满环主要是 Android Recovery/fastbootd
的重复日志，不是当前原生 XNU 的运行证据。

**固件只能走本轮专用 staging/packaging 路径。** `board.toml` 的普通 firmware DSC
尚未迁移，默认 preflight 已改为直接拒绝；不要用 `--skip-preflight` 绕过。
当前保留 7 MiB FD 与嵌入 FAT，不是标准 initrd；独立 RAW 二维码不等于 Advanced Logger/OneSend 集成。
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
已获用户授权推送脱敏源码并运行 CI；另行授权的首次 reader-only `boot_b` 写入、B 槽激活与重启已完成。
菜单/reader 照片已私有保存。当前满环形没有 8192 字节未用线性尾部，不能继续 ARM；
`STABLE=1` 只表示一次冻结读取的 CRC 一致，照片不是完整 RAM 备份。
新增 RAW-v1 只读导出已完成全区校验落盘；用户已另行授权 Init-only。
首次 Init 已安全拒绝变化后的非满线性快照；下一步仅 Read 与新快照独立只读导出。
不复用旧回执、不故意写满日志、不重启、不 ARM 或启动 XNU；导出不等于解除现有 Init 策略限制。
原生启动、实机恢复闭环和 IRQ/调度仍未准入；新刷写/复位及扩大测试范围需另行授权。

公开仓库只保留代码、配置、哈希与脱敏结论，不发布 Apple/Lenovo 输入、
完整 DTB/ADT、镜像、照片视频、原始日志、设备标识、随机种子、凭据或私钥。

## 记录入口

- [权威 porting.toml](porting.toml) · [研究索引](research/README.md)
- [P22 收尾](docs/P22_CLOSED_NATIVE_MAINLINE.md) · [原生主线](docs/NATIVE_MAINLINE.md)
- [原生 IRQ observer 的既有范围](docs/NATIVE_IRQ_OBSERVER.md)
- [历史进度/上游对照](docs/PROGRESS_AFTER_V23P22_20261027.md)
