# V23P17M：chosen、stall 与 CPU 拓扑分组诊断

## 基线与目的

P16 已实机验证原始 WFE/misc 返回与立即重启后的记录保留。
P17 延续已验证的 BCM 整段排除和日志映射，让原始初始化继续经过一组相关路径。
它仍是冻结内核的终点诊断，与原生定时器源码集成候选分别构建。

## 六个独立检查点

| phase | 含义 |
| --- | --- |
| 16 | BCM 路由整段已排除 |
| 17 | MMU M1 直接读回 |
| 18 | 原始 WFE/misc 已返回，继续原来的栈变量初始化 |
| 19 | /chosen 的 SecureDTLookupEntry 已返回，保留原始结果检查及失败分支 |
| 20 | xnu_platform_stall 的 SecureDTGetProperty 已返回，保留原始属性处理分支 |
| 21 | 原始 platform_stall_panic_or_spin、ml_parse_cpu_topology 和 ml_get_boot_cpu_number 均已返回 |

phase12 终点记录实际 SCTLR_EL1，要求 M=1；之后请求 SYSTEM_RESET。
终点位于原始 arm_init `0xfffffff0079e1a98`，尚未发布 boot CPU 编号，
也未执行后续 max-CPU 检查、驱动匹配和调度。
查询“返回”检查点不等于查询成功；失败、stall 和 panic 均按原始控制流处理。

所有原始 BL/LR 保留。三个继续执行的 hook 恢复31个通用寄存器、SP、NZCV，
并执行被替换的原始 STR/CMP，再回到原始分支；终点主动结束。
ARM cookie=P17KERNL，槽值=P17S | ARM CRC；旧 P16 记录不会成为本版终点。

## 主机结果

- 45项实际内核机器码检查通过：原始查询/stall/拓扑路径、hook ABI、错误分支、
  中断的初始化、日志余量/CRC/绑定、映射失败、六个独立记录和 consumed ARM。
- 31项实际链接读取器检查通过：只读冻结、Query/ARM、终点绑定、满日志处理、
  会话取消/失败和内核往返。
- 关机/重启 app 与 P11 已验证版本逐字节一致，复用22项检查。
- FD/FAT/EFI、源文件哈希、内核允许修改范围/HMAC、非负载固件/SEC、
  Android v2 镜像及分区容量校验通过。
- 系统寄存器、MMU 别名、屏障、UEFI 与 SMC 为主机模拟，不能证明实机结果。

## 实机步骤与当前状态

1. fastboot 与录像就绪后，仅刷 boot_b、核对 B 槽可启动并进入无倒计时菜单。
2. 首先打开 P17 reader，保存完整初始页；旧记录允许存在，不要直接清日志。
3. 在本次 reader 中 Query → ARM 各一次，保存 ID/CRC/六模板结果。
4. 返回菜单，只启动 P17M 一次，连续拍摄启动及可能的重启。
5. 返回菜单后首先打开 reader，先拍完整页面再进行任何写入。

不要直接再次启动已消耗 ARM 的内核。若停住，先保留录像和画面。
本版是否有实机结果以 `research/v23p17-chosen-topology.json` 为准。

## 首次实机页面

已仅刷 `boot_b`，B 槽激活/可启动核对与 reboot 命令均成功。
首张照片确认 P17 r1 reader、EL1、ENTRY/NOW VALID，next=0x93b7、
size=0x3fff4；逻辑/原始 P17 计数和全部检查点命中均为0，
THIS RUN ID1、query0、armed0。这是有效的满环形日志，不是空日志。
父菜单没有出现在照片中；已观察的是 P17 reader 身份。

照片已保存后，先选 **New linear session** 连续确认两次（不要在中间切换选项），
拍下成功页面再 Query/ARM。该动作重置逻辑历史，旧字节仍可能作为 raw/stale 存在；
冻结 ENTRY 不变。尚无新会话成功、P17 内核启动、终点或重启保留结果。

后续照片确认新会话成功：NOW VALID next=size=0x40，logical/raw=1/1，
ID1/query0/armed0，INIT CRC=DF3BCB06；从旧next/size与新prior0/0重算完全匹配。
本阶段仅验证满环形日志的显式会话重建和 INIT 写入，尚未启动 P17 内核。

## P17M 实机结果

连续13.065秒视频确认 P17M 菜单身份、启动后的黑屏、Lenovo/Android 标志、
返回同版菜单，以及首次打开只读 reader。末帧391与独立照片显示同一结果：

- ENTRY/NOW VALID，next=size=0x380，logical/raw=14/14；新 reader ID2/query0/armed0。
- ID1 的 ARM6303C7DA 与启动前一致；ENTRY CRC4D5B8E21、SCTLR0xc00808（M0），
  高32位绑定此次ARM CRC。
- 六条独立检查点全部L1/R1，CRC依次为3D72FA9F、BECFE439、A5EF9603、
  8E58F439、93AF7277、DFE1C439，槽值均为503137536303C7DA。
- 终点phase12 CRC912996CC，实际SCTLR_EL1=0x3454593d（M1）。
  ARM、ENTRY、六槽及终点共9条CRC均独立重算匹配；日志头达到预期0x380。
- UNUSED_PRE、MAP_FAILURE、RESET_RETURN无命中；PMIC菜单值02000001→02000041，
  没有独立据此确定复位来源。

原始 `/chosen` 查询、stall属性处理、platform_stall_panic_or_spin、
ml_parse_cpu_topology和ml_get_boot_cpu_number返回路径已到达本版终点；
这组新记录在录像观察到的一次重启后完整保留。到达终点后诊断代码设计上请求
SYSTEM_RESET，不能把“自动重启”单独当作后续原始初始化崩溃的证据。
复位来源仍未独立验证，单个查询返回标记也不等于单独证明返回状态成功。

该终点在0xfffffff0079e1a98，尚未发布boot CPU编号、执行max-CPU校验或继续
cpu_bootstrap/rtclock_early_init；未验证定时器IRQ、GIC驱动、调度、存储或用户态。

## 下一组边界审查

原始反汇编显示后续将发布boot CPU编号、调用ml_get_max_cpu_number并检查边界，
建立当前线程/CPU状态，再调用machine_set_current_thread、cpu_bootstrap和
rtclock_early_init。下一版应分组记录这些边界，同时重新核对栈和日志映射的
生存期。此处仅审查；下一版尚未构建或刷入。独立原生平台源码候选仍待编译结果。
