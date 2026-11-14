"""Wire a separately named seven-TU storage archive into official kernel builds."""
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[3]
DEVICE = ROOT / 'work/UniXNU-j716f-host-storage-r30'
OLD = ROOT / 'work/UniXNU-j716f-host-ufs-observation'
PLATFORM = 'socs/sm8250/kits/Sm8250Platform'

for folder in (PLATFORM, 'drivers/ShengUfs'):
    (DEVICE / folder).mkdir(parents=True, exist_ok=True)
    for src in (OLD / folder).iterdir():
        if src.is_file() and src.suffix in ('.c', '.cpp', '.h'):
            dst = DEVICE / folder / src.name
            if dst.exists() and dst.read_bytes() != src.read_bytes():
                if dst.read_text() == src.read_text():
                    continue  # retain the parent bytes when only line endings differ
                raise ValueError('Source overlap differs: ' + str(dst))
            shutil.copyfile(src, dst)

board = DEVICE / 'board.toml'
text = board.read_text()
text = text.replace('# Eight archive recipes and prelink ABI settings are inherited unchanged.', '''# The eight original recipes remain ordered before the independent storage archive.
archives = [
  "build-corecrypto-14.py",
  "build-corecrypto-14.py --namespace",
  "component:ArmDTPlatform",
  "component:ArmCpu",
  "component:SoftwareAES",
  "build-software-aes-bindings.py",
  "build-mockfs-fixture.py",
  "build-libpthread-kernel.py",
  "build-j716f-storage-r30.py",
]''')
board.write_text(text, newline='\n')
with (DEVICE / 'feeds.toml').open('a') as stream:
    stream.write('''
# Explicit local component: consuming board supplies the archive recipe.
[[source]]
name = "J716fStorageBringup"
kind = "driver"
local = "../J716fStorageBringup-r30"
''')
with (DEVICE / 'configs/j716f_host_storage_r30_defconfig').open('a') as stream:
    stream.write('''
# Five frozen SM8250 TUs plus two board transaction/logging TUs; one owner.
CONFIG_XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,{tree}/build/j716f-storage-r30/libJ716fStorageBringup.a"
''')

recipe = (OLD / 'scripts/build-sm8250-ufs-adapter.py').read_text()
recipe = recipe.replace('Build the frozen SM8250 adapter', 'Build the independent r30 storage integration')
recipe = recipe.replace("'AppsSmmuOwner'))", "'AppsSmmuOwner')) + (\n    'component:J716fStorageBringup/J716fStorageBringup.cpp',\n    'component:J716fStorageBringup/J716fStorageKernel.cpp',\n)")
recipe = recipe.replace("'status', 'observe'))", "'status', 'observe'))\nBRINGUP_EXPORTS = tuple('_j716f_storage_bringup_' + name for name in (\n    'init', 'status', 'observe', 'allocate', 'activate', 'read_known', 'stop', 'discard_unused'))")
recipe = recipe.replace('ufs-observation-inputs.json', 'storage-inputs.json')
recipe = recipe.replace('exactly five translation units', 'exactly seven translation units')
recipe = recipe.replace("required = set(SOURCES) | {", "required = {source for source in SOURCES if not source.startswith('component:')} | {")
recipe = recipe.replace('build-sm8250-ufs-adapter.py', 'build-j716f-storage-r30.py')
recipe = recipe.replace('j716f_host_ufs_observation_defconfig', 'j716f_host_storage_r30_defconfig')
recipe = recipe.replace('sm8250-ufs-adapter-init.patch', 'j716f-storage-init.patch')
recipe = recipe.replace('sm8250-ufs-adapter-dependency.patch', 'j716f-storage-dependency.patch')
recipe = recipe.replace("    return {'manifest_sha256': digest(path), 'files': seen}", '''    component = (device.parent / 'J716fStorageBringup-r30').resolve(strict=True)
    if (ROOT / 'drivers/J716fStorageBringup').resolve(strict=True) != component:
        raise ValueError('Official tree resolved the wrong storage component')
    component_entries = manifest['component_files']
    if not {'J716fStorageBringup.h', 'J716fStorageBringup.cpp', 'J716fStorageKernel.cpp'} <= set(component_entries):
        raise ValueError('Incomplete storage component manifest')
    for relative, expected in component_entries.items():
        rel = Path(relative)
        file = component / rel
        if rel.is_absolute() or '..' in rel.parts or file.is_symlink() or not file.is_file():
            raise ValueError('Invalid component input: ' + relative)
        if not file.resolve().is_relative_to(component):
            raise ValueError('Component input escapes its root')
        actual = {'bytes': file.stat().st_size, 'sha256': digest(file)}
        if actual != expected:
            raise ValueError('Component input mismatch: ' + relative)
    for file in component.rglob('*'):
        if file.is_file() and file.suffix in ('.h', '.hpp', '.c', '.cpp'):
            if file.relative_to(component).as_posix() not in component_entries:
                raise ValueError('Unfrozen component source/header: ' + str(file))
    return {'manifest_sha256': digest(path), 'files': seen, 'component_files': component_entries}''')
recipe = recipe.replace('UniXNU-j716f-host-ufs-observation', 'UniXNU-j716f-host-storage-r30')
recipe = recipe.replace('build/sm8250-ufs-adapter', 'build/j716f-storage-r30')
recipe = recipe.replace("ROOT / '.cache/xnu-work/osfmk']", "ROOT / '.cache/xnu-work/osfmk', ROOT / 'drivers/J716fStorageBringup']")
recipe = recipe.replace("        source = device / relative", "        source = (ROOT / 'drivers' / relative.removeprefix('component:')\n                  if relative.startswith('component:') else device / relative)")
recipe = recipe.replace('libSm8250UfsAdapter', 'libJ716fStorageBringup')
recipe = recipe.replace('exact five ordered object members', 'exact seven ordered object members')
recipe = recipe.replace('exactly the five new adapter objects', 'exactly the seven new storage objects')
recipe = recipe.replace('OWNER_EXPORTS + RUNTIME_EXPORTS', 'OWNER_EXPORTS + RUNTIME_EXPORTS + BRINGUP_EXPORTS')
recipe = recipe.replace('frozen SM8250 adapter, five fresh', 'r30 storage integration, seven fresh')
(DEVICE / 'scripts/build-j716f-storage-r30.py').write_text(recipe, newline='\n')

init_patch = '''--- a/bsd/kern/bsd_init.c
+++ b/bsd/kern/bsd_init.c
@@ -993,6 +993,8 @@
 \tnet_init_run();
 #endif /* NETWORKING */
 
+\tprintf("[J716F boot] BSD pre-root reached; inittodr next; root lookup not reached\\n");
+
 \tbsd_init_kprintf("calling inittodr\\n");
 \tinittodr(0);
 
@@ -1302,6 +1304,16 @@ bsd_autoconf(void)
 \textern void native_legacy_display_late_probe(void);
 \tnative_legacy_display_late_probe();
 #endif
+\t/* Interface registration only. Live ownership/MMIO/DMA evidence must be
+\t * supplied separately; a successful IOKitBSDInit return is not a grant.
+\t */
+\tprintf("[J716F boot] IOKitBSDInit returned result=0x%x\\n", (unsigned int)result);
+\tif (result == KERN_SUCCESS) {
+\t\textern void j716f_storage_bringup_init(void);
+\t\tj716f_storage_bringup_init();
+\t} else {
+\t\tprintf("[J716F storage] registration skipped; IOKitBSDInit failed\\n");
+\t}
 \treturn result;
 }
 
'''
(DEVICE / 'patches/xnu/j716f-storage-init.patch').write_text(init_patch, newline='\n')
dependency = (OLD / 'patches/xnu/sm8250-ufs-adapter-dependency.patch').read_text()
dependency = dependency.replace('J716F_UFS_ADAPTER_ARCHIVE', 'J716F_STORAGE_ARCHIVE')
dependency = dependency.replace('sm8250-ufs-adapter/libSm8250UfsAdapter.a', 'j716f-storage-r30/libJ716fStorageBringup.a')
(DEVICE / 'patches/xnu/j716f-storage-dependency.patch').write_text(dependency, newline='\n')
with (DEVICE / 'patches/xnu/series').open('a') as stream:
    stream.write('j716f-storage-init.patch\nj716f-storage-dependency.patch\n')

print('Integrated seven-TU recipe and two source patches; input freeze pending.')
