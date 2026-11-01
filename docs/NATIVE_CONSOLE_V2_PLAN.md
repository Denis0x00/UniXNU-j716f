# RAW/Init v2 方案：安全建立原生日志会话

历史设计文档；最新实际实现及授权状态见[MAINT v2](NATIVE_MAINTENANCE_V2.md)。
本轮采用更小的单应用Export→Init，不实施跨应用ticket；菜单缩字/电源项暂缓。
以下保留方案形成时的范围与未完成声明，不覆盖后续实施记录。

2026-10-27，Asia/Shanghai。状态：**方案与主机边界模型；尚未实现进固件、编译、推送或刷写。**
本轮用户允许准备下一版方案。此前 Init-only 授权不延伸为新镜像部署、重启/关机、ARM 或 XNU 启动授权。

## 目标与不变项

下一成果是：同一原生候选，在完整快照已落盘且仍匹配现场时，能够显式建立新日志区。
不回到 P22/代码洞/逐函数复位，不改变内核入口、VM、IRQ 或调度路径。

- 固定内核 CI40458123294/attempt1，logger identity
  `1c19ee51d5388dcfbbaefd41dcf361a54ccee3d08d889f6f0e4bb6f69a08d892`。
- 预链接内核 SHA256 `150c5c5f13a4337eb05218232f3a157429f0974ba49f71b4627789497132d074`；
  ADT SHA256 `806ad48c85881b51f6acbeeea7ff4ec065f9edbe32d90f4aa304b5046faa64a3`。
- 不改 `NativeLog.h`、映射/内核 hooks、candidate identity 输入、ARM 会话 ABI 或8192字节准入。
- 新固件、菜单、工具及协议使用独立维护版本/manifest；最终镜像重新反解、另记哈希。
  不覆盖 RAW-v1 镜像、旧完整导出、Init 拒绝照片或 P22 冻结证据。

依据[官方构建顺序](https://unixnu.com/zh/building)，只有固件/工具变更时复用已核验内核，
重新构建相关 EFI、FD、payload 和 boot.img。源码 CI 成功、主机模型通过和真机结果分开记录。

## 1. Init 状态分类：不是移除 size 检查

现场依据是[Init v1 拒绝记录](../research/native-init-blocked-20261027.json)：
`next=size=0x3f56e`；对齐后仅剩2688字节。此前满环快照仍完整保存，但不能覆盖当前快照身份。

令 `total=0x40000`、`capacity=total-12=0x3fff4`、`session=8192`。
先验证 magic 与整数边界，再计算 `offset=align_up(12+next,8)`，避免非法长度参与运算。

| 头状态 | v2 拟定处理 |
| --- | --- |
| `size=capacity, 0<=next<capacity`：满环 | 可进入备份回执审查，仍不是写入许可 |
| `next=size=capacity`：满线性 | 单独识别原生线性 ABI 的满边界；经相同审查可进入初始化 |
| `next=size<capacity, offset+8192>total`：线性空间不足 | 新增支持；当前 `0x3f56e` 属于此类 |
| `next=size<capacity, offset+8192<=total`：线性空间足够 | **拒绝 Init**，保留现有日志；另审查正常 Query/ARM，不自动跳转 |
| 非满但 next!=size、长度越界、坏 magic | 拒绝；只读导出，不修复、不推测有效长度 |

精确边界：`next=size=0x3dff4` 仍能放下8192字节；`0x3dff5` 起空间不足。
`next=size=capacity` 是原生线性发布允许的边界表达，不等同于旧 Ring.h 的环形 next-write 约束；
实施时必须作为独立分支测试，不能混用两套有效性判断。

以上只是头分类。所有可初始化类别仍须通过：

1. 完整262144字节冻结稳定；整个原始区扫描，不仅扫描逻辑有效长度。
2. **任意候选的完整未消费 ARM 一律拒绝**，包括逻辑范围外的物理字节。
3. 可识别但版本/CRC/offset/state/reserved 不合法的原生会话头，归为 `SUSPECT_SESSION`，
   同样拒绝；不把撕裂或未知记录直接视作“没有 ARM”。完整已消费记录可以进入备份审查。
4. 当前固件会话的导出凭据、维护版本、候选、完整快照 SHA 和主机回执一致。
5. 用户显式输入回执、两次独立确认；提交前重新哈希并两次逐字节核对 live。

扫描规则的漏检边界须明确：没有识别到当前 ABI 标记，不证明任意历史格式或被破坏的记录不存在。
不为了通过门槛人为写满环；不增加绕过开关。

## 2. 同次启动导出凭据：重启后必须重新导出

本轮发现的流程缺口是 Export 与 Init 之间可以重启，而 v1 回执只绑定 candidate+快照字节。
拟由**仍驻留的主菜单**管理一个版本化、纯数据的 Boot Services 临时协议，供独立 Export/Init 使用：

- 菜单启动时从空状态创建；仅普通 Boot Services 内存，不写 NVRAM、分区或保留日志区。
- Export 每次稳定冻结后发布新 ticket，绑定维护 profile、candidate、整区 SHA/CRC 和传输 ID。
  新 Export 取代旧 ticket；没有 ticket、重复协议实例、未知 ABI、owner 不存活均拒绝 Init。
- Init 只接受同次菜单运行中的 ticket，冻结 SHA 必须一致。变化、提交尝试、菜单退出及已确认的
  电源操作都撤销 ticket；重启后初始状态为空，必须重新 Export。
- 协议不持有已返回 EFI 应用的代码指针；由驻留 owner 负责生命周期、注销和清理。
  所有错误退出和 ExitBootServices 路径都必须验证，不允许悬空指针或旧状态复活。

主机接收还需显式选择新固件 manifest/profile，并与当场 Export/Init 页面 ticket 核对；
不能仅凭一个格式正确的旧视频认定它属于当前启动。
计划使用独立 `J716F2` 光学帧和 `J716F-INIT-RECEIPT-V2` 回执域，绑定
`profile + candidate + snapshot SHA256 + ticket/transfer`。完整帧集落盘、fsync、读回以后才能生成回执。
v1 解码仍保留作历史读取，但 v1/v2、不同 profile 或 transfer 均不得混合，也不得跨版本使用回执。
每帧仍携带1024字节原始载荷，全部256帧；新增字段的 QR 容量、帧边界与实机可读性须重新测试。

profile 是规范化**构建前输入清单**的哈希，不包含自身或最终镜像哈希，避免循环定义。
最终 boot.img 哈希另由主机审计绑定 profile、工具 EFI 和原生内核。
ticket 是流程防误操作凭据，**不是密码学启动认证或主机备份完成证明**；普通 EFI 同权限代码仍可改写它。
无合格随机源时不得宣称硬件唯一、不可重放或安全熵。

## 3. 写入范围、读回与界面

- 保留当前有界提交顺序：signature 先失效 → 写初始化记录 → next/size → signature 最后发布。
  修改仅限前272字节，后续原始字节必须逐字节不变；不整区清零，不自动 ARM。
- 初始化记录保持256字节、单独 version=2；在已预留字段记录 profile、ticket、分类原因。
  结构偏移、CRC覆盖、撕裂边界必须静态断言并在独立解码器验证；内核会话 ABI 不变。
- 任意提交失败终止当前 invocation，不自动重试。TPL 屏蔽固件事件不等于防 DMA/安全世界并发。
- Read 增加只读 Init-v2 记录解码，明确显示 `INIT VALID / NOT ARMED`，避免把“无原生会话”
  误读成初始化失败；已有原生会话扫描规则保留，Read 不清理/修复任何字节。
- Init 首页分开显示 `FULL_WRAPPED / FULL_LINEAR / LINEAR_EXHAUSTED / SPACE_AVAILABLE`、
  可用对齐字节、8192字节需求、ticket 状态、完整 SHA（两行32位）及具体拒绝码。
- 回执按4位一组显示；提供退格/取消并清楚显示当前位置。必须验证连按、长按、释放和事件残留，
  不让最后一个数字的 Power 事件顺带完成确认。输入错误和确认取消都不写保留区。

## 4. 菜单缩字与电源动作

参考[官方启动配置](https://unixnu.com/zh/reference/boot-configuration)并保留固定上游 UI 源码：

- 主菜单由当前 `scale=3` 改为候选 `scale=2`，缩短标题，沿用官方 logo、选择样式和横屏布局。
  2560×1600 下字形高度由57像素降为38像素；必须做实际绘制截图/边界检查，不凭参数认定布局合格。
- 主菜单拟保留 Read、Export、Init、ARM、Native、Restart、Shutdown、Exit 八项，默认仍为 Read。
  不添加 Tools/自动发现项目到严格白名单；官方 Advanced Logger 集成另外立项，不冒称 RAW v2 等于它。
- Read/Init 的 Panel.h 当前已是 scale2，**不能盲目一起改小**。主要改善信息布局、断行和可读性。
  QR 区、白边和密度不因文字缩小而缩小。
- 使用官方 `action="reset"` / `action="shutdown"`，强制确认文字和默认 Cancel 的选项。
  展示“未导出 RAM 可能丢失，重启后需重新 Export”；危险项不作为默认或错误回退。
- 新 action 白名单必须核对动作、顺序、参数、confirm 和 confirm choices；现有生成 guard 只认识 Exit，
  不能简单增加两项而不更新 guard、打包审计及负例测试。

电源路径已查到：固定上游 Boot.c 使用 `gRT->ResetSystem(EfiResetCold/EfiResetShutdown)`；
当前构建报告实际链接 `ResetSystemRuntimeDxe → ArmPsciResetSystemLib → ArmMonitorLib`。
本地对应源码与构建后依赖清单哈希匹配。**这只是来源审查，不是本板重启/关机验证。**
实施时还须核对实际链接指令/PCD 的 SMC conduit，并增加动作前的 PSCI VERSION/FEATURES 能力审查。
只允许 SYSTEM_RESET/SYSTEM_OFF，不走 warm/RESET2，不写 PMIC 寄存器，不增加 fallback。
用户取消/能力不支持/输入出错时不得调用电源动作；调用返回只报失败，不再次尝试。
菜单电源项不得出现在内核自动错误处理路径；真正电源测试单独授权且先导出现场。

## 5. 实施顺序与验收门槛

1. **来源冻结**：保存 RAW-v1 工作树实现和构建哈希；新 profile/目录另建，不改旧 staging、快照或报告。
2. **策略与协议**：实现分类/拒绝码、可疑记录保护、Boot Services ticket 生命周期、v2帧/回执、Init-v2记录。
   C 与独立 Python 逐字节互验，不把本方案的 Python 模型当作真实 C 测试。
3. **UI/电源**：scale2、短标题、完整哈希、取消优先确认、白名单和临时 ticket 撤销。
4. **主机验证**：双编译器、真实 linked EFI 模拟、实际 UI 截图、QR 编解码、输入事件、断电撕裂及无越界写。
5. **固件构建与反解**：复用固定内核/ADT，重新编译 EFI/FD/payload/boot，验证实际源文件/模块/库映射。
   内核和 ADT 必须与上面的哈希逐字节一致；新镜像不得混入旧复位探针。
6. **硬件准备**：先完整保存当前新快照；新镜像部署须另行授权。部署经过重启后，**再在新固件内重新
   Export → 主机落盘核验 → Init**，中间不重启、不进 fastboot。刷写前备份不能代替新启动的 ticket。
7. **下一次成果**：Init读回成功 + 独立Read验证初始化记录，明确尚未ARM。之后才单独审查
   原生启动和失败取证路径，再验证平台→CPU→GIC→真实timer IRQ/rtclock/EOI→单核调度。

必须覆盖的反例：完整满环/满线性/当前0x3f56e、临界容量前后、足够空间、非法头、pending ARM任意候选、
损坏/未知会话、旧v1回执、错profile/错SHA/错ticket、跨重启无ticket、重复协议、owner退出、
残留按键、未双确认、快照/现场变化、四个提交屏障中断、写入范围、Init后Read/正常ARM兼容性。

主机方案检查：`python scripts/check-native-init-v2-design.py`，只验证分类数学与抽象状态机，
**不执行生产初始化、不生成回执、不访问设备、不验证物理保留。**

## 6. 上游差距与明确不做的事

[官方日志指南](https://unixnu.com/zh/porting/log-capture)区分固件日志和内核交接后的日志；
本方案仍是本板保留 RAM 原始证据通道，不是 Advanced Logger/OneSend。
[bring-up顺序](https://unixnu.com/zh/porting/bring-up)要求先保存失败现场，再修改与复测。
标准 initrd、干净固件依赖闭包、Advanced Logger、保留区所有权/挂死恢复仍需后续验证。
不以本次 Init-v2 修补宣称这些已完成；不切换到 UFS/根卷/用户态，不自动发起新内核 CI。

当前新快照仍待用户提供完整只读导出。该缺口不阻止准备方案/主机工作，但会阻止任何清理写入或新部署。
