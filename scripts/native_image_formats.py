"""Independent FAT32 reader for the private entry volume; never mounts it."""
import hashlib
import struct
import tomllib
import lzma
import uuid


def fv_files(data):
    if data[40:44]!=b'_FVH':raise ValueError('Expected one FV header')
    size=struct.unpack_from('<Q',data,32)[0];header=struct.unpack_from('<H',data,48)[0]
    if size!=len(data) or header<56 or header%2 or header>size:raise ValueError('FV bounds')
    if sum(struct.unpack_from('<'+'H'*(header//2),data))&0xffff:raise ValueError('FV checksum')
    at=(header+7)&~7;result={}
    while at+24<=size:
        h=data[at:at+24]
        if h==b'\xff'*24:
            if data[at:]!=b'\xff'*(size-at):raise ValueError('Unexpected bytes after FV end')
            break
        short=int.from_bytes(h[20:23],'little');n=32 if h[19]&1 else 24
        length=struct.unpack_from('<Q',data,at+24)[0] if n==32 else short
        if length<n or at+length>size or (n==32 and (short or length<=0xffffff)):
            raise ValueError('FFS bounds')
        if (sum(data[at:at+n])-h[17]-h[23])&255:raise ValueError('FFS checksum')
        if h[18]!=0xf0:
            guid=str(uuid.UUID(bytes_le=h[:16]))
            if guid in result:raise ValueError('Duplicate FFS GUID')
            result[guid]=list(efi_sections(data[at+n:at+length]))
        at=(at+length+7)&~7
    return result


def efi_sections(data):
    at=0
    while at+4<=len(data):
        if not any(data[at:]):return
        length=int.from_bytes(data[at:at+3],'little');kind=data[at+3];header=4
        if length==0xffffff:length=struct.unpack_from('<I',data,at+4)[0];header=8
        if length<header or at+length>len(data):raise ValueError('EFI section bounds')
        body=data[at+header:at+length]
        if kind==2:
            if body[:16]!=uuid.UUID('ee4e5898-3914-4259-9d6e-dc7bd79403cf').bytes_le:
                raise ValueError('Unexpected guided section')
            start,attrs=struct.unpack_from('<HH',body,16)
            if attrs!=1 or start<header+20 or start>=length:raise ValueError('Guided bounds/attributes')
            yield from efi_sections(lzma.decompress(data[at+start:at+length]))
        else:yield kind,body
        at=(at+length+3)&~3


def extract_fd(fd):
    if len(fd) not in (0x700000, 0xb00000, 0x1000000):raise ValueError('FD exceeds the fixed board window')
    outer=fv_files(fd)
    inner=[body for parts in outer.values() for kind,body in parts if kind==0x17]
    sec=[body for parts in outer.values() for kind,body in parts if kind in (0x10,0x12)]
    if len(inner)!=1 or len(sec)!=1 or len(outer)!=2:raise ValueError('Unexpected outer FV selection')
    return inner[0],fv_files(inner[0]),sec[0]

def read_fat32(data):
    assert len(data) == 0x4000000 and data[510:512] == b'\x55\xaa'
    assert data[82:90] == b'FAT32   '
    sector, spc, reserved, copies = struct.unpack_from('<HBHB', data, 11)
    total, fatsz, root = struct.unpack_from('<I', data, 32)[0], struct.unpack_from('<I', data, 36)[0], struct.unpack_from('<I', data, 44)[0]
    assert sector == 512 and spc == 1 and copies == 2 and total*sector == len(data)
    assert struct.unpack_from('<H', data, 22)[0] == 0
    fat = data[reserved*sector:(reserved+fatsz)*sector]
    assert fat == data[(reserved+fatsz)*sector:(reserved+2*fatsz)*sector]
    first_data = reserved+copies*fatsz
    clusters = (total-first_data)//spc
    assert clusters >= 65525
    used = set()
    def chain(first):
        value = bytearray()
        c = first
        while c < 0xffffff8:
            assert 2 <= c < clusters+2 and c not in used
            used.add(c)
            at = (first_data+(c-2)*spc)*sector
            value.extend(data[at:at+spc*sector])
            c = struct.unpack_from('<I', fat, c*4)[0] & 0xfffffff
        return bytes(value)
    files = {}
    def directory(first, prefix=''):
        raw = chain(first)
        lfn = {}
        checksum = None
        expected_slot = 0
        for at in range(0, len(raw), 32):
            e = raw[at:at+32]
            if not e[0]: break
            assert e[0] != 0xe5, 'unexpected deleted entry'
            if e[11] == 0xf:
                slot = e[0] & 0x1f
                if e[0] & 0x40:
                    assert not lfn
                    expected_slot = slot
                    checksum = e[13]
                assert slot == expected_slot and e[13] == checksum
                lfn[slot] = e[1:11]+e[14:26]+e[28:32]
                expected_slot -= 1
                continue
            name = e[:8].decode('ascii').rstrip()
            ext = e[8:11].decode('ascii').rstrip()
            if ext: name += '.'+ext
            if lfn:
                assert expected_slot == 0
                actual = 0
                for byte in e[:11]: actual = (((actual & 1)<<7)+(actual>>1)+byte)&255
                assert actual == checksum
                name = b''.join(lfn[i] for i in sorted(lfn)).decode('utf-16-le').split('\0')[0]
                lfn = {}
            if name in ['.', '..'] or e[11] & 8: continue
            c = (struct.unpack_from('<H', e, 20)[0]<<16)|struct.unpack_from('<H', e, 26)[0]
            path = prefix+'/'+name
            if e[11] & 16: directory(c, path)
            else:
                n = struct.unpack_from('<I', e, 28)[0]
                content = chain(c) if n else b''
                assert n <= len(content) and path not in files
                files[path] = content[:n]
        assert not lfn
    directory(root)
    return files
