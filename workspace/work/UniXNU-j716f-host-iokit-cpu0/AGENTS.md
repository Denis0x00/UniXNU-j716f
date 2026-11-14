# Independent IOKit CPU0 host kernel target

Preserve the frozen host-early-log target and all its outputs. Retain its 60
patches, early-log/text-console source, five components and eight archive recipes.
The only new compiled source delta is the six reviewed ArmCpu asm separators in
the separately named local component. Do not add diagnostic workers, SGI tests,
SMP, new drivers or device actions. Run scripts/host.py for the official kernel
pipeline. Reparse all addresses and ABI contracts after the new build.
