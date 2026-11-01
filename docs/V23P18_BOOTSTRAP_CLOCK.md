# V23P18M：boot CPU、当前线程与早期时钟分组诊断

## 基线与范围

P17 已实机到达原始 chosen/stall/CPU拓扑与boot CPU编号读取返回，
实际SCTLR=0x3454593d（M1），9条CRC匹配并在一次录像观察到的重启后保留。
P18 延续同一冻结内核、固定ADT、BCM整段排除及日志映射，推进后续原始调用。
P17镜像和实机记录保留；独立原生平台源码集成候选仍分别验收。

## 六个独立检查点

| phase | 读取器名称 | 位置与含义 |
| --- | --- | --- |
| 16 | BCM_ROUTE_SKIPPED | 与P17相同的BCM整段排除 |
| 17 | M1_READBACK | MMU开启后直接读回M1 |
| 18 | BOOT_CPU_VALIDATED | 原始boot CPU发布、负数检查及max-CPU校验通过；0xfffffff0079e1ab8 |
| 19 | CURRENT_THREAD_SET | 原始线程bootstrap和machine_set_current_thread已返回；0xfffffff0079e1b0c |
| 20 | RTC_EARLY_ENTRY | 原始cpu_bootstrap已返回，进入rtclock_early_init；0xfffffff007405bd4 |
| 21 | RTC_EARLY_RETURN | 原始rtclock_early_init返回；0xfffffff0079e1b18 |

终点phase12记录实际SCTLR_EL1，要求M1，之后请求SYSTEM_RESET。
该终点尚未执行后续kernel_debug_string_early、timer_call_init、cpu_init或processor_bootstrap。
**到达终点后的自动重启是本版预设行为；复位来源仍须独立证据。**

## 钩子、栈与映射

原始BL及其LR全部保留。三个继续执行的钩子恢复31个GPR、SP、NZCV，
分别重放被替换的LDRSW、STR或SUB SP，然后回到原始下一条指令。
RTC入口钩子在原始函数prologue之前执行；先恢复原始LR/SP，再执行原SUB SP。
日志临时状态不跨原始调用放在SP以下。新路径没有原始栈切换、TTBR改写或日志映射拆除；
machine_set_current_thread写TPIDR/CONTEXTIDR及CPU当前线程字段，映射策略沿用P17。

ARM cookie=P18KERNL，独立槽值=P18S | ARM CRC。P17旧记录不成为P18终点。
只读冻结、满环形日志显式双确认新会话、较小字体、关机/重启菜单均保留。

## 解释限制

- 此基线的cpu_bootstrap实际只有RET；其返回不代表后续cpu_init完成。
- CURRENT_THREAD_SET证明原始setter返回；本版不单独持久记录实际TPIDR值。
- rtclock_early_init会注册回调并同步计算timebase换算参数，不代表定时器IRQ已工作。
- 主机模型中单核编号为0，线程指针与TPIDR_EL1一致；19.2MHz输入得到625/12的
  纳秒换算比例。系统寄存器和MMU等为模拟，不能当成实机读回。

## 主机校验

- 50项实际内核机器码检查通过，含原始路径、ABI、错误分支、失败/中断不误报、
  零频率原始panic路径、日志绑定、独立槽、映射守卫和已消耗ARM拒绝重用。
- 31项实际链接reader检查通过，含只读冻结、Query/ARM、满日志会话及内核往返。
- 字节相同的P11关机/重启app复用22项检查。
- 最终FD/FAT/EFI、源码哈希、内核允许修改范围/HMAC、非负载固件/SEC、
  Android v2镜像及分区容量核对通过。
- 主机检查中的特权寄存器、MMU别名、屏障、UEFI和SMC为模拟；后续独立实机结果见末节。

## 实机步骤（已按下述过程完成一次）

1. fastboot与录像就绪后，仅刷boot_b，核对B槽激活/可启动，进入无倒计时菜单。
2. 首先打开P18 reader拍完整初始页。旧记录允许存在；满日志时另行显式建立新会话。
3. 本次Query→ARM各一次，保存ID/CRC和六模板结果。
4. 返回菜单，只启动P18M一次，连续录像。
5. 自动重启后先打开reader，拍完整页面再做任何Query/ARM；若停住先保留画面。

不要直接再次启动已消耗ARM的内核。以research/v23p18-bootstrap-clock.json为当前结果依据。

## P18 首次实机 reader（2026-10-27）

已仅刷 boot_b，B 槽激活/可启动核对和 reboot 命令均成功。
照片确认本版 r1 reader、EL1，ENTRY/NOW VALID：next=0x23dc2、
size=0x3fff4，console CRC32=FFA3E073。P18 logical/raw=0/0，
冻结 checkpoint ID0，各检查点及终点命中均为0；本次 ID1/query0/armed0。

这是有效的满环形 console，P18 计数为0不代表整个 console 为空。
初始照片已私下保存并记录哈希；须先显式选择 New linear session 连续确认两次，
拍下 New session OK 页面后再 Query/ARM。尚无新会话成功、P18 内核尝试、
检查点或重启保留结果。父菜单未在照片中出现，不把 reader 身份当作父菜单观察。

## 新线性会话成功（2026-10-27）

后续照片显示 New session OK、NOW VALID next=size=0x40、logical/raw=1/1，
本次 ID1/query0/armed0、EL1。INIT CRC=5A2B8DA8，按旧 next=0x23dc2、
size=0x3fff4 及新 prior0/0 独立重算完全匹配；冻结 ENTRY 保持初始状态。
该 CRC 是 INIT 记录，不是 ARM。下一步在当前 reader 内 Query→ARM 各一次，
拍下 ID/ARM CRC/六模板结果后再允许单次 P18M。尚无内核或重启保留结果。

## Fresh Query/ARM（2026-10-27）

准备照片确认 NOW VALID next=size=0x180、logical/raw=6/6，本次
ID1/query1/armed1、EL1，PSCI=0x00010001、SYSTEM_RESET FEATURES=0，
状态 ARM + six invalid templates OK。ARM CRC=209C5114，按 P18 cookie
及 prior0x140/0x140 独立重算完全一致。冻结 ENTRY 和零检查点行保持初始快照。

下一步返回菜单，连续录像单次启动 P18M；重启后首先打开 reader 拍完整页，
不先 Query/ARM。预期完整终点 header0x380、14条记录只是推导值，尚未观察到。
本次未建立 P18 内核进度或重启保留结果。

## P18M 实机结果（2026-10-27）

14.277秒连续录像（428帧）确认 P18M 单次启动、黑屏、Lenovo/Android
标志、返回同版菜单，以及首次进入只读 reader。独立完整照片与录像可读字段一致：

- ENTRY/NOW VALID，next=size=0x380、logical/raw=14/14；最新ID1/phase12。
- 新 reader ID2/query0/armed0，状态只读；冻结结果保留本次 ID1。
- ARM CRC209C5114与启动前相同；ENTRY CRC664EEDB4，值209C511400C00808（M0）。
- 六条独立槽全部 L1/R1，值50313853209C5114。
- 实际终点 SCTLR_EL1=0x3454593d（M1），终点CRC912996CC。
- UNUSED_PRE、MAP_FAILURE、RESET_RETURN均无命中。

| phase | 检查点 | CRC32（独立重算匹配） |
| --- | --- | --- |
| 9 | ARM | 209C5114 |
| 10 | KERNEL_ENTRY | 664EEDB4 |
| 16 | BCM_ROUTE_SKIPPED | 25EB17EA |
| 17 | M1_READBACK | A656094C |
| 18 | BOOT_CPU_VALIDATED | BD767B76 |
| 19 | CURRENT_THREAD_SET | 96C1194C |
| 20 | RTC_EARLY_ENTRY | 8B369F02 |
| 21 | RTC_EARLY_RETURN | C778294C |
| 12 | BOOTSTRAP_STOP_SCTLR | 912996CC |

全部9条记录按ID、值及各自prior头独立重算匹配，六槽绑定本次完整ARM CRC。
照片显示的整个console CRC32为B923BC13；没有读取完整console字节，
故这项仅是reader显示值，不声称对整个console CRC独立复算。

本轮新增到达原始boot CPU编号发布与负数/max-CPU校验通过、线程bootstrap和
machine_set_current_thread返回、cpu_bootstrap的RET路径以及rtclock_early_init
入口/返回。日志在录像观察到的一次重启后完整保留。终点0xfffffff0079e1b18
有意结束并随后请求SYSTEM_RESET；自动重启与此设计相符，不能单凭重启推断崩溃，
复位来源仍需独立验证。PMIC菜单02000001→02000041本身不能确定来源。

该冻结基线的cpu_bootstrap仍仅RET；CURRENT_THREAD_SET不是独立TPIDR读回。
早期时钟返回不证明counter前进、timer IRQ或调度。后续kernel_debug_string_early、
timer_call_init、完整cpu_init和processor_bootstrap尚未执行。下一步先审查这组
原始边界及栈/映射/当前线程和时钟依赖，再设计后继诊断；无需重复本次P18。
