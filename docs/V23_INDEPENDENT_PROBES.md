# V23：分别测试 PSCI、counter 与 MMU 开关

日期：2026-10-25。四个变体均完成构建与最终 boot 镜像主机审计。V23P 已完成首次真机尝试，功能结论仍待定位。

## 首次 V23P 真机观察（2026-10-25）

仅刷 boot_b，激活 B 槽后启动。录像确认 V23P 菜单与诊断首行
`V23 0000000000000001`；未见 EL/SCTLR、A0/A1、RETURN 或 E0。
首行约1.50秒出现，18.597秒开始消失，之后 Lenovo Logo 再现并返回 V23P 菜单。
尝试前后 PMIC 均为 `0x02000041`。PSCI 是否执行、复位来源仍未知。
下一步先定位首行后的屏障/返回、寄存器读取和后续渲染；C/V/M 均未真机测试。



## 为什么拆分

V22A/1s 的品红至黑屏为 20.245s，V22B/3s 为 19.730s，各一轮，没有设计中的 +2s 响应。
B 最终进入 A 槽 ZUI。MMU 后检查点执行与复位来源都仍未确认；不能直接归因 watchdog。

V23 保持原始 prelink baseline、ADT、DarwinBoot、UEFI 内存图、模块选择与单核 bootargs。
所有探针均在原 MMU 开启位置接管，最终停住或请求复位，不继续正常 XNU/platform/root 启动。
BCM2837 的其他残留路径仍存在，留待后续平台源码构建；本轮用独立终止诊断缩小故障范围。

## 四个镜像与测试顺序

| 顺序 | 变体 | 内容 | 明确排除的依赖 |
| --- | --- | --- | --- |
| 1 | V23P | MMU 前调用 PSCI SYSTEM_RESET `0x84000009`，SMC 返回则显示返回值 | 不读取 counter，不开启 MMU |
| 2 | V23C | 读取 CNTFRQ，固定 1,048,576 次循环采样 CNTPCT | 不调用 PSCI，不开启 MMU |
| 3 | V23V | 同样采样 CNTVCT，供物理/虚拟计数器对照 | 不调用 PSCI，不开启 MMU |
| 4 | V23M | 显式映射 framebuffer，执行 SCTLR 写入和 ISB 后直接显示阶段与寄存器 | 不读取 counter，不调用 PSCI |

菜单标题显示变体名。进入探针后，第一行 `V23` 后的十六进制值 1/2/3/4 分别对应 P/C/V/M。
文字为自带 5x7 字体放大八倍，数值统一显示为 16 位十六进制。

### V23P：PSCI

- `STAGE A0`：即将调用 SYSTEM_RESET；这一画面可能非常短，应连续录像。
- 若很快出现黑屏/Lenovo Logo，记录从选择菜单到复位的完整时序，并与 C/V 的停点作对照。
- `STAGE A1`：SMC 已返回，`RETURN` 显示返回值。此结果表明该调用没有按期望复位。
- `STAGE E0`：进入可显示的异常路径，拍下寄存器。

仅看到后来某次重启，仍不足以独立认定原始 V22 的复位来源。P 不使用 counter，消除了坏计数器等待造成的混淆。

### V23C / V23V：counter

- `STAGE B0`：CNTFRQ 读取之前。
- `STAGE B1`：所选 counter 的首次读取之前。
- `STAGE B2`：有限次采样已完成，进入 WFE 停点。
- `FREQ`：CNTFRQ 原始值；板级期望是 19,200,000，即 `000000000124F800`，仍需读数核对。
- `FIRST / LAST / DELTA`：首末值与无符号差值。
- `BACK`：任意相邻样本倒退则为 1；`MOVES`：首末值不同则为 1。
- 即使 counter 恒定，循环仍会结束，输出 `DELTA=0 / MOVES=0`。counter 访问本身 trap 时尝试显示异常，不会伪造采样成功。

`B2 + DELTA>0 + BACK=0 + MOVES=1` 支持该计数器在此交接状态下可读且推进；没有验证精确时钟速率、timer 中断/PPI、睡眠恢复或平台驱动。

### V23M：直接 MMU 证据

- `STAGE D0`：进入 MMU 前诊断，打印 TTBR0/TCR/MAIR。
- `STAGE D1`：framebuffer 映射已建立，尚未写入 SCTLR。
- `STAGE D2`：执行 `msr sctlr_el1` 与紧随其后的 `isb` 后，直接向 framebuffer 写出的记录。
- `STAGE D3`：MMU 后 SCTLR/TTBR0 采集完成。`AFTER` 为实际 SCTLR，应核对 bit 0。
- `STAGE DF`：映射前提不成立，拒绝开启 MMU，保留前面的寄存器供拍照。

若真机出现 D2/D3 且 SCTLR.M=1，即可证明本版已经执行 MMU 后的指令、映射访问和显示路径；它不证明正常进入 `arm_init` 或其他平台初始化。

本版采用实时屏幕记录，没有新增持久 RAM 日志区，也没有跨重启保持承诺。需要连续录像，尤其是约 20 秒后仍可能发生的外部复位。

## 映射边界

- display 物理保留区来自现有 J716F 内存图：`0x9c000000..0x9e400000`。
- boot_args 必须报告 2560x1600、32bpp、rowBytes=10240，framebuffer 全部落在该区且对齐，否则停止。
- 仅 M 在当前内核拥有的 64KiB bootstrap tables 内使用最后一个空闲 4KiB 页作为 L2。
- 验证精确 TCR=`0x22a51a251a`（4KiB、L1 root、T0SZ=26）、MAIR index1=`0x44`、TTBR0 与 x25 一致。
- TTBR0 L1 slot2 必须为空；spare 页必须全零，两个 L1 roots 均不能引用它。
- 映射完整 display 区为 2MiB block、Normal non-cacheable、AF、outer-shareable、PXN/UXN。保持既有 kernel 映射。
- 不使用 UMI 的保留 RAM 地址，不写未知 MMIO，不改变 firmware 内存所有权。

这是针对固定 baseline 的诊断映射，不是可复用的完整显示驱动，也不能直接带入后续 `arm_vm_init`。

## 异常记录

当前固定内核的 early bootstrap vector16个入口各替换8字节，跳转到终止诊断处理器。
显示 `STAGE E0`、`PRIOR`（最近阶段）、`VECTOR`、`ESR`、`FAR`、`ELR`、`SPSR`，然后停止。
FAR 是否有效需要结合 ESR 判断。若向量/代码/显示映射本身已不可访问，异常画面也可能无法输出。
本处理器不返回内核，不改变 TzDxe，不处理正常多核或用户态异常。

## 构建和审计

源码：`scripts/v23-probe.S`、`scripts/build-v23-payload.py`。
配套固件：`Platforms/Lenovo/j716fPkg/j716f-entry-v23.dsc`，独立 V23 UniXNULib/FDF/include。
固定 baseline SHA256：`cab8f385a44d21fc6f455cb501227452212ba44d34a9fa03d04fcf7e85324214`。
固定 ADT SHA256：`b68fc49b2d9bb6b0a387e59a7816fd4848dffc3bde706a439cbbff3cbebb1aa7`。

```sh
python3 scripts/build-v23-payload.py \
  --baseline /private/baseline.macho --adt /private/kernel.macho.adt \
  --sealer /pinned/UniXNU/scripts/seal-source-crypto-kernel.py \
  --variant p --output /new/private/v23p
```

构建器拒绝非固定 baseline、非空 cave、超界代码、非预期入口和 vector 指令。
所有指令修改之后重新 seal；独立计算 HMAC。仅允许 entry、MMU hook、零填充 cave、vector 入口和 HMAC 字段变化。
最终包装审计从实际 FD/FAT 重新取出内核，再核对字节、补丁和 HMAC，检查 Android v2 header、gzip/BootShim、DTB、大小及 padding。

本地主机模拟完成16种逻辑场景，覆盖停滞/倒退/异常、PSCI 返回值和映射拒绝条件；实际 renderer 逐像素检查118,272个值及 x17..x29 保持。
模拟中的系统寄存器、counter、PSCI 与 SCTLR 写入均受控替代，不具备真机 MMU 翻译或固件行为证明力。

公开摘要见 `research/v23-independent-probes.json`；原始日志、镜像、kernel/ADT、VM 路径及凭据不进入公开仓库。

## 真机步骤

先准备连续录像并进入 fastboot。任何槽位变更前读取 current-slot、slot-successful:b、slot-unbootable:b、slot-retry-count:b。
只刷选定变体到 `boot_b`；必要时激活 b，再重启。先 P，再 C/V，最后 M，每轮核对菜单版本。
复位前拍下结果屏幕；若停点后又自行重启，录像录到最终菜单或 Android，不把重启直接记为 PSCI 成功。
不得写 boot_a、vbmeta 或 userdata。P 首次观察见上；C/V/M 尚未测试，先定位公共前置路径。
