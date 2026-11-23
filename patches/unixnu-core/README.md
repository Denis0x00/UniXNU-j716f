# Required Upstream UniXNU Core Patches (Pending PRs)

Until merged into [UniXNU/UniXNU](https://github.com/UniXNU/UniXNU), two small,
self-contained core patches are required when assembling **real-hardware**
`payload.img` (ADT) and `boot.img` (Android boot header v2) for TB-J716F:

| Patch | Target | Why TB-J716F Needs It |
| --- | --- | --- |
| `0001-devicetree-emit-aapl-phandle.patch` | `scripts/unixnu/devicetree.py` | XNU `AddPHandle`/`FindPHandle` indexes ADT nodes by `AAPL,phandle`. Without it, timer/GIC interrupt lookup hits a NULL pointer abort in `IODTGetICellCounts`. |
| `0002-bootimg-support-header-version-2.patch` | `scripts/unixnu/bootimg.py`, `scripts/unixnu/test_bootimg.py` | TB-J716F SM8250 ABL requires Android boot image header v2 (1660-byte header + page-aligned `--dtb` section at `dtb_offset`). Includes offline unit tests (`test_bootimg.py`). |

## Apply to a Clean `UniXNU/UniXNU` Checkout

```bash
cd ../UniXNU
git apply ../UniXNU-j716f/patches/unixnu-core/0001-devicetree-emit-aapl-phandle.patch
git apply ../UniXNU-j716f/patches/unixnu-core/0002-bootimg-support-header-version-2.patch
python3 scripts/unixnu/test_bootimg.py -v
```

Both patches are continuously verified against `UniXNU/UniXNU@main` in
`.github/workflows/j716f-ci.yml` (`git apply`, `test_bootimg.py`, and
`AAPL,phandle` presence check in the generated ADT).

## Submitting Upstream to `UniXNU/UniXNU`

Both files carry standard `From:` / `Subject:` headers and can be imported
directly onto a fork of `UniXNU/UniXNU` via `git am`:

```bash
git am ../UniXNU-j716f/patches/unixnu-core/*.patch
```
