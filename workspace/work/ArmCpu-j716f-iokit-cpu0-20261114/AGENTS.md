# CPU0 assembly separator integration

This independent ArmCpu source override contains only six approved asm template
separator repairs over d90e457b8392623853ec85c860feef546c767621. Preserve all
other source bytes and identifiers. No diagnostic headers/workers, SGI/selfprobe
or secondary cache-maintenance edits. Use the official ArmCpu build recipe via
the host-iokit-cpu0 target. New kernel machine-code and runtime evidence are
required; prior diagnostic builds are not proof of this candidate.
