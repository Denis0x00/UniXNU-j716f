#!/usr/bin/env python3
"""Read-only validation of the independent TB-J716F UFS adapter host build.

Only --output-report is written. The official XnuPrelink readers are imported with
bytecode disabled; no planner, emitter, compiler or device command is invoked.
Success establishes the checks listed in the report, not runtime ABI or boot.
"""
import argparse
from bisect import bisect_right
from collections import Counter
import copy
import datetime
import hashlib
import json
from pathlib import Path, PurePosixPath
import posixpath
import re
import shlex
import struct
import subprocess
import sys
import tomllib

sys.dont_write_bytecode = True

MAIN_REVISION = 'dff276535db3ac394f628cec58a718cd46c20ac2'
PRELINK_REVISION = '440589b0c31625f17edf363e24b3c82c51c70bb9'
ORIGINAL_SHA256 = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
PROFILE = 'ipados-17.7.11-21H461'
SLIDE = -0x1000000
AMFI = 'com.apple.driver.AppleMobileFileIntegrity'
AMFI_ORIGINAL = 0xfffffff005ab4ae0
FALSE_FLAGS = (
    'images_modified', 'instruction_relocation_complete', 'abi_qualified',
    'loadable', 'register_dataflow_qualified', 'source_inputs_modified',
    'runtime_qualified',
)


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def digest(data):
    return hashlib.sha256(data).hexdigest()


def read_json(path):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f'Duplicate JSON key: {key}')
            result[key] = value
        return result
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def effective_config(text):
    """Independently parse effective CONFIG values, allowing repeated fragment values."""
    values = {}
    for number, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        unset = re.fullmatch(r'#\s*CONFIG_(\w+) is not set', line)
        if unset:
            values[unset.group(1)] = False
        elif not line or line.startswith('#'):
            continue
        else:
            match = re.fullmatch(r'CONFIG_(\w+)=(.*)', line)
            require(match is not None, f'Invalid config line {number}')
            name, value = match.groups()
            if value in ('y', 'n'):
                values[name] = value == 'y'
            elif value.startswith('"') and value.endswith('"'):
                values[name] = value[1:-1]
            elif re.fullmatch(r'-?(0x[0-9a-fA-F]+|\d+)', value):
                values[name] = int(value, 16 if value.lstrip('-').startswith('0x') else 10)
            else:
                raise ValueError(f'Invalid config value on line {number}')
    return values


def compare_configuration(current_text, baseline_text):
    current, baseline = effective_config(current_text), effective_config(baseline_text)
    require(current.get('XNU_DT_PANIC_REGION') is True, 'Effective DT panic-region option is not enabled')
    require(baseline.get('XNU_DT_PANIC_REGION', False) is False, 'Retained baseline already enabled DT panic-region')
    current_rest, baseline_rest = dict(current), dict(baseline)
    current_rest.pop('XNU_DT_PANIC_REGION', None)
    baseline_rest.pop('XNU_DT_PANIC_REGION', None)
    require(current_rest == baseline_rest, 'Effective configuration changed beyond DT panic-region')
    return {'only_effective_delta': {'XNU_DT_PANIC_REGION': {'before': baseline.get('XNU_DT_PANIC_REGION'), 'after': True}},
            'current_effective_keys': len(current), 'baseline_effective_keys': len(baseline)}


def check_panic_compile_record(path, tree):
    """Clang -MJ writes one JSON object followed by a comma; validate that one command."""
    raw = path.read_text(encoding='utf-8').strip()
    if raw.endswith(','):
        raw = raw[:-1].rstrip()
    command = json.loads(raw)
    require(isinstance(command, dict), 'Expected exactly one clang compilation record')
    argv = command.get('arguments')
    require(isinstance(argv, list) and argv and all(isinstance(x, str) for x in argv), 'Compilation record lacks an arguments array')
    directory = Path(command['directory']).resolve()
    source = Path(command['file'])
    source = (source if source.is_absolute() else directory/source).resolve()
    output = Path(command['output'])
    output = (output if output.is_absolute() else directory/output).resolve()
    expected_source = (tree/'.cache/xnu-work/pexpert/arm/pe_init.c').resolve()
    expected_object = path.with_suffix('').resolve()
    require(source == expected_source and source.is_file(), 'pe_init record names a different source')
    require(directory == path.parent.resolve() and output == expected_object and output.is_file(), 'pe_init record names a different or missing object')
    definitions = []
    index = 0
    while index < len(argv):
        value = argv[index]
        if value in ('-D', '-U') and index + 1 < len(argv):
            name = argv[index + 1]
            if name.split('=', 1)[0] == 'XNU_DT_PANIC_REGION':
                require(value == '-D', 'Compilation undefines DT panic-region')
                definitions.append(name.split('=', 1)[1] if '=' in name else '1')
                index += 2
                continue
        if value.startswith('-DXNU_DT_PANIC_REGION'):
            name = value[2:]
            require(name.split('=', 1)[0] == 'XNU_DT_PANIC_REGION', 'Unexpected similarly named DT panic macro')
            definitions.append(name.split('=', 1)[1] if '=' in name else '1')
        elif 'XNU_DT_PANIC_REGION' in value:
            raise ValueError('Unrecognized or conflicting DT panic-region compiler argument')
        index += 1
    require(definitions == ['1'], 'pe_init.o must compile with exactly one DT panic-region definition of 1')
    return {'path': str(path), 'sha256': digest(path.read_bytes()), 'source': str(source),
            'source_sha256': digest(source.read_bytes()), 'object': str(output),
            'object_sha256': digest(output.read_bytes()), 'define': 'XNU_DT_PANIC_REGION=1'}


OWNER_NAMES = tuple('_apps_smmu_owner_' + name for name in (
    'install_handoff', 'try_acquire', 'validate', 'validate_ready',
    'publication_begin', 'publication_complete', 'queue_started',
    'mark_dma_possible', 'complete_dma', 'confirm_stopped',
    'confirm_unpublished', 'quarantine', 'release'))
RUNTIME_NAMES = tuple('_sm8250_ufs_' + name for name in (
    'adapter_init', 'allocate', 'activate', 'read_known', 'stop', 'discard_unused', 'status'))
OBJECT_NAMES = ('Sm8250UfsRuntime.o', 'Sm8250UfsHci.o', 'Sm8250UfsHandoff.o', 'AppsSmmuOwner.o')


def symbols(data, image):
    """Read defined N_SECT symbols, including local function/data identities."""
    tables = [at for command, at, size in image['commands'] if command == 2 and size == 24]
    require(len(tables) == 1, 'Expected one actual Mach-O symbol table')
    symoff, count, stroff, strsize = struct.unpack_from('<4I', data, tables[0] + 8)
    require(symoff + count * 16 <= len(data) and stroff + strsize <= len(data), 'Invalid symbol table extent')
    strings, rows = data[stroff:stroff + strsize], []
    for index in range(count):
        strx, kind, section, desc, address = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
        if kind & 0xe0 or kind & 0x0e != 0x0e:
            continue
        end = strings.find(b'\0', strx)
        require(strx < strsize and end >= strx, 'Invalid defined-symbol name')
        rows.append({'name': strings[strx:end].decode(), 'address': address,
                     'external': bool(kind & 1), 'section': section})
    return rows


def named(rows, name, external=False):
    found = [row['address'] for row in rows if row['name'] == name and (row['external'] or not external)]
    require(len(found) == 1, 'Expected one defined symbol: ' + name + '; found ' + str(len(found)))
    return found[0]


def function_bounds(image, rows):
    sections = [sec for seg in image['segments'] if seg['initprot'] & 4 for sec in seg['sections']
                if sec['name'] in ('__text', '__stubs')]
    result = {}
    for sec in sections:
        addresses = sorted({row['address'] for row in rows
                            if sec['va'] <= row['address'] < sec['va'] + sec['size']})
        for i, start in enumerate(addresses):
            result[start] = addresses[i + 1] if i + 1 < len(addresses) else sec['va'] + sec['size']
    return result


def instructions(np, data, image, start, end):
    require(start < end and (end - start) % 4 == 0, 'Invalid AArch64 function extent')
    at = np.backed_offset(image['segments'], start, end - start, len(data))
    return [(start + index * 4, word) for index, word in enumerate(
        struct.unpack_from('<' + str((end - start) // 4) + 'I', data, at))]


def branch_target(pc, word):
    imm = word & 0x03ffffff
    if imm & 0x02000000:
        imm -= 0x04000000
    return (pc + imm * 4) & ((1 << 64) - 1)


def registration_code(np, native, image, rows, bounds):
    """Decode a small whitelist: address formation, literal one, one STLR, RET."""
    start = named(rows, RUNTIME_NAMES[0], True)
    require(start in bounds, 'Adapter init has no executable function extent')
    state = [None] * 32
    stores, decoded, returned = [], [], False
    for pc, word in instructions(np, native, image, start, bounds[start]):
        if returned:
            require(word in (0, 0xd503201f), 'Unexpected instructions after adapter init return')
            continue
        require(len(decoded) < 16, 'Registration init exceeds the simple no-hardware instruction envelope')
        rd, rn = word & 31, (word >> 5) & 31
        if word in (0xd503201f, 0xd503245f):
            meaning = 'nop/bti'
        elif word & 0x9f000000 in (0x10000000, 0x90000000):
            immediate = ((word >> 5 & 0x7ffff) << 2) | (word >> 29 & 3)
            if immediate & 0x100000:
                immediate -= 0x200000
            state[rd] = ((pc & ~4095) + immediate * 4096 if word & 0x80000000 else pc + immediate) & ((1 << 64) - 1)
            meaning = 'adrp/adr'
        elif word & 0xff000000 == 0x91000000:
            require(state[rn] is not None and rd != 31 and rn != 31, 'Unknown init address base')
            state[rd] = (state[rn] + ((word >> 10 & 4095) << (12 if word & (1 << 22) else 0))) & ((1 << 64) - 1)
            meaning = 'add immediate'
        elif word & 0xff800000 in (0x52800000, 0xd2800000):
            state[rd] = (word >> 5 & 0xffff) << ((word >> 21 & 3) * 16)
            meaning = 'movz'
        elif word & 0xfffffc00 == 0x889ffc00:
            require(state[rn] is not None and state[rd] == 1, 'Init release store is not literal one to a known address')
            stores.append({'pc': hex(pc), 'address': state[rn], 'value': 1, 'width': 4, 'ordering': 'release'})
            meaning = 'stlr w'
        elif word == 0xd65f03c0:
            returned = True
            meaning = 'ret'
        else:
            raise ValueError('Unqualified adapter init instruction at ' + hex(pc) + ': ' + hex(word))
        decoded.append({'pc': hex(pc), 'word': hex(word), 'operation': meaning})
    require(returned and len(stores) == 1, 'Init must return after exactly one release store')
    flags = [row for row in rows if 'runtime_initialized' in row['name']]
    require(len(flags) == 1 and flags[0]['address'] == stores[0]['address'], 'Init store does not target the unique registration flag')
    address = stores[0]['address']
    require(any(seg['initprot'] & 2 and not seg['initprot'] & 4 and
                seg['va'] <= address and address + 4 <= seg['va'] + seg['size'] for seg in image['segments']),
            'Registration flag is not writable native kernel data')
    return {'symbol': RUNTIME_NAMES[0], 'address': hex(start), 'instructions': decoded,
            'stores': stores, 'flag_symbol': flags[0]['name'], 'calls': [],
            'mmio_or_allocator_instructions': False, 'instruction_whitelist_verified': True}


def archive_members(data):
    require(data.startswith(b'!<arch>\n'), 'Invalid actual adapter archive')
    at, members, longnames = 8, {}, b''
    while at < len(data):
        require(at + 60 <= len(data) and data[at + 58:at + 60] == b'`\n', 'Malformed archive member header')
        header = data[at:at + 60]
        size = int(header[48:58].strip())
        name = header[:16].decode().rstrip()
        payload = data[at + 60:at + 60 + size]
        require(len(payload) == size, 'Truncated archive member')
        if name.startswith('#1/'):
            width = int(name[3:])
            name = payload[:width].rstrip(b'\0').decode()
            payload = payload[width:]
        elif name == '//':
            longnames = payload
        elif name.startswith('/') and name[1:].isdigit():
            begin = int(name[1:])
            end = longnames.find(b'/\n', begin)
            require(end >= begin, 'Bad GNU archive name offset')
            name = longnames[begin:end].decode()
        else:
            name = name.rstrip('/')
        if name not in ('', '/', '//') and not name.startswith('__.SYMDEF'):
            require(name not in members, 'Duplicate actual archive member: ' + name)
            members[name] = payload
        at += 60 + size + (size % 2)
    require(at == len(data), 'Archive trailing data is malformed')
    return members


def frozen_snapshot(device):
    manifest_path = device/'docs/ufs-adapter-inputs.json'
    manifest = read_json(manifest_path)
    require(manifest['schema_version'] == 1, 'Unexpected adapter manifest schema')
    for name, spec in manifest['files'].items():
        path = (device/name).resolve()
        require(path.is_relative_to(device.resolve()) and path.is_file() and not (device/name).is_symlink(), 'Invalid frozen input: ' + name)
        require(path.stat().st_size == spec['bytes'] and digest(path.read_bytes()) == spec['sha256'], 'Frozen input mismatch: ' + name)
    return manifest, digest(manifest_path.read_bytes())


def actual_archive_link(log_path, native_path, archive_path):
    """Find an executed verbose clang++ invocation, never an echo/stamp recipe.

    MakeInc.kernel's .LDFLAGS omits LD_KERNEL_ARCHIVES. VERBOSE supplied as a
    make command variable exposes the actual compiler-driver command. The
    successful official run and artifact identities are checked separately.
    """
    raw = log_path.read_bytes()
    text = raw.decode('utf-8')
    logical, pending, begin = [], '', 0
    for number, line in enumerate(text.splitlines(), 1):
        if not pending:
            begin = number
        if line.endswith('\\'):
            pending += line[:-1] + ' '
        else:
            logical.append((begin, number, pending + line))
            pending = ''
    require(not pending, 'Truncated shell continuation in verbose kernel log')
    matches = []
    expected_native = str(native_path)
    expected_archive = str(archive_path)
    require(PurePosixPath(expected_native).is_absolute() and PurePosixPath(expected_archive).is_absolute(),
            'Native link expectations must be absolute POSIX paths')
    expected_native = posixpath.normpath(expected_native)
    force_load = '-Wl,-force_load,' + expected_archive
    for first, last, line in logical:
        try:
            lexer = shlex.shlex(line, posix=True, punctuation_chars=';&|<>()')
            lexer.whitespace_split = True
            lexer.commenters = ''
            argv = list(lexer)
        except ValueError:
            continue  # Other tools also print prose/diagnostics in this log.
        if not argv or not PurePosixPath(argv[0]).is_absolute() or not re.fullmatch(r'clang\+\+(?:-\d+(?:\.\d+)*)?', PurePosixPath(argv[0]).name):
            continue
        # An actual direct driver invocation starts with the executable, not
        # echo/printf/REPLACECONTENTS, and has no compound shell operators.
        if any(re.fullmatch(r'[;&|<>()]+', token) for token in argv) or '`' in line or '$(' in line:
            continue
        if argv.count('-o') != 1:
            continue
        at = argv.index('-o')
        if at + 1 == len(argv):
            continue
        output = PurePosixPath(argv[at + 1])
        if not output.is_absolute() or posixpath.normpath(str(output)) != expected_native:
            continue
        require('-c' not in argv and '-E' not in argv and '-S' not in argv, 'Native output matched a compile-only command')
        require(argv.count(force_load) == 1, 'Actual native link lacks the unique exact adapter force-load argument')
        require(not any('libSm8250UfsAdapter.a' in value and value != force_load for value in argv),
                'Actual native link names another adapter archive or an unexpected argument form')
        require(argv.count('-filelist') == 1 and argv.index('-filelist') + 1 < len(argv) and
                argv[argv.index('-filelist') + 1] == 'link.filelist', 'Actual native link lacks the native object filelist')
        matches.append({'first_line': first, 'last_line': last, 'executable': argv[0],
                        'output': str(output), 'normalized_output': expected_native, 'force_load_argument': force_load,
                        'arguments': argv, 'command_sha256': digest(line.encode())})
    require(len(matches) == 1, 'Expected exactly one executed verbose clang++ native link with the adapter archive; found ' + str(len(matches)))
    require(log_path.read_bytes() == raw, 'Verbose kernel log changed during command validation')
    return {'path': str(log_path), 'sha256': digest(raw),
            'evidence': 'Direct executed verbose compiler-driver invocation in the successful official kernel run',
            **matches[0]}


def relative_switch(np, native, image, code, index, function_start, function_end):
    """Prove LLVM's guarded signed-offset jump table; never treat unknown BR as a call.

    Recognized shape: CMP Xidx,#limit; B.HI outside dispatch; ADRP/ADD table;
    ADR anchor; LDRSW Xoff,[Xtable,Xidx,LSL#2]; ADD Xtarget,Xanchor,Xoff;
    BR Xtarget. Every table entry must resolve inside this same function.
    """
    require(index >= 7, 'Unqualified BR without a complete switch dispatch sequence')
    seq = code[index - 7:index + 1]
    (cmp_pc, cmp), (gate_pc, gate), (page_pc, page), (_, add), (anchor_pc, anchor), (_, load), (_, target_add), (branch_pc, branch) = seq
    require(cmp & 0xff80001f == 0xf100001f and not cmp & (1 << 22), 'BR switch lacks bounded 64-bit CMP immediate')
    require(gate & 0xff00001f == 0x54000008, 'BR switch lacks unsigned-high guard')
    limit, index_reg = cmp >> 10 & 4095, cmp >> 5 & 31
    require(limit < 4096 and index_reg != 31, 'Invalid switch table index/limit')
    immediate = gate >> 5 & 0x7ffff
    if immediate & 0x40000:
        immediate -= 0x80000
    reject = gate_pc + immediate * 4
    require(function_start <= reject < function_end and not cmp_pc < reject <= branch_pc,
            'Switch high-index branch does not bypass the dispatch within the same function')
    require(page & 0x9f000000 == 0x90000000 and add & 0xff000000 == 0x91000000 and not add & (1 << 22),
            'BR switch lacks literal ADRP/ADD table base')
    base_reg = page & 31
    require(add & 31 == base_reg and add >> 5 & 31 == base_reg, 'Switch table base register changed')
    require(anchor & 0x9f000000 == 0x10000000, 'BR switch lacks relative ADR target anchor')
    target_reg, offset_reg = anchor & 31, load & 31
    require(len({index_reg, base_reg, target_reg, offset_reg}) == 4 and 31 not in (base_reg, target_reg, offset_reg),
            'Switch dispatch aliases its index, table, anchor or offset registers')
    require(load & 0xffe0fc00 == 0xb8a07800 and load >> 5 & 31 == base_reg and load >> 16 & 31 == index_reg,
            'Switch does not load a signed 32-bit relative entry with the guarded index')
    require(target_add & 0xffe0fc00 == 0x8b000000 and target_add & 31 == target_reg and
            target_add >> 5 & 31 == target_reg and target_add >> 16 & 31 == offset_reg,
            'Switch target is not anchor plus the signed table entry')
    require(branch & 0xfffffc1f == 0xd61f0000 and branch >> 5 & 31 == target_reg,
            'Switch BR uses a different target register')

    def adr_address(pc, word, page_relative):
        imm = ((word >> 5 & 0x7ffff) << 2) | (word >> 29 & 3)
        if imm & 0x100000:
            imm -= 0x200000
        return ((pc & ~4095) + imm * 4096 if page_relative else pc + imm) & ((1 << 64) - 1)

    table = adr_address(page_pc, page, True) + (add >> 10 & 4095)
    anchor_address = adr_address(anchor_pc, anchor, False)
    size = (limit + 1) * 4
    require(table % 4 == 0 and any(not seg['initprot'] & 2 and seg['va'] <= table and
                table + size <= seg['va'] + seg['filesize'] for seg in image['segments']),
            'Switch table is not backed read-only native data')
    at = np.backed_offset(image['segments'], table, size, len(native))
    table_bytes = native[at:at + size]
    targets = sorted({(anchor_address + offset) & ((1 << 64) - 1)
                      for offset in struct.unpack('<' + str(limit + 1) + 'i', table_bytes)})
    require(all(function_start <= target < function_end and target % 4 == 0 and
                not cmp_pc < target <= branch_pc for target in targets),
            'Switch table contains a nonlocal, unaligned or guard-bypassing target')
    # No direct branch in this function may enter after the CMP and bypass it.
    for pc, word in code:
        destination = None
        if word & 0x7c000000 == 0x14000000:
            destination = branch_target(pc, word)
        elif word & 0xff000010 == 0x54000000 or word & 0x7e000000 == 0x34000000:
            imm = word >> 5 & 0x7ffff
            destination = pc + (imm - (0x80000 if imm & 0x40000 else 0)) * 4
        elif word & 0x7e000000 == 0x36000000:
            imm = word >> 5 & 0x3fff
            destination = pc + (imm - (0x4000 if imm & 0x2000 else 0)) * 4
        require(destination is None or not cmp_pc < destination <= branch_pc,
                'Direct branch bypasses the switch index guard')
    return {'branch_pc': hex(branch_pc), 'guard_pc': hex(cmp_pc), 'index_limit_inclusive': limit,
            'table_address': hex(table), 'table_entries': limit + 1, 'table_sha256': digest(table_bytes),
            'anchor': hex(anchor_address), 'targets': [hex(target) for target in targets],
            'all_targets_within_current_function': True, 'adds_stack_frame': False}


def stack_budget(np, native, image, rows, bounds, frames):
    """Native direct branches plus conservative source-defined callback sets.

    This bounds only the adapter's own synchronous frames. External XNU calls,
    dynamic IOKit dispatch and interrupt nesting require a separate analysis.
    """
    mapped, missing = {}, []
    for name, size in frames.items():
        found = {row['address'] for row in rows if row['name'] == name and row['address'] in bounds}
        if not found:
            missing.append(name)  # dead stripped/COMDAT folding; exported entrypoints checked separately
        for address in found:
            mapped[address] = {'bytes': max(size, mapped.get(address, {}).get('bytes', 0)), 'name': name}
    require(mapped, 'No compiler stack frames matched final native functions')
    io_callbacks = {address for address, entry in mapped.items() if '_GLOBAL__N_' in entry['name'] and
                    any(token in entry['name'] for token in ('6read32', '7write32', '7barrier', '5ticks', '5cache'))}
    hci_callbacks = {address for address, entry in mapped.items() if 'HciPort' in entry['name'] and 'Thunk' in entry['name']}
    handoff_callbacks = {address for address, entry in mapped.items() if 'HandoffTransaction' in entry['name'] and 'readThunk' in entry['name']}
    require(len(io_callbacks) == 5 and len(hci_callbacks) == 6 and len(handoff_callbacks) == 1, 'Runtime callback identities are incomplete')
    edges, external, indirect, tail_loops, switches = {address: set() for address in mapped}, [], [], [], []
    for address, entry in mapped.items():
        name = entry['name']
        code = instructions(np, native, image, address, bounds[address])
        for index, (pc, word) in enumerate(code):
            if word & 0x7c000000 == 0x14000000:
                destination = branch_target(pc, word)
                if destination in mapped:
                    if destination == address and not word & 0x80000000:
                        tail_loops.append(hex(pc))
                    else:
                        edges[address].add(destination)
                elif word & 0x80000000:
                    external.append({'caller': name, 'pc': hex(pc), 'target': hex(destination)})
            elif word & 0xfffffc1f == 0xd61f0000:
                switches.append({'function': name, **relative_switch(
                    np, native, image, code, index, address, bounds[address])})
            elif word & 0xfffffc1f == 0xd63f0000:
                choices, label = set(), ''
                if 'SingleSlot' in name or (name.startswith('_sm8250_ufs_') and
                        any(token in name for token in ('activate', 'read_known', 'stop'))) or (
                        '_GLOBAL__N_' in name and '7execute' in name):
                    choices, label = hci_callbacks, 'restricted Port callbacks'
                elif 'HciPort' in name:
                    choices, label = io_callbacks, 'bounded runtime Io callbacks'
                elif 'observeUfs' in name:
                    choices, label = handoff_callbacks, 'observer Read32 callback'
                elif 'HandoffTransaction' in name:
                    choices, label = set(io_callbacks), 'handoff Io callbacks'
                    if '7observe' in name or '3run' in name:
                        choices |= handoff_callbacks
                        label += ' plus potentially inlined observer'
                elif name.startswith('_sm8250_ufs_') or '_GLOBAL__N_' in name:
                    # Runtime allocation/release uses real external OSObject methods.
                    require(any(token in name for token in ('allocate', 'discard_unused', 'map_range', 'release_unused')),
                            'Unknown adapter indirect-call boundary: ' + name)
                    label = 'external XNU/IOKit virtual method; outside adapter-only budget'
                else:
                    raise ValueError('Unmodeled adapter indirect-call boundary: ' + name)
                edges[address] |= choices
                indirect.append({'caller': name, 'pc': hex(pc), 'model': label,
                                 'possible_adapter_targets': [mapped[x]['name'] for x in sorted(choices)]})
    cache, visiting = {}, set()

    def longest(address):
        require(address not in visiting, 'Cycle in adapter call graph at ' + mapped[address]['name'])
        if address in cache:
            return cache[address]
        visiting.add(address)
        children = [longest(child) for child in edges[address]]
        best = max(children, default=(0, []), key=lambda row: row[0])
        result = (mapped[address]['bytes'] + best[0], [mapped[address]['name']] + best[1])
        visiting.remove(address)
        cache[address] = result
        return result

    paths = {name: longest(named(rows, name, True)) for name in RUNTIME_NAMES}
    # Also covers exported owner methods and functions reachable only by a callback.
    overall = max((longest(address) for address in mapped), key=lambda row: row[0])
    require(overall[0] <= 8192, 'Adapter-only conservative frame chain exceeds 8 KiB: ' + str(overall))
    return {'per_frame_limit': 4096, 'adapter_chain_limit': 8192, 'max_chain_bytes': overall[0],
            'max_chain': overall[1], 'entrypoint_chains': paths, 'mapped_functions': len(mapped),
            'unretained_compiler_functions': missing, 'indirect_call_model': indirect,
            'external_direct_calls': external, 'tail_loops': tail_loops,
            'verified_intra_function_switches': switches,
            'scope': 'Compiler frames plus native direct calls and conservative internal callback sets; excludes external XNU/IOKit frames and interrupt nesting'}


def verify_adapter(root, out, tree, device, native_path, np, images, parsed, passed):
    manifest, manifest_sha = frozen_snapshot(device)
    build = tree/'build/sm8250-ufs-adapter'
    result_path = build/'result.json'
    result = read_json(result_path)
    result_sha = digest(result_path.read_bytes())
    require(result['passed'] is True and result['triple'] == 'arm64-apple-ios17.0', 'Adapter archive recipe did not pass')
    require(result['inputs_before'] == result['inputs_after'] and result['inputs_before']['manifest_sha256'] == manifest_sha,
            'Recipe manifest identity or before/after inputs differ')
    require(result['inputs_before']['files'] == manifest['files'], 'Recipe input inventory differs from final freeze')
    require(Path(result['tree']).resolve() == tree.resolve() and Path(result['target']).resolve() == device.resolve(), 'Archive result belongs to another build')
    archive = build/'libSm8250UfsAdapter.a'
    require(digest(archive.read_bytes()) == result['archive_sha256'], 'Actual archive differs from recipe result')
    members = archive_members(archive.read_bytes())
    require(tuple(members) == OBJECT_NAMES and result['archive_members'] == list(OBJECT_NAMES), 'Adapter archive must contain exactly four objects')
    compile_record = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    require(Path(result['fresh_compile_record']).resolve() == compile_record.resolve() and
            digest(compile_record.read_bytes()) == result['compile_record_sha256'], 'Fresh ABI record differs')
    objects, frames, defined_counts = {}, {}, Counter()
    for name in OBJECT_NAMES:
        path = build/name
        data = path.read_bytes()
        require(members[name] == data and digest(data) == result['objects'][name]['sha256'], 'Archive/object/result byte mismatch: ' + name)
        image = np.macho(data)
        require(image['kind'] == 1, 'Adapter archive member is not an ordinary ARM64 object')
        versions = [struct.unpack_from('<II', data, at + 8) for command, at, size in image['commands'] if command == 0x32]
        require(versions == [(2, 17 << 16)], 'Adapter object deployment differs from iOS 17.0')
        sections = [section['name'] for segment in image['segments'] for section in segment['sections']]
        require(not set(sections) & {'__mod_init_func', '__mod_term_func'}, 'Adapter has dynamic global constructor/destructor section')
        object_symbols = symbols(data, image)
        require(not any('_GLOBAL__sub_I' in row['name'] or '__cxx_global_var_init' in row['name'] for row in object_symbols), 'Adapter has dynamic initializer symbol')
        defined_counts.update(row['name'] for row in object_symbols if row['external'])
        usage = path.with_suffix('.su')
        require(digest(usage.read_bytes()) == result['objects'][name]['stack_usage_sha256'], 'Actual stack report differs')
        entries = []
        for line in usage.read_text().splitlines():
            location, size, kind = line.rsplit('\t', 2)
            require(size.isdigit() and kind == 'static' and int(size) <= 4096, 'Invalid/dynamic/oversized adapter stack frame: ' + line)
            entries.append({'function': location, 'bytes': int(size), 'kind': kind})
            symbol = '_' + location.rsplit(':', 1)[-1]
            frames[symbol] = max(int(size), frames.get(symbol, 0))
        require(entries and entries == result['objects'][name]['functions'], 'Recipe stack entries differ from compiler output')
        command_path = build/(path.stem + '-compile-command.json')
        command = read_json(command_path)
        for flag in ('-Wall', '-Wextra', '-Werror', '-fstack-usage', '-Wframe-larger-than=4096', '-mgeneral-regs-only'):
            require(flag in command, 'Missing required adapter compile flag: ' + flag)
        source = device/'socs/sm8250/kits/Sm8250Platform'/path.with_suffix('.cpp').name
        require(command.count(str(source)) == 1 and command.count('-o') == 1 and command[command.index('-o') + 1] == str(path), 'Unexpected source/object compile paths')
        require(not any('NativeUfs.cpp' in argument for argument in command), 'Unexpected SM8550 NativeUfs compilation')
        objects[name] = {'sha256': digest(data), 'stack_sha256': digest(usage.read_bytes()),
                         'max_frame_bytes': max(row['bytes'] for row in entries), 'functions': len(entries)}
    for name in OWNER_NAMES + RUNTIME_NAMES:
        require(defined_counts[name] == 1 and result['required_defined_symbols'][name] == 1, 'Adapter archive export is not unique: ' + name)
    require({name for name in defined_counts if name.startswith('_apps_smmu_owner_')} == set(OWNER_NAMES), 'Unexpected owner exports')
    native_rows = symbols(images['native'], parsed['native'])
    native_bounds = function_bounds(parsed['native'], native_rows)
    try:
        candidate_rows = symbols(images['candidate'], parsed['candidate'])
    except ValueError as exc:
        require('Expected one actual Mach-O symbol table' in str(exc), 'Malformed candidate symbol table')
        candidate_rows = []
    retained = {}
    for name in OWNER_NAMES + RUNTIME_NAMES:
        address = named(native_rows, name, True)
        require(address in native_bounds, 'Required native function lacks executable extent: ' + name)
        length = native_bounds[address] - address
        native_at = np.backed_offset(parsed['native']['segments'], address, length, len(images['native']))
        candidate_at = np.backed_offset(parsed['candidate']['segments'], address, length, len(images['candidate']))
        code = images['native'][native_at:native_at + length]
        require(images['candidate'][candidate_at:candidate_at + length] == code, 'Required native function bytes differ in prelink: ' + name)
        candidate_symbols = [row for row in candidate_rows if row['external'] and row['name'] == name]
        require(not candidate_symbols or len(candidate_symbols) == 1 and candidate_symbols[0]['address'] == address, 'Candidate export identity differs: ' + name)
        retained[name] = {'native_address': hex(address), 'bytes': length, 'code_sha256': digest(code),
                          'prelinked_retention': 'symbol and identical bytes' if candidate_symbols else 'native symbol map and identical prelinked executable bytes'}
    init = registration_code(np, images['native'], parsed['native'], native_rows, native_bounds)
    init_address = named(native_rows, RUNTIME_NAMES[0], True)
    iokit = named(native_rows, '_IOKitBSDInit', True)
    callers = []
    for name in ('_bsd_autoconf', '_bsd_init'):
        starts = [row['address'] for row in native_rows if row['name'] == name and row['address'] in native_bounds]
        require(len(starts) <= 1, 'Ambiguous BSD caller: ' + name)
        for start in starts:
            code = instructions(np, images['native'], parsed['native'], start, native_bounds[start])
            calls = [(pc, branch_target(pc, word)) for pc, word in code if word & 0xfc000000 == 0x94000000]
            for pc, target in calls:
                if target == init_address:
                    earlier = [site for site, destination in calls if destination == iokit and site < pc]
                    require(earlier, 'Adapter init BL lacks preceding IOKitBSDInit in its caller')
                    callers.append({'function': name, 'iokit_call': hex(earlier[-1]), 'adapter_init_bl': hex(pc)})
    require(callers, 'No actual BSD BL instruction calls the unique adapter init')
    budget = stack_budget(np, images['native'], parsed['native'], native_rows, native_bounds, frames)
    # Kernel command evidence is associated with the actual artifact hashes.
    runs = []
    for file in sorted((root/'analysis/host-ufs-adapter-20261113').glob('kernel-*.json')):
        run = read_json(file)
        if run.get('status') == 'passed' and run.get('target') == 'j716f-host-ufs-adapter':
            artifacts = {Path(row['path']).resolve(): row['sha256'] for row in run.get('artifacts', [])}
            if (artifacts.get((out/'kernel/kernel.macho').resolve()) == digest(images['candidate']) and
                    artifacts.get(native_path.resolve()) == digest(images['native'])):
                runs.append((file, run))
    require(runs, 'No successful official kernel run matches this native/candidate pair')
    run_path, run = runs[-1]
    require(run['target_source_manifest']['sha256'] == manifest_sha, 'Successful official build uses a different source snapshot')
    require(any(entry.get('exit_code') == 0 and entry.get('argv', [])[-1:] == ['kernel'] and
                any(arg.endswith('/scripts/host.py') for arg in entry['argv']) for entry in run['commands']), 'Missing successful official build kernel command')
    logs = {}
    for label in ('kernel-bootstrap.log', 'storage-headers.log',
                  'archive-01-build-corecrypto-14.log', 'archive-02-build-corecrypto-14.log',
                  'component-ArmDTPlatform.log', 'component-ArmCpu.log', 'component-SoftwareAES.log',
                  'archive-06-build-software-aes-bindings.log', 'archive-07-build-mockfs-fixture.log',
                  'archive-08-build-libpthread-kernel.log', 'archive-09-build-sm8250-ufs-adapter.log',
                  'kernel.log', 'prelink.log'):
        file = out/'logs'/label
        require(file.is_file() and file.stat().st_size, 'Required actual official build log missing: ' + label)
        logs[label] = {'sha256': digest(file.read_bytes()), 'bytes': file.stat().st_size}
    actual_link = actual_archive_link(out/'logs/kernel.log', native_path, archive)
    require(actual_link['sha256'] == logs['kernel.log']['sha256'], 'Official kernel log changed during verification')
    # MakeInc.kernel writes only basic flags to .LDFLAGS; LD_KERNEL_ARCHIVES is
    # appended in the executed link recipe and verified above, not in this stamp.
    ldflags = [{'path': str(file), 'sha256': digest(file.read_bytes()), 'bytes': file.stat().st_size}
               for file in sorted((tree/'build/xnu/ios-obj').rglob('.LDFLAGS'))]
    # Reread the full snapshot; this checks every byte again, not just the JSON.
    after_manifest, after_sha = frozen_snapshot(device)
    require(after_sha == manifest_sha and after_manifest == manifest, 'Frozen source inputs changed during verification')
    require(digest(result_path.read_bytes()) == result_sha, 'Adapter build result changed during verification')
    details = {'source_manifest_sha256': manifest_sha, 'archive_sha256': digest(archive.read_bytes()),
               'archive_result_sha256': result_sha,
               'objects': objects, 'unique_exports_in_native_and_prelink': retained,
               'registration_code': init, 'bsd_callers': callers, 'stack_budget': budget,
               'official_kernel_run': {'path': str(run_path), 'sha256': digest(run_path.read_bytes())},
               'official_logs': logs, 'actual_native_link': actual_link, 'basic_linker_flag_stamps': ldflags,
               'device_access': False, 'runtime_qualified': False}
    passed('adapter_archive_native_prelink_caller_init_and_stack', **details)
    return details


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True, help='VM workspace root')
    parser.add_argument('--output-report', dest='report', type=Path, required=True, help='New output JSON; never overwrites an earlier report')
    args = parser.parse_args()
    root = args.root.resolve()
    report_path = args.report.resolve()
    main_repo = root / 'UniXNU'
    out = main_repo / 'out-host-ufs-adapter-20261113/j716f-host-ufs-adapter' / PROFILE / 'j716f_host_ufs_adapter'
    tree = out / 'tree'
    status_path = root / 'analysis/host-ufs-adapter-20261113/status.json'
    candidate_path = out / 'kernel/kernel.macho'
    emitted_report_path = candidate_path.with_suffix('.json')
    original_path = main_repo / 'out/ipsw/21H461/kernelcache.decompressed'
    profile_path = main_repo / 'profiles/os' / PROFILE / 'profile.toml'
    require(not report_path.exists(), f'Refusing to overwrite report: {report_path}')
    require(not report_path.is_relative_to(main_repo), 'The verification report must be outside the UniXNU checkout and its build outputs')
    require(report_path != status_path, 'The verification report cannot replace the official status input')
    record = {
        'schema_version': 1, 'started_utc': utc(), 'status': 'running',
        'scope': 'Read-only host candidate verification; no emission, compilation, runtime ABI qualification or device action',
        'root': str(root), 'report': str(report_path), 'checks': [],
        'device_actions': False, 'build_inputs_modified': False,
    }

    def passed(name, **details):
        record['checks'].append({'name': name, 'passed': True, **details})

    def revision(path):
        return subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()

    try:
        component = main_repo / 'kits/XnuPrelink'
        require(revision(main_repo) == MAIN_REVISION, 'Unexpected UniXNU revision')
        require(revision(component) == PRELINK_REVISION, 'Unexpected XnuPrelink revision')
        sys.path.insert(0, str(component))
        import native_prelink as np
        require(Path(np.__file__).resolve() == (component / 'native_prelink.py').resolve(), 'Imported an unexpected XnuPrelink reader')
        record['sources'] = {
            'ordinary_prelink_audit_reused_from': 'analysis/tools/verify_host_dt_panic.py (independent target copy; prior tool unchanged)',
            'UniXNU': MAIN_REVISION, 'XnuPrelink': PRELINK_REVISION,
            'native_prelink.py_sha256': digest((component / 'native_prelink.py').read_bytes()),
            'emit_prelink.py_sha256': digest((component / 'emit_prelink.py').read_bytes()),
            'verifier_sha256': digest(Path(__file__).read_bytes()),
        }
        status = read_json(status_path)
        require(status['name'] == 'j716f-host-ufs-adapter', 'Status belongs to another target')
        require(Path(status['output']).resolve() == out.resolve(), 'Status output differs from the isolated host target')
        require(status['board']['board']['example'] is False, 'Host status is still an example')
        require(status['board']['board']['os_profile'] == PROFILE, 'Unexpected target OS profile')
        baseline_status_path = root/'analysis/host-dt-panic-20261113/status.json'
        baseline_status = read_json(baseline_status_path)
        config_path = Path(status['config_path']).resolve()
        require(config_path == (out/'.config').resolve(), 'Effective config is outside the new host output')
        baseline_config = Path(baseline_status['config_path']).resolve()
        require(baseline_config == (main_repo/'out-host-dt-panic-20261113/j716f-host-dt-panic'/PROFILE/'j716f_host_dt_panic/.config').resolve(), 'Unexpected DT panic baseline config path')
        current = effective_config(config_path.read_text())
        baseline = effective_config(baseline_config.read_text())
        expected_archive = '-Wl,-force_load,{tree}/build/sm8250-ufs-adapter/libSm8250UfsAdapter.a'
        require(current.get('XNU_EXTRA_LD_ARCHIVES') == expected_archive, 'Adapter force-load configuration is absent or differs')
        rest = dict(current)
        rest.pop('XNU_EXTRA_LD_ARCHIVES')
        require(rest == baseline, 'Effective configuration changed beyond adapter link input')
        require(current.get('XNU_DT_PANIC_REGION') is True and not current.get('XNU_NATIVE_UFS'), 'DT panic/native UFS flags differ')
        config_check = {'only_effective_delta': {'XNU_EXTRA_LD_ARCHIVES': expected_archive}, 'dt_panic_region': True}
        require(len(status['patch_order']) == 60 and
                [digest(Path(p).read_bytes()) for p in status['patch_order'][:58]] ==
                [digest(Path(p).read_bytes()) for p in baseline_status['patch_order']], 'Original 58-patch prefix changed')
        device = root/'work/UniXNU-j716f-host-ufs-adapter'
        expected_patches = [device/'patches/xnu'/name for name in
                            ('sm8250-ufs-adapter-init.patch', 'sm8250-ufs-adapter-dependency.patch')]
        require([Path(p).resolve() for p in status['patch_order'][58:]] == [p.resolve() for p in expected_patches], 'Wrong local adapter patch suffix')
        require(len(status['components']) == 5 and [c['id'] for c in status['components']] ==
                [c['id'] for c in baseline_status['components']], 'Original five-component selection changed')
        archives = status['board']['kernel']['archives']
        require(len(archives) == 9 and archives[:8] == baseline_status['board']['kernel']['archives'] and
                archives[-1] == 'build-sm8250-ufs-adapter.py', 'Original eight archives or new ninth archive differs')
        pe_record = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_init.o.json'
        compilation_check = check_panic_compile_record(pe_record, tree)
        mode_stamp = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/.dt-panic-region-mode'
        require(mode_stamp.read_text().strip() == '1', 'DT panic-region build mode stamp is not 1')
        config_check.update(current_path=str(config_path), current_sha256=digest(config_path.read_bytes()),
                            baseline_path=str(baseline_config), baseline_sha256=digest(baseline_config.read_bytes()))
        record['dt_panic_region'] = {'effective_config': config_check, 'compilation': compilation_check,
                                    'mode_stamp': {'path': str(mode_stamp), 'sha256': digest(mode_stamp.read_bytes()), 'value': 1},
                                    'machine_code_control_flow_verified': False,
                                    'runtime_verified': False, 'old_probe_addresses_reused': False}
        passed('dt_panic_region_effective_config_and_pe_init_compile', **record['dt_panic_region'])
        spec = status['board']['kernel']['prelink']
        profile = tomllib.loads(profile_path.read_text(encoding='utf-8'))
        require(spec == profile['kernel']['prelink'], 'Resolved prelink inputs differ from the fixed OS profile')
        require(spec['kernelcache_sha256'] == ORIGINAL_SHA256 and int(spec['slide'], 0) == SLIDE, 'Prelink identity or slide differs from the fixed target')
        require(spec['early_security_init'] == f'{AMFI}={AMFI_ORIGINAL:#x}', 'Unexpected early security initializer')
        native_path = (tree / spec['kernel']).resolve()
        require(native_path.is_relative_to(tree.resolve()), 'Native path escapes the host build tree')
        paths = {'native': native_path, 'candidate': candidate_path, 'original': original_path}
        images = {name: path.read_bytes() for name, path in paths.items()}
        hashes = {name: digest(data) for name, data in images.items()}
        require(hashes['original'] == ORIGINAL_SHA256, 'Original kernelcache hash differs from the fixed input')
        emitted = read_json(emitted_report_path)
        for field, name in (('native_sha256', 'native'), ('original_sha256', 'original'), ('candidate_sha256', 'candidate')):
            require(emitted[field] == hashes[name], f'Emitter report {field} does not match actual bytes')
        require(emitted['candidate_bytes'] == len(images['candidate']), 'Emitter candidate size differs from actual bytes')
        require(Path(emitted['output']).resolve() == candidate_path.resolve(), 'Emitter report names a different output')
        require(emitted['slide'] == SLIDE, 'Emitter slide differs from the selected profile')
        require(not any(key in emitted for key in ('missing', 'stubbed', 'collection_slide')), 'Unexpected fileset-style fields in ordinary prelink report')
        record['inputs'] = {name: {'path': str(paths[name]), 'sha256': hashes[name], 'bytes': len(data)} for name, data in images.items()}
        record['inputs']['status'] = {'path': str(status_path), 'sha256': digest(status_path.read_bytes())}
        record['inputs']['emitter_report'] = {'path': str(emitted_report_path), 'sha256': digest(emitted_report_path.read_bytes())}
        passed('input_identity_and_report', ordinary_prelink=True, slide=SLIDE,
               missing_stubbed_fields='Not present in the ordinary ARM64 report; not interpreted as zero counts')

        parsed = {name: np.macho(data) for name, data in images.items()}
        for name, image in parsed.items():
            require(image['kind'] == 2, f'{name} is not MH_EXECUTE')
            for segment in image['segments']:
                require(segment['fileoff'] + segment['filesize'] <= len(images[name]), f'{name}: segment {segment["name"]} exceeds file')
            occupied = sorted((s['va'], s['va'] + s['size'], s['name']) for s in image['segments'] if s['size'])
            for left, right in zip(occupied, occupied[1:]):
                require(left[1] <= right[0], f'{name}: virtual segments overlap: {left[2]}, {right[2]}')
        passed('macho_headers_and_segments', magic='0xfeedfacf', cpu='0x0100000c', subtype=0, file_type='MH_EXECUTE')

        def metadata(name):
            segments = parsed[name]['segments']
            matches = [s for s in segments if s['name'] == '__PRELINK_INFO']
            require(len(matches) == 1, f'{name}: expected one __PRELINK_INFO')
            segment = matches[0]
            at = np.backed_offset(segments, segment['va'], segment['filesize'], len(images[name]))
            value = np.unserialize_xml(images[name][at:at + segment['filesize']])
            entries = {}
            for entry in value['_PrelinkInfoDictionary']:
                identifier = entry['CFBundleIdentifier']
                require(identifier not in entries, f'{name}: duplicate bundle {identifier}')
                entries[identifier] = entry
            return value, entries

        original_meta, original_entries = metadata('original')
        candidate_meta, candidate_entries = metadata('candidate')
        roots = spec['roots']
        require(len(roots) == len(set(roots)) == 31, 'Expected 31 unique selected roots')
        selected, interfaces = set(), set()
        pending = list(roots)
        while pending:
            identifier = pending.pop()
            if identifier in selected or identifier in interfaces:
                continue
            require(identifier in original_entries, f'Missing declared dependency {identifier}')
            entry = original_entries[identifier]
            if entry.get('OSKernelResource') is True:
                interfaces.add(identifier)
            else:
                selected.add(identifier)
                pending.extend(entry.get('OSBundleLibraries') or {})
        original_segments = parsed['original']['segments']
        candidate_segments = parsed['candidate']['segments']
        modules, owned = {}, []
        for identifier, entry in original_entries.items():
            if entry.get('OSKernelResource') is True or not entry.get('_PrelinkExecutableSize'):
                continue
            va = entry.get('_PrelinkExecutableLoadAddr')
            require(va is not None, f'{identifier}: executable size without address')
            at = np.backed_offset(original_segments, va, 32, len(images['original']))
            module = np.macho(images['original'], at)
            require(module['kind'] == 11, f'{identifier}: expected MH_KEXT_BUNDLE')
            modules[identifier] = module
            owned.extend((s['va'], s['va'] + s['size'], identifier, s['name'])
                         for s in module['segments'] if s['size'] and s['name'] != '__LINKEDIT')
        owned.sort()
        for left, right in zip(owned, owned[1:]):
            require(left[1] <= right[0], 'Original component extents overlap')
        starts = [item[0] for item in owned]

        def owner(address):
            index = bisect_right(starts, address) - 1
            return owned[index] if index >= 0 and address < owned[index][1] else None

        codeless = selected - modules.keys()
        executable = selected - codeless
        require(len(executable) == 30 and len(interfaces) == 7 and codeless == {'com.apple.iokit.IOHIDEventDriver'}, 'Selected closure differs from the fixed input audit')
        for field, expected in (('components', executable), ('codeless_components', codeless), ('kernel_interfaces', interfaces)):
            require(emitted[field] == sorted(expected), f'Emitter {field} differs from the actual roots closure')
        require(set(candidate_entries) == selected | interfaces, 'Candidate metadata bundle set differs from selected closure')
        for identifier in codeless:
            entry = original_entries[identifier]
            require(isinstance(entry.get('IOKitPersonalities'), dict) and bool(entry['IOKitPersonalities']), 'Codeless bundle lacks personalities')
            require(not any(key in entry for key in ('CFBundleExecutable', '_PrelinkExecutableLoadAddr', '_PrelinkExecutableSourceAddr', '_PrelinkExecutableSize', '_PrelinkKmodInfo')), 'Codeless bundle has executable fields')
        for identifier in selected | interfaces:
            expected = copy.deepcopy(original_entries[identifier])
            require('_NativePrelinkEarlySecurityInit' not in expected, 'Original input unexpectedly supplies a native initializer')
            if identifier == AMFI:
                expected['_NativePrelinkEarlySecurityInit'] = AMFI_ORIGINAL + SLIDE
            for key in ('_PrelinkExecutableLoadAddr', '_PrelinkExecutableSourceAddr', '_PrelinkKmodInfo'):
                if expected.get(key):
                    expected[key] += SLIDE
            require(candidate_entries[identifier] == expected, f'Candidate bundle metadata differs: {identifier}')
        passed('roots_closure_and_bundle_metadata', roots=roots, executable_components=sorted(executable),
               codeless_components=sorted(codeless), kernel_interfaces=sorted(interfaces))

        native_exports = np.exports(images['native'], parsed['native'])
        original_exports = np.exports(images['original'], parsed['original'])
        native_by_name = {}
        for address, names in native_exports.items():
            for name in names:
                native_by_name.setdefault(name, set()).add(address)
        for marker in ('_kalloc_sparse_prelink_index_version', '_native_prelink_security_init_version'):
            addresses = native_by_name.get(marker, set())
            require(len(addresses) == 1, f'Native ABI marker is not unique: {marker}')
            at = np.backed_offset(parsed['native']['segments'], next(iter(addresses)), 4, len(images['native']))
            require(struct.unpack_from('<I', images['native'], at)[0] == 1, f'Native ABI marker version differs: {marker}')
        packed = original_meta['_PrelinkLinkKASLROffsets']
        count, = struct.unpack_from('<I', packed)
        require(len(packed) == 4 + count * 4, 'Invalid original pointer table extent')
        offsets = struct.unpack_from(f'<{count}I', packed, 4)
        require(len(set(offsets)) == count, 'Original pointer table has duplicate sites')
        base = next(s['va'] for s in original_segments if s['name'] == '__PRELINK_TEXT')
        bindings = emitted['bindings']
        require(len({b['site'] for b in bindings}) == len(bindings), 'Emitter bindings have duplicate sites')
        actual_bindings, kinds, kernel_targets = [], Counter(), set()
        for offset in offsets:
            site = base + offset
            source = owner(site)
            if source is None or source[2] not in selected:
                continue
            require(source == owner(site + 7), f'Pointer spans module extent at {site:#x}')
            old_at = np.backed_offset(original_segments, site, 8, len(images['original']))
            target, = struct.unpack_from('<Q', images['original'], old_at)
            destination = owner(target)
            if destination:
                require(destination[2] in selected, f'Pointer targets an unselected component at {site:#x}')
                replacement, kind = target + SLIDE, 'component'
            else:
                choices = {new for name in original_exports.get(target, []) for new in native_by_name.get(name, set())}
                require(len(choices) == 1, f'Original kernel target {target:#x} lacks a unique native symbol binding')
                replacement, kind = next(iter(choices)), 'kernel'
                require(any(s['va'] <= replacement < s['va'] + s['size'] for s in parsed['native']['segments']), 'Native symbol binding lies outside mapped segments')
                kernel_targets.add(target)
            expected = {'site': site, 'target': target, 'new_site': site + SLIDE,
                        'new_target': replacement, 'kind': kind, 'module': source[2]}
            actual_bindings.append(expected)
            new_at = np.backed_offset(candidate_segments, site + SLIDE, 8, len(images['candidate']))
            require(struct.unpack_from('<Q', images['candidate'], new_at)[0] == replacement, f'Candidate pointer differs at {site + SLIDE:#x}')
            kinds[kind] += 1
        require(bindings == actual_bindings, 'Emitter bindings differ from independently reconstructed original relocations/native exports')
        require(dict(kinds) == {'kernel': 42532, 'component': 26762} and len(kernel_targets) == 1794, 'Pointer inventory differs from the fixed original-input audit')
        require(emitted['pointer_counts'] == dict(kinds), 'Emitter pointer counts differ from actual bindings')
        rebased_offsets = [b['new_site'] - (base + SLIDE) for b in actual_bindings]
        require(candidate_meta['_PrelinkLinkKASLROffsets'] == struct.pack(f'<{len(rebased_offsets) + 1}I', len(rebased_offsets), *rebased_offsets), 'Candidate KASLR pointer table differs from selected actual bindings')
        passed('pointer_table_native_symbol_bindings_and_candidate_readback', pointer_counts=dict(kinds),
               total=len(actual_bindings), unique_original_kernel_targets=len(kernel_targets))

        text_hashes, entrypoints, headers = {}, {}, []
        for identifier in sorted(executable):
            entry = original_entries[identifier]
            old_header = entry['_PrelinkExecutableLoadAddr']
            headers.append(old_header + SLIDE)
            new_at = np.backed_offset(candidate_segments, old_header + SLIDE, 32, len(images['candidate']))
            new_module = np.macho(images['candidate'], new_at)
            require(new_module['kind'] == 11, f'{identifier}: candidate module is not MH_KEXT_BUNDLE')
            old_module = modules[identifier]
            require(len(old_module['segments']) == len(new_module['segments']), f'{identifier}: module segment count differs')
            for old, new in zip(old_module['segments'], new_module['segments']):
                for key in ('name', 'size', 'fileoff', 'filesize', 'initprot', 'maxprot'):
                    require(old[key] == new[key], f'{identifier}: module segment {key} differs')
                require(new['va'] == (old['va'] + SLIDE if old['va'] else 0), f'{identifier}: segment address was not rebased correctly')
                require(len(old['sections']) == len(new['sections']), f'{identifier}: section count differs')
                for old_section, new_section in zip(old['sections'], new['sections']):
                    for key in ('name', 'size', 'fileoff'):
                        require(old_section[key] == new_section[key], f'{identifier}: section {key} differs')
                    require(new_section['va'] == (old_section['va'] + SLIDE if old_section['va'] else 0), f'{identifier}: section address differs')
                if old['name'] == '__TEXT_EXEC' and old['filesize']:
                    before_at = np.backed_offset(original_segments, old['va'], old['filesize'], len(images['original']))
                    after_at = np.backed_offset(candidate_segments, old['va'] + SLIDE, old['filesize'], len(images['candidate']))
                    before = images['original'][before_at:before_at + old['filesize']]
                    require(images['candidate'][after_at:after_at + old['filesize']] == before, f'{identifier}: original executable bytes changed')
                    text_hashes[identifier] = digest(before)
            kmod = entry['_PrelinkKmodInfo']
            old_at = np.backed_offset(original_segments, kmod, 196, len(images['original']))
            new_at = np.backed_offset(candidate_segments, kmod + SLIDE, 196, len(images['candidate']))
            original_address, = struct.unpack_from('<Q', images['original'], old_at + 156)
            require(original_address in (0, old_header), f'{identifier}: original kmod address disagrees with module header')
            require(struct.unpack_from('<Q', images['candidate'], new_at + 156)[0] == (original_address + SLIDE if original_address else 0), f'{identifier}: candidate kmod address differs')
            start, stop = struct.unpack_from('<QQ', images['original'], old_at + 180)
            expected_start, expected_stop = start + SLIDE if start else 0, stop + SLIDE if stop else 0
            require(struct.unpack_from('<QQ', images['candidate'], new_at + 180) == (expected_start, expected_stop), f'{identifier}: candidate module entrypoints differ')
            entrypoints[identifier] = {'kmod_info_va': kmod + SLIDE, 'start_va': expected_start, 'stop_va': expected_stop}
        require(emitted['original_executable_sha256'] == text_hashes, 'Emitter executable hashes differ from actual original/candidate ranges')
        require(emitted['module_entrypoints'] == entrypoints, 'Emitter module entrypoints differ from actual metadata')
        expected_extents = [{'module': identifier, 'segment': name, 'va': start + SLIDE, 'size': end - start}
                            for start, end, identifier, name in owned if identifier in selected]
        require(emitted['extents'] == expected_extents, 'Emitter selected extents differ from original module segments')
        passed('module_headers_entrypoints_and_original_executable_bytes', executable_hashes=text_hashes, modules=len(executable))

        headers.sort()
        prelink_text = next(s for s in candidate_segments if s['name'] == '__PRELINK_TEXT')
        index_sections = [s for s in prelink_text['sections'] if s['name'] == '__macho_list']
        require(len(index_sections) == 1, 'Candidate lacks a unique sparse header index section')
        index_section = index_sections[0]
        require(index_section['size'] == len(headers) * 8, 'Sparse index size differs from executable component count')
        index_at = np.backed_offset(candidate_segments, index_section['va'], index_section['size'], len(images['candidate']))
        require(index_at == index_section['fileoff'], 'Sparse index section file offset differs from mapping')
        require(list(struct.unpack_from(f'<{len(headers)}Q', images['candidate'], index_at)) == headers, 'Sparse index bytes differ from actual selected module headers')
        expected_index = {'va': index_section['va'], 'size': index_section['size'], 'fileoff': index_at, 'headers': headers, 'abi': 1}
        require(emitted['sparse_index'] == expected_index, 'Emitter sparse index differs from candidate section')
        require(emitted['early_security_initializers'] == {AMFI: AMFI_ORIGINAL + SLIDE}, 'Emitter AMFI initializer differs from selected input')
        require(original_entries[AMFI].get('AppleSecurityExtension') is True, 'AMFI is not marked as a security extension')
        require(any(s['name'] == '__TEXT_EXEC' and s['initprot'] == 5 and not s['maxprot'] & 2
                    and s['va'] <= AMFI_ORIGINAL and AMFI_ORIGINAL + 4 <= s['va'] + s['filesize']
                    for s in modules[AMFI]['segments']), 'AMFI initializer does not lie in original readonly executable bytes')
        require(candidate_meta['_PrelinkKCID'] == hashlib.sha256(images['native'] + images['original']).digest()[:16], 'Candidate KCID differs from actual input pair')
        for item in emitted['coalesced_segments']:
            matches = [s for s in candidate_segments if s['name'] == item['name']]
            require(len(matches) == 1, 'Report coalesced segment is not unique in candidate')
            segment = matches[0]
            require(all(item[key] == segment[key] for key in ('name', 'va', 'size', 'filesize', 'fileoff')), 'Reported coalesced segment differs from candidate')
            require(segment['va'] % 4096 == segment['size'] % 4096 == segment['fileoff'] % 4096 == 0, 'Coalesced segment is not 4 KiB aligned')
        require({s['name'] for s in emitted['coalesced_segments']} == {'__PRELINK_TEXT', '__PLK_TEXT_EXEC', '__PLK_DATA_CONST', '__PRELINK_DATA', '__PRELINK_INFO'}, 'Unexpected emitted coalesced segment set')
        passed('sparse_index_security_initializer_kcid_and_coalesced_geometry', sparse_index_abi=1,
               sparse_headers=len(headers), amfi_candidate_address=hex(AMFI_ORIGINAL + SLIDE))

        for item in emitted['known_inline_data']:
            require(item['module'] in executable, 'Inline-data inventory names an unselected executable component')
            before_at = np.backed_offset(original_segments, item['va'], item['size'], len(images['original']))
            after_at = np.backed_offset(candidate_segments, item['va'] + SLIDE, item['size'], len(images['candidate']))
            before = images['original'][before_at:before_at + item['size']]
            require(digest(before) == item['sha256'] and images['candidate'][after_at:after_at + item['size']] == before, 'Recognized inline data differs from original/candidate bytes')
        require(type(emitted['pc_relative_encodings_preserved']) is int and emitted['pc_relative_encodings_preserved'] > 0, 'Missing emitter PC-relative inventory count')
        for flag in FALSE_FLAGS:
            require(emitted[flag] is False, f'Unexpected qualification/modification marker: {flag}')
        record['qualification_markers'] = {flag: emitted[flag] for flag in FALSE_FLAGS}
        record['limitations'] = [
            'DT panic-region effective configuration and pe_init.o arguments are checked. UFS adapter init and caller machine code are independently checked; hardware execution remains unverified.',
            'Stack budget covers adapter functions and explicitly modeled internal callbacks; external XNU/IOKit methods and interrupt nesting are outside this bound.',
            'No runtime ABI, C++ vtable/structure/call compatibility, register dataflow, hardware handoff or boot qualification.',
            'The emitter PC-relative inventory count is recorded, not independently re-decoded by this verifier.',
            'The ordinary ARM64 format has no missing/stubbed fields; actual selected pointer sites and unique native export bindings were verified instead.',
            'AMFI initializer range and native version markers do not prove runtime policy behavior; code preservation does not prove AES request semantics.',
        ]
        passed('qualification_boundaries_and_inline_data', known_inline_data_ranges=len(emitted['known_inline_data']),
               emitter_pc_relative_encodings_preserved=emitted['pc_relative_encodings_preserved'])
        record['adapter'] = verify_adapter(root, out, tree, device, native_path, np, images, parsed, passed)
        for name, path in paths.items():
            require(digest(path.read_bytes()) == hashes[name], f'{name} changed while validation was running')
        for name, path in (('status', status_path), ('emitter_report', emitted_report_path)):
            require(digest(path.read_bytes()) == record['inputs'][name]['sha256'], f'{name} changed during validation')
        for path, before in ((config_path, config_check['current_sha256']),
                             (baseline_config, config_check['baseline_sha256']),
                             (pe_record, compilation_check['sha256']),
                             (mode_stamp, record['dt_panic_region']['mode_stamp']['sha256']),
                             (Path(compilation_check['object']), compilation_check['object_sha256']),
                             (Path(compilation_check['source']), compilation_check['source_sha256'])):
            require(digest(path.read_bytes()) == before, f'{path.name} changed during validation')
        passed('inputs_unchanged_during_verification')
        record['status'] = 'passed'
    except Exception as exc:
        record['status'] = 'failed'
        record['error'] = {'type': type(exc).__name__, 'message': str(exc)}
        raise
    finally:
        record['finished_utc'] = utc()
        report_path.parent.mkdir(parents=True, exist_ok=True)
        with report_path.open('x', encoding='utf-8') as stream:
            stream.write(json.dumps(record, indent=2) + '\n')
        print(f'Host candidate verification {record["status"]}: {report_path}', file=sys.stderr)


if __name__ == '__main__':
    main()
