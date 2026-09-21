#pragma once

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace manyforge::component_manager {

class ManagerSceneActivationCoordinatorError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// The exact activation is waiting for its accepted native stage to finish.
class ManagerSceneActivationPending final
    : public ManagerSceneActivationCoordinatorError {
public:
  using ManagerSceneActivationCoordinatorError::
      ManagerSceneActivationCoordinatorError;
};

/// Recovery context when activation/retirement may have reached the owner but
/// the Manager cannot yet prove one exact durable result.
class ManagerSceneActivationEffectUncertainError final
    : public ManagerSceneActivationCoordinatorError {
public:
  ManagerSceneActivationEffectUncertainError(
      std::string detail,
      manyforge::core::ProjectionApplyStatusQueryDTO recovery_query,
      std::optional<manyforge::core::ProjectionApplyStatusDTO> observed_status =
          std::nullopt,
      std::optional<manyforge::core::ProjectionApplyActivationResultDTO>
          observed_activation_result = std::nullopt);

  [[nodiscard]] const manyforge::core::ProjectionApplyStatusQueryDTO &
  recovery_query() const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ProjectionApplyStatusDTO> &
  observed_status() const noexcept;
  [[nodiscard]] const std::optional<
      manyforge::core::ProjectionApplyActivationResultDTO> &
  observed_activation_result() const noexcept;

private:
  manyforge::core::ProjectionApplyStatusQueryDTO recovery_query_;
  std::optional<manyforge::core::ProjectionApplyStatusDTO> observed_status_;
  std::optional<manyforge::core::ProjectionApplyActivationResultDTO>
      observed_activation_result_;
};

enum class ManagerSceneActivationPath {
  kActivatedFromNewCall,
  kRecoveredOwnerStatus,
  kDurableActivatedReplay,
};

struct ManagerSceneActivationResult final {
  ManagerApplyActivationSelectionWriteResult durable_selection;
  ManagerApplySceneActivatedWriteResult durable_scene_activated;
  ManagerSceneActivationPath path;
};

/// Irreversibly selects and activates one exact durable scene stage.
///
/// The selection is committed before any activation/retirement call. Recovery
/// observes the owner first and may only complete this selected transition
/// forward; it never discards the candidate or restores the predecessor.
class ManagerSceneActivationCoordinator final {
public:
  ManagerSceneActivationCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      manyforge::core::IManagerSceneProjectionHandoffPort &scene_owner);

  [[nodiscard]] ManagerSceneActivationResult
  ActivateOrRecover(const manyforge::core::ApplyId &apply_id,
                    manyforge::core::ApplyGeneration apply_generation);

  ManagerSceneActivationCoordinator(const ManagerSceneActivationCoordinator &) =
      delete;
  ManagerSceneActivationCoordinator &
  operator=(const ManagerSceneActivationCoordinator &) = delete;

private:
  SqliteManagerApplyJournal &apply_journal_;
  manyforge::core::IManagerSceneProjectionHandoffPort &scene_owner_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
