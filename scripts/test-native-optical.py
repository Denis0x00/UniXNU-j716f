#!/usr/bin/env python3
"""Synthetic offline video -> all frames -> saved raw dump -> receipt.

Uses zxing-cpp and opencv-python. This tests the host video path, NOT a real
camera/panel. Actual Mu encoder/EFI execution must be checked separately.
"""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import cv2
import numpy as np
import zxingcpp
from native_export import Capture, encode, publish, receipt

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    args=p.parse_args();args.output.mkdir(exist_ok=False)
    candidate=struct.pack('<4Q',1,2,3,4).hex()
    raw=struct.pack('<3I',0x43474244,0x2a354,0x3fff4)+bytes((i*17+(i>>8))&255 for i in range(12,0x40000))
    frames=list(encode(raw,candidate,123))
    video=args.output/'synthetic-256-frames.avi'
    writer=cv2.VideoWriter(str(video),cv2.VideoWriter_fourcc(*'MJPG'),4,(960,960))
    if not writer.isOpened():raise RuntimeError('Video encoder unavailable')
    try:
        for frame in frames:
            bitmap=np.asarray(zxingcpp.write_barcode(zxingcpp.BarcodeFormat.QRCode,frame,width=880,height=880))
            if max(bitmap.shape)>960:raise ValueError('QR fixture unexpectedly large')
            canvas=np.full((960,960),255,dtype=np.uint8)
            y=(960-bitmap.shape[0])//2;x=(960-bitmap.shape[1])//2
            canvas[y:y+bitmap.shape[0],x:x+bitmap.shape[1]]=bitmap
            writer.write(cv2.cvtColor(canvas,cv2.COLOR_GRAY2BGR))
    finally:writer.release()
    spec=importlib.util.spec_from_file_location('receive_native_console',Path(__file__).with_name('receive-native-console.py'))
    receiver=importlib.util.module_from_spec(spec);spec.loader.exec_module(receiver)
    capture=Capture(candidate)
    for frame in receiver.video_frames(video,4):capture.add(frame)
    report=publish(capture,args.output/'reconstructed')
    assert (args.output/'reconstructed/raw-console.bin').read_bytes()==raw
    assert report['initialization_receipt']==receipt(candidate,hashlib.sha256(raw).hexdigest())
    print(json.dumps({'passed':True,'unique_frames':len(capture.frames),'bytes':len(raw),
                      'scope':'synthetic video with independent QR encoder; no physical display/camera'},indent=2))

if __name__=='__main__':main()
