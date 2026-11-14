#!/usr/bin/env python3
"""Stdlib-only verifier for the dedicated r9 display-ownership FD provenance.

Pins the independently reviewed archive, all 116 evidence files, and extracts
the actual bounded FV/FFS/LZMA contents. This never qualifies physical behavior.
"""
from pathlib import Path, PurePosixPath
import argparse
import hashlib
import json
import lzma
import struct
import uuid

FD_SHA = '4a04f13e77067732533821c39ba575a61babd46d5791ce801c67e9c0b8386dcb'
PROVENANCE_SHA = '61a16cefa8ab27a8c09834a79a6b36d5fe071671da99fa113eee597bcc195c1c'
MANIFEST_SHA = '3d6aabcfd20c08bcadaa4c1fac9f2b1adff3ecf2c0a5c7bba5d6b7a9ea7b129a'
REVIEW_SHA = 'e66623da15585501258e852493cc744149dd458e78669b4a956327379c9cce73'
ARCHIVE_REL = 'artifacts/j716f-menu-ownership-r9-firmware-20261113'
REVIEW_REL = 'analysis/menu-ownership-r9-review-20261113/binary-review.json'
EXTRACTION_REL = 'analysis/menu-ownership-r9-review-20261113/fd-extraction.json'
DIAGNOSTICS = {'initrd_full_crc32': True, 'max_initrd_bytes': 83886080,
               'diagnostic_menu_hold': True, 'qr_export': True,
               'early_handoff': True, 'old_footer_absent': True}

def require(ok, message):
    if not ok:
        raise ValueError(message)

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

def checked_relative(name):
    require(isinstance(name, str) and name and '\\' not in name and ':' not in name,
            'Expected canonical workspace-relative POSIX path')
    p = PurePosixPath(name)
    require(not p.is_absolute() and all(c not in ('', '.', '..') for c in name.split('/')),
            'Unsafe evidence path')
    return Path(*p.parts)

def checked_file(root, name):
    root = root.resolve(strict=True)
    p = (root / checked_relative(name)).resolve(strict=True)
    require(p.is_relative_to(root) and p.is_file(), 'File escapes evidence root')
    return p

def identity(raw):
    return {'bytes': len(raw), 'sha256': sha(raw)}

def check(raw, spec):
    require(type(spec.get('bytes')) is int and identity(raw) ==
            {'bytes': spec['bytes'], 'sha256': spec['sha256']}, 'Evidence identity changed')

def pinned_json(path, digest):
    raw = path.read_bytes()
    require(sha(raw) == digest, 'Reviewed metadata identity changed: '+path.name)
    return json.loads(raw)

def sections(data, start, end, depth):
    require(depth <= 8, 'Nested section depth exceeded')
    pos = start
    while pos + 4 <= end:
        if not any(data[pos:end]):
            return
        size, kind = int.from_bytes(data[pos:pos+3], 'little'), data[pos+3]
        header = 4
        if size == 0xffffff:
            require(pos+8 <= end, 'Truncated extended section')
            size, header = struct.unpack_from('<I', data, pos+4)[0], 8
        require(header <= size <= end-pos, 'Invalid section bounds')
        if kind == 2:
            require(size >= header+20, 'Truncated guided section')
            guid = str(uuid.UUID(bytes_le=data[pos+header:pos+header+16]))
            offset, flags = struct.unpack_from('<HH', data, pos+header+16)
            require(header+20 <= offset <= size and flags & 1, 'Invalid guided descriptor')
            require(guid == 'ee4e5898-3914-4259-9d6e-dc7bd79403cf', 'Unknown compression GUID')
            decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE)
            expanded = decoder.decompress(data[pos+offset:pos+size], max_length=32*1024*1024)
            require(decoder.eof and not decoder.unused_data and len(expanded) <= 32*1024*1024,
                    'Invalid or excessive LZMA stream')
            yield from sections(expanded, 0, len(expanded), depth+1)
        else:
            yield kind, data[pos+header:pos+size]
        pos = (pos+size+3) & ~3

def volumes(data, depth=0):
    require(depth <= 8, 'Nested FV depth exceeded')
    position = 0
    while True:
        mark = data.find(b'_FVH', position)
        if mark < 0:
            return
        position = mark+4
        base = mark-40
        require(base >= 0 and base+56 <= len(data), 'Invalid FV start')
        size = struct.unpack_from('<Q', data, base+32)[0]
        header = struct.unpack_from('<H', data, base+48)[0]
        require(header >= 56 and header % 2 == 0 and header <= size <= len(data)-base, 'Invalid FV bounds')
        require(sum(struct.unpack_from('<'+'H'*(header//2), data, base)) & 0xffff == 0, 'Invalid FV checksum')
        pos = base+((header+7) & ~7)
        while pos+24 <= base+size:
            head = data[pos:pos+24]
            if head == b'\xff'*24:
                break
            length = int.from_bytes(head[20:23], 'little')
            require(length >= 24 and pos+length <= base+size, 'Invalid FFS bounds')
            require((sum(head)-head[17]-head[23]) & 255 == 0, 'Invalid FFS header checksum')
            if head[18] != 0xf0:
                guid = str(uuid.UUID(bytes_le=head[:16]))
                leaves = list(sections(data, pos+24, pos+length, depth))
                for kind, raw in leaves:
                    if kind == 0x17:
                        yield from volumes(raw, depth+1)
                yield guid, head[18], leaves
            pos = (pos+length+7) & ~7
        position = base+size

def extract_modules(fd):
    require(identity(fd) == {'bytes':7340032, 'sha256':FD_SHA}, 'Expected exact reviewed ownership FD')
    result = {}
    for guid, kind, leaves in volumes(fd):
        ui = [raw.decode('utf-16-le').rstrip('\0') for typ, raw in leaves if typ == 0x15]
        require(len(ui) <= 1, 'Ambiguous module UI')
        name = ui[0] if ui else ('Sec' if guid == '9affb503-e643-4141-8b90-17e8588b1d35' else guid)
        pe = [(typ, raw) for typ, raw in leaves if typ in (0x10, 0x12)]
        if not pe:
            continue
        require(len(pe) == 1 and name not in result, 'Ambiguous executable module')
        typ, raw = pe[0]
        result[name] = dict(name=name, guid=guid, ffs_type=kind, section_type=typ, **identity(raw))
    require(len(result)==35, 'Unexpected executable module count')
    return result

def common(fd, provenance, manifest, evidence_root):
    require(provenance.get('schema_version')==9 and
            provenance.get('kind')=='j716f-menu-display-ownership-firmware-r9', 'Wrong dedicated provenance schema')
    check(fd, provenance['fd'])
    require(provenance['diagnostics']==DIAGNOSTICS and
            provenance['hardware_tested'] is False and provenance['deployment_ready'] is False and
            provenance['unrelated_modules_byte_identical'] is False and
            provenance['xnu_checkpoint_advanced'] is False, 'Invalid scope or diagnostic contract')
    require(set(manifest['outputs'])=={'SILICIUM_UEFI.fd','fd-provenance.json'} and
            manifest['hardware_tested'] is False and manifest['deployment_ready'] is False,
            'Unexpected reviewed archive manifest')
    check(fd, manifest['outputs']['SILICIUM_UEFI.fd'])
    seen = set()
    for item in provenance['evidence']:
        name = item['path']
        require(name not in seen and isinstance(item['role'], str), 'Duplicate or invalid evidence')
        check(checked_file(evidence_root, name).read_bytes(), item)
        seen.add(name)
    require(len(seen)==116, 'Incomplete dedicated evidence')
    review = pinned_json(checked_file(evidence_root, REVIEW_REL), REVIEW_SHA)
    extracted = json.loads(checked_file(evidence_root, EXTRACTION_REL).read_bytes())
    modules = extract_modules(fd)
    expected = extracted['modules']['ownership']
    require(modules.keys()==expected.keys(), 'Module set differs from independent extraction')
    for name, spec in modules.items():
        require(all(expected[name][key]==value for key,value in spec.items()), 'Module identity differs: '+name)
    require(len(extracted['changed_modules'])==35 and not extracted['unchanged_modules'] and
            review['hardware_tested'] is False and review['source_overlay_files_checked']==6 and
            review['host_test_source_files_checked']==22 and review['host_candidate_checks']==65,
            'Independent review contract differs')
    return {'sha256':FD_SHA, 'bytes':len(fd), 'evidence_files':len(seen),
            'diagnostics':DIAGNOSTICS, 'executable_modules_verified':len(modules),
            'critical_machine_code_review_sha256':REVIEW_SHA,
            'scope':'Exact reviewed candidate identity and selected machine-code provenance; physical menu/QR unqualified',
            'hardware_tested':False,'kernel_checkpoint_advanced':False,
            'unrelated_modules_byte_identical':False}

def validate_fd(root, fd, provenance_path):
    """Validate before packaging; evidence remains at workspace-relative paths."""
    root=Path(root).resolve(strict=True)
    fd=Path(fd).resolve(strict=True)
    provenance_path=Path(provenance_path).resolve(strict=True)
    require(fd==root/ARCHIVE_REL/'SILICIUM_UEFI.fd' and
            provenance_path==fd.parent/'fd-provenance.json', 'Expected dedicated reviewed firmware archive')
    manifest_path=fd.parent/'manifest.json'
    doc=pinned_json(provenance_path,PROVENANCE_SHA)
    manifest=pinned_json(manifest_path,MANIFEST_SHA)
    check(provenance_path.read_bytes(),manifest['outputs']['fd-provenance.json'])
    raw=fd.read_bytes()
    common(raw,doc,manifest,root)
    return doc,identity(raw),manifest_path

def verify_fd_provenance(artifact_root, fd_bytes):
    """Validate packaged evidence without needing original workspace or tools."""
    root=Path(artifact_root).resolve(strict=True)
    doc=pinned_json(checked_file(root,'fd-provenance.json'),PROVENANCE_SHA)
    manifest=pinned_json(checked_file(root,'fd-archive-manifest.json'),MANIFEST_SHA)
    source=json.loads(checked_file(root,'fd-source.json').read_bytes())
    require(source=={'fd':ARCHIVE_REL+'/SILICIUM_UEFI.fd',
                     'provenance':ARCHIVE_REL+'/fd-provenance.json',
                     'manifest':ARCHIVE_REL+'/manifest.json'}, 'Packaged source archive identity differs')
    check(checked_file(root,'fd-provenance.json').read_bytes(),manifest['outputs']['fd-provenance.json'])
    return common(fd_bytes,doc,manifest,root/'fd-evidence')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root',type=Path,required=True)
    parser.add_argument('--packaged',action='store_true')
    args=parser.parse_args()
    if args.packaged:
        report=verify_fd_provenance(args.root,(args.root/'SILICIUM_UEFI.fd').read_bytes())
    else:
        archive=args.root/ARCHIVE_REL
        _,checked,_=validate_fd(args.root,archive/'SILICIUM_UEFI.fd',archive/'fd-provenance.json')
        report=dict(result='passed',**checked,hardware_tested=False)
    print(json.dumps(report,indent=2))
