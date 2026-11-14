#!/usr/bin/env python3
"""Static firmware contract study. Never execute ELF/PE/SMC/MMIO/device operations.

Only writes analysis/stock-ufs-contract-20261113. --check re-derives immutable outputs.
ELF virtual addresses below are file analysis addresses, not callable addresses.
"""
import argparse,hashlib,json,re,runpy,struct,sys,uuid
from pathlib import Path
sys.dont_write_bytecode=True
import capstone,pefile
ROOT=Path(__file__).resolve().parents[2]
STUDY=ROOT/'analysis/stock-ufs-contract-20261113'
ROM=ROOT/'TB_J716F_ZUI_15.0.485/image'
PREV=ROOT/'analysis/stock-ufs-providers-20261113'
INPUTS={
 'imagefv.elf':'da9b3f6c7fe43737c96c277a34d961345595a2edd74ef57b553427dd7aba97ae',
 'xbl_config.elf':'da722c906d65b3b06f47c13f43f63ee0797a519a9a955ffba86edee808fcd1b4',
 'uefi_sec.mbn':'e03c03513609447b80b656df61b23452026248e9769e79e3444d302cf17ce455',
 'tz.mbn':'f0d068809af8dd6fdc59e16fbf4dcc1f13f939f31ce2517c99f2983f7c0724c4',
 'hyp.mbn':'341353305c118a949729c865340558fc2dddda4893536379b4745d0415a31893',
 'storsec.mbn':'72d51cb11124972a0148c4f6f69d34bb53e481abaf16a49755834a7f1b083a96',
 'devcfg.mbn':'1ab8d0848d94d535a1cb980c761d327c8a2ae09568f6ef03619612ad6923763e',
 'cmnlib64.mbn':'980a592ea6d4518df111f6f8b8635e8b481e456f67f114abb6bd3fc30a0740b8'}
GUID='b9be5350-c6c5-4fda-9b09-551ff061c0ba'
def sha(b):return hashlib.sha256(b).hexdigest()
def enc(v):return (json.dumps(v,ensure_ascii=False,indent=2)+'\n').encode()
def save(name,b,check=False):
 p=STUDY/name
 if check or p.exists():
  if not p.exists() or p.read_bytes()!=b:raise ValueError('Missing or differing evidence: '+str(p))
 else:p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(b)
def findall(b,p):
 out=[];at=0
 while True:
  at=b.find(p,at)
  if at<0:return out
  out.append(at);at+=1
def guid_hits(b):
 u=uuid.UUID(GUID)
 return {k:findall(b,v) for k,v in {'uefi_bytes_le':u.bytes_le,'network_bytes':u.bytes,
  'ascii_lower':GUID.encode(),'ascii_upper':GUID.upper().encode(),
  'utf16le_lower':GUID.encode('utf-16-le'),'utf16le_upper':GUID.upper().encode('utf-16-le')}.items()}
class Elf:
 def __init__(self,name,body):
  self.name=name;self.b=body;assert body[:4]==b'\x7fELF' and body[5]==1
  self.bits=body[4]*32;assert self.bits in (32,64)
  self.machine=struct.unpack_from('<H',body,18)[0]
  self.entry=struct.unpack_from('<I' if self.bits==32 else '<Q',body,24)[0]
  po=struct.unpack_from('<I' if self.bits==32 else '<Q',body,28 if self.bits==32 else 32)[0]
  es,n=struct.unpack_from('<HH',body,42 if self.bits==32 else 54);self.ph=[]
  for i in range(n):
   if self.bits==32:t,o,v,p,fs,ms,f,a=struct.unpack_from('<IIIIIIII',body,po+i*es)
   else:t,f,o,v,p,fs,ms,a=struct.unpack_from('<IIQQQQQQ',body,po+i*es)
   assert o+fs<=len(body)
   self.ph.append({'index':i,'type':t,'flags':f,'file_offset':o,'virtual_address':v,'physical_address':p,'file_bytes':fs,'memory_bytes':ms,'alignment':a,'sha256':sha(body[o:o+fs])})
 def off(self,va,size=1):
  p=next(x for x in self.ph if x['type']==1 and x['virtual_address']<=va and va+size<=x['virtual_address']+x['file_bytes'])
  return p['file_offset']+va-p['virtual_address']
 def raw(self,va,size):return self.b[self.off(va,size):self.off(va,size)+size]
 def dynamic(self):
  assert self.bits==32
  ph=next(x for x in self.ph if x['type']==2);tags={};entries=[]
  for off in range(ph['file_offset'],ph['file_offset']+ph['file_bytes'],8):
   k,v=struct.unpack_from('<II',self.b,off);entries.append({'file_offset':off,'tag':k,'value':v});tags[k]=v
   if not k:break
  assert tags[11]==16 and tags[19]==8
  nb,nc=struct.unpack('<II',self.raw(tags[4],8));strings=self.raw(tags[5],tags[10]);symbols=[]
  for i in range(nc):
   va=tags[6]+i*16;no,value,size,info,other,section=struct.unpack('<IIIBBH',self.raw(va,16))
   assert no<len(strings);name=strings[no:strings.index(0,no)].decode('ascii')
   symbols.append({'index':i,'record_va':va,'record_file_offset':self.off(va),'name':name,'value':value,
                   'bytes':size,'type':info&15,'binding':info>>4,'section_index':section,'other':other})
  rels=[]
  for va,size,label in [(tags[17],tags[18],'REL'),(tags[23],tags[2],'JMPREL')]:
   assert size%8==0
   for at in range(va,va+size,8):
    dest,info=struct.unpack('<II',self.raw(at,8));idx=info>>8;assert idx<len(symbols)
    rels.append({'table':label,'record_va':at,'record_file_offset':self.off(at),'destination_va':dest,'type':info&255,'symbol_index':idx,'symbol':symbols[idx]['name']})
  return {'elf':self.name,'dynamic':entries,'hash_buckets':nb,'hash_chains':nc,'symbols':symbols,'relocations':rels}

def window(name,read,offset,label,start,end,arch,mode):
 md=capstone.Cs(arch,mode);md.detail=True;b=read(start,end-start);items=[]
 for i in md.disasm(b,start):
  target=None
  if i.mnemonic in ('bl','blx','b','b.w') and i.operands and i.operands[0].type==capstone.CS_OP_IMM:target=i.operands[0].imm
  items.append({'address':i.address,'file_offset':offset(i.address),'bytes':i.bytes.hex(),'mnemonic':i.mnemonic,'operands':i.op_str,'direct_target':target})
 assert items and items[-1]['address']+len(bytes.fromhex(items[-1]['bytes']))==end,(name,label)
 return {'module':name,'label':label,'start':start,'end_exclusive':end,'bytes':end-start,'sha256':sha(b),'raw_hex':b.hex(),'instructions':items}

def run(check=False):
 before=[];images={};scans=[]
 for name,h in INPUTS.items():
  b=(ROM/name).read_bytes();assert sha(b)==h
  e=Elf(name,b);images[name]=e;before.append({'path':(ROM/name).relative_to(ROOT).as_posix(),'bytes':len(b),'sha256':h})
  scans.append({'name':name,'bits':e.bits,'machine':e.machine,'entry':e.entry,'program_headers':e.ph,'fv_signature_offsets':findall(b,b'_FVH'),'guid_hits':guid_hits(b)})
 assert all(not offsets for s in scans for offsets in s['guid_hits'].values())
 parser=ROOT/'analysis/stock-ufs-handoff-20261113/extractor-v1.py'
 assert sha(parser.read_bytes())=='9061a12238024593fa1221455c128687ea764e5d7141f812ffe9443f4edbdae8'
 base=runpy.run_path(str(parser),run_name='static_contract_helpers');base['file_walk'].__globals__['STUDY']=STUDY
 ffs=[]
 for guid,kind,secs in base['file_walk'](images['imagefv.elf'].b,'TB_J716F_ZUI_15.0.485/image/imagefv.elf'):
  name=next((b.decode('utf-16-le').rstrip('\0') for k,b,t in secs if k==0x15),'')
  ffs.append({'guid':guid,'kind':kind,'name':name,'sections':[{'type':k,'bytes':len(b),'sha256':sha(b),'head_hex':b[:24].hex(),'guid_hits':guid_hits(b),'trace':t} for k,b,t in secs]})
 assert len(ffs)==11 and all(s['type']!=0x10 for f in ffs for s in f['sections'])
 assert all(not off for f in ffs for s in f['sections'] for off in s['guid_hits'].values())
 b=images['xbl_config.elf'].b;assert b[0x3000:0x3004]==b'CFGL'
 assert struct.unpack_from('<H',b,0x3006)[0]==3 and struct.unpack_from('<I',b,0x3008)[0]==0x84
 config=[]
 for i in range(3):
  pos=0x300c+40*i;flags,relative,n,namelen=struct.unpack_from('<IIII',b,pos)
  name=b[pos+16:pos+16+namelen].rstrip(b'\0').decode();payload=b[0x3000+relative:0x3000+relative+n]
  assert len(payload)==n and namelen<=24
  assert any(p['type']==1 and p['file_offset']==0x3000+relative and p['file_bytes']==n for p in images['xbl_config.elf'].ph)
  config.append({'record_file_offset':pos,'flags_raw':flags,'name':name,'relative_offset':relative,'file_offset':0x3000+relative,'bytes':n,'sha256':sha(payload),'guid_hits':guid_hits(payload)})
 assert [x['name'] for x in config]==['/pmic_settings.bin','/6008_0100_1_dcb.bin','/6008_0200_1_dcb.bin']
 save('image-inventory.json',enc({'inputs':before,'guid':GUID,'scan_scope':'Exact six encodings in eight raw images and every parsed imagefv leaf section; not runtime absence proof.','images':scans,'imagefv_ffs':ffs,'xbl_config_observed_records':config}),check)
 e=images['uefi_sec.mbn'];dyn=e.dynamic();assert len(dyn['symbols'])==306
 save('uefi-sec-dynamic.json',enc(dyn),check)
 symbols={x['name']:x for x in dyn['symbols']};funcs={}
 for name,expected,size in [('svc_uefi_var_entry',0x589,1928),('TzSyncVarTables',0x7315,744),('SyncTable',0x15f5,152),('TzVariablePolicyExitBootServices',0x377d,24)]:
  s=symbols[name];assert (s['value'],s['bytes'],s['type'])==(expected,size,2)
  body=e.raw(expected&~1,size);save('function-bytes/'+name+'.bin',body,check)
  funcs[name]={**s,'instruction_mode':'Thumb; symbol low bit cleared only for byte addressing','function_sha256':sha(body)}
 jump=[struct.unpack('<H',e.raw(0x5da+2*i,2))[0]*2+0x5da for i in range(25)]
 assert (jump[0xa],jump[0xb])==(0x8a2,0x8e4)
 rel=next(x for x in dyn['relocations'] if x['destination_va']==0x1908c)
 assert rel['symbol']=='qsee_stor_write_sectors' and rel['type']==22
 # ARM veneer: architectural PC at 0x180 is 0x188, then +0x18000 +0xf04.
 assert e.raw(0x180,12).hex()=='00c68fe218ca8ce204ffbce5' and 0x188+0x18000+0xf04==0x1908c
 wins=[]
 for label,a,z in [('secure command dispatcher',0x588,0x5da),('EBS case 0x800a',0x8a2,0x8e4),('sync case validation 0x800b',0x8e4,0x908),
                   ('sync case payload',0xb36,0xb52),('response status',0xb86,0xb8c),('dispatcher return',0xbae,0xbbc),
                   ('TzSyncVarTables executable prefix',0x7314,0x753c),('SyncTable executable prefix',0x15f4,0x1688),
                   ('table validation helper',0x168c,0x16dc),('storage write wrapper',0xead8,0xebac),
                   ('EBS marker setter getter',0xf144,0xf14e),('EBS marker getter',0xf154,0xf15c),
                   ('variable policy EBS',0x377c,0x3790)]:
  wins.append(window(e.name,e.raw,e.off,label,a,z,capstone.CS_ARCH_ARM,capstone.CS_MODE_THUMB))
 wins.append(window(e.name,e.raw,e.off,'write-sectors ARM import veneer',0x180,0x18c,capstone.CS_ARCH_ARM,capstone.CS_MODE_ARM))
 pe_modules={}
 for name in ['UFSDxe','ScmDxe']:
  p=PREV/name/(name+'.efi');body=p.read_bytes();meta=json.loads((PREV/name/'pe.json').read_text());assert sha(body)==meta['sha256']
  pe=pefile.PE(data=body);assert pe.OPTIONAL_HEADER.ImageBase==0
  pe_modules[name]={'body':body,'pe':pe,'sha256':sha(body),'source_path':p.relative_to(ROOT).as_posix()}
  save('pe/'+name+'.efi',body,check)
 ranges={
  'UFSDxe':[('listener 0x2001 register',0xdb30,0xdb8c),('listener 0x2001 SCM lookup',0xdb8c,0xdbd0),
            ('listener 0x2001 dispatcher',0xdbd0,0xdc50),('BlockIoRefresh registration',0xdc50,0xdcd4),
            ('BlockIoRefresh flag callback',0xe5a0,0xe5c0),('listener 0x2000 register',0xe70c,0xe770),
            ('listener 0x2000 protocol lookup',0xe770,0xe828),('listener 0x2000 dispatcher',0xe828,0xe8a8),
            ('listener 0x103 request handler',0xec10,0xee3c)],
  'ScmDxe':[('register callback record',0x2498,0x24e4),('legacy exit loop',0x2534,0x2614),
            ('alternate register callback',0x32a4,0x337c),('alternate exit loop',0x34f8,0x35a4)]}
 for name,rs in ranges.items():
  data=pe_modules[name];pe=data['pe'];body=data['body']
  def read(a,n,pe=pe,body=body):
   off=pe.get_offset_from_rva(a);assert pe.get_offset_from_rva(a+n-1)==off+n-1;return body[off:off+n]
  for label,a,z in rs:wins.append(window(name,read,pe.get_offset_from_rva,label,a,z,capstone.CS_ARCH_ARM64,capstone.CS_MODE_LITTLE_ENDIAN))
 anchors=[('uefi_sec.mbn',0xb4c,'bl','#0x7314'),('uefi_sec.mbn',0x8da,'bl','#0x377c'),
          ('uefi_sec.mbn',0x1664,'bl','#0x168c'),('uefi_sec.mbn',0x166e,'mov','r8, r0'),
          ('uefi_sec.mbn',0x167e,'bl','#0xead8'),('uefi_sec.mbn',0x1682,'mov','r4, r8'),
          ('uefi_sec.mbn',0xeb70,'blx','#0x180'),('uefi_sec.mbn',0x73e8,'movs','r0, #0'),
          ('uefi_sec.mbn',0x73ea,'str','r0, [r5]'),('UFSDxe',0xdb60,'mov','w1, #0x2001'),
          ('UFSDxe',0xe73c,'mov','w1, #0x2000'),('UFSDxe',0xe5b8,'strb','w10, [x9, x8]'),
          ('ScmDxe',0x25d4,'mov','x19, x0'),('ScmDxe',0x354c,'mov','x19, x0')]
 for module,at,mn,op in anchors:
  ins=next(i for w in wins if w['module']==module for i in w['instructions'] if i['address']==at)
  assert (ins['mnemonic'],ins['operands'])==(mn,op),(module,hex(at),ins)
 # Mark data jump tables as data, not executable instructions.
 uf=pe_modules['UFSDxe']['body'];tables={}
 for a in [0x16120,0x16408]:tables[hex(a)]=list(struct.unpack_from('<QQQQ',uf,a))
 assert tables['0x16408']==[0xe868,0xe884,0xe890,0xe89c]
 assert tables['0x16120']==[0xdc10,0xdc2c,0xdc38,0xdc44]
 assert uuid.UUID(bytes_le=uf[0x181e8:0x181f8])==uuid.UUID('77ed108d-8524-4b8b-9d2e-34987aecb9c1')
 refs=[]
 for name in ['Include/Protocol/EFIScm.h','QcomPkg.dec']:
  p=ROOT/'work/uefi-j716f/Silicon/Qualcomm/QcomPkg'/name;b=p.read_bytes()
  save('references/'+p.name,b,check);refs.append({'path':p.relative_to(ROOT).as_posix(),'sha256':sha(b)})
 assert b'0xB1EB3D10' in (STUDY/'references/QcomPkg.dec').read_bytes()
 result={'schema_version':1,'symbols':funcs,'command_jump_table_va':0x5da,'command_targets':jump,
         'write_import_relocation':rel,'write_veneer_va':0x180,'write_import_slot_va':0x1908c,
         'ufsdxe_dispatch_tables':tables,'pe_inputs':[{k:v for k,v in d.items() if k not in ('body','pe')} for d in pe_modules.values()],
         'reference_headers':refs,'instruction_anchor_count':len(anchors),'windows':wins,
         'limits':['ELF dynamic symbols are original evidence; function-local literal pools excluded from executable windows.',
                   'No static result proves runtime execution, successful queue drain, TZ/HYP silence, or a complete HandoffGrant.',
                   'GUID source-negative result is bounded; no runtime absence claim.',
                   'Listener response payload status and callback return are distinct; no UFS drain inference from callback EFI_SUCCESS.'],
         'firmware_executed':False,'device_actions':False,'complete_handoff_grant':False}
 save('contract-evidence.json',enc(result),check)
 lines=['Static evidence only. ELF addresses are virtual addresses; PE addresses are RVAs.','']
 for w in wins:
  lines.append(f"[{w['module']}] {w['label']} {w['start']:x}..{w['end_exclusive']:x} (end exclusive), SHA256 {w['sha256']}")
  for i in w['instructions']:lines.append(f"{i['address']:08x} FILE {i['file_offset']:08x} {i['bytes']:8} {i['mnemonic']:9} {i['operands']}")
  lines.append('')
 save('evidence-disassembly.txt',('\n'.join(lines)+'\n').encode(),check)
 for x in before:assert sha((ROOT/x['path']).read_bytes())==x['sha256']
 return {'raw_images':len(before),'imagefv_records':len(ffs),'imagefv_executable_PE_sections':0,'secure_symbols':len(dyn['symbols']),
         'instruction_anchors':len(anchors),'bounded_windows':len(wins),'guid_provider_found':False,'inputs_unchanged':True,'complete_handoff_grant':False}
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--check',action='store_true');a=p.parse_args()
 result=run(a.check)
 if a.check:
  body=(STUDY/'README.md').read_text(encoding='utf-8');links=re.findall(r'\[[^\]]*\]\(([^)]+)\)',body)
  for link in links:
   if '://' not in link and not link.startswith('#'):assert (STUDY/link).resolve().exists(),link
  files=[{'path':x.relative_to(STUDY).as_posix(),'bytes':x.stat().st_size,'sha256':sha(x.read_bytes())} for x in sorted(STUDY.rglob('*')) if x.is_file() and x.name!='verification.json']
  save('verification.json',enc({'checks':result,'analyzer_sha256':sha(Path(__file__).read_bytes()),'artifacts':files,'firmware_executed':False,'device_actions':False}))
  result['verified_artifacts']=len(files)
 print(json.dumps(result,indent=2))
if __name__=='__main__':main()
