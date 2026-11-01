# UniXNU-j716f

Lenovo Xiaoxin Pad Pro 2021 (TB-J716F, SM8250-AC / Kona) device support for UniXNU.

Firmware repository: [uefi-j716f](https://github.com/Denis0x00/uefi-j716f)  
Specification: [UniXNU Porting Guide](https://unixnu.com/zh/porting/)

## Device Specifications

- **Device**: Lenovo Xiaoxin Pad Pro 2021 (TB-J716F / j716f)
- **SoC**: Qualcomm SM8250-AC (Snapdragon 870)
- **CPU**: 8-core Kryo 585 (1x Cortex-A77 @ 3.2GHz + 3x Cortex-A77 @ 2.42GHz + 4x Cortex-A55 @ 1.8GHz)
- **Timer**: ARM Generic Architectural Timer (19.2 MHz counter frequency, PPI 11 / INTID 27)
- **GIC**: ARM GICv3 (Distributor `0x17a00000`, Redistributor `0x17a60000`, stride `0x20000`)
- **DRAM**: 6 GiB physical DRAM bank (`0xc0000000` - `0x200000000`)
- **Storage**: UFS 3.1 host controller at `0x01D84000`, Userdata partition pre-flashed with iPadOS 17.7.11 APFS container
- **Target OS**: iPadOS 17.7.11 (Build 21H461, device class `j120ap`)

## Repository Layout

```text
UniXNU-j716f/
├── board.toml                           # Format 2 board specification
├── porting.toml                         # Porting progress and verification record
├── feeds.toml                           # Shared dependencies and upstream commits
├── unixnu.lock                          # Locked source revisions
├── configs/
│   └── j716f_defconfig                  # Kernel build configuration
├── dts/
│   ├── j716f.dts                        # Device Tree Source
│   └── kernel.macho.adt                 # Qualified Apple-format Device Tree
├── drivers/
│   └── J716FUfs/                        # SM8250 native UFS storage driver
├── patches/
│   └── xnu/                             # Upstream XNU kernel patch series
└── scripts/
    ├── check-j716f.py                   # Platform preflight validator
    ├── make-j716f-adt.py                # Qualified ADT generator
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

## Flashing & Boot

All experimental flashes must target **Slot B (`boot_b`)** to preserve the stock system and partitions in Slot A:

```sh
fastboot flash boot_b boot-j716f.img
fastboot reboot
```

## License

BSD-2-Clause-Patent, see [LICENSE](LICENSE).
