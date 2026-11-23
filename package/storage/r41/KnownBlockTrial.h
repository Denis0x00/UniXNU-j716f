// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J716F_KNOWN_BLOCK_TRIAL_H
#define J716F_KNOWN_BLOCK_TRIAL_H
#include "J716fStorageBringup.h"

namespace j716f_first_block {
constexpr uint8_t kCandidateLun = 4;
constexpr uint64_t kLba = 155466;
constexpr size_t kBytes = 4096;
extern const uint8_t reference[kBytes];
extern const char reference_sha256[];
enum class Error : unsigned {
  None, Busy, AlreadyAttempted, WrongStage, InvalidCapacity,
  ReadFailed, Mismatch, StopFailed,
};
struct Result {
  Error error = Error::None;
  j716f_storage::Error bridge_error = j716f_storage::Error::None;
  j716f_storage::Error stop_bridge_error = j716f_storage::Error::None;
  bool read_attempted = false;
  bool reference_matched = false;
  bool stop_attempted = false;
  bool stopped = false;
};
struct Trial { unsigned busy = 0; bool attempted = false; };

// Configure only the single permitted reference block. This neither supplies
// access/handoff evidence nor claims that candidate LUN4 has been discovered.
void setKnownBlockRequest(sm8250_ufs::RuntimeStart &);

// Called once by the serialized board owner after real allocate/activate.
// State, Trial, output and Report must be distinct retained kernel-owned
// objects; do not concurrently use the bridge through another caller.
// No allocation, activation, authority creation, reinitialization or retry.
// A pre-submission policy refusal or invalid measured capacity stops the still
// active queue. Other transport failures leave the retained runtime untouched.
Result readVerifyAndStop(Trial &, j716f_storage::State &,
                        const j716f_storage::Backend &, uint8_t (&output)[kBytes],
                        j716f_storage::Report &);
}
#endif
