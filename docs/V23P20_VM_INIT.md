# V23P20M：arm_vm_init 早期虚拟内存准备诊断

## 当前状态

已构建并通过主机校验，现已仅刷入boot_b，重启命令成功；初始画面待确认。P19仍是最新实机里程碑；其原始startup、
timer_call_init、cpu_init、processor_bootstrap返回及一次重启保留证据保持不变。
本轮继续原始arm_init调用arm_vm_init，停在arm_vm_prot_init返回后的
0xfffffff0073f5c04，记录实际SCTLR_EL1/M1并有意请求SYSTEM_RESET。
**本版不会让arm_vm_init整体返回，也不会启用它新建的TCR/TTBR配置。**

## 六个独立检查点

| phase | 名称 | 到达范围 |
| --- | --- | --- |
| 16 | BCM_ROUTE_SKIPPED | 继续排除原BCM整段 |
| 17 | M1_READBACK | 初始MMU开启后读回M1 |
| 18 | VM_ENTRY | 原始arm_vm_init入口；0xfffffff0073f5298 |
| 19 | VM_TABLES_READY | early_random、初始页表根/上层表分配返回；0xfffffff0073f55b8；表尚不完整 |
| 20 | VM_LAYOUT_READY | 内核段/section处理与vm_set_page_size返回；0xfffffff0073f5b28 |
| 21 | VM_PROT_RETURN | 原始arm_vm_prot_init返回；0xfffffff0073f5c04 |

终点phase12另记实际SCTLR_EL1，要求M1。保留一次性Query→ARM、CRC绑定与
六个互不复用的提交字节。cookie=P20KERNL，tag=P20S。新版本reader拒绝旧cookie。
没有替换原始VM函数为成功返回；原BL/LR及错误分支保留。
返回探针恢复GPR/SP/NZCV，再重放SUB SP、STR X27或ADRP X8。

## 页表、日志与栈审查

此处仍用初始bootstrap页表；新的VM代码在主机成功路径中不修改活动bootstrap
页表，也未改写TCR/TTBR。活动表仍映射日志64页为Normal NC且不可执行，探针与栈可达。
原始VM分配的候选根位于DarwinBoot拥有窗口的空闲尾部。此终点下它们尚未形成
完整的高地址映射，且均没有日志低地址映射；不能直接假设下一步切换后仍能记日志。
后继需要核对完整新表、probe代码可执行权限、栈及日志映射，再接入切换后的检查点。

主机模型扩展为加载完整Mach-O段，按DarwinBoot算法将boot_args/ADT放在所有段
末端之后，并对齐TopOfKernelData；不再使用此前仅覆盖早期内核窗口的简化布局。
AT/PAR由实际初始页表walk生成。219个高地址数据页的初始映射被核对。
寄存器、物理/虚拟别名、屏障及SMC仍是模型；不等于平板上的MMU或缓存验证。
原始early_random执行不证明真实熵质量，固定counter也不证明counter前进。

## 构建校验

- 27项实际内核机器码检查：原始VM准备/保护路径、ABI、独立槽、中断不误报、
  映射守卫、边界空间、旧cookie与已消耗ARM拒绝。
- 5项依赖检查：成功路径与描述符walk、TopOfKernelData越界、零内存、
  page-size溢出、保护区起点越界，均保留原错误路径且不误记终点。
- 31项实际链接reader检查，含只读冻结、新会话、Query/ARM、完整内核往返及中断读取。
- 相同P11电源app复用22项；较小字体、重启/关机菜单保留。
- 最终FD/FAT/EFI、源码/输入哈希、HMAC、修改白名单、非payload固件/SEC及
  Android v2镜像检查通过；身份见结构化记录。

设备仓库记录为`research/v23p20-vm-init.json`，固件构建快照为
`docs/v23p20-validation.json`。本轮仍未验证timer IRQ、GIC、调度、存储或用户态。

## 实机顺序

1. fastboot与本轮录像就绪后，仅刷boot_b，启动到无倒计时菜单。
2. 首先打开P20 reader，拍完整初始页；只在空间不足时按提示双确认新会话。
3. Query、ARM各一次，拍ID/ARM CRC及六模板状态。
4. 返回菜单，仅启动一次P20M，持续录像。
5. 重启后首先打开P20 reader拍完整页，再进行其他操作；若停住先保留画面。

每次重新启动都必须fresh Query/ARM。终点有意请求复位，不能仅凭自动重启判崩溃。
目前已仅刷入boot_b，尚无P20内核实机结果；源构建pin、固定内核/ADT和原生平台候选均未改变。

## 上游方向

依据已审查的[bring-up顺序](https://unixnu.com/zh/porting/bring-up/)、
[设备记录要求](https://unixnu.com/zh/porting/device-repository/)及
[现有上游/sheng对照](PROGRESS_AFTER_V23P16_20261026.md)推进阶段5早期内核。
保留BCM整段排除；不复制SM8550/sheng物理地址，也不把主机结果写成实机结论。

## boot_b 刷入与重启命令（2026-10-27）

用户确认本轮fastboot和录像就绪，镜像哈希、设备模式、解锁、槽数及容量核对通过。
仅刷入boot_b，激活B槽并核对可启动，重启命令成功。镜像SHA256：
`d464f16c6dd23b4ac6abdd5ab3981197ec8cc3720b6dd475956607649a01d9c4`。

菜单及reader画面尚待照片确认，未观察到本轮Query/ARM、内核启动或VM结果。
下一步首先打开P20 reader拍完整初始页，先不Query/ARM。P19仍为最新实机里程碑。

## 初始只读 reader（2026-10-27）

完整照片确认 J716F V23P20 VM TRACE r1、EL1。ENTRY/NOW VALID，
next=size=0x2c11f，reader显示console CRC32=5171FF83；logical/raw均0，
冻结checkpoint ID0、八行及四个终点/failure行全部无命中。
THIS RUN ID1/query0/armed0/CRC0，处于只读初始状态。父菜单未在照片中出现。

剩余线性空间81621字节，超过Query所需896字节，不需要新建线性会话。
下一步在当前reader内Query→ARM各一次，拍下ID/ARM CRC及模板结果，
核对后才启动一次P20M。尚无本轮Query/ARM、内核检查点或重启保留结果。
console CRC仅为照片显示值，没有完整console字节进行独立重算。
