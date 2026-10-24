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


def revision(path):
    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unixnu", type=Path, required=True)
    parser.add_argument("--device", type=Path, required=True)
    parser.add_argument("--jobs", type=int, default=3)
    args = parser.parse_args()
    base, device = args.unixnu.resolve(), args.device.resolve()
    if platform.system() != "Darwin":
        raise SystemExit("Run this configuration on the macOS runner")
    if revision(base) != UNIXNU_COMMIT:
        raise SystemExit("Unexpected UniXNU commit")
    if args.jobs < 1:
        raise SystemExit("jobs must be positive")
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
