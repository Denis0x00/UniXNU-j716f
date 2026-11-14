# Independent BSD pre-root checkpoint r30

Read README.md, board.toml, source-provenance.json, kernel-expectation.json and firmware-expectation.json. Only parent controls VM/build operations. The user is away: current authorization is host preparation and acceptance only. Do not operate, flash or reboot the tablet. Keep the accepted image pending a later physical test. Do not modify frozen earlier targets, helpers, artifacts, evidence or r23 core.

Reuse pristine artifacts/j716f-host-iokit-cpu0-20261114/kernel.macho and unchanged firmware. No source-kernel rebuild, new driver or rootfs. Select unique bsd_init -> inittodr BL occurrence 1, after return, six-second delay; restore IOKitBSDInit, sysctl_mib_init, kperf_init_early, PE_lockdown_iokit and every older checkpoint. Only official payload generation may apply independently reviewed entry/paint/checkpoint regions.

Preserve actual r29 ADT hardware properties; only random-seed may vary. Keep exact FD/EFI/stock DTB/shim, CPU0/4KiB/256MiB/80MiB and -v serial=3 j716f-earlylog=1. Five manual entries, default XNU, timeout=-1. No QEMU, storage writes, installation, Android A/data changes or global matching claims.

The selected bootstrap-thread return follows the reviewed DTrace/network prefix, net_init_run and inittodr, before IOFindBSDRoot and vfs_mountroot. Normal return does not prove accurate time-of-day, network connectivity, resource publication success or dependent matching completion. Asynchronous workers are not globally stopped. No timer IRQ, full matching, root, completed bsd_init or userland claim. The later reset delay is a CNTPCT busy-wait. A later no-reset hardware result requires actual retained XNU logs before choosing the next probe.
