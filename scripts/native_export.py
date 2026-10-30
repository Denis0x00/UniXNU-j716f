"""Offline raw-console transfer codec. No network, device commands or public outputs."""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import zlib

SIZE, CHUNK, COUNT = 0x40000, 1024, 256
DOMAIN = b'J716F-INIT-RECEIPT-V1'
PATTERN = re.compile(r'J716F1/([0-9A-F]{64})/([0-9A-F]{64})/([0-9A-F]{16})/'
                     r'([0-9A-F]{8})/([0-9A-F]{4})/0100/([0-9A-F]{2048})/([0-9A-F]{8})')


def receipt(candidate, digest):
    return f'{int.from_bytes(hashlib.sha256(DOMAIN + bytes.fromhex(candidate) + bytes.fromhex(digest)).digest()[:8], "little"):016X}'


def encode(raw, candidate, transfer):
    """Fixture/reference encoder; firmware is independently implemented in C."""
    if len(raw) != SIZE or not re.fullmatch('[0-9a-fA-F]{64}', candidate) or not 0 < transfer < 2**64:
        raise ValueError('Invalid raw size, candidate or transfer')
    digest, crc = hashlib.sha256(raw).hexdigest().upper(), zlib.crc32(raw)
    for i in range(COUNT):
        prefix = f'J716F1/{candidate.upper()}/{digest}/{transfer:016X}/{crc:08X}/{i:04X}/0100/{raw[i*CHUNK:(i+1)*CHUNK].hex().upper()}/'
        yield prefix + f'{zlib.crc32(prefix.encode("ascii")):08X}'


class Capture:
    def __init__(self, candidate):
        if not re.fullmatch('[0-9a-fA-F]{64}', candidate):
            raise ValueError('Explicit full candidate identity required')
        self.candidate = candidate.upper()
        self.identity = None
        self.frames = {}

    def add(self, text):
        match = PATTERN.fullmatch(text)
        if not match:
            raise ValueError('Malformed/noncanonical native raw frame')
        candidate, digest, transfer, crc, index, payload, frame_crc = match.groups()
        if candidate != self.candidate or int(transfer, 16) == 0:
            raise ValueError('Wrong candidate or zero transfer ID')
        if zlib.crc32(text[:-8].encode('ascii')) != int(frame_crc, 16):
            raise ValueError('Frame CRC mismatch')
        index = int(index, 16)
        if index >= COUNT:
            raise ValueError('Frame index out of range')
        identity = (candidate, digest, transfer, crc)
        if self.identity is not None and self.identity != identity:
            raise ValueError('Mixed snapshots/transfers; preserve them in separate evidence directories')
        if index in self.frames and self.frames[index] != text:
            raise ValueError('Conflicting duplicate frame')
        self.identity = identity
        self.frames[index] = text

    def reconstruct(self):
        if len(self.frames) != COUNT:
            missing = [f'{i:04X}' for i in range(COUNT) if i not in self.frames]
            raise ValueError(f'Incomplete capture: {len(self.frames)}/{COUNT}; missing ' + ','.join(missing))
        raw = b''.join(bytes.fromhex(PATTERN.fullmatch(self.frames[i])[6]) for i in range(COUNT))
        if len(raw) != SIZE or hashlib.sha256(raw).hexdigest().upper() != self.identity[1]:
            raise ValueError('Complete snapshot size/SHA256 mismatch')
        if zlib.crc32(raw) != int(self.identity[3], 16):
            raise ValueError('Complete console CRC mismatch')
        return raw


def init_eligibility(raw):
    """Independent Python equivalent of the narrowly allowed initialization gate."""
    if len(raw) != SIZE:
        return False, 'wrong-size'
    magic, nxt, size = struct.unpack_from('<3I', raw)
    if magic != 0x43474244 or size != SIZE-12 or nxt >= SIZE-12:
        return False, 'not-a-well-formed-full-wrapped-ring'
    for off in range(16, SIZE-8192+1, 8):
        h = raw[off:off+128]
        if struct.unpack_from('<QII', h) != (0x31474f4c4e36314a, 1, 8192):
            continue
        session, offset, crc, state = struct.unpack_from('<QIII', h, 48)
        if session and offset == off and crc == zlib.crc32(h[:60]) and state == 0x414c4e4a and not any(h[68:]):
            return False, 'intact-unconsumed-ARM-found-in-physical-snapshot'
    return True, 'full-wrapped-ring-no-intact-pending-ARM'


def save_new(path, data):
    """No clobber, flush/fsync, then reopen/readback before authorizing a receipt."""
    with Path(path).open('xb') as stream:
        stream.write(data)
        stream.flush()
        os.fsync(stream.fileno())
    if Path(path).read_bytes() != data:
        raise OSError('Saved evidence readback differs')


def publish(capture, output):
    output = Path(output)
    output.mkdir(parents=False, exist_ok=False)
    # Retain partial captures too; they NEVER receive raw-console.bin / a receipt.
    lines = '\n'.join(capture.frames[i] for i in sorted(capture.frames)) + '\n'
    save_new(output/'frames.txt', lines.encode('ascii'))
    raw = capture.reconstruct()
    save_new(output/'raw-console.bin', raw)
    eligible, reason = init_eligibility(raw)
    token = receipt(capture.identity[0], capture.identity[1]) if eligible else None
    if token == '0000000000000000':
        eligible, reason, token = False, 'zero-receipt-refused', None
    report = {
        'schema': 1, 'scope': 'Private complete RAW optical snapshot, not kernel/retention qualification',
        'candidate': capture.identity[0].lower(), 'snapshot_sha256': capture.identity[1].lower(),
        'transfer_id': capture.identity[2], 'console_crc32': capture.identity[3],
        'bytes': SIZE, 'unique_frames': COUNT, 'complete': True,
        'saved_fsynced_and_readback_verified': True, 'displayed_console_crc_independently_recomputed': True,
        'dbgc': [f'{n:08X}' for n in struct.unpack_from('<3I', raw)],
        'initialization_eligible_by_host_policy': eligible, 'eligibility_reason': reason,
        'initialization_receipt': token,
        'receipt_scope': 'Human accident-prevention code bound to exact candidate and raw bytes, NOT authentication or device authorization. Identical bytes yield identical receipts.',
        'device_operations': False, 'native_kernel_result_inferred': False,
        'next_action': 'Keep both dump and frames private. Review this receipt against Init snapshot SHA256 and obtain separate authorization before any destructive header change.'
    }
    save_new(output/'receipt.json', (json.dumps(report, indent=2)+'\n').encode('utf-8'))
    if os.name == 'posix':
        fd = os.open(output, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    return report
