#!/usr/bin/env python3
"""Safe boot_b-only deployment tool for J716F NATIVE FAST-V1.

Safety invariants:
  - Default mode is read-only probe (no writes unless --flash-boot-b is passed).
  - Never touches boot_a, vbmeta, userdata, or TzDxe.
  - Verifies boot_a fallback remains bootable (slot-unbootable:a == 'no').
  - Supports BOTH ABL Bootloader fastboot (is-userspace: no, PREFERRED so Linux
    ramoops never pollutes 0xb00be000!) and fastbootd (is-userspace: yes).
  - Zero git invocations.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timedelta, timezone
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_DIR = ROOT / "Firmware/NATIVE-40458123294-FAST-V1"
FASTBOOT = Path("D:/platform-tools/fastboot.exe")
EXPECTED_KERNEL_SHA = "150c5c5f13a4337eb05218232f3a157429f0974ba49f71b4627789497132d074"
EXPECTED_ADT_SHA = "806ad48c85881b51f6acbeeea7ff4ec065f9edbe32d90f4aa304b5046faa64a3"
EXPECTED_ADT_V12_SHA = "5a0c7fe4efd5d6210bff5373e160edc86faa780996858601042400f70174558e"
EXPECTED_CANDIDATE_SHA = "1c19ee51d5388dcfbbaefd41dcf361a54ccee3d08d889f6f0e4bb6f69a08d892"
TZ = timezone(timedelta(hours=8))


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def require(cond: bool, msg: str) -> None:
    if not cond:
        raise RuntimeError(msg)


def verify_audited_image(fw_dir: Path) -> tuple[Path, dict]:
    audit_path = fw_dir / "host-audit.json"
    require(audit_path.exists(), f"Missing {audit_path}; run build script first")
    audit = json.loads(audit_path.read_text(encoding="utf-8"))
    require(
        audit.get("stage") in (
            "native-fast-v1-host-audited",
            "native-fast-v2-host-audited",
            "native-fast-v3-host-audited",
            "native-fast-v4-host-audited",
            "native-fast-v5-host-audited",
            "native-fast-v6-host-audited",
            "native-fast-v7-host-audited",
            "native-fast-v8-host-audited",
            "native-fast-v9-host-audited",
            "native-fast-v10-host-audited",
            "native-fast-v11-host-audited",
            "native-fast-v12-host-audited",
        ),
        "Unexpected audit stage",
    )
    require(len(audit.get("kernel_sha256", "")) == 64, "Missing kernel SHA256 in audit")
    require(audit.get("adt_sha256") in (EXPECTED_ADT_SHA, EXPECTED_ADT_V12_SHA), "ADT SHA256 mismatch in audit")
    require(audit.get("logger_candidate_sha256") == EXPECTED_CANDIDATE_SHA, "Candidate SHA256 mismatch in audit")
    require(audit.get("automatic_start") is False, "automatic_start must be False")
    img_path = fw_dir / audit["image"]
    require(img_path.exists(), f"Missing image {img_path}")
    require(img_path.stat().st_size == audit["image_bytes"] < 0x6000000, "Image size mismatch")
    require(sha(img_path) == audit["image_sha256"], "Image SHA256 mismatch against host-audit.json")
    return img_path, audit


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--firmware-dir", type=Path, default=DEFAULT_DIR)
    parser.add_argument("--serial", help="Optional fastboot serial filter")
    parser.add_argument("--flash-boot-b", action="store_true", help="Flash boot_b, set_active b, and reboot")
    args = parser.parse_args()

    fw_dir = args.firmware_dir.resolve()
    img_path, audit = verify_audited_image(fw_dir)
    now = lambda: datetime.now(TZ).isoformat()
    stamp = datetime.now(TZ).strftime("%Y%m%d-%H%M%S")
    record_path = fw_dir / f"{'flash' if args.flash_boot_b else 'probe'}-{stamp}.json"

    report: dict = {
        "schema": 1,
        "started_at": now(),
        "mode": "flash-boot-b" if args.flash_boot_b else "read-only-probe",
        "image": str(img_path),
        "image_sha256": audit["image_sha256"],
        "kernel_sha256": audit["kernel_sha256"],
        "operations": [],
        "partitions_flashed": [],
    }

    def save() -> None:
        record_path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    def fb(words: list[str], serial: str | None = None, optional: bool = False) -> str:
        allowed_ro = words == ["devices"] or (len(words) == 2 and words[0] == "getvar")
        allowed_rw = args.flash_boot_b and words in (
            ["flash", "boot_b", str(img_path)],
            ["set_active", "b"],
            ["reboot"],
        )
        require(allowed_ro or allowed_rw, f"Blocked fastboot command outside boot_b scope: {words}")
        cmd = [str(FASTBOOT), *(["-s", serial] if serial else []), *words]
        op = {"cmd": words, "started_at": now()}
        report["operations"].append(op)
        res = subprocess.run(cmd, capture_output=True, text=True, timeout=60 if words[0] == "flash" else 15)
        out = res.stdout + res.stderr
        op.update(exit_code=res.returncode, output=out.splitlines(), completed_at=now())
        save()
        failed = res.returncode != 0 or bool(re.search(r"\bFAILED\b|error:", out, re.I))
        require(not failed or optional, f"fastboot {words[0]} failed: {out.strip()}")
        return out

    rows = [ln.split() for ln in fb(["devices"]).splitlines() if ln.strip()]
    if args.serial:
        rows = [r for r in rows if r[0] == args.serial]
    require(len(rows) == 1 and rows[0][1] == "fastboot", f"Expected exactly 1 fastboot device, got: {rows}")
    serial = rows[0][0]
    report["serial"] = serial

    def getvar(name: str, optional: bool = False) -> str | None:
        out = fb(["getvar", name], serial=serial, optional=optional)
        m = re.search(r"(?:\(bootloader\)\s*)?" + re.escape(name) + r":\s*(\S+)", out)
        if not m:
            require(optional, f"Missing fastboot var: {name}")
            return None
        return m.group(1)

    probe = {
        k: getvar(k)
        for k in (
            "product",
            "is-userspace",
            "unlocked",
            "slot-count",
            "has-slot:boot",
            "partition-size:boot_b",
            "current-slot",
            "slot-unbootable:a",
            "slot-unbootable:b",
        )
    }
    report["probe"] = probe
    save()

    require(probe["product"] in ("p11_pro_plus", "J716F", "kona"), f"Unexpected product: {probe['product']}")
    require(probe["unlocked"] == "yes" and probe["slot-count"] == "2" and probe["has-slot:boot"] == "yes", "Invalid slot/unlock state")
    require(int(probe["partition-size:boot_b"] or "0", 0) == 0x6000000, "Unexpected boot_b partition size")
    require(probe["slot-unbootable:a"] == "no", "Refusing: fallback slot A is marked unbootable!")

    if not args.flash_boot_b:
        print(json.dumps({"read_only": True, "serial": serial, "probe": probe, "record": str(record_path)}, indent=2))
        return

    fb(["flash", "boot_b", str(img_path)], serial=serial)
    report["partitions_flashed"] = ["boot_b"]
    fb(["set_active", "b"], serial=serial)
    require(getvar("current-slot") == "b", "Active slot did not switch to b")
    require(getvar("slot-unbootable:b") == "no", "boot_b marked unbootable")
    require(getvar("slot-unbootable:a") == "no", "boot_a state changed unexpectedly")
    fb(["reboot"], serial=serial)
    report["completed_at"] = now()
    save()
    print(
        json.dumps(
            {
                "flashed": "boot_b",
                "active_slot": "b",
                "fastboot_mode": "bootloader (clean RAM)" if probe["is-userspace"] == "no" else "fastbootd (userspace)",
                "rebooted": True,
                "record": str(record_path),
            },
            indent=2,
        )
    )


if __name__ == "__main__":
    main()
