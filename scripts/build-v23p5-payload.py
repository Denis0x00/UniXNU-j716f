#!/usr/bin/env python3
"""Build matched P5A STR / P5B NOP controls over the exact P4 r1 envelope.

The pair has identical geometry, clear filter and title DSB. Only the relocated
legacy instruction and its crypto HMAC differ in the kernels. No device I/O.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('j716f_p4', HERE/'build-v23p4-payload.py')
p4 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(p4)
base = p4.base
BASELINE_SHA, ADT_SHA = p4.BASELINE_SHA, p4.ADT_SHA
CONTROL_SHA = '70557ad0961d1df6be51a95f171c71d95d85f7167cbeb21c27bbaf775a06b97b'
VARIANTS = {'p5a': (5, 'A: original STR; matched MMIO envelope'),
            'p5b': (5, 'B: skip STR; matched MMIO envelope')}


def audit(original, candidate, code, symbols, variant):
    assert variant in VARIANTS
    assert len(code) == 2108
    m = base.MachO(candidate)
    assert m.hmac_info()['matches']
    start = m.offset(base.CAVE, len(code))
    at = m.offset(symbols['mmio_store'])
    code_at = symbols['mmio_store'] - base.CAVE
    expected = p4.STORE if variant == 'p5a' else p4.NOP
    assert candidate[start:start+len(code)] == code
    assert candidate[at:at+4] == code[code_at:code_at+4] == expected
    restored = bytearray(candidate)
    restored[at:at+4] = p4.STORE
    control_code = bytearray(code)
    control_code[code_at:code_at+4] = p4.STORE
    seal = base.MachO(restored).hmac_info()
    restored[seal['offset']:seal['offset']+32] = bytes.fromhex(seal['computed'])
    assert base.sha(restored) == CONTROL_SHA
    report = p4.audit(original, bytes(restored), bytes(control_code), symbols, 'p4')
    report.update(variant='V23'+variant.upper(), diagnostic_revision='matched-str-nop',
                  kernel_sha256=base.sha(candidate), source_crypto_hmac=m.hmac_info(),
                  matched_control_kernel_sha256=CONTROL_SHA,
                  matched_factor={'site':hex(symbols['mmio_store']), 'original_site':hex(p4.LEGACY_STORE),
                                  'instruction': 'str w1, [x0]' if variant == 'p5a' else 'nop',
                                  'bytes':expected.hex(), 'allowed_delta':'Only this 4-byte instruction and the 32-byte HMAC versus P5A'},
                  single_factor_verified=True, hardware_verified=False,
                  menu_scale=3, menu_scale_hardware_verified=False,
                  envelope_interpretation={
                      'both_markers':'Execution advanced past the factor site; B has no legacy MMIO write there.',
                      'before_only':'No visible after-marker; no PC trace or store-completion proof.',
                      'limits':'Neither missing square 3 nor reboot alone proves a DSB stall, PSCI execution or reset cause.'})
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ['baseline','adt','sealer','output']:
        parser.add_argument('--'+name,type=Path,required=True)
    parser.add_argument('--variant',choices=VARIANTS,required=True)
    args=parser.parse_args()
    out=args.output.resolve()
    out.mkdir(parents=True,exist_ok=False)
    control=out/'control-p4'
    subprocess.run([sys.executable,str(HERE/'build-v23p4-payload.py'),
                    '--baseline',str(args.baseline),'--adt',str(args.adt),'--sealer',str(args.sealer),
                    '--output',str(control),'--variant','p4'],check=True)
    metadata=json.loads((control/'payload.json').read_text())
    symbols=metadata['symbols']
    data=bytearray((control/'private/Payloads/kernel.macho').read_bytes())
    assert base.sha(data)==CONTROL_SHA
    code=bytearray((control/'probe.bin').read_bytes())
    m=base.MachO(data)
    if args.variant=='p5b':
        at=m.offset(symbols['mmio_store'])
        assert data[at:at+4]==p4.STORE
        data[at:at+4]=p4.NOP
        at=symbols['mmio_store']-base.CAVE
        assert code[at:at+4]==p4.STORE
        code[at:at+4]=p4.NOP
    shutil.copytree(control/'private',out/'private')
    kernel=out/'private/Payloads/kernel.macho'
    kernel.write_bytes(data)
    before_seal=base.sha(data)
    sealed=subprocess.run([sys.executable,str(args.sealer),str(kernel)],check=True,capture_output=True,text=True)
    (out/'crypto-seal.log').write_text(sealed.stdout+sealed.stderr,encoding='utf-8')
    kernel.with_suffix('.macho.crypto-hmac.json').rename(out/'crypto-hmac.json')
    (out/'probe.bin').write_bytes(code)
    p4.write_elf(out,bytes(code),symbols)
    report=audit(args.baseline.read_bytes(),kernel.read_bytes(),bytes(code),symbols,args.variant)
    config=(control/'private/EFI/UniXNU/unixnu.toml').read_text(encoding='utf-8')
    tag='V23'+args.variant.upper()
    title=VARIANTS[args.variant][1]
    config=config.replace('V23P4',tag).replace(p4.VARIANTS['p4'][1],title)
    (out/'private/EFI/UniXNU/unixnu.toml').write_text(config,encoding='utf-8',newline='\n')
    private=out/'private'
    for path in [private,*private.rglob('*')]:
        os.utime(path,(315532800,315532800))
    env=dict(os.environ,MTOOLS_SKIP_CHECK='1',TZ='UTC')
    fat=out/'J716fEntryV23.fat'
    subprocess.run(['mformat','-C','-F','-c','1','-T','131072','-v',tag,'-N','716f0023','-i',str(fat),'::'],check=True,env=env)
    for name in ['EFI','Payloads']:
        subprocess.run(['mcopy','-i',str(fat),'-s','-Q','-m',str(private/name),'::/'],check=True,env=env)
    report.update(symbols=symbols,before_seal_sha256=before_seal,
                  assembly_sha256=base.sha((HERE/'v23p2-probe.S').read_bytes()),
                  builder_sha256=base.sha(Path(__file__).read_bytes()),
                  control_builder_sha256=base.sha((HERE/'build-v23p4-payload.py').read_bytes()),
                  sealer_sha256=base.sha(args.sealer.read_bytes()),adt_sha256=ADT_SHA,
                  payload_sha256=base.sha(fat.read_bytes()),
                  files={p.relative_to(private).as_posix():{'bytes':p.stat().st_size,'sha256':base.sha(p.read_bytes())}
                         for p in sorted(private.rglob('*')) if p.is_file()})
    (out/'payload.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:report[k] for k in ['variant','kernel_sha256','single_factor_verified','menu_scale']},indent=2))

if __name__=='__main__':
    main()
