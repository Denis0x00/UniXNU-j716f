#!/usr/bin/env python3
"""Build two terminal single-factor diagnostics against the exact V23P2 kernel.

A removes one legacy store. B changes only the title-row DSB to DMB via a
register/flag-preserving dispatcher; other rows retain DSB. No device access.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('j716f_p2', HERE / 'build-v23p2-payload.py')
p2 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p2)
base = p2.base
BASELINE_SHA, ADT_SHA = base.BASELINE_SHA, base.ADT_SHA
P2_KERNEL_SHA = '5984888de0c85d8a06e56b3524ce185fa608851ada096b531a23d059edbcf8de'
P2_PROBE_SHA = 'f0e9819df126934e1388d0219fd11ac45deeb1d2f19444d66b50ec0c3e544039'
P2_BYTES = 1824
LEGACY_STORE = 0xfffffff0073fe32c
TITLE_DSB = 0xfffffff0073fca54
STORE = bytes.fromhex('010000b9')  # str w1,[x0]
NOP = bytes.fromhex('1f2003d5')
DSB = bytes.fromhex('9f3f03d5')
DMB = bytes.fromhex('bf3f03d5')
VARIANTS = {
    'p3a': (5, 'P2 control: skip legacy BCM store only'),
    'p3b': (5, 'P2 control: title DMB only; other rows DSB'),
}


def dispatcher():
    start = base.CAVE + P2_BYTES
    after = TITLE_DSB + 4
    # CBNZ X1,+12 has no register or NZCV side effects. Never use BL here.
    return (struct.pack('<I', 0xb5000061) + DMB + base.branch(start + 8, after)
            + DSB + base.branch(start + 16, after))


def audit(original, candidate, code, symbols, variant):
    assert variant in VARIANTS and base.sha(original) == BASELINE_SHA
    assert len(candidate) == len(original)
    m = base.MachO(candidate)
    at_store, at_barrier = m.offset(LEGACY_STORE), m.offset(TITLE_DSB)
    start = m.offset(base.CAVE, base.CAVE_END - base.CAVE)
    assert candidate[start:start + len(code)] == code
    restored = bytearray(candidate)
    if variant == 'p3a':
        assert len(code) == P2_BYTES and base.sha(code) == P2_PROBE_SHA
        assert candidate[at_store:at_store + 4] == NOP
        assert candidate[at_barrier:at_barrier + 4] == DSB
        restored[at_store:at_store + 4] = STORE
        mutation = {'site': hex(LEGACY_STORE), 'before': STORE.hex(), 'after': NOP.hex()}
    else:
        stub = dispatcher()
        assert len(code) == P2_BYTES + len(stub)
        assert candidate[at_store:at_store + 4] == STORE
        expected_branch = base.branch(TITLE_DSB, base.CAVE + P2_BYTES)
        assert candidate[at_barrier:at_barrier + 4] == expected_branch
        assert code[P2_BYTES:] == stub
        restored[at_barrier:at_barrier + 4] = DSB
        restored[start + P2_BYTES:start + len(code)] = b'\0' * len(stub)
        mutation = {'site': hex(TITLE_DSB), 'before': DSB.hex(),
                    'after': expected_branch.hex(), 'dispatcher': hex(base.CAVE + P2_BYTES),
                    'dispatcher_bytes': stub.hex(), 'title_row': 'DMB SY', 'other_rows': 'DSB SY'}
    # Reconstruct the control seal, then demand byte-for-byte equality to pinned P2.
    control_integrity = base.MachO(restored).hmac_info()
    h = control_integrity['offset']
    restored[h:h + 32] = bytes.fromhex(control_integrity['computed'])
    control = bytes(restored)
    assert base.sha(control) == P2_KERNEL_SHA, 'Unexpected change outside the single factor/HMAC'
    control_code = control[start:start + P2_BYTES]
    assert base.sha(control_code) == P2_PROBE_SHA
    report = p2.audit(original, control, control_code, symbols, 'p2')
    integrity = m.hmac_info()
    assert integrity['matches'], 'Final source-crypto HMAC mismatch'
    report.update(variant='V23' + variant.upper(), kernel_sha256=base.sha(candidate),
                  probe_bytes=len(code), source_crypto_hmac=integrity,
                  control_kernel_sha256=P2_KERNEL_SHA, control_probe_sha256=P2_PROBE_SHA,
                  single_factor_verified=True, mutation=mutation,
                  diagnostic_mode_unchanged=5, hardware_verified=False)
    report['checkpoints']['3'] = ('After original title DSB; before RET' if variant == 'p3a'
                                 else 'After title-only DMB dispatcher; before RET')
    return report


def write_elf(out, code, symbols):
    # Assemble the final bytes with the actual symbol addresses, including B's stub.
    assembly = '.text\n.incbin "' + str((out / 'probe.bin').resolve()) + '"\n'
    for name, value in symbols.items():
        assembly += '.global ' + name + '\n'
        assembly += '.set ' + name + ', ' + ('.text + ' + hex(value - base.CAVE)
                                            if base.CAVE <= value < base.CAVE_END else hex(value)) + '\n'
    (out / 'final-probe.S').write_text(assembly)
    (out / 'probe.ld').write_text('SECTIONS { . = %#x; .text : { *(.text*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n' % base.CAVE)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(out / 'final-probe.S'), '-o', str(out / 'probe.o')], check=True)
    subprocess.run(['ld.lld', '-T', str(out / 'probe.ld'), '-e', 'stage1', str(out / 'probe.o'), '-o', str(out / 'probe.elf')], check=True)
    subprocess.run(['llvm-objcopy', '-O', 'binary', '-j', '.text', str(out / 'probe.elf'), str(out / 'elf-check.bin')], check=True)
    assert (out / 'elf-check.bin').read_bytes() == code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['baseline', 'adt', 'sealer', 'output']:
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--variant', choices=VARIANTS, required=True)
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    control_dir = out / 'control'
    subprocess.run([sys.executable, str(HERE / 'build-v23p2-payload.py'),
                    '--baseline', str(args.baseline), '--adt', str(args.adt),
                    '--sealer', str(args.sealer), '--output', str(control_dir), '--variant', 'p2'], check=True)
    control = (control_dir / 'private/Payloads/kernel.macho').read_bytes()
    code = bytearray((control_dir / 'probe.bin').read_bytes())
    assert base.sha(control) == P2_KERNEL_SHA and base.sha(code) == P2_PROBE_SHA
    metadata = json.loads((control_dir / 'payload.json').read_text())
    symbols = metadata['symbols']
    data = bytearray(control)
    m = base.MachO(control)
    if args.variant == 'p3a':
        at = m.offset(LEGACY_STORE)
        assert data[at:at + 4] == STORE
        data[at:at + 4] = NOP
    else:
        at = TITLE_DSB - base.CAVE
        assert code[at:at + 4] == DSB
        code[at:at + 4] = base.branch(TITLE_DSB, base.CAVE + P2_BYTES)
        code += dispatcher()
        start = m.offset(base.CAVE, len(code))
        assert not any(data[start + P2_BYTES:start + len(code)])
        data[start:start + len(code)] = code
        symbols.update(title_barrier_dispatch=base.CAVE + P2_BYTES,
                       title_dmb=base.CAVE + P2_BYTES + 4,
                       non_title_dsb=base.CAVE + P2_BYTES + 12)
    (out / 'probe.bin').write_bytes(code)
    write_elf(out, bytes(code), symbols)
    shutil.copytree(control_dir / 'private', out / 'private')
    kernel = out / 'private/Payloads/kernel.macho'
    kernel.write_bytes(data)
    before_seal = base.sha(data)
    sealed = subprocess.run([sys.executable, str(args.sealer), str(kernel)], check=True, capture_output=True, text=True)
    (out / 'crypto-seal.log').write_text(sealed.stdout + sealed.stderr)
    kernel.with_suffix('.macho.crypto-hmac.json').rename(out / 'crypto-hmac.json')
    report = audit(args.baseline.read_bytes(), kernel.read_bytes(), bytes(code), symbols, args.variant)
    tag = 'V23' + args.variant.upper()
    config = (control_dir / 'private/EFI/UniXNU/unixnu.toml').read_text()
    config = config.replace('V23P2', tag).replace(p2.VARIANTS['p2'][1], VARIANTS[args.variant][1])
    (out / 'private/EFI/UniXNU/unixnu.toml').write_text(config, newline='\n')
    private = out / 'private'
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    for path in [private, *private.rglob('*')]:
        os.utime(path, (315532800, 315532800))
    fat = out / 'J716fEntryV23.fat'
    subprocess.run(['mformat', '-C', '-F', '-c', '1', '-T', '131072', '-v', tag,
                    '-N', '716f0023', '-i', str(fat), '::'], check=True, env=env)
    for name in ['EFI', 'Payloads']:
        subprocess.run(['mcopy', '-i', str(fat), '-s', '-Q', '-m', str(private / name), '::/'], check=True, env=env)
    report.update(symbols=symbols, before_seal_sha256=before_seal,
                  assembly_sha256=base.sha((HERE / 'v23p2-probe.S').read_bytes()),
                  builder_sha256=base.sha(Path(__file__).read_bytes()),
                  sealer_sha256=base.sha(args.sealer.read_bytes()), adt_sha256=ADT_SHA,
                  payload_sha256=base.sha(fat.read_bytes()),
                  files={f.relative_to(private).as_posix(): {'bytes': f.stat().st_size, 'sha256': base.sha(f.read_bytes())}
                         for f in sorted(private.rglob('*')) if f.is_file()})
    (out / 'payload.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['variant', 'probe_bytes', 'kernel_sha256', 'single_factor_verified']}, indent=2))


if __name__ == '__main__':
    main()
