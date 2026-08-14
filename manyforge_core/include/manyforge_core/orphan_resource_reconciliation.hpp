#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/stop_fault_recovery.hpp>
#include <optional>
#include <string_view>
#include <vector>

namespace manyforge::core {

inline constexpr std::size_t kMaximumOrphanResourceReconciliationFacts = 4096U;

/// One command-free, owner-issued proof that a formerly orphaned resource has
/// no live goal, is freshly quiescent, and still belongs to the exact retained
/// route/configuration against which the proof was sampled.
struct OrphanResourceReconciliationFactDTO final {
  ResourceId resource_id;
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  ControlDomainId control_domain_id;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation{0U};
  RuntimeUnitId provider_runtime_unit_id;
  RuntimeEpoch provider_runtime_epoch;
  StopFaultRecoveryResourceFactDTO settlement;
  ContentDigest no_active_goal_evidence_digest;
  ContentDigest configuration_evidence_digest;

  friend bool operator==(const OrphanResourceReconciliationFactDTO&,
                         const OrphanResourceReconciliationFactDTO&) = default;
};

/// Private reconciliation snapshot. The runtime owner builds it from current
/// goal/activity, route, and addressed-state sources; no public request may
/// supply or amend these facts.
struct OrphanResourceReconciliationFactsDTO final {
  RequestOwnerEpoch request_owner_epoch;
  CellId cell_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  std::uint64_t expected_operation_resource_generation{};
  std::int64_t assessed_at_ns{};
  std::vector<OrphanResourceReconciliationFactDTO> resources;

  friend bool operator==(const OrphanResourceReconciliationFactsDTO&,
                         const OrphanResourceReconciliationFactsDTO&) = default;
};

struct OrphanResourceReconciliationResultDTO final {
  std::vector<ResourceId> reconciled_resources;
  std::vector<ResourceId> remaining_unreconciled_resources;
  std::uint64_t operation_resource_generation{};

  friend bool operator==(const OrphanResourceReconciliationResultDTO&,
                         const OrphanResourceReconciliationResultDTO&) = default;
};

[[nodiscard]] std::optional<std::string_view> orphan_resource_reconciliation_facts_field_error(
    const OrphanResourceReconciliationFactsDTO& facts) noexcept;

[[nodiscard]] std::optional<std::string_view> orphan_resource_reconciliation_result_field_error(
    const OrphanResourceReconciliationResultDTO& result) noexcept;

}  // namespace manyforge::core
