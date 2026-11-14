# TB-J716F：macOS Data seed 工作流

本目录是准备合并到设备仓库的暂存源码，不是已推送的仓库，也不是已生成的 Data 镜像。保持目录结构复制 `.github/workflows/data-seed.yml` 和 `scripts/ci/`；本文可放到仓库文档目录。文件不包含本地设备序列号、唯一芯片身份、SSH 凭证、IPSW 或实机日志。

## 固定输入与官方调用

固定官方 UniXNU 提交 `7b730b231746667d8b7f54746c2c1ce45852611c`；只初始化其 DarwinBoot 子模块，提交 `5243c68ec332a227fe3680cd5da6d5a0e127035c`。两者 HEAD 和 tracked 修改都在 runner 上检查。OS profile 是 `ipados-17.7.11-21H461`，原始 Apple 来源、BuildManifest、OS、SystemOS 和 trust bundle 的固定身份见 `scripts/ci/data_seed_inputs.json`。没有接受自定义下载 URL 或任意 pre-build 命令的入口。

官方 [`make-data-seed.py`](https://github.com/UniXNU/UniXNU/blob/7b730b231746667d8b7f54746c2c1ce45852611c/scripts/make-data-seed.py) 的第一个参数是 **OS 文件系统挂载根**，要求存在 `sbin/launchd` 与 `private/var`，不是 OS 镜像文件名，也不是 SystemOS cryptex 的挂载目录。本工作流先验证原始 OS SHA256，以 `hdiutil -readonly` 挂载，再用 `diskutil` 校验根卷 UUID、SystemVersion 的 17.7.11/21H461 及目录。之后原样调用：

```text
python3 <fixed-core>/scripts/make-data-seed.py <read-only-OS-mount> <new-seed.dbr> --gib 16 --build 21H461
```

这个版本创建初始 32 MiB APFS、复制原始 `private/var`（排除 staged_system_apps）、添加运行时目录，继而调用同提交的 `seed-data-grow.py` 扩容，并用 DarwinBoot 的 DBRDISK1 工具打包。工作流没有修改官方脚本，也不使用为旧单一 launchd 哈希写死的 `prepare-owner-data-fixture.py`。保留官方默认 Setup 行为，不加 `--skip-setup`，不生成个人账户或设备身份。

下载使用固定官方 `unixnu.ipsw.Ipsw` 的 HTTP range/ZIP 组件提取。wrapper 为初始 HEAD/探测的网络错误补充最多四次连接尝试（官方数据块已有重试）；不对 pin/身份不匹配重试，更不自动重建已有 Data。仅下载 OS（约 5.16 GB）与两份很小的原始 trust cache，再按官方工具捆绑并验证已知 SHA256。**SystemOS 本轮只核对 BuildManifest/ZIP 成员名、长度及 profile 中的 pin；不下载其 3.3 GB 内容，不声称本轮验证了它的实际 SHA256。** 先前本地 OS/SystemOS/trust 的完整身份是 pin 来源，不上传这些本地文件。

## 触发与产物

- `workflow_dispatch`：Data 逻辑容量可选 3/4/8/16/32/64 GiB，默认 16 GiB。工作流进入默认分支后可从 Actions 页面运行。
- 推送 `run-data-seed-*` 标签：默认 16 GiB，适合已有 SSH push 权限而没有 PAT/API dispatch 的情形。标签必须指向含有此 workflow 的提交；每次用唯一名称。不需要在 workflow 中保存 SSH key 或 PAT。
- 只有这两个触发器；普通源码 push 和 pull request 不触发创建。

任务使用官方构建工作流同一可用基线 `macos-26`，Data-only 不安装/编译 Xcode 工具链。actions/setup-python 固定 3.12，并向 sudo 传该 action 输出的绝对解释器路径，避免误用系统旧 Python。checkout v4.2.2、setup-python v5.6.0 与 upload-artifact v4.6.2 都固定到完整提交，权限仅 `contents: read`，checkout 不持久化凭证。未使用 secrets、发布 release、写仓库或设备刷写操作。

成功产物 `data-21H461-<run-id>-<attempt>` 包含：

| 文件 | 含义 |
| --- | --- |
| `data-21H461.dbr` | 新安装的 packed APFS Data seed |
| `data-21H461.json` | 固定源码/OS 来源、Data UUID、实际 macOS 校验结果与 SHA256 |
| `SHA256SUMS` | 对 dbr 和 json 文件实际字节的 SHA256 |

独立 `data-evidence-...` artifact 在成功或失败时保留脱敏 JSON/log；只上传明确列出的文件，不上传 runner 工作目录、原始 IPSW/OS/SystemOS、环境变量或机器目录清单。工作流产物默认保留 30 天、诊断记录 14 天。应及时通过已登录的 GitHub 浏览器或有相应权限的 API 下载，在本地长期保存安装 seed；SSH push 能触发任务，不代表匿名 API 能下载 artifact。本工作流不将二进制提交到 Git 或发布 public release。

## 实际 APFS 验证与磁盘预算

直接只读挂载原始 UDIF OS，不生成额外 OS.raw。Data 扩容以及校验重建只 `truncate` 逻辑容量并写入非零 extents；不会主动写满 16 GiB 的零。创建前要求至少 8 GiB 可用空间，重建时检查实际分配字节数小于逻辑容量。异常磁盘占用或官方工具失败会保留诊断并退出。

完成官方打包后，脚本独立检查 DBRDISK1 结构、APFS NXSB/APSB 校验和、容器几何和唯一 Data UUID；重新构造 sparse raw，并以真实 `hdiutil` 只读挂载。`diskutil info` 的 APFS 类型、Data 名称、卷 UUID 必须匹配，初始目录与 `/tmp` 权限必须符合官方输出，`diskutil verifyVolume` 必须成功。全部镜像解除挂载、seed SHA256 未改变后才发布产物。最后再运行独立 artifact 字节验证器。

正常路径、捕获到的失败及单独的 `always()` 步骤都只清理本任务工作目录或新 core/build 下的镜像挂载；不会按任意磁盘号扫描并 detach 系统磁盘。子命令使用独立进程组；超时或中断时先终止该组中的 make/grow/diskutil 后代，再清理挂载，避免仅杀直接 Python 子进程却留下写入者。先尝试普通 detach，失败后只对已经从本任务镜像识别出的整盘设备强制 detach。硬性终止 runner 进程仍可能来不及记录最后清理结果，不能把该情形报告成成功。

## 下载后的检查和接入

Windows、Linux 或 macOS 可运行：

```text
python3 scripts/ci/verify_data_seed.py <downloaded-artifact-directory>
```

它验证文件 SHA256、packed extents、APFS superblock 校验和、Data UUID、格式身份及固定来源元数据。它不会在 Windows/Linux 上伪称执行 `diskutil`，输出分别标识“本机字节检查”与“macOS 创建报告记录的校验”。SHA256SUMS 用于完整性核对，不是发布者的数字签名。

16 GiB 是初始逻辑文件系统容量，**不是已决定的设备分区布局或内存分配方案**。这是新安装输入准备；不说明 TB-J716F 已有 UFS 块设备、根卷/Data 挂载或 iPadOS 用户态成功。macOS 26 主机的 APFS 检查也不能替代 iPadOS 17 目标 APFS 驱动的挂载验证；报告保留实际容器 feature 位及 `target_apfs_mount_verified=false`。后续接入官方构建时，使用这个确切 seed 并把实际 Data UUID 绑定到内核配置，评估 payload/存储容量；不要把每次构建的 cache miss 当成重新创建 Data 的理由。新的运行会生成不同 APFS UUID/事务元数据，必须作为不同安装保留，不能静默替换既有 seed。

## 本地验收状态

本暂存包已通过 Python 语法、JSON/YAML 结构及 14 项纯字节/拒绝边界测试。测试夹具只检验解析和清理范围，不能制造一个已挂载或经过 APFS 驱动检验的文件系统。首次 GitHub macOS job **尚未运行**；只有 job 产生成功 artifact 后才能填入实际 Data UUID 和宣称 APFS 主机校验通过。

GitHub 行为参考：[workflow 语法](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax)、[checkout](https://github.com/actions/checkout)、[artifact 上传](https://github.com/actions/upload-artifact)。
