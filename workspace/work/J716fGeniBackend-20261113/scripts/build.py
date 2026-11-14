#!/usr/bin/env python3
"""Build the inactive GENI bundle from a fresh official XNU C compile record."""
import argparse
from collections import Counter
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

UNITS = ['J716fGoodix-20261113/J716fGoodix.c',
         'J716fTouchTransaction-20261113/J716fTouchTransaction.c',
         'J716fGeniBackend-20261113/J716fGeniBackend.c',
         'J716fGeniBackend-20261113/J716fGeniBridge.c',
         'J716fGeniBackend-20261113/J716fGeniMmio.c']
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def save(p,v): p.write_text(json.dumps(v,indent=2)+'\n')
def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--tree',type=Path,required=True)
 p.add_argument('--source',type=Path,required=True)
 a=p.parse_args();tree=a.tree.resolve(strict=True);source=a.source.resolve(strict=True)
 device=Path(os.environ['UNIXNU_DEVICE']).resolve(strict=True);root=device.parents[1]
 assert device.name=='UniXNU-j716f-host-geni' and source==root/'work/J716fGeniBackend-20261113'
 manifest_path=device/'docs/host-source-manifest.json'
 manifest=json.loads(manifest_path.read_text());manifest_sha=sha(manifest_path)
 assert manifest.get('prepare_only') is False,'Full backend source freeze is required for compilation'
 for rel,spec in manifest['inputs'].items():
  path=(root/rel).resolve(strict=True)
  assert path.is_relative_to(root) and path.stat().st_size==spec['bytes'] and sha(path)==spec['sha256'],rel
 record_path=tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_init.o.json'
 record=json.loads(record_path.read_text().strip().rstrip(','));record_sha=sha(record_path)
 directory=Path(record['directory']).resolve(strict=True);assert directory.is_relative_to(tree)
 out=tree/'build/j716f-geni';out.mkdir(parents=True,exist_ok=True)
 archive=out/'libJ716fGeniBackend.a'
 attempt=out/'attempts'/datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
 snap=attempt/'source';snap.mkdir(parents=True)
 report={'status':'running','automatic_activation':False,'runtime_qualified':False,
         'manifest_sha256':manifest_sha,'compile_record_sha256':record_sha,'objects':[],
         'source_sha256':{},'commands':[]}
 if archive.exists(): shutil.copyfile(archive,attempt/'previous-archive.a');archive.unlink()
 def run(argv,label):
  result=subprocess.run([str(x) for x in argv],cwd=directory,capture_output=True,text=True)
  (attempt/(label+'.log')).write_text(result.stdout+result.stderr)
  report['commands'].append({'argv':[str(x) for x in argv],'exit_code':result.returncode,'log':label+'.log'})
  assert result.returncode==0,label+': '+result.stderr[-3000:]
  return result.stdout
 try:
  for rel,spec in manifest['inputs'].items():
   if any(rel.startswith('work/'+folder+'/') for folder in ('J716fGoodix-20261113','J716fTouchTransaction-20261113','J716fGeniBackend-20261113')):
    dst=snap/rel;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(root/rel,dst)
    assert sha(dst)==spec['sha256'];report['source_sha256'][rel]=sha(dst)
  args=record['arguments'];assert args.count(record['file'])==args.count('-o')==1
  objects=[];defined=Counter();undefined=set();nm=shutil.which('llvm-nm');assert nm
  for unit in UNITS:
   src=snap/'work'/unit;obj=attempt/(src.stem+'.o');command=[];i=0
   while i<len(args):
    value=args[i]
    if value==record['file']:command.append(str(src))
    elif value=='-o':command.extend(['-o',str(obj)]);i+=1
    elif value in ('-MF','-MJ','-MT','-MQ'):i+=1
    elif value=='-Xclang' and i+1<len(args) and args[i+1].startswith('-W'):i+=1
    elif value.startswith(('-W','-std=')):pass
    else:command.append(value)
    i+=1
   command+=['-std=c11','-Wall','-Wextra','-Werror','-mgeneral-regs-only','-fstack-usage','-Wframe-larger-than=4096']
   run(command,src.stem+'-compile')
   assert struct.unpack_from('<4I',obj.read_bytes())==(0xfeedfacf,0x100000c,0,1)
   defs={x.split()[-1] for x in run([nm,'--defined-only','--extern-only',obj],src.stem+'-defined').splitlines() if x.strip()}
   undefs={x.split()[-1] for x in run([nm,'--undefined-only',obj],src.stem+'-undefined').splitlines() if x.strip()}
   defined.update(defs);undefined|=undefs;objects.append(obj)
   usage=obj.with_suffix('.su');assert usage.is_file()
   stack=[int(x.rsplit('\t',2)[1]) for x in usage.read_text().splitlines() if x.strip()]
   assert stack and max(stack)<=4096
   report['objects'].append({'name':obj.name,'sha256':sha(obj),'defined':sorted(defs),
                             'undefined':sorted(undefs),'max_stack':max(stack)})
  assert all(count==1 for count in defined.values())
  assert {'_jb_init','_jb_start','_jb_poll','_jb_bridge_step','_jbm_bind','_jt_init','_jg_decode'} <= set(defined)
  external=undefined-set(defined);assert external<= {'_bzero','_memset','_memcpy','___stack_chk_guard','___stack_chk_fail'},external
  temporary=attempt/archive.name
  run(['xcrun','ar','rcs',temporary,*objects],'archive')
  listing=run(['xcrun','ar','t',temporary],'archive-members').splitlines()
  assert [x for x in listing if not x.startswith('__.SYMDEF')]==[x.name for x in objects]
  assert sha(record_path)==record_sha and sha(manifest_path)==manifest_sha
  for rel,spec in manifest['inputs'].items():assert sha(root/rel)==spec['sha256'],rel
  shutil.copyfile(temporary,archive)
  report.update(status='passed',archive_sha256=sha(archive),external_symbols=sorted(external),
                defined_symbols=dict(defined),archive=str(archive),attempt=str(attempt))
 except Exception as exc:
  report.update(status='failed',error=str(exc));raise
 finally:
  save(attempt/'result.json',report);save(out/'result.json',report)
 print(json.dumps({'status':'passed','archive':str(archive),'sha256':sha(archive)}))
if __name__=='__main__':main()
