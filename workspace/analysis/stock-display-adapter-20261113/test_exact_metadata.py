#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import struct
import sys
import unittest
sys.dont_write_bytecode=True
spec=importlib.util.spec_from_file_location('metadata_audit',Path(__file__).with_name('audit_exact_metadata.py'))
a=importlib.util.module_from_spec(spec);spec.loader.exec_module(a)

class PatchView:
    def __init__(self,image,patch): self.image,self.patch=image,patch
    def read(self,pc,size):
        result=bytearray(self.image.read(pc,size))
        for at,data in self.patch.items():
            left=max(pc,at);right=min(pc+size,at+len(data))
            if right>left: result[left-pc:right-pc]=data[left-at:right-at]
        return bytes(result)

class TestMetadata(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.helper=a.load_helper()
        cls.kernel=(a.ROOT/'artifacts/j716f-host-dt-panic-20261113/kernel.macho').read_bytes()
        cls.original=(a.ROOT/'artifacts/j716f-ipados-inputs-20261112/21H461/kernelcache.decompressed').read_bytes()
        cls.metadata=(a.ROOT/'artifacts/j716f-host-dt-panic-20261113/kernel.json').read_bytes()
        cls.image=cls.helper.MachO(cls.kernel)
    def changed(self,pc,value):return PatchView(self.image,{pc:struct.pack('<I',value)})
    def test_full_exact_evidence(self):
        r=a.audit(self.kernel,self.metadata,self.original,self.helper)
        self.assertEqual([x['instance_size'] for x in r['metaclasses']],[144,144,6304])
        self.assertTrue(r['metaclass_name_parent_size_chain_verified'])
        self.assertFalse(r['complete_display_abi']);self.assertFalse(r['swap_retirement_qualified'])
    def test_mutated_kernel_refused(self):
        b=bytearray(self.kernel);b[-1]^=1
        with self.assertRaisesRegex(ValueError,'kernel SHA'):a.audit(b,self.metadata,self.original,self.helper)
    def test_mutated_original_refused(self):
        b=bytearray(self.original);b[-1]^=1
        with self.assertRaisesRegex(ValueError,'original kernelcache SHA'):a.audit(self.kernel,self.metadata,b,self.helper)
    def test_adrp_wrong_register_refused(self):
        pc=0xfffffff005786558
        with self.assertRaises(ValueError):a.page(self.changed(pc,a.word(self.image,pc)^1),pc,0)
    def test_adrp_wrong_opcode_refused(self):
        pc=0xfffffff005786558
        with self.assertRaises(ValueError):a.page(self.changed(pc,0xd503201f),pc,0)
    def test_add_wrong_register_refused(self):
        pc=0xfffffff005786558
        with self.assertRaises(ValueError):a.address(self.changed(pc+4,a.word(self.image,pc+4)^1),pc,0)
    def test_add_shifted_immediate_refused(self):
        pc=0xfffffff005786558
        with self.assertRaises(ValueError):a.address(self.changed(pc+4,a.word(self.image,pc+4)|(1<<22)),pc,0)
    def test_size_wrong_register_refused(self):
        pc=0xfffffff0057866b4
        with self.assertRaises(ValueError):a.size_argument(self.changed(pc,a.word(self.image,pc)^1),pc)
    def test_size_wrong_width_refused(self):
        pc=0xfffffff0057866b4
        with self.assertRaises(ValueError):a.size_argument(self.changed(pc,a.word(self.image,pc)|(1<<31)),pc)
    def test_size_wrong_shift_refused(self):
        pc=0xfffffff0057866b4
        with self.assertRaises(ValueError):a.size_argument(self.changed(pc,a.word(self.image,pc)|(1<<21)),pc)
    def test_import_load_wrong_register_refused(self):
        pc=0xfffffff0057a2818
        with self.assertRaises(ValueError):a.indirect(self.changed(pc+4,a.word(self.image,pc+4)^1),pc,16)
    def test_import_load_wrong_width_refused(self):
        pc=0xfffffff0057a2818
        with self.assertRaises(ValueError):a.indirect(self.changed(pc+4,a.word(self.image,pc+4)^(1<<30)),pc,16)

if __name__=='__main__':unittest.main(verbosity=2)
