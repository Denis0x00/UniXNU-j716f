"""Run backend/bridge/resource-boundary tests with sanitizers and compile ARM64."""
from pathlib import Path
import argparse,hashlib,json,shutil,struct,subprocess,sys
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,required=True);p.add_argument('--attempt',type=int,required=True)
a=p.parse_args();assert sys.platform=='linux' and a.attempt>0
r=a.root.resolve(strict=True);out=r/'analysis/geni-backend-20261113'/f'host-{a.attempt:02d}'
assert not out.exists();out.mkdir(parents=True)
folders=['J716fGoodix-20261113','J716fTouchTransaction-20261113','J716fGeniBackend-20261113']
files=[p for name in folders for p in (r/'work'/name).rglob('*') if p.is_file()]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
inputs={str(p.relative_to(r)):sha(p) for p in files}
snapshot=out/'source'
for rel,digest in inputs.items():
 dst=snapshot/rel;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(r/rel,dst);assert sha(dst)==digest
src=[r/'work'/folders[0]/'J716fGoodix.c',r/'work'/folders[1]/'J716fTouchTransaction.c']
src += [r/'work'/folders[2]/n for n in ('J716fGeniBackend.c','J716fGeniBridge.c','J716fGeniMmio.c')]
src=[snapshot/p.relative_to(r) for p in src]
report={'status':'running','inputs':inputs,'commands':[],'device_actions':False,'qemu':False,'runtime_qualified':False}
def run(argv,label):
 result=subprocess.run([str(x) for x in argv],capture_output=True,text=True)
 (out/(label+'.log')).write_text(result.stdout+result.stderr)
 report['commands'].append({'argv':[str(x) for x in argv],'exit_code':result.returncode,'log':label+'.log'})
 assert result.returncode==0,label+': '+result.stdout[-2000:]+result.stderr[-4000:]
 return result.stdout
try:
 report['test_outputs']={}
 tests=sorted((snapshot/'work'/folders[2]/'tests').glob('test_*.c'));assert len(tests)>=2
 for test in tests:
  exe=out/test.stem
  run(['clang','-std=c11','-Wall','-Wextra','-Werror','-pedantic','-fsanitize=address,undefined','-fno-omit-frame-pointer',*src,test,'-o',exe],test.stem+'-compile')
  report['test_outputs'][test.name]=run([exe],test.stem)
 compiler='/opt/unixnu-linux/llvm/bin/clang-24';nm=shutil.which('llvm-nm');ar=shutil.which('llvm-ar');assert nm and ar
 defined=set();undefined=set();objects=[];report['objects']=[]
 for source in src:
  obj=out/(source.stem+'.o')
  run([compiler,'-std=c11','--target=arm64-apple-ios17.0','-mgeneral-regs-only','-ffreestanding','-fno-builtin','-fno-stack-protector','-O2','-Wall','-Wextra','-Werror','-pedantic','-c',source,'-o',obj],source.stem+'-compile')
  assert struct.unpack_from('<4I',obj.read_bytes())==(0xfeedfacf,0x100000c,0,1)
  defs={x.split()[-1] for x in run([nm,'--defined-only','--extern-only',obj],source.stem+'-defined').splitlines() if x.strip()}
  undefs={x.split()[-1] for x in run([nm,'--undefined-only',obj],source.stem+'-undefined').splitlines() if x.strip()}
  assert not defined&defs;defined|=defs;undefined|=undefs;objects.append(obj)
  report['objects'].append({'name':obj.name,'sha256':sha(obj),'defined':sorted(defs),'undefined':sorted(undefs)})
 external=undefined-defined;assert external<= {'_bzero','_memcpy','_memset'},external
 archive=out/'libJ716fGeniBackend.a';run([ar,'rcs',archive,*objects],'archive')
 report['archive']={'sha256':sha(archive),'bytes':archive.stat().st_size,'external_symbols':sorted(external)}
 assert all(sha(r/name)==digest for name,digest in inputs.items()),'source changed during run'
 report['status']='passed'
except Exception as exc:report.update(status='failed',error=str(exc))
(out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
sys.exit(0 if report['status']=='passed' else 1)
