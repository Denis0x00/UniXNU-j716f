#!/usr/bin/env python3
"""Build terminal J716F V23 diagnostics from a pinned local kernel copy.

No device access. Cross tools and mtools are required. P=PSCI, C=CNTPCT,
V=CNTVCT, M=direct post-MMU framebuffer evidence. Never disables crypto POST.
"""
import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

BASELINE_SHA = 'cab8f385a44d21fc6f455cb501227452212ba44d34a9fa03d04fcf7e85324214'
ADT_SHA = 'b68fc49b2d9bb6b0a387e59a7816fd4848dffc3bde706a439cbbff3cbebb1aa7'
ENTRY = 0xfffffff0073fc488
FIRST = 0xfffffff0073fe05c
MMU = 0xfffffff0073fe374
VECTOR = 0xfffffff0073f9000
CAVE = 0xfffffff0073fc500
CAVE_END = 0xfffffff0073fcff0
VARIANTS = {'p': (1, 'PSCI reset without counter'), 'c': (2, 'CNTPCT bounded sampling'),
            'v': (3, 'CNTVCT bounded sampling'), 'm': (4, 'Direct MMU-on screen record')}
sha = lambda data: hashlib.sha256(data).hexdigest()


class MachO:
    def __init__(self, data):
        assert struct.unpack_from('<4I', data) == (0xfeedfacf, 0x100000c, 0, 2)
        self.data, self.segments, self.sections = data, [], []
        pos = 32
        for _ in range(struct.unpack_from('<I', data, 16)[0]):
            cmd, length = struct.unpack_from('<II', data, pos)
            if cmd == 0x19:
                name, va, size, off, count = struct.unpack_from('<16s4Q', data, pos + 8)
                self.segments.append((name.rstrip(b'\0'), va, size, off, count))
                for i in range(struct.unpack_from('<I', data, pos + 64)[0]):
                    n, s, a, z, f = struct.unpack_from('<16s16sQQI', data, pos + 72 + 80*i)
                    self.sections.append((n.rstrip(b'\0'), s.rstrip(b'\0'), a, z, f))
            if cmd == 5:
                assert struct.unpack_from('<Q', data, pos + 272)[0] == ENTRY
            pos += length
        self.header_end = pos

    def offset(self, address, size=4):
        found = [f + address - a for _, a, z, f, count in self.segments
                 if a <= address and address + size <= a + min(z, count)]
        assert len(found) == 1, hex(address)
        return found[0]

    def hmac_info(self):
        assert b'_source_corecrypto14_bootstrap\0' in self.data
        text = next(s for s in self.sections if s[0] == b'__text' and s[1] in (b'__TEXT', b'__TEXT_EXEC'))
        fields = [s for s in self.sections if s[0] == b'__fips_hmacs']
        assert len(fields) == 1 and fields[0][3] == 32
        base = next(a for n,a,_,_,_ in self.segments if n == b'__TEXT')
        assert text[2] - base == text[4]
        field = fields[0][4]
        assert not (text[4] < field + 32 and field < text[4] + text[3])
        computed = hmac.digest(b'\0', self.data[text[4]:text[4]+text[3]], 'sha256')
        return {'offset': field, 'computed': computed.hex(),
                'stored': self.data[field:field+32].hex(),
                'matches': computed == self.data[field:field+32]}


def branch(site, target):
    assert (target-site) % 4 == 0 and -(1 << 27) <= target-site < (1 << 27)
    return struct.pack('<I', 0x14000000 | (((target-site)//4) & 0x3ffffff))


def audit(original, candidate, code, symbols, variant):
    assert sha(original) == BASELINE_SHA and len(original) == len(candidate)
    m = MachO(original)
    assert candidate[:m.header_end] == original[:m.header_end]
    start = m.offset(CAVE, CAVE_END-CAVE)
    assert not any(original[start:start+CAVE_END-CAVE])
    assert len(code) <= CAVE_END-CAVE
    assert symbols['stage1'] == CAVE and symbols['pre'] == CAVE+0x60
    assert sha(code[:0x60]) == 'fdc5c49f2010e6b57937ab639984747a9f035ee20559d48a1e9b73fc30111593'
    assert candidate[start:start+len(code)] == code
    masked = bytearray(candidate)
    masked[start:start+len(code)] = original[start:start+len(code)]
    for site,target in [(ENTRY,CAVE),(MMU,symbols['pre'])]:
        at = m.offset(site)
        assert candidate[at:at+4] == branch(site,target)
        masked[at:at+4] = original[at:at+4]
    assert original[m.offset(ENTRY):m.offset(ENTRY)+4] == branch(ENTRY,FIRST)
    assert struct.unpack_from('<I',original,m.offset(MMU))[0] == 0xd5181000
    for i in range(16):
        site = VECTOR+i*0x80
        at = m.offset(site,8)
        first, second = struct.unpack_from('<II', original, at)
        assert first == 0xd503201f or first >> 26 == 0b000101
        assert second == 0xd503201f
        expected = struct.pack('<I',0x5280001b | (i << 5)) + branch(site+4,symbols['exception'])
        assert candidate[at:at+8] == expected
        masked[at:at+8] = original[at:at+8]
    integrity = MachO(candidate).hmac_info()
    assert integrity['matches'], 'Final source-crypto HMAC must match'
    at = integrity['offset']
    masked[at:at+32] = original[at:at+32]
    assert masked == original, 'Unapproved change outside probe/vector/HMAC sites'
    smc = bytes.fromhex('030000d4')
    pct = bytes.fromhex('35e03bd5')  # mrs x21,cntpct_el0
    vct = bytes.fromhex('55e03bd5')  # mrs x21,cntvct_el0
    if variant == 'p':
        assert code.count(smc) == 1 and pct not in code and vct not in code
    else:
        assert smc not in code
    if variant == 'm':
        assert pct not in code and vct not in code
        post = symbols['post']-CAVE
        assert code[post-8:post] == bytes.fromhex('001018d5df3f03d5')  # msr sctlr; isb
    return {'variant': 'V23'+variant.upper(), 'kernel_sha256':sha(candidate),
            'baseline_sha256':BASELINE_SHA,'changes_confined':True,
            'probe_bytes':len(code),'probe_capacity':CAVE_END-CAVE,
            'exception_vectors':16,'source_crypto_hmac':integrity,
            'counter_loop_bound':0x100000 if variant in ('c','v') else None,
            'psci_function':'0x84000009' if variant=='p' else None,
            'uses_counter':variant in ('c','v'),'uses_psci':variant=='p',
            'direct_mmu_display_mapping':variant=='m','normal_kernel_continuation':False,
            'hardware_verified':False}


def main():
    p=argparse.ArgumentParser(description=__doc__)
    for name in ('baseline','adt','sealer','output'):
        p.add_argument('--'+name,type=Path,required=True)
    p.add_argument('--variant',choices=list(VARIANTS),required=True)
    p.add_argument('--assembly',type=Path,default=Path(__file__).with_name('v23-probe.S'))
    a=p.parse_args()
    original=a.baseline.read_bytes()
    adt=a.adt.read_bytes()
    assert sha(original)==BASELINE_SHA and sha(adt)==ADT_SHA
    assert MachO(original).hmac_info()['matches']
    a.output.mkdir(parents=True,exist_ok=False)
    out=a.output
    mode,title=VARIANTS[a.variant]
    linker=out/'probe.ld'
    linker.write_text('SECTIONS { . = %#x; .text : { *(.text*) } /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) } }\n'%CAVE)
    subprocess.run(['clang','--target=aarch64-none-elf','-DMODE='+str(mode),'-c',str(a.assembly),'-o',str(out/'probe.o')],check=True)
    subprocess.run(['ld.lld','-T',str(linker),'--defsym=original_first='+hex(FIRST),'-e','stage1',str(out/'probe.o'),'-o',str(out/'probe.elf')],check=True)
    subprocess.run(['llvm-objcopy','-O','binary','-j','.text',str(out/'probe.elf'),str(out/'probe.bin')],check=True)
    symbols={parts[2]:int(parts[0],16) for line in subprocess.check_output(['llvm-nm','--defined-only','-n',str(out/'probe.elf')],text=True).splitlines() if len(parts:=line.split())==3}
    code=(out/'probe.bin').read_bytes()
    assert symbols['stage1']==CAVE and len(code)<=CAVE_END-CAVE
    m=MachO(original)
    data=bytearray(original)
    start=m.offset(CAVE,CAVE_END-CAVE)
    assert not any(data[start:start+CAVE_END-CAVE])
    data[start:start+len(code)]=code
    for site,target in [(ENTRY,CAVE),(MMU,symbols['pre'])]:
        at=m.offset(site);data[at:at+4]=branch(site,target)
    for i in range(16):
        site=VECTOR+i*0x80;at=m.offset(site,8)
        data[at:at+8]=struct.pack('<I',0x5280001b|(i<<5))+branch(site+4,symbols['exception'])
    private=out/'private'
    (private/'Payloads').mkdir(parents=True)
    (private/'EFI/UniXNU').mkdir(parents=True)
    kernel=private/'Payloads/kernel.macho'
    kernel.write_bytes(data)
    before_seal=sha(data)
    sealed=subprocess.run([sys.executable,str(a.sealer),str(kernel)],check=True,capture_output=True,text=True)
    (out/'crypto-seal.log').write_text(sealed.stdout+sealed.stderr)
    report=audit(original,kernel.read_bytes(),code,symbols,a.variant)
    report.update({'symbols':symbols,'before_seal_sha256':before_seal,
                   'assembly_sha256':sha(a.assembly.read_bytes()),'sealer_sha256':sha(a.sealer.read_bytes())})
    (private/'Payloads/kernel.macho.adt').write_bytes(adt)
    tag='V23'+a.variant.upper()
    config='''version = 1
[boot]
timeout = -1
fallback_timeout = -1
rotate = 0
connect_all = false
default = "entry"
[kits]
autoload = false
[ui]
title = "J716F TAG - TITLE"
[[entry]]
id = "entry"
title = "TAG: TITLE"
type = "xnu"
path = "\\\\Payloads\\\\kernel.macho"
args = "-v serial=0 debug=0x14e cpus=1 -no_slto_panic"
[[entry]]
id = "reset"
title = "Restart"
type = "action"
action = "reset"
'''.replace('TAG',tag).replace('TITLE',title)
    (private/'EFI/UniXNU/unixnu.toml').write_text(config,newline='\n')
    # Keep the sealer report local, outside the FAT's deliberately small file set.
    kernel.with_suffix('.macho.crypto-hmac.json').rename(out/'crypto-hmac.json')
    env=dict(os.environ,MTOOLS_SKIP_CHECK='1',TZ='UTC')
    for item in [private,*private.rglob('*')]:os.utime(item,(315532800,315532800))
    fat=out/'J716fEntryV23.fat'
    subprocess.run(['mformat','-C','-F','-c','1','-T','131072','-v',tag,'-N','716f0023','-i',str(fat),'::'],check=True,env=env)
    for name in ('EFI','Payloads'):
        subprocess.run(['mcopy','-i',str(fat),'-s','-Q','-m',str(private/name),'::/'],check=True,env=env)
    report.update({'payload_sha256':sha(fat.read_bytes()),'adt_sha256':ADT_SHA,
       'files':{f.relative_to(private).as_posix():{'bytes':f.stat().st_size,'sha256':sha(f.read_bytes())} for f in sorted(private.rglob('*')) if f.is_file()}})
    (out/'payload.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('variant','probe_bytes','kernel_sha256','payload_sha256')},indent=2))


if __name__=='__main__':main()
