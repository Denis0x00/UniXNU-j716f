#!/usr/bin/env python3
"""Instrument only the pinned ArmCpu copy in an isolated source-only build tree."""
import hashlib
import json
from pathlib import Path
import shutil

ARMCPU_COMMIT = '9066b53bbc82e92a98ec44a11dd50fd13eaf1a1b'
ARMCPU_SHA256 = '3c87b3bd617be5582a6a24ff797c359497ee9feaa702ee883ecee4270db21c9f'
FILES = ('J716fArmCpuState.h', 'J716fArmCpuObserver.inc')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def instrument(data):
    if digest(data) != ARMCPU_SHA256:
        raise ValueError('Refuse unreviewed ArmCpu bytes; pinned SHA256 mismatch')
    text = data.decode('utf-8')
    edits = [
        ('#define ARMCPU_SMP_SLOTS 8', '#include "J716fArmCpuObserver.inc"\n\n#define ARMCPU_SMP_SLOTS 8'),
        ('    if ((!skipGic && !prepareGic ())', '    j716fArmCpuSetup(1, skipGic);\n    if ((!skipGic && !prepareGic ())'),
        ('  cpuName = OSSymbol::withCStringNoCopy ("ARM architectural boot CPU");',
         '  j716fArmCpuSetup(2, 0);\n  cpuName = OSSymbol::withCStringNoCopy ("ARM architectural boot CPU");'),
        ('  armcpu_registered = 1;', '  armcpu_registered = 1;\n  j716fArmCpuSetup(3, logicalId);'),
        ('  gController = controller;', '  j716fArmCpuSetup(4, cpuCount);\n  gController = controller;'),
        ('  armcpu_gic_ready = 1;', '  armcpu_gic_ready = 1;\n  if (!secondary) { j716fArmCpuSetup(5, 0); }'),
        ('\n  enableCPUTimeBase (true);', '\n  enableCPUTimeBase (true);\n  if (!secondary) { j716fArmCpuSetup(6, 0); }'),
        ('  drainPending ();\n  return true;', '  drainPending ();\n  j716fArmCpuSetup(7, 0);\n  return true;'),
        ('      rtclock_intr (0);', '      j716fArmCpuEvent(1, logicalId, id);\n      rtclock_intr (0);\n      j716fArmCpuEvent(2, logicalId, id);'),
        ('    __asm__ volatile ("msr icc_eoir1_el1, %0; isb" :: "r" (id) : "memory");',
         '    __asm__ volatile ("msr icc_eoir1_el1, %0; isb" :: "r" (id) : "memory");\n    if ((id & 0xffffff) == 27) { j716fArmCpuEvent(3, logicalId, id); }'),
        ('  armcpu_quiesce_calls=armcpu_quiesce_calls+1;',
         '  armcpu_quiesce_calls=armcpu_quiesce_calls+1;\n  if (!secondary) { j716fArmCpuSetup(8, 0); }'),
        ('  armcpu_halt_calls=armcpu_halt_calls+1;',
         '  armcpu_halt_calls=armcpu_halt_calls+1;\n  if (!secondary) { j716fArmCpuSetup(9, 0); }'),
    ]
    for old, new in edits:
        if text.count(old) != 1:
            raise ValueError('Observer hook must match exactly once: '+old)
        text = text.replace(old, new)
    restored = text
    for old, new in reversed(edits):
        if restored.count(new) != 1:
            raise ValueError('Observer reverse audit is ambiguous')
        restored = restored.replace(new, old)
    if restored.encode('utf-8') != data:
        raise ValueError('Original ArmCpu instructions were changed')
    return text.encode('utf-8')


def install(device, tree, source, commit):
    device, tree, source = Path(device), Path(tree), Path(source)
    if commit != ARMCPU_COMMIT:
        raise ValueError('Unexpected ArmCpu commit')
    route = json.loads((device/'scripts/j716f-native-timer-route.json').read_text(encoding='utf-8'))
    baseline = json.loads((device/'scripts/j716f-hardware-evidence.json').read_text(encoding='utf-8'))
    if (route['virtual_timer_intid'] != 27 or route['virtual_timer_ppi'] != 11
            or route['interrupt_specifiers'][2][:2] != [1, 11]
            or not route['stock_recovery_interrupts_agree']
            or route['timer_hz'] != baseline['timer_hz']
            or any(route[key] != baseline[key] for key in ('stock_dtb_sha256', 'recovery_fdt_sha256'))):
        raise ValueError('Pinned timer INTID differs from independent board input evidence')
    link = tree/'drivers/ArmCpu'
    if not link.is_symlink() or link.resolve() != source.resolve():
        raise ValueError('Expected pristine component symlink in isolated tree')
    destination = tree.parent/'native-observer/ArmCpu'
    if destination.exists():
        raise ValueError('Observer source already exists; refuse overwrite')
    original = (source/'ArmCpu.cpp').read_bytes()
    patched = instrument(original)
    # Keep the fetched/cache component pristine. Replace only the build-tree link.
    shutil.copytree(source, destination, ignore=shutil.ignore_patterns('.git'))
    (destination/'ArmCpu.cpp').write_bytes(patched)
    hashes = {}
    for name in FILES:
        data = (device/'package/native-observer'/name).read_bytes()
        (destination/name).write_bytes(data)
        hashes[name] = digest(data)
    link.unlink()
    link.symlink_to(destination.resolve(), target_is_directory=True)
    return {'armcpu_commit': commit, 'original_sha256': digest(original),
            'instrumented_sha256': digest(patched), 'observer_files': hashes,
            'original_source_reverse_audit': True,
            'snapshot_bytes': 192*8, 'sample_irqs': [1, 2, 8],
            'timer_route_sha256': digest((device/'scripts/j716f-native-timer-route.json').read_bytes()),
            'scope': 'live kernel-owned memory only; no persistent RAM writer',
            'hardware_validated': False}
