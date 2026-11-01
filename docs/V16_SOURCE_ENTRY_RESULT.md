# J716F V16：真实 XNU 入口结果

V16 使用 iPadOS 17.7.11 的 source-prelinked ARM64 内核、J716F ADT 和已审计的
UEFI FD，全部装入独立 FAT 诊断卷。真机只写入 `boot_b`；`boot_a`、vbmeta 和
userdata 未修改。

## 主机产物

- GitHub Actions source build：
  [run 40154302088](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40154302088)
- source kernel SHA-256：`8fe76c67afb6effa50db6c5bece7c92ea1e22c22181083e5d5df6372e7df534e`
- J716F ADT SHA-256：`b68fc49b2d9bb6b0a387e59a7816fd4848dffc3bde706a439cbbff3cbebb1aa7`
- boot image SHA-256：`1bff65fab46c7e4639e3025fb02aed8513265d97bfe75d62218bef3b89492110`
- 本地操作记录：`v16-flash-operation.json`（仅保存在工作区，不上传私有负载）

## 真机观察

设备 `HA1Q9SKY` 在 `current-slot=b` 下启动 V16。屏幕连续显示三条阶段条：

1. 绿色阶段条：`ExitBootServices` 已完成，`DarwinEnter` 已把控制权交给 XNU
   入口路径。
2. 蓝色阶段条：XNU 入口探针在 MMU 关闭状态运行。
3. 品红色阶段条：执行已到达 MMU 开启前的探针位置。

底部文字为：

```text
Exiting boot services. A green bar across the top means the kernel entry code reached.
SecurityLock::LockCapsuleInterface / SOFTWARE_LOCK
```

`SecurityLock::LockCapsuleInterface` 是 EDK2 `CapsuleRuntimeDxe` 在
`ExitBootServices` 事件中的胶囊接口锁定回调，属于正常回调信息；它不是
TzDxe 断言，也不是 XNU panic。

## 结论边界

本次设备证据确认了以下链路：

```text
UEFI → UniXNU → DarwinBoot → ExitBootServices → XNU early entry
```

尚未确认 MMU 开启后的持续执行、`ArmCpu`/`ArmDTPlatform` 平台初始化、UFS/root
访问、用户态或 iPadOS 启动。source qualification kernel 没有启用这两个板级
驱动，因此本结果不代表完整 J716F 平台适配。

## 下一步

V17 在 MMU 开启之后增加可保持的诊断停点，记录停点是否仍能稳定刷新并保留
屏幕；真机验证继续只使用 `boot_b`。在此之前不处理 TzDxe，也不写入其它分区。
