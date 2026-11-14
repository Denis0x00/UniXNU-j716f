"""Check final build evidence and command scope without device access."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tomllib

root = Path(sys.argv[1]).resolve()
report = root / 'analysis/host-images-20261112/diagnostics-verification.json'
if report.exists():
    raise SystemExit('Refusing to overwrite prior verification')
build = root / 'work/uefi-j716f/Build/j716fPkg/DEBUG_CLANGPDB'
driver = 'J716fHandoffDiagnosticsDxe'
binary = build / 'AARCH64' / (driver + '.efi')
data = binary.read_bytes()
assert data[:2] == b'MZ'
pe = int.from_bytes(data[0x3c:0x40], 'little')
assert data[pe:pe + 4] == b'PE\0\0' and int.from_bytes(data[pe + 4:pe + 6], 'little') == 0xaa64
markers = [b'J716FDIAG BEGIN', b'J716FDIAG MAP[', b'J716FDIAG END',
           b'J716FDIAG FDT incoming-x0', b'J716FDIAG GOP', b'system-table', b'hob-list']
assert all(marker in data for marker in markers), [m.decode() for m in markers if m not in data]
guid = '5A40B705-332E-4C9E-A979-BE3D309F26F6'
assert guid in (build / 'FV/FVMAIN.Fv.txt').read_text()
assert guid + ' ' + driver in (build / 'FV/Guid.xref').read_text()
libs = (build / 'AARCH64/j716fPkg/Drivers' / driver / driver / 'OUTPUT/static_library_files.lst').read_text()
assert 'DxeAdvancedLoggerLib.lib' in libs and 'BaseDebugLibAdvancedLogger.lib' in libs
source = root / 'work/uefi-j716f/Platforms/Lenovo/j716fPkg/Drivers' / driver / 'J716fHandoffDiagnostics.c'
tests = (root / 'analysis/host-images-20261112/diagnostics-test.log').read_text()
assert '31 diagnostic safety cases passed' in tests
rejections = []
for argv in [['bootimg'], ['userdata'], ['all'], ['kernel'], ['release'], ['payload', '--kernel', '/tmp/unreviewed']]:
    r = subprocess.run([sys.executable, str(root / 'work/UniXNU-j716f/scripts/images.py'), *argv],
                       capture_output=True, text=True)
    assert r.returncode == 2
    rejections.append(argv)
device = tomllib.loads((root / 'work/UniXNU-j716f/porting.toml').read_text())
assert all(x['status'] == 'planned' for x in device['area'])
record = {'status': 'passed', 'device_actions': False, 'hardware_tested': False,
          'firmware_pe_machine': 'AARCH64', 'module_guid_in_final_fv': guid,
          'diagnostic_binary_sha256': hashlib.sha256(data).hexdigest(),
          'diagnostic_source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
          'logger_library_binding': 'AdvancedLogger DXE', 'fault_and_boundary_cases': 31,
          'rejected_wrapper_arguments': rejections, 'full_device_areas_planned': len(device['area']),
          'limits': 'Build evidence and host tests only; no live firmware map or handoff observed.'}
report.write_text(json.dumps(record, indent=2) + '\n')
print(json.dumps(record, indent=2))
