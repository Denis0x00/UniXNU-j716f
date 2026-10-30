#!/usr/bin/env python3
"""100% git-free builder and binary roundtrip auditor for J716F NATIVE FAST-V1.

Preserves:
  - Prelinked native XNU kernel 150c5c5f13a4337eb05218232f3a157429f0974ba49f71b4627789497132d074
  - Audited hardware ADT 806ad48c85881b51f6acbeeea7ff4ec065f9edbe32d90f4aa304b5046faa64a3
  - Logger candidate identity 1c19ee51d5388dcfbbaefd41dcf361a54ccee3d08d889f6f0e4bb6f69a08d892
  - Qualified BootShim 72bfc9e9137a0e3ab9f7264162968c546ba63533b94d6813814fe84c831d03b4
  - Firmware DTB 2c0d57deb7f99a081fe4994535c51ca0362bf44b6ad5422a7300f01496f6b3ec
  - FD window 0xce000000 + 0x700000 and RAM reservations 0xb0000000 + 0xd0000000

Replaces ONLY the slow UEFI maintenance tools with package/native-fast/:
  - J716fNativeRead.efi v4 (raw physical scan + stage decoder + 8-QR 4s session export)
  - J716fNativeArm.efi  v4 (3s cold/dirty DBGC clean-init + session ARM + rollover gate)
  - J716fPowerAction.efi   (confirmed PSCI warm restart 0x84000009 / poweroff 0x84000008)
  - Manual-only 6-entry UniXNU menu at readable scale=2

Strictly ZERO git invocations on Windows or Linux.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import os
from pathlib import Path
import posixpath
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tomllib
import zlib

ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT / "github-export/UniXNU-j716f"
sys.path.insert(0, str(REPO / "scripts"))
from native_image_formats import extract_fd, read_fat32  # noqa: E402

BASE_STAGE = ROOT / "debug/native-maintenance-stage-v2"
FAST_SRC = REPO / "package/native-fast"
MAINT_OUT = ROOT / "Firmware/NATIVE-40458123294-MAINT-V2"
MKBOOTIMG = ROOT / "github-export/uefi-j716f/Resources/Scripts/mkbootimg.py"
REMOTE_UEFI = "/home/test/tb-j716f-unixnu/mu-bringup-20261023/devices/uefi-j716f"

EXPECTED_KERNEL_SHA = "150c5c5f13a4337eb05218232f3a157429f0974ba49f71b4627789497132d074"
EXPECTED_ADT_SHA = "806ad48c85881b51f6acbeeea7ff4ec065f9edbe32d90f4aa304b5046faa64a3"
EXPECTED_CANDIDATE_SHA = "1c19ee51d5388dcfbbaefd41dcf361a54ccee3d08d889f6f0e4bb6f69a08d892"
EXPECTED_SHIM_SHA = "72bfc9e9137a0e3ab9f7264162968c546ba63533b94d6813814fe84c831d03b4"
EXPECTED_DTB_SHA = "2c0d57deb7f99a081fe4994535c51ca0362bf44b6ad5422a7300f01496f6b3ec"
FAT_GUID = "90f40472-1975-40e2-8028-c76d530d15a7"

import importlib.util

def verify_logger_candidate_identity() -> str:
    spec = importlib.util.spec_from_file_location("prepare_native_logger", REPO / "scripts/prepare-native-logger.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    cand, _inputs, header = mod.identity(REPO)
    if cand != EXPECTED_CANDIDATE_SHA:
        raise RuntimeError(f"Logger candidate identity changed! Expected {EXPECTED_CANDIDATE_SHA}, got {cand}")
    staged_hdr = (BASE_STAGE / "J716fNativeLog/NativeCandidate.h").read_text(encoding="utf-8")
    if staged_hdr != header:
        raise RuntimeError("NativeCandidate.h header mismatch against prepare-native-logger.py")
    return cand

FAST_UNIXNU_TOML = r'''version = 1
[boot]
timeout = -1
fallback_timeout = -1
rotate = 0
connect_all = false
default = "reader"
[kits]
autoload = false
[ui]
title = "J716F NATIVE FAST-V1 (XNU 17.7.11 / 40458123294)"
scale = 2
[[entry]]
id = "reader"
title = "1. Read XNU Trace & Fast 8-QR Export (4s, read-only)"
type = "efi"
path = "\\EFI\\Tools\\J716fNativeRead.efi"
[[entry]]
id = "arm"
title = "2. Fast Prepare & ARM XNU Session (3s, cold/dirty safe)"
type = "efi"
path = "\\EFI\\Tools\\J716fNativeArm.efi"
[[entry]]
id = "native"
title = "3. Launch Native XNU 17.7.11 (cpus=1 -v debug=0x14e)"
type = "xnu"
path = "\\Payloads\\kernel.macho"
args = "-v serial=0 debug=0x14e cpus=1 -no_slto_panic"
confirm = "Session ARMED in [2]? Press Power/Enter to boot Native XNU!"
[[entry]]
id = "restart"
title = "4. Warm Restart Tablet (PSCI SYSTEM_RESET -> inspect RAM)"
type = "efi"
path = "\\EFI\\Tools\\J716fPowerAction.efi"
args = "restart"
[[entry]]
id = "shutdown"
title = "5. Power Off Tablet (PSCI SYSTEM_OFF)"
type = "efi"
path = "\\EFI\\Tools\\J716fPowerAction.efi"
args = "shutdown"
[[entry]]
id = "exit"
title = "6. Exit Menu & Halt Firmware"
type = "action"
action = "exit"
'''

NATIVE_LOG_APPS_DSC = """[Defines]
  PLATFORM_NAME = J716fNativeLogApps
  PLATFORM_GUID = da593c7b-de34-47cc-b8a3-0d991d607984
  PLATFORM_VERSION = 1.0
  DSC_SPECIFICATION = 0x00010005
  OUTPUT_DIRECTORY = Build/NativeLogApps
  SUPPORTED_ARCHITECTURES = AARCH64
  BUILD_TARGETS = RELEASE|DEBUG
  SKUID_IDENTIFIER = DEFAULT
[LibraryClasses]
  QrEncoderLib|MsGraphicsPkg/Library/QrEncoderLib/QrEncoderLib.inf
  BaseLib|MdePkg/Library/BaseLib/BaseLib.inf
  ArmLib|MdePkg/Library/ArmLib/ArmBaseLib.inf
  BaseMemoryLib|MdePkg/Library/BaseMemoryLib/BaseMemoryLib.inf
  MemoryAllocationLib|MdePkg/Library/UefiMemoryAllocationLib/UefiMemoryAllocationLib.inf
  DebugLib|MdePkg/Library/BaseDebugLibNull/BaseDebugLibNull.inf
  PrintLib|MdePkg/Library/BasePrintLib/BasePrintLib.inf
  PcdLib|MdePkg/Library/BasePcdLibNull/BasePcdLibNull.inf
  UefiBootServicesTableLib|MdePkg/Library/UefiBootServicesTableLib/UefiBootServicesTableLib.inf
  UefiRuntimeServicesTableLib|MdePkg/Library/UefiRuntimeServicesTableLib/UefiRuntimeServicesTableLib.inf
  UefiLib|MdePkg/Library/UefiLib/UefiLib.inf
  DevicePathLib|MdePkg/Library/UefiDevicePathLib/UefiDevicePathLib.inf
  UefiApplicationEntryPoint|MdePkg/Library/UefiApplicationEntryPoint/UefiApplicationEntryPoint.inf
  RegisterFilterLib|MdePkg/Library/RegisterFilterLibNull/RegisterFilterLibNull.inf
  StackCheckLib|MdePkg/Library/StackCheckLibNull/StackCheckLibNull.inf
[PcdsFixedAtBuild]
  gEfiMdePkgTokenSpaceGuid.PcdDebugPropertyMask|0
[Components]
  J716fNativeLog/Read.inf
  J716fNativeLog/Arm.inf
  J716fNativeLog/Power.inf
"""

PAYLOAD_BUILDER_PY = r'''#!/usr/bin/env python3
"""Build NativePayload.fat for FAST-V1 on Linux build host (mtools)."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

def build(stage: Path) -> None:
    out = stage / "payload"
    out.mkdir(exist_ok=False)
    tree = out / "tree"
    (tree / "EFI/UniXNU").mkdir(parents=True)
    (tree / "EFI/Tools").mkdir()
    (tree / "Payloads").mkdir()
    shutil.copyfile(stage / "unixnu.toml", tree / "EFI/UniXNU/unixnu.toml")
    for name in ("kernel.macho", "kernel.macho.adt"):
        shutil.copyfile(stage / "inputs" / name, tree / "Payloads" / name)
    for name in ("J716fNativeRead.efi", "J716fNativeArm.efi", "J716fPowerAction.efi"):
        shutil.copyfile(stage / "Build/NativeLogApps/RELEASE_CLANGPDB/AARCH64" / name, tree / "EFI/Tools" / name)
    for path in (tree, *tree.rglob("*")):
        os.utime(path, (315532800, 315532800))
    fat = stage / "Platforms/Lenovo/j716fPkg/Resources/NativePayload.fat"
    if fat.exists():
        raise ValueError("Preserve prior FAT")
    env = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    subprocess.run(
        ["mformat", "-C", "-F", "-c", "1", "-T", "131072", "-v", "J716FNATIVE", "-N", "716f1001", "-i", str(fat), "::"],
        check=True,
        env=env,
    )
    for folder in ("EFI", "Payloads"):
        subprocess.run(["mcopy", "-i", str(fat), "-s", "-Q", "-m", str(tree / folder), "::/"], check=True, env=env)
    digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    report = {
        "payload_sha256": digest(fat),
        "payload_bytes": fat.stat().st_size,
        "automatic_start": False,
        "fast_v1": True,
        "files": {
            p.relative_to(tree).as_posix(): {"sha256": digest(p), "bytes": p.stat().st_size}
            for p in tree.rglob("*")
            if p.is_file()
        },
    }
    (out / "payload.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"payload_sha256": report["payload_sha256"], "file_count": len(report["files"])}))

if __name__ == "__main__":
    build(Path(sys.argv[1]))
'''


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def extract_qualified_shim_and_dtb(out_dir: Path) -> tuple[Path, Path]:
    packed = (MAINT_OUT / "Image.gz-dtb").read_bytes()
    dec = zlib.decompressobj(31)
    raw = dec.decompress(packed)
    if not dec.eof:
        raise RuntimeError("Corrupt Image.gz-dtb gzip stream")
    dtb = dec.unused_data
    if len(raw) <= 0x700000:
        raise RuntimeError("Unexpected decompressed FD+BootShim size")
    shim = raw[:-0x700000]
    if sha(shim) != EXPECTED_SHIM_SHA:
        raise RuntimeError(f"BootShim SHA256 mismatch: {sha(shim)}")
    if sha(dtb) != EXPECTED_DTB_SHA:
        raise RuntimeError(f"Firmware DTB SHA256 mismatch: {sha(dtb)}")
    out_dir.mkdir(parents=True, exist_ok=True)
    shim_path = out_dir / "BootShim.bin"
    dtb_path = out_dir / "dtb.bin"
    shim_path.write_bytes(shim)
    dtb_path.write_bytes(dtb)
    return shim_path, dtb_path


def make_menu_guard(toml_text: str) -> str:
    entries = tomllib.loads(toml_text)["entry"]
    types = {"efi": "UniXNUEntryEfi", "xnu": "UniXNUEntryXnu", "action": "UniXNUEntryAction"}
    conditions = [f"Config.Entries[{i}].Type != {types[e['type']]}" for i, e in enumerate(entries)]
    for i, entry in enumerate(entries):
        for key, field in [("path", "Path"), ("args", "Args")]:
            if key in entry:
                val = json.dumps(entry[key])
                conditions.append(f"!Config.Entries[{i}].{field} || StrCmp (Config.Entries[{i}].{field}, L{val})")
        if entry["type"] == "action":
            conditions.append(f"Config.Entries[{i}].Action != UniXNUActionExit")
    ids = ", ".join(json.dumps(e["id"]) for e in entries)
    return f"""  UniXNUConfigLoad (ImageHandle, &Config);
  // Exact explicit FAST-V1 profile; no fallback, injected drivers or automatic launch.
  if (Config.Error[0] || Config.EntryCount != {len(entries)} || Config.DefaultIndex != 0 ||
      Config.Timeout != -1 || Config.DriverCount || Config.PatchCount || Config.AcpiCount ||
      Config.SliceCount || Config.KitsLoaded) {{ return EFI_SECURITY_VIOLATION; }}
  {{
    STATIC CONST CHAR8 *Ids[] = {{{ids}}};
    for (Index = 0; Index < {len(entries)}; Index++) {{
      if (!Config.Entries[Index].Id || AsciiStrCmp (Config.Entries[Index].Id, Ids[Index])) {{
        return EFI_SECURITY_VIOLATION;
      }}
    }}
    if ({' || '.join(conditions)}) {{ return EFI_SECURITY_VIOLATION; }}
  }}"""


def stage_fast_v1(stage: Path) -> dict:
    cand = verify_logger_candidate_identity()
    if stage.exists():
        if (stage / "built/build-result.json").exists():
            raise RuntimeError(f"Stage {stage} already has a completed build; choose a fresh --stage path")
        shutil.rmtree(stage)
    stage.mkdir(parents=True)

    # Copy base EDK II packages & verified binary inputs from native-maintenance-stage-v2
    for folder in ("Platforms", "package", "DarwinBoot"):
        shutil.copytree(BASE_STAGE / folder, stage / folder)

    # Update version string in j716f-native.dsc so screen header clearly shows FAST-V1
    dsc_path = stage / "Platforms/Lenovo/j716fPkg/j716f-native.dsc"
    dsc_text = dsc_path.read_text(encoding="utf-8")
    dsc_text = dsc_text.replace(
        'L"J716F NATIVE LOG 40458123294"',
        'L"J716F NATIVE FAST-V1 40458123294"',
    )
    dsc_path.write_text(dsc_text, encoding="utf-8", newline="\n")

    # Build NativeMenu from clean upstream UniXNULib + ManualMenu.inc + 6-entry guard
    menu = stage / "NativeMenu"
    shutil.copytree(stage / "package/UniXNUPkg/Library/UniXNULib", menu)
    menu_c = (menu / "Menu.c").read_text(encoding="utf-8")
    start = menu_c.index("STATIC\nVOID\nDrawCountdown (")
    end = menu_c.index("//\n// The Tools submenu", start)
    manual_inc = (REPO / "package/native-log/ManualMenu.inc").read_text(encoding="utf-8")
    menu_c = menu_c[:start] + manual_inc + "\n" + menu_c[end:]
    menu_c = menu_c.replace(
        "Menu loop: input, countdown and idle rotation.",
        "Pinned upstream menu with J716F manual-only loop.",
    )
    (menu / "Menu.c").write_text(menu_c, encoding="utf-8", newline="\n")

    inf_text = (menu / "UniXNULib.inf").read_text(encoding="utf-8")
    inf_text = inf_text.replace("BASE_NAME      = UniXNULib", "BASE_NAME      = J716fNativeUniXNULib", 1)
    (menu / "UniXNULib.inf").write_text(inf_text, encoding="utf-8", newline="\n")

    main_c = (menu / "Main.c").read_text(encoding="utf-8")
    if main_c.count("  UniXNUConfigLoad (ImageHandle, &Config);") != 1:
        raise RuntimeError("Unexpected Main.c structure")
    main_c = main_c.replace("  UniXNUConfigLoad (ImageHandle, &Config);", make_menu_guard(FAST_UNIXNU_TOML), 1)
    (menu / "Main.c").write_text(main_c, encoding="utf-8", newline="\n")

    # Stage J716fNativeLog apps (shared headers + FAST-V1 Read/Arm/Power/Tests)
    app_dir = stage / "J716fNativeLog"
    app_dir.mkdir()
    for shared in ("Common.h", "Panel.h", "Font.c", "Smc.S", "NativeLog.h", "NativeFirmware.h", "NativeCandidate.h", "NativeSha256.h"):
        shutil.copyfile(BASE_STAGE / "J716fNativeLog" / shared, app_dir / shared)
    for fast_file in ("NativeFast.h", "FastRead.c", "FastArm.c", "FastPower.c", "FastTests.c", "Read.inf", "Arm.inf", "Power.inf"):
        shutil.copyfile(FAST_SRC / fast_file, app_dir / fast_file)

    (stage / "NativeLogApps.dsc").write_text(NATIVE_LOG_APPS_DSC, encoding="utf-8", newline="\n")
    (stage / "unixnu.toml").write_text(FAST_UNIXNU_TOML, encoding="utf-8", newline="\n")
    (stage / "build-native-payload.py").write_text(PAYLOAD_BUILDER_PY, encoding="utf-8", newline="\n")

    # Copy & verify exact prelinked kernel and audited ADT
    inputs = stage / "inputs"
    inputs.mkdir()
    for name in ("kernel.macho", "kernel.macho.adt", "kernel.macho.json", "hardware.dtb", "j716f.dts", "sm8250.dtsi"):
        shutil.copyfile(BASE_STAGE / "inputs" / name, inputs / name)

    kernel_sha = sha((inputs / "kernel.macho").read_bytes())
    adt_sha = sha((inputs / "kernel.macho.adt").read_bytes())
    if kernel_sha != EXPECTED_KERNEL_SHA:
        raise RuntimeError(f"Kernel SHA256 mismatch: {kernel_sha}")
    if adt_sha != EXPECTED_ADT_SHA:
        raise RuntimeError(f"ADT SHA256 mismatch: {adt_sha}")

    shim_path, dtb_path = extract_qualified_shim_and_dtb(stage / "bootwrap")

    manifest = {
        "schema": 1,
        "profile": "FAST-V1",
        "logger_candidate_sha256": cand,
        "prelinked_kernel_sha256": kernel_sha,
        "adt_sha256": adt_sha,
        "bootshim_sha256": sha(shim_path.read_bytes()),
        "firmware_dtb_sha256": sha(dtb_path.read_bytes()),
        "fd_base": "0xce000000",
        "fd_bytes": 0x700000,
        "fat_guid": FAT_GUID,
        "source_files": {
            p.relative_to(stage).as_posix(): sha(p.read_bytes())
            for p in sorted(stage.rglob("*"))
            if p.is_file() and p.relative_to(stage).parts[0] not in ("inputs", "bootwrap")
        },
    }
    (stage / "firmware-source.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def connect_ssh(host: str, user: str):
    import paramiko

    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.load_system_host_keys()

    pw = os.environ.get("J716F_SSH_PASSWORD")
    key_path = Path.home() / ".ssh/id_ed25519"
    if key_path.exists() and not pw:
        try:
            client.connect(host, username=user, key_filename=str(key_path), timeout=12, look_for_keys=True, allow_agent=True)
            return client
        except Exception as exc:
            raise RuntimeError(
                f"SSH public-key login to {user}@{host} failed ({exc}). "
                f"Authorize {key_path}.pub on {user}@{host} or set J716F_SSH_PASSWORD."
            ) from None
    if pw:
        client.connect(host, username=user, password=pw, timeout=12, look_for_keys=False, allow_agent=False)
        return client
    raise RuntimeError(f"No SSH key at {key_path} and J716F_SSH_PASSWORD not set.")


def remote_build(stage: Path, host: str, user: str) -> dict:
    spec = json.loads((stage / "firmware-source.json").read_text(encoding="utf-8"))
    client = connect_ssh(host, user)

    def run_ssh(cmd: str) -> tuple[int, str, str]:
        _, out, err = client.exec_command(cmd)
        stdout = out.read().decode("utf-8", errors="replace")
        stderr = err.read().decode("utf-8", errors="replace")
        return out.channel.recv_exit_status(), stdout, stderr

    try:
        code, remote_raw, err = run_ssh("mktemp -d /home/test/tb-j716f-unixnu/native-fast-v1-XXXXXXXX")
        remote = remote_raw.strip()
        if code != 0 or not remote.startswith("/home/test/tb-j716f-unixnu/native-fast-v1-"):
            raise RuntimeError(f"Cannot create remote workspace: {err}")
        print(f"Remote build workspace: {remote}", flush=True)

        with client.open_sftp() as sftp:
            created_dirs = set()
            for path in stage.rglob("*"):
                if not path.is_file() or "__pycache__" in path.parts or path.relative_to(stage).parts[0] == "bootwrap":
                    continue
                rel = path.relative_to(stage).as_posix()
                rpath = f"{remote}/{rel}"
                cur = remote
                for part in posixpath.dirname(rel).split("/"):
                    if not part or part == ".":
                        continue
                    cur = f"{cur}/{part}"
                    if cur not in created_dirs:
                        sftp.mkdir(cur)
                        created_dirs.add(cur)
                sftp.put(str(path), rpath)

        # Zero git commands in build script! Includes host unit test of FastTests.c first.
        build_sh = f"""set -eo pipefail
source /home/test/tb-j716f-unixnu/.venv/bin/activate
cd {shlex.quote(remote)}
gcc -O2 -fno-strict-aliasing -Wall -Wextra -Werror -IJ716fNativeLog J716fNativeLog/FastTests.c -o fast-tests
./fast-tests > fast-tests.log 2>&1
export WORKSPACE="$PWD" CONF_PATH="$PWD/Conf"
export PACKAGES_PATH="$PWD:$PWD/Platforms/Lenovo:$PWD/package:$PWD/DarwinBoot:{REMOTE_UEFI}:{REMOTE_UEFI}/Common/Mu:{REMOTE_UEFI}/Common/Mu_OEM_Sample:{REMOTE_UEFI}/Mu_Basecore:{REMOTE_UEFI}/Silicon/Qualcomm:{REMOTE_UEFI}/Silicon/Silicium:/home/test/tb-j716f-unixnu/ports/j716f/edk2-overlay"
mkdir -p "$CONF_PATH"
cp {REMOTE_UEFI}/Conf/target.txt {REMOTE_UEFI}/Conf/tools_def.txt {REMOTE_UEFI}/Conf/build_rule.txt "$CONF_PATH/"
source {REMOTE_UEFI}/Mu_Basecore/BaseTools/BuildEnv
build -a AARCH64 -t CLANGPDB -b RELEASE -p NativeLogApps.dsc > reader-build.log 2>&1
python3 build-native-payload.py "$PWD" > payload-build.log 2>&1
build -a AARCH64 -t CLANGPDB -b RELEASE -p Platforms/Lenovo/j716fPkg/j716f-native.dsc \\
  -D FD_BASE=0xce000000 -D FD_SIZE=0x700000 -D FD_BLOCKS=0x700 -D ENABLE_SECUREBOOT=0 \\
  -y firmware-build-report.txt -Y LIBRARY -Y DEPEX > firmware-build.log 2>&1
"""
        print("Running FastTests.c + EDK II CLANGPDB build on 10.1.2.110...", flush=True)
        code, out, err = run_ssh("bash -c " + shlex.quote(build_sh))

        built = stage / "built"
        built.mkdir(exist_ok=True)
        wanted = {
            "fast-tests.log": "fast-tests.log",
            "reader-build.log": "reader-build.log",
            "payload-build.log": "payload-build.log",
            "firmware-build.log": "firmware-build.log",
            "firmware-build-report.txt": "firmware-build-report.txt",
            "payload/payload.json": "payload.json",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd": "SILICIUM_UEFI.fd",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/FV/FVMAIN.Fv": "FVMAIN.Fv",
            "Platforms/Lenovo/j716fPkg/Resources/NativePayload.fat": "NativePayload.fat",
            "Build/NativeLogApps/RELEASE_CLANGPDB/AARCH64/J716fNativeRead.efi": "J716fNativeRead.efi",
            "Build/NativeLogApps/RELEASE_CLANGPDB/AARCH64/J716fNativeArm.efi": "J716fNativeArm.efi",
            "Build/NativeLogApps/RELEASE_CLANGPDB/AARCH64/J716fPowerAction.efi": "J716fPowerAction.efi",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/AARCH64/UniXNU.efi": "UniXNU.efi",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/AARCH64/DarwinBoot.efi": "DarwinBoot.efi",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/AARCH64/J716fNativeFatDxe.efi": "J716fNativeFatDxe.efi",
            "Build/j716fNativePkg/RELEASE_CLANGPDB/AARCH64/J716fKeysDxe.efi": "J716fKeysDxe.efi",
        }
        with client.open_sftp() as sftp:
            for rrel, lname in wanted.items():
                try:
                    sftp.stat(f"{remote}/{rrel}")
                except FileNotFoundError:
                    continue
                sftp.get(f"{remote}/{rrel}", str(built / lname))

        if code != 0:
            for log_name in ("reader-build.log", "payload-build.log", "firmware-build.log"):
                lp = built / log_name
                if lp.exists():
                    print(f"=== {log_name} ===", file=sys.stderr)
                    print(lp.read_text(encoding="utf-8", errors="replace")[-3500:], file=sys.stderr)
            raise RuntimeError(f"Remote build failed with exit code {code}: {out + err}")

        result = {
            "exit_code": 0,
            "remote": remote,
            "profile": spec["profile"],
            "artifacts": {p.name: sha(p.read_bytes()) for p in sorted(built.iterdir()) if p.is_file()},
        }
        (built / "build-result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        return result
    finally:
        client.close()


def one_section(parts: list[tuple[int, bytes]], kind: int) -> bytes:
    items = [body for k, body in parts if k == kind]
    if len(items) != 1:
        raise ValueError(f"Expected 1 EFI section of type {kind}, got {len(items)}")
    return items[0]


def package_and_audit(stage: Path, out_dir: Path) -> dict:
    built = stage / "built"
    spec = json.loads((stage / "firmware-source.json").read_text(encoding="utf-8"))
    fd = (built / "SILICIUM_UEFI.fd").read_bytes()
    fv, modules, sec = extract_fd(fd)
    if fv != (built / "FVMAIN.Fv").read_bytes():
        raise RuntimeError("Compressed FV differs from FVMAIN.Fv")

    # Verify SEC + DXE memory reservations (0xb0000000, 0xd0000000, 0xce000000)
    map_users = [sec] + [
        one_section(modules[g], 0x10)
        for g in (
            "d6a2cb7f-6a18-4e2f-b43b-9920a733700a",
            "6c2004ef-4e0e-4be4-b14c-340eb4aa5891",
            "b8d9777e-d72a-451f-9bdb-bafb52a68415",
            "40d49ba8-448a-40e5-bf92-75fdc4a760d4",
        )
    ]
    for binary in map_users:
        for pair in ((0xB0000000, 0x100000), (0xD0000000, 0x18000000), (0xCE000000, 0x700000)):
            if binary.count(struct.pack("<QQ", *pair)) != 1:
                raise RuntimeError(f"Missing linked memory reservation {pair}")

    fat = one_section(modules[FAT_GUID], 0x19)
    if fat != (built / "NativePayload.fat").read_bytes():
        raise RuntimeError("Embedded FAT differs from NativePayload.fat")
    content = read_fat32(fat)
    wanted = {
        "/EFI/UniXNU/unixnu.toml",
        "/EFI/Tools/J716fNativeRead.efi",
        "/EFI/Tools/J716fNativeArm.efi",
        "/EFI/Tools/J716fPowerAction.efi",
        "/Payloads/kernel.macho",
        "/Payloads/kernel.macho.adt",
    }
    if set(content) != wanted:
        raise RuntimeError(f"Unexpected FAT32 file set: {sorted(content)}")
    if sha(content["/Payloads/kernel.macho"]) != EXPECTED_KERNEL_SHA:
        raise RuntimeError("Embedded kernel.macho SHA256 mismatch!")
    if sha(content["/Payloads/kernel.macho.adt"]) != EXPECTED_ADT_SHA:
        raise RuntimeError("Embedded kernel.macho.adt SHA256 mismatch!")
    for app in ("J716fNativeRead", "J716fNativeArm", "J716fPowerAction"):
        if content[f"/EFI/Tools/{app}.efi"] != (built / f"{app}.efi").read_bytes():
            raise RuntimeError(f"FAT binary mismatch for {app}.efi")
    if "Native manual-only:".encode("utf-16le") not in (built / "UniXNU.efi").read_bytes():
        raise RuntimeError("Manual-only menu loop missing in UniXNU.efi")

    shim = (stage / "bootwrap/BootShim.bin").read_bytes()
    dtb = (stage / "bootwrap/dtb.bin").read_bytes()
    if shim[56:60] != b"ARM\x64" or struct.unpack_from("<QQ", shim, 8) != (0xCE000000, len(shim) + len(fd)):
        raise RuntimeError("BootShim/FD geometry mismatch")

    out_dir.mkdir(parents=True, exist_ok=False)
    raw = shim + fd
    packed = gzip.compress(raw, mtime=0) + dtb
    (out_dir / "Image.gz-dtb").write_bytes(packed)
    (out_dir / "empty-ramdisk").write_bytes(b"")
    (out_dir / "dtb.bin").write_bytes(dtb)

    target = out_dir / "j716f-native-log-40458123294-fast-v1-boot.img"
    subprocess.run(
        [
            sys.executable,
            str(MKBOOTIMG),
            "--kernel",
            str(out_dir / "Image.gz-dtb"),
            "--ramdisk",
            str(out_dir / "empty-ramdisk"),
            "--dtb",
            str(out_dir / "dtb.bin"),
            "--output",
            str(target),
            "--header_version",
            "2",
            "--base",
            "0",
            "--kernel_offset",
            "32768",
            "--tags_offset",
            "256",
            "--pagesize",
            "4096",
            "--dtb_offset",
            "32505856",
        ],
        check=True,
    )

    # Full roundtrip unpack & Android v2 header verification
    final = target.read_bytes()
    ks, ka, rs, _ra, ss, _sa, tags, page, version, _ = struct.unpack_from("<10I", final, 8)
    if final[:8] != b"ANDROID!" or (ka, rs, ss, tags, page, version) != (0x8000, 0, 0, 0x100, 4096, 2):
        raise RuntimeError("Invalid Android boot image v2 header")
    dec = zlib.decompressobj(31)
    actual = dec.decompress(final[page : page + ks])
    if not dec.eof or dec.unused_data != dtb or actual != raw:
        raise RuntimeError("Android boot image gzip roundtrip failed")
    _, again, _ = extract_fd(actual[len(shim) :])
    if read_fat32(one_section(again[FAT_GUID], 0x19)) != content:
        raise RuntimeError("Android boot image FAT32 roundtrip failed")
    if len(final) >= 0x6000000:
        raise RuntimeError("Image exceeds 96 MiB boot_b partition size")

    audit = {
        "schema": 1,
        "stage": "native-fast-v1-host-audited",
        "image": target.name,
        "image_bytes": len(final),
        "image_sha256": sha(final),
        "fd_sha256": sha(fd),
        "fv_sha256": sha(fv),
        "payload_sha256": sha(fat),
        "kernel_sha256": EXPECTED_KERNEL_SHA,
        "adt_sha256": EXPECTED_ADT_SHA,
        "logger_candidate_sha256": EXPECTED_CANDIDATE_SHA,
        "bootshim_sha256": EXPECTED_SHIM_SHA,
        "firmware_dtb_sha256": EXPECTED_DTB_SHA,
        "reservation_descriptors_linked": True,
        "automatic_start": False,
    }
    (out_dir / "host-audit.json").write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
    return audit


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stage", type=Path, default=ROOT / "debug/native-fast-stage-v1")
    parser.add_argument("--output", type=Path, default=ROOT / "Firmware/NATIVE-40458123294-FAST-V1")
    parser.add_argument("--ssh-host", default="10.1.2.110")
    parser.add_argument("--ssh-user", default="test")
    parser.add_argument("--stage-only", action="store_true", help="Verify & stage locally without SSH build")
    args = parser.parse_args()

    manifest = stage_fast_v1(args.stage.resolve())
    if args.stage_only:
        print(
            json.dumps(
                {
                    "staged": True,
                    "stage_dir": str(args.stage.resolve()),
                    "logger_candidate_sha256": manifest["logger_candidate_sha256"],
                    "prelinked_kernel_sha256": manifest["prelinked_kernel_sha256"],
                    "adt_sha256": manifest["adt_sha256"],
                    "bootshim_sha256": manifest["bootshim_sha256"],
                    "firmware_dtb_sha256": manifest["firmware_dtb_sha256"],
                    "staged_file_count": len(manifest["source_files"]),
                },
                indent=2,
            )
        )
        return

    remote_build(args.stage.resolve(), args.ssh_host, args.ssh_user)
    audit = package_and_audit(args.stage.resolve(), args.output.resolve())
    print(json.dumps(audit, indent=2))


if __name__ == "__main__":
    main()
