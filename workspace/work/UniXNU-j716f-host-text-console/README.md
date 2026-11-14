# J716F independent XNU text-console host build

Derived from ../UniXNU-j716f-host-dt-panic; iPadOS 17.7.11/21H461, ordinary arm64, CPU0 and 4 KiB.
The only new XNU source change is the PE_create_console -v text-mode hunk from Denis UniXNU-j716f bf2d0f32f706adbe414551e452f1426ed7c4bd19. No ISB/VFS/driver/SMP/memory expansion is included. CONFIG_XNU_DT_PANIC_REGION=y remains selected.

Keep the five components, eight archive recipes and original 58 patches, followed by one board console patch. Build in UniXNU/out-host-text-console-20261113 with scripts/host.py via the official defconfig/fetch/status/tree/build kernel. Original kernelcache remains pinned. Fresh source tree, SDK, compilation records, archives and native/prelinked kernels; only validated source caches may be reused.

UniXNU current revision 7d85ede80092050f4d87f72ac7229bb605ce07fb has no scripts/profiles/component-catalog changes from the prior host build revision dff276535db3ac394f628cec58a718cd46c20ac2. Existing local menu/bootimg changes are retained separately. Re-derive all native/prelinked symbols and diagnostic pins after build.

This target builds a kernel only. The later independently identified r21 image will keep the r20 InitIOKit-return checkpoint, r9 FD and -v serial=3, subject to completed acceptance. No build success is inferred from source preparation. No QEMU or device access from this target. Historical provenance/baseline documents copied from the parent remain input records, not this target result.
