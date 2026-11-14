# Independent host UFS adapter kernel

Read README.md and board.toml. Use scripts/host.py for the official host steps.
This new target preserves the DT panic base inputs, five components, all eight
original archive recipes, and adds one adapter archive plus two device patches.
The selected patch count is 60 (58 original plus two device-local patches).
Only this target/output may be changed; preserve all prior targets and results.
Use a fresh official tree. Never copy objects, SDKs or compiler records.
Fixed source caches may be reused after revision validation.
Retain genuine bootstrap, archive, native link and prelink; never skip prelink.
Compile exactly Runtime, Hci, Handoff and one AppsSmmuOwner implementation.
Freeze source/header inputs before kernel build; no live main-board source links.
The BSD init caller only registers the adapter after successful IOKitBSDInit.
Hardware access requires an external caller and genuine verified evidence.
Do not infer hardware readiness, root mount or device success from host builds.
No QEMU, firmware/image assembly, tablet partition writes or device actions.
