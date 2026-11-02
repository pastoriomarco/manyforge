#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <stdexcept>
#include <string>
#include <string_view>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment::detail {

class ExactCatalogReferentError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Performs only the byte/reference/schema preflight shared by exact catalog
/// families. This private helper deliberately returns an unqualified
/// normalized candidate; only a family-specific decoder may create a public
/// qualified type after identity and semantic checks.
template <typename TypedCatalogReference>
[[nodiscard]] UnqualifiedSchemaNormalizedArtifactCandidate
resolve_and_normalize_exact_catalog_referent(
    const TypedCatalogReference& reference,
    const core::ContentAddressedStoreKey& resolved_store_key, std::string_view stored_bytes,
    std::string_view expected_api_version, std::string_view diagnostic_label) {
  const std::string label(diagnostic_label);
  if (stored_bytes.size() > kMaxArtifactEncodedBytes) {
    throw ExactCatalogReferentError(label + " stored bytes exceed the fixed artifact limit");
  }
  if (reference.content_addressed_store_key.empty() ||
      reference.content_addressed_store_key.value().size() > kMaxArtifactScalarBytes ||
      resolved_store_key.empty() || resolved_store_key.value().size() > kMaxArtifactScalarBytes) {
    throw ExactCatalogReferentError(label + " content-addressed store key is outside v1 bounds");
  }

  const auto& registry = build_qualified_closed_schema_registry();
  const auto* family = registry.find_qualified_catalog_schema(
      TypedCatalogReference::kRecordKind, reference.schema_id, reference.schema_revision);
  if (family == nullptr) {
    throw ExactCatalogReferentError(label + " catalog reference selects an unqualified schema");
  }
  if (reference.schema_id.value() != expected_api_version) {
    throw ExactCatalogReferentError(label + " catalog reference selects the wrong schema family");
  }
  if (reference.content_addressed_store_key != resolved_store_key) {
    throw ExactCatalogReferentError(label + " bytes were resolved under a different store key");
  }
  if (reference.content_digest.value() != sha256_digest(stored_bytes)) {
    throw ExactCatalogReferentError(label + " exact stored-byte digest does not match");
  }

  return validate_and_normalize_unqualified_artifact_candidate(
      *family->compiled_schema(), parse_strict_json_model_yaml(stored_bytes));
}

}  // namespace manyforge::deployment::detail
