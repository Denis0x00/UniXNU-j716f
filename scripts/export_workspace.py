#!/usr/bin/env python3
"""Export source paths; optionally fetch pinned cores. No builds or device I/O."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

REPO=Path(__file__).resolve().parents[1]

def identity(path):
    raw=path.read_bytes()
    return {'bytes':len(raw),'sha256':hashlib.sha256(raw).hexdigest()}

def inside(root,relative):
    rel=Path(relative)
    if rel.is_absolute() or '..' in rel.parts:
        raise ValueError('Unsafe manifest path: '+relative)
    result=root/rel
    if not result.resolve().is_relative_to(root.resolve()):
        raise ValueError('Path escapes root: '+relative)
    return result

def run(*args,cwd=None):
    subprocess.run(list(args),cwd=cwd,check=True)

def check_sources():
    manifest=json.loads((REPO/'pins/source-snapshot.json').read_text())
    source=REPO/'workspace'
    entries=manifest['files']
    actual={p.relative_to(source).as_posix() for p in source.rglob('*') if p.is_file()}
    if actual != set(entries):
        raise ValueError('Snapshot file set differs from the manifest')
    for relative,spec in entries.items():
        path=inside(source,relative)
        if path.is_symlink() or not path.is_file() or identity(path)!=spec:
            raise ValueError('Source changed: '+relative)
    cores=json.loads((REPO/'pins/core.json').read_text())['cores']
    for core in cores:
        patch=inside(REPO,core['patch'])
        if identity(patch)['sha256']!=core['patch_sha256']:
            raise ValueError('Core patch changed: '+core['name'])
        for item in core['overlay_files']:
            path=inside(REPO/'overlays'/core['name']/'files',item['path'])
            if identity(path)!={'bytes':item['bytes'],'sha256':item['sha256']}:
                raise ValueError('Overlay changed: '+item['path'])
    return manifest,cores

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--destination',type=Path)
    p.add_argument('--fetch-core',action='store_true')
    p.add_argument('--check',action='store_true',help='Verify source/pins only; no export/network')
    a=p.parse_args()
    manifest,cores=check_sources()
    if a.check:
        if a.destination or a.fetch_core: p.error('--check takes no destination or fetch')
        print('PASS:',len(manifest['files']),'source files and',len(cores),'core overlays')
        return
    if a.destination is None: p.error('--destination is required')
    dest=a.destination.expanduser().resolve()
    if dest.exists() or dest.is_relative_to(REPO) or REPO.is_relative_to(dest):
        p.error('Choose a new destination; existing paths are never overwritten')
    dest.mkdir(parents=True)
    for relative in manifest['files']:
        target=inside(dest,relative)
        target.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(inside(REPO/'workspace',relative),target)
    if a.fetch_core:
        for core in cores:
            target=inside(dest,core['workspace_path'])
            if target.exists(): raise ValueError('Core destination already exists')
            target.mkdir(parents=True)
            run('git','init',str(target))
            run('git','config','core.autocrlf','false',cwd=target)
            run('git','remote','add','origin',core['repository'],cwd=target)
            run('git','fetch','--depth','1','origin',core['revision'],cwd=target)
            run('git','checkout','--detach','FETCH_HEAD',cwd=target)
            head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=target,text=True).strip()
            if head!=core['revision']: raise ValueError('Fetched core revision differs')
            run('git','submodule','update','--init','--recursive',cwd=target)
            for module in core['submodules']:
                module_path=inside(target,module['path'])
                revision=subprocess.check_output(['git','rev-parse','HEAD'],cwd=module_path,text=True).strip()
                if revision!=module['revision']:
                    raise ValueError('Submodule revision differs: '+module['path'])
            patch=inside(REPO,core['patch'])
            run('git','apply','--check',str(patch),cwd=target)
            run('git','apply','--whitespace=nowarn',str(patch),cwd=target)
            for item in core['overlay_files']:
                source=inside(REPO/'overlays'/core['name']/'files',item['path'])
                output=inside(target,item['path'])
                if output.exists(): raise ValueError('Overlay would overwrite: '+item['path'])
                output.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(source,output)
    receipt={'source_files':len(manifest['files']),'core_materialized':a.fetch_core,
             'build_performed':False,'hardware_tested':False}
    (dest/'source-export.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('Exported sources:',dest)
    print('Read docs/reconstruction.md for toolchain, private inputs and fresh acceptance.')

if __name__=='__main__': main()
