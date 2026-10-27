"""Historical generic firmware preflight; NOT the admitted native build path.

Preserved for provenance only. Includes legacy TzDxe integration checks.
"""
import hashlib
import json
import re
import subprocess
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit('j716f preflight: ' + message)


tree = Path.cwd()
uefi = tree / '.cache/firmware'
require((uefi / 'Platforms/Lenovo/j716fPkg/j716f.dsc').is_file(),
        'link device .cache/firmware to the prepared uefi-j716f checkout')
vendor = uefi / 'Binaries/j716f'
manifest = json.loads((vendor / 'manifest.json').read_text())
require(manifest['source_sha256'] == 'e8e6d75e19b666466a3c07e48fe133c0a3a6338db79452e31067dd1042e344e8',
        'wrong ZUI XBL source')
count = 0
for item in manifest['files']:
    for name, info in item['files'].items():
        path = vendor / name
        require(path.is_file(), f'missing {path}')
        data = path.read_bytes()
        require(len(data) == info['bytes'] and hashlib.sha256(data).hexdigest() == info['sha256'], f'changed vendor input {name}')
        count += 1
board = uefi / 'Platforms/Lenovo/j716fPkg'
adapter = 'j716fPkg/Drivers/J716fScmCompatDxe/J716fScmCompatDxe.inf'
require((board / 'Drivers/J716fScmCompatDxe/J716fScmCompatDxe.inf').is_file(),
        'SCM compatibility adapter missing')
require(adapter in (board / 'j716f.dsc').read_text() and
        adapter in (board / 'Include/DXE.inc').read_text(),
        'SCM adapter must be built and included in firmware')
apriori = (board / 'Include/APRIORI.inc').read_text()
require(apriori.index('Binaries/j716f/Drivers/ScmDxe/ScmDxe.inf') < apriori.index(adapter) <
        apriori.index('Binaries/j716f/Drivers/TzDxe/TzDxe.inf'),
        'SCM adapter must run after ScmDxe and before TzDxe')
source = (board / 'Library/MemoryMapLib/MemoryMapLib.c').read_text()
regions = []
for match in re.finditer(r'\{"([^"]+)",\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*([^}]+)\}', source):
    regions.append([match[1], int(match[2],16), int(match[3],16), *[s.strip() for s in match[4].split(',')]])
require(regions == json.loads((board / 'memory-map.json').read_text()), 'C memory map differs from audited JSON')
# Names are a runtime interface: the stock TzDxe queries this descriptor by name.
stock = (vendor / 'RawFiles/uefiplat.cfg').read_text()
tzmatch = re.search(r'^(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*"TZApps Reserved",\s*(.+)$', stock, re.M)
require(tzmatch is not None, 'stock TZApps Reserved descriptor missing')
stock_tzapps = ['TZApps Reserved', int(tzmatch[1],16), int(tzmatch[2],16),
                *[s.strip() for s in tzmatch[3].split(',')]]
require([r for r in regions if r[0] == 'TZApps Reserved'] == [stock_tzapps],
        'TZApps Reserved must retain its stock name, range and attributes for TzDxe')
ram = sorted((r for r in regions if r[1] >= 0x80000000), key=lambda r:r[1])
require(len(regions) < 128, 'too many memory descriptors')
for a,b in zip(ram,ram[1:]):
    require(a[1] + a[2] <= b[1], f'overlapping {a[0]} and {b[0]}')
smem = next(r for r in regions if r[0] == 'SMEM')
require(smem[1:3] == [0x80900000,0x200000] and smem[-1] == 'UNCACHED_UNBUFFERED_XN', 'SMEM must use Normal noncacheable memory')
require([r[0] for r in ram if r[3] == 'AddMem' and r[4] == 'SYS_MEM' and r[-2] == 'Conv'] == ['DXE Heap'], 'unexpected allocatable DRAM')
require(not re.search(r'^\s*INF\s+.*RamManagerDxe', (board / 'j716f.fdf').read_text(), re.M), 'diagnostic map must not be expanded by RamManager')
for path in (board/'Include').glob('*.inc'):
    text = path.read_text()
    require('9707f' not in text, f'foreign board input in {path}')
    for relative in re.findall(r'Binaries/[^\s]+', text):
        require((uefi / relative).is_file(), f'missing {relative}')
for relative, pin in [
    ('Common/Mu', 'b8d46c71610318e36a661819e1e3c0b8bdda413a'),
    ('Common/Mu_OEM_Sample', 'b70b4b7bb02c43bf9db2172dfb3ed0457e2361d6'),
    ('Mu_Basecore', '3c9c7b2e211e62d2a954ad760a36fe05adc43aa3'),
    ('Silicon/Silicium/OpensslPkg/Library/OpensslLib/openssl', '67b5686b4419b4cb8caa502711c41815f5279751')]:
    result = subprocess.check_output(['git','-C',str(uefi / relative),'rev-parse','HEAD'],text=True).strip()
    require(result == pin, f'wrong dependency {relative}: {result}')
print(f'j716f preflight: {count} vendor files, {len(regions)} memory regions checked')
print('j716f: diagnostic build only; live RAM/overlay/stride and hardware boot remain unverified')
