#pragma once

#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/stop_control.hpp>

namespace manyforge::core {

/// Internal owner request. Public adapters cannot supply captured membership.
struct ProgramExecutionSelectionRequestDTO final {
  PublicRequestContextDTO request_context;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  std::string group_id;
  RequestAttributionDTO attribution;
  friend bool operator==(const ProgramExecutionSelectionRequestDTO&,
                         const ProgramExecutionSelectionRequestDTO&) = default;
};

enum class ProgramNodeHoldStateDTO : std::uint8_t { kRunning, kHeld, kStopping, kFaulted };

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

struct ProgramExecutionScopeStateDTO final {
  RequestOwnerEpoch request_owner_epoch;
  RequestOwnerEpoch program_runner_owner_epoch;
  CellAdmissionClaimGeneration claim_generation;
  ProgramRunId program_run_id;
  std::uint64_t selection_revision;
  // Sparse, node-index ordered; omitted nodes are not held.
  std::vector<ProgramNodeHoldDTO> held_nodes;
  std::vector<ProgramNodeStopEffectDTO> pending_effects;
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
