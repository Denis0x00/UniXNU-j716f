# Status and provenance

This is a source snapshot of the current rebuild, not a continuation of older
claims that the entire iPadOS desktop or all peripherals have been qualified.
Historical target README/manifests retain the status at their creation time;
this document describes the overall release boundary.

| Area | Current evidence | Still required |
| --- | --- | --- |
| UEFI UI | Menu, power/volume selection, QR export and redraw tested | Further firmware changes need regression testing |
| Early XNU | CPU0, VM, IOKit core/configuration/matching, BSD autoconf and pre-root reached | Root lookup, mount and userspace |
| r30 storage bridge | Registration runs after successful IOKitBSDInit | Power/clock observations, HCI readiness, SMMU/DMA ownership, transport and block publication |
| r31 GCC experiment | Official kernel/prelink and Linux/Windows image verification passed, including seven rejection cases on each host; only boot_b flashed; user reports approximately six-second return to the r31 menu | Complete retained Power log and register values; MMIO success, UFS block I/O and root mounting remain unproven |
| UFS command and slot core | Offline command whitelist, state/ownership interfaces and board adapters | Live resource/ownership providers and qualified UFS I/O |
| Goodix/GENI | Stock-based parsing and bounded FIFO/transaction work, synthetic tests | Live clocks/pins/reset/IRQ/engine grant, transport, orientation and HID |
| Display | Resource reservation/handoff and provider code, host tests | Actual reservation proof, framebuffer display publication and graphics |
| OS/System/Data inputs | Separate host preparation work | Storage integration and successful tablet mount; Data-only CI is not a device result |

Board register/binding research refers to **TB_J716F_ZUI_15.0.485**. The firmware
and hardware evidence remain local. Source provenance records include hashes
and locations for review but do not distribute those binaries or private logs.

The source design follows official UniXNU device, SoC, component, patch-series,
official `build kernel`, prelink, firmware and payload boundaries. Diagnostic
return checkpoints, board RAM logging, the ADT correction, CPU0 assembly repair
and restricted adapters are local bring-up additions. They do not establish a
completed official port or make sheng's SM8550 drivers interchangeable with SM8250.

`pins/source-snapshot.json` binds each selected workspace source byte. Current
component manifests, archive recipes, target locks and original licenses are
preserved. The CPU0 override is based on upstream ArmCpu
`d90e457b8392623853ec85c860feef546c767621` with the six recorded assembly-template
separator repairs. The local SoC snapshot records its original device-repository
base `bf2d0f32f706adbe414551e452f1426ed7c4bd19` and subsequent board changes.
`ShengUfs-sm8250` retains its own upstream license and source notices.

Reference work can be found at [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng)
and [ShengUfs](https://github.com/UniXNU/ShengUfs). Their validated board behavior
must not be copied into this board's status. This snapshot records source
dependencies; it does not auto-enable all upstream components.
