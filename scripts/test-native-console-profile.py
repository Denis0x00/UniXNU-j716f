#!/usr/bin/env python3
"""Compile actual generated firmware menu guard for BOTH profiles; no hardware."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile
import tomllib
from native_candidate import load_module

STUB=r'''
#include <assert.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <stdio.h>
#define STATIC static
#define CONST const
#define CHAR8 char
#define EFI_SECURITY_VIOLATION 1
#define UniXNUConfigLoad(i,c) ((void)0)
#define AsciiStrCmp strcmp
#define StrCmp wcscmp
typedef size_t UINTN;
enum { UniXNUEntryEfi, UniXNUEntryXnu, UniXNUEntryAction, UniXNUActionExit };
typedef struct { const char *Id; int Type,Action; const wchar_t *Path,*Args; } Entry;
typedef struct { int Error[1],EntryCount,DefaultIndex,Timeout,DriverCount,PatchCount,AcpiCount,SliceCount,KitsLoaded;Entry Entries[6]; } Configuration;
static int check(Configuration Config) {
  UINTN Index;
GUARD
  return 0;
}
int main(void) {
  Configuration original={0};original.Timeout=-1;original.EntryCount=COUNT;
ENTRIES
  assert(!check(original));
  Configuration c=original;c.EntryCount--;assert(check(c));
  c=original;c.DefaultIndex=1;assert(check(c));
  c=original;c.Timeout=0;assert(check(c));
  c=original;c.Error[0]=1;assert(check(c));
  c=original;c.DriverCount=1;assert(check(c));
  c=original;c.PatchCount=1;assert(check(c));
  c=original;c.AcpiCount=1;assert(check(c));
  c=original;c.SliceCount=1;assert(check(c));
  c=original;c.KitsLoaded=1;assert(check(c));
  for(int i=0;i<COUNT;i++) {
    c=original;c.Entries[i].Id=NULL;assert(check(c));
    c=original;c.Entries[i].Id="wrong";assert(check(c));
    c=original;c.Entries[i].Type=99;assert(check(c));
    if(original.Entries[i].Path) {
      c=original;c.Entries[i].Path=NULL;assert(check(c));
      c=original;c.Entries[i].Path=L"\\wrong";assert(check(c));
    }
    if(original.Entries[i].Args) {
      c=original;c.Entries[i].Args=NULL;assert(check(c));
      c=original;c.Entries[i].Args=L"unsafe";assert(check(c));
    }
  }
  c=original;c.Entries[COUNT-1].Action=99;assert(check(c));
  puts("PASS generated menu guard: exact profile, order/types/paths/args/action, no injection or countdown");
}
'''

def main():
    p=argparse.ArgumentParser();p.add_argument('--cc',default='cc');args=p.parse_args()
    payload=load_module('build-native-payload');firmware=load_module('prepare-native-firmware')
    with tempfile.TemporaryDirectory(prefix='native-profile-') as folder:
        work=Path(folder)
        for enabled in (False,True):
            config=tomllib.loads(payload.config(enabled));entries=config['entry']
            assert [e['id'] for e in entries]==(['reader','export','init','arm','native','exit'] if enabled else ['reader','arm','native','exit'])
            assert config['boot']=={'timeout':-1,'fallback_timeout':-1,'rotate':0,'connect_all':False,'default':'reader'}
            types={'efi':'UniXNUEntryEfi','xnu':'UniXNUEntryXnu','action':'UniXNUEntryAction'}
            lines=[]
            for i,e in enumerate(entries):
                path='L'+json.dumps(e['path']) if 'path' in e else 'NULL'
                arg='L'+json.dumps(e['args']) if 'args' in e else 'NULL'
                lines.append(f'original.Entries[{i}]=(Entry){{{json.dumps(e["id"])},{types[e["type"]]},UniXNUActionExit,{path},{arg}}};')
            text=STUB.replace('GUARD',firmware.menu_guard(enabled)).replace('ENTRIES','\n'.join(lines)).replace('COUNT',str(len(entries)))
            source=work/f'guard-{enabled}.c';source.write_text(text)
            binary=source.with_suffix('')
            subprocess.run([args.cc,'-std=c11','-Wall','-Wextra','-Werror',str(source),'-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True)
    print('PASS: legacy four-entry and explicit RAW six-entry profiles; no hardware or image overwrite')

if __name__=='__main__':main()
