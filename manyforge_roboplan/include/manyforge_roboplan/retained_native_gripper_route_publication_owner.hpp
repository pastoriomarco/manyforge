#pragma once

#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_deployment/route_configuration_publication.hpp>
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace manyforge::roboplan_backend {

/// CCLC route-publication owner for one independently retained native
/// gripper. Reconciled publication verifies the gripper backend, complete
/// cell registration set and Stop readiness before returning a Kernel
/// readback; uncertain results leave the route closed.
class RetainedNativeGripperRoutePublicationOwner final
    : public deployment::IRouteConfigurationKernelPublicationOwner {
 public:
  struct KernelCandidate final {
    /// Complete cell registration set, not just the selected gripper. The
    /// builder is supplied by the stable cell owner and must preserve every
    /// unaffected peer participant and alias.
    std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
    core::RuntimeParticipantAdmissionResolver participant_resolver;
  };

  using CellKernelCandidateBuilder = std::function<
      core::Result<KernelCandidate>(
          const core::ActiveControlConfigurationDTO& configuration,
          const core::RuntimeParticipantExecutionRegistration& gripper_registration)>;

  RetainedNativeGripperRoutePublicationOwner(
      std::shared_ptr<RetainedNativeGripperRuntime> gripper,
      core::IClock& clock, core::ResourceAddressedExecutionKernel& kernel,
      CellKernelCandidateBuilder cell_candidate_builder = {});

  [[nodiscard]] core::Result<
      std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO& configuration) const override;

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO& configuration,
      std::vector<deployment::VerifiedRouteReadinessParticipant> participants)
      override;

  /// Adopt a durable route from the predecessor runtime epoch into this
  /// process under the Kernel's route-recovery exclusion. The route is
  /// deliberately command-closed until CCLC reconciles a current successor.
  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  AdoptAndReadBackRouteConfigurationPredecessor(
      const core::ActiveControlConfigurationDTO& configuration);

 private:
  /// Retains the engine whose registration is already installed in the Kernel.
  std::shared_ptr<RetainedNativeGripperRuntime> gripper_;
  core::IClock& clock_;
  /// The stable Kernel owner must outlive this route-publication seam.
  core::ResourceAddressedExecutionKernel& kernel_;
  CellKernelCandidateBuilder cell_candidate_builder_;
  mutable std::mutex mutex_;
};

}  // namespace manyforge::roboplan_backend
