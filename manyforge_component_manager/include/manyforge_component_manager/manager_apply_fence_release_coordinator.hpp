#pragma once

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace manyforge::component_manager {

class ManagerApplyFenceReleaseCoordinatorError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Exact recovery context for a status or Release boundary that cannot be
/// accepted as complete.
class ManagerApplyFenceReleaseEffectUncertainError final
    : public ManagerApplyFenceReleaseCoordinatorError {
 public:
  ManagerApplyFenceReleaseEffectUncertainError(
      std::string detail, bool release_invoked_in_this_call,
      manyforge::core::ConfigurationFenceStatusQueryDTO status_query,
      std::optional<manyforge::core::ConfigurationFenceReleaseDTO> durable_release_request =
          std::nullopt,
      std::optional<manyforge::core::ConfigurationFenceStatusDTO> observed_status = std::nullopt,
      std::optional<manyforge::core::ConfigurationFenceReleasedDTO> observed_release =
          std::nullopt);

  [[nodiscard]] bool release_invoked_in_this_call() const noexcept;
  [[nodiscard]] const manyforge::core::ConfigurationFenceStatusQueryDTO& status_query()
      const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ConfigurationFenceReleaseDTO>&
  durable_release_request() const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ConfigurationFenceStatusDTO>& observed_status()
      const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ConfigurationFenceReleasedDTO>&
  observed_release() const noexcept;

 private:
  bool release_invoked_in_this_call_;
  manyforge::core::ConfigurationFenceStatusQueryDTO status_query_;
  std::optional<manyforge::core::ConfigurationFenceReleaseDTO> durable_release_request_;
  std::optional<manyforge::core::ConfigurationFenceStatusDTO> observed_status_;
  std::optional<manyforge::core::ConfigurationFenceReleasedDTO> observed_release_;
};

enum class ManagerApplyFenceReleasePath {
  kReleasedFromNewCall,
  kRecoveredReleasedStatus,
  kCompletedFromDurableRelease,
  kDurableTerminalReplay,
};

struct ManagerApplyFenceReleaseResult final {
  ManagerApplyFenceReleaseIntentWriteResult durable_release_intent;
  ManagerApplyFenceReleasedWriteResult durable_release;
  ManagerApplyTerminalOutcomeWriteResult durable_terminal_outcome;
  ManagerApplyFenceReleasePath path;
};

/// Releases one named-Apply Kernel fence only after Scene reports the exact
/// publication as completed.
///
/// Every nonterminal invocation observes exact Kernel status before any
/// Release call. The complete compare-and-swap request is journaled first.
/// Exact durable completion replays without a Kernel operation.
class ManagerApplyFenceReleaseCoordinator final {
 public:
  ManagerApplyFenceReleaseCoordinator(SqliteManagerApplyJournal& apply_journal,
                                      manyforge::core::IManagerKernelApplyFencePort& kernel_fence,
                                      manyforge::core::IManagerSceneProjectionMaintenancePort&
                                          scene_owner,
                                      manyforge::core::RuntimeEpoch manager_runtime_epoch);

  [[nodiscard]] ManagerApplyFenceReleaseResult ReleaseOrRecover(
      const manyforge::core::ApplyId& apply_id, manyforge::core::ApplyGeneration apply_generation);

  ManagerApplyFenceReleaseCoordinator(const ManagerApplyFenceReleaseCoordinator&) = delete;
  ManagerApplyFenceReleaseCoordinator& operator=(const ManagerApplyFenceReleaseCoordinator&) =
      delete;

 private:
  SqliteManagerApplyJournal& apply_journal_;
  manyforge::core::IManagerKernelApplyFencePort& kernel_fence_;
  manyforge::core::IManagerSceneProjectionMaintenancePort& scene_owner_;
  manyforge::core::RuntimeEpoch manager_runtime_epoch_;
  std::mutex operation_mutex_;
};

}  // namespace manyforge::component_manager
