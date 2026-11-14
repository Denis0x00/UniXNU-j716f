"""Verify the inactive bundle reached both official linked images, without execution."""
from collections import Counter
from pathlib import Path
import argparse,copy,hashlib,json,shutil,struct,subprocess
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,required=True);p.add_argument('--attempt',type=int,required=True)
a=p.parse_args();r=a.root.resolve(strict=True);assert a.attempt>0
dest=r/'analysis/geni-backend-20261113'/f'integration-{a.attempt:02d}.json';assert not dest.exists()
out=r/'UniXNU/out-host-geni-20261113/j716f-host-geni/ipados-17.7.11-21H461/j716f_host_geni'
tree=out/'tree';sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
report={'status':'running','runtime_qualified':False,'device_actions':False,'qemu':False,'images':[]}
try:
 manifest_path=r/'work/UniXNU-j716f-host-geni/docs/host-source-manifest.json'
 manifest=json.loads(manifest_path.read_text());assert manifest['prepare_only'] is False
 for rel,spec in manifest['inputs'].items():assert sha(r/rel)==spec['sha256'],rel
 bundle=json.loads((tree/'build/j716f-geni/result.json').read_text());assert bundle['status']=='passed'
 assert bundle['manifest_sha256']==sha(manifest_path)
 archive=Path(bundle['archive']);assert sha(archive)==bundle['archive_sha256']
 expected=set(bundle['defined_symbols']);assert len(expected)>=20
 assert all(n==1 for n in bundle['defined_symbols'].values())
 nm=shutil.which('llvm-nm');assert nm
 for file in (tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf',out/'kernel/kernel.macho'):
  data=file.read_bytes();assert struct.unpack_from('<4I',data)==(0xfeedfacf,0x100000c,0,2)
  ncmd,size=struct.unpack_from('<II',data,16);assert 32+size<=len(data)
  at=32;executable=[];instruction_sections=[]
  for _ in range(ncmd):
   cmd,length=struct.unpack_from('<II',data,at);assert length>=8 and at+length<=32+size
   if cmd==0x19:
    vmaddr,vmsize,fileoff,filesize=struct.unpack_from('<4Q',data,at+24)
    maxprot,initprot=struct.unpack_from('<II',data,at+56)
    assert fileoff+filesize<=len(data)
    if initprot&4:
     executable.append((vmaddr,vmaddr+filesize,fileoff))
     nsects=struct.unpack_from('<I',data,at+64)[0]
     assert 72+nsects*80<=length
     for index in range(nsects):
      section=at+72+index*80
      address,extent,offset=struct.unpack_from('<QQI',data,section+32)
      flags=struct.unpack_from('<I',data,section+64)[0]
      if flags&0x80000000:  # S_ATTR_PURE_INSTRUCTIONS
       assert vmaddr<=address and address+extent<=vmaddr+filesize
       assert offset==fileoff+address-vmaddr and offset+extent<=len(data)
       instruction_sections.append((address,address+extent,offset))
   at+=length
  assert at==32+size and executable and instruction_sections
  result=subprocess.run([nm,'--defined-only','--extern-only','--format=posix',str(file)],capture_output=True,text=True,check=True)
  symbols={};counts=Counter()
  for line in result.stdout.splitlines():
   fields=line.split()
   if len(fields)>=3 and fields[0] in expected:
    name,kind,address=fields[:3];counts[name]+=1;symbols[name]={'kind':kind,'address':int(address,16)}
  assert set(counts)==expected and all(n==1 for n in counts.values())
  for name,entry in symbols.items():
   # LLVM nm labels kernel __TEXT_EXEC symbols S instead of ordinary __TEXT T.
   # Derive executable code from actual section flags and file-backed ranges.
   assert entry['kind'] in ('T','S') and any(lo<=entry['address']<hi for lo,hi,_ in instruction_sections),name
   lo,hi,offset=next(x for x in instruction_sections if x[0]<=entry['address']<x[1])
   position=offset+entry['address']-lo
   assert position+4<=len(data)
   entry['first_instruction']=data[position:position+4].hex()
   entry['address']=hex(entry['address'])
  report['images'].append({'path':str(file),'bytes':len(data),'sha256':sha(file),'symbols':symbols})
 records=sorted((r/'analysis/host-geni-20261113').glob('kernel-*.json'))
 passed=[json.loads(x.read_text()) for x in records if json.loads(x.read_text()).get('status')=='passed'];assert passed
 official=passed[-1]
 record_path=tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_init.o.json'
 def bind_official(value):
  assert value['target']=='j716f-host-geni' and value['output']==str(out)
  assert value['upstream_revision']==manifest['upstream_revision']
  assert value['target_source_manifest']=={'path':str(manifest_path),'sha256':sha(manifest_path)}
  assert value['dt_panic_compile']['path']==str(record_path)
  assert value['dt_panic_compile']['sha256']==bundle['compile_record_sha256']==sha(record_path)
  expected_artifacts=[{k:image[k] for k in ('path','bytes','sha256')} for image in report['images']]
  assert value['artifacts']==expected_artifacts
  assert value['retained_inputs_unchanged'] is True
  assert value['commands'] and all(c.get('exit_code')==0 for c in value['commands'])
 bind_official(official)
 # A correct image list must not accept a different run's success receipt.
 corruptions=[]
 for field in ('path','bytes','sha256'):
  c=copy.deepcopy(official);c['artifacts'][0][field]=0 if field=='bytes' else 'wrong';corruptions.append(c)
 for parent,field in (('target_source_manifest','sha256'),('dt_panic_compile','sha256')):
  c=copy.deepcopy(official);c[parent][field]='wrong';corruptions.append(c)
 c=copy.deepcopy(official);c['output']='wrong';corruptions.append(c)
 c=copy.deepcopy(official);c['commands'][0]['exit_code']=1;corruptions.append(c)
 for wrong in corruptions:
  try:bind_official(wrong)
  except AssertionError:pass
  else:raise AssertionError('Corrupted official receipt was accepted')
 report.update(status='passed',manifest_sha256=sha(manifest_path),archive_sha256=sha(archive),
               linked_bundle_exports=len(expected),official_kernel_report=official,
               receipt_corruption_checks=len(corruptions),
               activation='No registration caller or boot entry; bundle is force-linked and inactive.')
except Exception as exc:report.update(status='failed',error=str(exc))
dest.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k not in ('images','official_kernel_report')},indent=2))
raise SystemExit(0 if report['status']=='passed' else 1)
