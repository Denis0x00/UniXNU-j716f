# Independent IOKit CPU0 host build

Derived from the accepted host-early-log source target. Its 60 patches, early RAM
logging, text console, DT panic fix, 4 KiB configuration, five components and eight
archive recipes are retained. The ArmCpu local-source override contains exactly
six reviewed CPU0 asm-separator repairs, without diagnostic worker or SGI changes.

Use analysis/tools/run_host_iokit_cpu0.py prepare, then kernel, on the prepared
Linux host. scripts/host.py invokes the official defconfig/fetch/tree/build kernel
workflow. Only source caches are reused; old objects and compiled records are not
inputs. Independent output: UniXNU/out-host-iokit-cpu0-20261114.

Host build and structural verification do not establish execution. New native and
prelinked machine code, early-log ABI and r26 checkpoint addresses must be audited
before image creation. This target performs no QEMU or tablet operations.
