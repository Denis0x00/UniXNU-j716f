#!/usr/bin/env python3
"""Build NativeAdrenoMetal.bundle, owner-trust.bundle, and gpu-addon-clients.bin (NGPUADD2) for TB-J716F.

Runs inside the assembled UniXNU build tree (out/j716f/.../tree) as part of [kernel] archives,
producing:
  - build/adreno-metal-plugin/NativeAdrenoMetal.bundle
  - build/adreno-metal-plugin/trust.json
  - build/adreno-metal-plugin/owner-trust.bundle
  - build/adreno-metal-plugin/gpu-addon-clients.bin
"""
import hashlib
import json
import os
import plistlib
import re
import shutil
import struct
import subprocess
import sys
import uuid
import zlib
from pathlib import Path

ROOT = Path('.').resolve()
PKG_REPO = next(
    (
        p for p in (
            ([Path(os.environ['UNIXNU_DEVICE'])] if os.environ.get('UNIXNU_DEVICE') else [])
            + [Path(__file__).resolve().parents[1], Path('/home/test/tb-j716f-unixnu/sources/UniXNU-j716f')]
        )
        if (p / 'board.toml').is_file()
    ),
    ROOT,
)
UNIXNU_REPO = next(
    (p for p in [ROOT, Path('/home/test/tb-j716f-unixnu/sources/UniXNU')] if (p / 'kits/AdrenoPM4').exists()),
    ROOT,
)
GPU_WORK = Path(os.environ.get('UNIXNU_GPU_WORK', '/home/test/tb-j716f-unixnu/gpu-userspace-work'))
CLIENTS_DEFAULT = os.environ.get('UNIXNU_GPU_CLIENTS', 'iconservicesagent,Preferences')


def ensure_link(target: Path, link: Path) -> None:
    if not target.exists():
        return
    if link.is_symlink() or link.exists():
        if link.resolve() == target.resolve():
            return
        if link.is_symlink() or link.is_file():
            link.unlink()
        else:
            return
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(target.resolve())


def stage_inputs() -> None:
    ensure_link(PKG_REPO / 'drivers/Adreno650', ROOT / 'drivers/Adreno650')
    ensure_link(PKG_REPO / 'drivers/AdrenoMetal', ROOT / 'drivers/AdrenoMetal')
    ensure_link(UNIXNU_REPO / 'kits/AdrenoPM4', ROOT / 'kits/AdrenoPM4')
    if GPU_WORK.is_dir():
        for rel in ('.cache/mesa-work', 'build/mesa-tu-ios', 'build/adreno-metal-bridge', 'build/m2v-target'):
            ensure_link(GPU_WORK / rel, ROOT / rel)

    orig_trust = ROOT / 'build/original-static-system-trust/original-static-system.bundle'
    if not orig_trust.is_file():
        candidates = [
            UNIXNU_REPO / 'out/ipsw/21H461/trustcache.bundle',
            GPU_WORK / 'build/original-static-system-trust/original-static-system.bundle',
        ]
        src = next((c for c in candidates if c.is_file()), None)
        if src is not None:
            orig_trust.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, orig_trust)


def valid_like_the_loader(data: bytes) -> bool:
    count = struct.unpack_from('<I', data)[0]
    if not 0 < count <= 128:
        return False
    for index in range(count):
        start = struct.unpack_from('<I', data, 4 + 4 * index)[0]
        end = struct.unpack_from('<I', data, 8 + 4 * index)[0] if index + 1 < count else len(data)
        if start < 4 + 4 * count or start & 3 or end > len(data) or end - start < 24 or struct.unpack_from('<I', data, start)[0] != 2:
            return False
        entries = struct.unpack_from('<I', data, start + 20)[0]
        if end - start != 24 + entries * 24:
            return False
        for entry in range(entries):
            at = start + 24 + entry * 24
            if not 1 <= data[at + 20] <= 4 or (entry and data[at - 24:at - 4] >= data[at:at + 20]):
                return False
    return True


def pad8(data: bytes) -> bytes:
    return data + b'\0' * ((8 - len(data) % 8) % 8)


def main() -> int:
    stage_inputs()

    adreno_dir = PKG_REPO / 'drivers/Adreno650' if (PKG_REPO / 'drivers/Adreno650').is_dir() else (UNIXNU_REPO / 'drivers/Adreno740')
    metal_dir = PKG_REPO / 'drivers/AdrenoMetal' if (PKG_REPO / 'drivers/AdrenoMetal').is_dir() else (ROOT / 'drivers/AdrenoMetal')
    pm4_dir = ROOT / 'kits/AdrenoPM4' if (ROOT / 'kits/AdrenoPM4').is_dir() else (UNIXNU_REPO / 'kits/AdrenoPM4')
    vulkan_include = ROOT / '.cache/mesa-work/include'
    turnip = ROOT / 'build/mesa-tu-ios/src/freedreno/vulkan/libvulkan_freedreno.dylib'
    bridge = ROOT / 'build/adreno-metal-bridge/ios/libadreno_metal_bridge.a'
    orig_trust_path = ROOT / 'build/original-static-system-trust/original-static-system.bundle'

    for required, label in (
        (turnip, 'Turnip libvulkan_freedreno.dylib'),
        (bridge, 'libadreno_metal_bridge.a'),
        (vulkan_include, 'Vulkan headers (.cache/mesa-work/include)'),
        (orig_trust_path, 'original-static-system.bundle'),
    ):
        if not required.exists():
            print(f'SKIP build-j716f-gpu-addon: missing {label} at {required}')
            return 0

    gen_shaders = next(
        (p for p in [ROOT / 'scripts/gen-turnip-smoke-shaders.py', UNIXNU_REPO / 'scripts/gen-turnip-smoke-shaders.py'] if p.is_file()),
        None,
    )
    if gen_shaders is not None:
        subprocess.run(['python3', str(gen_shaders)], check=True)
    (ROOT / 'build/turnip-smoke/TurnipReplay.h').unlink(missing_ok=True)

    out = ROOT / 'build/adreno-metal-plugin'
    bundle = out / 'NativeAdrenoMetal.bundle'
    if out.exists():
        shutil.rmtree(out)
    bundle.mkdir(parents=True)

    if Path('/opt/unixnu-linux/ios-user-bin').is_dir():
        os.environ['XNU_TOOLCHAIN_BIN'] = (
            '/opt/unixnu-linux/ios-user-bin '
            '/opt/unixnu-linux/CommandLineTools/usr/bin '
            '/opt/unixnu-linux/XcodeDefault.xctoolchain/usr/bin'
        )
        os.environ['PATH'] = f"/usr/local/bin:/opt/unixnu-linux/ios-user-bin:{os.environ.get('PATH', '')}"
    sdk = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--show-sdk-path'], text=True).strip()
    if not (Path(sdk) / 'System/Library/Frameworks/Foundation.framework').exists():
        linux_sdks = sorted(Path('/opt/unixnu-linux/CommandLineTools/SDKs').glob('iPhoneOS*.sdk'))
        if linux_sdks:
            sdk = str(linux_sdks[-1])
            os.environ['UNIXNU_SDKROOT'] = sdk
    os.environ['CROSS_EXTRA_FLAGS'] = ''
    strip_tool = subprocess.check_output(['xcrun', '--sdk', 'iphoneos', '--find', 'strip'], text=True).strip()
    base = [
        'xcrun', 'clang', '-target', 'arm64-apple-ios17.0',
        '-isysroot', sdk, '-O2', '-g', '-Wall', '-Wextra', '-Werror',
        '-fstack-protector-strong',
    ]

    source_hash = hashlib.sha256()
    for path in sorted(metal_dir.rglob('*')):
        if path.is_file() and '.git' not in path.parts and path.suffix in ('.h', '.mm', '.cpp', '.rs', '.patch'):
            source_hash.update(str(path.relative_to(metal_dir)).encode())
            source_hash.update(path.read_bytes())
    metal_build_id = source_hash.hexdigest()
    base += [f'-DNATIVE_METAL_BUILD_ID="{metal_build_id}"']
    (bundle / 'gpu-build.json').write_text(json.dumps({'metal_source_sha256': metal_build_id, 'target': 'ios'}, indent=2) + '\n')

    pm4 = out / 'AdrenoPM4.o'
    subprocess.run([*base, '-std=c11', '-I', str(pm4_dir), '-c', str(pm4_dir / 'AdrenoPM4.c'), '-o', str(pm4)], check=True)

    exe = bundle / 'NativeAdrenoMetal'
    prime_list = ROOT / 'build/prime-list.txt'
    prime_section = [f'-Wl,-sectcreate,__DATA,__primelist,{prime_list}'] if prime_list.is_file() else []
    subprocess.run([
        *(['xcrun', 'clang++'] + base[2:]), '-std=c++17', '-fobjc-arc', '-Wno-deprecated-declarations', '-bundle',
        '-I', str(adreno_dir), '-I', str(pm4_dir), '-I', str(vulkan_include), '-I', str(ROOT / 'build/turnip-smoke'),
        '-x', 'objective-c++', str(metal_dir / 'NativeAdrenoMetalPlugin.mm'),
        str(metal_dir / 'NativeBridgeMetalFrontend.mm'), '-x', 'none', str(pm4), str(bridge),
        '-Wl,-dead_strip', *prime_section, '-lobjc', '-framework', 'Foundation', '-framework', 'Metal', '-framework', 'IOSurface',
        '-o', str(exe),
    ], check=True)

    subprocess.run([strip_tool, '-x', str(exe)], check=True)
    symbols = out / 'NativeAdrenoMetal.dSYM'
    shutil.rmtree(symbols, ignore_errors=True)
    if (bundle / 'NativeAdrenoMetal.dSYM').exists():
        shutil.move(bundle / 'NativeAdrenoMetal.dSYM', symbols)

    turnip_lib = bundle / 'libvulkan_freedreno.dylib'
    shutil.copyfile(turnip, turnip_lib)
    subprocess.run([strip_tool, '-x', str(turnip_lib)], check=True)
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.turnip', str(turnip_lib)], check=True)
    turnip_detail = subprocess.run(['codesign', '--display', '--verbose=4', str(turnip_lib)], capture_output=True, text=True, check=True).stderr
    turnip_cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', turnip_detail, re.M)[1]

    (bundle / 'Info.plist').write_bytes(plistlib.dumps(dict(
        CFBundleIdentifier='org.blu-sharky.NativeAdrenoMetal', CFBundleExecutable='NativeAdrenoMetal', CFBundlePackageType='BNDL',
        CFBundleVersion='1', CFBundleShortVersionString='1', NSPrincipalClass='NativeAdrenoMetalDevice',
        MinimumOSVersion='17.0'), fmt=plistlib.FMT_XML))
    subprocess.run(['codesign', '--force', '--sign', '-', '--pagesize', '4096', '--identifier', 'org.blu-sharky.NativeAdrenoMetal', str(bundle)], check=True)
    subprocess.run(['codesign', '--verify', '--strict', str(bundle)], check=True)
    detail = subprocess.run(['codesign', '--display', '--verbose=4', str(bundle)], capture_output=True, text=True, check=True).stderr
    cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', detail, re.M)[1]

    entitlements = out / 'adrenoprobed-entitlements.plist'
    entitlements.write_bytes(plistlib.dumps({
        'application-identifier': 'org.blu-sharky.adrenoprobed',
        'platform-application': True,
        'com.apple.private.security.no-container': True,
        'com.apple.private.security.storage.AppInstallation': True,
        'com.apple.private.coreservices.canmaplsdatabase': True,
        'com.apple.private.installcoordinationd.client': True,
        'com.apple.security.iokit-user-client-class': ['AGXDeviceUserClient'],
        'com.apple.private.logging.admin': True,
        'com.apple.private.logging.diagnostic': True,
        'com.apple.private.logging.stream': True,
        'com.apple.private.mobileinstall.allowedSPI': [
            'Install',
            'InstallForLaunchServices',
            'InstallForInstallCoordination',
            'Lookup',
            'Uninstall',
            'UninstallForLaunchServices',
        ],
        'com.apple.security.exception.mach-lookup.global-name': [
            'com.apple.mobile.installd',
            'com.apple.installcoordinationd',
            'com.apple.lsd.mapdb',
            'com.apple.lsd.modifydb',
            'com.apple.frontboard.systemappservices',
        ],
        'com.apple.springboard.launchapplications': True,
        'com.apple.frontboard.launchapplications': True,
        'com.apple.springboard.opensensitiveurl': True,
        'com.apple.frontboard.opensensitiveurl': True,
    }))
    daemon = out / 'adrenoprobed'
    subprocess.run([
        'xcrun', 'clang++', *base[2:], '-std=c++17',
        '-I', str(adreno_dir), '-I', str(pm4_dir), '-I', str(vulkan_include), '-I', str(ROOT / 'build/turnip-smoke'),
        str(metal_dir / 'NativeAdrenoProbeDaemon.cpp'), '-x', 'objective-c++', '-fobjc-arc',
        str(metal_dir / 'NativeOsLogExport.mm'), '-x', 'none', str(pm4), '-framework', 'Foundation', '-lz',
        '-o', str(daemon),
    ], check=True)
    subprocess.run([
        'codesign', '--force', '--sign', '-', '--pagesize', '4096',
        '--identifier', 'org.blu-sharky.adrenoprobed', '--entitlements', str(entitlements), str(daemon),
    ], check=True)
    subprocess.run(['codesign', '--verify', '--strict', str(daemon)], check=True)
    daemon_detail = subprocess.run(['codesign', '--display', '--verbose=4', str(daemon)], capture_output=True, text=True, check=True).stderr
    daemon_cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', daemon_detail, re.M)[1]

    (out / 'org.blu-sharky.adrenoprobed.plist').write_bytes(plistlib.dumps(dict(
        Label='org.blu-sharky.adrenoprobed', ProgramArguments=['/private/var/ngpu/bin/adrenoprobed'], RunAtLoad=True,
        KeepAlive=False, ThrottleInterval=30, UserName='root', StandardErrorPath='/dev/null'), fmt=plistlib.FMT_XML))
    (out / 'com.apple.iomfb_fdr_loader.plist').write_bytes(plistlib.dumps(dict(
        Label='com.apple.iomfb_fdr_loader', ProgramArguments=['/private/var/ngpu/bin/adrenoprobed'], RunAtLoad=True,
        KeepAlive=False, LimitLoadToSessionType='System', ProcessType='Interactive', StandardErrorPath='/dev/null'), fmt=plistlib.FMT_XML))

    bt_dir = GPU_WORK / 'build/bluetooth-userland'
    wifi_dir = GPU_WORK / 'build/wifi-userland'
    appsync_dir = GPU_WORK / 'build/appsync-userland'
    extra_cdhashes = []
    extra_files = []

    if (bt_dir / 'bluetoothd').is_file() and (bt_dir / 'BlueTool').is_file():
        for name in ('bluetoothd', 'BlueTool'):
            bin_path = bt_dir / name
            bin_detail = subprocess.run(['codesign', '--display', '--verbose=4', str(bin_path)], capture_output=True, text=True, check=True).stderr
            bin_cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', bin_detail, re.M)[1]
            extra_cdhashes.append(bin_cdhash)
            extra_files.append(('bluetooth-bin/' + name, 0o755, bin_path.read_bytes()))
        shutil.copyfile(bt_dir / 'com.apple.bluetoothd.plist', out / 'com.apple.bluetoothd.plist')
        extra_files.append(('daemons/com.apple.BlueTool.plist', 0o644, (bt_dir / 'com.apple.BlueTool.plist').read_bytes()))
        extra_files.append(('bluetooth-bin/IOMFB_FDR_Loader', 0o755, daemon.read_bytes()))
    else:
        (out / 'com.apple.bluetoothd.plist').write_bytes(plistlib.dumps(dict(
            Label='com.apple.bluetoothd', ProgramArguments=['/private/var/ngpu/bin/adrenoprobed'], RunAtLoad=True, KeepAlive=False,
            ProcessType='Interactive', StandardErrorPath='/dev/null'), fmt=plistlib.FMT_XML))

    if (wifi_dir / 'unixnu-wpa_supplicant').is_file():
        wpa_path = wifi_dir / 'unixnu-wpa_supplicant'
        wpa_detail = subprocess.run(['codesign', '--display', '--verbose=4', str(wpa_path)], capture_output=True, text=True, check=True).stderr
        wpa_cdhash = re.search(r'^CDHash=([a-f0-9]{40})$', wpa_detail, re.M)[1]
        extra_cdhashes.append(wpa_cdhash)
        extra_files.append(('wifi-bin/unixnu-wpa_supplicant', 0o755, wpa_path.read_bytes()))
        extra_files.append(('etc/wifi/wpa_supplicant.conf', 0o644, (wifi_dir / 'wpa_supplicant.conf').read_bytes()))
        if os.environ.get('UNIXNU_WIFI_DAEMON_PLIST', '1') != '0' and (wifi_dir / 'org.unixnu.wifi.supplicant.plist').is_file():
            extra_files.append(('daemons/org.unixnu.wifi.supplicant.plist', 0o644, (wifi_dir / 'org.unixnu.wifi.supplicant.plist').read_bytes()))

    if (appsync_dir / 'lib/UniXNUAppSync.dylib').is_file():
        appsync_src = UNIXNU_REPO / 'scripts/appsync'
        dylib_out = appsync_dir / 'lib/UniXNUAppSync.dylib'
        if (appsync_src / 'AppSync.m').is_file():
            subprocess.run([
                'xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk,
                '-fobjc-arc', '-O2', '-Wall', '-Wextra', '-Werror',
                '-dynamiclib',
                str(appsync_src / 'AppSync.m'),
                str(appsync_src / 'Signing.m'),
                str(appsync_src / 'BacklightMode.m'),
                '-install_name', '/usr/lib/UniXNUAppSync.dylib',
                '-framework', 'Foundation',
                '-Wl,-compatibility_version,300.0.0', '-Wl,-current_version,2503.1.0',
                '-o', str(dylib_out),
            ], check=True)
            dbytes = bytearray(dylib_out.read_bytes())
            _, _, _, _, ncmds, _, mflags, _ = struct.unpack_from('<8I', dbytes, 0)
            struct.pack_into('<I', dbytes, 24, mflags & ~0x00100000)
            at = 32
            for _ in range(ncmds):
                cmd_id, cmd_len = struct.unpack_from('<II', dbytes, at)
                if cmd_id == 0xC:
                    name_off = struct.unpack_from('<I', dbytes, at + 8)[0]
                    name = bytes(dbytes[at + name_off:at + cmd_len]).split(b'\0', 1)[0]
                    if name == b'/System/Library/Frameworks/Foundation.framework/Foundation':
                        struct.pack_into('<I', dbytes, at, 0x8000001F)
                at += cmd_len
            dylib_out.write_bytes(bytes(dbytes))
            subprocess.run([
                'codesign', '--force', '--sign', '-', '--pagesize', '4096',
                '--identifier', 'org.unixnu.appsync', str(dylib_out),
            ], check=True)
            subprocess.run(['codesign', '--verify', '--strict', str(dylib_out)], check=True)
        if (appsync_src / 'appinst.m').is_file():
            appinst_out = appsync_dir / 'libexec/unixnu-appinst'
            subprocess.run([
                'xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-isysroot', sdk,
                '-fobjc-arc', '-O2', '-Wall', '-Wextra', '-Werror',
                str(appsync_src / 'appinst.m'),
                '-framework', 'Foundation',
                '-o', str(appinst_out),
            ], check=True)
            appinst_ent = out / 'appinst-entitlements.plist'
            appinst_ent.write_bytes(plistlib.dumps({
                'application-identifier': 'org.unixnu.appinst',
                'platform-application': True,
                'com.apple.private.security.no-container': True,
                'com.apple.private.security.storage.AppInstallation': True,
                'com.apple.private.coreservices.canmaplsdatabase': True,
                'com.apple.private.installcoordinationd.client': True,
                'com.apple.private.mobileinstall.allowedSPI': [
                    'Install',
                    'InstallForLaunchServices',
                    'InstallForInstallCoordination',
                    'Lookup',
                    'Uninstall',
                    'UninstallForLaunchServices',
                ],
                'com.apple.security.exception.mach-lookup.global-name': [
                    'com.apple.mobile.installd',
                    'com.apple.installcoordinationd',
                    'com.apple.lsd.mapdb',
                    'com.apple.lsd.modifydb',
                    'com.apple.frontboard.systemappservices',
                ],
                'com.apple.springboard.launchapplications': True,
                'com.apple.frontboard.launchapplications': True,
            }))
            subprocess.run([
                'codesign', '--force', '--sign', '-', '--pagesize', '4096',
                '--identifier', 'org.unixnu.appinst', '--entitlements', str(appinst_ent), str(appinst_out),
            ], check=True)
            subprocess.run(['codesign', '--verify', '--strict', str(appinst_out)], check=True)
        appsync_bins = (
            'lib/UniXNUAppSync.dylib',
            'lib/libunixnu-ldid.dylib',
            'libexec/unixnu-ldid',
            'libexec/unixnu-appinst',
            'libexec/installd',
            'libexec/backboardd',
            'helper/com.apple.MobileInstallationHelperService',
            'coreservices/SpringBoard.app/SpringBoard',
        )
        for rel in appsync_bins:
            bpath = appsync_dir / rel
            if not bpath.is_file():
                continue
            bdetail = subprocess.run(['codesign', '--display', '--verbose=4', str(bpath)], capture_output=True, text=True, check=True).stderr
            bcdhash = re.search(r'^CDHash=([a-f0-9]{40})$', bdetail, re.M)[1]
            extra_cdhashes.append(bcdhash)
        for subdir in ('lib', 'libexec', 'helper', 'coreservices'):
            sdir = appsync_dir / subdir
            if not sdir.is_dir():
                continue
            for p in sorted(sdir.rglob('*')):
                if not p.is_file():
                    continue
                rel = p.relative_to(appsync_dir).as_posix()
                mode = 0o755 if (rel in appsync_bins or p.name == 'SpringBoardDiagnosticExtension') else 0o644
                extra_files.append(('appsync/' + rel, mode, p.read_bytes()))
        if (wifi_dir / 'unixnu-wpa_supplicant').is_file():
            extra_files.append(('appsync/libexec/unixnu-wpa_supplicant', 0o755, (wifi_dir / 'unixnu-wpa_supplicant').read_bytes()))
        extra_files.append(('appsync/libexec/IOMFB_FDR_Loader', 0o755, daemon.read_bytes()))
        extra_files.append(('appsync/libexec/mtmergeprops', 0o755, daemon.read_bytes()))
        extra_files.append(('appsync/libexec/misd', 0o755, daemon.read_bytes()))
        for ipa_cand in (
            Path('/tmp/unixnu_test_ipa/UniXNUTest.ipa'),
            appsync_dir / 'autoinstall/UniXNUTest.ipa',
            PKG_REPO / 'scripts/UniXNUTest.ipa',
        ):
            if ipa_cand.is_file():
                extra_files.append(('appsync/autoinstall/UniXNUTest.ipa', 0o644, ipa_cand.read_bytes()))
                break

    trust_data = {
        'cdhash': cdhash,
        'daemon_cdhash': daemon_cdhash,
        'turnip_cdhash': turnip_cdhash,
        'extra_cdhashes': extra_cdhashes,
        'bundle': 'NativeAdrenoMetal.bundle',
        'executable_bytes': exe.stat().st_size,
        'purpose': 'owner-built plugin and probe daemon; trusted only through an extra owner trust module',
    }
    (out / 'trust.json').write_text(json.dumps(trust_data, indent=2) + '\n')

    # Build owner-trust.bundle (own module + both original Apple trust modules)
    original = orig_trust_path.read_bytes()
    if hashlib.sha256(original).hexdigest() != '7416ca4f8377146626bb284e2133c76bee6cb4e3ee0af5d648b186235a7323a7':
        raise SystemExit('The original static/System trust bundle differs from the unchanged target input')
    count = struct.unpack_from('<I', original)[0]
    assert count == 2
    offsets = struct.unpack_from('<2I', original, 4)
    originals = [original[offsets[0]:offsets[1]], original[offsets[1]:]]
    hashes = sorted({bytes.fromhex(cdhash), bytes.fromhex(daemon_cdhash), bytes.fromhex(turnip_cdhash), *(bytes.fromhex(h) for h in extra_cdhashes)})
    identifier = uuid.uuid5(uuid.NAMESPACE_URL, 'org.blu-sharky.adreno-metal:' + ''.join(h.hex() for h in hashes))
    own = struct.pack('<I16sI', 2, identifier.bytes, len(hashes)) + b''.join(h + bytes([2, 0, 0, 0]) for h in hashes)
    modules = [own, *originals]
    cursor, starts = 16, []
    for mod in modules:
        starts.append(cursor)
        cursor += len(mod)
    trust_bundle = struct.pack('<4I', 3, *starts) + b''.join(modules)
    if not valid_like_the_loader(trust_bundle):
        raise SystemExit('The trust bundle would be refused by the boot loader')
    owner_trust_out = out / 'owner-trust.bundle'
    owner_trust_out.write_bytes(trust_bundle)
    (out / 'owner-trust.json').write_text(json.dumps({
        'bundle_sha256': hashlib.sha256(trust_bundle).hexdigest(),
        'own_cdhashes': [h.hex() for h in hashes],
        'original_modules_unchanged': True,
        'apple_trust_chain_verified': False,
        'purpose': 'owner trust for the Adreno Metal plugin only',
    }, indent=2) + '\n')

    # Build gpu-addon-clients.bin (NGPUADD2)
    files = []
    for path in sorted(bundle.rglob('*')):
        if not path.is_file():
            continue
        mode = 0o755 if path.name in ('NativeAdrenoMetal', 'libvulkan_freedreno.dylib') else 0o644
        files.append(('addons/NativeAdrenoMetal.bundle/' + path.relative_to(bundle).as_posix(), mode, path.read_bytes()))
    files.append(('addons/NativeAdrenoMetal.publish', 0o644, b'1\n'))
    if CLIENTS_DEFAULT:
        files.append(('addons/NativeAdrenoMetal.clients', 0o644, (CLIENTS_DEFAULT + '\n').encode()))
    files.append(('bin/adrenoprobed', 0o755, daemon.read_bytes()))
    for plist in ('com.apple.bluetoothd.plist', 'com.apple.iomfb_fdr_loader.plist', 'org.blu-sharky.adrenoprobed.plist'):
        files.append(('daemons/' + plist, 0o644, (out / plist).read_bytes()))
    files.extend(extra_files)

    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + pad8(encoded) + pad8(packed)
    addon_blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
    addon_out = out / 'gpu-addon-clients.bin'
    addon_out.write_bytes(addon_blob)

    print(
        f'PASS: NativeAdrenoMetal CDHash={cdhash} Turnip={turnip_cdhash} '
        f'owner-trust={hashlib.sha256(trust_bundle).hexdigest()[:12]} '
        f'gpu-addon={addon_out} ({len(addon_blob)} bytes)'
    )
    return 0


if __name__ == '__main__':
    sys.exit(main())
