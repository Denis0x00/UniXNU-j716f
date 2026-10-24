# UniXNU-j716f

UniXNU 设备仓库草稿，用于联想小新 Pad Pro 2021（TB-J716F，`j716f`，高通 SM8250）。

这个仓库目前处于 **research / bring-up** 阶段。它记录可复现的板级配置和预检，不宣称已经能够启动 iPadOS、iOS 或真实 XNU。

## 当前验证范围

- Android bootloader 可以通过 `boot_b` 启动实验 Android boot image。
- J716F 的 Mu-Silicium UEFI 固件已经在本地 Ubuntu 环境编译并完成离线校验。
- 真机已验证 UniXNU 菜单、音量键、电源键、RAM FAT / BlockIO / 文件系统路径。
- 真机已验证官方 DarwinBoot v2 协议调用和无效负载拒绝路径。
- V14 真机完成256 MiB窗口分配、两轮完整缓存维护后读回与释放，7/7 PASS；EFI_SUCCESS与匹配报告返回也已确认。见 [V14结果](docs/V14_MEMORY_WINDOW.md)。
- 当前没有验证物理 UFS、真实 Apple DeviceTree、真实 XNU 内核入口、ExitBootServices、trust cache 或用户态。

## IPSW 候选

`board.toml` 暂记录 UniXNU `sheng` 参考页使用的 iPadOS 17.7.11 / build `21H461` / product `iPad7,1` / `j120ap` 身份。它只是 Apple IPSW 来源候选；`j716f` 仍是联想板级代号，不能把两者当作同一个设备。

在得到 J716F 专用 kernelcache、ADT、trust cache、userdata 窗口和芯片种子前，不要下载并写入真实系统负载，也不要填写 `[payload]`、`[userdata]` 或 `[kernel.prelink]` 的猜测值。

## 目录

```text
board.toml                         板级和 IPSW 候选描述
configs/j716f_research_defconfig   研究阶段内核配置草案
feeds.toml                         固定的 Mu Basecore 依赖
patches/build-driver/              Android boot image header-v2 补丁
scripts/check-j716f.py             本地固件输入预检，不执行设备 I/O
scripts/darwinboot-source-lock.json DarwinBoot 源码固定提交
```

配套的 Mu-Silicium UEFI 工作树仍在本地 `uefi-j716f` 目录中。它包含厂商输入和生成资源，没有复制到这个公开草稿；公开时应单独维护 `uefi-j716f` 仓库，并继续把厂商二进制、真机照片、IPSW、kernelcache、trust cache、userdata、Data seed 和构建产物放在仓库外。

## 本地构建前提

首轮 XNU 编译使用独立的 [macOS Actions 流程](docs/XNU_SOURCE_BUILD.md)
和 `j716f_source_defconfig`。它只验证源码及依赖编译，尚不生成可启动的 J716F
预链接内核。原 `j716f_research_defconfig` 仍是历史研究草案。

固件预检需要一个本地私有链接：

```text
.cache/firmware -> <local path to the prepared uefi-j716f checkout>
```

该链接目标和 ZUI XBL 输入不会提交。`scripts/check-j716f.py` 只检查文件、哈希、依赖和内存描述，不访问设备或执行分区写入。

## 真机安全边界

当前研究流程只允许临时或 `boot_b` 验证。不要写 `boot_a`、`vbmeta` 或 `userdata`，也不要把诊断镜像当作可恢复的 iPadOS 负载。

## 许可证

仓库沿用 UniXNU 的 BSD-2-Clause-Patent 许可证；上游 Mu-Silicium、DarwinBoot 和其它依赖仍受各自许可证约束。
