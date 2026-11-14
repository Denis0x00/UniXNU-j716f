#!/usr/bin/env python3
"""Offline format and scope tests; no APFS host execution is simulated as success."""
import contextlib
import hashlib
import io
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest import mock
import uuid

import make_data_seed as create
import verify_data_seed as verify


def fixture():
    container = bytearray(4096)
    container[32:36] = b'NXSB'
    struct.pack_into('<IQ', container, 36, 4096, 3 * verify.GIB // 4096)
    container[:8] = verify.fletcher64(container[8:])
    volume = bytearray(4096)
    volume[32:36] = b'APSB'
    volume[240:256] = uuid.UUID('00000000-0000-4000-8000-000000000001').bytes
    volume[:8] = verify.fletcher64(volume[8:])
    return struct.pack('<8sIIQQQQ', b'DBRDISK1', 1, 4096, 3 * verify.GIB, 1, 0, 8192) + container + volume


class FormatTests(unittest.TestCase):
    def test_valid_structural_fixture(self):
        data = fixture()
        result = verify.inspect_bytes(data)
        self.assertEqual(result['logical_bytes'], 3 * verify.GIB)
        self.assertEqual(result['encoded_sha256'], hashlib.sha256(data).hexdigest())
        self.assertEqual(result['checksummed_volume_superblocks'], 1)

    def test_bad_magic(self):
        with self.assertRaisesRegex(ValueError, 'format'):
            verify.inspect_bytes(b'X' + fixture()[1:])

    def test_trailing_data(self):
        with self.assertRaisesRegex(ValueError, 'Trailing'):
            verify.inspect_bytes(fixture() + b'X')

    def test_out_of_bounds_extent(self):
        data = bytearray(fixture())
        struct.pack_into('<Q', data, 32, 3 * verify.GIB)
        with self.assertRaisesRegex(ValueError, 'extent'):
            verify.inspect_bytes(data)

    def test_corrupt_apfs_superblock(self):
        data = bytearray(fixture())
        data[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'volume checksum'):
            verify.inspect_bytes(data)

    def test_container_geometry(self):
        data = bytearray(fixture())
        struct.pack_into('<Q', data, 48 + 40, 1)
        data[48:56] = verify.fletcher64(data[56:48 + 4096])
        with self.assertRaisesRegex(ValueError, 'geometry'):
            verify.inspect_bytes(data)

    def test_reject_duplicate_checksums(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            line = '0' * 64 + '  data-21H461.dbr\n'
            (root / 'SHA256SUMS').write_text(line + line)
            with self.assertRaisesRegex(ValueError, 'duplicate'):
                verify.verify(root, {})

    def test_artifact_manifest_and_tamper(self):
        data = fixture()
        facts = verify.inspect_bytes(data)
        pins = {'core_commit': 'a' * 40, 'darwinboot_commit': 'b' * 40,
                'build': '21H461', 'version': '17.7.11',
                'os': {'sha256': 'c' * 64}, 'trust_bundle': {'sha256': 'd' * 64}}
        report = {**facts, **{k: pins[k] for k in ('core_commit', 'darwinboot_commit', 'build', 'version')},
                  'logical_gib': 3, 'diskutil_info': {'VolumeUUID': facts['data_volume_uuid']},
                  'inputs': {'os': {'expected_sha256': pins['os']['sha256'], 'body_sha256_verified': True},
                             'trust_bundle': {'sha256': pins['trust_bundle']['sha256'], 'body_sha256_verified': True}}}
        for key in ('first_install', 'preserve_for_future_builds', 'diskutil_verify_volume_passed', 'mounts_detached'):
            report[key] = True
        for key in ('private_device_identity_used', 'installed', 'runtime_tested', 'skip_setup', 'target_apfs_mount_verified'):
            report[key] = False
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'data-21H461.dbr').write_bytes(data)
            content = json.dumps(report).encode()
            (root / 'data-21H461.json').write_bytes(content)
            (root / 'SHA256SUMS').write_text(hashlib.sha256(data).hexdigest() + '  data-21H461.dbr\n' +
                                            hashlib.sha256(content).hexdigest() + '  data-21H461.json\n')
            result = verify.verify(root, pins)
            self.assertFalse(result['apfs_validation_executed_here'])
            self.assertFalse(result['hardware_validated'])
            (root / 'data-21H461.dbr').write_bytes(data + b'X')
            with self.assertRaisesRegex(ValueError, 'Artifact checksum'):
                verify.verify(root, pins)


class ScopeTests(unittest.TestCase):
    def test_initial_network_failure_retries_only_before_creation(self):
        factory = mock.Mock(side_effect=[OSError('TLS EOF'), 'source'])
        with mock.patch.object(create.time, 'sleep') as sleep:
            self.assertEqual(create.open_ipsw(mock.Mock(), factory, 'context', 'https://example.invalid'), 'source')
            self.assertEqual(factory.call_count, 2)
            sleep.assert_called_once_with(1)

    def test_non_network_identity_failure_is_not_retried(self):
        factory = mock.Mock(side_effect=ValueError('Wrong fixed input'))
        with mock.patch.object(create.time, 'sleep') as sleep:
            with self.assertRaisesRegex(ValueError, 'Wrong fixed input'):
                create.open_ipsw(mock.Mock(), factory, 'context', 'https://example.invalid')
            self.assertEqual(factory.call_count, 1)
            sleep.assert_not_called()

    def test_timeout_stops_owned_command_group(self):
        with tempfile.TemporaryDirectory() as directory:
            log = create.Log(Path(directory) / 'test.log', [])
            process = mock.Mock(pid=123456, returncode=-15)
            process.communicate.side_effect = [subprocess.TimeoutExpired('helper', 1), (b'', b'')]
            with mock.patch.object(create.subprocess, 'Popen', return_value=process) as popen, \
                 mock.patch.object(create.os, 'killpg', create=True) as killpg, \
                 mock.patch.object(log, 'write'):
                with self.assertRaises(subprocess.TimeoutExpired):
                    log.command('helper', timeout=1)
                self.assertTrue(popen.call_args.kwargs['start_new_session'])
                killpg.assert_called_once_with(123456, create.signal.SIGTERM)

    def test_cleanup_only_owned_image(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            images = [{'image-path': str(root / 'owned/seed.raw'),
                       'system-entities': [{'dev-entry': '/dev/disk91'}]},
                      {'image-path': str(root / 'another/image.dmg'),
                       'system-entities': [{'dev-entry': '/dev/disk92'}]}]
            with mock.patch.object(create, 'image_entries', side_effect=[images, images[1:]]), \
                 mock.patch.object(create, 'detach') as detach:
                create.cleanup(None, [root / 'owned'])
                detach.assert_called_once_with(None, '/dev/disk91')

    def test_non_macos_stops_before_host_commands(self):
        argv = ['make_data_seed.py', '--core', 'core', '--work', 'work',
                '--output', 'output', '--evidence', 'evidence']
        with mock.patch('sys.argv', argv), mock.patch.object(create.platform, 'system', return_value='Linux'), \
             mock.patch.object(create.subprocess, 'Popen') as run:
            with self.assertRaisesRegex(RuntimeError, 'actual macOS'):
                create.main()
            run.assert_not_called()

    def test_detach_rejects_partition_or_arbitrary_path(self):
        for path in ('/dev/disk91s1', '/dev/rdisk91', '/Volumes/another', 'disk91'):
            with self.assertRaisesRegex(RuntimeError, 'detach'):
                create.detach(None, path)


if __name__ == '__main__':
    unittest.main()
