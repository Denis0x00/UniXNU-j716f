#!/usr/bin/env python3
"""Prepare pinned iPadOS 17 inputs only; no device, partition or frozen r30 writes.

Uses the official UniXNU IPSW identity/member resolver and UDIF decoder. Large
ZIP members are fetched with validated HTTP ranges into resumable sidecars.
SHA256 pins are checked over actual bytes, never trusted from cache JSON.
"""
from __future__ import annotations
import argparse
import concurrent.futures
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import plistlib
import struct
import subprocess
import sys
import time
import tomllib
import types
import urllib.request
import zlib

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
CORE = ROOT / 'UniXNU'
ARTIFACT = ROOT / 'artifacts/j716f-ipados-root-inputs-20261114-r30'
OLD = ROOT / 'artifacts/j716f-ipados-inputs-20261112/21H461'
CHUNK = 8 << 20
sys.path.insert(0, str(CORE / 'scripts'))
from unixnu.ipsw import Ipsw, im4p_payload
from unixnu.udif import Image


def sha(path):
    with Path(path).open('rb') as handle:
        return hashlib.file_digest(handle, 'sha256').hexdigest()


def record(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    tmp.replace(path)


def profile():
    path = CORE / 'profiles/os/ipados-17.7.11-21H461/profile.toml'
    p = tomllib.loads(path.read_text(encoding='utf-8'))
    assert p['ipsw']['build'] == '21H461' and p['ipsw']['version'] == '17.7.11'
    assert p['ipsw']['product'] == 'iPad7,1' and p['ipsw']['device_class'] == 'j120ap'
    return p


def remote_info():
    p = profile()
    ctx = types.SimpleNamespace(board=p, out_root=ARTIFACT, option_text=lambda *a: None)
    source = Ipsw(ctx)
    manifest_bytes = source.zip.read('BuildManifest.plist')
    (HERE / 'BuildManifest.plist').write_bytes(manifest_bytes)
    rows = []
    for component in ('OS', 'Cryptex1,SystemOS', 'StaticTrustCache', 'Cryptex1,SystemTrustCache'):
        name, size, packed = source.info(component)
        entry = source.zip.getinfo(name)
        source.stream.seek(entry.header_offset)
        local_header = source.stream.read(30)
        if local_header[:4] != b'PK\x03\x04':
            raise ValueError(f'{name}: invalid local ZIP header')
        flags, method = struct.unpack_from('<HH', local_header, 6)
        if flags & 1 or method != entry.compress_type or method not in (0, 8):
            raise ValueError(f'{name}: encrypted/unsupported ZIP compression')
        filename_length, extra_length = struct.unpack_from('<HH', local_header, 26)
        local_name = source.stream.read(filename_length).decode('utf-8')
        if local_name != name:
            raise ValueError(f'{name}: local/central member name mismatch')
        rows.append(dict(component=component, name=name, size=size, packed=packed,
                         compression=method, crc=entry.CRC,
                         data_offset=entry.header_offset + 30 + filename_length + extra_length,
                         sha256=p['ipsw'].get('sha256', {}).get(Path(name).name)))
    output = dict(schema_version=1, url=source.spec, archive_size=source.stream.size,
                  build=source.manifest['ProductBuildVersion'], device=source.identity['Info'],
                  build_manifest_sha256=hashlib.sha256(manifest_bytes).hexdigest(),
                  profile_sha256=sha(CORE / 'profiles/os/ipados-17.7.11-21H461/profile.toml'),
                  members=rows, expected_root_volume_uuid=p['root']['os_volume_uuid'],
                  uuid_verified_from_image=False, device_access=False)
    record(HERE / 'remote-info.json', output)
    return output


def metadata():
    data = json.loads((HERE / 'remote-info.json').read_text(encoding='utf-8'))
    p = profile()
    if data['url'] != p['ipsw']['url'] or data['build'] != p['ipsw']['build']:
        raise ValueError('cached remote metadata does not match selected OS profile')
    if sha(HERE / 'BuildManifest.plist') != data['build_manifest_sha256']:
        raise ValueError('BuildManifest changed')
    # Reuse the official identity resolver even for offline validation.
    obj = object.__new__(Ipsw)
    obj.config, obj.manifest = p['ipsw'], plistlib.loads((HERE / 'BuildManifest.plist').read_bytes())
    identity = obj._identity()
    for row in data['members']:
        if row['name'] != identity['Manifest'][row['component']]['Info']['Path']:
            raise ValueError('manifest/member association changed')
        if row['sha256'] != p['ipsw'].get('sha256', {}).get(Path(row['name']).name):
            raise ValueError('member pin changed')
    return data


def trust():
    p = profile()
    previous = json.loads((OLD.parent / 'manifest.json').read_text(encoding='utf-8'))
    inputs = [OLD / '044-76040-038.dmg.trustcache', OLD / '044-76034-038.dmg.trustcache']
    for path in [*inputs, OLD / 'kernelcache.decompressed', OLD / 'DeviceTree.j120ap.im4p']:
        expected = previous['files'][path.name]
        if path.stat().st_size != expected['bytes'] or sha(path) != expected['sha256']:
            raise ValueError(f'{path}: cached original input mismatch')
    modules = [im4p_payload(path.read_bytes()) for path in inputs]
    for module in modules:
        if struct.unpack_from('<I', module)[0] != 2 or len(module) != 24 + 24 * struct.unpack_from('<I', module, 20)[0]:
            raise ValueError('trust cache is not exact v2')
    ARTIFACT.mkdir(parents=True, exist_ok=True)
    output = ARTIFACT / 'trustcache-21H461.bin'
    expected_bundle = struct.pack('<III', 2, 12, 12 + len(modules[0])) + b''.join(modules)
    if hashlib.sha256(expected_bundle).hexdigest() not in p['payload']['trust_cache_sha256']:
        raise ValueError('original trust bundle differs from OS profile allow-list')
    if output.exists() and output.read_bytes() != expected_bundle:
        raise ValueError('refuse to replace a different trust bundle')
    if not output.exists():
        subprocess.run([sys.executable, str(CORE / 'scripts/bundle-ipsw-trust.py'), str(output), *map(str, inputs)], check=True)
    if output.read_bytes() != expected_bundle:
        raise ValueError('official bundle output differs from byte-for-byte expected modules')
    report = dict(status='verified-original-modules-bundled', path=str(output), bytes=output.stat().st_size,
                  sha256=sha(output), modules_sha256=[hashlib.sha256(x).hexdigest() for x in modules],
                  bundle_allowed_by_os_profile=True,
                  entries=[struct.unpack_from('<I', x, 20)[0] for x in modules],
                  source_paths=list(map(str, inputs)), modified=False, kernelcache_sha256=sha(OLD / 'kernelcache.decompressed'),
                  official_tool_sha256=sha(CORE / 'scripts/bundle-ipsw-trust.py'), runtime_tested=False)
    record(HERE / 'trust-ready.json', report)
    return report


def checked_range(url, start, end):
    for attempt in range(5):
        try:
            req = urllib.request.Request(url, headers={'Range': f'bytes={start}-{end}', 'Accept-Encoding': 'identity'})
            with urllib.request.urlopen(req, timeout=60) as response:
                if response.status != 206:
                    raise ValueError('server ignored Range')
                declared = response.headers.get('Content-Range', '')
                if not declared.startswith(f'bytes {start}-{end}/'):
                    raise ValueError(f'wrong Content-Range: {declared}')
                result = response.read(end - start + 2)
            if len(result) != end - start + 1:
                raise OSError('short or oversized response')
            return result
        except (OSError, TimeoutError):
            if attempt == 4:
                raise
            time.sleep(min(2 ** attempt, 8))


def fetch_member(meta, row):
    name = Path(row['name']).name
    if not row['sha256']:
        raise ValueError('large member has no trusted SHA256 pin')
    folder = ARTIFACT / 'ipsw/21H461'
    folder.mkdir(parents=True, exist_ok=True)
    destination = folder / name
    if destination.is_file():
        if destination.stat().st_size != row['size'] or sha(destination) != row['sha256']:
            raise ValueError(f'{name}: existing image failed full checksum; not replacing it')
        return dict(path=str(destination), reused=True, sha256=row['sha256'], bytes=row['size'])
    part = destination.with_name(name + '.zip-member.partial')
    identity = part.with_suffix('.json')
    expected = dict(url=meta['url'], member=row, build_manifest_sha256=meta['build_manifest_sha256'])
    if part.exists():
        if not identity.exists() or json.loads(identity.read_text()) != expected:
            raise ValueError(f'{name}: cannot resume without exact source identity')
    else:
        record(identity, expected)
    have = part.stat().st_size if part.exists() else 0
    if have > row['packed']:
        raise ValueError('partial member is larger than declared compressed size')
    progress = HERE / (name + '.progress.json')
    # Separate fully written ranges allow parallel transfer without trusting
    # holes in a pre-sized sparse file. Only an ordered contiguous prefix is
    # appended to .partial. Interrupted ranges are retried on the next run.
    ranges = [(start, min(start + CHUNK, row['packed'])) for start in range(have, row['packed'], CHUNK)]
    def get_range(bounds):
        start, end = bounds
        target = part.with_name(part.name + f'.range-{start}-{end}')
        if target.exists() and target.stat().st_size == end - start:
            return bounds, target
        data = checked_range(meta['url'], row['data_offset'] + start, row['data_offset'] + end - 1)
        temporary = target.with_name(target.name + '.tmp')
        temporary.write_bytes(data)
        temporary.replace(target)
        return bounds, target
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as workers, part.open('ab') as output:
        for (start, end), chunk_path in workers.map(get_range, ranges):
            if have != start:
                raise ValueError('non-contiguous download assembly')
            output.write(chunk_path.read_bytes())
            output.flush()
            have = end
            chunk_path.unlink()
            record(progress, dict(state='downloading', component=row['component'], received=have, expected=row['packed'],
                                  percent=round(100 * have / row['packed'], 2), resumable=True, range_workers=6, updated_unix=time.time()))
    digest = hashlib.sha256()
    crc, size = 0, 0
    tmp = destination.with_name(name + '.decoded.partial')
    decoder = zlib.decompressobj(-15) if row['compression'] == 8 else None
    with part.open('rb') as source, tmp.open('wb') as output:
        while data := source.read(CHUNK):
            data = decoder.decompress(data) if decoder else data
            digest.update(data)
            crc = zlib.crc32(data, crc)
            size += len(data)
            output.write(data)
        if decoder:
            data = decoder.flush()
            digest.update(data)
            crc = zlib.crc32(data, crc)
            size += len(data)
            output.write(data)
            if not decoder.eof or decoder.unused_data or decoder.unconsumed_tail:
                raise ValueError('deflate stream is truncated or has trailing bytes')
    if size != row['size'] or crc != row['crc'] or digest.hexdigest() != row['sha256']:
        raise ValueError(f'{name}: size, ZIP CRC or pinned SHA256 mismatch')
    tmp.replace(destination)
    result = dict(component=row['component'], member=row['name'], path=str(destination), size=size,
                  sha256=digest.hexdigest(), source='remote-range', zip_crc32=f'{crc:08x}', verified=True)
    record(destination.with_name(name + '.json'), result)
    record(progress, dict(state='verified', **result))
    # Source-preserving cache removal is deliberately not automatic: the packed
    # member remains for repeatable recovery and may be cleaned after acceptance.
    return result


def images(fetch=False):
    data = metadata()
    rows = [r for r in data['members'] if r['component'] in ('OS', 'Cryptex1,SystemOS')]
    if fetch:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as executor:
            list(executor.map(lambda row: fetch_member(data, row), rows))
    output = []
    for row in rows:
        path = ARTIFACT / 'ipsw/21H461' / Path(row['name']).name
        if not path.exists():
            output.append(dict(component=row['component'], path=str(path), status='missing', expected_sha256=row['sha256']))
            continue
        if path.stat().st_size != row['size'] or sha(path) != row['sha256']:
            raise ValueError(f'{path}: actual image differs from OS profile pin')
        image = Image(path)
        kinds = sorted({r[2] for r in image.chunks}) if image.chunks else []
        output.append(dict(component=row['component'], path=str(path), status='verified',
                           bytes=row['size'], sha256=row['sha256'], raw_bytes=image.size,
                           udif=image.chunks is not None, chunk_types=[hex(x) for x in kinds]))
    result = dict(images=output, expected_root_volume_uuid=data['expected_root_volume_uuid'],
                  uuid_verified_from_image=False, geometry_selected=False, device_access=False,
                  data_seed_status='missing-requires-macos-first-install-preparation',
                  official_decoder_sha256=sha(CORE / 'scripts/unixnu/udif.py'))
    record(HERE / 'images-ready.json', result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['remote-info', 'trust', 'fetch-images', 'verify-images'])
    args = parser.parse_args()
    result = remote_info() if args.action == 'remote-info' else trust() if args.action == 'trust' else images(args.action == 'fetch-images')
    print(json.dumps(result, indent=2, ensure_ascii=False))


if __name__ == '__main__':
    main()
