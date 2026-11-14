#!/usr/bin/env python3
"""Read-only 21H461 metadata and queue-instruction evidence; never an ABI grant."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
HELPER_SHA = 'f9af9805c3affd2f7ac79ca86a1f274905502f560e029e63f5a2fbaeaa8c1f00'
KERNEL_SHA = '7970051f50e280b50e42f5ed372ea095dc8646a36fb92e4716f85349bd6118f2'
ORIGINAL_SHA = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
CLASSES = [
    ('IOMobileFramebuffer', 0xfffffff00577b2a4,0xfffffff00577b2b8,
     0xfffffff00577b3c8,0xfffffff00577b3f0,0xfffffff006b313d8,0x90),
    ('IOMobileFramebufferService',0xfffffff00577b49c,0xfffffff00577b4b0,
     0xfffffff00577b5c8,0xfffffff00577b5f0,0xfffffff006b31400,0x90),
    ('IOMobileFramebufferLegacy',0xfffffff005786558,0xfffffff00578656c,
     0xfffffff00578667c,0xfffffff0057866a4,0xfffffff006b35940,0x18a0),
]
WINDOWS = [
    ('queue_head_and_tail_initialization',0xfffffff005787d7c,24),
    ('request_append_tail_links',0xfffffff00578a684,48),
    ('pending_head_to_current',0xfffffff00578a708,12),
    ('current_unlink_and_next_previous_link',0xfffffff00578a80c,124),
    ('normal_completion_entry_true_argument',0xfffffff005795b88,44),
    ('completion_optional_report_dispatch',0xfffffff00578aa70,36),
    ('completion_current_vcall_then_clear_and_queue_notification',0xfffffff00578af98,56),
]

def require(value, message):
    if not value: raise ValueError(message)

def sha(blob): return hashlib.sha256(blob).hexdigest()

def load_helper():
    path = ROOT/'analysis/tools/check_j716f_display_abi.py'
    require(sha(path.read_bytes()) == HELPER_SHA, 'Existing read-only helper changed')
    spec = importlib.util.spec_from_file_location('bounded_display_reader',path)
    module = importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
    return module

def word(image, pc): return struct.unpack('<I',image.read(pc,4))[0]

def page(image, pc, reg):
    value = word(image,pc)
    require(value & 0x9f00001f == 0x90000000 | reg, 'Expected ADRP register at '+hex(pc))
    imm = ((value>>5 & 0x7ffff)<<2) | (value>>29 & 3)
    if imm & (1<<20): imm -= 1<<21
    return (pc & ~0xfff) + (imm<<12)

def address(image, pc, reg):
    base = page(image,pc,reg); add = word(image,pc+4)
    require(add & 0xffc003ff == 0x91000000 | reg<<5 | reg,
            'Expected same-register unshifted ADD at '+hex(pc+4))
    return base + (add>>10 & 0xfff)

def indirect(image, pc, reg):
    base = page(image,pc,reg); load = word(image,pc+4)
    require(load & 0xffc003ff == 0xf9400000 | reg<<5 | reg,
            'Expected same-register unsigned LDR at '+hex(pc+4))
    cell = base + (load>>10 & 0xfff)*8
    return cell,struct.unpack('<Q',image.read(cell,8))[0]

def size_argument(image,pc):
    value = word(image,pc)
    require(value & 0xffe0001f == 0x52800003, 'Expected MOVZ W3 with no shift')
    return value>>5 & 0xffff

def ldrx(rt,rn,offset): return 0xf9400000 | (offset//8)<<10 | rn<<5 | rt
def strx(rt,rn,offset): return 0xf9000000 | (offset//8)<<10 | rn<<5 | rt

def audit(kernel, metadata, original, helper):
    require(sha(kernel)==KERNEL_SHA, 'Exact kernel SHA mismatch')
    require(sha(original)==ORIGINAL_SHA, 'Exact original kernelcache SHA mismatch')
    helper.audit(kernel,metadata,original,KERNEL_SHA)
    image = helper.MachO(kernel); stock = helper.MachO(original)
    by_address, by_name = image.exports()
    stub = 0xfffffff0057a2818
    cell,native = indirect(image,stub,16)
    require(word(image,stub+8)==0xd61f0200,'Expected BR X16 import stub')
    require(helper.unique(by_name,'__ZN11OSMetaClassC2EPKcPKS_j')==native,
            'Metadata constructor import must resolve to native OSMetaClass C2')
    meta = []; windows = []
    def keep(name, pc, length):
        data = image.read(pc,length)
        require(data == stock.read(pc-helper.SLIDE,length),'Retained original instruction window mismatch: '+name)
        windows.append({'name':name,'candidate_address':hex(pc),'original_address':hex(pc-helper.SLIDE),
                        'bytes':length,'hex':data.hex(),'sha256':sha(data),'unchanged_original':True})
    previous = helper.unique(by_name,'__ZN9IOService10gMetaClassE')
    for name,caller,wrapper,ctor,args,own,extent in CLASSES:
        require(address(image,caller,0)==own, 'Wrong global metadata object: '+name)
        require(helper.branch(image,caller+8)==wrapper, 'Wrong global constructor target')
        # The wrapper saves and restores the same x0 argument before calling C2.
        require(image.read(wrapper+12,12)==bytes.fromhex('e00700f9e00740f9e00300f9'),
                'Metadata wrapper must preserve this pointer')
        require(helper.branch(image,wrapper+24)==ctor,'Wrong metadata C2 target')
        string = address(image,args,1)
        require(image.read(string,len(name)+1)==name.encode()+b'\0','Wrong class-name argument')
        if name=='IOMobileFramebuffer':
            parent_cell,parent = indirect(image,args+8,2)
        else:
            parent_cell = None; parent = address(image,args+8,2)
        require(parent==previous,'Wrong metadata parent chain: '+name)
        require(size_argument(image,args+16)==extent,'Wrong metadata instance extent: '+name)
        require(helper.branch(image,args+20)==stub,'Wrong OSMetaClass call')
        keep(name+'_global_call',caller,12)
        keep(name+'_forwarding_constructor',wrapper,36)
        keep(name+'_metadata_constructor',ctor,88)
        meta.append({'class':name,'metadata_object':hex(own),'parent_metadata':hex(parent),
                     'parent_import_cell':hex(parent_cell) if parent_cell else None,
                     'class_name_address':hex(string),'instance_size':extent,
                     'instance_size_hex':hex(extent),'size_argument_pc':hex(args+16),
                     'constructor_pc':hex(ctor),'global_this_pc':hex(caller)})
        previous=own
    # These instructions qualify field/dispatch anchors only. No full locking,
    # reference ownership, cancellation or completion lifecycle is inferred.
    expected = {
        0xfffffff005787d84:strx(31,9,5480),
        0xfffffff005787d90:strx(8,9,5488),
        0xfffffff00578a68c:strx(31,8,2960),
        0xfffffff00578a690:ldrx(8,9,5488),
        0xfffffff00578a698:strx(8,10,2968),
        0xfffffff00578a6b0:strx(8,9,5488),
        0xfffffff00578a70c:ldrx(8,9,5480),
        0xfffffff00578a710:strx(8,9,5472),
        0xfffffff00578a830:ldrx(8,8,2968),
        0xfffffff00578a838:ldrx(9,9,2960),
        0xfffffff00578a83c:strx(8,9,2968),
        0xfffffff00578a850:strx(8,9,5488),
        0xfffffff00578af9c:ldrx(0,8,5472),
        0xfffffff00578afa4:ldrx(8,8,5*8),
        0xfffffff00578afb0:strx(31,8,5472),
        0xfffffff00578aa80:ldrx(8,8,337*8),
    }
    for pc,value in expected.items():
        require(word(image,pc)==value,'Instruction field anchor mismatch at '+hex(pc))
    require(helper.branch(image,0xfffffff005795ba4)==0xfffffff00578a96c,
            'Normal completion helper call mismatch')
    require(image.read(0xfffffff005795b9c,8)==bytes.fromhex('2800805201010012'),
            'Normal completion must pass true in W1')
    for entry in WINDOWS: keep(*entry)
    return {'scope':'static original metadata and selected queue/retirement instruction anchors',
            'kernel_sha256':KERNEL_SHA,'original_sha256':ORIGINAL_SHA,
            'helper_sha256':HELPER_SHA,'original_retained_family_bytes_unchanged':True,
            'metaclass_name_parent_size_chain_verified':True,'metaclasses':meta,
            'osmetaclass_import':{'stub':hex(stub),'cell':hex(cell),'native':hex(native),
                                  'symbol':'__ZN11OSMetaClassC2EPKcPKS_j'},
            'queue_field_instruction_anchors_verified':True,
            'queue_fields':{'self_current':5472,'self_pending_head':5480,'self_tail_link':5488,
                            'request_next':2960,'request_previous_link':2968},
            'normal_completion':{'slot':295,'entry':hex(0xfffffff005795b88),
                                 'helper':hex(0xfffffff00578a96c),'argument_w1':True},
            'complete_display_abi':False,'swap_reference_lifecycle_qualified':False,
            'swap_retirement_qualified':False,'scanout_ownership_qualified':False,
            'runtime_qualified':False,'windows':windows,
            'remaining':['Full path calling convention and locking proof',
                         'Request class allocation/retain/release identity and transfer ownership',
                         'Every cancellation, power, error and concurrent queue path',
                         'Completion callback/report and wakeup obligations',
                         'Current boot reservation, cache mapping and scanout grant',
                         'Compiled ABI against actual native headers and real runtime evidence']}

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--kernel',type=Path,default=ROOT/'artifacts/j716f-host-dt-panic-20261113/kernel.macho')
    p.add_argument('--metadata',type=Path,default=ROOT/'artifacts/j716f-host-dt-panic-20261113/kernel.json')
    p.add_argument('--original',type=Path,default=ROOT/'artifacts/j716f-ipados-inputs-20261112/21H461/kernelcache.decompressed')
    p.add_argument('--out',type=Path,default=Path(__file__).with_name('exact-metadata-evidence.json'))
    a=p.parse_args(); helper=load_helper()
    report=audit(a.kernel.read_bytes(),a.metadata.read_bytes(),a.original.read_bytes(),helper)
    a.out.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ['metaclass_name_parent_size_chain_verified',
        'queue_field_instruction_anchors_verified','complete_display_abi','swap_retirement_qualified']}))

if __name__=='__main__': main()
