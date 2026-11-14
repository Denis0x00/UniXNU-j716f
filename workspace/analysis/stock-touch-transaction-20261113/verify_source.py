#!/usr/bin/env python3
"""Read-only source identity verification; --write creates the initial manifest."""
from pathlib import Path
import argparse
import hashlib
import json

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
MANIFEST = HERE / "source-manifest.json"
FROZEN = {
    "work/J716fGoodix-20261113/J716fGoodix.h": "34ac4ecc4466a9d2271627d237372262b322306901e134caf35ae730a7a65b3e",
    "work/J716fGoodix-20261113/J716fGoodix.c": "3f4fff348903b1a82488e06066794d4880fccb49bd8fcd1a755bda3a6d438b0c",
}
INPUTS = list(FROZEN) + [
    "work/J716fTouchTransaction-20261113/J716fTouchTransaction.h",
    "work/J716fTouchTransaction-20261113/J716fTouchTransaction.c",
    "analysis/stock-touch-transaction-20261113/test_transaction.c",
    "analysis/stock-touch-transaction-20261113/stock-ack/evidence.json",
    "analysis/stock-touch-transaction-20261113/stock-ack/audit_stock_ack.py",
]


def identities():
    rows = []
    for relative in INPUTS:
        data = (ROOT / relative).read_bytes()
        digest = hashlib.sha256(data).hexdigest()
        if relative in FROZEN and digest != FROZEN[relative]:
            raise SystemExit(f"Frozen Goodix dependency changed: {relative}")
        rows.append({"path": relative, "bytes": len(data), "sha256": digest})
    return rows


def main():
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--write", action="store_true")
    args = parser.parse_args()
    actual = {"schema": 1, "scope": "source identities only; no hardware claim", "files": identities()}
    if args.write:
        if MANIFEST.exists():
            raise SystemExit("Manifest already exists; do not silently replace a frozen acceptance input")
        MANIFEST.write_text(json.dumps(actual, indent=2) + "\n", encoding="utf-8")
    else:
        expected = json.loads(MANIFEST.read_text(encoding="utf-8"))
        if actual != expected:
            raise SystemExit("Source manifest differs from current inputs")
    print(f"PASS {len(actual['files'])} source identities; frozen Goodix core unchanged")


if __name__ == "__main__":
    main()
