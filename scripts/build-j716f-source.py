#!/usr/bin/env python3
"""Compile source XNU and its archives using a pinned UniXNU build driver.

Do not call kernel.prelink: this phase has no original kernelcache or J716F ADT.
No device access or firmware, payload, userdata construction is performed.
"""
import argparse
import hashlib
import json
import platform
import struct
import subprocess
import sys
from pathlib import Path

UNIXNU_COMMIT = "245aca1a9ec5b730b1cad80fade295800d2981f1"
DEFCONFIG = "j716f_source_defconfig"
SOURCE_FIX = "patches/build-driver/0002-guard-optional-legacy-display-probe.patch"


def revision(path):
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def prepare_source_fix(base, device):
    """Apply the recorded fix to the pinned source, then check both C branches."""
    patch = device / SOURCE_FIX
    command = ["git", "-C", str(base), "apply"]
    applied = subprocess.run(
        [*command, "--reverse", "--check", str(patch)],
        capture_output=True,
    ).returncode == 0
    if not applied:
        subprocess.run([*command, "--check", str(patch)], check=True)
        subprocess.run([*command, str(patch)], check=True)
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
    return {"patch": SOURCE_FIX,
            "sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
            "patched_source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "preprocessor_cases": cases}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unixnu", type=Path, required=True)
    parser.add_argument("--device", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=3)
    parser.add_argument("--check-source-fix-only", action="store_true")
    args = parser.parse_args()
    base, device = args.unixnu.resolve(), args.device.resolve()
    if revision(base) != UNIXNU_COMMIT:
        raise SystemExit("Unexpected UniXNU commit")
    if args.jobs < 1:
        raise SystemExit("jobs must be positive")
    if not args.check_source_fix_only and platform.system() != "Darwin":
        raise SystemExit("Run this configuration on the macOS runner")
    source_fix = prepare_source_fix(base, device)
    if args.check_source_fix_only:
        print(json.dumps(source_fix, indent=2))
        return
    sys.path.insert(0, str(base / "scripts"))
    from unixnu.cli import cmd_defconfig, parser as cli_parser, prepare
    from unixnu.config import Context
    from unixnu import kernel

    options = cli_parser().parse_args(
        ["defconfig", DEFCONFIG, "-D", str(device), "-j", str(args.jobs)]
    )
    ctx = Context(options)
    ctx.refuse_example()
    cmd_defconfig(ctx)
    # Context reads .config at creation, matching the CLI's separate processes.
    ctx = Context(options)
    config = ctx.need_config()
    if not config.get("XNU_BUILD_ARCHIVES"):
        raise SystemExit("Source archive construction must be enabled")
    for feature in ("XNU_NATIVE_AES", "XNU_NATIVE_UFS", "XNU_ARMCPU",
                    "XNU_ARM_DT_PLATFORM", "XNU_ARMVIRT_PLATFORM",
                    "XNU_NATIVE_ARCH_TIMER", "XNU_NATIVE_ADRENO"):
        if config.get(feature):
            raise SystemExit(f"Source qualification must not enable {feature}")
    ctx.logs.mkdir(parents=True, exist_ok=True)
    report_path = ctx.logs / "source-build-report.json"
    report = {
        "phase": "source-only",
        "board": ctx.name,
        "defconfig": DEFCONFIG,
        "unixnu_commit": revision(base),
        "device_commit": revision(device),
        "source_fixes": [source_fix],
        "status": "started",
        "prelink_performed": False,
        "hardware_boot_validated": False,
        "ipsw_required": False,
        "artifacts": [],
    }
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    try:
        modules = prepare(ctx, ["kernel"])
        report["references"] = {
            name: module.commit for (kind, name), module in modules.items()
            if kind == "refer" and module.commit
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
        paths = [native, ctx.tree / "build/libpthread-kernel/libpthread_kernel.a",
                 ctx.tree / "build/corecrypto-2023/libcorecrypto_kernel_noasm.a",
                 ctx.tree / "build/corecrypto-2023/libcorecrypto_kernel_noasm-namespaced.a"]
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
    main()
