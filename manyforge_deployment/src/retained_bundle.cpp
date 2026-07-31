#include "manyforge_deployment/retained_bundle.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <utility>

#include "compiler_semantic_limits_internal.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "retained_bundle_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

[[nodiscard]] std::vector<ProductContractArtifactRole>
flattened_model_artifact_roles() {
  std::vector<ProductContractArtifactRole> roles;
  const auto qualified = detail::flattened_model_product_artifact_roles();
  roles.reserve(qualified.size());
  for (const auto& role : qualified) {
    roles.push_back(ProductContractArtifactRole{
        core::ArtifactRoleId(std::string(role.artifact_role_id)),
        std::string(role.media_type),
        role.required ? ProductArtifactCardinality::kRequiredExactlyOne
                      : ProductArtifactCardinality::kOptionalAtMostOne,
    });
  }
  return roles;
}

[[nodiscard]] std::vector<ProductContractArtifactRole>
composed_model_symbol_role_map_artifact_roles() {
  std::vector<ProductContractArtifactRole> roles;
  const auto qualified =
      detail::composed_model_symbol_role_map_product_artifact_roles();
  roles.reserve(qualified.size());
  for (const auto& role : qualified) {
    roles.push_back(ProductContractArtifactRole{
        core::ArtifactRoleId(std::string(role.artifact_role_id)),
        std::string(role.media_type),
        role.required ? ProductArtifactCardinality::kRequiredExactlyOne
                      : ProductArtifactCardinality::kOptionalAtMostOne,
    });
  }
  return roles;
}

}  // namespace

BuildQualifiedProductContract::BuildQualifiedProductContract(
    core::ProductContractId product_contract_id,
    core::ProductContractRevision product_contract_revision,
    core::RetainedProductKind product_kind, ProductPlacement placement,
    std::vector<ProductContractArtifactRole> artifact_roles,
    CanonicalValue normalized_descriptor)
    : product_contract_id_(std::move(product_contract_id)),
      product_contract_revision_(std::move(product_contract_revision)),
      product_kind_(product_kind),
      placement_(placement),
      artifact_roles_(std::move(artifact_roles)),
      normalized_descriptor_(std::move(normalized_descriptor)) {}

BuildQualifiedProductContractRegistry::
    BuildQualifiedProductContractRegistry() {
  contracts_.push_back(BuildQualifiedProductContract(
      core::ProductContractId(
          std::string(detail::flattened_model_product_contract_id())),
      detail::flattened_model_product_contract_revision(),
      core::RetainedProductKind::kPlanningCollisionModel,
      ProductPlacement::kCellPlanningCollisionModel,
      flattened_model_artifact_roles(),
      detail::flattened_model_product_contract_descriptor()));
  contracts_.push_back(BuildQualifiedProductContract(
      core::ProductContractId(std::string(
          detail::composed_model_symbol_role_map_product_contract_id())),
      detail::composed_model_symbol_role_map_product_contract_revision(),
      core::RetainedProductKind::kSymbolRoleMap,
      ProductPlacement::kSymbolRoleMap,
      composed_model_symbol_role_map_artifact_roles(),
      detail::composed_model_symbol_role_map_product_contract_descriptor()));
}

const BuildQualifiedProductContract*
BuildQualifiedProductContractRegistry::find(
    const core::ProductContractId& id,
    const core::ProductContractRevision& revision) const noexcept {
  const auto match = std::find_if(
      contracts_.begin(), contracts_.end(),
      [&id, &revision](const BuildQualifiedProductContract& contract) {
        return contract.product_contract_id() == id &&
               contract.product_contract_revision() == revision;
      });
  return match == contracts_.end() ? nullptr : &*match;
}

const BuildQualifiedProductContractRegistry&
build_qualified_product_contract_registry() {
  static const BuildQualifiedProductContractRegistry registry;
  return registry;
}

namespace detail {

void preflight_retained_product(
    const core::ProductId& product_id,
    core::RetainedProductKind product_kind, ProductPlacement placement,
    const core::ProductContractId& product_contract_id,
    const core::ProductContractRevision& product_contract_revision,
    std::span<const RetainedArtifactInput> artifacts) {
  require_retained_portable_identity(product_id.value(), "product_id");
  const auto* contract = build_qualified_product_contract_registry().find(
      product_contract_id, product_contract_revision);
  if (contract == nullptr) {
    fail_retained_bundle(
        "product selects an unknown or unqualified contract revision");
  }
  if (contract->product_kind() != product_kind ||
      contract->placement() != placement) {
    fail_retained_bundle(
        "product kind or placement does not match its qualified contract");
  }
  if (artifacts.size() > contract->artifact_roles().size()) {
    fail_retained_bundle(
        "product contains more artifacts than its contract admits");
  }

  std::map<std::string, const RetainedArtifactInput*> supplied;
  for (const auto& artifact : artifacts) {
    require_retained_portable_identity(artifact.artifact_role_id.value(),
                                       "artifact_role_id");
    if (!supplied.emplace(artifact.artifact_role_id.value(), &artifact)
             .second) {
      fail_retained_bundle("product contains a duplicate artifact role");
    }
  }

  for (const auto& [role, unused] : supplied) {
    (void)unused;
    const auto expected = std::find_if(
        contract->artifact_roles().begin(), contract->artifact_roles().end(),
        [&role](const ProductContractArtifactRole& candidate) {
          return candidate.artifact_role_id.value() == role;
        });
    if (expected == contract->artifact_roles().end()) {
      fail_retained_bundle(
          "product contains an artifact role not admitted by its contract");
    }
  }
  for (const auto& expected : contract->artifact_roles()) {
    const auto found = supplied.find(expected.artifact_role_id.value());
    if (expected.cardinality ==
            ProductArtifactCardinality::kRequiredExactlyOne &&
        found == supplied.end()) {
      fail_retained_bundle("product is missing a required artifact role");
    }
    if (found != supplied.end() &&
        found->second->media_type != expected.media_type) {
      fail_retained_bundle(
          "product artifact media type does not match its contract");
    }
    if (found != supplied.end() &&
        found->second->bytes.size() > detail::kMaximumIJsonInteger) {
      fail_retained_bundle(
          "product artifact byte size exceeds the I-JSON safe integer");
    }
  }
}

core::RetainedProductDTO build_and_retain_product(
    const core::ProductId& product_id,
    core::RetainedProductKind product_kind, ProductPlacement placement,
    const core::ProductContractId& product_contract_id,
    const core::ProductContractRevision& product_contract_revision,
    std::span<const RetainedArtifactInput> artifacts,
    IRawArtifactStore& artifact_store) {
  preflight_retained_product(
      product_id, product_kind, placement, product_contract_id,
      product_contract_revision, artifacts);
  const auto* contract = build_qualified_product_contract_registry().find(
      product_contract_id, product_contract_revision);
  if (contract == nullptr) {
    throw std::logic_error(
        "retained product contract disappeared after pure preflight");
  }
  std::map<std::string, const RetainedArtifactInput*> supplied;
  for (const auto& artifact : artifacts) {
    supplied.emplace(artifact.artifact_role_id.value(), &artifact);
  }
  std::vector<core::RetainedArtifactRefDTO> retained;
  retained.reserve(supplied.size());
  for (const auto& expected : contract->artifact_roles()) {
    const auto found = supplied.find(expected.artifact_role_id.value());
    if (found == supplied.end()) {
      continue;
    }
    const std::uint64_t expected_byte_size =
        static_cast<std::uint64_t>(found->second->bytes.size());
    const std::string expected_digest = sha256_digest(found->second->bytes);
    const std::string expected_key =
        retained_store_key_for_digest(expected_digest);
    const StoredRawArtifact stored =
        artifact_store.put_if_absent(found->second->bytes);
    if (stored.byte_size != expected_byte_size ||
        stored.content_digest.value() != expected_digest ||
        stored.content_addressed_store_key.value() != expected_key) {
      throw std::runtime_error(
          "raw artifact store returned inconsistent immutable metadata");
    }
    const auto read_back = artifact_store.get_verified(
        core::ContentAddressedStoreKey(expected_key),
        core::ContentDigest(expected_digest), expected_byte_size);
    if (!read_back.has_value() || *read_back != found->second->bytes) {
      throw std::runtime_error(
          "raw artifact store failed immutable put verification");
    }
    retained.push_back(core::RetainedArtifactRefDTO{
        expected.artifact_role_id,
        expected.media_type,
        expected_byte_size,
        core::ContentDigest(expected_digest),
        core::ContentAddressedStoreKey(expected_key),
    });
  }

  core::RetainedProductDTO product{
      product_id,
      product_kind,
      product_contract_id,
      product_contract_revision,
      std::move(retained),
  };
  require_current_retained_product_conformance(product);
  return product;
}

}  // namespace detail

}  // namespace manyforge::deployment
