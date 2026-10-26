#!/usr/bin/env python3
"""Build V23P4 with before/after markers around the legacy BCM MMIO store.

The existing V23P2 probe and kernel are the control.  P4 changes one original
instruction: a branch at the legacy store enters a small code-cave stub that
draws a marker, executes the original ``str w1,[x0]`` unchanged, draws a second
marker, and branches back to the following instruction.  The title DSB and all
other P2 code remain byte-for-byte identical except for a clear-loop filter
that preserves these two markers. The envelope initializes both markers before
the store so that pixels from an earlier run cannot impersonate completion.
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
LEGACY_AFTER = LEGACY_STORE + 4
STORE = bytes.fromhex('010000b9')
NOP = bytes.fromhex('1f2003d5')
VARIANTS = {'p4': (5, 'r1: legacy MMIO envelope; preserve early markers')}


def stub_source():
    # The fixed framebuffer reservation and pitch were already checked by the
    # P2 probe. x9..x14/NZCV are dead at this insertion point until overwritten
    # by the terminal probe; x0/x1, x15..x30, SP are retained. This is an inline
    # instruction replacement, so an ABI caller-saved rule is not sufficient.
    return r'''.text
.global mmio_envelope, mmio_store, mmio_after, mmio_return
.global clear_filter, clear_skip_return, clear_write_return
.type mmio_envelope,%function
mmio_envelope:
  // Initialize the complete bounding rectangle, including both markers.
  mov x9, #0x9c000000
  movz x10, #0x1400
  movk x10, #0x1e, lsl #16
  add x9, x9, x10
  mov w12, #0
  mov x14, #10240
  mov x10, #96
5:
  mov x11, x9
  mov x13, #100
6:
  stp w12, w12, [x11], #8
  subs x13, x13, #1
  b.ne 6b
  add x9, x9, x14
  subs x10, x10, #1
  b.ne 5b

  mov x9, #0x9c000000
  movz x10, #0x1400
  movk x10, #0x1e, lsl #16 // (y=192)*10240 + (x=1280)*4
  add x9, x9, x10
  mov w12, #-1
  mov x14, #10240
  mov x10, #96
1:
  mov x11, x9
  mov x13, #48
2:
  stp w12, w12, [x11], #8
  subs x13, x13, #1
  b.ne 2b
  add x9, x9, x14
  subs x10, x10, #1
  b.ne 1b

mmio_store:
  str w1, [x0]              // original instruction, unchanged

mmio_after:
  mov x9, #0x9c000000
  movz x10, #0x15a0
  movk x10, #0x1e, lsl #16 // (y=192)*10240 + (x=1384)*4
  add x9, x9, x10
  mov w12, #-1
  mov x10, #96
3:
  mov x11, x9
  mov x13, #48
4:
  stp w12, w12, [x11], #8
  subs x13, x13, #1
  b.ne 4b
  add x9, x9, x14
  subs x10, x10, #1
  b.ne 3b
mmio_return:
  nop                       // replaced by B to original instruction + 4

clear_filter:
  // Original clear writes 16 bytes at x9, then compares x9 to x10.
  // Skip only the two rectangles. Every other byte is cleared normally.
  sub x11, x9, x19
  mov x12, #10240
  udiv x12, x11, x12
  cmp x12, #192
  b.lo clear_write
  cmp x12, #288
  b.hs clear_write
  mov x13, #10240
  msub x11, x12, x13, x11
  mov x13, #5120
  cmp x11, x13
  b.lo clear_write
  mov x13, #5504
  cmp x11, x13
  b.lo clear_skip
  mov x13, #5536
  cmp x11, x13
  b.lo clear_write
  mov x13, #5920
  cmp x11, x13
  b.hs clear_write
clear_skip:
  add x9, x9, #16
clear_skip_return:
  nop                       // replaced by B to original clear CMP
clear_write:
  stp xzr, xzr, [x9], #16
clear_write_return:
  nop                       // replaced by B to original clear CMP
'''


def assemble_stub(out, clear_resume):
    source = out / 'mmio-envelope.S'
    source.write_text(stub_source(), encoding='utf-8', newline='\n')
    linker = out / 'stub.ld'
    linker.write_text('SECTIONS { . = %#x; .text : { *(.text*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n' % (base.CAVE + P2_BYTES), encoding='utf-8')
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(source), '-o', str(out / 'stub.o')], check=True)
    subprocess.run(['ld.lld', '-T', str(linker), '-e', 'mmio_envelope', str(out / 'stub.o'), '-o', str(out / 'stub.elf')], check=True)
    subprocess.run(['llvm-objcopy', '-O', 'binary', '-j', '.text', str(out / 'stub.elf'), str(out / 'stub.bin')], check=True)
    stub = bytearray((out / 'stub.bin').read_bytes())
    symbols = {parts[2]: int(parts[0], 16) for line in subprocess.check_output(
        ['llvm-nm', '--defined-only', '-n', str(out / 'stub.elf')], text=True).splitlines()
        if len(parts := line.split()) == 3}
    for name, target in [('mmio_return', LEGACY_AFTER), ('clear_skip_return', clear_resume),
                         ('clear_write_return', clear_resume)]:
        site = symbols[name]
        offset = site - base.CAVE - P2_BYTES
        assert stub[offset:offset + 4] == NOP
        stub[offset:offset + 4] = base.branch(site, target)
    (out / 'stub-patched.bin').write_bytes(stub)
    return bytes(stub), symbols


def audit(original, candidate, code, symbols, variant='p4'):
    assert variant == 'p4'
    assert base.sha(original) == base.BASELINE_SHA
    assert len(candidate) == len(original)
    assert len(code) > P2_BYTES and len(code) <= base.CAVE_END - base.CAVE
    m = base.MachO(candidate)
    start = m.offset(base.CAVE, base.CAVE_END - base.CAVE)
    at = m.offset(LEGACY_STORE)
    stub_start = base.CAVE + P2_BYTES
    expected_branch = base.branch(LEGACY_STORE, stub_start)
    assert candidate[at:at + 4] == expected_branch
    assert candidate[start:start + len(code)] == code
    clear_at = symbols['clear'] - base.CAVE
    clear_original = bytes.fromhex('3f7d81a8')  # stp xzr,xzr,[x9],#16
    assert code[clear_at:clear_at + 4] == base.branch(symbols['clear'], symbols['clear_filter'])
    assert code[symbols['mmio_store'] - base.CAVE:symbols['mmio_store'] - base.CAVE + 4] == STORE
    for name, target in [('mmio_return', LEGACY_AFTER), ('clear_skip_return', symbols['clear'] + 4),
                         ('clear_write_return', symbols['clear'] + 4)]:
        offset = symbols[name] - base.CAVE
        assert code[offset:offset + 4] == base.branch(symbols[name], target)
    # Remove only the P4 factor and rebuild the control HMAC.  The result must
    # be exactly the pinned P2 kernel, proving there are no hidden mutations.
    restored = bytearray(candidate)
    restored[at:at + 4] = STORE
    restored[start + clear_at:start + clear_at + 4] = clear_original
    restored[start + P2_BYTES:start + len(code)] = b'\0' * (len(code) - P2_BYTES)
    hmac_info = base.MachO(restored).hmac_info()
    restored[hmac_info['offset']:hmac_info['offset'] + 32] = bytes.fromhex(hmac_info['computed'])
    control = bytes(restored)
    assert base.sha(control) == P2_KERNEL_SHA
    control_code = control[start:start + P2_BYTES]
    assert base.sha(control_code) == P2_PROBE_SHA
    report = p2.audit(original, control, control_code, symbols, 'p2')
    integrity = m.hmac_info()
    assert integrity['matches']
    report.update(variant='V23P4', kernel_sha256=base.sha(candidate), probe_bytes=len(code),
                  source_crypto_hmac=integrity, control_kernel_sha256=P2_KERNEL_SHA,
                  control_probe_sha256=P2_PROBE_SHA, single_factor_verified=True,
                  diagnostic_mode_unchanged=5, hardware_verified=False, diagnostic_revision='r1',
                  clear_filter={'site': hex(symbols['clear']), 'target': hex(symbols['clear_filter']),
                                'before': clear_original.hex(), 'preserves_only_envelope_rectangles': True},
                  mutation={'site': hex(LEGACY_STORE), 'before': STORE.hex(),
                            'after': expected_branch.hex(), 'stub': hex(stub_start),
                            'return': hex(LEGACY_AFTER),
                            'before_marker': {'x': 1280, 'y': 192, 'width': 96, 'height': 96},
                            'after_marker': {'x': 1384, 'y': 192, 'width': 96, 'height': 96},
                            'original_store': 'str w1, [x0]'},
                  envelope_interpretation={
                      'before_only': 'legacy MMIO store did not return or later execution failed',
                      'before_and_after': 'legacy store returned; failure is later in the path',
                      'neither': 'probe did not reach the envelope or display record failed',
                      'limits': 'markers are framebuffer stores, not a PC trace or a reset-source proof'})
    return report


def write_elf(out, code, symbols):
    assembly = '.text\n.incbin "' + str((out / 'probe.bin').resolve()) + '"\n'
    for name, value in symbols.items():
        assembly += '.global ' + name + '\n'
        assembly += '.set ' + name + ', ' + ('.text + ' + hex(value - base.CAVE)
                                            if base.CAVE <= value < base.CAVE_END else hex(value)) + '\n'
    (out / 'final-probe.S').write_text(assembly, encoding='utf-8', newline='\n')
    (out / 'probe.ld').write_text('SECTIONS { . = %#x; .text : { *(.text*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n' % base.CAVE, encoding='utf-8')
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(out / 'final-probe.S'), '-o', str(out / 'probe.o')], check=True)
    subprocess.run(['ld.lld', '-T', str(out / 'probe.ld'), '-e', 'mmio_envelope', str(out / 'probe.o'), '-o', str(out / 'probe.elf')], check=True)
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
    stub, stub_symbols = assemble_stub(out, symbols['clear'] + 4)
    code += stub
    symbols.update(stub_symbols)
    clear_at = symbols['clear'] - base.CAVE
    assert code[clear_at:clear_at + 4] == bytes.fromhex('3f7d81a8')
    code[clear_at:clear_at + 4] = base.branch(symbols['clear'], symbols['clear_filter'])
    m = base.MachO(control)
    data = bytearray(control)
    at = m.offset(LEGACY_STORE)
    assert data[at:at + 4] == STORE
    data[at:at + 4] = base.branch(LEGACY_STORE, base.CAVE + P2_BYTES)
    start = m.offset(base.CAVE, len(code))
    assert not any(data[start + P2_BYTES:start + len(code)])
    data[start:start + len(code)] = code
    (out / 'probe.bin').write_bytes(code)
    write_elf(out, bytes(code), symbols)
    shutil.copytree(control_dir / 'private', out / 'private')
    kernel = out / 'private/Payloads/kernel.macho'
    kernel.write_bytes(data)
    before_seal = base.sha(data)
    sealed = subprocess.run([sys.executable, str(args.sealer), str(kernel)], check=True, capture_output=True, text=True)
    (out / 'crypto-seal.log').write_text(sealed.stdout + sealed.stderr, encoding='utf-8')
    kernel.with_suffix('.macho.crypto-hmac.json').rename(out / 'crypto-hmac.json')
    report = audit(args.baseline.read_bytes(), kernel.read_bytes(), bytes(code), symbols, args.variant)
    config = (control_dir / 'private/EFI/UniXNU/unixnu.toml').read_text(encoding='utf-8')
    config = config.replace('V23P2', 'V23P4').replace('PSCI prefix checkpoints 1-12', VARIANTS['p4'][1])
    (out / 'private/EFI/UniXNU/unixnu.toml').write_text(config, encoding='utf-8', newline='\n')
    private = out / 'private'
    env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC')
    for path in [private, *private.rglob('*')]:
        os.utime(path, (315532800, 315532800))
    fat = out / 'J716fEntryV23.fat'
    subprocess.run(['mformat', '-C', '-F', '-c', '1', '-T', '131072', '-v', 'V23P4', '-N', '716f0023', '-i', str(fat), '::'], check=True, env=env)
    for name in ['EFI', 'Payloads']:
        subprocess.run(['mcopy', '-i', str(fat), '-s', '-Q', '-m', str(private / name), '::/'], check=True, env=env)
    report.update(symbols=symbols, before_seal_sha256=before_seal,
                  assembly_sha256=base.sha((HERE / 'v23p2-probe.S').read_bytes()),
                  builder_sha256=base.sha(Path(__file__).read_bytes()),
                  sealer_sha256=base.sha(args.sealer.read_bytes()), adt_sha256=base.ADT_SHA,
                  payload_sha256=base.sha(fat.read_bytes()),
                  files={f.relative_to(private).as_posix(): {'bytes': f.stat().st_size, 'sha256': base.sha(f.read_bytes())}
                         for f in sorted(private.rglob('*')) if f.is_file()})
    (out / 'payload.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({k: report[k] for k in ['variant', 'probe_bytes', 'kernel_sha256', 'single_factor_verified']}, indent=2))


if __name__ == '__main__':
    main()
