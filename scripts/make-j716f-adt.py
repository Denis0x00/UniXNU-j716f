#!/usr/bin/env python3
"""Generate the qualified J716F hardware Apple-format Device Tree (ADT).

Ports the verified SM8250 architecture from UniXNU-pipa and UniXNU-sheng,
tailored to Lenovo Xiaoxin Pad Pro 2021 (TB-J716F) hardware parameters:
  - GICv3 Distributor: 0x17a00000 (0x10000 bytes)
  - GICv3 Redistributor: 0x17a60000 (0x100000 bytes, stride 0x20000)
  - Architectural Timer: 19.2 MHz counter (PPI11 / INTID 27)
  - Single boot CPU: Kryo 585 Silver (Cortex-A55, Affinity 0)
  - DRAM: base 0xc0000000, 256 MiB owned DarwinBoot window
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
DRAM_BASE = 0xC0000000
DRAM_SIZE = 0x140000000         # 6 GiB physical DRAM bank
OWNED_MEMORY_SIZE = 0x10000000  # 256 MiB DarwinBoot contiguous window
ARMIO_BASE = 0x17000000         # APSS peripheral block base
ARMIO_SIZE = 0x08000000         # 128 MiB peripheral aperture
GIC_DIST = 0x17A00000
GIC_DIST_SIZE = 0x10000
GIC_REDIST = 0x17A60000
GIC_REDIST_SIZE = 0x100000
GIC_REDIST_STRIDE = 0x20000
TIMEBASE_HZ = 19200000
CHIP_ID = 0x8011                # iPad7,1 / J120 target chip-id


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


def build_adt(unique_chip_id: int | None = None,
              root_match_uuid: str | None = None,
              data_ramdisk: bool = False) -> bytes:
    if unique_chip_id is None:
        unique_chip_id = int.from_bytes(os.urandom(8), 'little') | 1

    # Single Boot Core 0 (Kryo Silver / Cortex-A55)
    cpus = [
        node('cpu0', [
            ('state', b'running\0'),
            ('reg', struct.pack('<I', 0)),
            ('device_type', b'cpu\0'),
            ('compatible', b'arm,cortex-a55\0'),
            ('timebase-frequency', struct.pack('<I', TIMEBASE_HZ))
        ])
    ]

    # Standard armv8-timer interrupts specifiers:
    # 4 PPIs: Secure Phys, Non-Secure Phys, Virtual (PPI11=INTID27), Hypervisor
    timer_interrupts = struct.pack('<12I',
        1, 13, 0xff08,
        1, 14, 0xff08,
        1, 11, 0xff08,
        1, 12, 0xff08
    )

    tree = node('device-tree', [
        ('model', b'iPad7,1\0'),
        ('target-type', b'J120\0'),
        ('compatible', b'lenovo,j716f\0qcom,sm8250\0'),
        ('#address-cells', struct.pack('<I', 2)),
        ('#size-cells', struct.pack('<I', 2)),
    ], [
        node('chosen', [
            ('random-seed', os.urandom(64)),
            ('unique-chip-id', struct.pack('<Q', unique_chip_id)),
            ('chip-id', struct.pack('<I', CHIP_ID)),
            ('osenvironment', b'main\0'),
            ('certificate-production-status', struct.pack('<I', 0)),
            ('effective-production-status-ap', struct.pack('<I', 0)),
            ('certificate-security-mode', struct.pack('<I', 1)),
            ('effective-security-mode-sep', struct.pack('<I', 1)),
            ('darwinboot-memory-size', struct.pack('<Q', OWNED_MEMORY_SIZE)),
            ('dram-base', struct.pack('<Q', DRAM_BASE)),
            ('dram-size', struct.pack('<Q', DRAM_SIZE)),
            ('crypto-hash-method', b'sha2-384\0'),
            ('amfi-only-platform-code', struct.pack('<I', 1)),
            *([('root-matching', (
                '<dict><key>IOProviderClass</key><string>IOMedia</string>'
                '<key>IOPropertyMatch</key><dict><key>UUID</key><string>' +
                root_match_uuid.upper() + '</string></dict></dict>\0'
            ).encode())] if root_match_uuid else []),
            *([('ephemeral-storage', struct.pack('<I', 1))] if data_ramdisk else []),
            *empty_nvram_proxy()
        ], [
            node('memory-map', [('RAMDisk', bytes(16))]),
            node('manifest-properties')
        ]),
        node('cpus', children=cpus),
        node('psci', [
            ('compatible', b'arm,psci-1.0\0'),
            ('method', b'smc\0')
        ]),
        node('defaults', [
            ('sleep-disabled', struct.pack('<I', 1))
        ]),
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
            ('artwork-device-idiom', b'pad\0'),
            ('artwork-scale-factor', struct.pack('<I', 2)),
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
                ('interrupt-controller', b''),
                ('phandle', struct.pack('<I', 1))
            ]),
            node('timer', [
                ('compatible', b'arm,armv8-timer\0'),
                ('clock-frequency', struct.pack('<I', TIMEBASE_HZ)),
                ('interrupts', timer_interrupts),
                ('interrupt-parent', struct.pack('<I', 1))
            ]),
            node('ufshc', [
                ('compatible', b'qcom,sm8550-ufshc\0')
            ])
        ])
    ])
    return tree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=ROOT / 'dts/kernel.macho.adt')
    parser.add_argument('--root-uuid', default=None)
    parser.add_argument('--data-ramdisk', action='store_true')
    args = parser.parse_args()

    adt_data = build_adt(root_match_uuid=args.root_uuid, data_ramdisk=args.data_ramdisk)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(adt_data)
    digest = hashlib.sha256(adt_data).hexdigest()
    print(f'Generated J716F Qualified ADT: {len(adt_data)} bytes, sha256: {digest}')


if __name__ == '__main__':
    main()
