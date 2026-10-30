#!/usr/bin/env python3
"""Verify actual Mu QR inputs before building; read-only, no dependency changes."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

def check(manifest, mu):
    report=json.loads(Path(manifest).read_text())
    if not report['export_init'] or not report['qr_inputs']:
        raise ValueError('Explicit RAW export/init manifest required')
    mu=Path(mu)
    actual=subprocess.check_output(['git','-C',str(mu),'rev-parse','HEAD'],text=True).strip()
    if actual!=report['qr_mu_commit']:
        raise ValueError('Unreviewed Mu dependency revision')
    for name,digest in report['qr_inputs'].items():
        if hashlib.sha256((mu/'MsGraphicsPkg'/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest()!=digest:
            raise ValueError('Unreviewed QR source: '+name)
    print('PASS: actual Mu QR source/header/INF identity; does not qualify unrelated shared dependencies')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--manifest',type=Path,required=True)
    p.add_argument('--mu',type=Path,required=True);a=p.parse_args();check(a.manifest,a.mu)
