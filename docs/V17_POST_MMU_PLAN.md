# J716F V17：post-MMU 诊断计划

V16 已在 `boot_b` 观察到 `ExitBootServices`、MMU 关闭时的 XNU 入口以及
MMU 开启前的探针。V17 保留这三个阶段，并启用官方入口探针的定时复位：
MMU 开启后等待 6 秒，通过 PSCI `SYSTEM_RESET` 复位。

## 判读

- 6 秒后重新出现 Lenovo Logo：确认 CPU 执行通过 MMU 开启点并到达复位探针。
- 停在 V16 的品红色阶段条：MMU 开启点之前或紧邻处未继续执行。
- 出现 panic screen：保留原始画面，记录 panic 文本和寄存器；不把它当作
  iPadOS 启动成功。

V17 的配置位于 `board.toml` 的 `[payload]`：`panic_screen = true`、
`entry_probe_reset_delay = 6`。该配置只影响后续 payload 打包；本次源代码
资格构建仍不启用 `ArmCpu`、`ArmDTPlatform`，也不提供 UFS、root 或用户态。

主机侧候选已生成并完成字节级校验（尚未上机）：

- boot image：`D:\TB-J716F\Firmware\j716f-mu-zui13-entry-v17-boot.img`
- boot image SHA-256：`109cdf94b87ded5687d1a91891a33e1b328517af9f4338407e20785b4db175f9`
- FAT payload SHA-256：`14194adb40956a1c8283bd6b0561b4601af378310152c30dc1ec4d990bbb5785`
- kernel SHA-256：`28f56efd3b7266ef6b51e3998f2fb9fb03692a8c37a0cad44b727f05470068cc`

校验确认 V16 的绿色、蓝色和品红色入口探针仍在，新增的 stage 3 代码位于
MMU 开启后的保留空洞，并等待 6 秒后调用 PSCI 复位。

## V17 真机结果

2026-10-25，设备 `HA1Q9SKY` 只刷写 `boot_b`。等待一段时间后设备自动复位，
随后重新进入 UniXNU 菜单并再次显示绿色、蓝色和品红色阶段条。这个结果证明：

1. V16 已确认的 `ExitBootServices`、MMU-off 入口和 pre-MMU 探针仍然执行；
2. V17 的 stage 3 等待循环结束并执行 PSCI `SYSTEM_RESET`；
3. 复位后的 ABL/UEFI 链路仍能重新启动 UniXNU。

这次测试没有证明 MMU 开启后能继续进入 `ArmCpu`、`ArmDTPlatform`、UFS、root
或用户态，因为诊断探针在 MMU 开启后主动复位。操作明细保存在本机
`D:\TB-J716F\Firmware\v17-flash-operation.json`，不上传仓库。

## 真机约束

测试前核对 boot image SHA-256，仍只写 `boot_b`。不写 `boot_a`、vbmeta 或
userdata；TzDxe 继续保持不处理。若 6 秒复位发生，先回到 fastboot 并读取
槽位状态，再决定是否继续下一次诊断。
