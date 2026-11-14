"""Read-only verification of the prior upstream-audit preservation baseline."""
from pathlib import Path
import argparse,hashlib,json
p=argparse.ArgumentParser();p.add_argument('--report',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[2];dest=a.report.resolve();assert not dest.exists()
baseline=root/'analysis/progress-upstream-audit-20261113-r2/preservation-before.json'
before=json.loads(baseline.read_text(encoding='utf-8'));bad=[];checked=0
for rel,entry in before['files'].items():
 path=root/rel;expected=entry['identity'];checked+=1
 if not path.is_file() or path.stat().st_size!=expected['bytes'] or hashlib.sha256(path.read_bytes()).hexdigest()!=expected['sha256']:bad.append(rel)
report={'status':'passed' if not bad else 'failed','baseline':str(baseline),
        'baseline_sha256':hashlib.sha256(baseline.read_bytes()).hexdigest(),'checked_files':checked,'mismatches':bad,
        'device_actions':False}
dest.write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8');print(json.dumps(report));raise SystemExit(bool(bad))
