# Isolated J716F resource-provider work

Read README.md before changing sources. Only this directory and the companion
analysis/stock-display-adapter-20261113 directory belong to this task. Preserve
production targets, deployed r6, prior audits and original input binaries.

Use the actual resolved XNU compile record and the consuming target's official
build kernel. This is resource-provider-only, not NativeLegacyDisplay. Never
substitute a boot argument or a physical address constant for a trusted current
boot reservation. No framebuffer mapping, stores, GPU/DPU access, display
registration, fabricated ABI or runtime claims. Record hardware separately.
