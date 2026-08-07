#pragma once

#include <cstdint>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <manyforge_core/types/runtime_scale.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

enum class OperationStateKind {
  kQueued,
  kResolving,
  kPlanning,
  kValidating,
  kPrepared,
  kDispatching,
  kExecuting,
  kPaused,
  kStopping,
  kCompleted,
  kStopped,
  kFailed,
  kInterrupted,
  kInterventionRequired,
  kCancelledPreDispatch,
};

enum class OperationParticipantPayloadKind {
  kMotion,
  kCapabilityCommand,
};

struct OperationParticipantStateDTO final {
  OperationParticipantId participant_id;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  std::optional<CapabilityProviderBindingId> capability_provider_binding_id;
  std::optional<CapabilityProviderBindingRevision> capability_provider_binding_revision;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  OperationStateKind state{OperationStateKind::kResolving};
  double progress{};
  std::vector<ResourceId> resources;
  std::optional<ErrorDTO> terminal_error;

  friend bool operator==(const OperationParticipantStateDTO&,
                         const OperationParticipantStateDTO&) = default;
};

/// Bounded observation for one Kernel-owned operation. It carries final
/// addressed identities rather than subordinate engine handles or legacy
/// robot/group selectors.
struct OperationStateDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  ProgramBindingId program_binding_id;
  ProgramRunId program_run_id;
  OperationCoordinationPolicy coordination_policy{OperationCoordinationPolicy::kIndependent};
  OperationStateKind state{OperationStateKind::kResolving};
  std::vector<OperationParticipantStateDTO> participants;
  std::int64_t updated_at_ns{};
  std::optional<StopId> active_stop_id;
  std::optional<StopGeneration> active_stop_generation;
  std::optional<RuntimeScaleGeneration> runtime_scale_generation;

  friend bool operator==(const OperationStateDTO&, const OperationStateDTO&) = default;
};

struct StopOperationTargetDTO final {
  OperationId operation_id;
  OperationRevision expected_operation_revision;
  StopGeneration expected_stop_generation;

  friend bool operator==(const StopOperationTargetDTO&, const StopOperationTargetDTO&) = default;
};

/// Ordinary operation Stop intent. I8 extends the same resource-addressed
/// identity with policy/certificate settlement; it does not replace this
/// caller-owned scope.
struct StopIntentDTO final {
  PublicRequestContextDTO request_context;
  std::vector<StopOperationTargetDTO> operations;
  std::optional<std::string> reason;
  /// Receiving-adapter provenance. Public payloads cannot populate this field.
  RequestAttributionDTO attribution{};

  friend bool operator==(const StopIntentDTO&, const StopIntentDTO&) = default;
};

struct StopAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  StopId stop_id;
  StopGeneration stop_generation;
  std::vector<OperationId> operations;
  OperationRevision operation_revision_after_acceptance;
  std::int64_t accepted_at_ns{};

  friend bool operator==(const StopAcceptedDTO&, const StopAcceptedDTO&) = default;
};

enum class OperationControlKind {
  kPause,
  kResume,
  kReset,
  kAcknowledgeCompleted,
};

struct OperationControlRequestDTO final {
  PublicRequestContextDTO request_context;
  OperationId operation_id;
  OperationRevision expected_operation_revision;
  std::optional<std::string> reason;
  /// Receiving-adapter provenance. Public payloads cannot populate this field.
  RequestAttributionDTO attribution{};

  friend bool operator==(const OperationControlRequestDTO&,
                         const OperationControlRequestDTO&) = default;
};

struct OperationControlAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  OperationId operation_id;
  OperationRevision accepted_against_revision;
  OperationControlKind control{OperationControlKind::kPause};
  OperationRevision operation_revision_after_acceptance;
  std::int64_t accepted_at_ns{};

  friend bool operator==(const OperationControlAcceptedDTO&,
                         const OperationControlAcceptedDTO&) = default;
};

}  // namespace manyforge::core
