#!/usr/bin/env python3
"""Synthetic firmware/FAT parser boundaries; no vendor or kernel input."""
import struct
import unittest
from native_image_formats import efi_sections, extract_fd, fv_files, read_fat32


def fat_fixture():
    data=bytearray(0x4000000)
    data[510:512]=b'\x55\xaa';data[82:90]=b'FAT32   '
    struct.pack_into('<HBHB',data,11,512,1,32,2)
    struct.pack_into('<I',data,32,len(data)//512)
    struct.pack_into('<I',data,36,1024)
    struct.pack_into('<I',data,44,2)
    for base in (32*512,1056*512):
        struct.pack_into('<4I',data,base,0xffffff8,0xfffffff,0xfffffff,0xfffffff)
    at=2080*512
    data[at:at+11]=b'HELLO   TXT';data[at+11]=0x20
    struct.pack_into('<H',data,at+26,3);struct.pack_into('<I',data,at+28,3)
    data[at+512:at+515]=b'abc'
    return data


class FormatsTest(unittest.TestCase):
    def test_valid_small_fv(self):
        data=bytearray(b'\0'*56+b'\xff'*8)
        data[40:44]=b'_FVH';struct.pack_into('<Q',data,32,len(data))
        struct.pack_into('<H',data,48,56)
        checksum=(-sum(struct.unpack_from('<28H',data)))&0xffff
        struct.pack_into('<H',data,50,checksum)
        self.assertEqual(fv_files(data),{})
        data[50]^=1
        with self.assertRaises(ValueError):fv_files(data)

    def test_invalid_fd_size(self):
        with self.assertRaises(ValueError):extract_fd(bytes(4096))

    def test_section_bounds_and_guid(self):
        self.assertEqual(list(efi_sections(b'\x07\0\0\x19abc')),[(0x19,b'abc')])
        for data in (b'\x03\0\0\x19',b'\x08\0\0\x19abc',b'\x14\0\0\x02'+bytes(16)):
            with self.assertRaises(ValueError):list(efi_sections(data))

    def test_fat_file_roundtrip(self):
        self.assertEqual(read_fat32(fat_fixture()),{'/HELLO.TXT':b'abc'})

    def test_fat_corruption_refused(self):
        for mode in ('signature','copy','cycle','length','alias','lfn','cluster'):
            with self.subTest(mode=mode):
                data=fat_fixture();at=2080*512
                if mode=='signature':data[510]=0
                elif mode=='copy':data[32*512+12]^=1
                elif mode in ('cycle','cluster'):
                    for base in (32*512,1056*512):
                        struct.pack_into('<I',data,base+12,3 if mode=='cycle' else 0xffff000)
                elif mode=='length':struct.pack_into('<I',data,at+28,513)
                elif mode=='alias':
                    data[at+32:at+64]=data[at:at+32];data[at+32:at+37]=b'OTHER'
                else:data[at+11]=0xf
                with self.assertRaises((AssertionError,ValueError,struct.error)):
                    read_fat32(data)


if __name__=='__main__':unittest.main()
