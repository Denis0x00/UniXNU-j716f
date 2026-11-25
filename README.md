# UniXNU-j716f

Official UniXNU Format 2 device repository for the **Lenovo Xiaoxin Pad Pro 2021
(`TB-J716F`, Snapdragon 870 / SM8250-AC)** targeting **iPadOS 17.7.11 (`21H461`)**.

Companion repositories:
- Core build system & drivers: [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU)
- Board UEFI firmware (r38): [Denis0x00/uefi-j716f](https://github.com/Denis0x00/uefi-j716f)

## Repository Layout (UniXNU Format 2)

```text
board.toml                     # Format 2 board manifest (kernel, firmware, payload, bootimg, userdata)
feeds.toml                     # Local SM8250 SoC feed pointer (socs/sm8250)
unixnu.lock                    # Pinned upstream Apple OSS & mu_basecore commits
porting.toml                   # Structured UniXNU porting record (14 areas)
configs/
  j716f_defconfig              # Stage 6 single-core CPU0 + DT panic region + read-only storage archive
  j716f_stage7_defconfig       # Stage 7+ RAM Data + SystemOS union + GopFramebuffer + AppleKeyStoreState
dts/j716f.dts                  # Board device tree (overlays socs/sm8250/dts/sm8250.dtsi)
patches/xnu/                   # Board XNU patches (earlylog, verbose console, storage, APFS root probe, skip-setup)
patches/unixnu-core/           # Pending upstream UniXNU core PR patches (AAPL,phandle & bootimg v2)
package/storage/               # SHNGROOT UFS window driver + volatile RAM / opt-in persistent UFS store + skip-setup plists
package/display/               # Board probes for upstream GopFramebuffer (CPU + opt-in fb-gpu=1) & AppleKeyStoreState (aks=1)
package/input/                 # Unmodified NativeHidInput (21H461 IOHIDDevice) + Goodix GT7386 I2C + PM8150 buttons
package/power/                 # SM8250-AC EPSS 3-domain CPUfreq + L3 (cpufreq=4) & PM8150B battery (j716f-battery=1)
package/gpu/                   # Adreno 650 v2.0 Phase 1..4b KMD (GMU, ArmSmmu4K, CP, TZ PAS 13 ZAP, Ring/IRQ, AGXAccelerator)
package/usb/                   # Bare-metal SM8250 DWC3 USB CDC-ACM debug terminal (j716f-usb=1) & reboot-mode control
socs/sm8250/                   # SM8250 SoC config, DTSI, SMP/Edge patches, UFS HCI/SMMU library & host tests
scripts/
  build-j716f-storage-active.py    # Builds libJ716fStorageBringup.a (r41 storage + NativeRamBlock)
  build-j716f-display-keystore.py  # Audits 21H461 display/HID/GPU ABIs & builds libNativeLegacyDisplay.late.a
  pack-j716f-gpu-addon.py          # Packs Phase 4b NGPUADD2 gpuaddon.bin + owner-trust.bundle into boot.img
  j716f-usb-debug.py               # Host USB CDC-ACM CLI (monitor, exec, probe, shell, reboot, GSC1 PNG snapshot)
  package.sh                       # Standard UniXNU release entrypoint
  ci/                              # macOS Data seed tools, GPU packer & USB wire host unit tests (see README-data-runner.md)
```

## Bring-Up Status

- **UEFI & Display (`uefi-j716f` r38)**: Verified boot menu, volume/power key
  navigation, clean rendering, Advanced Logger QR export, retained early XNU
  RAM log (`JXLG`) across resets, 80 MiB initrd ReadyToBoot CRC32 checks, and
  SEC-to-XNU UFS/SMMU RAM handoff slot (`0xB007E000`).
- **Kernel & Early BSD (Stage 5 — verified on hardware)**: Verified CPU0
  bring-up through MMU enable, `arm_init`, `PE_init_iokit`, `machine_lockdown`,
  `StartIOKitMatching`, `bsd_init`, `sysctl_mib_init`, and `IOKitBSDInit`.
- **Storage & APFS Root Mount (Stage 6 — verified on hardware)**: Consolidated
  official `./unixnu` build verified `vfs_mountroot()` mounting the real iPadOS
  17.7.11 APFS OS root (`disk3s1`, `DawnUpdate21H461.J120J121J207J208OS`) and
  `SystemOS` (`disk1s1`), reading `/sbin/launchd` (`561,856 B`, Mach-O
  `0xfeedfacf`) and `SystemVersion.plist` (`574 B`) with zero UFS writes and
  zero I/O errors (`270` commands / `477` blocks, `quar=0`, `disp4=1`).
- **Desktop, Input, 8-Core SMP & CPUfreq (Stage 7+ — SpringBoard desktop verified on hardware)**:
  `configs/j716f_stage7_defconfig` links unmodified upstream `RamBlock`,
  `GopFramebuffer` (`NativeLegacyDisplay` + `NativeGOPPresenter`),
  `AppleKeyStoreState` (`aks=1`), and `NativeHidInput` (`IOHIDDevice`, 207-slot
  21H461 ABI verified) together with J716F board drivers:
  - **Storage & Memory**: `R41UfsTransport` poll-yield/retry hardening eliminated
    post-Hello UFS quarantine (0 errors through 240+ APFS `disk5` transactions).
    Default boots remain strictly read-only on UFS with a volatile 2-level sparse
    RAM store (512 MiB cap, zero UFS writes) for `/private/var` (`data-reset=1
    data-fua=1`); opt-in persistent UFS Data storage (`j716f-data-persist=1` RO
    probe / `j716f-data-persist=2` RW direct) is guarded by a one-shot immutable
    write window strictly between `SHNGROOT + 64 MiB` and `userdata_end - 1 GiB`
    (verified under ASan/UBSan in Test 14). Contiguous XNU RAM is expanded to
    **3.5 GiB (`0xE0000000`)** inside `uefi-j716f`'s `High RAM` window.
  - **Input**: Goodix GT7386 GENI I2C touchscreen (`J716fTouchProbe.cpp`) and
    PM8150 SPMI hardware keys (`J716fButtons.c`, Power/VolUp/VolDown + lock/wake)
    verified on hardware (`status=0x0`, `errs=0`).
  - **Skip-Setup**: `j716f-skip-setup.patch` (`j716f-skip-setup=1`) seeds the 5
    official binary preference plists into `/private/var` at mount time without
    requiring macOS `hdiutil`, booting straight to the iPadOS 17.7.11 SpringBoard
    desktop with wallpaper.
  - **8-Core SMP & EPSS CPUfreq**: All 8 Kryo 585 cores (4× A55 E + 4× A77 P)
    with Edge cluster scheduling (`smp=1 armcpu-spi-ipi=1 armcpu-self-ipi=1
    armcpu-edge=1`) and SM8250-AC EPSS 3-domain CPUfreq + L3 scaling
    (`J716fCpuFreq.cpp`, `cpufreq=4`) verified on hardware (~2.3× faster early
    boot and smoother interactive desktop). `-v` is omitted from default
    `kernel_args` so XNU stays in `kPEGraphicsMode` without scrolling text over
    SpringBoard.
  - **USB Debug CDC-ACM & PM8150B Battery (verified on hardware)**:
    Opt-in bare-metal SM8250 DWC3 USB CDC-ACM diagnostic terminal (`j716f-usb=1`,
    VID `0x1209:0x0001`, driverless Windows `usbser.sys` / Linux `cdc_acm`)
    paired with SMP-safe all-8-CPU `JXLG` + 256 KiB live kernel log ring
    (`PE_j716f_klog_read`), live EPSS MHz inspection (`cpufreq`), hands-free
    `reboot bootloader` via PM8150 SPMI + IMEM, GSC1 PNG framebuffer snapshots
    (`scripts/j716f-usb-debug.py`), and read-only SPMI PM8150B fuel gauge / SMB5
    charger `IOPMPowerSource` provider (`j716f-battery=1`).
  - **Adreno 650 GPU (Phase 1–4a verified on hardware, Phase 4b ready)**:
    Hardware-verified GMU cold-start + HFI + GX 670 MHz power-on, KGSL SMMU
    `ArmSmmu4K` Bank 1/0 contexts, A650 SQE/CP Ring 0, retail TrustZone PAS 13
    ZAP authentication (`trust=1->0`), GPU SPI 300 IRQ, per-process TTBR0
    switching (`CP_SMMU_TABLE_UPDATE`), and 32/32 `NativeAdrenoABI` v2 user-client
    rounds with zero SMMU faults. Phase 4b userspace `gpuaddon.bin`
    (`NativeAdrenoMetal.bundle` + Mesa Turnip + `metal2vulkan`) builds via
    `.github/workflows/j716f-gpu-addon.yml`, packs via
    `scripts/pack-j716f-gpu-addon.py`, and supports opt-in `fb-gpu=1`
    `NativeGOPPresenter` handoff.

See [porting.toml](porting.toml) for the full structured per-area record.

## Building with Official `./unixnu`

Place `UniXNU`, `UniXNU-j716f`, and `uefi-j716f` side-by-side on a Linux
(Ubuntu 24.04 x86_64) host with the UniXNU toolchain installed:

```bash
git clone https://github.com/UniXNU/UniXNU.git
git clone https://github.com/Denis0x00/UniXNU-j716f.git
git clone https://github.com/Denis0x00/uefi-j716f.git

cd UniXNU
# Apply the two pending upstream UniXNU core patches required for TB-J716F
# real-hardware ADT (AAPL,phandle) and Android boot image header v2:
git apply ../UniXNU-j716f/patches/unixnu-core/*.patch

# Stage 6 (APFS root mount baseline)
./unixnu defconfig j716f_defconfig -D ../UniXNU-j716f --os 17
./unixnu status --json --os 17
./unixnu tree --os 17
./unixnu build package --os 17
./unixnu build firmware --os 17 --uefi ../uefi-j716f
./unixnu build kernel --os 17

# Stage 7+ (RAM Data + SystemOS union + GopFramebuffer + AppleKeyStoreState)
./unixnu defconfig j716f_stage7_defconfig -D ../UniXNU-j716f
./unixnu build kernel
```

To assemble `payload.img` and `boot.img`, supply the user-owned inputs listed in
[proprietary-files.txt](proprietary-files.txt) (normalized stock DTB and
`UNIXNU_UNIQUE_CHIP_ID`):

```bash
export UNIXNU_UNIQUE_CHIP_ID="<your-16-hex-chip-id>"
./unixnu build payload --os 17
./unixnu bootimg --os 17 --uefi ../uefi-j716f --dtb /path/to/j716f-stock-base00-overlay01-normalized.dtb
```

For generating a fresh iPadOS Data seed on macOS via GitHub Actions, see
[README-data-runner.md](README-data-runner.md).

## Running Host Tests

All C++ storage/SMMU unit tests and Python seed-tool tests run locally and in
GitHub Actions (`j716f-ci.yml`) with AddressSanitizer and UndefinedBehaviorSanitizer:

```bash
# Python CI seed helper tests
PYTHONPATH=scripts/ci python3 -m unittest discover -s scripts/ci -p 'test_*.py' -v
```

## License

Original source files carry SPDX license headers; repository license is in
[LICENSE](LICENSE). Proprietary vendor firmware, IPSW contents, and device
identities are excluded from this repository.
