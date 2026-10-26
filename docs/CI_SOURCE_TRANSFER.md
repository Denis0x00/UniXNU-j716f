# Source build CI repair (2026-10-26)

## Observed failure

[Run 40348196149](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/40348196149)
at device revision `e104fe3b59861e8e42d198c06a87a2be1805c599` completed
the source build step successfully, then failed in encrypted output transfer.
Run 40345424549 has the same failed step. Detailed authenticated logs were not
available during this review; the public Jobs API provides the step outcomes.

The native platform build report contains the four baseline outputs plus
`build/armcpu/libArmCpu.a` and `build/armdt-platform/libArmDTPlatform.a`.
The previous transfer script required exactly the four baseline outputs.
A synthetic native report reproduces its `Unexpected build output manifest`
failure locally. Its decrypt path also required the old four-file list.

## Fix and verification

- Select an exact four/six-file allowlist using `native_arch_timer_requested`.
  Older baseline reports without that flag remain readable.
- Use the same profile validation when sealing and opening an archive; reject
  duplicate, missing or extra paths and retain size/SHA256 verification.
- Run synthetic round-trip and rejection tests in an Ubuntu job before the
  macOS source build. No real source outputs or private transfer keys enter tests.
- Collect text diagnostics after encrypted transfer so that its log is included.
- Keep the pinned UniXNU/component revisions and device diagnostic inputs intact.

```sh
python3 -m pip install cryptography==46.0.6
python3 scripts/test-source-artifact-transfer.py -v
```

Five test methods pass locally, including both current profiles, a legacy report,
modified input/ciphertext, output overwrite prevention, malformed manifests and
authenticated archives with invalid contents. Remote end-to-end verification is
pending the run triggered by this repair. Source compilation/linking and encrypted
artifact upload do not establish prelink, firmware boot or timer IRQ operation.

## Companion firmware repository

[UEFI run 40348220735](https://github.com/Denis0x00/uefi-j716f/actions/runs/40348220735)
passed at `7117a2b5`. Its old all-device build failed with exit 126 in
[run 40097309657](https://github.com/Denis0x00/uefi-j716f/actions/runs/40097309657).
Commit `fe29c4be697b1bac32ff75ea51dfaf9cc560c049` restored Git submodule links
and executable modes, and separated J716F source checks from the manual
upstream all-device builder. Current checks enforce those modes and pass.
They do not compile a complete J716F firmware volume; private board inputs are
still required. The old Dependabot failure is a separate dependency update task,
with no detailed public error beyond its updater failure annotation.
