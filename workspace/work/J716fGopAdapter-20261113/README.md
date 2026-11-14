# J716F GOP resource provider, 2026-11-13

This is a CPU-only resource metadata provider for the independent
`UniXNU-j716f-host-display-adapter` kernel target. It does **not** compile
`NativeLegacyDisplay`, implement an Apple framebuffer, publish a display service,
map/copy pixels, operate GPU/DPU/DSI/backlight hardware or qualify swap completion.
The archive occupies the official legacy-display linker slot solely to supply
the two existing C initialization entries. The source has no custom metaclass,
global constructor or `__mod_init_func` transformation.

`native_legacy_display_probe` and `native_legacy_display_late_probe` are disabled
unless `j716f-gop-resource=1`. Selecting checks cannot grant resource ownership.
With the current target there is no platform reservation registration caller;
the enabled early entry therefore records `NoPlatformReservation` (8) and stops.
Late entry does not retry. Unselected entries have no observable side effects.

The future trusted platform integration API is
`j716f_gop_register_platform_reservation(IOService *, IODeviceMemory *)`.
Its caller must have already excluded the complete reservation from the current
kernel's physical allocator and must keep that reservation valid until reboot.
The current package has no such caller and does not infer this proof from a DT
property, UEFI history, address constant, boot argument or descriptor alone.
Registration retains current kernel objects, records the owner's registry ID,
rejects a second claim, and verifies exact descriptor membership and range.
It accepts no serialized pointer/token from an earlier boot. A changed registry
ID, inactive owner, replaced descriptor or changed extent is rejected again at
late entry. There is no general capability security boundary between arbitrary
trusted kernel code; the API records a platform assertion, not independent proof
of allocator exclusion or scanout ownership.

The consistency envelope is the **existing local UEFI** `MemoryMapLib.c`
`Display Reserved` range `[0x9c000000,0x9e400000)` (36 MiB), plus the inherited
GOP source's 2560x1600, 32-bit, 10240-byte stride framebuffer. These constants
are not claimed as independently recovered original ZUI DT facts. Current
`PE_current_console` must agree; nonzero offsets, rotation, bad lengths,
arithmetic overflow and changes between entries are rejected. The provider
exposes only `0xfa0000` visible bytes while retaining the full reservation.
It never claims a pixel format from PE metadata, refresh timing or scanout grant.

At fixed XNU `d8b80295118ef25ac3a784134bcf95cd8e88109f`,
[pexpert/arm/pe_init.c](https://github.com/apple-oss-distributions/xnu/blob/d8b80295118ef25ac3a784134bcf95cd8e88109f/pexpert/arm/pe_init.c#L505)
copies `PE_state.video` and returns zero. Therefore zero is the success convention;
the console snapshot still needs all independent bounds and resource checks.

With a future valid registration, the late entry also requires the actual
`IOBSD` property on the resource service. It allocates a plain IOService,
IODeviceMemory and one-element OSArray, verifies insertion, attaches/starts
the plain service, and retains only the resource on success. It neither calls
`registerService` nor attaches a `display-subsystem` DT node. Every failed path
releases local objects and registered claim references; successful attach before
failed start is detached. The reservation owner must not revoke or replace the
registered memory until reboot. Runtime suspension/hotplug is not implemented.

Build in the official resolved tree, after the native XNU C++ compile record
exists:

```sh
python3 scripts/build.py --tree /absolute/resolved/tree --source /absolute/J716fGopAdapter-20261113
python3 tests/run_host_tests.py --cxx clang++-19 --out /absolute/test-output
```

The build recipe uses the actual `IOPlatformExpert.cpo.json` compiler, target,
SDK and header flags, redirects its source/output, removes dependency and JSON
outputs, and adds `-mgeneral-regs-only`. It copies exact source inputs into a new
attempt directory. It checks ordinary arm64 MH_OBJECT / iOS 17.0, strong entry
and capability exports, no global constructors, and no legacy class imports.
It records all undefined symbols for the final official link to resolve.
`llvm-ar` is resolved beside the actual compiler (including its version suffix)
or through PATH, with `--ar` available for an explicit binary.

Outputs consumed by official `build kernel`:

* `build/native-legacy-display/libNativeLegacyDisplay.late.a`
* `build/native-legacy-display/build-report.json`

The report's `source_sha256` names the exact copied inputs relative to the tree;
upstream validates those and the archive hash before linking. Failed new attempts
retain their logs and previous products under `attempts/`, but remove stale
top-level success products. Host tests compile the **same provider .cpp** against
modeled IOKit APIs, including allocation and lifecycle failure injection. They
do not establish target C++ ABI, physical reservation, display or hardware success.
Actual compilation, official linking and hardware observations must be reported
separately in `analysis/stock-display-adapter-20261113`.
