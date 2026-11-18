#pragma once

#include <cstdint>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/io_motion_rule.hpp>
#include <manyforge_core/types/participant_binding_identity.hpp>
#include <manyforge_core/types/operation_intent.hpp>
#include <manyforge_core/types/runtime_scale.hpp>
#include <manyforge_core/types/stop_event.hpp>
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

/// One participant's stop state as the operation read shows it (§6.3.1).
/// Filled by the read projection from the participant's stop state; the
/// Kernel does not store it on the operation state at Stop acceptance.
/// Absent while the participant was never covered by a Stop.
struct ParticipantStopObservationDTO final {
  StopPhase phase{StopPhase::kRunning};
  std::uint16_t attempt{};
  StopEventId stop_event_id{0U};
  StopMode mode{StopMode::kOrdinary};
  StopAttributionClass attribution_class{StopAttributionClass::kProgramOwn};
  /// `participant_stop_invocation_plan_digest` of the executed plan.
  std::optional<ContentDigest> plan_digest{};
  /// Read from the executed plan by index; absent when no stage contained it.
  std::optional<StopEscalationStageId> containing_stage_id{};
  std::optional<std::uint8_t> containing_stage_index{};
  bool escalated{};

  friend bool operator==(const ParticipantStopObservationDTO&,
                         const ParticipantStopObservationDTO&) = default;
};

struct OperationParticipantStateDTO final {
  OperationParticipantId participant_id;
  ControllerGoalId command_id;
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
  std::optional<ParticipantStopObservationDTO> stop{};

  OperationParticipantStateDTO() = default;

  OperationParticipantStateDTO(
      OperationParticipantId participant, ControllerGoalId goal,
      ControllableComponentId component, CommandTargetId target, CapabilityId capability,
      ParticipantBindingIdentityDTO identity, OperationParticipantPayloadKind kind,
      OperationStateKind phase, double supplied_progress, std::vector<ResourceId> owned_resources,
      std::optional<ErrorDTO> error, std::optional<ProgramNodeExecutionScopeDTO> scope = {},
      std::optional<ParticipantExecutionFact> fact = {}, std::vector<IoMotionRuleOutcomeDTO> rules = {})
      : participant_id(participant), command_id(goal),
        controllable_component_id(std::move(component)),
        command_target_id(std::move(target)), capability_id(std::move(capability)),
        binding_identity(std::move(identity)), payload_kind(kind), state(phase), progress(supplied_progress),
        resources(std::move(owned_resources)), terminal_error(std::move(error)), execution_scope(std::move(scope)),
        terminal_fact(std::move(fact)), io_rule_outcomes(std::move(rules)) {}

  OperationParticipantStateDTO(
      OperationParticipantId participant, ControllerGoalId goal,
      ControllableComponentId component, CommandTargetId target, CapabilityId capability,
      ControlDomainId domain, ControlDomainRevision domain_revision, ControlBindingId control_binding,
      ControlBindingRevision control_revision, std::optional<CapabilityProviderBindingId> provider,
      std::optional<CapabilityProviderBindingRevision> provider_revision,
      OperationParticipantPayloadKind kind, OperationStateKind phase, double supplied_progress,
      std::vector<ResourceId> owned_resources, std::optional<ErrorDTO> error,
      std::optional<ProgramNodeExecutionScopeDTO> scope = {}, std::optional<ParticipantExecutionFact> fact = {},
      std::vector<IoMotionRuleOutcomeDTO> rules = {})
      : OperationParticipantStateDTO(
            participant, goal, std::move(component), std::move(target),
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
  OperationNumber operation_id;
  ProgramNodeInstanceId node_instance_id;
  NodeAttemptNumber node_attempt_id;

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
  OperationNumber incumbent_operation_id;
  MotionTakeoverPathDTO path{MotionTakeoverPathDTO::kMovingReplacement};
  std::optional<ErrorDTO> cause{};
  // Kernel monotonic time, never a wall-clock timestamp. Elapsed includes
  // admission/coordination as well as planning; it is not planning-only time.
  std::int64_t decided_at_ns{};
  std::int64_t elapsed_since_acceptance_ns{};
  std::int64_t preparation_budget_ns{};
  /// The Stop event that stopped the incumbent (non-moving paths only).
  std::optional<StopEventId> stop_event_id{};
  // The planning cap that applied: the explicit planning budget, or 4/5 of
  // the preparation budget. This is the deadline a planning timeout trips.
  std::int64_t planning_budget_ns{};

  [[nodiscard]] bool locally_valid() const noexcept {
    const bool moving = path == MotionTakeoverPathDTO::kMovingReplacement;
    return incumbent_operation_id.nonzero() && !to_string(path).empty() &&
           decided_at_ns >= 0 && elapsed_since_acceptance_ns >= 0 &&
           elapsed_since_acceptance_ns <= decided_at_ns && preparation_budget_ns > 0 &&
           planning_budget_ns >= 0 &&
           (moving ? !cause && !stop_event_id : cause && !cause->code.empty()) &&
           (!stop_event_id || stop_event_id->nonzero());
  }

  friend bool operator==(const MotionTakeoverDecisionDTO&,
                         const MotionTakeoverDecisionDTO&) = default;
};

/// Bounded observation for one Kernel-owned operation. It carries final
/// addressed identities rather than subordinate engine handles or legacy
/// robot/group selectors.
struct OperationStateDTO final {
  OperationNumber operation_id;
  OperationRevision operation_revision;
  ProgramBindingId program_binding_id;
  /// Absent for manual work (a manual handover has its parent instead).
  std::optional<ProgramRunNumber> program_run_id;
  OperationCoordinationPolicy coordination_policy{OperationCoordinationPolicy::kIndependent};
  OperationStateKind state{OperationStateKind::kResolving};
  std::vector<OperationParticipantStateDTO> participants;
  std::int64_t updated_at_ns{};
  /// The Stop event that started the newest attempt on this operation (E6).
  /// Set when a Stop first covers it and never cleared while it is resident.
  std::optional<StopEventId> stop_event_id;
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
  OperationNumber operation_id;
  OperationRevision expected_operation_revision;

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

  friend bool operator==(const StopResourceTargetDTO&, const StopResourceTargetDTO&) = default;
};

using StopSelectorDTO =
    std::variant<std::vector<StopOperationTargetDTO>, std::vector<StopComponentTargetDTO>,
                 std::vector<StopResourceTargetDTO>>;

/// Kernel-owned classification of one Stop target after bounded resolution
/// against the current operation table. Stale caller metadata is diagnostic
/// when the semantic target is still unique; an unresolvable target makes the
/// request invalid and the invalid-Stop rule widens it (PD-1,
/// `StopWidenReason`).
enum class StopResolutionDispositionDTO {
  kExact,
  kStaleButUnambiguouslyResolved,
  kAlreadyTerminalNoEffect,
  kInvalid,
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
    case StopResolutionDispositionDTO::kInvalid:
      return "invalid";
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
  }
  return {};
}

/// Stale-metadata diagnostic of a Stop reply (K15): only the operation
/// revision is compared. The named target's current scope was stopped.
struct StopStaleDiagnosticDTO final {
  OperationNumber operation_id;
  OperationRevision expected_operation_revision;
  OperationRevision current_operation_revision;

  friend bool operator==(const StopStaleDiagnosticDTO&, const StopStaleDiagnosticDTO&) = default;
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

/// Body of the cell-wide Stops that name no target: Stop all, Halt and the
/// Program node's cell Stop (`stop_all`, `halt`, `cell_stop`). The Kernel
/// entry, not the body, says which level it is.
struct FullStopIntentDTO final {
  PublicRequestContextDTO request_context;
  RequestAttributionDTO attribution{};

  friend bool operator==(const FullStopIntentDTO&, const FullStopIntentDTO&) = default;
};

/// Forced Program End (S16): the runner ends its run (Program Reset's first
/// step). Internal attribution only.
struct ProgramRunEndIntentDTO final {
  PublicRequestContextDTO request_context;
  ProgramRunNumber program_run_id;
  RequestAttributionDTO attribution{};

  friend bool operator==(const ProgramRunEndIntentDTO&, const ProgramRunEndIntentDTO&) = default;
};

/// Internal-only value minted after a transport has recognized the exact Stop
/// method, route, and receiving Kernel for a Stop it does not forward as
/// asked: its bounded public payload cannot be normalized
/// (`recognized_stop_payload_invalid`, `recognized_stop_body_oversized`), or
/// it names another application start or none
/// (`recognized_stop_start_not_current`). It deliberately carries no
/// caller-selected scope: the Kernel widens it by the recognized route
/// (PD-1): a group, operation or Stop-all route becomes Stop all, a Halt
/// route or an unknown route becomes Halt. The widen reason is `stale_start`
/// for `recognized_stop_start_not_current`, else `malformed`
/// (`unreadable_kind` for an unknown route). `source_label` is the caller's
/// label where the transport has a validated one; without it (or with an
/// invalid one) the record's label is `recognized_stop_ingress`.
struct RecognizedStopIngressDTO final {
  RequestKey request_key;
  RequestAttributionDTO attribution{};
  std::string recognized_route;
  std::string reason;
  std::optional<std::string> source_label{};

  friend bool operator==(const RecognizedStopIngressDTO&,
                         const RecognizedStopIngressDTO&) = default;
};

/// Closed local-shape validation for normalized internal Stop values. This
/// verifies bounds, exact selector alternative, canonical ordering, and typed
/// expectation presence. Live target resolution and stale/promote semantics
/// remain Kernel authority decisions.
[[nodiscard]] std::optional<std::string_view> stop_intent_field_error(
    const StopIntentDTO& intent) noexcept;
[[nodiscard]] std::optional<std::string_view> full_stop_intent_field_error(
    const FullStopIntentDTO& intent) noexcept;
[[nodiscard]] std::optional<std::string_view> program_run_end_intent_field_error(
    const ProgramRunEndIntentDTO& intent) noexcept;
[[nodiscard]] std::optional<std::string_view> recognized_stop_ingress_field_error(
    const RecognizedStopIngressDTO& ingress) noexcept;
/// A Stop request's source label is valid (bounded NFC UTF-8, as in the
/// request context of every Stop).
[[nodiscard]] bool stop_source_label_valid(std::string_view label) noexcept;

/// One Stop-qualified participant a Stop event covers (K15). `started`:
/// this event started its stop attempt; false: it was already stopping.
struct StopCoveredParticipantDTO final {
  OperationNumber operation_id;
  /// Index of the participant in its operation (`OperationParticipantId`
  /// ordinal).
  std::uint32_t participant_ordinal{};
  bool started{};

  friend bool operator==(const StopCoveredParticipantDTO&,
                         const StopCoveredParticipantDTO&) = default;
};

/// The reply to every accepted Stop request (K15, §7.2), built after the
/// Kernel lock is released from a copy of the event record. A Stop is never
/// refused: an invalid request is widened and says so (`applied` differs from
/// `requested`, `widen_reason`).
struct StopAcceptedDTO final {
  RequestKey request_key;
  RequestReplayStatusDTO replay_status{RequestReplayStatusDTO::kAcceptedNew};
  StopEventId event_id{0U};
  StopKind requested{StopKind::kOperation};
  StopKind applied{StopKind::kOperation};
  StopWidenReason widen_reason{StopWidenReason::kNone};
  /// The run the Stop names (S1, S10, S16 only).
  std::optional<ProgramRunNumber> program_run_id{};
  /// At most the stored diagnostics; `stale_total` counts all of them.
  std::vector<StopStaleDiagnosticDTO> stale_diagnostics;
  std::uint32_t stale_total{};
  /// Every covered participant, including those already stopping.
  std::vector<StopCoveredParticipantDTO> covered;
  /// The Kernel's hold revision after this event (G6).
  std::uint64_t hold_revision{};
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

struct OperationControlRequestDTO final {
  PublicRequestContextDTO request_context;
  OperationNumber operation_id;
  OperationRevision expected_operation_revision;
  std::optional<std::string> reason;
  /// Receiving-adapter provenance. Public payloads cannot populate this field.
  RequestAttributionDTO attribution{};
  /// Private authored control-node origin, distinct from the target operation.
  /// Public control payloads cannot supply this field.
  std::optional<ProgramNodeExecutionScopeDTO> execution_scope{};

  friend bool operator==(const OperationControlRequestDTO&,
                         const OperationControlRequestDTO&) = default;
};

[[nodiscard]] std::optional<std::string_view> operation_control_request_field_error(
    const OperationControlRequestDTO& request) noexcept;

struct OperationControlAcceptedDTO final {
  RequestKey request_key;
  RequestReplayStatusDTO replay_status;
  OperationNumber operation_id;
  OperationRevision accepted_against_revision;
  OperationControlKind control{OperationControlKind::kReset};
  OperationRevision operation_revision_after_acceptance;
  std::int64_t accepted_at_ns{};

  friend bool operator==(const OperationControlAcceptedDTO&,
                         const OperationControlAcceptedDTO&) = default;
};

}  // namespace manyforge::core
