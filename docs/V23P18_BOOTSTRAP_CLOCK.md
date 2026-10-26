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
- 特权寄存器、MMU别名、屏障、UEFI和SMC为主机模拟；尚无P18实机结果。

## 实机步骤（尚未执行）

1. fastboot与录像就绪后，仅刷boot_b，核对B槽激活/可启动，进入无倒计时菜单。
2. 首先打开P18 reader拍完整初始页。旧记录允许存在；满日志时另行显式建立新会话。
3. 本次Query→ARM各一次，保存ID/CRC和六模板结果。
4. 返回菜单，只启动P18M一次，连续录像。
5. 自动重启后先打开reader，拍完整页面再做任何Query/ARM；若停住先保留画面。

不要直接再次启动已消耗ARM的内核。以research/v23p18-bootstrap-clock.json为当前结果依据。
