"""Bounded, offline GENI audit. No device/VM/code execution or existing edits.

--write writes only this directory's evidence/disassembly/source receipts.
--check re-derives those receipts without changing them.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
import struct
from pathlib import Path
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

OUT = Path(__file__).resolve().parent
ROOT = OUT.parents[1]
BOOT = 'TB_J716F_ZUI_15.0.485/image/boot.img'
KERNEL_SHA = 'a60d6de51822a18358ad005991bb11c08b34e0e66f28589cdab738902eb7d46c'
BOOT_SHA = '84cba57ff27e34669fa34d0b78d776ad621960898cdfbca3c70bcd4645043105'
BASE = 'analysis/firmware/boot-base-00.dtb'
OVERLAY = 'analysis/firmware/dtbo-overlay-01.dtbo'
RUNNING = 'analysis/device/20261111-231708/running.dtb'
PARSER = 'analysis/device/20261111-231708/parse_input_display.py'
WINDOWS = [
    ('probe_frequency', 0xa9a97c, 0xa9aa84),
    ('irq', 0xa9ad30, 0xa9b260),
    ('cancel_abort_waits', 0xa9be40, 0xa9bfe4),
    ('runtime_mode_and_clock', 0xa9c99c, 0xa9cb88),
    ('register_helpers', 0xd8eb60, 0xd8eef0),
    ('packing_helper', 0xd8f008, 0xd8f150),
    ('error_register_snapshot', 0xd90808, 0xd90948),
]
FACTS = {
    'runtime_protocol_helper': (0xa9ca20, 'bl', '#0xd8eb90'),
    'runtime_requires_i2c3': (0xa9ca24, 'cmp', 'w0, #3'),
    'runtime_protocol_reject': (0xa9ca28, 'b.ne', '#0xa9cb5c'),
    'runtime_if_disable_offset': (0xa9ca30, 'add', 'x8, x8, #0x64'),
    'runtime_if_disable_load': (0xa9ca34, 'ldr', 'w8, [x8]'),
    'runtime_if_disable_zero_to_fifo': (0xa9ca38, 'cbz', 'w8, #0xa9ca70'),
    'runtime_gsi_mode2': (0xa9ca44, 'mov', 'w1, #2'),
    'runtime_gsi_select': (0xa9ca4c, 'bl', '#0xd8ec70'),
    'runtime_gsi_string_page': (0xa9ca58, 'adrp', 'x1, #0x1d7b000'),
    'runtime_gsi_string_offset': (0xa9ca5c, 'add', 'x1, x1, #0x5e8'),
    'runtime_tx_depth_helper': (0xa9ca74, 'bl', '#0xd8eec0'),
    'runtime_rx_watermark_depth_minus1': (0xa9ca7c, 'sub', 'w1, w0, #1'),
    'runtime_fifo_init': (0xa9ca94, 'bl', '#0xd8ebc0'),
    'runtime_pack8': (0xa9ca9c, 'mov', 'w1, #8'),
    'runtime_pack4': (0xa9caa0, 'mov', 'w2, #4'),
    'runtime_fifo_string_page': (0xa9cb14, 'adrp', 'x1, #0x1d7b000'),
    'runtime_fifo_string_offset': (0xa9cb1c, 'add', 'x1, x1, #0x5f6'),
    'protocol_offset': (0xd8eb90, 'add', 'x8, x0, #0x68'),
    'protocol_field': (0xd8eb98, 'ubfx', 'w0, w8, #8, #8'),
    'tx_depth_offset': (0xd8eec0, 'add', 'x8, x0, #0xe24'),
    'tx_depth_field': (0xd8eec8, 'ubfx', 'w0, w8, #0x10, #6'),
    'tx_width_field': (0xd8eed8, 'ubfx', 'w0, w8, #0x18, #6'),
    'rx_depth_offset': (0xd8eee0, 'add', 'x8, x0, #0xe28'),
    'rx_depth_field': (0xd8eee8, 'ubfx', 'w0, w8, #0x10, #6'),
    'cancel_irq_enable_offset': (0xd8ee70, 'add', 'x8, x0, #0x614'),
    'cancel_masks_done_irq': (0xd8ee78, 'and', 'w9, w9, #0xfffffffe'),
    'cancel_control_offset': (0xd8ee80, 'add', 'x8, x0, #0x604'),
    'cancel_value4': (0xd8ee84, 'mov', 'w9, #4'),
    'cancel_store': (0xd8ee88, 'str', 'w9, [x8]'),
    'abort_value2': (0xd8eea0, 'mov', 'w8, #2'),
    'abort_control_offset': (0xd8eea4, 'add', 'x9, x0, #0x604'),
    'abort_store': (0xd8eea8, 'str', 'w8, [x9]'),
    'timeout_cancel_call': (0xa9bedc, 'bl', '#0xd8ee58'),
    'timeout_cancel_wait': (0xa9bee8, 'bl', '#0x1430f20'),
    'timeout_cancel_ack_branch': (0xa9beec, 'cbnz', 'w0, #0xa9bf5c'),
    'timeout_abort_call': (0xa9bf20, 'bl', '#0xd8eea0'),
    'timeout_abort_wait': (0xa9bf2c, 'bl', '#0x1430f20'),
    'timeout_abort_ack_branch': (0xa9bf30, 'cbnz', 'w0, #0xa9bf5c'),
    'irq_status_offset': (0xa9ad4c, 'add', 'x8, x8, #0x610'),
    'irq_rx_count_offset': (0xa9ad58, 'add', 'x8, x8, #0x804'),
    'irq_dma_mode_offset': (0xa9ad7c, 'add', 'x8, x8, #0x258'),
    'irq_rx_wc_mask': (0xa9b0c4, 'ands', 'w9, w9, #0x1ffffff'),
    'irq_rx_fifo_offset': (0xa9b0d4, 'add', 'x11, x11, #0x780'),
    'irq_tx_fifo_offset': (0xa9ae90, 'add', 'x12, x12, #0x700'),
    'irq_clear_offset': (0xa9b16c, 'add', 'x8, x8, #0x618'),
    'irq_clear_observed_status': (0xa9b170, 'str', 'w22, [x8]'),
    'dump_status_offset': (0xd90820, 'add', 'x12, x1, #0x40'),
    'dump_ios_offset': (0xd90824, 'add', 'x13, x1, #0x908'),
    'clock_cfg_offset': (0xa9cacc, 'mov', 'w2, #0x48'),
    'clock_div_insert': (0xa9cad8, 'bfi', 'w20, w8, #4, #8'),
    'scl_counters_offset': (0xa9cae8, 'mov', 'w2, #0x278'),
    'scl_high_shift20': (0xa9caf8, 'lsl', 'w8, w8, #0x14'),
    'scl_low_shift10': (0xa9cafc, 'bfi', 'w8, w9, #0xa, #8'),
    'scl_cycle_field': (0xa9cb00, 'orr', 'w0, w8, w10'),
    'clock_default400k_low': (0xa9a9f0, 'mov', 'w2, #0x1a80'),
    'clock_default400k_high': (0xa9a9f4, 'movk', 'w2, #6, lsl #16'),
    'pack_start_index': (0xd8f014, 'sub', 'w8, w1, #1'),
    'pack_direction_bit': (0xd8f05c, 'mov', 'w11, #0x10'),
    'pack_length7_times2': (0xd8f06c, 'mov', 'w13, #0xe'),
    'pack_start_shift5': (0xd8f094, 'bfi', 'w3, w9, #5, #0x1b'),
    'pack_last_stop_bit': (0xd8f0d0, 'orr', 'w9, w9, #1'),
    'pack_join_word0': (0xd8f0e0, 'orr', 'w8, w8, w9, lsl #10'),
    'pack_join_word1': (0xd8f0e4, 'orr', 'w9, w10, w11, lsl #10'),
    'pack_tx0_offset': (0xd8f0ec, 'add', 'x10, x0, #0x260'),
    'pack_tx1_offset': (0xd8f0f0, 'add', 'x11, x0, #0x264'),
    'pack_rx0_offset': (0xd8f0f4, 'add', 'x12, x0, #0x284'),
    'pack_rx1_offset': (0xd8f0f8, 'add', 'x13, x0, #0x288'),
    'pack_tx0_store': (0xd8f0fc, 'str', 'w8, [x10]'),
    'pack_tx1_store': (0xd8f100, 'str', 'w9, [x11]'),
    'pack_rx0_store': (0xd8f104, 'str', 'w8, [x12]'),
    'pack_rx1_store': (0xd8f108, 'str', 'w9, [x13]'),
    'pack_byte_gran_offset': (0xd8f114, 'add', 'x8, x0, #0x254'),
    'pack_byte_gran_value_shift4': (0xd8f118, 'asr', 'w9, w1, #4'),
}


def sha(data): return hashlib.sha256(data).hexdigest()
def ident(path):
    data = (ROOT / path).read_bytes()
    return {'bytes': len(data), 'sha256': sha(data)}
def need(ok, why):
    if not ok: raise ValueError(why)
def canonical(data): return json.dumps(data, indent=2, ensure_ascii=False) + '\n'


def audit():
    blob = (ROOT / BOOT).read_bytes()
    need(sha(blob) == BOOT_SHA, 'boot identity')
    need(blob[:8] == b'ANDROID!' and struct.unpack_from('<I', blob, 8)[0] == 39528464, 'boot header')
    kernel = blob[4096:4096 + 39528464]
    need(sha(kernel) == KERNEL_SHA, 'kernel identity')
    cs = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    instructions = {}
    disassembly = []
    windows = []
    for label, start, end in WINDOWS:
        windows.append({'name': label, 'start': hex(start), 'end_exclusive': hex(end), 'sha256': sha(kernel[start:end])})
        disassembly.append(f'[{label} Image+{start:#x}..{end:#x})')
        for insn in cs.disasm(kernel[start:end], start):
            instructions[insn.address] = insn
            disassembly.append(f'{insn.address:08x} {insn.bytes.hex()} {insn.mnemonic:8} {insn.op_str}')
    facts = {}
    for name, (address, op, args) in FACTS.items():
        insn = instructions[address]
        need((insn.mnemonic, insn.op_str) == (op, args), 'instruction ' + name)
        facts[name] = {'image_offset': hex(address), 'bytes': insn.bytes.hex(), 'op': op, 'args': args}
    markers = {0x1d7b5e8: b'i2c GSI mode\n\0', 0x1d7b5f6: b'i2c fifo/se-dma mode. fifo depth:%d\n\0',
               0x1d7b1a2: b'geni_i2c_xfer\0', 0x1d7b627: b'geni_i2c_runtime_resume\0'}
    for address, value in markers.items():
        need(kernel[address:address+len(value)] == value, 'string marker')
    rows = [struct.unpack_from('<I4B', kernel, 0x198dcd8 + 8 * i) for i in range(3)]
    need(rows == [(100000,7,10,11,26),(400000,2,5,12,24),(1000000,1,3,9,18)], 'stock timing table')
    # Evaluated descriptor construction from the asserted stock helper for its
    # actual caller args (8 bits, 4 packed bytes, MSB-to-LSB within each byte).
    # This is data arithmetic; no stock executable is run.
    packing_vectors = [((7 + 8*i) << 5) | 0x10 | 0xe | (1 if i == 3 else 0) for i in range(4)]
    packing = [packing_vectors[0] | packing_vectors[1] << 10,
               packing_vectors[2] | packing_vectors[3] << 10]
    need(packing == [0x7f8fe, 0xffefe], 'packing derivation')

    spec = importlib.util.spec_from_file_location('saved_dt_parser', ROOT / PARSER)
    dt = importlib.util.module_from_spec(spec); spec.loader.exec_module(dt)
    base_raw, overlay_raw = (ROOT / BASE).read_bytes(), (ROOT / OVERLAY).read_bytes()
    dtbo = (ROOT / 'TB_J716F_ZUI_15.0.485/image/dtbo.img').read_bytes()
    need(blob.find(base_raw) == 40689664, 'base bytes in original boot')
    need(dtbo.find(overlay_raw) == 251484, 'overlay bytes in original dtbo')
    base, overlay, running = dt.fdt(base_raw), dt.fdt(overlay_raw), dt.fdt((ROOT / RUNNING).read_bytes())
    node = '/soc/i2c@a94000'; wrapper = '/soc/qcom,qupv3_1_geni_se@ac0000'
    goodix = node + '/goodix-gt738x@5d'
    need(base['/__symbols__']['qupv3_se13_i2c'] == (node + '\0').encode(), 'stock I2C symbol')
    need(overlay['/__fixups__']['qupv3_se13_i2c'] == b'/fragment@71:target:0\0', 'overlay target binding')
    need(running[node]['reg'] == struct.pack('>II', 0xa94000, 0x4000), 'running reg')
    need(running[node]['interrupts'] == struct.pack('>III',0,358,4), 'master irq')
    need(running[node]['compatible'] == b'qcom,i2c-geni\0', 'vendor compatible')
    need(running[goodix]['reg'] == struct.pack('>I',0x5d), 'Goodix address')
    selected = {node, wrapper, goodix}
    refs = set()
    for path in (node, goodix):
        for key, value in running[path].items():
            if (key.startswith('pinctrl-') and key != 'pinctrl-names') or key.endswith('-supply'):
                refs.update(struct.unpack('>' + str(len(value)//4) + 'I', value))
    for path, props in running.items():
        if props.get('phandle') and struct.unpack('>I', props['phandle'])[0] in refs:
            selected.update(p for p in running if p == path or p.startswith(path + '/'))
    readable = lambda props: {key: dt.readable(value) for key, value in props.items()}
    dt_evidence = {
        'original_base_file_offset_in_boot': 40689664,
        'original_overlay_file_offset_in_dtbo': 251484,
        'stock_base_controller': readable(base[node]),
        'stock_base_wrapper': readable(base[wrapper]),
        'stock_overlay_target': 'qupv3_se13_i2c -> /fragment@71',
        'stock_overlay_nodes': {p:readable(v) for p,v in overlay.items() if p.startswith('/fragment@71')},
        'saved_android_selected_nodes': {p:readable(running[p]) for p in sorted(selected)},
        'runtime_register_dump_available': False,
    }

    source_paths = [BOOT, 'TB_J716F_ZUI_15.0.485/image/dtbo.img', BASE, OVERLAY, RUNNING, PARSER,
        'analysis/device/20261111-231708/i2c.txt', 'analysis/device/20261111-231708/chip-details.txt',
        'analysis/stock-goodix-20261113/stock/goodix_cfg_group.bin',
        'analysis/stock-goodix-20261113/stock-protocol-evidence.json',
        'analysis/driver-adaptation-20261113/input-sources/linux-v6.12/drivers/i2c/busses/i2c-qcom-geni.c',
        'analysis/driver-adaptation-20261113/input-sources/linux-v6.12/drivers/clk/qcom/gcc-sm8250.c',
        'analysis/driver-adaptation-20261113/input-sources/linux-v6.12/drivers/pinctrl/qcom/pinctrl-sm8250.c']
    downloads = json.loads((OUT/'download-index.json').read_text())
    for item in downloads:
        need(ident(item['path']) == {key:item[key] for key in ('bytes','sha256')}, 'download file identity')
        source_paths.append(item['path'])
    sources = {p: ident(p) for p in source_paths}
    evidence = {
        'scope': 'Original Lenovo stock instructions/DT plus explicitly separate generic Linux references',
        'stock_kernel': {'boot': BOOT, 'boot_sha256': BOOT_SHA, 'offset':4096,'bytes':len(kernel),'sha256':KERNEL_SHA},
        'address_basis': 'Image-relative offsets, not physical/runtime addresses; function labels inferred from string xrefs and calls.',
        'instruction_assertions': len(facts), 'facts': facts, 'bounded_windows': windows,
        'stock_strings': {hex(p):v[:-1].decode() for p,v in markers.items()},
        'stock_timing_table': {'offset':'0x198dcd8','rows_frequency_div_high_low_cycle': rows, 'default_hz_if_property_absent':400000},
        'stock_packing_derivation': {'caller_args':[8,4,1], 'vectors':[hex(x) for x in packing_vectors],
                                    'tx_rx_pack0':hex(packing[0]), 'tx_rx_pack1':hex(packing[1]), 'byte_gran':8>>4,
                                    'scope':'Arithmetic evaluation of asserted instruction semantics; no binary execution'},
        'proven_stock_behaviour': [
            'IF_DISABLE register is checked as an entire 32-bit value: zero chooses FIFO/SE-DMA initialization; nonzero selects GSI.',
            'Protocol extraction is FW_REVISION bits15:8 and I2C requires3.',
            'Stock TX/RX FIFO depth helpers extract6 bits at16; TX width extracts6 bits at24.',
            'Timeout/error recovery attempts cancel then abort with separate completion waits; it logs failure and continues cleanup.',
            'Cancel writes4 and abort writes2 to0x604. For I2C, cancel first masks DONE in0x614.',
            'I2C FIFO RX routine uses count24:0 and expected message length; it does not inspect LAST_VALID.',
            'Error diagnostics read GENI_STATUS0x40 and GENI_IOS0x908; there is no evidence for IOS0x50.',
        ],
        'generic_reference_only': [
            'STATUS M_ACTIVE bit0/S_ACTIVE bit12; these bit meanings are pinned Linux reference, not asserted as a new stock-I2C instruction finding.',
            'TX_WC27:0, RX_LAST31, LAST_VALID30:28; UART reference documents valid0 means a full4-byte word.',
            'Linux UART console masks M/S IRQ_EN then polls status. This is a generic polling precedent, not I2C7 live validation.',
            'Modern Linux expands depth mask for QUP>=3.10; this is not a measured version of this tablet.',
        ],
        'unproven': [
            'The currently deployed firmware IF_DISABLE, GPI/FIFO mode, firmware protocol, depth/width, clocks, pin mux and regulators.',
            'I2C SDA/SCL idle meaning of IOS bits; M_ACTIVE0 and cancel/abort ACK cannot establish electrical bus idle.',
            'Target-side side effects, late callback/IRQ retirement and complete ownership fence.',
            'Availability of raw MIRQ polling with all IRQ routes disabled on this exact controller/firmware.',
        ],
        'device_actions':False,'VM_actions':False,'builds':False,'image_executed':False,'runtime_qualified':False,
    }
    return {'evidence.json': canonical(evidence), 'dt-evidence.json':canonical(dt_evidence),
            'source-manifest.json': canonical({'files':sources,'downloads':downloads}),
            'disassembly.txt':'\n'.join(disassembly)+'\n'}


if __name__ == '__main__':
    parser=argparse.ArgumentParser(); group=parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--write',action='store_true'); group.add_argument('--check',action='store_true')
    args=parser.parse_args(); outputs=audit()
    for name,data in outputs.items():
        if args.write: (OUT/name).write_text(data,encoding='utf-8')
        else: need((OUT/name).read_text(encoding='utf-8')==data, 'receipt drift '+name)
    print(json.dumps({'status':'passed','instruction_assertions':len(FACTS),'bounded_windows':len(WINDOWS),
                      'reports':len(outputs),'device_actions':False,'runtime_qualified':False}))
