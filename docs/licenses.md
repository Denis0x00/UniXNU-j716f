# License and source preservation

- The top-level `LICENSE` is copied unchanged from UniXNU
  (`BSD-2-Clause-Patent`, UniXNU contributors). Original source copyright/SPDX
  headers and component `LICENSE` files remain in their source directories.
- `ArmCpu-j716f-iokit-cpu0-20261114` and the historical CPU0 diagnostic source
  retain ArmCpu's license. Their source override does not change that license.
- `ShengUfs-sm8250` retains the ShengUfs upstream license; its local policy and
  observation code retains the source notices. Its upstream source base is
  `b6fe07b307eeddc5e258c9d5d4387b968a55e4e9`.
- J716F Goodix, touch-transaction and GENI files marked `BSD-2-Clause` retain
  that identifier. They are newly authored isolated cores; no GPL Linux driver
  object or stock firmware blob is linked or redistributed by this snapshot.
- UFS bridge/Power and early-log source files marked `BSD-2-Clause-Patent`
  retain their identifiers. Existing device and SoC source/patch notices are
  kept. A root license does not erase any per-file terms.
- XNU, corecrypto, libdispatch/libplatform/libpthread, edk2/Mu and other fetched
  dependencies keep their own licenses at the exact locked repositories. The
  exporter fetches their core references; it does not relicense them.
- Core overlays are diffs against pinned upstream source plus original local
  source tests. Apply them only to those recorded revisions for review.
- Stock ROM, original iPadOS inputs, system images, kernelcache, trust cache,
  proprietary DTBs, device identifiers and raw device logs are excluded. Their
  mention in provenance is not a redistribution grant.
