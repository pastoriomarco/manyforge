#pragma once

#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <mutex>
#include <optional>
#include <stdexcept>

namespace manyforge::component_manager {

class ManagerSelectedRuntimeCompositionCoordinatorError final
    : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

enum class ManagerSelectedRuntimeCompositionResultState {
  kPending,
  kInstalled,
};

struct ManagerSelectedRuntimeCompositionCoordinatorResult final {
  ManagerSelectedRuntimeCompositionResultState state;
  ManagerSelectedRuntimeCompositionResultDTO owner_result;
  std::optional<ManagerApplyPrivateMaterializationWriteResult>
      durable_materialization;
};

/// Installs the selected product inside the already-running fence-owning host
/// before Manager records private materialization or publishes the candidate.
/// Exact replay always re-observes the current child, so Manager/child
/// replacement cannot reuse a journal row as live-runtime evidence.
class ManagerSelectedRuntimeCompositionCoordinator final {
public:
  ManagerSelectedRuntimeCompositionCoordinator(
      SqliteManagerApplyJournal &apply_journal,
      manyforge::core::IManagerSceneProjectionMaintenancePort &scene_owner,
      IManagerSelectedRuntimeCompositionPort &runtime_composition,
      manyforge::core::RuntimeEpoch manager_runtime_epoch);

  [[nodiscard]] ManagerSelectedRuntimeCompositionCoordinatorResult
  PrepareOrObserve(const manyforge::core::ApplyId &apply_id,
                   manyforge::core::ApplyGeneration apply_generation,
                   const manyforge::deployment::RetainedBundle &candidate);

  /// Original configuration-only preparation after qualified private Stage.
  /// Its actual captured snapshot/claim replaces ordinary Scene initialization,
  /// without fabricating a linked plan or logical Scene status.
  [[nodiscard]] ManagerSelectedRuntimeCompositionCoordinatorResult
  PrepareOriginalConfiguration(
      const manyforge::core::QualifiedSceneConfigurationAdoptionRequestDTO &original,
      const manyforge::deployment::RetainedBundle &candidate);

  /// Original process input only. The named coordinator positively verifies
  /// its captured claim before calling; retained plans alone never delegate.
  /// This preparation does not initialize or materialize logical Scene.
  [[nodiscard]] ManagerSelectedRuntimeCompositionCoordinatorResult
  PrepareOriginalApplyComponentBringUp(
      const PersistedManagerApplyBringUpPlan &original_plan,
      const manyforge::core::LogicalSceneSnapshotIdentityDTO &captured_predecessor_snapshot,
      const manyforge::core::CellAdmissionClaimDTO &original_parent_claim,
      const manyforge::deployment::RetainedBundle &candidate);

  [[nodiscard]] ManagerSelectedRuntimeCompositionCoordinatorResult
  InstallOrRecover(const manyforge::core::ApplyId &apply_id,
                   manyforge::core::ApplyGeneration apply_generation,
                   const manyforge::deployment::RetainedBundle &candidate);

  /// Existing typed transport only. These calls grant no continuation from
  /// retained data; the named coordinator must retain its original capture.
  [[nodiscard]] manyforge::core::Result<ManagerApplyComponentBringUpResultDTO>
  SubmitApplyComponentBringUp(const ManagerApplyComponentBringUpRequestDTO &request);
  [[nodiscard]] manyforge::core::Result<ManagerApplyComponentBringUpResultDTO>
  ReadApplyComponentBringUp(const ManagerApplyComponentBringUpReadRequestDTO &request);

private:
  [[nodiscard]] ManagerSelectedRuntimeCompositionCoordinatorResult
  Advance(const manyforge::core::ApplyId &apply_id,
          manyforge::core::ApplyGeneration apply_generation,
          const manyforge::deployment::RetainedBundle &candidate,
          ManagerSelectedRuntimeCompositionPhase phase);
  SqliteManagerApplyJournal &apply_journal_;
  manyforge::core::IManagerSceneProjectionMaintenancePort &scene_owner_;
  IManagerSelectedRuntimeCompositionPort &runtime_composition_;
  manyforge::core::RuntimeEpoch manager_runtime_epoch_;
  std::mutex operation_mutex_;
};

} // namespace manyforge::component_manager
