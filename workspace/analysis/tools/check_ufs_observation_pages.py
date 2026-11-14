"""Read actual XNU compiler macros without replacing any kernel build output."""
import hashlib
import json
from pathlib import Path
import subprocess

root = Path('/home/test/unixnu-work')
out = root / 'UniXNU/out-host-ufs-observation-20261113/j716f-host-ufs-observation/ipados-17.7.11-21H461/j716f_host_ufs_observation'
tree = out / 'tree'
reports = root / 'analysis/host-ufs-observation-20261113'
record_path = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(record_path.read_text().strip().rstrip(','))


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


if not __debug__:
    raise SystemExit('Assertions must be enabled')
report_path = reports / 'page-granule-02.json'
assert not report_path.exists()
arguments = record['arguments']
assert arguments.count('-o') == arguments.count('-c') == 1
object_path = Path(arguments[arguments.index('-o') + 1])
if not object_path.is_absolute():
    object_path = Path(record['directory']) / object_path
before = {str(p): sha(p) for p in (record_path, object_path)}
command, index = [], 0
while index < len(arguments):
    arg = arguments[index]
    if arg in ('-o', '-MF', '-MJ', '-MT', '-MQ'):
        index += 2
        continue
    if arg != '-c':
        command.append(arg)
    index += 1
macros_path = reports / 'IOPlatformExpert-page-macros-02.txt'
command += ['-E', '-dM', '-o', str(macros_path)]
assert not macros_path.exists()
result = subprocess.run(command, cwd=record['directory'], text=True, capture_output=True)
(reports / 'page-granule-compile-02.log').write_text(result.stdout + result.stderr)
data = {'passed': False, 'command': command, 'exit_code': result.returncode,
        'compiler_record': str(record_path), 'compiler_record_sha256': before[str(record_path)],
        'scope': 'Actual build preprocessor page-size configuration; not hardware page-table readback',
        'hardware_qualified': False}
try:
    assert result.returncode == 0
    macros = {}
    for line in macros_path.read_text().splitlines():
        if not line.startswith('#define '):
            continue
        parts = line.split(maxsplit=2)
        macros[parts[1]] = parts[2] if len(parts) > 2 else ''
    selected = {key: macros.get(key) for key in ('BCM2837', 'ARM64_BOARD_CONFIG_BCM2837', 'PAGE_SHIFT', 'PAGE_SIZE', 'PAGE_SIZE_FIXED',
                'ARM_PGSHIFT', 'ARM_PGBYTES', '__ARM_4K_PG__', '__ARM_16K_PG__', 'APPLE_ARM64_ARCH_FAMILY')}
    data['macros'] = selected
    assert 'BCM2837' in macros and selected['ARM64_BOARD_CONFIG_BCM2837'] == '1', 'Wrong compiled board configuration'
    assert selected['PAGE_SHIFT'] == 'PAGE_SHIFT_CONST' and selected['PAGE_SIZE_FIXED'] == '1', 'Unexpected kernel page-size policy'
    assert selected['ARM_PGSHIFT'] == '12'
    assert '__ARM_16K_PG__' not in macros and 'APPLE_ARM64_ARCH_FAMILY' not in macros, 'Unexpected 16K or Apple page branch'
    source = tree / '.cache/xnu-work'
    arm_init = source / 'osfmk/arm/arm_init.c'
    vm_param = source / 'osfmk/mach/arm/vm_param.h'
    proc_reg = source / 'osfmk/arm64/proc_reg.h'
    expected = '#if !defined(APPLE_ARM64_ARCH_FAMILY) && !__ARM_16K_PG__\n\t\tPAGE_SHIFT_CONST = ARM_PGSHIFT;\n#else'
    assert expected in arm_init.read_text(), 'Missing native 4K runtime page-size initialization branch'
    assert 'extern int PAGE_SHIFT_CONST;' in vm_param.read_text(), 'Unexpected runtime PAGE_SHIFT_CONST declaration'
    assert '#ifdef __ARM_16K_PG__\n#define ARM_PGSHIFT 14\n#else\n#define ARM_PGSHIFT 12\n#endif' in proc_reg.read_text(), 'Unexpected hardware granule definitions'
    data['source_hashes'] = {str(p.relative_to(tree)): sha(p) for p in (arm_init, vm_param, proc_reg)}
    data['configuration_and_source_select_4k'] = True
    data['runtime_page_tables_observed'] = False
    data['limits'] = ['PAGE_SHIFT_CONST is initialized at runtime, not a preprocessor literal.',
                      'This verifies actual compiler configuration and patched initialization source, not live pmap mappings.']
    data['prior_audit'] = 'page-granule.json retained: incorrect literal/optional-macro expectations; no driver or kernel inputs changed.'
    data['macro_output_sha256'] = sha(macros_path)
    data['retained_records_and_object_unchanged'] = all(sha(Path(p)) == expected for p, expected in before.items())
    assert data['retained_records_and_object_unchanged']
    data['passed'] = True
except Exception as exc:
    data['error'] = str(exc)
finally:
    report_path.write_text(json.dumps(data, indent=2) + '\n')
print(json.dumps(data.get('macros', data)))
raise SystemExit(0 if data['passed'] else 1)
