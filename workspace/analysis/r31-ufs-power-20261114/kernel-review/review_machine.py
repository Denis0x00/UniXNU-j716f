#!/usr/bin/env python3
"""Read-only actual-build evidence for the separate r31 Power kernel.

Produces evidence, never a frozen acceptance contract or a device image.
The caller must independently inspect these bytes before freezing a contract.
"""
import json
from pathlib import Path
import struct
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE.parent))
import verify_storage_kernel as v
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM

raw = (HERE / 'kernel.macho').read_bytes()
native = (HERE / 'native.macho').read_bytes()
image, layout = v._discover(raw)
nm = v.MachO(native)
disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
build = json.loads((HERE / 'build-result.json').read_text())
archive = json.loads((HERE / 'storage-archive-result.json').read_text())
v.require(build['status'] == 'passed' and build['commands'][0]['exit_code'] == 0, 'Official build did not pass')
v.require(build['commands'][0]['argv'][-1] == 'kernel' and build['target'] == 'j716f-host-ufs-power-r31', 'Wrong official target')
v.require(not build['device_actions'] and not build['qemu'], 'Unexpected device/QEMU work')
v.require(build['target_source_manifest']['sha256'] == v.sha((ROOT / 'work/UniXNU-j716f-host-ufs-power-r31/docs/host-source-manifest.json').read_bytes()), 'Build target provenance changed')
v.require(build['artifacts'][0]['sha256'] == v.sha(native) and build['artifacts'][1]['sha256'] == v.sha(raw), 'Actual build executable mismatch')
v.require(archive['passed'] and archive['unique_owner_translation_unit'] and not archive['native_ufs_cpp_compiled'], 'Storage archive composition failed')
v.require(len(archive['translation_units']) == len(archive['objects']) == len(archive['archive_members']) == 9, 'Not nine storage translation units')
v.require(archive['inputs_before'] == archive['inputs_after'], 'Storage inputs changed while compiling')
v.require(all(not item['dynamic_global_initializers'] for item in archive['objects'].values()), 'Dynamic storage constructor present')
v.require(all(n == 1 for n in archive['required_defined_symbols'].values()), 'Duplicate required archive definition')

def function(name):
    f = v.base._function(image, name)
    nf = v.base._function(nm, name)
    low, high = v.address(f['start']), v.address(f['end'])
    v.require(f == nf and image.body(low, high-low) == nm.body(low, high-low), 'Native/prelink changed ' + name)
    return dict(f, native_identical=True, instructions=[
        {'va': hex(i.address), 'hex': i.bytes.hex(), 'mnemonic': i.mnemonic, 'operands': i.op_str}
        for i in disassembler.disasm(image.body(low, high-low), low)])

initialize = '__ZN13j716f_storage10initializeERNS_5StateERKNS_7BackendEPNS_6ReportE'
log = '__ZN12_GLOBAL__N_111storage_logEN13j716f_storage5EventERKNS0_6ReportE'
backend = '__ZN12_GLOBAL__N_115storage_backendE'
names = (v.CALLER_NAME, v.CALLEE_NAME, '_bsd_autoconf', initialize, log,
         '_PE_j716f_earlylog_init', '_PE_j716f_earlylog_write', '_PE_init_printf',
         '_console_write', '_console_write_unbuffered', '_consdebug_log')
functions = {name: function(name) for name in names + v.BRINGUP_NAMES + v.RUNTIME_NAMES + v.OWNER_NAMES + (v.POWER_NAME,)}
power_symbols = {}
for fragment in ('actualContext', 'mapPage', 'readRegister', 'unmapPage', 'logEvent'):
    hits = [name for name in image.names if name.startswith('__ZN12_GLOBAL__N_1' + str(len(fragment)) + fragment)]
    v.require(len(hits) == 1, 'Need unique Power binding function: ' + fragment)
    power_symbols[fragment] = hits[0]
for fragment in ('3run', '7address', '12registerName', '10resultName'):
    hits = [name for name in image.names if name.startswith('__ZN11j716f_power') and fragment in name]
    v.require(len(hits) == 1, 'Need unique Power core function: ' + fragment)
    power_symbols[fragment] = hits[0]
for name in power_symbols.values(): functions[name] = function(name)
hooks = {}
for caller, callee in (('_PE_init_printf', '_PE_j716f_earlylog_init'),
                       ('_console_write', '_PE_j716f_earlylog_write'),
                       ('_console_write_unbuffered', '_PE_j716f_earlylog_write'),
                       ('_consdebug_log', '_PE_j716f_earlylog_write')):
    hooks[caller] = v._one_call(image, functions[caller], callee)
backend_va = v.base._symbol(image, backend)
pointers = struct.unpack('<8Q', image.body(backend_va, 64))
backend_names = ('_sm8250_ufs_adapter_init', '_sm8250_ufs_observe', '_sm8250_ufs_allocate',
                 '_sm8250_ufs_activate', '_sm8250_ufs_read_known', '_sm8250_ufs_stop',
                 '_sm8250_ufs_discard_unused', log)
v.require(pointers == tuple(v.base._symbol(image, name) for name in backend_names), 'Wrong backend pointers')
v.require(image.body(backend_va, 64) == nm.body(backend_va, 64), 'Backend changed during prelink')

# Actual adapter initialization is only ADRP/ADD of its private RAM flag,
# MOV w9,1, release-store, RET. There is no MMIO/allocator call in this function.
adapter_va = v.address(functions['_sm8250_ufs_adapter_init']['start'])
words = struct.unpack('<5I', image.body(adapter_va, 20))
v.require(words[0] & 0x9f00001f == 0x90000008 and words[1] & 0xffc003ff == 0x91000108,
          'Unexpected adapter flag address materialization')
flag_va = (adapter_va & ~0xfff) + (v._signed21(words[0]) << 12) + ((words[1] >> 10) & 0xfff)
v.require(words[2:] == (0x52800029, 0x889ffd09, 0xd65f03c0), 'Adapter initializer acquired extra behavior')
v.require(any(seg == '__DATA' and section == '__bss' and vm <= flag_va and flag_va+4 <= vm+count
              for seg,section,vm,count in image.sections), 'Adapter flag not in private kernel BSS')
v.require(v.address(functions['_sm8250_ufs_adapter_init']['end']) == adapter_va + 20, 'Extra adapter init code')

# Dynamic code layout was reviewed above: init receives the private Backend,
# invokes only Backend.init and Backend.log for successful registration.
# Remaining bridge calls are exported but are never called by bsd_autoconf.
for name in v.BRINGUP_NAMES[1:]:
    v.require(not v._calls(image, functions['_bsd_autoconf'], name) and
              not v._calls(image, functions['_bsd_init'], name), 'Automatic storage action: ' + name)

regions = []
for va, code in v.expected_regions(layout):
    is_reset = va == v.address(layout['caves']['4'])
    instruction_bytes = code[:64] if is_reset else code
    regions.append({'va': hex(va), 'bytes': len(code), 'sha256': v.sha(code),
                    'instructions': [{'va': hex(i.address), 'hex': i.bytes.hex(),
                                      'mnemonic': i.mnemonic, 'operands': i.op_str}
                                     for i in disassembler.disasm(instruction_bytes, va)],
                    'literal_delay_ticks': struct.unpack_from('<Q', code, 64)[0] if is_reset else None})
v.require(len(regions) == 6 and regions[-1]['literal_delay_ticks'] == 115200000, 'Wrong diagnostic regions')
report = {
    'status': 'unreviewed-power-machine-evidence',
    'kernel': {'bytes': len(raw), 'sha256': v.sha(raw)},
    'native': {'bytes': len(native), 'sha256': v.sha(native)},
    'functions': functions,
    'backend_table': {'va': hex(backend_va), 'native_identical': True,
                      'callbacks': dict(zip(backend_names, map(hex, pointers)))},
    'adapter_init': {'flag_va': hex(flag_va), 'only_private_ram_flag_store': True},
    'earlylog_hooks': hooks,
    'registration': {'source_and_actual_callback_path_reviewed': True,
                     'only_init_and_logging_inside_registration': True, 'power_called_after_registration': layout['storage']['power_call'], 'success_gate': layout['storage']['success_gate'],
                     'autoconf_out_of_line': True, 'automatic_other_bridge_calls': False},
    'power_symbols': power_symbols,
    'power_scope_manual_review': {'fixed_eleven_register_addresses': False, 'single_volatile_read_only': False,
                                  'map_readonly_inhibit_cache': False, 'per_read_log_order_and_single_attempt': False,
                                  'actual_context_and_unchanged_old_authority': False, 'no_false_abort_containment': False},
    'probe_regions': regions,
    'abi': {'original_x0_zero_retained': True, 'original_sp_retained': True,
            'callee_original_frame_and_lr_restoration': True,
            'wrapper_bl_returns_before_timer_start': True, 'no_root_lookup_or_mount_before_reset': True},
    'archive': {'translation_units': len(archive['translation_units']), 'global_constructors': False,
                'bridge_exports': len(v.BRINGUP_NAMES), 'power_entry_exports': 1, 'runtime_exports': len(v.RUNTIME_NAMES),
                'owner_exports': len(v.OWNER_NAMES), 'all_unique': True,
                'max_reported_stack_bytes': max(o['max_reported_stack_bytes'] for o in archive['objects'].values())},
    'hardware_tested': False,
    'limits': 'Only host source/build/binary evidence. Live fixed Power MMIO results, abort containment, UFS ownership, DMA, capacity, known-block read, root mount and userland remain untested. Prior BSD initialization may block or start workers.'
}
with (HERE / 'machine-review.json').open('x', encoding='utf-8', newline='\n') as stream:
    json.dump(report, stream, indent=2); stream.write('\n')
print(json.dumps({key: value for key, value in report.items() if key not in ('functions','probe_regions')}, indent=2))
