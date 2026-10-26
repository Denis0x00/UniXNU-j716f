#!/usr/bin/env python3
"""Pinned pre-MMU terminal prefix controls; no device access or crypto bypass."""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('base', HERE / 'build-v23-payload.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
STACK = 0xfffffff007be8000
HOOKS = {
    'a': (0xfffffff0073fe32c, '010000b9', 'before BCM STR'),
    'b': (0xfffffff0073fe330, 'df3f03d5', 'after BCM STR, before ISB'),
    'c': (base.MMU, '001018d5', 'before SCTLR, no console mapping'),
}


def audit(original, candidate, code, symbols, mode):
    assert base.sha(original) == base.BASELINE_SHA and len(original) == len(candidate)
    m = base.MachO(original)
    assert candidate[:m.header_end] == original[:m.header_end]
    cave = m.offset(base.CAVE, base.CAVE_END - base.CAVE)
    assert not any(original[cave:cave + base.CAVE_END - base.CAVE])
    assert len(code) <= base.CAVE_END - base.CAVE
    assert symbols['stage1'] == base.CAVE
    assert candidate[cave:cave + len(code)] == code
    masked = bytearray(candidate)
    masked[cave:cave + len(code)] = original[cave:cave + len(code)]
    hook, displaced, description = HOOKS[mode]
    assert original[m.offset(hook):m.offset(hook) + 4].hex() == displaced
    assert original[m.offset(base.ENTRY):m.offset(base.ENTRY) + 4] == base.branch(base.ENTRY, base.FIRST)
    for site, target in [(base.ENTRY, symbols['stage1']), (hook, symbols['prefix_stop'])]:
        at = m.offset(site)
        assert candidate[at:at + 4] == base.branch(site, target)
        masked[at:at + 4] = original[at:at + 4]
    seal = base.MachO(candidate).hmac_info()
    assert seal['matches']
    at = seal['offset']
    masked[at:at + 32] = original[at:at + 32]
    assert masked == original, 'Change outside cave, entry/terminal branches or HMAC'
    if mode != 'a':
        at = m.offset(HOOKS['a'][0])
        assert candidate[at:at + 4] == original[at:at + 4]
    # Actual instruction tests must establish reachability. This byte check also
    # forbids adding an SCTLR write or a counter read to the terminal probe.
    assert bytes.fromhex('001018d5') not in code
    assert bytes.fromhex('35e03bd5') not in code and bytes.fromhex('55e03bd5') not in code
    return dict(version='V23P10' + mode.upper(), mode=mode,
                kernel_sha256=base.sha(candidate), baseline_sha256=base.BASELINE_SHA,
                probe_bytes=len(code), probe_capacity=base.CAVE_END - base.CAVE,
                changes_confined=True, source_crypto_hmac=seal,
                entry_site=hex(base.ENTRY), terminal_site=hex(hook),
                displaced_instruction_hex=displaced, terminal_description=description,
                record_phases=[9, 10, 11, 14], phase11_value='actual SCTLR_EL1 with M=0',
                phase11_scope='named terminal prefix checkpoint; no P9M map_console',
                console_base='0xb00be000', console_bytes='0x40000',
                normal_kernel_continuation=False, mmu_enable_attempted=False,
                counter_used=False, mapping_setup_used=False,
                original_bcm_store_executed=(mode != 'a'),
                automatic_terminal_system_reset=True, hardware_verified=False)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ['baseline', 'adt', 'sealer', 'output']:
        p.add_argument('--' + name, type=Path, required=True)
    p.add_argument('--mode', choices=HOOKS, required=True)
    a = p.parse_args()
    original, adt = a.baseline.read_bytes(), a.adt.read_bytes()
    assert base.sha(original) == base.BASELINE_SHA and base.sha(adt) == base.ADT_SHA
    assert base.MachO(original).hmac_info()['matches']
    assembly = (HERE / 'v23p10-prefix.S').read_text()
    old = (HERE / 'v23p9-trace.S').read_text()
    assert assembly[assembly.index('.macro CRC'):] == old[old.index('.macro CRC'):], 'Preserve proven writer source'
    out = a.output
    out.mkdir(parents=True, exist_ok=False)
    (out / 'probe.ld').write_text('SECTIONS { . = %#x; .text : { *(.text*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n' % base.CAVE)
    subprocess.run(['clang', '--target=aarch64-none-elf', '-c', str(HERE / 'v23p10-prefix.S'), '-o', str(out / 'probe.o')], check=True)
    subprocess.run(['ld.lld', '-T', str(out / 'probe.ld'), '--defsym=original_first=' + hex(base.FIRST), '--defsym=original_stack=' + hex(STACK), '-e', 'stage1', str(out / 'probe.o'), '-o', str(out / 'probe.elf')], check=True)
    subprocess.run(['llvm-objcopy', '-O', 'binary', '-j', '.text', str(out / 'probe.elf'), str(out / 'probe.bin')], check=True)
    symbols = {v[2]: int(v[0], 16) for line in subprocess.check_output(['llvm-nm', '--defined-only', '-n', str(out / 'probe.elf')], text=True).splitlines() if len(v := line.split()) == 3}
    code = (out / 'probe.bin').read_bytes()
    assert len(code) <= base.CAVE_END - base.CAVE
    data = bytearray(original)
    m = base.MachO(original)
    at = m.offset(base.CAVE, len(code))
    assert not any(data[at:at + len(code)])
    data[at:at + len(code)] = code
    for site, target in [(base.ENTRY, symbols['stage1']), (HOOKS[a.mode][0], symbols['prefix_stop'])]:
        at = m.offset(site)
        data[at:at + 4] = base.branch(site, target)
    kernel = out / 'kernel.macho'
    kernel.write_bytes(data)
    r = subprocess.run([sys.executable, str(a.sealer), str(kernel)], check=True, capture_output=True, text=True)
    (out / 'crypto-seal.log').write_text(r.stdout + r.stderr)
    kernel.with_suffix('.macho.crypto-hmac.json').rename(out / 'crypto-hmac.json')
    (out / 'kernel.macho.adt').write_bytes(adt)
    report = audit(original, kernel.read_bytes(), code, symbols, a.mode)
    report.update(symbols=symbols, assembly_sha256=base.sha((HERE / 'v23p10-prefix.S').read_bytes()),
                  builder_sha256=base.sha(Path(__file__).read_bytes()), adt_sha256=base.sha(adt),
                  sealer_sha256=base.sha(a.sealer.read_bytes()),
                  writer_source_matches_p9=True)
    (out / 'kernel.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({k: report[k] for k in ['version', 'probe_bytes', 'terminal_site', 'kernel_sha256']}))


if __name__ == '__main__':
    main()
