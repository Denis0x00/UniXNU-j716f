"""J716F2 offline maintenance codec. V1 remains historical/read-only input.

Receipts bind the build profile, candidate, exact raw bytes and live invocation's
transfer. They are accident-prevention codes, NOT authentication or proof that
the device has retained memory across a reset.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import zlib
from native_export import SIZE, CHUNK, COUNT, save_new

DOMAIN = b'J716F-INIT-RECEIPT-V2'
PATTERN = re.compile(r'J716F2/([0-9A-F]{64})/([0-9A-F]{64})/([0-9A-F]{64})/'
                     r'([0-9A-F]{16})/([0-9A-F]{8})/([0-9A-F]{4})/0100/'
                     r'([0-9A-F]{2048})/([0-9A-F]{8})')


def identity(value):
    if not re.fullmatch('[0-9a-fA-F]{64}', value) or not int(value, 16):
        raise ValueError('Explicit nonzero 32-byte identity required')
    return value.upper()


def receipt(candidate, digest, profile, transfer):
    if not 0 < transfer < 2**64:
        raise ValueError('Nonzero transfer required')
    value = DOMAIN + bytes.fromhex(identity(profile)) + bytes.fromhex(identity(candidate))
    value += bytes.fromhex(digest) + struct.pack('<Q', transfer)
    return f'{int.from_bytes(hashlib.sha256(value).digest()[:8], "little"):016X}'


def classify(raw):
    if len(raw) != SIZE:
        return 0, 0
    magic, nxt, used = struct.unpack_from('<3I', raw)
    if magic != 0x43474244 or max(nxt, used) > SIZE-12:
        return 0, 0
    if used == SIZE-12:
        return (2 if nxt == used else 1), 0
    if nxt != used:
        return 0, 0
    free = SIZE - ((12+nxt+7) & ~7)
    return (3 if free < 8192 else 4), free


def init_eligibility(raw):
    kind, free = classify(raw)
    names = {0:'INVALID_HEADER', 1:'FULL_WRAPPED', 2:'FULL_LINEAR', 3:'LINEAR_EXHAUSTED', 4:'SPACE_AVAILABLE'}
    if kind not in (1, 2, 3):
        return False, names[kind]
    for off in range(16, SIZE-7, 8):
        if struct.unpack_from('<Q', raw, off)[0] != 0x31474f4c4e36314a:
            continue
        if off > SIZE-8192:
            return False, 'SUSPECT_SESSION'
        h = raw[off:off+128]
        version, size = struct.unpack_from('<II', h, 8)
        session, offset, crc, state = struct.unpack_from('<QIII', h, 48)
        if (version != 1 or size != 8192 or not session or offset != off or
                not any(h[16:48]) or crc != zlib.crc32(h[:60]) or any(h[68:]) or
                state not in (0x414c4e4a, 0x554c4e4a)):
            return False, 'SUSPECT_SESSION'
        if state == 0x414c4e4a:
            return False, 'PENDING_ARM'
    return True, names[kind]


def encode(raw, candidate, profile, transfer):
    candidate, profile = identity(candidate), identity(profile)
    if len(raw) != SIZE or not 0 < transfer < 2**64:
        raise ValueError('Wrong raw size or transfer')
    digest, crc = hashlib.sha256(raw).hexdigest().upper(), zlib.crc32(raw)
    for i in range(COUNT):
        text = f'J716F2/{profile}/{candidate}/{digest}/{transfer:016X}/{crc:08X}/{i:04X}/0100/{raw[i*CHUNK:(i+1)*CHUNK].hex().upper()}/'
        yield text + f'{zlib.crc32(text.encode()):08X}'


class Capture:
    def __init__(self, candidate, profile):
        self.candidate, self.profile = identity(candidate), identity(profile)
        self.identity = None
        self.frames = {}

    def add(self, text):
        m = PATTERN.fullmatch(text)
        if not m:
            raise ValueError('Malformed/wrong-version maintenance frame')
        profile, candidate, digest, transfer, crc, index, payload, check = m.groups()
        if candidate != self.candidate or profile != self.profile or not int(transfer, 16):
            raise ValueError('Wrong maintenance profile/candidate or zero transfer')
        if zlib.crc32(text[:-8].encode()) != int(check, 16):
            raise ValueError('Frame CRC mismatch')
        index = int(index, 16)
        if index >= COUNT:
            raise ValueError('Out-of-range frame')
        key = profile, candidate, digest, transfer, crc
        if self.identity is not None and self.identity != key:
            raise ValueError('Mixed profiles, snapshots or invocations')
        if index in self.frames and self.frames[index] != text:
            raise ValueError('Conflicting duplicate')
        self.identity = key
        self.frames[index] = text

    def reconstruct(self):
        if len(self.frames) != COUNT:
            raise ValueError(f'Incomplete capture {len(self.frames)}/{COUNT}; no initialization receipt')
        raw = b''.join(bytes.fromhex(PATTERN.fullmatch(self.frames[i])[7]) for i in range(COUNT))
        if len(raw) != SIZE or hashlib.sha256(raw).hexdigest().upper() != self.identity[2] or zlib.crc32(raw) != int(self.identity[4], 16):
            raise ValueError('Whole snapshot SHA/CRC mismatch')
        return raw


def publish(capture, output):
    output = Path(output)
    output.mkdir(parents=False, exist_ok=False)
    save_new(output/'frames.txt', ('\n'.join(capture.frames[i] for i in sorted(capture.frames))+'\n').encode('ascii'))
    raw = capture.reconstruct()
    save_new(output/'raw-console.bin', raw)
    profile, candidate, digest, transfer, crc = capture.identity
    eligible, reason = init_eligibility(raw)
    token = receipt(candidate, digest, profile, int(transfer, 16)) if eligible else None
    if token == '0000000000000000':
        eligible, reason, token = False, 'ZERO_RECEIPT', None
    result = {'schema':2, 'scope':'Private maintenance snapshot, not a native execution result',
              'maintenance_profile':profile.lower(), 'candidate':candidate.lower(),
              'snapshot_sha256':digest.lower(), 'transfer_id':transfer, 'console_crc32':crc,
              'bytes':SIZE, 'unique_frames':COUNT, 'complete':True,
              'saved_fsynced_and_readback_verified':True,
              'dbgc':[f'{v:08X}' for v in struct.unpack_from('<3I', raw)],
              'initialization_eligible_by_host_policy':eligible, 'eligibility_reason':reason,
              'initialization_receipt':token,
              'receipt_scope':'Only the still-open MAINT v2 invocation with this profile/candidate/SHA/transfer. Not authentication.',
              'device_operations':False, 'native_kernel_result_inferred':False}
    save_new(output/'receipt.json', (json.dumps(result, indent=2)+'\n').encode())
    if os.name == 'posix':
        fd = os.open(output, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
    return result


def decode_init(raw, candidate):
    if len(raw) != SIZE:
        raise ValueError('Wrong snapshot size')
    magic, nxt, size = struct.unpack_from('<3I', raw)
    h = raw[16:272]
    if magic != 0x43474244 or nxt != size or not 260 <= size <= SIZE-12:
        raise ValueError('Invalid committed linear header')
    if struct.unpack_from('<QII', h) != (0x3254494e4936314a, 2, 256) or h[16:48].hex() != candidate.lower():
        raise ValueError('Wrong init identity')
    if any(h[148:248]) or struct.unpack_from('<II', h, 248) != (zlib.crc32(h[:248]), 0xc04d17ed):
        raise ValueError('Invalid init record CRC/commit/reserved')
    digest, profile = h[48:80].hex(), h[80:112].hex()
    transfer = struct.unpack_from('<Q', h, 112)[0]
    expected = receipt(candidate, digest, profile, transfer)
    if struct.unpack_from('<Q', h, 136)[0] != int(expected, 16):
        raise ValueError('Init receipt binding differs')
    kind = struct.unpack_from('<I', h, 144)[0]
    old = h[120:132] + bytes(SIZE-12)
    if kind not in (1, 2, 3) or classify(old)[0] != kind:
        raise ValueError('Invalid init classification')
    return {'maintenance_profile':profile, 'snapshot_sha256':digest, 'transfer_id':f'{transfer:016X}', 'classification':kind}
