"""Target of objects linked into the kernel: architecture and iOS deployment of the kernel build.

The kernel build exports UNIXNU_XNU_ARCH and UNIXNU_IOS_VERSION from the selected profiles. Without them
the arm64 iOS 17.0 target is used.
"""
import os
import struct

ARCH = os.environ.get('UNIXNU_XNU_ARCH', 'arm64')
IOS_VERSION = os.environ.get('UNIXNU_IOS_VERSION', '17.0')
TRIPLE = f'{ARCH}-apple-ios{IOS_VERSION}'
_major, _minor = (IOS_VERSION.split('.') + ['0'])[:2]
DEPLOYMENT = (2, int(_major) << 16 | int(_minor) << 8)
_SUBTYPES = {'arm64': 0, 'arm64e': 2}
if ARCH not in _SUBTYPES:
    raise SystemExit(f'Unsupported kernel architecture {ARCH}')
SUBTYPE = _SUBTYPES[ARCH]


def check_object(data, what):
    """Refuse a relocatable object whose architecture or deployment differs from the kernel target."""
    magic, cpu, subtype, kind, count = struct.unpack_from('<5I', data)
    if (magic, cpu, subtype & 0xff, kind) != (0xfeedfacf, 0x100000c, SUBTYPE, 1):
        raise SystemExit(f'{what}: not an {ARCH} object')
    offset, deployment = 32, None
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, offset)
        if command == 0x32:
            deployment = struct.unpack_from('<II', data, offset + 8)
        offset += length
    if deployment != DEPLOYMENT:
        raise SystemExit(f'{what}: deployment {deployment}, expected {DEPLOYMENT}')
