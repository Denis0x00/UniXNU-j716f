#!/usr/bin/env python3
"""Exercise encrypted source transfer with synthetic inputs, never vendor files."""
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import zipfile

SCRIPT = Path(__file__).with_name("source-artifact-transfer.py")
spec = importlib.util.spec_from_file_location("transfer", SCRIPT)
transfer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(transfer)


class SourceTransferTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.private = self.root / "private.pem"
        self.public = self.root / "public.pem"
        self.run_cli("keygen", "--private-key", self.private,
                     "--public-key", self.public)

    def run_cli(self, *args, success=True):
        result = subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def report(self, native):
        names = transfer.OUTPUTS + transfer.NATIVE_OUTPUTS if native else transfer.OUTPUTS
        entries = []
        for name in names:
            raw = ("synthetic source output: " + name).encode()
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(raw)
            entries.append(dict(path=name, bytes=len(raw), sha256=transfer.sha(raw)))
        return dict(status="compiled", prelink_performed=False,
                    native_arch_timer_requested=native, artifacts=entries)

    def seal(self, report, success=True):
        path = self.root / "report.json"
        path.write_text(json.dumps(report), encoding="utf-8")
        return self.run_cli("seal", "--root", self.root, "--report", path,
                            "--public-key", self.public, "--output", self.root / "source.enc",
                            success=success)

    def test_round_trip_both_profiles_and_legacy_report(self):
        for native in (False, True, None):
            with self.subTest(native=native):
                report = self.report(bool(native))
                if native is None:
                    del report["native_arch_timer_requested"]
                self.seal(report)
                self.run_cli("open", "--private-key", self.private,
                             "--input", self.root / "source.enc", "--output", self.root / "source.zip")
                with zipfile.ZipFile(self.root / "source.zip") as archive:
                    self.assertEqual(json.loads(archive.read("source-build-report.json")), report)
                    self.assertEqual(set(archive.namelist()),
                                     {e["path"] for e in report["artifacts"]} | {"source-build-report.json"})
                    for entry in report["artifacts"]:
                        self.assertEqual(archive.read(entry["path"]), (self.root / entry["path"]).read_bytes())
                (self.root / "source.enc").unlink()
                (self.root / "source.zip").unlink()

    def test_reject_wrong_profile_missing_extra_and_duplicate_entries(self):
        baseline = self.report(True)
        bad = []
        for key, value in (("status", "failed"), ("prelink_performed", True),
                           ("native_arch_timer_requested", False), ("native_arch_timer_requested", "true")):
            report = copy.deepcopy(baseline)
            report[key] = value
            bad.append(report)
        for name in ("missing", "extra", "duplicate", "traversal"):
            report = copy.deepcopy(baseline)
            if name == "missing":
                report["artifacts"].pop()
            elif name == "duplicate":
                report["artifacts"].append(copy.deepcopy(report["artifacts"][0]))
            else:
                report["artifacts"].append(dict(path="../private.pem" if name == "traversal" else "unexpected.a"))
            bad.append(report)
        for index, report in enumerate(bad):
            with self.subTest(index=index):
                self.seal(report, success=False)
                self.assertFalse((self.root / "source.enc").exists())

    def test_reject_modified_source_and_existing_output(self):
        report = self.report(True)
        first = self.root / report["artifacts"][0]["path"]
        original = first.read_bytes()
        first.write_bytes(b"modified")
        self.seal(report, success=False)
        first.write_bytes(original)
        self.seal(report)
        original = (self.root / "source.enc").read_bytes()
        self.seal(report, success=False)
        self.assertEqual((self.root / "source.enc").read_bytes(), original)

    def test_reject_modified_ciphertext(self):
        self.seal(self.report(True))
        path = self.root / "source.enc"
        data = bytearray(path.read_bytes())
        data[-1] ^= 1
        path.write_bytes(data)
        self.run_cli("open", "--private-key", self.private, "--input", path,
                     "--output", self.root / "source.zip", success=False)
        self.assertFalse((self.root / "source.zip").exists())

    def test_reject_authenticated_archive_manifest_mismatch(self):
        # Even an authenticated bundle must satisfy the source-profile allowlist.
        for case in ("missing", "duplicate", "extra", "bad_hash"):
            with self.subTest(case=case):
                report = self.report(True)
                if case == "bad_hash":
                    report["artifacts"][0]["sha256"] = "0" * 64
                buffer = io.BytesIO()
                with zipfile.ZipFile(buffer, "w") as archive:
                    archive.writestr("source-build-report.json", json.dumps(report))
                    for entry in report["artifacts"][:-1] if case == "missing" else report["artifacts"]:
                        archive.writestr(entry["path"], (self.root / entry["path"]).read_bytes())
                    if case in ("duplicate", "extra"):
                        archive.writestr(report["artifacts"][0]["path"] if case == "duplicate" else "extra.a", b"extra")
                key = transfer.serialization.load_pem_public_key(self.public.read_bytes())
                secret = transfer.AESGCM.generate_key(bit_length=256)
                nonce = os.urandom(12)
                wrapped = key.encrypt(secret, transfer.oaep())
                header = (transfer.MAGIC + transfer.struct.pack(">H", len(wrapped)) +
                          bytes.fromhex(transfer.sha(transfer.public_der(key))) + nonce + wrapped)
                path = self.root / "invalid.enc"
                path.write_bytes(header + transfer.AESGCM(secret).encrypt(nonce, buffer.getvalue(), header))
                self.run_cli("open", "--private-key", self.private, "--input", path,
                             "--output", self.root / "invalid.zip", success=False)
                self.assertFalse((self.root / "invalid.zip").exists())


if __name__ == "__main__":
    unittest.main()
