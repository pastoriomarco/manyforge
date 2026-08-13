#include <algorithm>
#include <manyforge_deployment/retained_runtime_policy.hpp>
#include <string>
#include <variant>

namespace manyforge::deployment {

std::optional<QualifiedRuntimePolicy> resolve_retained_runtime_policy(
    const RetainedBundle& bundle, const ICatalogArtifactStore& artifact_store) {
  if (!verify_retained_bundle_identity(bundle)) {
    throw RetainedRuntimePolicyError(
        "runtime policy selection requires an "
        "identity-verified retained bundle");
  }

  const core::RuntimePolicyCatalogRecordRefDTO* selected = nullptr;
  for (const auto& retained : bundle.value().retained_policy_records) {
    const auto* runtime_policy = std::get_if<core::RuntimePolicyCatalogRecordRefDTO>(&retained);
    if (runtime_policy == nullptr) {
      continue;
    }
    if (selected != nullptr) {
      throw RetainedRuntimePolicyError("retained cell selects more than one v1 runtime policy");
    }
    selected = runtime_policy;
  }
  if (selected == nullptr) {
    return std::nullopt;
  }

  const auto& records = bundle.value().catalogs.records;
  const auto record =
      std::find(records.begin(), records.end(), core::CatalogRecordRefDTO(*selected));
  if (record == records.end()) {
    throw RetainedRuntimePolicyError(
        "retained runtime policy is absent from the exact catalog closure");
  }
  std::optional<std::string> bytes;
  try {
    bytes = artifact_store.get_verified(selected->content_addressed_store_key,
                                        selected->content_digest);
  } catch (const std::exception& error) {
    throw RetainedRuntimePolicyError(
        std::string("retained runtime policy readback failed: ") +
        error.what());
  } catch (...) {
    throw RetainedRuntimePolicyError(
        "retained runtime policy readback failed");
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
  } catch (const RetainedRuntimePolicyError&) {
    throw;
  } catch (const std::exception& error) {
    throw RetainedRuntimePolicyError(std::string("retained runtime policy qualification failed: ") +
                                     error.what());
  } catch (...) {
    throw RetainedRuntimePolicyError(
        "retained runtime policy qualification failed");
  }
}

}  // namespace manyforge::deployment
