#pragma once

#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_roboplan/retained_gripper_runtime.hpp>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <vector>

namespace manyforge::roboplan_backend {

class RetainedCellRuntime;
struct RetainedExecutionHost;

/// CCLC route-publication owner for one independently retained native
/// gripper. Reconciled publication verifies the gripper backend, complete
/// cell registration set and Stop readiness before returning a Kernel
/// readback; uncertain results leave the route closed.
struct RouteReadinessParticipant;

class RetainedGripperRoutePublicationOwner final {
 public:
  struct KernelCandidate final {
    /// Complete cell registration set, not just the selected gripper. The
    /// builder is supplied by the stable cell owner and must preserve every
    /// unaffected peer participant and alias.
    std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
    core::RuntimeParticipantAdmissionResolver participant_resolver;
    // Pre-staged cache mirrors; invoked only after exact successful readback.
    // The owning callback performs allocation-free, no-throw swaps.
    std::function<void()> adopt_component_owners{};
  };

  using CellKernelCandidateBuilder = std::function<
      core::Result<KernelCandidate>(
          const core::ActiveControlConfigurationDTO& configuration,
          const std::vector<core::RuntimeParticipantExecutionRegistration>& gripper_registrations)>;

  RetainedGripperRoutePublicationOwner(
      std::shared_ptr<RetainedGripperRuntime> gripper,
      core::IClock& clock, core::ResourceAddressedExecutionKernel& kernel,
      CellKernelCandidateBuilder cell_candidate_builder = {},
      RetainedCellRuntime* cell_owner = nullptr);

  /// Construction-only joins. Domain maps are immutable before CCLC uses
  /// this owner; peers borrow the same existing transition mutex and Kernel.
  void BindMotionRoot(const std::shared_ptr<RetainedExecutionHost>& root);
  void BindParticipantPeers(
      const std::shared_ptr<RetainedGripperRoutePublicationOwner>& root,
      std::map<core::ControlDomainId,
          std::shared_ptr<RetainedGripperRoutePublicationOwner>> peers);
  /// The no-motion readiness candidate retains this owner while holding the
  /// same mutex through its synchronous publish/readback/adoption call.
  [[nodiscard]] std::mutex& CellTransitionMutex() const noexcept {
    return *transition_mutex_;
  }

  [[nodiscard]] core::Result<
      std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO& configuration) const;

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO& configuration,
      std::vector<RouteReadinessParticipant> participants);

  /// Adopt a durable route from the predecessor runtime epoch into this
  /// process under the Kernel's route-recovery exclusion. The route is
  /// deliberately command-closed until CCLC reconciles a current successor.
  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  AdoptAndReadBackRouteConfigurationPredecessor(
      const core::ActiveControlConfigurationDTO& configuration);

 private:
  /// Retains the engine whose registration is already installed in the Kernel.
  std::shared_ptr<RetainedGripperRuntime> gripper_;
  core::IClock& clock_;
  /// The stable Kernel owner must outlive this route-publication seam.
  core::ResourceAddressedExecutionKernel& kernel_;
  CellKernelCandidateBuilder cell_candidate_builder_;
  // Same Kernel owner; must outlive this route seam. Adoption is a no-fail swap.
  RetainedCellRuntime* cell_owner_{};
  [[nodiscard]] core::Result<std::shared_ptr<void>> PinTransitionOwner() const;
  [[nodiscard]] core::Result<void> ValidateInstalledOwner(
      const std::shared_ptr<void>& transition_owner) const;
  std::map<core::ControlDomainId,
      std::shared_ptr<RetainedGripperRoutePublicationOwner>> participant_peers_;
  // Weak back-reference prevents root -> peer -> root ownership cycles.
  std::weak_ptr<void> transition_owner_;
  bool transition_is_motion_root_{};
  mutable std::mutex mutex_;
  std::mutex* transition_mutex_{&mutex_};
};

}  // namespace manyforge::roboplan_backend
