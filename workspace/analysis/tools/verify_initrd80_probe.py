#!/usr/bin/env python3
"""Independent host readback of an 80 MiB J716F probe, without device access."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import tomllib
import zlib
from verify_menu_artifact import require, sha256, load_json, verify as verify_baseline

def read_fat16(data):
    require(len(data) == 80 * 1024 * 1024 and data[510:512] == b'\x55\xaa', 'Invalid probe FAT boot sector')
    bps, spc, reserved, fats, roots, total16, media, spf = struct.unpack_from('<HBHBHHBH', data, 11)
    total = total16 or struct.unpack_from('<I', data, 32)[0]
    require(bps == 512 and spc == 4 and reserved > 0 and fats == 2 and roots > 0 and spf > 0, 'Unsupported FAT geometry')
    require(media == 0xf8 and data[38] == 0x29 and data[43:54] == b'J716F80M'.ljust(11, b' '), 'Unexpected FAT identity fields')
    require(total * bps == len(data), 'FAT sector count differs from file size')
    root_sectors = (roots * 32 + bps - 1) // bps
    data_sector = reserved + fats * spf + root_sectors
    clusters = (total - data_sector) // spc
    require(4085 <= clusters < 65525, 'Probe must be FAT16 by data cluster count')
    fat = data[reserved*bps:(reserved+spf)*bps]
    require(len(fat) >= (clusters+2)*2, 'FAT too short for cluster count')
    require(fat == data[(reserved+spf)*bps:(reserved+2*spf)*bps], 'FAT copies differ')
    require(fat[:4] == b'\xf8\xff\xff\xff', 'Invalid reserved FAT entries')
    cluster_bytes = spc*bps
    used = set()

    def chain(first):
        if first == 0:
            return b''
        parts, seen = [], set()
        current = first
        while current < 0xfff8:
            require(2 <= current < clusters+2 and current not in seen and current not in used, 'Invalid, looping or crosslinked FAT chain')
            seen.add(current); used.add(current)
            pos = data_sector*bps + (current-2)*cluster_bytes
            parts.append(data[pos:pos+cluster_bytes])
            current = struct.unpack_from('<H', fat, current*2)[0]
        return b''.join(parts)

    files = {}
    namespace = set()

    def directory(blob, prefix='', depth=0):
        require(depth <= 4, 'Unexpected FAT directory depth')
        lfn = {}
        checksum = None
        expected_order = None
        for pos in range(0,len(blob),32):
            e = blob[pos:pos+32]
            require(len(e) == 32, 'Truncated directory entry')
            if e[0] == 0:
                require(not lfn, 'Unterminated LFN chain')
                break
            if e[0] == 0xe5:
                lfn={}; checksum=None; expected_order=None; continue
            if e[11] == 0x0f:
                order = e[0] & 0x1f
                require(1 <= order <= 20 and not (e[0] & 0xa0) and e[12] == 0 and e[26:28] == b'\0\0', 'Invalid LFN entry')
                if e[0] & 0x40:
                    require(not lfn, 'Unterminated previous LFN chain')
                    checksum = e[13]; expected_order = order
                require(order == expected_order and order not in lfn and checksum == e[13], 'Invalid LFN chain')
                lfn[order] = e[1:11]+e[14:26]+e[28:32]
                expected_order -= 1
                continue
            if e[11] & 8:
                require(not lfn, 'LFN attached to volume label')
                lfn={}; checksum=None; expected_order=None; continue
            if lfn:
                require(expected_order == 0 and sorted(lfn) == list(range(1,max(lfn)+1)), 'Incomplete LFN sequence')
                actual=0
                for b in e[:11]:
                    actual=(((actual&1)<<7)+(actual>>1)+b)&255
                require(actual == checksum, 'LFN checksum mismatch')
                name=b''.join(lfn[i] for i in sorted(lfn)).decode('utf-16le').split('\0',1)[0].rstrip('\uffff')
            else:
                stem=e[:8].decode('ascii').rstrip(); ext=e[8:11].decode('ascii').rstrip()
                name=stem+('.'+ext if ext else '')
            lfn={}; checksum=None; expected_order=None
            if name in ('.','..'):
                continue
            require(name and '/' not in name and '\\' not in name and ':' not in name, 'Unsafe FAT filename')
            require(struct.unpack_from('<H',e,20)[0] == 0, 'Unexpected FAT16 high cluster')
            first=struct.unpack_from('<H',e,26)[0]
            size=struct.unpack_from('<I',e,28)[0]
            content=chain(first)
            path=(prefix+'/'+name).strip('/').lower()
            require(path not in namespace, 'Duplicate FAT file or directory name')
            namespace.add(path)
            if e[11] & 16:
                require(first >= 2, 'Invalid directory cluster')
                directory(content,path,depth+1)
            else:
                require(path not in files and size <= len(content) and len(content)-size < cluster_bytes, 'Invalid file extent')
                require(not any(content[size:]), 'Nonzero file slack')
                files[path]=content[:size]
    root_start=(reserved+fats*spf)*bps
    directory(data[root_start:root_start+roots*32])
    for number in range(2, clusters + 2):
        value = struct.unpack_from('<H', fat, number * 2)[0]
        if number not in used:
            require(value == 0, 'Unreferenced allocated or bad FAT cluster')
            start = data_sector*bps + (number-2)*cluster_bytes
            require(not any(data[start:start+cluster_bytes]), 'Nonzero unallocated FAT cluster')
    require(not any(data[data_sector*bps + clusters*cluster_bytes:]), 'Nonzero trailing FAT sectors')
    require(not any(fat[(clusters+2)*2:]), 'Nonzero unused FAT entries')
    return files, {'bytes_per_sector':bps,'sectors_per_cluster':spc,'data_clusters':clusters,'volume_id':hex(struct.unpack_from('<I',data,39)[0]),'allocated_clusters':len(used),'last_allocated_byte_exclusive':data_sector*bps+(max(used)-1)*cluster_bytes}


def checked_relative(value):
    relative = PurePosixPath(value)
    require(value and not relative.is_absolute() and '..' not in relative.parts and
            ':' not in value and '\\' not in value, 'Unsafe archived path')
    return relative


def check_identity(path, spec):
    require(path.is_file() and path.stat().st_size == spec['bytes'], f'Invalid size: {path}')
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    require(digest == spec['sha256'], f'Hash mismatch: {path}')


def verify_fd_provenance(root, fd):
    doc = load_json(root / 'fd-provenance.json')
    require(doc.get('schema_version') == 1, 'Unsupported firmware provenance')
    require(doc['fd']['bytes'] == len(fd) == 7340032 and doc['fd']['sha256'] == sha256(fd), 'FD provenance identity differs')
    require(sha256(fd) != 'f96e9e19ac4cfd22577964e72943b570e34031736be2108a3dc5db7e878e0c2c', 'Old FD lacks new full-initrd CRC diagnostics')
    flags = doc['diagnostics']
    require(flags.get('initrd_full_crc32') is True and flags.get('diagnostic_menu_hold') is True and
            type(flags.get('max_initrd_bytes')) is int and flags['max_initrd_bytes'] >= 83886080,
            'Missing required firmware diagnostics contract')
    source = load_json(root / 'fd-source.json')
    paths = {key: checked_relative(value) for key, value in source.items()}
    require(set(paths) == {'fd', 'provenance', 'manifest'} and
            len({path.parent for path in paths.values()}) == 1 and
            paths['fd'].name == 'SILICIUM_UEFI.fd' and paths['manifest'].name == 'manifest.json',
            'Firmware source files do not share one archive')
    original_manifest = load_json(root / 'fd-archive-manifest.json')['outputs']
    check_identity(root / 'SILICIUM_UEFI.fd', original_manifest[paths['fd'].name])
    check_identity(root / 'fd-provenance.json', original_manifest[paths['provenance'].name])
    roles, seen = set(), set()
    for item in doc['evidence']:
        relative = checked_relative(item['path'])
        require(str(relative) not in seen, 'Duplicate firmware evidence')
        path = (root / 'fd-evidence' / relative).resolve(strict=True)
        require(path.is_relative_to(root / 'fd-evidence'), 'Firmware evidence escapes archive')
        check_identity(path, item)
        roles.add(item['role'])
        seen.add(str(relative))
    require({'firmware-build-report', 'crc-source', 'platform-config'} <= roles, 'Missing firmware evidence roles')
    return {'sha256': sha256(fd), 'evidence_files': len(seen), 'diagnostics': flags,
            'scope': 'Archive/evidence identity only; source assertions do not prove machine code or hardware execution'}


def verify(root, baseline):
    root = root.resolve(strict=True)
    old = verify_baseline(baseline)
    require(old['image']['image_sha256'] == 'f8b82f6da135ba236dc7bd959af039cda05f9ffcc81ae16c73d212fd9dfd9479', 'Wrong empty-menu baseline')
    manifest_doc = load_json(root / 'manifest.json')
    manifest = manifest_doc['outputs']
    required = {'boot.img', 'boot.json', 'SILICIUM_UEFI.fd', 'firmware.dtb', 'shim.bin', 'uefi-config.toml',
                'board.toml', 'probe.img', 'unixnu.toml', 'probe.txt', 'pattern.bin', 'payload-manifest.json',
                'fd-provenance.json', 'fd-source.json', 'fd-archive-manifest.json', 'build-result.json'}
    require(required <= manifest.keys(), 'Incomplete probe artifact manifest')
    require(manifest_doc.get('hardware_tested') is False and manifest_doc.get('deployment_ready') is False,
            'Host artifact must not claim hardware/deployment qualification')
    for name, spec in manifest.items():
        relative = checked_relative(name)
        path = (root / relative).resolve(strict=True)
        require(path.is_relative_to(root) and path.is_file(), 'Artifact manifest escapes root')
        check_identity(path, spec)
    for name in ('firmware.dtb', 'shim.bin', 'uefi-config.toml'):
        require((root / name).read_bytes() == (baseline / name).read_bytes(), 'Baseline input changed: ' + name)
    fd = (root / 'SILICIUM_UEFI.fd').read_bytes()
    firmware = verify_fd_provenance(root, fd)
    data = (root / 'boot.img').read_bytes()
    original = (baseline / 'boot.img').read_bytes()
    ramdisk = (root / 'probe.img').read_bytes()
    dtb = (root / 'firmware.dtb').read_bytes()
    shim = (root / 'shim.bin').read_bytes()
    require(data[:8] == b'ANDROID!' and 1660 < len(data) <= 100663296, 'Invalid Android image magic or boot capacity')
    ks, ka, rs, ra, ss, sa, tags, page, version, osver = struct.unpack_from('<10I', data, 8)
    recovery, roff, hsize = struct.unpack_from('<IQI', data, 1632)
    ds, da = struct.unpack_from('<IQ', data, 1648)
    require((ka, rs, ra, ss, sa, tags, page, version, recovery, roff, hsize, ds, da) ==
            (0x8000, 83886080, 0x1000000, 0, 0, 0x100, 4096, 2, 0, 0, 1660, len(dtb), 0x1f00000),
            'Boot header fields differ from fixed 80 MiB scope')
    # Only kernel size, ramdisk size/address and Android image ID may differ.
    for lo, hi in ((0, 8), (12, 16), (24, 576), (608, 1660)):
        require(data[lo:hi] == original[lo:hi], 'Unexpected boot header change')
    aligned = lambda n: (n + page - 1) // page * page
    rp = page + aligned(ks)
    dp = rp + aligned(rs)
    require(ks > 0 and len(data) == dp + aligned(ds), 'Invalid Android section layout')
    require(not any(data[hsize:page]) and not any(data[page+ks:rp]) and
            not any(data[rp+rs:dp]) and not any(data[dp+ds:]), 'Nonzero Android section padding')
    kernel = data[page:page+ks]
    combined = shim + fd
    decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
    unpacked = decoder.decompress(kernel, len(combined) + 1)
    require(unpacked == combined and decoder.eof and not decoder.unconsumed_tail and decoder.unused_data == dtb,
            'Kernel does not decode exactly to archived shim + new FD followed by original DTB')
    require(len(shim) == 112 and shim[56:60] == b'ARM\x64' and
            struct.unpack_from('<QQ', shim, 8) == (0xce000000, len(combined)), 'BootShim header differs')
    require(data[rp:rp+rs] == ramdisk and data[dp:dp+ds] == dtb, 'Boot ramdisk or independent DTB differs')
    digest = hashlib.sha1()
    for part in (kernel, ramdisk, b'', b'', dtb):
        digest.update(part)
        digest.update(struct.pack('<I', len(part)))
    require(data[576:608] == digest.digest() + b'\0' * 12, 'Android image ID mismatch')
    board = tomllib.loads((root / 'board.toml').read_text(encoding='utf-8'))
    require(board['board']['name'] == 'j716f-initrd80-probe' and
            board['kernel']['defconfig'] == 'j716f_initrd80_probe_defconfig' and
            board['package']['boot_partition_bytes'] == 100663296, 'Incorrect target/capacity')
    files, geometry = read_fat16(ramdisk)
    require(set(files) == {'efi/unixnu/unixnu.toml', 'probe.txt', 'pattern.bin'}, 'Unexpected FAT file set')
    for internal, external in (('efi/unixnu/unixnu.toml', 'unixnu.toml'), ('probe.txt', 'probe.txt'), ('pattern.bin', 'pattern.bin')):
        require(files[internal] == (root / external).read_bytes(), 'FAT content differs: ' + internal)
    cfg = tomllib.loads(files['efi/unixnu/unixnu.toml'].decode('utf-8'))
    require(set(cfg) == {'version', 'boot', 'ui', 'kits', 'entry'} and cfg['version'] == 1 and
            cfg['ui'] == {'title': 'J716F 80 MiB initrd probe'} and cfg['kits'] == {'autoload': False} and
            cfg['boot'] == {'timeout': -1, 'default': 'tools', 'rotate': 0}, 'Wrong runtime config identity/hold/default')
    action_titles = [('tools', 'Tools'), ('exit', 'Return to firmware'), ('reset', 'Restart'), ('shutdown', 'Power off')]
    require(cfg['entry'] == [{'id': a, 'title': t, 'type': 'action', 'action': a} for a, t in action_titles], 'Wrong standard actions')
    marker = files['probe.txt'].decode('utf-8').splitlines()
    require([x for x in marker if x.startswith('Probe identity:')] == ['Probe identity: j716f-initrd80-probe-v1'], 'Wrong marker')
    require('Volume bytes: 83886080' in marker, 'Marker volume size differs')
    pattern = files['pattern.bin']
    require(len(pattern) == 79 * 1024 * 1024, 'Wrong pattern size')
    for position in range(0, len(pattern), 4096):
        block_number = position // 4096
        expected = hashlib.sha256(b'J716F-80MiB-initrd-probe-v1:' + block_number.to_bytes(8, 'little')).digest() * 128
        require(pattern[position:position+4096] == expected, f'Pattern content mismatch at {position:#x}')
    require(geometry['volume_id'] == '0x716f0080' and geometry['last_allocated_byte_exclusive'] > 79 * 1024 * 1024,
            'Wrong volume ID or missing allocated content near end of 80 MiB volume')
    payload_report = load_json(root / 'payload-manifest.json')
    require(payload_report['bytes'] == len(ramdisk) and payload_report['sha256'] == sha256(ramdisk) and
            payload_report['crc32'] == f'{zlib.crc32(ramdisk):08x}', 'Payload full-volume identity differs')
    require({x.lower() for x in payload_report['files']} == set(files), 'Payload manifest file list differs')
    for name, spec in payload_report['files'].items():
        raw = files[name.lower()]
        require(spec['bytes'] == len(raw) and spec['sha256'] == sha256(raw) and spec['crc32'] == f'{zlib.crc32(raw):08x}',
                'Payload file manifest mismatch: ' + name)
    report = load_json(root / 'boot.json')
    expected = {'image_sha256': sha256(data), 'image_bytes': len(data), 'ramdisk_bytes': len(ramdisk),
                'header_version': 2, 'kernel_address': hex(ka), 'tags_address': hex(tags), 'dtb_address': hex(da),
                'dtb_file_offset': dp, 'kernel_sha256': sha256(kernel), 'fd_sha256': sha256(fd),
                'dtb_sha256': sha256(dtb), 'dtb_bytes': len(dtb), 'shim_bytes': len(shim), 'hardware_tested': False}
    for key, value in expected.items():
        require(report.get(key) == value, 'boot.json mismatch: ' + key)
    build = load_json(root / 'build-result.json')
    require(build.get('status') == 'host-generation-passed' and build.get('hardware_tested') is False and
            build.get('device_actions') is False and build.get('deployment_ready') is False,
            'Build report is not a successful host-only result')
    return {'result': 'passed', 'scope': 'Independent 80 MiB non-XNU host bytes/layout/config verification',
            'menu_title': cfg['ui']['title'], 'menu_actions': [a for a, _ in action_titles], 'image': expected,
            'boot_partition_bytes': 100663296, 'boot_partition_remaining_bytes': 100663296 - len(data),
            'ramdisk_address': hex(ra), 'ramdisk_sha256': sha256(ramdisk), 'ramdisk_crc32': f'{zlib.crc32(ramdisk):08x}',
            'nominal_ramdisk_end': hex(ra+rs), 'nominal_dtb_start': hex(da), 'nominal_overlap': ra <= da < ra+rs,
            'firmware': firmware, 'fat': geometry, 'pattern_blocks_verified': len(pattern)//4096,
            'files': {k: {'bytes': len(v), 'sha256': sha256(v)} for k, v in files.items()},
            'manifest_entries_verified': len(manifest), 'hardware_tested': False, 'deployment_ready': False,
            'limits': ['Nominal header addresses do not measure ABL placements; overlap requires separate trial review.',
                       'Firmware provenance is evidence identity, not proof that diagnostic code executed.',
                       'Runtime CRC, source placement, final EBS map and XNU ownership remain unmeasured.']}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact', type=Path)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    require(not args.report.exists(), 'Refusing to overwrite a verification report')
    result = verify(args.artifact, args.baseline)
    args.report.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(result, indent=2))

