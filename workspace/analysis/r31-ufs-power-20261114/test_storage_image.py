#!/usr/bin/env python3
"""Focused rejection tests for the new trust input, menu and boot header checks."""
import argparse
import json
from pathlib import Path
import shutil
import struct
import tempfile
import verify_storage_image as v

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('artifact',type=Path);p.add_argument('--report',type=Path,required=True)
    a=p.parse_args();source=a.artifact.resolve(strict=True)
    phase=Path(__file__).resolve().parent
    cases=[]
    def reject(label,operation):
        try:operation()
        except ValueError as exc:cases.append(dict(name=label,rejected=True,reason=str(exc)))
        else:raise AssertionError('Invalid image accepted: '+label)
    with tempfile.TemporaryDirectory(prefix='image-negative-',dir=phase) as temp:
        d=Path(temp).resolve()
        assert d.is_relative_to(phase) and d!=phase
        for name in ('boot.img','payload.img','SILICIUM_UEFI.fd','firmware.dtb','shim.bin','unixnu.toml',
                     'trustcache-21H461.bin','original-kernel.macho','kernel-expectation.json','baseline.adt','payload.official.json'):
            shutil.copyfile(source/name,d/name)
        shutil.copytree(source/'review-evidence',d/'review-evidence')
        v.verify(d)
        original=(d/'boot.img').read_bytes()
        for label,offset in (('android-os-version',44),('android-name',48),('android-cmdline',64),('android-extra-cmdline',608)):
            changed=bytearray(original);changed[offset]=1
            (d/'boot.img').write_bytes(changed)
            reject(label,lambda:v.verify(d))
        (d/'boot.img').write_bytes(original)
        trust=(d/'trustcache-21H461.bin').read_bytes()
        changed=bytearray(trust);changed[-1]^=1
        (d/'trustcache-21H461.bin').write_bytes(changed)
        reject('archived-trust-substitution',lambda:v.verify(d))
        (d/'trustcache-21H461.bin').write_bytes(trust)
        menu=(d/'unixnu.toml').read_bytes()
        reject('automatic-xnu-timeout',lambda:v.menu_check(menu.replace(b'timeout = -1',b'timeout = 0')))
        reject('wrong-default',lambda:v.menu_check(menu.replace(b'default = "xnu"',b'default = "tools"')))
    report=dict(passed=True,cases=cases,device_actions=False,hardware_tested=False)
    a.report.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(passed=True,rejected=len(cases))))

if __name__=='__main__':main()
