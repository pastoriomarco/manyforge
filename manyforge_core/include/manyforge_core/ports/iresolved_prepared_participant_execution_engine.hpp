#pragma once

#include <cstdint>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/gripper.hpp>
#include <manyforge_core/types/io_output.hpp>
#include <manyforge_core/types/io_rule_stop_response.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/prepared_execution.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace manyforge::core {

namespace detail {
struct MotionResumeIntent;
}

struct ResolvedParticipantStopStageDTO;
struct SpringBackReturnPointDTO;
struct StopCertificateDTO;

struct ResolvedPreparedMotionSpringReturnContextDTO final {
  std::shared_ptr<const ResolvedParticipantStopStageDTO> stage;
  std::shared_ptr<const SpringBackReturnPointDTO> point;
  /// Attach computation to this still-owned sent candidate; not a new session.
  std::uint64_t abandoned_preparation_id{};
};

/// Complete owner-issued authority for preparing one motion participant.
///
/// The addressed Kernel mints operation/participant/goal identity and owns the
/// sealed binding, readiness, and ordinary Stop plan. The
/// subordinate motion engine owns planning and validation evidence. Neither
/// side can construct a prepared participant without the other's exact facts.
struct ResolvedPreparedMotionReplacementContextDTO final {
  PreparedMotionReplacementSourceDTO source;
  /// Strictly positive whole preparation budget resolved from cell Deployment.
  /// No implicit reuse of the ordinary planning-only timeout.
  std::int64_t preparation_budget_ns{};
  std::optional<ResolvedPreparedMotionSpringReturnContextDTO> spring_return{};
  /// Optional cap measured from the same preparation start; connector and
  /// validation keep the whole preparation deadline.
  std::optional<std::int64_t> planning_budget_ns{};
};

struct ResolvedMotionInterruptScopeActionDTO final {
  MotionInterruptScopeId scope_id;
  InterruptPreemptionAction preemption_action;
  std::optional<StopEscalationStageId> stop_entry_stage_id{};
  std::optional<StopEscalationStageId> success_through_stage_id{};
};

struct ResolvedPreparedMotionExecutionRequestDTO final {
  ExecuteRequestDTO execution_request;
  OperationRevision operation_revision;
  RuntimeParticipantBinding binding;
  RuntimeParticipantStopReadiness stop_readiness;
  std::optional<double> requested_tcp_linear_speed_mps;
  double deployment_maximum_tcp_linear_speed_mps{};
  std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
  MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
  std::shared_ptr<const PreparedParticipantStopPlanDTO> ordinary_stop_plan;
  std::shared_ptr<const PreparedParticipantStopPlanDTO> fault_stop_plan;
  // Existing owner supplies semantic destinations only, never old dispatch authority.
  std::shared_ptr<const detail::MotionResumeIntent> continuation{};
  std::optional<ResolvedPreparedMotionReplacementContextDTO> replacement{};
  /// Exact sealed actions, supplied by the existing addressed admission owner.
  /// An empty/missing action cannot implicitly authorize a desired trip.
  std::vector<ResolvedMotionInterruptScopeActionDTO> interrupt_scope_actions{};
};

enum class ResolvedPreparedMotionExecutionStateDTO : std::uint8_t {
  kPreparing = 0,
  kPrepared,
  kDispatchRequested,
  kDispatched,
  kPreparationFailed,
  kCancelledPreDispatch,
  kColdPrepared,  // sealed computation only; backend Prepare has not run
};

/// Data belonging to the observed physical session. The reserved identity is
/// not addressable until exact physical retirement promotes this product.
struct ResolvedPreparedMotionSpringColdResultDTO final {
  std::uint64_t reserved_execution_id{};
  ResolvedPreparedMotionExecutionStateDTO state{
      ResolvedPreparedMotionExecutionStateDTO::kPreparing};
  std::shared_ptr<const PreparedOperationParticipantDTO> participant;
  std::optional<ErrorDTO> error;
};

/// Observer-safe, process-local preparation state. Large canonical products
/// are retained by shared immutable ownership; this value is never serialized
/// and carries no public authority.
struct ResolvedPreparedMotionExecutionSnapshotDTO final {
  std::uint64_t execution_id{};
  ResolvedPreparedMotionExecutionStateDTO state{
      ResolvedPreparedMotionExecutionStateDTO::kPreparing};
  std::shared_ptr<const PreparedOperationParticipantDTO> participant;
  std::optional<PrepareParticipantResultDTO> backend_prepare_result;
  std::optional<ErrorDTO> error;
  /// Exact trajectory position for last_dispatched_participant;
  /// absent while moving, after an uncertain replacement, or without support.
  /// This is continuation data, not a Stop certificate or dispatch permission.
  std::optional<double> stopped_trajectory_time_s{};
  /// Last verified dispatch.
  /// Shared immutable reference only; no additional product copy or authority.
  std::shared_ptr<const PreparedOperationParticipantDTO> last_dispatched_participant{};
  std::optional<PreparedMotionReplacementObservationDTO> replacement{};
  std::optional<ResolvedPreparedMotionSpringColdResultDTO> spring_cold_result{};
};

/// Complete owner-issued authority for preparing one non-trajectory command
/// participant. The prepared participant is already sealed by the addressed
/// Kernel; the private command request is the exact compiled transport input
/// derived from that value and its sealed runtime binding.
struct ResolvedPreparedGripperCommandAuthorityDTO final {
  RuntimeParticipantStopReadiness stop_readiness;
  std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
  MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
  GripperCommandRequestDTO command_request;
};

struct ResolvedPreparedIoOutputCommandAuthorityDTO final {
  IoSignalId signal_id;
  ResourceId output_resource;
  bool value;
  ResolvedLifecycleProviderOccurrenceDTO provider;
  IoOutputCompletionPolicyDTO completion_policy;
};

struct ResolvedPreparedCapabilityCommandExecutionRequestDTO final {
  std::shared_ptr<const PreparedOperationParticipantDTO> participant;
  RuntimeParticipantBinding binding;
  std::variant<ResolvedPreparedGripperCommandAuthorityDTO,
               ResolvedPreparedIoOutputCommandAuthorityDTO> authority;

  ResolvedPreparedCapabilityCommandExecutionRequestDTO(
      std::shared_ptr<const PreparedOperationParticipantDTO> supplied_participant,
      RuntimeParticipantBinding supplied_binding, RuntimeParticipantStopReadiness stop_readiness,
      std::vector<HardStopResourceLimitDTO> limits, MotionInterruptNodeCategory category,
      GripperCommandRequestDTO command_request)
      : participant(std::move(supplied_participant)), binding(std::move(supplied_binding)),
        authority(ResolvedPreparedGripperCommandAuthorityDTO{
            std::move(stop_readiness), std::move(limits), category, std::move(command_request)}) {}

  ResolvedPreparedCapabilityCommandExecutionRequestDTO(
      std::shared_ptr<const PreparedOperationParticipantDTO> supplied_participant,
      RuntimeParticipantBinding supplied_binding,
      ResolvedPreparedIoOutputCommandAuthorityDTO output)
      : participant(std::move(supplied_participant)), binding(std::move(supplied_binding)),
        authority(std::move(output)) {}

  [[nodiscard]] const ResolvedPreparedGripperCommandAuthorityDTO& gripper() const {
    return std::get<ResolvedPreparedGripperCommandAuthorityDTO>(authority);
  }
  [[nodiscard]] const ResolvedPreparedIoOutputCommandAuthorityDTO* output() const noexcept {
    return std::get_if<ResolvedPreparedIoOutputCommandAuthorityDTO>(&authority);
  }
};

enum class ResolvedPreparedCapabilityCommandStateDTO : std::uint8_t {
  kPrepared = 0,
  kDispatchRequested,
  kDispatched,
  kDispatchFailed,
  kPreparationFailed,
  kCancelledPreDispatch,
};

/// Observer-safe process-local command preparation state. A dispatched result
/// is retained so exact goal/digest replay never emits a second transport
/// command.
struct ResolvedPreparedCapabilityCommandSnapshotDTO final {
  std::uint64_t preparation_id{};
  ResolvedPreparedCapabilityCommandStateDTO state{
      ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed};
  std::shared_ptr<const PreparedOperationParticipantDTO> participant;
  std::optional<PrepareParticipantResultDTO> backend_prepare_result;
  std::optional<ControllerGoalId> controller_goal_id;
  std::optional<ControllerGoalGeneration> controller_goal_generation;
  std::optional<GripperCommandResultDTO> dispatch_result;
  std::optional<ErrorDTO> error;
  /// Correlated provider evidence is distinct from the neutral dispatch receipt.
  std::optional<IoOutputObservationDTO> output_observation{};
  /// Same serialized observation as output_observation, avoiding split reads.
  std::optional<ExecutionStatus> output_execution_status{};
};

/// Two-phase subordinate motion engine used by the addressed Kernel's
/// coordinated prepare-all barrier. Prepare performs planning, validation, and
/// backend Prepare asynchronously but cannot dispatch physical motion.
class IResolvedPreparedParticipantExecutionEngine {
 public:
  virtual ~IResolvedPreparedParticipantExecutionEngine() = default;

  virtual Result<std::uint64_t> PrepareMotion(
      ResolvedPreparedMotionExecutionRequestDTO request) = 0;

  virtual Result<ResolvedPreparedMotionExecutionSnapshotDTO> ObservePreparedMotion(
      std::uint64_t execution_id) const = 0;

  /// Release one exact prepared participant after the owner has observed that
  /// every coordinated participant is prepared. Digest disagreement and
  /// repeated or stale identities fail before backend dispatch.
  virtual Result<void> DispatchPreparedMotion(std::uint64_t execution_id,
                                              const ContentDigest& prepared_contract_digest) = 0;

  /// Bounded pre-dispatch cancellation. This requests cancellation of active
  /// planning/validation and releases a waiting prepare barrier; it never calls
  /// a transport Stop primitive and is idempotent for a terminal session.
  virtual Result<void> CancelPreparedMotion(std::uint64_t execution_id) = 0;

  /// Complete cleanup of an exact canceled/known-no-effect candidate, or a
  /// candidate contained by the exact settled source Stop, while
  /// keeping it observable until this call succeeds. Called outside addressed
  /// state; never sends a command or retires a possibly live candidate. Success
  /// proves cleanup only, not NoEffect, and leaves the stopped source resident.
  virtual Result<bool> RetireCancelledPreparedMotion(std::uint64_t /*candidate_id*/) {
    return Result<bool>::Err(ErrorCode::kUnsupportedCapability);
  }

  /// Read one exact incumbent source without changing preparation or effects.
  virtual Result<PreparedMotionReplacementSourceDTO> CaptureMotionReplacementSource(
      std::uint64_t /*execution_id*/) const {
    return Result<PreparedMotionReplacementSourceDTO>::Err(ErrorCode::kUnsupportedCapability);
  }

  /// Nonblocking adoption at the existing worker safe point. False means not
  /// parked/ready yet; no ownership transfer or worker wait occurs in that case.
  virtual Result<bool> CommitPreparedMotionReplacement(
      std::uint64_t /*candidate_id*/, const PreparedMotionReplacementSourceDTO& /*source*/,
      PreparedMotionReplacementCommitDTO /*commit*/) {
    return Result<bool>::Err(ErrorCode::kUnsupportedCapability);
  }

  /// Reuse one cold sealed spring-return candidate only after exact source
  /// Stop/local settlement. Called outside addressed state; may retire the
  /// completed source worker/membership, but never dispatches or resets.
  virtual Result<bool> PrepareColdMotionAfterStop(
      std::uint64_t /*candidate_id*/, const StopCertificateDTO& /*certificate*/) {
    return Result<bool>::Err(ErrorCode::kUnsupportedCapability);
  }

  /// Side-effect-free preparation of one exact capability-command product.
  /// The returned identity addresses only this retained preparation; no
  /// transport command may be issued before DispatchPreparedCapabilityCommand.
  virtual Result<std::uint64_t> PrepareCapabilityCommand(
      ResolvedPreparedCapabilityCommandExecutionRequestDTO request) = 0;

  virtual Result<ResolvedPreparedCapabilityCommandSnapshotDTO> ObservePreparedCapabilityCommand(
      std::uint64_t preparation_id) const = 0;

  /// Release one exact prepared command under the Kernel-minted goal. Exact
  /// repetition returns the retained dispatch result without a second effect.
  virtual Result<GripperCommandResultDTO> DispatchPreparedCapabilityCommand(
      std::uint64_t preparation_id, const ContentDigest& prepared_contract_digest,
      const ControllerGoalId& controller_goal_id,
      ControllerGoalGeneration controller_goal_generation) = 0;

  /// Cancel one command preparation before dispatch. This is idempotent and
  /// cannot call a physical Stop primitive.
  virtual Result<void> CancelPreparedCapabilityCommand(std::uint64_t preparation_id) = 0;
};

}  // namespace manyforge::core
