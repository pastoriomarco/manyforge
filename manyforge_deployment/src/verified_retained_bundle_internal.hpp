#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "manyforge_deployment/verified_retained_bundle.hpp"

namespace manyforge::deployment::detail {

struct QualifiedRetainedProductArtifactRole final {
  std::string_view artifact_role_id;
  std::string_view media_type;
  bool required;
};

/// The exact build-owned product contract understood by the current retained
/// envelope codec. The public compiler registry projects this same internal
/// record; the read-only verifier never links that registry.
[[nodiscard]] std::string_view
flattened_model_product_contract_id() noexcept;
[[nodiscard]] const core::ProductContractRevision&
flattened_model_product_contract_revision();
[[nodiscard]] std::span<const QualifiedRetainedProductArtifactRole>
flattened_model_product_artifact_roles() noexcept;
[[nodiscard]] const CanonicalValue&
flattened_model_product_contract_descriptor();

[[nodiscard]] std::string_view viewer_model_product_contract_id() noexcept;
[[nodiscard]] const core::ProductContractRevision&
viewer_model_product_contract_revision();
[[nodiscard]] std::span<const QualifiedRetainedProductArtifactRole>
viewer_model_product_artifact_roles() noexcept;
[[nodiscard]] const CanonicalValue&
viewer_model_product_contract_descriptor();

[[nodiscard]] std::string_view
control_domain_control_product_contract_id() noexcept;
[[nodiscard]] const core::ProductContractRevision &
control_domain_control_product_contract_revision();
[[nodiscard]] std::span<const QualifiedRetainedProductArtifactRole>
control_domain_control_product_artifact_roles() noexcept;
[[nodiscard]] const CanonicalValue &
control_domain_control_product_contract_descriptor();

[[nodiscard]] std::string_view
composed_model_symbol_role_map_product_contract_id() noexcept;
[[nodiscard]] const core::ProductContractRevision&
composed_model_symbol_role_map_product_contract_revision();
[[nodiscard]] std::span<const QualifiedRetainedProductArtifactRole>
composed_model_symbol_role_map_product_artifact_roles() noexcept;
[[nodiscard]] const CanonicalValue&
composed_model_symbol_role_map_product_contract_descriptor();

[[noreturn]] void fail_retained_bundle(std::string_view message);
void require_retained_portable_identity(std::string_view value,
                                        std::string_view field);
void require_retained_digest(std::string_view value, std::string_view field);
[[nodiscard]] std::string
retained_store_key_for_digest(std::string_view digest);

[[nodiscard]] std::optional<std::string_view>
retained_digest_hex(std::string_view digest) noexcept;
[[nodiscard]] bool
is_retained_digest_hex(std::string_view hex) noexcept;
[[nodiscard]] std::string
retained_digest_from_hex(std::string_view hex);
[[nodiscard]] std::optional<std::string_view> retained_raw_key_hex(
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& expected_digest) noexcept;

/// Verifies the exact current retained-product contract without depending on
/// the compiler-facing product-contract registry.
void require_current_retained_product_conformance(
    const core::RetainedProductDTO& product);

[[nodiscard]] CanonicalValue
retained_product_projection(const core::RetainedProductDTO& product);

/// Domain-separated digest used by the product's sole provenance output row.
[[nodiscard]] core::ContentDigest
retained_product_output_digest(const core::RetainedProductDTO& product);

class RetainedBundleConstructionAuthority final {
 public:
  [[nodiscard]] static std::shared_ptr<const RetainedBundle> construct(
      core::RetainedBundleId retained_bundle_id,
      core::RetainedBundleDTO value, CanonicalValue normalized_root,
      std::string canonical_bytes);
};

/// Sole reconstruction authority for durable semantic repository bytes.
///
/// Reload reparses the exact canonical envelope, verifies the complete
/// currently qualified retained-bundle projection and semantic identity, and
/// only then reconstructs the sealed RetainedBundle value.
class RetainedBundleReloadAuthority final {
 public:
  [[nodiscard]] static std::shared_ptr<const RetainedBundle> reload(
      std::string_view canonical_bytes);
};

}  // namespace manyforge::deployment::detail
