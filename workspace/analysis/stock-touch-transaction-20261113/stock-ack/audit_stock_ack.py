"""Bounded read-only Goodix IRQ/ACK audit of the saved stock boot image.

No ROM code, device tools, VM command, or transport is executed. --write creates
only this audit's JSON/text files; --check compares them without writing.
"""
from pathlib import Path
import argparse
import hashlib
import json
import struct
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

OUT = Path(__file__).resolve().parent
ROOT = OUT.parents[2]
BOOT = ROOT / 'TB_J716F_ZUI_15.0.485/image/boot.img'
BOOT_SHA = '84cba57ff27e34669fa34d0b78d776ad621960898cdfbca3c70bcd4645043105'
KERNEL_SHA = 'a60d6de51822a18358ad005991bb11c08b34e0e66f28589cdab738902eb7d46c'
CONFIG_SHA = '96b8e74d1bbeb2098eb26e238998a8a327d076ecc4ef70b4bd47b033257941ca'
LINK_BASE = 0xffffff8008080000
WINDOWS = [
    ('ops_bind', 0xa74578, 0xa74584, 'edb633d68317d31abca87096c40a8450037d4995308d6bb3ed3b9d2942bbbcd1'),
    ('config_assign', 0xa7c808, 0xa7c9a0, '6331683470abd192baeeb67a817c54bfe45459f536cdb04ce81785211058d74d'),
    ('irq_setup', 0xa76bc0, 0xa76c98, '4a055235a45e7569110eecb18e84253686a73ce045269f3d0ecba2307a6b98e3'),
    ('irq_thread', 0xa76c98, 0xa77410, '90b6b4e80425e71b7293f60382cd5abd053be81327f0d2277001087d4d17ef43'),
    ('event', 0xa75370, 0xa758a8, '35310fc86d23e91fe29f4221d0ad113351fca5ebcc805d77c6e97004312d44cd'),
    ('read_nodelay', 0xa73728, 0xa73950, '70807c2a14491484d46eebcc9794969584c2b1ebe20f10aa6ed1ae8f32458f75'),
    ('write_nodelay', 0xa73950, 0xa73ae0, 'd9836c784fdcfe8149779d25b44ae0388e65b9a87e1c0e3f947cfc1764a3be62'),
    ('relocations', 0x2104bb0, 0x2104c50, '668c5ffb76386cec144f20c05c2a4ebb9ec3753dc8173b7e4fae8390df9f9aa0'),
]
# These expected instructions are assertions, not a live discovery algorithm.
FACTS = {
    'bind_ops_page': (0xa74578, 'adrp', 'x8, #0x198b000'),
    'bind_ops_base': (0xa7457c, 'add', 'x8, x8, #0xc90'),
    'bind_ops_device_field': (0xa74580, 'str', 'x8, [x20, #0x990]'),
    'config_package_arg': (0xa7c81c, 'mov', 'x20, x1'),
    'config_device_arg': (0xa7c820, 'mov', 'x19, x0'),
    'config_coordinate_source': (0xa7c970, 'ldrh', 'w8, [x20, #0x60]'),
    'config_coordinate_destination': (0xa7c974, 'strh', 'w8, [x19, #0x34]'),
    'irq_setup_thread_page': (0xa76c2c, 'adrp', 'x3, #0xa76000'),
    'irq_setup_thread_address': (0xa76c3c, 'add', 'x3, x3, #0xc98'),
    'irq_setup_null_primary': (0xa76c44, 'mov', 'x2, xzr'),
    'irq_setup_flags': (0xa76c50, 'orr', 'x4, x9, #0x2000'),
    'irq_setup_call': (0xa76c54, 'bl', '#0xd5160'),
    'extension_call': (0xa76d24, 'blr', 'x9'),
    'extension_cancel_value': (0xa76d28, 'cmp', 'w0, #1'),
    'extension_continue': (0xa76d2c, 'b.ne', '#0xa76d00'),
    'extension_skip_event_and_ack': (0xa76d38, 'b', '#0xa773d0'),
    'event_ops_field': (0xa76d48, 'ldr', 'x8, [x19, #0x990]'),
    'event_ops_slot': (0xa76d54, 'ldr', 'x8, [x8, #0x58]'),
    'event_call': (0xa76d58, 'blr', 'x8'),
    'negative_event_still_acks': (0xa76d5c, 'tbnz', 'w0, #0x1f, #0xa773b0'),
    'finger_event_gate': (0xa76d64, 'tbz', 'w8, #0, #0xa76d74'),
    'finger_report_before_ack': (0xa76d70, 'bl', '#0xa789f0'),
    'pen_disabled_still_acks': (0xa76d78, 'cbz', 'w8, #0xa773b0'),
    'no_pen_event_still_acks': (0xa76d80, 'tbz', 'w8, #1, #0xa773b0'),
    'ack_buffer': (0xa773b4, 'add', 'x2, sp, #8'),
    'ack_register_field': (0xa773b8, 'ldrh', 'w1, [x19, #0x34]'),
    'ack_length_two': (0xa773c0, 'mov', 'w3, #2'),
    'ack_zero_two_bytes': (0xa773c4, 'strh', 'wzr, [sp, #8]'),
    'ack_ops_slot': (0xa773c8, 'ldr', 'x8, [x8, #0x30]'),
    'ack_call': (0xa773cc, 'blr', 'x8'),
    'irq_ignores_ack_return': (0xa773e8, 'mov', 'w0, #1'),
    'prefix_length': (0xa75398, 'mov', 'w3, #0x10'),
    'prefix_register': (0xa753a8, 'ldrh', 'w8, [x0, #0x34]'),
    'prefix_read_call': (0xa753b0, 'bl', '#0xa73728'),
    'prefix_result_retained': (0xa753b4, 'mov', 'w20, w0'),
    'prefix_fail_exit': (0xa753b8, 'tbnz', 'w0, #0x1f, #0xa7577c'),
    'status_sign_byte': (0xa753bc, 'ldrsb', 'w8, [sp, #8]'),
    'status_touch_bit7': (0xa753c0, 'tbz', 'w8, #0x1f, #0xa757b0'),
    'clear_event_size': (0xa753c4, 'mov', 'w2, #0x118'),
    'clear_event_only_on_touch_path': (0xa753d0, 'bl', '#0x1415cc0'),
    'pen_flag': (0xa753f8, 'tst', 'w26, #0x20'),
    'count_nibble': (0xa753fc, 'and', 'w23, w26, #0xf'),
    'count_limit': (0xa75404, 'cmp', 'w23, #0xa'),
    'bad_count_exit': (0xa75408, 'b.hi', '#0xa7577c'),
    'more_than_one_needs_tail': (0xa75410, 'b.hs', '#0xa757c4'),
    'zero_skips_checksum': (0xa75414, 'cbz', 'w23, #0xa75448'),
    'bad_checksum_branch': (0xa75444, 'b.ne', '#0xa757f4'),
    'zero_release_prior_fingers': (0xa755dc, 'bl', '#0xa762a8'),
    'event_returns_initial_or_request_result': (0xa75790, 'mov', 'w0, w20'),
    'gesture_bit5_exit': (0xa757b0, 'tbnz', 'w8, #5, #0xa7577c'),
    'non_touch_second_byte': (0xa757b4, 'ldrb', 'w8, [sp, #9]'),
    'zero_second_byte_requests': (0xa757b8, 'cbz', 'w8, #0xa75818'),
    'unknown_negative_return': (0xa757bc, 'mov', 'w20, #-0x16'),
    'tail_length_formula': (0xa757cc, 'sub', 'w3, w24, #0x18'),
    'tail_buffer_offset': (0xa757d0, 'add', 'x2, x22, #0x10'),
    'tail_register_offset': (0xa757d8, 'add', 'w1, w9, #0x10'),
    'tail_read_call': (0xa757dc, 'bl', '#0xa73728'),
    'tail_failure_ignores_new_result': (0xa757e0, 'tbnz', 'w0, #0x1f, #0xa7577c'),
    'checksum_failure_exits_without_result_assignment': (0xa75814, 'b', '#0xa7577c'),
    'request_read_register_field': (0xa75818, 'ldrh', 'w1, [x19, #0x38]'),
    'request_read_length': (0xa75824, 'mov', 'w3, #1'),
    'request_read_call': (0xa75828, 'bl', '#0xa73348'),
    'request_write_zero': (0xa7584c, 'strb', 'wzr, [sp, #4]'),
    'request_write_call': (0xa75850, 'bl', '#0xa73588'),
    'request_result_assigned': (0xa75854, 'mov', 'w20, w0'),
    'request_config_call': (0xa7587c, 'bl', '#0xa74778'),
    'read_register_big_endian_high': (0xa737c4, 'lsr', 'w9, w28, #8'),
    'read_register_big_endian_low': (0xa737e4, 'strb', 'w28, [x9, #1]'),
    'read_attempt_one': (0xa737f0, 'bl', '#0xa93b58'),
    'read_success_requires_two': (0xa737f4, 'cmp', 'w0, #2'),
    'read_attempt_two': (0xa73844, 'bl', '#0xa93b58'),
    'read_attempt_three': (0xa73874, 'bl', '#0xa93b58'),
    'read_failure_return': (0xa738c4, 'mov', 'w20, #-0x3e8'),
    'write_register_big_endian_high': (0xa739c0, 'lsr', 'w8, w21, #8'),
    'write_register_big_endian_low': (0xa739d0, 'strb', 'w21, [x8, #1]'),
    'write_payload_offset': (0xa739ec, 'add', 'x0, x9, #2'),
    'write_attempt_one': (0xa73a04, 'bl', '#0xa93b58'),
    'write_success_requires_one': (0xa73a08, 'cmp', 'w0, #1'),
    'write_attempt_two': (0xa73a34, 'bl', '#0xa93b58'),
    'write_attempt_three': (0xa73a4c, 'bl', '#0xa93b58'),
    'write_failure_default': (0xa739bc, 'mov', 'w22, #-0x3e8'),
}


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def json_bytes(value):
    return (json.dumps(value, indent=2, ensure_ascii=False) + '\n').encode('utf-8')


def file_record(path):
    data = path.read_bytes()
    return {'path': path.relative_to(ROOT).as_posix(), 'bytes': len(data), 'sha256': sha(data)}


def collect():
    raw = BOOT.read_bytes()
    require(len(raw) == 100663296 and sha(raw) == BOOT_SHA, 'original boot identity')
    require(raw[:8] == b'ANDROID!' and struct.unpack_from('<I', raw, 8)[0] == 39528464, 'boot header')
    kernel = raw[4096:4096 + 39528464]
    require(sha(kernel) == KERNEL_SHA, 'stock kernel identity')
    cfgpath = ROOT / 'analysis/stock-goodix-20261113/stock/goodix_cfg_group.bin'
    cfg = cfgpath.read_bytes()
    require(sha(cfg) == CONFIG_SHA and len(cfg) == 583, 'stock config identity')
    require(struct.unpack_from('<I', cfg)[0] == len(cfg) and sum(cfg[5:]) & 255 == cfg[4], 'config length/checksum')
    package = struct.unpack_from('<H', cfg, 16)[0]
    require(package == 18 and cfg[package + 19] == 1 and cfg[package + 20] == 8, 'package identity')
    require(struct.unpack_from('<H', cfg, package + 0x60)[0] == 0x824d, 'coordinate register')
    require(struct.unpack_from('<H', cfg, package + 0x68)[0] == 0x8044, 'request register')
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    facts = {}
    for label, (address, op, args) in FACTS.items():
        instructions = list(cs.disasm(kernel[address:address + 4], address))
        require(len(instructions) == 1, 'instruction decode ' + label)
        ins = instructions[0]
        require((ins.mnemonic, ins.op_str) == (op, args), 'instruction mismatch ' + label)
        facts[label] = {'image_offset': hex(address), 'bytes': ins.bytes.hex(), 'op': op, 'args': args}
    windows, text = [], []
    for name, start, end, expected in WINDOWS:
        data = kernel[start:end]
        require(sha(data) == expected, 'window mismatch ' + name)
        windows.append({'name': name, 'start': hex(start), 'end_exclusive': hex(end), 'bytes': len(data), 'sha256': expected})
        text.append(f'[{name}] Image+{start:#x}..{end:#x} (end exclusive), sha256={expected}\n')
        if name != 'relocations':
            rows = list(cs.disasm(data, start))
            require(sum(i.size for i in rows) == len(data), 'incomplete window decode ' + name)
            text.extend(f'{i.address:08x}  {i.bytes.hex()}  {i.mnemonic} {i.op_str}\n' for i in rows)
        else:
            text.append(data.hex() + '\n')
    relocs = []
    for entry, destination, target in [(0x2104bb8, 0x198bcb8, 0xa73728), (0x2104bd0, 0x198bcc0, 0xa73950), (0x2104c48, 0x198bce8, 0xa75370)]:
        # entry points to the addend in an ELF64 Rela-compatible triple.
        offset, info, addend = struct.unpack_from('<QQQ', kernel, entry - 16)
        require((offset, info, addend) == (LINK_BASE + destination, 0x403, LINK_BASE + target), 'ops relocation mismatch')
        require(kernel[destination:destination + 8] == bytes(8), 'expected initially zero relocated slot')
        relocs.append({'record_image_offset': hex(entry - 16), 'r_offset': hex(offset), 'r_info': hex(info), 'r_addend': hex(addend), 'table_base_image': '0x198bc90', 'table_slot': hex(destination - 0x198bc90), 'target_image': hex(target), 'raw': kernel[entry - 16:entry + 8].hex()})
    strings = {}
    for address, expected in [(0x1d71bc4, b'goodix_i2c_read_nodelay'), (0x1d72eb4, b'goodix_ts_irq_setup'), (0x1d72b8c, b'goodix_request_handler'), (0x1d7567a, b'goodix_extract_cfg_pkg')]:
        require(kernel[address:address + len(expected) + 1] == expected + b'\0', 'name string mismatch')
        strings[hex(address)] = expected.decode('ascii')
    result = {
        'schema': 1, 'source_rom': 'TB_J716F_ZUI_15.0.485',
        'scope': {'read_only_rom': True, 'device_access': False, 'vm_access': False, 'firmware_executed': False, 'hardware_qualified': False},
        'boot': file_record(BOOT), 'kernel': {'offset': 4096, 'bytes': len(kernel), 'sha256': KERNEL_SHA},
        'config': file_record(cfgpath), 'address_basis': 'Image-relative byte offsets; LINK_BASE is a static relocation-address normalization, not a claimed current runtime address.',
        'link_base_for_relocations': hex(LINK_BASE), 'instruction_facts': facts, 'windows': windows, 'ops_relocations': relocs, 'name_strings': strings,
        'ack': {'coordinate_register': '0x824d', 'payload_hex': '0000', 'payload_bytes': 2, 'i2c_write_message_hex': '824d0000', 'irq_thread_entry_image': '0xa76c98', 'write_nodelay_entry_image': '0xa73950', 'caller_observes_write_result': False, 'input_report_precedes_ack': True},
        'references': [file_record(ROOT / 'analysis/driver-adaptation-20261113/input-sources/google-gt738x' / name) for name in ('goodix_ts_core.c', 'goodix_ts_core.h', 'goodix_ts_i2c.c', 'goodix_cfg_bin.c')],
        'limitations': [
            'Related-source names aid identification; asserted instructions and relocation records bind selected behavior to original ROM.',
            'A returned transport operation is synchronous at this Linux call boundary. Physical bus timeout/cancel/drain, controller barriers and IRQ deassertion are not established.',
            'Three calls per Goodix helper chunk are not a bound on adapter retries or physical bus attempts.',
            'Disarmed and unsupported are new-driver states; no corresponding stock states were proved. Stock parses pen rather than rejecting it.',
            'The non-touch request path may send configuration; this audit does not authorize implementing or executing that path.',
            'Successful event return is not valid-packet proof: failed tail read, bad count and bad checksum preserve successful prefix return.',
            'Original IRQ thread ignores ACK result and reports inputs before ACK. New transaction policy may deliberately differ.',
        ],
    }
    require(sha(BOOT.read_bytes()) == BOOT_SHA, 'boot changed during audit')
    return {'evidence.json': json_bytes(result), 'disassembly.txt': ''.join(text).encode('utf-8')}


def main():
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--write', action='store_true')
    mode.add_argument('--check', action='store_true')
    args = parser.parse_args()
    outputs = collect()
    if args.write:
        for name, data in outputs.items():
            (OUT / name).write_bytes(data)
    else:
        for name, data in outputs.items():
            require((OUT / name).read_bytes() == data, 'saved output mismatch ' + name)
    summary = {'status': 'passed', 'instruction_facts': len(FACTS), 'windows': len(WINDOWS), 'ops_relocations': 3, 'original_boot_unchanged': True, 'hardware_qualified': False, 'outputs': [file_record(OUT / name) for name in outputs]}
    if args.write:
        (OUT / 'verification.json').write_bytes(json_bytes(summary))
    else:
        require((OUT / 'verification.json').read_bytes() == json_bytes(summary), 'saved verification mismatch')
    print(json.dumps(summary, ensure_ascii=False))


if __name__ == '__main__':
    main()
