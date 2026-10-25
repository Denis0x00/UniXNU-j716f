# Research notes

公开仓库只保留可复核的板级结论，不放设备专属备份、照片、序列号、ADB 输出、Ubuntu 日志、完整 DTB、厂商二进制或 IPSW。

截至 2026-10-25，已确认的公开结论：

- 板级代号：`j716f`；SoC：SM8250；设备：Lenovo Xiaoxin Pad Pro 2021 / TB-J716F。
- UEFI GOP 已观察到 2560×1600、10240 字节行跨度和 32 位像素；原生显示驱动、
  刷新率控制与背光仍未完成。
- V14 的一个 256 MiB 内存窗口通过 7 项检查，两轮读写累计验证 512 MiB；不代表全部 RAM。
- V16/V17 记录早期 XNU 入口检查点；V18 WFE 后复位的来源仍未知。
- V21 录像确认入口三色条及 Lenovo Logo 再现；PMIC 变为 `0x02000041`，原因未分类。
- V22A／V22B 的 1 秒／3 秒检查点镜像通过主机审计，真机验证待完成。
- 物理 UFS/root、原生平台驱动和用户态仍未验证。
- TzDxe 暂不处理。

最新纯元数据：[v22-checkpoint-preparation.json](v22-checkpoint-preparation.json)。
测试方法：[V22 检查点](../docs/V22_POST_MMU_CHECKPOINT.md)。
此前基线：[v21-reset-diagnostics.json](v21-reset-diagnostics.json)。
版本链、源码引用、主机审计与真机观察分别记录在
[V21 诊断汇总](../docs/V21_RESET_DIAGNOSTICS.md)。

完整原始测量资料保存在本地工作区，不属于公开发布内容。
