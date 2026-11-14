# Isolated CPU0 diagnostics

This directory is an explicitly authorized diagnostic source override for ArmCpu.
Keep the production component and all frozen targets/artifacts unchanged. The base
is ArmCpu d90e457b8392623853ec85c860feef546c767621. No DTS, UFS, SMP, QEMU or tablet
actions belong to this component. No Boot Services or unqualified framebuffer
access may be added. Diagnostic modes are opt-in and CPU0-only. Compile through
the official build-arm-driver.py ArmCpu recipe in the independent resolved tree.
Treat build/test results separately from runtime observations.
