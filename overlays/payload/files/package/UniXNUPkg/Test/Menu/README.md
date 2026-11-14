# Diagnostic menu hold

`PcdUniXNUDiagnosticMenuHold` defaults to `FALSE`. For diagnostic firmware, set
`gUniXNUPkgTokenSpaceGuid.PcdUniXNUDiagnosticMenuHold|TRUE` in the platform DSC
`[PcdsFixedAtBuild]`. This is a firmware policy; a loaded `unixnu.toml` cannot
override it. It covers the built-in menu, loaded payload menus, re-entry after a
payload returns, and the Tools submenu.

With the PCD enabled, the highlight stays still and the menu requires an explicit
selection. No timeout, missing input, input error, or timer/event failure selects
the default entry. The loop calls `Stall(100000)` before each input poll; it does
not create timer events, use `WaitForKey`, or enumerate physical input handles.
The initial `input` log field reports whether a `ConIn` callback is available.
It rechecks `ConIn` each time so an input protocol that becomes available later
can still work. Arrow keys, j/k,
Home/End and the usual explicit selection keys retain their existing behavior.
Escape is not an exit request. An explicit Return to firmware, Restart, Power
off, XNU, or Tools selection still runs normally.

This mode does not change driver injection, the selected entry's behavior,
confirmation dialogs, or a plugin's own event loop. It relies on functional UEFI
`Stall` and `ReadKeyStroke` services and does not guarantee that an input driver
works on hardware. With no working input, returning to Android requires the
device's physical reset/bootloader recovery procedure. The firmware watchdog is
already disabled by `UniXNURun`; this patch adds no reset or storage operation.

The older `PcdUniXNUBuiltinMenuWaitForInput` retains its original meaning:
built-in settings only, with the ordinary no-input fallback timer. It is not an
alternative to the diagnostic hold policy. All default behavior remains intact
when the new PCD is `FALSE`.

Run the focused host test on a machine with Python 3 and GCC or Clang:

```sh
python3 package/UniXNUPkg/Test/Menu/test_menu.py --cc gcc --sanitize
```

The runner compiles the actual prefix of `Menu.c` through `UniXNUMenuRun`, only
replacing its platform include with a fake UEFI surface. The tests simulate absent
or late consoles, input/event failures, explicit actions, and ordinary timeout
regressions. A bounded harness aborts endless waits after 12 simulated seconds;
this exceeds the test fallback timeout and demonstrates that hold does not
return automatically. It also asserts one 100 ms `Stall` per poll, no event API
or physical-input enumeration calls in hold mode, no idle highlight rotation,
and the PCD's disabled default. The rendering-call contract also checks a complete
initial frame and status line 1 before the first hold poll, no status-line-0 call
at all during hold (including empty-string clears), an unchanged Tools default,
and the ordinary mode's retained countdown/manual status prompt. These checks
exercise call order and counts; they do not render or inspect pixels.
Pixel rendering, actual elapsed time, device input, and plugin execution require the
real firmware/device test. This suite is a host test, not a QEMU test.
