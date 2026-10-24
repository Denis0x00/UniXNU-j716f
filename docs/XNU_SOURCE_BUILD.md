# 首轮 macOS XNU 编译

Actions workflow 为 `J716F XNU source build`，由相关配置推送到 main 或
`workflow_dispatch` 手动触发。它检出官方 UniXNU 固定提交
`245aca1a9ec5b730b1cad80fade295800d2981f1`，使用其固定 feeds 和模块提交。

## 范围

- macOS 26 ARM64 runner、Xcode 26.6、Python 3.13。
- XNU `10063.141.1`，Darwin `23.6.0`，iOS 17 ABI，BCM2837 4 KiB 布局。
- 源码 libpthread、corecrypto，以及官方构建脚本使用的基础依赖。
- 新配置 `j716f_source_defconfig` 不启用 CPU、ADT 平台、AES、UFS、图形驱动。
  这些功能还需要静态库、kernelcache 闭包或 J716F 硬件适配。

`scripts/build-j716f-source.py` 调用官方驱动的 prepare 和 kernel.build 阶段，
不调用 kernel.prelink。官方 `unixnu build kernel` 会同时调用二者，目前的
J716F 配置尚无合格的 `[kernel.prelink]`，因此不能把整个命令成功作为本轮目标。

corecrypto 使用官方 feeds 指定的第三方源码快照；其来源和编译审计由上游
builder 的文本报告记录。编译不代表其运行时自检已通过。

## 固定版本的兼容修复

首轮最终链接缺少 `_native_legacy_display_probe`：上游
`offline-component-load-probes.patch` 的一处调用未受显示驱动开关控制。
本仓库的 `patches/build-driver/0002-guard-optional-legacy-display-probe.patch`
为这处调用补上 `XNU_NATIVE_LEGACY_DISPLAY` 条件，并修正原补丁的 hunk 行数。
构建入口在固定的官方 checkout 上应用此修复，不改变固定提交号。

完整编译前，使用 Clang 对实际补丁中的 C 代码进行预处理检查：宏未定义或为
0 时不得引用显示 provider，为 1 时必须保留引用。修复补丁及修改后的源码补丁
SHA256、检查结果写入 `source-build-report.json`，可与上游原始内容区分。

首轮 bootstrap pass 缺少 corecrypto 入口是预期的中间状态：该阶段先生成依赖
编译记录，随后构建 libpthread/corecrypto 并重新链接；最终链接不应再缺少它。

## 结果

2026-10-24 第二轮构建已全部通过：[Actions 40099467304](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40099467304)，
测试配置提交为 `47a037efc95c655bfb249d275a80b140113ff53e`。最终链接、ARM64
Mach-O executable 检查及三份静态库的非空检查均已通过。这证明该配置的源码
编译成功；真实 J716F 内核预链接及启动尚未验证。

前两轮 Actions artifact 仅含文本日志和源码依赖报告，保留 7 天。编译成功会检查原生
Mach-O 的 ARM64 架构，以及 libpthread、普通 corecrypto、命名空间隔离版
corecrypto 的输出文件，并记录大小和 SHA256。

前两轮未上传内核和静态库二进制，输出随临时 runner 回收。

## 取回下一轮源码产物

为继续本地入口调试，后续成功构建另外提供 `j716f-source-encrypted-<run>-<attempt>`
artifact（保留 3 天），其中只有加密的 `j716f-source-only.enc`。包含经过构建报告
校验的原生内核、三份静态库及报告，不含 IPSW、原始 Apple kernelcache 或厂商文件。

`scripts/source-artifact-transfer.py` 使用 RSA-OAEP-SHA256 包装随机 AES-256-GCM
密钥，密文及格式头均经认证。CI 只持有 `ci/source-artifacts-public.pem`；私钥在
本地已忽略的 `.cache/artifact-keys/source-artifacts-private.pem`，必须保留，不能提交。
下载并解开 GitHub artifact 外层 ZIP 后，在设备仓库运行：

```sh
python3 scripts/source-artifact-transfer.py open \
  --private-key .cache/artifact-keys/source-artifacts-private.pem \
  --input /path/to/j716f-source-only.enc --output /path/to/j716f-source-only.zip
```

解密时会校验四份产物的大小和 SHA256；已有输出拒绝覆盖。
这些仍是源码构建产物，后续须准备原始 kernelcache、预链接、J716F ADT 和
owned-memory window，才能构造真实内核负载。

2026-10-24，运行 `40100682659` 的加密产物已下载、本地解密并通过四份输出的
大小与 SHA256 校验。与固定 Apple kernelcache 的离线预链接、入口/panic 探针生成
及静态修改范围审计也已通过；具体纯元数据见
`research/offline-prelink-40100682659.json`。这些产物没有 J716F ADT 或已验证的
物理内存窗口，尚未组成可启动 boot 镜像。

## 官方依据

- [构建内核](https://unixnu.com/zh/guide/build-kernel)
- [macOS runner](https://unixnu.com/zh/building#macos-runner)
- [固定版本的官方 workflow](https://github.com/UniXNU/UniXNU/blob/245aca1a9ec5b730b1cad80fade295800d2981f1/.github/workflows/macos.yml)
