#include "stop_timing_resolution_internal.hpp"
#include <manyforge_deployment/retained_stop_deadline_budget.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <set>
#include <string>
#include <variant>

namespace manyforge::deployment {

std::optional<core::StopDeadlineBudgetDTO>
resolve_retained_stop_deadline_budget(
    const RetainedBundle &bundle,
    const ICatalogArtifactSource &artifact_store) {
  if (!verify_retained_bundle_identity(bundle)) {
    throw RetainedStopDeadlineBudgetError(
        "Stop envelope requires an identity-verified retained bundle");
  }
  using PolicyIdentity =
      std::pair<core::StopEvidencePolicyId, core::StopEvidencePolicyRevision>;
  std::set<PolicyIdentity> required;
  for (const auto &binding : bundle.value().cell_graph.control_bindings) {
    required.emplace(binding.stop_evidence_policy_id,
                     binding.stop_evidence_policy_revision);
  }
  std::vector<QualifiedStopEvidencePolicy> policies;
  for (const auto &record : bundle.value().catalogs.records) {
    const auto *reference =
        std::get_if<core::StopEvidencePolicyCatalogRecordRefDTO>(&record);
    if (reference == nullptr ||
        !required.contains({reference->record_id, reference->record_revision}))
      continue;
    const auto bytes = artifact_store.get_verified(
        reference->content_addressed_store_key, reference->content_digest);
    if (!bytes.has_value())
      throw RetainedStopDeadlineBudgetError(
          "retained Stop policy artifact is unavailable");
    auto policy = resolve_and_qualify_stop_evidence_policy(
        *reference, reference->content_addressed_store_key, *bytes);
    if (policy.reference() != *reference)
      throw RetainedStopDeadlineBudgetError(
          "retained Stop policy readback differs");
    required.erase({reference->record_id, reference->record_revision});
    policies.push_back(std::move(policy));
  }
  if (!required.empty())
    throw RetainedStopDeadlineBudgetError(
        "retained Stop policy is absent from the catalog");
  const auto budgets = detail::resolve_binding_stop_timing(
      bundle.value().cell_graph.control_bindings, policies,
      bundle.value().stop_timing_parameters);
  if (!budgets.ok())
    throw RetainedStopDeadlineBudgetError(budgets.error().message);
  if (budgets.value().empty())
    return std::nullopt;
  auto envelope = budgets.value().begin()->second;
  envelope.phases.clear();
  return envelope;
}

} // namespace manyforge::deployment
