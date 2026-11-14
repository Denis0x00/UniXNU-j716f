"""Create a fresh, inactive GENI archive integration target from DT-panic base."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[2]
base = ROOT/'work/UniXNU-j716f-host-dt-panic'
target = ROOT/'work/UniXNU-j716f-host-geni'
assert not target.exists()
shutil.copytree(base, target, ignore=shutil.ignore_patterns('.git', '__pycache__', 'host-source-manifest.json'))
for path in target.rglob('*'):
    if path.is_file() and path.suffix in ('.toml', '.py', '.md', '.json'):
        text = path.read_text(encoding='utf-8')
        text = text.replace('host-dt-panic', 'host-geni').replace('host_dt_panic', 'host_geni')
        path.write_text(text, encoding='utf-8', newline='\n')
(target/'configs/j716f_host_dt_panic_defconfig').rename(target/'configs/j716f_host_geni_defconfig')
path = target/'board.toml'
text = path.read_text().replace('# Eight archive recipes and prelink ABI settings are inherited unchanged.', '''# Original eight recipes plus the inactive Goodix/transaction/GENI archive.
archives = [
  "build-corecrypto-14.py",
  "build-corecrypto-14.py --namespace",
  "component:ArmDTPlatform",
  "component:ArmCpu",
  "component:SoftwareAES",
  "build-software-aes-bindings.py",
  "build-mockfs-fixture.py",
  "build-libpthread-kernel.py",
  "build-j716f-geni.py",
]''')
path.write_text(text, newline='\n')
path = target/'configs/j716f_host_geni_defconfig'
path.write_text(path.read_text()+'\nCONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,{tree}/build/j716f-geni/libJ716fGeniBackend.a"\n', newline='\n')
path = target/'patches/xnu/series'
path.write_text('j716f-geni-archive-dependency.patch\n', newline='\n')
patch = (ROOT/'work/UniXNU-j716f-host-ufs-adapter/patches/xnu/sm8250-ufs-adapter-dependency.patch').read_text()
patch = patch.replace('J716F_UFS_ADAPTER', 'J716F_GENI').replace('sm8250-ufs-adapter/libSm8250UfsAdapter.a', 'j716f-geni/libJ716fGeniBackend.a')
(target/'patches/xnu/j716f-geni-archive-dependency.patch').write_text(patch, newline='\n')
(target/'scripts/build-j716f-geni.py').write_text('''#!/usr/bin/env python3
"""Invoke the frozen backend builder from the official archive stage."""
from pathlib import Path
import os
import subprocess
import sys
device = Path(os.environ['UNIXNU_DEVICE']).resolve(strict=True)
assert device.name == 'UniXNU-j716f-host-geni'
source = device.parent/'J716fGeniBackend-20261113'
raise SystemExit(subprocess.call([sys.executable, str(source/'scripts/build.py'),
                                 '--tree', str(Path.cwd()), '--source', str(source)]))
''', newline='\n')
(target/'README.md').write_text('''# Independent J716F GENI backend kernel link

Host-only target derived from the frozen DT-panic kernel. It retains the five
components, eight original archive recipes and 58 original patches. A ninth
archive force-links the Goodix decoder, transaction owner, FIFO backend and
borrowed-MMIO adapter. One additional patch tracks that archive as an incremental
link dependency. The archive has no boot caller or automatic activation.

Only the standard official defconfig/fetch/tree/status/build kernel entry points
in scripts/host.py are supported. The official build retains bootstrap, native
kernel linking and prelink. No QEMU, payload, boot image or device operations.

This proves source and link integration, not touch availability. Actual use needs
an independently verified live owner, Device MMIO mapping, clock/pin/power grants,
FIFO-mode evidence, external bus-effect fence and real HID sink. Stock registers
and saved DT identify resources but cannot grant current-boot ownership. All
complete porting areas remain planned. Deployed r6 remains the next onsite test.
''', newline='\n')
(target/'AGENTS.md').write_text('''# Independent inactive GENI host target

Read README.md and board.toml. Use scripts/host.py for official kernel operations.
Preserve all old targets and artifacts. Keep the five components and original
eight archives/58 patches; the ninth archive and dependency patch only integrate
the new inactive backend. Never claim compile/link success as hardware support.
Do not activate MMIO, DMA, IRQs, clocks, power or touch publication here.
No QEMU, payload, firmware, root/Data or device actions.
''', newline='\n')
# Retain the source-cache preparation and official-build orchestration, not outputs.
runner = (ROOT/'analysis/tools/run_host_dt_panic.py').read_text()
runner = runner.replace('host-dt-panic', 'host-geni').replace('host_dt_panic', 'host_geni')
runner = runner.replace('from verify_host_geni import', 'from verify_host_dt_panic import')
runner = runner.replace('dff276535db3ac394f628cec58a718cd46c20ac2', '7d85ede80092050f4d87f72ac7229bb605ce07fb')
runner = runner.replace("assert len(status['patch_order']) == 58", "assert len(status['patch_order']) == 59")
runner = runner.replace("[digest(x) for x in status['patch_order']] ==", "[digest(x) for x in status['patch_order'][:58]] ==")
runner = runner.replace("assert status['board']['kernel']['archives'] == expected['board']['kernel']['archives']", "assert status['board']['kernel']['archives'] == expected['board']['kernel']['archives'] + ['build-j716f-geni.py']")
runner = runner.replace("assert len(status['board']['kernel']['archives']) == 8", "assert len(status['board']['kernel']['archives']) == 9")
runner = runner.replace("compare_configuration(Path(status['config_path']).read_text(), Path(expected['config_path']).read_text())", "compare_configuration(Path(status['config_path']).read_text().replace('CONFIG_XNU_EXTRA_LD_ARCHIVES=\"-Wl,-force_load,{tree}/build/j716f-geni/libJ716fGeniBackend.a\"', 'CONFIG_XNU_EXTRA_LD_ARCHIVES=\"\"'), Path(expected['config_path']).read_text())")
runner = runner.replace("'patches': 58, 'archive_recipes': 8", "'patches': 59, 'archive_recipes': 9")
(ROOT/'analysis/tools/run_host_geni.py').write_text(runner, newline='\n')
print(target)
