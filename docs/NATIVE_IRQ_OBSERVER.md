# 原生平台 IRQ 观测候选（2026-10-27）

本轮把同 SM8250 设备的经验落实到独立源码候选。P21 仍是最新实机边界，
P22 仍为已构建、按用户要求暂缓刷入。这里没有预链接、刷机或新的实机结果。

## 参考了什么

- [umi 观测器](https://github.com/reAAAq/UniXNU-umi/blob/3eb72578597d2ca16471b172d45eddc096a34a04/package/UmiArmCpuTrace.inc)：
  借鉴 CPU 注册/GIC/timebase 与真实 IRQ 进入、rtclock 返回、EOI 后的分段观测。
  J716F 重新实现自己的 ABI，不引入 umi 的日志地址、输出函数或复位路径。
- [pipa 固定 ArmCpu](https://github.com/xVanTuring/ArmCpu/blob/a9c4585dbb1b75b8594dd0ae54962cc59969d98f/ArmCpu.cpp)：
  由设备仓库 `cdb3d627` 的实际 gitlink 定位。它增加可配置 timer INTID、
  CPU 注册收尾与调度簇接线、timer deadline 余量等观测。
  这些变化没有整包移入本板；单核真实 IRQ 成立后再审查多核调度接线。
- 本项目继续固定上游 ArmCpu `9066b53b` 与 UniXNU `42bffe8a`，
  保留现有 GIC 初始化、timer 编程、原始 `rtclock_intr(0)` 及 EOI/ISB。

## 新增实现

`prepare-native-observer.py` 在 UniXNU 已组装的隔离构建目录中建立 ArmCpu 副本，
只替换该目录的组件链接。下载缓存和固定组件源码不变。
进入转换前同时核对 commit、源文件 SHA256、每个唯一插入点；
去除所有新增片段后必须逐字节还原原始 ArmCpu。再次安装或未知源文件被拒绝。

板级记录器位于 `package/native-observer/`。候选编译时包含到 ArmCpu 单一翻译单元中，
不添加 MMIO 写入、定时器编程、合成中断、复位或 IRQ 成功替代返回。
观测前暂时屏蔽 IRQ/FIQ，记录后恢复原始 DAIF。单核是本候选的资格边界，
不能把这里的记录器直接用于多核并发采集。

真实 dispatch 中的顺序为：

```text
IAR 确认 INTID27
  → 原始计数更新
  → ENTER 记录
  → 原始 rtclock_intr(0)
  → RETURN 记录
  → 原始 IRQ 后续路径
  → 原始 ICC_EOIR1_EL1 / ISB
  → EOI 记录
```

仅保存第 1、2、8 次 IRQ 各三个阶段，共九个快照；第八次 EOI 以后 IRQ 观测停止写入。
第八次返回/EOI、计数前进、注册/GIC 就绪及事件顺序均满足时，
才从该实际路径调用 `j716f_armcpu_timer_checkpoint`。手工调用此符号没有证明效力。
CPU quiesce/halt 另记，发生后解码器拒绝把该快照当正常启动资格。

## 本板 timer 路由输入

对既有原厂 DTB 和 Recovery FDT 重新只读解码，`/soc/timer` 的 compatible、
19.2 MHz 频率和四组 interrupts 完全一致。第三组 virtual timer 为
`<1 11 0xff08>`，按 GIC PPI 编码对应 INTID27。
独立摘要见 [timer 路由](../scripts/j716f-native-timer-route.json)，保留原始输入哈希，
不公开 DTB。构建安装时将路由摘要与既有板级证据哈希再核对。

顺序参考 [Linux arch timer binding](https://github.com/torvalds/linux/blob/v6.12/Documentation/devicetree/bindings/timer/arm%2Carch_timer.yaml)，
PPI 编码参考 [GICv3 binding](https://github.com/torvalds/linux/blob/v6.12/Documentation/devicetree/bindings/interrupt-controller/arm%2Cgic-v3.yaml)。
这是输入描述的一致性，尚未观察到本板真正的 timer IRQ。

## 观测 ABI 与内存生命周期

导出 `j716f_armcpu_trace`：192 个 little-endian uint64，共 1536 字节，版本 1。
内存属于链接后的内核静态数据，完全不借用 P22 的 bootstrap 子表或旧物理 RAM 日志区。
**当前没有持久化 writer，也没有从重启后的 P22 reader 读取它的路径。**
后续必须先具备能冻结读取内核虚拟地址的调试通道，或单独审查过的原生映射/持久化桥，
再制作可采集实机证据的候选。本轮的编译产物本身不能直接刷入。

| word | 含义 |
| --- | --- |
| 0–3 | magic `0x315152494636314a`、ABI 版本、word 数、写入序号 |
| 4–5 | startup phase 位图、最近 startup phase |
| 6–8 | IRQ enter、rtclock 返回、EOI 完成次数 |
| 9–12 | 首/末 IRQ entry counter、第八次检查点就绪、粘性错误位 |
| 13–15 | boot logical ID、MPIDR、第 1/2/8 次九快照有效位图 |
| 16–23 | 上游已读取的八个 GIC boot readbacks，不额外读 MMIO |
| 24–33 | 最近 startup 的 FRQ/CT/CVAL/CTL/DAIF/detail/IRQ phase/global timer/registered/GIC ready |
| 34–36 | 最近 IRQ 观测阶段、quiesce/halt 计数 |
| 37–47 | 保留，必须为零 |
| 48–191 | 九个快照，每个 16 word，依次为 IRQ1/2/8 的 ENTER/RETURN/EOI |

单快照依次包含 phase、enter/return/EOI 次数、CNTFRQ、CNTVCT、CVAL、CTL、DAIF、
raw IAR、registered、GIC ready、MPIDR、全局 timer 次数、上游 IRQ phase、logical ID。
startup phase 1–9：boot start、GIC prepare/IOCPU start 返回、注册完成、controller IRQ 注册完成、
GIC ready、timebase enable 返回、driver start 收尾、quiesce、halt。
错误位 1/2/4/8 分别表示顺序错误、CPU/INTID 不符、counter 未前进、provider 未就绪。

序号奇数表示写入中。离线解码要求**先冻结 CPU**再取完整内存，偶数序号本身不能保证
运行中跨多次读取的一致性。调试器和采集通道自身也须独立验证。

```sh
python3 scripts/test-native-irq-observer.py
python3 scripts/check-native-irq-trace.py --dump /private/frozen-observer.bin
```

## 校验与下一步

Linux GCC/Clang 分别编译运行实际状态记录器：十个正反例及十种解码篡改检查通过，
包括仅进入、未 EOI、乱序、counter 不前进、错误 CPU/INTID、provider 缺失、
quiesce、startup 缺失。实际生产 sysreg 观测封装另经 AArch64 交叉编译汇编检查。
固定源字节门槛、反向还原、原始调用/EOI 顺序和隔离安装/拒绝覆盖也通过。

macOS CI 将构建真实 XNU/ArmCpu，并要求最终 Mach-O 定义观测数组与检查点符号，
核对数组在文件内恰好一处、8 字节对齐、ABI/长度和初始零状态。
完整内核编译及新增链接门槛已在
[CI 40445461837](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40445461837)
通过，受测设备源码 `9ae5d75cdcee5a3c4293d6ecd80cac7c5b8d935d`。
源码编译、实际链接观测数组初始 ABI/零状态、加密导出与上传步骤全部成功。
未下载并独立解密远端产物；CI 步骤结果不构成 IRQ、驱动匹配或设备启动证据。

首次 CI 的观测器与导出主机检查通过；完整构建在编译前被新增 pin 门槛拒绝：
上游 in-tree submodule 的 `Module.commit` 为 `None`，只有 feed-fetch 路径填写该字段。
现改核对实际组件 Git HEAD，并增加实际 ArmDTPlatform pin 校验；源码字节门槛保留。
上述修复后的完整构建已通过。随后解码器补充最近 startup 阶段、寄存器和错误位输出，
并再次通过 Linux GCC/Clang 的实际 writer/decoder 检查；观测器、ABI、内核构建脚本与
受测版本相同。解码器输出扩展单独按主机结果记录。

后续顺序：P22 独立实机完成 VM 返回验证；完成新源码候选的 ABI/预链接和可采集通道审查，
再验证 ArmDTPlatform 匹配 → ArmCpu 注册 → GIC → counter → 真实 IRQ/rtclock/EOI。
调度、SMP、UFS 和根卷仍在更后面。
