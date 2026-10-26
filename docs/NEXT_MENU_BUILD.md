# Next firmware menu build

User request (2026-10-26): Add **Reboot** and **Power off** to the XUI/UniXNU main boot menu on the next compilation. V23P10A/B/C were already built when this was requested; keep their diagnostic image identities. Before the next build, confirm supported action wiring and test both menu entries. Reboot must use the J716F-validated SYSTEM_RESET route; do not assume the old RESET2-first runtime driver is functional. Power-off support and return behavior still need source/hardware verification. This is a future build requirement, not a verified device result.

## Implemented in V23P11

Reboot and Power off are now present in all P11 FAT menus via J716fPowerAction.efi. 22 linked-app host cases and packed-menu audits pass. Direct SYSTEM_RESET/SYSTEM_OFF after fresh VERSION/FEATURES queries; unused SMC inputs zero, no RESET2. New application hardware behavior, especially SYSTEM_OFF, remains untested. See V23P11_COMMIT_PUBLICATION.md.
