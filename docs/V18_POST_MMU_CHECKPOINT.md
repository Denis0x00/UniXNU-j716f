# J716F V18：持久化 post-MMU checkpoint

V17 已证明 MMU 开启后的代码能够运行到 PSCI 复位调用。V18 将同一 stage 3
复位代码替换为 `WFE` 永久循环：不再自动复位，也不再调用任何平台驱动。
因此屏幕应保持 V16/V17 的绿色、蓝色和品红色阶段条；保持画面本身就是
“已越过 MMU 开启点”的停点证据。

## 主机候选

- boot image：`D:\TB-J716F\Firmware\j716f-mu-zui13-entry-v18-boot.img`
- boot image SHA-256：`62f02389a251b090d42a81c8ec25e23af7c7c41fddaa72af367cbf671d7a9323`
- FAT payload SHA-256：`cb715ac4e5bf5cb3e198afdfff035a6f256e3fcd95efde5db8a3f999d09d2953`
- kernel SHA-256：`fbd98ee925bc0e6d2ad612b0a657e13efa3b25afddd9d21ef27a71dfd60f9faf`

这些是本地私有输入生成的诊断镜像；二进制、ADT、照片和 vendor 输入不提交
到 GitHub。主机校验确认 V18 FAT 与 boot image 中的 kernel 字节完全一致，
stage 3 cave 为 `WFE; B .`。

## 真机判读

- 保持三色条：已到达持久化 post-MMU checkpoint。
- 又回到 UniXNU 菜单：仍在使用旧镜像，或发生了外部复位；先核对 SHA-256。
- 出现 panic screen：记录完整画面；这不等于 iPadOS 启动。

测试仍只允许写 `boot_b`，不写 `boot_a`、vbmeta 或 userdata；TzDxe 继续不处理。
