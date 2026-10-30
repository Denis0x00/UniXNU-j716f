#!/usr/bin/env python3
"""Receive private offline QR text/video; never uses a website or accesses a device.

Requires zxing-cpp + opencv-python only for --video. Save videos privately.
Missing frames are retained as frames.txt in a NEW output directory; combine
that file with a later recording of the SAME transfer in another NEW directory.
"""
import argparse
import json
from pathlib import Path
import sys
from native_export import Capture, publish


def video_frames(path, sample_fps):
    try:
        import cv2
        import zxingcpp
    except ImportError as exc:
        raise ValueError('Offline video decoding needs opencv-python and zxing-cpp') from exc
    video = cv2.VideoCapture(str(path))
    if not video.isOpened():
        raise ValueError('Cannot open video: '+str(path))
    rate = video.get(cv2.CAP_PROP_FPS)
    step = max(1, int(rate/sample_fps)) if rate > 0 else 1
    index = 0
    try:
        while True:
            ok, frame = video.read()
            if not ok:
                break
            if index % step == 0:
                for result in zxingcpp.read_barcodes(frame, formats=zxingcpp.BarcodeFormat.QRCode):
                    if result.text.startswith(('J716F1/','J716F2/')):
                        yield result.text
            index += 1
    finally:
        video.release()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--candidate', required=True)
    p.add_argument('--maintenance-profile', help='Exact MAINT v2 profile from the audited image; never inferred from video')
    p.add_argument('--frames', type=Path, action='append', default=[])
    p.add_argument('--video', type=Path, action='append', default=[])
    p.add_argument('--sample-fps', type=float, default=4)
    p.add_argument('--output', type=Path, required=True)
    args = p.parse_args()
    if not args.frames and not args.video:
        p.error('At least one --frames or --video is required')
    if not 0 < args.sample_fps <= 60:
        p.error('--sample-fps must be in (0, 60]')
    if args.output.exists():
        p.error('Preserve existing evidence directory; choose a new --output')
    if args.maintenance_profile:
        from native_maintenance import Capture as MaintenanceCapture, publish as maintenance_publish
        capture = MaintenanceCapture(args.candidate, args.maintenance_profile)
        publisher = maintenance_publish
    else:
        capture = Capture(args.candidate)
        publisher = publish
    try:
        for path in args.frames:
            with path.open(encoding='ascii') as stream:
                for line in stream:
                    # Only strip line endings, not arbitrary noncanonical spaces.
                    line = line.rstrip('\r\n')
                    if line:
                        capture.add(line)
        for path in args.video:
            for frame in video_frames(path, args.sample_fps):
                capture.add(frame)
        result = publisher(capture, args.output)
    except (ValueError, OSError) as exc:
        print('REFUSED: '+str(exc), file=sys.stderr)
        return 2
    # No raw bytes or device identifiers. Receipt is emitted only after durable
    # write + readback of BOTH the complete dump and the report.
    print(json.dumps({k: result[k] for k in ('bytes','unique_frames','snapshot_sha256',
        'console_crc32','initialization_eligible_by_host_policy','initialization_receipt')}, indent=2))
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
