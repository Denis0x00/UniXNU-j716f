# Independent host UFS observation target

Read README.md and board.toml. Use scripts/host.py for official host steps.
Preserve all earlier targets/artifacts and the deployed r6. No QEMU or device operations.
Build only kernel/prelink from fresh bootstrap records; no SDK or object copies.
Five TUs: Runtime, Hci, Handoff, Diagnostics and one AppsSmmuOwner.
Freeze final sources using scripts/freeze-inputs.py before official build kernel.
Keep the init registration-only; observation has no automatic caller or boot flag.
The new explicit read-only diagnostic API requires real externally established
access authority; it creates no firmware/TZ ownership or DMA handoff grant.
Keep host and device qualification separate and all 14 areas planned.
