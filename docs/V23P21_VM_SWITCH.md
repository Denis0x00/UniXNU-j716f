# V23P21M：原始 TTBR1/TTBR0 切换诊断

## 当前状态

P21M已构建并通过主机校验，现已仅刷入boot_b，重启命令成功；初始reader、线性会话及fresh Query/ARM已确认，单次内核测试待进行。P20实机结果及旧镜像完整保留。
继续原始VM准备、physmap/protection与qsort，执行原始TTBR1、TTBR0设置函数，
完成原始TLBI/DSB/ISB后停在0xfffffff0073f5e0c，记录实际SCTLR_EL1并请求SYSTEM_RESET。
**这仍不是arm_vm_init整体返回，不进入后续pmap_bootstrap。**

## 六个独立检查点

| phase | 名称 | 范围 |
| --- | --- | --- |
| 16 | BCM_ROUTE_SKIPPED | 继续整段排除BCM |
| 17 | M1_READBACK | 初始MMU后读回M1 |
| 18 | LOG_BRIDGE_READY | 日志桥接通过守卫并读回，原始TTBR1调用前 |
| 19 | TTBR1_RETURN | 原始set_mmu_ttb_alternate返回 |
| 20 | TTBR0_RETURN | 原始set_mmu_ttb返回，原始TLBI之前 |
| 21 | VM_TLB_SYNCED | 原始TLBI/DSB/ISB之后 |

终点phase12另记SCTLR_EL1；cookie=P21KERNL、tag=P21S。每次启动需fresh Query→ARM。
原BL/LR和错误分支保留；三个返回hook恢复GPR/SP/NZCV后重放AND、ADRP或TLBI。
CRC实现合并为共享代码以容纳桥接，探针仍在原2800字节零洞内；独立CRC/ABI检查覆盖实际机器码。

## 跨切换日志映射

P20旧候选walker错误地把bit38纳入T1SZ=26的根索引；更正后高根已映射探针RX与栈RW/NX，
缺少的是低地址日志映射。更正仅改变主机解释，P20实机九CRC和镜像不变。
P21在两个新根安装之前检查TCR、MAIR、旧根关系、精确console子表描述符和新根空槽，
仅在新低地址根L1 slot2写入一个8字节描述符，复用原console专用L2/L3并屏障、读回。
64页日志仍为Normal NC、RW、PXN/UXN，两端相邻页不映射。原bootstrap表不修改。
切换后立即停在本轮终点，没有额外分配、释放或pmap调用；借用子表不能推广到更晚生命周期。

## 校验与限制

- 26项实际内核检查：原始根切换、返回ABI、边界空间、六独立槽、初始映射守卫、
  桥接守卫和读回失败、旧cookie与消耗ARM拒绝。原VM函数不模拟成功返回。
- 5项依赖检查：层级APTable/PXNTable和叶属性、单一根项写入、原始错误/中断不误报后续槽。
- 31项链接reader检查；相同P11电源app复用22项，小字体、关机/重启和无倒计时菜单保留。
- 最终FD/FAT/EFI、HMAC、源码/输入hash、修改白名单、非payload模块/SEC和Android v2镜像核对通过。

主机按38位VA/8位根索引选当前TTBR0或TTBR1，检查切换后实际访问页的PA与AF/AP/PXN，
并独立核对层级属性。完整Mach-O与DarwinBoot参数/ADT/尾部放置采用模型。
系统寄存器、物理/高地址别名、AT/PAR、TLB/缓存/屏障、SMC和UEFI仍为模拟。
不证明真实根切换、缓存一致性、完整VM、timer IRQ、GIC、调度或用户态。

## 实机顺序

1. 本轮fastboot和录像就绪后，仅刷boot_b并启动无倒计时菜单。
2. 首先打开P21 reader拍完整初始页，核对空间后再Query与ARM各一次，拍ID/CRC/模板状态。
3. 核对后返回菜单，只启动一次P21M，持续录像。
4. 重启后首先打开P21 reader拍完整页，先不Query/ARM；若停住先保存画面。

上游[bring-up顺序](https://unixnu.com/zh/porting/bring-up/)仍属阶段5早期内核；
按[设备记录要求](https://unixnu.com/zh/porting/device-repository/)分开源码、主机和设备证据。
冻结source pin/内核/ADT与native候选不改变，不搬用sheng/SM8550物理地址。
结构化记录：设备research/v23p21-vm-switch.json；固件docs/v23p21-validation.json为构建快照。

## boot_b 刷入与重启命令（2026-10-27）

本轮录像已获用户确认，镜像哈希、唯一设备、模式、解锁、槽数及容量核对通过。
仅刷入boot_b，激活B槽并核对可启动，重启命令成功。镜像SHA256：
`77f3a49bff49f9bf842cb4715649f3c862c0084b81564e31d1e997e92f180131`。

菜单及reader画面尚待照片确认；尚无本轮Query/ARM、内核启动或根切换实机结果。
下一步首先打开P21 reader拍完整初始页，先不Query/ARM。P20仍为最新内核实机里程碑。

## 初始只读 reader（2026-10-27）

完整照片确认 J716F V23P21 VM SWITCH TRACE r1、EL1；父菜单未在照片中出现。
ENTRY/NOW VALID，next=0x125fb、size=0x3fff4（满环形日志），
reader显示console CRC32=633B24C3，logical/raw均0，冻结checkpoint ID0。
八个检查点及四个终点/failure行均无命中；THIS RUN ID1/query0/armed0/CRC0。
PSCI与FEATURES显示0为未查询状态，不是功能不支持的结论。

有效满环形日志不满足探针所需的线性尾部条件，不能把容量减next当作可用空间。
需要先在当前reader选择New linear session两次，确认后丢弃旧逻辑历史，
旧数据仍可能被raw扫描视作陈旧记录；ENTRY冻结显示继续保留原始满环形状态。
先拍New session OK和NOW新日志头，再核对后执行Query/ARM；尚无P21内核或根切换结果。
原始照片仅私有保存，console CRC为照片显示值，未独立重算完整console。

## 显式线性会话成功（2026-10-27）

下一张完整照片显示New session OK；NOW VALID、next=size=0x40、logical/raw=1/1，
THIS RUN仍ID1/query0/armed0/EL1。ENTRY冻结保留原next0x125fb/size0x3fff4，属预期。
最后记录CRC=B00D3D5C；按reader格式重建INIT前48字节独立重算一致，
INIT携带旧next/size，记录自身头字段来自重置后的空日志。未直接读取RAM原始字节。
当前剩余线性空间262068字节，足够Query/ARM；冻结检查点及终点/failure仍零。
下一步在同一reader内Query→ARM各一次，拍完整成功状态再核对；尚无P21内核或TTBR切换结果。

## Fresh Query/ARM（2026-10-27）

照片确认NOW VALID next=size0x180、logical/raw6/6，THIS RUN ID1/query1/armed1，
EL1、PSCI1.1、SYSTEM_RESET FEATURES0，底部ARM + six invalid templates OK。
按P21 cookie5032314b45524e4c、ID1、phase9及ARM前头0x140/0x140重建记录，
CRC145911AB独立重算匹配；不是原始RAM转储。相对新会话0x40增长320字节，
符合四条Query记录加ARM；六条有效记录包含先前INIT，六个模板仍无效待内核提交。
ENTRY冻结旧满环形头、ID0和零检查点行为预期，不代表ARM失败。

下一步返回菜单并连续录像，只启动一次P21M；重启后首先打开P21 reader拍完整页，
先不Query/ARM。若停住先保留画面和等待时长。当前尚无内核、TTBR切换或重启保留结果。
