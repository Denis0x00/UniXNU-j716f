#!/usr/bin/env python3
"""Discovery and ownership rejection tests; never execute host disk commands."""
import contextlib
import io
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

import apfs_seed_compat as compat


def container(store='disk9', reference='disk10'):
    return {'ContainerReference': reference, 'PhysicalStores': [{'DeviceIdentifier': store}]}


class DiscoveryTests(unittest.TestCase):
    def test_exact_physical_store(self):
        self.assertEqual(compat.exact_container({'Containers': [container()]}, 'disk9'), 'disk10')

    def test_no_host_prefix_or_partition_fallback(self):
        for store in ('disk0', 'disk90', 'disk9s1'):
            self.assertIsNone(compat.exact_container({'Containers': [container(store)]}, 'disk9'))

    def test_duplicate_container_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'Ambiguous'):
            compat.exact_container({'Containers': [container(), container(reference='disk11')]}, 'disk9')

    def test_multiple_physical_stores_rejected(self):
        entry = container()
        entry['PhysicalStores'].append({'DeviceIdentifier': 'disk0'})
        with self.assertRaisesRegex(RuntimeError, 'additional'):
            compat.exact_container({'Containers': [entry]}, 'disk9')

    def test_invalid_or_same_container_rejected(self):
        for name in ('disk9', 'disk10s1', '/dev/disk10', 'disk10;echo'):
            with self.assertRaises(RuntimeError):
                compat.exact_container({'Containers': [container(reference=name)]}, 'disk9')

    def test_first_entity_not_guessed(self):
        for entry in ({}, {'system-entities': [{'dev-entry': '/dev/disk9s1'}]},
                      {'system-entities': [{'dev-entry': '/dev/rdisk9'}]}):
            with self.assertRaises(RuntimeError):
                compat.attachment_store(entry)

    def test_owned_path_must_be_unique_and_device_unchanged(self):
        raw = Path('specific-seed.raw').resolve()
        own = {'image-path': str(raw), 'system-entities': [{'dev-entry': '/dev/disk9'}]}
        wrong = {'image-path': str(raw), 'system-entities': [{'dev-entry': '/dev/disk0'}]}
        for images in ([], [own, own], [wrong]):
            with mock.patch.object(compat, 'plist_command', return_value={'images': images}):
                with self.assertRaises(RuntimeError):
                    compat.owned_image(raw, 'disk9')

    def exercise(self, listings, *, owned_container='disk10', detach_code=0):
        """Return the yielded container or an exception, and all command calls."""
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        raw = Path(directory.name) / 'seed.raw'
        raw.touch()
        attached = {'system-entities': [{'dev-entry': '/dev/disk9'},
                                        {'dev-entry': '/dev/' + owned_container}]}
        own = {'image-path': str(raw), **attached}
        queue = list(listings)
        calls = []
        def fake_plist(*args, **kwargs):
            calls.append(args)
            if args[:2] == ('hdiutil', 'attach'):
                return attached
            if args[:2] == ('hdiutil', 'info'):
                return {'images': [own]}
            if args[:3] == ('diskutil', 'apfs', 'list'):
                return queue.pop(0) if len(queue) > 1 else queue[0]
            if args[:2] == ('diskutil', 'info'):
                return {'DeviceIdentifier': args[-1].removeprefix('/dev/')}
            raise AssertionError(args)
        output = io.StringIO()
        with mock.patch.object(compat, 'plist_command', side_effect=fake_plist), \
             mock.patch.object(compat.subprocess, 'run', return_value=subprocess.CompletedProcess([], detach_code)), \
             mock.patch.object(compat.time, 'sleep'), contextlib.redirect_stdout(output):
            try:
                with compat.attached_seed_container(raw) as selected:
                    result = selected
            except RuntimeError as error:
                result = error
        return result, calls, output.getvalue()

    def test_delayed_exact_match_on_last_attempt(self):
        listings = [{'Containers': [container('disk0', 'disk3')]}] * 10
        result, calls, output = self.exercise(listings + [{'Containers': [container()]}])
        self.assertEqual(result, 'disk10')
        self.assertEqual(sum(c[:3] == ('diskutil', 'apfs', 'list') for c in calls), 11)
        self.assertIn('"attempt": 11', output)
        self.assertNotIn('disk0', output)

    def test_timeout_never_selects_host_and_diagnostics_are_scoped(self):
        result, calls, output = self.exercise([{'Containers': [container('disk0', 'disk3')]}])
        self.assertIsInstance(result, RuntimeError)
        self.assertNotIn('disk0', output)
        self.assertNotIn('disk3', output)
        self.assertFalse(any('resizeContainer' in c for c in calls))

    def test_stale_container_outside_owned_entities_rejected(self):
        result, _, output = self.exercise([{'Containers': [container(reference='disk3')]}])
        self.assertIsInstance(result, RuntimeError)
        self.assertIn('absent from this image attachment', str(result))
        self.assertNotIn('"apfs_seed_discovery": "selected"', output)

    def test_detach_failure_propagates(self):
        result, _, _ = self.exercise([{'Containers': [container()]}], detach_code=1)
        self.assertIsInstance(result, RuntimeError)
        self.assertIn('detach failed', str(result))

    def test_deadline_bounds_discovery(self):
        with mock.patch.object(compat.time, 'monotonic', side_effect=[0, 16]):
            result, calls, _ = self.exercise([{'Containers': [container()]}])
        self.assertIsInstance(result, RuntimeError)
        self.assertFalse(any(c[:3] == ('diskutil', 'apfs', 'list') for c in calls))


if __name__ == '__main__':
    unittest.main()
