#!/usr/bin/env python3
"""Generate the qualified J716F hardware Apple-format Device Tree (ADT).

Ports the verified SM8250 architecture from UniXNU-pipa and UniXNU-sheng,
tailored to Lenovo Xiaoxin Pad Pro 2021 (TB-J716F) hardware parameters:
  - GICv3 Distributor: 0x17a00000 (0x10000 bytes)
  - GICv3 Redistributor: 0x17a60000 (0x100000 bytes, stride 0x20000)
  - Architectural Timer: 19.2 MHz counter (PPI11 / INTID 27)
  - Single/Multi-core CPU: Kryo 585 Silver (A55) & Gold (A77)
  - DRAM: base 0x80000000 or 0xc0000000, configurable owned DarwinBoot window
  - CHRP volatile NVRAM proxy (8192 bytes, signature 0x70)
  - iPad7,1 platform identity for AppleImage4 and MobileGestalt
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import argparse
import hashlib
import os
from pathlib import Path
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]

# J716F / SM8250 Platform Constants
DRAM_BASE = 0x80000000
ARMIO_BASE = 0x17000000         # APSS peripheral block base
ARMIO_SIZE = 0x08000000         # 128 MiB peripheral aperture
GIC_DIST = 0x17A00000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x17A60000
GIC_REDIST_SIZE = 0x100000
GIC_REDIST_STRIDE = 0x20000
TIMEBASE_HZ = 19200000
CHIP_ID = 0x8011                # iPad7,1 / J120 target chip-id
LITTLE_CORES = 4


def node(name, properties=(), children=()):
    props = [('name', name.encode() + b'\0'), *properties]
    result = struct.pack('<II', len(props), len(children))
    for key, data in props:
        result += struct.pack('<32sI', key.encode(), len(data))
        result += data + bytes(-len(data) % 4)
    return result + b''.join(children)


def empty_nvram_proxy():
    """Construct volatile CHRP NVRAM proxy bank for IODTNVRAM."""
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size // 16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255) + (checksum >> 8)
        raw[1] = checksum
        return raw

    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16 + 2048)
    free_at = 32 + 16 + 2048
    image[free_at:free_at + 16] = header(b'free', len(image) - free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [
        ('nvram-bank-size', struct.pack('<I', len(image))),
        ('nvram-bank-count', struct.pack('<I', 1)),
        ('nvram-current-bank', struct.pack('<I', 0)),
        ('nvram-proxy-data', bytes(image))
    ]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'dts/kernel.macho.adt')
    parser.add_argument('--dram-size', type=lambda value: int(value, 0), default=0x180000000,
                        help='Actual observed DRAM size (default 6 GiB 0x180000000)')
    parser.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=0xE0000000,
                        help='Explicit DarwinBoot contiguous window request, default 3.5 GiB (0xE0000000)')
    parser.add_argument('--root-match-volume-uuid', default=None,
                        help='/chosen/root-matching UUID')
    parser.add_argument('--unique-chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--chip-id', type=lambda value: int(value, 16), default=None)
    parser.add_argument('--ipad-identity', action='store_true', default=True)
    parser.add_argument('--cpu-regs', default='0',
                        help='Comma-separated CPU MPIDR affinity values; default single boot core 0')
    parser.add_argument('--psci-method', choices=['smc', 'hvc'], default='smc')
    parser.add_argument('--packed-data-ramdisk', action='store_true', help='Use PackedRAMDisk placeholder')
    parser.add_argument('--data-ramdisk', action='store_true', help='Use RAMDisk placeholder')
    parser.add_argument('--trust-cache', action='store_true', help='TrustCache placeholder')
    parser.add_argument('--gpu-addon', action='store_true', help='GpuAddon placeholder')
    args = parser.parse_args()

    unique_chip_id = args.unique_chip_id
    if unique_chip_id is None:
        unique_chip_id = int.from_bytes(os.urandom(8), 'little') | 1
    chip_id = args.chip_id if args.chip_id is not None else CHIP_ID

    try:
        cpu_regs = [int(v, 0) for v in args.cpu_regs.split(',')]
    except ValueError:
        cpu_regs = [0]

    cpus = [
        node(f'cpu{idx}', [
            ('state', b'running\0' if idx == 0 else b'waiting\0'),
            ('reg', struct.pack('<I', reg)),
            ('device_type', b'cpu\0'),
            ('compatible', b'arm,cortex-a55\0' if (reg >> 8) < LITTLE_CORES else b'arm,cortex-a77\0'),
            ('cluster-type', b'E\0' if (reg >> 8) < LITTLE_CORES else b'P\0'),
            ('timebase-frequency', struct.pack('<I', TIMEBASE_HZ))
        ])
        for idx, reg in enumerate(cpu_regs)
    ]

    mem_map_entries = []
    if args.data_ramdisk or args.packed_data_ramdisk:
        mem_map_entries.append(("PackedRAMDisk" if args.packed_data_ramdisk else "RAMDisk", bytes(16)))
    if args.trust_cache:
        mem_map_entries.append(('TrustCache', bytes(16)))
    if args.gpu_addon:
        mem_map_entries.append(('GpuAddon', bytes(16)))

    chosen_props = [
        ('random-seed', os.urandom(64)),
        ('unique-chip-id', struct.pack('<Q', unique_chip_id)),
        ('chip-id', struct.pack('<I', chip_id)),
        ('osenvironment', b'main\0'),
        ('certificate-production-status', struct.pack('<I', 0)),
        ('effective-production-status-ap', struct.pack('<I', 0)),
        ('certificate-security-mode', struct.pack('<I', 1)),
        ('effective-security-mode-sep', struct.pack('<I', 1)),
        ('darwinboot-memory-size', struct.pack('<Q', args.owned_memory_size)),
        ('dram-base', struct.pack('<Q', DRAM_BASE)),
        ('dram-size', struct.pack('<Q', args.dram_size)),
        ('crypto-hash-method', b'sha2-384\0'),
        ('amfi-only-platform-code', struct.pack('<I', 1)),
        ('text-fixups', b'dyld_shared_cache_arm64.15,421e8bc,28ad1bb0008544f9,c0035fd6008544f9\0'
                        b'dyld_shared_cache_arm64.13,1b05bec,44008052,24008052\0'
                        b'dyld_shared_cache_arm64.13,1b05b08,44008052,24008052\0'
                        b'BTAudioHALPlugin,38bec,ffc300d1f44f01a9,000080d2c0035fd6\0'),
    ]

    if args.root_match_volume_uuid:
        chosen_props.append(('root-matching', (
            '<dict><key>IOProviderClass</key><string>IOMedia</string>'
            '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
            args.root_match_volume_uuid.upper() + '</string></dict></dict>\0'
        ).encode()))

    if args.data_ramdisk or args.packed_data_ramdisk:
        chosen_props.append(('ephemeral-storage', struct.pack('<I', 1)))

    chosen_props.extend(empty_nvram_proxy())

    tree = node('device-tree', [
        ('model', b'iPad7,1\0'),
        ('target-type', b'J120\0'),
        ('compatible', b'lenovo,j716f\0qcom,sm8250\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', chosen_props, [
            node('memory-map', mem_map_entries),
            node('manifest-properties')
        ]),
        node('cpus', children=cpus),
        *([node('psci', [('compatible', b'arm,psci-1.0\0'), ('method', args.psci_method.encode() + b'\0')])] if len(cpus) > 1 else []),
        node('defaults', [('sleep-disabled', struct.pack('<I', 1))]),
        node('options', []),
        node('buttons', [
            ('compatible', b'buttons\0'),
            ('device_type', b'buttons\0'),
            ('button-names', b'hold\0voldown\0volup\0'),
            ('home-button-type', struct.pack('<I', 2))
        ]),
        node('product', [
            ('product-name', b'iPad Pro (12.9-inch) (2nd generation)\0'),
            ('model', b'lenovo,j716f\0'),
            ('product-description', b'iPad Pro (12.9-inch) (2nd generation)\0'),
            ('artwork-device-idiom', b'pad\0'),
            ('artwork-device-subtype', struct.pack('<I', 2732)),
            ('artwork-scale-factor', struct.pack('<I', 2)),
            ('artwork-display-gamut', b'P3\0'),
            ('compatible-device-fallback', b'iPad6,7\0'),
            ('thin-bezel', struct.pack('<I', 1))
        ]),
        node('arm-io', [
            ('device_type', b'arm-io\0'),
            ('#address-cells', struct.pack('<I', 2)),
            ('#size-cells', struct.pack('<I', 2)),
            ('ranges', struct.pack('<QQQ', 0, ARMIO_BASE, ARMIO_SIZE))
        ], [
            node('intc', [
                ('compatible', b'arm,gic-v3\0'),
                ('reg', struct.pack('<4Q',
                                    GIC_DIST - ARMIO_BASE, GIC_DIST_SIZE,
                                    GIC_REDIST - ARMIO_BASE, GIC_REDIST_SIZE)),
                ('redistributor-stride', struct.pack('<Q', GIC_REDIST_STRIDE)),
                ('#interrupt-cells', struct.pack('<I', 3)),
                ('interrupt-controller', b'master\0'),
                ('phandle', struct.pack('<I', 1)),
                ('AAPL,phandle', struct.pack('<I', 1))
            ]),
            node('timer', [
                ('device_type', b'timer\0'),
                ('reg', struct.pack('<2Q', 0, 0x1000)),
                ('compatible', b'arm,armv8-timer\0'),
                ('clock-frequency', struct.pack('<I', TIMEBASE_HZ)),
            ]),
            node('ufshc', [
                ('compatible', b'qcom,sm8550-ufshc\0')
            ])
        ])
    ])

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    digest = hashlib.sha256(tree).hexdigest()
    print(f'Generated J716F Qualified ADT: {len(tree)} bytes, owned_memory_size: {args.owned_memory_size:#x}, sha256: {digest}')


if __name__ == '__main__':
    main()
