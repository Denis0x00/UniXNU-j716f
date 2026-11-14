#!/usr/bin/env python3
"""Compile and exercise the actual Menu.c loop with fake UEFI services."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    package = here.parents[1]
    source = package / "Library/UniXNULib/Menu.c"
    original = source.read_text(encoding="utf-8")
    marker = "//\n// The Tools submenu reuses the ordinary menu loop"
    assert original.count(marker) == 1, "Menu.c boundary changed; review harness"
    tested = original.split(marker)[0]
    assert tested.count('#include "Internal.h"') == 1
    tested = tested.replace('#include "Internal.h"', '#include "menu_shim.h"')
    declaration = "gUniXNUPkgTokenSpaceGuid.PcdUniXNUDiagnosticMenuHold"
    assert declaration + "|FALSE|BOOLEAN|0x00000006" in (
        package / "UniXNUPkg.dec"
    ).read_text(encoding="utf-8")
    assert declaration in (
        package / "Library/UniXNULib/UniXNULib.inf"
    ).read_text(encoding="utf-8")
    # Every config and the Tools submenu reaches this actual menu function.
    assert "Selected = UniXNUMenuRun (Ui, &Tools, FALSE);" in original
    compiler = shutil.which(args.cc)
    if not compiler:
        raise SystemExit(f"C compiler not found: {args.cc}")
    with tempfile.TemporaryDirectory(prefix="unixnu-menu-test-") as work:
        root = Path(work)
        (root / "menu_under_test.c").write_text(tested, encoding="utf-8")
        executable = root / ("menu_test.exe" if os.name == "nt" else "menu_test")
        command = [
            compiler, "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wno-unused-parameter", "-g", "-O1", "-I", str(here),
            "-I", str(root), str(here / "menu_harness.c"), "-o", str(executable),
        ]
        if args.sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        subprocess.run(command, check=True)
        subprocess.run([str(executable)], check=True, timeout=30)
    print(f"Menu.c SHA-256: {hashlib.sha256(source.read_bytes()).hexdigest()}")
    print("PCD default/INF/Tools integration checks passed")


if __name__ == "__main__":
    main()
