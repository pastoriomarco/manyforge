#pragma once

#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/stop_control.hpp>

#include <string_view>

namespace manyforge::core {

/// Internal owner request. Public adapters cannot supply captured membership.
struct ProgramExecutionSelectionRequestDTO final {
  PublicRequestContextDTO request_context;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  // One bounded selection union, resolved atomically by the captured owner.
  std::vector<std::string> group_ids;
  RequestAttributionDTO attribution;
  // Individual hold release only; mutually exclusive with nonempty group_ids.
  std::optional<ProgramNodeInstanceId> node_instance_id{};
  friend bool operator==(const ProgramExecutionSelectionRequestDTO&,
                         const ProgramExecutionSelectionRequestDTO&) = default;
};

/// Internal no-effect operator-alarm attention check for one captured Program
/// node: the held node a motion-producing recovery would move again. Only
/// that node's effectful target resources are tested (spec 450 §5.2), so an
/// alarm on disjoint resources does not block its recovery.
struct ProgramNodeMotionAttentionCheckDTO final {
  PublicRequestContextDTO request_context;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  ProgramNodeInstanceId node_instance_id;
  RequestAttributionDTO attribution;
  friend bool operator==(const ProgramNodeMotionAttentionCheckDTO&,
                         const ProgramNodeMotionAttentionCheckDTO&) = default;
};

enum class ProgramNodeHoldStateDTO : std::uint8_t {
  kRunning,
  kReady,
  kHeld,
  kStopping,
  kFaulted
};

struct ProgramNodeHoldDTO final {
  std::uint32_t node_index;
  ProgramNodeHoldStateDTO state;
};

/// Only unresolved physical containment. Terminal reduction folds its result
/// into the node hold and removes this reference, independently of history.
struct ProgramNodeStopEffectDTO final {
  std::uint32_t node_index;
  OperationId operation_id;
  OperationParticipantId participant_id;
  std::string node_attempt_id;
  StopId stop_id;
  StopGeneration stop_generation;
};

enum class ProgramSelectionReleaseBlockReasonDTO : std::uint8_t {
  kCellMotionAdmissionClosed,
  kCellMotionAdmissionUnreconciled,
  kProgramRunAdmissionPending,
  kProgramHaltActive
};

[[nodiscard]] inline constexpr std::string_view program_selection_release_block_reason_token(
    const ProgramSelectionReleaseBlockReasonDTO reason) noexcept {
  switch (reason) {
    case ProgramSelectionReleaseBlockReasonDTO::kCellMotionAdmissionClosed:
      return "cell_motion_admission_closed";
    case ProgramSelectionReleaseBlockReasonDTO::kCellMotionAdmissionUnreconciled:
      return "cell_motion_admission_unreconciled";
    case ProgramSelectionReleaseBlockReasonDTO::kProgramRunAdmissionPending:
      return "program_run_admission_pending";
    case ProgramSelectionReleaseBlockReasonDTO::kProgramHaltActive:
      return "program_halt_active";
  }
  return "selection_release_unavailable";
}

struct ProgramExecutionScopeStateDTO final {
  RequestOwnerEpoch request_owner_epoch;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  std::uint64_t selection_revision;
  // Sparse, node-index ordered; omitted nodes are not held.
  std::vector<ProgramNodeHoldDTO> held_nodes;
  std::vector<ProgramNodeStopEffectDTO> pending_effects;
  std::optional<ProgramSelectionReleaseBlockReasonDTO> selection_release_block_reason{};
  /// A global rule's `hold` trip held the run (stopped_by: rule, AUD-6).
  std::optional<std::string> stopped_by_rule_id{};
};

struct ProgramExecutionSelectionAcceptedDTO final {
  // Unresolvable recognized selections promote through the existing Full
  // Stop contract; they cannot fabricate a captured Program scope.
  std::optional<ProgramExecutionScopeStateDTO> state;
  std::vector<std::uint32_t> selected_node_indices;
  std::vector<std::uint32_t> widened_node_indices;
  // Absent for idle/logical holds or adoption of existing containment. This
  // is the existing physical transaction, never a synthetic certificate.
  std::optional<StopAcceptedDTO> fresh_containment;
  std::optional<ErrorDTO> selection_error{};
  RequestReplayStatusDTO replay_status{RequestReplayStatusDTO::kAcceptedNew};
};

}  // namespace manyforge::core
