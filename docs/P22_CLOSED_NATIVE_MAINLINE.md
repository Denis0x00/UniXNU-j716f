# P22 已封存；原生平台成为主线

2026-10-27：P22M 已成功完成本轮有界实验，不再等待 Query/ARM，不应重复启动。
以 `research/p22-closed-20261027.json` 为本轮收尾记录；旧文档中逐次追加的
“下一步”均是当时的历史状态，不再是当前操作指令。

本地 `Firmware/V23P22/FROZEN.json` 冻结了 51 个文件，包括镜像、构建材料、
照片、视频及结果。工具 `scripts/freeze-p22-evidence.py` 首次创建清单，之后只验证；
不修改旧 RESULT、会话、ARM、照片、录像或 host-audit。

实机边界：原始 `pmap_bootstrap` 与紧随的 `disable_preemption` 返回；
VM 收尾与原始栈保护检查完成；原始 `arm_vm_init` RET 返回调用方
`0xfffffff0079e1bbc`。六槽 L1/R1，fresh ID1/ARM `B70F9702`，
九条 CRC 按照片字段独立重建一致。终点 SCTLR=`0x3454593d`、M=1，
next=size=`0x380`、logical/raw=14/14；一次观察到的重启后 reader 为 ID2/query0/armed0。

这不是原生平台、IRQ 或调度成功。终点主动请求 SYSTEM_RESET，实际复位源未独立证明。
借用 bootstrap 子表只合格至立即返回终点，不能沿用为原生长期日志映射。
完整 console CRC 仅为屏幕读数，未获取原始 RAM 重算。

原冻结内核不含 ArmDTPlatform/ArmCpu；其代码洞实际为
`0xfffffff0073fc500..0xfffffff0073fcff0`，2776/2800 字节。
历史诊断源码与二进制保留，不默认制作后继逐函数复位版本。

当前验收改为：固定原生 CI 产物 → XnuPrelink → 专用负载/最终镜像逐层核验，
同时建立入口、VM、平台、异常、IRQ 的可恢复日志，再在同一候选上验证
平台匹配、CPU 注册、GIC、timer IRQ/rtclock/EOI 和单核调度。
详见 [原生主线](NATIVE_MAINLINE.md)。
