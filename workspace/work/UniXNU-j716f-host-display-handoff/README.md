# Independent J716F current-boot display memory handoff

Host-only successor of the frozen display-adapter target. The original eight
recipes and the fixed CPU0/DT-panic inputs remain; the ninth archive combines
the unchanged resource provider with a new current-boot exclusion checker and
platform registration caller. One additional device patch invokes it only
after virtual platform configure succeeds. No NativeLegacyDisplay, pixel
mapping or stores, scanout, payload, boot image, tablet access or QEMU.

The new checker must derive authority from actual kernel memory globals and
pmap state. A boot flag only requests checking. The proof only covers this
fixed ordinary-arm64, CPU0, 4 KiB XNU memory implementation, and refuses a
window that is not entirely above the 36 MiB stock display allocation.

Use scripts/host.py for official defconfig/fetch/tree/status/build kernel.
Results live in ../../analysis/touch-display-handoff-20261113/README.md and
../../analysis/display-handoff-chain-20261113/README.md. Keep all previous
targets frozen and all 14 areas planned. Onsite next step is still deployed r6.
