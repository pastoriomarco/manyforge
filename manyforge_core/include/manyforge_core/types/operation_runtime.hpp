#pragma once

#include <cstdint>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/io_motion_rule.hpp>
#include <manyforge_core/types/io_output.hpp>
#include <manyforge_core/types/participant_binding_identity.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <manyforge_core/types/runtime_scale.hpp>
#include <manyforge_core/types/stop_certificate.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <stdexcept>
#include <utility>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumStopSelectorTargets = 4096U;
inline constexpr std::size_t kMaximumStopIdentityBytes = 128U;
inline constexpr std::size_t kMaximumStopRequestLabelBytes = 256U;
inline constexpr std::size_t kMaximumStopReasonBytes = 1024U;
inline constexpr std::size_t kMaximumStopMalformedReasonBytes = 256U;
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
  ControllerGoalId command_id;
  ControllerGoalGeneration command_generation;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ParticipantBindingIdentityDTO binding_identity;
  OperationParticipantPayloadKind payload_kind{OperationParticipantPayloadKind::kMotion};
  OperationStateKind state{OperationStateKind::kResolving};
  double progress{};
  std::vector<ResourceId> resources;
  std::optional<ErrorDTO> terminal_error;
  std::optional<ProgramNodeExecutionScopeDTO> execution_scope{};
  /// Actual participant proof; absence grants no effect/settlement knowledge.
  std::optional<ParticipantExecutionFact> terminal_fact{};
  std::vector<IoMotionRuleOutcomeDTO> io_rule_outcomes{};
  /// Actual correlated provider fact, separate from a local dispatch receipt.
  std::optional<IoOutputEvidence> output_evidence{};

  OperationParticipantStateDTO() = default;

  OperationParticipantStateDTO(
      OperationParticipantId participant, ControllerGoalId goal, ControllerGoalGeneration goal_generation,
      ControllableComponentId component, CommandTargetId target, CapabilityId capability,
      ParticipantBindingIdentityDTO identity, OperationParticipantPayloadKind kind,
      OperationStateKind phase, double supplied_progress, std::vector<ResourceId> owned_resources,
      std::optional<ErrorDTO> error, std::optional<ProgramNodeExecutionScopeDTO> scope = {},
      std::optional<ParticipantExecutionFact> fact = {}, std::vector<IoMotionRuleOutcomeDTO> rules = {},
      std::optional<IoOutputEvidence> output = {})
      : participant_id(std::move(participant)), command_id(std::move(goal)),
        command_generation(goal_generation), controllable_component_id(std::move(component)),
        command_target_id(std::move(target)), capability_id(std::move(capability)),
        binding_identity(std::move(identity)), payload_kind(kind), state(phase), progress(supplied_progress),
        resources(std::move(owned_resources)), terminal_error(std::move(error)), execution_scope(std::move(scope)),
        terminal_fact(std::move(fact)), io_rule_outcomes(std::move(rules)), output_evidence(output) {}

  OperationParticipantStateDTO(
      OperationParticipantId participant, ControllerGoalId goal, ControllerGoalGeneration goal_generation,
      ControllableComponentId component, CommandTargetId target, CapabilityId capability,
      ControlDomainId domain, ControlDomainRevision domain_revision, ControlBindingId control_binding,
      ControlBindingRevision control_revision, std::optional<CapabilityProviderBindingId> provider,
      std::optional<CapabilityProviderBindingRevision> provider_revision,
      OperationParticipantPayloadKind kind, OperationStateKind phase, double supplied_progress,
      std::vector<ResourceId> owned_resources, std::optional<ErrorDTO> error,
      std::optional<ProgramNodeExecutionScopeDTO> scope = {}, std::optional<ParticipantExecutionFact> fact = {},
      std::vector<IoMotionRuleOutcomeDTO> rules = {})
      : OperationParticipantStateDTO(
            std::move(participant), std::move(goal), goal_generation, std::move(component), std::move(target),
            std::move(capability),
            ParticipantBindingIdentityDTO{std::move(domain), std::move(domain_revision),
                ControllerParticipantBindingIdentityDTO{std::move(control_binding), std::move(control_revision),
                    provider && provider_revision
                        ? std::optional(CapabilityProviderBindingIdentityDTO{std::move(*provider), std::move(*provider_revision)})
                        : std::nullopt}},
            kind, phase, supplied_progress, std::move(owned_resources), std::move(error), std::move(scope),
            std::move(fact), std::move(rules)) {
    if (provider.has_value() != provider_revision.has_value())
      throw std::invalid_argument("participant capability provider identity is partial");
  }

  friend bool operator==(const OperationParticipantStateDTO&,
                         const OperationParticipantStateDTO&) = default;
};

/// Logical Program outcome, independent of physical participant settlement.
enum class OperationPreemptionKind : std::uint8_t {
  kPreempted,
  kSupersededUnsent,
};

[[nodiscard]] constexpr std::string_view to_string(OperationPreemptionKind value) noexcept {
  switch (value) {
    case OperationPreemptionKind::kPreempted:
      return "preempted";
    case OperationPreemptionKind::kSupersededUnsent:
      return "superseded_unsent";
  }
  return {};
}

struct OperationPreemptionActorDTO final {
  OperationId operation_id;
  ProgramNodeInstanceId node_instance_id;
  std::string node_attempt_id;

  friend bool operator==(const OperationPreemptionActorDTO&,
                         const OperationPreemptionActorDTO&) = default;
};

struct OperationPreemptionDTO final {
  OperationPreemptionKind kind{OperationPreemptionKind::kPreempted};
  OperationPreemptionActorDTO by;

  friend bool operator==(const OperationPreemptionDTO&, const OperationPreemptionDTO&) = default;
};

/// Observed physical handover choice for one accepted newcomer. This is
/// diagnostic evidence only; it grants no Stop, dispatch or recovery authority.
enum class MotionTakeoverPathDTO : std::uint8_t {
  kMovingReplacement,
  kSpringBackHold,
  kOrdinaryContainment,
};

[[nodiscard]] constexpr std::string_view to_string(MotionTakeoverPathDTO value) noexcept {
  switch (value) {
    case MotionTakeoverPathDTO::kMovingReplacement:
      return "moving_replacement";
    case MotionTakeoverPathDTO::kSpringBackHold:
      return "spring_back_hold";
    case MotionTakeoverPathDTO::kOrdinaryContainment:
      return "ordinary_containment";
  }
  return {};
}

struct MotionTakeoverDecisionDTO final {
  OperationId incumbent_operation_id;
  MotionTakeoverPathDTO path{MotionTakeoverPathDTO::kMovingReplacement};
  std::optional<ErrorDTO> cause{};
  // Kernel monotonic time, never a wall-clock timestamp. Elapsed includes
  // admission/coordination as well as planning; it is not planning-only time.
  std::int64_t decided_at_ns{};
  std::int64_t elapsed_since_acceptance_ns{};
  std::int64_t preparation_budget_ns{};
  std::optional<StopCertificateRefDTO> stop_certificate{};

  [[nodiscard]] bool locally_valid() const noexcept {
    const bool moving = path == MotionTakeoverPathDTO::kMovingReplacement;
    return !incumbent_operation_id.empty() && !to_string(path).empty() &&
           decided_at_ns >= 0 && elapsed_since_acceptance_ns >= 0 &&
           elapsed_since_acceptance_ns <= decided_at_ns && preparation_budget_ns > 0 &&
           (moving ? !cause && !stop_certificate : cause && !cause->code.empty()) &&
           (!stop_certificate || stop_certificate->locally_valid());
  }

  friend bool operator==(const MotionTakeoverDecisionDTO&,
                         const MotionTakeoverDecisionDTO&) = default;
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
  // Private Kernel/journal capture order, never a public control precondition.
  // Zero is reserved for unsequenced historical records; fresh Kernel captures
  // use a checked int64-range value independent of clock and control revision.
  std::uint64_t journal_capture_order{};
  /// Acceptance-time logical outcome; never proves stopped/settled effects.
  std::optional<OperationPreemptionDTO> preemption{};
  /// One accepted attempt's choice, retained with its ordinary operation history.
  std::optional<MotionTakeoverDecisionDTO> motion_takeover{};

  friend bool operator==(const OperationStateDTO&, const OperationStateDTO&) = default;
};

[[nodiscard]] inline bool has_program_execution_scope(const OperationStateDTO& state) noexcept {
  for (const auto& participant : state.participants) {
    if (participant.execution_scope.has_value()) return true;
  }
  return false;
}

struct StopOperationTargetDTO final {
  OperationId operation_id;
  OperationRevision expected_operation_revision;
  StopGeneration expected_stop_generation;

  friend bool operator==(const StopOperationTargetDTO&, const StopOperationTargetDTO&) = default;
};

/// Internal Program-owner selector. Public transports do not expose this
/// alternative; the addressed Kernel expands it atomically from its current
/// operation table so a phase transition cannot escape Program Stop.
struct StopProgramRunTargetDTO final {
  ProgramRunId program_run_id;

  friend bool operator==(const StopProgramRunTargetDTO&, const StopProgramRunTargetDTO&) = default;
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
                 std::vector<StopResourceTargetDTO>, std::vector<StopProgramRunTargetDTO>>;

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
  kUnknownProgramRun,
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
    case StopScopeResolutionReasonDTO::kUnknownProgramRun:
      return "unknown_program_run";
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

/// Internal-only value minted after a transport has recognized the exact Stop
/// method, route, and receiving Kernel but cannot normalize the bounded public
/// payload. It deliberately carries no caller-selected scope: the Kernel must
/// conservatively promote it to Full Stop.
struct MalformedStopIngressDTO final {
  RequestId request_id;
  RequestOwnerEpoch receiver_owner_epoch;
  RequestAttributionDTO attribution{};
  std::string recognized_route;
  std::string reason;

  friend bool operator==(const MalformedStopIngressDTO&, const MalformedStopIngressDTO&) = default;
};

/// Closed local-shape validation for normalized internal Stop values. This
/// verifies bounds, exact selector alternative, canonical ordering, and typed
/// expectation presence. Live target resolution and stale/promote semantics
/// remain Kernel authority decisions.
[[nodiscard]] std::optional<std::string_view> stop_intent_field_error(
    const StopIntentDTO& intent) noexcept;
[[nodiscard]] std::optional<std::string_view> full_stop_intent_field_error(
    const FullStopIntentDTO& intent) noexcept;
[[nodiscard]] std::optional<std::string_view> malformed_stop_ingress_field_error(
    const MalformedStopIngressDTO& ingress) noexcept;

struct StopAffectedOperationRevisionDTO final {
  OperationId operation_id;
  OperationRevision operation_revision_after_acceptance;

  friend bool operator==(const StopAffectedOperationRevisionDTO&,
                         const StopAffectedOperationRevisionDTO&) = default;
};

/// Exact immutable acceptance truth inherited by a promoted aggregate Stop.
struct StopPredecessorAcceptanceRefDTO final {
  StopId predecessor_stop_id;
  StopGeneration predecessor_stop_generation;
  ContentDigest captured_scope_digest;

  friend bool operator==(const StopPredecessorAcceptanceRefDTO&,
                         const StopPredecessorAcceptanceRefDTO&) = default;
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
  /// Q51: no shared Stop budget is accepted; each robot's own deadlines are
  /// computed at stage dispatch from its own profile and lane.
  std::int64_t accepted_at_ns{};

  friend bool operator==(const StopAcceptedDTO&, const StopAcceptedDTO&) = default;
};

/// Closed local validation of an immutable owner-minted acceptance result.
/// Digest recomputation requires the original intent/resolution and remains a
/// separate exact join at the owner boundary.
[[nodiscard]] std::optional<std::string_view> stop_accepted_field_error(
    const StopAcceptedDTO& accepted) noexcept;

enum class OperationControlKind {
  kReset,
  kAcknowledgeCompleted,
};

struct UncertainOutputAcknowledgementDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  friend bool operator==(const UncertainOutputAcknowledgementDTO&,
                         const UncertainOutputAcknowledgementDTO&) = default;
};

struct OperationControlRequestDTO final {
  PublicRequestContextDTO request_context;
  OperationId operation_id;
  OperationRevision expected_operation_revision;
  std::optional<std::string> reason;
  /// Receiving-adapter provenance. Public payloads cannot populate this field.
  RequestAttributionDTO attribution{};
  /// Private authored control-node origin, distinct from the target operation.
  /// Public control payloads cannot supply this field.
  std::optional<ProgramNodeExecutionScopeDTO> execution_scope{};
  /// Reset only, explicit operator choice. Absence never acknowledges unknown
  /// output effects; automatic retirement/Scenario Close cannot populate it.
  std::optional<std::vector<UncertainOutputAcknowledgementDTO>> acknowledged_uncertain_outputs{};

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
  OperationControlKind control{OperationControlKind::kReset};
  OperationRevision operation_revision_after_acceptance;
  std::int64_t accepted_at_ns{};

  friend bool operator==(const OperationControlAcceptedDTO&,
                         const OperationControlAcceptedDTO&) = default;
};

}  // namespace manyforge::core
