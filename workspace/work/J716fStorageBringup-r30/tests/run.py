#!/usr/bin/env python3
"""Compile new orchestration tests and existing real adapter model regressions."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys

def identity(p):
    b = p.read_bytes()
    return {"bytes": len(b), "sha256": hashlib.sha256(b).hexdigest()}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--root", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--compiler", default="clang++")
    a = p.parse_args()
    root, out = a.root.resolve(), a.output.resolve()
    if out.exists():
        p.error("Output exists; use a fresh evidence directory")
    out.mkdir(parents=True)
    component = root / "work/J716fStorageBringup-r30"
    target = root / "work/UniXNU-j716f-host-ufs-observation"
    platform = target / "socs/sm8250/kits/Sm8250Platform"
    sheng = target / "drivers/ShengUfs"
    flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-g"]
    flags += ["-I", str(component), "-I", str(platform), "-I", str(sheng)]
    tests = {
        "StorageBringup": [component / "tests/StorageBringup.cpp", component / "J716fStorageBringup.cpp", platform / "Sm8250UfsDiagnostics.cpp"],
        "Sm8250UfsDiagnostics": [platform / "tests/Sm8250UfsDiagnostics.cpp", platform / "Sm8250UfsDiagnostics.cpp"],
        "Sm8250UfsPipeline": [platform / "tests/Sm8250UfsPipeline.cpp", platform / "Sm8250UfsHci.cpp", platform / "Sm8250UfsHandoff.cpp", platform / "AppsSmmuOwner.cpp"],
    }
    sources = {}
    for folder in (component, platform, sheng):
        for file in folder.rglob("*"):
            if file.is_file() and file.suffix in (".h", ".cpp", ".py"):
                sources[file.relative_to(root).as_posix()] = identity(file)
    report = {"schema_version": 1, "passed": False, "device_access": False, "qemu": False,
              "runtime_iokit_execution": False, "scope": "bridge and real pure adapter against instrumented host models",
              "inputs": sources, "steps": []}
    env = dict(os.environ, ASAN_OPTIONS="halt_on_error=1:abort_on_error=1:detect_leaks=1", UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1")
    try:
        for name, files in tests.items():
            binary = out / name
            for label, argv in (("compile", [a.compiler, *flags, *map(str, files), "-o", str(binary)]), ("run", [str(binary)])):
                proc = subprocess.run(argv, cwd=root, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=180)
                (out / (name + "-" + label + ".log")).write_text(proc.stdout)
                report["steps"].append({"name": name, "label": label, "argv": argv, "exit_code": proc.returncode})
                if proc.returncode:
                    raise RuntimeError(name + " " + label + " failed: " + proc.stdout[-4000:])
        for relative, expected in sources.items():
            if identity(root / relative) != expected:
                raise RuntimeError("Input changed: " + relative)
        report["passed"] = True
    except Exception as exc:
        report["error"] = str(exc)
    (out / "result.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({k: report[k] for k in ("passed", "device_access", "runtime_iokit_execution")}))
    return 0 if report["passed"] else 1

if __name__ == "__main__":
    sys.exit(main())
