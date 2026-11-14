# Independent text-console host kernel

Read README.md and board.toml. Use scripts/host.py for the official kernel-only pipeline.
Keep fresh output and old targets/artifacts. The only new selected patch is the Denis PE_create_console verbose text-mode hunk; preserve the DT panic fix, CPU0, five components and eight archives. Do not add ISB/VFS/SMP/peripheral changes or copy old objects/SDK/compile records. Reparse new symbols before diagnostic integration. No QEMU, firmware/payload/root/Data/device actions here.
