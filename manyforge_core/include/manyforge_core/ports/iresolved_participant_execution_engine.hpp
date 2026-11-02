#pragma once

#include <cstdint>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/gripper.hpp>
#include <manyforge_core/types/kernel_event.hpp>
#include <manyforge_core/types/participant_stop_stage.hpp>
#include <manyforge_core/types/recovery.hpp>
#include <manyforge_core/types/runtime_scale.hpp>
#include <manyforge_core/types/stop_certificate.hpp>
#include <optional>
#include <string>

namespace manyforge::core {

/// Known terminal effects are necessary for motion-free retirement; an
/// internal consistency failure still requires diagnosis, not acknowledgement.
inline bool is_settled_execution_failure(const ParticipantExecutionFact& fact) {
  return fact.kind == ParticipantExecutionFactKind::kLocalFailure &&
         fact.effect == ParticipantEffectKnowledge::kEffectSettled && fact.error &&
         fact.error->code != to_string(ErrorCode::kInternalError) &&
         fact.error->code != to_string(ErrorCode::kInvariantViolation) &&
         fact.error->code != to_string(ErrorCode::kInvalidStateTransition);
}

/// Exact process-local handoff after the addressed owner has proved either
/// physical containment for one dispatched participant or effect-free
/// cancellation behind the coordinated dispatch barrier. The aggregate Stop
/// certificate identity is reserved by the addressed owner; the participant
/// records only this reference and MUST NOT issue another Stop or certificate.
struct AddressedParticipantStopSettlementRequestDTO final {
  std::uint64_t execution_id{};
  StopCertificateRefDTO aggregate_stop_certificate;
  /// Carries the exact addressed operation/participant/goal identity in both
  /// cases. For effect-free cancellation it has kNotDispatched disposition,
  /// successful outcome, no evidence items, and is never retained as a
  /// physical Stop-stage attempt.
  ParticipantStopStageResultDTO containment;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  bool effect_prevented_before_dispatch{};
};

/// Participant recovery evidence returned synchronously to the addressed
/// reducer after it recorded the owner's containment. The participant commits
/// no outcome; the addressed Kernel owns the operation terminal. Repeating the
/// exact request returns the same evidence; a conflicting Stop identity is
/// rejected without changing the participant.
struct AddressedParticipantStopSettlementDTO final {
  std::uint64_t execution_id{};
  StopCertificateRefDTO aggregate_stop_certificate;
  OperationId operation_id;
  OperationParticipantId participant_id;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  KernelRecoveryAssessmentDTO recovery_assessment;
  /// A contained local failure keeps its original cause and cannot acquire
  /// ordinary-Stop retry permission. Absence retains the ordinary Stop path.
  std::optional<ParticipantExecutionFact> terminal_fact{};
};

/// Result of the exact process-local Stop fence at the subordinate dispatch
/// handoff.  This is deliberately smaller than a Stop result: it says only
/// whether the addressed owner prevented the physical effect before dispatch
/// committed, or must continue with the prepared physical Stop cascade.
enum class AddressedParticipantStopFenceDisposition : std::uint8_t {
  kUnknown = 0,
  kEffectPreventedBeforeDispatch,
  kDispatchCommitted,
};

/// Subordinate single-participant execution mechanism used by the addressed
/// Kernel owner. It does not resolve Program roles, select a robot/group, or
/// reserve resources; those responsibilities remain outside this port.
class IResolvedParticipantExecutionEngine {
 public:
  virtual ~IResolvedParticipantExecutionEngine() = default;

  /// Close lower-priority participant work as soon as the addressed owner has
  /// linearized an exact Stop, before it publishes the physical Stop stages.
  /// This process-local, non-blocking fence does not issue a backend command,
  /// claim containment, or publish a terminal. The addressed owner remains
  /// responsible for the physical result and aggregate certificate.
  virtual AddressedParticipantStopFenceDisposition fence_addressed_participant_stop(
      std::uint64_t execution_id) noexcept {
    (void)execution_id;
    return AddressedParticipantStopFenceDisposition::kUnknown;
  }
  /// Record the addressed owner's containment for one exact participant
  /// execution after the owner has already contained its dispatched goal or
  /// proved that dispatch never occurred, and return the participant's
  /// recovery evidence. The participant commits no outcome. This
  /// process-local handoff MUST NOT issue another backend Stop command or
  /// publish another Stop certificate.
  virtual Result<AddressedParticipantStopSettlementDTO> settle_addressed_participant_stop(
      const AddressedParticipantStopSettlementRequestDTO& request) {
    (void)request;
    return Result<AddressedParticipantStopSettlementDTO>::Err(
        ErrorDTO{"addressed_participant_stop_settlement_unavailable",
                 "participant execution engine cannot settle addressed Stop evidence",
                 {{"reason", "addressed_participant_stop_settlement_unavailable"}}});
  }
  /// Retires one exact subordinate execution after the addressed Kernel has
  /// already committed an immutable containment certificate. Retirement is
  /// motion-free: it retires the prepared membership and local bookkeeping and
  /// MUST NOT reset the backend, command motion or issue another Stop.
  virtual ResetResultDTO reset_after_certified_containment(
      std::uint64_t execution_id, std::optional<std::uint64_t> expected_epoch = std::nullopt) = 0;
  /// Retires one exact Completed subordinate execution in process-local state.
  /// This method MUST NOT call a controller, vendor transport, execution
  /// backend, or any other physical-effect boundary.
  virtual AcknowledgeCompletedResultDTO acknowledge_completed(
      std::uint64_t execution_id, std::optional<std::uint64_t> expected_epoch = std::nullopt) = 0;

  /// Private single-participant realization for the addressed owner's exact,
  /// generation-fenced runtime-scale contract. The engine validates and
  /// echoes the owner-resolved binding; it never selects or mints identities.
  virtual Result<ResolvedRuntimeScaleAcceptedDTO> set_runtime_scale(
      const ResolvedRuntimeScaleCommandDTO& command) = 0;
  virtual Result<ResolvedRuntimeScaleStatusDTO> runtime_scale_status(
      const OperationId& operation_id, const ControlBindingId& control_binding_id) const = 0;

  /// Read-only selected target actuation. Existing non-gripper engines retain
  /// their position/motion behavior; the genuine gripper owner supplies its
  /// strictly configured target mode before prepared or Stop work is built.
  virtual GripperActuationKind gripper_actuation_kind(const std::string&) const {
    return GripperActuationKind::kPositionCommanded;
  }
  /// Existing engines retain commanded output; only strict authored targets
  /// may expose the explicit pneumatic none choice.
  virtual GripperOutputMode gripper_output_mode(const std::string&) const {
    return GripperOutputMode::kCommanded;
  }

  virtual GripperCommandResultDTO command_gripper(const GripperCommandRequestDTO& request) = 0;
  virtual Result<void> validate_gripper_command(const GripperCommandRequestDTO& request) const = 0;
  virtual GripperCommandStatusDTO get_gripper_status(const std::string& gripper_id) const = 0;
  /// Cancel or reset the exact subordinate gripper generation without routing
  /// through the motion state machine. Reset is invoked only after the
  /// addressed owner has either observed an ordinary terminal generation or
  /// committed an immutable containment certificate. In the latter case the
  /// backend may already have advanced to a newer idle generation.
  virtual Result<std::uint64_t> stop_gripper(const std::string& gripper_id,
                                             std::uint64_t expected_command_id,
                                             std::uint64_t expected_generation) = 0;
  virtual Result<void> reset_gripper(const std::string& gripper_id,
                                     std::uint64_t expected_command_id,
                                     std::uint64_t expected_generation) = 0;

  /// Participant-local Reset after Kernel admission. Physical implementations
  /// preserve their original gripper dispatch.
  virtual Result<void> reset_capability_command(const std::string& target,
                                                std::uint64_t command_id,
                                                std::uint64_t generation) {
    return reset_gripper(target, command_id, generation);
  }

  /// Working phase plus, once the current execution has ended, the
  /// participant fact about how it ended (`ExecutionStatus::fact`). Engines
  /// report facts; the addressed Kernel derives the operation outcome.
  virtual ExecutionStatus get_status() const = 0;
  virtual KernelRecoveryAssessmentDTO get_recovery_assessment() const { return {}; }
  virtual KernelRecoveryActionResultDTO apply_recovery_action(
      const KernelRecoveryActionRequestDTO& request) {
    return KernelRecoveryActionResultDTO{
        request.request_id, request.action, false, get_recovery_assessment(),
        ErrorDTO{"recovery_unavailable",
                 "participant execution engine does not expose recovery",
                 {{"reason", "recovery_port_unavailable"}}}};
  }
  virtual SubscriptionHandle subscribe_events(EventCallback callback) = 0;
  virtual void unsubscribe_events(SubscriptionHandle handle) = 0;
};

}  // namespace manyforge::core
