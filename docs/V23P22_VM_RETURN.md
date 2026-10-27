# V23P22M：arm_vm_init 剩余流程与原始返回诊断

## 当前状态

P22M已构建并通过主机校验，现已仅刷入boot_b；B槽激活/可启动核对及重启命令成功，初始reader已确认，VALID满环形日志需先新建线性会话。P21实机结果、旧源码及镜像完整保留。
本轮执行原始pmap_bootstrap、后续动态/复制窗口页表分配、静态区域记录补丁、
enable_preemption及栈保护检查，保留原始RET，停在调用方0xfffffff0079e1bbc。
目标是验证arm_vm_init整体返回；**内核目前只有主机结果，尚无P22内核实机结论。**

## 日志子表生命周期审查

固定XNU源码d8b80295118ef25ac3a784134bcf95cd8e88109f将bootstrap_pagetables声明为
SECURITY_READ_ONLY_LATE静态数组。冻结Mach-O的符号位于0xfffffff007179000，
65536字节，属于__DATA_CONST,__const；日志借用其中偏移0xe000/0xf000两页。
它们属于内核静态镜像，原始动态分配器从boot_args.topOfKernelData之后分配。
固定机器码、原始pmap_bootstrap路径和本轮主机写入跟踪未发现截至返回时对该数组的回收或改写。

原始TTBR切换后，cpu_tte/invalid_tte指针转换为physmap别名。
主机模型使用共享物理后备内存，并同时核对当前页表描述符与ptov反向转换，
避免把physmap别名误当成新内存、复制后数据失配或假定函数成功返回。
八个边界检查完整bootstrap数组不变，所有切换后写入均检查是否触碰该数组，
分配器avail_start写入检查在初始top与模型所有权窗口之间。
桥接仍只写新低根空L1 slot2的一项8字节描述符；日志64页仍NC/RW/PXN/UXN，两端页不映射。

**结论仅适用于本轮立即返回终点。** 后续arm_init、内存回收、线程/用户映射必须再次审查。
给原本invalid的TTBR0加入日志映射是临时诊断策略，不能推广成正式用户地址空间方案。
源码参考：[arm_vm_init](https://github.com/apple-oss-distributions/xnu/blob/d8b80295118ef25ac3a784134bcf95cd8e88109f/osfmk/arm64/arm_vm_init.c)、
[pmap_bootstrap](https://github.com/apple-oss-distributions/xnu/blob/d8b80295118ef25ac3a784134bcf95cd8e88109f/osfmk/arm/pmap/pmap.c)。

## 六个独立检查点

| phase | 名称 | 所在边界 |
| --- | --- | --- |
| 16 | BCM_ROUTE_SKIPPED | 整段BCM路由继续排除 |
| 17 | M1_READBACK | 初始MMU开启后读回 |
| 18 | LOG_BRIDGE_READY | 0xfffffff0073f5de4，桥接守卫与读回完成，原始TTBR1调用前 |
| 19 | PMAP_BOOTSTRAP_RETURN | 0xfffffff0073f61d0，pmap_bootstrap及紧随其后的disable_preemption均已返回 |
| 20 | VM_FINAL_READY | 0xfffffff0073f64e0，动态/复制窗口表、静态区域补丁、启用抢占及栈保护检查后 |
| 21 | ARM_VM_RETURN | 0xfffffff0079e1bbc，原始RET已返回调用方，终点 |

phase12记录实际SCTLR_EL1并请求SYSTEM_RESET。cookie=P22KERNL，slot tag=P22S。
原始arm_vm_init调用BL、函数RET、失败分支和152处BL保持；非终点hook恢复GPR/SP/NZCV并重放原指令。
探针仍2776/2800字节；不扩洞、不分配新诊断页面、不改旧内核或native候选。

## 校验及限制

- 26项内核机器码检查：原始VM完整返回、ABI、边界空间、六独立槽、原映射及桥接守卫、消耗ARM拒绝。
- 8项依赖检查：层级权限、精确根项变化、静态子表生命周期、原始中断/栈保护失败和子表损坏拒绝。
- 31项链接reader检查，包含满环形日志显式会话、Query/ARM和真实组装内核往返；相同P11电源app复用22项。
- 两轮UEFI构建，最终FD/FAT/EFI、源码hash、HMAC、白名单、非payload模块/SEC及Android v2镜像审计通过。

所有寄存器、初始boot参数、AT/PAR、TLB/缓存/屏障效果、SMC与UEFI交互仍为模型。
共享后备内存只保证主机别名一致，不证明真实缓存一致性。
尚无P22内核启动记录，不证明完整系统VM、timer IRQ、GIC、调度、存储或用户态。
结构化记录：research/v23p22-vm-return.json；固件docs/v23p22-validation.json为构建快照。

## 实机顺序

1. 本轮fastboot与录像就绪后仅刷boot_b，激活B并启动无倒计时菜单。
2. P22 reader FIRST，先拍完整初始页；若日志满，按显式新会话步骤拍照核对。
3. 同一reader内Query→ARM各一次，拍ID/CRC/模板状态，核对后仅启动一次P22M。
4. 连续录像；重启后首先打开P22 reader拍完整页，先不Query/ARM或再次启动；若停住先保存画面。

## 首次 boot_b 刷写命令记录（2026-10-27）

用户已确认本轮 fastboot 与录像就绪。固定镜像大小8,347,648字节、SHA256与主机合格清单一致。
唯一设备、型号、fastboot模式、解锁、槽数和boot_b容量门槛通过；仅刷入boot_b。
B槽已激活，current-slot=b、slot-unbootable:b=no核对通过；reboot命令成功。
镜像SHA256：`c49fb7542528ca3a8aca005a2f0f4fb304e6a43f379da65555a6525519e3ced2`。

首次刷写时以上仅为命令证据；随后初始reader照片见下节，父菜单与内核启动尚未由照片/录像确认。
首次刷写后的初始页已采集；满环形日志需先显式新建线性会话，具体步骤见下节。
本轮尚无P22 VM返回、日志保留或原生IRQ结论，P21仍为最新内核实机里程碑。

## 初始只读 reader（2026-10-27）

完整照片确认 J716F V23P22 VM RETURN TRACE r1、EL1；父菜单未在照片中出现。
ENTRY/NOW VALID，next=0x2a871、size=0x3fff4（满环形日志）；
reader 显示 console CRC32=BE70D923，logical/raw 均0，冻结 checkpoint ID0。
八个检查点及四个终点/failure 行的命中、CRC和值均0；THIS RUN ID1/query0/armed0/CRC0。
PSCI 与 FEATURES 显示0是尚未查询的状态，不代表不支持。

满环形日志的可用线性尾部为0，不满足 Query 所需896字节；不能把容量减 next 当成可用尾部。
下一步在当前 reader 的 New linear session 项连续确认两次，中间不要移动选择。
这会丢弃旧逻辑历史，旧数据字节仍可能作为 raw/stale 被扫描，冻结 ENTRY 保留原状。
出现 New session OK 后先拍完整页，核对 NOW 和 INIT 记录后再安排 Query/ARM。
原始照片仅私有保存；完整 console 字节未取得，照片显示的 console CRC 未独立重算。
本轮尚未观察到内核启动、VM 返回或重启后保留，P21仍为最新内核实机里程碑。
