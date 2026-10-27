#!/usr/bin/env python3
"""Compile the actual state writer, decode its fixtures, refuse misleading traces."""
import argparse
import importlib.util
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, ROOT/'scripts'/f'{name}.py')
    value = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(value)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--armcpu-source', type=Path)
    parser.add_argument('--target-cxx', default='clang++')
    args = parser.parse_args()
    checker, patcher = module('check-native-irq-trace'), module('prepare-native-observer')
    evidence = json.loads((ROOT/'scripts/j716f-hardware-evidence.json').read_text(encoding='utf-8'))
    with tempfile.TemporaryDirectory(prefix='j716f-irq-host-') as directory:
        out = Path(directory)
        executable = out/'state-tests'
        subprocess.run([args.cxx, '-std=c++14', '-Wall', '-Wextra', '-Werror', '-O2',
                        str(ROOT/'package/native-observer/StateTests.cpp'), '-o', str(executable)], check=True)
        subprocess.run([str(executable), str(out)], check=True)
        fixtures = sorted(out.glob('*.bin'))
        assert len(fixtures) == 10
        for path in fixtures:
            result = checker.check(path.read_bytes(), evidence)
            assert result['trace_consistent'] == (path.stem == 'complete'), (path.name, result)
            assert not result['hardware_validated']
            if path.stem == 'complete':
                assert result['last_startup_phase'] == 7
                assert result['last_startup_registers']['frequency'] == 19200000
            elif path.stem == 'setup_missing':
                assert not result['startup_phase_bitmap'] & (1 << 4)
        complete = (out/'complete.bin').read_bytes()
        mutations = {'torn_sequence': (3, 1), 'missing_slot': (15, 0xff),
                     'missing_checkpoint': (11, 0), 'wrong_frequency': (52, 24000000),
                     'wrong_affinity': (60, 1), 'bad_return_count': (66, 0),
                     'wrong_irq_phase': (78, 6), 'counter_regression': (69, 1),
                     'header_counter_disagrees': (9, 1), 'reserved_word': (37, 1)}
        for name, (word, value) in mutations.items():
            data = bytearray(complete)
            struct.pack_into('<Q', data, word*8, value)
            assert not checker.check(data, evidence)['trace_consistent'], name
        for bad in (complete[:-1], b'\0'*len(complete)):
            try:
                checker.check(bad, evidence)
            except ValueError:
                pass
            else:
                raise AssertionError('Bad ABI accepted')
        # Assemble actual production sysreg accesses for AArch64 as well as
        # executing the architecture-independent state writer on the host.
        target_source = out/'target.cpp'
        target_source.write_text('''#include <stdint.h>
volatile uint32_t armcpu_registered, armcpu_gic_ready, armcpu_irq_id, armcpu_irq_phase;
volatile uint32_t armcpu_gic_boot[8], armcpu_quiesce_calls, armcpu_halt_calls;
volatile uint64_t armcpu_timer_interrupts;
#include "J716fArmCpuObserver.inc"
extern "C" void exercise_observer() {
  j716fArmCpuSetup(1, 0);
  j716fArmCpuEvent(1, 0, 27);
}
''', encoding='utf-8')
        subprocess.run([args.target_cxx, '--target=aarch64-none-elf', '-ffreestanding',
                        '-std=c++14', '-Wall', '-Wextra', '-Werror', '-O2',
                        '-mgeneral-regs-only', '-I', str(ROOT/'package/native-observer'),
                        '-c', str(target_source), '-o', str(out/'target.o')], check=True)
        source = args.armcpu_source.read_bytes() if args.armcpu_source else urllib.request.urlopen(
            f'https://raw.githubusercontent.com/UniXNU/ArmCpu/{patcher.ARMCPU_COMMIT}/ArmCpu.cpp', timeout=45).read()
        patched = patcher.instrument(source)
        assert patched.count(b'rtclock_intr (0);') == source.count(b'rtclock_intr (0);') == 1
        for content in (source, patched):
            dispatch = content.split(b'void ArmCpu::interrupt (void)', 1)[1].split(b'void ArmCpu::signalCPU', 1)[0]
            assert dispatch.count(b'msr icc_eoir1_el1, %0; isb') == 1
            assert dispatch.index(b'rtclock_intr (0);') < dispatch.index(b'msr icc_eoir1_el1, %0; isb')
        for bad in (source+b'\n', patched):
            try:
                patcher.instrument(bad)
            except ValueError:
                pass
            else:
                raise AssertionError('Unreviewed source accepted')
        # Real install must preserve the fetched source and reject a second install.
        source_dir = out/'source'
        source_dir.mkdir()
        (source_dir/'ArmCpu.cpp').write_bytes(source)
        tree = out/'work/tree'
        (tree/'drivers').mkdir(parents=True)
        (tree/'drivers/ArmCpu').symlink_to(source_dir, target_is_directory=True)
        report = patcher.install(ROOT, tree, source_dir, patcher.ARMCPU_COMMIT)
        assert (source_dir/'ArmCpu.cpp').read_bytes() == source
        assert (tree/'drivers/ArmCpu/ArmCpu.cpp').read_bytes() == patched
        assert not report['hardware_validated']
        try:
            patcher.install(ROOT, tree, source_dir, patcher.ARMCPU_COMMIT)
        except ValueError:
            pass
        else:
            raise AssertionError('Overwrite accepted')
    print('PASS: 10 compiled-writer fixtures, 10 decoder mutations, AArch64 sysreg assembly, ABI/source/refusal/install audits; host only')


if __name__ == '__main__':
    main()
