#!/usr/bin/env python3
"""Executable DESIGN model only: no production imports, receipt, file or device I/O.

Passing this model does not implement or qualify Init v2. Production C/Python,
linked EFI, UI and hardware tests are separate acceptance gates in the plan.
"""
import unittest

TOTAL = 0x40000
CAPACITY = TOTAL - 12
SESSION = 8192
LAST_FITTING_END = TOTAL - SESSION - 12
ELIGIBLE = frozenset(('FULL_WRAPPED', 'FULL_LINEAR', 'LINEAR_EXHAUSTED'))


def classify(signature, nxt, size):
    if any(type(v) is not int for v in (signature, nxt, size)):
        return 'INVALID_HEADER'
    if signature != 0x43474244 or not 0 <= nxt <= CAPACITY or not 0 <= size <= CAPACITY:
        return 'INVALID_HEADER'
    if size == CAPACITY:
        return 'FULL_LINEAR' if nxt == CAPACITY else 'FULL_WRAPPED'
    if nxt != size:
        return 'INVALID_HEADER'
    offset = (12 + nxt + 7) & ~7
    return 'SPACE_AVAILABLE' if offset + SESSION <= TOTAL else 'LINEAR_EXHAUSTED'


def abstract_commit_allowed(kind, *, stable=True, pending=False, suspect=False,
                            current_export=True, identity_match=True,
                            receipt_v2_match=True, confirmations=2,
                            live_same=True, consumed=False):
    """Boolean design oracle, NOT a replacement for actual byte/hash validation."""
    return (kind in ELIGIBLE and stable and not pending and not suspect
            and current_export and identity_match and receipt_v2_match
            and confirmations == 2 and live_same and not consumed)


class DesignTests(unittest.TestCase):
    def test_photographed_header(self):
        end = 0x3f56e
        self.assertEqual(classify(0x43474244, end, end), 'LINEAR_EXHAUSTED')
        self.assertEqual(CAPACITY-end, 2694)
        self.assertEqual(TOTAL-((12+end+7) & ~7), 2688)

    def test_capacity_boundary(self):
        self.assertEqual(LAST_FITTING_END, 0x3dff4)
        for delta, expected in ((-1, 'SPACE_AVAILABLE'), (0, 'SPACE_AVAILABLE'), (1, 'LINEAR_EXHAUSTED')):
            end = LAST_FITTING_END + delta
            self.assertEqual(classify(0x43474244, end, end), expected)

    def test_all_linear_sizes(self):
        counts = {'SPACE_AVAILABLE': 0, 'LINEAR_EXHAUSTED': 0, 'FULL_LINEAR': 0}
        for end in range(CAPACITY+1):
            actual = classify(0x43474244, end, end)
            expected = ('FULL_LINEAR' if end == CAPACITY else
                        'SPACE_AVAILABLE' if end <= LAST_FITTING_END else 'LINEAR_EXHAUSTED')
            self.assertEqual(actual, expected)
            counts[actual] += 1
        self.assertEqual(counts, {'SPACE_AVAILABLE': 253941, 'LINEAR_EXHAUSTED': 8191, 'FULL_LINEAR': 1})

    def test_all_full_ring_cursors(self):
        for nxt in range(CAPACITY):
            self.assertEqual(classify(0x43474244, nxt, CAPACITY), 'FULL_WRAPPED')

    def test_malformed_headers(self):
        for signature, nxt, size in ((0, 0, 0), (0x43474244, 1, 0),
                                     (0x43474244, 0, 1), (0x43474244, -1, 0),
                                     (0x43474244, 0, -1), (0x43474244, CAPACITY+1, CAPACITY),
                                     (0x43474244, CAPACITY, CAPACITY+1),
                                     (0x43474244, 2**32-1, 2**32-1),
                                     (0x43474244, True, 1)):
            self.assertEqual(classify(signature, nxt, size), 'INVALID_HEADER')

    def test_every_safety_gate_still_required(self):
        failures = ({'stable': False}, {'pending': True}, {'suspect': True},
                    {'current_export': False}, {'identity_match': False},
                    {'receipt_v2_match': False}, {'confirmations': 0},
                    {'confirmations': 1}, {'confirmations': 3},
                    {'live_same': False}, {'consumed': True})
        for kind in ELIGIBLE:
            self.assertTrue(abstract_commit_allowed(kind))
            for failure in failures:
                self.assertFalse(abstract_commit_allowed(kind, **failure))
        for kind in ('SPACE_AVAILABLE', 'INVALID_HEADER', 'UNKNOWN'):
            self.assertFalse(abstract_commit_allowed(kind))

    def test_new_boot_needs_new_export_even_if_bytes_equal(self):
        self.assertFalse(abstract_commit_allowed('LINEAR_EXHAUSTED', current_export=False))
        self.assertFalse(abstract_commit_allowed('LINEAR_EXHAUSTED', receipt_v2_match=False))


if __name__ == '__main__':
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(DesignTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    print('DESIGN ONLY: 262133 linear sizes + 262132 full-ring cursors checked; no firmware or device qualification.')
