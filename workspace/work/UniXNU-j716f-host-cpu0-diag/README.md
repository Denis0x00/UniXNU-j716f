# Independent TB-J716F CPU0 diagnostic kernel

This host-only target derives from the frozen `../UniXNU-j716f-host-dt-panic`.
It keeps iPadOS 17.7.11 / 21H461, ordinary arm64, 4 KiB, CPU0, the DT panic
correction and the eight existing archive recipes. ArmCpu alone resolves to
`../ArmCpu-j716f-diag-20261113`; this is a diagnostic component, not an upstream
update. Source creation does not establish compilation or runtime success.

Run `scripts/host.py defconfig`, `fetch`, `tree`, `status` and `kernel` on the
prepared Linux VM. The final action invokes the official `build kernel` and
produces its own bootstrap records, native link and ordinary ARM64 prelink
under `UniXNU/out-host-cpu0-diag-20261113`. Results and integration limits are
recorded in `../../analysis/offline-driver-followup-20261113/README.md` and
`../../analysis/cpu0-diagnostics-20261113/README.md`.

No image assembly, device access, QEMU or UFS activation is included. Deployed
r6 and its pending observation remain separate; physical progress is early XNU.
