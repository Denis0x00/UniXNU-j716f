#!/usr/bin/env python3
"""Audit the linked logger, not source strings or a standalone host stub."""
import argparse
import json
from pathlib import Path
import re
import shutil
import struct
import subprocess

from native_candidate import MachO, check_kernel, sha

ROOTS = ('_j716f_native_begin', '_j716f_native_bootstrap', '_j716f_native_early_fault')
REQUIRED = set(ROOTS) | {
    '_j716f_native_event', '_j716f_native_irq', '_j716f_native_sched',
    '_j716f_native_window', '_j716f_native_early_vectors',
}
HELPERS = {'_jl_barrier', '_jl_counter', '_jl_registers', '_jl_physical_event',
           '_jl_boot_resolve', '_jl_boot_allocate', '_jl_map_window',
           '_jl_find', '_jl_crc', '_jl_valid', '_jl_write', '_jl_record'}


def disassemble(path):
    tool = shutil.which('llvm-objdump')
    if not tool:
        for prefix in ('/opt/homebrew/opt/llvm/bin', '/usr/local/opt/llvm/bin'):
            if (Path(prefix)/'llvm-objdump').is_file():
                tool = str(Path(prefix)/'llvm-objdump'); break
    if not tool:
        # Local Windows receipt checks can use Capstone. CI still uses LLVM.
        # Walk executable sections independently; skip embedded data without
        # letting it truncate the remainder. Reached undecodable bytes fail.
        try:
            from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        except ImportError as exc:
            raise ValueError('Linked pre-MMU audit requires llvm-objdump or capstone') from exc
        image = MachO(Path(path).read_bytes())
        decoder = Cs(CS_ARCH_ARM64, CS_MODE_ARM); decoder.skipdata = True
        instructions = {}
        for _,va,_,off,size,prot in image.segments:
            if not prot & 4: continue
            for ins in decoder.disasm(image.data[off:off+size],va):
                if ins.mnemonic != '.byte':
                    instructions[ins.address] = (ins.mnemonic.lower(),ins.op_str)
        if not instructions: raise ValueError('No decoded AArch64 instructions')
        return instructions
    result = subprocess.run([tool, '--disassemble', '--no-show-raw-insn', str(path)],
                            capture_output=True, text=True, check=True)
    instructions = {}
    for line in result.stdout.splitlines():
        match = re.match(r'^\s*([0-9a-fA-F]+):\s+([a-zA-Z][a-zA-Z0-9.]*)\s*(.*)$', line)
        if match:
            instructions[int(match[1], 16)] = (match[2].lower(), match[3])
    if not instructions:
        raise ValueError('No decoded AArch64 instructions')
    return instructions


def audit_early(image, instructions):
    """Walk real control flow, including out-of-line helpers and tail calls.

    No indirect call/jump, external runtime, SIMD/FP or firmware call is allowed
    before MMU. Direct branch destinations come from opcodes, not printed names.
    This is a static gate, not proof of physical memory access on the device.
    """
    definitions = dict(image.symbols)
    for name, addresses in image.local_symbols.items():
        if name in HELPERS and len(addresses) == 1:
            definitions[name] = addresses[0]
    starts = {address: name for name, address in definitions.items()
              if name in HELPERS or name in ROOTS}
    executable = [s for s in image.segments if s[-1] & 4]
    queue = [definitions[name] for name in ROOTS]
    seen, calls = set(), set()

    def relative(word, shift, bits, pc):
        value = (word >> shift) & ((1 << bits)-1)
        if value & (1 << (bits-1)): value -= 1 << bits
        return pc + value*4

    while queue:
        pc = queue.pop()
        if pc in seen: continue
        if len(seen) >= 20000 or pc & 3 or pc not in instructions:
            raise ValueError(f'Undecodable/oversized early call graph at {pc:#x}')
        if not any(va <= pc < va+filesz for _,va,_,_,filesz,_ in executable):
            raise ValueError('Early graph leaves executable segment')
        mnemonic, operands = instructions[pc]
        if (mnemonic in {'blr','br','blraa','blrab','braa','brab','smc','hvc','svc','eret'} or
                re.search(r'\b[vsdqhb][0-9]+\b', operands.lower())):
            raise ValueError(f'Unsafe pre-MMU instruction at {pc:#x}: {mnemonic} {operands}')
        seen.add(pc)
        word = struct.unpack_from('<I', image.data, image.offset(pc,4))[0]
        if mnemonic in {'ret','retaa','retab'}: continue
        if word & 0x7c000000 == 0x14000000:  # B or BL
            target = relative(word,0,26,pc)
            if word & 0x80000000:
                if target not in starts:
                    raise ValueError(f'Early BL calls unreviewed runtime at {target:#x}')
                calls.add(starts[target]); queue.append(pc+4)
            elif target in image.symbols.values() and target not in starts:
                raise ValueError(f'Early tail branch leaves logger at {target:#x}')
            queue.append(target); continue
        if word & 0xff000010 == 0x54000000 or word & 0x7e000000 == 0x34000000:
            queue.append(relative(word,5,19,pc))  # B.cond / CBZ / CBNZ
        elif word & 0x7e000000 == 0x36000000:
            queue.append(relative(word,5,14,pc))  # TBZ / TBNZ
        queue.append(pc+4)
    return {'reachable_instructions':len(seen),'direct_helper_calls':sorted(calls),
            'indirect_calls':False,'simd_fp':False,'external_runtime_calls':False}


def check(path, candidate):
    data = Path(path).read_bytes(); image = MachO(data)
    base = check_kernel(data)
    if not REQUIRED <= set(image.symbols):
        raise ValueError('Missing linked native logger definitions: '+str(sorted(REQUIRED-set(image.symbols))))
    sections = [s for s in image.sections if s[0]==b'__j716f_log']
    if len(sections)!=1:
        raise ValueError('Logger window section must be unique')
    name, segment, address, size, offset = sections[0]
    if (segment!=b'__DATA' or size!=0x40000 or address%4096 or
            address!=image.symbols['_j716f_native_window'] or
            offset!=image.offset(address,size) or data[offset]!=0x7f or
            any(data[offset+1:offset+size])):
        raise ValueError('Logger window must be dedicated, file-backed, page-aligned permanent DATA')
    # This initialized identity is referenced by the retained writer, not an
    # arbitrary occurrence elsewhere in the binary.
    ids=image.local_symbols.get('_j716f_candidate',[])
    if len(ids)!=1 or data[image.offset(ids[0],32):image.offset(ids[0],32)+32].hex()!=candidate:
        raise ValueError('Linked retained-log identity differs from generated candidate')
    if image.symbols['_j716f_native_early_vectors']%2048:
        raise ValueError('Early exception vectors not aligned')
    if any(s in data for s in (b'sheng-platform-stage', b'sheng-platform-panic', b'platformStageReset')):
        raise ValueError('Old platform reset/panic diagnostics linked')
    return {'kernel_sha256':sha(data),'candidate_sha256':candidate,
            'window_segment':segment.decode(),'window_va':hex(address),'window_bytes':size,
            'symbols':sorted(REQUIRED),'source_crypto_hmac':base['source_crypto_hmac'],
            'pre_mmu':audit_early(image,disassemble(path)),
            'hardware_validated':False,'launch_admitted':False}


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('kernel',type=Path);parser.add_argument('--candidate',required=True)
    args=parser.parse_args();print(json.dumps(check(args.kernel,args.candidate),indent=2))
