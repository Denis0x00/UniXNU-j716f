// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../AppsSmmuOwner.h"

// Deliberately compiled as another TU. These calls must use the same owner
// implementation and transaction gate as the main test and every other driver.
extern "C" AppsSmmuResult owner_test_peer_acquire(const AppsSmmuClaim *claim, AppsSmmuToken *token)
{ return apps_smmu_owner_try_acquire(claim, token); }
extern "C" AppsSmmuResult owner_test_peer_validate(const AppsSmmuToken *token, const AppsSmmuClaim *claim)
{ return apps_smmu_owner_validate(token, claim); }
