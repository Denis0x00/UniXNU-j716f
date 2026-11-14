"""Verify final firmware linkage, diagnostic policy and prior payload identity."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys

root = Path(sys.argv[1]).resolve()
reports = root / 'analysis/host-protection-20261112'
attempt = int(sys.argv[2]) if len(sys.argv) > 2 else 0
report = reports / ('binary-verification' + (f'-{attempt:02d}' if attempt else '') + '.json')
if report.exists():
    raise SystemExit('Refusing to overwrite prior verification')
build = root / 'work/uefi-j716f/Build/j716fPkg/DEBUG_CLANGPDB'
package = root / 'UniXNU/out/j716f/ipados-17.7.11-21H461/j716f/package/Build/UniXNU/DEBUG_CLANGPDB'
module = 'AARCH64/UniXNUPkg/Library/UniXNULib/UniXNULib/DEBUG/AutoGen.h'
def pcd(path):
    text = path.read_text()
    match = re.search(r'^#define _PCD_VALUE_PcdUniXNUDiagnosticMenuHold (\d+)', text, re.M)
    assert match, str(path)
    return int(match.group(1))
assert pcd(build / module) == 1
assert pcd(package / module) == 0
sec_libs = (build / 'AARCH64/SiliciumPkg/Sec/Sec/OUTPUT/static_library_files.lst').read_text()
assert 'J716fEarlyHandoffLib.lib' in sec_libs
assert 'EarlyHandoffLibNull.lib' not in sec_libs
assert 'BaseFdtLib.lib' in sec_libs
libfdt = build / 'AARCH64/MdePkg/Library/BaseFdtLib/BaseFdtLib/OUTPUT/BaseFdtLib.lib'
nm = subprocess.check_output(['llvm-nm-18', str(libfdt)], text=True)
assert re.search(r'\bT fdt_check_full$', nm, re.M)
checked = {}
markers = {
    'Sec.efi': [b'J716FHANDOFF', b'invalid FDT structure or strings', b'invalid FDT block layout/version',
                b'initrd/padding outside input RAM or overlaps live region',
                b'accepted; input pages reserved before heap allocation',
                b'handoff rejected before heap initialization'],
    'UniXNU.efi': [b'menu: diagnostic hold enabled', b'enumeration skipped',
                   'Diagnostic hold:'.encode('utf-16le')],
    'J716fHandoffDiagnosticsDxe.efi': [b'J716FDIAG BEGIN', b'J716FDIAG MAP[', b'J716FDIAG END'],
}
for name, required in markers.items():
    data = (build / 'AARCH64' / name).read_bytes()
    assert data[:2] == b'MZ'
    pe = int.from_bytes(data[0x3c:0x40], 'little')
    assert data[pe:pe+4] == b'PE\0\0'
    assert int.from_bytes(data[pe+4:pe+6], 'little') == 0xaa64
    assert all(x in data for x in required), [x for x in required if x not in data]
    checked[name] = {'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()}
sec_map = (build / 'AARCH64/SiliciumPkg/Sec/Sec/OUTPUT/Sec.map').read_text()
assert 'InputRange' in sec_map and 'AppendRegion' in sec_map
# The SEC file belongs to the uncompressed FV, other modules to FVMAIN.
assert '9AFFB503-E643-4141-8B90-17E8588B1D35' in (build / 'FV/Guid.xref').read_text()
assert '5A40B705-332E-4C9E-A979-BE3D309F26F6' in (build / 'FV/FVMAIN.Fv.txt').read_text()
payload = root / 'UniXNU/out/j716f/ipados-17.7.11-21H461/j716f/payload/payload.img'
payload_hash = hashlib.sha256(payload.read_bytes()).hexdigest()
assert payload_hash == 'fc4d73dd2e8f95c8482e2fe17b446ee1397f7b64ab5f011d7b89de03dadde853'
data = {'status': 'passed', 'hardware_tested': False, 'device_actions': False, 'qemu': False,
        'firmware_diagnostic_hold_pcd': 1, 'standalone_package_diagnostic_hold_pcd': 0,
        'sec_library': 'J716fEarlyHandoffLib', 'native_full_fdt_validator_link_input': True,
        'note': 'LTO inlines Prepare/Apply/full-validator; retained code/strings and linked archive checked.',
        'pe_files': checked, 'unchanged_previous_payload_sha256': payload_hash}
report.write_text(json.dumps(data, indent=2) + '\n')
print(json.dumps(data, indent=2))
