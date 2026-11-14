# Reconstructing the source build

## Scope and layout

`scripts/export_workspace.py` exports only recorded text sources. Run original
commands from the exported root, where `work/`, `analysis/`, and `UniXNU/` are
siblings. The helper optionally fetches the two upstream cores and their pinned
submodules; it does not fetch Apple firmware, install a toolchain, build or flash.

| Consumer | Preserved dependency | Preparation still required |
| --- | --- | --- |
| r31 kernel host target | `UniXNU/`, CPU0 source, `J716fStorageBringup-r31` | Linux cross toolchain, original kernelcache, official source/header fetch |
| r30 registration host target | same kernel core, `J716fStorageBringup-r30` | its own build output; no reuse of r31 objects |
| r31 image target | `work/UniXNU-core-adt-phandle-r23` | reviewed new kernel and image inputs |
| early-log firmware wrapper | `work/uefi-j716f-early-log` | companion firmware source with early-log/menu fixes, pinned edk2/Mu dependencies |
| image assembly | `work/uefi-j716f`, built FD and BootShim | source-built firmware receipt and freshly reviewed firmware expectation |
| legacy/offline host targets | their own local feeds/archives | individual host build and hardware qualifications |

Do not simply rename a target. The r31 archive checks its directory name, the
resolved local component and frozen input hashes. `--local-module` and the
exact `feeds.toml` entries are part of the current build contract.

## Core dependencies

`pins/core.json` records two **different** current bases:

- kernel core: `7b730b231746667d8b7f54746c2c1ce45852611c`;
- payload/core phandle branch: `7d85ede80092050f4d87f72ac7229bb605ce07fb`.

Each has a separate patch and added tests under `overlays/`. The payload overlay
includes the source fix producing little-endian `AAPL,phandle`, validating aliases
and exported interrupt-parent references. Menu ownership and boot-image layout
changes are also preserved. Do not merge the two cores without parity checks.
The recorded submodule revisions and each target's `unixnu.lock` preserve the
upstream dependencies. Source exports contain no nested Git metadata.

## Inputs deliberately absent

The following dependency inventory is separate from the tablet's readiness:

| Build layer | Dependencies |
| --- | --- |
| Source/export | Python 3.11+, Git, network for pinned upstream checkouts |
| Linux kernel toolchain | glibc Linux; official patched XNU Clang, cctools-port ld64/libtool/nm/lipo, bootstrap_cmds MIG, iig-port, xcrun/plutil/codesign shims, lzfse; installed via the pinned core's `scripts/linux/setup-toolchain.py` |
| Linux installer prerequisites | distribution Clang/LLVM development files, libclang, libbsd, UUID, OpenSSL, zlib; Git/make, cmake/ninja and the official installer's printed package list |
| Source ABI | target `unixnu.lock`: xnu, AvailabilityVersions, IOStorageFamily, corecrypto-2023, libdispatch, libplatform, libpthread; official DriverKit/crypto/trustcache headers required by the selected ABI |
| Kernel components | pinned ArmDTPlatform, SoftwareAES, XnuPrelink and DarwinBoot plus the local CPU0 override; the nine selected archive recipes are ordered in the target board file |
| r31 private archive | four J716fStorageBringup/Power TUs plus five local SM8250 UFS/ownership TUs; no precompiled archive is supplied |
| Firmware/package | the appropriate companion UEFI variant, edk2 `cfb93724a06ff86ff5ba971413368a7809ee42a5`, Mu/Mu_OEM_Sample source, LLVM/lld and the companion's locked BaseTools/package dependencies |
| Payload/boot image | mtools, dtc, selected original kernelcache/trust cache, source-built FD/EFI/BootShim, user-supplied normalized board DTB, private platform seed, independently reviewed fresh binary contracts |
| Data-only CI | actual macOS 26, Python 3.12, Git, hdiutil/diskutil and sudo; fixed official core/DarwinBoot and original Apple OS/trust inputs fetched by the workflow; no kernel compiler or device access |

From the exported kernel core, inspect and prepare the supported Linux tools:

```sh
cd UniXNU
python3 scripts/linux/setup-toolchain.py --print-packages
sudo python3 scripts/linux/setup-toolchain.py
python3 scripts/linux/setup-toolchain.py --check
```

The installer needs root because it creates `/opt/unixnu-linux` and tool shims.
These commands are documented prerequisites, not actions performed by the
exporter. Do not replace the patched XNU Clang with an arbitrary system clang.
The authoritative list/version pins are the checked-out setup script and
[official requirements](https://github.com/UniXNU/UniXNU/blob/7b730b231746667d8b7f54746c2c1ce45852611c/docs/guide/requirements.md).

1. `/opt/unixnu-linux` cross toolchain and its SDK/build dependencies. The current
   `host.py` wrappers explicitly use that Linux location; they are not macOS CI
   wrappers. A Darwin port must use official Xcode tooling and new binary review.
2. Original iPadOS 17.7.11/21H461 kernelcache, SHA256
   `f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7`.
   Place the decompressed input at
   `UniXNU/out/ipsw/21H461/kernelcache.decompressed` using the official extraction
   workflow. Original trust cache and OS/SystemOS inputs are also private build
   inputs, not source files in this repository.
3. TB_J716F_ZUI_15.0.485 boot/DTBO and the normalized DTB0+DTBO1. Source DTS and
   extraction/audit code are included where present; proprietary DTB output is
   excluded. Supply the same verified local input expected by the image target.
4. A private platform identity. Image packaging refers to
   `private-inputs/j716f/platform-seed.hex`; no value is included. The portable
   future payload CI should use a masked secret, never a checked-in seed.
5. Original build receipts, baseline payloads and runtime logs. Legacy
   `workspace/analysis/*/run_host_storage.py` and image acceptance scripts retain
   their frozen historical evidence paths. These scripts cannot be treated as
   an immediately runnable fresh-clone pipeline without regenerating the
   referenced receipts and independently reviewing new machine expectations.

The original Linux preparation seeded Apple header/source caches from a local
offline cache. For a new environment, use the official source fetch/prepare
route. Do not copy cached object files or pretend missing source metadata exists.
No fresh-clone full kernel/image build has yet qualified this export.

## What can be reproduced immediately

`python3 scripts/export_workspace.py --check` validates the selected source
snapshot. `--destination NEW_PATH` verifies and exports it without network.
`--fetch-core` additionally materializes official cores at the recorded pins.
Pure parser/transaction tests use synthetic inputs and need only their stated
host compiler/Python prerequisites. The r31 component's `freeze.py --check`
checks that the byte-identical r30 bridge and nine-TU source contract remain.

Packaging a new executable requires a fresh source compile, native/prelink
verification, actual symbol/address discovery, independent expectation review,
and image verification. Firmware and kernel addresses or hashes from old builds
are not generic portable acceptance values. Device deployment remains a separate,
user-coordinated `boot_b` step; this repository does not auto-flash.
