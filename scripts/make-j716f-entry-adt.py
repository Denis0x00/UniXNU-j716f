#!/usr/bin/env python3
"""J716F single-CPU entry diagnostic ADT; no Apple board identity or storage.

Apple node encoding and empty CHRP NVRAM construction follow UniXNU's pinned
qemu-entry/make-dt.py and UniXNU-sheng 251b291adf941b9953fe3ae66a808b9b626e164d.
All board resources are read from the separately audited J716F evidence JSON.
SPDX-License-Identifier: BSD-2-Clause-Patent
"""
import argparse
import json
import os
from pathlib import Path
import struct
import zlib

def node(name, properties=(), children=()):
    props = [('name', name.encode()+b'\0'), *properties]
    assert len({k for k, _ in props}) == len(props)
    data = bytearray(struct.pack('<II', len(props), len(children)))
    for key, value in props:
        assert len(key.encode()) < 32
        data += struct.pack('<32sI', key.encode(), len(value)) + value + bytes(-len(value) % 4)
    return bytes(data)+b''.join(children)

def empty_nvram():
    def header(name, size):
        raw = bytearray(struct.pack('<BBH12s', 0x70, 0, size//16, name))
        checksum = sum(raw)
        while checksum > 255:
            checksum = (checksum & 255)+(checksum >> 8)
        raw[1] = checksum
        return raw
    image = bytearray(8192)
    image[:16] = header(b'nvram', 32)
    image[32:48] = header(b'common', 16+2048)
    free_at = 32+16+2048
    image[free_at:free_at+16] = header(b'free', len(image)-free_at)
    struct.pack_into('<I', image, 16, zlib.adler32(image[20:]))
    return [('nvram-bank-size', struct.pack('<I', len(image))), ('nvram-bank-count', struct.pack('<I', 1)),
            ('nvram-current-bank', struct.pack('<I', 0)), ('nvram-proxy-data', bytes(image))]

def make(evidence):
    assert evidence['board'] == 'j716f' and evidence['stock_recovery_resources_agree']
    assert evidence['owned_memory_bytes'] == evidence['v14_qualified_window']['bytes'] == 0x10000000
    assert evidence['cpu0']['mpidr_affinity'] == 0 and evidence['timer_hz'] == 19200000
    g = evidence['gic']
    bus_base = g['distributor_base']
    bus_end = max(g['distributor_base']+g['distributor_bytes'], g['redistributor_base']+g['redistributor_bytes'])
    primary = evidence['primary_contiguous_dram_bank']
    u32 = lambda value: struct.pack('<I', value)
    u64 = lambda value: struct.pack('<Q', value)
    # chosen DRAM is the observed primary physical bank, not allocatable memory.
    # DarwinBoot alone fills boot_args with the actual 256 MiB owned window.
    return node('device-tree', [('model', b'Lenovo TB-J716F entry diagnostic\0'),
                ('compatible', b'lenovo,j716f\0'+b''.join(v.encode()+b'\0' for v in evidence['root_compatible'])),
                ('#address-cells', u32(2)), ('#size-cells', u32(2))], [
        node('chosen', [('random-seed', os.urandom(64)), ('osenvironment', b'main\0'),
                       ('dram-base', u64(primary['base'])), ('dram-size', u64(primary['bytes'])),
                       ('darwinboot-memory-size', u64(evidence['owned_memory_bytes'])),
                       ('crypto-hash-method', b'sha2-384\0'), ('amfi-only-platform-code', u32(1)),
                       *empty_nvram()], [node('memory-map'), node('manifest-properties')]),
        node('cpus', children=[node('cpu0', [('state', b'running\0'), ('reg', u32(0)),
                    ('device_type', b'cpu\0'), ('compatible', evidence['cpu0']['compatible'].encode()+b'\0'),
                    ('timebase-frequency', u32(evidence['timer_hz']))])]),
        node('memory', [('device_type', b'memory\0'),
                        ('reg', b''.join(struct.pack('<QQ', b['base'], b['bytes']) for b in evidence['ram_banks']))]),
        node('defaults', [('sleep-disabled', u32(1))]), node('options'),
        node('product', [('product-name', b'Lenovo Xiaoxin Pad Pro 2021\0'), ('model', b'Lenovo TB-J716F\0')]),
        node('arm-io', [('device_type', b'arm-io\0'), ('#address-cells', u32(2)), ('#size-cells', u32(2)),
                        ('ranges', struct.pack('<QQQ', 0, bus_base, bus_end-bus_base))], [
            node('intc', [('compatible', b'arm,gic-v3\0'),
                          ('reg', struct.pack('<4Q', g['distributor_base']-bus_base, g['distributor_bytes'],
                                              g['redistributor_base']-bus_base, g['redistributor_bytes'])),
                          ('redistributor-stride', u64(g['redistributor_stride']))])])])

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--hardware-evidence', type=Path, default=Path(__file__).with_name('j716f-hardware-evidence.json'))
    p.add_argument('--dram-size', type=lambda value: int(value, 0), help='Must match the recorded primary physical DRAM bank')
    p.add_argument('--owned-memory-size', type=lambda value: int(value, 0), default=0x10000000)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    evidence = json.loads(args.hardware_evidence.read_text(encoding='utf-8'))
    if args.dram_size is not None and args.dram_size != evidence['primary_contiguous_dram_bank']['bytes']:
        p.error('--dram-size differs from the recorded primary contiguous physical bank')
    if args.owned_memory_size != 0x10000000:
        p.error('This diagnostic supports only the V14-qualified 256 MiB window size')
    tree = make(evidence)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(tree)
    print(f'J716F single-CPU ADT: {len(tree)} bytes; owned window 256 MiB; no root volume/ramdisk/trust-cache sidecars')

if __name__ == '__main__': main()
