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

截至 2026-10-25：

- 诊断镜像通过 `boot_b` 进入 UniXNU 菜单；GOP、音量键和电源键已有真机观察。
- V14 验证一个 256 MiB DarwinBoot 窗口，7/7 检查通过；两轮读写累计验证 512 MiB。
- V16 观察到 ExitBootServices 与 XNU 早期入口探针；V17 记录 post-MMU 复位检查点。
- V18 改为 WFE 停点后仍复位，具体来源尚未确定。
- V19/V20 增加复位诊断；V21 已构建、校验、刷入 `boot_b` 并进入放大菜单，
  照片确认 `PMIC RESET: 0x02000001`。这是当前启动的历史基线，尚不能据此归因复位。
- V21 的 XNU 启动尝试仍待观察；持续 MMU-on 执行、原生平台驱动、物理 UFS/root、
  trust cache、userdata 和用户态均未验证，未启动 iPadOS。

最新汇总：[V21 复位诊断与真机基线](docs/V21_RESET_DIAGNOSTICS.md)，
[公开验证元数据](research/v21-reset-diagnostics.json)，
[配套固件仓库](https://github.com/Denis0x00/uefi-j716f)。

`board.toml` 的 payload 选项仍保留 V17 定时复位方案；它不是已刷入 V21 的完整
配方。V21 使用独立 UEFI 诊断配置，内嵌负载与 V20 相同，具体来源与边界见最新汇总。

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

当前诊断测试只允许 `boot_b`；不要写 `boot_a`、vbmeta、userdata 或其他分区。
TzDxe 仍不在本轮范围内。
