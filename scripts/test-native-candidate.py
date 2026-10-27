#!/usr/bin/env python3
"""Synthetic provenance/Mach-O/policy refusal checks; no device evidence."""
import copy
import hmac
import io
import json
from pathlib import Path
import struct
import tempfile
import tomllib
import unittest
import zipfile

from native_candidate import (BCM_ROUTE, HERE, KERNEL, REQUIRED, MachO, check_board,
                              check_kernel, check_report, load_module, sha)


def fixture():
    base = 0xfffffff007000000
    data = bytearray(0x5000)
    commands = []
    sections = [(b'__DATA_CONST', b'__fips_hmacs', 0x1000, 32),
                (b'__TEXT_EXEC', b'__text', 0x2000, 4096),
                (b'__DATA', b'__data', 0x3000, 4096)]
    for seg, off, prot in [(b'__TEXT', 0, 1), (b'__DATA_CONST', 0x1000, 1),
                            (b'__TEXT_EXEC', 0x2000, 5), (b'__DATA', 0x3000, 3),
                            (b'__LINKEDIT', 0x4000, 1)]:
        children = [s for s in sections if s[0] == seg]
        command = struct.pack('<II16s4Q4I', 0x19, 72+80*len(children), seg,
                              base+off, 4096, off, 4096, prot, prot, len(children), 0)
        for s, n, a, z in children:
            command += struct.pack('<16s16sQQ8I', n, s, base+a, z, a, 3, 0, 0, 0, 0, 0, 0)
        commands.append(command)
    names, strings = sorted(REQUIRED), bytearray(b'\0')
    for index, name in enumerate(names):
        address = base+0x3000 if name == '_j716f_armcpu_trace' else base+0x2000+index*4
        struct.pack_into('<IBBHQ', data, 0x4000+index*16, len(strings), 0xf,
                         3 if name == '_j716f_armcpu_trace' else 2, 0, address)
        strings += name.encode()+b'\0'
    data[0x4400:0x4400+len(strings)] = strings
    commands.append(struct.pack('<6I', 2, 24, 0x4000, len(names), 0x4400, len(strings)))
    command_data = b''.join(commands)
    struct.pack_into('<8I', data, 0, 0xfeedfacf, 0x100000c, 0, 2, len(commands), len(command_data), 0, 0)
    data[32:32+len(command_data)] = command_data
    struct.pack_into('<3Q', data, 0x3000, 0x315152494636314a, 1, 192)
    data[0x1000:0x1020] = hmac.digest(b'\0', data[0x2000:0x3000], 'sha256')
    return bytes(data)


class NativeCandidateTest(unittest.TestCase):
    def setUp(self):
        self.lock = json.loads((HERE.parent/'configs/native-candidate-lock.json').read_text())
        transfer = load_module('source-artifact-transfer')
        self.report = dict(status='compiled', prelink_performed=False, native_arch_timer_requested=True,
                           unixnu_commit=self.lock['unixnu_commit'], device_commit=self.lock['device_commit'],
                           references={'xnu': self.lock['xnu_commit']},
                           components={k: self.lock['components'][k] for k in ('ArmCpu', 'ArmDTPlatform')},
                           armcpu_driver_linked=True, armdt_platform_driver_linked=True,
                           artifacts=[{'path': n, 'bytes': 1, 'sha256': sha(b'x')}
                                      for n in transfer.OUTPUTS+transfer.NATIVE_OUTPUTS])

    def test_defined_symbols_and_post(self):
        result = check_kernel(fixture())
        self.assertTrue(result['source_crypto_hmac']['matches'])
        self.assertFalse(result['hardware_validated'])

    def test_undefined_symbol_is_not_a_provider(self):
        data = bytearray(fixture())
        data[0x4004] = 1
        with self.assertRaises(ValueError):
            check_kernel(bytes(data))

    def test_local_symbol_is_not_a_provider(self):
        data = bytearray(fixture())
        data[0x4004] = 0xe
        with self.assertRaises(ValueError):
            check_kernel(bytes(data))

    def test_local_name_can_overlap_external_but_export_conflicts_fail(self):
        data = bytearray(fixture())
        count = len(REQUIRED)
        # Add a same-named local at a different address, using spare __LINKEDIT.
        data[0x4000+16*count:0x4010+16*count] = data[0x4000:0x4010]
        data[0x4004+16*count] = 0xe
        struct.pack_into('<Q', data, 0x4008+16*count, 0xfffffff007002800)
        pos = 32
        while struct.unpack_from('<I', data, pos)[0] != 2:
            pos += struct.unpack_from('<I', data, pos+4)[0]
        struct.pack_into('<I', data, pos+12, count+1)
        self.assertTrue(check_kernel(bytes(data)))
        data[0x4004+16*count] = 0xf
        with self.assertRaises(ValueError):
            check_kernel(bytes(data))

    def test_bad_post_or_observer_refused(self):
        for offset in (0x1000, 0x2020, 0x3000, 0x3018):
            data = bytearray(fixture()); data[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                check_kernel(bytes(data))

    def test_legacy_route_refused(self):
        data = bytearray(fixture())
        data[0x2100:0x2100+len(BCM_ROUTE)] = BCM_ROUTE
        data[0x1000:0x1020] = hmac.digest(b'\0', data[0x2000:0x3000], 'sha256')
        with self.assertRaises(ValueError):
            check_kernel(bytes(data))

    def test_malformed_macho_bounds(self):
        for offset, value in ((20, 0xffffffff), (36, 0), (36, 9), (16, 0xffffffff)):
            data = bytearray(fixture()); struct.pack_into('<I', data, offset, value)
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                check_kernel(bytes(data))
        for data in (b'', fixture()[:60], fixture()[:-1]):
            with self.assertRaises(ValueError):
                check_kernel(data)

    def test_selected_six_artifacts(self):
        self.assertEqual(len(check_report(self.report, self.lock)[0]), 6)

    def test_wrong_revision_and_legacy_report(self):
        for key, value in (('device_commit', '0'*40), ('unixnu_commit', '0'*40),
                           ('native_arch_timer_requested', False), ('armcpu_driver_linked', False)):
            report = copy.deepcopy(self.report); report[key] = value
            with self.subTest(key=key), self.assertRaises(ValueError):
                check_report(report, self.lock)
        for key in ('ArmCpu', 'ArmDTPlatform'):
            report = copy.deepcopy(self.report); report['components'][key] = '0'*40
            with self.assertRaises(ValueError):
                check_report(report, self.lock)

    def test_manifest_missing_extra_duplicate(self):
        for mode in ('missing', 'extra', 'duplicate'):
            report = copy.deepcopy(self.report)
            if mode == 'missing': report['artifacts'].pop()
            elif mode == 'extra': report['artifacts'].append({'path': '../escape', 'bytes': 1, 'sha256': sha(b'x')})
            else: report['artifacts'].append(report['artifacts'][0])
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                check_report(report, self.lock)

    def test_outer_digest_and_archive_names(self):
        receiver = load_module('receive-native-candidate')
        def make(names):
            buf = io.BytesIO()
            with zipfile.ZipFile(buf, 'w') as archive:
                for name in names: archive.writestr(name, b'encrypted-fixture')
            return buf.getvalue()
        data = make(['j716f-source-only.enc'])
        lock = {'artifact_bytes': len(data), 'artifact_sha256': sha(data)}
        self.assertEqual(receiver.outer(data, lock), b'encrypted-fixture')
        with self.assertRaises(ValueError): receiver.outer(data+b'x', lock)
        data = make(['../j716f-source-only.enc'])
        with self.assertRaises(ValueError):
            receiver.outer(data, {'artifact_bytes': len(data), 'artifact_sha256': sha(data)})

    def test_default_board_and_reset_refusal(self):
        board = tomllib.loads((HERE.parent/'board.toml').read_text())
        self.assertFalse(check_board(board)['automatic_boot'])
        for key, value in (('panic_screen', True), ('entry_probe_reset_delay', 6),
                           ('entry_probe_reset_delay', 0), ('timeout', 0)):
            bad = copy.deepcopy(board); bad['payload'][key] = value
            with self.subTest(key=key), self.assertRaises(ValueError): check_board(bad)
        for arg in ('sheng-platform-stage=1', 'sheng-platform-panic=1', 'armcpu-skip-gic',
                    'armcpu-self-ipi', 'sheng-smp=1', 'cpus=2', 'cpus=1 armcpu-skip-gic'):
            bad = copy.deepcopy(board); bad['payload']['kernel_args'].append(arg)
            with self.subTest(arg=arg), self.assertRaises(ValueError): check_board(bad)

    def test_isolated_candidate_and_refuse_overwrite(self):
        prepare = load_module('prepare-native-platform')
        with tempfile.TemporaryDirectory() as temp:
            out = Path(temp)/'candidate'
            report = prepare.prepare(HERE.parent, out)
            self.assertFalse(report['legacy_entry_reset_enabled'])
            check_board((out/'board.toml').read_text())
            with self.assertRaises(ValueError): prepare.prepare(HERE.parent, out)

    def test_native_executable_mutation_refused(self):
        prelink = load_module('prelink-native-candidate')
        data = fixture()
        self.assertTrue(prelink.audit_native_preserved(data, data))
        bad = bytearray(data); bad[0x2000] ^= 1
        with self.assertRaises(ValueError): prelink.audit_native_preserved(data, bytes(bad))


if __name__ == '__main__':
    unittest.main()
