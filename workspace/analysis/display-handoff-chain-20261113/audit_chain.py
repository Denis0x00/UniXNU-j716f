#!/usr/bin/env python3
"""Verify the retained exact-source chain and line anchors; not a runtime proof."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
XNU = HERE/'sources/xnu'
LOADER = 'UniXNU/drivers/DarwinBoot/DarwinBootPkg/Driver/'

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
def check(ok, why):
    if not ok: raise ValueError(why)

def anchor(path, text, claim):
    data=path.read_text(); pos=data.find(text)
    check(pos >= 0, 'Missing source anchor: '+claim)
    check(data.find(text,pos+1) < 0, 'Ambiguous source anchor: '+claim)
    line=data[:pos].count('\n')+1
    return {'claim':claim,'path':path.relative_to(ROOT).as_posix(),
            'line':line,'end_line':line+text.count('\n'),'excerpt':text}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--snapshot',action='store_true')
    p.add_argument('--out',type=Path,default=HERE/'source-chain-verification.json')
    a=p.parse_args()
    mapping={
      LOADER+'DarwinBoot.c':'sources/loader/DarwinBoot.c',
      LOADER+'BootArgs.h':'sources/loader/BootArgs.h',
      LOADER+'MemoryWindow.h':'sources/loader/MemoryWindow.h',
      'UniXNU/scripts/unixnu/devicetree.py':'sources/devicetree.py',
      'UniXNU/drivers/ArmDTPlatform/ArmDTPlatform.cpp':'sources/ArmDTPlatform.cpp',
    }
    if a.snapshot:
        check(not (HERE/'source-evidence-manifest.json').exists(),'Snapshot already exists')
        hashes={}
        for origin,target in mapping.items():
            dst=HERE/target; dst.parent.mkdir(parents=True,exist_ok=True)
            shutil.copyfile(ROOT/origin,dst)
            hashes[target]={'sha256':sha(dst),'original':origin}
        (HERE/'source-evidence-manifest.json').write_text(json.dumps(hashes,indent=2)+'\n')
    for name,item in json.loads((HERE/'source-evidence-manifest.json').read_text()).items():
        check(sha(HERE/name)==item['sha256'],'Retained evidence changed: '+name)
        check(sha(ROOT/item['original'])==item['sha256'],'Selected upstream input changed: '+item['original'])
    remote=json.loads((HERE/'sources/remote-sha256.json').read_text())
    xhash={}
    for row in remote['stdout'].splitlines():
        digest,path=row.split('  ',1); rel=path.split('/.cache/xnu-work/',1)[1]
        check(sha(XNU/rel)==digest,'Downloaded XNU hash mismatch: '+rel)
        xhash[rel]=digest
    check(len(xhash)==12,'Expected all 12 source files')
    v1=json.loads((HERE/'source-freeze-v1.json').read_text())
    for name,item in v1['files'].items():
        check(sha(HERE/'source-v1'/name)==item['sha256'],'Retained v1 changed: '+name)
    v2=json.loads((HERE/'source-freeze-v2.json').read_text())
    for name,item in v2['files'].items():
        check(sha(HERE/'source-v2'/name)==item['sha256'],'Retained v2 changed: '+name)
    freeze=json.loads((HERE/'source-freeze-v3.json').read_text())
    for name,item in freeze['files'].items():
        check(sha(ROOT/freeze['source']/name)==item['sha256'],'Frozen source changed: '+name)
    for row in json.loads((HERE/'reclamation-sha256.json').read_text())['stdout'].splitlines():
        digest,path=row.split('  ',1); rel=path.split('/.cache/xnu-work/',1)[1]
        check(sha(XNU/rel)==digest,'Reclaim source hash mismatch: '+rel)
        xhash[rel]=digest
    A=[]
    def put(name,text,claim): A.append(anchor(HERE/name,text,claim))
    put('sources/devicetree.py',
        "chosen = [('random-seed', os.urandom(256)), ('dram-base', struct.pack('<Q', dram_base)),\n              ('dram-size', struct.pack('<Q', dram_size)), *empty_nvram_proxy()]",
        'DTS enabled bank produces ADT dram-base/size, not loader allocation address')
    put('sources/loader/DarwinBoot.c',
        'if (WindowSize == 0) { WindowSize = WINDOW_SIZE; }',
        'Loader defaults to its independent 256MiB window when no owned-size property')
    put('sources/loader/DarwinBoot.c',
        'Status = gBS->AllocatePages (AllocateAddress, EfiLoaderCode, EFI_SIZE_TO_PAGES (WindowSize), &Candidate);',
        'Contiguous path obtains actual physical allocation from UEFI')
    put('sources/loader/DarwinBoot.c',
        'Args->PhysicalBase = Address;\n  Args->MemorySize = WindowSize;\n  Args->MemorySizeActual = WindowSize;\n  Args->TopOfKernelData = Address + Used;',
        'Actual chosen window and used boundary, not ADT inventory, enter boot_args')
    put('sources/loader/MemoryWindow.h',
        'return (Type == EfiConventionalMemory) || (Type == EfiLoaderCode) || (Type == EfiLoaderData);',
        'Newer optional exclusion mode classifies usability, but current XNU17 lacks that consumer')
    put('sources/xnu/pexpert/arm/pe_init.c',
        'PE_state.video.v_baseAddr = boot_args_ptr->Video.v_baseAddr;',
        'Current PE console initially originates in boot_args Video')
    put('sources/xnu/pexpert/arm/pe_init.c',
        '*info = PE_state.video;\n\treturn 0;',
        'PE_current_console copies current mutable console, with zero success convention')
    put('sources/xnu/osfmk/arm64/arm_vm_init.c',
        'gPhysBase = args->physBase;',
        'Kernel managed span lower bound comes from actual boot_args')
    put('sources/xnu/osfmk/arm64/arm_vm_init.c',
        'gPhysSize = mem_size = ((gPhysBase + args->memSize) & ~PAGE_MASK) - gPhysBase;',
        'Original page-aligned managed span extent captured before optional maxmem clamp')
    put('sources/xnu/osfmk/arm64/arm_vm_init.c',
        'first_avail_phys = avail_start = args->topOfKernelData;',
        'Early first-free boundary captured as retained scalar')
    put('sources/xnu/osfmk/arm64/arm_vm_init.c',
        'vaddr = phystokv(avail_start);\n\t\tavail_start += ARM_PGBYTES;',
        'Qualified dynamic page-table allocator increments physical cursor')
    put('sources/xnu/osfmk/arm64/arm_vm_init.c',
        'avail_end = gPhysBase + mem_size;',
        'Managed upper boundary is original or maxmem-clamped span end')
    put('sources/xnu/osfmk/arm/pmap/pmap.c',
        'vm_first_phys = gPhysBase;\n\tvm_last_phys = trunc_page(avail_end);',
        'pmap installs the same current physical bounds')
    put('sources/xnu/osfmk/arm/pmap/pmap.c',
        '*pnum = (ppnum_t)atop(first_avail);\n\t\tfirst_avail += PAGE_SIZE;',
        'Boot page enumeration advances monotonically to avail_end')
    put('sources/xnu/osfmk/arm/pmap/pmap.c',
        'return pa_valid(ptoa(pn));',
        'Real pmap_valid_page delegates to physical range predicate')
    put('sources/xnu/osfmk/arm/pmap/pmap_data.h',
        'return (pa >= vm_first_phys) && (pa < vm_last_phys);',
        'Managed-page predicate uses half-open current bounds')
    put('sources/xnu/osfmk/vm/vm_resident.c',
        'if (!pmap_next_page(&phys_page)) {\n\t\t\tbreak;\n\t\t}',
        'VM page initialization enumerates actual pmap pages')
    put('sources/xnu/osfmk/vm/vm_resident.c',
        'if (!VMP_ERROR_GET(&vm_pages[--i])) {               /* skip retired pages */\n\t\t\t\tvm_page_release_startup(&vm_pages[i]);',
        'Non-high-memory startup release uses initialized loop count, not xnu12377 npages behavior')
    put('sources/xnu/iokit/Kernel/IOPlatformExpert.cpp',
        'return configure(provider);',
        'The patched point is after entire virtual configure has returned success')
    put('sources/xnu/osfmk/arm64/machine_routines.c',
        'vm_page_create(ppn, (ppn + 1));',
        'Later static reclaim can add physical pages, independently of initial pmap span')
    put('sources/xnu/iokit/Kernel/IODeviceTreeSupport.cpp',
        'ml_static_mfree( virt, range[1] );',
        'Generic loader-info free is another reason a lasting policy needs an adoption guard')
    test_helper=ROOT/freeze['source']/'tests/run_adoption_tests.py'
    spec=importlib.util.spec_from_file_location('adoption_test',test_helper)
    module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    patched=module.apply_vm_hunk((XNU/'osfmk/vm/vm_resident.c').read_text(),
                                (ROOT/freeze['source']/'patches/j716f-platform-display-handoff.patch').read_text())
    contract=json.loads((ROOT/freeze['source']/'package/source-contract.json').read_text())
    for rel in ['osfmk/mach/vm_param.h','osfmk/mach/arm/vm_param.h',
                'osfmk/mach/arm/vm_types.h','osfmk/arm/pmap_public.h']:
        check(sha(XNU/rel)==contract['resolved_source_sha256'][rel],
              'Exact type header mismatch: '+rel)
        xhash[rel]=sha(XNU/rel)
    check(hashlib.sha256(patched.encode()).hexdigest()==contract['resolved_source_sha256']['osfmk/vm/vm_resident.c'],
          'Patched VM source does not match the final build contract')
    function=patched[patched.index('void\nvm_page_create(\n'):]
    check(function.index('panic("J716F display reservation overlaps VM page adoption:') <
          function.index('for (phys_page = start;'), 'Adoption guard must precede any page iteration')
    # Current XNU snapshot is a contiguous span implementation. This negative
    # check is scoped to the fixed source files, not every possible runtime.
    for rel in ['osfmk/arm64/arm_vm_init.c','osfmk/arm/pmap/pmap.c']:
        check('darwinboot-memory-excluded' not in (XNU/rel).read_text(),
              'Unexpected exclusion-table consumer; re-audit required')
    command=['git','apply','--check','--directory=analysis/display-handoff-chain-20261113/sources/xnu',
             'work/J716fDisplayHandoff-20261113/patches/j716f-platform-display-handoff.patch']
    tested=subprocess.run(command,cwd=ROOT,text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    check(tested.returncode==0,'Patch no longer applies: '+tested.stderr)
    result={'status':'passed','scope':'retained source hashes and human-reviewed source-chain anchors; not runtime/formal proof',
            'xnu_revision':'d8b80295118ef25ac3a784134bcf95cd8e88109f',
            'darwinboot_revision':'5243c68ec332a227fe3680cd5da6d5a0e127035c',
            'remote_xnu_files':len(xhash),'source_frozen_files':len(freeze['files']),
            'retained_v1_files':len(v1['files']),'retained_v2_files':len(v2['files']),
            'guarded_vm_source_sha256':hashlib.sha256(patched.encode()).hexdigest(),
            'anchors':A,'patch_check':{'command':command,'exit_code':tested.returncode},
            'runtime_allocator_exclusion_observed':False,'scanout_qualified':False}
    a.out.write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n')
    print(json.dumps({'status':'passed','anchors':len(A),'xnu_files':len(xhash)}))
if __name__=='__main__': main()
