# NATIVE FAST-V1..V4：原生候选 40458123294 实机 `arm_vm_init` / `TTBR1_EL1` 切换完成与后续可观测性（2026-10-29）

## 1. 背景与效率重构（`package/native-fast/`）

在 `MAINT-v2` 完整光学导出验证（265 秒 256 帧 QR 恢复 256 KiB `0xb00be000` 区域，全区零 `JL_MAGIC` 会话且冷启动头为未初始化 DRAM 字节）之后，为消除每次试验 4.5 分钟录像与冷启动 `INVALID_HEADER` 死锁，我们在不改变内核 `JLHeader + JLRecord[63]`（8,192 字节，候选 SHA256 `1c19ee51d5388dcfbbaefd41dcf361a54ccee3d08d889f6f0e4bb6f69a08d892`）ABI 的前提下，新增了 `package/native-fast/` 快速闭环工具链：

1. **物理全区扫描（`jlf_scan_raw`）**：直接在 `0xb00be000 + 0x40000` 全区按 8 字节对齐扫描合法 `JL_MAGIC` 会话并校验头部 CRC32 与候选指纹。即使重启进入 `fastbootd` 时 Linux `ramoops` 改写了外层 12 字节 `DBGC` 头，已落盘的 8 KiB XNU 会话仍可 100% 识别与恢复。
2. **3 秒冷启动/脏头安全初始化与 ARM（`FastArm.c`）**：
   - 若线性 `DBGC` 头有效且尾部剩余 $\ge 8192$ 字节：直接 `APPEND` 新会话，保留历史会话；
   - 若全区 `JL_MAGIC` 会话数为 `0`（冷启动随机字节或纯 Android 日志）：直接 `CLEAN_INIT` 初始化 12 字节 `DBGC` 头并 `ARM` 8 KiB 会话；
   - 若已存在历史 `USED` 会话但外层 `DBGC` 满环：默认拒绝覆盖，仅在先完成 `Read` 导出后允许显式选 `[2] Query with Rollover`。
3. **4 秒 8 帧 QR 会话导出（`FastRead.c` + `scripts/decode_fast_session.py`）**：仅导出当前 8,192 字节 `JLHeader + JLRecord[63]` 会话（8 帧 $\times 1024$ 字节，`J716FS1/...` 协议，含整会话 SHA-256、会话 CRC32 与单帧 CRC32），手机录制 5 秒即可离线恢复完整 8 KiB `session.bin` 与寄存器级 JSON 报告。
4. **菜单内确认式热重启与关机（`FastPower.c`）**：直接在 UEFI 主菜单提供双次 Power 确认的 `PSCI_SYSTEM_RESET (0x84000009)` 与 `PSCI_SYSTEM_OFF (0x84000008)`。

---

## 2. 三轮实机闭环与两个 XNU ARM64 VM 映射缺陷修复

脱敏实机证据汇总见 [`research/native-fast-vm-done-20261029.json`](../research/native-fast-vm-done-20261029.json)。三轮测试均仅写入 `boot_b`，`boot_a`、`vbmeta`、`userdata` 与 `TzDxe` 未触碰。

### 2.1 Run 1（`FAST-V1`）：定位并修复 `T1SZ=26` L1 根表索引掩码缺陷

- **镜像**：`j716f-native-log-40458123294-fast-v1-boot.img`（SHA256 `eca22352580ea417b3e7b8a4fe66ebdb7292ba7abbbf6a7727aa6cd1fae09b73`）
- **实机恢复会话**：`0x000000010d60aff7`（`Valid=2/2`），进度：`0:start_begin -> 43:BOOT_MAP_ERR`
- **根因**：iPadOS 17.7.11（A12X/A12Z `T8027`）ARM64 内核配置 `TCR_EL1.T1SZ = 26`（38-bit VA 空间：`0xfffffff000000000 .. 0xffffffffffffffff`），因此 L1 根页表（`bootstrap_pagetables` @ `0xd2f7e000` 及永久 `cpu_tte` @ `0xd4229000`）仅有 **256 项**（2 KiB，紧邻的 `0xd2f7f000` 已是 L2 页表）。原 `package/native-log/NativeMap.h` 使用 39-bit VA 掩码 `(at >> 30) & 511` 计算出索引 `448 (0x1c0)`，越界读取了 `0xd2f7f000` 处的 L2 块描述符 `0x00000000d0000601`（`bit 1 == 0`），触发 `43:BOOT_MAP_ERR`。
- **修复**：在 [`package/native-fast/NativeMapFixed.h`](../package/native-fast/NativeMapFixed.h) 与 `scripts/build_fast_v2.py` 中将 `_j716f_native_bootstrap`（`0xfffffff0073f972c`）与 `j716f_native_vm_map`（`0xfffffff0073fadd4`）的 `ubfx xN, xM, #30, #9` 修正为 `ubfx xN, xM, #30, #8`（`(at >> 30) & 255 = 192 (0xc0)`）。

### 2.2 Run 2（`FAST-V2`）：定位并修复 `TTBR1_EL1` 切换前 `_phystokv` 虚实转换缺陷

- **镜像**：`j716f-native-log-40458123294-fast-v2-boot.img`（SHA256 `28463fde65f6459009849b41fb29dac205235ea13dc1de9b749dafc06680084f`）
- **实机恢复会话**：`0x000000001c2b070d`（`Valid=6/6`），进度：`0:start_begin -> 1:boot_pt_map -> 2:arm_init -> 3:vm_entry -> 44:PERM_MAP_ERR -> 41:EL1_SYNC_FLT`
- **根因**：`1:boot_pt_map`、`2:arm_init`（MMU 开启，`SCTLR_EL1=0x3454593d`）及 `3:vm_entry` 全部通过。但在 `arm_vm_init` 中，`j716f_native_vm_map` 位于 `arm_vm_physmap_init(args)` 之后、`set_mmu_ttb_alternate(cpu_ttep)` 切换 `TTBR1_EL1` 之前。此时 `_ptov_table` 已被填入永久页表下的 KVA（例如 `cpu_ttep = 0xd4229000` 被映射为 `0xfffffff018629000`），而硬件 `TTBR1_EL1` 仍指向 `bootstrap_pagetables (0xd2f7e000)`（仅线性映射 `0xfffffff00427c000 + (pa - 0xd007c000)`）。`j716f_native_vm_map` 调用 `_phystokv(l2_pa)` 解析永久页表各级子表 PA 时拿到尚未激活的 `0xfffffff01862a000` 等地址，导致 permanent Window 重绑失败并在 `set_mmu_ttb_alternate` 切换瞬间因永久表缺少日志窗口映射而失败。
- **修复**：在 [`package/native-fast/NativeVmMapFixed.h`](../package/native-fast/NativeVmMapFixed.h) 与 `scripts/build_fast_v3.py` 中，将 `j716f_native_vm_map` 内部三次 `bl _phystokv`（`0xfffffff0073fadec`、`0xfffffff0073fae40`、`0xfffffff0073faebc`）重定向至 `_phystokv+0x168`（`0xfffffff0073f8548`）线性回退尾块 `pa - gPhysBase + gVirtBase`。

### 2.3 Run 3（`FAST-V3`）：实机 100% 通过 `arm_vm_init`、永久页表切换与 `pmap_bootstrap`！

- **镜像**：`j716f-native-log-40458123294-fast-v3-boot.img`（SHA256 `d367d4ee3fa10c04b84a2837b1d33d301d9b2d39763cd806eaf0abf0c3cf55e8`，内核 SHA256 `a0b548d0f89c264e3d9c5ba81069a8792434e7791e59d5057ed725ed24bd7242`）
- **实机恢复会话**：`0x000000004a9650ec`（偏移 `0x28b08`，SHA256 `020ca14f001e36b78acb0a35a8220676271c46ffbce739678d50a37995b28fa1`，CRC32 `0x6613d3dc`，`Valid=8/8`，**`Faults: none`**）：

| 槽位 | 阶段标签 | 实机记录关键寄存器 / 参数 | 验证结论 |
| :---: | :--- | :--- | :--- |
| `0` | `start_begin` | `boot_args=0xd4224000`, `SCTLR_EL1=0x00c00808`, `SP=0xd3a3ff40`, `CNTFRQ=19.2MHz` | `_start` 物理入口正常 |
| `1` | `boot_pt_map` | `root_pa=0xd2f7e000`, `window_va=0xfffffff007aac000`, `attr=0x006000000000060f` | `T1SZ=26` L1 `root[192]` L3 拆表映射成功 |
| `2` | `arm_init` | `boot_args_kva=0xfffffff008424000`, `SCTLR_EL1=0x3454593d`, `TTBR1_EL1=0xd2f7e000` | MMU 开启并进入 `_arm_init` |
| `3` | `vm_entry` | `gVirtBase=0xfffffff00427c000`, `gPhysBase=0xd007c000`, `mem_size=0x10000000` | 进入 `arm_vm_init` |
| `4` | `vm_perm_map` | `window_va=0xfffffff007aac000`, `cpu_ttep=0xd4229000` | 永久页表 `cpu_tte` 64 页 Device-nGnRnE 映射成功 |
| `5` | `vm_ttbr_sw` | `cpu_tte=0xfffffff018629000`, **`TTBR1_EL1=0xd4229000`**, `TTBR0=0xd4228000` | **硬件 `TTBR1_EL1` 成功切换至永久内核页表且零异常** |
| `6` | `pmap_boot` | `dynamic_memory_begin=0xfffffff029400000`, `TTBR1_EL1=0xd4229000` | **`pmap_bootstrap` 完整返回** |
| `7` | `vm_done` | `first_avail=0xd4320000`, `window_va=0xfffffff007aac000`, `TTBR1_EL1=0xd4229000` | **`arm_vm_init` 100% 完成并返回 `_arm_init`** |

---

## 3. `FAST-V4`：打通 `7:vm_done` 之后的 `_arm_init` 尾声、`_kernel_bootstrap`、`_PE_init_iokit` 与 `_panic` 可观测性

Run 3 在 `7:vm_done` 之后未触发任何异常（`40/41/42` 均为空），是因为：
1. 原 `prepare-native-logger.py` 在 `7:vm_done` 与 `50:sched_pre1`（`machine_switch_context`）之间未设置任何观测点；
2. `_kernel_bootstrap` 末尾通过 `_load_context`（`0xfffffff0072c8870` -> `_machine_load_context`）直接切换到首个线程 `_kernel_bootstrap_thread`（`0xfffffff0079c224c`），**根本不经过 `machine_switch_context`（`50:sched_pre1`）**；
3. `7:vm_done` 之后 `_arm_init` 立即初始化 `_serial_init` 与 `_PE_init_printf`，此后任何 `_panic(...)` 均不会触发同步异常（`41:EL1_SYNC_FLT`），而是走 `_ml_panic_trap_to_debugger` / `_PE_halt_restart` 静默停机。

为此，`scripts/build_fast_v4.py`（主机审计清单见 [`research/native-fast-v4-build-20261029.json`](../research/native-fast-v4-build-20261029.json)，镜像 SHA256 `8ac335978a17f3369e45735736bec962a0171a86c257ef300f44bbb58409e428`）利用 `__TEXT_EXEC` 末尾空闲区（`0xfffffff0073fd9c0`，使用 988 字节）与 21 个空余槽位，完整注入了以下观测点并重算了 `__fips_hmacs`：

- **`_arm_init` 尾声（Slots `33..38`）**：
  - `33:serial_ok`（`_PE_init_printf` 返回）
  - `34:pe_plat`（`_PE_init_platform` 返回）
  - `35:cpu_tb`（`_cpu_timebase_init` 返回）
  - `36:pe_fiq`（`_fiq_context_init` 返回）
  - `37:early_rng`（`_early_random` 返回）
  - `38:kboot_ent`（`_machine_startup` 中 `_machine_conf` 返回、即将进入 `_kernel_bootstrap`）
- **全部 55 处 `_kernel_debug_string_early` 自动追踪（Slots `39` & `47`）**：
  - `39:kdbg_1st`：记录首次调用的 LR 与前 16 字节 ASCII 阶段名；
  - `47:KDBG_LAST`：在 `jl_write` 中特许槽位 `47` 滚动覆写，始终保存停机前最后一次 `_kernel_debug_string_early` 的调用方 LR 与 16 字节 ASCII 阶段字符串（`decode_fast_session.py` 自动解码为 `kdbg_text`）。
- **`_kernel_bootstrap` 关键里程碑（Slots `57..62` & `49`）**：
  - `57:vm_mem_ok`（`_vm_mem_bootstrap` 返回，标志着 `kernel_memory_allocate`、`kalloc`、`zinit` 全部完成）
  - `58:con_ok`（`_console_init` 返回）
  - `59:sched_ok`（`_sched_init` 返回）
  - `60:mach_ok`（`_machine_init` 返回）
  - `61:clock_ok`（`_clock_init` 返回）
  - `62:thread_ok`（`_thread_init` 返回）
  - `49:LOAD_CTX`（`_kernel_bootstrap` 末尾即将调用 `_load_context` 切入首个内核线程）
- **`_kernel_bootstrap_thread` 与 IOKit 启动（Slots `54..56`）**：
  - `54:kboot_thr`（`_kernel_bootstrap_thread` 入口）
  - `55:iokit_pre`（`_PE_init_iokit` 调用前）
  - `56:iokit_post`（`_PE_init_iokit` 返回后，开始匹配 `ArmDTPlatform` / `ArmCpu`）
- **Panic 与异常捕获（Slots `45..46` & `48`）**：
  - `45:PANIC_CALL` + `46:PANIC_STR`：拦截 `_ml_panic_trap_to_debugger`（`0xfffffff0073ffa84`），直接抓取 `_panic` 调用方 LR（`x19`）、格式化字符串指针（`x23`）、格式化字符串前 8 字节 ASCII 及前 3 个可变参数（`vararg0..2`）；
  - `48:SERR_FIQ`：拦截 `_fleh_fiq` 与 `_fleh_serror`。
