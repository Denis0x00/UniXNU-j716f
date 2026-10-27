# 原生主线：接收、预链接、日志与镜像准入

本轮目标是让一个可追溯的原生内核进入最终镜像，并能带回失败或成功证据。
仅新增观测数组、源码编译成功或预链接成功，均不足以准许上机。

## 固定接收对象

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
本轮源码允许推送与运行 macOS CI，但 reader/ARM app、专用固件及最终镜像仍未完成。

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
| 硬件树 | CPU0/GIC/19.2 MHz/PSCI 已有输入证据；标准 DTS 的 timer 路由及保留区仍需与最终 ADT 核对 |
| 固件链 | 现有嵌入 FAT 诊断链与标准 initrd 链不同；不得仅改声明就称已迁移 |
| 固件日志 | Advanced Logger/二维码尚未完成本板集成，列为缺口；不阻塞保留 RAM 早期采集的独立实现 |
| 原生日志 | 必须接入真实源代码边界与保留 RAM；旧静态 observer 不能算持久化通道 |
| 最终镜像 | 必须反解最终 boot/FD/FAT，核对实际内核/ADT/菜单/读取器及所有来源哈希 |
| 发布 | 用户已授权本设备仓库脱敏源码推送/CI；不变更中央设备记录，不公开私有二进制，不操作设备 |

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
5. 硬件：先交付候选哈希和读取/恢复步骤；仍仅限 boot_b，保留 boot_a/vbmeta/userdata。
   没有可恢复日志的镜像不得作为本轮上机候选。

## 同一候选的观测顺序

入口与 MMU/VM → 平台专家 match/configure → CPU0 注册 → GIC 初始化 →
真实 timer INTID27 进入、原始 rtclock 返回、原始 EOI → 多次 IRQ 与单核调度/超时。
失败后读取该次候选的现场再定位。UFS、根卷、用户态不列入这一轮成功判据。
