#pragma once

#include <functional>
#include <manyforge_core/gripper_participant_engine.hpp>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/time/ischeduler.hpp>
#include <manyforge_roboplan/retained_gripper_position_limits.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace manyforge::roboplan_backend {

/// Performs the product-specific verification and backend route update needed
/// to re-seal one retained gripper for a candidate publication. The callback
/// must return success only after it has verified that the backend now serves
/// the supplied binding through the candidate state channel and readiness.
using RetainedNativeGripperRouteRebinder = std::function<core::Result<void>(
    const core::RuntimeParticipantBinding&, const std::shared_ptr<IRetainedObservedStateChannel>&,
    const std::optional<core::RuntimeParticipantStopReadiness>&)>;

struct RetainedNativeGripperRuntimeInput final {
  core::RuntimeParticipantBinding binding;
  std::shared_ptr<core::IGripperCommandBackend> backend;
  std::shared_ptr<core::IClock> clock;
  std::shared_ptr<IRetainedObservedStateChannel> state_channel;
  core::IControlDomainExecutionPort* stop_execution_port{};
  std::optional<core::RuntimeParticipantStopReadiness> stop_readiness;
  bool intrinsic_direct_control{};
  RetainedNativeGripperRouteRebinder rebind_route;
  /// Keeps the backend's non-owning scheduler reference valid.
  std::shared_ptr<core::IScheduler> scheduler;
  std::optional<RetainedGripperPositionLimits> position_limits;
};

/// Stable owner for one independently controlled native gripper.
///
/// Unlike a robot runtime, this owner contains no planner, motion FSM, or
/// motion execution backend. It keeps the existing Core gripper participant
/// engine and addressed registration alive while a verified route callback
/// updates the binding for a retained controller.
class RetainedNativeGripperRuntime final {
 public:
  explicit RetainedNativeGripperRuntime(RetainedNativeGripperRuntimeInput input);

  RetainedNativeGripperRuntime(const RetainedNativeGripperRuntime&) = delete;
  RetainedNativeGripperRuntime& operator=(const RetainedNativeGripperRuntime&) = delete;

  /// A snapshot suitable for adding this participant to the cell Kernel.
  /// The owner and its backend must outlive every Kernel registration copy.
  [[nodiscard]] core::RuntimeParticipantExecutionRegistration registration() const;

  [[nodiscard]] core::RuntimeParticipantBinding binding() const;
  [[nodiscard]] std::optional<core::RuntimeParticipantStopReadiness> stop_readiness() const;
  [[nodiscard]] std::shared_ptr<IRetainedObservedStateChannel> state_channel() const;
  [[nodiscard]] std::optional<RetainedGripperPositionLimits>
  position_limits() const noexcept;
  [[nodiscard]] core::GripperParticipantEngine* participant_engine() const noexcept;
  [[nodiscard]] core::IGripperCommandBackend* backend() const noexcept;
  /// Stop-stage port owned by this runtime's backend, if any.
  [[nodiscard]] core::IControlDomainExecutionPort* stop_execution_port() const noexcept;

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
  /// without reusing successor Stop readiness. CCLC must publish and verify a
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
  RetainedNativeGripperRouteRebinder rebind_route_;
  std::shared_ptr<core::GripperParticipantEngine> participant_engine_;
  std::vector<core::HardStopResourceLimitDTO> retained_default_stop_limits_;
};

}  // namespace manyforge::roboplan_backend
