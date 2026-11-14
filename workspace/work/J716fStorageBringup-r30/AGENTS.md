# J716F r30 storage bridge

Read README.md and source-manifest.json before changing this component.
This is a host-prepared bridge to the frozen SM8250 restricted transport.
Do not modify the old r30 artifacts, old five-TU adapter, ROM inputs or device.
Initialize only registration/logging in the BSD startup path. No automatic
MMIO, DMA allocation, ownership grant, storage publication or boot-argument grant.
Explicit trusted-platform calls must supply independent live access/ownership
evidence. Follow the official UniXNU UFS milestone order. Runtime hardware
results must remain separate from model tests and official kernel links.
Keep one AppsSmmuOwner implementation, the original command allowlist and
quarantine retention. No storage writes, cold initialization, UIC, PHY/ICE/TCU,
firmware-pointer calls, reset recovery or wider block-read API in this bridge.
Use tests/run.py for the actual bridge/model suite; use the parent's new host
target for official fresh-header/native/prelink verification.
