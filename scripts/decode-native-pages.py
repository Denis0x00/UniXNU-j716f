#!/usr/bin/env python3
"""Reconstruct all nonzero raw pages transcribed from the frozen EFI reader.

JSON: candidate (hex digest), dbgc (3 integers), stable (true), header (16 hex
uint64 strings), present_count, records [{slot, words:[16 hex strings]}],
session_crc32 (8 hex digits). The entire console CRC cannot be reconstructed
from these pages, and is never claimed independently verified.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import re
import struct
import zlib


def raw_words(words):
    if (not isinstance(words,list) or len(words)!=16 or
        any(not isinstance(w,str) or not re.fullmatch('[0-9a-fA-F]{16}',w) for w in words)):
        raise ValueError('Each page must contain exactly sixteen full hex uint64 words')
    return struct.pack('<16Q',*[int(w,16) for w in words])


def reconstruct(pages,candidate,session):
    if pages.get('candidate')!=candidate or pages.get('stable') is not True:
        raise ValueError('Wrong candidate or unstable reader acquisition')
    header=raw_words(pages['header'])
    if struct.unpack_from('<Q',header,48)[0]!=int(session,16):
        raise ValueError('Session must match the independently photographed ARM session')
    dbgc=pages['dbgc']
    if not isinstance(dbgc,list) or len(dbgc)!=3 or any(type(v)!=int or not 0<=v<2**32 for v in dbgc):
        raise ValueError('Expected complete DBGC header')
    offset=struct.unpack_from('<I',header,56)[0]
    if offset%8 or offset<16 or offset+8192>0x40000 or offset+8192>12+dbgc[1]:
        raise ValueError('Out-of-bounds/uncommitted session')
    if pages['present_count']!=len(pages['records']):raise ValueError('Missing/extra record pages')
    data=bytearray(0x40000);struct.pack_into('<3I',data,0,*dbgc)
    data[offset:offset+128]=header;seen=set()
    for r in pages['records']:
        slot=r['slot'];raw=raw_words(r['words'])
        if type(slot)!=int or not 0<=slot<63 or slot in seen or not any(raw):
            raise ValueError('Invalid/duplicate/zero record page')
        seen.add(slot);at=offset+128*(slot+1);data[at:at+128]=raw
    if zlib.crc32(data[offset:offset+8192])!=int(pages['session_crc32'],16):
        raise ValueError('Reconstructed session CRC differs; preserve photos and correct transcription')
    spec=importlib.util.spec_from_file_location('native_decode',Path(__file__).with_name('decode-native-log.py'))
    decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
    result=decoder.decode(bytes(data),candidate,session)
    result['acquisition']='manually-transcribed-frozen-reader-pages'
    result['session_crc_independently_reconstructed']=True
    result['whole_console_crc_independently_reconstructed']=False
    return result


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('pages',type=Path)
    p.add_argument('--candidate',required=True);p.add_argument('--session',required=True)
    a=p.parse_args();print(json.dumps(reconstruct(json.loads(a.pages.read_text()),a.candidate,a.session),indent=2))
