#!/usr/bin/env python3
"""Build a resource-only archive through the resolved kernel's actual C++ recipe."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ENTRY = {'_native_legacy_display_probe', '_native_legacy_display_late_probe',
         '_j716f_gop_register_platform_reservation',
         '_j716f_gop_display_service_available', '_j716f_gop_scanout_available'}

def require(test, why):
    if not test: raise ValueError(why)

def sha(path): return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def audit_object(path):
    data = path.read_bytes()
    require(len(data) >= 32, 'Truncated object')
    magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
    require((magic,cpu,subtype,kind) == (0xfeedfacf,0x100000c,0,1),
            'Expected MH_OBJECT ordinary arm64, not arm64e')
    end = 32+size
    require(end <= len(data) and count <= size//8, 'Bad load commands')
    pos = 32; symtab = None; deployment = None; sections = []
    for _ in range(count):
        cmd, length = struct.unpack_from('<II',data,pos)
        require(length >= 8 and pos+length <= end and not length%8, 'Bad command extent')
        if cmd == 2:
            require(length == 24 and symtab is None, 'Bad symbol table')
            symtab = struct.unpack_from('<4I',data,pos+8)
        if cmd == 0x32:
            require(length >= 24 and deployment is None, 'Bad deployment')
            deployment = struct.unpack_from('<II',data,pos+8)
        if cmd == 0x19:
            require(length >= 72, 'Bad segment')
            n = struct.unpack_from('<I',data,pos+64)[0]
            require(length == 72+80*n, 'Bad section count')
            for i in range(n):
                at = pos+72+80*i
                name = data[at:at+16].split(b'\0')[0].decode('ascii')
                extent = struct.unpack_from('<Q',data,at+40)[0]
                sections.append({'name':name,'bytes':extent})
                require(not (name == '__mod_init_func' and extent),
                        'Unexpected static constructor: resource-only provider needs none')
        pos += length
    require(pos == end and symtab, 'Missing symbol table')
    require(deployment == (2,17<<16), 'Wrong deployment: expected iOS 17.0')
    sy,count,strings,length = symtab
    require(sy+count*16 <= len(data) and strings+length <= len(data), 'Bad symtab bounds')
    names = data[strings:strings+length]; defined = []; undefined = []
    for i in range(count):
        ix,kind,section,desc,value = struct.unpack_from('<IBBHQ',data,sy+i*16)
        require(ix < length, 'Bad symbol string offset')
        end = names.find(b'\0',ix); require(end >= ix, 'Unterminated symbol')
        name = names[ix:end].decode()
        if kind & 0xe0 or not kind & 1: continue
        (defined if kind & 0x0e else undefined).append(name)
    require(ENTRY <= set(defined), 'Missing strong official/provider/capability exports: '+str(ENTRY-set(defined)))
    require(not any('__GLOBAL__sub_I' in s for s in defined), 'Unexpected global initialization')
    require(not any('NativeLegacyDisplay' in s or 'IOMobileFramebuffer' in s for s in defined+undefined),
            'Resource-only archive must not instantiate a legacy display')
    return {'cpu':'ordinary-arm64','deployment':'iOS17.0','sections':sections,
            'defined_symbols':sorted(defined),'undefined_symbols':sorted(undefined),
            'official_entry_exports':sorted(ENTRY),'global_constructors':False,
            'provider_resolution':'pending-official-kernel-link'}

def compile_command(record, source, output, include, tree):
    args = record['arguments']; result = []; i = 0; found = 0; outputs = 0
    paired = {'-MF','-MT','-MQ','-MJ','-dependency-file','--serialize-diagnostics'}
    switches = {'-MD','-MMD','-MP','-MG'}
    while i < len(args):
        value = args[i]
        if value == record['file']:
            result.append(str(source)); found += 1
        elif value == '-o':
            require(i+1 < len(args), 'Missing output argument')
            result += ['-o',str(output)]; outputs += 1; i += 1
        elif value in paired:
            require(i+1 < len(args), 'Missing dependency argument'); i += 1
        elif value in switches: pass
        elif any(value.startswith(flag) and value != flag for flag in ['-MF','-MT','-MQ','-MJ']): pass
        elif value == '-Xclang' and i+1 < len(args) and args[i+1] in paired:
            require(i+3 < len(args) and args[i+2] == '-Xclang', 'Unknown cc1 dependency argument')
            i += 3
        elif value == '-Xclang' and i+1 < len(args) and args[i+1].startswith('-W'): i += 1
        elif value.startswith('-W'): pass
        else: result.append(value)
        i += 1
    require(found == 1 and outputs == 1, 'Compile record must contain exactly one source and output')
    result += ['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-c99-designator',
               '-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion',
               '-Wno-unnecessary-virtual-specifier','-mgeneral-regs-only',
               '-I',str(include),'-I',str(tree/'.cache/xnu-work/osfmk')]
    return result

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--tree',type=Path,required=True)
    p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1])
    p.add_argument('--ar',help='Explicit llvm-ar binary, otherwise compiler sibling or PATH')
    a = p.parse_args(); tree = a.tree.resolve(); source = a.source.resolve()
    require(tree != source, 'Source package is not a resolved build tree')
    require((tree/'components.toml').is_file() and (tree/'scripts/build-xnu-4k-baseline.sh').is_file(),
            'Resolved UniXNU build tree required')
    require(os.environ.get('UNIXNU_XNU_ARCH','arm64') == 'arm64', 'Only ordinary arm64 is qualified')
    require(os.environ.get('UNIXNU_IOS_VERSION','17.0') == '17.0', 'Only iOS 17.0 deployment is qualified')
    recipe = tree/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
    record = json.loads(recipe.read_text().strip().rstrip(','))
    out = tree/'build/native-legacy-display'; out.mkdir(parents=True,exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    attempt = out/'attempts'/stamp; inputs = attempt/'source'; inputs.mkdir(parents=True)
    archive = out/'libNativeLegacyDisplay.late.a'; report_path = out/'build-report.json'
    # Never let a failed new attempt leave a stale successful archive/report.
    for old in [archive,report_path]:
        if old.exists(): shutil.copy2(old,attempt/('previous-'+old.name)); old.unlink()
    input_hashes = {}; original_hashes = {}
    for name in ['package/J716fDisplayCore.h','package/J716fGopProvider.cpp','scripts/build.py']:
        origin = source/name; dest = inputs/name; dest.parent.mkdir(parents=True,exist_ok=True)
        shutil.copyfile(origin,dest)
        original_hashes[name] = sha(origin)
        require(sha(dest) == original_hashes[name], 'Input changed while snapshotting: '+name)
        input_hashes[str(dest.relative_to(tree))] = sha(dest)
    obj = attempt/'J716fGopProvider.o'
    cmd = compile_command(record,inputs/'package/J716fGopProvider.cpp',obj,inputs/'package',tree)
    (attempt/'compile-command.json').write_text(json.dumps(cmd,indent=2)+'\n')
    built = subprocess.run(cmd,cwd=record['directory'],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
    (attempt/'compile.log').write_text(built.stdout)
    require(built.returncode == 0, 'Compile failed:\n'+built.stdout[-6000:])
    audit = audit_object(obj)
    compiler = Path(shutil.which(cmd[0]) or cmd[0]).resolve()
    suffix = re.search(r'-(\d+(?:\.\d+)*)$', compiler.name)
    names = (['llvm-ar-'+suffix.group(1)] if suffix else []) + ['llvm-ar']
    candidates = [str(compiler.parent/name) for name in names if (compiler.parent/name).is_file()]
    candidates += [found for name in names if (found := shutil.which(name))]
    ar = a.ar or (candidates[0] if candidates else None)
    require(ar, 'llvm-ar not found; supply --ar')
    local_archive = attempt/'libNativeLegacyDisplay.late.a'
    subprocess.run([ar,'rcs',str(local_archive),str(obj)],check=True)
    shutil.copyfile(local_archive,archive)
    report = {'schema':1,'integration_level':'resource-provider-only',
              'native_legacy_display_compiled':False,'display_service_published':False,
              'scanout_available':False,'swap_retirement_qualified':False,
              'platform_reservation_caller_present':False,'default_enabled':False,
              'runtime_qualification':False,'archive_sha256':sha(archive),
              'source_sha256':input_hashes,'original_package_sha256':original_hashes,
              'compile_record_sha256':sha(recipe),'object_sha256':sha(obj),
              'archive_tool':ar,
              'attempt':str(attempt.relative_to(tree)),'object_audit':audit,
              'reservation_authority':'trusted platform caller contract; descriptor alone does not prove allocator exclusion',
              'limitation':'No current trusted reservation caller; enabling resource check refuses NO_PLATFORM_RESERVATION.'}
    report_path.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'archive':str(archive),'sha256':report['archive_sha256'],
                      'integration_level':report['integration_level'],'undefined_count':len(audit['undefined_symbols'])}))

if __name__ == '__main__':
    try: main()
    except (ValueError,OSError,subprocess.CalledProcessError) as exc:
        raise SystemExit(str(exc))
