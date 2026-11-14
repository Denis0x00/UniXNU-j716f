# Independent CPU0 diagnostic host target

Read README.md and board.toml. Use scripts/host.py for the unmodified official
UniXNU kernel workflow. Source baseline is the frozen host-dt-panic target;
the only component override is ../ArmCpu-j716f-diag-20261113. Keep ordinary
arm64, 4 KiB, CPU0, DT panic correction, eight archives and original patch order.
No UFS activation, payload, boot image, tablet operation or QEMU in this target.
Freeze source hashes before compilation. Do not copy compiled objects or SDKs.
Host and physical qualification remain separate; all 14 areas stay planned.
