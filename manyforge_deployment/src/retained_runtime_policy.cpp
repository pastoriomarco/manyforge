#include "stop_timing_resolution_internal.hpp"
#include <algorithm>
#include <manyforge_deployment/retained_runtime_policy.hpp>
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
    throw RetainedRuntimePolicyError(
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
      throw RetainedRuntimePolicyError(
          "retained Stop policy artifact is unavailable");
    auto policy = resolve_and_qualify_stop_evidence_policy(
        *reference, reference->content_addressed_store_key, *bytes);
    if (policy.reference() != *reference)
      throw RetainedRuntimePolicyError("retained Stop policy readback differs");
    required.erase({reference->record_id, reference->record_revision});
    policies.push_back(std::move(policy));
  }
  if (!required.empty())
    throw RetainedRuntimePolicyError(
        "retained Stop policy is absent from the catalog");
  const auto budgets = detail::resolve_binding_stop_timing(
      bundle.value().cell_graph.control_bindings, policies,
      bundle.value().stop_timing_parameters);
  if (!budgets.ok())
    throw RetainedRuntimePolicyError(budgets.error().message);
  if (budgets.value().empty())
    return std::nullopt;
  auto envelope = budgets.value().begin()->second;
  envelope.phases.clear();
  return envelope;
}

std::optional<QualifiedRuntimePolicy>
resolve_retained_runtime_policy(const RetainedBundle &bundle,
                                const ICatalogArtifactSource &artifact_store) {
  if (!verify_retained_bundle_identity(bundle)) {
    throw RetainedRuntimePolicyError("runtime policy selection requires an "
                                     "identity-verified retained bundle");
  }

  const core::RuntimePolicyCatalogRecordRefDTO *selected = nullptr;
  for (const auto &retained : bundle.value().retained_policy_records) {
    const auto *runtime_policy =
        std::get_if<core::RuntimePolicyCatalogRecordRefDTO>(&retained);
    if (runtime_policy == nullptr) {
      continue;
    }
    if (selected != nullptr) {
      throw RetainedRuntimePolicyError(
          "retained cell selects more than one v1 runtime policy");
    }
    selected = runtime_policy;
  }
  if (selected == nullptr) {
    return std::nullopt;
  }

  const auto &records = bundle.value().catalogs.records;
  const auto record = std::find(records.begin(), records.end(),
                                core::CatalogRecordRefDTO(*selected));
  if (record == records.end()) {
    throw RetainedRuntimePolicyError(
        "retained runtime policy is absent from the exact catalog closure");
  }
  std::optional<std::string> bytes;
  try {
    bytes = artifact_store.get_verified(selected->content_addressed_store_key,
                                        selected->content_digest);
  } catch (const std::exception &error) {
    throw RetainedRuntimePolicyError(
        std::string("retained runtime policy readback failed: ") +
        error.what());
  } catch (...) {
    throw RetainedRuntimePolicyError("retained runtime policy readback failed");
  }
  if (!bytes.has_value()) {
    throw RetainedRuntimePolicyError(
        "retained runtime policy artifact is absent or fails exact readback");
  }
  try {
    auto qualified = resolve_and_qualify_runtime_policy(
        *selected, selected->content_addressed_store_key, *bytes);
    if (qualified.reference() != *selected) {
      throw RetainedRuntimePolicyError(
          "qualified runtime policy differs from the retained reference");
    }
    return qualified;
  } catch (const RetainedRuntimePolicyError &) {
    throw;
  } catch (const std::exception &error) {
    throw RetainedRuntimePolicyError(
        std::string("retained runtime policy qualification failed: ") +
        error.what());
  } catch (...) {
    throw RetainedRuntimePolicyError(
        "retained runtime policy qualification failed");
  }
}

} // namespace manyforge::deployment
