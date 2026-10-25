# UniXNU-j716f

Lenovo Xiaoxin Pad Pro 2021 (`TB-J716F`, board name `j716f`, Qualcomm
SM8250/Kona) 的 UniXNU 设备仓库。当前仍是 research / bring-up port，不能宣称
已经启动 iPadOS、iOS 或完整真实 XNU。

## 迁移状态

仓库已按 UniXNU 格式 2 组织：

- `board.toml` 固定 `ipados-17.7.11-21H461` 候选、`j716f_source_defconfig`、DTS
  入口、payload 和 Android boot image 参数。
- `dts/j716f.dts` 与 `socs/sm8250/` 保存从 ZUI DTB0/DTBO8 和 recovery FDT
  交叉核对的 CPU0、GIC、计时器和主连续 DRAM 描述。
- `porting.toml`、`AGENTS.md` 和记录工作流覆盖配置、固件、驱动适配、组装与诊断。
- `socs/sm8250` 是临时的 board-local evidence snapshot。官方尚无已审查的
  `UniXNU-sm8250-common` 公共仓库；在拆出共享仓库并固定提交前，不把它当作完成的
  SoC 支持。

## 当前验证范围

- `boot_b` 临时启动实验 Android boot image 已进入 UniXNU 菜单。
- V14 真机完成 256 MiB DarwinBoot 窗口分配、缓存维护、读回和释放，7/7 PASS。
- V15 已完成主机侧真实 kernel/ADT/payload 打包与静态审计，尚未上机验证。
- GOP 菜单、音量键和电源键已有设备观察；物理 UFS、真实 XNU 入口、
  `ExitBootServices`、trust cache、userdata 和用户态均未验证。

## 构建与检查

从最新 UniXNU checkout 执行：

```sh
./unixnu defconfig j716f_source_defconfig -D ../UniXNU-j716f
./unixnu fetch -D ../UniXNU-j716f
./unixnu status --json -D ../UniXNU-j716f
./unixnu tree -D ../UniXNU-j716f
python3 scripts/porting-record.py check --device ../UniXNU-j716f
```

DTS 独立检查：

```sh
dtc -I dts -O dtb -i ../UniXNU-j716f/socs/sm8250/dts \
  -o /tmp/j716f.dtb ../UniXNU-j716f/dts/j716f.dts
```

私有路径放在 `local.toml`（由 `local.toml.example` 复制），不要提交厂商二进制、IPSW、
原始 DTB、kernelcache、trust cache、Data seed、payload、照片、日志或密钥。

## 真机边界

V15 测试只允许 `boot_b`；不要写 `boot_a`、vbmeta 或 userdata。TzDxe 仍不在本轮范围内。
