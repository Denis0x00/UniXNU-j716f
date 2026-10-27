# V23P22M：arm_vm_init 剩余流程与原始返回诊断

## 当前状态

P22M已构建并通过主机校验，尚未刷入。P21实机结果、旧源码及镜像完整保留。
本轮执行原始pmap_bootstrap、后续动态/复制窗口页表分配、静态区域记录补丁、
enable_preemption及栈保护检查，保留原始RET，停在调用方0xfffffff0079e1bbc。
目标是验证arm_vm_init整体返回；**现在只有主机结果，尚无P22实机结论。**

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
尚无P22设备记录，不证明完整系统VM、timer IRQ、GIC、调度、存储或用户态。
结构化记录：research/v23p22-vm-return.json；固件docs/v23p22-validation.json为构建快照。

## 实机顺序

1. 本轮fastboot与录像就绪后仅刷boot_b，激活B并启动无倒计时菜单。
2. P22 reader FIRST，先拍完整初始页；若日志满，按显式新会话步骤拍照核对。
3. 同一reader内Query→ARM各一次，拍ID/CRC/模板状态，核对后仅启动一次P22M。
4. 连续录像；重启后首先打开P22 reader拍完整页，先不Query/ARM或再次启动；若停住先保存画面。
