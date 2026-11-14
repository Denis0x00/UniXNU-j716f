"""Bind the isolated Goodix core to local ROM/DT/reference evidence.

Reads only saved files. No shell, device, VM, firmware execution, or hardware IO.
Writes dt-evidence.json and source-manifest.json beside this script.
"""
from pathlib import Path
import hashlib
import json
import struct

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent


def require(value, reason):
    if not value:
        raise ValueError(reason)


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        while data := stream.read(1024 * 1024):
            h.update(data)
    return h.hexdigest()


def record(relative):
    path = ROOT / relative
    return {'path': relative, 'bytes': path.stat().st_size, 'sha256': digest(path)}


def fdt_nodes(data):
    require(len(data) >= 40, 'FDT short header')
    magic, total, off_struct, off_strings, _, version, _, _, size_strings, size_struct = struct.unpack_from('>10I', data)
    require(magic == 0xd00dfeed and version >= 17, 'FDT identity')
    require(total <= len(data) and off_strings + size_strings <= total and off_struct + size_struct <= total, 'FDT bounds')
    strings = data[off_strings:off_strings + size_strings]
    pos, end = off_struct, off_struct + size_struct
    nodes, stack = {}, []
    while pos + 4 <= end:
        token, = struct.unpack_from('>I', data, pos)
        pos += 4
        if token == 1:
            stop = data.find(b'\0', pos, end)
            require(stop >= pos, 'FDT node name')
            stack.append(data[pos:stop].decode('ascii'))
            nodes['/' + '/'.join(stack[1:])] = {}
            pos = (stop + 4) & ~3
        elif token == 2:
            require(bool(stack), 'FDT unbalanced end node')
            stack.pop()
        elif token == 3:
            require(pos + 8 <= end and bool(stack), 'FDT short property')
            length, name_offset = struct.unpack_from('>II', data, pos)
            pos += 8
            require(pos + length <= end and name_offset < len(strings), 'FDT property bounds')
            stop = strings.find(b'\0', name_offset)
            require(stop >= name_offset, 'FDT property name')
            name = strings[name_offset:stop].decode('ascii')
            nodes['/' + '/'.join(stack[1:])][name] = data[pos:pos + length]
            pos = (pos + length + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            require(not stack, 'FDT unbalanced end')
            return nodes
        else:
            raise ValueError('FDT unknown token')
    raise ValueError('FDT missing end')


def show_properties(node):
    strings = {'compatible', 'goodix,vdd-name', 'goodix,avdd-name', 'goodix,vddio-name', 'pinctrl-names', 'status'}
    result = {}
    for name, raw in node.items():
        if name in strings:
            value = raw.rstrip(b'\0').decode('ascii').split('\0')
        elif not raw:
            value = True
        elif len(raw) % 4 == 0:
            value = [hex(v) for v in struct.unpack('>' + 'I' * (len(raw) // 4), raw)]
        else:
            value = {'raw_hex': raw.hex()}
        result[name] = value
    return result


def main():
    dtbo_path = ROOT / 'TB_J716F_ZUI_15.0.485/image/dtbo.img'
    data = dtbo_path.read_bytes()
    magic, total, header_size, entry_size, count, entries, page_size, version = struct.unpack_from('>8I', data)
    require((magic, header_size, entry_size, count, version) == (0xd7b7ab1e, 32, 32, 11, 0), 'DTBO identity')
    require(total <= len(data) and entries + count * entry_size <= total, 'DTBO bounds')
    size, offset, board_id, revision, *custom = struct.unpack_from('>8I', data, entries + entry_size)
    require(offset + size <= total, 'overlay bounds')
    overlay = data[offset:offset + size]
    saved = ROOT / 'analysis/firmware/dtbo-overlay-01.dtbo'
    require(overlay == saved.read_bytes(), 'saved overlay differs from stock DTBO entry1')
    opath = '/fragment@71/__overlay__/goodix-gt738x@5d'
    rpath = '/soc/i2c@a94000/goodix-gt738x@5d'
    original = fdt_nodes(overlay)[opath]
    running = fdt_nodes((ROOT / 'analysis/device/20261111-231708/running.dtb').read_bytes())[rpath]
    names = ['reg', 'goodix,panel-max-x', 'goodix,panel-max-y', 'goodix,panel-max-w', 'goodix,swap-axis', 'goodix,x2x', 'goodix,irq-flags']
    require(all(original[n] == running[n] for n in names), 'stock/runtime touch property mismatch')
    require(original['reg'] == struct.pack('>I', 0x5d), 'touch address')
    require(original['goodix,panel-max-x'] == struct.pack('>I', 2560), 'panel x')
    require(original['goodix,panel-max-y'] == struct.pack('>I', 1600), 'panel y')
    for name, pin in [('goodix,reset-gpio', 38), ('goodix,irq-gpio', 39)]:
        require(original[name][4:] == running[name][4:] and struct.unpack_from('>I', original[name], 4)[0] == pin, 'GPIO mismatch')
    evidence = {
        'scope': 'Saved original ROM DTBO and saved running Android DT only; no hardware actions.',
        'dtbo': record('TB_J716F_ZUI_15.0.485/image/dtbo.img'),
        'entry_index': 1,
        'entry': {'bytes': size, 'offset': offset, 'id': board_id, 'revision': revision, 'custom': custom, 'page_size': page_size},
        'overlay': record('analysis/firmware/dtbo-overlay-01.dtbo'),
        'running_dtb': record('analysis/device/20261111-231708/running.dtb'),
        'original_node': {'path': opath, 'properties': show_properties(original)},
        'running_node': {'path': rpath, 'properties': show_properties(running)},
        'matched_value_properties': names,
        'matched_gpio_specifiers_except_controller_phandle': ['goodix,reset-gpio', 'goodix,irq-gpio'],
        'limits': [
            'Overlay controller/supply phandles are unresolved 0xffffffff until fixups; never use them as runtime handles.',
            'DT dimensions and swap/x2x flags are proven values, not qualified raw-packet endpoint/orientation semantics.',
            'Core preserves raw axes and accepts inclusive X<=2560,Y<=1600 as a conservative policy; no display transform.',
            'panel-max-w=2368 is recorded, not imposed as a decoded width validity limit.',
            'Saved Android identity/DT do not prove current controller power or bus readiness.'
        ]
    }
    (OUT / 'dt-evidence.json').write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    paths = [
        'work/J716fGoodix-20261113/J716fGoodix.h',
        'work/J716fGoodix-20261113/J716fGoodix.c',
        'work/J716fGoodix-20261113/README.md',
        'analysis/stock-goodix-20261113/test_goodix.c',
        'analysis/stock-goodix-20261113/audit_stock.py',
        'analysis/stock-goodix-20261113/collect_provenance.py',
        'analysis/stock-goodix-20261113/README.md',
        'analysis/stock-goodix-20261113/stock-protocol-evidence.json',
        'analysis/stock-goodix-20261113/stock-protocol-disassembly.json',
        'analysis/stock-goodix-20261113/dt-evidence.json',
        'analysis/stock-goodix-20261113/stock/goodix_cfg_group.bin',
        'analysis/stock-goodix-20261113/stock/goodix_firmware.bin',
        'analysis/device/20261111-231708/chip-details.txt',
        'analysis/device/20261111-231708/touch-discovery.txt',
        'analysis/device/20261111-231708/running.dtb',
        'analysis/firmware/dtbo-overlay-01.dtbo',
        'TB_J716F_ZUI_15.0.485/image/dtbo.img',
        'TB_J716F_ZUI_15.0.485/image/boot.img',
        'TB_J716F_ZUI_15.0.485/image/rawprogram0_unsparse.xml',
        'TB_J716F_ZUI_15.0.485/image/super_1.img',
        'TB_J716F_ZUI_15.0.485/image/super_6.img',
        'tools/e2fsprogs/debugfs.exe',
    ]
    references = json.loads((ROOT / 'analysis/driver-adaptation-20261113/input-source-files.json').read_text())
    pinned = []
    for source in references['source_files']:
        if '/google-gt738x/' in source['path']:
            current = record(source['path'])
            require(current['sha256'] == source['sha256'], 'reference hash drift')
            current['url'] = source['url']
            current['license_notice'] = 'File SPDX GPL-2.0-only; header additionally contains version 2 or later wording. Preserved reference, not linked into new core.'
            pinned.append(current)
    result_base = 'analysis/stock-driver-followup-20261113/goodix-host-02'
    result = json.loads((ROOT / result_base / 'result.json').read_text())
    require(result['status'] == 'passed' and result['sources_unchanged'], 'host/cross audit not passed')
    require(all(digest(ROOT / p) == expected for p, expected in result['inputs'].items()), 'tested source differs')
    require(all(c['exit_code'] == 0 for c in result['commands']), 'test command failed')
    require(result['host_output'] == 'PASS 6293 stock Goodix core checks\n', 'host test summary')
    require((ROOT / result_base / 'host-test.log').read_text() == result['host_output'], 'host test log mismatch')
    obj = ROOT / result['object']['path']
    require(digest(obj) == result['object']['sha256'] and obj.stat().st_size == result['object']['bytes'], 'object hash/length')
    require(struct.unpack_from('<III', obj.read_bytes()) == (0xfeedfacf, 0x100000c, 0), 'ordinary arm64 Mach-O header')
    require(not (ROOT / result_base / 'arm64-undefined.log').read_text().strip() and not result['object']['undefined'], 'undefined symbols')
    exports = sorted(line.split()[-1] for line in (ROOT / result_base / 'arm64-exports.log').read_text().splitlines())
    require(exports == sorted(result['object']['exports']) and len(exports) == 9, 'exported API mismatch')
    test_artifacts = [record(result_base + '/' + name) for name in ['result.json', 'host-compile.log', 'host-test.log', 'arm64-compile.log', 'arm64-exports.log', 'arm64-undefined.log', 'J716fGoodix.o']]
    paths.append('analysis/stock-goodix-20261113/verify_manifest.py')
    manifest = {
        'schema': 1,
        'purpose': 'Independent stock-backed Goodix pure protocol/state core; not a production driver.',
        'scope': {'runtime_qualified': False, 'raw_touch_capture_available': False, 'device_access': False, 'vm_access_by_this_subtask': False, 'hid_publication': False, 'firmware_executed_or_flashed': False},
        'files': [record(path) for path in paths],
        'references': pinned,
        'reference_revision': references['external_revisions']['google_gt738x'],
        'kernel_slice': json.loads((OUT / 'stock-protocol-evidence.json').read_text())['kernel'],
        'authorship': 'J716fGoodix.h/.c and test_goodix.c newly authored pure C11; SPDX BSD-2-Clause. Reference Linux driver retained separately under original license notices.',
        'validation': {
            'owner': 'Parent task executed host tests and ARM64 compile; this subtask only validates downloaded evidence.',
            'status': 'passed', 'checks': 6293, 'sanitizers': ['address', 'undefined'],
            'inputs': result['inputs'], 'object': result['object'], 'artifacts': test_artifacts,
            'failed_historical_attempt': 'analysis/stock-driver-followup-20261113/goodix-host-01/result.json; runner used a missing llvm-nm path after test/compile passed. Preserved, not counted as a successful complete audit.'
        },
        'not_covered': ['I2C transport or device ACK write', 'IRQ and power lifecycle', 'GENI/MMIO/DMA', 'HID sink delivery/release', 'live packet qualification', 'stylus', 'firmware/config upload']
    }
    (OUT / 'source-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'dt_match': True, 'source_files': len(paths), 'reference_files': len(pinned), 'outputs': ['dt-evidence.json', 'source-manifest.json']}))


if __name__ == '__main__':
    main()
