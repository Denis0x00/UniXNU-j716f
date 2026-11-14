# Local SM8250 transition source

Copied from `Denis0x00/UniXNU-j716f`, commit
`bf2d0f32f706adbe414551e452f1426ed7c4bd19`, subdirectory `socs/sm8250`.
The source is explicitly selected by `local = "socs/sm8250"` in feeds.toml.
This is a local preparation snapshot, not a separately qualified shared SoC repository.

Current hardware evidence is Android runtime FDT from batch `20261111-231708`,
**DTB0 / DTBO1**, with eight CPU affinities `0x000..0x700`, 19.2 MHz timer,
GICD `0x17a00000/0x10000`, GICR `0x17a60000/0x100000`, stride `0x20000`.
The original `soc.toml` claim `DTBO8` does not describe this boot.

Local changes: source metadata updated; the generic timer node is renamed from
`timer@17a00000` to `timer`, the fabricated GIC-range timer `reg` is removed,
and the four PPI tuples from the runtime FDT are restored. Generic timer routing
still requires review against the selected ArmCpu ABI before actual boot.
Original and current per-file hashes are in `../../docs/source-provenance.json`.

`configs/sm8250.config` remains a placeholder. `kits/Sm8250Platform` contains
legacy UART/register and SMMU helpers; nothing in the current minimal selection
links those helpers. The static inline SMMU lock deserves review for whether
all translation units truly share one lock before reuse.

`patches/xnu/series` and its SMP/Edge patches are retained for review, but the
board's active patch_series intentionally excludes this SoC series. It also
references an upstream power patch. Qualify patch applicability, per-core
interrupt/timer behavior and the 4+3+1 hardware topology before enabling it.
No SMP, UART, power, SMMU, or runtime verification is claimed by this copy.
