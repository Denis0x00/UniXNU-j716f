# V23P17M：chosen、stall 与 CPU 拓扑分组诊断

## 基线与目的

P16 已实机验证原始 WFE/misc 返回与立即重启后的记录保留。
P17 延续已验证的 BCM 整段排除和日志映射，让原始初始化继续经过一组相关路径。
它仍是冻结内核的终点诊断，与原生定时器源码集成候选分别构建。

## 六个独立检查点

| phase | 含义 |
| --- | --- |
| 16 | BCM 路由整段已排除 |
| 17 | MMU M1 直接读回 |
| 18 | 原始 WFE/misc 已返回，继续原来的栈变量初始化 |
| 19 | /chosen 的 SecureDTLookupEntry 已返回，保留原始结果检查及失败分支 |
| 20 | xnu_platform_stall 的 SecureDTGetProperty 已返回，保留原始属性处理分支 |
| 21 | 原始 platform_stall_panic_or_spin、ml_parse_cpu_topology 和 ml_get_boot_cpu_number 均已返回 |

phase12 终点记录实际 SCTLR_EL1，要求 M=1；之后请求 SYSTEM_RESET。
终点位于原始 arm_init `0xfffffff0079e1a98`，尚未发布 boot CPU 编号，
也未执行后续 max-CPU 检查、驱动匹配和调度。
查询“返回”检查点不等于查询成功；失败、stall 和 panic 均按原始控制流处理。

所有原始 BL/LR 保留。三个继续执行的 hook 恢复31个通用寄存器、SP、NZCV，
并执行被替换的原始 STR/CMP，再回到原始分支；终点主动结束。
ARM cookie=P17KERNL，槽值=P17S | ARM CRC；旧 P16 记录不会成为本版终点。

## 主机结果

- 45项实际内核机器码检查通过：原始查询/stall/拓扑路径、hook ABI、错误分支、
  中断的初始化、日志余量/CRC/绑定、映射失败、六个独立记录和 consumed ARM。
- 31项实际链接读取器检查通过：只读冻结、Query/ARM、终点绑定、满日志处理、
  会话取消/失败和内核往返。
- 关机/重启 app 与 P11 已验证版本逐字节一致，复用22项检查。
- FD/FAT/EFI、源文件哈希、内核允许修改范围/HMAC、非负载固件/SEC、
  Android v2 镜像及分区容量校验通过。
- 系统寄存器、MMU 别名、屏障、UEFI 与 SMC 为主机模拟，不能证明实机结果。

## 实机步骤与当前状态

1. fastboot 与录像就绪后，仅刷 boot_b、核对 B 槽可启动并进入无倒计时菜单。
2. 首先打开 P17 reader，保存完整初始页；旧记录允许存在，不要直接清日志。
3. 在本次 reader 中 Query → ARM 各一次，保存 ID/CRC/六模板结果。
4. 返回菜单，只启动 P17M 一次，连续拍摄启动及可能的重启。
5. 返回菜单后首先打开 reader，先拍完整页面再进行任何写入。

不要直接再次启动已消耗 ARM 的内核。若停住，先保留录像和画面。
本版是否有实机结果以 `research/v23p17-chosen-topology.json` 为准。

## 首次实机页面

已仅刷 `boot_b`，B 槽激活/可启动核对与 reboot 命令均成功。
首张照片确认 P17 r1 reader、EL1、ENTRY/NOW VALID，next=0x93b7、
size=0x3fff4；逻辑/原始 P17 计数和全部检查点命中均为0，
THIS RUN ID1、query0、armed0。这是有效的满环形日志，不是空日志。
父菜单没有出现在照片中；已观察的是 P17 reader 身份。

照片已保存后，先选 **New linear session** 连续确认两次（不要在中间切换选项），
拍下成功页面再 Query/ARM。该动作重置逻辑历史，旧字节仍可能作为 raw/stale 存在；
冻结 ENTRY 不变。尚无新会话成功、P17 内核启动、终点或重启保留结果。

后续照片确认新会话成功：NOW VALID next=size=0x40，logical/raw=1/1，
ID1/query0/armed0，INIT CRC=DF3BCB06；从旧next/size与新prior0/0重算完全匹配。
本阶段仅验证满环形日志的显式会话重建和 INIT 写入，尚未启动 P17 内核。
