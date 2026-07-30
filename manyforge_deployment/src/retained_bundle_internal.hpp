#pragma once

#include <span>
#include <string>

#include "manyforge_deployment/retained_bundle.hpp"

namespace manyforge::deployment::detail {

/// Compiler-internal input for one product-contract artifact role.
///
/// This type and the retention functions below are deliberately absent from
/// the installed API: only the deployment compiler may turn qualified
/// semantics into retained products.
struct RetainedArtifactInput final {
  core::ArtifactRoleId artifact_role_id;
  std::string media_type;
  std::string bytes;
};

/// Validates an exact code-owned contract and retains its raw artifacts.
///
/// Contract shape is checked completely before the first store effect.
[[nodiscard]] core::RetainedProductDTO build_and_retain_product(
    const core::ProductId &product_id, core::RetainedProductKind product_kind,
    ProductPlacement placement,
    const core::ProductContractId &product_contract_id,
    const core::ProductContractRevision &product_contract_revision,
    std::span<const RetainedArtifactInput> artifacts,
    IRawArtifactStore &artifact_store);

[[nodiscard]] CanonicalValue
retained_product_projection(const core::RetainedProductDTO &product);

/// Domain-separated digest used by the product's sole provenance output row.
[[nodiscard]] core::ContentDigest
retained_product_output_digest(const core::RetainedProductDTO &product);

}  // namespace manyforge::deployment::detail
