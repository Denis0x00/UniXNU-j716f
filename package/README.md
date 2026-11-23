# TB-J716F Board Package (`package/` -> `kits/j716f/`)

During `./unixnu build package`, UniXNU mirrors files under `package/` into
`kits/j716f/` inside the assembled build tree:

- `package/storage/`: Board read-only `SHNGROOT` UFS window storage bring-up
  driver (`J716fStorageBringup*`, `J716fStoragePower*`, `include/`, `r41/`),
  compiled by `scripts/build-j716f-storage-active.py` into
  `libJ716fStorageBringup.a`.
- `package/storage/r41/tests/R41RootWindowModel.cpp`: Host ASan/UBSan model
  test for SHNGROOT manifest SHA-256 verification and 512-byte logical window
  slicing across 4 KiB and 128 KiB boundaries.
- `package/firmware/` (local only, git-ignored): Place your extracted
  `j716f-stock-base00-overlay01-normalized.dtb` here (or pass `--dtb` to
  `./unixnu bootimg`). Proprietary `.dtb` binaries are not tracked in Git.
