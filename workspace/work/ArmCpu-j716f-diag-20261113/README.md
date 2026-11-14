# ArmCpu: isolated TB-J716F CPU0 diagnostics

This is a source override of ArmCpu `d90e457b8392623853ec85c860feef546c767621`,
for the independently named `UniXNU-j716f-host-cpu0-diag` build. It is not a new
hardware driver or a change to the frozen r6 candidate. No display, UFS, SMP,
device-tree or user-data operation is added.

The original `ArmCpu.cpp`, bundle identifiers and build recipe are retained.
The opt-in `j716f-cpu0-diag=1`, `2`, or `3` boot argument selects exactly one
diagnostic stop. Missing/zero means the existing driver behavior; invalid
nonzero modes, multiple CPUs or incompatible SMP/skip-GIC arguments refuse.
An atomic one-attempt latch rejects any repeated nonzero-mode start in the same
boot before resetting records; a previous IRQ latch is never reused by a retry.

The isolated override also repairs six inherited CPU0 assembly strings:
the counter read, GIC SRE and group-enable barriers, IRQ EOI barrier, timer
enable/disable barrier, and GIC group-disable barriers in quiesceCPU. Darwin
AArch64 assembly treats semicolons as comments; these now use explicit newline
separators, as do the five newly added multi-instruction helpers. These syntax
repairs also apply with mode zero. The production component remains unchanged.
Unselected SGI/selfprobe/secondary cache-flush assembly is not blindly activated;
it needs separate operand and caller review before any broader CPU/SMP work.

| Mode | Actual caller and acceptance | Observable result |
| --- | --- | --- |
| 1 | `ArmCpu::start`, before GIC preparation: CPU0 at EL1, IRQ masked while sampling; 19.2 MHz CNTFRQ; advancing physical and virtual counters with bracket-consistent deltas | Only after success, mask virtual timer, wait nominal six seconds with CNTPCT and an iteration cap, issue PSCI SYSTEM_RESET |
| 2 | CPU0 `initCPU`, after real prepareGic/enableGic, before normal timer enable: bounded mapped ranges, GICv3, matching redistributor, awake/RWP-clear, PPI27 Group1NS/level/enabled/priority 0x80, priority mask and system interface | Same guarded six-second reset, only after counter and GIC checks |
| 3 | Original IAR1=27 branch, count 0→1, enabled/unmasked/expired CNTV timer with signed TVAL≤0, `rtclock_intr(0)` returns, original EOI instruction completes | IRQ writes one record and release-publishes it. A normal kernel thread acquire-reads the latch and requests the same guarded reset after all conditions pass |

The timer deadline is never programmed by these diagnostics. The source only
masks CNTV_CTL during controlled initialization and before the selected reset;
normal `enableCPUTimeBase` and XNU scheduling retain deadline control. The IRQ
path performs bounded register reads and one record; it never logs, allocates,
waits or resets. Mode 3 starts an ordinary thread after normal CPU registration.
Its IRQ wait uses a two-second **physical-counter** deadline plus 100 million
iterations, not a timer interrupt or scheduler sleep. If the thread is not
scheduled, no result is observable; that is not proof of timer failure.

The original `waitClear` has a time-and-poll condition which can fail to finish
if its virtual counter stops. Diagnostic modes add a separate one-million-poll
limit; mode zero preserves the original logic. These bounds cannot interrupt a
hardware MMIO transaction that never completes, a synchronous trap, or an
upstream allocation/driver routine that does not return. They do not make the
whole kernel bring-up a bounded computation.

There is no framebuffer writer, assumed physical mapping, Boot Services call,
new SRAM mailbox or persistent log. Private static records reside in normal
linked kernel data. `IOLog` emits small best-effort lines from non-IRQ context,
but no available post-EBS sink is claimed. The guarded PSCI channel reports
one Boolean acceptance condition for the exact image/mode, not raw register
values. An unknown reboot, wrong delay or different image is still ambiguous.
If the reset SMC returns, the diagnostic reports failure and retains the virtual
source masked. A failure never intentionally invokes SYSTEM_RESET.

All three modes require execution to have reached ArmCpu during IOKit startup.
**The frozen r6 payload stops after fiq_context_init before IOKit and cannot run
these callers.** A future separate payload must use the newly linked kernel,
omit the old early-reset stop and select one mode explicitly. New symbols,
addresses, hashes, payload and image must be verified independently. No old
physical result transfers to this source or any new image.

## Official build integration

Use this entire directory as the ArmCpu source override in the independent
device's `feeds.toml`:

```toml
[[source]]
name = "ArmCpu"
kind = "driver"
local = "../ArmCpu-j716f-diag-20261113"
```

Keep the existing host-dt-panic baseline's eight archive recipes, selected
patch series, original ABI input and 4 KiB configuration. The official kernel
pipeline invokes `scripts/build-arm-driver.py ArmCpu` using the prepared
XNU `IOPlatformExpert.cpo.json` ABI command and audits the resulting ARM64
object. Run official defconfig/status/build-kernel in the new target only.
Do not copy these sources over the production component or frozen build tree.

Tests and provenance are in `analysis/cpu0-diagnostics-20261113` at workspace
root: `test_core.cpp`, `verify_integration.py`, `prepare_source.py`, patch and
source manifest. The portable test compiles the **same core header**, including
counter consistency, GIC readback, real IRQ evidence, reset acceptance and
non-IRQ timeout logic. The source verifier tests real hook ordering and rejects
mutations that would bypass the reset guard, timer expiry or EOI requirement.

中文范围：这是独立候选中的实际 ArmCpu 诊断实现，编译/主机测试不等于实机通过。
计数器采样点在 ArmCpu::start，已经位于 IOKit 阶段；它没有取代 r6 的早期探针。
各模式只有条件完整成立后才主动复位。没有复位可能是尚未到达 caller、失败关闭、
线程未调度、异常或复位不可用，不能单凭此现象定位故障。没有可导出的 post-EBS
寄存器日志；读回值只参与专用条件判断，普通静态变量不被当作可获取日志。
