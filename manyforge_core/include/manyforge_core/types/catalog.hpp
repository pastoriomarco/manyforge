#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace manyforge::core {

/// Closed v1 catalog-record kind set (spec 430 §3).
enum class CatalogRecordKind : std::uint8_t {
  kModelAsset = 0,
  kRuntimeProfile,
  kComponentType,
  kControlInterfaceContract,
  kControllerTopology,
  kProviderManifest,
  kCommandSchema,
  kCapabilityDefinition,
  kContactPolicy,
  kStopEvidencePolicy,
};

template <CatalogRecordKind Kind, typename Id, typename Revision>
struct TypedCatalogRecordRefDTO {
  using IdType = Id;
  using RevisionType = Revision;

  static constexpr CatalogRecordKind kRecordKind = Kind;

  Id record_id;
  Revision record_revision;
  SchemaId schema_id;
  SchemaRevision schema_revision;
  ContentDigest content_digest;
  ContentAddressedStoreKey content_addressed_store_key;

  friend bool operator==(const TypedCatalogRecordRefDTO&,
                         const TypedCatalogRecordRefDTO&) = default;
};

using ModelAssetCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kModelAsset, ModelAssetId, ModelRevision>;
using RuntimeProfileCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kRuntimeProfile, RuntimeProfileId,
                             RuntimeProfileRevision>;
using ComponentTypeCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kComponentType, ComponentTypeId,
                             ComponentTypeRevision>;
using ControlInterfaceContractCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kControlInterfaceContract,
                             ControlInterfaceContractId, ControlInterfaceRevision>;
using ControllerTopologyCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kControllerTopology, ControllerTopologyId,
                             TopologyRevision>;
using ProviderManifestCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kProviderManifest, ProviderManifestId,
                             ProviderManifestRevision>;
using CommandSchemaCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kCommandSchema, CommandSchemaId,
                             CommandSchemaRevision>;
using CapabilityDefinitionCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kCapabilityDefinition, CapabilityId,
                             CapabilityDefinitionRevision>;
using ContactPolicyCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kContactPolicy, ContactPolicyId,
                             ContactPolicyRevision>;
using StopEvidencePolicyCatalogRecordRefDTO =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kStopEvidencePolicy, StopEvidencePolicyId,
                             StopEvidencePolicyRevision>;

/// Closed tagged union. The selected alternative fixes both record_id and
/// record_revision types; an ID/revision pair from different registry rows
/// cannot be represented.
using CatalogRecordRefDTO =
    std::variant<ModelAssetCatalogRecordRefDTO, RuntimeProfileCatalogRecordRefDTO,
                 ComponentTypeCatalogRecordRefDTO, ControlInterfaceContractCatalogRecordRefDTO,
                 ControllerTopologyCatalogRecordRefDTO, ProviderManifestCatalogRecordRefDTO,
                 CommandSchemaCatalogRecordRefDTO, CapabilityDefinitionCatalogRecordRefDTO,
                 ContactPolicyCatalogRecordRefDTO, StopEvidencePolicyCatalogRecordRefDTO>;

struct CatalogRecordTypeDescriptor {
  CatalogRecordKind record_kind;
  std::string_view serialized_kind;
  std::string_view own_id_field;
  std::string_view own_revision_field;
  std::string_view schema_family;

  friend bool operator==(const CatalogRecordTypeDescriptor&,
                         const CatalogRecordTypeDescriptor&) = default;
};

inline constexpr std::array<CatalogRecordTypeDescriptor, 10> kCatalogRecordTypeRegistry{{
    {CatalogRecordKind::kModelAsset, "model_asset", "model_asset_id", "model_revision",
     "manyforge.model_asset.v1"},
    {CatalogRecordKind::kRuntimeProfile, "runtime_profile", "runtime_profile_id",
     "runtime_profile_revision", "manyforge.runtime_profile.v1"},
    {CatalogRecordKind::kComponentType, "component_type", "component_type_id",
     "component_type_revision", "manyforge.component_type.v1"},
    {CatalogRecordKind::kControlInterfaceContract, "control_interface_contract",
     "control_interface_contract_id", "control_interface_revision",
     "manyforge.control_interface_contract.v1"},
    {CatalogRecordKind::kControllerTopology, "controller_topology", "controller_topology_id",
     "topology_revision", "manyforge.controller_topology.v1"},
    {CatalogRecordKind::kProviderManifest, "provider_manifest", "provider_manifest_id",
     "provider_manifest_revision", "manyforge.provider_manifest.v1"},
    {CatalogRecordKind::kCommandSchema, "command_schema", "command_schema_id",
     "command_schema_revision", "manyforge.command_schema.v1"},
    {CatalogRecordKind::kCapabilityDefinition, "capability_definition", "capability_id",
     "capability_definition_revision", "manyforge.capability_definition.v1"},
    {CatalogRecordKind::kContactPolicy, "contact_policy", "policy_id", "version",
     "manyforge.contact_policy.v1"},
    {CatalogRecordKind::kStopEvidencePolicy, "stop_evidence_policy", "stop_evidence_policy_id",
     "stop_evidence_policy_revision", "manyforge.stop_evidence_policy.v2"},
}};

static_assert(std::variant_size_v<CatalogRecordRefDTO> == kCatalogRecordTypeRegistry.size());

[[nodiscard]] constexpr const CatalogRecordTypeDescriptor* catalog_record_type_descriptor(
    CatalogRecordKind kind) noexcept {
  for (const auto& descriptor : kCatalogRecordTypeRegistry) {
    if (descriptor.record_kind == kind) {
      return &descriptor;
    }
  }
  return nullptr;
}

[[nodiscard]] constexpr std::string_view to_string(CatalogRecordKind kind) noexcept {
  const auto* descriptor = catalog_record_type_descriptor(kind);
  return descriptor == nullptr ? std::string_view{} : descriptor->serialized_kind;
}

[[nodiscard]] inline CatalogRecordKind catalog_record_kind(const CatalogRecordRefDTO& record) {
  return std::visit(
      [](const auto& typed_record) { return std::decay_t<decltype(typed_record)>::kRecordKind; },
      record);
}

[[nodiscard]] inline const CatalogRecordTypeDescriptor& catalog_record_type_descriptor(
    const CatalogRecordRefDTO& record) {
  return std::visit(
      [](const auto& typed_record) -> const CatalogRecordTypeDescriptor& {
        using Record = std::decay_t<decltype(typed_record)>;
        return *catalog_record_type_descriptor(Record::kRecordKind);
      },
      record);
}

[[nodiscard]] inline const std::string& catalog_record_id_value(const CatalogRecordRefDTO& record) {
  return std::visit(
      [](const auto& typed_record) -> const std::string& { return typed_record.record_id.value(); },
      record);
}

[[nodiscard]] inline const std::string& catalog_record_revision_value(
    const CatalogRecordRefDTO& record) {
  return std::visit(
      [](const auto& typed_record) -> const std::string& {
        return typed_record.record_revision.value();
      },
      record);
}

[[nodiscard]] inline bool catalog_record_ref_matches_registry_family(
    const CatalogRecordRefDTO& record) {
  const auto& descriptor = catalog_record_type_descriptor(record);
  return descriptor.record_kind == catalog_record_kind(record) &&
         std::visit(
             [&descriptor](const auto& typed_record) {
               return typed_record.schema_id.value() == descriptor.schema_family;
             },
             record);
}

/// Canonical order for a schema-validated catalog record table.
///
/// Enum ordinals and tagged-union alternative indexes are deliberately not
/// semantic. The validated bounded-ASCII ID grammar makes raw string order
/// identical to RFC 8785/UTF-16 order.
struct CanonicalCatalogRecordRefLess final {
  [[nodiscard]] bool operator()(const CatalogRecordRefDTO& left,
                                const CatalogRecordRefDTO& right) const {
    const std::string_view left_kind = to_string(catalog_record_kind(left));
    const std::string_view right_kind = to_string(catalog_record_kind(right));
    if (left_kind != right_kind) {
      return left_kind < right_kind;
    }
    const std::string& left_id = catalog_record_id_value(left);
    const std::string& right_id = catalog_record_id_value(right);
    if (left_id != right_id) {
      return left_id < right_id;
    }
    return catalog_record_revision_value(left) < catalog_record_revision_value(right);
  }
};

}  // namespace manyforge::core
