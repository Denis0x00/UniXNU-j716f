"""Independent readback of the fixed 8MiB TB-J716F discovery artifact."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import struct
import tomllib
import zlib

from verify_menu_artifact import verify as verify_baseline, require, sha256, load_json


def read_fat16(data):
    require(len(data) == 8 * 1024 * 1024 and data[510:512] == b'\x55\xaa', 'Invalid probe FAT boot sector')
    bps, spc, reserved, fats, roots, total16, media, spf = struct.unpack_from('<HBHBHHBH', data, 11)
    total = total16 or struct.unpack_from('<I', data, 32)[0]
    require(bps == 512 and spc == 1 and reserved > 0 and fats == 2 and roots > 0 and spf > 0, 'Unsupported FAT geometry')
    require(media == 0xf8 and data[38] == 0x29 and data[43:54] == b'J716FPROBE'.ljust(11, b' '), 'Unexpected FAT identity fields')
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
                files[path]=content[:size]
    root_start=(reserved+fats*spf)*bps
    directory(data[root_start:root_start+roots*32])
    return files, {'bytes_per_sector':bps,'sectors_per_cluster':spc,'data_clusters':clusters,'volume_id':hex(struct.unpack_from('<I',data,39)[0])}


def verify(root, baseline, revision=1):
    require(revision in (1,2), 'Unsupported probe revision')
    old = verify_baseline(baseline)
    require(old['image']['image_sha256'] == 'f8b82f6da135ba236dc7bd959af039cda05f9ffcc81ae16c73d212fd9dfd9479', 'Wrong baseline image')
    root=root.resolve()
    manifest=load_json(root/'manifest.json')['outputs']
    required={'boot.img','boot.json','SILICIUM_UEFI.fd','firmware.dtb','shim.bin','uefi-config.toml','board.toml','probe.img','unixnu.toml','probe.txt'}
    require(required <= manifest.keys(), 'Incomplete probe manifest')
    for name,spec in manifest.items():
        rel=PurePosixPath(name)
        require(name and not rel.is_absolute() and '..' not in rel.parts and ':' not in name and '\\' not in name, 'Unsafe manifest path')
        path=(root/name).resolve(strict=True)
        require(path.is_relative_to(root) and path.is_file(), 'Manifest path escapes artifact')
        raw=path.read_bytes()
        require(len(raw)==spec['bytes'] and sha256(raw)==spec['sha256'], f'Manifest mismatch {name}')
    for name in ['SILICIUM_UEFI.fd','firmware.dtb','shim.bin','uefi-config.toml']:
        require((root/name).read_bytes()==(baseline/name).read_bytes(), f'Baseline changed: {name}')
    data=(root/'boot.img').read_bytes(); original=(baseline/'boot.img').read_bytes()
    ramdisk=(root/'probe.img').read_bytes(); dtb=(root/'firmware.dtb').read_bytes()
    require(data[:8]==b'ANDROID!' and len(data)==10211328 and len(data)<=100663296,'Unexpected image size or magic')
    ks,ka,rs,ra,ss,sa,tags,page,version,osver=struct.unpack_from('<10I',data,8)
    recovery,roff,hsize=struct.unpack_from('<IQI',data,1632)
    ds,da=struct.unpack_from('<IQ',data,1648)
    require((ka,rs,ra,ss,sa,tags,page,version,recovery,roff,hsize,ds,da)==
            (0x8000,8388608,0x1000000,0,0,0x100,4096,2,0,0,1660,len(dtb),0x1f00000),'Boot header fields differ from fixed probe scope')
    for lo,hi in [(0,16),(24,576),(608,1660)]:
        require(data[lo:hi] == original[lo:hi], 'Unexpected header mutation outside ramdisk fields/image ID')
    aligned=lambda n:(n+page-1)//page*page
    rp=page+aligned(ks); dp=rp+aligned(rs)
    require(len(data)==dp+aligned(ds),'Invalid boot section layout')
    require(not any(data[hsize:page]) and not any(data[page+ks:rp]) and not any(data[rp+rs:dp]) and not any(data[dp+ds:]),'Nonzero section padding')
    kernel=data[page:page+ks]
    old_ks=struct.unpack_from('<I',original,8)[0]
    require(ks==old_ks and kernel==original[page:page+old_ks], 'Compressed shim/FD/DTB changed from verified baseline')
    require(data[rp:rp+rs]==ramdisk and data[dp:dp+ds]==dtb, 'Ramdisk or DTB bytes differ')
    digest=hashlib.sha1()
    for part in [kernel,ramdisk,b'',b'',dtb]:
        digest.update(part); digest.update(struct.pack('<I',len(part)))
    require(data[576:608]==digest.digest()+b'\0'*12,'Android image ID mismatch')
    board=tomllib.loads((root/'board.toml').read_text(encoding='utf-8'))
    require(board['board']['name']=='j716f-initrd-probe' and board['package']['boot_partition_bytes']==100663296,'Incorrect target or boot capacity')
    files, geometry=read_fat16(ramdisk)
    require(set(files)=={'efi/unixnu/unixnu.toml','probe.txt'},'Unexpected FAT file set')
    for internal,external in [('efi/unixnu/unixnu.toml','unixnu.toml'),('probe.txt','probe.txt')]:
        require(files[internal]==(root/external).read_bytes(), f'FAT content mismatch {internal}')
    cfg=tomllib.loads(files['efi/unixnu/unixnu.toml'].decode('utf-8'))
    require(set(cfg)=={'version','boot','ui','kits','entry'} and cfg['version']==1, 'Unexpected runtime sections')
    require(cfg['kits']=={'autoload':False}, 'Automatic kit loading must remain disabled')
    title='J716F 8 MiB initrd probe' + (' r2' if revision == 2 else '')
    require(cfg['ui']['title']==title and cfg['boot']['timeout']==-1, 'Probe marker/hold configuration missing')
    require(cfg['boot']['rotate']==0 and cfg['boot']['default']=='tools', 'Unexpected default or automatic rotation')
    allowed={'tools','exit'} if revision == 1 else {'tools','exit','reset','shutdown'}
    require(len(cfg['entry'])==len(allowed) and {e.get('action') for e in cfg['entry']}==allowed, 'Wrong probe actions')
    require(all(e.get('type')=='action' and not (set(e)&{'path','initrd','kext','args'}) for e in cfg['entry']), 'Executable or unexpected entry input')
    require(geometry['volume_id']=='0x716f0008','Wrong probe volume identity')
    report=load_json(root/'boot.json')
    expected={'image_sha256':sha256(data),'image_bytes':len(data),'ramdisk_bytes':len(ramdisk),'header_version':2,'kernel_address':hex(ka),'tags_address':hex(tags),'dtb_address':hex(da),'dtb_file_offset':dp,'kernel_sha256':sha256(kernel),'fd_sha256':sha256((root/'SILICIUM_UEFI.fd').read_bytes()),'dtb_sha256':sha256(dtb),'dtb_bytes':len(dtb),'shim_bytes':112,'hardware_tested':False}
    for key,value in expected.items():
        require(report.get(key)==value, f'boot.json mismatch {key}')
    return {'result':'passed','revision':revision,'menu_title':title,'menu_actions':sorted(allowed),
            'scope':'host byte/layout/config validation only; non-XNU 8MiB discovery probe',
            'image':expected,'ramdisk_address':hex(ra),'ramdisk_sha256':sha256(ramdisk),'ramdisk_crc32':f'{zlib.crc32(ramdisk):08x}',
            'nominal_ramdisk_end':hex(ra+rs),'nominal_dtb_start':hex(da),'nominal_overlap':ra<=da<ra+rs,
            'baseline_kernel_identical':True,'fat':geometry,'files':{k:{'bytes':len(v),'sha256':sha256(v)} for k,v in files.items()},
            'manifest_entries_verified':len(manifest),'hardware_tested':False,
            'limits':['Header addresses are not measured ABL placements.','No runtime full-buffer checksum, BootShim source address, 80MiB layout, EBS or XNU verification.']}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('artifact',type=Path);p.add_argument('baseline',type=Path);p.add_argument('--report',type=Path,required=True)
    p.add_argument('--revision',type=int,choices=(1,2),default=1)
    a=p.parse_args()
    require(not a.report.exists(),'Refusing to overwrite a verification report')
    result=verify(a.artifact,a.baseline,a.revision)
    a.report.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(result,indent=2))
