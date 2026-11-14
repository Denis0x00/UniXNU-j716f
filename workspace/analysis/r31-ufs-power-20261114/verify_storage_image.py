#!/usr/bin/env python3
"""Read-only r31 image verification; no compiler, firmware or device calls."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
import tomllib
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from verify_xnu_entry import read_fat32, validate_adt
from verify_first_payload import adt_read
from verify_xnu_adt_phandle_probe import iokit_interrupt_reference_model
from verify_storage_kernel import verify_candidate

FD = 'f45f81be07b83062b6e8a24c8e531eaf7d8c00bfadfddfbae66c5048cfe7a2b9'
EFI = '42fe2f42085aee507efe1ceb151026934dd740a3b440c3d5da062ab3f234e108'
DTB = '6c87f77599a2056befac0080718d826205afed462b9abacac7aa0664e4d3e6e3'
SHIM = '72bfc9e9137a0e3ab9f7264162968c546ba63533b94d6813814fe84c831d03b4'
TRUST = '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7'
BASE_ADT = 'a9ba81a00c94eb5943052095192ed85da40f44931503002560a3c2ddb4b35126'
TITLE = 'J716F XNU UFS power r31'
XNU_TITLE = 'Start XNU (UFS power diagnostics)'
FAT_BYTES = 80 << 20

def sha(raw):
    return hashlib.sha256(raw).hexdigest()

def require(ok, message):
    if not ok:
        raise ValueError(message)

def identity(path):
    b = path.read_bytes()
    return dict(bytes=len(b), sha256=sha(b))

def compare_adt(baseline, candidate):
    require(sha(baseline) == BASE_ADT, 'Wrong independently retained hardware ADT')
    hardware = validate_adt(baseline)
    before, after = adt_read(baseline), adt_read(candidate)
    require(set(before) == set(after), 'ADT node set changed')
    for node, old in before.items():
        new = dict(after[node])
        if node == '/chosen/memory-map':
            require(new.pop('TrustCache', None) == bytes(16), 'Missing exact empty trust-cache placeholder')
        require(set(old) == set(new), 'Unexpected ADT property set change')
        for key, value in old.items():
            if (node, key) == ('/chosen', 'random-seed'):
                require(len(new[key]) == 256 and any(new[key]), 'Invalid random seed')
            else:
                require(new[key] == value, 'Unexpected ADT property value change')
    return dict(hardware=hardware, interrupt_model=iokit_interrupt_reference_model(candidate),
                allowed_differences=['/chosen/random-seed', '/chosen/memory-map/TrustCache'],
                hardware_properties_unchanged=True, identity_value_disclosed=False)

def menu_check(raw):
    c = tomllib.loads(raw.decode())
    actions = [('tools', 'Tools'), ('exit', 'Return to firmware'), ('reset', 'Restart'), ('shutdown', 'Power off')]
    expected = {'version': 1, 'boot': {'timeout': -1, 'default': 'xnu', 'rotate': 0},
                'ui': {'title': TITLE}, 'kits': {'autoload': False},
                'entry': [{'id': 'xnu', 'title': XNU_TITLE, 'type': 'xnu', 'path': 'Payloads/kernel.macho',
                           'args': '-v serial=3 j716f-earlylog=1'}] +
                         [dict(id=i, title=t, type='action', action=i) for i, t in actions]}
    require(c == expected, 'Manual five-entry menu differs')
    return c

def verify(directory):
    d = directory.resolve(strict=True)
    b, ramdisk, fd, dtb, shim = [(d/n).read_bytes() for n in ('boot.img', 'payload.img', 'SILICIUM_UEFI.fd', 'firmware.dtb', 'shim.bin')]
    for content, digest in ((fd,FD),(dtb,DTB),(shim,SHIM)):
        require(sha(content) == digest, 'Retained firmware input differs')
    require(len(fd) == 7340032 and len(dtb) == 737306 and len(shim) == 112, 'Fixed input size differs')
    require(b[:8] == b'ANDROID!' and 1660 < len(b) <= 100663296, 'Invalid Android image/capacity')
    ks,ka,rs,ra,ss,sa,tags,page,version,osver = struct.unpack_from('<10I',b,8)
    recovery,roff,hsize = struct.unpack_from('<IQI',b,1632)
    ds,da = struct.unpack_from('<IQ',b,1648)
    require(osver == 0 and not any(b[48:576]) and not any(b[608:1632]),
            'Unexpected Android version/name/cmdline fields')
    require((ka,rs,ra,ss,sa,tags,page,version,recovery,roff,hsize,ds,da) ==
            (0x8000,FAT_BYTES,0x1000000,0,0,0x100,4096,2,0,0,1660,len(dtb),0x1f00000), 'Boot header differs')
    aligned = lambda n:(n+page-1)//page*page
    rp = page+aligned(ks)
    dp = rp+aligned(rs)
    require(ks > 0 and len(b) == dp+aligned(ds), 'Invalid image extent')
    require(not any(b[hsize:page]) and not any(b[page+ks:rp]) and not any(b[rp+rs:dp]) and not any(b[dp+ds:]), 'Nonzero padding')
    firmware = b[page:page+ks]
    dec = zlib.decompressobj(16+zlib.MAX_WBITS)
    decoded = dec.decompress(firmware,len(fd)+len(shim)+1)
    require(decoded == shim+fd and dec.eof and not dec.unconsumed_tail and dec.unused_data == dtb, 'Firmware compression/tail differs')
    require(b[rp:rp+rs] == ramdisk and b[dp:dp+ds] == dtb, 'Boot payload differs')
    image_id = hashlib.sha1()
    for raw in (firmware,ramdisk,b'',b'',dtb):
        image_id.update(raw); image_id.update(struct.pack('<I',len(raw)))
    require(b[576:608] == image_id.digest()+bytes(12), 'Android image ID differs')
    files, fat = read_fat32(ramdisk)
    prefix = 'efi/unixnu/'
    names = ['unixnu.toml','payloads/kernel.macho','payloads/kernel.macho.adt','payloads/kernel.macho.trustcache','tools/j716fearlylogprobe.efi']
    require(set(files) == {prefix+n for n in names}, 'Unexpected payload file set')
    require(files[prefix+'unixnu.toml'] == (d/'unixnu.toml').read_bytes(), 'Menu readback differs')
    menu = menu_check(files[prefix+'unixnu.toml'])
    require(sha(files[prefix+'tools/j716fearlylogprobe.efi']) == EFI, 'Early logger EFI differs')
    trust = files[prefix+'payloads/kernel.macho.trustcache']
    require(len(trust) == 61356 and sha(trust) == TRUST, 'Original trust-cache differs')
    require(trust == (d/'trustcache-21H461.bin').read_bytes(), 'Archived trust-cache differs')
    original = (d/'original-kernel.macho').read_bytes()
    expectation = json.loads((d/'kernel-expectation.json').read_text())
    kernel = files[prefix+'payloads/kernel.macho']
    kernel_report = verify_candidate(original,kernel,expectation)
    for relative,spec in expectation['review_evidence'].items():
        evidence=(d/'review-evidence'/relative).resolve(strict=True)
        require(evidence.is_relative_to(d/'review-evidence') and identity(evidence)==spec,
                'Archived kernel review evidence differs: '+relative)
    adt = files[prefix+'payloads/kernel.macho.adt']
    adt_report = compare_adt((d/'baseline.adt').read_bytes(),adt)
    official = json.loads((d/'payload.official.json').read_text())
    for k,v in {'kernel_sha256':sha(original),'payload_kernel_sha256':sha(kernel),'adt_sha256':sha(adt),
                'trust_cache_sha256':TRUST,'data_ramdisk_sha256':None,'gpu_addon_sha256':None,
                'kernel_args':'-v serial=3 j716f-earlylog=1','panic_screen':False,'skip_setup':False,
                'hardware_tested':False,'payload_bytes':FAT_BYTES}.items():
        require(official[k] == v, 'Official payload report differs: '+k)
    return dict(passed=True, image=identity(d/'boot.img'), payload=identity(d/'payload.img'),
                kernel=kernel_report, adt=adt_report, fat=fat, menu_title=menu['ui']['title'],
                trust_cache_sha256=TRUST, hardware_tested=False, device_actions=False,
                storage_runtime_qualified=False, root_ready=False, data_seed_present=False)

if __name__ == '__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('artifact',type=Path);p.add_argument('--report',type=Path,required=True)
    a=p.parse_args()
    r=verify(a.artifact)
    a.report.parent.mkdir(parents=True,exist_ok=True)
    a.report.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps({k:r[k] for k in ('passed','image','hardware_tested','root_ready')}))
