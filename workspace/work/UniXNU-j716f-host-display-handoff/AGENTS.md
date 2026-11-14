# Isolated current-boot display handoff target

Read README.md and board.toml. Official kernel builds only through scripts/host.py.
Keep the original 58 patches and eight recipes, frozen ArmCpu and frozen GOP
provider unchanged; add only the documented current-boot integration patch
and archive caller. Record source identities before each build and preserve
failed attempts. No pixel maps/writes, display service, scanout/GPU, payload or
device actions. A bootarg/DT description is not an ownership grant. All 14
areas remain planned and deployed r6 is unchanged.
