#!/usr/bin/env python3
"""Decode a frozen native observer memory dump; never promote host fixtures to hardware evidence."""
import argparse
import json
from pathlib import Path
import struct

MAGIC = 0x315152494636314a
WORDS = 192


def check(data, evidence):
    if len(data) != WORDS*8:
        raise ValueError('Expected exactly 1536 bytes of observer memory')
    t = struct.unpack('<192Q', data)
    if t[:3] != (MAGIC, 1, WORDS):
        raise ValueError('Observer ABI mismatch')
    reasons = []
    def need(condition, why):
        if not condition:
            reasons.append(why)
    need(t[3] != 0 and not t[3] & 1, 'incomplete or in-flight sequence')
    need((t[4] & 0xfe) == 0xfe, 'platform startup observations incomplete')
    need(not t[4] & 0x300 and t[35] == 0 and t[36] == 0, 'CPU quiesce/halt observed')
    need(t[6:9] == (8, 8, 8), 'eight IRQ enter/rtclock return/EOI completions required')
    need(t[11] == 1 and t[12] == 0 and t[34] == 3, 'checkpoint missing or event-order error')
    need(t[9] < t[10], 'counter did not advance')
    need(t[15] == 0x1ff, 'missing first/second/eighth IRQ snapshots')
    need(t[13] == 0 and t[32] == 1 and t[33] == 1, 'boot CPU providers not ready')
    affinity = evidence['cpu0']['mpidr_affinity']
    need(t[14] & 0xff00ffffff == affinity, 'boot MPIDR differs from board evidence')
    need(not any(t[37:48]), 'unexpected reserved header fields')
    previous = 0
    samples = []
    for sample, count in enumerate((1, 2, 8)):
        group = []
        for phase in (1, 2, 3):
            slot = sample*3+phase-1
            r = t[48+slot*16:48+(slot+1)*16]
            expected = (phase, count, count-(phase == 1), count-(phase != 3))
            need(r[:4] == expected, f'IRQ {count} phase {phase} counters/order mismatch')
            need(r[4] == evidence['timer_hz'], f'IRQ {count} phase {phase} CNTFRQ mismatch')
            need(r[5] >= previous and r[5] != 0, 'snapshot counter went backwards or is zero')
            previous = r[5]
            need((r[9] & 0xffffff) == 27 and r[15] == 0, 'not boot CPU INTID 27')
            need(r[10:12] == (1, 1), 'IRQ without registered CPU/GIC provider')
            need(r[12] & 0xff00ffffff == affinity, 'IRQ MPIDR differs from board evidence')
            need(r[13] == count, 'global timer count inconsistent with single CPU candidate')
            # phase2 hook is immediately after rtclock_intr, before upstream phase=3.
            need(r[14] == (6 if phase == 3 else 2), 'upstream IRQ phase mismatch')
            group.append({'phase': phase, 'counter': r[5], 'deadline': r[6],
                          'deadline_minus_counter': r[6]-r[5], 'control': r[7], 'daif': r[8]})
        samples.append({'irq': count, 'stages': group})
    need(t[9] == t[53] and t[10] == t[149], 'header counters disagree with snapshots')
    return {'trace_consistent': not reasons, 'reasons': reasons, 'samples': samples,
            'entered': t[6], 'rtclock_returned': t[7], 'eoi_completed': t[8],
            'gic_readbacks': list(t[16:24]), 'hardware_validated': False,
            'scope': 'structural consistency only; requires independently captured live-device provenance; '
                     'stock/recovery INTID27 input description does not prove live IRQ delivery'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dump', type=Path, required=True)
    parser.add_argument('--hardware-evidence', type=Path, default=Path(__file__).with_name('j716f-hardware-evidence.json'))
    args = parser.parse_args()
    result = check(args.dump.read_bytes(), json.loads(args.hardware_evidence.read_text(encoding='utf-8')))
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['trace_consistent'] else 1)
