#!/usr/bin/env python3
"""Pin bridge source and the untouched frozen five-TU adapter dependencies."""
import argparse
import hashlib
import json
from pathlib import Path

def identity(path):
    data = path.read_bytes()
    return {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    component = Path(__file__).resolve().parent
    root = component.parents[1]
    target = root / "work/UniXNU-j716f-host-ufs-observation"
    platform = target / "socs/sm8250/kits/Sm8250Platform"
    original_path = target / "docs/ufs-observation-inputs.json"
    original = json.loads(original_path.read_text())
    for relative, expected in original["files"].items():
        if identity(target / relative) != expected:
            raise ValueError("Frozen adapter changed: " + relative)
    files = {}
    for folder in (component, platform, target / "drivers/ShengUfs"):
        for path in sorted(folder.rglob("*")):
            if path.is_file() and path.suffix in (".cpp", ".h", ".py", ".md"):
                if path.is_symlink():
                    raise ValueError("Symlink source: " + str(path))
                files[path.relative_to(root).as_posix()] = identity(path)
    tus = [component / "J716fStorageBringup.cpp", component / "J716fStorageKernel.cpp"]
    tus += [platform / (name + ".cpp") for name in (
        "Sm8250UfsRuntime", "Sm8250UfsHci", "Sm8250UfsHandoff", "Sm8250UfsDiagnostics", "AppsSmmuOwner")]
    result = {"schema_version": 1, "component": "J716fStorageBringup-r30",
              "target": "TB-J716F / SM8250 / iPadOS 17.7.11 21H461 / ordinary arm64",
              "automatic_behavior": "registration and logging only",
              "old_adapter_manifest": identity(original_path),
              "translation_units": [p.relative_to(root).as_posix() for p in tus],
              "files": files, "hardware_qualified": False, "root_device_published": False}
    destination = component / "source-manifest.json"
    if args.check:
        if json.loads(destination.read_text()) != result:
            raise ValueError("Bridge source manifest differs")
    else:
        destination.write_text(json.dumps(result, indent=2) + "\n")
    print("PASS: %d pinned files, seven TUs, one owner; original adapter pins unchanged" % len(files))

if __name__ == "__main__":
    main()
