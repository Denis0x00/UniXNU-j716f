#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Version-locked native iPadOS 17.7.11 (21H461) arm64 Bluetooth routing & capability adapter.

Targets the stock non-PAC arm64 /usr/sbin/bluetoothd from iPadOS 17.7.11 (21H461,
UUID 0F836381-D07B-36E4-9DBB-8A44782B9C14) for QCA6390 H4 Type-0 bring-up on TB-J716F.
All Apple IPSW binaries stay strictly local and are never committed to Git.
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import struct
import uuid
from pathlib import Path

ORIGINAL_SHA256 = '3f1e4493ea6a76976625f7af58a8f330c0c90d82d9020b11110b4f3f5b758df4'
ORIGINAL_UUID = '0F836381-D07B-36E4-9DBB-8A44782B9C14'
ORIGINAL_CDHASH = 'fe7bc182386a2766c7574f8b48412ff9df113564'
ORIGINAL_CPU = 0x100000c
ORIGINAL_SUBTYPE = 0x0  # pure arm64 (non-PAC)

BASE = 0x100000000
# Zero-filled __TEXT header tail (load_cmds_end=0x2b40, __text starts at 0x5550).
# Reserve 0x2b40..0x3000 for appending LC_LOAD_DYLIB; use 0x3000..0x5550 as code cave.
CAVE = 0x100003000
CAVE_END = 0x100005550

# Native HCI descriptor table and runtime state (21H461).
DESCRIPTORS = 0x100925dc0
DESCRIPTOR_COUNT = 0x109  # verified at 0x10005512c: cmp w0, #0x109
SUPPORT_TABLE = 0x100945820
SUPPORT_BYTES = 64
LE_FEATURES = 0x1009457b4  # 0x100945790 + 0x24

# Controller factory & platform checks.
FACTORIES = (0x10031b544, 0x10031c99c, 0x10031c9f0)
CONTROLLER_THUNK = 0x1002d50e4
CONTROLLER_INIT = 0x1002d4680
UNKNOWN_PRODUCT_CHECK = 0x10031cc90
VIRTUAL_MACHINE_CHECK = 0x10037792c

# Optional HCI setup & LE random address refresh fixes.
HOST_BUFFER_STATUS = 0x100184c60
HOST_FLOW_UNSUPPORTED = 0x100184c94
ERRONEOUS_STATUS = 0x100184d4c
ADDRESS_REFRESH = 0x10020eb00
ADDRESS_REFRESH_TARGET = 0x10020cef8
ADV_ADDRESS_CACHED = 0x1001f5c5c
ADDRESS_FALLBACK = 0x10020edf0
ADDRESS_FALLBACK_DONE = 0x1002112e8
ADDRESS_FALLBACK_NOTIFY = 0x10018c08c
ADDRESS_RETURN = 0x10020ec20
COMMAND_SUBMIT = 0x100050934
SET_RANDOM_ADDRESS = 0xa5

# Vendor backend registration & dispatch (iOS 17 range is 1..3999, 2 vendor backends).
REGISTER = 0x1001e1dc8
REGISTER_ACCEPT = 0x1001e1dd4  # REGISTER + 12 (no PACIBSP)
REGISTERED_CHIP = 0x100927754
REGISTERED_CONTEXT = 0x1009321c0

VENDOR_INIT = 0x1001e1cdc
VENDOR_INIT_SITE = 0x1001e1cf8
VENDOR_INIT_EPILOGUE = 0x1001e1da0
VENDOR_CHIP = 0x100931830
VENDOR_OPS = 0x100931940
VENDOR_OPS_BYTES = 0x878
# Note: 0x100931180 is used by initCoreStack and 0x1009321c0 is REGISTERED_CONTEXT;
# only clear the three vendor-specific hooks.
VENDOR_HOOKS = (0x100931188, 0x1009311a8, 0x1009321b8)

# (entry, patch_site, displaced_word, epilogue, generic_context)
CALLBACKS = (
    (0x1001e1f9c, 0x1001e1fbc, 0x51000509, 0x1001e2040, 0x1009321e8),
    (0x1001e205c, 0x1001e207c, 0x51000508, 0x1001e20dc, 0x100932358),
)

# Optional HCI 0x0c5b (Read Local Supported Codecs / Read Enhanced TX Power / etc.).
OPTIONAL_STEP = 0x1001840a0
CHAIN_NEXT = 0x10023e1f0
CHAIN_CONTEXT = 0x100930de8

# Volatile root keys (readRootKeys at 0x10062dfd4).
ROOT_KEY_READS = (
    0x10062e0ac,
    0x10062e168,
    0x10062e224,
    0x10062e278,
    0x10062e30c,
    0x10062e604,
)
ROOT_KEY_WRITES = (
    0x10062e114,
    0x10062e1cc,
    0x10062e34c,
    0x10062e428,
    0x10062e46c,
    0x10062e514,
    0x10062e59c,
    0x10062e674,
)
ROOT_KEY_STORE_CHECK = 0x10062e5cc
ROOT_KEY_FAILURE = 0x10062e69c
ROOT_KEY_LOAD_RESULT = 0x10062e738

# H4 shutdown pre-stop guard.
TRANSPORT = 0x100925d30
H4_TABLE = 0x100925608
H4_PRESTOP = 0x10015f238

# Capability model vtable & Type-0 shared state in __DATA zero-fill tail.
MODEL_GETTER = 0x10004681c
MODEL_VTABLE = 0x1008db430
MODEL_CAPABILITIES = dict(
    ext_adv=0x708,
    periodic_adv=0x710,
    periodic_sync=0x718,
    past_sender=0x720,
    past_recipient=0x728,
    ext_scan=0x778,
    enhanced_connection=0x780,
    spec_adv=0x788,
    coded_phy=0x810,
    phy_2m=0x818,
    adv_sets=0xbb8,
)
NATIVE_STATE = 0x100947f00
NATIVE_STATE_BYTES = 128
NATIVE_DYLIB = b'/usr/lib/QcaBTType0.dylib'

# LE features callback (iOS 17 has only legacy initCmdReadLeLocalSupportedFeaturesCB).
LE_FEATURES_CB = (0x1001845f4, 0x1001846ec, 0x1001846fc, 0xd10143ff)

# LeBroadcaster::stackDidStart and advertising table capacity.
ADV_START = 0x1005a405c
ADV_START_PROLOGUE = 0xd10183ff  # sub sp, sp, #0x60
ADV_MODE = 0x88
ADV_CAPACITY = 5  # 0x1001f312c..0x1001f3144: 7-byte records, memset_s bound 0x23 (35 = 5 * 7)

CAP_OFFSETS = {
    name: 16 + index
    for index, name in enumerate(MODEL_CAPABILITIES)
    if name != 'adv_sets'
}
CAP_OFFSETS['adv_sets'] = 28

NATIVE_BINDINGS = dict(
    STATE=NATIVE_STATE,
    STATE_BYTES=NATIVE_STATE_BYTES,
    DESCRIPTORS=DESCRIPTORS,
    DESCRIPTOR_COUNT=DESCRIPTOR_COUNT,
    SUPPORT_TABLE=SUPPORT_TABLE,
    SUPPORT_BYTES=SUPPORT_BYTES,
    LE_FEATURES=LE_FEATURES,
    SUBMIT=COMMAND_SUBMIT,
    ADVANCE=0x100181a48,
    CHAIN_FAIL=0x10023e254,
    CHAIN_CONTEXT=CHAIN_CONTEXT,
    INIT_SUBMIT_FLAG=0x100945804,
    MODEL=MODEL_GETTER,
    SET_EXT_ADV=0x1001f3698,
    SET_SPEC_ADV=0x1001f368c,
    SET_ADV_HANDLES=0x1001f36b4,
    SET_ENH_CONNECTION=0x100204898,
    SET_EXT_SCAN=0x100216f34,      # writes 0x10093c32d (Apple VSC scan ext -> false)
    SET_EXT_SCAN_HCI=0x100216ee0,  # writes 0x10093c32e (standard HCI 0x2041/0x2042)
    SET_CODED_SCAN=0x100217018,    # writes 0x10093c329 (LowEnergyLongRange)
    SET_PERIODIC_ADV=0x1001f4c18,
    SET_PERIODIC_SYNC=0x100105c34,
    CACHE_EXT_ADV=0x10093e8c0,
    CACHE_EXT_SCAN=0x10093e8b0,
    CACHE_SPEC_ADV=0x10093ea28,
    CACHE_ENH_CONNECTION=0x10093ea38,
    CACHE_PERIODIC_ADV=0x10093e8d0,
    CACHE_PERIODIC_SYNC=0x10093e8e0,
    CACHE_PAST_SEND=0x10093e8f0,
    CACHE_PAST_RECV=0x10093e900,
    CACHE_CODED_PHY=0x10093e9e0,
    CACHE_PHY_2M=0x100927a68,
    LEGACY_STEP=0x1007a7deb,
    ALL_STEP=0x1007a7deb,
    MODEL_MULTIPLE_ADV=0xa0,
    ADV_MODE=ADV_MODE,
    ADV_CAPACITY=ADV_CAPACITY,
    ID_ADV_SETS=0xd1,
)

BZERO = 0x100708bcc
NOP = 0xd503201f
RET = 0xd65f03c0
BLR_X16 = 0xd63f0200


def relative(pc: int, target: int, bits: int) -> int:
    delta = target - pc
    if delta % 4 or not -(1 << (bits + 1)) <= delta < (1 << (bits + 1)):
        raise ValueError(f'branch outside aligned range: {pc:#x} -> {target:#x}')
    return (delta // 4) & ((1 << bits) - 1)


def branch(pc: int, target: int, link: bool = False) -> int:
    return (0x94000000 if link else 0x14000000) | relative(pc, target, 26)


def cbnz(pc: int, reg: int, target: int) -> int:
    return 0x35000000 | relative(pc, target, 19) << 5 | reg


def cbz(pc: int, reg: int, target: int) -> int:
    return 0x34000000 | relative(pc, target, 19) << 5 | reg


def adrp(pc: int, reg: int, target: int) -> int:
    pages = ((target & ~4095) - (pc & ~4095)) >> 12
    if not -(1 << 20) <= pages < (1 << 20):
        raise ValueError('ADRP outside range')
    pages &= (1 << 21) - 1
    return 0x90000000 | (pages & 3) << 29 | (pages >> 2) << 5 | reg


def add(reg: int, source: int, value: int) -> int:
    if not 0 <= value < 4096:
        raise ValueError('ADD immediate outside range')
    return 0x91000000 | value << 10 | source << 5 | reg


def memory(op: int, reg: int, base: int, offset: int, scale: int) -> int:
    if offset % scale or not 0 <= offset // scale < 4096:
        raise ValueError('load/store offset outside range')
    return op | (offset // scale) << 10 | base << 5 | reg


def mov_w(reg: int, value: int) -> int:
    if not 0 <= value <= 65535:
        raise ValueError('MOV immediate outside range')
    return 0x52800000 | value << 5 | reg


def decode_signed_imm19_target(site: int, word: int) -> int:
    offset = (word >> 5) & 0x7ffff
    if offset & (1 << 18):
        offset -= 1 << 19
    return site + (offset << 2)


class Image:
    """Minimal thin 64-bit little-endian Mach-O reader."""

    def __init__(self, path: Path | str):
        self.data = Path(path).read_bytes()
        d = self.data
        if len(d) < 32 or d[:4] != b'\xcf\xfa\xed\xfe':
            raise ValueError('requires a thin 64-bit little-endian Mach-O')
        _, self.cpu, self.subtype, _, count, sizeofcmds, _, _ = struct.unpack_from('<8I', d)
        self.segments = []
        self.sections = []
        self.functions = []
        self.uuid = None
        self.load_commands_end = 32 + sizeofcmds
        at = 32
        starts = None
        symtab = None
        indirect = None
        for _ in range(count):
            if at + 8 > len(d):
                raise ValueError('truncated load command')
            cmd, size = struct.unpack_from('<II', d, at)
            if size < 8 or at + size > len(d):
                raise ValueError('invalid load command')
            if cmd == 0x19:  # LC_SEGMENT_64
                name, va, vs, off, fs, _, _, n, _ = struct.unpack_from('<16sQQQQIIII', d, at + 8)
                self.segments.append((name.rstrip(b'\0').decode(), va, vs, off, fs))
                for i in range(n):
                    sname, seg, addr, nbytes, offset = struct.unpack_from('<16s16sQQI', d, at + 72 + i * 80)
                    flags, r1, r2 = struct.unpack_from('<III', d, at + 72 + i * 80 + 64)
                    self.sections.append(dict(
                        name=sname.rstrip(b'\0').decode(),
                        segment=seg.rstrip(b'\0').decode(),
                        address=addr,
                        size=nbytes,
                        offset=offset,
                        flags=flags,
                        reserved1=r1,
                        reserved2=r2,
                    ))
            elif cmd == 0x1b:  # LC_UUID
                self.uuid = str(uuid.UUID(bytes=d[at + 8:at + 24])).upper()
            elif cmd == 0x26:  # LC_FUNCTION_STARTS
                starts = struct.unpack_from('<II', d, at + 8)
            elif cmd == 0x2:  # LC_SYMTAB
                symtab = struct.unpack_from('<4I', d, at + 8)
            elif cmd == 0xb:  # LC_DYSYMTAB
                indirect = struct.unpack_from('<II', d, at + 56)
            at += size
        self.base = next(v for name, v, _, _, _ in self.segments if name == '__TEXT')
        self.stubs = {}
        if symtab and indirect:
            symoff, symcount, stroff, strsize = symtab
            inoff, incount = indirect
            for s in self.sections:
                if (s['flags'] & 255) != 8 or not s['reserved2']:
                    continue
                for i in range(s['size'] // s['reserved2']):
                    index = s['reserved1'] + i
                    if index >= incount:
                        raise ValueError('indirect symbol index outside table')
                    n = struct.unpack_from('<I', d, inoff + index * 4)[0]
                    if n & 0xc0000000:
                        continue
                    if n >= symcount:
                        raise ValueError('symbol index outside table')
                    name_off = struct.unpack_from('<I', d, symoff + n * 16)[0]
                    if name_off >= strsize:
                        raise ValueError('symbol string outside table')
                    end = d.index(0, stroff + name_off, stroff + strsize)
                    self.stubs[d[stroff + name_off:end].decode()] = s['address'] + i * s['reserved2']
        if starts:
            at, end = starts[0], starts[0] + starts[1]
            address = self.base
            while at < end:
                delta = shift = 0
                while at < end:
                    b = d[at]
                    at += 1
                    delta |= (b & 127) << shift
                    shift += 7
                    if not (b & 128):
                        break
                if not delta:
                    break
                address += delta
                self.functions.append(address)

    def address(self, offset: int) -> int | None:
        for _, va, _, off, size in self.segments:
            if off <= offset < off + size:
                return va + offset - off
        return None

    def offset(self, address: int) -> int:
        for _, va, _, off, size in self.segments:
            if va <= address < va + size:
                return off + address - va
        raise ValueError(f'unbacked virtual address: {address:#x}')

    def word(self, address: int) -> int:
        return struct.unpack_from('<I', self.data, self.offset(address))[0]

    def function(self, address: int):
        i = bisect.bisect_right(self.functions, address) - 1
        if i < 0:
            return None
        return [hex(self.functions[i]), hex(self.functions[i + 1]) if i + 1 < len(self.functions) else None]


class NativeImage(Image):
    """Audited patcher for stock iPadOS 17.7.11 (21H461) arm64 bluetoothd."""

    def __init__(self, path: Path | str):
        super().__init__(path)
        if hashlib.sha256(self.data).hexdigest() != ORIGINAL_SHA256:
            raise ValueError('bluetoothd must match the locked iPadOS 17.7.11 (21H461) original SHA-256')
        if self.uuid != ORIGINAL_UUID or self.cpu != ORIGINAL_CPU or self.subtype != ORIGINAL_SUBTYPE:
            raise ValueError('bluetoothd UUID or arm64 cpu/subtype differs')
        self.original = self.data
        self.data = bytearray(self.data)
        self.cursor = CAVE
        self.patches = []
        self.entries = {}
        self.cdhash = None
        self.fixups = self.chained_fixups()
        start, end = self.offset(CAVE), self.offset(CAVE_END - 1) + 1
        if any(self.data[start:end]):
            raise ValueError('executable cave is occupied')
        for section in self.sections:
            if max(CAVE, section['address']) < min(CAVE_END, section['address'] + section['size']):
                raise ValueError('executable cave overlaps a section')
        text = next(s for s in self.segments if s[0] == '__TEXT')
        if CAVE_END > text[1] + text[4]:
            raise ValueError('executable cave is not file-backed __TEXT')
        if self.stubs.get('_bzero') != BZERO:
            raise ValueError(f'bzero import differs: {self.stubs.get("_bzero")}')

    def chained_fixups(self) -> dict[int, int]:
        """Enumerate DYLD_CHAINED_PTR_64_OFFSET (format 6) chains without altering links.

        Format 6 layout (mach-o/fixup-chains.h):
          bit 63:     bind (0 = rebase, 1 = bind)
          bits 62:51: next (12-bit stride in 4-byte units)
          bits 50:44: reserved
          bits 43:36: high8
          bits 35:0:  target offset from __TEXT base
        """
        at = 32
        payload = None
        count = struct.unpack_from('<I', self.data, 16)[0]
        for _ in range(count):
            command, size = struct.unpack_from('<II', self.data, at)
            if command == 0x80000034:  # LC_DYLD_CHAINED_FIXUPS
                payload, length = struct.unpack_from('<II', self.data, at + 8)
                if payload + length > len(self.data):
                    raise ValueError('chained fixups extend beyond image')
            at += size
        if payload is None:
            raise ValueError('native image has no chained fixups')
        version, starts = struct.unpack_from('<II', self.data, payload)
        if version != 0:
            raise ValueError('unknown chained fixup version')
        starts += payload
        seg_count = struct.unpack_from('<I', self.data, starts)[0]
        if seg_count != len(self.segments):
            raise ValueError('chained fixup segment count differs')
        result: dict[int, int] = {}
        for index, segment in enumerate(self.segments):
            delta = struct.unpack_from('<I', self.data, starts + 4 + index * 4)[0]
            if not delta:
                continue
            header = starts + delta
            size, page_size, form, offset, _, pages = struct.unpack_from('<IHHQIH', self.data, header)
            if form != 6 or page_size != 16384 or size < 22 + pages * 2:
                raise ValueError(f'unexpected chained fixup format {form} / page_size {page_size}')
            if self.base + offset != segment[1]:
                raise ValueError('chained fixup segment address differs')
            for page in range(pages):
                first = struct.unpack_from('<H', self.data, header + 22 + page * 2)[0]
                if first == 0xffff:
                    continue
                if first & 0x8000 or first >= page_size:
                    raise ValueError('unsupported multi-start native fixup page')
                page_va = self.base + offset + page * page_size
                va = page_va + first
                while True:
                    if va in result or va + 8 > page_va + page_size:
                        raise ValueError('native fixup chain crosses its page')
                    raw = struct.unpack_from('<Q', self.data, self.offset(va))[0]
                    result[va] = raw
                    step = (raw >> 51) & 0xfff
                    if not step:
                        break
                    va += step * 4
        return result

    def capability_slots(self) -> dict[str, dict[str, str]]:
        text = next(s for s in self.segments if s[0] == '__TEXT')
        text_lo, text_hi = text[1], text[1] + text[4]
        result = {}
        for name, slot in MODEL_CAPABILITIES.items():
            address = MODEL_VTABLE + slot
            raw = self.fixups.get(address)
            if raw is None or (raw >> 63) != 0 or ((raw >> 36) & 0xff) != 0:
                raise ValueError(f'{name} is not a DYLD_CHAINED_PTR_64_OFFSET rebase')
            target = self.base + (raw & 0xfffffffff)
            if not (text_lo <= target < text_hi):
                raise ValueError(f'{name} rebase target outside __TEXT: {target:#x}')
            result[name] = dict(
                slot=hex(slot),
                address=hex(address),
                target=hex(target),
                next=hex((raw >> 51) & 0xfff),
            )
        return result

    def put(self, va: int, words: list[int], expected: list[int] | None = None) -> None:
        if expected is not None:
            actual = [self.word(va + 4 * n) for n in range(len(expected))]
            if actual != expected:
                raise ValueError(
                    f'instruction mismatch at {va:#x}: got {[hex(w) for w in actual]}, '
                    f'expected {[hex(w) for w in expected]}'
                )
        for n, word in enumerate(words):
            struct.pack_into('<I', self.data, self.offset(va + 4 * n), word)
        if expected is not None:
            self.patches.append(dict(
                address=hex(va),
                original=[hex(w) for w in expected],
                patched=[hex(w) for w in words],
            ))

    def emit(self, name: str, words: list) -> int:
        start = self.cursor
        if start + 4 * len(words) > CAVE_END:
            raise ValueError('native trampoline cave exhausted')
        resolved = [word(start + n * 4) if callable(word) else word for n, word in enumerate(words)]
        self.put(start, resolved)
        self.cursor += 4 * len(words)
        self.entries[name] = hex(start)
        return start

    def address_words(self, reg: int, address: int) -> list:
        return [lambda pc: adrp(pc, reg, address), add(reg, reg, address & 4095)]

    def descriptor(self, opcode: int) -> tuple[int, int]:
        matches = []
        for index in range(DESCRIPTOR_COUNT):
            va = DESCRIPTORS + index * 24
            op, bits = struct.unpack_from('<HH', self.data, self.offset(va) + 2)
            if op == opcode:
                matches.append((index, bits))
        if len(matches) != 1:
            raise ValueError(f'opcode {opcode:#x} has no unique native descriptor')
        return matches[0]

    def descriptors_bytes(self) -> bytes:
        start = self.offset(DESCRIPTORS)
        return bytes(self.original[start:start + DESCRIPTOR_COUNT * 24])

    def vendor_routing(self) -> NativeImage:
        # Force controller factory paths to Type 0 (standard H4).
        for site in FACTORIES:
            self.put(site, [mov_w(1, 0)], [mov_w(1, 15)])

        entry = self.emit('controller_type', [
            mov_w(1, 0),
            lambda pc: branch(pc, CONTROLLER_INIT),
        ])
        self.put(CONTROLLER_THUNK, [branch(CONTROLLER_THUNK, entry)],
                 [branch(CONTROLLER_THUNK, CONTROLLER_INIT)])

        # Unknown board product ID falls through into the final model constructor.
        self.put(UNKNOWN_PRODUCT_CHECK, [NOP], [0x34000400])

        # Ignore ADT vmm-present so chip initialization proceeds normally.
        self.put(VIRTUAL_MACHINE_CHECK, [mov_w(0, 0), RET], [0xa9bf7bfd, 0x910003fd])

        # Treat host flow control & erroneous data reporting rejections as non-fatal.
        self.put(HOST_BUFFER_STATUS, [0xaa1f03e0], [0xaa1303e0])
        self.put(HOST_FLOW_UNSUPPORTED, [NOP], [0x540001c1])
        self.put(ERRONEOUS_STATUS, [0xaa1f03e0], [0xaa1303e0])

        # Queue 0x2005 (LE Set Random Address) before per-set address updates so
        # concurrent random-address scanning never fails with Invalid Parameters.
        start = self.cursor
        skip = start + 8 * 4
        entry = self.emit('address_first', [
            lambda pc: branch(pc, ADDRESS_REFRESH_TARGET, True),
            0x39400268,  # ldrb w8, [x19]
            lambda pc: cbz(pc, 8, skip),
            0x91000668,  # add x8, x19, #1
            0xf90003e8,  # str x8, [sp]
            0xaa1f03e1,  # mov x1, xzr
            mov_w(0, SET_RANDOM_ADDRESS),
            lambda pc: branch(pc, COMMAND_SUBMIT, True),
            lambda pc: branch(pc, ADDRESS_REFRESH + 4),
        ])
        self.put(ADDRESS_REFRESH, [branch(ADDRESS_REFRESH, entry)],
                 [branch(ADDRESS_REFRESH, ADDRESS_REFRESH_TARGET, True)])

        # Always send 0x2035 (LE Set Advertising Set Random Address) instead of
        # skipping when an earlier 0x42 rejection polluted the cached address.
        self.put(ADV_ADDRESS_CACHED, [NOP], [0x340002e8])

        # On fallback when no RPA can be generated, x19 was clobbered by os_log;
        # clear busy flag via _refreshLeAddressDone + notify before returning.
        entry = self.emit('address_fallback_finish', [
            lambda pc: branch(pc, ADDRESS_FALLBACK_DONE, True),
            lambda pc: branch(pc, ADDRESS_FALLBACK_NOTIFY, True),
            lambda pc: branch(pc, ADDRESS_RETURN),
        ])
        self.put(ADDRESS_FALLBACK, [branch(ADDRESS_FALLBACK, entry)],
                 [branch(ADDRESS_FALLBACK, ADDRESS_RETURN)])

        # Preserve native range (1..3999) while accepting chip type 0.
        start = self.cursor
        entry = self.emit('registration', [
            lambda pc: cbnz(pc, 0, start + 8),
            lambda pc: branch(pc, REGISTER_ACCEPT),
            0x51000409,  # sub w9, w0, #1
            lambda pc: branch(pc, REGISTER + 4),
        ])
        self.put(REGISTER, [branch(REGISTER, entry)], [0x51000409])

        # Skip vendor backend setup for Type 0; zero vendor ops & hooks and return.
        start = self.cursor
        words = [None] + self.address_words(0, VENDOR_OPS) + [
            mov_w(1, VENDOR_OPS_BYTES),
            lambda pc: branch(pc, BZERO, True),
        ]
        for address in VENDOR_HOOKS:
            words.extend([
                lambda pc, a=address: adrp(pc, 8, a),
                memory(0xf9000000, 31, 8, address & 4095, 8),
            ])
        words.append(lambda pc: branch(pc, VENDOR_INIT_EPILOGUE))
        original = start + 4 * len(words)
        words[0] = lambda pc: cbnz(pc, 0, original)
        words.extend([0x51000408, lambda pc: branch(pc, VENDOR_INIT_SITE + 4)])
        entry = self.emit('vendor_init', words)
        self.put(VENDOR_INIT_SITE, [branch(VENDOR_INIT_SITE, entry)], [0x51000408])

        # Vendor callback registration hooks: return immediately for chip type 0.
        for n, (_, site, displaced, epilogue, _) in enumerate(CALLBACKS):
            start = self.cursor
            entry = self.emit(f'callbacks_{n}', [
                lambda pc, t=start + 8: cbnz(pc, 8, t),
                lambda pc, t=epilogue: branch(pc, t),
                displaced,
                lambda pc, t=site + 4: branch(pc, t),
            ])
            self.put(site, [branch(site, entry)], [displaced])

        # Skip optional HCI 0x0c5b when controller support bitmap clears its bit.
        index, bits = self.descriptor(0x0c5b)
        if (index, bits) != (0x7c, 0x1208):
            raise ValueError(f'optional 0x0c5b descriptor differs: {(hex(index), hex(bits))}')
        byte, mask = bits >> 8, bits & 255
        if byte >= SUPPORT_BYTES or mask != 8:
            raise ValueError('optional command support bit differs')
        address = SUPPORT_TABLE + byte
        start = self.cursor
        entry = self.emit('optional_0c5b', [
            lambda pc: adrp(pc, 16, address),
            memory(0x39400000, 16, 16, address & 4095, 1),
            lambda pc: 0x36180010 | relative(pc, start + 20, 14) << 5,  # tbz w16, #3, skip
            0xd100c3ff,  # sub sp, sp, #0x30
            lambda pc: branch(pc, OPTIONAL_STEP + 4),
            lambda pc: adrp(pc, 8, CHAIN_CONTEXT),
            memory(0xf9400000, 0, 8, CHAIN_CONTEXT & 4095, 8),
            lambda pc: branch(pc, CHAIN_NEXT),
        ])
        self.put(OPTIONAL_STEP, [branch(OPTIONAL_STEP, entry)], [0xd100c3ff])
        return self

    def volatile_root_keys(self) -> NativeImage:
        # Continue with generated root keys if Keychain is unavailable.
        for site in ROOT_KEY_READS:
            word = self.word(site)
            if (word & 0xff00001f) != 0x54000001:
                raise ValueError(f'root key read branch differs at {site:#x}: {word:#x}')
            target = decode_signed_imm19_target(site, word)
            self.put(site, [branch(site, target)], [word])
        for site in ROOT_KEY_WRITES:
            word = self.word(site)
            if (word & 0xff00001f) != 0x34000000:
                raise ValueError(f'root key write branch differs at {site:#x}: {word:#x}')
            target = decode_signed_imm19_target(site, word)
            self.put(site, [branch(site, target)], [word])
        self.put(ROOT_KEY_STORE_CHECK, [NOP], [0x350017c0])
        self.put(ROOT_KEY_FAILURE, [mov_w(24, 0)], [mov_w(24, 0x82)])
        self.put(ROOT_KEY_LOAD_RESULT, [0xaa1f03f8], [0xaa0003f8])
        return self

    def h4_shutdown(self) -> NativeImage:
        # Guard H4 (transport == 1) against NULL +0x60 pre-stop callback dereference.
        if self.word(H4_TABLE) != 1 or struct.unpack_from('<Q', self.data, self.offset(H4_TABLE) + 0x60)[0] != 0:
            raise ValueError('native H4 table or empty pre-stop extension differs')
        expected = [
            adrp(H4_PRESTOP, 8, TRANSPORT),
            memory(0xf9400000, 8, 8, TRANSPORT & 4095, 8),
            memory(0xb9400000, 9, 8, 0, 4),
        ]
        if [self.word(H4_PRESTOP + i * 4) for i in range(3)] != expected:
            raise ValueError('native pre-stop transport lookup differs')
        start = self.cursor
        entry = self.emit('h4_prestop', [
            lambda pc: adrp(pc, 8, TRANSPORT),
            expected[1],
            expected[2],
            0x7100053f,  # cmp w9, #1
            lambda pc: 0x54000001 | relative(pc, start + 24, 19) << 5,  # b.ne continue
            RET,
            lambda pc: branch(pc, H4_PRESTOP + 12),
        ])
        self.put(H4_PRESTOP, [branch(H4_PRESTOP, entry)], expected[:1])
        return self

    def capability_adapter(self) -> NativeImage:
        # Verify NATIVE_STATE lies inside __DATA zero-fill tail without overlapping any section.
        segment = next(s for s in self.segments if s[0] == '__DATA')
        if not (segment[1] <= NATIVE_STATE and NATIVE_STATE + NATIVE_STATE_BYTES <= segment[1] + segment[2]):
            raise ValueError('native capability storage is outside writable __DATA')
        for section in self.sections:
            if max(NATIVE_STATE, section['address']) < min(
                NATIVE_STATE + NATIVE_STATE_BYTES,
                section['address'] + section['size'],
            ):
                raise ValueError('native capability storage overlaps a section')
        if self.descriptor(0x203b) != (NATIVE_BINDINGS['ID_ADV_SETS'], 0x2480):
            raise ValueError('0x203b advertising sets descriptor differs')
        if self.descriptor(0x2005)[0] != SET_RANDOM_ADDRESS:
            raise ValueError('0x2005 set random address descriptor differs')

        self.capability_slots()
        for name, slot in MODEL_CAPABILITIES.items():
            address = NATIVE_STATE + CAP_OFFSETS[name]
            op, scale = (0xb9400000, 4) if name == 'adv_sets' else (0x39400000, 1)
            getter = self.emit('get_' + name, [
                lambda pc, a=address: adrp(pc, 8, a),
                memory(op, 0, 8, address & 4095, scale),
                RET,
            ])
            raw = self.fixups[MODEL_VTABLE + slot]
            # Preserve bind=0, next stride (bits 62:51), reserved, and high8 (bits 43:36).
            new = (raw & 0xfffffff000000000) | (getter - self.base)
            self.put(MODEL_VTABLE + slot, [new & 0xffffffff, new >> 32],
                     [raw & 0xffffffff, raw >> 32])

        # Publisher trampoline calls Type0.dylib publish() via NATIVE_STATE->publish.
        publisher = self.emit('publish', [
            0xa9bf7bfd,  # stp x29, x30, [sp, #-16]!
            0x910003fd,  # mov x29, sp
            lambda pc: adrp(pc, 16, NATIVE_STATE),
            memory(0xf9400000, 16, 16, NATIVE_STATE & 4095, 8),
            BLR_X16,
            0xa8c17bfd,  # ldp x29, x30, [sp], #16
            RET,
        ])

        entry, tail, epilogue, displaced = LE_FEATURES_CB
        capture = self.emit('features_0', [
            lambda pc: adrp(pc, 8, NATIVE_STATE),
            memory(0xb9000000, 0, 8, (NATIVE_STATE & 4095) + 64, 4),  # status = w0
            mov_w(9, 0),
            memory(0xb9000000, 9, 8, (NATIVE_STATE & 4095) + 68, 4),  # kind = 0
            memory(0xf9000000, 1, 8, (NATIVE_STATE & 4095) + 72, 8),  # parameters = x1
            displaced,
            lambda pc: branch(pc, entry + 4),
        ])
        self.put(entry, [branch(entry, capture)], [displaced])

        step_str = NATIVE_BINDINGS['LEGACY_STEP']
        expected_tail = [
            adrp(tail, 1, step_str),
            add(1, 1, step_str & 4095),
            mov_w(0, 0),
            branch(tail + 12, NATIVE_BINDINGS['ADVANCE'], True),
        ]
        self.put(tail, [branch(tail, publisher, True), branch(tail + 4, epilogue), NOP, NOP],
                 expected_tail)

        # Refresh LeBroadcaster advertising mode right before stackDidStart uses [x0 + 0x88].
        prepare = self.emit('prepare_advertising', [
            0xa9bf7be0,  # stp x0, x30, [sp, #-16]!
            lambda pc: adrp(pc, 16, NATIVE_STATE),
            memory(0xf9400000, 16, 16, (NATIVE_STATE & 4095) + 8, 8),
            BLR_X16,
            0xa8c17be0,  # ldp x0, x30, [sp], #16
            ADV_START_PROLOGUE,
            lambda pc: branch(pc, ADV_START + 4),
        ])
        self.put(ADV_START, [branch(ADV_START, prepare)], [ADV_START_PROLOGUE])

        # Append LC_LOAD_DYLIB for /usr/lib/QcaBTType0.dylib in the zeroed header slack.
        at = self.load_commands_end
        size = (24 + len(NATIVE_DYLIB) + 1 + 7) & ~7
        if at + size > self.offset(CAVE) or any(self.data[at:at + size]):
            raise ValueError('no empty load-command room for native library')
        command = struct.pack('<6I', 0xc, size, 24, 2, 0x10000, 0x10000) + NATIVE_DYLIB
        words = list(struct.unpack('<' + 'I' * (size // 4), command.ljust(size, b'\0')))
        self.put(self.base + at, words, [0] * len(words))
        count, total = struct.unpack_from('<II', self.data, 16)
        self.put(self.base + 16, [count + 1, total + size], [count, total])
        self.entries['capability_state'] = hex(NATIVE_STATE)
        self.entries['library'] = NATIVE_DYLIB.decode()
        return self

    def resign_adhoc(self) -> NativeImage:
        """Update the embedded ad-hoc 4 KiB-page LC_CODE_SIGNATURE in-place.

        Stock 21H461 bluetoothd already carries an ad-hoc SHA-256 4 KiB-page
        SuperBlob with XML (slot 5) and DER (slot 7) entitlements. Recomputing
        only the code page hashes preserves every entitlement and special slot
        byte-for-byte without requiring macOS codesign.
        """
        at = 32
        ncmds = struct.unpack_from('<I', self.original, 16)[0]
        cs_off = cs_size = 0
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', self.original, at)
            if cmd == 0x1d:  # LC_CODE_SIGNATURE
                cs_off, cs_size = struct.unpack_from('<II', self.original, at + 8)
                break
            at += size
        if not cs_off or cs_off + cs_size > len(self.data):
            raise ValueError('missing or invalid LC_CODE_SIGNATURE')

        sb = bytes(self.original[cs_off:cs_off + cs_size])
        magic, _sb_len, count = struct.unpack_from('>III', sb, 0)
        if magic != 0xfade0cc0:
            raise ValueError(f'unexpected CodeSignature SuperBlob magic: {magic:#x}')

        slots: dict[int, tuple[int, int]] = {}
        for i in range(count):
            slot, off = struct.unpack_from('>II', sb, 12 + i * 8)
            _bm, blen = struct.unpack_from('>II', sb, off)
            slots[slot] = (off, blen)
        if 0 not in slots:
            raise ValueError('CodeSignature missing CSSLOT_CODEDIRECTORY')

        cd_rel, cd_len = slots[0]
        cd_orig = sb[cd_rel:cd_rel + cd_len]
        if hashlib.sha256(cd_orig).hexdigest()[:40] != ORIGINAL_CDHASH:
            raise ValueError('stock bluetoothd CDHash mismatch before re-signing')

        cmagic, clen_field, _ver, flags, hoff, _ioff, nspec, ncode, code_limit, hsize, htype, _plat, pshift = (
            struct.unpack_from('>IIIIIIIIIBBBB', cd_orig, 0)
        )
        if (cmagic, clen_field, flags, hsize, htype, pshift, code_limit) != (
            0xfade0c02, cd_len, 0x2, 32, 2, 12, cs_off
        ):
            raise ValueError('unexpected CodeDirectory parameters for in-place ad-hoc resign')

        # Verify special slots (-2 requirements, -5 XML entitlements, -7 DER entitlements) stay intact.
        for s_slot in (2, 5, 7):
            if s_slot <= nspec and s_slot in slots:
                s_off, s_len = slots[s_slot]
                expected_hash = cd_orig[hoff - s_slot * 32 : hoff - (s_slot - 1) * 32]
                actual_hash = hashlib.sha256(sb[s_off:s_off + s_len]).digest()
                if actual_hash != expected_hash:
                    raise ValueError(f'special slot -{s_slot} hash mismatch')

        page_size = 1 << pshift
        if (code_limit + page_size - 1) // page_size != ncode:
            raise ValueError('CodeDirectory page count mismatch')

        # Self-check original page hashes against self.original before patching.
        orig_hashes = b''.join(
            hashlib.sha256(self.original[i * page_size : min((i + 1) * page_size, code_limit)]).digest()
            for i in range(ncode)
        )
        if cd_orig[hoff : hoff + ncode * 32] != orig_hashes:
            raise ValueError('stock CodeDirectory page hash self-check failed')

        new_hashes = b''.join(
            hashlib.sha256(self.data[i * page_size : min((i + 1) * page_size, code_limit)]).digest()
            for i in range(ncode)
        )
        cd_abs = cs_off + cd_rel
        self.data[cd_abs + hoff : cd_abs + hoff + ncode * 32] = new_hashes
        self.cdhash = hashlib.sha256(self.data[cd_abs : cd_abs + cd_len]).hexdigest()[:40]
        self.entries['cdhash'] = self.cdhash
        return self

    def report(self) -> dict:
        return dict(
            target='ipados-17.7.11-21H461-arm64',
            input_sha256=ORIGINAL_SHA256,
            input_uuid=self.uuid,
            input_cdhash=ORIGINAL_CDHASH,
            output_sha256=hashlib.sha256(self.data).hexdigest(),
            output_cdhash=self.cdhash,
            stage='native-capability-adapter' if 'library' in self.entries else 'vendor-routing-and-h4-shutdown',
            adapter_ready='library' in self.entries,
            hardware_verified=False,
            cave_start=hex(CAVE),
            cave_end=hex(self.cursor),
            cave_used=self.cursor - CAVE,
            cave_capacity=CAVE_END - CAVE,
            entries=self.entries,
            patches=self.patches,
            capability_slots=self.capability_slots(),
        )


def layout_header() -> str:
    """Generate the shared private ABI header for the version-locked 21H461 Type0 dylib."""
    uuid_bytes = uuid.UUID(ORIGINAL_UUID).bytes
    uuid_init = '{' + ', '.join(f'0x{b:02x}' for b in uuid_bytes) + '}'
    lines = [
        '/* Generated by native_ios17.py; external Apple image stays local. */',
        '#ifndef QBT_IOS17_LAYOUT_H',
        '#define QBT_IOS17_LAYOUT_H',
        '#include <stddef.h>',
        '#include <stdint.h>',
        f'#define QBT_NATIVE_UUID {uuid_init}',
    ]
    lines += [f'#define QBT_{name} 0x{address:x}ULL' for name, address in NATIVE_BINDINGS.items()]
    lines += [
        'struct qbt_native_state {',
        '    void (*publish)(void);',
        '    void (*prepare)(void *);',
    ]
    lines += [f'    uint8_t {name};' for name in MODEL_CAPABILITIES if name != 'adv_sets']
    lines += [
        '    uint8_t reserved[2];',
        '    uint32_t adv_sets;',
        '    uint8_t reserved_tail[32];',
        '    uint32_t status, kind;',
        '    uintptr_t parameters;',
        '};',
    ]
    for name, offset in CAP_OFFSETS.items():
        lines += [f'_Static_assert(offsetof(struct qbt_native_state, {name}) == {offset}, "{name} ABI");']
    lines += [
        '_Static_assert(offsetof(struct qbt_native_state, status) == 64, "status ABI");',
        '_Static_assert(offsetof(struct qbt_native_state, parameters) == 72, "parameters ABI");',
        '#endif',
        '',
    ]
    return '\n'.join(lines)


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Emit generated qbt_ios17_layout.h header')
    ap.add_argument('--layout-header', type=Path, required=True)
    ns = ap.parse_args()
    ns.layout_header.parent.mkdir(parents=True, exist_ok=True)
    ns.layout_header.write_text(layout_header(), encoding='utf-8')
    print(f'Wrote {ns.layout_header}')
