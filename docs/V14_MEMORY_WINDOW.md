# V14 内存窗口验证

2026-10-24 真机完成 7/7 PASS：EL1、内存图候选、256 MiB AllocatePages、
地址/对齐、第一轮全窗读回、第二轮反码读回、FreePages。

- 候选区域：`[0xD0000000,0xE8000000)`（384 MiB）。
- 实际分配：`[0xD007C000,0xE007C000)`（256 MiB）。
- 每轮完整写入、缓存清理/失效后逐 UINT64 读回；累计 verified=512 MiB。
- 返回诊断页：Status=Success、Code=0、EFI file read=25600 bytes、
  Probe count=7/PASS=7/FAIL=0、Complete=1、verified=512 MiB。
- 原 V13 内存表保留；V14 使用独立 MemoryMapLib、外部 EFI 及匹配的 Version=2 报告。

Ubuntu 两阶段构建与主机审计通过：32个源码PE、无厂商PE、APRIORI、
压缩FD/FV身份、嵌入FAT和ARM64 EFI身份、固定ZUI DTB、Android header-v2。

数值和SHA256见 [research/v14-memory-window.json](../research/v14-memory-window.json)。
固件源码在配套 uefi-j716f 的 `Platforms/Lenovo/j716fPkg/j716f-window-v14.dsc`。
照片、固件、FAT、原始DTB与Apple负载仅留本地。

## 后续

准备J716F专用最小ADT及独立真实内核入口诊断。当前验证只覆盖一个256 MiB窗口，
没有真实XNU入口、ExitBootServices、UFS根卷或用户态成功证据。
