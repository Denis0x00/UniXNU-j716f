# Research notes

公开仓库只保留可复核的板级结论，不放设备专属备份、照片、序列号、ADB 输出、Ubuntu 日志、完整 DTB、厂商二进制或 IPSW。

截至 2026-10-24，已确认的公开结论：

- 板级代号：`j716f`；SoC：SM8250；设备：Lenovo Xiaoxin Pad Pro 2021 / TB-J716F。
- 面板观察值：2560×1600，60/90 Hz；这些值仍需在 UEFI 显示驱动中继续确认。
- UEFI 内存图、DRAM overlay、XNU owned-memory window 和 UFS 访问尚未完成真机验证。
- TzDxe 暂不处理。

完整原始测量资料保存在本地工作区，不属于公开发布内容。
