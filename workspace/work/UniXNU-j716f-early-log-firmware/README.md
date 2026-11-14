# Independent early-log firmware target

Build through scripts/firmware.py and the official UniXNU firmware command.
The new companion source is ../uefi-j716f-early-log, derived from the actual
r9 display-ownership source and the separate early-log overlay. Preserve old
source and artifacts. This target builds the firmware and retention-probe EFI;
image assembly and physical retention tests are recorded separately.
