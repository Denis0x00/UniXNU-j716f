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
package/storage/               # Board read-only SHNGROOT UFS window driver + volatile RAM store + skip-setup plists
package/display/               # Board probes for upstream GopFramebuffer and AppleKeyStoreState (aks=1)
package/input/                 # Unmodified NativeHidInput (21H461 IOHIDDevice) + Goodix GT7386 I2C + PM8150 buttons
package/power/                 # SM8250-AC EPSS 3-domain CPUfreq + L3 scaling driver (cpufreq=4)
socs/sm8250/                   # SM8250 SoC config, DTSI, SMP/Edge patches, UFS HCI/SMMU library & host tests
scripts/
  build-j716f-storage-active.py    # Builds libJ716fStorageBringup.a (r41 storage + NativeRamBlock)
  build-j716f-display-keystore.py  # Audits 21H461 display/HID ABIs & builds libNativeLegacyDisplay.late.a
  package.sh                       # Standard UniXNU release entrypoint
  ci/                              # macOS first-install Data seed tools (see README-data-runner.md)
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
  - **Storage**: `R41UfsTransport` poll-yield/retry hardening eliminated post-Hello
    UFS quarantine (0 errors through 240+ APFS `disk5` transactions), backed by
    a volatile 2-level sparse RAM store (512 MiB cap, zero UFS writes) for
    read-write `/private/var` (`data-reset=1 data-fua=1`).
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
