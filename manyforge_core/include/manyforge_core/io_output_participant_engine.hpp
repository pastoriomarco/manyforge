#pragma once

#include <atomic>
#include <manyforge_core/kernel_event_publisher.hpp>
#include <manyforge_core/ports/iio_output_provider.hpp>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/ports/iresolved_prepared_participant_execution_engine.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <mutex>

namespace manyforge::core {

/// One subordinate command session for one actual output channel. Aliases of
/// the same sealed channel share this engine and provider. Kernel owns resource
/// admission, dispatch, replay and retirement; this engine owns no worker/timer.
class IoOutputParticipantEngine final : public IResolvedParticipantExecutionEngine,
                                        public IResolvedPreparedParticipantExecutionEngine {
 public:
  IoOutputParticipantEngine(IIoOutputProvider& provider, IClock& clock);
  ~IoOutputParticipantEngine() override;
  Result<std::uint64_t> PrepareCapabilityCommand(
      ResolvedPreparedCapabilityCommandExecutionRequestDTO request) override;
  Result<ResolvedPreparedCapabilityCommandSnapshotDTO> ObservePreparedCapabilityCommand(
      std::uint64_t preparation_id) const override;
  Result<GripperCommandResultDTO> DispatchPreparedCapabilityCommand(
      std::uint64_t preparation_id, const ContentDigest& digest, const ControllerGoalId& goal,
      ControllerGoalGeneration generation) override;
  Result<void> CancelPreparedCapabilityCommand(std::uint64_t preparation_id) override;
  ExecutionStatus get_status() const override;
  KernelRecoveryAssessmentDTO get_recovery_assessment() const override;
  KernelRecoveryActionResultDTO apply_recovery_action(
      const KernelRecoveryActionRequestDTO& request) override;
  AcknowledgeCompletedResultDTO acknowledge_completed(
      std::uint64_t execution_id, std::optional<std::uint64_t> expected_epoch = {}) override;
  AddressedParticipantStopFenceDisposition fence_addressed_participant_stop(
      std::uint64_t execution_id) noexcept override;
  SubscriptionHandle subscribe_events(EventCallback callback) override {
    return events_.subscribe(std::move(callback));
  }
  void unsubscribe_events(SubscriptionHandle handle) override { events_.unsubscribe(handle); }

  // An output transaction cannot acquire controller/gripper/Stop authority.
  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO&) override {
    return {false, 0U, 0U, unsupported_error()};
  }
  Result<void> validate_gripper_command(const GripperCommandRequestDTO&) const override {
    return unsupported<void>();
  }
  GripperCommandStatusDTO get_gripper_status(const std::string&) const override { return {}; }
  Result<std::uint64_t> stop_gripper(const std::string&, std::uint64_t, std::uint64_t) override {
    return unsupported<std::uint64_t>();
  }
  Result<void> reset_gripper(const std::string&, std::uint64_t, std::uint64_t) override {
    return unsupported<void>();
  }
  Result<void> reset_capability_command(
      const std::string& target, std::uint64_t command_id, std::uint64_t generation,
      const std::optional<IoOutputResetAuthorityDTO>& authority = std::nullopt) override;
  ResetResultDTO reset_after_certified_containment(std::uint64_t,
                                                   std::optional<std::uint64_t> = {}) override {
    return {false, unsupported_error()};
  }
  Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO&) override {
    return unsupported<ResolvedRuntimeScaleAcceptedDTO>();
  }
  Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationId&, const ControlBindingId&) const override {
    return unsupported<ResolvedRuntimeScaleStatusDTO>();
  }
  Result<std::uint64_t> PrepareMotion(ResolvedPreparedMotionExecutionRequestDTO) override {
    return unsupported<std::uint64_t>();
  }
  Result<ResolvedPreparedMotionExecutionSnapshotDTO> ObservePreparedMotion(
      std::uint64_t) const override {
    return unsupported<ResolvedPreparedMotionExecutionSnapshotDTO>();
  }
  Result<void> DispatchPreparedMotion(std::uint64_t, const ContentDigest&) override {
    return unsupported<void>();
  }
  Result<void> CancelPreparedMotion(std::uint64_t) override { return unsupported<void>(); }

 private:
  static ErrorDTO unsupported_error();
  template <class T>
  static Result<T> unsupported() {
    return Result<T>::Err(unsupported_error());
  }
  struct Session;
  void exclude_unknown_group_model_locked(Session& session) const;
  void refresh_locked(Session& session) const;
  ExecutionStatus status_locked(const Session& session) const;
  KernelRecoveryAssessmentDTO assessment_locked(Session& session) const;
  Result<void> retire_locked(Session& session);
  IIoOutputProvider& provider_;
  IClock& clock_;
  IoOutputEvidence strongest_;
  mutable std::mutex resident_mutex_;
  std::atomic<std::shared_ptr<Session>> resident_;
  std::uint64_t next_id_{1U};
  std::atomic<std::uint64_t> command_epoch_{1U};
  KernelEventPublisher events_;
};
}  // namespace manyforge::core
