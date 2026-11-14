# Independent host DT panic-region kernel

Read README.md and board.toml. Use scripts/host.py for official host-only steps.
Keep this fresh output root and preserve every old target/artifact.
Only effective configuration change from the original host target is
CONFIG_XNU_DT_PANIC_REGION=y; preserve 5 components, 58 patches and 8 archives.
Use a fresh official tree; do not copy object files, SDKs or compile records.
Source caches may be reused after fixed revision/hash validation.
Retain bootstrap, archive, link and prelink stages; no skip-prelink or fake stamps.
Verify pe_init.o compile flag and rederive new kernel symbols before diagnostics.
No QEMU, firmware, payload, root/Data, flash or other device actions here.
