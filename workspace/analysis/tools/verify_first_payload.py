#!/usr/bin/env python3
"""Read back the first TB-J716F payload without building or changing its inputs.

Only a fresh --report and automatically removed temporary extraction files are
written. This verifies host bytes; it does not qualify memory ownership, ABI,
the display, input handling, bootloader relocation or execution on the tablet.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import tomllib

sys.dont_write_bytecode = True
PROFILE = 'ipados-17.7.11-21H461'
KERNEL_SHA = '62215d029ca1b96f5c140abf2fb435e97da8b3ae3574ef10b568a0b8a980f3e8'
MIB = 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def json_read(path):
    def unique(items):
        result = {}
        for key, value in items:
            require(key not in result, 'Duplicate JSON key')
            result[key] = value
        return result
    return json.loads(path.read_text(encoding='utf-8'), object_pairs_hook=unique)


def adt_read(blob):
    """Independent bounded Apple DT decoder, including lengths and padding."""
    nodes = {}
    cursor = 0

    def take(count):
        nonlocal cursor
        require(0 <= count <= len(blob) - cursor, 'ADT extends past its buffer')
        result = blob[cursor:cursor + count]
        cursor += count
        return result

    def node(parent, depth):
        require(depth <= 32 and len(nodes) < 256, 'ADT nesting/node count exceeds bounds')
        props_count, child_count = struct.unpack('<II', take(8))
        require(1 <= props_count <= 512 and child_count <= 128, 'Invalid ADT node counts')
        properties = {}
        for _ in range(props_count):
            raw_key, length = struct.unpack('<32sI', take(36))
            require(b'\0' in raw_key and length <= 1024 * 1024, 'Invalid ADT property header')
            end = raw_key.index(b'\0')
            require(end and not any(raw_key[end:]), 'Invalid ADT property name padding')
            key = raw_key[:end].decode('ascii')
            require(key not in properties, 'Duplicate ADT property')
            properties[key] = take(length)
            require(not any(take(-length % 4)), 'Nonzero ADT property padding')
        raw_name = properties.get('name', b'')
        require(raw_name.endswith(b'\0') and b'\0' not in raw_name[:-1], 'Invalid ADT node name')
        name = raw_name[:-1].decode('ascii')
        require(name and '/' not in name, 'Invalid ADT path component')
        if depth == 0:
            require(name == 'device-tree', 'Unexpected ADT root')
            path = '/'
        else:
            path = parent.rstrip('/') + '/' + name
        require(path not in nodes, 'Duplicate ADT node path')
        nodes[path] = properties
        for _ in range(child_count):
            node(path, depth + 1)
    node('', 0)
    require(cursor == len(blob), 'Trailing bytes after ADT')
    return nodes


def scalar(properties, key, width):
    raw = properties.get(key, b'')
    require(len(raw) == width, f'ADT {key} has an invalid width')
    return int.from_bytes(raw, 'little')


def macho_read(data):
    """Only the ordinary ARM64 kernel format used by this fixed candidate."""
    require(len(data) >= 32, 'Truncated Mach-O header')
    magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
    require((magic, cpu, subtype, kind) == (0xfeedfacf, 0x100000c, 0, 2), 'Unexpected Mach-O identity')
    limit, cursor = 32 + size, 32
    require(limit <= len(data) and count <= size // 8, 'Invalid Mach-O command extent')
    segments, sections, symbols, entries = [], {}, {}, []
    symtab = None
    for _ in range(count):
        require(cursor + 8 <= limit, 'Truncated load command')
        cmd, length = struct.unpack_from('<II', data, cursor)
        require(length >= 8 and length % 4 == 0 and cursor + length <= limit, 'Invalid load command size')
        if cmd == 0x19:
            require(length >= 72, 'Truncated segment')
            raw_name, address, memory_size, offset, file_size = struct.unpack_from('<16s4Q', data, cursor + 8)
            nsects, = struct.unpack_from('<I', data, cursor + 64)
            require(72 + nsects * 80 <= length and offset + file_size <= len(data), 'Segment/section exceeds buffer')
            segments.append((address, memory_size, offset, file_size))
            for index in range(nsects):
                at = cursor + 72 + index * 80
                sn, gn, va, sz, pos = struct.unpack_from('<16s16sQQI', data, at)
                key = (gn.rstrip(b'\0'), sn.rstrip(b'\0'))
                require(key not in sections, 'Duplicate Mach-O section')
                sections[key] = (va, sz, pos)
        elif cmd == 0x2:
            require(length == 24 and symtab is None, 'Invalid/duplicate symbol table')
            symtab = struct.unpack_from('<4I', data, cursor + 8)
        elif cmd == 0x5:
            require(length >= 280, 'Truncated ARM64 thread state')
            flavor, words = struct.unpack_from('<II', data, cursor + 8)
            require(flavor == 6 and words == 68 and length >= 16 + words * 4, 'Unexpected ARM64 thread state')
            entries.append(struct.unpack_from('<Q', data, cursor + 16 + 32 * 8)[0])
        cursor += length
    require(cursor == limit and len(entries) == 1 and symtab is not None, 'Missing or inconsistent Mach-O metadata')
    so, ns, st, ss = symtab
    require(so + ns * 16 <= len(data) and st + ss <= len(data), 'Symbol table extends beyond image')
    wanted = {'_start_first_cpu', '_panic', '_panic_trap_to_debugger', '_vinfo', '_boot_progress_panic_vinfo'}
    for index in range(ns):
        at, typ, _, _, value = struct.unpack_from('<IBBHQ', data, so + index * 16)
        require(at < ss, 'Symbol name index outside string table')
        end = data.find(b'\0', st + at, st + ss)
        require(end >= 0, 'Unterminated symbol')
        name = data[st + at:end].decode('utf-8', errors='strict')
        if name in wanted and typ & 0x0e == 0x0e:
            require(name not in symbols or symbols[name] == value, 'Ambiguous diagnostic symbol')
            symbols[name] = value
    return {'segments': segments, 'sections': sections, 'symbols': symbols, 'entry': entries[0], 'commands_end': limit}


def file_offset(image, address, length=4):
    matches = [pos + address - va for va, size, pos, backed in image['segments']
               if va <= address and address + length <= va + min(size, backed)]
    require(len(matches) == 1, 'Diagnostic range lacks one file-backed mapping')
    return matches[0]


def branch(source, destination):
    delta = destination - source
    require(delta % 4 == 0 and -(1 << 27) <= delta < (1 << 27), 'Probe branch is out of range')
    return 0x14000000 | ((delta // 4) & 0x3ffffff)


def branch_target(word, source):
    require(word & 0xfc000000 == 0x14000000, 'Diagnostic site is not an unconditional branch')
    delta = word & 0x3ffffff
    if delta & 0x2000000:
        delta -= 0x4000000
    return source + delta * 4


def probe_readback(original, patched):
    require(len(original) == len(patched) and original != patched, 'Expected a same-size diagnostic kernel copy')
    require(b'_source_corecrypto14_bootstrap\0' not in original, 'This fixed candidate unexpectedly requires POST resealing')
    image = macho_read(original)
    require(macho_read(patched) == image, 'Diagnostic patch changed kernel metadata')
    entry, names = image['entry'], image['symbols']
    for key in ('_start_first_cpu', '_panic', '_panic_trap_to_debugger', '_vinfo'):
        require(key in names, f'Missing probe symbol {key}')
    first = names['_start_first_cpu']

    def word(data, address):
        return struct.unpack_from('<I', data, file_offset(image, address))[0]

    matches = [va for va in range(first, first + 0x1000, 4)
               if word(original, va) == 0xd5181000 and word(original, va + 4) == 0xd5033fdf
               and word(original, va + 0x20) == 0xd2a00600 and word(original, va + 0x24) == 0xd5181040]
    require(len(matches) == 1, 'MMU diagnostic site is not unique')
    mmu = matches[0]
    cave1, cave2 = entry + 0x78, entry + 0xd8
    panic_start, panic_limit = entry + 0x138, entry + 0xb68
    require(word(original, entry) == branch(entry, first), 'Unexpected original entry instruction')
    require(word(patched, entry) == branch(entry, cave1), 'Stage one branch differs')
    require(word(patched, mmu) == branch(mmu, cave2), 'Stage two branch differs')
    require(word(patched, mmu + 0x20) == 0xd2a00600, 'No-reset configuration changed the post-MMU reset site')
    cave_offset = file_offset(image, cave1, panic_limit - cave1)
    cave_end = cave_offset + panic_limit - cave1
    require(not any(original[cave_offset:cave_end]), 'Probe injection overwrites original non-padding bytes')
    for start, end, target, prefix in ((cave1, cave2, first, 0xf9401401),
                                       (cave2, panic_start, mmu + 4, 0xf9401681)):
        require(word(patched, start) == prefix, 'Probe does not load the intended boot_args framebuffer')
        body = patched[file_offset(image, start):file_offset(image, end)]
        branches = [(start + index, value) for index in range(0, len(body), 4)
                    if (value := struct.unpack_from('<I', body, index)[0]) & 0xfc000000 == 0x14000000]
        require(len(branches) == 1 and branch_target(branches[0][1], branches[0][0]) == target, 'Stage cave has an unexpected return branch')
        if start == cave2:
            require(word(patched, branches[0][0] - 4) == 0xd5181000, 'Stage two does not restore the displaced SCTLR write')
        require(not any(body[branches[0][0] - start + 4:]), 'Unexpected bytes after stage return')
    panic_entry = branch_target(word(patched, names['_panic']), names['_panic'])
    common = branch_target(word(patched, names['_panic_trap_to_debugger']), names['_panic_trap_to_debugger'])
    require(panic_start <= panic_entry < panic_limit and panic_entry <= common < panic_limit - 8, 'Panic renderer entry is outside padding')
    require(word(patched, common) == 0xf9400021, 'Panic common entry does not dereference Darwin va_list')
    common_main = branch_target(word(patched, common + 4), common + 4)
    require(panic_entry <= common_main < panic_limit, 'Panic common entry leaves the renderer')
    ranges = [(file_offset(image, va), file_offset(image, va) + 4)
              for va in (entry, mmu, names['_panic'], names['_panic_trap_to_debugger'])]
    ranges.append((cave_offset, cave_end))
    ranges.sort()
    cursor, changed = 0, 0
    for start, end in ranges:
        require(start >= cursor, 'Diagnostic patch ranges overlap')
        require(original[cursor:start] == patched[cursor:start], 'Kernel changed outside permitted diagnostic ranges')
        changed += sum(a != b for a, b in zip(original[start:end], patched[start:end]))
        cursor = end
    require(original[cursor:] == patched[cursor:], 'Kernel tail changed outside diagnostics')
    return {'changed_bytes': changed, 'entry': hex(entry), 'mmu_site': hex(mmu),
            'panic_entry': hex(panic_entry), 'common_panic_entry': hex(common),
            'patch_file_ranges': [[hex(a), hex(b)] for a, b in ranges],
            'injected_region_sha256': sha(patched[cave_offset:cave_end]),
            'reset_probe': False, 'post_reseal_required': False,
            'scope': 'Branch targets, required instructions, original zero padding and all changes bounded; renderer behavior requires hardware.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    root, report = args.root.resolve(), args.report.resolve()
    repo = root / 'UniXNU'
    out = repo / 'out/j716f' / PROFILE / 'j716f/payload'
    paths = {'image': out / 'payload.img', 'payload_report': out / 'payload.json',
             'original_kernel': root / 'artifacts/j716f-host-kernel-20261112/kernel.macho',
             'board': root / 'work/UniXNU-j716f/board.toml',
             'uefi_config': root / 'work/uefi-j716f/Resources/Configs/j716f.toml'}
    require(not report.exists(), 'Refusing to overwrite a prior report')
    require(not report.is_relative_to(repo) and report not in paths.values(), 'Report must be outside checkout and inputs')
    record = {'schema_version': 1, 'started_utc': utc(), 'status': 'running', 'checks': [],
              'device_actions': False, 'hardware_tested': False, 'runtime_qualified': False,
              'build_inputs_modified': False, 'scope': 'Static host-only payload readback; not a bootable-device qualification'}

    def passed(name, **details):
        record['checks'].append({'name': name, 'passed': True, **details})

    try:
        inputs = {name: path.read_bytes() for name, path in paths.items()}
        hashes = {name: sha(data) for name, data in inputs.items()}
        record['inputs'] = {name: {'path': str(paths[name]), 'sha256': hashes[name], 'bytes': len(data)}
                            for name, data in inputs.items()}
        require(hashes['original_kernel'] == KERNEL_SHA, 'Original validated kernel hash changed')
        official = json_read(paths['payload_report'])
        board = tomllib.loads(inputs['board'].decode())
        payload = board['payload']
        require(payload.get('panic_screen') is True and not payload.get('entry_probe_reset_delay'), 'Unexpected probe configuration')
        require(payload.get('timeout') == -1 and payload.get('tools') is True and not payload.get('install'), 'Unexpected payload menu scope')
        require(official['kernel_sha256'] == KERNEL_SHA and official['payload_sha256'] == hashes['image'], 'Payload report input/image hash mismatch')
        require(official['payload_bytes'] == len(inputs['image']) <= 80 * MIB, 'FAT image exceeds diagnostic budget')
        require(official['panic_screen'] is True and official['hardware_tested'] is False and official['skip_setup'] is False, 'Unexpected report qualification/options')
        for name in ('trust_cache_sha256', 'data_ramdisk_sha256', 'gpu_addon_sha256'):
            require(official[name] is None, 'Unexpected optional payload input')
        require(len(inputs['image']) == int(payload['size_mib']) * MIB, 'FAT size differs from board budget')
        passed('fixed_input_identity_and_payload_report')
        env = dict(os.environ, MTOOLS_SKIP_CHECK='1', TZ='UTC', LC_ALL='C')
        listing = subprocess.check_output(['mdir', '-i', str(paths['image']), '-/', '::'], env=env, text=True)
        record['fat_listing'] = listing
        with tempfile.TemporaryDirectory(prefix='unixnu-payload-readback-') as directory:
            extracted = Path(directory)
            subprocess.run(['mcopy', '-i', str(paths['image']), '-s', '::/EFI', str(extracted)], env=env, check=True, capture_output=True)
            actual_files = {path.relative_to(extracted).as_posix(): path.read_bytes()
                            for path in extracted.rglob('*') if path.is_file()}
            expected_files = {'EFI/UniXNU/unixnu.toml', 'EFI/UniXNU/Payloads/kernel.macho', 'EFI/UniXNU/Payloads/kernel.macho.adt'}
            require(set(actual_files) == expected_files, 'FAT has unexpected or missing files under EFI')
            # A root listing independently rejects files outside the only copied directory.
            root_listing = subprocess.check_output(['mdir', '-b', '-i', str(paths['image']), '::/'], env=env, text=True)
            require([s.strip().rstrip('/') for s in root_listing.splitlines() if s.strip()] == ['::/EFI'], 'FAT root contains an unexpected entry')
            kernel = actual_files['EFI/UniXNU/Payloads/kernel.macho']
            adt = actual_files['EFI/UniXNU/Payloads/kernel.macho.adt']
            require(sha(kernel) == official['payload_kernel_sha256'] and sha(adt) == official['adt_sha256'], 'Extracted FAT bytes differ from report')
            record['fat_files'] = {name: {'bytes': len(data), 'sha256': sha(data)} for name, data in actual_files.items()}
            passed('fat_inventory_and_extracted_hashes')
            config = tomllib.loads(actual_files['EFI/UniXNU/unixnu.toml'].decode())
            require(set(config) == {'version', 'boot', 'kits', 'entry'}, 'Unexpected runtime configuration tables')
            require(config['version'] == 1 and config['boot'] == {'timeout': -1, 'default': 'xnu'} and config['kits'] == {'autoload': False}, 'Unexpected menu policy')
            entries = config['entry']
            require([e['id'] for e in entries] == ['xnu', 'reset', 'shutdown', 'tools'], 'Unexpected runtime menu entry')
            xnu = entries[0]
            require(set(xnu) == {'id', 'title', 'type', 'path', 'args'} and xnu['type'] == 'xnu' and xnu['path'] == 'Payloads/kernel.macho', 'Unexpected XNU entry')
            require(xnu['args'] == official['kernel_args'] == ' '.join(payload['kernel_args']), 'Runtime kernel arguments differ')
            for argument in xnu['args'].split():
                require(argument.split('=', 1)[0].lstrip('-') not in {'rd', 'root', 'native-ufs-root', 'data-persistence', 'install', 'reset-data', 'data-reset'}, 'Unexpected root or persistence boot argument')
            for item, action in zip(entries[1:], ('reset', 'shutdown', 'tools')):
                require(set(item) == {'id', 'title', 'type', 'action'} and item['type'] == 'action' and item['action'] == action, 'Unexpected action entry')
            passed('runtime_menu_and_boot_arguments', timeout=-1, tools_present=True, data_install_persistence=False,
                   caveat='Menu.c may boot the default entry through its no-input fallback; timeout=-1 does not guarantee an indefinite wait without working input.')
            passed('kernel_copy_diagnostic_changes', **probe_readback(inputs['original_kernel'], kernel))
            nodes = adt_read(adt)
            required = {'/', '/chosen', '/chosen/memory-map', '/chosen/manifest-properties', '/cpus', '/cpus/cpu0', '/arm-io', '/arm-io/timer', '/arm-io/intc', '/defaults', '/options', '/product'}
            require(required <= nodes.keys(), 'Missing required ADT nodes')
            chosen = nodes['/chosen']
            require(scalar(chosen, 'unique-chip-id', 8) != 0 and scalar(chosen, 'chip-id', 4) == 0x8011, 'Missing/invalid local platform identity')
            require(len(chosen.get('random-seed', b'')) == 256 and any(chosen['random-seed']), 'Invalid random seed')
            require(scalar(chosen, 'dram-base', 8) == 0xc0000000 and scalar(chosen, 'dram-size', 8) == 0x140000000, 'Unexpected DRAM inventory')
            require(scalar(chosen, 'darwinboot-memory-size', 8) == 256 * MIB, 'Unexpected requested owned window')
            for key in ('darwinboot-memory-excluded', 'darwinboot-memory-outside', 'darwinboot-memory-outside-kept', 'root-matching', 'ephemeral-storage'):
                require(key not in chosen, 'Unexpected exclusion, outside memory, or root/Data handoff')
            loader = chosen.get('darwinboot-loader-inputs')
            require(loader is None or (len(loader) == 64 and not any(loader)), 'Invalid loader-input placeholder')
            require(set(nodes['/chosen/memory-map']) == {'name'}, 'Unexpected Data/trust/addon memory-map placeholder')
            require([p for p in nodes if p.startswith('/cpus/')] == ['/cpus/cpu0'] and '/psci' not in nodes, 'Expected single boot CPU')
            cpu = nodes['/cpus/cpu0']
            require(scalar(cpu, 'reg', 4) == 0 and cpu.get('state') == b'running\0' and cpu.get('compatible') == b'arm,cortex-a55\0', 'Unexpected boot CPU')
            require(scalar(cpu, 'timebase-frequency', 4) == 19200000, 'Incorrect CPU timebase')
            timer, gic = nodes['/arm-io/timer'], nodes['/arm-io/intc']
            require(scalar(timer, 'clock-frequency', 4) == 19200000 and timer.get('compatible') == b'arm,armv8-timer\0', 'Incorrect timer')
            require(timer.get('interrupts') == struct.pack('<12I', 1, 13, 0xff08, 1, 14, 0xff08, 1, 11, 0xff08, 1, 12, 0xff08), 'Incorrect timer PPIs')
            require('reg' not in timer and gic.get('compatible') == b'arm,gic-v3\0', 'Incorrect timer/GIC register provider')
            require(scalar(gic, 'redistributor-stride', 8) == 0x20000 and scalar(gic, '#interrupt-cells', 4) == 3 and gic.get('interrupt-controller') == b'', 'Incorrect GIC controller properties')
            require(scalar(timer, 'interrupt-parent', 4) == scalar(gic, 'phandle', 4) != 0, 'Timer interrupt provider does not resolve')
            require(nodes['/arm-io'].get('ranges') == struct.pack('<3Q', 0, 0x17000000, 0x1000000), 'Unexpected arm-io window')
            require(gic.get('reg') == struct.pack('<4Q', 0xa00000, 0x10000, 0xa60000, 0x100000), 'Incorrect GIC physical register ranges')
            require(nodes['/'].get('model') == b'iPad7,1\0' and nodes['/'].get('compatible') == b'J120AP\0lenovo,j716f\0qcom,sm8250\0', 'Incorrect OS/board identity')
            passed('adt_structure_identity_memory_and_soc', node_paths=sorted(nodes), identity_nonzero=True, identity_value_disclosed=False,
                   cpu_count=1, timebase_hz=19200000, requested_owned_bytes=256 * MIB,
                   actual_memory_ownership_verified=False, memory_map_placeholders=[], loader_input_placeholder=loader is not None)
        config = tomllib.loads(inputs['uefi_config'].decode())
        header = config['boot_image']
        base = int(header.get('base', 0))
        start, dtb = base + int(header['ramdisk_offset']), base + int(header['dtb_offset'])
        image_end = start + len(inputs['image'])
        passed('separate_file_size_and_nominal_header_analysis', payload_bytes=len(inputs['image']),
               boot_partition_budget_bytes=96 * MIB, remaining_for_other_packed_bytes=96 * MIB - len(inputs['image']),
               packed_bootimg_size_verified=False,
               nominal_ramdisk_range=[hex(start), hex(image_end)], nominal_dtb_start=hex(dtb),
               nominal_dtb_start_inside_ramdisk=start <= dtb < image_end,
               physical_load_collision_proven=False,
               explanation='Legacy header arithmetic is not evidence of actual ABL physical placement. Firmware/shim/DTB/page padding still consume the separate 96 MiB file budget; no combined boot image is certified.')
        for name, path in paths.items():
            require(sha(path.read_bytes()) == hashes[name], f'Input changed during verification: {name}')
        passed('all_source_inputs_unchanged')
        record['limitations'] = [
            'No device commands or boot image build; no runtime ABI, display, input, memory allocation or kernel execution validation.',
            'The 256 MiB owned window is a loader request, not a measured available or allocated EFI range.',
            'Panic injection validation checks bounded modifications and control transfers, not full renderer semantics.',
            'A valid FAT and matched hashes do not establish bootability; actual ABL relocation and EFI memory map remain unknown.',
        ]
        record['status'] = 'passed'
    except Exception as exc:
        record['status'] = 'failed'
        record['error'] = {'type': type(exc).__name__, 'message': str(exc)}
        raise
    finally:
        record['finished_utc'] = utc()
        report.parent.mkdir(parents=True, exist_ok=True)
        with report.open('x', encoding='utf-8') as stream:
            stream.write(json.dumps(record, indent=2) + '\n')
        print(f'First payload verification {record["status"]}: {report}', file=sys.stderr)


if __name__ == '__main__':
    main()
