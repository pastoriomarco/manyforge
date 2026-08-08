#pragma once

#include <manyforge_core/pick_place_candidate.hpp>
#include <manyforge_core/ports/iaddressed_operation_owner.hpp>
#include <manyforge_core/ports/iscene_authority.hpp>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

enum class PickPlaceExecutionKindDTO {
  kPick,
  kPlace,
};

enum class PickPlaceSequencePhaseDTO {
  kIdle,
  kApproach,
  kDescend,
  kToolCommand,
  kSceneTransition,
  kRetreat,
  kCompleted,
  kFailed,
  kInterventionRequired,
};

struct PickPlaceExecutionPlanDTO final {
  PickPlaceExecutionKindDTO kind{PickPlaceExecutionKindDTO::kPick};
  SceneResourceId selected_object_id{std::string{}};
  ProjectionRevisionDTO predecessor_projection_revision;
  ProjectionRevisionDTO preview_successor_projection_revision;
  MotionSpecDTO approach_motion;
  MotionSpecDTO descend_motion;
  MotionSpecDTO retreat_motion;
  SceneTransactionRequestDTO scene_transition;
};

/// Four ordinary addressed operations plus one owner-issued scene transition.
/// Motion payloads are templates: the coordinator replaces only their motion
/// values with the exact already-previewed plan and preserves every Program,
/// target, capability, attribution, timeout, and contact-policy field.
struct PickPlaceSequenceRequestDTO final {
  std::string sequence_id;
  PickPlaceExecutionPlanDTO plan;
  OperationIntentDTO approach_operation;
  OperationIntentDTO descend_operation;
  OperationIntentDTO tool_operation;
  OperationIntentDTO retreat_operation;
};

struct PickPlaceSequenceStatusDTO final {
  std::string sequence_id;
  PickPlaceExecutionKindDTO kind{PickPlaceExecutionKindDTO::kPick};
  PickPlaceSequencePhaseDTO phase{PickPlaceSequencePhaseDTO::kIdle};
  SceneResourceId selected_object_id{std::string{}};
  std::vector<OperationId> completed_operations;
  std::optional<OperationId> active_operation_id;
  std::optional<ProjectionRevisionDTO> committed_projection_revision;
  std::optional<ErrorDTO> terminal_error;
};

[[nodiscard]] Result<PickPlaceExecutionPlanDTO> make_pick_execution_plan(
    const PlannedPickCandidateDTO& planned);
[[nodiscard]] Result<PickPlaceExecutionPlanDTO> make_place_execution_plan(
    const PlannedPlaceDTO& planned,
    const SceneResourceId& selected_object_id);

/// NRT sequencing layer over the existing durable operation and scene owners.
/// It does not own robot/gripper state, collision truth, or scene truth. It
/// advances only after an owner-issued terminal operation state, acknowledges
/// that exact revision, and never compensates a completed tool effect with an
/// automatic reopen/drop.
class PickPlaceSequenceCoordinator final {
 public:
  PickPlaceSequenceCoordinator(IAddressedOperationOwner& operations,
                               ISceneAuthority& scene)
      : operations_(operations), scene_(scene) {}

  [[nodiscard]] Result<PickPlaceSequenceStatusDTO> Begin(
      PickPlaceSequenceRequestDTO request);
  [[nodiscard]] Result<PickPlaceSequenceStatusDTO> Reconcile();
  [[nodiscard]] PickPlaceSequenceStatusDTO status() const;

 private:
  [[nodiscard]] Result<void> DispatchCurrentPhase();
  [[nodiscard]] Result<void> CompleteActiveOperation(
      const OperationStateDTO& operation);
  [[nodiscard]] Result<void> CommitSceneTransition();
  void SetTerminal(PickPlaceSequencePhaseDTO phase, ErrorDTO error);

  IAddressedOperationOwner& operations_;
  ISceneAuthority& scene_;
  mutable std::mutex mutex_;
  std::optional<PickPlaceSequenceRequestDTO> request_;
  PickPlaceSequenceStatusDTO status_;
};

[[nodiscard]] constexpr std::string_view to_string(PickPlaceExecutionKindDTO kind) noexcept {
  switch (kind) {
    case PickPlaceExecutionKindDTO::kPick:
      return "pick";
    case PickPlaceExecutionKindDTO::kPlace:
      return "place";
  }
  return {};
}

[[nodiscard]] constexpr std::string_view to_string(PickPlaceSequencePhaseDTO phase) noexcept {
  switch (phase) {
    case PickPlaceSequencePhaseDTO::kIdle:
      return "idle";
    case PickPlaceSequencePhaseDTO::kApproach:
      return "approach";
    case PickPlaceSequencePhaseDTO::kDescend:
      return "descend";
    case PickPlaceSequencePhaseDTO::kToolCommand:
      return "tool_command";
    case PickPlaceSequencePhaseDTO::kSceneTransition:
      return "scene_transition";
    case PickPlaceSequencePhaseDTO::kRetreat:
      return "retreat";
    case PickPlaceSequencePhaseDTO::kCompleted:
      return "completed";
    case PickPlaceSequencePhaseDTO::kFailed:
      return "failed";
    case PickPlaceSequencePhaseDTO::kInterventionRequired:
      return "intervention_required";
  }
  return {};
}

}  // namespace manyforge::core
