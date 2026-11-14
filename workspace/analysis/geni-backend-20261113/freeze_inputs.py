"""Record immutable inputs for the independent GENI build; preserve earlier freezes."""
from pathlib import Path
import argparse,hashlib,json
p=argparse.ArgumentParser();p.add_argument('--attempt',type=int,required=True);p.add_argument('--prepare-only',action='store_true');a=p.parse_args();assert a.attempt>0
root=Path(__file__).resolve().parents[2];report=root/'analysis/geni-backend-20261113'
freeze=report/f'source-freeze-{a.attempt:02d}.json';assert not freeze.exists()
target=root/'work/UniXNU-j716f-host-geni';manifest=target/'docs/host-source-manifest.json'
inputs={}
for rel in ('work/UniXNU-j716f-host-geni','work/J716fGoodix-20261113',
            'work/J716fTouchTransaction-20261113','work/J716fGeniBackend-20261113'):
 if a.prepare_only and rel!='work/UniXNU-j716f-host-geni':continue
 for path in (root/rel).rglob('*'):
  if path.is_file() and path!=manifest and not any(x in ('.git','__pycache__') for x in path.parts):
   inputs[path.relative_to(root).as_posix()]={'bytes':path.stat().st_size,'sha256':hashlib.sha256(path.read_bytes()).hexdigest()}
data={'origin':'work/UniXNU-j716f-host-dt-panic','upstream_revision':'7d85ede80092050f4d87f72ac7229bb605ce07fb',
      'inputs':inputs,'runtime_qualified':False,'automatic_activation':False,'prepare_only':a.prepare_only,
      'effective_config_delta_from_dt_panic':{'XNU_EXTRA_LD_ARCHIVES':'-Wl,-force_load,{tree}/build/j716f-geni/libJ716fGeniBackend.a'}}
text=json.dumps(data,indent=2)+'\n';manifest.write_text(text,encoding='utf-8',newline='\n');freeze.write_text(text,encoding='utf-8',newline='\n')
print(f'{len(inputs)} files frozen at {freeze}')
