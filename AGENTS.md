# J716F device repository guide

This repository owns the TB-J716F board description, SoC transition snapshot,
component selection, firmware references and packaging policy. It does not
contain vendor binaries, IPSW members, trust caches, userdata or boot images.

## Read first

Read the UniXNU [porting guide](https://unixnu.com/zh/porting/),
[migration guide](https://unixnu.com/zh/porting/migration), [device repository](https://unixnu.com/zh/porting/device-repository),
[device tree](https://unixnu.com/zh/porting/device-tree), [UEFI integration](https://unixnu.com/zh/porting/uefi-firmware),
and [bring-up order](https://unixnu.com/zh/porting/bring-up) before changing this port.

## Work boundaries

- Keep physical addresses, CPU affinity, GIC ranges, memory reservations and
  firmware paths tied to the committed research evidence.
- Keep the local `socs/sm8250` source marked as a transition snapshot until a
  reviewed `UniXNU-sm8250-common` repository and fixed commit exist.
- Add reusable drivers to their canonical component repository; board wiring and
  package policy belong here.
- Do not add or publish Apple/Lenovo binary inputs, raw DTBs, payload images,
  photos, logs, seeds or VM credentials.
- Current diagnostic hardware testing remains `boot_b` only; do not write `boot_a`, vbmeta or
  userdata. TzDxe is outside the current scope.

## Current checkpoint

See `docs/V22_POST_MMU_CHECKPOINT.md` and `research/v22-checkpoint-preparation.json`.
V21 video confirms its own green/blue/magenta entry bands and a restart through
the Lenovo logo; startup PMIC changes from `0x02000001` to `0x02000041`. The reset
cause and sustained MMU-on execution remain unverified. V22A (1s) and V22B (3s)
passed host builds and artifact audits but have not been flashed. Keep version
evidence distinct. `board.toml` retains the historical V17 recipe; V22 uses
`scripts/build-v22-payload.py` and the independent firmware V22 DSC/FDF.

## Commands

From the latest UniXNU checkout:

```sh
./unixnu defconfig j716f_source_defconfig -D ../UniXNU-j716f
./unixnu status --json -D ../UniXNU-j716f
./unixnu tree -D ../UniXNU-j716f
python3 scripts/porting-record.py check --device ../UniXNU-j716f
```

Use a local `unixnu.local.toml` for private paths. Build the kernel, package,
firmware, payload and boot image in dependency order only after the required
IPSW, trust-cache, Data seed and firmware DTB inputs are available.

## Porting record

`porting.toml` is authoritative. Update the affected area's findings or
evidence whenever board code, configuration, firmware integration or packaging
changes. Separate host verification from device observations; the current
record does not claim a bootable iPadOS port.
