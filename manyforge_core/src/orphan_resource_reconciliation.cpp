#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/orphan_resource_reconciliation.hpp>

namespace manyforge::core {
namespace {

template <typename Identity>
[[nodiscard]] bool identity(const Identity& value) noexcept {
  return !value.empty() && value.value().size() <= kMaximumStopIdentityBytes;
}

[[nodiscard]] bool digest(const ContentDigest& value) noexcept {
  return content_identity::is_sha256_digest(value.value());
}

template <typename Value, typename Less>
[[nodiscard]] bool canonical(const std::vector<Value>& values, Less less) noexcept {
  return std::is_sorted(values.begin(), values.end(), less) &&
         std::adjacent_find(values.begin(), values.end(), [&](const auto& left, const auto& right) {
           return !less(left, right) && !less(right, left);
         }) == values.end();
}

[[nodiscard]] bool settlement_valid(const StopFaultRecoveryResourceFactDTO& fact,
                                    const ResourceId& resource) noexcept {
  return fact.resource_id == resource &&
         fact.state == StopFaultRecoveryResourceStateDTO::kSettled &&
         fact.source_epoch.has_value() && identity(*fact.source_epoch) &&
         fact.source_sequence.has_value() && *fact.source_sequence > 0U &&
         fact.source_age_ns.has_value() && *fact.source_age_ns >= 0 &&
         fact.freshness_budget_ns.has_value() && *fact.freshness_budget_ns > 0 &&
         *fact.source_age_ns <= *fact.freshness_budget_ns && fact.evidence_digest.has_value() &&
         digest(*fact.evidence_digest) && !fact.error.has_value();
}

}  // namespace

std::optional<std::string_view> orphan_resource_reconciliation_facts_field_error(
    const OrphanResourceReconciliationFactsDTO& facts) noexcept {
  if (!identity(facts.request_owner_epoch) || !identity(facts.cell_id) ||
      !identity(facts.retained_bundle_id) || !identity(facts.graph_revision) ||
      facts.assessed_at_ns < 0 || facts.resources.empty() ||
      facts.resources.size() > kMaximumOrphanResourceReconciliationFacts ||
      !canonical(facts.resources, [](const auto& left, const auto& right) {
        return left.resource_id < right.resource_id;
      })) {
    return "orphan_resource_reconciliation_facts_invalid";
  }
  for (const auto& fact : facts.resources) {
    if (!identity(fact.resource_id) || !identity(fact.controllable_component_id) ||
        !identity(fact.command_target_id) || !identity(fact.control_domain_id) ||
        !identity(fact.control_binding_id) || !identity(fact.control_binding_revision) ||
        !identity(fact.active_control_configuration_id) ||
        !fact.active_control_configuration_generation.nonzero() ||
        !identity(fact.provider_runtime_unit_id) || !identity(fact.provider_runtime_epoch) ||
        !settlement_valid(fact.settlement, fact.resource_id) ||
        !digest(fact.no_active_goal_evidence_digest) ||
        !digest(fact.configuration_evidence_digest)) {
      return "orphan_resource_reconciliation_fact_invalid";
    }
  }
  return std::nullopt;
}

std::optional<std::string_view> orphan_resource_reconciliation_result_field_error(
    const OrphanResourceReconciliationResultDTO& result) noexcept {
  const auto canonical_ids = [](const std::vector<ResourceId>& values) {
    return canonical(values, [](const auto& left, const auto& right) { return left < right; }) &&
           std::all_of(values.begin(), values.end(), identity<ResourceId>);
  };
  if (result.reconciled_resources.empty() ||
      result.reconciled_resources.size() > kMaximumOrphanResourceReconciliationFacts ||
      result.remaining_unreconciled_resources.size() > kMaximumOrphanResourceReconciliationFacts ||
      result.operation_resource_generation == 0U || !canonical_ids(result.reconciled_resources) ||
      !canonical_ids(result.remaining_unreconciled_resources) ||
      std::any_of(result.reconciled_resources.begin(), result.reconciled_resources.end(),
                  [&](const auto& resource) {
                    return std::binary_search(result.remaining_unreconciled_resources.begin(),
                                              result.remaining_unreconciled_resources.end(),
                                              resource);
                  })) {
    return "orphan_resource_reconciliation_result_invalid";
  }
  return std::nullopt;
}

}  // namespace manyforge::core
