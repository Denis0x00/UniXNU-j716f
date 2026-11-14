# Independent J716F GENI backend kernel link

Host-only target derived from the frozen DT-panic kernel. It retains the five
components, eight original archive recipes and 58 original patches. A ninth
archive force-links the Goodix decoder, transaction owner, FIFO backend and
borrowed-MMIO adapter. One additional patch tracks that archive as an incremental
link dependency. The archive has no boot caller or automatic activation.

Only the standard official defconfig/fetch/tree/status/build kernel entry points
in scripts/host.py are supported. The official build retains bootstrap, native
kernel linking and prelink. No QEMU, payload, boot image or device operations.

This proves source and link integration, not touch availability. Actual use needs
an independently verified live owner, Device MMIO mapping, clock/pin/power grants,
FIFO-mode evidence, external bus-effect fence and real HID sink. Stock registers
and saved DT identify resources but cannot grant current-boot ownership. All
complete porting areas remain planned. Deployed r6 remains the next onsite test.
