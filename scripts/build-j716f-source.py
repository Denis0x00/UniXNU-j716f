#!/usr/bin/env python3
"""Compile source XNU and its archives using a pinned UniXNU build driver.

Do not call kernel.prelink: this phase has no original kernelcache or J716F ADT.
No device access or firmware, payload, userdata construction is performed.
"""
import argparse
import importlib.util
import hashlib
import json
import platform
import struct
import subprocess
import sys
from pathlib import Path

UNIXNU_COMMIT = "42bffe8a56823dad2e448c776b4f94862f3e290a"
DEFCONFIG = "j716f_source_defconfig"
SOURCE_FIX = "patches/build-driver/0002-guard-optional-legacy-display-probe.patch"
PATCH_SERIES_FIX = "patches/build-driver/0003-allow-boot-progress-without-sheng-pmgr.patch"
HEADER_RESTORE_FIX = "patches/build-driver/0004-restore-xnu-headers-after-reclone.patch"


def revision(path):
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def prepare_source_fix(base, device):
    """Apply recorded compatibility fixes to the pinned source tree."""
    patches = [device / SOURCE_FIX, device / PATCH_SERIES_FIX, device / HEADER_RESTORE_FIX]
    command = ["git", "-C", str(base), "apply"]
    for patch in patches:
        applied = subprocess.run(
            [*command, "--reverse", "--check", str(patch)],
            capture_output=True,
        ).returncode == 0
        if not applied:
            for suffix in (["--check", str(patch)], [str(patch)]):
                result = subprocess.run([*command, *suffix], capture_output=True, text=True)
                if result.returncode:
                    raise RuntimeError(f"Source compatibility patch {patch.name}: "
                                       +(result.stdout+result.stderr)[-1800:])
    # Preprocess the actual added C code in the first bsd_init patch hunk.
    # No Apple headers are needed to test the optional provider reference.
    source = base / "patches/xnu/offline-component-load-probes.patch"
    first_hunk = source.read_text().split("@@", 2)[2].split("@@", 1)[0]
    added = "\n".join(line[1:] for line in first_hunk.splitlines()
                      if line.startswith("+")) + "\n"
    cases = {}
    for mode in (None, "0", "1"):
        flags = [] if mode is None else [f"-DXNU_NATIVE_LEGACY_DISPLAY={mode}"]
        result = subprocess.run(
            ["clang", "-E", "-P", "-x", "c", "-DDEVELOPMENT=1", "-DDEBUG=0",
             *flags, "-"], input=added, text=True, capture_output=True, check=True,
        )
        present = "native_legacy_display_probe" in result.stdout
        if present != (mode == "1"):
            raise RuntimeError(f"Legacy display guard failed for mode {mode}")
        cases["unset" if mode is None else mode] = {"provider_referenced": present}
    print("== source fix: legacy display guard passed (unset, disabled, enabled)",
          flush=True)
    return [{"patch": str(path.relative_to(device)),
             "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
            for path in patches] + [{
            "patched_source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "preprocessor_cases": cases}]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unixnu", type=Path, required=True)
    parser.add_argument("--device", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--check-source-fix-only", action="store_true")
    parser.add_argument("--native-arch-timer", action="store_true",
                        help="Build/link native timer with ArmDTPlatform and ArmCpu; no prelink or hardware claim")
    args = parser.parse_args()
    base, device = args.unixnu.resolve(), args.device.resolve()
    if revision(base) != UNIXNU_COMMIT:
        raise SystemExit("Unexpected UniXNU commit")
    if args.jobs < 1:
        raise SystemExit("jobs must be positive")
    if not args.check_source_fix_only and platform.system() != "Darwin":
        raise SystemExit("Run this configuration on the macOS runner")
    source_fixes = prepare_source_fix(base, device)
    if args.check_source_fix_only:
        print(json.dumps(source_fixes, indent=2))
        return
    sys.path.insert(0, str(base / "scripts"))
    from unixnu.cli import cmd_defconfig, parser as cli_parser, prepare
    from unixnu.config import Context
    from unixnu import kernel

    original_device = device
    if args.native_arch_timer:
        spec = importlib.util.spec_from_file_location(
            'native_candidate', device/'scripts/prepare-native-platform.py')
        candidate = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(candidate)
        device = base.parent/'j716f-native-platform-candidate'
        candidate.prepare(original_device, device)
    selected_defconfig = "j716f_arch_timer_source_defconfig" if args.native_arch_timer else DEFCONFIG
    options = cli_parser().parse_args(
        ["defconfig", selected_defconfig, "-D", str(device), "-j", str(args.jobs)]
    )
    ctx = Context(options)
    ctx.refuse_example()
    cmd_defconfig(ctx)
    # Context reads .config at creation, matching the CLI's separate processes.
    ctx = Context(options)
    config = ctx.need_config()
    if not config.get("XNU_BUILD_ARCHIVES"):
        raise SystemExit("Source archive construction must be enabled")
    for feature in ("XNU_NATIVE_AES", "XNU_NATIVE_UFS", "XNU_ARMVIRT_PLATFORM",
                    "XNU_NATIVE_ADRENO"):
        if config.get(feature):
            raise SystemExit(f"Source qualification must not enable {feature}")
    if bool(config.get("XNU_NATIVE_ARCH_TIMER")) != args.native_arch_timer:
        raise SystemExit("Architectural timer feature must match the selected qualification profile")
    for feature in ("XNU_ARMCPU", "XNU_ARM_DT_PLATFORM"):
        if bool(config.get(feature)) != args.native_arch_timer:
            raise SystemExit(f"{feature} must match the native platform candidate")
    ctx.logs.mkdir(parents=True, exist_ok=True)
    report_path = ctx.logs / "source-build-report.json"
    report = {
        "phase": "source-only",
        "board": ctx.name,
        "defconfig": selected_defconfig,
        "native_arch_timer_requested": args.native_arch_timer,
        "armcpu_driver_integrated": False,
        "armcpu_driver_requested": args.native_arch_timer,
        "armdt_platform_driver_requested": args.native_arch_timer,
        "unixnu_commit": revision(base),
        "device_commit": revision(original_device),
        "source_fixes": source_fixes,
        "status": "started",
        "prelink_performed": False,
        "hardware_boot_validated": False,
        "ipsw_required": False,
        "artifacts": [],
    }
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    try:
        modules = prepare(ctx, ["kernel"])
        if args.native_arch_timer:
            spec = importlib.util.spec_from_file_location(
                'native_observer', original_device/'scripts/prepare-native-observer.py')
            observer = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(observer)
            armcpu = modules[('driver', 'ArmCpu')]
            # In-tree git submodules have origin='base' and Module.commit=None;
            # that field is populated only by feed fetches. Check actual HEAD.
            armcpu.commit = revision(armcpu.path)
            armdt = modules[('driver', 'ArmDTPlatform')]
            armdt.commit = revision(armdt.path)
            if armdt.commit != '417ff95d6a6e8607ddea9309ec45c3bfd4a67c60':
                raise RuntimeError('Unexpected ArmDTPlatform commit')
            report['native_irq_observer'] = observer.install(
                original_device, ctx.tree, armcpu.path, armcpu.commit)
        report["references"] = {
            name: module.commit for (kind, name), module in modules.items()
            if kind == "refer" and module.commit
        }
        report["components"] = {
            name: module.commit for (kind, name), module in modules.items()
            if kind == "driver" and name in ("ArmCpu", "ArmDTPlatform")
        }
        submodules = subprocess.check_output(
            ["git", "-C", str(base), "submodule", "status", "--recursive"],
            text=True,
        )
        (ctx.logs / "source-revisions.log").write_text(submodules)
        # The official CLI calls kernel.build followed by kernel.prelink.
        # Until original kernelcache qualification, invoke just its build phase.
        kernel.build(ctx, modules)
        native = ctx.tree / (
            "build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/"
            "mach.development.bcm2837.unstripped.noctf"
        )
        data = native.read_bytes()
        if struct.unpack_from("<4I", data) != (0xFEEDFACF, 0x100000C, 0, 2):
            raise RuntimeError("Expected ordinary ARM64 Mach-O executable")
        bcm_route = bytes.fromhex(
            "af0038d5ef1d4092000088d200bc70d30000019100080f8b011e8052010000b9df3f03d5"
        )
        route_count = data.count(bcm_route)
        if route_count != (0 if args.native_arch_timer else 1):
            raise RuntimeError("Linked BCM route count does not match timer profile")
        report["linked_bcm_route_sequence_count"] = route_count
        report["native_arch_timer_hardware_validated"] = False
        paths = [native, ctx.tree / "build/libpthread-kernel/libpthread_kernel.a",
                 ctx.tree / "build/corecrypto-2023/libcorecrypto_kernel_noasm.a",
                 ctx.tree / "build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a"]
        if args.native_arch_timer:
            # Require defined symbols in the linked Mach-O, not just byte strings
            # or the existence of unreferenced archive files.
            symbols = set()
            symbol_values = {}
            segments = []
            command_offset = 32
            for _ in range(struct.unpack_from('<I', data, 16)[0]):
                command, size = struct.unpack_from('<II', data, command_offset)
                if command == 2:
                    symoff, count, stroff, _ = struct.unpack_from('<4I', data, command_offset+8)
                    for index in range(count):
                        string, kind, section, _, value = struct.unpack_from('<IBBHQ', data, symoff+16*index)
                        if kind & 0xe == 0xe and section and value:
                            end = data.index(b'\0', stroff+string)
                            name = data[stroff+string:end].decode('utf-8')
                            symbols.add(name)
                            symbol_values[name] = value
                elif command == 0x19:
                    segments.append(struct.unpack_from('<4Q', data, command_offset+24))
                command_offset += size
            required = {'_armcpu_timer_interrupts', '_armcpu_registered',
                        '_armdt_platform_configured', '_rtclock_intr',
                        '_j716f_armcpu_trace', '_j716f_armcpu_timer_checkpoint'}
            if not required <= symbols:
                raise RuntimeError('Missing linked platform providers: '+str(sorted(required-symbols)))
            report['linked_platform_symbols'] = sorted(required)
            trace_va = symbol_values['_j716f_armcpu_trace']
            candidates = [fileoff+trace_va-vmaddr for vmaddr, _, fileoff, filesize in segments
                          if vmaddr <= trace_va and trace_va+1536 <= vmaddr+filesize]
            if len(candidates) != 1 or trace_va & 7:
                raise RuntimeError('Observer snapshot must be aligned and file-backed exactly once')
            trace_data = data[candidates[0]:candidates[0]+1536]
            if (struct.unpack_from('<3Q', trace_data) != (0x315152494636314a, 1, 192)
                    or any(trace_data[24:])):
                raise RuntimeError('Linked observer initial ABI/data differs from reviewed state')
            report['linked_native_irq_observer_abi'] = {
                'version': 1, 'bytes': 1536, 'initial_state_verified': True,
                'hardware_validated': False}
            report['armcpu_driver_linked'] = True
            report['armdt_platform_driver_linked'] = True
            paths += [ctx.tree/'build/armcpu/libArmCpu.a',
                      ctx.tree/'build/armdt-platform/libArmDTPlatform.a']
        for path in paths:
            content = path.read_bytes()
            if not content:
                raise RuntimeError(f"Empty output: {path}")
            report["artifacts"].append({
                "path": str(path.relative_to(ctx.tree)),
                "bytes": len(content),
                "sha256": hashlib.sha256(content).hexdigest(),
            })
        report["status"] = "compiled"
    except Exception as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    print("Source compilation passed; no prelinked or device-bootable kernel produced")


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Make the actual failure visible in the check annotation as well as
        # the source build log. No tokens or environment values are emitted.
        message = str(error).replace('%', '%25').replace('\r', '%0D').replace('\n', '%0A')
        print('::error title=J716F source build::'+message, flush=True)
        raise
