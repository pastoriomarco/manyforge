#pragma once

#include <manyforge_core/types/scene_transaction.hpp>

#include <atomic>

namespace manyforge::headless::detail {

// A normal pre-acceptance refusal has no retained transaction and leaves the
// Scene's existing gates open. It cannot create a new admission settlement.
// A not-attempted projection alone is insufficient: acquisition may have lost
// its reply after the Kernel retained a claim. Unavailable reconciliation of
// older history is also unresolved even without an outcome for this request.
template <class SceneOwner>
core::SceneCommitResultDTO CommitSceneWithAdmissionObservation(
    SceneOwner &owner, const core::SceneTransactionRequestDTO &request,
    std::atomic<bool> &admission_reconciled) {
  try {
    auto result = owner.Commit(request);
    if (!result.success) {
      bool needs_reconciliation = true;
      if (result.projection_status ==
              core::SceneProjectionCommitStatusDTO::kNotAttempted &&
          result.error.has_value() &&
          result.error->code != "kernel_owner_unavailable" &&
          !owner.ReadCommitResult(request).has_value()) {
        const auto access = owner.ReadAccessStatus();
        needs_reconciliation =
            !access.scene_writes_open || !access.projection_leases_open;
      }
      if (needs_reconciliation)
        admission_reconciled.store(false, std::memory_order_release);
    }
    return result;
  } catch (...) {
    // Includes unknown acquire/release outcomes and an unreadable journal.
    admission_reconciled.store(false, std::memory_order_release);
    throw;
  }
}

} // namespace manyforge::headless::detail
