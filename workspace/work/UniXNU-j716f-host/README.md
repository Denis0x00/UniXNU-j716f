# TB-J716F 独立主机内核目标

2026-11-12 本轮完整进度与上游审计见
[审计记录](../../analysis/progress-upstream-audit-20261112/README.md)。
当前 Windows/VM 主树为 `2bbb39dcee0cada3b04e41f0256d678e80667c5b`；
下述已完成内核仍来自 `0e2fb2bcdf11b104d28b0d4aae4f77a7945e3de5`
及构建报告记录的当时本地修改，不能回填为新 SHA 的构建结果。
文档同步结果见本轮审计记录；源码同步不改变原内核的 ABI/运行资格，也不表示已用新源码重建。

目标是 iPadOS 17.7.11 / 21H461 / arm64 的 XNU 编译与 prelink。
本目录从 `../UniXNU-j716f` 的已审查输入建立；创建时原完整草稿保持
`example=true`，后续已独立具体化实验性镜像输入，见其当前 README。
这里的 `example=false` 只表示主机内核构建配置已具体化，
不是完整设备移植或实机验收。

保持 5 个组件、单核配置、58 项补丁及 ARM64 profile 继承的 8 项归档配方。
SoC、DTS 和补丁副本的来源与哈希见 `docs/host-source-manifest.json`、
`docs/source-provenance.json` 与 SoC README。硬件基线是原草稿的历史快照，
其中提到的完整草稿 guard 及待建主机目标描述不用于本目录当前状态。

VM 布局为 `/home/test/unixnu-work/work/UniXNU-j716f-host`。输出独立位于
`/home/test/unixnu-work/UniXNU/out-host/j716f-host/ipados-17.7.11-21H461/j716f_host/`，
包括 tree、SDK、编译记录、logs 和最终 kernel。out-host 的 state 与 feed
checkout 独立；固定 feed 可从已核验本地仓库克隆复用对象，不改源版本。

在本目录使用固定入口：

```sh
python3 scripts/host.py defconfig
python3 scripts/host.py fetch
python3 scripts/host.py status
python3 scripts/host.py tree
nice -n 15 ionice -c 3 python3 scripts/host.py kernel
```

`kernel` 调用官方 `unixnu build kernel`，包含 bootstrap、archives、最终
链接和默认 prelink；只有已有 native kernel 且只修改预链接输入时才用
`python3 scripts/host.py prelink`。不启用 `--skip-prelink`。
XNU make 设 `-j 1`；部分官方归档构建器有自己的并行度。

入口拒绝未知操作与额外参数，不提供 firmware、payload、userdata、bootimg、
release 或设备操作。原始官方 CLI 没有 host-only 限制，OS profile 仍会
合并默认 payload/root 配置；不得用原始 CLI 绕开本目标约定的范围。

遗漏头文件使用已核验 Git 来源的目录缓存供官方恢复器读取；不复制正式
XNU 工作树、SDK、编译记录或完成标记。Linux compiler-rt 与 Libc 固定
版本预取到本目标树，之后由官方 SDK/库构建步骤消费。

首次官方构建于 2026-11-12 完成，退出码 0；8 项归档、最终链接及 prelink
成功。native 为 18,187,952 字节，prelink 候选为 51,032,064 字节；后者
SHA-256 为 `62215d029ca1b96f5c140abf2fb435e97da8b3ae3574ef10b568a0b8a980f3e8`。
独立回读校验了 69,294 个指针及组件/代码/metadata，结果通过。
上游 `abi_qualified`、`runtime_qualified`、`loadable` 等标记仍为 false。
这是一份实验候选；未宣称其完整 ABI 或实机启动得到验证。

构建记录保存在工作区 `analysis/host-kernel-20261112/`，内核和证据归档在
`artifacts/j716f-host-kernel-20261112/`。完整结果见
[主机构建记录](../UniXNU-j716f/docs/host-build.md)。
没有固件、载荷、启动镜像或任何实机动作属于本次主机构建。
