#pragma once

#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <manyforge_core/types/stop_control.hpp>
#include <manyforge_core/types/stop_event.hpp>

#include <string_view>

namespace manyforge::core {

/// A Program selection request. Stop accepts any caller (the operator's
/// principal is recorded, K-10); Run and Resume are the Program owner's.
/// Callers name groups; captured membership is the Kernel's.
struct ProgramExecutionSelectionRequestDTO final {
  PublicRequestContextDTO request_context;
  ProgramRunNumber program_run_id;
  // One bounded selection union, resolved atomically by the captured owner.
  std::vector<std::string> group_ids;
  RequestAttributionDTO attribution;
  // Individual hold release only; mutually exclusive with nonempty group_ids.
  std::optional<ProgramNodeInstanceId> node_instance_id{};
  // Stop only, from the Program owner: the Program node whose own Stop this
  // is (OD-342). The nodes it holds are attributed `stopped_by: program`.
  std::optional<ProgramNodeInstanceId> stopped_by_node_instance_id{};
  friend bool operator==(const ProgramExecutionSelectionRequestDTO&,
                         const ProgramExecutionSelectionRequestDTO&) = default;
};

/// Internal no-effect operator-alarm attention check for one captured Program
/// node: the held node a motion-producing recovery would move again. Only
/// that node's effectful target resources are tested, so an
/// alarm on disjoint resources does not block its recovery.
struct ProgramNodeMotionAttentionCheckDTO final {
  PublicRequestContextDTO request_context;
  ProgramRunNumber program_run_id;
  ProgramNodeInstanceId node_instance_id;
  RequestAttributionDTO attribution;
  friend bool operator==(const ProgramNodeMotionAttentionCheckDTO&,
                         const ProgramNodeMotionAttentionCheckDTO&) = default;
};

inline constexpr std::string_view kProgramExecutionScopeSchema =
    "manyforge.program_execution_scope.v4";
inline constexpr std::string_view kProgramExecutionSelectionSchema =
    "manyforge.program_execution_selection.v3";

/// Node display state of the scope read (unchanged wire values). Only
/// `kRunning`, `kReady` and `kHeld` are stored (`RunHoldState::node_hold`);
/// `kStopping` and `kFaulted` are derived from the per-node counters
/// (stop state model §4.2).
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

/// A held node whose hold a Program node's own Stop set (OD-342,
/// `stopped_by: program`): the held node and the requesting node, both as
/// captured node indices.
struct ProgramNodeProgramHoldDTO final {
  std::uint32_t node_index;
  std::uint32_t stopped_by_node_index;
  friend bool operator==(const ProgramNodeProgramHoldDTO&,
                         const ProgramNodeProgramHoldDTO&) = default;
};

enum class ProgramSelectionReleaseBlockReasonDTO : std::uint8_t {
  kCellMotionAdmissionClosed,
  kProgramRunAdmissionPending,
  kProgramHaltActive,
  /// A participant in the release scope is still stopping (C2).
  kStopping
};

[[nodiscard]] inline constexpr std::string_view program_selection_release_block_reason_token(
    const ProgramSelectionReleaseBlockReasonDTO reason) noexcept {
  switch (reason) {
    case ProgramSelectionReleaseBlockReasonDTO::kCellMotionAdmissionClosed:
      return "cell_motion_admission_closed";
    case ProgramSelectionReleaseBlockReasonDTO::kProgramRunAdmissionPending:
      return "program_run_admission_pending";
    case ProgramSelectionReleaseBlockReasonDTO::kProgramHaltActive:
      return "program_halt_active";
    case ProgramSelectionReleaseBlockReasonDTO::kStopping:
      return "stopping";
  }
  return "selection_release_unavailable";
}

/// The scope read (`manyforge.program_execution_scope.v4`): one memory read
/// of the claimed run's hold state.
struct ProgramExecutionScopeStateDTO final {
  CellAdmissionClaimGeneration claim_generation{0U};
  ProgramRunNumber program_run_id{0U};
  /// The Kernel's one hold revision (`hold_revision_`); releases name it.
  std::uint64_t hold_revision{};
  // Sparse, node-index ordered; omitted nodes are running. Display states,
  // stopping and faulted derived (§4.2).
  std::vector<ProgramNodeHoldDTO> held_nodes;
  std::optional<ProgramSelectionReleaseBlockReasonDTO> selection_release_block_reason{};
  /// A global rule's `hold` trip held the run (stopped_by: rule).
  std::optional<std::string> stopped_by_rule_id{};
  /// Node-index ordered: the held nodes a Program node's own Stop holds
  /// (stopped_by: program), until their release.
  std::vector<ProgramNodeProgramHoldDTO> program_held_nodes{};
  RunFrozenLevel frozen{RunFrozenLevel::kNone};
  /// The event that froze the run; absent while not frozen.
  std::optional<StopEventId> frozen_by{};
  /// A Stop arrived after the claim and before the capture.
  bool all_groups_held{};
};

/// The claimed run's hold facts for the event stream `state` (K18,
/// member `program_run_hold`). Memory only.
struct ProgramRunHoldFactsDTO final {
  ProgramRunNumber program_run_id;
  std::uint64_t hold_revision{};
  RunFrozenLevel frozen{RunFrozenLevel::kNone};
  std::optional<StopEventId> frozen_by{};
  bool all_groups_held{};
  std::optional<InterruptPolicyId> held_by_rule{};
  /// The freezing event's requested kind and source label, read from the event
  /// ring when the facts are built; absent when the run is not frozen or that
  /// record is no longer resident.
  std::optional<StopKind> frozen_kind{};
  std::optional<std::string> frozen_source_label{};
  friend bool operator==(const ProgramRunHoldFactsDTO&,
                         const ProgramRunHoldFactsDTO&) = default;
};

/// Everything the event stream's `state` carries about Stops (K18):
/// the claimed run's hold facts (absent with no claim) and the two
/// cell-wide change markers (§2.3). Not release fences.
struct StopStreamFactsDTO final {
  std::optional<ProgramRunHoldFactsDTO> program_run_hold{};
  /// The newest event; absent before the first Stop of this start.
  std::optional<StopEventId> last_stop_event_id{};
  /// Raised every time a participant ends a stop attempt (P4).
  std::uint64_t stop_progress_sequence{};
  friend bool operator==(const StopStreamFactsDTO&, const StopStreamFactsDTO&) = default;
};

/// The reply to a group Stop (S1). The Stop is never refused; an invalid
/// request is widened and `stop` says so.
struct ProgramExecutionSelectionAcceptedDTO final {
  /// The run's scope read after the Stop; absent when the request named no
  /// current claim.
  std::optional<ProgramExecutionScopeStateDTO> state;
  std::vector<std::uint32_t> selected_node_indices;
  /// The selection after the coordinated closure.
  std::vector<std::uint32_t> widened_node_indices;
  StopAcceptedDTO stop;
};

}  // namespace manyforge::core
