# UniXNU-j716f

Lenovo Xiaoxin Pad Pro 2021 (`TB-J716F`, SM8250-AC / Kona) device support for UniXNU (`iPadOS 17.7.11` Build `21H461`).

Firmware repository: [uefi-j716f](https://github.com/Denis0x00/uefi-j716f)  
Specification: [UniXNU Porting Guide](https://unixnu.com/zh/porting/)

## Device Specifications

- **Device**: Lenovo Xiaoxin Pad Pro 2021 (`TB-J716F` / `j716f`)
- **SoC**: Qualcomm SM8250-AC (Snapdragon 870)
- **CPU**: 8-core Kryo 585 (1x Cortex-A77 @ 3.2 GHz + 3x Cortex-A77 @ 2.42 GHz + 4x Cortex-A55 @ 1.8 GHz)
- **GPU**: Qualcomm Adreno 650 (`06050002`, 670 MHz top OPP, GMU + SQE + TZ PAS 13 Zap shader)
- **Display**: 11.5" 2560x1600 90 Hz OLED (`0x9c000000` UEFI GOP scanout, scale 2x)
- **Touch**: Goodix `GT7386` 10-point multi-touch over QUPv3 SE11 I2C (`0x00990000`, addr `0x5d`)
- **DRAM**: 6 GiB LPDDR4X (`0x80000000` - `0x200000000`)
- **Storage**: UFS 3.1 host controller at `0x01D84000` (APFS OS + SystemOS volumes)
- **Target OS**: iPadOS 17.7.11 (Build `21H461`, device class `j120ap`)

## Hardware Support Status

| Subsystem | Status | Notes |
| --- | --- | --- |
| Boot & XNU Kernel | Device-verified | UEFI (`uefi-j716f`) -> DarwinBoot -> XNU ARM64 VM + IOKit -> iPadOS 17.7.11 desktop |
| CPU & SMP | Device-verified | All 8 Kryo 585 cores online (`sheng-smp=1`) + AMP big.LITTLE E/P edge scheduler (`armcpu-edge=1`) + dynamic 3-cluster EPSS `cpufreq` (`pipa-cpufreq=4`, touch boost + `300 / 691 / 844 MHz` idle floors) |
| GPU Acceleration | Device-verified | Native `Adreno650` KMD (`pipa-gpu=12`) + GMU HFI dynamic DCVS (`pipa-gpu-dcvs=1`, `305..670 MHz`) + `NativeAdrenoMetal` / Turnip (`sysmem,nocb`, `MESA_SHADER_CACHE_DIR`, `54 us/draw`) |
| Display & Brightness | Device-verified | 2560x1600 BGRX8888 GOP framebuffer + GPU hardware scanout blitter (`sheng-fb-gpu=1`) + `AppleARMBacklight` (`sheng-backlight=1`) & `copy_rows` compute brightness slider + lock/wake power re-arm |
| Touch & Buttons | Device-verified | Goodix `GT7386` multi-touch (`j716f-touch=1`, rolling `0..255` tracking-ID slot mapper) + PM8150/PM8150B PON keys (`pipa-buttons=1`) |
| Storage & System Apps | Device-verified | SM8250 native UFS (`ShengUfs`) mounting iPadOS 17.7.11 APFS container + persistent Data volume + `owner-staged-apps=1` built-in system apps + `appsync=1` dynamic user-code trust & IPA installer |
| Battery Gauge | Device-verified | PM8150B FG / SMB5 SPMI battery gauge (`NativeBattery`, `IOPMPowerSource` SoC, open-circuit voltage fallback, current, charging state) |
| USB Diagnostics & Native Apple USB | Device-verified | DWC3 USB0 (`0x0A600000`) CDC-ACM console (`1209:0001`, `GSC1` screen capture, `reboot.fastboot`) + runtime switch (`usb.xdci` / `usb.xdci.30`) to native `QcomDwc3` / `AppleUSBXDCI` (`05ac:12ab`, `AppleUSBDeviceMux`, `lockdownd` pairing) |
| Wi-Fi & Bluetooth | Device-verified | QCA6390 PCIe0 + MHI AMSS Mission Mode (`Ath12kWifi`, `sheng-wifi=4`, `en0` + `wifid` + `unixnu-wpa_supplicant`) and QUPv3_0 SE6 GENI UART @ 3.2 Mbps (`QcaBluetooth`, `sheng-bluetooth=1`, `BlueTool` + `bluetoothd` + live BLE advertising RX) |
| Audio | Planned | WCD938x / WSA881x ADSP Q6AFE audio planned |

## Repository Layout

```text
UniXNU-j716f/
├── board.toml                           # Format 2 board specification
├── porting.toml                         # Porting progress and hardware verification record
├── feeds.toml                           # Shared dependencies and upstream commits
├── proprietary-files.txt                # Vendor firmware SHA-256 manifest (ZUI 15.0.485)
├── configs/
│   └── j716f_defconfig                  # Kernel build configuration
├── dts/
│   └── j716f.dts                        # Device Tree Source (Format 2)
├── drivers/
│   ├── Adreno650/                       # SM8250 Adreno 650 kernel driver + staged AGXAccelerator
│   ├── AdrenoMetal/                     # Metal-to-Turnip userspace bridge & plugin sources
│   ├── ArmCpu/                          # ARM64 SMP & interrupt controller driver
│   ├── GopFramebuffer/                  # Display framebuffer & GPU scanout hand-off driver
│   ├── ShengHidInput/                   # Multi-touch & physical button HID injection driver
│   └── ShengUfs/                        # SM8250 UFS 3.1 host controller driver
├── kits/
│   └── ArmSmmu4K/                       # Qualcomm ARM SMMU v2 4K context bank kit
├── package/                             # Board probes (GPU, GMU, Touch, Battery, Buttons, USB CDC-ACM)
├── patches/
│   └── xnu/                             # Upstream XNU kernel patch series
└── scripts/
    ├── build-adreno650.py               # Archive builder for libAdreno650.a
    ├── build-gop-legacy-display.py      # Archive builder for GopFramebuffer + GPU presenter
    ├── build-j716f-gpu-addon.py         # GPU userspace bundle & firmware addon packager
    ├── check-j716f.py                   # Platform preflight validator & DTB stager
    └── package.sh                       # Standard UniXNU release packaging entrypoint
```

## Building & Verification

In the root of the UniXNU tree:

```sh
# 1. Select board defconfig
./unixnu defconfig j716f_defconfig -D ../UniXNU-j716f

# 2. Inspect dependency status
./unixnu status --json -D ../UniXNU-j716f

# 3. Verify porting record
python3 scripts/porting-record.py check --device ../UniXNU-j716f

# 4. Standard release build
./scripts/package.sh
```

## Flashing & USB Diagnostics

Always flash **Slot B (`boot_b`)** via `fastbootd` so Slot A remains untouched:

```sh
fastboot reboot fastboot
fastboot set_active b
fastboot flash boot_b boot.img
fastboot reboot
```

Once booted with `j716f-usb=1`, the tablet enumerates as a USB CDC-ACM serial port (`1209:0001`, serial `J716FDBG01`) supporting live telemetry (`status`, `display`, `gpu.status`, `battery.status`), `GSC1` framebuffer capture (`screen.snapshot`), warm reboot directly back to Fastboot (`reboot.fastboot`), and runtime handoff (`usb.xdci.30` or persistent `usb.xdci`) to the native `QcomDwc3` / `AppleUSBXDCI` stack (`05ac:12ab`, `AppleUSBDeviceMux` + `lockdownd` host pairing).

## License

BSD-2-Clause-Patent, see [LICENSE](LICENSE).
