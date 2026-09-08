#pragma once

#include <cstdint>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

enum class GripperCommandState {
  kIdle,
  kExecuting,
  kCompleted,
  kFailed,
};

inline constexpr const char* gripper_command_state_name(GripperCommandState state) noexcept {
  switch (state) {
    case GripperCommandState::kIdle:
      return "idle";
    case GripperCommandState::kExecuting:
      return "executing";
    case GripperCommandState::kCompleted:
      return "completed";
    case GripperCommandState::kFailed:
      return "failed";
  }
  return "failed";
}

enum class GripperProjectionState {
  kNotRequired,
  kPending,
  kApplied,
  kFailed,
};

inline constexpr const char* gripper_projection_state_name(GripperProjectionState state) noexcept {
  switch (state) {
    case GripperProjectionState::kNotRequired:
      return "not_required";
    case GripperProjectionState::kPending:
      return "pending";
    case GripperProjectionState::kApplied:
      return "applied";
    case GripperProjectionState::kFailed:
      return "failed";
  }
  return "failed";
}

enum class GripperInvalidationReason {
  kReset,
  kStop,
  kPause,
  kReplacement,
  kDeploymentSwap,
  kShutdown,
};

/// Exact addressed-operation identity stamped only when a prepared command is
/// released. Direct/manual gripper commands leave this absent. A Stop adapter
/// uses it to reject a stale goal before cancelling a newer command.
struct AddressedGripperCommandIdentityDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  OperationParticipantId participant_id;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;

  friend bool operator==(const AddressedGripperCommandIdentityDTO&,
                         const AddressedGripperCommandIdentityDTO&) = default;
};

struct GripperCommandRequestDTO {
  std::string gripper_id;
  std::string group_id;
  std::string joint_name;
  double target_position = 0.0;
  double duration_s = 0.0;
  std::optional<double> max_effort;

  std::string commanded_state;
  std::string actuation_kind;
  std::string state_authority;
  std::string completion_policy;
  std::string grasp_confirmation_policy;
  std::string transport_kind;
  std::vector<std::string> evidence;
  std::string evidence_state;
  /// Owner-resolved terminal deadline for this private command. The native
  /// fixed-duration mechanism rejects a duration that cannot fit this bound.
  double completion_timeout_s = 1.0;
  std::optional<AddressedGripperCommandIdentityDTO> addressed_identity = std::nullopt;
};

struct GripperCommandResultDTO {
  bool accepted = false;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  std::optional<ErrorDTO> error;
};

enum class GraspConfirmationSourceKind {
  kOperator,
  kDiscreteInput,
  kPerception,
  kForceCurrent,
};

/// A source adapter's observation, not a Program's assertion that a command
/// succeeded. observed_at_ns is in the receiving owner's clock domain; remote
/// operator commands pass the common freshness gate before it is stamped.
struct GripperGraspObservationDTO final {
  std::string gripper_id;
  std::uint64_t command_id{};
  std::uint64_t generation{};
  std::uint64_t expected_status_revision{};
  std::string observation_id;
  bool confirmed{};
  TimePoint observed_at_ns{};
  std::string attribution;

  friend bool operator==(const GripperGraspObservationDTO&,
                         const GripperGraspObservationDTO&) = default;
};

/// One bounded, immutable evidence record for the current gripper command.
/// Its source/time are independent of commanded position and joint samples.
struct GripperGraspEvidenceDTO final {
  GraspConfirmationSourceKind source_kind{GraspConfirmationSourceKind::kOperator};
  ControlBindingId source_id;
  ControlBindingRevision source_revision;
  RuntimeEpoch source_epoch;
  GripperGraspObservationDTO observation;
  TimePoint expires_at_ns{};

  friend bool operator==(const GripperGraspEvidenceDTO&, const GripperGraspEvidenceDTO&) = default;
};

[[nodiscard]] inline bool gripper_grasp_evidence_is_fresh(const GripperGraspEvidenceDTO& evidence,
                                                          TimePoint now_ns) noexcept {
  return now_ns >= evidence.observation.observed_at_ns && now_ns < evidence.expires_at_ns;
}

/// Immutable observer snapshot published by the owning gripper adapter.
/// Reading this DTO must never advance a deadline, mutate command state, or
/// project a joint into a planner scene.
struct GripperCommandStatusDTO {
  GripperCommandState state = GripperCommandState::kIdle;
  std::string gripper_id;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  std::uint64_t revision = 0;
  double progress = 0.0;
  std::optional<double> current_position;
  std::optional<double> target_position;
  std::optional<double> commanded_position;
  std::optional<double> observed_position;
  std::string commanded_state;
  std::string actuation_kind;
  std::string state_authority;
  std::string completion_policy;
  std::string grasp_confirmation_policy;
  std::string transport_kind;
  std::vector<std::string> evidence;
  std::string evidence_state;
  GripperProjectionState projection_state = GripperProjectionState::kNotRequired;
  std::string projection_reason;
  std::optional<ErrorDTO> error;
  std::optional<GripperGraspEvidenceDTO> grasp_evidence;
};

}  // namespace manyforge::core
