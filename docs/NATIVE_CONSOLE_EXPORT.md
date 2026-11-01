# 完整保留区只读导出：RAW v1

历史v1流程与证据。当前MAINT-v2镜像及第3项同应用导出/Init请见[新流程](NATIVE_MAINTENANCE_V2.md)。
旧回执不可用于v2；下文“当前”均指当时v1检查点。

当前镜像为 `j716f-native-log-40458123294-raw-v1-boot.img`，8,372,224 字节，
SHA256 `63399ed5260c5291c25a9a5b55c5214aa0099799b6cc040d9b7ceef20eb5addc`。
它仍使用固定 CI40458123294 内核与原 ADT；只增加显式 opt-in 的 Export/Init 工具。
旧原生镜像、host audit、刷写记录及 P22 冻结证据均保留。

2026-10-27：另行授权的 boot_b-only 部署后，菜单、Read 和 Export 首帧已拍照。
首帧通过独立 ZXing 解码及生产帧格式/candidate/CRC 校验，恢复 1024 字节，
其中 DBGC 头与 reader 照片一致。此前仅 1/256 帧的[首帧记录](../research/native-console-optical-20261027.json)保留不变。

随后的视频完整恢复 **256/256 帧、262144 字节**，整区 SHA256
`0ebb2cdca1c32957283eeffc1cdff9cdc19f8b035cd71b10f0f08146725382fa` / CRC `1CC58AC7`
已独立重算，保存和读回通过，并留有第二份私有目录副本。
视频 transfer `00000001AD5A00BB` 不同于首张照片；接收器拒绝混合，最终数据全部来自该视频。
初始采样得到 250 帧，缺的 6 帧从同一视频密集采样恢复，没有推测或合成任何载荷。
参见[完整导出验收](../research/native-console-complete-20261027.json)。
**旧快照导出已完成，用户随后已授权 Init-only；首次 Init 现场已安全拒绝，未初始化写入。ARM/XNU/复位不在本次授权内。**
旧完整导出记录保留当时“Init 未授权”的事实；后续状态见[日志分析与授权检查点](../research/native-console-analysis-init-20261027.json)。

## 最新现场：Init 拒绝，未写入

用户随后说明中途重启并进入 fastboot。新 Init 照片中的 Candidate 编译常量仍匹配，
但 DBGC 已是 `next=size=0x3f56e`，屏幕 SHA 为 `a6c467c4...83d3e567`，不再匹配旧完整备份。
页面显示 `ready=0 used=0`、确认 `0/2`、未输入回执，以及 `BLOCKED ... No writes`。
详见[首次 Init 拒绝检查点](../research/native-init-blocked-20261027.json)。

当前头在结构上是有效的**未满线性区**；Init v1 只接受满 wrapped 环，因此 size 条件
已经足以拒绝，不能据综合错误提示断言存在 pending ARM 或内存损坏。
剩余数据空间 2694 字节，对齐后只有 2688 字节，不够 8192 字节原生会话，不能跳过 Init 去 ARM。
也不是 Init 成功：预期成功头应为 `next=size=0x104`。

现在只退出拒绝页，先拍 Read，再对当前快照进行另一次完整只读 Export；保留为独立 transfer，
不得混入旧帧或复用旧回执。完成后保持 Export 打开并暂停，不再重启或进入 fastboot。
**重新导出只解决新证据保存，不会使这个未变的头通过 Init v1。**
“完整备份后处理容量不足的有效线性区”是当前实现尚未支持的状态，需要单独审查实现与测试；
不放宽现有门槛、不故意写满环、不据此刷入新固件。原始两份完整备份均已复核未变。

用户随后允许准备[RAW/Init v2 方案](NATIVE_CONSOLE_V2_PLAN.md)。它只定义新增安全分类、
同次启动导出凭据及下一版 UI/电源动作；当前 v1 源码/镜像/回执策略没有改变。

## 完整日志结论与下一步

按 DBGC 的 next-write 游标 `0x3cbd2` 展开满环后，载荷为 262132 字节、3164 行
（第一行是被环覆盖截断的旧行）。主要来自 Android Recovery/fastbootd：
Keymaster 服务重试占约 58% 字节，电池 cycle_count 占约 25%，另有 healthd/充电信息。
末尾记录 fastbootd 发起重启、Android 关闭流程和存储缓存同步，符合此前部署重启的上下文，
但不是独立的操作身份或原生复位保留证明。没有当前原生会话标记；不能把 Android 报错
归因于 XNU，更不能据此宣称平台、CPU、IRQ 或调度通过。

两份私有目录中的 dump/frames/receipt/recovery-audit 已再次逐字节核对。
原计划从 Export 返回菜单，选择第 3 项 Init，**先拍完整首页，不输入回执、不连续确认**。
核对 Candidate、完整 Snapshot SHA256 与已保存备份一致，以及 `ready=1 used=0` 后，
才指导输入私有回执并执行两次独立确认。若现场变化，停止写入并另做只读导出。
预期成功提示为 `INIT READBACK OK: next=size=00000104`；成功后拍照，再进 Read 取证。
这只建立初始化记录，不是 ARM，也不启动 XNU。

## 当前只读操作

1. 先拍新菜单和第 1 项 Read 页面，再进入第 2 项 Export。
2. Export 冻结完整 `0xb00be000+0x40000` 物理区，不只导出能解释的有效记录。
   初始暂停在第 1 帧；短按 Power 播放/暂停。暂停时音量键切帧，播放时调速度。
3. 保持同一次 Export 应用打开。手机固定、对焦二维码、保留完整白边，使用原始高质量录像。
   默认每帧等待 1 秒，另有编码/绘制开销；建议至少 5 分钟，**以 256/256 回到 1/256 为准**。
   录像前先开始录制再播放，完成后只暂停；不要退出、重启或选择 Init/ARM/Native。
4. 视频与所有帧均是私有原始 RAM 证据，不发网站、不提交 Git，不经聊天视频压缩。
   缺帧可继续录制同一个仍打开的 transfer；重新进入应用会生成另一传输，不能混合。

## 主机离线接收

视频接收需本机 OpenCV 和 ZXing-C++；接收器没有网络或设备操作。
以下是路径占位符，输出目录必须不存在。已保存首帧可通过 `--frames` 合并：

```sh
python3 scripts/receive-native-console.py \
  --candidate 1c19ee51d5388dcfbbaefd41dcf361a54ccee3d08d889f6f0e4bb6f69a08d892 \
  --frames /private/first-frame/frames.txt \
  --video /private/original-capture.mp4 \
  --output /private/new-received-directory
```

每帧检查格式、candidate、transfer、快照摘要、帧序号和帧 CRC；集齐 256 帧后才重建
262144 字节并独立重算整区 SHA256/CRC。成功时保存 `frames.txt`、`raw-console.bin`
及 `receipt.json`，执行 flush/fsync 和重新读回。缺帧只保留 partial frames，无完整 dump/回执。
补录时将该 frames 文件与新视频交给另一个新输出目录，绝不覆盖前次证据。

## Init 是另一道授权门槛

只读导出授权本身不包含 Init；Init 实现及其主机模拟通过也不构成设备初始化授权。
本轮完整导出及落盘校验后，用户已另行允许 Init-only，仍须审查回执与当场快照。
回执只是绑定 candidate 和完整快照的防误操作码，不是认证，设备也不能据此证明外部磁盘持久化。

Init 只接受有效的满 wrapped 环，发现任何 candidate 的完整未消费 ARM 都拒绝。
提交前核对现场字节，要求输入回执及两次确认；写入范围限定在前 272 字节内，保存初始化记录，
不整区清零，不自动 ARM。TPL 不阻止 DMA/安全世界并发，保留区所有权与恢复仍需硬件验证。
即使 Init 成功，旧 Read 的“无有效匹配会话 / Do not ARM”也不能当作内核结果；
应停止并审核初始化 readback，后续 Query/ARM 与 XNU 启动各自单独审查。

## 不应扩大结论

- 首帧实机光学解码不等于全区导出、完整视频接收或重启后可恢复。
- 原生入口、VM、平台、异常、IRQ writer 已编译进选定内核，但该内核尚无本轮启动证据。
- 首次更换固件需要重启；旧 reader 没有全区导出功能，旧未导出快照不能保证保留。
- 复用 Mu QrEncoderLib 不代表集成了上游 Advanced Logger 协议或 OneSend 格式。
- Init 的真实按键流程、现场竞争、挂死恢复与长期映射尚未实机验证。

## 下一轮固件 UI 请求（尚未实现）

用户要求下一轮编译时缩小菜单字号，参考 UniXNU 官方布局，并增加独立“重启”“关机”项。
实施时核对固定上游菜单动作与 J716F 的实际复位/关机路径；使用显式确认，不引入倒计时、
自动重试、自动复位或启动失败回退。不能把仅出现菜单项当作实际重启/关机功能已验证。
本轮镜像不变；新固件必须另记源码/镜像哈希并取得相应硬件测试授权。
