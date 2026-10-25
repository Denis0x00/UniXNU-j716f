#!/usr/bin/env python3
"""Build one pinned J716F V22 checkpoint payload; never accesses a device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

BASELINE_SHA = 'cab8f385a44d21fc6f455cb501227452212ba44d34a9fa03d04fcf7e85324214'
ADT_SHA = 'b68fc49b2d9bb6b0a387e59a7816fd4848dffc3bde706a439cbbff3cbebb1aa7'
TOOL_SHA = '159eb1ea8d6c1da30f26913a281f46e21d940a1ae7886656529907ec3edf96ad'
ENTRY = 0xfffffff0073fc488
MMU = 0xfffffff0073fe374
AFTER_MMU = MMU + 0x20
CAVE = ENTRY + 0x78
STAGE12_SHA = [
    'fdc5c49f2010e6b57937ab639984747a9f035ee20559d48a1e9b73fc30111593',
    '9549bea33d8829ec4c6c1c340466db14d1af6663caecb80f1faf0033c3b7cf33',
]
# Independent instruction expectation: timer loop, PSCI SYSTEM_RESET, WFE fallback.
RESET_CODE = bytes.fromhex(
    '0006a0d221e03bd5c20100582200028bdf3f03d521e03bd53f0002eba3ffff54'
    '200180520080b072e1031faae2031faae3031faa030000d45f2003d5ffffff17')


def sha(data):
    return hashlib.sha256(data).hexdigest()


def audit_probes(original, probe, delay):
    """Check exact patch bounds and instructions independently of the patch tool."""
    assert sha(original) == BASELINE_SHA and len(original) == len(probe)
    segments = []
    pos = 32
    for _ in range(struct.unpack_from('<I', original, 16)[0]):
        cmd, size = struct.unpack_from('<II', original, pos)
        if cmd == 0x19:
            va, vsize, off, fsize = struct.unpack_from('<QQQQ', original, pos + 24)
            segments.append((va, off, min(vsize, fsize)))
        if cmd == 5:
            assert struct.unpack_from('<Q', original, pos + 272)[0] == ENTRY
        pos += size
    assert original[:pos] == probe[:pos], 'Mach-O layout changed'

    def offset(va, size=4):
        matches = [off + va - base for base, off, count in segments
                   if base <= va and va + size <= base + count]
        assert len(matches) == 1
        return matches[0]

    def word(data, va):
        return struct.unpack_from('<I', data, offset(va))[0]

    def branch(site, target):
        return 0x14000000 | (((target - site) // 4) & 0x3ffffff)

    assert word(original, ENTRY) == branch(ENTRY, 0xfffffff0073fe05c)
    assert word(original, MMU) == 0xd5181000
    assert word(original, AFTER_MMU) == 0xd2a00600
    start = offset(CAVE, 0x120)
    assert not any(original[start:start + 0x120])
    for i, expected in enumerate(STAGE12_SHA):
        assert sha(probe[start + i * 0x60:start + (i + 1) * 0x60]) == expected
    reset = probe[start + 0xc0:start + 0x120]
    assert reset[:0x40] == RESET_CODE
    assert struct.unpack_from('<Q', reset, 0x40)[0] == 19200000 * delay
    assert not any(reset[0x48:])
    masked = bytearray(probe)
    masked[start:start + 0x120] = original[start:start + 0x120]
    for site, target in [(ENTRY, CAVE), (MMU, CAVE + 0x60), (AFTER_MMU, CAVE + 0xc0)]:
        assert word(probe, site) == branch(site, target)
        at = offset(site)
        masked[at:at + 4] = original[at:at + 4]
    assert masked == original, 'Changes outside the three entry sites and verified padding'
    return {'baseline_sha256': sha(original), 'kernel_sha256': sha(probe),
            'kernel_bytes': len(probe), 'entry_va': hex(ENTRY),
            'mmu_site': hex(MMU), 'post_mmu_site': hex(AFTER_MMU),
            'reset_cave': hex(CAVE + 0xc0), 'delay_seconds': delay,
            'timer_ticks': 19200000 * delay, 'psci_function': '0x84000009',
            'changes_confined_to_verified_sites': True,
            'panic_renderer_installed': False, 'hardware_verified': False}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--unixnu', type=Path, required=True)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--adt', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--variant', choices=['a', 'b'], required=True)
    a = p.parse_args()
    delay = {'a': 1, 'b': 3}[a.variant]
    tag = 'V22' + a.variant.upper()
    tool = a.unixnu / 'scripts/patch-xnu-entry-probe.py'
    assert sha(tool.read_bytes()) == TOOL_SHA
    original = a.baseline.read_bytes()
    adt = a.adt.read_bytes()
    assert sha(original) == BASELINE_SHA and sha(adt) == ADT_SHA
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    private = out / 'private'
    (private / 'Payloads').mkdir(parents=True, exist_ok=True)
    (private / 'EFI/UniXNU').mkdir(parents=True, exist_ok=True)
    kernel = private / 'Payloads/kernel.macho'
    kernel.write_bytes(original)
    command = [sys.executable, str(tool), str(kernel), '--reset-delay', str(delay)]
    patched = subprocess.run(command, check=True, text=True, capture_output=True)
    (out / 'probe-build.log').write_text(patched.stdout + patched.stderr)
    report = audit_probes(original, kernel.read_bytes(), delay)
    report.update({'variant': tag, 'adt_sha256': ADT_SHA,
                   'probe_tool_sha256': TOOL_SHA,
                   'probe_tool_revision': '42bffe8a56823dad2e448c776b4f94862f3e290a'})
    (private / 'Payloads/kernel.macho.adt').write_bytes(adt)
    config = f'''version = 1
[boot]
timeout = -1
fallback_timeout = -1
rotate = 0
connect_all = false
default = "entry"
[kits]
autoload = false
[ui]
title = "J716F {tag} - post-MMU reset {delay}s"
[[entry]]
id = "entry"
title = "{tag}: MMU-on checkpoint / reset {delay}s"
type = "xnu"
path = "\\\\Payloads\\\\kernel.macho"
args = "-v serial=0 debug=0x14e cpus=1 -no_slto_panic"
[[entry]]
id = "reset"
title = "Restart"
type = "action"
action = "reset"
'''
    (private / 'EFI/UniXNU/unixnu.toml').write_text(config, newline='\n')
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    for item in [private, *private.rglob('*')]:
        os.utime(item, (315532800, 315532800))
    fat = out / 'J716fEntryV22.fat'
    # Refuse accidental replacement of a previously audited variant.
    assert not fat.exists(), str(fat)
    subprocess.run(['mformat', '-C', '-F', '-c', '1', '-T', '131072', '-v', tag,
                    '-N', '716f0022', '-i', str(fat), '::'], check=True, env=env)
    for name in ['EFI', 'Payloads']:
        subprocess.run(['mcopy', '-i', str(fat), '-s', '-Q', '-m', str(private / name),
                        '::/'], check=True, env=env)
    data = fat.read_bytes()
    assert len(data) == 0x4000000 and data[82:90] == b'FAT32   '
    report['payload_sha256'] = sha(data)
    report['payload_bytes'] = len(data)
    report['files'] = {f.relative_to(private).as_posix(): {'bytes': f.stat().st_size,
                      'sha256': sha(f.read_bytes())} for f in sorted(private.rglob('*')) if f.is_file()}
    (out / 'payload.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
