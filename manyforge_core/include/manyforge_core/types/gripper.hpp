#pragma once

#include <cstdint>
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
};

}  // namespace manyforge::core
