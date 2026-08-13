#pragma once

#include <cstdint>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/runtime_participant_policy.hpp>
#include <manyforge_core/runtime_participant_stop_readiness.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/gripper.hpp>
#include <manyforge_core/types/prepared_execution.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace manyforge::core {

/// Complete owner-issued authority for preparing one motion participant.
///
/// The addressed Kernel mints operation/participant/goal identity and owns the
/// sealed binding, readiness, runtime policy, and ordinary Stop plan. The
/// subordinate motion engine owns planning and validation evidence. Neither
/// side can construct a prepared participant without the other's exact facts.
struct ResolvedPreparedMotionExecutionRequestDTO final {
  ExecuteRequestDTO execution_request;
  OperationRevision operation_revision;
  RuntimeParticipantBinding binding;
  RuntimeParticipantStopReadiness stop_readiness;
  RuntimeParticipantPolicySelection runtime_policy;
  std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
  MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
  std::shared_ptr<const PreparedParticipantStopPlanDTO> ordinary_stop_plan;
};

enum class ResolvedPreparedMotionExecutionStateDTO : std::uint8_t {
  kPreparing = 0,
  kPrepared,
  kDispatchRequested,
  kDispatched,
  kPreparationFailed,
  kCancelledPreDispatch,
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
};

/// Complete owner-issued authority for preparing one non-trajectory command
/// participant. The prepared participant is already sealed by the addressed
/// Kernel; the private command request is the exact compiled transport input
/// derived from that value and its sealed runtime binding.
struct ResolvedPreparedCapabilityCommandExecutionRequestDTO final {
  std::shared_ptr<const PreparedOperationParticipantDTO> participant;
  RuntimeParticipantBinding binding;
  RuntimeParticipantStopReadiness stop_readiness;
  std::vector<HardStopResourceLimitDTO> retained_default_stop_limits;
  MotionInterruptNodeCategory stop_node_category{MotionInterruptNodeCategory::kGeneric};
  GripperCommandRequestDTO command_request;
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
