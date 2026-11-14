# Independent early XNU log host target

Derived from the accepted r21 text-console host source target. Keep its original
59 patches, then append the J716F early log integration patch when reviewed.
This scaffold is not a build result. Same CPU0, ordinary arm64, 4 KiB, DT panic
fix, five components and eight archive recipes. No new device drivers.

Use scripts/host.py with the official defconfig/fetch/tree/build kernel workflow.
Only validated source caches may be seeded. Old objects, SDK and compilation
records are not reused. Fresh output: UniXNU/out-host-early-log-20261114.
The fixed 0xB0000000/512 KiB Boot Log is mapped only under a strict board opt-in.
Host tests do not establish retained RAM across physical reset.
