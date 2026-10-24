# J716F V15：ADT 与真实 XNU 入口诊断

已在 Ubuntu 构建并通过主机审计。本轮未刷写；真机仍为已验证的 V14。

## 本版组成

- 沿用 V14 内存表：原 DXE/FD/Guard 布局和新增384 MiB候选均不变。FD仍位于0xCE000000，大小7 MiB。
- 独立只读64 MiB FAT32卷，含真实实验预链接内核（52,113,408字节）、J716F ADT（10,396字节）和无自动启动的菜单配置。
- ADT硬件数据来自原厂ZUI DTB0/DTBO8，并与TWRP FDT交叉核对：GIC distributor=0x17A00000、redistributor=0x17A60000、stride=0x20000，CPU0 affinity=0、时基19.2MHz。
- 单核，无UFS节点、根卷、Data ramdisk、trust cache sidecar或Apple板级身份。/chosen DRAM描述主连续物理bank；boot_args仅授予实际分配的256 MiB窗口。
- 固定官方DarwinBoot，真实内核副本带入口蓝条、MMU开启前品红条及panic停屏探针，无自动复位探针。
- 参数：`-v serial=0 debug=0x14e cpus=1 -no_slto_panic`。本板UART尚未验证，因此不指定示例中的serial=3；boot-progress也未编入本内核。

## 已完成的主机验证

- 真实ParseKernel/AdtNode函数体以Clang+UBSan trap在主机编译：接受本版真实输入，拒绝5个损坏输入。这不模拟完整Boot调用或硬件。
- 独立解码ADT、核对硬件来源、CHRP NVRAM头与Adler32、内存窗口及boot_args布局。
- 独立读取FAT32的LFN与簇链，核对每份负载的字节身份；验证64 MiB大FFS文件的扩展头和校验和。
- 核对32个源码PE、无厂商PE、APRIORI、压缩FD/FV身份、Android header-v2、固定ZUI DTB及7 MiB FD。

## 状态与后续

独立入口镜像已在本地构建，尚未刷写。真实XNU入口、ExitBootServices与新负载卷都需真机证据。
较大负载会增加UEFI堆开销，运行时的256 MiB AllocateWindow仍是检查点。
源码资格内核未启用ArmDTPlatform/ArmCpu，此版不宣称平台专家、UFS或iPadOS可用。

纯元数据见 [v15-entry-preparation.json](../research/v15-entry-preparation.json)。
ADT生成器：`scripts/make-j716f-entry-adt.py`，默认读取相邻的板级证据JSON；每次生成新的64字节随机种子。
Apple负载、生成ADT、FAT、固件与照片仅留本地。
