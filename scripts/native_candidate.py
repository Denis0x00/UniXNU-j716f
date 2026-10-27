#!/usr/bin/env python3
"""Pure host-side native candidate checks. Never access or modify a device."""
import hashlib
import hmac
import importlib.util
import json
from pathlib import Path
import struct
import tomllib

HERE = Path(__file__).resolve().parent
KERNEL = 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf'
BCM_ROUTE = bytes.fromhex('af0038d5ef1d4092000088d200bc70d30000019100080f8b011e8052010000b9df3f03d5')
REQUIRED = {'_armcpu_registered', '_armcpu_timer_interrupts', '_armdt_platform_configured',
            '_rtclock_intr', '_j716f_armcpu_trace', '_j716f_armcpu_timer_checkpoint',
            '_source_corecrypto14_bootstrap'}
CRYPTO_BOOTSTRAP = '_source_corecrypto14_bootstrap'


def sha(data):
    return hashlib.sha256(data).hexdigest()


def load_module(name):
    spec = importlib.util.spec_from_file_location(name, HERE/(name+'.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class MachO:
    """Bounded parser for ordinary arm64 MH_EXECUTE files, including definitions."""
    def __init__(self, data):
        self.data, self.segments, self.sections, self.symbols = data, [], [], {}
        self.local_symbols = {}
        if len(data) < 32 or struct.unpack_from('<4I', data) != (0xfeedfacf, 0x100000c, 0, 2):
            raise ValueError('Expected ordinary ARM64 executable, not frozen/opaque bytes')
        count, size = struct.unpack_from('<II', data, 16)
        end, pos, symtabs = 32+size, 32, []
        if end > len(data) or count > size//8:
            raise ValueError('Invalid Mach-O load-command bounds')
        for _ in range(count):
            if pos+8 > end:
                raise ValueError('Truncated load command')
            cmd, length = struct.unpack_from('<II', data, pos)
            if length < 8 or length % 8 or pos+length > end:
                raise ValueError('Invalid load command length')
            if cmd == 0x19:
                if length < 72:
                    raise ValueError('Short segment')
                name, va, vmsize, off, filesz, maxprot, prot, nsects, _flags = struct.unpack_from('<16s4Q4I', data, pos+8)
                if filesz > vmsize or off+filesz > len(data) or 72+80*nsects != length:
                    raise ValueError('Invalid segment/section bounds')
                self.segments.append((name.rstrip(b'\0'), va, vmsize, off, filesz, prot))
                for index in range(nsects):
                    n, s, a, z, f = struct.unpack_from('<16s16sQQI', data, pos+72+80*index)
                    if not va <= a <= a+z <= va+vmsize:
                        raise ValueError('Section outside segment')
                    self.sections.append((n.rstrip(b'\0'), s.rstrip(b'\0'), a, z, f))
            elif cmd == 2:
                if length != 24:
                    raise ValueError('Unexpected symtab command')
                symtabs.append(struct.unpack_from('<4I', data, pos+8))
            pos += length
        if pos != end or len(symtabs) != 1:
            raise ValueError('Load commands/symbol table not exact')
        symoff, count, stroff, strsize = symtabs[0]
        if symoff+16*count > len(data) or stroff+strsize > len(data):
            raise ValueError('Symbol table outside image')
        for index in range(count):
            string, kind, section, _desc, value = struct.unpack_from('<IBBHQ', data, symoff+16*index)
            if string >= strsize:
                raise ValueError('Symbol string outside table')
            stop = data.find(b'\0', stroff+string, stroff+strsize)
            if stop < 0:
                raise ValueError('Unterminated symbol')
            # Local N_SECT names can legitimately repeat across translation units.
            # Providers must be real external definitions, not a same-named local,
            # STAB debug entry, undefined import or absolute constant.
            if not kind & 0xe0 and kind & 0xe == 0xe and section and value:
                if section > len(self.sections):
                    raise ValueError('Invalid defined-symbol section')
                name = data[stroff+string:stop].decode('utf-8')
                if not kind & 1:
                    self.local_symbols.setdefault(name, []).append(value)
                    continue
                if name in self.symbols and self.symbols[name] != value:
                    raise ValueError('Conflicting defined symbol')
                self.symbols[name] = value

    def offset(self, address, size):
        found = [off+address-va for _, va, _, off, filesz, _ in self.segments
                 if va <= address and address+size <= va+filesz]
        if len(found) != 1:
            raise ValueError('Expected unique file-backed address')
        return found[0]

    def integrity(self):
        text = [s for s in self.sections if s[0] == b'__text' and s[1] in (b'__TEXT', b'__TEXT_EXEC')]
        fields = [s for s in self.sections if s[0] == b'__fips_hmacs']
        bases = [s[1] for s in self.segments if s[0] == b'__TEXT']
        if len(text) != 1 or len(fields) != 1 or fields[0][3] != 32 or len(bases) != 1:
            raise ValueError('Unexpected source POST section geometry')
        code, field = text[0], fields[0]
        pos = self.offset(code[2], code[3])
        at = self.offset(field[2], 32)
        if pos != code[4] or at != field[4] or code[2]-bases[0] != pos:
            raise ValueError('Source POST header-relative contract mismatch')
        if pos < at+32 and at < pos+code[3]:
            raise ValueError('POST field overlaps code')
        computed = hmac.digest(b'\0', self.data[pos:pos+code[3]], 'sha256')
        if self.data[at:at+32] != computed:
            raise ValueError('Source crypto POST HMAC mismatch; never bypass POST')
        return {'matches': True, 'code_bytes': code[3], 'code_sha256': sha(self.data[pos:pos+code[3]])}


def check_kernel(data):
    image = MachO(data)
    missing = REQUIRED-set(image.symbols)
    # Upstream intentionally makes this startup routine static. Accept one real
    # local definition only for it; native platform providers still require N_EXT.
    if len(image.local_symbols.get(CRYPTO_BOOTSTRAP, [])) == 1:
        missing.discard(CRYPTO_BOOTSTRAP)
    if missing:
        raise ValueError('Missing native provider definitions: '+', '.join(sorted(missing)))
    if BCM_ROUTE in data:
        raise ValueError('Legacy BCM timer route present')
    at = image.offset(image.symbols['_j716f_armcpu_trace'], 1536)
    trace = data[at:at+1536]
    if image.symbols['_j716f_armcpu_trace'] % 8 or struct.unpack_from('<3Q', trace) != (0x315152494636314a, 1, 192) or any(trace[24:]):
        raise ValueError('Observer initial ABI mismatch')
    return {'kernel_sha256': sha(data), 'defined_platform_symbols': sorted(REQUIRED-{CRYPTO_BOOTSTRAP}),
            'source_crypto_bootstrap_defined': True,
            'source_crypto_hmac': image.integrity(), 'observer_bytes': 1536,
            'bcm_route_present': False, 'hardware_validated': False}


def check_report(report, lock):
    transfer = load_module('source-artifact-transfer')
    names, entries = transfer.output_manifest(report)
    if report.get('native_arch_timer_requested') is not True or len(names) != 6:
        raise ValueError('Legacy source-only profile is not a native candidate')
    for key in ('unixnu_commit', 'device_commit'):
        if report.get(key) != lock[key]:
            raise ValueError('Wrong selected '+key)
    if report.get('references', {}).get('xnu') != lock['xnu_commit']:
        raise ValueError('Wrong XNU revision')
    for name in ('ArmCpu', 'ArmDTPlatform'):
        if report.get('components', {}).get(name) != lock['components'][name]:
            raise ValueError('Wrong native driver revision: '+name)
    if not report.get('armcpu_driver_linked') or not report.get('armdt_platform_driver_linked'):
        raise ValueError('Driver link gates did not pass')
    return names, entries


def check_board(board):
    if isinstance(board, (bytes, str)):
        board = tomllib.loads(board.decode() if isinstance(board, bytes) else board)
    if board['components']['include'] != ['XnuPrelink', 'DarwinBoot', 'ArmDTPlatform', 'ArmCpu']:
        raise ValueError('Native mainline component list mismatch')
    if board['kernel']['defconfig'] != 'j716f_arch_timer_source_defconfig':
        raise ValueError('Native defconfig required')
    payload = board['payload']
    if payload.get('panic_screen') is not False or any(k.startswith('entry_probe_') for k in payload):
        raise ValueError('Legacy binary patch/reset policy forbidden')
    if payload.get('timeout') != -1 or payload.get('kernel_args', []).count('cpus=1') != 1:
        raise ValueError('Native mainline requires no countdown and one CPU')
    allowed_args = {'-v', 'serial=0', 'debug=0x14e', 'cpus=1', '-no_slto_panic'}
    args = payload.get('kernel_args', [])
    if any(not isinstance(arg, str) or arg not in allowed_args for arg in args) or len(args) != len(set(args)):
        raise ValueError('Unreviewed boot argument/reset/IRQ bypass policy forbidden')
    archives = board['kernel']['archives']
    if archives[:2] != ['component:ArmDTPlatform', 'component:ArmCpu'] or len(set(archives)) != len(archives):
        raise ValueError('Native archive order/uniqueness mismatch')
    return {'native_selected': True, 'legacy_binary_patching': False, 'automatic_boot': False}


def save_new(path, report):
    with Path(path).open('x', encoding='utf-8') as stream:
        stream.write(json.dumps(report, indent=2)+'\n')
