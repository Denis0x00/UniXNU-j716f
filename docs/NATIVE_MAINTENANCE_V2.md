# MAINT v2：同一应用内完整导出，再安全 Init

2026-10-29 完整导出：[录像独立恢复 256/256 帧](../research/native-maint-complete-20261029.json)，
262144 字节，所有帧 CRC 与整区 SHA256/CRC 重算通过。SHA256 为
`5583ff6edf75f5bd2aa7ec2019548a3cebf891fc0c34d4302c810b8197e18811`，CRC `B6D19E53`。
同 profile/candidate/transfer，录像首帧与前一照片的帧文本逐字相同；接收时未混入照片。
原视频已私有复制并核对哈希；raw/frames/report 已落盘读回，另存同盘第二目录副本。
**这份快照不再需要补录；当前 Init 仍拒绝 INVALID_HEADER，没有生成初始化令牌。**
全区未找到当前原生会话、事件或 Init 格式的精确标记；不能据此归因写坏、映射错误或 XNU 崩溃。
请保持应用暂停，不 Init/ARM/Native、退出或重启。下一步审查显式冷启动会话建立及区域所有权，
固定内核/ADT/ABI 不变，不直接放宽现有无效头门槛。本轮未修改实现、构建或操作设备。

以下首帧状态为历史检查点：[实机 MAINT v2 首帧](../research/native-maint-first-frame-20261029.json)
已离线解码；profile/candidate 与固定构建匹配，帧 CRC 通过，屏幕 CRC 与前一 Read 一致。
当前 PAUSE、1000ms，transfer `000000039CA7ACD9`；只保存 **1/256 帧、1024 字节**，
总 SHA/CRC 仍只是设备声明，尚未独立重算。二维码头 `00B7266B/8E025595/2A002821`
更正前一照片的人工转录，仍为 INVALID_HEADER。原图与首帧文本已私有保存；未生成回执。
下一步直接在**当前仍打开的应用**录制完整 `1→256→1` 轮播再多十几秒，短 Power 播放/暂停；
保持二维码及白边清晰，上传原视频。不要退出/重启，也不要 Init/ARM/Native。

2026-10-29 更新：[当前 Read v3 照片](../research/native-read-coldboot-20261029.json)已收到。
用户确认完全关机后再开机；照片显示无效 DBGC 头、STABLE=1、无匹配会话。
**当前只能原样导出，不得执行下文步骤5的 Init，也不得 ARM/Native。**
v2 的有效满环/空间不足策略不覆盖冷启动无效头；完整导出也不会自动解除这个拒绝。
首帧和完整快照现均已保存验证；后续冷启动会话建立另作审查。
这不是 XNU 崩溃、映射错误或 warm-reset 保留失败的证明。下文保留 10-27 构建时的记录。

2026-10-27：用户已进入 fastboot 并允许按最小主线方案实施。
新固件构建、主机验证、最终镜像反解通过；仅 boot_b 刷写、B 槽确认和重启命令成功。
**当时仍待新菜单/Read 照片与本次完整导出；后续照片进展见上方，仍无实机 Init、ARM 或原生内核运行结果。**
脱敏记录：[本轮实现与部署](../research/native-maintenance-v2-20261027.json)。

## 固定对象与范围

- 原生内核仍为 CI40458123294/attempt1；预链接 SHA256
  `150c5c5f13a4337eb05218232f3a157429f0974ba49f71b4627789497132d074`。
- ADT 逐字节复用：`806ad48c85881b51f6acbeeea7ff4ec065f9edbe32d90f4aa304b5046faa64a3`。
- 新镜像 `j716f-native-log-40458123294-maint-v2-boot.img`，8,380,416 字节，SHA256
  `627dc31c8049c8b4948058f987691a5ce790319676c4b94f22044f6c4168f7bc`。
- 维护 profile：`fb6ea084e0ac3c4f9cf8acb37464d2e1e28835141acb099812cc844718946537`。
- 不改 NativeLog.h、内核/驱动、timer、映射、ARM ABI、8192 字节准入或重启策略。
  无代码洞补丁、逐函数复位、倒计时或自动原生启动。

相对[原设计](NATIVE_CONSOLE_V2_PLAN.md)的明确收缩：不引入跨应用临时协议。
菜单第3项是一个仍驻留的 EFI 应用：先冻结/导出，再在该 invocation 内输入回执和 Init。
退出即丢失本次上下文，必须新导出；无 NVRAM 或共享回调指针。
transfer 来自 CNTVCT，仅用于防误操作，不是安全熵、不可重放保证或启动认证。
主菜单缩字、Restart/Shutdown 暂缓；诊断文字本来就是 scale2，本轮仅优化信息断行。
不因此增加新内核 CI、UFS、根卷或用户态工作。

## 实际安全策略

`NativeMaintenance.h` 实现满环、满线性、有效但空间不足的线性头三类。
`next=size=0x3dff4` 仍能放下8192字节，拒绝 Init；从0x3dff5才是空间不足。
此前0x3f56e对应2688字节，但该照片不是本次现场。

整个物理区按8字节扫描；任意候选未消费 ARM、可识别但损坏/未知的原生头均拒绝。
头非法、空间充足、不稳定、旧版本/错 profile/错 transfer 回执、现场变化均不能提交。
这不证明未知历史格式或标记已损坏的记录不存在。

J716F2 的256帧包含全部262144字节，每帧1024字节载荷；host 独立重算整区SHA/CRC，
新目录落盘、fsync并读回后才返回回执。回执绑定 profile、candidate、SHA和transfer。
两次确认分别要求“音量上→Power”和“音量下→Power”；重复Power不能完成确认。
队列残留先排空，错误或64键洪泛中止。方向错误清除输入；无自动重试。

提交前重算SHA并两次逐字节对比live。写入仅前272字节；先失效signature，写256字节
初始化记录和next/size，最后发布signature。后部不清零，原字节保持不变。
成功为`next=size=0x104`，**NOT ARMED**。Read v3独立验证v2记录，不写RAM。
TPL不能防DMA/安全世界并发；保留RAM所有权、永久映射和挂死恢复仍未实机合格。

## 10-27 原计划：仅适用有效满环／空间不足头，非当前冷启动现场

1. 先拍新菜单和第1项Read完整页；核对当前头和稳定性，暂不ARM/Native。
2. 返回菜单，进入**第3项 MAINT v2**，不是第2项历史v1 Export。
3. 拍首帧/transfer/profile，录完256帧；Power播放/暂停。保持同一应用打开、暂停，
   不退出、不重启、不进fastboot。默认每帧1000ms；光学实机解码尚待本轮验证。
4. 主机以明确profile接收并确认256/256帧、SHA/CRC和持久保存。部分视频不给回执。
5. 现场与回执均核对后，长Power进入选项（默认Exit）；音量上选择Init审查，再Power。
   按新回执输入16位，完成两个方向分离的确认。失败就停止，不绕过、不填满日志区。
6. 拍成功页，再退出到第1项Read，拍`INIT v2 VALID / NOT ARMED`和完整身份。
   此后再审查fresh Query/ARM/一次原生启动，不将初始化当作内核证据。

此前未完整导出的0x3f56e快照在用户再次重启/fastboot前未保存，现存与否未知。
旧完整v1 dump和照片均保留，但不能冒充本轮备份。刷写后必须重新走上述流程。
此次没有分区读回；fastboot成功不等于菜单或内核已在设备运行。

## 验证与未完成项

- GCC/Clang：实际C头分类边界、完整帧/回执与独立Python互验、pending/suspect拒绝、
  现场变化、有界写、四处提交屏障中断、Init后正常ARM兼容；另有4组host持久化/拒绝测试。
- 原有v1、logger和严格菜单白名单双编译器回归通过。
- 实际linked EFI：14组维护模拟和14组Read/ARM回归；UEFI/输入/计时/TPL/大块CRC模拟，
  SHA、回执、提交、Mu QR编码与字形绘制执行实际指令。独立ZXing解码新2295字符帧通过。
  已检查2560×1600合成显示和Init后Read v3；这些不是物理屏幕/按键/掉电测试。
- 最终boot→FD→FV→FAT→内核/ADT/EFI反解一致；测试EFI与最终打包EFI逐字节相同。
- 新依赖快照1907文件/826依赖文件；961个共用输入与v1完全一致，无本轮共享树修改。
  历史dirty依赖仍在，不冒称全干净重建。
- 没有新源码推送/CI；内核未重编译。标准initrd、Advanced Logger/OneSend、干净依赖闭包、
  原生入口→VM→平台→CPU→GIC→真实timer IRQ/rtclock/EOI→单核调度仍待各自证据。

生产host测试：`python scripts/test-native-maintenance.py --cc gcc`，再以`--cc clang`运行。
实际EFI构建/模拟及部署证据留私有目录；公开只列脱敏哈希，不包含原始日志、镜像或设备标识。
