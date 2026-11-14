#!/usr/bin/env python3
"""Freeze the independently inspected r31 executable and Power-read scope."""
from pathlib import Path
import hashlib
import json
import struct
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import verify_storage_kernel as v

raw = (HERE / 'kernel.macho').read_bytes()
native = (HERE / 'native.macho').read_bytes()
v.require(v.sha(raw) == '6c49b9fac897be9ac4504af54a652402bb2d9adf497ba7b86186b1e965efccb5', 'Wrong reviewed kernel')
v.require(v.sha(native) == 'ef300f399642c4025ec08600b2af5f1c05c40ee6a151af8fc659787f2c36c43c', 'Wrong reviewed native')
image, layout = v._discover(raw)
nm = v.MachO(native)
machine = json.loads((HERE / 'machine-review.json').read_text())
verification = json.loads((HERE / 'verification.json').read_text())
v.require(machine['status'] == 'unreviewed-power-machine-evidence' and verification['status'] == 'passed', 'Missing actual build verification')
functions = machine['functions']
symbols = machine['power_symbols']


def identity(path):
    data = path.read_bytes()
    return {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}


def instructions(key):
    return functions[symbols.get(key, key)]['instructions']


def instruction(va, mnemonic, operands):
    hits = [item for function in functions.values() for item in function['instructions'] if item['va'] == hex(va)]
    v.require(len(hits) == 1 and hits[0]['mnemonic'] == mnemonic and hits[0]['operands'] == operands,
              'Reviewed instruction changed: ' + hex(va))


def cstring(va):
    result = bytearray()
    while len(result) < 1024:
        byte = image.body(va + len(result), 1)
        if byte == b'\0':
            return result.decode()
        result += byte
    raise ValueError('Unterminated reviewed string')


# These are the bytes and addresses just inspected in this new executable, not
# addresses borrowed from r30. All functions were independently compared with
# the un-prelinked native executable by review_machine.py.
offset_va = image.names['__ZN11j716f_power12_GLOBAL__N_17offsetsE']
offsets = struct.unpack('<11I', image.body(offset_va, 44))
v.require(offsets == (4, 0, 0x10, 0x18, 0x64, 0x9c, 0x20, 0xb8, 0x1c, 0x5c, 0xcc), 'Wrong GCC register table')
names_va = image.names['__ZN11j716f_power12_GLOBAL__N_15namesE']
name_pointers = struct.unpack('<11Q', image.body(names_va, 88))
names = [cstring(va) for va in name_pointers]
v.require(names == ['GDSCR', 'BCR', 'AXI', 'AHB', 'ICE', 'AUX', 'RX0', 'RX1', 'TX0', 'UNIPRO', 'AGGRE_AXI'], 'Wrong GCC register names')
v.require(image.body(offset_va, 44) == nm.body(offset_va, 44) and image.body(names_va, 88) == nm.body(names_va, 88), 'Tables changed during prelink')

# address(index) rejects >10, uses the reviewed table and adds only 0x177000.
instruction(0xfffffff0079e2998, 'cmp', 'w0, #0xa')
instruction(0xfffffff0079e299c, 'b.hi', '#0xfffffff0079e29b4')
instruction(0xfffffff0079e29a8, 'ldr', 'w8, [x8, w0, uxtw #2]')
instruction(0xfffffff0079e29ac, 'add', 'x0, x8, #0x177, lsl #12')

# The wrapper binds the five callbacks in the declared Backend field order.
for va, register, key in ((0xfffffff0079e2e4c, 'x9', 'actualContext'),
                          (0xfffffff0079e2e58, 'x8', 'mapPage'),
                          (0xfffffff0079e2e60, 'x9', 'readRegister'),
                          (0xfffffff0079e2e6c, 'x8', 'unmapPage'),
                          (0xfffffff0079e2e74, 'x9', 'logEvent')):
    instruction(va, 'adr', register + ', #' + hex(image.names[symbols[key]]))
instruction(0xfffffff0079e2ecc, 'bl', '#' + hex(image.names[symbols['3run']]))
for va, name in ((0xfffffff0079e2f00, '_current_thread'), (0xfffffff0079e2f08, '_ml_at_interrupt_context'),
                 (0xfffffff0079e2f10, '_ml_get_interrupts_enabled'), (0xfffffff0079e2f1c, '_get_preemption_level'),
                 (0xfffffff0079e2f74, '_j716f_storage_bringup_status'), (0xfffffff0079e2fe4, '_sm8250_ufs_status')):
    instruction(va, 'bl', '#' + hex(image.names[name]))
v.require(image.names['_kernel_task'] == 0xfffffff00716a818, 'Wrong kernel_task reference')
instruction(0xfffffff0079e2f34, 'ldr', 'x25, [x8, #0x818]')

# withRange gets one exact GCC page, and map gets Anywhere|ReadOnly|InhibitCache.
instruction(0xfffffff0079e307c, 'mov', 'w0, #0x7000')
instruction(0xfffffff0079e3080, 'movk', 'w0, #0x17, lsl #16')
instruction(0xfffffff0079e3084, 'mov', 'w1, #0x1000')
instruction(0xfffffff0079e3088, 'bl', '#' + hex(image.names['__ZN14IODeviceMemory9withRangeEyy']))
instruction(0xfffffff0079e30ac, 'mov', 'w1, #0x1101')
instruction(0xfffffff0079e30b0, 'blr', 'x8')
instruction(0xfffffff0079e3180, 'and', 'w8, w22, #0x1f00')
instruction(0xfffffff0079e3184, 'mov', 'w9, #0x1100')
instruction(0xfffffff0079e3190, 'cmp', 'x0, #1, lsl #12')

# readRegister compares its PA against all eleven address() results. The only
# MMIO access follows those comparisons. x19 is the caller's ordinary RAM output.
read_items = instructions('readRegister')
v.require(sum(item['mnemonic'] == 'bl' and item['operands'] == '#' + hex(image.names[symbols['7address']]) for item in read_items) == 11, 'Incomplete whitelist')
for va, mnemonic, operands in ((0xfffffff0079e32d0, 'ldr', 'x8, [x21, #8]'),
                               (0xfffffff0079e32d4, 'add', 'x8, x20, x8'),
                               (0xfffffff0079e32d8, 'sub', 'x8, x8, #0x177, lsl #12'),
                               (0xfffffff0079e32dc, 'dsb', 'sy'),
                               (0xfffffff0079e32e0, 'ldr', 'w8, [x8]'),
                               (0xfffffff0079e32e4, 'dsb', 'sy'),
                               (0xfffffff0079e32e8, 'str', 'w8, [x19]')):
    instruction(va, mnemonic, operands)
v.require(sum(item['mnemonic'] == 'ldr' and item['operands'] == 'w8, [x8]' for item in read_items) == 1, 'Wrong MMIO load count')
v.require(not any(item['mnemonic'].startswith('st') and '[x8' in item['operands'] for item in read_items), 'Store to computed MMIO address')

# State.attempted is set before context/map and never cleared; lock release is
# separate. Read-before callback, one read callback, read-after callback, then
# the eleven-iteration loop. Ordinary callback failure still unmaps once.
instruction(0xfffffff0079e2b2c, 'strb', 'w8, [x0, #4]')
instruction(0xfffffff0079e2c44, 'mov', 'w1, #4')
instruction(0xfffffff0079e2c4c, 'blr', 'x8')
instruction(0xfffffff0079e2c68, 'blr', 'x8')
instruction(0xfffffff0079e2ca8, 'mov', 'w1, #5')
instruction(0xfffffff0079e2cb0, 'blr', 'x9')
instruction(0xfffffff0079e2cb8, 'cmp', 'x21, #0xb')
instruction(0xfffffff0079e2cbc, 'b.ne', '#0xfffffff0079e2c2c')
instruction(0xfffffff0079e2d98, 'blr', 'x8')

power_function_names = [v.POWER_NAME, *symbols.values()]
for name in power_function_names:
    body = functions[name]
    v.require(body['native_identical'], 'Uncompared Power code')
    for forbidden in v.OWNER_NAMES + v.BRINGUP_NAMES[:1] + v.BRINGUP_NAMES[2:] + tuple(n for n in v.RUNTIME_NAMES if n != '_sm8250_ufs_status'):
        v.require(not v._calls(image, body, forbidden), 'Power path calls old authority/action API: ' + forbidden)
markers = [b'fault-contained=0 retry=0\n\0', b'bus-readability=unproven\n\0',
           b'snapshot-only storage-stage-unchanged hci=0 smmu=0 dma=0 writes=0 next=review-power-snapshot\n\0']
for marker in markers:
    v.require(raw.count(marker) == 1, 'Required scope log missing')

power = {
    'status': 'independently-reviewed-power-machine-code',
    'original_kernel_sha256': v.sha(raw), 'native_sha256': v.sha(native),
    'review': {name: True for name in machine['power_scope_manual_review']},
    'gcc_page': '0x177000', 'page_bytes': 4096, 'map_options': '0x1101',
    'offset_table_va': hex(offset_va), 'offsets': list(offsets), 'register_names': names,
    'register_physical_addresses': [hex(0x177000 + offset) for offset in offsets],
    'mmio_read_instruction': '0xfffffff0079e32e0: ldr w8, [x8]',
    'mmio_barriers': ['0xfffffff0079e32dc: dsb sy', '0xfffffff0079e32e4: dsb sy'],
    'callback_bindings': symbols, 'function_ranges_and_hashes': {name: {k: value for k, value in functions[name].items() if k != 'instructions'} for name in power_function_names},
    'context_gates': ['thread present', 'not interrupt context', 'interrupts enabled', 'preemption level zero', 'kernel_task present', 'bridge registered', 'runtime Cold'],
    'single_attempt_per_boot': True, 'ordinary_cleanup_reviewed': True,
    'hardware_mmio_succeeded': False, 'external_abort_contained': False,
    'no_fault_recovery_claim': 'The map can succeed while a read stalls or aborts. This experimental path deliberately has no external-abort handler.',
    'old_authority_changed_by_power': False, 'dma_or_block_device_published': False,
    'observed_hardware_result': None,
}
power_path = HERE / 'power-machine-review.json'
with power_path.open('x', encoding='utf-8', newline='\n') as stream:
    json.dump(power, stream, indent=2); stream.write('\n')

contract = v.discover_layout(raw)
contract['status'] = 'independently-frozen'
contract['review'] = {key: True for key in v.REVIEW_KEYS}
evidence_paths = [
    ROOT / 'work/UniXNU-j716f-host-ufs-power-r31/docs/host-source-manifest.json',
    ROOT / 'work/UniXNU-j716f-host-ufs-power-r31/docs/storage-inputs.json',
    ROOT / 'work/J716fStorageBringup-r31/source-manifest.json',
    HERE.parent / 'kernel-target/physical-input-review.json',
    HERE.parent / 'kernel-target/get-preemption-level-abi-review.json',
    HERE / 'build-result.json', HERE / 'verification.json', HERE / 'storage-archive-result.json',
    HERE / 'machine-review.json', power_path, HERE / 'power-disassembly.txt',
    HERE.parent / 'verify_storage_kernel.py', HERE / 'review_machine.py', Path(__file__),
]
contract['review_evidence'] = {path.relative_to(ROOT).as_posix(): identity(path) for path in evidence_paths}
v.validate_expectation(contract)
with (HERE.parent / 'kernel-expectation.json').open('x', encoding='utf-8', newline='\n') as stream:
    json.dump(contract, stream, indent=2); stream.write('\n')
candidate = v.derive_expected(raw, contract)
with (HERE / 'kernel.probe.macho').open('xb') as stream:
    stream.write(candidate)
print(json.dumps({'status': contract['status'], 'original': contract['original'], 'expected': contract['expected'],
                  'power_review': identity(power_path), 'hardware_tested': False}, indent=2))
