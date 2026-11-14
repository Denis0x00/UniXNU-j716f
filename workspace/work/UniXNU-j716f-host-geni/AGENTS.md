# Independent inactive GENI host target

Read README.md and board.toml. Use scripts/host.py for official kernel operations.
Preserve all old targets and artifacts. Keep the five components and original
eight archives/58 patches; the ninth archive and dependency patch only integrate
the new inactive backend. Never claim compile/link success as hardware support.
Do not activate MMIO, DMA, IRQs, clocks, power or touch publication here.
No QEMU, payload, firmware, root/Data or device actions.
