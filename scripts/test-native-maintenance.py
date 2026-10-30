#!/usr/bin/env python3
"""Exercise production C and an independent offline v2 decoder, without devices."""
import argparse
import hashlib
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
from native_maintenance import Capture, encode, publish, receipt, init_eligibility, classify, decode_init
from native_export import encode as encode_v1

ROOT=Path(__file__).resolve().parents[1]
CANDIDATE=struct.pack('<4Q',1,2,3,4).hex()
PROFILE='01'+'00'*31
RAW=struct.pack('<3I',0x43474244,0x3f56e,0x3f56e)+bytes((i*17+(i>>8))&255 for i in range(12,0x40000))

class Tests(unittest.TestCase):
    def capture(self):
        c=Capture(CANDIDATE,PROFILE)
        for f in reversed(list(encode(RAW,CANDIDATE,PROFILE,123))):c.add(f)
        return c

    def test_complete_and_publish(self):
        c=self.capture();self.assertEqual(c.reconstruct(),RAW)
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'new';r=publish(c,out)
            self.assertEqual((out/'raw-console.bin').read_bytes(),RAW)
            self.assertTrue(r['initialization_eligible_by_host_policy'])
            self.assertEqual(r['initialization_receipt'],receipt(CANDIDATE,hashlib.sha256(RAW).hexdigest(),PROFILE,123))
            with self.assertRaises(FileExistsError):publish(c,out)

    def test_old_version_and_mixed_invocation_profile(self):
        c=self.capture()
        bad=[next(encode_v1(RAW,CANDIDATE,123)),next(encode(RAW,CANDIDATE,PROFILE,124)),
             next(encode(RAW,CANDIDATE,'02'+'00'*31,123)),next(encode(RAW[:-1]+b'z',CANDIDATE,PROFILE,123))]
        for f in bad:
            with self.assertRaises(ValueError):c.add(f)
        f=next(encode(RAW,CANDIDATE,PROFILE,123))
        for b in (f.lower(),f+' ',f[:-1]+('0' if f[-1]!='0' else '1')):
            with self.assertRaises(ValueError):c.add(b)

    def test_missing_and_disk_error_no_receipt(self):
        with tempfile.TemporaryDirectory() as d:
            out=Path(d)/'partial';c=Capture(CANDIDATE,PROFILE);c.add(next(encode(RAW,CANDIDATE,PROFILE,123)))
            with self.assertRaises(ValueError):publish(c,out)
            self.assertFalse((out/'receipt.json').exists())
            with patch('native_export.os.fsync',side_effect=OSError('disk error')):
                with self.assertRaises(OSError):publish(self.capture(),Path(d)/'failed')
            self.assertFalse((Path(d)/'failed/receipt.json').exists())

    def test_space_and_suspect(self):
        self.assertEqual(classify(RAW),(3,2688))
        raw=bytearray(RAW);struct.pack_into('<II',raw,4,0x3dff4,0x3dff4)
        self.assertEqual(init_eligibility(raw),(False,'SPACE_AVAILABLE'))
        raw=bytearray(RAW);struct.pack_into('<Q',raw,len(raw)-8,0x31474f4c4e36314a)
        self.assertEqual(init_eligibility(raw),(False,'SUSPECT_SESSION'))

def main():
    p=argparse.ArgumentParser();p.add_argument('--cc',default='cc');p.add_argument('--python-only',action='store_true');a=p.parse_args()
    if not a.python_only:
        with tempfile.TemporaryDirectory(prefix='native-maintenance-') as d:
            d=Path(d);binary=d/'check'
            # Match EDK2's raw-buffer aliasing model for the firmware ABI helpers.
            subprocess.run([a.cc,'-std=c11','-Wall','-Wextra','-Werror','-O2','-fno-strict-aliasing',str(ROOT/'package/native-log/MaintenanceTests.c'),'-o',str(binary)],check=True)
            output=subprocess.check_output([str(binary),str(d)],text=True)
            assert (d/'raw.bin').read_bytes()==RAW
            assert (d/'classes.bin').read_bytes()==bytes(2 if n==0x3fff4 else 4 if n<=0x3dff4 else 3 for n in range(0x3fff5))
            assert (d/'frames.txt').read_text().splitlines()==list(encode(RAW,CANDIDATE,PROFILE,123))
            assert 'RECEIPT '+receipt(CANDIDATE,hashlib.sha256(RAW).hexdigest(),PROFILE,123) in output
            decoded=decode_init((d/'initialized.bin').read_bytes(),CANDIDATE)
            assert decoded['classification']==3 and decoded['transfer_id']=='000000000000007B'
            print(output.splitlines()[-1])
    if not unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(Tests)).wasSuccessful():
        raise SystemExit(1)
    print('PASS maintenance v2 HOST tests; no native hardware claim')

if __name__=='__main__':main()
