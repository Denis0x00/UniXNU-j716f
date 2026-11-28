#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Generate the audited iPadOS 17.7.11 (21H461) native bluetoothd binary, report, and optional ABI header."""
from __future__ import annotations

import argparse
import json
from pathlib import Path

from native_ios17 import NativeImage, layout_header


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path, help='Path to local stock 21H461 /usr/sbin/bluetoothd')
    parser.add_argument('--output', type=Path, required=True, help='Path to write patched bluetoothd')
    parser.add_argument('--layout-header', type=Path, help='Optional path to write qbt_ios17_layout.h')
    parser.add_argument('--dump-descriptors', type=Path, help='Optional path to dump raw 265-entry HCI descriptor table')
    parser.add_argument('--no-resign', action='store_true', help='Skip in-place ad-hoc LC_CODE_SIGNATURE page hash update')
    args = parser.parse_args()

    image = (
        NativeImage(args.image)
        .vendor_routing()
        .volatile_root_keys()
        .h4_shutdown()
        .capability_adapter()
    )
    if not args.no_resign:
        image.resign_adhoc()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image.data)
    report = args.output.with_suffix(args.output.suffix + '.json')
    report.write_text(json.dumps(image.report(), indent=2) + '\n', encoding='utf-8')

    if args.layout_header:
        args.layout_header.parent.mkdir(parents=True, exist_ok=True)
        args.layout_header.write_text(layout_header(), encoding='utf-8')
    if args.dump_descriptors:
        args.dump_descriptors.parent.mkdir(parents=True, exist_ok=True)
        args.dump_descriptors.write_bytes(image.descriptors_bytes())

    info = image.report()
    print(
        f'Patched 21H461 bluetoothd: {args.output} '
        f'(patches={len(info["patches"])}, cave={info["cave_used"]}/{info["cave_capacity"]} B, '
        f'cdhash={info["output_cdhash"]}, report={report})'
    )
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
