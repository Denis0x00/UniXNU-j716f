#!/usr/bin/env python3
"""Reproduce static stock XBL/ABL extraction and bounded call-path evidence.

Never executes firmware, communicates with a device/VM, or writes outside STUDY.
--extract uses the byte-pinned original extractor. --analyze emits evidence;
--check re-decodes input bytes and compares every generated evidence artifact.
"""
import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
import sys
import uuid
sys.dont_write_bytecode = True
import capstone
import pefile

ROOT = Path(__file__).resolve().parents[2]
STUDY = ROOT / "analysis/stock-ufs-handoff-20261113"
SNAPSHOT_SHA = "9061a12238024593fa1221455c128687ea764e5d7141f812ffe9443f4edbdae8"
INPUTS = {
    "xbl.elf": "d665a97c8b53288575a8720095e1032f1c8c29c49c9a716158013bf9a50e1088",
    "abl.elf": "694e55a6bb205e3bd31cf803dbc29bc6ee1d1ea40435c3e96ed79559cdf04e1d",
}
# These are inspected spans, NOT an automatically complete function inventory.
SPANS = {
 "UFSDxe": [(0x114c,0x14d4,"entry_and_constructors"), (0x23dc,0x2c3c,"main_and_notify_registration"),
  (0x2d20,0x2d90,"sleep_callback"),(0x5bc8,0x5c58,"gBS_constructor"),
  (0x6258,0x6278,"sleep_registration_wrapper"),(0x639c,0x6488,"kernel_protocol_resolution"),
  (0x7520,0x7590,"library_destructor"),(0x8e3c,0x8ecc,"gate_wrappers_and_smmu_thunk"),
  (0xb608,0xb6a4,"clock_gate_ungate"),(0xc358,0xc59c,"UIC_command"),
  (0xcc24,0xcddc,"hibernate_command_path"),(0xda60,0xdcd4,"SCM_listener_and_refresh_event"),
  (0xe5a0,0xe5c0,"refresh_callback"),(0x1303c,0x1325c,"clock_protocol_operations"),
  (0x136f4,0x138d0,"IOMMU_domain_configuration")],
 "ScmDxe": [(0x114c,0x14b0,"entry_and_constructors"),(0x14ec,0x15b0,"protocol_install_and_optional_event"),
  (0x24e4,0x2848,"legacy_exit_handler_and_buffer_event"),(0x345c,0x35a4,"alternate_exit_handler"),
  (0x3efc,0x3f40,"SetMem_wrapper"),(0x3fec,0x40c0,"SetMem_body"),
  (0x4580,0x45bc,"gBS_constructor")],
 "QcomBds": [(0x1150,0x1530,"entry_constructors_register_EBS"),
  (0x78d4,0x7908,"gBS_constructor"),(0xf860,0xf93c,"EBS_callback_SCM_dispatch"),
  (0x1015c,0x101e8,"EBS_event_registration")],
 "LinuxLoader": [(0x1000,0x10c4,"entry_and_first_constructor"),
  (0x4360,0x4448,"gBS_constructor"),(0xfdf0,0xff90,"memory_map_and_EBS"),
  (0x13280,0x134b0,"caller_of_shutdown_services"),(0x2f0e8,0x2f0ec,"pre_EBS_noop")],
}
# Independent exact decoder assertions at decisive branches/loads. Other bytes
# are pinned by full PE digest and per-span bytes+digest in control-flow.json.
ANCHORS = {
 "UFSDxe": {
  0x1190:("bl","#0x123c"),0x120c:("bl","#0x14d0"),0x14d0:("b","#0x2474"),
  0x5be8:("ldr","x8, [x1, #0x60]"),0x5bf0:("str","x8, [x10, #0x8e0]"),
  0x2c14:("bl","#0x6258"),0x625c:("ldr","x8, [x9, #0x920]"),
  0x6264:("ldr","x10, [x8, #0x40]"),0x6268:("ldr","x2, [x10, #0x58]"),0x626c:("br","x2"),
  0x63c8:("add","x0, x0, #0x298"),0x63d4:("ldr","x9, [x11, #0x140]"),
  0xb620:("bl","#0xcc24"),0xb644:("bl","#0x1303c"),0xb670:("bl","#0x1303c"),0xb67c:("bl","#0xcc24"),
  0xcc60:("mov","w1, #0x17"),0xcc74:("bl","#0xc358"),0xcca8:("mov","w1, #0x18"),0xccbc:("bl","#0xc358"),
  0xc3cc:("sub","w9, w21, #3"),0xc3d0:("cmp","w9, #1"),0xc3d4:("b.hi","#0xc414"),
  0xc3d8:("mov","w9, #0x4058"),0xc3dc:("mov","w10, #0x4078"),
  0xc464:("str","w21, [x13, x14]"),0x13184:("ldr","x4, [x0, #0x18]"),0x131b8:("ldr","x5, [x0, #0x10]"),
  0xdc7c:("ldr","x8, [x9, #0x170]"),0xdc8c:("blr","x8"),
  0xdb60:("mov","w1, #0x2001"),0xdb64:("blr","x12"),0xdb88:("b","#0xdc50"),
  0x2420:("bl","#0xda60"),0x2580:("bl","#0x8eac"),0x2590:("bl","#0x8eac"),0x8eb4:("bl","#0x136f4"),
  0x137a4:("blr","x10"),0x137d8:("ldr","x13, [x12, #0x40]"),
  0x13830:("mov","w2, wzr"),0x138a4:("mov","w2, #0x10"),
 },
 "ScmDxe": {
  0x45a0:("ldr","x8, [x1, #0x60]"),0x45a8:("str","x8, [x10, #0x3e0]"),
  0x1538:("blr","x8"),0x1570:("blr","x9"),0x1594:("bl","#0x2614"),
  0x2794:("add","x2, x2, #0x804"),0x2798:("ldr","x17, [x10, #0x170]"),0x27b0:("blr","x17"),
  0x2814:("mov","w1, #0x1000"),0x2818:("mov","w2, wzr"),0x2820:("bl","#0x3efc"),
  0x2830:("ldr","x8, [x0, #0x10]"),0x2834:("blr","x8"),0x283c:("str","xzr, [x19, #0x378]"),
  0x3f3c:("b","#0x3fec"),0x3fec:("dup","v0.16b, w2"),
  0x2528:("blr","x9"),0x25d0:("bl","#0x18a0"),0x34ec:("blr","x10"),0x3548:("bl","#0x2be4"),
 },
 "QcomBds": {
  0x1194:("bl","#0x123c"),0x14e4:("bl","#0x1015c"),
  0x78f4:("ldr","x8, [x1, #0x60]"),0x78fc:("str","x8, [x10, #0x698]"),
  0x1017c:("add","x2, x2, #0x860"),0x10180:("add","x4, x4, #0x148"),0x10198:("blr","x8"),
  0xf8c0:("add","x0, x0, #0x258"),0xf8c8:("ldr","x11, [x9, #0x140]"),0xf8d0:("blr","x11"),
  0xf91c:("cbz","x0, #0xf930"),0xf920:("ldr","x1, [x0, #0x30]"),0xf92c:("br","x1"),
 },
 "LinuxLoader": {
  0x1054:("bl","#0x10a8"),0x10b8:("bl","#0x4360"),
  0x43e8:("ldr","x8, [x19, #0x60]"),0x43ec:("str","x8, [x20, #0x10]"),
  0x1330c:("bl","#0xfdf0"),0xfe38:("bl","#0x2f0e8"),0x2f0e8:("ret",""),
  0xfe7c:("ldr","x8, [x8, #0x38]"),0xfe80:("blr","x8"),
  0xfec8:("ldr","x8, [x8, #0x38]"),0xfecc:("blr","x8"),
  0xfee0:("ldr","x8, [x8, #0xe8]"),0xfee4:("blr","x8"),0xfeec:("mov","x0, x26"),0xfefc:("b","#0xfe60"),
 },
}
GUIDS = {
 "UFSDxe": {0x18178:("BlockIoRefresh","b1eb3d10-9d67-40ca-9559-f1488b1b2ddb"),
  0x181b8:("Clock","241afae6-885f-4f6c-a7ea-c28eab79c3e5"),
  0x181e8:("SCM","77ed108d-8524-4b8b-9d2e-34987aecb9c1"),
  0x18228:("IOMMU, identified by registration/error xrefs","54b6d3b4-5d33-4f91-8600-6c41d5deb19a"),
  0x18298:("Kernel","b5062be7-170b-4a32-be21-689262ff4399")},
 "ScmDxe": {0xa028:("EBS event group","27abf055-b1b8-4c26-8048-748f37baa2df"),
  0xa068:("SCM","77ed108d-8524-4b8b-9d2e-34987aecb9c1"),
  0xa078:("provider unresolved","b9be5350-c6c5-4fda-9b09-551ff061c0ba"),
  0xa088:("buffer provider unresolved","9c1eb71f-dd6c-4ed5-9f6a-5cc0ca789f16")},
 "QcomBds": {0x18148:("EBS event group","27abf055-b1b8-4c26-8048-748f37baa2df"),
  0x18258:("SCM","77ed108d-8524-4b8b-9d2e-34987aecb9c1"),
  0x182a8:("provider unresolved","f9085b9d-9304-40fb-8fe0-4aee3b1a784b")},
 "LinuxLoader": {0x4c5c4:("SCM","77ed108d-8524-4b8b-9d2e-34987aecb9c1")},
}
EDGES = [
 {"id":"ufs-main","module":"UFSDxe","path":["0x114c","BL@0x120c -> 0x14d0","B@0x14d0 -> 0x2474"],"kind":"direct"},
 {"id":"ufs-sleep-register","module":"UFSDxe","path":["0x2474","BL@0x2c14 -> 0x6258","KernelProtocol.MpCpu(+0x40).RegisterPwrTransitionNotify(+0x58)"],"callback":"0x2d20","kind":"typed-indirect","condition":"Kernel protocol/global must resolve; registration success unobserved"},
 {"id":"ufs-sleep-gate","module":"UFSDxe","path":["0x2d20 mode=1","0x8e3c -> 0xb608","0xcc24(mode=1)","0xc358(command=0x17)","0x1303c(mode=0) -> Clock.DisableClock(+0x18)"],"kind":"conditional-direct-plus-typed-indirect","condition":"handle and software state checks; UIC and clock errors have exits"},
 {"id":"ufs-sleep-ungate","module":"UFSDxe","path":["0x2d20 mode=2","0x8e74 -> 0xb658","0x1303c(mode=1) -> Clock.EnableClock(+0x10)","0xcc24(mode=0)","0xc358(command=0x18)"],"kind":"conditional-direct-plus-typed-indirect"},
 {"id":"ufs-scm-notify","module":"UFSDxe","path":["CreateEvent@0x2ad4","RegisterProtocolNotify@0x2b24 for SCM GUID"],"callback":"0x23dc","kind":"event-registration","condition":"not EBS; success unobserved"},
 {"id":"ufs-refresh-register","module":"UFSDxe","path":["0x23dc callback","BL@0x2420 -> 0xda60","SCM+0x20 RegisterCallback(0x2001, 0xdbd0, ...)","B@0xdb88 -> 0xdc50","CreateEventEx@0xdc8c, BlockIoRefresh GUID"],"callback":"0xe5a0","kind":"conditional-direct-plus-typed-indirect","condition":"initialization guards may return first; callback sets a software flag, not UFS queue stop"},
 {"id":"stock-ebs-register","module":"QcomBds","path":["0x1150","BL@0x1194 -> constructors 0x123c","BL@0x14e4 -> 0x1015c","CreateEventEx@0x10198, EBS GUID"],"callback":"0xf860","kind":"event-registration","condition":"constructor path present, event registration success unobserved"},
 {"id":"stock-ebs-scm","module":"QcomBds","path":["0xf860","LocateProtocol(SCM GUID)@0xf8d0","if nonnull SCM+0x30 tail call @0xf92c"],"kind":"typed-indirect","condition":"runtime provider must be present; untraced external provider +8 occurs earlier"},
 {"id":"scm-exit-vtables","module":"ScmDxe","path":["install SCM table at 0xa0b8 OR 0xa100","table+0x30 -> 0x24e4 OR 0x345c"],"kind":"installed-function-table","condition":"mode selected by 0x35a4 result; handler includes opaque external protocol and secure-call descendants"},
 {"id":"scm-buffer-ebs","module":"ScmDxe","path":["0x14ec","conditional BL@0x1594 -> 0x2614","CreateEventEx@0x27b0, EBS GUID"],"callback":"0x2804","kind":"event-registration","condition":"SecurityFlag and allocation/protocol conditions; zeroes 4096 bytes then calls buffer provider +0x10"},
 {"id":"abl-ebs","module":"LinuxLoader","path":["BL@0x1330c -> 0xfdf0","GetMemoryMap@0xfe80/0xfecc","ExitBootServices@0xfee4","error: free-pages then retry @0xfe60"],"kind":"direct-plus-typed-indirect","condition":"static code path; not a trace proving current BootShim reached it"},
 {"id":"ufs-smmu-init","module":"UFSDxe","path":["0x2474","conditional 0x2580/0x2590 -> 0x8eac","BL@0x8eb4 -> 0x136f4","IOMMU LocateProtocol -> provider +8 -> domain ops 0/0x40/0x10, ARID 0 and 0x10"],"kind":"conditional-direct-plus-unresolved-provider","condition":"configuration/attach path, no detach/revoke proof"},
]
SOURCES = [
 "work/uefi-j716f/Silicon/Qualcomm/QcomPkg/QcomPkg.dec",
 "work/uefi-j716f/Silicon/Qualcomm/QcomPkg/Include/Protocol/EFIKernelInterface.h",
 "work/uefi-j716f/Silicon/Qualcomm/QcomPkg/Include/Protocol/EFIScm.h",
 "work/uefi-j716f/Silicon/Qualcomm/QcomPkg/Include/Protocol/EFIClock.h",
]

def sha(b): return hashlib.sha256(b).hexdigest()
def encoded(x): return (json.dumps(x,ensure_ascii=False,indent=2)+"\n").encode()
def emit(path,body,check):
 p=STUDY/path
 if p.exists():
  if p.read_bytes()!=body: raise ValueError("Evidence differs: "+str(p))
 elif check: raise ValueError("Missing evidence: "+str(p))
 else:
  p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(body)

def analyze(check):
 original=[]
 for name,expected in INPUTS.items():
  p=ROOT/"TB_J716F_ZUI_15.0.485/image"/name;b=p.read_bytes()
  if sha(b)!=expected:raise ValueError("Original ROM digest mismatch: "+name)
  original.append({"path":p.relative_to(ROOT).as_posix(),"bytes":len(b),"sha256_before":sha(b)})
 ex=json.loads((STUDY/"extraction.json").read_text())
 if sha((STUDY/"extractor-v1.py").read_bytes())!=SNAPSHOT_SHA or ex["tool_sha256"]!=SNAPSHOT_SHA:
  raise ValueError("Extractor snapshot mismatch")
 md=capstone.Cs(capstone.CS_ARCH_ARM64,capstone.CS_MODE_ARM)
 spans=[];anchors=[];guids=[];negatives=[];tables=[]
 for rec in ex["modules"]:
  if "extracted" not in rec:continue
  name=rec["name"];d=(ROOT/rec["extracted"]["path"]).read_bytes()
  if sha(d)!=rec["extracted"]["sha256"]:raise ValueError("PE digest mismatch: "+name)
  if name not in SPANS:continue
  pe=pefile.PE(data=d,fast_load=False)
  def off(rva):return pe.get_offset_from_rva(rva)
  for begin,end,label in SPANS[name]:
   body=d[off(begin):off(end-1)+1]
   if len(body)!=end-begin:raise ValueError("Noncontiguous inspected span")
   instructions=[]
   for addr in range(begin,end,4):
    raw=d[off(addr):off(addr)+4]; decoded=list(md.disasm(raw,addr,count=1))
    if decoded: i=decoded[0];instructions.append({"rva":addr,"file_offset":off(addr),"bytes":raw.hex(),"mnemonic":i.mnemonic,"operands":i.op_str})
    else: instructions.append({"rva":addr,"file_offset":off(addr),"bytes":raw.hex(),"mnemonic":".word","operands":hex(int.from_bytes(raw,"little"))})
   snippet="\n".join(f'{x["rva"]:08x}  {x["file_offset"]:08x}  {x["bytes"]}  {x["mnemonic"]:8} {x["operands"]}' for x in instructions)+"\n"
   rel=f"snippets/{name}-{label}.txt";emit(rel,snippet.encode(),check)
   calls=[x for x in instructions if x["mnemonic"] in ("bl","blr","br","b")]
   spans.append({"module":name,"label":label,"begin_rva":begin,"end_rva_exclusive":end,"file_offset":off(begin),"sha256":sha(body),"bytes_hex":body.hex(),"snippet":rel,"branches_and_calls":calls})
  for addr,(mn,op) in ANCHORS[name].items():
   raw=d[off(addr):off(addr)+4];ins=list(md.disasm(raw,addr,count=1))
   if len(ins)!=1 or (ins[0].mnemonic,ins[0].op_str)!=(mn,op):raise ValueError(f"Anchor mismatch {name}@{addr:x}")
   anchors.append({"module":name,"rva":addr,"file_offset":off(addr),"bytes":raw.hex(),"mnemonic":mn,"operands":op})
  for addr,(role,guid) in GUIDS[name].items():
   raw=d[off(addr):off(addr)+16]
   if raw!=uuid.UUID(guid).bytes_le:raise ValueError("GUID mismatch")
   guids.append({"module":name,"rva":addr,"file_offset":off(addr),"role":role,"guid":guid,"bytes":raw.hex()})
  if name=="UFSDxe":
   for role,g in [("EBS group","27abf055-b1b8-4c26-8048-748f37baa2df"),("DriverBinding","18a031ab-b443-4d1a-a5c0-0c09261e9f71")]:
    b=uuid.UUID(g).bytes_le;matches=[n for n in range(len(d)-15) if d[n:n+16]==b]
    if matches:raise ValueError("Negative raw GUID result changed")
    negatives.append({"module":name,"kind":"raw bytes search ONLY","name":role,"guid":g,"matches":matches,"not_proven":"No constructed/dynamic registration, Stop routine, or external callback exists"})
  if name=="ScmDxe":
   for addr,expected in [(0xa0b8,0x24e4),(0xa100,0x345c)]:
    value=struct.unpack_from("<Q",d,off(addr+0x30))[0]
    if value!=expected:raise ValueError("SCM table mismatch")
    tables.append({"module":name,"table_rva":addr,"revision":struct.unpack_from("<Q",d,off(addr))[0],"member_offset":0x30,"target_rva":value,"basis":"installed under exact SCM GUID; common 7-member prefix matches EFIScm.h; stock revision differs from current header"})
 refs=[{"path":p,"sha256":sha((ROOT/p).read_bytes())} for p in SOURCES]
 for rec in original:
  rec["sha256_after"]=sha((ROOT/rec["path"]).read_bytes())
  if rec["sha256_after"]!=rec["sha256_before"]:raise ValueError("ROM changed")
 result={"schema_version":1,"device":"TB-J716F","rom":"ZUI 15.0.485","firmware_executed":False,"device_actions":False,
  "tool_sha256":sha(Path(__file__).read_bytes()),"extractor_snapshot_sha256":SNAPSHOT_SHA,
  "capstone_version":capstone.__version__,"pefile_version":pefile.__version__,"sources":original,"reference_sources":refs,
  "anchors":anchors,"guids":guids,"tables":tables,"edges":EDGES,"spans":spans,"limited_negative_searches":negatives,
  "conclusion":"No complete HandoffGrant producer established. Static event/SCM/hibernate/domain paths are not runtime drain, queue-stop, exclusive ownership or SMMU sync proof.",
  "limitations":["Not whole-program reachability analysis; embedded .text data excluded only by manually inspected spans.","Protocol dispatch depends on actual provider and successful registration.","Stock XBL events/protocols do not survive by assumption into current independent BootShim/SEC/DXE environment.","Secure-world and IOMMU provider descendants remain untraced; no guessed SMC was executed."]}
 emit("control-flow.json",encoded(result),check)
 print(json.dumps({"ok":True,"mode":"check" if check else "analyze","anchors":len(anchors),"guid_assertions":len(guids),"spans":len(spans),"call_paths":len(EDGES),"input_hashes_unchanged":True},ensure_ascii=False))

def main():
 p=argparse.ArgumentParser(description=__doc__);g=p.add_mutually_exclusive_group(required=True)
 g.add_argument("--extract",action="store_true");g.add_argument("--analyze",action="store_true");g.add_argument("--check",action="store_true");args=p.parse_args()
 if args.extract:
  q=STUDY/"extractor-v1.py"
  if sha(q.read_bytes())!=SNAPSHOT_SHA:raise ValueError("Extractor snapshot mismatch")
  runpy.run_path(str(q),run_name="__main__")
 else:analyze(args.check)
if __name__=="__main__":main()
