#pragma once

#include <cstddef>
#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_core/types/runtime_object_binding.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/timeouts.hpp>
#include <manyforge_core/types/tolerances.hpp>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaxOperationParticipants = 4096U;
inline constexpr std::size_t kMaxOperationCommandOptions = 64U;
inline constexpr std::size_t kMaxOperationRuntimeObjectBindings = 4096U;

enum class OperationCoordinationPolicy {
  kIndependent,
  kCoordinatedAll,
};

struct GripperPositionCommandValueDTO final {
  CoordinatePositionDTO position;
  std::optional<CoordinateEffortDTO> max_effort;
  friend bool operator==(const GripperPositionCommandValueDTO&,
                         const GripperPositionCommandValueDTO&) = default;
};

using CapabilityCommandValueDTO = std::variant<GripperPositionCommandValueDTO>;

struct CommandDurationOptionDTO final {
  CommandOptionId option_id;
  double value_s{};
  friend bool operator==(const CommandDurationOptionDTO&,
                         const CommandDurationOptionDTO&) = default;
};

struct CapabilityCommandIntentDTO final {
  CommandSchemaId command_schema_id;
  CommandSchemaRevision command_schema_revision;
  CapabilityCommandValueDTO command_value;
  double completion_timeout_s{};
  std::vector<CommandDurationOptionDTO> caller_options;
  friend bool operator==(const CapabilityCommandIntentDTO&,
                         const CapabilityCommandIntentDTO&) = default;
};

struct MotionParticipantIntentDTO final {
  MoveId move_id;
  FrameRoleId base_frame_role_id;
  FrameRoleId tcp_frame_role_id;
  TolerancesDTO tolerances;
  SpeedProfileDTO speed_profile;
  TimeoutsDTO timeouts;
  CacheMode cache_mode{CacheMode::kDefault};
  PlanningProfileDTO planning_profile{PlanningProfileDTO::kFirstFeasible};
  MotionTargetModeDTO target_mode{MotionTargetModeDTO::kAbsolute};
  std::optional<ReferenceDTO> target_reference;
  std::optional<ReferenceDTO> offset_reference;
  MotionSpecDTO motion;
  std::optional<ContactPolicyId> contact_policy_id;
  /// Source-local URDF joint names scoped by `target_role_id`.
  std::map<std::string, JointDynamicsLimitDTO> per_joint_dynamics_limits;
  std::vector<MotionInterruptScopeDTO> motion_interrupt_scopes;
  /// Public selection of one declared Program runtime-object key. Admission
  /// resolves and validates these rows against the exact Program Binding and
  /// retained logical scene; equal spelling is never treated as proof.
  std::vector<RuntimeObjectReferenceBindingDTO> runtime_object_bindings{};
  friend bool operator==(const MotionParticipantIntentDTO&,
                         const MotionParticipantIntentDTO&) = default;
};

using OperationParticipantPayloadDTO =
    std::variant<MotionParticipantIntentDTO, CapabilityCommandIntentDTO>;

struct OperationParticipantIntentDTO final {
  ComponentRoleId component_role_id;
  TargetRoleId target_role_id;
  CapabilityId required_capability_id;
  std::optional<ActionSpanDTO> action_span;
  OperationParticipantPayloadDTO payload;
  friend bool operator==(const OperationParticipantIntentDTO&,
                         const OperationParticipantIntentDTO&) = default;
};

struct OperationIntentDTO final {
  PublicRequestContextDTO request_context;
  ProgramId program_id;
  ProgramRevision program_revision;
  ProgramBindingId program_binding_id;
  ProgramRunId program_run_id;
  GraphRevision expected_graph_revision;
  RetainedBundleId expected_retained_bundle_id;
  std::optional<SceneRevision> expected_scene_revision;
  std::vector<OperationParticipantIntentDTO> participants;
  OperationCoordinationPolicy coordination_policy{OperationCoordinationPolicy::kIndependent};
  /// Receiving-adapter provenance. Public payloads cannot populate this field.
  RequestAttributionDTO attribution{};
  friend bool operator==(const OperationIntentDTO&, const OperationIntentDTO&) = default;
};

struct OperationParticipantAssignmentDTO final {
  std::size_t input_index{};
  OperationParticipantId participant_id;
  friend bool operator==(const OperationParticipantAssignmentDTO&,
                         const OperationParticipantAssignmentDTO&) = default;
};

struct OperationAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  OperationId operation_id;
  OperationRevision operation_revision;
  std::vector<OperationParticipantAssignmentDTO> participant_assignments;
  std::int64_t accepted_at_ns{};
  friend bool operator==(const OperationAcceptedDTO&, const OperationAcceptedDTO&) = default;
};

/// Pure public-boundary validation. Runtime binding, current-generation,
/// resource, scene, and backend admission are intentionally later Kernel
/// checks and are not inferred here.
[[nodiscard]] Result<void> validate_operation_intent(const OperationIntentDTO& intent);

}  // namespace manyforge::core
