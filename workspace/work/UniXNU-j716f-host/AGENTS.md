# TB-J716F host build target

Read README.md, board.toml, porting.toml, docs/hardware-baseline.md and
../UniXNU-j716f/docs/build-and-boot-gates.md. This target is authorized for host
kernel/prelink preparation and compilation only, using the official UniXNU CLI.

Use scripts/host.py with one allowed action. Do not add arbitrary argument
passthrough, build all, firmware, payload, userdata, bootimg, release, ADB or
fastboot. The wrapper limits its own entry points; the original CLI does not
implement a host-only security boundary. The parent device target now separately
allows experimental package/firmware/payload assembly; its scope does not expand
this kernel/prelink target. Follow its current phase document, not its historical guard.

Keep the five selected components, inherited eight archive recipes, single-core
defconfig/DTS and reviewed 58-patch order until a concrete build failure justifies
a scoped change. Retain logs and record any repair. Do not silently skip prelink,
disable checks, fabricate compile records or replace missing ABI inputs with stubs.
Runtime EFI memory ownership and root/Data policy are not prerequisites to this
kernel build. A host build does not verify hardware. Preserve references and
existing menu images. No device actions while the user is away.
