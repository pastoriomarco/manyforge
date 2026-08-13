#pragma once

#include <cstdint>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <manyforge_core/types/runtime_scale.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumStopSelectorTargets = 4096U;
inline constexpr std::size_t kMaximumStopIdentityBytes = 128U;
inline constexpr std::size_t kMaximumStopRequestLabelBytes = 256U;
inline constexpr std::size_t kMaximumStopReasonBytes = 1024U;
inline constexpr std::size_t kMaximumStopStaleDiagnostics = 1U + (3U * kMaximumStopSelectorTargets);
inline constexpr std::size_t kMaximumStopAcceptedPhases = 64U;
inline constexpr std::size_t kMaximumStopAcceptedParticipants = 4096U;
inline constexpr std::size_t kMaximumStopPredecessors = 64U;
inline constexpr std::int64_t kMaximumStopDurationNs = 9'007'199'254'740'991LL;

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

struct StopComponentTargetDTO final {
  ControllableComponentId controllable_component_id;
  GraphRevision expected_graph_revision;
  ComponentLifecycleGeneration expected_component_lifecycle_generation;

  friend bool operator==(const StopComponentTargetDTO&, const StopComponentTargetDTO&) = default;
};

struct StopResourceTargetDTO final {
  ResourceId resource_id;
  GraphRevision expected_graph_revision;
  std::uint64_t expected_operation_resource_generation{};
  StopGeneration expected_stop_generation;

  friend bool operator==(const StopResourceTargetDTO&, const StopResourceTargetDTO&) = default;
};

using StopSelectorDTO =
    std::variant<std::vector<StopOperationTargetDTO>, std::vector<StopComponentTargetDTO>,
                 std::vector<StopResourceTargetDTO>>;

/// Kernel-owned classification of the public Stop selector after bounded
/// resolution against the current cell snapshot. Stale caller metadata is
/// diagnostic when the semantic target is still unique; an unsafe or
/// unresolvable selector is accepted only through the Full-Stop path.
enum class StopResolutionDispositionDTO {
  kExact,
  kStaleButUnambiguouslyResolved,
  kAlreadyTerminalNoEffect,
  kPromotedToFull,
};

[[nodiscard]] constexpr std::string_view to_string(
    StopResolutionDispositionDTO disposition) noexcept {
  switch (disposition) {
    case StopResolutionDispositionDTO::kExact:
      return "exact";
    case StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved:
      return "stale_but_unambiguously_resolved";
    case StopResolutionDispositionDTO::kAlreadyTerminalNoEffect:
      return "already_terminal_no_effect";
    case StopResolutionDispositionDTO::kPromotedToFull:
      return "promoted_to_full";
  }
  return {};
}

enum class StopScopeResolutionReasonDTO {
  kExactCurrentScope,
  kStaleMetadataCurrentScope,
  kKnownTerminalOrIdleNoEffect,
  kMalformedOrdinaryIntent,
  kUnknownOperation,
  kUnknownComponent,
  kUnknownResource,
  kRequestIdentityConflict,
  kOverlappingStopTransactions,
  kEmergencyReserveCoalesced,
  kExplicitFullStop,
};

[[nodiscard]] constexpr std::string_view to_string(StopScopeResolutionReasonDTO reason) noexcept {
  switch (reason) {
    case StopScopeResolutionReasonDTO::kExactCurrentScope:
      return "exact_current_scope";
    case StopScopeResolutionReasonDTO::kStaleMetadataCurrentScope:
      return "stale_metadata_current_scope";
    case StopScopeResolutionReasonDTO::kKnownTerminalOrIdleNoEffect:
      return "known_terminal_or_idle_no_effect";
    case StopScopeResolutionReasonDTO::kMalformedOrdinaryIntent:
      return "malformed_ordinary_intent";
    case StopScopeResolutionReasonDTO::kUnknownOperation:
      return "unknown_operation";
    case StopScopeResolutionReasonDTO::kUnknownComponent:
      return "unknown_component";
    case StopScopeResolutionReasonDTO::kUnknownResource:
      return "unknown_resource";
    case StopScopeResolutionReasonDTO::kRequestIdentityConflict:
      return "request_identity_conflict";
    case StopScopeResolutionReasonDTO::kOverlappingStopTransactions:
      return "overlapping_stop_transactions";
    case StopScopeResolutionReasonDTO::kEmergencyReserveCoalesced:
      return "emergency_reserve_coalesced";
    case StopScopeResolutionReasonDTO::kExplicitFullStop:
      return "explicit_full_stop";
  }
  return {};
}

enum class StopScopeStaleFieldDTO {
  kRequestOwnerEpoch,
  kOperationRevision,
  kOperationStopGeneration,
  kGraphRevision,
  kComponentLifecycleGeneration,
  kOperationResourceGeneration,
  kResourceStopGeneration,
  kCellStopGeneration,
};

[[nodiscard]] constexpr std::string_view to_string(StopScopeStaleFieldDTO field) noexcept {
  switch (field) {
    case StopScopeStaleFieldDTO::kRequestOwnerEpoch:
      return "request_owner_epoch";
    case StopScopeStaleFieldDTO::kOperationRevision:
      return "operation_revision";
    case StopScopeStaleFieldDTO::kOperationStopGeneration:
      return "operation_stop_generation";
    case StopScopeStaleFieldDTO::kGraphRevision:
      return "graph_revision";
    case StopScopeStaleFieldDTO::kComponentLifecycleGeneration:
      return "component_lifecycle_generation";
    case StopScopeStaleFieldDTO::kOperationResourceGeneration:
      return "operation_resource_generation";
    case StopScopeStaleFieldDTO::kResourceStopGeneration:
      return "resource_stop_generation";
    case StopScopeStaleFieldDTO::kCellStopGeneration:
      return "cell_stop_generation";
  }
  return {};
}

using StopScopeDiagnosticTargetDTO =
    std::variant<std::monostate, OperationId, ControllableComponentId, ResourceId>;
using StopScopeStaleValueDTO =
    std::variant<std::uint64_t, RequestOwnerEpoch, GraphRevision, OperationRevision,
                 ComponentLifecycleGeneration, StopGeneration>;

/// Closed mismatch evidence. Field, target, and values retain their semantic
/// types; implementations do not manufacture free-form diagnostic strings.
struct StopScopeStaleDiagnosticDTO final {
  StopScopeStaleFieldDTO field{StopScopeStaleFieldDTO::kRequestOwnerEpoch};
  StopScopeDiagnosticTargetDTO target{};
  StopScopeStaleValueDTO expected{};
  StopScopeStaleValueDTO current{};

  friend bool operator==(const StopScopeStaleDiagnosticDTO&,
                         const StopScopeStaleDiagnosticDTO&) = default;
};

/// Ordinary scoped Stop intent. Exactly one selector alternative is present by
/// construction. Each alternative is non-empty, canonical, and bounded before
/// it crosses a Kernel authority boundary.
struct StopIntentDTO final {
  PublicRequestContextDTO request_context;
  StopSelectorDTO selector;
  std::optional<std::string> reason;
  /// Receiving-adapter provenance. Public payloads cannot populate this field.
  RequestAttributionDTO attribution{};

  friend bool operator==(const StopIntentDTO&, const StopIntentDTO&) = default;
};

struct FullStopIntentDTO final {
  PublicRequestContextDTO request_context;
  StopGeneration expected_stop_generation;
  RequestAttributionDTO attribution{};

  friend bool operator==(const FullStopIntentDTO&, const FullStopIntentDTO&) = default;
};

/// Closed local-shape validation for normalized internal Stop values. This
/// verifies bounds, exact selector alternative, canonical ordering, and typed
/// expectation presence. Live target resolution and stale/promote semantics
/// remain Kernel authority decisions.
[[nodiscard]] std::optional<std::string_view> stop_intent_field_error(
    const StopIntentDTO& intent) noexcept;
[[nodiscard]] std::optional<std::string_view> full_stop_intent_field_error(
    const FullStopIntentDTO& intent) noexcept;

struct StopAffectedOperationRevisionDTO final {
  OperationId operation_id;
  OperationRevision operation_revision_after_acceptance;

  friend bool operator==(const StopAffectedOperationRevisionDTO&,
                         const StopAffectedOperationRevisionDTO&) = default;
};

/// Exact immutable acceptance truth inherited by a promoted aggregate Stop.
/// The remaining duration is sampled again at the aggregate acceptance time;
/// it is never copied from an older public acceptance projection.
struct StopPredecessorAcceptanceRefDTO final {
  StopId predecessor_stop_id;
  StopGeneration predecessor_stop_generation;
  ContentDigest captured_scope_digest;
  std::int64_t remaining_outer_duration_ns_at_join{};

  friend bool operator==(const StopPredecessorAcceptanceRefDTO&,
                         const StopPredecessorAcceptanceRefDTO&) = default;
};

/// One immutable phase allocation published with Stop acceptance. The fixed
/// offset retains the selected plan meaning; the remaining offset is the
/// owner-stamped shortened duration at this publication and is never an
/// absolute timestamp.
struct StopAcceptedPhaseBudgetDTO final {
  StopBudgetPhaseId phase_id;
  std::int64_t fixed_deadline_offset_ns{};
  std::int64_t remaining_deadline_offset_ns{};

  friend bool operator==(const StopAcceptedPhaseBudgetDTO&,
                         const StopAcceptedPhaseBudgetDTO&) = default;
};

/// One participant-local phase table from the exact immutable Stop plan
/// prepared before operation dispatch. Phase identifiers are intentionally
/// scoped by this row: two participants may use the same phase spelling with
/// different retained offsets.
struct StopAcceptedParticipantBudgetDTO final {
  StopId origin_stop_id;
  StopGeneration origin_stop_generation;
  OperationId operation_id;
  OperationParticipantId participant_id;
  ContentDigest participant_stop_invocation_plan_digest;
  std::vector<StopAcceptedPhaseBudgetDTO> phases;

  friend bool operator==(const StopAcceptedParticipantBudgetDTO&,
                         const StopAcceptedParticipantBudgetDTO&) = default;
};

struct StopAcceptedDTO final {
  RequestId request_id;
  RequestReplayStatusDTO replay_status;
  StopId stop_id;
  StopGeneration stop_generation;
  ContentDigest requested_selector_digest;
  StopResolutionDispositionDTO resolution_disposition{StopResolutionDispositionDTO::kExact};
  StopScopeResolutionReasonDTO resolution_reason{StopScopeResolutionReasonDTO::kExactCurrentScope};
  std::vector<StopScopeStaleDiagnosticDTO> stale_diagnostics;
  ContentDigest captured_scope_digest;
  std::vector<StopPredecessorAcceptanceRefDTO> joined_predecessor_stops;
  std::vector<StopAffectedOperationRevisionDTO> affected_operation_revisions;
  std::int64_t fixed_outer_duration_ns{};
  std::int64_t fixed_kernel_completion_reserve_ns{};
  std::int64_t remaining_outer_duration_ns{};
  std::int64_t accepted_at_ns{};
  std::vector<StopAcceptedParticipantBudgetDTO> participant_budget_allocations;

  friend bool operator==(const StopAcceptedDTO&, const StopAcceptedDTO&) = default;
};

/// Closed local validation of an immutable owner-minted acceptance result.
/// Digest recomputation requires the original intent/resolution and remains a
/// separate exact join at the owner boundary.
[[nodiscard]] std::optional<std::string_view> stop_accepted_field_error(
    const StopAcceptedDTO& accepted) noexcept;

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

[[nodiscard]] std::optional<std::string_view> operation_control_request_field_error(
    const OperationControlRequestDTO& request) noexcept;

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
