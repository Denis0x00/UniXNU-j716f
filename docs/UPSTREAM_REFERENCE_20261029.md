# sheng / pipa / umi 的分层复用与当前候选（2026-10-29）

本次按用户确认的方式推进：sheng 对照完整启动和日志流程，pipa/umi 对照
SM8250 实现，J716F 原厂及 Recovery 资料确定板级参数。不因 SoC 不同排除
通用源码，也不把另一个设备的硬件常量当成可直接使用的配置。

本次完成源码对照和**已有最终镜像实物的只读复核**，未改内核、ADT、固件、
菜单或准入策略；未构建、推送、运行 CI、刷写或发出设备命令。
[主机复核记录](../research/upstream-mainline-reference-20261029.json)不替代旧构建报告，
也不提升硬件验证等级。用户报告刚开机并打开 Read；当前照片仍待接收。

## 参考的具体实现

| 层次 | 固定参考 | J716F 对应位置与结论 |
| --- | --- | --- |
| 平台与构建 | [sheng board.toml](https://github.com/UniXNU/UniXNU-sheng/blob/08cde20edc3254ed6f7df8d0f5384fb887786e8f/board.toml) 中的共享 ArmDTPlatform/ArmCpu 与有序 archive | `board.toml` 已选择共享组件；最终 Mach-O 中有真实 provider/rtclock 定义。源码选择、链接、设备匹配是三个不同结论。 |
| 负载发现 | [sheng DXE 模块清单](https://github.com/UniXNU/uefi-sheng/blob/20ac23d5e0937106c2129303dfc931b3390ae660/Platforms/Xiaomi/shengPkg/Include/DXE.inc) 与 [共享 UniXNURamDiskDxe](https://github.com/UniXNU/UniXNU/blob/066c8d8315f83749d28bcae2faf3595718646be5/package/UniXNUPkg/Driver/UniXNURamDiskDxe/UniXNURamDiskDxe.c) | 专用 `prepare-native-firmware.py` 仍选择嵌入 FAT。现有最终 FV 含 J716fNativeFatDxe，不含 UniXNURamDiskDxe。不是标准 initrd 已完成。 |
| 固件日志 | [官方分阶段日志流程](https://unixnu.com/zh/porting/log-capture) 的收集器、AdvancedLogDxe、Tools | `board.toml` 的 `tools=true` 没有进入专用 `build-native-payload.py` 的实际菜单；实际 FV 也无 AdvancedLogDxe。仅改声明或加一个 INF 不能完成集成。 |
| 存储前日志 | [pipa BootRamoops.c](https://github.com/xVanTuring/UniXNU-pipa/blob/cdb3d62771e31d48d832853ee0c90964346d78da/package/BootRamoops.c) 和 [Linux 读取器](https://github.com/xVanTuring/UniXNU-pipa/blob/cdb3d62771e31d48d832853ee0c90964346d78da/package/linux-log/init.c) | 可借鉴连续 msgbuf、panic 尾部和批量导出。当前 `NativeXnu.inc` 是有界检查点日志，不是连续文本日志；不得复制 pipa 内存区间或自动复位策略。新读取器也需要验证恢复链是否保存 RAM。 |
| 真实中断 | [umi UmiArmCpuTrace.inc](https://github.com/reAAAq/UniXNU-umi/blob/3eb72578597d2ca16471b172d45eddc096a34a04/package/UmiArmCpuTrace.inc) | `J716fArmCpuObserver.inc` 已借鉴 IRQ1/2/8 的 enter、rtclock-return、EOI。`NativeXnu.inc` 将观测写入保留区；下一成果应是这些记录的实机回收，不再新增一套静态数组。 |
| 持久化日志与恢复 | [sheng BootLog.c](https://github.com/UniXNU/UniXNU-sheng/blob/08cde20edc3254ed6f7df8d0f5384fb887786e8f/package/BootLog.c)、[BootResetCore.h](https://github.com/UniXNU/UniXNU-sheng/blob/08cde20edc3254ed6f7df8d0f5384fb887786e8f/package/BootResetCore.h) | 可复用 boot ID、写入状态、快照/解码/归档的组织方式。其实际日志后端依赖 NativeUfs；SPMI v7/PMIC 地址及 userdata 边界属于 sheng，不用于本轮早期候选。 |

## 本次实际复核结果

从已有 `627dc31c...` 镜像重新解析 Android v2 → gzip/BootShim/FD → FV → FAT，
不是只读取旧 JSON 的通过字段：

- 镜像仍为 8,380,416 字节，全部 7 个负载文件、34 个 FV 文件 GUID 与旧审计一致。
- 内核仍为 `150c5c5f...`，ADT 仍为 `806ad48c...`，逐字节匹配原 staging 输入。
- 原生 provider 定义、source crypto HMAC 和初始 observer ABI 检查通过。
- 实际 ADT 的 CPU0、GIC 资源、19.2 MHz、SMC 与板级证据检查通过；不是驱动匹配成功。
- 菜单与当前 MAINT-v2 配置完全一致，维护身份仍为 `fb6ea084...`。
- 对同字节内核重新运行 `check-native-logger.py`：早期可达 711 条指令检查通过；
  独占 DATA 窗口、候选身份匹配，无外部 runtime/间接调用/SIMD。这不是物理映射或恢复证明。

## 推进次序：实测当前候选，随后收敛通用集成

### 当前现场只推进已有候选

1. 接收并审查用户刚打开的 Read 首页照片，核对版本、候选、DBGC next/size 和稳定性。
   旧照片、旧 transfer 或主机哈希不能代替本次现场。
2. 照片通过后，沿用 [MAINT v2](NATIVE_MAINTENANCE_V2.md)：第3项同一 EFI 应用内
   完整导出并保持打开；主机确认 256/256 帧、SHA/CRC、持久保存后才提供本次回执。
3. 再按现场执行受限 Init、独立 Read；fresh Query/ARM 和一次原生启动另作现场审查。
   不自动启动、重启、清环或放宽门槛；不重复刷写已经部署的镜像。
4. 原生试验的成功标准仍是入口/VM → 平台 → CPU0 → GIC → 真实 IRQ/rtclock/EOI →
   单核调度。RAM 跨复位保留及挂死恢复尚未合格；无日志记为未知，不能当作通过。

### 后续固件集成的具体缺项

这些是工程缺口，不应归因于 SoC 差异；也不是要求在当前测试前再制作一个维护版本：

- **标准 initrd**：直接复用共享驱动，先证明 BootShim/SEC 的 FDT→HOB 交接、
  `linux,initrd-start/end` 及物理区间所有权，再替换嵌入 FAT。不能只加模块就宣称迁移完成。
- **Advanced Logger**：一起核对平台收集器、DebugLib 接线、协议、库映射、DSC/FDF 和
  实际 Tools 菜单。现有菜单严格白名单必须显式适配，不能绕过；日志缓冲还需板级所有权检查。
  它用于固件，不替代 ExitBootServices 后的 XNU 日志。
- **统一构建与干净依赖**：现有普通 preflight 继续拒绝旧路径。把已用依赖差异固定到
  可追溯来源后，整合标准构建入口；不能用事后文件哈希冒称可干净重建。
- **存储成熟后的完整日志**：按 sheng 的写入/读取/解码分工接入板级有界后端；
  此前可评估 pipa 的存储前批量读取方案，但不能为导出而覆盖未备份的现存证据。

上述迁移若改变镜像或内核身份，应单独记录新产物；不得静默替换本轮固定对象。
本轮不做 UFS/根卷/用户态、SMP 或驱动整栈升级，不恢复 P22 逐函数复位分支。
