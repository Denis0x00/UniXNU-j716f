# SPDX-License-Identifier: BSD-2-Clause-Patent
import importlib.util
import struct
import unittest
import uuid
from pathlib import Path


def _load_packer_module():
    script_path = Path(__file__).resolve().parents[1] / "pack-j716f-gpu-addon.py"
    spec = importlib.util.spec_from_file_location("pack_j716f_gpu_addon", script_path)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


PACK = _load_packer_module()


def _make_trust_module(cdhashes: list[bytes], *, version: int = 2, hash_type: int = 2) -> bytes:
    ident = uuid.uuid5(uuid.NAMESPACE_URL, "test:" + "".join(h.hex() for h in cdhashes))
    return struct.pack("<I16sI", version, ident.bytes, len(cdhashes)) + b"".join(
        h + bytes([hash_type, 0, 0, 0]) for h in cdhashes
    )


def _make_bundle(modules: list[bytes]) -> bytes:
    cursor = 4 + 4 * len(modules)
    starts = []
    for mod in modules:
        starts.append(cursor)
        cursor += len(mod)
    return struct.pack(f"<{1 + len(modules)}I", len(modules), *starts) + b"".join(modules)


class PackGpuAddonTests(unittest.TestCase):
    def _sample_orig_bundle(self) -> bytes:
        m0 = _make_trust_module([bytes([0x10]) * 20, bytes([0x20]) * 20])
        m1 = _make_trust_module([bytes([0x30]) * 20])
        return _make_bundle([m0, m1])

    def test_build_owner_trust_bundle_sorts_and_validates(self) -> None:
        orig = self._sample_orig_bundle()
        self.assertTrue(PACK.validate_trust_bundle(orig))
        meta = {
            "cdhash": "cc" * 20,
            "daemon_cdhash": "aa" * 20,
            "turnip_cdhash": "bb" * 20,
        }
        bundle = PACK.build_owner_trust_bundle(orig, meta, expected_orig_sha256=None)
        self.assertTrue(PACK.validate_trust_bundle(bundle))
        count = struct.unpack_from("<I", bundle, 0)[0]
        self.assertEqual(count, 3)
        start0 = struct.unpack_from("<I", bundle, 4)[0]
        entries0 = struct.unpack_from("<I", bundle, start0 + 20)[0]
        self.assertEqual(entries0, 3)
        # Entries must be strictly sorted lexicographically for DarwinBoot binary search.
        first = bundle[start0 + 24 : start0 + 44]
        second = bundle[start0 + 48 : start0 + 68]
        third = bundle[start0 + 72 : start0 + 92]
        self.assertEqual([first, second, third], [bytes([0xAA]) * 20, bytes([0xBB]) * 20, bytes([0xCC]) * 20])

    def test_validate_trust_bundle_rejects_unsorted_and_bad_version(self) -> None:
        unsorted = _make_bundle([_make_trust_module([bytes([0xBB]) * 20, bytes([0xAA]) * 20])])
        self.assertFalse(PACK.validate_trust_bundle(unsorted))
        bad_ver = _make_bundle([_make_trust_module([bytes([0xAA]) * 20], version=1)])
        self.assertFalse(PACK.validate_trust_bundle(bad_ver))
        bad_type = _make_bundle([_make_trust_module([bytes([0xAA]) * 20], hash_type=0)])
        self.assertFalse(PACK.validate_trust_bundle(bad_type))

    def test_build_owner_trust_bundle_rejects_sha_mismatch_and_duplicates(self) -> None:
        orig = self._sample_orig_bundle()
        meta = {"cdhash": "aa" * 20, "daemon_cdhash": "bb" * 20, "turnip_cdhash": "cc" * 20}
        with self.assertRaises(ValueError):
            PACK.build_owner_trust_bundle(orig, meta, expected_orig_sha256="00" * 32)
        dup_meta = {"cdhash": "aa" * 20, "daemon_cdhash": "aa" * 20, "turnip_cdhash": "cc" * 20}
        with self.assertRaises(ValueError):
            PACK.build_owner_trust_bundle(orig, dup_meta, expected_orig_sha256=None)

    def test_validate_gpu_addon_header(self) -> None:
        payload = b"NGPUADD2" + struct.pack("<IIQ", 2, 24, 32) + b"\x00" * 8
        info = PACK.validate_gpu_addon(payload)
        self.assertEqual(info, {"version": 2, "header_len": 24, "total_len": 32})
        with self.assertRaises(ValueError):
            PACK.validate_gpu_addon(b"BADMAGIC" + payload[8:])
        with self.assertRaises(ValueError):
            PACK.validate_gpu_addon(payload + b"\x00")
        with self.assertRaises(ValueError):
            PACK.validate_gpu_addon(b"short")

    def test_compute_payload_size_mib_clamps_bounds(self) -> None:
        mib = 1024 * 1024
        self.assertEqual(PACK.compute_payload_size_mib(50 * mib, 1 * mib, 1 * mib, 4096), 80)
        self.assertEqual(PACK.compute_payload_size_mib(54 * mib, 2 * mib, 22 * mib, 65536), 86)
        self.assertEqual(PACK.compute_payload_size_mib(60 * mib, 5 * mib, 30 * mib, 65536), 88)

    def test_default_phase4b_kernel_args_modes(self) -> None:
        default_args = PACK.default_phase4b_kernel_args()
        self.assertIn("data-reset=1", default_args)
        self.assertIn("data-fua=1", default_args)
        self.assertNotIn("fb-gpu=1", default_args)
        self.assertNotIn("j716f-data-persist=2", default_args)

        persist_args = PACK.default_phase4b_kernel_args(
            ["custom=1"], fb_gpu=True, data_persist=2, data_reset=False
        )
        self.assertIn("j716f-data-persist=2", persist_args)
        self.assertIn("fb-gpu=1", persist_args)
        self.assertIn("custom=1", persist_args)
        self.assertNotIn("data-reset=1", persist_args)

        reset_args = PACK.default_phase4b_kernel_args(data_persist=2, data_reset=True)
        self.assertIn("j716f-data-persist=2", reset_args)
        self.assertIn("data-reset=1", reset_args)

    def test_resolve_stage7_out_dir_prefers_default_j716f_with_fallback(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            core = Path(tmp)
            s7 = core / "out/j716f/ipados-17.7.11-21H461/j716f_stage7/kernel"
            s7.mkdir(parents=True)
            (s7 / "kernel.macho").write_bytes(b"k7")
            self.assertEqual(PACK.resolve_stage7_out_dir(core), s7.parent)

            dflt = core / "out/j716f/ipados-17.7.11-21H461/j716f/kernel"
            dflt.mkdir(parents=True)
            (dflt / "kernel.macho").write_bytes(b"kd")
            self.assertEqual(PACK.resolve_stage7_out_dir(core), dflt.parent)
            self.assertEqual(PACK.resolve_stage7_out_dir(core, "j716f_stage7"), s7.parent)

    def test_parse_args_supports_boot_menu_theme_override(self) -> None:
        base = [
            "--unixnu-core", "/tmp/core",
            "--config", "/tmp/cfg.toml",
            "--addon", "/tmp/addon.bin",
            "--trust-json", "/tmp/trust.json",
            "--orig-trust", "/tmp/orig.bin",
            "--data-ramdisk", "/tmp/data.dbr",
            "--dtb", "/tmp/dtb.dtb",
        ]
        self.assertIsNone(PACK.parse_args(base).boot_menu_theme)
        self.assertEqual(
            PACK.parse_args(base + ["--boot-menu-theme", "oc-like"]).boot_menu_theme,
            "oc-like",
        )
        self.assertEqual(
            PACK.parse_args(base + ["--boot-menu-theme", "default"]).boot_menu_theme,
            "default",
        )
        self.assertEqual(PACK.parse_args(base).menu_timeout, 1)
        self.assertEqual(PACK.parse_args(base + ["--menu-timeout", "0"]).menu_timeout, 0)
        self.assertEqual(PACK.parse_args(base + ["--menu-timeout", "-1"]).menu_timeout, -1)

    def test_replace_payload_timeout(self) -> None:
        sample = "[payload]\nsize_mib = 80\ntimeout = -1\ntools = true\n"
        updated, old = PACK._replace_payload_timeout(sample, 1)
        self.assertEqual(old, -1)
        self.assertIn("timeout = 1\n", updated)
        same, prev = PACK._replace_payload_timeout(updated, 1)
        self.assertEqual(prev, 1)
        self.assertEqual(same, updated)

    def test_strip_board_kernel_args(self) -> None:
        sample = (
            'kernel_args = [\n'
            '  "serial=3",\n'
            '  "data-reset=1",\n'
            '  "data-fua=1",\n'
            '  "aks=1",\n'
            ']\n'
            'confirm_args = ["data-reset=1", "data-reset=2"]\n'
        )
        updated, removed = PACK._strip_board_kernel_args(sample, ("data-reset", "data-fua"))
        self.assertEqual(removed, ["data-reset=1", "data-fua=1"])
        self.assertNotIn('  "data-reset=1",\n', updated)
        self.assertNotIn('  "data-fua=1",\n', updated)
        self.assertIn('confirm_args = ["data-reset=1", "data-reset=2"]\n', updated)
        self.assertIn('  "aks=1",\n', updated)


if __name__ == "__main__":
    unittest.main()
