# TB-J716F ArmCpu CPU0 separator integration

Independent override of ArmCpu d90e457b8392623853ec85c860feef546c767621.
Only six existing CPU0 templates replace semicolon separators with explicit
\n\t: counter read, SRE, Group1 enable, EOI, virtual timer control, and Group1
disable. Instruction operands, constraints, functions and component identities
are unchanged. The compiler must emit every intended instruction; source edits
alone do not qualify the result.

No J716FCpu0Diag headers, workers, SGI/selfprobe changes or secondary cache
maintenance repairs are included. Existing unselected paths retain their known
limitations. The source diff and exact input inventory reside in the independent
host-iokit-cpu0 target's docs. No hardware result transfers from another kernel.
