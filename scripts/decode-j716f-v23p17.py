#!/usr/bin/env python3
"""Decode P17 CRC-checked records (ENTRY high32=ARM CRC, low32=SCTLR); input stays private. No device commands."""
import argparse
import binascii
import hashlib
import json
from pathlib import Path
import struct

CAPACITY = 0x3fff4
PHASES = {1: 'INIT', 2: 'BASELINE', 3: 'PRE_RESET', 4: 'RESET_RETURN',
          5: 'PRE_VERSION', 6: 'VERSION_RETURN', 7: 'PRE_FEATURE', 8: 'FEATURE_RETURN',
          9: 'ARM_KERNEL', 10: 'KERNEL_ENTRY', 11: 'PRE_MMU', 12: 'TOPOLOGY_STOP_SCTLR',
          13: 'MAP_FAILURE', 14: 'KERNEL_RESET_RETURN',
          16: 'P17_BCM_ROUTE_SKIPPED', 17: 'P17_M1_READBACK', 18: 'P17_WFE_MISC_RETURN',
          19: 'P17_CHOSEN_LOOKUP_RETURN', 20: 'P17_STALL_PROPERTY_RETURN', 21: 'P17_CPU_TOPOLOGY_RETURN'}


def scan(data, circular=False):
    records = []
    view = data + data[:63] if circular else data
    for at in range(len(data) if circular else max(0, len(data) - 63)):
        p = view[at:at + 64]
        if p[:8] != b'J716FP8!' or len(p) != 64 or p[56:] != b'P8DONE!\n':
            continue
        version, size, token, phase = struct.unpack_from('<4I', p, 8)
        crc, inverse = struct.unpack_from('<2I', p, 48)
        if (version != 1 or size != 64 or not token or phase not in PHASES or
                p[44:48] != b'\0'*4 or crc != binascii.crc32(p[:48]) or inverse != crc ^ 0xffffffff):
            continue
        sig, start, length = struct.unpack_from('<3I', p, 32)
        records.append({'offset': at, 'id': token, 'phase': PHASES[phase],
                        'crc32': f'{crc:08x}', 'value': hex(struct.unpack_from('<Q', p, 24)[0]),
                        'before_header': {'signature': hex(sig), 'next': start, 'size': length}})
    return records


def decode(data, kind):
    result = {'input_sha256': hashlib.sha256(data).hexdigest(), 'input_kind': kind}
    if kind == 'raw-console':
        if len(data) != CAPACITY + 12:
            raise ValueError('Raw console must be exactly 0x40000 bytes, including the 12-byte header')
        sig, start, size = struct.unpack_from('<3I', data)
        valid = sig == 0x43474244 and start < CAPACITY and size <= CAPACITY and start <= size
        result['header'] = {'signature': hex(sig), 'next': start, 'size': size, 'valid': valid}
        payload = data[12:]
        logical = payload[start:] + payload[:start] if valid and size == CAPACITY else payload[:size] if valid else b''
        result['logical_records'] = scan(logical)
        result['raw_records_may_be_stale'] = scan(payload, True)
    else:
        result['pstore_records'] = scan(data)
    result['interpretation'] = ('Match the photographed pre-reset ID and CRC in a controlled reboot. '
                                'Raw records alone establish surviving payload bytes, not a valid ring or the reset path. '
                                'P17 slot value binds the ARM CRC; templates are invalid until one byte commits. '
                                'Stage21 follows original stall/topology and boot CPU number return; TOPOLOGY_STOP_SCTLR contains actual SCTLR_EL1 with M1 required. No timer IRQ or scheduler claim. '
                                'Validate ARM/ENTRY, offsets and expected slot before attributing raw records. '
                                'Absence does not identify the loss stage.')
    if kind == 'raw-console': result['p17_bound_attempt'] = bound_p17(result)
    return result


def bound_p17(result):
    """Independent reconstruction of the same ARM/ENTRY/slot binding as reader."""
    raw=result.get('raw_records_may_be_stale',[])
    latest=max((q['id'] for q in raw),default=0)
    rows={name:None for name in [PHASES[i] for i in range(16,22)]}
    anchors=[q for q in raw if q['id']==latest and q['phase']=='ARM_KERNEL' and
             int(q['value'],16)==0x5031374b45524e4c and
             q['offset']==q['before_header']['next']==q['before_header']['size'] and
             q['before_header']['signature']=='0x43474244' and q['offset']<=CAPACITY-576]
    out={'id':latest,'anchor_unique':len(anchors)==1,'entry_bound':False,'witnesses':rows}
    if len(anchors)!=1:return out
    arm=anchors[0];base=arm['offset']+128;crc=int(arm['crc32'],16)
    entries=[q for q in raw if q['id']==latest and q['phase']=='KERNEL_ENTRY' and
             q['offset']==base-64 and q['before_header']=={'signature':'0x43474244','next':base-64,'size':base-64} and
             int(q['value'],16)>>32==crc and not(int(q['value'],16)&1)]
    out.update(arm_crc32=arm['crc32'],slot_base=hex(base),entry_bound=len(entries)==1)
    if len(entries)!=1:return out
    out['entry_sctlr']=hex(int(entries[0]['value'],16)&0xffffffff)
    header=result['header']
    for i in range(6):
        at=base+i*64;phase=PHASES[16+i]
        matches=[q for q in raw if q['id']==latest and q['phase']==phase and q['offset']==at and
                 int(q['value'],16)==0x5031375300000000|crc and
                 q['before_header']=={'signature':'0x43474244','next':at,'size':at}]
        if len(matches)==1:
            rows[phase]={'raw':True,'logical':bool(header['valid'] and header['next']==header['size'] and at+64<=header['size']),
                         'crc32':matches[0]['crc32'],'offset':hex(at)}
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('input', type=Path)
    p.add_argument('--kind', choices=['raw-console', 'pstore'], required=True)
    p.add_argument('--capture-manifest', type=Path, help='Required for pstore; accepted collector entry and SHA must match')
    a = p.parse_args()
    data = a.input.read_bytes()
    if a.kind == 'pstore':
        if not a.capture_manifest:
            p.error('pstore requires --capture-manifest; error text is not a log')
        manifest = json.loads(a.capture_manifest.read_text())
        accepted = [c for c in manifest['captures'] if c['file'] == a.input.name and
                    c.get('accepted') is True and c.get('remote_status') == 0 and
                    c['sha256'] == hashlib.sha256(data).hexdigest()]
        if len(accepted) != 1:
            p.error('No uniquely accepted capture matching this file and hash')
    print(json.dumps(decode(data, a.kind), indent=2))


if __name__ == '__main__':
    main()
