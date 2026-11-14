"""Finalize hashes and acceptance after host tests and official integration."""
from pathlib import Path
import argparse,hashlib,json,re
p=argparse.ArgumentParser();p.add_argument('--integration-attempt',type=int,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[2];d=r/'analysis/geni-backend-20261113'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
freeze=d/'source-freeze-02.json';inputs=json.loads(freeze.read_text(encoding='utf-8'))
assert not inputs['prepare_only']
for rel,spec in inputs['inputs'].items():assert sha(r/rel)==spec['sha256'],rel
host=json.loads((d/'host-02/result.json').read_text(encoding='utf-8'));assert host['status']=='passed'
for rel,digest in host['inputs'].items():assert sha(r/rel)==digest,rel
integration=json.loads((d/f'integration-{a.integration_attempt:02d}.json').read_text(encoding='utf-8'))
assert integration['status']=='passed' and integration['manifest_sha256']==sha(freeze)
preserved=json.loads((d/'preserved-after.json').read_text(encoding='utf-8'));assert preserved['status']=='passed'
art=r/'artifacts/j716f-host-geni-20261113';artmanifest=json.loads((art/'manifest.json').read_text(encoding='utf-8'))
for rel,spec in artmanifest['files'].items():assert sha(art/rel)==spec['sha256'],rel
checks={name:int(re.search(r'(\d+) checks passed',text).group(1)) for name,text in host['test_outputs'].items()}
report={'status':'passed','scope':'bounded FIFO source implementation and inactive official kernel link',
        'source_freeze_sha256':sha(freeze),'frozen_files':len(inputs['inputs']),
        'host_checks':checks,'total_checks':sum(checks.values()),'sanitizers':['address','undefined'],
        'ordinary_arm64_objects':len(host['objects']),'linked_bundle_exports':integration['linked_bundle_exports'],
        'preserved_files':preserved['checked_files'],'official_flow':{'components':5,'patches':59,'archive_recipes':9,'native':True,'prelink':True},
        'artifacts':artmanifest['files'],'runtime_qualified':False,'boot_verified':False,'automatic_activation':False,
        'device_actions':False,'qemu':False,'onsite_next':'Observe the already deployed r6; no new image produced.',
        'remaining':['live Device mapping and fault-aware accesses','exclusive SE/DMA/power/clock/pin ownership',
                     'current FIFO mode and polling qualification','external electrical/device terminal and full bus fence',
                     'attention scheduling and real HID sink']}
(d/'acceptance.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
roots=['analysis/geni-backend-20261113','analysis/stock-geni-20261113','analysis/host-geni-20261113',
       'work/J716fGeniBackend-20261113','work/UniXNU-j716f-host-geni','artifacts/j716f-host-geni-20261113']
files={}
for rel in roots:
 for path in (r/rel).rglob('*'):
  if path.is_file() and path.name not in ('final-sync-manifest.json','sync-windows.json','sync-vm.json') and '__pycache__' not in path.parts:
   files[path.relative_to(r).as_posix()]={'bytes':path.stat().st_size,'sha256':sha(path)}
files['analysis/tools/run_host_geni.py']={'bytes':(r/'analysis/tools/run_host_geni.py').stat().st_size,'sha256':sha(r/'analysis/tools/run_host_geni.py')}
(d/'final-sync-manifest.json').write_text(json.dumps({'files':files},indent=2)+'\n',encoding='utf-8')
print(json.dumps({'status':'passed','checks':sum(checks.values()),'delivery_files':len(files)}))
