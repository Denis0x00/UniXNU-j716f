# V23P6 RAMLOG r1：先验证复位后的日志保留

## 当前结论

V23P6 已完成主机编译、环形缓冲区边界测试、固件差异审计和 Android
boot image 校验。已仅刷 boot_b、激活 B 槽并成功发送重启命令；首次日志头和保留结果等待录像。此前 TWRP 采集均为只读，测试应用是否追加标记尚无设备证据。
这版是独立 UEFI 应用，不包含 XNU 内核；不能用它证明 XNU、counter、PSCI
或持续 MMU-on 已工作。

本地镜像：`j716f-mu-zui13-ramlog-v23p6-boot.img`，1,699,840 字节。
SHA-256：`a939905b05f4db01aa88311f7e0a1f420ed0b95e39661a871e03108e903f82e3`。
镜像和原始采集资料仅保存在私有工作区。

## J716F 的实际布局

Recovery：TWRP 3.7.1_12-0，Linux 4.19.157-perf+，2023-06-30 构建。
运行时 `/proc/version` 与本地 TWRP 内核版本字符串一致；没有对运行中内核
做逐字节身份验证。采集器读取 module 参数、DT、dmesg、iomem、内核配置及
已有 pstore 文件。布局审计见 `research/v23p6-ramoops-layout.json`。

| 区域 | 半开物理范围 | 大小 |
| --- | --- | --- |
| ramoops 总区 | `0xb0000000..0xb0100000` | 1 MiB |
| dump records | `0xb0000000..0xb00be000` | 190 × 4 KiB |
| console | `0xb00be000..0xb00fe000` | 256 KiB |
| ftrace | `0xb00fe000..0xb00ff000` | 8 × 512 B |
| pmsg | `0xb00ff000..0xb0100000` | 4 KiB |

使用本地 TWRP 内核的实际 AArch64 初始化代码，在主机模拟执行 probe、
init_przs 和 init_prz；内存分配、persistent_ram_new 和 zap 调用被替换为
主机桩，记录分配请求。它证明这些参数对应的分配范围，**不证明已读取真实
RAM 头或跨复位保留**。console 的 persistent_ram_new 签名参数为 0，
与 DBGC 异或后得到 `0x43474244`；头依次为 32-bit sig、next、size，
数据容量为 `0x3fff4`，ECC 为 0。

此前 iomem 只出现前 512 KiB 的 `persistent_ram`，原因是后半段列在
`System RAM` 的 `reserved` 子区中。实际内核在 pfn_valid 分支采用 vmap，
另一路才 request_mem_region/ioremap。这与
[Linux 4.19 ram_core.c](https://github.com/torvalds/linux/blob/v4.19/fs/pstore/ram_core.c)
的两种映射路径一致。DT boot_log 只占前 512 KiB，second_log_offset 为
`0x40000`，与 1 MiB pstore 有重叠，不能照抄 umi 的 4 MiB/2 MiB 布局。

## 固件变化与写入限制

- 独立 DSC/FDF、输出目录和内存映射库，旧 V23/P5 文件保持原样。
- 新映射仅把 Boot Log 保留范围从 `0x80000` 扩到 DT 已保留的 `0x100000`。
  实际 FD 与 P5B 比较：SEC、DxeCore、StatusCodeHandler、CpuDxe、继承 GOP
  各仅改变该描述符的一个字节。其余非载荷内容及 APRIORI 完全相同。
- embedded FAT 只有菜单配置与 `J716fRamLogTest.efi`，无 XNU。
- 菜单与应用均无倒计时启动；应用默认选中只读刷新。字体为 scale 3。
- 写入前校验 DBGC、next < capacity、size <= capacity、next <= size。
  不初始化、不修复非法头。仅手动追加一条带版本、构建日期和序号的短标记，
  每次应用运行最多追加一次，数据与头提交后均执行 DSB。
- 满环按 Linux console 语义覆盖最旧的标记长度字节，不清空整个日志。
- Warm restart 调用 EFI `EfiResetWarm`；它只是请求类型，不能证明平台实际
  采用何种复位。应用不直接访问 PMIC/SCM，不引入 counter 等待或 SMC。

主机 C 测试覆盖空环、物理回绕、边界哨兵、非法签名/索引/长度拒写、
零长度/超长拒写及整容量写入；在 GCC ASan/UBSan 下通过。
校验完整打包镜像的 FD、FAT、AArch64 EFI application、源码哈希、保留区
实际链接、模块差异和 boot header。主机通过不等于设备通过。

## 实机顺序

1. 保留当前 Recovery 采集作为 before。进入 fastboot 后仅刷 `boot_b`，
   激活 B 槽并启动；全过程录像。
2. 选择 **P6 RAM console**，拍下第一次只读页面：sig/next/size、VALID 或
   INVALID、`Markers on entry`。非法头时不要尝试其他 RAM 地址。
3. VALID 时选 **Append marker**，拍下 `APPEND READBACK OK`。同一次运行
   重复追加会被拒绝。失败则停止推断保留能力。
4. 选 **Warm restart**。重回 P6 菜单后再次打开应用，先不要追加；拍下
   `Markers on entry`。同版本旧标记可见说明该次 UEFI→复位→UEFI 链路保留。
5. 再进入 TWRP，在首次进入时立即采集 console，尽量避免无关操作使日志回绕。
   设备端只读，输出目录必须全新：

   ```sh
   python scripts/collect-j716f-ramoops.py --adb <adb> --serial <private-serial> --output <private-after-dir>
   python scripts/decode-j716f-ramlog.py <private-after-dir>/console-ramoops-0 --manifest <private-v23p6-build-manifest.json> --image <private-v23p6-boot.img>
   ```

6. 将读回标记与录像中的追加、重启及再次进入 Recovery 的顺序关联。
   找到标记才能证明这次完整链路保留。未找到时，可能是某段清除、重启类型
   或 Recovery 日志覆盖，不能单凭缺失定位清除阶段。

完成这一步后，再为 XNU MMU 前后加入无栈直接记录，并审计保存的寄存器、
MMU-off 物理访问和 MMU-on 映射。P5B 的 SMC→原地 B . 对照仍是独立实验，
本版没有实施。完整自动循环测试继续等待可重复的 reader 与返回路径验证。

禁止写 boot_a、vbmeta、userdata、GPT、TzDxe；不使用 9008。

## 刷入记录

V23P6 RAMLOG r1 was flashed only to boot_b through fastbootd with the audited image; B activation, current-slot b and slot-unbootable:b=no were verified, and reboot command succeeded. First live header, marker append, warm-reset retention and Recovery readback await device observation. No other partition write or repository push occurred.

## 首次只读实机结果

照片与约 5.97 秒视频确认正确菜单及应用。`sig=43474244`、`next=size=00037BD9`、`capacity=0003FFF4`、`VALID`；标记计数及本次追加均为 0。当前还有 33819 字节空余。这证明 console 头和数据在新映射下可读、数值符合预期；尚未追加或验证重启保留。下一步手动追加一次，拍下 APPEND READBACK OK，再 warm restart 并先读回标记数。

## 追加成功，Warm restart 返回

第二段约 33.52 秒视频中，约 2 秒可见 `APPEND READBACK OK`，`now=1`、`appended this run=1`。`next=size` 从 `00037BD9` 增至 `00037C09`，增加 48 字节；签名及 VALID 保持。约 15 秒显示 `Warm reset returned. Reset/retention NOT proven.`，没有实际重启；约 28 秒应用返回，父级显示 START IMAGE、Success、code 0。此 Success 仅表示应用正常返回。

因此 RAM 追加及同次运行读回已验证，跨复位保留仍未验证；不能据此认定 RAM 丢失或 PSCI 不受支持。当前应用未记录实际安装的 ResetSystem 路径和 PSCI 返回值，需先核对该路径，再建立可观察的真实复位测试。旧首图索引曾把 7 误读为 F，现已更正为 `00037BD9`，初始空余为 33819 字节。
