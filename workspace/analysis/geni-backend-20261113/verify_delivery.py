"""Verify byte-identical delivery of this GENI source, evidence and host artifact set."""
from pathlib import Path
import argparse,hashlib,json
p=argparse.ArgumentParser();p.add_argument('--report',type=Path);a=p.parse_args()
r=Path(__file__).resolve().parents[2];manifest=r/'analysis/geni-backend-20261113/final-sync-manifest.json'
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();items=json.loads(manifest.read_text(encoding='utf-8'))['files'];bad=[]
for rel,spec in items.items():
 path=(r/rel).resolve()
 if not path.is_relative_to(r) or not path.is_file() or path.stat().st_size!=spec['bytes'] or sha(path)!=spec['sha256']:bad.append(rel)
result={'status':'passed' if not bad else 'failed','checked_files':len(items),'manifest_sha256':sha(manifest),'mismatches':bad}
if a.report:
 assert not a.report.exists();a.report.write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
print(json.dumps(result));raise SystemExit(bool(bad))
