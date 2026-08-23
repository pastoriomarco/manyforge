#pragma once

#include <filesystem>
#include <manyforge_component_manager/runtime_convergence.hpp>
#include <manyforge_component_manager/manager_liveness_transport.hpp>
#include <manyforge_component_manager/selected_runtime_composition.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::deployment {
class IVerifiedRetainedBundleSource;
class IVerifiedRetainedProductArtifactSource;
} // namespace manyforge::deployment

namespace manyforge::component_manager {

class RetainedNativeRuntimeUnitAdapterError final : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

/// Manager-owned local adapter for the retained native execution product.
///
/// The adapter resolves only the contract-fixed installed target beneath one
/// trusted target directory, launches it as a direct child, and communicates
/// over one inherited AF_UNIX packet channel. There is no process discovery,
/// adoption, public socket, executable/argument selection, or caller
/// authorization. A child is staged-ready only after its direct status report
/// matches the verified retained product and reports a closed command surface.
class RetainedNativeRuntimeUnitAdapter final
    : public IRuntimeUnitProcessPort,
      public IRuntimeUnitProgressPort,
      public manyforge::core::IManagerKernelApplyFencePort,
      public manyforge::core::IKernelCellAdmissionPort,
      public manyforge::core::IPlannerSceneProjectionResolver,
      public IManagerSelectedRuntimeCompositionPort,
      public IManagerLivenessTransportPort {
public:
  RetainedNativeRuntimeUnitAdapter(
      std::filesystem::path trusted_installed_target_root,
      std::filesystem::path trusted_package_root,
      std::filesystem::path manager_state_root,
      std::filesystem::path private_staging_root,
      const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      std::string operating_system, std::string launch_environment_contract,
      std::filesystem::path logical_scene_database = {},
      std::filesystem::path program_cycle_journal = {},
      std::filesystem::path authoring_transaction_journal = {});
  RetainedNativeRuntimeUnitAdapter(
      std::filesystem::path trusted_installed_target_root,
      std::filesystem::path trusted_package_root,
      std::filesystem::path manager_state_root,
      std::filesystem::path private_staging_root,
      const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      std::string operating_system,
      std::vector<std::string> launch_environment_contracts,
      std::filesystem::path logical_scene_database = {},
      std::filesystem::path program_cycle_journal = {},
      std::filesystem::path authoring_transaction_journal = {});
  ~RetainedNativeRuntimeUnitAdapter() override;

  RetainedNativeRuntimeUnitAdapter(const RetainedNativeRuntimeUnitAdapter &) =
      delete;
  RetainedNativeRuntimeUnitAdapter &
  operator=(const RetainedNativeRuntimeUnitAdapter &) = delete;
  RetainedNativeRuntimeUnitAdapter(RetainedNativeRuntimeUnitAdapter &&) =
      delete;
  RetainedNativeRuntimeUnitAdapter &
  operator=(RetainedNativeRuntimeUnitAdapter &&) = delete;

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) override;
  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const manyforge::core::RuntimeEpoch &runtime_epoch) override;
  [[nodiscard]] manyforge::core::ConfigurationFenceAdmissionSnapshotDTO
  ObserveAdmission(const manyforge::core::ConfigurationFenceAdmissionQueryDTO
                       &query) const override;
  [[nodiscard]] manyforge::core::ConfigurationFenceAcceptedDTO Acquire(
      const manyforge::core::ApplyConfigurationFenceIntentDTO &intent) override;
  [[nodiscard]]
  manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const manyforge::core::ApplyFenceAcquireAttemptQueryDTO &query)
      const override;
  [[nodiscard]] manyforge::core::ApplyFenceReconcileResultDTO
  Rehydrate(const manyforge::core::ApplyFenceRehydrateDTO &request) override;
  [[nodiscard]] manyforge::core::ConfigurationFenceStatusDTO
  QueryStatus(const manyforge::core::ConfigurationFenceStatusQueryDTO &query)
      const override;
  [[nodiscard]] manyforge::core::ConfigurationFenceReleasedDTO Release(
      const manyforge::core::ConfigurationFenceReleaseDTO &request) override;
  [[nodiscard]] manyforge::core::AuthoringAdmissionDTO
  GetAuthoringAdmission() const override;
  [[nodiscard]] manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
  UpdateCellAdmissionReadiness(
      const manyforge::core::CellAdmissionReadinessSnapshotDTO &snapshot)
      override;
  [[nodiscard]] manyforge::core::Result<void> Publish(
      const manyforge::core::ManagerLivenessObservationDTO &observation)
      override;
  [[nodiscard]]
  manyforge::core::Result<manyforge::core::CellAdmissionClaimDTO>
  AcquireCellAdmissionClaim(
      const manyforge::core::CellAdmissionClaimIntentDTO &intent) override;
  [[nodiscard]] manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
  ReleaseCellAdmissionClaim(
      const manyforge::core::CellAdmissionClaimReleaseDTO &release) override;
  [[nodiscard]] manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
  ReconcileCellAdmissionClaims(
      const manyforge::core::CellAdmissionReconcileDTO &reconcile) override;
  [[nodiscard]] manyforge::core::Result<
      manyforge::core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const manyforge::core::CellAdmissionRecoveryReceiptQueryDTO &query)
      const override;
  [[nodiscard]] manyforge::core::Result<
      std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
  ResolveProjection(const manyforge::core::ProjectionNativeRuntimeBindingDTO
                        &native_runtime_binding) const override;
  [[nodiscard]] ManagerSelectedRuntimeCompositionResultDTO InstallOrObserve(
      const ManagerSelectedRuntimeCompositionRequestDTO &request) override;
  [[nodiscard]] manyforge::core::ManagerLivenessObservationDTO
  RenewManagerLiveness(
      const ManagerLivenessTransportRequestDTO& request) override;
  [[nodiscard]]
  std::vector<manyforge::core::ManagerLivenessObservationDTO>
  RenewManagerLivenessBatch(
      const std::vector<ManagerLivenessTransportRequestDTO>& requests)
      override;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace manyforge::component_manager
