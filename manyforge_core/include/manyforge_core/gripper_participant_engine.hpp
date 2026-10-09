#pragma once

#include <atomic>
#include <manyforge_core/kernel_event_publisher.hpp>
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/ports/iresolved_prepared_participant_execution_engine.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <mutex>

namespace manyforge::core {

/// Command-only participant over the existing gripper backend. One preparation,
/// exact dispatch and recovery facts; no motion FSM, planner, worker or timer.
/// All aliases of one physical gripper share this instance. The addressed owner
/// still reserves resources and owns physical Stop/certificates.
class GripperParticipantEngine final : public IResolvedParticipantExecutionEngine,
                                       public IResolvedPreparedParticipantExecutionEngine {
 public:
  GripperParticipantEngine(IGripperCommandBackend& backend, IClock& clock);
  ~GripperParticipantEngine() override;

  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) override;
  Result<void> validate_gripper_command(const GripperCommandRequestDTO& request) const override;
  GripperCommandStatusDTO get_gripper_status(const std::string& gripper_id) const override;
  Result<std::uint64_t> stop_gripper(const std::string& gripper_id, std::uint64_t command_id,
                                     std::uint64_t generation) override;
  Result<void> reset_gripper(const std::string& gripper_id, std::uint64_t command_id,
                             std::uint64_t generation) override;
  Result<std::uint64_t> PrepareCapabilityCommand(
      ResolvedPreparedCapabilityCommandExecutionRequestDTO request) override;
  Result<ResolvedPreparedCapabilityCommandSnapshotDTO> ObservePreparedCapabilityCommand(
      std::uint64_t preparation_id) const override;
  Result<GripperCommandResultDTO> DispatchPreparedCapabilityCommand(
      std::uint64_t preparation_id, const ContentDigest& digest, const ControllerGoalId& goal,
      ControllerGoalGeneration generation) override;
  Result<void> CancelPreparedCapabilityCommand(std::uint64_t preparation_id) override;
  AddressedParticipantStopFenceDisposition fence_addressed_participant_stop(
      std::uint64_t execution_id) noexcept override;
  Result<AddressedParticipantStopSettlementDTO> settle_addressed_participant_stop(
      const AddressedParticipantStopSettlementRequestDTO& request) override;
  KernelRecoveryAssessmentDTO get_recovery_assessment() const override;
  KernelRecoveryActionResultDTO apply_recovery_action(
      const KernelRecoveryActionRequestDTO& request) override;
  SubscriptionHandle subscribe_events(EventCallback callback) override;
  void unsubscribe_events(SubscriptionHandle handle) override;

  // Motion-only calls cannot acquire authority over this command participant.
  ResetResultDTO reset_after_certified_containment(
      std::uint64_t, std::optional<std::uint64_t> = std::nullopt) override {
    return {false, unsupported_error()};
  }
  AcknowledgeCompletedResultDTO acknowledge_completed(
      std::uint64_t, std::optional<std::uint64_t> = std::nullopt) override {
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
  ExecutionStatus get_status() const override {
    ExecutionStatus status;
    status.state = ExecutionState::kFailed;
    status.last_error = ErrorCode::kUnsupportedCapability;
    return status;
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
  template <typename T>
  static Result<T> unsupported() {
    return Result<T>::Err(unsupported_error());
  }
  std::uint64_t bump_control_command_epoch();
  void emit_event(KernelEvent event);
  struct PreparedCapabilityCommandSession;
  IGripperCommandBackend& backend_;
  IClock& clock_;
  std::uint64_t next_execution_id_{1U};  // guarded by prepared_command_session_mutex_
  std::atomic<std::uint64_t> control_command_epoch_{1U};
  mutable std::mutex prepared_command_session_mutex_;
  std::shared_ptr<PreparedCapabilityCommandSession> active_prepared_command_session_;
  KernelEventPublisher event_publisher_;
};

}  // namespace manyforge::core
