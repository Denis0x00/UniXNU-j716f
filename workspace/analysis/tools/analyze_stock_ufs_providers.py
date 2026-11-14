#!/usr/bin/env python3
"""Static-only TB-J716F provider follow-up; never executes firmware/device/VM/SMC.

--extract replays pinned extraction and adds DALSys + bounded GUID search.
--analyze creates byte-derived evidence; --check recomputes/compares all evidence.
All evidence writes are immutable and confined to the new study directory.
"""
import argparse, hashlib, json, re, runpy, struct, sys, uuid
from pathlib import Path
sys.dont_write_bytecode = True
import capstone, pefile

ROOT=Path(__file__).resolve().parents[2]
STUDY=ROOT/'analysis/stock-ufs-providers-20261113'
OLD=ROOT/'analysis/stock-ufs-handoff-20261113'
BASE_HASH='9061a12238024593fa1221455c128687ea764e5d7141f812ffe9443f4edbdae8'
EXTRACT_HASH='cc4e44f34d71ced51291000f3f2f1b4375dbed393b232c053d19c0a6762852e2'
INPUTS={'xbl.elf':'d665a97c8b53288575a8720095e1032f1c8c29c49c9a716158013bf9a50e1088',
        'abl.elf':'694e55a6bb205e3bd31cf803dbc29bc6ee1d1ea40435c3e96ed79559cdf04e1d'}
GUIDS={'ufs_iommu':'54b6d3b4-5d33-4f91-8600-6c41d5deb19a',
       'scm_exit_slot10':'b9be5350-c6c5-4fda-9b09-551ff061c0ba',
       'bds_exit_slot8':'f9085b9d-9304-40fb-8fe0-4aee3b1a784b',
       'scm_buffer_slot10':'9c1eb71f-dd6c-4ed5-9f6a-5cc0ca789f16'}
def sha(b): return hashlib.sha256(b).hexdigest()
def encode(v): return (json.dumps(v,ensure_ascii=False,indent=2)+'\n').encode()
def save(name,b,check=False):
    p=STUDY/name
    if check or p.exists():
        if not p.exists() or p.read_bytes()!=b: raise ValueError('Missing/differing evidence: '+str(p))
    else:
        p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b)
def originals():
    rows=[]
    for name,h in INPUTS.items():
        p=ROOT/'TB_J716F_ZUI_15.0.485/image'/name;b=p.read_bytes();assert sha(b)==h
        rows.append({'path':p.relative_to(ROOT).as_posix(),'bytes':len(b),'sha256':h})
    return rows
def helpers():
    p=OLD/'extractor-v1.py';assert sha(p.read_bytes())==BASE_HASH
    base=runpy.run_path(str(p),run_name='readonly_provider_helpers')
    base['file_walk'].__globals__['STUDY']=STUDY
    return base
def hits(b,pat):
    out=[];at=0
    while True:
        at=b.find(pat,at)
        if at<0:return out
        out.append(at);at+=1
def supplemental(check=False):
    before=originals();base=helpers();old=json.loads((OLD/'extraction.json').read_text(encoding='utf-8'))
    target=uuid.UUID(GUIDS['scm_exit_slot10']).bytes_le
    match_rows=[];raw_rows=[];counts=[];dal=None
    for filename in INPUTS:
        p=ROOT/'TB_J716F_ZUI_15.0.485/image'/filename;b=p.read_bytes()
        raw_rows.append({'source':filename,'file_offsets':hits(b,target)})
        rows=list(base['file_walk'](b,p.relative_to(ROOT).as_posix()))
        prev=[x for x in old['modules'] if x['source']==filename];assert len(rows)==len(prev)
        section_count=0
        for (guid,kind,sections),prior in zip(rows,prev):
            assert (guid,kind)==(prior['guid'],prior['ffs_type'])
            assert [(k,len(v),sha(v)) for k,v,_ in sections]==[(x['type'],x['bytes'],x['sha256']) for x in prior['sections']]
            name=next((v.decode('utf-16-le').rstrip('\0') for k,v,_ in sections if k==0x15),'')
            for k,v,trace in sections:
                section_count+=1;found=hits(v,target)
                if found:match_rows.append({'source':filename,'name':name,'ffs_guid':guid,'section_type':k,'section_sha256':sha(v),'payload_offsets':found,'trace':trace})
                if name=='DALSys' and k==0x10:
                    assert sha(v)=='e5818b62a94fe15a5b357f7df494d7de6672e52078a767fe6169dab8d0f622a6'
                    base['save']('DALSys/DALSys.efi',v);base['pe_inventory']('DALSys',v)
                    dal={'source':filename,'name':name,'ffs_guid':guid,'pe_bytes':len(v),'pe_sha256':sha(v),'trace':trace}
        counts.append({'source':filename,'ffs_records':len(rows),'parsed_leaf_section_records':section_count})
    pe_hits=[x for x in match_rows if x['section_type']==0x10]
    assert len(pe_hits)==1 and pe_hits[0]['name']=='ScmDxe' and pe_hits[0]['payload_offsets']==[0xa078]
    assert dal is not None and originals()==before
    out={'schema_version':1,'sources_before':before,'sources_after':originals(),'base_extractor_sha256':BASE_HASH,
         'previous_inventory_agrees':True,'supplementary_module':dal,'scanned':counts,
         'unresolved_guid':GUIDS['scm_exit_slot10'],'raw_input_hits':raw_rows,'all_parsed_section_hits':match_rows,
         'negative_scope':'Exact GUID bytes in these raw inputs and all parsed/decompressed leaf sections only; not proof of runtime absence.',
         'firmware_executed':False}
    save('supplemental-extraction.json',encode(out),check);return out

class Module:
    def __init__(self,name):
        self.name=name;self.body=(STUDY/name/(name+'.efi')).read_bytes();self.pe=pefile.PE(data=self.body)
        assert self.pe.FILE_HEADER.Machine==0xaa64 and self.pe.OPTIONAL_HEADER.ImageBase==0
        prior=json.loads((STUDY/name/'pe.json').read_text(encoding='utf-8'));assert prior['sha256']==sha(self.body)
        self.md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_LITTLE_ENDIAN);self.md.detail=True
    def raw(self,rva,n):
        off=self.pe.get_offset_from_rva(rva);assert self.pe.get_offset_from_rva(rva+n-1)==off+n-1
        return self.body[off:off+n]
    def q(self,rva):return struct.unpack('<Q',self.raw(rva,8))[0]
    def u(self,rva):return struct.unpack('<I',self.raw(rva,4))[0]
    def cstr(self,rva):
        off=self.pe.get_offset_from_rva(rva);return self.body[off:self.body.index(0,off)].decode('ascii')
    def data(self,label,rva,n):
        b=self.raw(rva,n);return {'label':label,'module':self.name,'rva':rva,'pe_file_offset':self.pe.get_offset_from_rva(rva),'bytes':n,'sha256':sha(b),'raw_hex':b.hex()}
    def window(self,label,lo,hi):
        insns=[]
        for at in range(lo,hi,4):
            b=self.raw(at,4);decoded=list(self.md.disasm(b,at,1));assert len(decoded)==1,(self.name,hex(at))
            i=decoded[0];target=i.operands[0].imm if i.mnemonic in ('b','bl') and i.operands[0].type==capstone.arm64.ARM64_OP_IMM else None
            insns.append({'rva':at,'pe_file_offset':self.pe.get_offset_from_rva(at),'bytes':b.hex(),'mnemonic':i.mnemonic,'operands':i.op_str,'direct_target':target})
        return {'label':label,'module':self.name,'rva_begin':lo,'rva_end_exclusive':hi,'bytes':hi-lo,'sha256':sha(self.raw(lo,hi-lo)),'instructions':insns}
    def expect(self,rva,mnemonic,operands):
        i=list(self.md.disasm(self.raw(rva,4),rva,1))[0];assert (i.mnemonic,i.op_str)==(mnemonic,operands),(self.name,hex(rva),i.op_str)

WINDOWS={
 'HALIOMMU':[
  ('protocol getter and installation',0x14cc,0x15b4),('context invalidation dispatch',0x15b4,0x16b4),
  ('create and destroy domain',0x16b4,0x1920),('attach mapping selector and SMR formation',0x1d40,0x1e58),
  ('detach body',0x27c0,0x2a9c),('detach stream route clearing',0x2b50,0x2c88),
  ('domain configure and invalidation wrappers',0x2f10,0x3174),('MMIO primitives and context offset',0x327c,0x33bc),
  ('S2CR write',0x3690,0x36f0),('SMR write',0x36f0,0x3750),
  ('CB all invalidation and sync loop',0x3864,0x3950),('CB address invalidation and sync loop',0x3950,0x3ac8),
  ('gBS constructor',0x42d4,0x4304),('named mapping accessors',0x48a8,0x4c10),
  ('SMMU IORT property read',0x4e14,0x4f28),('DAL property wrappers',0x6ab0,0x6af0)],
 'ShmBridgeDxe':[
  ('protocol install allocate free wrappers',0x1464,0x14fc),('gBS constructor',0x18c8,0x18f8),
  ('pool free bounded body',0x294c,0x2994),('pool wrappers',0x2a18,0x2a44)],
 'VariableDxe':[
  ('sync wrapper',0x15ec,0x15f4),('EBS callback and sync protocol method',0x18c0,0x1930),
  ('EBS registration',0x1e34,0x1e70),('protocol installation',0x1f14,0x1fa4),
  ('SCM locate and secure-app lookup',0x20a0,0x2174),('EBS request and sync payload',0x2a60,0x2c30),
  ('gBS constructor',0x456c,0x459c)],
 'ScmDxe':[
  ('first exit unresolved provider call',0x24e4,0x2534),('separate EBS buffer release',0x2804,0x2874),
  ('alternate exit conditional provider call',0x345c,0x3508)],
 'QcomBds':[('exit callback consumer calls',0xf860,0xf934)],
 'DALSys':[
  ('DAL table returned',0x141c,0x1460),('named property handle resolver',0x2ab0,0x2d28),
  ('property name comparison',0x2e04,0x2e50),('property value dispatcher',0x2eb4,0x3154),
  ('property base getter',0x3b08,0x3b18),('property set registration',0x3c0c,0x3c8c)]}
ANCHORS={
 'HALIOMMU':[(0x1558,'blr','x8'),(0x308c,'bl','#0x15b4'),(0x160c,'bl','#0x3864'),
  (0x38b0,'bl','#0x32b0'),(0x38f8,'bl','#0x32b0'),(0x3924,'bl','#0x327c'),(0x3938,'cbnz','w8, #0x38fc'),
  (0x32e4,'str','w0, [x8]'),(0x32a4,'ldr','w0, [x8]'),(0x2990,'bl','#0x2544'),(0x2a38,'bl','#0x2b50'),
  (0x2a64,'bl','#0x15ac'),(0x2c14,'bl','#0x36f0'),(0x2c34,'bl','#0x3690'),(0x4e4c,'bl','#0x6ab0'),(0x4e68,'bl','#0x6ad0')],
 'ShmBridgeDxe':[(0x14a0,'blr','x8'),(0x14ec,'bl','#0x2a30'),(0x297c,'sturb','wzr, [x8, #-4]'),(0x2988,'add','x12, x11, x8, lsl #12')],
 'VariableDxe':[(0x1f98,'blr','x10'),(0x1e68,'blr','x15'),(0x18c8,'bl','#0x2a60'),(0x18d4,'strb','w9, [x8, #0x280]'),
  (0x1900,'bl','#0x15ec'),(0x2b74,'mov','w8, #0x800b'),(0x2bc8,'ldr','x9, [x0, #0x28]'),(0x2bd4,'blr','x9')],
 'ScmDxe':[(0x2528,'blr','x9'),(0x34b0,'tbnz','w8, #1, #0x34bc'),(0x34ec,'blr','x10')],
 'DALSys':[(0x2c8c,'ldr','w8, [x8, #0xc]'),(0x2e20,'ldr','w8, [x0, #4]'),(0x3124,'add','x16, x21, x5, lsl #4'),(0x3128,'ldr','x6, [x16, #8]')]}

def iort(m):
    props,blob,structures,devices=0x7bc8,0x1ba38,0x1b618,0x1be08
    assert [m.q(props),m.q(props+8),m.u(props+16),m.q(props+24)]==[blob,structures,14,devices]
    assert m.q(0x1d220)==props and m.q(0x1d180)==0x2ca4 and m.q(0x1d1b8)==0x2eb4
    device=devices+7*0x28;assert device==0x1bf20 and m.cstr(m.q(device))=='SMMU'
    namehash=5381
    for ch in b'SMMU':namehash=(namehash*33+ch)&0xffffffff
    assert m.u(device+8)==namehash==0x7c8be4c7
    prop=blob+m.u(device+12);assert prop==0x1bc1c and m.u(prop)==0x128000af
    assert m.cstr(blob+m.u(blob+4)+(m.u(prop)&0x7fffff))=='IORT'
    assert m.u(prop+4)==9 and m.q(0x6d38)==0x30f4
    pointer=structures+m.u(prop+4)*16;assert pointer==0x1b6a8 and m.q(pointer)==8
    rva=m.q(pointer+8);assert rva==0x2bcf0 and m.raw(rva,4)==b'IORT'
    length=m.u(rva+4);table=m.raw(rva,length);assert length==4468 and table[8]==0
    count,pos=struct.unpack_from('<II',table,36);assert (count,pos)==(25,0x30)
    nodes=[]
    for _ in range(count):
        kind,size,rev=struct.unpack_from('<BHB',table,pos);assert size>=16 and pos+size<=len(table)
        n,off=struct.unpack_from('<II',table,pos+8);assert n==0 or 16<=off and off+20*n<=size
        row={'table_offset':pos,'pe_rva':rva+pos,'type':kind,'bytes':size,'revision':rev,'mapping_count':n,'mapping_offset':off}
        if kind==1:row['name']=table[pos+0x1d:table.index(0,pos+0x1d,pos+size)].decode('ascii')
        if kind==3:row['smmu_base'],row['smmu_span']=struct.unpack_from('<QQ',table,pos+0x10)
        row['raw_mappings']=[dict(zip(('input_base','raw_count_field','output_base','output_reference','flags'),struct.unpack_from('<IIIII',table,pos+off+20*i))) for i in range(n)]
        nodes.append(row);pos+=size
    assert pos==len(table)
    ufs=[n for n in nodes if n.get('name')==r'\_SB_.UFS0'];assert len(ufs)==1 and ufs[0]['table_offset']==0x760
    assert [tuple(x.values()) for x in ufs[0]['raw_mappings']]==[(0,1,0x4e0,0x30,0),(0x10,1,0xe0,0x30,0)]
    smmu=next(n for n in nodes if n['table_offset']==0x30);assert (smmu['smmu_base'],smmu['smmu_span'])==(0x15000000,0xe0000)
    out={'module':m.name,'property_set_rva':props,'property_blob_rva':blob,'device_record_rva':device,
         'device_name_hash':namehash,'property_record_rva':prop,'struct_index':9,'pointer_record_rva':pointer,
         'table_rva':rva,'table_file_offset':m.pe.get_offset_from_rva(rva),'bytes':length,'sha256':sha(table),
         'revision':table[8],'checksum_byte':table[9],'checksum_mod256':sum(table)&255,'acpi_checksum_valid':(sum(table)&255)==0,
         'node_count':count,'nodes_end_at_declared_length':True,'nodes':nodes,'ufs_node':ufs[0],'referenced_smmu_node':smmu,
         'limit':'Static DAL SMMU/IORT property template; not a runtime capture or proof of active SID/CB state or handoff.'}
    return out,table

def analyze(check=False):
    before=originals();manifest=json.loads((STUDY/'provider-candidates.json').read_text(encoding='utf-8'))
    assert manifest['tool_sha256']==EXTRACT_HASH and sha((STUDY/'extractor-v1.py').read_bytes())==EXTRACT_HASH
    mods={n:Module(n) for n in WINDOWS};windows=[];data=[];nanchors=0
    for name,m in mods.items():
        if name!='DALSys':assert sha(m.body)==next(x for x in manifest['matches'] if x['name']==name)['pe_sha256']
        for label,lo,hi in WINDOWS[name]:windows.append(m.window(label,lo,hi))
        for at,mn,op in ANCHORS.get(name,[]):m.expect(at,mn,op);nanchors+=1
    tables=[('HALIOMMU',0x9048,GUIDS['ufs_iommu'],0x9098,[1,0x14cc,0x1500]),
            ('ShmBridgeDxe',0x7058,GUIDS['scm_buffer_slot10'],0x7088,[0x10001,0x14d0,0x14e0]),
            ('VariableDxe',0xa168,GUIDS['bds_exit_slot8'],0xa220,[0x10000,0x18e0])]
    providers=[]
    for name,guid_rva,guid,tab,expected in tables:
        m=mods[name];assert m.raw(guid_rva,16)==uuid.UUID(guid).bytes_le
        assert [m.q(tab+8*i) for i in range(len(expected))]==expected
        data.extend([m.data('installed GUID',guid_rva,16),m.data('installed protocol table',tab,8*len(expected))])
        providers.append({'name':name,'guid':guid,'guid_rva':guid_rva,'table_rva':tab,'u64_slots':expected,
                          'provider_installation_statically_identified':True,'runtime_installation_observed':False})
    values=[0x16b4,0x1768,0x1920,0x27c0,0x2c88,0x3038,0x30c8,0x316c,0x2f10]
    assert [mods['HALIOMMU'].q(0x90b0+i*8) for i in range(9)]==values
    data.append(mods['HALIOMMU'].data('domain operations',0x90b0,72))
    for name,label,start,size in [
        ('HALIOMMU','DAL names',0x7afe,10),('VariableDxe','UEFI EBS event GUID',0xa128,16),
        ('VariableDxe','SCM GUID',0xa178,16),('VariableDxe','sync error text',0x7325,56),
        ('VariableDxe','secure app name',0x74a0,19),('ScmDxe','unresolved GUID',0xa078,16),
        ('DALSys','registered property set reference',0x1d210,32),('DALSys','DAL property vtable',0x1d180,0x40),
        ('DALSys','property set',0x7bc8,32),('DALSys','SMMU device record',0x1bf20,40),
        ('DALSys','SMMU name',0x8ae9,5),('DALSys','IORT property header',0x1bc1c,12),
        ('DALSys','IORT property name',0x1bb1f,5),('DALSys','IORT struct reference',0x1b6a8,16),
        ('DALSys','property type 0x12 branch',0x6d38,8)]:data.append(mods[name].data(label,start,size))
    iommu,table=iort(mods['DALSys']);save('iort-property-template.bin',table,check);save('iort-property.json',encode(iommu),check)
    free=next(w for w in windows if w['label']=='pool free bounded body')
    assert not any(i['mnemonic'] in ('bl','blr','br','smc','hvc') for i in free['instructions'])
    detach=next(w for w in windows if w['label']=='detach body')
    assert not any(i['direct_target'] in (0x3038,0x30c8,0x3864,0x3950) for i in detach['instructions'])
    assert before==originals()
    out={'schema_version':1,'scope':'Static provider follow-up only','capstone_version':capstone.__version__,'pefile_version':pefile.__version__,
         'sources_before':before,'sources_after':originals(),'firmware_executed':False,'providers':providers,
         'domain_table_rva':0x90b0,'domain_u64_slots':values,'unresolved_provider_guid':GUIDS['scm_exit_slot10'],
         'runtime_handoff_grant_proven':False,'asserted_instruction_anchor_count':nanchors,'windows':windows,'data_excerpts':data,
         'limits':['All addresses are PE RVAs, not callable live addresses.','Names are analyst semantics, not recovered symbols.',
                   'Static provider installation does not prove runtime success.','TLBI/TLBSYNC exists; UFS exit caller, drain and exclusive ownership remain unproved.',
                   'DAL property mapping is static; the template ACPI checksum is invalid.','Exact GUID searches cannot rule out dynamic or external providers.']}
    save('provider-analysis.json',encode(out),check)
    lines=['Static bounded evidence; addresses are PE RVAs; no firmware executed.','']
    for w in windows:
        lines.append(f"[{w['module']}] {w['label']} RVA 0x{w['rva_begin']:x}..0x{w['rva_end_exclusive']:x} end exclusive; SHA256 {w['sha256']}")
        for i in w['instructions']:lines.append(f"RVA {i['rva']:08x} FILE {i['pe_file_offset']:08x} {i['bytes']} {i['mnemonic']:8} {i['operands']}")
        lines.append('')
    for d in data:lines.extend([f"[{d['module']}] {d['label']} RVA {d['rva']:08x} FILE {d['pe_file_offset']:08x} SHA256 {d['sha256']}",d['raw_hex'],''])
    save('evidence-disassembly.txt',('\n'.join(lines)+'\n').encode(),check)
    return {'providers_resolved':len(providers),'unresolved':GUIDS['scm_exit_slot10'],'instruction_anchors':nanchors,
            'bounded_windows':len(windows),'iort_ufs_mappings':iommu['ufs_node']['raw_mappings'],
            'iort_checksum_mod256':iommu['checksum_mod256'],'complete_handoff_grant':False}

def verification(summary):
    body=(STUDY/'README.md').read_text(encoding='utf-8');checked=[]
    for link in re.findall(r'\[[^\]]*\]\(([^)]+)\)',body):
        if '://' in link or link.startswith('#'):continue
        assert (STUDY/link.split('#')[0]).resolve().exists(),link
        checked.append(link)
    for h in re.findall(r'SHA256: `([^`]+)`',body):assert re.fullmatch('[0-9a-f]{64}',h),h
    files=[]
    for p in sorted(STUDY.rglob('*')):
        if p.is_file() and p.name!='verification.json':files.append({'path':p.relative_to(STUDY).as_posix(),'bytes':p.stat().st_size,'sha256':sha(p.read_bytes())})
    out={'schema_version':1,'checks':summary,'original_inputs':originals(),'artifact_count':len(files),
         'analyzer_sha256':sha(Path(__file__).read_bytes()),'local_markdown_links_checked':checked,'firmware_executed':False,'device_actions':False,'artifacts':files}
    save('verification.json',encode(out));return {'verified_artifacts':len(files),'local_links':len(checked),**summary}
def main():
    p=argparse.ArgumentParser(description=__doc__);g=p.add_mutually_exclusive_group(required=True)
    g.add_argument('--extract',action='store_true');g.add_argument('--analyze',action='store_true');g.add_argument('--check',action='store_true');a=p.parse_args()
    if a.extract:
        snap=STUDY/'extractor-v1.py';assert sha(snap.read_bytes())==EXTRACT_HASH
        runpy.run_path(str(snap),run_name='provider_initial_extraction')['extract']()
        out=supplemental();print(json.dumps({'supplementary_module':out['supplementary_module']['name'],'scanned':out['scanned']},indent=2))
    elif a.analyze:print(json.dumps(analyze(),indent=2))
    else:
        supplemental(check=True);print(json.dumps(verification(analyze(check=True)),indent=2))
if __name__=='__main__':main()
