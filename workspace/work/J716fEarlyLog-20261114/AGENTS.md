# J716F early log protocol

This directory is a new board-private, offline implementation. Preserve all
existing firmware, device targets, images and evidence. Do not operate the VM
or tablet from a subagent; the parent owns builds and device work.

The public protocol is `include/J716fEarlyLog.h`. Only source 1 (UEFI retention
probe) and source 2 (early XNU) exist. Fixed physical region is
`0xB0000000+0x80000`, from TB_J716F_ZUI_15.0.485/runtime evidence. The writer
touches only the first 30848 bytes; the existing display-owner tail at
`0xB007FFF4` must remain unchanged. Do not import another board's ramoops size,
PON reset, USB or storage path.

The same region has consecutive UEFI JLOG and XNU JXLG epochs. Existing JLOG
capacity is not reduced. SEC must validate/copy JXLG before RamLogReset and
publish an immutable <=32 KiB HOB after memory initialization; DXE exports that
copy. The shared protocol does not perform these integration operations.

No hardware retention is established by these files or their tests. Exact reset
retention, post-ExitBootServices ownership and uncached mappings remain caller
obligations. Host test results must remain distinct from device observations.

Protocol edits require the independent Python decoder and C byte-store
interruption tests to agree. Do not silently change layout or reuse a version
for incompatible semantics. A zero XOR commit is unrepresentable and must be
rejected before any write, as must sequence overflow.
