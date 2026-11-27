# UniXNU-j716f Repository Guidelines

This is the official UniXNU Format 2 device repository for the Lenovo Xiaoxin
Pad Pro 2021 (`TB-J716F`, Snapdragon 870 / SM8250-AC), targeting iPadOS 17.7.11
(`21H461`).

## Key Files

- `board.toml`: Format 2 board definition (`schema_version = 2`), components,
  kernel patches/archives, firmware, device tree, userdata geometry, payload,
  and bootimg layout.
- `porting.toml`: Structured UniXNU porting record validated by
  `UniXNU/scripts/porting-record.py`. Any non-markdown file change must belong
  to an area's `paths` and update that area's `findings` or `evidence`.
- `configs/j716f_defconfig`: Default Stage 7+ desktop defconfig (`j716f_stage6_defconfig` preserves the minimal single-core APFS root baseline; `j716f_stage7_defconfig` is retained as a compatibility alias).
- `patches/unixnu-core/`: Minimal pending upstream patches applied to a clean `UniXNU/UniXNU` checkout (`0001` `AAPL,phandle` ADT alias, `0002` Android bootimg v2).
- `package/storage/`: Board read-only `SHNGROOT` UFS window driver staged into
  `kits/j716f/storage` during `./unixnu build package`.
- `socs/sm8250/`: SM8250 SoC DTSI, config, UFS HCI/SMMU handoff library, and
  C++ ASan/UBSan host tests.

## Rules

- Never commit proprietary vendor binaries (`.dtb`, `.dtbo`, `.img`, `.fd`,
  `.macho`, `.ipsw`, `.dmg`, `.dbr`), private device identities (`platform-seed`,
  `unique_chip_id`), or host-local absolute paths. See `proprietary-files.txt`.
- Keep all scripts, patches, configs, TOML, and DTS files with Unix LF endings.
  Keep `scripts/package.sh` executable (`100755`).
- Keep default `board.toml` compatible with unmodified upstream `UniXNU/UniXNU`.
- Run C++ ASan/UBSan tests, Python `scripts/ci` unit tests, and
  `porting-record.py check` before opening a PR or pushing to `main`.
