# V23P19M：startup、timer、CPU 与 processor 分组诊断

## 状态与范围

P19M 已仅在boot_b完成受控实机测试：原始startup、timer_call_init、cpu_init、
processor_bootstrap返回，9条CRC匹配并经一次录像观察到的重启后保留。
本节为当前状态，以下刷入/初始/ARM章节保留当时观察；最终结果见文末。
本轮保留冻结内核、固定 ADT、BCM 整段排除、日志映射和一次性 Query→ARM。

原始 arm_init 的顺序经反汇编核对为：

`kernel_debug_string_early → kernel_startup_bootstrap → timer_call_init → cpu_init → processor_bootstrap`

其中 startup 不能遗漏：它按24字节项排序原始表，再初始化到 subsystem3。
主机执行了849次、27类原始早期回调，涉及 tunable、超时换算、锁、SMR 等；
没有用模拟返回值跳过这些原始函数。debug 路径会读取 CNTVCT_EL0。

## 独立检查点

| phase | reader 名称 | 含义 |
| --- | --- | --- |
| 16 | BCM_ROUTE_SKIPPED | 延续整段排除 BCM 的路径 |
| 17 | M1_READBACK | MMU开启后直接读回 M1 |
| 18 | STARTUP_RETURN | debug/string与startup均返回，进入timer_call_init；0xfffffff0072e5724 |
| 19 | TIMER_CALL_RETURN | timer_call_init返回，进入cpu_init；0xfffffff0073f1014 |
| 20 | CPU_INIT_RETURN | cpu_init返回，进入processor_bootstrap；0xfffffff0072aa9b8 |
| 21 | PROCESSOR_RETURN | processor_bootstrap返回；0xfffffff0079e1b34 |

终点 phase12 记录实际 SCTLR_EL1，要求 M1，随后请求 SYSTEM_RESET。
**自动重启是该终点的预设行为；只读 reader 的记录决定本轮到达哪里。**
尚未执行后续 arm_init/arm_vm_init，不把回到菜单视为崩溃证据。

## ABI、栈与硬件依赖

五条原始 BL 和 LR 不变；三个函数入口钩子先完整恢复31个GPR、SP、NZCV，
再重放原始 SUB SP 或 STP prologue。日志临时状态不跨原始调用保留在SP以下。
probe不使用SIMD，不改变原始失败分支。cookie=P19KERNL，槽值=P19S | ARM CRC。

cpu_init 原代码短暂切换 SPSEL1 检查异常栈非零，再恢复 SPSEL0；其 boot CPU
路径读取 MIDR、CLIDR、CCSIDR、debug ID。原始 do_cpuid 会把架构字段规范为12，
这是冻结基线已有行为，P19未添加CPU身份伪造。boot CPU/processor的原始分支
绕过次CPU等待和次CPU专用调度初始化。新范围内没有观察到主机路径改写TTBR。

主机读取的 MIDR/cache/debug/DAIF/counter、SP bank、MMU别名、屏障、SMC和
UEFI均为明确模型，不是平板读数。constant counter 不验证前进；timer_call_init
及processor timer结构初始化不证明timer IRQ、GIC或调度已经工作。

## 验证

- 57项实际内核机器码检查通过，包含独立槽、ABI、映射守卫、模板绑定及中断路径。
- 另6项依赖检查通过：原始startup断言、CPU架构失败、三个函数体内中断不误报后续槽；
  成功路径849次回调和processor_count=1，并检查191个高地址数据页的原始页表映射。
- 31项实际链接reader检查通过，含只读冻结、满环形新会话、Query/ARM及内核往返。
- 相同P11关机/重启app复用22项检查；较小菜单字体和电源选项保留。
- 最终FD/FAT/EFI、输入/源码哈希、内核修改白名单、HMAC、非负载固件与SEC、
  Android v2镜像及容量核对通过。镜像身份见结构化结果。

## 实机测试

1. fastboot和录像就绪后，仅刷boot_b，核对B槽并启动到无倒计时菜单。
2. 首先打开P19 reader，拍完整初始页。仅在提示空间不足时显式双确认新会话。
3. Query→ARM各一次，拍下本轮ID、ARM CRC及六模板结果。
4. 返回菜单，仅启动一次P19M，持续录像。
5. 自动重启后首先打开P19 reader拍完整页，再做其他操作；若停住先保留画面。

每次内核启动都要fresh Query/ARM；已消耗ARM直接再次启动会按设计停止。
Query需要896字节线性余量。既有P17/P18镜像、源码和实机证据保留。

## 与上游/sheng的关系

继续遵循[官方移植流程](https://unixnu.com/zh/porting/)、
[bring-up验收](https://unixnu.com/zh/porting/bring-up/)及已固定的
[上游/sheng对照](PROGRESS_AFTER_V23P16_20261026.md)。
BCM整段排除方向保留；SM8550/sheng地址和驱动常数不搬到SM8250。
独立[原生平台源码候选](NATIVE_PLATFORM_SOURCE_CANDIDATE.md)的编译成功，
与本版冻结诊断及将来的timer IRQ实机验收分别记录。

以 `research/v23p19-startup-init.json`（设备仓库）与
`docs/v23p19-validation.json`（固件仓库）为构建快照；设备仓库结构化记录另含本轮实机结果。

## boot_b 刷入与重启命令（2026-10-27）

用户确认本轮fastboot和录像就绪后，重新核对镜像SHA256、设备模式、解锁、
槽数及boot_b容量，仅刷入boot_b，激活B槽并核对current-slot=b、B可启动；
重启命令成功。镜像SHA256：
`7a68766b1f8c05a6bf373d93811c7d1eac30fa84604c4b324e0f9f81afebf7d8`。

父菜单/reader页面尚待照片确认，尚未观察到本轮Query/ARM或内核启动。
下一步首先打开P19 reader拍完整初始页，再决定本轮准备操作。
刷入与重启命令成功不代表四组原始初始化已在平板通过。

## 初始只读 reader（2026-10-27）

完整照片确认 J716F V23P19 STARTUP TRACE r1、EL1。ENTRY/NOW VALID，
next=size=0x2cbb8，reader显示console CRC32=22FC408C；logical/raw均0，
冻结checkpoint ID0、八行及四个终点/failure行全部无命中。
THIS RUN ID1/query0/armed0/CRC0，处于只读初始状态。父菜单未在照片中出现。

剩余线性空间78908字节，超过Query所需896字节，不需要新建线性会话。
下一步在当前reader内Query→ARM各一次，拍下ID/ARM CRC及模板结果，
核对后才启动一次P19M。尚无本轮Query/ARM、内核检查点或重启保留结果。
console CRC仅为照片显示值，没有完整console字节进行独立重算。

## Fresh Query/ARM（2026-10-27）

准备照片确认 NOW VALID next=size0x2ccf8，logical/raw=5/5，本次
ID1/query1/armed1、EL1，PSCI1.1、SYSTEM_RESET FEATURES0；状态为
ARM + six invalid templates OK。ARM CRC17BBA853按P19 cookie、ID1和
prior0x2ccb8/0x2ccb8独立重算匹配。相对初始头增长320字节，符合四条Query记录加ARM。
冻结初始ENTRY0x2cbb8/CRC22FC408C、ID0及零检查点行不变，是初始快照。

接下来返回菜单，连续录像，仅启动一次P19M；重启后首先打开reader拍完整页，
不先Query/ARM。尚无内核或重启保留结果；完整终点header0x2cef8只是推导值。

## 实机结果（2026-10-27，取代以上待测状态）

连续录像11.365秒，341帧全部解码：一次P19M启动、交接条、黑屏、Lenovo/Android
启动画面、回同版菜单，随后首次进入只读reader。独立完整照片与录像尾段一致。
ENTRY/NOW VALID next=size0x2cef8，logical/raw13/13；冻结ID1，最新phase12。
新reader为ID2/query0/armed0，未重新Query/ARM。相对fresh ARM头增长512字节，
符合ENTRY、六槽和终点；六槽全部L1/R1。

| 记录 | 独立重算并匹配的CRC32 |
| --- | --- |
| ARM | 17BBA853 |
| KERNEL_ENTRY | F85FF937 |
| BCM_ROUTE_SKIPPED | AC7FE282 |
| M1_READBACK | B4346A82 |
| STARTUP_RETURN | 9CE8F282 |
| TIMER_CALL_RETURN | 84A37A82 |
| CPU_INIT_RETURN | 02A26A6A |
| PROCESSOR_RETURN | 1AE9E26A |
| INIT_STOP_SCTLR | 71ECEFD3 |

终点实际SCTLR_EL1=0x3454593d（M1），位于0xfffffff0079e1b34。
原始debug/string与kernel_startup_bootstrap、timer_call_init、cpu_init、
processor_bootstrap均返回。这是本轮新增的有界实机进展；849次/27类回调是主机
执行统计，未在实机单独计数。记录在录像观察到的一次重启后完整保留。
终点预设请求SYSTEM_RESET，自动重启符合设计；复位来源未独立证明。
整console CRC显示AB0560BF，仅记录显示值，未取得全部字节重算。

尚未执行arm_vm_init，不证明counter持续前进、timer IRQ、GIC、调度或用户态。
下一步审查剩余arm_init及arm_vm_init的映射/栈生命周期，再设计下一组诊断；
无需重复本轮P19启动。原始媒体仅私有保存，公开结构化记录保留媒体SHA256。
