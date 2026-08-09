#pragma once

#include <filesystem>
#include <manyforge_component_manager/runtime_convergence.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <memory>
#include <stdexcept>
#include <string>

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
      public manyforge::core::IPlannerSceneProjectionResolver {
public:
  RetainedNativeRuntimeUnitAdapter(
      std::filesystem::path trusted_installed_target_root,
      std::filesystem::path trusted_package_root,
      std::filesystem::path manager_state_root,
      std::filesystem::path private_staging_root,
      const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      std::string operating_system, std::string launch_environment_contract);
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
  [[nodiscard]] manyforge::core::Result<
      std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
  ResolveProjection(
      const manyforge::core::ProjectionNativeRuntimeBindingDTO
          &native_runtime_binding) const override;

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
};

} // namespace manyforge::component_manager
