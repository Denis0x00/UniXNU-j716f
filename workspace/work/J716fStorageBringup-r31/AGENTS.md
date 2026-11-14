# J716F r31 GCC Power experiment

This independent component retains the frozen r30 bridge and adds a fixed,
one-shot GCC read experiment. Never edit r30, ROM inputs or device artifacts.
Read README.md before changing this component. The main entry is
`j716f_storage_power_probe()` after successful IOKitBSDInit and registration.
Only the fixed 11 GCC status addresses in page 0x177000 may be read. No write,
HCI/SMMU access, DMA, firmware call, ownership grant or automatic next stage.
Do not manufacture DiagnosticReadAccessEstablished or reuse the old observe
contract for first-access experimentation. Ordinary MMIO may abort or stall;
keep fault-contained=0 and before/after logs. Host tests are not hardware proof.
RX/TX BRANCH_HALT_DELAY entries must remain raw, without a lane-ready claim.
Freeze source before the parent's official compile; later edits require new pins.
