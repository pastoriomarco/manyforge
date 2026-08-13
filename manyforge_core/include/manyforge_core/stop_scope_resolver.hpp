#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/operation_runtime.hpp>
#include <manyforge_core/types/prepared_participant_stop_plan.hpp>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumStopScopeSnapshotOperations = 4096U;
inline constexpr std::size_t kMaximumStopScopeSnapshotComponents = 4096U;
inline constexpr std::size_t kMaximumStopScopeSnapshotResources = 4096U;
inline constexpr std::size_t kMaximumStopScopeMembershipRows = 65536U;

/// One immutable Kernel-owned operation row used only for bounded Stop-scope
/// resolution. Any operation that can still have an effect must carry its
/// complete current component/resource closure.
struct StopScopeOperationSnapshotDTO final {
  OperationId operation_id;
  OperationRevision operation_revision;
  StopGeneration stop_generation;
  OperationStateKind state{OperationStateKind::kResolving};
  bool scope_complete{};
  bool effect_uncertain{};
  std::vector<ControllableComponentId> controllable_components;
  std::vector<ResourceId> resources;

  friend bool operator==(const StopScopeOperationSnapshotDTO&,
                         const StopScopeOperationSnapshotDTO&) = default;
};

/// Current commandable-resource membership of one semantic component.
struct StopScopeComponentSnapshotDTO final {
  ControllableComponentId controllable_component_id;
  ComponentLifecycleGeneration component_lifecycle_generation;
  std::vector<ResourceId> resources;

  friend bool operator==(const StopScopeComponentSnapshotDTO&,
                         const StopScopeComponentSnapshotDTO&) = default;
};

/// Current resource row. `effect_uncertain` covers retained external/orphan
/// work that must remain containable even when no live operation row owns it.
struct StopScopeResourceSnapshotDTO final {
  ResourceId resource_id;
  StopGeneration stop_generation;
  bool effect_uncertain{};

  friend bool operator==(const StopScopeResourceSnapshotDTO&,
                         const StopScopeResourceSnapshotDTO&) = default;
};

/// One same-host immutable capture. Multi-host fan-out can later aggregate
/// equivalent owner captures without changing public semantic selectors.
struct StopScopeResolverSnapshotDTO final {
  RequestOwnerEpoch request_owner_epoch;
  GraphRevision graph_revision;
  std::uint64_t operation_resource_generation{};
  StopGeneration cell_stop_generation;
  std::vector<StopScopeOperationSnapshotDTO> operations;
  std::vector<StopScopeComponentSnapshotDTO> components;
  std::vector<StopScopeResourceSnapshotDTO> resources;

  friend bool operator==(const StopScopeResolverSnapshotDTO&,
                         const StopScopeResolverSnapshotDTO&) = default;
};

struct StopScopeResolutionDTO final {
  StopResolutionDispositionDTO disposition{StopResolutionDispositionDTO::kExact};
  StopScopeResolutionReasonDTO reason{StopScopeResolutionReasonDTO::kExactCurrentScope};
  bool full_stop{};
  std::vector<OperationId> operations;
  std::vector<ControllableComponentId> controllable_components;
  std::vector<ResourceId> resources;
  std::vector<StopScopeStaleDiagnosticDTO> stale_diagnostics;

  friend bool operator==(const StopScopeResolutionDTO&, const StopScopeResolutionDTO&) = default;
};

/// One exact predecessor supplied to aggregate acceptance construction. The
/// acceptance is immutable owner truth; `remaining_outer_duration_ns_at_join`
/// is freshly sampled under the same Kernel ordering fence as the aggregate.
struct StopAcceptedPredecessorInput final {
  StopAcceptedDTO acceptance;
  std::int64_t remaining_outer_duration_ns_at_join{};
};

/// Validates the immutable resolution snapshot, including complete scope for
/// every nonterminal or effect-uncertain operation and unique component
/// ownership of every commandable resource.
[[nodiscard]] std::optional<std::string_view> stop_scope_snapshot_field_error(
    const StopScopeResolverSnapshotDTO& snapshot);

/// Pure bounded ordinary-Stop resolution. Malformed normalized ordinary
/// intent is conservatively promoted; an invalid owner snapshot is an
/// invariant error and cannot be used to issue any effect.
[[nodiscard]] Result<StopScopeResolutionDTO> resolve_stop_scope(
    const StopIntentDTO& intent, const StopScopeResolverSnapshotDTO& snapshot);

/// Pure bounded Full-Stop capture over all current nonterminal/effect-
/// uncertain work in the same immutable snapshot.
[[nodiscard]] Result<StopScopeResolutionDTO> resolve_full_stop_scope(
    const FullStopIntentDTO& intent, const StopScopeResolverSnapshotDTO& snapshot);

/// Canonical content identities used by the emergency acceptance reserve and
/// immutable StopAcceptedDTO. The selector digest covers only the requested
/// semantic selector/expectations, never request attribution or diagnostics.
[[nodiscard]] Result<ContentDigest> ComputeRequestedStopSelectorDigest(const StopIntentDTO& intent);
[[nodiscard]] Result<ContentDigest> ComputeRequestedStopSelectorDigest(
    const FullStopIntentDTO& intent);
[[nodiscard]] Result<ContentDigest> ComputeRequestedStopSelectorDigest(
    const MalformedStopIngressDTO& ingress);

/// Canonical identity of the complete normalized caller-owned Stop intent.
/// Receiving-adapter attribution is compared separately by the emergency
/// reserve and is deliberately not caller-controlled digest input.
[[nodiscard]] Result<ContentDigest> ComputeNormalizedStopIntentDigest(const StopIntentDTO& intent);
[[nodiscard]] Result<ContentDigest> ComputeNormalizedStopIntentDigest(
    const FullStopIntentDTO& intent);

/// Canonical digest of the exact resolved operation/component/resource capture
/// and its scoped-vs-Full classification. Diagnostics and disposition do not
/// alter the accepted physical scope identity.
[[nodiscard]] Result<ContentDigest> ComputeCapturedStopScopeDigest(
    const StopScopeResolutionDTO& resolution);

/// Construct one immutable Stop acceptance from an already-resolved scope and
/// the exact participant plans retained before dispatch. The constructor
/// derives both content identities, requires one common outer/reserve budget,
/// and publishes participant-qualified phase rows without flattening equal
/// phase spelling across participants.
[[nodiscard]] Result<StopAcceptedDTO> MakeStopAccepted(
    const StopIntentDTO& intent, StopId stop_id, StopGeneration stop_generation,
    StopScopeResolutionDTO resolution,
    std::vector<StopAffectedOperationRevisionDTO> affected_operation_revisions,
    const StopDeadlineBudgetDTO& fixed_budget, std::int64_t remaining_outer_duration_ns,
    std::int64_t accepted_at_ns, std::span<const PreparedParticipantStopPlanDTO> participant_plans);

[[nodiscard]] Result<StopAcceptedDTO> MakeStopAccepted(
    const FullStopIntentDTO& intent, StopId stop_id, StopGeneration stop_generation,
    StopScopeResolutionDTO resolution,
    std::vector<StopAffectedOperationRevisionDTO> affected_operation_revisions,
    const StopDeadlineBudgetDTO& fixed_budget, std::int64_t remaining_outer_duration_ns,
    std::int64_t accepted_at_ns, std::span<const PreparedParticipantStopPlanDTO> participant_plans);

/// Construct one promoted aggregate Full Stop. Predecessor revisions and
/// participant rows are inherited exactly and shortened to the aggregate's
/// earliest deadline; only `newly_affected_operation_revisions` and
/// `new_participant_plans` describe freshly fenced scope.
[[nodiscard]] Result<StopAcceptedDTO> MakeAggregatedStopAccepted(
    const StopIntentDTO& intent, StopId stop_id, StopGeneration stop_generation,
    StopScopeResolutionDTO resolution,
    std::vector<StopAffectedOperationRevisionDTO> newly_affected_operation_revisions,
    const StopDeadlineBudgetDTO& fixed_budget, std::int64_t proposed_remaining_outer_duration_ns,
    std::int64_t accepted_at_ns,
    std::span<const PreparedParticipantStopPlanDTO> new_participant_plans,
    std::span<const StopAcceptedPredecessorInput> predecessors);

[[nodiscard]] Result<StopAcceptedDTO> MakeAggregatedStopAccepted(
    const FullStopIntentDTO& intent, StopId stop_id, StopGeneration stop_generation,
    StopScopeResolutionDTO resolution,
    std::vector<StopAffectedOperationRevisionDTO> newly_affected_operation_revisions,
    const StopDeadlineBudgetDTO& fixed_budget, std::int64_t proposed_remaining_outer_duration_ns,
    std::int64_t accepted_at_ns,
    std::span<const PreparedParticipantStopPlanDTO> new_participant_plans,
    std::span<const StopAcceptedPredecessorInput> predecessors);

}  // namespace manyforge::core
