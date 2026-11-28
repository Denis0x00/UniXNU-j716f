# SPDX-License-Identifier: BSD-2-Clause-Patent
import importlib.util
import plistlib
import struct
import unittest
from pathlib import Path


def _load_overlay_module():
    script_path = Path(__file__).resolve().parents[1] / "pack-j716f-wifi-overlay.py"
    spec = importlib.util.spec_from_file_location("pack_j716f_wifi_overlay", script_path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


MOD = _load_overlay_module()


class PackWifiOverlayTests(unittest.TestCase):
    def test_ngpuadd2_roundtrip_and_validation(self) -> None:
        entries = [
            ("etc/wifi/wpa_supplicant.conf", 0o644, b"ap_scan=1\n"),
            ("wifi-bin/unixnu-wpa_supplicant", 0o755, b"\xcf\xfa\xed\xfe" + b"\x00" * 64),
        ]
        blob = MOD.pack_ngpuadd2(entries)
        self.assertEqual(blob[:8], b"NGPUADD2")
        decoded = MOD.unpack_ngpuadd2(blob)
        self.assertEqual(decoded, entries)

        with self.assertRaises(ValueError):
            MOD.unpack_ngpuadd2(b"BADMAGIC" + blob[8:])
        with self.assertRaises(ValueError):
            MOD.unpack_ngpuadd2(blob + b"\x00")
        with self.assertRaises(ValueError):
            MOD.pack_ngpuadd2([("/abs/path", 0o644, b"x")])
        with self.assertRaises(ValueError):
            MOD.pack_ngpuadd2([("a/../b", 0o644, b"x")])

    def test_detached_codesig_structure_and_determinism(self) -> None:
        payload = b"hello world" * 600  # spans 2x 4 KiB pages
        sig1, cdh1 = MOD.build_detached_codesig(payload, "org.unixnu.wifi.launchd-cache")
        sig2, cdh2 = MOD.build_detached_codesig(payload, "org.unixnu.wifi.launchd-cache")
        self.assertEqual(sig1, sig2)
        self.assertEqual(cdh1, cdh2)
        self.assertEqual(len(cdh1), 40)
        magic, total_len, blob_count = struct.unpack_from(">III", sig1, 0)
        self.assertEqual(magic, 0xFADE0CC0)
        self.assertEqual(total_len, len(sig1))
        self.assertEqual(blob_count, 3)

        # Different payload must change CDHash.
        _, cdh3 = MOD.build_detached_codesig(payload + b"!", "org.unixnu.wifi.launchd-cache")
        self.assertNotEqual(cdh1, cdh3)

    def test_merge_launchd_cache_bytes(self) -> None:
        orig_dict = {
            "VersionNumber": 7,
            "LaunchDaemons": {
                "/System/Library/LaunchDaemons/com.apple.wifid.plist": {"Label": "com.apple.wifid"},
            },
        }
        orig_bytes = plistlib.dumps(orig_dict, fmt=plistlib.FMT_BINARY)
        merged_bytes, orig_count = MOD.merge_launchd_cache_bytes(orig_bytes)
        self.assertEqual(orig_count, 1)

        merged = plistlib.loads(merged_bytes)
        self.assertEqual(len(merged["LaunchDaemons"]), 2)
        self.assertIn("/System/Library/LaunchDaemons/com.apple.wifid.plist", merged["LaunchDaemons"])
        job = merged["LaunchDaemons"][MOD.SUPPLICANT_JOB_KEY]
        self.assertEqual(job["Label"], "org.unixnu.wifi.supplicant")
        self.assertEqual(job["ProgramArguments"][0], "/usr/libexec/unixnu-wpa_supplicant")
        self.assertTrue(job["RunAtLoad"])
        self.assertTrue(job["KeepAlive"])

        # Reject double injection.
        with self.assertRaises(ValueError):
            MOD.merge_launchd_cache_bytes(merged_bytes)

        # Reject unexpected schema version.
        bad_ver = plistlib.dumps({"VersionNumber": 6, "LaunchDaemons": {}}, fmt=plistlib.FMT_BINARY)
        with self.assertRaises(ValueError):
            MOD.merge_launchd_cache_bytes(bad_ver)


if __name__ == "__main__":
    unittest.main()
