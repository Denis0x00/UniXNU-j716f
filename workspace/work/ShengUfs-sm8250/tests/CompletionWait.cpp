// SPDX-License-Identifier: BSD-2-Clause-Patent
// The completion wait policy (CompletionWait.h) driven through pollUfsTransfer as the kernel port drives it: the port's delay asks
// ufsWaitStep what to do, a spin advances the clock by one event-stream period, a pause by an observed blocking pause, a sleep until
// the completion interrupt or its timeout.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "../TransferPoll.h"
#include "../CompletionWait.h"

struct Clock {
  uint64_t now = 0;
};

// One command that completes `completeAt` us after the wait began. Times are in microseconds; the spin budget is spinUs.
struct WaitPort {
  Clock &clock;
  uint32_t mode;
  bool interruptMode = true, canBlock = true;
  uint64_t completeAt = 0, spinUs = 200, pauseUs = 37, wakeUs = 36, deadline = 1000000;
  uint64_t preemptFrom = ~0ULL;   // a preemption is pending from this time on
  unsigned spins = 0, pauses = 0, sleeps = 0;
  uint32_t doorbell () { return clock.now < completeAt ? 1U : 0U; }
  uint32_t interrupt () { return 0; }
  uint32_t completedOcs () { assert (clock.now >= completeAt); return 0; }
  bool expired () { return clock.now >= deadline; }
  void delay ()
  {
    const bool preempt = canBlock && clock.now >= preemptFrom;
    switch (ufsWaitStep (mode, interruptMode, canBlock, preempt, clock.now, spinUs)) {
    case UfsWaitStep::Spin: ++spins; clock.now += 1; break;
    case UfsWaitStep::Pause: ++pauses; clock.now += pauseUs; break;
    case UfsWaitStep::Sleep:
      ++sleeps;
      // The interrupt ends the sleep, or its 1 ms timeout does.
      if (clock.now < completeAt) clock.now = completeAt - clock.now > 1000 ? clock.now + 1000 : completeAt + wakeUs;
      break;
    }
  }
};

static uint64_t run (WaitPort &port)
{
  UfsTransferPoll state;
  assert (pollUfsTransfer (port, state) == UfsTransferEnd::Completed);
  return port.clock.now;
}

int main ()
{
  // Spin (default): a 95 us command is seen within one spin pause of completing, with no block.
  { Clock c; WaitPort p { c, kUfsWaitSpin }; p.completeAt = 95;
    assert (run (p) == 95 && p.sleeps == 0 && p.pauses == 0 && p.spins == 95); }
  // Paced (earlier behaviour): the same command takes three blocking pauses and is seen up to a pause late.
  { Clock c; WaitPort p { c, kUfsWaitPaced }; p.completeAt = 95;
    assert (run (p) == 111 && p.pauses == 3 && p.spins == 0 && p.sleeps == 0); }
  // Sleep: one sleep, seen a wake latency late.
  { Clock c; WaitPort p { c, kUfsWaitSleep }; p.completeAt = 95;
    assert (run (p) == 131 && p.sleeps == 1 && p.spins == 0 && p.pauses == 0); }
  // A command longer than the spin budget: spin the budget, then one sleep.
  { Clock c; WaitPort p { c, kUfsWaitSpin }; p.completeAt = 900;
    assert (run (p) == 936 && p.spins == 200 && p.sleeps == 1); }
  // A pending preemption ends the spin at once: the CPU goes to the other thread and the wait sleeps.
  { Clock c; WaitPort p { c, kUfsWaitSpin }; p.completeAt = 95; p.preemptFrom = 10;
    assert (run (p) == 131 && p.spins == 10 && p.sleeps == 1); }
  // A zero budget sleeps from the first pause in both polling modes.
  { Clock c; WaitPort p { c, kUfsWaitSpin }; p.completeAt = 95; p.spinUs = 0;
    assert (run (p) == 131 && p.spins == 0 && p.sleeps == 1); }
  { Clock c; WaitPort p { c, kUfsWaitPaced }; p.completeAt = 95; p.spinUs = 0;
    assert (run (p) == 131 && p.pauses == 0 && p.sleeps == 1); }
  // Without the interrupt, with interrupts masked or with preemption disabled, a wait never sleeps or spins: IODelay pauses only.
  for (unsigned mode = 0; mode <= 2; ++mode) {
    { Clock c; WaitPort p { c, mode }; p.completeAt = 900; p.interruptMode = false;
      run (p); assert (p.sleeps == 0 && p.spins == 0 && p.pauses == 25); }
    { Clock c; WaitPort p { c, mode }; p.completeAt = 900; p.canBlock = false; p.preemptFrom = 0;
      run (p); assert (p.sleeps == 0 && p.spins == 0 && p.pauses == 25); }
  }
  // An unknown mode value polls like Spin; the driver only accepts 0 to 2.
  assert (ufsWaitStep (7, true, true, false, 0, 200) == UfsWaitStep::Spin);
  // A command that never completes still ends at its deadline, whatever the mode.
  for (unsigned mode = 0; mode <= 2; ++mode) {
    Clock c; WaitPort p { c, mode }; p.completeAt = ~0ULL; p.deadline = 5000;
    UfsTransferPoll state;
    assert (pollUfsTransfer (p, state) == UfsTransferEnd::Timeout);
  }
  // Histogram ranges.
  assert (ufsWaitRange (0) == 0 && ufsWaitRange (49) == 0 && ufsWaitRange (50) == 1 && ufsWaitRange (99) == 1);
  assert (ufsWaitRange (100) == 2 && ufsWaitRange (199) == 2 && ufsWaitRange (200) == 3 && ufsWaitRange (399) == 3);
  assert (ufsWaitRange (400) == 4 && ufsWaitRange (~0ULL) == 4 && kUfsWaitRanges == 5);
  puts ("PASS: spin, paced, sleep, long command, preemption, zero budget, no interrupt, masked, timeout and wait ranges");
}
