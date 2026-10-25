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

## 真机约束

测试前核对 boot image SHA-256，仍只写 `boot_b`。不写 `boot_a`、vbmeta 或
userdata；TzDxe 继续保持不处理。若 6 秒复位发生，先回到 fastboot 并读取
槽位状态，再决定是否继续下一次诊断。
