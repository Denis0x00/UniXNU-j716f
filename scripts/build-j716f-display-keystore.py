#!/usr/bin/env python3
"""Build libNativeLegacyDisplay.late.a (GopFramebuffer + AppleKeyStoreState) for J716F.

Cross-platform (Linux + macOS) archive builder for iPadOS 17.7.11 (21H461) arm64.
Derives and verifies all IOMobileFramebufferLegacy / IOSurface ABI bindings directly
against the decompressed 21H461 kernelcache at the prelink slide (-0x1000000), compiles
unmodified upstream GopFramebuffer (NativeLegacyDisplay.cpp, NativeGOPPresenter.cpp) and
AppleKeyStoreState (NativeKeyStoreEvents.cpp) with late-init staging, and links them with
the J716F board probes into build/native-legacy-display/libNativeLegacyDisplay.late.a.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/native-legacy-display'
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))

from kernel_target import ARCH, IOS_VERSION, TRIPLE
from native_prelink import backed_offset, macho

PRELINK_SLIDE = 0x1000000
LEGACY_VTABLE_LEN = 358
EXPECTED_PURE_SLOTS = [211, 213, 245, 246, 306, 307, 337, 338, 345]
EXPECTED_OWNED_SLOTS = sorted(
    [0, 1, 7, 14, 84, 86, 87, 141, 155, 184, 188, 288, 290, 296, 301] + EXPECTED_PURE_SLOTS
)
HID_VTABLE_LEN = 207
EXPECTED_HID_PURE_SLOTS = [180]
EXPECTED_HID_OWNED_SLOTS = [0, 1, 7, 14, 86, 87, 111, 171, 173, 180]


def require(cond: bool, why: str) -> None:
    if not cond:
        raise SystemExit(f'build-j716f-display-keystore: {why}')


def sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(cmd: list[str | Path], cwd: Path | None = None) -> str:
    proc = subprocess.run(
        [str(x) for x in cmd],
        cwd=str(cwd) if cwd else None,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        timeout=180,
    )
    if proc.returncode != 0:
        raise SystemExit(
            f'command failed ({proc.returncode}): {" ".join(str(x) for x in cmd)}\n{proc.stdout[-4000:]}'
        )
    return proc.stdout


def find_tool(names: list[str]) -> Path:
    candidates: list[Path] = []
    cross_clang = os.environ.get('CROSS_CLANG')
    if cross_clang:
        candidates.append(Path(cross_clang).parent)
    toolchain = os.environ.get('UNIXNU_TOOLCHAIN', '/opt/unixnu-linux')
    candidates.extend([
        Path(toolchain) / 'llvm/bin',
        Path(toolchain) / 'CommandLineTools/usr/bin',
        Path('/opt/homebrew/opt/llvm/bin'),
        Path('/usr/local/opt/llvm/bin'),
    ])
    for directory in candidates:
        for name in names:
            tool = directory / name
            if tool.is_file():
                return tool
    for name in names:
        found = shutil.which(name)
        if found:
            return Path(found)
    raise SystemExit(f'Missing required build tool: {names}')


def find_kernelcache() -> Path:
    env_kc = os.environ.get('UNIXNU_KERNELCACHE')
    if env_kc and Path(env_kc).is_file():
        return Path(env_kc)
    # Standard Format 2 layout: out/<board>/<os>/<defconfig>/tree -> out/ipsw/21H461/kernelcache.decompressed
    for parent in (ROOT.parents[3] if len(ROOT.parents) > 3 else ROOT, ROOT.parent):
        candidate = parent / 'ipsw/21H461/kernelcache.decompressed'
        if candidate.is_file():
            return candidate
    raise SystemExit(
        'Cannot locate 21H461 kernelcache.decompressed; pass UNIXNU_KERNELCACHE or run inside ./unixnu out/'
    )


def find_upstream_dir(rel: str) -> Path:
    for base in (ROOT, *ROOT.parents[:5]):
        cand = base / rel
        if cand.is_dir():
            return cand
    raise SystemExit(f'Cannot locate upstream UniXNU directory: {rel}')


def audit_accelerator_kernelcache(kc_path: Path) -> dict[str, int]:
    """Verify exact 21H461 IOAcceleratorES base bindings (136 B, 168 vtable slots, 0 operational overrides)."""
    raw = kc_path.read_bytes()
    im = macho(raw)
    segs = im['segments']

    def read_slid(prelinked_va: int, n: int) -> bytes:
        at = backed_offset(segs, prelinked_va + PRELINK_SLIDE, n, len(raw))
        return raw[at : at + n]

    expected = {
        'C2': (0xFFFFFFF004BD3B20, '9ed90924d429e607b964fdc110c4b6f7bedf6ade17ec2b021ed60279b42987ec'),
        'D2': (0xFFFFFFF004BD3B64, '456b2a9a418a432e7f58612f92318c20a88f5292ccbfe294a1f5f0404a300617'),
        'getMetaClass': (0xFFFFFFF004BD3BAC, '9daad2c67183fcc551bd461dac5b03fd1fc140c96995a09ec2a54d59532c0ce9'),
    }
    for label, (va, digest) in expected.items():
        got = hashlib.sha256(read_slid(va, 32)).hexdigest()
        require(got == digest, f'21H461 IOAcceleratorES {label} hash mismatch: {got} != {digest}')

    # Confirm vtable slots 0 (D2) and 7 (getMetaClass) at slid IOAcceleratorES vptr 0xfffffff005d439a0
    vptr_prelinked = 0xFFFFFFF005D439A0
    vt_raw = struct.unpack('<8Q', read_slid(vptr_prelinked, 64))
    require(
        (vt_raw[0] - PRELINK_SLIDE) == 0xFFFFFFF004BD3B64
        and (vt_raw[7] - PRELINK_SLIDE) == 0xFFFFFFF004BD3BAC,
        '21H461 IOAcceleratorES vtable D2/getMetaClass slot mismatch',
    )
    return {
        '__ZN15IOAcceleratorESC2EPK11OSMetaClass': 0xFFFFFFF004BD3B20,
        '__ZN15IOAcceleratorESD2Ev': 0xFFFFFFF004BD3B64,
        '__ZN15IOAcceleratorES10gMetaClassE': 0xFFFFFFF006AEB688,
    }


def code_bytes(raw: bytes) -> bytes:
    info = macho(raw)
    sec = next(s for g in info['segments'] for s in g['sections'] if s['name'] == '__text')
    return raw[sec['fileoff'] : sec['fileoff'] + sec['size']]


def audit_kernelcache(kc_path: Path) -> tuple[int, list[int], list[int], dict[str, int]]:
    blob = kc_path.read_bytes()
    img = macho(blob)

    def read_slid(va_slid: int, size: int) -> bytes:
        at = backed_offset(img['segments'], va_slid + PRELINK_SLIDE, size, len(blob))
        require(at is not None, f'unbacked kernelcache VA {va_slid:#x} (+slide)')
        return blob[at : at + size]

    def vptr_from_ctor(pc_slid: int) -> int:
        a, b, c = struct.unpack('<3I', read_slid(pc_slid, 12))
        require(
            (a & 0x9F00001F) == 0x90000008 and (b & 0xFFC003FF) == 0x91000108 and c == 0x91004108,
            f'unexpected constructor ADRP/ADD/vptr header at {pc_slid:#x}',
        )
        imm = ((a >> 5) & 0x7FFFF) * 4 + ((a >> 29) & 3)
        if imm & (1 << 20):
            imm -= 1 << 21
        pc_unslid = pc_slid + PRELINK_SLIDE
        vptr_unslid = (pc_unslid & ~4095) + (imm << 12) + ((b >> 10) & 4095) + 16
        return vptr_unslid - PRELINK_SLIDE

    def branch_target(pc_slid: int) -> int:
        w = struct.unpack('<I', read_slid(pc_slid, 4))[0]
        require((w & 0xFC000000) == 0x94000000, f'expected direct BL at {pc_slid:#x}')
        imm = w & 0x3FFFFFF
        if imm & (1 << 25):
            imm -= 1 << 26
        return pc_slid + 4 * imm

    # Verify IOMobileFramebufferLegacy -> Service -> Framebuffer constructor chain
    base_vptr = vptr_from_ctor(0xFFFFFFF00577B324)
    service_vptr = vptr_from_ctor(0xFFFFFFF00577B51C)
    legacy_vptr = vptr_from_ctor(0xFFFFFFF0057865D8)
    require(
        branch_target(0xFFFFFFF0057865FC) == 0xFFFFFFF00577B510
        and branch_target(0xFFFFFFF00577B540) == 0xFFFFFFF00577B318
        and branch_target(0xFFFFFFF005786630) == 0xFFFFFFF00577B55C,
        'original Legacy -> Service -> Framebuffer constructor/destructor chain mismatch',
    )

    base_vt = struct.unpack('<262Q', read_slid(base_vptr, 262 * 8))
    service_vt = struct.unpack('<262Q', read_slid(service_vptr, 262 * 8))
    require(
        [i for i, (a, b) in enumerate(zip(base_vt, service_vt)) if a != b] == [0, 1, 7],
        'IOMobileFramebufferService overrides unexpected slots',
    )

    legacy_vt_unslid = struct.unpack(f'<{LEGACY_VTABLE_LEN}Q', read_slid(legacy_vptr, LEGACY_VTABLE_LEN * 8))
    require(
        all(legacy_vt_unslid) and read_slid(legacy_vptr + LEGACY_VTABLE_LEN * 8, 24) == bytes(24),
        'unexpected IOMobileFramebufferLegacy vtable extent or boundary',
    )
    pure_target = legacy_vt_unslid[EXPECTED_PURE_SLOTS[0]]
    pure = [i for i, va in enumerate(legacy_vt_unslid) if va == pure_target]
    require(pure == EXPECTED_PURE_SLOTS, f'unexpected abstract slots: {pure}')

    # Slide every IOMobileGraphicsFamily slot by -PRELINK_SLIDE
    legacy_vt_slid = [va - PRELINK_SLIDE for va in legacy_vt_unslid]

    # Verify key original methods and instruction signatures
    require(
        read_slid(0xFFFFFFF005787EF8, 16) == bytes.fromhex('080040f9088144f900013fd6e0000037'),
        'startHardware() call/check mismatch',
    )
    getter = legacy_vt_slid[249]
    for off, field in [(0x30, 5808), (0x58, 5812)]:
        insn = struct.unpack('<I', read_slid(getter + off, 4))[0]
        require(insn == (0xB9400000 | ((field // 4) << 10) | (8 << 5) | 8), 'width/height getter field mismatch')
    require(
        legacy_vt_slid[295] == 0xFFFFFFF005795B88
        and read_slid(0xFFFFFFF005795B98, 16) == bytes.fromhex('e00740f9280080520101001272d3ff97'),
        'completeCurrentSwap() slot 295 mismatch',
    )
    require(
        read_slid(0xFFFFFFF00578AA7C, 24)
        == bytes.fromhex('080040f9084545f9030080d2e20303aa0400805200013fd6'),
        'collectCompletionReports() call mismatch',
    )
    require(
        read_slid(0xFFFFFFF005777E9C, 16) == bytes.fromhex('080040f908e142f900013fd6c0000036'),
        'isMainDisplay() dispatch mismatch',
    )
    require(
        read_slid(0xFFFFFFF005796720, 16) == bytes.fromhex('ff0302d1fd7b07a9fdc30191a0831ff8')
        and read_slid(0xFFFFFFF005796830, 4) == bytes.fromhex('08ed4af9')
        and read_slid(0xFFFFFFF00579685C, 16) == bytes.fromhex('e2230091c3008052240080521684ff97')
        and read_slid(0xFFFFFFF005795FBC, 4) == bytes.fromhex('0ab182d2'),
        'original vsync notify sender mismatch',
    )
    require(
        legacy_vt_slid[301] == 0xFFFFFFF00579EA5C
        and read_slid(0xFFFFFFF00579EA68, 8) == bytes.fromhex('888888d28888a0f2')
        and read_slid(0xFFFFFFF00579EC54, 4) == bytes.fromhex('08b544f9'),
        'slot 301 refresh words mismatch',
    )
    require(
        read_slid(0xFFFFFFF00579EBB8, 16) == bytes.fromhex('ff0302d1fd7b07a9fdc30191a0031ff8')
        and read_slid(0xFFFFFFF00579ED54, 16) == bytes.fromhex('086540f90173ff90218c0c9100013fd6'),
        'setDisplayRefreshProperties mismatch',
    )

    fixed_bindings: dict[str, int] = {
        '__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass': 0xFFFFFFF0057865CC,
        '__ZN25IOMobileFramebufferLegacyD2Ev': 0xFFFFFFF005786618,
        '__ZN25IOMobileFramebufferLegacy10gMetaClassE': 0xFFFFFFF006B35940,
        '__ZTV25IOMobileFramebufferLegacy': legacy_vptr - 16,
        '_native_legacy_vsync_notify': 0xFFFFFFF005796720,
        '_native_legacy_set_refresh_properties': 0xFFFFFFF00579EBB8,
    }

    for name, address, field, wide in [
        ('descriptor', 0xFFFFFFF004B9EAD0, 48, True),
        ('width', 0xFFFFFFF004B9EBBC, 88, True),
        ('height', 0xFFFFFFF004B9EBC4, 96, True),
        ('stride', 0xFFFFFFF004B9EBD4, 104, True),
        ('allocation', 0xFFFFFFF004B9EBFC, 144, True),
        ('format', 0xFFFFFFF004B9EBAC, 128, False),
        ('planes', 0xFFFFFFF004B9EBF4, 152, False),
    ]:
        expected = (0xF9400000 if wide else 0xB9400000) | ((field // (8 if wide else 4)) << 10)
        require(
            read_slid(address, 8) == struct.pack('<2I', expected, 0xD65F03C0),
            f'original IOSurface getter {name} mismatch at {address:#x}',
        )
        fixed_bindings['_native_legacy_surface_' + name] = address

    return legacy_vptr, legacy_vt_slid, pure, fixed_bindings


def audit_hid_kernelcache(kc_path: Path) -> tuple[int, list[int], dict[str, int]]:
    blob = kc_path.read_bytes()
    img = macho(blob)

    def read_slid(va_slid: int, size: int) -> bytes:
        at = backed_offset(img['segments'], va_slid + PRELINK_SLIDE, size, len(blob))
        require(at is not None, f'unbacked HID kernelcache VA {va_slid:#x} (+slide)')
        return blob[at : at + size]

    hid_vptr = 0xFFFFFFF005D045C0
    hid_vt_unslid = struct.unpack(f'<{HID_VTABLE_LEN}Q', read_slid(hid_vptr, HID_VTABLE_LEN * 8))
    require(
        all(hid_vt_unslid) and read_slid(hid_vptr + HID_VTABLE_LEN * 8, 24) == bytes(24),
        'unexpected IOHIDDevice vtable extent or boundary',
    )
    pure_target = hid_vt_unslid[EXPECTED_HID_PURE_SLOTS[0]]
    pure = [i for i, va in enumerate(hid_vt_unslid) if va == pure_target]
    require(pure == EXPECTED_HID_PURE_SLOTS, f'unexpected IOHIDDevice abstract slots: {pure}')

    hid_vt_slid = [va - PRELINK_SLIDE for va in hid_vt_unslid]
    require(
        read_slid(0xFFFFFFF004AD94F8, 0x20)
        == bytes.fromhex('fd7bbfa9fd0300912bd90094489100f008011791080000f9fd7bc1a8c0035fd6'),
        'IOHIDDevice C2 constructor / vptr store mismatch',
    )
    require(
        read_slid(0xFFFFFFF004AD9AEC, 0x34)
        == bytes.fromhex(
            '680240f908a142f9e00313aae10314aa00013fd6680240f9603d003608d142f9e1630091e00313aa00013fd6a02d0035e00f40f9'
        ),
        'IOHIDDevice handleStart / report-descriptor ABI mismatch',
    )
    require(
        hid_vt_slid[181] == 0xFFFFFFF004ADD0C4
        and read_slid(0xFFFFFFF004ADD110, 28)
        == bytes.fromhex('680240f9081543f9e00313aae20316aae30315aae40314aa00013fd6'),
        'IOHIDDevice handleReport ABI mismatch',
    )

    fixed_bindings: dict[str, int] = {
        '__ZN11IOHIDDeviceC2EPK11OSMetaClass': 0xFFFFFFF004AD94F8,
        '__ZN11IOHIDDeviceD2Ev': 0xFFFFFFF004AD9518,
        '__ZN11IOHIDDevice10gMetaClassE': 0xFFFFFFF006AE6FA0,
        '__ZTV11IOHIDDevice': hid_vptr - 16,
    }
    return hid_vptr, hid_vt_slid, fixed_bindings


def write_hid_abi_header() -> Path:
    lines = [
        '// SPDX-License-Identifier: BSD-2-Clause-Patent',
        '// Generated from 21H461 IOHIDDevice vtable audit.',
        '#pragma once',
        '#include <IOKit/IOService.h>',
        '#include <IOKit/IOMemoryDescriptor.h>',
        'class IOHIDDevice : public IOService {',
        ' OSDeclareAbstractStructors(IOHIDDevice)',
        ' unsigned char originalPrivate[0xd0 - sizeof(IOService)];',
        'public:',
        ' using IOService::setProperty;',
        ' IOReturn Dispatch(IORPC) override;',
        ' void free() override;',
        ' bool init(OSDictionary *properties = nullptr) override;',
        ' bool setProperty(const OSSymbol *, OSObject *) override;',
        ' bool willTerminate(IOService *, IOOptionBits) override;',
        ' IOReturn configureReport(IOReportChannelList *, IOReportConfigureAction, void *, void *) override;',
        ' IOReturn updateReport(IOReportChannelList *, IOReportUpdateAction, void *, void *) override;',
        ' bool start(IOService *) override;',
        ' void stop(IOService *) override;',
        ' bool handleOpen(IOService *, IOOptionBits, void *) override;',
        ' void handleClose(IOService *, IOOptionBits) override;',
        ' bool handleIsOpen(const IOService *) const override;',
        ' bool matchPropertyTable(OSDictionary *, SInt32 *) override;',
        ' IOReturn message(UInt32, IOService *, void *) override;',
        ' IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;',
    ]
    for i in range(168, HID_VTABLE_LEN):
        if i == 171:
            lines.append(' virtual OSString *newTransportString() const;')
        elif i == 173:
            lines.append(' virtual OSString *newProductString() const;')
        elif i == 180:
            lines.append(' virtual IOReturn newReportDescriptor(IOMemoryDescriptor **) const = 0;')
        elif i == 181:
            lines.append(' virtual IOReturn handleReport(IOMemoryDescriptor *, unsigned, IOOptionBits);')
        else:
            lines.append(
                f' virtual __attribute__((deprecated("Unqualified prototype: never call from source"))) void opaque{i}();'
            )
    lines.append('};')
    lines.append('static_assert(sizeof(IOHIDDevice) == 0xd0, "Original device instance size");')
    header = OUT / 'OriginalHidDeviceABI.h'
    header.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return header


def write_abi_header(pure: list[int]) -> Path:
    lines = [
        '// SPDX-License-Identifier: BSD-2-Clause-Patent',
        '// Generated from 21H461 IOMobileFramebufferLegacy vtable audit.',
        '#pragma once',
        '#include <IOKit/IOService.h>',
        '#include <libkern/libkern.h>',
        'struct OriginalLegacySwapRequest;',
        'struct OriginalLegacyStateRequest { uint32_t skipRequested, requested, skipPowered, powered; uint8_t changed; uint8_t pad[11]; };',
        'static constexpr unsigned kLegacyVsyncClients = 5592;',
        'static constexpr unsigned kRequestRect0 = 17, kRequestRect1 = 81, kRequestMasks = 792, kRequestOptions = 804, kRequestDims = 816, kRequestSurfaces = 2624;',
        '#define NATIVE_LEGACY_LEAVES(LEAF) LEAF(211) LEAF(245) LEAF(246) LEAF(306) LEAF(307) LEAF(338) LEAF(345)',
        'class IOMobileFramebufferLegacy : public IOService {',
        ' OSDeclareAbstractStructors(IOMobileFramebufferLegacy)',
        ' unsigned char originalPrivate[0x18a0 - sizeof(IOService)];',
        'public:',
        ' void installBootDimensions(uint32_t width, uint32_t height) {',
        '  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this) + 5808) = width;',
        '  *reinterpret_cast<uint32_t *>(reinterpret_cast<unsigned char *>(this) + 5812) = height;',
        ' }',
        ' const OriginalLegacySwapRequest *pendingSwapRequest() const {',
        '  const OriginalLegacySwapRequest *request = nullptr;',
        '  memcpy(&request, reinterpret_cast<const unsigned char *>(this) + 5480, sizeof(request));',
        '  return request;',
        ' }',
        ' bool activateOnlyPendingSwap(const OriginalLegacySwapRequest *request) {',
        '  auto self = reinterpret_cast<unsigned char *>(this);',
        '  auto bytes = reinterpret_cast<const unsigned char *>(request);',
        '  const void *current = nullptr, *head = nullptr, *tail = nullptr, *next = nullptr, *previous = nullptr;',
        '  memcpy(&current, self + 5472, 8); memcpy(&head, self + 5480, 8); memcpy(&tail, self + 5488, 8);',
        '  if (!request || current || head != request) return false;',
        '  memcpy(&next, bytes + 2960, 8); memcpy(&previous, bytes + 2968, 8);',
        '  if (previous != self + 5480) return false;',
        '  if (next) {',
        '   const void *link = self + 5480;',
        '   memcpy(const_cast<unsigned char *>(reinterpret_cast<const unsigned char *>(next)) + 2968, &link, 8);',
        '  } else if (tail != bytes + 2960) return false;',
        '  memcpy(self + 5472, &request, 8);',
        '  if (next) { memcpy(self + 5480, &next, 8); }',
        '  else { const void *empty = nullptr, *emptyTail = self + 5480; memcpy(self + 5480, &empty, 8); memcpy(self + 5488, &emptyTail, 8); }',
        '  return true;',
        ' }',
        ' void free() override;',
        ' bool init(OSDictionary *properties = nullptr) override;',
        ' IOReturn setProperties(OSObject *) override;',
        ' void systemWillShutdown(IOOptionBits) override;',
        ' IOService *probe(IOService *, SInt32 *) override;',
        ' bool start(IOService *) override;',
        ' void stop(IOService *) override;',
        ' IOWorkLoop *getWorkLoop() const override;',
        ' IOReturn newUserClient(task_t, void *, UInt32, IOUserClient **) override;',
        ' IOReturn setPowerState(unsigned long, IOService *) override;',
    ]
    for i in range(168, LEGACY_VTABLE_LEN):
        if i == 184:
            lines.append(' virtual bool isMainDisplay();')
        elif i == 188:
            lines.append(' virtual IOReturn getDotPitch(uint32_t *);')
        elif i == 213:
            lines.append(' virtual IOReturn supportedFrameInfo(uint32_t, uint32_t *, char *, uint32_t *) = 0;')
        elif i == 249:
            lines.append(' virtual void getDisplayDimensions(uint32_t *, uint32_t *);')
        elif i == 288:
            lines.append(' virtual bool startHardware();')
        elif i == 290:
            lines.append(' virtual void kickSwapQueue();')
        elif i == 295:
            lines.append(' virtual void completeCurrentSwap();')
        elif i == 296:
            lines.append(' virtual bool validateSwap(const OriginalLegacySwapRequest *);')
        elif i == 301:
            lines.append(' virtual bool getRefreshAttributes(uint64_t *, bool);')
        elif i == 337:
            lines.append(' virtual IOReturn collectCompletionReports(uint32_t, void *, uint32_t *, uint32_t) = 0;')
        elif i == 354:
            lines.append(' virtual int applyStateRequest(OriginalLegacyStateRequest *, bool);')
        elif i in pure:
            lines.append(f' virtual __attribute__((noreturn)) void opaque{i}() = 0;')
        else:
            lines.append(f' virtual __attribute__((deprecated("Unclassified ABI"))) void opaque{i}();')
    lines.append('};')
    lines.append('static_assert(sizeof(IOMobileFramebufferLegacy) == 0x18a0, "Actual original instance extent");')
    header = OUT / 'OriginalLegacyFramebufferABI.h'
    header.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    return header


def build_cxx_cmd(
    record: dict,
    source: Path,
    dest: Path,
    extra_flags: list[str],
    allow_warnings: bool = False,
) -> list[str]:
    args = record['arguments']
    cmd: list[str] = []
    i = 0
    while i < len(args):
        val = args[i]
        if val == record['file']:
            cmd.append(str(source))
        elif val == '-o':
            cmd.extend(['-o', str(dest)])
            i += 1
        elif val == '-Xclang' and i + 1 < len(args) and args[i + 1].startswith('-W'):
            i += 1
        elif not val.startswith('-W'):
            cmd.append(val)
        i += 1
    cmd.extend([
        '-Wall',
        '-Wextra',
        '-Wno-unused-parameter',
        '-Wno-unnecessary-virtual-specifier',
        '-Wno-c99-designator',
        '-Wno-reorder-init-list',
        '-Wno-deprecated-enum-enum-conversion',
        '-I',
        str(ROOT / '.cache/xnu-work/osfmk'),
    ])
    if not allow_warnings:
        cmd.append('-Werror')
    cmd.extend(extra_flags)
    return cmd


def compile_cxx_from_record(
    record: dict,
    source: Path,
    dest: Path,
    extra_flags: list[str],
    allow_warnings: bool = False,
) -> None:
    cmd = build_cxx_cmd(record, source, dest, extra_flags, allow_warnings=allow_warnings)
    run(cmd, cwd=Path(record['directory']))


def find_c_record() -> dict | None:
    base = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
    for rel in (
        'osfmk/DEVELOPMENT/startup.o.json',
        'bsd/DEVELOPMENT/bsd_init.o.json',
    ):
        cand = base / rel
        if cand.is_file():
            return json.loads(cand.read_text(encoding='utf-8').strip().rstrip(','))
    osfmk_dir = base / 'osfmk/DEVELOPMENT'
    if osfmk_dir.is_dir():
        for cand in sorted(osfmk_dir.glob('*.o.json')):
            if not cand.name.endswith('.cpo.json'):
                return json.loads(cand.read_text(encoding='utf-8').strip().rstrip(','))
    return None


def compile_c_from_record(
    c_record: dict | None,
    cxx_record: dict,
    source: Path,
    dest: Path,
    extra_flags: list[str],
    allow_warnings: bool = False,
) -> None:
    record = c_record or cxx_record
    args = record['arguments']
    cmd: list[str] = []
    i = 0
    while i < len(args):
        val = args[i]
        if val == record['file']:
            cmd.append(str(source))
        elif val == '-o':
            cmd.extend(['-o', str(dest)])
            i += 1
        elif val in ('-MF', '-MJ', '-MT', '-MQ'):
            i += 1
        elif val == '-x' and i + 1 < len(args):
            i += 1
        elif val == '-Xclang' and i + 1 < len(args) and args[i + 1].startswith('-W'):
            i += 1
        elif val.startswith('-W'):
            pass
        elif not c_record and (
            val.startswith(('-std=', '-stdlib=', '-fc++'))
            or val in ('-fno-rtti', '-fno-exceptions', '-fcheck-new', '-fapple-kext', '-mkernel')
        ):
            pass
        else:
            cmd.append(val)
        i += 1
    if not c_record:
        cmd.extend(['-std=gnu11', '-mkernel'])
    cmd.extend([
        '-Wall',
        '-Wextra',
        '-Wno-unused-parameter',
        '-I',
        str(ROOT / '.cache/xnu-work/osfmk'),
    ])
    if not allow_warnings:
        cmd.append('-Werror')
    cmd.extend(extra_flags)
    run(cmd, cwd=Path(record['directory']))


def compile_late_from_record(
    record: dict,
    clang: Path,
    source: Path,
    late_obj: Path,
    extra_flags: list[str],
    allow_warnings: bool = False,
) -> None:
    sym = f'__GLOBAL__sub_I_{source.name}'
    raw_s = late_obj.with_name(source.stem + '.s')
    late_s = late_obj.with_name(source.stem + '.late.s')
    cmd_s = build_cxx_cmd(record, source, raw_s, extra_flags, allow_warnings=allow_warnings)
    cmd_s = ['-S' if a == '-c' else a for a in cmd_s]
    run(cmd_s, cwd=Path(record['directory']))

    lines = raw_s.read_text(encoding='utf-8').splitlines()
    out_lines: list[str] = []
    in_mod_init = False
    removed_refs = 0
    globalized = 0
    for line in lines:
        stripped = line.strip()
        if stripped.startswith('.section') and '__mod_init_func' in stripped:
            in_mod_init = True
            continue
        if in_mod_init:
            if stripped.startswith(('.section', '.subsections_via_symbols', '.text', '.data', '.const')):
                in_mod_init = False
            else:
                if sym in stripped:
                    removed_refs += 1
                continue
        if stripped.startswith(f'{sym}:'):
            out_lines.append(f'\t.globl\t{sym}')
            globalized += 1
        out_lines.append(line)

    require(
        removed_refs >= 1 and globalized == 1,
        f'late-init staging failed for {source.name}: removed_refs={removed_refs} globalized={globalized}',
    )
    late_s.write_text('\n'.join(out_lines) + '\n', encoding='utf-8')
    run([clang, '-target', TRIPLE, '-c', late_s, '-o', late_obj])

    # Confirm no __mod_init_func section remains
    info = macho(late_obj.read_bytes())
    secs = {s['name'] for g in info['segments'] for s in g['sections']}
    require('__mod_init_func' not in secs, f'__mod_init_func remained in {late_obj.name}')


def main() -> None:
    require(
        ARCH == 'arm64' and IOS_VERSION.split('.')[0] == '17',
        f'J716F display/keystore builder targets arm64 iOS 17, got {ARCH} {IOS_VERSION}',
    )
    OUT.mkdir(parents=True, exist_ok=True)

    kc_path = find_kernelcache()
    clang = find_tool(['clang'])
    ar = find_tool(['llvm-ar', 'ar'])

    legacy_vptr, legacy_vt_slid, pure, bindings = audit_kernelcache(kc_path)
    header = write_abi_header(pure)
    hid_vptr, hid_vt_slid, hid_bindings = audit_hid_kernelcache(kc_path)
    hid_header = write_hid_abi_header()
    accel_bindings = audit_accelerator_kernelcache(kc_path)

    record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    require(record_path.is_file(), f'missing XNU compile record: {record_path}')
    record = json.loads(record_path.read_text(encoding='utf-8').strip().rstrip(','))
    c_record = find_c_record()

    legacy_src = ROOT / 'drivers/GopFramebuffer/NativeLegacyDisplay.cpp'
    presenter_src = ROOT / 'drivers/GopFramebuffer/NativeGOPPresenter.cpp'
    keystore_src = ROOT / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp'
    board_display_src = ROOT / 'kits/j716f/display/J716fLegacyDisplayProbe.cpp'
    board_keystore_src = ROOT / 'kits/j716f/display/J716fKeyStoreProbe.cpp'
    input_dir = ROOT / 'kits/j716f/input'
    hid_src = input_dir / 'NativeHidInput.cpp'
    board_hid_src = input_dir / 'J716fHidInputProbe.cpp'
    touch_state_src = input_dir / 'J716fTouchState.cpp'
    touch_probe_src = input_dir / 'J716fTouchProbe.cpp'
    buttons_src = input_dir / 'J716fButtons.c'
    rpmh_src = input_dir / 'J716fQcomRpmh.c'
    power_dir = ROOT / 'kits/j716f/power'
    cpufreq_src = power_dir / 'J716fCpuFreq.cpp'
    battery_src = power_dir / 'J716fBattery.cpp'
    gpu_dir = ROOT / 'kits/j716f/gpu'
    gpu_probe_src = gpu_dir / 'J716fGpuProbe.cpp'
    gpu_gmu_src = gpu_dir / 'J716fA650Gmu.c'
    gpu_gmu_ctx_src = gpu_dir / 'J716fA650GmuContext.c'
    gpu_cp_src = gpu_dir / 'J716fA650Cp.c'
    gpu_zap_src = gpu_dir / 'J716fA650Zap.c'
    gpu_irq_src = gpu_dir / 'J716fA650Irq.cpp'
    gpu_ring_src = gpu_dir / 'J716fA650Ring.cpp'
    gpu_ctx_src = gpu_dir / 'J716fA650GpuContext.cpp'
    adreno740_dir = find_upstream_dir('drivers/Adreno740')
    adreno_metal_dir = find_upstream_dir('drivers/AdrenoMetal')
    adreno740_kmd_names = (
        'NativeAdrenoCpu.cpp',
        'NativeAdrenoMemory.cpp',
        'NativeAdrenoPool.cpp',
        'NativeAdrenoSmmuMemory.cpp',
        'NativeAdrenoSmmuContext.cpp',
        'NativeAdrenoVm.cpp',
        'NativeAdrenoClient.cpp',
        'NativeAdrenoUserClient.cpp',
        'NativeAdrenoSubmit.cpp',
        'NativeAdrenoPreempt.cpp',
        'NativeAdrenoTimer.cpp',
        'NativeAdrenoSha256.cpp',
        'NativeAdrenoPublish.cpp',
    )
    adreno740_kmd_srcs = [adreno740_dir / n for n in adreno740_kmd_names]
    adreno_accel_src = adreno740_dir / 'NativeAdrenoAccelerator.cpp'
    adreno_fw_dir = ROOT / 'kits/AdrenoFirmware'
    adreno_fw_srcs = [
        adreno_fw_dir / 'AdrenoFirmware.c',
        adreno_fw_dir / 'AdrenoHfi.c',
        adreno_fw_dir / 'AdrenoHfiStartup.c',
        adreno_fw_dir / 'AdrenoHfiVotes.c',
    ]
    adreno_pm4_dir = ROOT / 'kits/AdrenoPM4'
    adreno_pm4_src = adreno_pm4_dir / 'AdrenoPM4.c'
    smmu_dir = ROOT / 'kits/ArmSmmu4K'
    smmu_srcs = sorted(smmu_dir.glob('ArmSmmu4K*.c'))
    usb_dir = ROOT / 'kits/j716f/usb'
    usb_cdc_src = usb_dir / 'J716fUsbDebugCdc.cpp'

    for src in (
        legacy_src,
        presenter_src,
        keystore_src,
        board_display_src,
        board_keystore_src,
        hid_src,
        board_hid_src,
        touch_state_src,
        touch_probe_src,
        buttons_src,
        rpmh_src,
        cpufreq_src,
        battery_src,
        gpu_probe_src,
        gpu_gmu_src,
        gpu_gmu_ctx_src,
        gpu_cp_src,
        gpu_zap_src,
        gpu_irq_src,
        gpu_ring_src,
        gpu_ctx_src,
        *adreno740_kmd_srcs,
        adreno_accel_src,
        adreno_pm4_src,
        *adreno_fw_srcs,
        *smmu_srcs,
        usb_cdc_src,
    ):
        require(src.is_file(), f'missing source: {src}')

    # 1. Compile unmodified upstream NativeLegacyDisplay.cpp and verify exact 358-slot vtable
    legacy_obj = OUT / 'NativeLegacyDisplay.o'
    compile_cxx_from_record(
        record,
        legacy_src,
        legacy_obj,
        ['-I', str(OUT), '-I', str(legacy_src.parent)],
        allow_warnings=True,
    )
    raw = legacy_obj.read_bytes()
    image = macho(raw)
    st = next(p for c, p, _ in image['commands'] if c == 2)
    sy, count, strings, _ = struct.unpack_from('<4I', raw, st + 8)
    symbols: list[tuple[str, int, int]] = []
    for i in range(count):
        ix, kind, _section, _, va = struct.unpack_from('<IBBHQ', raw, sy + i * 16)
        symbols.append((raw[strings + ix : raw.index(b'\0', strings + ix)].decode(), va, kind))

    vtable = next(va for name, va, _ in symbols if name == '__ZTV19NativeLegacyDisplay')
    relocations: dict[int, str] = {}
    for segment in image['segments']:
        for sec in segment['sections']:
            if sec['va'] <= vtable < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', raw, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', raw, ro + i * 8)
                    if (bits >> 27) & 1 and (bits >> 28) == 0 and ((bits >> 25) & 3) == 3:
                        relocations[sec['va'] + offset] = symbols[bits & 0xFFFFFF][0]

    compiled = [relocations.get(vtable + 16 + i * 8) for i in range(LEGACY_VTABLE_LEN)]
    require(
        all(compiled) and (vtable + 16 + LEGACY_VTABLE_LEN * 8) not in relocations,
        'NativeLegacyDisplay vtable does not have exact 358 entries',
    )

    owned: list[int] = []
    inherited_native: list[int] = []
    for i, name in enumerate(compiled):
        assert name is not None
        if name.startswith(('__ZN19NativeLegacyDisplay', '__ZNK19NativeLegacyDisplay')):
            owned.append(i)
        elif '25IOMobileFramebufferLegacy' in name:
            require(i not in pure, f'unresolved abstract slot {i}: {name}')
            bindings[name] = legacy_vt_slid[i]
        else:
            require(i < 168, f'unexpected non-Legacy symbol at slot {i}: {name}')
            inherited_native.append(i)

    require(owned == EXPECTED_OWNED_SLOTS, f'owned vtable slots mismatch: {owned}')
    undefined_legacy = {n for n, _, k in symbols if k == 1 and 'IOMobileFramebufferLegacy' in n}
    bound_legacy = {n for n in bindings if 'IOMobileFramebufferLegacy' in n}
    require(
        undefined_legacy == bound_legacy,
        f'Legacy binding mismatch: missing={undefined_legacy - bound_legacy} extra={bound_legacy - undefined_legacy}',
    )

    # 1b. Compile unmodified upstream NativeHidInput.cpp and verify exact 207-slot vtable
    hid_obj = OUT / 'NativeHidInput.o'
    compile_cxx_from_record(
        record,
        hid_src,
        hid_obj,
        ['-I', str(OUT), '-I', str(input_dir)],
        allow_warnings=True,
    )
    hid_raw = hid_obj.read_bytes()
    hid_img = macho(hid_raw)
    hst = next(p for c, p, _ in hid_img['commands'] if c == 2)
    hsy, hcount, hstrings, _ = struct.unpack_from('<4I', hid_raw, hst + 8)
    hsymbols: list[tuple[str, int, int]] = []
    for i in range(hcount):
        ix, kind, _section, _, va = struct.unpack_from('<IBBHQ', hid_raw, hsy + i * 16)
        hsymbols.append((hid_raw[hstrings + ix : hid_raw.index(b'\0', hstrings + ix)].decode(), va, kind))

    hvtable = next(va for name, va, _ in hsymbols if name == '__ZTV14NativeHidInput')
    hrelocs: dict[int, str] = {}
    for segment in hid_img['segments']:
        for sec in segment['sections']:
            if sec['va'] <= hvtable < sec['va'] + sec['size']:
                ro, rc = struct.unpack_from('<2I', hid_raw, sec['command_offset'] + 56)
                for i in range(rc):
                    offset, bits = struct.unpack_from('<2I', hid_raw, ro + i * 8)
                    if (bits >> 27) & 1 and (bits >> 28) == 0 and ((bits >> 25) & 3) == 3:
                        hrelocs[sec['va'] + offset] = hsymbols[bits & 0xFFFFFF][0]

    hcompiled = [hrelocs.get(hvtable + 16 + i * 8) for i in range(HID_VTABLE_LEN)]
    require(
        all(hcompiled) and (hvtable + 16 + HID_VTABLE_LEN * 8) not in hrelocs,
        'NativeHidInput vtable does not have exact 207 entries',
    )
    hid_owned: list[int] = []
    for i, name in enumerate(hcompiled):
        assert name is not None
        if name.startswith(('__ZN14NativeHidInput', '__ZNK14NativeHidInput')):
            hid_owned.append(i)
        elif '11IOHIDDevice' in name:
            require(i not in EXPECTED_HID_PURE_SLOTS, f'unresolved HID abstract slot {i}: {name}')
            hid_bindings[name] = hid_vt_slid[i]
        else:
            require(i < 168, f'unexpected non-HID symbol at slot {i}: {name}')

    require(hid_owned == EXPECTED_HID_OWNED_SLOTS, f'HID owned vtable slots mismatch: {hid_owned}')
    undefined_hid = {n for n, _, k in hsymbols if k == 1 and '11IOHIDDevice' in n}
    require(
        undefined_hid == set(hid_bindings),
        f'HID binding mismatch: missing={undefined_hid - set(hid_bindings)} extra={set(hid_bindings) - undefined_hid}',
    )
    bindings.update(hid_bindings)
    bindings.update(accel_bindings)

    # 2. Assemble original-bindings.S -> original-bindings.o
    asm_path = OUT / 'original-bindings.S'
    asm_path.write_text(
        ''.join(f'.globl {name}\n.set {name}, {va:#x}\n' for name, va in sorted(bindings.items())),
        encoding='utf-8',
    )
    bindings_obj = OUT / 'original-bindings.o'
    run([clang, '-target', TRIPLE, '-c', asm_path, '-o', bindings_obj])

    # 3. Late-init stage NativeLegacyDisplay.cpp (and confirm identical __text)
    legacy_late = OUT / 'NativeLegacyDisplay.late.o'
    compile_late_from_record(
        record,
        clang,
        legacy_src,
        legacy_late,
        ['-I', str(OUT), '-I', str(legacy_src.parent)],
        allow_warnings=True,
    )
    require(
        code_bytes(raw) == code_bytes(legacy_late.read_bytes()),
        'NativeLegacyDisplay __text changed during late-init staging',
    )

    # 3b. Late-init stage NativeHidInput.cpp (and confirm identical __text)
    hid_late = OUT / 'NativeHidInput.late.o'
    compile_late_from_record(
        record,
        clang,
        hid_src,
        hid_late,
        ['-I', str(OUT), '-I', str(input_dir)],
        allow_warnings=True,
    )
    require(
        code_bytes(hid_raw) == code_bytes(hid_late.read_bytes()),
        'NativeHidInput __text changed during late-init staging',
    )

    # 4. Compile & late-init stage NativeGOPPresenter.cpp (with handoffAbandon export so
    # AGXDeviceUserClient::clientClose releases any in-flight GPU swap + kept IOSurfaces if backboardd exits).
    # Keep exact filename NativeGOPPresenter.cpp so clang emits __GLOBAL__sub_I_NativeGOPPresenter.cpp.
    staged_dir = OUT / 'staged'
    staged_dir.mkdir(parents=True, exist_ok=True)
    presenter_patched = staged_dir / 'NativeGOPPresenter.cpp'
    presenter_text = presenter_src.read_text(encoding='utf-8')
    p_anchor_decl = '    static IOMemoryDescriptor *handoffMemory();'
    p_anchor_def = 'extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void) {return NativeGOPPresenter::handoffMemory();}'
    require(p_anchor_decl in presenter_text and p_anchor_def in presenter_text,
            'NativeGOPPresenter handoff anchors missing')
    presenter_text = presenter_text.replace(
        p_anchor_decl,
        p_anchor_decl + '\n    static void handoffAbandon() {if(NativeGOPPresenter *p=gHandoff)p->abandonSwap();}',
        1,
    ).replace(
        p_anchor_def,
        p_anchor_def + '\nextern "C" void native_gop_handoff_abandon(void) {NativeGOPPresenter::handoffAbandon();}',
        1,
    )
    presenter_patched.write_text(presenter_text, encoding='utf-8')
    presenter_late = OUT / 'NativeGOPPresenter.late.o'
    compile_late_from_record(
        record,
        clang,
        presenter_patched,
        presenter_late,
        ['-I', str(presenter_src.parent)],
        allow_warnings=True,
    )

    # 5. Compile & late-init stage NativeKeyStoreEvents.cpp
    keystore_late = OUT / 'NativeKeyStoreEvents.late.o'
    compile_late_from_record(
        record,
        clang,
        keystore_src,
        keystore_late,
        ['-I', str(keystore_src.parent)],
        allow_warnings=True,
    )

    # 6. Compile J716F display, keystore, HID, touch, and button drivers
    board_display_obj = OUT / 'J716fLegacyDisplayProbe.o'
    board_keystore_obj = OUT / 'J716fKeyStoreProbe.o'
    board_hid_obj = OUT / 'J716fHidInputProbe.o'
    touch_state_obj = OUT / 'J716fTouchState.o'
    touch_probe_obj = OUT / 'J716fTouchProbe.o'
    buttons_obj = OUT / 'J716fButtons.o'
    rpmh_obj = OUT / 'J716fQcomRpmh.o'
    cpufreq_obj = OUT / 'J716fCpuFreq.o'
    battery_late = OUT / 'J716fBattery.late.o'
    gpu_probe_obj = OUT / 'J716fGpuProbe.o'
    gpu_gmu_obj = OUT / 'J716fA650Gmu.o'
    gpu_gmu_ctx_obj = OUT / 'J716fA650GmuContext.o'
    gpu_cp_obj = OUT / 'J716fA650Cp.o'
    gpu_zap_obj = OUT / 'J716fA650Zap.o'
    gpu_irq_obj = OUT / 'J716fA650Irq.o'
    gpu_ring_obj = OUT / 'J716fA650Ring.o'
    gpu_ctx_obj = OUT / 'J716fA650GpuContext.o'
    adreno740_kmd_objs = [OUT / f'{s.stem}.o' for s in adreno740_kmd_srcs]
    adreno_accel_late = OUT / 'NativeAdrenoAccelerator.late.o'
    adreno_pm4_obj = OUT / 'AdrenoPM4.o'
    adreno_fw_objs = [OUT / f'{s.stem}.o' for s in adreno_fw_srcs]
    smmu_objs = [OUT / f'{s.stem}.o' for s in smmu_srcs]
    usb_cdc_obj = OUT / 'J716fUsbDebugCdc.o'

    # Optional local-only GMU & SQE firmware embedding (never required for public/CI builds).
    gmu_fw_path: Path | None = None
    if os.environ.get('J716F_GMU_FIRMWARE'):
        gmu_fw_path = Path(os.environ['J716F_GMU_FIRMWARE'])
        require(gmu_fw_path.is_file(), f'J716F_GMU_FIRMWARE does not exist: {gmu_fw_path}')
    elif os.environ.get('J716F_GPU_FIRMWARE_DIR'):
        cand = Path(os.environ['J716F_GPU_FIRMWARE_DIR']) / 'a650_gmu.bin'
        if cand.is_file():
            gmu_fw_path = cand
    gmu_fw_src = OUT / 'J716fGmuFirmware.c'
    gmu_fw_obj = OUT / 'J716fGmuFirmware.o'
    gmu_fw_bytes = 0
    gmu_fw_sha256: str | None = None
    if gmu_fw_path is not None:
        blob = gmu_fw_path.read_bytes()
        require(
            16 <= len(blob) <= 0x100000 and len(blob) % 4 == 0,
            f'invalid GMU firmware image {gmu_fw_path} ({len(blob)} bytes)',
        )
        gmu_fw_bytes = len(blob)
        gmu_fw_sha256 = hashlib.sha256(blob).hexdigest()
        gmu_fw_src.write_text(
            f'/* Local optional GMU firmware ({gmu_fw_path.name}, sha256 {gmu_fw_sha256}) */\n'
            '#include <stdint.h>\n'
            'const uint8_t j716f_gpu_gmu_firmware[] __attribute__((aligned(16))) = {'
            + ','.join(str(b) for b in blob)
            + '};\n'
            f'const uint32_t j716f_gpu_gmu_firmware_size = {gmu_fw_bytes}u;\n',
            encoding='utf-8',
        )
    else:
        gmu_fw_src.write_text(
            '/* Public/CI build stub: no proprietary a650_gmu.bin linked. */\n'
            '#include <stdint.h>\n'
            'const uint8_t j716f_gpu_gmu_firmware[16] __attribute__((aligned(16))) = {0};\n'
            'const uint32_t j716f_gpu_gmu_firmware_size = 0u;\n',
            encoding='utf-8',
        )

    sqe_fw_path: Path | None = None
    if os.environ.get('J716F_SQE_FIRMWARE'):
        sqe_fw_path = Path(os.environ['J716F_SQE_FIRMWARE'])
        require(sqe_fw_path.is_file(), f'J716F_SQE_FIRMWARE does not exist: {sqe_fw_path}')
    elif os.environ.get('J716F_GPU_FIRMWARE_DIR'):
        cand = Path(os.environ['J716F_GPU_FIRMWARE_DIR']) / 'a650_sqe.fw'
        if cand.is_file():
            sqe_fw_path = cand
    sqe_fw_src = OUT / 'J716fSqeFirmware.c'
    sqe_fw_obj = OUT / 'J716fSqeFirmware.o'
    sqe_fw_bytes = 0
    sqe_fw_sha256: str | None = None
    if sqe_fw_path is not None:
        blob = sqe_fw_path.read_bytes()
        require(
            8 <= len(blob) <= 0x20004 and len(blob) % 4 == 0,
            f'invalid SQE firmware image {sqe_fw_path} ({len(blob)} bytes)',
        )
        sqe_fw_bytes = len(blob)
        sqe_fw_sha256 = hashlib.sha256(blob).hexdigest()
        sqe_fw_src.write_text(
            f'/* Local optional SQE firmware ({sqe_fw_path.name}, sha256 {sqe_fw_sha256}) */\n'
            '#include <stdint.h>\n'
            'const uint8_t j716f_gpu_sqe_firmware[] __attribute__((aligned(16))) = {'
            + ','.join(str(b) for b in blob)
            + '};\n'
            f'const uint32_t j716f_gpu_sqe_firmware_size = {sqe_fw_bytes}u;\n',
            encoding='utf-8',
        )
    else:
        sqe_fw_src.write_text(
            '/* Public/CI build stub: no proprietary a650_sqe.fw linked. */\n'
            '#include <stdint.h>\n'
            'const uint8_t j716f_gpu_sqe_firmware[16] __attribute__((aligned(16))) = {0};\n'
            'const uint32_t j716f_gpu_sqe_firmware_size = 0u;\n',
            encoding='utf-8',
        )

    zap_fw_path: Path | None = None
    if os.environ.get('J716F_ZAP_FIRMWARE'):
        zap_fw_path = Path(os.environ['J716F_ZAP_FIRMWARE'])
        require(zap_fw_path.is_file(), f'J716F_ZAP_FIRMWARE does not exist: {zap_fw_path}')
    elif os.environ.get('J716F_GPU_FIRMWARE_DIR'):
        for name in ('a650_zap.mbn', 'a650_zap.elf'):
            cand = Path(os.environ['J716F_GPU_FIRMWARE_DIR']) / name
            if cand.is_file():
                zap_fw_path = cand
                break
    zap_fw_src = OUT / 'J716fZapFirmware.c'
    zap_fw_obj = OUT / 'J716fZapFirmware.o'
    zap_fw_bytes = 0
    zap_fw_sha256: str | None = None
    if zap_fw_path is not None:
        blob = zap_fw_path.read_bytes()
        require(
            52 <= len(blob) <= 0x20000 and blob[:4] == b'\x7fELF',
            f'invalid ZAP firmware image {zap_fw_path} ({len(blob)} bytes)',
        )
        zap_fw_bytes = len(blob)
        zap_fw_sha256 = hashlib.sha256(blob).hexdigest()
        zap_fw_src.write_text(
            f'/* Local optional ZAP firmware ({zap_fw_path.name}, sha256 {zap_fw_sha256}) */\n'
            '#include <stdint.h>\n'
            'const uint8_t j716f_gpu_zap_firmware[] __attribute__((aligned(16))) = {'
            + ','.join(str(b) for b in blob)
            + '};\n'
            f'const uint32_t j716f_gpu_zap_firmware_size = {zap_fw_bytes}u;\n',
            encoding='utf-8',
        )
    else:
        zap_fw_src.write_text(
            '/* Public/CI build stub: no proprietary a650_zap.mbn linked. */\n'
            '#include <stdint.h>\n'
            'const uint8_t j716f_gpu_zap_firmware[16] __attribute__((aligned(16))) = {0};\n'
            'const uint32_t j716f_gpu_zap_firmware_size = 0u;\n',
            encoding='utf-8',
        )

    gpu_kmd_inc = [
        '-I', str(gpu_dir),
        '-I', str(adreno740_dir),
        '-I', str(adreno_metal_dir),
        '-I', str(adreno_fw_dir),
        '-I', str(adreno_pm4_dir),
        '-I', str(smmu_dir),
    ]

    compile_cxx_from_record(record, board_display_src, board_display_obj, [], allow_warnings=False)
    compile_cxx_from_record(record, board_keystore_src, board_keystore_obj, [], allow_warnings=False)
    compile_cxx_from_record(record, board_hid_src, board_hid_obj, ['-I', str(input_dir)], allow_warnings=False)
    compile_cxx_from_record(record, touch_state_src, touch_state_obj, ['-I', str(input_dir)], allow_warnings=False)
    compile_cxx_from_record(record, touch_probe_src, touch_probe_obj, ['-I', str(input_dir)], allow_warnings=True)
    compile_c_from_record(c_record, record, buttons_src, buttons_obj, ['-I', str(input_dir)], allow_warnings=False)
    compile_c_from_record(c_record, record, rpmh_src, rpmh_obj, ['-I', str(input_dir)], allow_warnings=False)
    compile_cxx_from_record(record, cpufreq_src, cpufreq_obj, [], allow_warnings=False)
    compile_late_from_record(
        record,
        clang,
        battery_src,
        battery_late,
        ['-I', str(power_dir)],
        allow_warnings=False,
    )
    compile_cxx_from_record(
        record,
        gpu_probe_src,
        gpu_probe_obj,
        gpu_kmd_inc,
        allow_warnings=False,
    )
    compile_cxx_from_record(record, gpu_irq_src, gpu_irq_obj, gpu_kmd_inc, allow_warnings=False)
    compile_cxx_from_record(record, gpu_ring_src, gpu_ring_obj, gpu_kmd_inc, allow_warnings=False)
    compile_cxx_from_record(record, gpu_ctx_src, gpu_ctx_obj, gpu_kmd_inc, allow_warnings=False)
    xnu17_gpu_compat = OUT / 'j716f_xnu17_gpu_compat.h'
    xnu17_gpu_compat.write_text(
        '#pragma once\n'
        '#include <stdint.h>\n'
        '#include <kern/task.h>\n'
        '#ifndef PRIO_DARWIN_GPU_ALLOW\n'
        '#define PRIO_DARWIN_GPU_ALLOW 0x1\n'
        '#endif\n'
        '#ifndef PRIO_DARWIN_GPU_DENY\n'
        '#define PRIO_DARWIN_GPU_DENY 0x2\n'
        '#endif\n'
        '#ifndef PRIO_DARWIN_GPU_BACKGROUND\n'
        '#define PRIO_DARWIN_GPU_BACKGROUND 0x4\n'
        '#endif\n'
        '#ifndef PRIO_DARWIN_GPU_UTILITY\n'
        '#define PRIO_DARWIN_GPU_UTILITY 0x8\n'
        '#endif\n'
        'static inline int32_t task_get_gpu_role(task_t task) {\n'
        '    (void)task;\n'
        '    return PRIO_DARWIN_GPU_ALLOW;\n'
        '}\n',
        encoding='utf-8',
    )

    for kmd_src, kmd_obj in zip(adreno740_kmd_srcs, adreno740_kmd_objs):
        extra = gpu_kmd_inc + ['-Wno-overloaded-virtual']
        compile_src = kmd_src
        if kmd_src.name == 'NativeAdrenoUserClient.cpp':
            extra = extra + ['-include', str(xnu17_gpu_compat)]
            uc_text = kmd_src.read_text(encoding='utf-8')
            uc_anchor_decl = 'extern "C" {volatile uint32_t native_adreno_wc_enabled=1,native_adreno_owned_coherent=0;extern volatile uint32_t native_adreno_io_coherent;}'
            uc_anchor_coh = 'const bool coherent=(entry->memory.imported() && native_adreno_io_coherent && !(permissions&2u)) || (!entry->memory.imported() && ownedCoherent());'
            require(uc_anchor_decl in uc_text, 'NativeAdrenoUserClient decl anchor missing')
            require(uc_anchor_coh in uc_text, 'NativeAdrenoUserClient coherent anchor missing')
            uc_text = uc_text.replace(
                uc_anchor_decl,
                uc_anchor_decl + '\nextern "C" {volatile uint32_t native_adreno_import_rw_coherent=0;}',
                1,
            ).replace(
                uc_anchor_coh,
                'const bool coherent=(entry->memory.imported() && native_adreno_io_coherent && (!(permissions&2u) || native_adreno_import_rw_coherent)) || (!entry->memory.imported() && ownedCoherent());',
                1,
            )
            compile_src = staged_dir / 'NativeAdrenoUserClient.cpp'
            compile_src.write_text(uc_text, encoding='utf-8')
        elif kmd_src.name == 'NativeAdrenoPublish.cpp':
            pub_text = kmd_src.read_text(encoding='utf-8')
            pub_anchor_decl = 'extern "C" IOMemoryDescriptor *native_gop_handoff_memory(void);'
            pub_anchor_close = 'IOReturn AGXDeviceUserClient::clientClose()\n{\n    dropViews(0);'
            pub_anchor_close_crlf = 'IOReturn AGXDeviceUserClient::clientClose()\r\n{\r\n    dropViews(0);'
            require(pub_anchor_decl in pub_text, 'NativeAdrenoPublish handoff decl anchor missing')
            close_anchor = pub_anchor_close if pub_anchor_close in pub_text else pub_anchor_close_crlf
            require(close_anchor in pub_text, 'NativeAdrenoPublish clientClose anchor missing')
            pub_text = pub_text.replace(
                pub_anchor_decl,
                pub_anchor_decl + '\nextern "C" void native_gop_handoff_abandon(void) __attribute__((weak_import));',
                1,
            ).replace(
                close_anchor,
                close_anchor + '\n    if(handoffOnly_) {handoffOnly_=false; if(&native_gop_handoff_abandon) native_gop_handoff_abandon();}',
                1,
            )
            compile_src = staged_dir / 'NativeAdrenoPublish.cpp'
            compile_src.write_text(pub_text, encoding='utf-8')
        compile_cxx_from_record(
            record,
            compile_src,
            kmd_obj,
            extra,
            allow_warnings=True,
        )
    compile_late_from_record(
        record,
        clang,
        adreno_accel_src,
        adreno_accel_late,
        ['-I', str(adreno740_dir), '-I', str(adreno_metal_dir), '-Wno-overloaded-virtual'],
        allow_warnings=True,
    )
    for fw_src, fw_obj in zip(adreno_fw_srcs, adreno_fw_objs):
        compile_c_from_record(c_record, record, fw_src, fw_obj, ['-I', str(adreno_fw_dir)], allow_warnings=False)
    compile_c_from_record(c_record, record, adreno_pm4_src, adreno_pm4_obj, ['-I', str(adreno_pm4_dir)], allow_warnings=False)
    for smmu_src, smmu_obj in zip(smmu_srcs, smmu_objs):
        compile_c_from_record(c_record, record, smmu_src, smmu_obj, ['-I', str(smmu_dir)], allow_warnings=False)
    compile_c_from_record(
        c_record,
        record,
        gpu_gmu_src,
        gpu_gmu_obj,
        ['-I', str(gpu_dir), '-I', str(adreno_fw_dir)],
        allow_warnings=False,
    )
    compile_c_from_record(
        c_record,
        record,
        gpu_gmu_ctx_src,
        gpu_gmu_ctx_obj,
        ['-I', str(gpu_dir), '-I', str(smmu_dir)],
        allow_warnings=False,
    )
    compile_c_from_record(
        c_record,
        record,
        gpu_cp_src,
        gpu_cp_obj,
        ['-I', str(gpu_dir), '-I', str(adreno_pm4_dir), '-I', str(smmu_dir)],
        allow_warnings=False,
    )
    compile_c_from_record(
        c_record,
        record,
        gpu_zap_src,
        gpu_zap_obj,
        ['-I', str(gpu_dir)],
        allow_warnings=False,
    )
    compile_c_from_record(c_record, record, gmu_fw_src, gmu_fw_obj, [], allow_warnings=False)
    compile_c_from_record(c_record, record, sqe_fw_src, sqe_fw_obj, [], allow_warnings=False)
    compile_c_from_record(c_record, record, zap_fw_src, zap_fw_obj, [], allow_warnings=False)
    compile_cxx_from_record(
        record,
        usb_cdc_src,
        usb_cdc_obj,
        ['-I', str(usb_dir), '-I', str(input_dir)],
        allow_warnings=False,
    )

    # 7. Archive libNativeLegacyDisplay.late.a and write build-report.json
    archive = OUT / 'libNativeLegacyDisplay.late.a'
    archive.unlink(missing_ok=True)
    members = [
        legacy_late,
        presenter_late,
        keystore_late,
        hid_late,
        battery_late,
        adreno_accel_late,
        bindings_obj,
        board_display_obj,
        board_keystore_obj,
        board_hid_obj,
        touch_state_obj,
        touch_probe_obj,
        buttons_obj,
        rpmh_obj,
        cpufreq_obj,
        gpu_probe_obj,
        gpu_gmu_obj,
        gpu_gmu_ctx_obj,
        gpu_cp_obj,
        gpu_zap_obj,
        gpu_irq_obj,
        gpu_ring_obj,
        gpu_ctx_obj,
        *adreno740_kmd_objs,
        gmu_fw_obj,
        sqe_fw_obj,
        zap_fw_obj,
        adreno_pm4_obj,
        *adreno_fw_objs,
        *smmu_objs,
        usb_cdc_obj,
    ]
    run([ar, 'rcs', archive, *members])

    tracked_sources = [
        legacy_src,
        ROOT / 'drivers/GopFramebuffer/NativeLegacyRequest.h',
        presenter_src,
        ROOT / 'drivers/GopFramebuffer/NativeGOPWire.h',
        keystore_src,
        board_display_src,
        board_keystore_src,
        hid_src,
        board_hid_src,
        touch_state_src,
        touch_probe_src,
        buttons_src,
        rpmh_src,
        cpufreq_src,
        battery_src,
        gpu_probe_src,
        gpu_gmu_src,
        gpu_gmu_ctx_src,
        gpu_cp_src,
        gpu_zap_src,
        gpu_irq_src,
        gpu_ring_src,
        gpu_ctx_src,
        *adreno740_kmd_srcs,
        adreno_accel_src,
        adreno_pm4_src,
        *adreno_fw_srcs,
        *smmu_srcs,
        usb_cdc_src,
    ]
    def rel_key(p: Path) -> str:
        return os.path.relpath(p, ROOT)

    report = {
        'source_sha256': {rel_key(p): sha256_file(p) for p in tracked_sources},
        'generated_abi_sha256': sha256_file(header),
        'generated_hid_abi_sha256': sha256_file(hid_header),
        'kernelcache_sha256': sha256_file(kc_path),
        'compiled_object_sha256': sha256_file(legacy_obj),
        'late_object_sha256': sha256_file(legacy_late),
        'hid_late_object_sha256': sha256_file(hid_late),
        'accel_late_object_sha256': sha256_file(adreno_accel_late),
        'archive_sha256': sha256_file(archive),
        'legacy_vptr': hex(legacy_vptr),
        'hid_vptr': hex(hid_vptr),
        'virtual_entries': LEGACY_VTABLE_LEN,
        'hid_virtual_entries': HID_VTABLE_LEN,
        'owned_entries': owned,
        'hid_owned_entries': hid_owned,
        'inherited_native_entries': inherited_native,
        'bindings_count': len(bindings),
        'gmu_firmware_bytes': gmu_fw_bytes,
        'gmu_firmware_sha256': gmu_fw_sha256,
        'sqe_firmware_bytes': sqe_fw_bytes,
        'sqe_firmware_sha256': sqe_fw_sha256,
        'zap_firmware_bytes': zap_fw_bytes,
        'zap_firmware_sha256': zap_fw_sha256,
        'hardware_tested': False,
    }
    (OUT / 'build-report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(
        f'j716f-display-keystore: PASS ({len(members)} objects, {len(bindings)} verified 21H461 bindings, '
        f'archive={archive.stat().st_size} B)'
    )


if __name__ == '__main__':
    main()
