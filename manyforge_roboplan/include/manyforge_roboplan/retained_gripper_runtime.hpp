#pragma once

#include <functional>
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/configured_robot_measured_state.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_roboplan/retained_gripper_position_limits.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

/// Prepared leases for the existing runtime's exact measured route association.
/// The callback enables the actual prepared producer and must not throw.
struct RetainedPreparedRobotMeasuredSources final {
  core::RobotMeasuredStateSourceView source_view;
  std::function<void()> enable_publication;
};

/// Same prepared-source descriptor and route validation used by the arm owner.
[[nodiscard]] core::Result<void> ValidateRetainedPreparedMeasuredSources(
    const RetainedPreparedRobotMeasuredSources &prepared,
    const core::ControllableComponentId &component,
    std::span<const core::RuntimeParticipantBinding> physical_bindings,
    const std::vector<core::ActiveControlConfigurationDTO> &configurations);



/// Performs the product-specific verification and backend route update needed
/// to re-seal one retained gripper for a candidate publication. The callback
/// must return success only after it has verified that the backend now serves
/// the supplied binding through the candidate state channel and readiness.
using RetainedGripperRouteRebinder = std::function<core::Result<void>(
    const core::RuntimeParticipantBinding&, const std::shared_ptr<IRetainedObservedStateChannel>&,
    const std::optional<core::RuntimeParticipantStopReadiness>&)>;

struct RetainedGripperRuntimeInput final {
  core::RuntimeParticipantBinding binding;
  std::shared_ptr<core::IGripperCommandBackend> backend;
  std::shared_ptr<core::IClock> clock;
  std::shared_ptr<IRetainedObservedStateChannel> state_channel;
  core::IControlDomainExecutionPort* stop_execution_port{};
  std::optional<core::RuntimeParticipantStopReadiness> stop_readiness;
  bool intrinsic_direct_control{};
  RetainedGripperRouteRebinder rebind_route;
  /// Keeps the backend's non-owning scheduler reference valid.
  std::shared_ptr<core::IScheduler> scheduler;
  std::optional<RetainedGripperPositionLimits> position_limits;
  std::function<core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>>(
      const core::RuntimeParticipantBinding&, const core::ActiveControlConfigurationDTO&)>
      prepare_measured_sources{};
  /// Complete source-verified logical bindings of this same physical target.
  /// Empty preserves the existing single-command construction.
  std::vector<core::RuntimeParticipantBinding> control_binding_representatives{};
};

/// Stable owner for one independently controlled native gripper.
///
/// Unlike a robot runtime, this owner contains no planner, motion FSM, or
/// motion execution backend. It keeps the existing Core gripper participant
/// engine and addressed registration alive while a verified route callback
/// updates the binding for a retained controller.
class RetainedGripperRuntime final {
 public:
  explicit RetainedGripperRuntime(RetainedGripperRuntimeInput input);

  RetainedGripperRuntime(const RetainedGripperRuntime&) = delete;
  RetainedGripperRuntime& operator=(const RetainedGripperRuntime&) = delete;

  /// A snapshot suitable for adding this participant to the cell Kernel.
  /// The owner and its backend must outlive every Kernel registration copy.
  [[nodiscard]] core::RuntimeParticipantExecutionRegistration registration() const;
  [[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration> registrations() const;

  /// Stage complete same-owner registrations before any route effect. Each
  /// reconciled representative supplies its own verified Stop readiness.
  [[nodiscard]] core::Result<std::vector<core::RuntimeParticipantExecutionRegistration>>
  PrepareRegistrations(
      const std::vector<core::RuntimeParticipantBinding>& representatives,
      const std::vector<std::optional<core::RuntimeParticipantStopReadiness>>& readiness) const;
  /// Caller has published/read back these exact staged rows while holding the
  /// existing transition exclusion. Physical binding/readiness use their
  /// existing RebindRoute/AdoptVerifiedReadiness joins; this swaps logical rows.
  void AdoptVerifiedRegistrations(
      std::vector<core::RuntimeParticipantExecutionRegistration>&& registrations) noexcept;

  [[nodiscard]] core::RuntimeParticipantBinding binding() const;
  [[nodiscard]] std::optional<core::RuntimeParticipantStopReadiness> stop_readiness() const;
  [[nodiscard]] std::shared_ptr<IRetainedObservedStateChannel> state_channel() const;
  [[nodiscard]] std::optional<RetainedGripperPositionLimits>
  position_limits() const noexcept;
  [[nodiscard]] core::GripperParticipantEngine* participant_engine() const noexcept;
  [[nodiscard]] core::IGripperCommandBackend* backend() const noexcept;
  /// Stop-stage port owned by this runtime's backend, if any.
  [[nodiscard]] core::IControlDomainExecutionPort* stop_execution_port() const noexcept;

  /// Cell Kernel borrowing remains exact across optional-motion products.
  void BindCellKernel(core::ResourceAddressedExecutionKernel& kernel);
  [[nodiscard]] core::Result<core::RobotMeasuredStateSourceView> PrepareCurrentMeasuredSources();

  /// Preserve this owner only across a candidate binding for the same
  /// physical gripper. The candidate channel must already be sealed to the
  /// candidate binding. The callback verifies and changes backend routing
  /// before binding/channel/readiness are advanced together. A failed callback
  /// keeps the old owner facts and must be treated by the caller as a failed
  /// Apply.
  [[nodiscard]] core::Result<void> RebindRoute(
      core::RuntimeParticipantBinding candidate,
      std::shared_ptr<IRetainedObservedStateChannel> candidate_channel,
      std::optional<core::RuntimeParticipantStopReadiness> readiness);

  /// Under route-recovery exclusion only, rebind to the durable predecessor
  /// without reusing successor Stop readiness. The host must publish and verify a
  /// new current route before command admission can reopen.
  [[nodiscard]] core::Result<void> RebindPredecessorRoute(
      core::RuntimeParticipantBinding candidate,
      std::shared_ptr<IRetainedObservedStateChannel> candidate_channel);

  /// Adopt a pre-staged generation-only Stop-readiness re-seal after the
  /// stable cell Kernel has published and exactly read back the same result.
  /// The lifecycle claim excludes a competing route rebind. No allocation or
  /// fallible validation belongs after Kernel publication.
  void AdoptVerifiedReadiness(
      std::optional<core::RuntimeParticipantStopReadiness>&& readiness) noexcept;

 private:
  [[nodiscard]] core::Result<void> RebindRouteImpl(
      core::RuntimeParticipantBinding candidate,
      std::shared_ptr<IRetainedObservedStateChannel> candidate_channel,
      std::optional<core::RuntimeParticipantStopReadiness> readiness,
      bool predecessor_recovery);
  mutable std::mutex mutex_;
  core::RuntimeParticipantBinding binding_;
  std::shared_ptr<core::IClock> clock_;
  std::shared_ptr<core::IScheduler> scheduler_;
  std::shared_ptr<IRetainedObservedStateChannel> state_channel_;
  std::shared_ptr<core::IGripperCommandBackend> backend_;
  core::IControlDomainExecutionPort* stop_execution_port_{};
  std::optional<core::RuntimeParticipantStopReadiness> stop_readiness_;
  std::optional<RetainedGripperPositionLimits> position_limits_;
  bool intrinsic_direct_control_{};
  RetainedGripperRouteRebinder rebind_route_;
  std::shared_ptr<core::GripperParticipantEngine> participant_engine_;
  std::vector<core::HardStopResourceLimitDTO> retained_default_stop_limits_;
  core::ResourceAddressedExecutionKernel* cell_kernel_{};
  std::function<core::Result<std::shared_ptr<const RetainedPreparedRobotMeasuredSources>>(
      const core::RuntimeParticipantBinding&, const core::ActiveControlConfigurationDTO&)> prepare_measured_sources_;
  std::shared_ptr<const RetainedPreparedRobotMeasuredSources> prepared_measured_sources_;
  std::vector<core::RuntimeParticipantExecutionRegistration> participant_registrations_;
};

}  // namespace manyforge::roboplan_backend
