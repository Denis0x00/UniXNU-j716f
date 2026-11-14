# Independent r30 storage integration host target

User-authorized extension of the accepted host-iokit-cpu0 target. Preserve its
60 XNU patches, CPU0 asm repairs, early log, text console, DT panic setting,
21H461 prelink ABI and eight archive recipes. Append the reviewed storage
component and explicit initialization recipe only in this independent target.

Use scripts/host.py for the official defconfig/status/tree/build kernel pipeline.
Every host build result is separate from hardware evidence. Do not modify the
parent target, frozen core or r29/r30 images. The storage bootstrap may register
interfaces and report gates; it must not invent a handoff, acquire controller
ownership, access MMIO, issue DMA or block writes, or mount a root filesystem.
No device operations or QEMU. Reparse the new executable before packaging.
