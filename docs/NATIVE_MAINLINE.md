# 原生主线：接收、预链接、日志与镜像准入

本轮目标是让一个可追溯的原生内核进入最终镜像，并能带回失败或成功证据。
仅新增观测数组、源码编译成功或预链接成功，均不足以准许上机。

## 当前结果与固定对象（2026-10-27）

**原生带日志候选已进入最终镜像，并通过主机反解审计；尚未上机。**
选定 [CI40458123294 / attempt1](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40458123294)，
设备源码 `474c8a65b731d293987afdf550112f7b02f9ea32`，独立锁为
`configs/native-log-candidate-40458123294.json`。该 CI 的完整 XNU/原生驱动编译、实际链接
日志审计和加密导出成功；同提交 Porting record CI40458123525 通过。
后续 host/reader/打包源码的 CI 是回归，不自动替换此已固定内核。

已发布实现提交 `36e1bd8a2c4482ca55386d005112501d305c37bc`：
[完整回归 CI40464005127](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40464005127)
与 [Porting record CI40464005454](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40464005454)
均成功。回归产物未重新选入本轮镜像；候选身份仍为下表所列身份。

| 对象 | SHA256 / 字节 |
| --- | --- |
| 已认证 artifact ZIP | `d0af0cd3ad4ec3b4d57b5ba1c9f9aec257b1c799af3e92183dc5cfc7a8880adb` / 7,140,809 |
| 源码内核 | `6485c11f6be7056f00da796595c153208556349fe3254281d08bfff19cc8de5c` |
| 最终预链接内核 | `150c5c5f13a4337eb05218232f3a157429f0974ba49f71b4627789497132d074` / 52,563,968 |
| 最终 boot.img | `801ddb86b29844e316053f4d061a9fd2cb266e47f1c27ed1d46745b036dbb8e7` / 8,359,936 |
| 实际公共 DTS 生成的 ADT | `806ad48c85881b51f6acbeeea7ff4ec065f9edbe32d90f4aa304b5046faa64a3` |
| 日志候选身份 | `1c19ee51d5388dcfbbaefd41dcf361a54ccee3d08d889f6f0e4bb6f69a08d892` |

私有输出名 `j716f-native-log-40458123294-boot.img`；公开只保留
[脱敏摘要](../research/native-image-40458123294.json)，不发布镜像、完整 ADT/DTB、种子或原始日志。
候选锁的 `native_adt_sha256=9a0c…` 是继承的旧基线参考，**本次未使用**；
本轮硬件 DTS、公共生成器及最终 ADT 由专用 manifest 单独绑定。为保留接收 receipt 的锁摘要，
不追改该锁冒充原始接收记录。下一版锁应显式区分历史参考和最终 ADT。

### 最终镜像检查实际做了什么

- Android v2 头、SHA1 image ID、gzip、附加 DTB、边界及 padding 反解核对。
- 从最终镜像解出 FD → LZMA FV → FAT → 五个文件，核对实际新内核、ADT、reader、ARM 和菜单；
  再核对 FD 模块 GUID 集合、实际链接 UniXNU/DarwinBoot/NativeFat/Keys 与构建产物。
- 检查原生 provider/observer、source POST HMAC、预链接前后 native executable segments；
  本机重新审计 logger，不仅信 CI JSON。预 MMU 可达 711 条指令，无间接/外部 runtime/SIMD 调用。
- 固定 FD `0xce000000+0x700000`，未扩容；FV 使用 6,929,720 字节，余 410,312 字节。
  SEC 与四个实际 DXE map users 含 Boot Log、分配窗口和 FD 保留描述符。
- 新 FAT GUID `90f40472-1975-40e2-8028-c76d530d15a7`；无旧 trace menu、代码洞补丁或逐函数复位。
  菜单仅 reader/ARM/native/exit；仅按 Enter 选择，没有倒计时、输入失败回退或自动重试。

上述结果均为 host-only。`hardware_launch_admitted=false`；完整保留 RAM 日志闭环仍未完成。

## 历史接收基线（保留，不再默认选用）

`configs/native-candidate-lock.json` 固定
[CI 40448429664 / attempt 1](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40448429664)，
设备源码 `fcf79e5624c2ab2b0d3c090a3f522fe494a89bdc`。
该对象是 native timer + ArmDTPlatform + ArmCpu + IRQ observer 的编译基线，
不含可恢复早期日志。加入日志后必须重新构建并新建候选锁，不能覆盖此锁或沿用其身份。

固定 UniXNU `42bffe8a56823dad2e448c776b4f94862f3e290a`。
上游标准审查参考 `f85b5a60a90847dd1d640ee787b54f6a79333969`，不是升级构建依赖。
预链接配方已经由 OS profile 继承，使用 XnuPrelink，不需要另造预链接器。

### 已验证的接收与预链接（2026-10-27）

外层 ZIP SHA256 `38948c29d705ef0d3d59531b07cf5c2ba4ee559c5b843820396a0b22e857dba3`。
实际源码内核 `dfcd6428da81396df744538384509a60a0ba704ed1549b39fda51985387ac6c8`，
预链接内核 `c74b038df2afb8bf260491f0168e44b47dde04e353575f489ecfd16c575f46e6`，
52,248,576 字节。六产物、原生 provider 定义、observer 初始 ABI、源内核 executable
segments 逐字节保留及 source POST HMAC 均通过。原始输入、报告、失败尝试只留私有目录。
这填补了旧原生基线的接收/预链接缺口，不是新日志候选或硬件成果。

### 原生日志构建接入

`prepare-native-logger.py` 以六个固定、完整上游补丁后的 XNU 文件为前像，生成正常设备
patch series；标准构建器再次从干净 XNU 重放。ArmDTPlatform/ArmCpu 只改隔离副本。
移除平台阶段复位/主动 panic 诊断，保留真实驱动及中断处理路径。
`check-native-logger.py` 检查真实链接内核中的符号、独占永久 DATA 窗口、候选身份、
source HMAC 和预 MMU 可达调用图，不接受外部 runtime、间接调用或 SIMD。

`package/native-log/` 使用已核对 console `0xb00be000+0x40000`，只在未用线性尾部
预留 8192 字节，63 条一次写入记录；不覆盖旧会话、不自动清环、不主动复位。
覆盖入口、VM、平台、CPU、同步/SP1 异常、IRQ1/2/8 enter/rtclock/EOI、单核 context switch。
header 与记录分别含候选/会话身份、CRC 和最终 commit；无有效新 ARM 时停在 WFE。
boot/permanent 映射使用独占内核静态 VA，禁止借用 P22 子表；完整运行期验证仍待实机。

主机 GCC/Clang 的生产 ABI/映射、边界/CRC/撕裂/旧会话拒绝、AArch64 汇编及隔离驱动
安装测试通过。辅助测试不替代完整 XNU 链接、UEFI reader 编译或硬件取证。
本轮已完成带日志 XNU CI、独立 reader/ARM 编译、专用固件和最终镜像的主机检查。
双编译器也执行生产固件门槛与 11 组手动菜单测试；真实 linked EFI 的 14 组模拟验证
reader 零保留 RAM store、无 SMC，ARM 只调用 VERSION/FEATURES，无 RESET。
主机模拟不能替代屏幕、按键、物理 RAM 或真实 SMC 的本板验证。

### reader 与恢复边界

Read 和 ARM 是两个独立 EFI 应用。Read 只冻结复制并校验整块 console 前后 CRC；
不 Query、不 ARM、不清环、不修复、不复位。ARM 要求只读 Query 成功及双确认，提交前再比对
整块 console CRC；只追加未用线性尾部，已有未消费 ARM/重复 session/无空间均拒绝。

内核只接受最新线性尾部的新 ARM，一次消费。reader 则可从有效线性区恢复被启动链追加数据
遮在尾部之前的完整会话；页面明确标注“retained session，不一定属于本次 boot”。
恢复证据必须与独立 ARM 照片中的候选、session、header CRC 对上，不能把旧会话当新结果。

按键：音量上下翻页，页边界切换会话；Power 下一页，长 Power/ESC 返回菜单。
拍下 header 及全部非零 record 页，以 `decode-native-pages.py --candidate <完整身份>
--session <独立ARM照片ID>` 解码；输入格式见脚本。重建 8192 字节会话 CRC，不冒称从照片
重建整块 console CRC。无有效日志只能记为未知，不能判为通过。

**当前没有可保证的挂死恢复方案。** WFE、异常或 VM 失败后，外部复位是否保留该区、
重启链是否重置 DBGC、永久映射是否存活，均待同镜像实机验证。不能承诺“任何失败都能取回证据”，
也不能用加回逐函数 SYSTEM_RESET 来绕开此问题。

## 专用构建路径与复现边界

普通 `board.toml` 固件配置仍指旧 `j716f.dsc`；默认 `check-j716f.py` 现在在读取厂商输入之前
直接拒绝，旧 TzDxe preflight 原样逻辑归档。**不要绕过 preflight，也不要声称标准 CLI 已端到端迁移。**
`board.toml` 属于当前内核日志身份输入，未为修注释而追改或冒充同一候选。

接收/预链接/专用 staging 均拒绝覆盖已有目录。以下路径均为私有工作目录占位符：
主机使用 Python 3.11+、cryptography；实际内核审计需要 LLVM objdump 或 Capstone。
Linux 固件构建另需 clang/lld、EDK II BaseTools、dtc 与 mtools。

```sh
python3 scripts/receive-native-candidate.py --selection configs/native-log-candidate-40458123294.json \
  --artifact-zip /private/artifact.zip --private-key /private/local-key.pem --output /private/received
python3 scripts/prelink-native-candidate.py --selection configs/native-log-candidate-40458123294.json \
  --received /private/received --unixnu /pinned/UniXNU --kernelcache /private/kernelcache --output /private/prelinked
python3 scripts/prepare-native-firmware.py --selection configs/native-log-candidate-40458123294.json \
  --prelinked /private/prelinked --unixnu /pinned/UniXNU --uefi /pinned/uefi-j716f --output /private/stage
```

staging 从 Git archive 读取板级 UEFI `21ac8e3477daf001ea6046283698165cbf3fd1d8`、
UniXNU `42bffe8…` 和 DarwinBoot `3325e352d7df68a9f9ad74832ed94a6acbbd7390`。
菜单派生自该固定上游库，不沿用 P22 trace library。新生成 DSC/FDF 包含旧板级的受限运输选型，
不代表板级历史实现已经全部标准化。

在 Linux 的独立 stage 配置 EDK II `WORKSPACE/CONF_PATH/PACKAGES_PATH`；路径必须含 stage、
stage/Platforms/Lenovo、stage/package、stage/DarwinBoot，以及完整 Mu/Silicon/overlay 依赖。
本轮第一次尝试漏掉 UEFI 根路径，在 Keys 编译前失败；失败目录保留，修正后独立 r2 成功。
实际顺序为：

```sh
dtc -I dts -O dtb -i inputs -o inputs/hardware.dtb inputs/j716f.dts
python3 /device/scripts/make-native-mainline-adt.py --unixnu /pinned/UniXNU \
  --dtb inputs/hardware.dtb --output inputs/kernel.macho.adt
build -a AARCH64 -t CLANGPDB -b RELEASE -p NativeLogApps.dsc
python3 build-native-payload.py /private/stage
build -a AARCH64 -t CLANGPDB -b RELEASE -p Platforms/Lenovo/j716fPkg/j716f-native.dsc \
  -D FD_BASE=0xce000000 -D FD_SIZE=0x700000 -D FD_BLOCKS=0x700 -D ENABLE_SECUREBOOT=0 \
  -y firmware-build-report.txt -Y LIBRARY -Y DEPEX
```

公共 DTS 修正 DRAM 误导出为 `/arm-io/dram`，保留四组 timer PPI；固定公共生成器仅为 SMP
输出 PSCI，因此设备适配器只补单 CPU 需要的 root PSCI 节点。验证 CPU0/GIC/19.2 MHz/SMC、
保留区 no-map、DRAM 所有权，移除本轮无关用户态 text fixups。ADT 内随机种子不公开。

本次共用固件依赖根实际是 `42fe563791e51bf5500c2e470ed294ca0c94b97c`，**不是**板级 archive
的 `21ac8e3…`。Mu_Basecore 为 `3c9c7b2e211e62d2a954ad760a36fe05adc43aa3`，
Common/Mu 为 `b8d46c71610318e36a661819e1e3c0b8bdda413a`，Mu_OEM 为
`b70b4b7bb02c43bf9db2172dfb3ed0457e2361d6`。保留构建日志、每个 staged source 哈希和
**构建后** 815 份依赖文件所引用的 1,884 个输入/生成文件哈希；不能把事后快照当作干净重建。

已审查的已用差异：虚拟 timer 的 CNTFRQ=0 fallback、AArch64 汇编 `.text` COFF 兼容、
framebuffer 字号与换页清屏；libfdt 差异为行尾格式（忽略行尾空白 diff 为空）。
BootManagerMenu 电源项和 USB TPL 改动也被编译，但对应模块不在最终 FDF/FV 中；不能把
“编译依赖集合”直接当成“最终模块集合”。共享树未修改。干净依赖闭包仍待正式固化。

最终 `package-native-image.py` 需要 staged source manifest、实际 build-result、payload manifest、
build report 和实际 FD/FV/EFI 等输入；本轮本地协调器生成这些私有构建记录。
它不是一个已完成的通用远程构建入口。齐备后：

```sh
python3 scripts/package-native-image.py --stage /private/stage --prelinked /private/prelinked \
  --shim /private/qualified-shim.bin --dtb /private/qualified-firmware.dtb \
  --uefi /pinned/uefi-j716f --output /private/final-new
```

不要覆盖本次 final、receipt、prelink 或失败尝试。新生成随机 ADT 或改变任一输入都必须记录为新镜像，
不能沿用上述 final hash。

## 上游标准差距

依据 [移植步骤](https://unixnu.com/zh/porting/steps)、
[设备仓库](https://unixnu.com/zh/porting/device-repository)、
[平台与内存](https://unixnu.com/zh/porting/platform) 和
[日志抓取](https://unixnu.com/zh/porting/log-capture)逐项执行：

| 项目 | 本轮处理与剩余边界 |
| --- | --- |
| 当前状态 | P22 收尾并冻结；旧观察保留；清除当前 next_action 中旧 P5/P7/P17/P22 操作 |
| 默认组件/构建 | board 默认 native timer、ArmDTPlatform、ArmCpu；完整 archive 顺序；旧 V17 配方归档 |
| 来源固定 | CI/组件/Apple 输入分别校验；当前 SoC 仍是明确标识的本地过渡快照，不冒称公共适配完成 |
| 四层解析 | arch→SoC→OS→device；预链接从 OS 继承；不得因 board 未显式声明而忽略 |
| 硬件树 | 公共 DTS→ADT 已实际编译核对；单 CPU PSCI 补充明确记录；真实匹配与 IRQ 待测 |
| 固件链 | 现有嵌入 FAT 诊断链与标准 initrd 链不同；不得仅改声明就称已迁移 |
| 固件日志 | Advanced Logger/二维码尚未完成本板集成，列为缺口；不阻塞保留 RAM 早期采集的独立实现 |
| 原生日志 | 真实源码 writer、独立只读 reader/ARM、解码器与主机测试完成；实机长期映射/恢复未完成 |
| 最终镜像 | 已反解 boot/FD/FAT，实际内核/ADT/菜单/读取器逐字节一致；没有实机结论 |
| 通用构建入口 | 普通 firmware preflight 明确拒绝，专用脚本可审计；仍需整合标准构建及干净固件依赖闭包 |
| 发布 | 已推送脱敏源码并运行 CI；只读仓库 API 核查 topics 为空，尚缺上游要求的 unixnu topic，目录发布未确认；未改仓库设置/中央设备记录，未公开私有二进制 |

## 准入门槛

1. 接收：外层 artifact ZIP 摘要、运行/提交、解密认证、六产物清单、实际 Mach-O 定义符号、
   native timer 路由和 observer 初始化 ABI 全部匹配。无凭据时等待用户正常登录/下载，不搜取令牌。
2. 预链接：锁定 kernelcache，使用固定工具/OS profile 的 roots、slide、早期安全初始化；
   保存报告并复核源内核代码段、平台符号和 source-crypto HMAC。不得用 P22 内核替代。
3. 专用打包：不运行历史 entry/panic 二进制补丁，不保留逐函数 SYSTEM_RESET，
   单核、无倒计时、无自动启动。源内核和预链接后的内核分别记录哈希。
4. 日志：保留 RAM 的所有权、早期物理访问、VM 切换、长期映射、缓存属性与异常重入都需核验；
   writer、只读 reader、主机 decoder 使用同一版本、候选身份和本次 boot/session 标识。
   CRC/commit 撕裂、越界、陈旧记录必须拒绝；不得把“日志缺失”解释成“函数通过”。
5. 硬件：先交付候选哈希和读取/恢复步骤并取得单独授权；仍仅限 boot_b，保留 boot_a/vbmeta/userdata。
   第一门槛仅为新菜单/只读 reader，拍照确认身份、稳定冻结和当前保留区，不进入 ARM/native。
   随后依据现场审查一次原生启动及挂死恢复方案；未经验证不能宣称无条件可恢复。

## 同一候选的观测顺序

入口与 MMU/VM → 平台专家 match/configure → CPU0 注册 → GIC 初始化 →
真实 timer INTID27 进入、原始 rtclock 返回、原始 EOI → 多次 IRQ 与单核调度/超时。
失败后读取该次候选的现场再定位。UFS、根卷、用户态不列入这一轮成功判据。
