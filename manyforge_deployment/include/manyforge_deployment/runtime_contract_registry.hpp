#pragma once

#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/retained_bundle.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {

/// A role-to-API assertion owned by the runtime-facing semantic owner.
struct RuntimeContractRoleApiMapping final {
  core::RuntimeRoleId runtime_role_id;
  core::ApiContractId api_contract_id;
  std::string semantic_owner_id;

  friend bool operator==(const RuntimeContractRoleApiMapping&,
                         const RuntimeContractRoleApiMapping&) = default;
};

/// A source-endpoint-to-API assertion owned by the endpoint semantic owner.
struct RuntimeContractEndpointApiMapping final {
  core::EndpointRoleId source_endpoint_role_id;
  core::ApiContractId api_contract_id;
  std::string semantic_owner_id;

  friend bool operator==(const RuntimeContractEndpointApiMapping&,
                         const RuntimeContractEndpointApiMapping&) = default;
};

/// One immutable build-qualified implementation/product declaration.
///
/// The declaration is code-owned and cannot be supplied by a deployment or
/// runtime process.  Its offer is generated once and is used for both the
/// package manifest-set and the embedded runtime manifest artifact.
class BuildQualifiedRuntimeContract final {
 public:
  [[nodiscard]] const core::RuntimeProfileId& runtime_profile_id()
      const noexcept {
    return runtime_profile_id_;
  }
  [[nodiscard]] const core::RuntimeProfileRevision&
  runtime_profile_revision() const noexcept {
    return runtime_profile_revision_;
  }
  [[nodiscard]] const core::ProductContractId& runtime_product_id()
      const noexcept {
    return runtime_product_id_;
  }
  [[nodiscard]] const core::ProductContractRevision&
  runtime_product_revision() const noexcept {
    return runtime_product_revision_;
  }
  [[nodiscard]] const core::RuntimeContractOfferDTO& offer() const noexcept {
    return offer_;
  }
  [[nodiscard]] std::span<const RuntimeContractRoleApiMapping>
  runtime_role_api_mappings() const noexcept {
    return runtime_role_api_mappings_;
  }
  [[nodiscard]] std::span<const RuntimeContractEndpointApiMapping>
  source_endpoint_api_mappings() const noexcept {
    return source_endpoint_api_mappings_;
  }

 private:
  BuildQualifiedRuntimeContract(
      core::RuntimeProfileId runtime_profile_id,
      core::RuntimeProfileRevision runtime_profile_revision,
      core::ProductContractId runtime_product_id,
      core::ProductContractRevision runtime_product_revision,
      core::RuntimeContractOfferDTO offer,
      std::vector<RuntimeContractRoleApiMapping> runtime_role_api_mappings,
      std::vector<RuntimeContractEndpointApiMapping>
          source_endpoint_api_mappings);

  core::RuntimeProfileId runtime_profile_id_;
  core::RuntimeProfileRevision runtime_profile_revision_;
  core::ProductContractId runtime_product_id_;
  core::ProductContractRevision runtime_product_revision_;
  core::RuntimeContractOfferDTO offer_;
  std::vector<RuntimeContractRoleApiMapping> runtime_role_api_mappings_;
  std::vector<RuntimeContractEndpointApiMapping>
      source_endpoint_api_mappings_;

  friend class BuildQualifiedRuntimeContractRegistry;
};

/// Deployment-owned single source of maintained runtime offers.
class BuildQualifiedRuntimeContractRegistry final {
 public:
  [[nodiscard]] std::span<const BuildQualifiedRuntimeContract> contracts()
      const noexcept {
    return contracts_;
  }
  [[nodiscard]] const BuildQualifiedRuntimeContract* find(
      const core::RuntimeProfileId& runtime_profile_id,
      const core::RuntimeProfileRevision& runtime_profile_revision,
      const core::ProductContractId& runtime_product_id,
      const core::ProductContractRevision& runtime_product_revision)
      const noexcept;
  [[nodiscard]] const core::RuntimeContractManifestSetDTO&
  package_manifest_set() const noexcept {
    return package_manifest_set_;
  }

 private:
  BuildQualifiedRuntimeContractRegistry();
  [[nodiscard]] static std::vector<BuildQualifiedRuntimeContract> make_rows();

  std::vector<BuildQualifiedRuntimeContract> contracts_;
  core::RuntimeContractManifestSetDTO package_manifest_set_;

  friend const BuildQualifiedRuntimeContractRegistry&
  build_qualified_runtime_contract_registry();
};

[[nodiscard]] const BuildQualifiedRuntimeContractRegistry&
build_qualified_runtime_contract_registry();

namespace detail {

/// Canonical package/embedded offer projection.  It is deliberately routed
/// through the Core codec so there is one wire definition.
[[nodiscard]] CanonicalValue runtime_contract_offer_projection(
    const core::RuntimeContractOfferDTO& offer);

[[nodiscard]] core::RuntimeContractOfferDTO
decode_runtime_contract_offer_projection(const CanonicalValue& value,
                                         std::string_view field);

[[nodiscard]] const BuildQualifiedRuntimeContract&
require_runtime_contract(
    const core::RuntimeProfileId& runtime_profile_id,
    const core::RuntimeProfileRevision& runtime_profile_revision,
    const core::ProductContractId& runtime_product_id,
    const core::ProductContractRevision& runtime_product_revision);

[[nodiscard]] const BuildQualifiedRuntimeContract&
require_runtime_contract_for_unit(const core::RuntimeUnitDTO& runtime_unit);

[[nodiscard]] std::vector<core::RetainedRuntimeContractRequirementDTO>
derive_runtime_contract_requirements(
    const core::CellGraphDTO& graph,
    std::span<const core::CapabilityProviderBindingDTO>
        capability_provider_bindings,
    std::span<const LogicalCapabilityTemplate> capabilities,
    std::span<const LogicalProviderEndpointTemplate> provider_endpoints,
    std::span<const QualifiedRuntimeProfile> runtime_profiles,
    std::span<const core::RetainedRuntimeUnitProductsDTO>
        runtime_unit_products);

void require_runtime_contract_requirements(
    const core::CellGraphDTO& graph,
    std::span<const core::CapabilityProviderBindingDTO>
        capability_provider_bindings,
    std::span<const core::RetainedRuntimeContractRequirementDTO> requirements,
    std::span<const core::RetainedRuntimeUnitProductsDTO> runtime_unit_products);

}  // namespace detail
}  // namespace manyforge::deployment
