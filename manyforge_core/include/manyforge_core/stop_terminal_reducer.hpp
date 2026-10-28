#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/participant_stop_stage.hpp>
#include <manyforge_core/types/stop_certificate.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumStopTerminalPredecessors = 64U;
inline constexpr std::size_t kMaximumStopScopeCaptureHistory = 65U;
inline constexpr std::size_t kMaximumStopTerminalScopeEntries = 4096U;
inline constexpr std::size_t kMaximumStopTerminalAttemptResults =
    kMaximumStopTerminalScopeEntries * kMaxStopEscalationStages;
inline constexpr std::size_t kMaximumStopTerminalEvidenceItems = 262144U;

enum class StopRequestedSelectorKindDTO : std::uint8_t {
  kOrdinaryOperations = 0,
  kOrdinaryComponents,
  kOrdinaryResources,
  kExplicitFull,
  kMalformedIngress,
  kInternalProgramRun,
};

struct StopRequestedSelectorSnapshotDTO final {
  StopRequestedSelectorKindDTO kind{StopRequestedSelectorKindDTO::kExplicitFull};
  ContentDigest requested_selector_digest;
  std::optional<StopSelectorDTO> selector;
  std::optional<std::string> malformed_ingress_reason;

  friend bool operator==(const StopRequestedSelectorSnapshotDTO&,
                         const StopRequestedSelectorSnapshotDTO&) = default;
};

struct StopParticipantScopeKeyDTO final {
  OperationId operation_id;
  OperationParticipantId participant_id;

  friend bool operator==(const StopParticipantScopeKeyDTO&,
                         const StopParticipantScopeKeyDTO&) = default;
};

using StopScopeSubjectKeyDTO = std::variant<StopParticipantScopeKeyDTO, ResourceId>;

struct StopScopeEntryKeyDTO final {
  StopId origin_stop_id;
  StopGeneration origin_stop_generation;
  StopScopeSubjectKeyDTO subject;

  friend bool operator==(const StopScopeEntryKeyDTO&, const StopScopeEntryKeyDTO&) = default;
};

enum class StopScopeEntryDispositionDTO : std::uint8_t {
  kStoppable = 0,
  kNotStoppable,
};

enum class StopScopeDispatchStateDTO : std::uint8_t {
  kNeverDispatchedCancelled = 0,
  kDispatched,
  kEffectUncertain,
};

struct StopParticipantScopeFactsDTO final {
  OperationRevision operation_revision_after_acceptance;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  CapabilityId capability_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerGoalId controller_goal_id;
  ControllerGoalGeneration controller_goal_generation;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  RuntimeEpoch provider_runtime_epoch;
  std::vector<ResourceId> resources;
  ContainmentGranularity containment_granularity{ContainmentGranularity::kResource};
  /// Whether the participant execution owner committed the same terminal
  /// occurrence after physical containment. Physical Stop evidence remains in
  /// attempted_stage_results even when this handoff is still unsettled.
  enum class TerminalSettlementState : std::uint8_t {
    kNotRequired = 0,
    kPending,
    kSettled,
    kContainedUnsettled,
  } terminal_settlement{TerminalSettlementState::kPending};

  friend bool operator==(const StopParticipantScopeFactsDTO&,
                         const StopParticipantScopeFactsDTO&) = default;
};

using StopScopeEntryFactsDTO = std::variant<StopParticipantScopeFactsDTO, ResourceId>;

struct StopScopeEntryDTO final {
  StopScopeEntryKeyDTO key;
  StopScopeEntryFactsDTO facts;
  StopScopeEntryDispositionDTO disposition{StopScopeEntryDispositionDTO::kStoppable};
  StopScopeDispatchStateDTO dispatch_state{StopScopeDispatchStateDTO::kDispatched};
  std::optional<StopEvidencePolicyId> stop_evidence_policy_id;
  std::optional<StopEvidencePolicyRevision> stop_evidence_policy_revision;
  std::optional<ContentDigest> participant_stop_invocation_plan_digest;
  std::int64_t accepted_at_ns{};

  friend bool operator==(const StopScopeEntryDTO&, const StopScopeEntryDTO&) = default;
};

enum class StopStageResultAuthorityDTO : std::uint8_t {
  kAuthoritative = 0,
  kSupersededLate,
};

struct StopAttemptedStageResultDTO final {
  StopScopeEntryKeyDTO scope_key;
  std::uint64_t result_sequence{};
  std::int64_t completed_at_ns{};
  StopStageResultAuthorityDTO authority{StopStageResultAuthorityDTO::kAuthoritative};
  ParticipantStopStageResultDTO result;

  friend bool operator==(const StopAttemptedStageResultDTO&,
                         const StopAttemptedStageResultDTO&) = default;
};

struct StopCertificateEvidenceItemDTO final {
  StopScopeEntryKeyDTO scope_key;
  StopEscalationStageId stage_id;
  std::size_t stage_index{};
  std::uint64_t result_sequence{};
  StopEvidenceItemDTO evidence_item;

  friend bool operator==(const StopCertificateEvidenceItemDTO&,
                         const StopCertificateEvidenceItemDTO&) = default;
};

struct StopInterruptCorrelationDTO final {
  InterruptSourceId interrupt_source_id;
  InterruptPolicyId interrupt_policy_id;
  MotionInterruptScopeId motion_interrupt_scope_id;
  std::optional<InterruptLatchId> interrupt_latch_id;

  friend bool operator==(const StopInterruptCorrelationDTO&,
                         const StopInterruptCorrelationDTO&) = default;
};

struct StopScopeCaptureHistoryDTO final {
  std::uint64_t capture_sequence{};
  StopResolutionDispositionDTO resolution_disposition{StopResolutionDispositionDTO::kExact};
  StopScopeResolutionReasonDTO resolution_reason{StopScopeResolutionReasonDTO::kExactCurrentScope};
  ContentDigest captured_scope_digest;
  std::vector<StopPredecessorAcceptanceRefDTO> predecessor_stops;
  std::vector<StopAffectedOperationRevisionDTO> newly_fenced_operation_revisions;
  std::vector<ResourceId> newly_captured_resources;

  friend bool operator==(const StopScopeCaptureHistoryDTO&,
                         const StopScopeCaptureHistoryDTO&) = default;
};

enum class StopDurableProjectionStateDTO : std::uint8_t {
  kNotApplicable = 0,
  kPending,
  kRecorded,
  kFailed,
};

struct StopDurableProjectionStatusDTO final {
  StopDurableProjectionStateDTO state{StopDurableProjectionStateDTO::kNotApplicable};
  std::optional<ErrorDTO> error;
  std::optional<std::uint64_t> retry_generation;

  friend bool operator==(const StopDurableProjectionStatusDTO&,
                         const StopDurableProjectionStatusDTO&) = default;
};

enum class StopCertificateResultDTO : std::uint8_t {
  kSucceededNominal = 0,
  kSucceededEscalated,
  kPartial,
  kFailedUncontained,
};

struct StopCertificateDTO final {
  StopCertificateId stop_certificate_id;
  StopCertificateRevision stop_certificate_revision;
  StopId stop_id;
  StopGeneration stop_generation;
  StopRequestedSelectorSnapshotDTO requested_selector;
  StopResolutionDispositionDTO resolution_disposition{StopResolutionDispositionDTO::kExact};
  StopScopeResolutionReasonDTO resolution_reason{StopScopeResolutionReasonDTO::kExactCurrentScope};
  std::vector<StopScopeStaleDiagnosticDTO> stale_diagnostics;
  std::vector<StopScopeCaptureHistoryDTO> capture_history;
  std::vector<StopCertificateRefDTO> predecessor_certificates;
  std::vector<StopScopeEntryDTO> scope_entries;
  std::vector<StopAttemptedStageResultDTO> attempted_stage_results;
  std::vector<StopCertificateEvidenceItemDTO> evidence_items;
  StopCertificateResultDTO result{StopCertificateResultDTO::kFailedUncontained};
  std::vector<StopScopeEntryKeyDTO> pre_dispatch_cancelled;
  std::vector<StopScopeEntryKeyDTO> dispatched_stopped;
  std::vector<StopScopeEntryKeyDTO> unsettled;
  std::vector<StopScopeEntryKeyDTO> failed;
  std::optional<StopInterruptCorrelationDTO> interrupt_correlation;
  std::optional<StopFaultLatchId> fault_latch_id;
  std::optional<StopFaultLatchGeneration> fault_latch_generation;
  std::int64_t completed_at_ns{};
  std::optional<ErrorDTO> terminal_error;
  /// Scope entries whose hard stop was the intended Stop: requested directly
  /// (a zero-speed hold timeout, or Reset without an operator Stop profile;
  /// Q27, Q31) or chosen by the Stop planner (Q33). Their strongest stage is
  /// then not an escalation. Canonically ordered; normally empty.
  std::vector<StopScopeEntryKeyDTO> lane_hard_stop_requested{};

  friend bool operator==(const StopCertificateDTO&, const StopCertificateDTO&) = default;
};

/// Immutable input to the pure terminal reducer. Predecessor certificates are
/// supplied as complete immutable values so the reducer can merge their
/// origin-tagged scope/attempt/evidence truth without repository or Kernel I/O.
struct StopTerminalReductionInputDTO final {
  StopCertificateId stop_certificate_id;
  StopCertificateRevision stop_certificate_revision;
  StopId stop_id;
  StopGeneration stop_generation;
  StopRequestedSelectorSnapshotDTO requested_selector;
  StopResolutionDispositionDTO resolution_disposition{StopResolutionDispositionDTO::kExact};
  StopScopeResolutionReasonDTO resolution_reason{StopScopeResolutionReasonDTO::kExactCurrentScope};
  std::vector<StopScopeStaleDiagnosticDTO> stale_diagnostics;
  std::vector<StopScopeCaptureHistoryDTO> capture_history;
  std::vector<StopCertificateDTO> predecessor_certificates;
  std::vector<StopScopeEntryDTO> newly_captured_scope_entries;
  std::vector<StopAttemptedStageResultDTO> newly_completed_stage_results;
  std::optional<StopInterruptCorrelationDTO> interrupt_correlation;
  std::optional<StopFaultLatchId> fault_latch_id;
  std::optional<StopFaultLatchGeneration> fault_latch_generation;
  std::int64_t completed_at_ns{};
  std::optional<ErrorDTO> terminal_error;
  /// This Stop's own entries whose hard stop was the intended Stop.
  std::vector<StopScopeEntryKeyDTO> lane_hard_stop_requested{};

  friend bool operator==(const StopTerminalReductionInputDTO&,
                         const StopTerminalReductionInputDTO&) = default;
};

/// Closed local validators. They verify canonical bounds/order/presence and
/// immutable joins only; they perform no repository, clock, scheduler,
/// provider, or observed-state operation.
[[nodiscard]] std::optional<std::string_view> stop_terminal_reduction_input_field_error(
    const StopTerminalReductionInputDTO& input) noexcept;
[[nodiscard]] std::optional<std::string_view> stop_certificate_field_error(
    const StopCertificateDTO& certificate) noexcept;

/// Flattened certificate evidence derived from the attempts alone: one row per
/// attempt evidence item, keyed by the attempt's scope key, its stage's
/// stop_escalation_stage_id and stage index (resolved through
/// result.request.participant_stop_plan.stage_commands), and its result
/// sequence; canonically sorted. The terminal reducer and the lean terminal
/// decoder both call this one function, so stored and reduced evidence cannot
/// diverge. It does not join attempts to scope entries (the reducer and
/// stop_certificate_field_error do). Errors: "stop_terminal_attempt_join_invalid"
/// (stage index outside the plan), "stop_terminal_evidence_capacity_exceeded",
/// "stop_terminal_evidence_conflict".
[[nodiscard]] Result<std::vector<StopCertificateEvidenceItemDTO>> DeriveStopCertificateEvidence(
    const std::vector<StopAttemptedStageResultDTO>& attempts);

/// Deterministically merge complete predecessor certificates and newly
/// captured rows, derive flattened evidence/subsets/result, and return one
/// immutable certificate. No input value is modified.
[[nodiscard]] Result<StopCertificateDTO> ReduceStopTerminal(
    const StopTerminalReductionInputDTO& input);

}  // namespace manyforge::core
