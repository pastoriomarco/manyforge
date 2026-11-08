#pragma once

#include <cstdint>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/json_value.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace manyforge::core {

class RuntimeParticipantBinding;

inline constexpr std::string_view kResolvedParticipantStopScopePolicyApiVersion =
    "manyforge.resolved_participant_stop_scope_policy.v1";
inline constexpr std::string_view kResolvedParticipantStopScopePolicyIdentityDomain =
    "resolved-participant-stop-scope-policy-digest";
inline constexpr std::size_t kMaximumStopEvidenceValueBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumStopEvidenceValueDepth = 16U;
inline constexpr std::size_t kMaximumStopEvidenceValueNodes = 4096U;

enum class StopStageAttemptDispositionDTO : std::uint8_t {
  kNotDispatched = 0,
  kDispatched,
  kEffectUncertain,
};

enum class ParticipantStopStageOutcomeDTO : std::uint8_t {
  kSucceeded = 0,
  kPartial,
  kFailed,
  kSupersededNoEffect,
};

enum class StopEvidenceItemStateDTO : std::uint8_t {
  kRequiredPending = 0,
  kSatisfied,
  kMissing,
  kFailed,
  kInapplicable,
};

/// The per-robot Stop-time values handed with one Stop stage. There
/// is no shared cell-wide budget: each robot is checked against its own
/// deadlines, computed by the Kernel at Stop time from its own profile.
/// Durations are relative to the stage dispatch, so receivers translate them
/// onto their own monotonic clocks and never see the Kernel's timestamps.
struct RemainingStopBudgetDTO final {
  StopId stop_id;
  StopGeneration stop_generation;
  /// Time since the Stop was accepted (deadlines run from the Stop
  /// request).
  std::int64_t elapsed_since_stop_ns{};
  /// Controlled stages only: the remaining time of this robot's own
  /// controlled-Stop deadline (`max_stop_time_s`, else deceleration time plus
  /// `complete_move_on_signal_timeout`). Absent for hard stages (the
  /// participant bounds them with its lane's stop-completion timeout) and for
  /// fire-and-forget gripper cancels.
  std::optional<std::int64_t> remaining_controlled_deadline_ns;
  /// Return-target timing only: age of original Kernel Stop ingress, captured
  /// after the executor queue. Relative duration, never a cross-clock stamp.
  /// Absent on legacy/direct callers; does not change any deadline budget.
  /// Transient worker/receiver copy only, not the Kernel's retained budget.
  /// Target provenance is emitted in provider diagnostics, not Stop journal timing.
  std::optional<std::int64_t> stop_ingress_age_ns{};

  friend bool operator==(const RemainingStopBudgetDTO&, const RemainingStopBudgetDTO&) = default;
};

/// Complete immutable stage command selected by the Kernel from one already
/// prepared participant Stop plan. The adapter receives no alternative stage,
/// endpoint, mode, or profile from which it could make a runtime choice.
struct ResolvedParticipantStopStageDTO final {
  StopId stop_id;
  StopGeneration stop_generation;
  OperationId operation_id;
  OperationRevision operation_revision;
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
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  std::optional<CapabilityProviderBindingId> capability_provider_binding_id;
  std::optional<CapabilityProviderBindingRevision> capability_provider_binding_revision;
  RuntimeEpoch provider_runtime_epoch;
  std::vector<ResourceId> resources;
  ContainmentGranularity containment_granularity{ContainmentGranularity::kResource};
  StopEvidencePolicyId stop_evidence_policy_id;
  StopEvidencePolicyRevision stop_evidence_policy_revision;
  PreparedParticipantStopPlanDTO participant_stop_plan;
  std::size_t stage_index{};
  ContentDigest resolved_stop_scope_policy_digest;

  friend bool operator==(const ResolvedParticipantStopStageDTO&,
                         const ResolvedParticipantStopStageDTO&) = default;
};

struct StopEvidenceItemDTO final {
  StopEvidenceItemId evidence_item_id;
  StopEvidenceRequirementKind required_class{StopEvidenceRequirementKind::kMotionRest};
  StopEvidenceItemStateDTO state{StopEvidenceItemStateDTO::kRequiredPending};
  ResolvedStopEvidenceSourceDTO resolved_source;
  std::optional<RuntimeEpoch> source_epoch;
  std::optional<std::uint64_t> source_sequence;
  std::optional<std::int64_t> source_age_at_evaluation_ns;
  std::optional<JsonValueDTO> evidence;
  std::optional<ErrorDTO> error;

  friend bool operator==(const StopEvidenceItemDTO&, const StopEvidenceItemDTO&) = default;
};

struct ParticipantStopStageResultDTO final {
  ResolvedParticipantStopStageDTO request;
  StopStageAttemptDispositionDTO attempt_disposition{
      StopStageAttemptDispositionDTO::kNotDispatched};
  ParticipantStopStageOutcomeDTO outcome{ParticipantStopStageOutcomeDTO::kFailed};
  std::optional<MotionControlledStopMode> actual_provider_contract_mode;
  /// Required exactly when an allowed actual mode differs from the prepared
  /// requested mode; forbidden otherwise.
  std::optional<std::string> recoverable_substitution_reason;
  std::optional<std::uint64_t> controller_acknowledgement_sequence;
  std::optional<RuntimeEpoch> source_epoch;
  std::optional<std::uint64_t> source_sequence;
  std::int64_t receiver_elapsed_duration_ns{};
  std::vector<StopEvidenceItemDTO> evidence_items;
  std::optional<ErrorDTO> error;

  friend bool operator==(const ParticipantStopStageResultDTO&,
                         const ParticipantStopStageResultDTO&) = default;
};

/// Derive the immutable scope/policy digest. This operation validates only the
/// digest projection's closed shape; it does not replace referent validation.
[[nodiscard]] Result<ContentDigest> ComputeResolvedParticipantStopScopePolicyDigest(
    const ResolvedParticipantStopStageDTO& stage);

/// Mint and validate a stage against all immutable Prepare referents. The
/// expanded resource scope is Kernel-owned current scope, not adapter input.
[[nodiscard]] Result<ResolvedParticipantStopStageDTO> MakeResolvedParticipantStopStage(
    ResolvedParticipantStopStageDTO stage, const RemainingStopBudgetDTO& budget,
    const ResolvedStopBasePlanDTO& verified_base_plan,
    const RuntimeParticipantBinding& verified_participant,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category,
    std::span<const ResourceId> expanded_scope_resources);

[[nodiscard]] Result<void> ValidateResolvedParticipantStopStage(
    const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
    const ResolvedStopBasePlanDTO& verified_base_plan,
    const RuntimeParticipantBinding& verified_participant,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category,
    std::span<const ResourceId> expanded_scope_resources);
[[nodiscard]] Result<void> ValidateParticipantStopStageResult(
    const ParticipantStopStageResultDTO& result, const ResolvedStopBasePlanDTO& verified_base_plan,
    const RuntimeParticipantBinding& verified_participant,
    std::span<const HardStopResourceLimitDTO> retained_default_limits,
    MotionInterruptNodeCategory node_category,
    std::span<const ResourceId> expanded_scope_resources);

}  // namespace manyforge::core
