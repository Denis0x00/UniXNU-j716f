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
| r31 GCC experiment | Official kernel/prelink and Linux/Windows image verification passed, including seven rejection cases on each host; only boot_b flashed; retained summary reports captured, 11/11 reads, valid=0x7ff, map/unmap=1; ten raw values recoverable | AHB raw result is missing with corrupt retained sequence 72; HCI/SMMU access, DMA, UFS block I/O and root mounting remain unproven |
| UFS command and slot core | Offline command whitelist, state/ownership interfaces and board adapters | Live resource/ownership providers and qualified UFS I/O |
| Goodix/GENI | Stock-based parsing and bounded FIFO/transaction work, synthetic tests | Live clocks/pins/reset/IRQ/engine grant, transport, orientation and HID |
| Display | Resource reservation/handoff and provider code, host tests | Actual reservation proof, framebuffer display publication and graphics |
| OS/System/Data inputs | Separate host preparation work; Data run 43597879267 succeeded, its seed passed Windows/VM byte/provenance checks and is preserved | Kernel Data-volume UUID binding, payload/storage integration and successful tablet mount; Data-only CI is not a device result |

Board register/binding research refers to **TB_J716F_ZUI_15.0.485**. The firmware
and hardware evidence remain local. Source provenance records include hashes
and locations for review but do not distribute those binaries or private logs.

The r31 retained-log importer reports `gaps=3 corrupt=3`, with sequences 13,
51 and 72 absent. The missing AHB raw value falls in sequence 72. These are
retained-record integrity failures reported before QR export, not a diagnosis
of missing QR pages. The surviving final Power summary reports all eleven
reads completed, `valid=0x7ff`, one map and one unmap; it is separate evidence
from the ten recoverable raw results. The AHB hint is present but cannot supply
the missing raw word. No claim is made that all eleven raw values were verified.
GDSC power-on, reset-deasserted and AXI clock hints support a next bounded HCI
read experiment; they do not establish HCI readability, link readiness, SMMU
ownership, DMA handoff or storage I/O. The raw r31 log is not published here.

The macOS [Data run 43597879267](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/43597879267)
completed successfully after a failed first run. The seed and its provenance
are retained locally and in the VM. Windows/VM checks verified artifact bytes
and recorded provenance; APFS execution/volume validation occurred on the macOS
runner. Preserve this exact seed for later builds instead of creating a new
Data identity. Its UUID is not yet bound into a successor kernel, the payload
has not integrated it, and no tablet mount or persistent Data storage is
qualified.

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
