#include "manyforge_deployment/verified_retained_bundle.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "compiler_semantic_limits_internal.hpp"
#include "content_identity_internal.hpp"
#include "deployment_catalog_codec_internal.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/cell_graph_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/deployment_applicability_codec.hpp"
#include "manyforge_deployment/structural_model_preparation.hpp"
#include "composed_model_symbol_role_map_internal.hpp"
#include "planning_resource_map_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kFlattenedModelContractId =
    "manyforge.flattened_model";
constexpr std::string_view kFlattenedModelPlacement =
    "cell_planning_collision_model";
constexpr std::string_view kSymbolRoleMapPlacement = "symbol_role_map";
constexpr std::string_view kContentIdentityPrefix =
    "manyforge.content-identity.v1";
constexpr std::string_view kRetainedProductOutputDomain =
    "retained-product-output";
constexpr std::string_view kRetainedProductSchemaTag =
    "manyforge.retained_bundle.v1#retained_product";
constexpr std::size_t kDigestHexLength = 64U;
constexpr std::string_view kDigestPrefix = "sha256:";
constexpr std::string_view kStoreKeyPrefix = "sha256/";

constexpr std::array<detail::QualifiedRetainedProductArtifactRole, 3U>
    kFlattenedModelArtifactRoles{{
        {detail::kPlanningResourceMapArtifactRole,
         detail::kPlanningResourceMapMediaType, true},
        {"srdf", "application/xml", true},
        {"urdf", "application/xml", true},
    }};
constexpr std::array<detail::QualifiedRetainedProductArtifactRole, 1U>
    kComposedModelSymbolRoleMapArtifactRoles{{
        {detail::kComposedModelSymbolRoleMapArtifactRole,
         detail::kComposedModelSymbolRoleMapMediaType, true},
    }};

[[nodiscard]] constexpr bool
is_ascii_alphanumeric(unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool is_portable_identity(std::string_view value) noexcept {
  if (value.empty() ||
      value.size() > detail::kMaximumPortableIdentityBytes ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
    return is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' ||
           byte == '-';
  });
}

[[nodiscard]] bool is_lower_hex(std::string_view value) noexcept {
  return !value.empty() &&
         std::all_of(value.begin(), value.end(), [](unsigned char byte) {
           return (byte >= '0' && byte <= '9') ||
                  (byte >= 'a' && byte <= 'f');
         });
}

[[nodiscard]] const CanonicalValue::Object&
require_object(const CanonicalValue& value, std::string_view field) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    detail::fail_retained_bundle(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array&
require_array(const CanonicalValue& value, std::string_view field) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    detail::fail_retained_bundle(std::string(field) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const std::string&
require_string(const CanonicalValue& value, std::string_view field) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    detail::fail_retained_bundle(std::string(field) + " must be a string");
  }
  return *text;
}

[[nodiscard]] const CanonicalValue&
require_member(const CanonicalValue::Object& object, std::string_view name,
               std::string_view field) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    detail::fail_retained_bundle(std::string(field) +
                                 " is missing required member " +
                                 std::string(name));
  }
  return found->second;
}

void require_exact_fields(
    const CanonicalValue::Object& object,
    std::initializer_list<std::string_view> expected,
    std::string_view field) {
  if (object.size() != expected.size()) {
    detail::fail_retained_bundle(std::string(field) +
                                 " has an incorrect closed field count");
  }
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      detail::fail_retained_bundle(std::string(field) +
                                   " contains unknown member " + name);
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, field);
  }
}

void require_empty_array(const CanonicalValue& value,
                         std::string_view field) {
  if (!require_array(value, field).empty()) {
    detail::fail_retained_bundle(
        std::string(field) +
        " is outside the currently qualified structural retained subset");
  }
}

void require_null(const CanonicalValue& value, std::string_view field) {
  if (!std::holds_alternative<std::nullptr_t>(value.storage())) {
    detail::fail_retained_bundle(std::string(field) + " must be null");
  }
}

[[nodiscard]] std::uint64_t require_i_json_size(
    const CanonicalValue& value, std::string_view field) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      std::trunc(*number) != *number ||
      *number > static_cast<double>(detail::kMaximumIJsonInteger)) {
    detail::fail_retained_bundle(
        std::string(field) +
        " must be a non-negative I-JSON safe integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] CanonicalValue artifact_role_projection(
    const detail::QualifiedRetainedProductArtifactRole& role) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(std::string(role.artifact_role_id))},
      {"media_type", CanonicalValue(std::string(role.media_type))},
      {"cardinality",
       CanonicalValue(role.required ? "required_exactly_one"
                                    : "optional_at_most_one")},
  });
}

[[nodiscard]] CanonicalValue product_contract_projection(
    std::string_view contract_id, core::RetainedProductKind product_kind,
    std::string_view placement,
    std::span<const detail::QualifiedRetainedProductArtifactRole>
        artifact_roles,
    std::string_view revision) {
  CanonicalValue::Array roles;
  roles.reserve(artifact_roles.size());
  for (const auto& role : artifact_roles) {
    roles.push_back(artifact_role_projection(role));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kProductContractApiVersion))},
      {"product_contract_id",
       CanonicalValue(std::string(contract_id))},
      {"product_contract_revision", CanonicalValue(std::string(revision))},
      {"product_kind",
       CanonicalValue(std::string(core::to_string(product_kind)))},
      {"placement", CanonicalValue(std::string(placement))},
      {"artifact_roles", CanonicalValue(std::move(roles))},
  });
}

struct QualifiedProductContractState final {
  core::ProductContractRevision revision;
  CanonicalValue descriptor;
};

[[nodiscard]] const QualifiedProductContractState&
qualified_product_contract_state(
    std::string_view contract_id, core::RetainedProductKind product_kind,
    std::string_view placement,
    std::span<const detail::QualifiedRetainedProductArtifactRole>
        artifact_roles) {
  const auto build_state = [=] {
    const std::string revision = detail::compute_content_identity(
        product_contract_projection(contract_id, product_kind, placement,
                                    artifact_roles, kZeroDigest),
        detail::ContentIdentitySpec{
            .identity_domain = "product-contract-revision",
            .api_version = std::string(core::kProductContractApiVersion),
            .own_field = "product_contract_revision",
        });
    CanonicalValue descriptor = product_contract_projection(
        contract_id, product_kind, placement, artifact_roles, revision);
    if (!detail::verify_content_identity(
            descriptor,
            detail::ContentIdentitySpec{
                .identity_domain = "product-contract-revision",
                .api_version = std::string(core::kProductContractApiVersion),
                .own_field = "product_contract_revision",
            })) {
      throw std::logic_error(
          "built-in product contract has an invalid content identity");
    }
    return QualifiedProductContractState{
        core::ProductContractRevision(revision), std::move(descriptor)};
  };
  if (contract_id == kFlattenedModelContractId &&
      product_kind == core::RetainedProductKind::kPlanningCollisionModel &&
      placement == kFlattenedModelPlacement &&
      artifact_roles.data() == kFlattenedModelArtifactRoles.data() &&
      artifact_roles.size() == kFlattenedModelArtifactRoles.size()) {
    static const QualifiedProductContractState state = build_state();
    return state;
  }
  if (contract_id == detail::kComposedModelSymbolRoleMapContractId &&
      product_kind == core::RetainedProductKind::kSymbolRoleMap &&
      placement == kSymbolRoleMapPlacement &&
      artifact_roles.data() == kComposedModelSymbolRoleMapArtifactRoles.data() &&
      artifact_roles.size() ==
          kComposedModelSymbolRoleMapArtifactRoles.size()) {
    static const QualifiedProductContractState state = build_state();
    return state;
  }
  throw std::logic_error("unknown built-in product contract state");
}

[[nodiscard]] CanonicalValue retained_artifact_projection(
    const core::RetainedArtifactRefDTO& artifact) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(artifact.artifact_role_id.value())},
      {"media_type", CanonicalValue(artifact.media_type)},
      {"byte_size", CanonicalValue(static_cast<double>(artifact.byte_size))},
      {"content_digest", CanonicalValue(artifact.content_digest.value())},
      {"content_addressed_store_key",
       CanonicalValue(artifact.content_addressed_store_key.value())},
  });
}

[[nodiscard]] std::string domain_separated_canonical_digest(
    std::string_view domain, std::string_view schema_tag,
    const CanonicalValue& value) {
  std::string preimage;
  const std::string payload = canonical_json(value);
  preimage.reserve(kContentIdentityPrefix.size() + domain.size() +
                   schema_tag.size() + payload.size() + 3U);
  preimage.append(kContentIdentityPrefix);
  preimage.push_back('\0');
  preimage.append(domain);
  preimage.push_back('\0');
  preimage.append(schema_tag);
  preimage.push_back('\0');
  preimage.append(payload);
  return sha256_digest(preimage);
}

[[nodiscard]] core::RetainedProductDTO parse_retained_product(
    const CanonicalValue& value, std::string_view field,
    core::RetainedProductKind expected_kind) {
  const auto& object = require_object(value, field);
  require_exact_fields(object,
                       {"product_id", "product_kind",
                        "product_contract_id", "product_contract_revision",
                        "artifact_refs"},
                       field);

  const std::string& product_id =
      require_string(require_member(object, "product_id", field),
                     std::string(field) + ".product_id");
  const std::string& product_kind =
      require_string(require_member(object, "product_kind", field),
                     std::string(field) + ".product_kind");
  if (product_kind != core::to_string(expected_kind)) {
    detail::fail_retained_bundle(
        std::string(field) + " has an invalid product_kind");
  }
  const std::string& contract_id = require_string(
      require_member(object, "product_contract_id", field),
      std::string(field) + ".product_contract_id");
  const std::string& contract_revision = require_string(
      require_member(object, "product_contract_revision", field),
      std::string(field) + ".product_contract_revision");
  detail::require_retained_digest(contract_revision,
                                  "product_contract_revision");

  std::vector<core::RetainedArtifactRefDTO> artifacts;
  const auto& artifact_values =
      require_array(require_member(object, "artifact_refs", field),
                    std::string(field) + ".artifact_refs");
  artifacts.reserve(artifact_values.size());
  for (std::size_t index = 0U; index < artifact_values.size(); ++index) {
    const std::string path = std::string(field) + ".artifact_refs[" +
                             std::to_string(index) + "]";
    const auto& artifact = require_object(artifact_values[index], path);
    require_exact_fields(
        artifact,
        {"artifact_role_id", "media_type", "byte_size", "content_digest",
         "content_addressed_store_key"},
        path);
    artifacts.push_back(core::RetainedArtifactRefDTO{
        core::ArtifactRoleId(require_string(
            require_member(artifact, "artifact_role_id", path),
            path + ".artifact_role_id")),
        require_string(require_member(artifact, "media_type", path),
                       path + ".media_type"),
        require_i_json_size(require_member(artifact, "byte_size", path),
                            path + ".byte_size"),
        core::ContentDigest(require_string(
            require_member(artifact, "content_digest", path),
            path + ".content_digest")),
        core::ContentAddressedStoreKey(require_string(
            require_member(artifact, "content_addressed_store_key", path),
            path + ".content_addressed_store_key")),
    });
  }

  core::RetainedProductDTO product{
      core::ProductId(product_id),
      expected_kind,
      core::ProductContractId(contract_id),
      core::ProductContractRevision(contract_revision),
      std::move(artifacts),
  };
  detail::require_current_retained_product_conformance(product);
  if (detail::retained_product_projection(product) != value) {
    detail::fail_retained_bundle(
        std::string(field) + " differs from its exact typed projection");
  }
  return product;
}

[[nodiscard]] GraphRevisionVerifiedCellGraph
verify_structural_graph_projection(const CanonicalValue& graph_value) {
  const auto typed = decode_cell_graph_candidate(
      parse_strict_json_model_yaml(canonical_json(graph_value)));
  const auto intrinsic = validate_intrinsic_cell_graph_candidate(typed);
  const auto ready = prepare_cell_graph_identity(
      intrinsic, std::span<const QualifiedRuntimeProfile>{});
  const auto verified = verify_graph_revision(ready);
  if (verified.normalized_root() != graph_value) {
    detail::fail_retained_bundle(
        "retained cell_graph differs from its exact verified projection");
  }
  const auto& graph = verified.graph();
  if (!graph.runtime_units.empty() ||
      !graph.component_attachments.empty()) {
    detail::fail_retained_bundle(
        "retained cell_graph is outside the structural no-runtime subset");
  }
  const bool passive = graph.controllable_components.empty() &&
                       graph.control_domains.empty() &&
                       graph.control_bindings.empty();
  if (passive) {
    if (std::any_of(
            graph.model_instances.begin(), graph.model_instances.end(),
            [](const core::ModelInstanceDTO& instance) {
              return !instance.root_pose_in_cell.has_value();
            })) {
      detail::fail_retained_bundle(
          "passive retained model instances require explicit placement");
    }
    return verified;
  }
  if (graph.model_instances.size() != 1U ||
      graph.controllable_components.size() != 1U ||
      graph.control_domains.size() != 1U ||
      graph.control_bindings.size() != 1U) {
    detail::fail_retained_bundle(
        "retained structural control graph is not the exact one-component "
        "subset");
  }
  const auto& model = graph.model_instances.front();
  const auto& component = graph.controllable_components.front();
  const auto& domain = graph.control_domains.front();
  const auto& binding = graph.control_bindings.front();
  if (model.root_pose_in_cell.has_value() ||
      !component.root_pose_in_cell.has_value() ||
      component.model_role_bindings.size() != 1U ||
      component.model_role_bindings.front().model_instance_id !=
          model.model_instance_id ||
      !component.runtime_unit_ids.empty() ||
      !domain.runtime_unit_ids.empty() ||
      binding.resource_bindings.empty() ||
      binding.legal_control_route_ids.size() != 1U ||
      binding.route_endpoint_projections.size() != 1U ||
      binding.controllable_component_id !=
          component.controllable_component_id ||
      binding.control_domain_id != domain.control_domain_id ||
      binding.controller_topology_id != domain.controller_topology_id ||
      binding.topology_revision != domain.topology_revision) {
    detail::fail_retained_bundle(
        "retained structural control graph violates its exact component, "
        "target, binding, or domain closure");
  }
  return verified;
}

[[nodiscard]] std::vector<core::CatalogRecordRefDTO>
parse_catalog_records_projection(const CanonicalValue& records_value) {
  const CanonicalValue catalogs(CanonicalValue::Object{
      {"records", records_value},
  });
  std::vector<core::CatalogRecordRefDTO> records;
  try {
    records = detail::decode_deployment_catalogs(catalogs);
  } catch (const detail::DeploymentCatalogCodecError& error) {
    detail::fail_retained_bundle(
        std::string("retained catalog record table is invalid: ") +
        error.what());
  }
  const CanonicalValue encoded =
      detail::encode_deployment_catalogs(records);
  const CanonicalValue* encoded_records =
      encoded.find_object_member("records");
  if (encoded_records == nullptr || *encoded_records != records_value) {
    detail::fail_retained_bundle(
        "retained catalog records differ from their exact typed projection");
  }
  return records;
}

using CatalogIdentity =
    std::tuple<core::CatalogRecordKind, std::string, std::string>;

void require_structural_catalog_closure(
    const core::CellGraphDTO& graph,
    std::span<const core::CatalogRecordRefDTO> records) {
  std::set<CatalogIdentity> graph_records;
  for (const auto& instance : graph.model_instances) {
    graph_records.emplace(
        core::CatalogRecordKind::kModelAsset,
        instance.model_asset_id.value(), instance.model_revision.value());
  }
  for (const auto& component : graph.controllable_components) {
    graph_records.emplace(
        core::CatalogRecordKind::kComponentType,
        component.component_type_id.value(),
        component.component_type_revision.value());
  }
  for (const auto& domain : graph.control_domains) {
    graph_records.emplace(
        core::CatalogRecordKind::kControllerTopology,
        domain.controller_topology_id.value(),
        domain.topology_revision.value());
  }
  for (const auto& binding : graph.control_bindings) {
    graph_records.emplace(
        core::CatalogRecordKind::kCapabilityDefinition,
        binding.capability_id.value(),
        binding.capability_definition_revision.value());
    graph_records.emplace(
        core::CatalogRecordKind::kControlInterfaceContract,
        binding.control_interface_contract_id.value(),
        binding.control_interface_revision.value());
    graph_records.emplace(
        core::CatalogRecordKind::kControllerTopology,
        binding.controller_topology_id.value(),
        binding.topology_revision.value());
    graph_records.emplace(
        core::CatalogRecordKind::kProviderManifest,
        binding.provider_manifest_id.value(),
        binding.provider_manifest_revision.value());
    graph_records.emplace(
        core::CatalogRecordKind::kStopEvidencePolicy,
        binding.stop_evidence_policy_id.value(),
        binding.stop_evidence_policy_revision.value());
  }

  std::set<CatalogIdentity> catalog_records;
  for (const auto& record : records) {
    const CatalogIdentity identity{
        core::catalog_record_kind(record),
        core::catalog_record_id_value(record),
        core::catalog_record_revision_value(record)};
    if (!catalog_records.insert(identity).second) {
      detail::fail_retained_bundle(
          "retained structural catalogs contain a duplicate identity");
    }
  }
  if (graph_records != catalog_records) {
    detail::fail_retained_bundle(
        "retained structural catalogs are not the exact graph-selected "
        "model/component/control closure");
  }
}

[[nodiscard]] std::vector<core::RetainedSchemaArtifactRefDTO>
parse_structural_schema_artifacts(
    const CanonicalValue& value,
    std::span<const core::CatalogRecordRefDTO> records) {
  using SchemaIdentity = std::pair<std::string, std::string>;
  std::map<SchemaIdentity, core::RetainedSchemaArtifactRefDTO>
      expected_schemas;
  const auto& registry = build_qualified_closed_schema_registry();
  for (const auto& record : records) {
    const auto kind = core::catalog_record_kind(record);
    const auto& schema_id = std::visit(
        [](const auto& typed) -> const core::SchemaId& {
          return typed.schema_id;
        },
        record);
    const auto& schema_revision = std::visit(
        [](const auto& typed) -> const core::SchemaRevision& {
          return typed.schema_revision;
        },
        record);
    const auto* schema = registry.find_qualified_catalog_schema(
        kind, schema_id, schema_revision);
    if (schema == nullptr || schema->compiled_schema() == nullptr) {
      detail::fail_retained_bundle(
          "retained structural catalog schema is not qualified by this "
          "build");
    }
    const std::string schema_bytes(schema->descriptor_bytes());
    const core::ContentDigest schema_digest(sha256_digest(schema_bytes));
    core::RetainedSchemaArtifactRefDTO expected{
        schema_id,
        schema_revision,
        schema_digest,
        core::ContentAddressedStoreKey(
            detail::retained_store_key_for_digest(schema_digest.value())),
    };
    const SchemaIdentity identity{
        schema_id.value(), schema_revision.value()};
    const auto [position, inserted] =
        expected_schemas.emplace(identity, expected);
    if (!inserted && position->second != expected) {
      detail::fail_retained_bundle(
          "one retained schema identity resolved to unequal build "
          "descriptors");
    }
  }

  const auto& rows =
      require_array(value, "retained_bundle.catalogs.schema_artifacts");
  std::vector<core::RetainedSchemaArtifactRefDTO> parsed;
  parsed.reserve(rows.size());
  std::optional<SchemaIdentity> previous_identity;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path =
        "retained_bundle.catalogs.schema_artifacts[" +
        std::to_string(index) + "]";
    const auto& row = require_object(rows[index], path);
    require_exact_fields(
        row,
        {"schema_id", "schema_revision", "content_digest",
         "content_addressed_store_key"},
        path);
    const std::string& schema_id = require_string(
        require_member(row, "schema_id", path), path + ".schema_id");
    const std::string& schema_revision = require_string(
        require_member(row, "schema_revision", path),
        path + ".schema_revision");
    const std::string& content_digest = require_string(
        require_member(row, "content_digest", path),
        path + ".content_digest");
    const std::string& store_key = require_string(
        require_member(row, "content_addressed_store_key", path),
        path + ".content_addressed_store_key");
    detail::require_retained_portable_identity(schema_id,
                                               path + ".schema_id");
    detail::require_retained_digest(schema_revision,
                                    path + ".schema_revision");
    detail::require_retained_digest(content_digest,
                                    path + ".content_digest");
    if (store_key != detail::retained_store_key_for_digest(content_digest)) {
      detail::fail_retained_bundle(
          path + " does not use a digest-derived retained CAS key");
    }
    const SchemaIdentity identity{schema_id, schema_revision};
    if (previous_identity.has_value() &&
        *previous_identity >= identity) {
      detail::fail_retained_bundle(
          "retained schema artifacts are not strictly canonical-sorted");
    }
    previous_identity = identity;
    parsed.push_back(core::RetainedSchemaArtifactRefDTO{
        core::SchemaId(schema_id),
        core::SchemaRevision(schema_revision),
        core::ContentDigest(content_digest),
        core::ContentAddressedStoreKey(store_key),
    });
  }

  if (parsed.size() != expected_schemas.size()) {
    detail::fail_retained_bundle(
        "retained schema artifacts are not the exact structural catalog "
        "schema "
        "closure");
  }
  for (const auto& schema : parsed) {
    const auto expected = expected_schemas.find(
        {schema.schema_id.value(), schema.schema_revision.value()});
    if (expected == expected_schemas.end() ||
        expected->second != schema) {
      detail::fail_retained_bundle(
          "retained schema artifact differs from its build-qualified "
          "descriptor");
    }
  }
  return parsed;
}

[[nodiscard]] std::vector<core::RetainedSourceRecordDTO>
parse_structural_source_records(
    const CanonicalValue& value,
    std::span<const core::CatalogRecordRefDTO> records) {
  std::vector<core::RetainedSourceRecordDTO> expected;
  expected.reserve(records.size());
  for (const auto& record : records) {
    expected.push_back(std::visit(
        [](const auto& typed) -> core::RetainedSourceRecordDTO {
          using Record = std::remove_cvref_t<decltype(typed)>;
          using Source = core::TypedRetainedSourceRecordDTO<
              Record::kRecordKind, typename Record::IdType,
              typename Record::RevisionType>;
          return Source{typed.record_id, typed.record_revision};
        },
        record));
  }

  const auto& rows =
      require_array(value, "retained_bundle.provenance.source_records");
  if (rows.size() != expected.size()) {
    detail::fail_retained_bundle(
        "retained source provenance is not the exact structural catalog "
        "closure");
  }
  std::vector<core::RetainedSourceRecordDTO> parsed;
  parsed.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path =
        "retained_bundle.provenance.source_records[" +
        std::to_string(index) + "]";
    const auto& row = require_object(rows[index], path);
    require_exact_fields(
        row, {"record_kind", "record_id", "revision"}, path);
    const std::string& record_kind = require_string(
        require_member(row, "record_kind", path), path + ".record_kind");
    const std::string& record_id = require_string(
        require_member(row, "record_id", path), path + ".record_id");
    const std::string& revision = require_string(
        require_member(row, "revision", path), path + ".revision");
    detail::require_retained_portable_identity(record_id,
                                               path + ".record_id");
    detail::require_retained_digest(revision, path + ".revision");
    const auto& catalog_record = records[index];
    if (record_kind != core::to_string(
                           core::catalog_record_kind(catalog_record)) ||
        record_id != core::catalog_record_id_value(catalog_record) ||
        revision !=
            core::catalog_record_revision_value(catalog_record)) {
      detail::fail_retained_bundle(
          "retained source provenance differs from the exact catalog "
          "projection");
    }
    parsed.push_back(expected[index]);
  }
  return parsed;
}

[[nodiscard]] std::vector<core::RetainedGeneratorRecordDTO>
parse_structural_generator_records(
    const CanonicalValue& value, std::size_t selected_asset_count) {
  const auto& rows = require_array(
      value, "retained_bundle.provenance.generator_records");
  if ((selected_asset_count == 0U && !rows.empty()) ||
      (selected_asset_count != 0U &&
       (rows.empty() || rows.size() > selected_asset_count))) {
    detail::fail_retained_bundle(
        "model-only generator provenance is not a bounded non-empty "
        "set for the selected structural model assets");
  }
  using Identity =
      std::tuple<std::string, std::string, std::string, std::string>;
  std::optional<Identity> previous;
  std::vector<core::RetainedGeneratorRecordDTO> parsed;
  parsed.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path =
        "retained_bundle.provenance.generator_records[" +
        std::to_string(index) + "]";
    const auto& row = require_object(rows[index], path);
    require_exact_fields(
        row,
        {"generator_id", "generator_version", "build_digest",
         "configuration_digest"},
        path);
    const std::string& generator_id = require_string(
        require_member(row, "generator_id", path),
        path + ".generator_id");
    const std::string& generator_version = require_string(
        require_member(row, "generator_version", path),
        path + ".generator_version");
    const std::string& build_digest = require_string(
        require_member(row, "build_digest", path),
        path + ".build_digest");
    const std::string& configuration_digest = require_string(
        require_member(row, "configuration_digest", path),
        path + ".configuration_digest");
    if (generator_id !=
            kStructuralModelPreparationGeneratorId ||
        generator_version !=
            kStructuralModelPreparationGeneratorVersion) {
      detail::fail_retained_bundle(
          path + " selects an unqualified structural generator");
    }
    detail::require_retained_digest(
        build_digest, path + ".build_digest");
    detail::require_retained_digest(
        configuration_digest, path + ".configuration_digest");
    Identity identity{
        generator_id, generator_version, build_digest,
        configuration_digest};
    if (previous.has_value() && *previous >= identity) {
      detail::fail_retained_bundle(
          "retained generator records are not strictly canonical-sorted");
    }
    previous = identity;
    parsed.push_back(core::RetainedGeneratorRecordDTO{
        core::GeneratorId(generator_id),
        core::GeneratorVersion(generator_version),
        core::ContentDigest(build_digest),
        core::ContentDigest(configuration_digest),
    });
  }
  return parsed;
}

void verify_node_environment_projection(const CanonicalValue& value) {
  const auto& object = require_object(value, "catalogs.node_environment");
  require_exact_fields(
      object,
      {"catalog_id", "content_version", "catalog_digest",
       "environment_digest"},
      "catalogs.node_environment");
  detail::require_retained_portable_identity(
      require_string(require_member(object, "catalog_id",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.catalog_id"),
      "catalogs.node_environment.catalog_id");
  detail::require_retained_portable_identity(
      require_string(require_member(object, "content_version",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.content_version"),
      "catalogs.node_environment.content_version");
  detail::require_retained_digest(
      require_string(require_member(object, "catalog_digest",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.catalog_digest"),
      "catalogs.node_environment.catalog_digest");
  detail::require_retained_digest(
      require_string(require_member(object, "environment_digest",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.environment_digest"),
      "catalogs.node_environment.environment_digest");
}

[[nodiscard]] RevisionVerifiedDeploymentApplicabilityInputs
verify_structural_applicability_projection(
    const CanonicalValue& desired_value,
    const CanonicalValue& provider_value,
    const core::CellGraphDTO& graph) {
  require_empty_array(
      provider_value,
      "retained_bundle.capability_provider_bindings");
  const CanonicalValue root(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"desired_control_configurations", desired_value},
      {"capability_provider_bindings", provider_value},
  });
  RevisionVerifiedDeploymentApplicabilityInputs verified =
      compute_capability_provider_binding_revisions(
          decode_deployment_applicability_inputs_candidate(
              parse_strict_json_model_yaml(canonical_json(root))));
  if (verified.normalized_desired_control_configurations() !=
          desired_value ||
      verified.normalized_capability_provider_bindings() !=
          provider_value) {
    detail::fail_retained_bundle(
        "retained applicability differs from its exact revision-verified "
        "projection");
  }
  const auto& desired = verified.value().desired_control_configurations;
  if (graph.control_domains.empty()) {
    if (!desired.empty()) {
      detail::fail_retained_bundle(
          "passive structural retention cannot select a control "
          "configuration");
    }
    return verified;
  }
  if (desired.size() != 1U || graph.control_domains.size() != 1U ||
      graph.control_bindings.size() != 1U) {
    detail::fail_retained_bundle(
        "one-component structural retention requires one desired control "
        "configuration");
  }
  const auto& domain = graph.control_domains.front();
  const auto& binding = graph.control_bindings.front();
  const auto& selected = desired.front();
  if (selected.control_domain_id != domain.control_domain_id ||
      selected.control_domain_revision != domain.control_domain_revision ||
      selected.controller_topology_id != domain.controller_topology_id ||
      selected.topology_revision != domain.topology_revision ||
      selected.binding_routes.size() != 1U ||
      selected.binding_routes.front().control_binding_id !=
          binding.control_binding_id ||
      selected.binding_routes.front().control_binding_revision !=
          binding.control_binding_revision ||
      selected.binding_routes.front().control_route_id !=
          binding.legal_control_route_ids.front()) {
    detail::fail_retained_bundle(
        "retained desired control configuration is not the exact selected "
        "domain/binding route");
  }
  return verified;
}

[[nodiscard]] core::RetainedBundleDTO
verify_current_retained_bundle_projection(const CanonicalValue& root) {
  const auto& object = require_object(root, "retained_bundle");
  require_exact_fields(
      object,
      {"api_version", "retained_bundle_id", "deployment_artifact_id",
       "deployment_revision", "cell_graph", "catalogs",
       "desired_control_configurations", "capability_provider_bindings",
       "compiled_products", "provenance"},
      "retained_bundle");
  if (require_string(require_member(object, "api_version", "retained_bundle"),
                     "retained_bundle.api_version") !=
      core::kRetainedBundleApiVersion) {
    detail::fail_retained_bundle(
        "retained_bundle.api_version is not the exact v1 API");
  }
  const std::string& retained_bundle_id = require_string(
      require_member(object, "retained_bundle_id", "retained_bundle"),
      "retained_bundle.retained_bundle_id");
  detail::require_retained_digest(retained_bundle_id,
                                  "retained_bundle.retained_bundle_id");
  const std::string& deployment_artifact_id = require_string(
      require_member(object, "deployment_artifact_id", "retained_bundle"),
      "retained_bundle.deployment_artifact_id");
  detail::require_retained_portable_identity(
      deployment_artifact_id, "retained_bundle.deployment_artifact_id");
  const std::string& deployment_revision = require_string(
      require_member(object, "deployment_revision", "retained_bundle"),
      "retained_bundle.deployment_revision");
  detail::require_retained_digest(deployment_revision,
                                  "retained_bundle.deployment_revision");
  const auto verified_graph = verify_structural_graph_projection(
      require_member(object, "cell_graph", "retained_bundle"));

  const auto& catalogs =
      require_object(require_member(object, "catalogs", "retained_bundle"),
                     "retained_bundle.catalogs");
  require_exact_fields(
      catalogs,
      {"dialect_api_version", "bootstrap_contract_digest",
       "qualification_registry_digest", "records", "schema_artifacts",
       "node_environment"},
      "retained_bundle.catalogs");
  const std::string& dialect_api_version = require_string(
      require_member(catalogs, "dialect_api_version",
                     "retained_bundle.catalogs"),
      "retained_bundle.catalogs.dialect_api_version");
  if (dialect_api_version != kClosedSchemaApiVersion) {
    detail::fail_retained_bundle(
        "retained catalog dialect does not match this build");
  }
  const std::string& bootstrap_contract_digest = require_string(
      require_member(catalogs, "bootstrap_contract_digest",
                     "retained_bundle.catalogs"),
      "retained_bundle.catalogs.bootstrap_contract_digest");
  if (bootstrap_contract_digest != kClosedSchemaBootstrapContractDigest) {
    detail::fail_retained_bundle(
        "retained bootstrap contract does not match this build");
  }
  const std::string& qualification_registry_digest = require_string(
      require_member(catalogs, "qualification_registry_digest",
                     "retained_bundle.catalogs"),
      "retained_bundle.catalogs.qualification_registry_digest");
  if (qualification_registry_digest !=
      build_qualified_closed_schema_registry()
          .qualification_registry_digest()
          .value()) {
    detail::fail_retained_bundle(
        "retained qualification registry does not match this build");
  }
  const auto catalog_records = parse_catalog_records_projection(
      require_member(catalogs, "records", "retained_bundle.catalogs"));
  require_structural_catalog_closure(
      verified_graph.graph(), catalog_records);
  const auto schema_artifacts = parse_structural_schema_artifacts(
      require_member(catalogs, "schema_artifacts",
                     "retained_bundle.catalogs"),
      catalog_records);
  const CanonicalValue& node_environment_value = require_member(
      catalogs, "node_environment", "retained_bundle.catalogs");
  verify_node_environment_projection(node_environment_value);
  const auto& node_environment =
      require_object(node_environment_value, "catalogs.node_environment");

  const auto applicability = verify_structural_applicability_projection(
      require_member(object, "desired_control_configurations",
                     "retained_bundle"),
      require_member(object, "capability_provider_bindings",
                     "retained_bundle"),
      verified_graph.graph());

  const auto& products = require_object(
      require_member(object, "compiled_products", "retained_bundle"),
      "retained_bundle.compiled_products");
  require_exact_fields(
      products,
      {"planning_collision_model", "viewer_model",
       "control_domain_products", "symbol_role_maps",
       "runtime_unit_products"},
      "retained_bundle.compiled_products");
  const core::RetainedProductDTO planning = parse_retained_product(
      require_member(products, "planning_collision_model",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.planning_collision_model",
      core::RetainedProductKind::kPlanningCollisionModel);
  require_null(require_member(products, "viewer_model",
                              "retained_bundle.compiled_products"),
               "retained_bundle.compiled_products.viewer_model");
  require_empty_array(
      require_member(products, "control_domain_products",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.control_domain_products");
  const auto& symbol_role_map_values = require_array(
      require_member(products, "symbol_role_maps",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.symbol_role_maps");
  std::vector<core::RetainedProductDTO> symbol_role_maps;
  if (verified_graph.graph().model_instances.empty()) {
    if (!symbol_role_map_values.empty()) {
      detail::fail_retained_bundle(
          "canonical-empty bundle cannot contain a model symbol/role map");
    }
  } else {
    if (symbol_role_map_values.size() != 1U) {
      detail::fail_retained_bundle(
          "non-empty model bundle requires exactly one composed-model "
          "symbol/role map");
    }
    symbol_role_maps.push_back(parse_retained_product(
        symbol_role_map_values.front(),
        "retained_bundle.compiled_products.symbol_role_maps[0]",
        core::RetainedProductKind::kSymbolRoleMap));
    if (symbol_role_maps.front().product_id.value() !=
        "cell_composed_model_symbol_role_map") {
      detail::fail_retained_bundle(
          "composed-model symbol/role map has the wrong stable product ID");
    }
  }
  require_empty_array(
      require_member(products, "runtime_unit_products",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.runtime_unit_products");

  const auto& provenance =
      require_object(require_member(object, "provenance", "retained_bundle"),
                     "retained_bundle.provenance");
  require_exact_fields(
      provenance,
      {"compiler_contract_digest", "compiler_build_digest",
       "source_records", "generator_records", "output_records"},
      "retained_bundle.provenance");
  const std::string& compiler_contract_digest = require_string(
      require_member(provenance, "compiler_contract_digest",
                     "retained_bundle.provenance"),
      "retained_bundle.provenance.compiler_contract_digest");
  detail::require_retained_digest(
      compiler_contract_digest,
      "retained_bundle.provenance.compiler_contract_digest");
  const std::string& compiler_build_digest = require_string(
      require_member(provenance, "compiler_build_digest",
                     "retained_bundle.provenance"),
      "retained_bundle.provenance.compiler_build_digest");
  detail::require_retained_digest(
      compiler_build_digest,
      "retained_bundle.provenance.compiler_build_digest");
  const auto source_records = parse_structural_source_records(
      require_member(provenance, "source_records",
                     "retained_bundle.provenance"),
      catalog_records);
  const std::size_t selected_model_asset_count =
      static_cast<std::size_t>(std::count_if(
          catalog_records.begin(), catalog_records.end(),
          [](const core::CatalogRecordRefDTO& record) {
            return core::catalog_record_kind(record) ==
                   core::CatalogRecordKind::kModelAsset;
          }));
  const auto generator_records = parse_structural_generator_records(
      require_member(provenance, "generator_records",
                     "retained_bundle.provenance"),
      selected_model_asset_count);

  const auto& outputs =
      require_array(require_member(provenance, "output_records",
                                   "retained_bundle.provenance"),
                    "retained_bundle.provenance.output_records");
  std::vector<const core::RetainedProductDTO*> expected_outputs{&planning};
  for (const auto& product : symbol_role_maps) {
    expected_outputs.push_back(&product);
  }
  std::sort(expected_outputs.begin(), expected_outputs.end(),
            [](const auto* left, const auto* right) {
              return left->product_id.value() < right->product_id.value();
            });
  if (outputs.size() != expected_outputs.size()) {
    detail::fail_retained_bundle(
        "structural provenance does not contain one output per product");
  }
  std::vector<core::RetainedOutputRecordDTO> output_records;
  output_records.reserve(outputs.size());
  for (std::size_t index = 0U; index < outputs.size(); ++index) {
    const std::string path =
        "retained_bundle.provenance.output_records[" +
        std::to_string(index) + "]";
    const auto& output = require_object(outputs[index], path);
    require_exact_fields(
        output, {"product_id", "product_kind", "content_digest"}, path);
    const auto& expected = *expected_outputs[index];
    const core::ContentDigest digest =
        detail::retained_product_output_digest(expected);
    if (require_string(require_member(output, "product_id", path), path) !=
            expected.product_id.value() ||
        require_string(require_member(output, "product_kind", path), path) !=
            core::to_string(expected.product_kind) ||
        require_string(require_member(output, "content_digest", path), path) !=
            digest.value()) {
      detail::fail_retained_bundle(
          "retained output record does not match its complete product "
          "projection");
    }
    output_records.push_back(core::RetainedOutputRecordDTO{
        expected.product_id, expected.product_kind, digest});
  }
  return core::RetainedBundleDTO{
      std::string(core::kRetainedBundleApiVersion),
      core::RetainedBundleId(retained_bundle_id),
      core::DeploymentArtifactId(deployment_artifact_id),
      core::DeploymentRevision(deployment_revision),
      verified_graph.graph(),
      core::RetainedCatalogDTO{
          dialect_api_version,
          core::ContentDigest(bootstrap_contract_digest),
          core::ContentDigest(qualification_registry_digest),
          catalog_records,
          schema_artifacts,
          core::RetainedNodeEnvironmentDTO{
              require_string(
                  require_member(node_environment, "catalog_id",
                                 "catalogs.node_environment"),
                  "catalogs.node_environment.catalog_id"),
              require_string(
                  require_member(node_environment, "content_version",
                                 "catalogs.node_environment"),
                  "catalogs.node_environment.content_version"),
              core::ContentDigest(require_string(
                  require_member(node_environment, "catalog_digest",
                                 "catalogs.node_environment"),
                  "catalogs.node_environment.catalog_digest")),
              core::ContentDigest(require_string(
                  require_member(node_environment, "environment_digest",
                                 "catalogs.node_environment"),
                  "catalogs.node_environment.environment_digest")),
          },
      },
      applicability.value().desired_control_configurations,
      applicability.value().capability_provider_bindings,
      core::RetainedCompiledProductsDTO{
          planning,
          std::nullopt,
          {},
          symbol_role_maps,
          {},
      },
      core::RetainedBundleProvenanceDTO{
          core::ContentDigest(compiler_contract_digest),
          core::ContentDigest(compiler_build_digest),
          source_records,
          generator_records,
          output_records,
      },
  };
}

}  // namespace

namespace detail {

std::string_view flattened_model_product_contract_id() noexcept {
  return kFlattenedModelContractId;
}

const core::ProductContractRevision&
flattened_model_product_contract_revision() {
  return qualified_product_contract_state(
             kFlattenedModelContractId,
             core::RetainedProductKind::kPlanningCollisionModel,
             kFlattenedModelPlacement, kFlattenedModelArtifactRoles)
      .revision;
}

std::span<const QualifiedRetainedProductArtifactRole>
flattened_model_product_artifact_roles() noexcept {
  return kFlattenedModelArtifactRoles;
}

const CanonicalValue& flattened_model_product_contract_descriptor() {
  return qualified_product_contract_state(
             kFlattenedModelContractId,
             core::RetainedProductKind::kPlanningCollisionModel,
             kFlattenedModelPlacement, kFlattenedModelArtifactRoles)
      .descriptor;
}

std::string_view
composed_model_symbol_role_map_product_contract_id() noexcept {
  return kComposedModelSymbolRoleMapContractId;
}

const core::ProductContractRevision&
composed_model_symbol_role_map_product_contract_revision() {
  return qualified_product_contract_state(
             kComposedModelSymbolRoleMapContractId,
             core::RetainedProductKind::kSymbolRoleMap,
             kSymbolRoleMapPlacement,
             kComposedModelSymbolRoleMapArtifactRoles)
      .revision;
}

std::span<const QualifiedRetainedProductArtifactRole>
composed_model_symbol_role_map_product_artifact_roles() noexcept {
  return kComposedModelSymbolRoleMapArtifactRoles;
}

const CanonicalValue&
composed_model_symbol_role_map_product_contract_descriptor() {
  return qualified_product_contract_state(
             kComposedModelSymbolRoleMapContractId,
             core::RetainedProductKind::kSymbolRoleMap,
             kSymbolRoleMapPlacement,
             kComposedModelSymbolRoleMapArtifactRoles)
      .descriptor;
}

[[noreturn]] void fail_retained_bundle(std::string_view message) {
  throw RetainedBundleError(std::string(message));
}

void require_retained_portable_identity(std::string_view value,
                                        std::string_view field) {
  if (!is_portable_identity(value)) {
    fail_retained_bundle(
        std::string(field) +
        " must use the portable 1..128-byte identity grammar");
  }
}

void require_retained_digest(std::string_view value, std::string_view field) {
  if (!is_sha256_digest(value)) {
    fail_retained_bundle(
        std::string(field) +
        " must be an exact lowercase sha256:<64 hex> digest");
  }
}

std::string retained_store_key_for_digest(std::string_view digest) {
  require_retained_digest(digest, "content_digest");
  return std::string(kStoreKeyPrefix) +
         std::string(digest.substr(kDigestPrefix.size()));
}

std::optional<std::string_view>
retained_digest_hex(std::string_view digest) noexcept {
  if (!is_sha256_digest(digest)) {
    return std::nullopt;
  }
  return digest.substr(kDigestPrefix.size());
}

bool is_retained_digest_hex(std::string_view hex) noexcept {
  return hex.size() == kDigestHexLength && is_lower_hex(hex);
}

std::string retained_digest_from_hex(std::string_view hex) {
  if (!is_retained_digest_hex(hex)) {
    fail_retained_bundle(
        "durable retention entry has an invalid digest filename");
  }
  return std::string(kDigestPrefix) + std::string(hex);
}

std::optional<std::string_view> retained_raw_key_hex(
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& expected_digest) noexcept {
  const auto hex = retained_digest_hex(expected_digest.value());
  if (!hex.has_value() ||
      key.value() != std::string(kStoreKeyPrefix) + std::string(*hex)) {
    return std::nullopt;
  }
  return hex;
}

void require_current_retained_product_conformance(
    const core::RetainedProductDTO& product) {
  require_retained_portable_identity(product.product_id.value(), "product_id");
  std::span<const QualifiedRetainedProductArtifactRole> artifact_roles;
  if (product.product_contract_id.value() == kFlattenedModelContractId &&
      product.product_contract_revision ==
          flattened_model_product_contract_revision() &&
      product.product_kind ==
          core::RetainedProductKind::kPlanningCollisionModel) {
    artifact_roles = kFlattenedModelArtifactRoles;
  } else if (
      product.product_contract_id.value() ==
          kComposedModelSymbolRoleMapContractId &&
      product.product_contract_revision ==
          composed_model_symbol_role_map_product_contract_revision() &&
      product.product_kind == core::RetainedProductKind::kSymbolRoleMap) {
    artifact_roles = kComposedModelSymbolRoleMapArtifactRoles;
  } else {
    fail_retained_bundle(
        "retained product references an unqualified contract");
  }
  if (product.artifact_refs.size() > artifact_roles.size()) {
    fail_retained_bundle(
        "retained product contains more artifacts than its contract admits");
  }

  std::set<std::string> present_roles;
  std::string previous_role;
  for (const auto& artifact : product.artifact_refs) {
    require_retained_portable_identity(artifact.artifact_role_id.value(),
                                       "artifact_role_id");
    if (!previous_role.empty() &&
        previous_role >= artifact.artifact_role_id.value()) {
      fail_retained_bundle(
          "retained artifact roles must be strictly canonical-sorted");
    }
    previous_role = artifact.artifact_role_id.value();
    const auto expected = std::find_if(
        artifact_roles.begin(), artifact_roles.end(),
        [&artifact](const QualifiedRetainedProductArtifactRole& role) {
          return role.artifact_role_id == artifact.artifact_role_id.value();
        });
    if (expected == artifact_roles.end()) {
      fail_retained_bundle(
          "retained product contains an artifact role outside its contract");
    }
    if (artifact.media_type != expected->media_type) {
      fail_retained_bundle(
          "retained artifact media type does not match its contract");
    }
    require_retained_digest(artifact.content_digest.value(),
                            "content_digest");
    if (artifact.content_addressed_store_key.value() !=
        retained_store_key_for_digest(artifact.content_digest.value())) {
      fail_retained_bundle(
          "retained artifact store key is not digest-derived");
    }
    if (artifact.byte_size > detail::kMaximumIJsonInteger) {
      fail_retained_bundle(
          "retained artifact byte size exceeds the I-JSON safe integer");
    }
    present_roles.insert(artifact.artifact_role_id.value());
  }
  for (const auto& expected : artifact_roles) {
    if (expected.required &&
        !present_roles.contains(std::string(expected.artifact_role_id))) {
      fail_retained_bundle(
          "retained product is missing a required artifact role");
    }
  }
}

CanonicalValue
retained_product_projection(const core::RetainedProductDTO& product) {
  require_current_retained_product_conformance(product);
  CanonicalValue::Array artifacts;
  artifacts.reserve(product.artifact_refs.size());
  for (const auto& artifact : product.artifact_refs) {
    artifacts.push_back(retained_artifact_projection(artifact));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"product_id", CanonicalValue(product.product_id.value())},
      {"product_kind",
       CanonicalValue(std::string(core::to_string(product.product_kind)))},
      {"product_contract_id",
       CanonicalValue(product.product_contract_id.value())},
      {"product_contract_revision",
       CanonicalValue(product.product_contract_revision.value())},
      {"artifact_refs", CanonicalValue(std::move(artifacts))},
  });
}

core::ContentDigest
retained_product_output_digest(const core::RetainedProductDTO& product) {
  return core::ContentDigest(domain_separated_canonical_digest(
      kRetainedProductOutputDomain, kRetainedProductSchemaTag,
      retained_product_projection(product)));
}

std::shared_ptr<const RetainedBundle>
RetainedBundleConstructionAuthority::construct(
    core::RetainedBundleId retained_bundle_id,
    core::RetainedBundleDTO value, CanonicalValue normalized_root,
    std::string canonical_bytes) {
  return std::shared_ptr<const RetainedBundle>(new RetainedBundle(
      std::move(retained_bundle_id), std::move(value),
      std::move(normalized_root), std::move(canonical_bytes)));
}

}  // namespace detail

bool verify_retained_bundle_identity(const RetainedBundle& bundle) {
  try {
    const core::RetainedBundleDTO parsed =
        verify_current_retained_bundle_projection(bundle.normalized_root());
    const CanonicalValue* declared_id =
        bundle.normalized_root().find_object_member("retained_bundle_id");
    const auto* declared_id_string =
        declared_id == nullptr
            ? nullptr
            : std::get_if<std::string>(&declared_id->storage());
    if (parsed != bundle.value() ||
        bundle.value().retained_bundle_id != bundle.retained_bundle_id() ||
        declared_id_string == nullptr ||
        *declared_id_string != bundle.retained_bundle_id().value() ||
        bundle.canonical_bytes() != canonical_json(bundle.normalized_root()) ||
        !detail::verify_content_identity(
            bundle.normalized_root(),
            detail::ContentIdentitySpec{
                .identity_domain = "retained-bundle-id",
                .api_version = std::string(core::kRetainedBundleApiVersion),
                .own_field = "retained_bundle_id",
            })) {
      return false;
    }
    return true;
  } catch (const std::exception&) {
    return false;
  }
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleReloadAuthority::reload(
    std::string_view canonical_bytes) {
  try {
    const ParsedArtifactCandidate parsed =
        parse_strict_json_model_yaml(canonical_bytes);
    CanonicalValue root = parsed.root();
    if (canonical_json(root) != canonical_bytes) {
      fail_retained_bundle(
          "retained-bundle repository bytes are not RFC 8785 canonical");
    }
    core::RetainedBundleDTO value =
        verify_current_retained_bundle_projection(root);
    const core::RetainedBundleId retained_bundle_id =
        value.retained_bundle_id;
    auto bundle = RetainedBundleConstructionAuthority::construct(
        retained_bundle_id, std::move(value), std::move(root),
        std::string(canonical_bytes));
    if (!verify_retained_bundle_identity(*bundle)) {
      fail_retained_bundle(
          "retained-bundle repository bytes have an invalid semantic "
          "identity");
    }
    return bundle;
  } catch (const CanonicalValueError& error) {
    fail_retained_bundle(
        std::string("retained-bundle repository bytes are invalid: ") +
        error.what());
  } catch (const ArtifactCodecError& error) {
    fail_retained_bundle(
        std::string("retained-bundle repository bytes are invalid: ") +
        error.what());
  } catch (const CellGraphCodecError& error) {
    fail_retained_bundle(
        std::string("retained-bundle repository bytes are invalid: ") +
        error.what());
  }
}

}  // namespace manyforge::deployment
