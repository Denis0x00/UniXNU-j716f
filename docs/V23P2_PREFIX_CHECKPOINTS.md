# V23P2：定位首行后的停点

日期：2026-10-25。V23P2 使用独立payload源码，保留 V23P/C/V/M。
菜单 `V23P2: PSCI prefix checkpoints 1-12`，探针首行模式值5。
仍为单核、MMU前终止诊断；不继续正常XNU启动。

## 右上方块

从左至右12格，每格96x96，起点(1280,32)，间距104像素。
标记是内联整数写循环，不调用字体函数、不读系统寄存器、不使用counter或额外屏障。

| 格 | 执行位置 |
| --- | --- |
| 1 | 调用首行绘字之前 |
| 2 | 首行像素存储之后、原DSB之前 |
| 3 | 原DSB之后、RET之前 |
| 4 | 首行函数返回、CurrentEL读取之前 |
| 5 | CurrentEL读取之后 |
| 6 | EL行返回、SCTLR读取之前 |
| 7 | SCTLR读取之后 |
| 8 | SCTLR行返回 |
| 9 | FUNC行返回 |
| 10 | A0行返回；随后设置SMC参数、DSB、SMC |
| 11 | SMC返回；返回值已保存在x21 |
| 12 | 异常入口，位于异常寄存器读取之前 |

第12格需要此前验证的framebuffer状态仍有效。PRIOR在首次A0设置前没有已定义的阶段值。
方块可见证明对应存储路径执行，缺失不单独证明指令未执行，也不保证跨重启留存。

## 主机与真机结果

主机：探针1824/2800字节，重新seal后的HMAC匹配；实际FD/FAT/PE/Android-v2审计通过。
5项模拟覆盖完整SMC返回、停在首行DSB、停在RET、停在CurrentEL、CurrentEL异常。
真实绘字函数与RAW写入执行，共检查552960个方块像素值；系统寄存器、SMC和异常受控替代。

真机仅刷boot_b。bootloader接口写入返回unknown command，切回fastbootd后成功刷写并激活B。
失败及成功操作分别留存，本次不改变其他镜像；TzDxe、boot_a、vbmeta和userdata不在范围内。

录像确认只亮第1、2格。1.200秒出现，18.578秒消失，随后Lenovo Logo再现并返回菜单。
PMIC从0x02000001变为0x02000041，复位源未知。真实内核中第2/3格夹着
`0xfffffff0073fca54: dsb sy`，应优先排查这一边界；尚无CurrentEL/SCTLR、PSCI或counter结果。
入口仍有 `0x40000040+CPU*4` 的BCM遗留写入，是候选相关访问，尚未证明因果。
后续须单因素对照屏障行为和遗留写入，不能直接将屏障删除视为平台修复。

源码：`scripts/v23p2-probe.S`、`scripts/build-v23p2-payload.py`（复用V23固定baseline与封装守卫）。
配套固件仍为原V23 DSC/FDF，payload单独替换。公开摘要：`research/v23p2-prefix-checkpoints.json`。
原始视频、镜像、内核/ADT和操作日志仅本地保存；当前源码未提交或推送。
