"""Read-only, targeted stock config extraction and instruction evidence.
Only debugfs (without -w) reads two fixed files; no ROM executable is run.
No complete super unpack, VM, device, firmware write, or HID action.
"""
from pathlib import Path
import hashlib
import json
import struct
import subprocess
import xml.etree.ElementTree as ET
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

ROOT=Path(__file__).resolve().parents[2]
OUT=Path(__file__).resolve().parent
ROM=ROOT/'TB_J716F_ZUI_15.0.485/image'
def need(ok,why):
    if not ok: raise ValueError(why)
def sha(data): return hashlib.sha256(data).hexdigest()
def file_hash(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        while b:=f.read(1024*1024): h.update(b)
    return h.hexdigest()
def write(name,data):
    (OUT/name).write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
def config(data):
    need(len(data)>=18,'config short')
    length=struct.unpack_from('<I',data)[0]
    need(length==len(data) and sum(data[5:])&255==data[4],'config length/checksum')
    count=data[9];need(count==1,'review different package count')
    offset=struct.unpack_from('<H',data,16)[0];need(offset==18,'package table')
    p=data[offset:];need(len(p)>=121,'package header')
    need(struct.unpack_from('<I',p)[0]==len(p),'package length')
    name=p[4:19].split(b'\0')[0].decode()
    need(name in ('phoneix','phoenix') and p[19]==1 and p[20]==8 and p[21:25]==b'7386','stock identity')
    fields=('cfg_send_flag','version_base','pid','vid','sensor_id','fw_mask','fw_status',
            'cfg_addr','esd','command','coor','gesture','fw_request','proximity')
    registers={key:{'address':hex(struct.unpack_from('<H',p,56+4*i)[0]),
                    'parameter':p[58+4*i],'reserved':p[59+4*i]} for i,key in enumerate(fields)}
    need(registers['coor']['address']=='0x824d','coordinate register drift')
    return {'bytes':len(data),'sha256':sha(data),'file_checksum':data[4],
            'package_offset':offset,'package_bytes':len(p),'ic_type':name,'cfg_type':p[19],
            'sensor_id':p[20],'pid':p[21:29].hex(),'vid_match_field':p[29:37].hex(),
            'fw_mask_match_field':p[37:46].hex(),'registers':registers,
            'payload_bytes':len(p)-121,'payload_sha256':sha(p[121:]),
            'note':'Package VID/fw-mask fields are zero; observed running VID is separately bound.'}
def main():
    vendor=ROM/'super_6.img';tool=ROOT/'tools/e2fsprogs/debugfs.exe'
    before=file_hash(vendor)
    metadata=(ROM/'super_1.img').read_bytes();at=12288
    magic,major,minor,hs=struct.unpack_from('<IHHI',metadata,at)
    need((magic,major,minor,hs)==(0x414c5030,10,2,256),'LP identity')
    header=bytearray(metadata[at:at+hs]);claimed=bytes(header[12:44]);header[12:44]=bytes(32)
    need(sha(header)==claimed.hex(),'LP header checksum')
    ts=struct.unpack_from('<I',metadata,at+44)[0];tables=metadata[at+hs:at+hs+ts]
    need(sha(tables)==metadata[at+48:at+80].hex(),'LP tables checksum')
    po,pn,ps=struct.unpack_from('<III',metadata,at+80)
    eo,en,es=struct.unpack_from('<III',metadata,at+92)
    entries=[tables[po+i*ps:po+(i+1)*ps] for i in range(pn)]
    parts=[p for p in entries if p[:36].split(b'\0')[0]==b'vendor_a'];need(len(parts)==1,'vendor partition')
    _,first,num,_=struct.unpack_from('<4I',parts[0],36);need(num==1 and first<en,'vendor extent')
    sectors,kind,start,source=struct.unpack_from('<QIQI',tables,eo+first*es)
    need(kind==source==0 and sectors*512==vendor.stat().st_size,'raw vendor extent')
    programs=ET.parse(ROM/'rawprogram0_unsparse.xml').getroot().findall('program')
    firstprog=next(p for p in programs if p.get('filename')=='super_1.img')
    vendorprog=next(p for p in programs if p.get('filename')=='super_6.img')
    need((int(vendorprog.get('start_sector'))-int(firstprog.get('start_sector')))*4096==start*512,'rawprogram mapping')
    with vendor.open('rb') as f:f.seek(1024+56);need(f.read(2)==b'\x53\xef','ext4 magic')
    extracted={}
    (OUT/'stock').mkdir(exist_ok=True)
    for name,size in [('goodix_cfg_group.bin',583),('goodix_firmware.bin',190720)]:
        run=subprocess.run([str(tool),'-R','cat /firmware/'+name,str(vendor)],
                           stdout=subprocess.PIPE,stderr=subprocess.PIPE,check=True)
        need(len(run.stdout)==size,'unexpected extracted file size')
        destination=OUT/'stock'/name
        if destination.exists():need(destination.read_bytes()==run.stdout,'saved extraction differs')
        else:destination.write_bytes(run.stdout)
        extracted[name]={'path':'/firmware/'+name,'bytes':size,'sha256':sha(run.stdout)}
    after=file_hash(vendor);need(before==after,'vendor changed')
    with (ROM/'boot.img').open('rb') as f:
        h=f.read(4096);need(h[:8]==b'ANDROID!' and struct.unpack_from('<I',h,8)[0]==39528464,'boot kernel size')
        kernel=f.read(39528464)
    need(sha(kernel)=='a60d6de51822a18358ad005991bb11c08b34e0e66f28589cdab738902eb7d46c','stock kernel identity')
    cs=Cs(CS_ARCH_ARM64,CS_MODE_ARM)
    facts={
      'prefix16':(0xa75398,'mov','w3, #0x10'),
      'touch_status_bit7':(0xa753c0,'tbz','w8, #0x1f, #0xa757b0'),
      'count_byte1':(0xa753f0,'ldrb','w26, [x22, #1]'),
      'pen_bit5':(0xa753f8,'tst','w26, #0x20'),
      'count_low4':(0xa753fc,'and','w23, w26, #0xf'),
      'count_limit10':(0xa75404,'cmp','w23, #0xa'),
      'zero_skips_checksum':(0xa75414,'cbz','w23, #0xa75448'),
      'checksum_from_byte1':(0xa75428,'add','x11, x22, #1'),
      'checksum_add':(0xa75430,'add','w8, w12, w8'),
      'checksum_mod256':(0xa75440,'tst','w8, #0xff'),
      'tail_eight_per_contact':(0xa757c8,'add','w24, w8, w23, lsl #3'),
      'read_tail_reg_plus16':(0xa757d8,'add','w1, w9, #0x10'),
      'remap_high_bit_skip':(0xa75fd0,'tbnz','w3, #0x1f, #0xa75fb4'),
      'record_header2':(0xa762d8,'add','x9, x9, #2'),
      'record_stride8':(0xa76304,'add','x9, x9, #8'),
      'x_le16_offset1':(0xa76328,'ldurh','w14, [x9, #1]'),
      'y_le16_offset3':(0xa7632c,'ldurh','w16, [x9, #3]'),
      'width_le16_offset5':(0xa76334,'ldurh','w13, [x9, #5]'),
      'width_zero_release_branch':(0xa76368,'cbnz','w13, #0xa762f0'),
    }
    evidence={}
    for label,(offset,op,args) in facts.items():
        ins=list(cs.disasm(kernel[offset:offset+4],offset));need(len(ins)==1,'instruction decode')
        i=ins[0];need((i.mnemonic,i.op_str)==(op,args),'instruction drift '+label)
        evidence[label]={'kernel_image_offset':hex(offset),'bytes':i.bytes.hex(),'op':op,'args':args}
    marker=b'goodix_touch_handler\0'
    need(kernel[0x1d72a71:0x1d72a71+len(marker)]==marker,'handler string anchor')
    rows=[{'kernel_image_offset':hex(i.address),'bytes':i.bytes.hex(),'op':i.mnemonic,'args':i.op_str}
          for i in cs.disasm(kernel[0xa75370:0xa766a0],0xa75370)]
    write('stock-protocol-disassembly.json',rows)
    report={'source_rom':'TB_J716F_ZUI_15.0.485','runtime_qualified':False,'raw_touch_capture_available':False,
      'device_actions':False,'image_executed':False,'full_super_unpack':False,
      'vendor':{'path':str(vendor.relative_to(ROOT)),'bytes':vendor.stat().st_size,'sha256':before,
                'post_sha256':after,'rawprogram_start_sector':vendorprog.get('start_sector'),
                'lp_start_sector512':start,'lp_sectors512':sectors},
      'extractor':{'path':str(tool.relative_to(ROOT)),'sha256':file_hash(tool),'write_mode':False},
      'extracted':extracted,'config':config((OUT/'stock/goodix_cfg_group.bin').read_bytes()),
      'kernel':{'boot_path':'TB_J716F_ZUI_15.0.485/image/boot.img','offset':4096,'bytes':len(kernel),'sha256':sha(kernel)},
      'facts':evidence,'address_basis':'Image-relative offsets, not runtime VAs; functions identified by string xrefs and instruction flow.',
      'limitations':['No raw live touch packet; test fixtures are synthetic.',
        'Core has no I2C, power, IRQ, DMA, ESD/config write or HID publication.',
        'Raw endpoint dimensions are accepted inclusively; orientation/pixel conversion remains unqualified.']}
    write('stock-protocol-evidence.json',report)
    print(json.dumps({'config_sha256':report['config']['sha256'],'facts':len(facts),'vendor_unchanged':before==after}))
if __name__=='__main__':main()
