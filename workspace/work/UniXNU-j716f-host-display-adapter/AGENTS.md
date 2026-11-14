# Isolated display resource-provider target

Read README.md and board.toml. Work only within the explicitly authorized
host-only scope and use scripts/host.py for the unmodified official CLI.
Keep the original eight recipes/58 patches, fixed ArmCpu source and DT panic
correction. The ninth archive is resource-provider-only, not the complete
GopFramebuffer/NativeLegacyDisplay. Do not fabricate framebuffer ownership,
ABI qualification, service publication or swap results. Freeze source hashes
before each build and retain failed attempts. All 14 areas remain planned;
deployed r6, Android A/data, QEMU and device execution are outside this target.
