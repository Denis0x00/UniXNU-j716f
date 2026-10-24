# 下一轮：内存勘测与内核入口诊断

## 已完成

- V13 UEFI 内存勘测固件在 Ubuntu 编译完成，启动头 v2、固件卷、APRIORI、FAT
  和外部 ARM64 EFI 的独立产物检查通过。源码位于配套 uefi-j716f 的
  `Platforms/Lenovo/j716fPkg/Applications/J716fMemoryProbe`。
- macOS 源码构建及加密产物保存成功：
  [Actions 40100682659](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40100682659)。
  本地取回和解密仍需从已登录的 GitHub 下载加密 artifact。
- 本地 IPSW 已确认为 iPad7,1 / j120ap / 17.7.11 / 21H461。解压的 ARM64
  kernelcache SHA256 与官方 sheng 配置相同；纯元数据在
  `research/ipsw-j120ap-21H461.json`。Apple 二进制只留在本地。
- 已取得官方 UniXNU-sheng `251b291adf941b9953fe3ae66a808b9b626e164d` 的入口
  探针源码供后续移植。没有将 sheng 的 MMIO、ADT 或内存窗口当作 J716F 数据。

## 先完成 V13 内存勘测

当前板级静态表仅声明 `0xC0000000 + 0x0E000000` 的 DXE 区域（224 MiB）；
固定 DarwinBoot 默认窗口为 256 MiB，ADT 自定义窗口的最小值也是 256 MiB。
实际可分配范围必须以真机 `GetMemoryMap` 为准。尚未指定内核物理窗口。

V13 保持 V12 的固件内存描述、显示和按键基线。第一项启动外部 EFI，显示：

1. conventional memory 总量、最大 2 MiB 对齐范围及其起点。
2. 分页内存图：类型、物理地址、大小、属性。
3. 每页停屏等待拍照；短按电源下一页，末页返回菜单。

对齐容量只是上限估计：还未扣除内核基址低位余量，也没有实际分配内核窗口。
本候选没有真实内核，不调用 DarwinBoot Boot 或 ExitBootServices。

## 内核入口候选的后续条件

- 从加密产物恢复内核和三份静态库，按报告核对大小和 SHA256。
- 结合真机内存图与原厂保留区审计，决定可分配窗口；必要时先修正 UEFI 内存发布。
- 对实际源码内核与已固定的 Apple kernelcache 做预链接分析，确定真实组件依赖。
- 生成 J716F ADT，并审计启动参数、framebuffer、内存和探针位置。
- 在检查实际指令匹配后加入入口色条及 panic 停屏，再生成独立入口诊断镜像。

源码资格配置关闭 CPU/ADT 平台等硬件驱动；编译通过不代表这些驱动已适配。
`boot-progress` 也未编入当前配置，单独添加启动参数无法启用它。
物理 UFS、根卷和用户态仍在后续阶段。真机实验范围继续限于 boot_b。
