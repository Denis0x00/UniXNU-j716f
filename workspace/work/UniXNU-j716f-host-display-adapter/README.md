# Independent J716F display resource provider kernel

This target derives from the frozen host-cpu0-diag target and reuses its fixed
ArmCpu override, ordinary arm64, CPU0, 4 KiB and DT panic-region correction.
It adds one board resource-provider archive after the original eight recipes.
CONFIG_XNU_NATIVE_LEGACY_DISPLAY enables the two official initialization callers,
whose implementations belong to the restricted provider. This does not build
NativeLegacyDisplay, instantiate an Apple framebuffer or validate swap completion.
GopFramebuffer is not added as a selected but uncompiled component.

Use scripts/host.py for official defconfig/fetch/tree/status/build kernel.
Sources live in ../J716fGopAdapter-20261113. The default path must not map or write
framebuffer memory, publish a display service or turn a boot flag into ownership.
Actual current-boot handoff evidence and later ABI/lifecycle work are separate.

Reports: ../../analysis/stock-display-adapter-20261113/README.md and
../../analysis/stock-driver-followup-20261113/README.md. Preserve all previous
targets and r6. No payload, boot image, tablet access or QEMU in this target.
