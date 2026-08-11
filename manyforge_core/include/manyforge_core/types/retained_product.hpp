#pragma once

#include <cstdint>
#include <manyforge_core/types/assistant.hpp>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/retained_artifact.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

// These I3 identities deliberately live in a separate header until the
// retained-product DTO family is linked into the complete core surface.  They
// remain StrongString domains and therefore cannot be interchanged with an
// equal-spelled graph, catalog, or raw-content identity.
struct DeploymentArtifactIdTag final {};
using DeploymentArtifactId = StrongString<DeploymentArtifactIdTag>;

struct DeploymentRevisionTag final {};
using DeploymentRevision = StrongString<DeploymentRevisionTag>;

struct GeneratorIdTag final {};
using GeneratorId = StrongString<GeneratorIdTag>;

struct GeneratorVersionTag final {};
using GeneratorVersion = StrongString<GeneratorVersionTag>;

inline constexpr std::string_view kRetainedBundleApiVersion = "manyforge.retained_bundle.v1";
inline constexpr std::string_view kProductContractApiVersion = "manyforge.product_contract.v1";

enum class RetainedProductKind : std::uint8_t {
  kPlanningCollisionModel = 0,
  kViewerModel,
  kControlDomainControl,
  kSymbolRoleMap,
  kRuntimeUnitConfiguration,
};

[[nodiscard]] constexpr std::string_view to_string(RetainedProductKind kind) noexcept {
  switch (kind) {
    case RetainedProductKind::kPlanningCollisionModel:
      return "planning_collision_model";
    case RetainedProductKind::kViewerModel:
      return "viewer_model";
    case RetainedProductKind::kControlDomainControl:
      return "control_domain_control";
    case RetainedProductKind::kSymbolRoleMap:
      return "symbol_role_map";
    case RetainedProductKind::kRuntimeUnitConfiguration:
      return "runtime_unit_configuration";
  }
  return {};
}

/// Exact five-field semantic retained-product value.
struct RetainedProductDTO final {
  ProductId product_id;
  RetainedProductKind product_kind;
  ProductContractId product_contract_id;
  ProductContractRevision product_contract_revision;
  std::vector<RetainedArtifactRefDTO> artifact_refs;

  friend bool operator==(const RetainedProductDTO&, const RetainedProductDTO&) = default;
};

/// Path-independent node-environment identity retained in every bundle.
struct RetainedNodeEnvironmentDTO final {
  std::string catalog_id;
  std::string content_version;
  ContentDigest catalog_digest;
  ContentDigest environment_digest;

  friend bool operator==(const RetainedNodeEnvironmentDTO&,
                         const RetainedNodeEnvironmentDTO&) = default;
};

/// Exact immutable reference to one retained closed-schema descriptor.
struct RetainedSchemaArtifactRefDTO final {
  SchemaId schema_id;
  SchemaRevision schema_revision;
  ContentDigest content_digest;
  ContentAddressedStoreKey content_addressed_store_key;

  friend bool operator==(const RetainedSchemaArtifactRefDTO&,
                         const RetainedSchemaArtifactRefDTO&) = default;
};

/// Complete six-field retained catalog/environment value.
struct RetainedCatalogDTO final {
  std::string dialect_api_version;
  ContentDigest bootstrap_contract_digest;
  ContentDigest qualification_registry_digest;
  std::vector<CatalogRecordRefDTO> records;
  std::vector<RetainedSchemaArtifactRefDTO> schema_artifacts;
  RetainedNodeEnvironmentDTO node_environment;

  friend bool operator==(const RetainedCatalogDTO&,
                         const RetainedCatalogDTO&) = default;
};

/// One canonical non-empty product group owned by a control domain.
struct RetainedControlDomainProductsDTO final {
  ControlDomainId control_domain_id;
  std::vector<RetainedProductDTO> products;

  friend bool operator==(const RetainedControlDomainProductsDTO&,
                         const RetainedControlDomainProductsDTO&) = default;
};

/// One canonical non-empty product group owned by a runtime unit.
struct RetainedRuntimeUnitProductsDTO final {
  RuntimeUnitId runtime_unit_id;
  std::vector<RetainedProductDTO> products;

  friend bool operator==(const RetainedRuntimeUnitProductsDTO&,
                         const RetainedRuntimeUnitProductsDTO&) = default;
};

/// Complete closed v1 compiled-product placement table.
struct RetainedCompiledProductsDTO final {
  RetainedProductDTO planning_collision_model;
  std::optional<RetainedProductDTO> viewer_model;
  std::vector<RetainedControlDomainProductsDTO> control_domain_products;
  std::vector<RetainedProductDTO> symbol_role_maps;
  std::vector<RetainedRuntimeUnitProductsDTO> runtime_unit_products;

  friend bool operator==(const RetainedCompiledProductsDTO&,
                         const RetainedCompiledProductsDTO&) = default;
};

template <CatalogRecordKind Kind, typename Id, typename Revision>
struct TypedRetainedSourceRecordDTO final {
  using IdType = Id;
  using RevisionType = Revision;

  static constexpr CatalogRecordKind kRecordKind = Kind;

  Id record_id;
  Revision revision;

  friend bool operator==(const TypedRetainedSourceRecordDTO&,
                         const TypedRetainedSourceRecordDTO&) = default;
};

using RetainedModelAssetSourceRecordDTO =
    TypedRetainedSourceRecordDTO<CatalogRecordKind::kModelAsset, ModelAssetId,
                                 ModelRevision>;
using RetainedRuntimeProfileSourceRecordDTO =
    TypedRetainedSourceRecordDTO<CatalogRecordKind::kRuntimeProfile,
                                 RuntimeProfileId, RuntimeProfileRevision>;
using RetainedComponentTypeSourceRecordDTO =
    TypedRetainedSourceRecordDTO<CatalogRecordKind::kComponentType,
                                 ComponentTypeId, ComponentTypeRevision>;
using RetainedControlInterfaceContractSourceRecordDTO =
    TypedRetainedSourceRecordDTO<
        CatalogRecordKind::kControlInterfaceContract,
        ControlInterfaceContractId, ControlInterfaceRevision>;
using RetainedControllerTopologySourceRecordDTO =
    TypedRetainedSourceRecordDTO<CatalogRecordKind::kControllerTopology,
                                 ControllerTopologyId, TopologyRevision>;
using RetainedProviderManifestSourceRecordDTO =
    TypedRetainedSourceRecordDTO<CatalogRecordKind::kProviderManifest,
                                 ProviderManifestId,
                                 ProviderManifestRevision>;
using RetainedCommandSchemaSourceRecordDTO =
    TypedRetainedSourceRecordDTO<CatalogRecordKind::kCommandSchema,
                                 CommandSchemaId, CommandSchemaRevision>;
using RetainedCapabilityDefinitionSourceRecordDTO =
    TypedRetainedSourceRecordDTO<
        CatalogRecordKind::kCapabilityDefinition, CapabilityId,
        CapabilityDefinitionRevision>;
using RetainedContactPolicySourceRecordDTO =
    TypedRetainedSourceRecordDTO<CatalogRecordKind::kContactPolicy,
                                 ContactPolicyId, ContactPolicyRevision>;
using RetainedStopEvidencePolicySourceRecordDTO =
    TypedRetainedSourceRecordDTO<CatalogRecordKind::kStopEvidencePolicy,
                                 StopEvidencePolicyId,
                                 StopEvidencePolicyRevision>;
using RetainedRuntimePolicySourceRecordDTO =
    TypedRetainedSourceRecordDTO<CatalogRecordKind::kRuntimePolicy,
                                 RuntimePolicyId, RuntimePolicyRevision>;

/// Closed source-provenance union. The selected record kind fixes the strong
/// source ID and revision domains.
using RetainedSourceRecordDTO = std::variant<
    RetainedModelAssetSourceRecordDTO,
    RetainedRuntimeProfileSourceRecordDTO,
    RetainedComponentTypeSourceRecordDTO,
    RetainedControlInterfaceContractSourceRecordDTO,
    RetainedControllerTopologySourceRecordDTO,
    RetainedProviderManifestSourceRecordDTO,
    RetainedCommandSchemaSourceRecordDTO,
    RetainedCapabilityDefinitionSourceRecordDTO,
    RetainedContactPolicySourceRecordDTO,
    RetainedStopEvidencePolicySourceRecordDTO,
    RetainedRuntimePolicySourceRecordDTO>;

static_assert(std::variant_size_v<RetainedSourceRecordDTO> ==
              kCatalogRecordTypeRegistry.size());

/// Exact offline generator provenance row.
struct RetainedGeneratorRecordDTO final {
  GeneratorId generator_id;
  GeneratorVersion generator_version;
  ContentDigest build_digest;
  ContentDigest configuration_digest;

  friend bool operator==(const RetainedGeneratorRecordDTO&,
                         const RetainedGeneratorRecordDTO&) = default;
};

/// Exact provenance projection for one semantic product.
struct RetainedOutputRecordDTO final {
  ProductId product_id;
  RetainedProductKind product_kind;
  ContentDigest content_digest;

  friend bool operator==(const RetainedOutputRecordDTO&, const RetainedOutputRecordDTO&) = default;
};

/// Complete five-field retained-bundle provenance value.
struct RetainedBundleProvenanceDTO final {
  ContentDigest compiler_contract_digest;
  ContentDigest compiler_build_digest;
  std::vector<RetainedSourceRecordDTO> source_records;
  std::vector<RetainedGeneratorRecordDTO> generator_records;
  std::vector<RetainedOutputRecordDTO> output_records;

  friend bool operator==(const RetainedBundleProvenanceDTO&,
                         const RetainedBundleProvenanceDTO&) = default;
};

/// Complete closed `manyforge.retained_bundle.v1` semantic value.
///
/// This aggregate is an untrusted value representation. Only the deployment
/// package's sealed RetainedBundle materializer may prove the complete
/// canonical projection and mint RetainedBundleId.
struct RetainedBundleDTO final {
  std::string api_version;
  RetainedBundleId retained_bundle_id;
  DeploymentArtifactId deployment_artifact_id;
  DeploymentRevision deployment_revision;
  CellGraphDTO cell_graph;
  RetainedCatalogDTO catalogs;
  std::vector<DesiredControlConfigurationDTO>
      desired_control_configurations;
  std::vector<CapabilityProviderBindingDTO> capability_provider_bindings;
  RetainedCompiledProductsDTO compiled_products;
  RetainedBundleProvenanceDTO provenance;
  std::optional<SiteOverlayRefDTO> selected_site_overlay_ref;
  std::vector<ExternalResourceDTO> external_resources;
  std::optional<RetainedAssistantProjectionDTO> assistant;

  friend bool operator==(const RetainedBundleDTO&,
                         const RetainedBundleDTO&) = default;
};

}  // namespace manyforge::core
