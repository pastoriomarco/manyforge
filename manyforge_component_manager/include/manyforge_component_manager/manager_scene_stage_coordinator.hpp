#pragma once

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>

namespace manyforge::component_manager {

class ManagerSceneStageCoordinatorError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// The exact stage is owned and progressing; retry by identity.
class ManagerSceneStagePending final : public ManagerSceneStageCoordinatorError {
public:
  using ManagerSceneStageCoordinatorError::ManagerSceneStageCoordinatorError;
};

/// Explicit recovery context when the scene owner may have observed Stage but
/// the Manager cannot yet prove one exact durable result.
class ManagerSceneStageEffectUncertainError final
    : public ManagerSceneStageCoordinatorError {
public:
  ManagerSceneStageEffectUncertainError(
      std::string detail, manyforge::core::ProjectionApplyStatusQueryDTO query,
      std::optional<manyforge::core::ProjectionApplyStatusDTO> observed_status =
          std::nullopt,
      std::optional<manyforge::core::ProjectionApplyStageResultDTO>
          observed_stage_result = std::nullopt);

  [[nodiscard]] const manyforge::core::ProjectionApplyStatusQueryDTO &
  recovery_query() const noexcept;
  [[nodiscard]] const std::optional<manyforge::core::ProjectionApplyStatusDTO> &
  observed_status() const noexcept;
  [[nodiscard]] const std::optional<
      manyforge::core::ProjectionApplyStageResultDTO> &
  observed_stage_result() const noexcept;

private:
  manyforge::core::ProjectionApplyStatusQueryDTO recovery_query_;
  std::optional<manyforge::core::ProjectionApplyStatusDTO> observed_status_;
  std::optional<manyforge::core::ProjectionApplyStageResultDTO>
      observed_stage_result_;
};

enum class ManagerSceneStagePath {
  kStagedFromNewCall,
  kRecoveredOwnerStatus,
  kDurableStagedReplay,
};

struct ManagerSceneStageResult final {
  ManagerApplySceneStageIntentWriteResult durable_scene_stage_intent;
  ManagerApplySceneStagedWriteResult durable_scene_staged;
  ManagerSceneStagePath path;
};

/// Durable observe-first boundary between runtime staged-ready and scene
/// staging for one named Apply.
///
/// The caller supplies only the durable Apply identity. The coordinator reads
/// the immutable Apply/runtime records, independently reloads both exact
/// retained bundles, validates the current predecessor publication, and
/// derives the transition set before persisting it. Recovery uses the already
/// persisted set and queries owner status before any Stage call. Only one
/// complete exact `kStaged` owner result becomes durable success evidence.
class ManagerSceneStageCoordinator final {
public:
  ManagerSceneStageCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      const manyforge::deployment::IVerifiedRetainedBundleSource
          &retained_bundle_source,
      const manyforge::core::IActiveBundlePublicationReader &publication_reader,
      manyforge::core::IManagerSceneProjectionHandoffPort &scene_owner,
      manyforge::core::IManagerSceneProjectionBootstrapPort
          *bootstrap_scene_owner = nullptr,
      IRuntimeUnitProcessPort *runtime_process_port = nullptr);

  [[nodiscard]] ManagerSceneStageResult
  StageOrRecover(const manyforge::core::ApplyId &apply_id,
                 manyforge::core::ApplyGeneration apply_generation);

  ManagerSceneStageCoordinator(const ManagerSceneStageCoordinator &) = delete;
  ManagerSceneStageCoordinator &
  operator=(const ManagerSceneStageCoordinator &) = delete;

private:
  SqliteManagerApplyJournal &apply_journal_;
  const manyforge::deployment::IVerifiedRetainedBundleSource
      &retained_bundle_source_;
  const manyforge::core::IActiveBundlePublicationReader &publication_reader_;
  manyforge::core::IManagerSceneProjectionHandoffPort &scene_owner_;
  manyforge::core::IManagerSceneProjectionBootstrapPort
      *bootstrap_scene_owner_;
  IRuntimeUnitProcessPort *runtime_process_port_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
