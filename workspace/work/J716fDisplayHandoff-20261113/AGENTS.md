# Independent display handoff integration

This package is limited to the pinned J716F/iPadOS 17 host target. The accepted
GOP resource provider and all prior targets are immutable. Read README.md and
the source-chain report before changes. No framebuffer mapping or pixel writes,
display publication, GPU/DPU access, new boot image, device action or QEMU.
Keep boot arguments as requests for validation, never reservation grants.
Use actual source host tests and the consuming target's official kernel build.
