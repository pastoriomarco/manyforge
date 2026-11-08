#pragma once

#include <atomic>
#include <functional>
#include <manyforge_core/execution_recovery_support.hpp>
#include <manyforge_core/kernel_event_publisher.hpp>
#include <manyforge_core/ports/icontrol_domain_execution_port.hpp>
#include <manyforge_core/ports/igripper_command_backend.hpp>
#include <manyforge_core/ports/iresolved_participant_execution_engine.hpp>
#include <manyforge_core/ports/iresolved_prepared_participant_execution_engine.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <memory>
#include <mutex>

namespace manyforge::core {

enum class GripperOwnerRecoveryProofMode { kMeasured, kNoFeedback };

/// Trusted retained owner supplies the exact commit fence and real measured
/// evidence; absence of an evidence object means proof is not yet available.
/// The callback is invoked only after rebuilt_unknown_pending has confirmed the
/// exact target, command and generation on the current replacement owner.
struct GripperOwnerRecoveryEvidence final {
  std::shared_ptr<const void> owner;
  GripperOwnerRecoveryProofMode mode{GripperOwnerRecoveryProofMode::kMeasured};
  /// First publication sequence allowed after the candidate callback gate.
  std::uint64_t first_sequence{};
  std::optional<ObservedStateEvidenceDTO> sample;
  std::string joint_order_token;
};
using GripperOwnerRecoverySource = std::function<std::optional<GripperOwnerRecoveryEvidence>(
    const std::string&, std::uint64_t, std::uint64_t, const std::shared_ptr<const void>&)>;

/// Command-only participant over the existing gripper backend. One preparation,
/// exact dispatch and recovery facts; no motion FSM, planner, worker or timer.
/// All aliases of one physical gripper share this instance. The addressed owner
/// still reserves resources and owns physical Stop/certificates.
class GripperParticipantEngine final : public IResolvedParticipantExecutionEngine,
                                       public IResolvedPreparedParticipantExecutionEngine,
                                       public IControlDomainExecutionPort {
 public:
  GripperParticipantEngine(IGripperCommandBackend& backend, IClock& clock);
  ~GripperParticipantEngine() override;

  /// Installed once by the retained owner; never supplied by a public caller.
  void InstallGripperOwnerRecoverySource(GripperOwnerRecoverySource source);
  /// Called by the existing observation wake/periodic path; no motion is sent.
  void AdvanceGripperOwnerRecovery() const;
  void notify_gripper_owner_loss_recorded(const std::string&, std::uint64_t,
                                           std::uint64_t) override;
  bool notify_exact_gripper_owner_loss_recorded(const std::string&, std::uint64_t,
                                                 std::uint64_t) override;

  GripperActuationKind gripper_actuation_kind(const std::string& target) const override;
  GripperOutputMode gripper_output_mode(const std::string& target) const override;
  GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) override;
  Result<void> validate_gripper_command(const GripperCommandRequestDTO& request) const override;
  GripperCommandStatusDTO get_gripper_status(const std::string& gripper_id) const override;
  bool rebuilt_gripper_unknown_pending(const std::string& gripper_id, std::uint64_t command_id,
                                        std::uint64_t generation) const override;
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
  ParticipantStopStageResultDTO TranslateStopStage(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation) override;
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
      std::uint64_t, std::optional<std::uint64_t> = std::nullopt) override;
  Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO&) override {
    return unsupported<ResolvedRuntimeScaleAcceptedDTO>();
  }
  Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationId&, const ControlBindingId&) const override {
    return unsupported<ResolvedRuntimeScaleStatusDTO>();
  }
  /// A command participant has no motion phase; its command state is read
  /// through get_gripper_status. Always Idle with no motion fact.
  ExecutionStatus get_status() const override { return {}; }
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
  /// reset_gripper without reopening admission: stale check plus the Reset
  /// invalidation of the exact backend command generation.
  Result<void> close_gripper_generation(const std::string& gripper_id, std::uint64_t command_id,
                                        std::uint64_t generation);
  void emit_event(KernelEvent event);
  struct PreparedCapabilityCommandSession;
  std::shared_ptr<const void> recovery_identity_ = std::make_shared<const int>(0);
  void AdvanceGripperOwnerRecoveryFor(
      const std::shared_ptr<PreparedCapabilityCommandSession>& session) const;
  IGripperCommandBackend& backend_;
  IClock& clock_;
  RecoveryReadinessPolicy owner_recovery_policy_;
  std::uint64_t next_execution_id_{1U};  // guarded by prepared_command_session_mutex_
  std::atomic<std::uint64_t> control_command_epoch_{1U};
  mutable std::mutex prepared_command_session_mutex_;
  std::atomic<std::shared_ptr<PreparedCapabilityCommandSession>> active_prepared_command_session_;
  mutable std::mutex owner_recovery_source_mutex_;
  GripperOwnerRecoverySource owner_recovery_source_;
  KernelEventPublisher event_publisher_;
};

}  // namespace manyforge::core
