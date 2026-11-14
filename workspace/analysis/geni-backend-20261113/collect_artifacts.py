"""Archive only the accepted new GENI build, never a boot image or device payload."""
from pathlib import Path
import argparse,hashlib,json,shutil
p=argparse.ArgumentParser();p.add_argument('--root',type=Path,required=True);p.add_argument('--integration-attempt',type=int,required=True)
a=p.parse_args();r=a.root.resolve(strict=True)
verification=r/'analysis/geni-backend-20261113'/f'integration-{a.integration_attempt:02d}.json'
review=json.loads(verification.read_text());assert review['status']=='passed'
dest=r/'artifacts/j716f-host-geni-20261113';assert not dest.exists();dest.mkdir()
tree=r/'UniXNU/out-host-geni-20261113/j716f-host-geni/ipados-17.7.11-21H461/j716f_host_geni/tree'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
copies={}
for image,name in zip(review['images'],('native.macho','kernel.macho')):
 path=Path(image['path']);assert sha(path)==image['sha256'];copies[name]=path
copies.update({'libJ716fGeniBackend.a':tree/'build/j716f-geni/libJ716fGeniBackend.a',
               'bundle-result.json':tree/'build/j716f-geni/result.json',
               'source-freeze.json':r/'work/UniXNU-j716f-host-geni/docs/host-source-manifest.json',
               'integration.json':verification,
               'host-tests.json':r/'analysis/geni-backend-20261113/host-02/result.json',
               'resolved-status.json':r/'analysis/host-geni-20261113/status.json'})
records={}
for name,source in copies.items():
 target=dest/name;shutil.copyfile(source,target);assert sha(source)==sha(target)
 records[name]={'bytes':target.stat().st_size,'sha256':sha(target),'source':str(source.relative_to(r))}
(dest/'README.md').write_text('''# Inactive J716F GENI host build

Native and prelinked kernels from the separate official host-geni target.
The new GENI bundle is force-linked but has no boot caller or auto-activation.
No boot image, payload, flashing instructions or hardware success is implied.
See analysis/geni-backend-20261113/README.md and source-freeze.json.
''')
records['README.md']={'bytes':(dest/'README.md').stat().st_size,'sha256':sha(dest/'README.md')}
manifest={'status':'passed','files':records,'runtime_qualified':False,'boot_verified':False,
          'automatic_activation':False,'device_actions':False,'qemu':False}
(dest/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'destination':str(dest),'files':len(records),'status':'passed'}))
