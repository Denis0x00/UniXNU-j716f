#!/usr/bin/env python3
"""V23P2 prefix isolation using the existing pinned baseline and reseal guards."""
import importlib.util
from pathlib import Path
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('j716f_v23_base', HERE / 'build-v23-payload.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
VARIANTS = {'p2': (5, 'PSCI prefix checkpoints 1-12')}
BASELINE_SHA, ADT_SHA = base.BASELINE_SHA, base.ADT_SHA
base_audit = base.audit
CHECKPOINTS = {
    1: 'Before title renderer call', 2: 'Title stores complete; before original DSB',
    3: 'After original title DSB; before RET', 4: 'Title call returned; before CurrentEL',
    5: 'CurrentEL read completed', 6: 'EL renderer returned; before SCTLR read',
    7: 'SCTLR read completed', 8: 'SCTLR renderer returned',
    9: 'FUNC renderer returned', 10: 'A0 renderer returned; before SMC argument setup',
    11: 'SMC returned; x0 retained in x21', 12: 'Exception entry before register reads',
}


def audit(original, candidate, code, symbols, variant):
    assert variant == 'p2'
    report = base_audit(original, candidate, code, symbols, 'p')
    assert all('prefix_' + str(n) in symbols for n in CHECKPOINTS)
    assert symbols['prefix_2'] < symbols['prefix_3']
    report.update(variant='V23P2', display_mode=5,
                  checkpoints={str(k): v for k, v in CHECKPOINTS.items()},
                  raw_checkpoint_geometry={'x': 1280, 'y': 32, 'pitch': 104, 'width': 96, 'height': 96},
                  checkpoint_limits='Inline stores without barriers; absence alone does not prove non-execution.')
    return report


def main():
    base.VARIANTS = VARIANTS
    base.audit = audit
    if '--assembly' not in sys.argv:
        sys.argv += ['--assembly', str(HERE / 'v23p2-probe.S')]
    base.main()


if __name__ == '__main__':
    main()
