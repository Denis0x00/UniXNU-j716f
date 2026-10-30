#!/usr/bin/env python3
"""Test real C transfer/init helpers against independent Python, without hardware."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import random
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch
import zlib
from native_export import Capture, encode, init_eligibility, publish, receipt

ROOT = Path(__file__).resolve().parents[1]
ID = struct.pack('<4Q', 1, 2, 3, 4).hex()
RAW = struct.pack('<3I', 0x43474244, 0x2a354, 0x3fff4) + bytes((i*17+(i>>8)) & 255 for i in range(12, 0x40000))


class TransportTests(unittest.TestCase):
    def complete(self):
        cap = Capture(ID)
        frames = list(encode(RAW, ID, 123))
        random.Random(7).shuffle(frames)
        for frame in frames+frames[:20]:
            cap.add(frame)
        return cap

    def test_complete_reordered_duplicates(self):
        self.assertEqual(self.complete().reconstruct(), RAW)

    def test_wrong_candidate_missing_bad_crc_noncanonical(self):
        frames = list(encode(RAW, ID, 123))
        for frame in [frames[0].lower(), frames[0]+' ', frames[0][:-1]+('1' if frames[0][-1]!='1' else '2')]:
            with self.assertRaises(ValueError):
                Capture(ID).add(frame)
        with self.assertRaises(ValueError):
            Capture('00'*32).add(frames[0])
        cap=Capture(ID);cap.add(frames[0])
        with self.assertRaises(ValueError):
            cap.reconstruct()

    def test_mixed_transfer_or_data(self):
        original = next(encode(RAW, ID, 123))
        for frame in [next(encode(RAW, ID, 124)), next(encode(RAW[:-1]+b'Q', ID, 123))]:
            cap=Capture(ID);cap.add(original)
            with self.assertRaises(ValueError):
                cap.add(frame)

    def test_tamper_with_recomputed_frame_crc(self):
        cap=Capture(ID)
        for i,frame in enumerate(encode(RAW, ID, 123)):
            if i==2:
                pieces=frame.split('/');pieces[7]='00'+pieces[7][2:]
                prefix='/'.join(pieces[:-1])+'/'
                frame=prefix+f'{zlib.crc32(prefix.encode()):08X}'
            cap.add(frame)
        with self.assertRaises(ValueError):
            cap.reconstruct()

    def test_publish_and_no_clobber(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)/'evidence'
            result=publish(self.complete(),out)
            self.assertEqual((out/'raw-console.bin').read_bytes(),RAW)
            self.assertEqual(result['initialization_receipt'],receipt(ID,hashlib.sha256(RAW).hexdigest()))
            with self.assertRaises(FileExistsError):
                publish(self.complete(),out)

    def test_incomplete_never_gets_receipt(self):
        with tempfile.TemporaryDirectory() as folder:
            out=Path(folder)/'evidence';cap=Capture(ID);cap.add(next(encode(RAW,ID,123)))
            with self.assertRaises(ValueError):
                publish(cap,out)
            self.assertTrue((out/'frames.txt').exists())
            self.assertFalse((out/'raw-console.bin').exists())
            self.assertFalse((out/'receipt.json').exists())

    def test_disk_failure_never_emits_receipt(self):
        with tempfile.TemporaryDirectory() as folder, patch('native_export.os.fsync',side_effect=OSError('fixture disk failure')):
            out=Path(folder)/'evidence'
            with self.assertRaises(OSError):
                publish(self.complete(),out)
            self.assertFalse((out/'receipt.json').exists())

    def test_invalid_header_still_exports_but_no_init(self):
        raw=bytes(0x40000);cap=Capture(ID)
        for frame in encode(raw,ID,123):
            cap.add(frame)
        with tempfile.TemporaryDirectory() as folder:
            result=publish(cap,Path(folder)/'evidence')
            self.assertTrue(result['complete'])
            self.assertIsNone(result['initialization_receipt'])

    def test_pending_arm_any_candidate_blocks(self):
        raw=bytearray(RAW);off=0x1000;h=bytearray(128)
        struct.pack_into('<QII',h,0,0x31474f4c4e36314a,1,8192)
        h[16:48]=bytes.fromhex('ab'*32)
        struct.pack_into('<QI',h,48,42,off)
        struct.pack_into('<II',h,60,zlib.crc32(h[:60]),0x414c4e4a)
        raw[off:off+128]=h
        self.assertFalse(init_eligibility(raw)[0])
        struct.pack_into('<I',raw,off+64,0x554c4e4a)
        self.assertTrue(init_eligibility(raw)[0])


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cc',default='cc');args=p.parse_args()
    with tempfile.TemporaryDirectory(prefix='native-export-test-') as folder:
        work=Path(folder);binary=work/'export-tests'
        subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror','-O2',
            str(ROOT/'package/native-log/ExportTests.c'),'-o',str(binary)],check=True)
        output=subprocess.check_output([str(binary),str(work)],text=True)
        cap=Capture(ID)
        c_frames=(work/'frames.txt').read_text().splitlines()
        for frame in c_frames:
            cap.add(frame)
        assert cap.reconstruct()==RAW==(work/'raw.bin').read_bytes()
        assert c_frames==list(encode(RAW,ID,123))
        assert 'RECEIPT '+receipt(ID,hashlib.sha256(RAW).hexdigest()) in output
        data=bytes((i*31+(i>>8)) & 255 for i in range(1000000))
        for line in output.splitlines():
            if line.startswith('SHA '):
                _,length,digest=line.split()
                assert hashlib.sha256(data[:int(length)]).hexdigest().upper()==digest
        print(output.splitlines()[-1])
    suite=unittest.defaultTestLoader.loadTestsFromTestCase(TransportTests)
    if not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful():
        raise SystemExit(1)
    print('PASS: C/Python all 256 frames byte-identical; 12 SHA vectors; offline capture/durability/refusal tests. HOST ONLY.')


if __name__=='__main__':
    main()
