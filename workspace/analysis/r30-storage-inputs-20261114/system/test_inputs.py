#!/usr/bin/env python3
"""Small host-only rejection tests for the new input transfer boundary."""
import contextlib
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zlib

import prepare_inputs as target


class Inputs(unittest.TestCase):
    def fixture(self, root, compressed=False):
        data = b'original immutable Apple member fixture\x00' * 19
        encode = zlib.compressobj(wbits=-15)
        packed = encode.compress(data) + encode.flush() if compressed else data
        row = dict(name='input.dmg', component='OS', sha256=hashlib.sha256(data).hexdigest(),
                   size=len(data), packed=len(packed), compression=8 if compressed else 0,
                   crc=zlib.crc32(data), data_offset=333)
        meta = dict(url='https://fixture.invalid/test.ipsw', build_manifest_sha256='manifest')
        return data, packed, row, meta

    @contextlib.contextmanager
    def paths(self, root):
        with patch.object(target, 'ARTIFACT', root / 'artifact'), patch.object(target, 'HERE', root / 'report'), patch.object(target, 'CHUNK', 17):
            yield

    def run_case(self, compressed=False, resume=False):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data, packed, row, meta = self.fixture(root, compressed)
            calls = []
            def transfer(url, start, end):
                self.assertEqual(url, meta['url'])
                calls.append((start, end))
                return packed[start - row['data_offset']:end - row['data_offset'] + 1]
            with self.paths(root), patch.object(target, 'checked_range', transfer):
                if resume:
                    partial = root / 'artifact/ipsw/21H461/input.dmg.zip-member.partial'
                    partial.parent.mkdir(parents=True)
                    partial.write_bytes(packed[:11])
                    target.record(partial.with_suffix('.json'), dict(url=meta['url'], member=row, build_manifest_sha256='manifest'))
                result = target.fetch_member(meta, row)
                self.assertEqual(Path(result['path']).read_bytes(), data)
                self.assertEqual(min(start for start, _ in calls), 344 if resume else 333)
                before = len(calls)
                self.assertTrue(target.fetch_member(meta, row)['reused'])
                self.assertEqual(len(calls), before)

    def test_stored_member(self):
        self.run_case()

    def test_deflated_member(self):
        self.run_case(compressed=True)

    def test_resume_stored_member(self):
        self.run_case(resume=True)

    def test_resume_deflated_member(self):
        self.run_case(compressed=True, resume=True)

    def test_corrupt_digest_never_publishes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data, packed, row, meta = self.fixture(root)
            row['sha256'] = '0' * 64
            with self.paths(root), patch.object(target, 'checked_range', lambda u, s, e: packed[s - 333:e - 333 + 1]):
                with self.assertRaisesRegex(ValueError, 'mismatch'):
                    target.fetch_member(meta, row)
                self.assertFalse((root / 'artifact/ipsw/21H461/input.dmg').exists())

    def test_wrong_resume_identity_refused(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            _, _, row, meta = self.fixture(root)
            with self.paths(root), patch.object(target, 'checked_range', side_effect=AssertionError('must fail before network')):
                partial = root / 'artifact/ipsw/21H461/input.dmg.zip-member.partial'
                partial.parent.mkdir(parents=True)
                partial.write_bytes(b'old')
                with self.assertRaisesRegex(ValueError, 'exact source identity'):
                    target.fetch_member(meta, row)

    def test_range_ignored_refused(self):
        class Response:
            status = 200
            def __enter__(self): return self
            def __exit__(self, *args): pass
        with patch.object(target.urllib.request, 'urlopen', return_value=Response()):
            with self.assertRaisesRegex(ValueError, 'ignored Range'):
                target.checked_range('https://fixture.invalid/a', 10, 20)

    def test_wrong_range_refused(self):
        class Response:
            status = 206
            headers = {'Content-Range': 'bytes 0-10/99'}
            def __enter__(self): return self
            def __exit__(self, *args): pass
        with patch.object(target.urllib.request, 'urlopen', return_value=Response()):
            with self.assertRaisesRegex(ValueError, 'wrong Content-Range'):
                target.checked_range('https://fixture.invalid/a', 10, 20)


if __name__ == '__main__':
    unittest.main(verbosity=2)
