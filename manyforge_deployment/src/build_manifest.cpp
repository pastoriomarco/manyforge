#include "manyforge_deployment/build_manifest.hpp"

#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/component_type.hpp>
#include <manyforge_core/types/control_interface_contract.hpp>
#include <manyforge_core/types/controller_topology.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <manyforge_core/types/retained_product.hpp>
#include <manyforge_core/types/runtime_profile.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_core/types/stop_control.hpp>
#include <manyforge_deployment/apply_fence_qualification.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <utf8proc.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "build_manifest_internal.hpp"
#include "compiler_semantic_limits_internal.hpp"
#include "content_identity_internal.hpp"
#include "embedded_build_evidence.hpp"
#include "model_source_contract_internal.hpp"
#include "mf2_joint_name_internal.hpp"
#include "posix_durability_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kCompilerContractIdentityDomain =
    "compiler-contract-digest";
constexpr std::string_view kCompilerBuildIdentityDomain =
    "compiler-build-digest";
constexpr std::string_view kNodeEnvironmentIdentityDomain =
    "node-environment-digest";
constexpr std::string_view kExecutablePackageApiVersion =
    "manyforge.executable_package.v1";
constexpr std::string_view kExecutablePackageIdentityDomain =
    "executable-package-content";
constexpr std::string_view kBuiltinCatalogApiVersion =
    "manyforge.node_catalog.v2";
constexpr std::string_view kBuiltinCatalogId = "manyforge_builtin";
constexpr std::string_view kBuiltinCatalogContentVersion = "1.1.0";
constexpr std::string_view kPythonSourcePackageFormat =
    "manyforge.python-source-package.v1";
constexpr std::string_view kCompositionNamingScheme =
    "mf1-non-joint+mf2-dependency-ordered-joint";
constexpr std::string_view kCompositionNamePreimagePrefixes =
    "non-joint=manyforge.occurrence-name.mf1;"
    "joint=manyforge.occurrence-joint-name.mf2";
constexpr std::string_view kCompositionGeneratedNamePrefixes =
    "non-joint=mf1_;joint=mf2_joint_";
constexpr std::string_view kMf2JointNameTokenFormat =
    "mf2_joint_<parent-local-rank:zero-padded-decimal-6>_"
    "<semantic-identity-sha256:lowerhex-64>";
constexpr std::size_t kMf2JointParentLocalRankDecimalBase = 10U;
constexpr std::string_view kMf2JointSourceTieBreak =
    "utf8-bytewise-source-joint-name";
constexpr std::string_view kMf2JointMimicProjectionPolicy =
    "master-before-follower-at-first-divergent-siblings-under-"
    "lowest-common-ancestor;ancestor-master=already-safe;"
    "descendant-master=reject";
constexpr std::string_view kMf2JointSiblingOrderingPolicy =
    "deterministic-kahn-topological-sort;"
    "ready-tie-break=utf8-bytewise-source-joint-name;"
    "projected-cycle=reject";
constexpr std::string_view kMf2FixedConnectionOrderingPolicy =
    "after-all-source-children;"
    "then=utf8-bytewise-model-instance-id";
constexpr std::string_view kMf2JointHashFieldEncoding =
    "domain-utf8;then-each-field=NUL+label+'='+"
    "decimal-utf8-byte-length+':'+raw-utf8-value";
constexpr std::string_view kMf2SourceJointHashFields =
    "cell_id,model_instance_id,model_asset_id,model_revision,"
    "kind,scope,source";
constexpr std::string_view kMf2FixedConnectionHashFields =
    "cell_id,model_instance_id,kind,parent_link,child_link";
constexpr std::string_view kOccurrenceSymbolRewriteKeyFields =
    "model_instance_id,kind,scope_name,source_name";
constexpr std::string_view kCellFramePolicy =
    "one-compiler-owned-cell-frame";
constexpr std::string_view kRewritePolicy =
    "total-forward-reverse-reference-rewrite";
constexpr std::string_view kFramePathPolicy =
    "cell-rooted-portable-frame-paths";
[[noreturn]] void fail(std::string_view message) {
  throw BuildManifestError(std::string(message));
}

[[nodiscard]] std::exception_ptr make_manifest_error(
    std::string_view message) {
  return std::make_exception_ptr(BuildManifestError(std::string(message)));
}

namespace posix = detail::posix_durability;

const posix::ErrorPolicy kReadOnlyManifestPosixErrors{
    make_manifest_error};
constexpr posix::TrustedRootMessages kReadOnlyManifestRootMessages{
    "canonical package root must not be empty",
    "cannot resolve canonical package root",
    "canonical package root must be an existing plain directory",
};
constexpr posix::ExistingDirectoryMessages
    kReadOnlyManifestDirectoryMessages{
        "cannot inspect canonical package manifest directory",
        "canonical package manifest directory must be an existing plain "
        "directory",
    };
constexpr posix::ReadMessages kReadOnlyManifestReadMessages{
    "cannot open canonical package build manifest",
    "cannot stat canonical package build manifest",
    "canonical package build manifest violates its immutable byte bound",
    "canonical package build manifest changed during exact read",
    "canonical package build manifest grew during exact read",
};

[[nodiscard]] std::filesystem::path existing_manifest_path(
    const std::filesystem::path& manifest_root,
    const BuildManifestKey& key) {
  const std::string_view kind = to_string(key.manifest_kind);
  if (kind.empty() || !is_sha256_digest(key.content_digest.value())) {
    fail("current build produced an invalid build-manifest key");
  }
  const std::filesystem::path kind_root =
      manifest_root / std::string(kind);
  posix::require_existing_plain_directory(
      kind_root, kReadOnlyManifestPosixErrors,
      kReadOnlyManifestDirectoryMessages);
  return kind_root /
         (key.content_digest.value().substr(
              std::string_view("sha256:").size()) +
          ".json");
}

[[nodiscard]] const CanonicalValue::Object& require_object(
    const CanonicalValue& value, std::string_view field) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(
    const CanonicalValue& value, std::string_view field,
    std::size_t maximum, bool allow_empty = true) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(std::string(field) + " must be an array");
  }
  if ((!allow_empty && array->empty()) || array->size() > maximum) {
    fail(std::string(field) + " violates its closed cardinality");
  }
  return *array;
}

[[nodiscard]] const CanonicalValue& require_member(
    const CanonicalValue::Object& object, std::string_view name,
    std::string_view field) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  if (found == object.end()) {
    fail(std::string(field) + " is missing required member " +
         std::string(name));
  }
  return found->second;
}

[[nodiscard]] const CanonicalValue* find_member(
    const CanonicalValue::Object& object, std::string_view name) {
  const auto found = std::find_if(
      object.begin(), object.end(),
      [name](const auto& member) { return member.first == name; });
  return found == object.end() ? nullptr : &found->second;
}

void require_exact_fields(
    const CanonicalValue::Object& object,
    std::initializer_list<std::string_view> expected,
    std::string_view field) {
  if (object.size() != expected.size()) {
    fail(std::string(field) + " has an incorrect closed field count");
  }
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) ==
        expected.end()) {
      fail(std::string(field) + " contains unknown member " + name);
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, field);
  }
}

[[nodiscard]] const std::string& require_text(
    const CanonicalValue& value, std::string_view field) {
  const auto* text = std::get_if<std::string>(&value.storage());
  if (text == nullptr || text->empty()) {
    fail(std::string(field) +
         " must be non-empty bounded text without control characters");
  }
  std::size_t offset = 0U;
  std::size_t code_points = 0U;
  while (offset < text->size()) {
    utf8proc_int32_t code_point = 0;
    const auto consumed = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(text->data()) + offset,
        static_cast<utf8proc_ssize_t>(text->size() - offset),
        &code_point);
    if (consumed <= 0 ||
        utf8proc_category(code_point) == UTF8PROC_CATEGORY_CC ||
        ++code_points > detail::kMaximumManifestTextCodePoints) {
      fail(std::string(field) +
           " must be non-empty bounded text without control characters");
    }
    offset += static_cast<std::size_t>(consumed);
  }
  return *text;
}

[[nodiscard]] bool is_portable_identifier(std::string_view value) noexcept {
  if (value.empty() ||
      !(value.front() >= 'a' && value.front() <= 'z')) {
    return false;
  }
  return std::all_of(
      value.begin() + 1, value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '_' ||
               byte == '.' || byte == '-';
      });
}

[[nodiscard]] const std::string& require_identifier(
    const CanonicalValue& value, std::string_view field) {
  const std::string& text = require_text(value, field);
  if (!is_portable_identifier(text)) {
    fail(std::string(field) +
         " must use the stable lowercase portable-ID grammar");
  }
  return text;
}

[[nodiscard]] bool is_logical_build_input_id(
    std::string_view value) noexcept {
  if (value.empty() ||
      !(value.front() >= 'a' && value.front() <= 'z')) {
    return false;
  }
  return std::all_of(
      value.begin() + 1, value.end(), [](unsigned char byte) {
        return (byte >= 'a' && byte <= 'z') ||
               (byte >= '0' && byte <= '9') || byte == '_' ||
               byte == '.' || byte == ':' || byte == '-';
      });
}

[[nodiscard]] const std::string& require_digest(
    const CanonicalValue& value, std::string_view field) {
  const std::string& text = require_text(value, field);
  if (!is_sha256_digest(text)) {
    fail(std::string(field) +
         " must be an exact lowercase SHA-256 digest");
  }
  return text;
}

[[nodiscard]] bool is_source_id(std::string_view value) noexcept {
  constexpr std::string_view kModulePrefix = "python-module:";
  if (value.starts_with(kModulePrefix)) {
    const std::string_view module = value.substr(kModulePrefix.size());
    if (module.empty() ||
        !(module.front() >= 'a' && module.front() <= 'z')) {
      return false;
    }
    return std::all_of(
        module.begin() + 1, module.end(), [](unsigned char byte) {
          return (byte >= 'a' && byte <= 'z') ||
                 (byte >= '0' && byte <= '9') || byte == '_' ||
                 byte == '.';
        });
  }
  return value == "python-project:manyforge_behavior";
}

[[nodiscard]] CanonicalValue with_replaced_member(
    const CanonicalValue& value, std::string_view member_name,
    CanonicalValue replacement) {
  CanonicalValue::Object result;
  result.reserve(value.as_object().size());
  bool replaced = false;
  for (const auto& [name, member] : value.as_object()) {
    if (name == member_name) {
      result.emplace_back(name, std::move(replacement));
      replaced = true;
    } else {
      result.emplace_back(name, member);
    }
  }
  if (!replaced) {
    throw std::logic_error("build manifest own field is absent");
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] detail::ContentIdentitySpec identity_spec(
    BuildManifestKind kind) {
  switch (kind) {
    case BuildManifestKind::kNodeEnvironment:
      return {
          std::string(kNodeEnvironmentIdentityDomain),
          std::string(detail::kNodeEnvironmentManifestApiVersion),
          "environment_digest",
      };
    case BuildManifestKind::kCompilerContract:
      return {
          std::string(kCompilerContractIdentityDomain),
          std::string(detail::kCompilerContractManifestApiVersion),
          "compiler_contract_digest",
      };
    case BuildManifestKind::kCompilerBuild:
      return {
          std::string(kCompilerBuildIdentityDomain),
          std::string(detail::kCompilerBuildManifestApiVersion),
          "compiler_build_digest",
      };
  }
  throw std::logic_error("unknown build manifest kind");
}

[[nodiscard]] std::string_view own_digest_field(
    BuildManifestKind kind) noexcept {
  switch (kind) {
    case BuildManifestKind::kNodeEnvironment:
      return "environment_digest";
    case BuildManifestKind::kCompilerContract:
      return "compiler_contract_digest";
    case BuildManifestKind::kCompilerBuild:
      return "compiler_build_digest";
  }
  return {};
}

[[nodiscard]] CanonicalValue finalize_identity(
    CanonicalValue placeholder, BuildManifestKind kind) {
  const std::string digest =
      detail::compute_content_identity(placeholder, identity_spec(kind));
  CanonicalValue result = with_replaced_member(
      placeholder, own_digest_field(kind), CanonicalValue(digest));
  if (!detail::verify_content_identity(result, identity_spec(kind))) {
    throw std::logic_error(
        "build manifest producer minted an invalid content identity");
  }
  return result;
}

[[nodiscard]] std::string declared_digest(
    const CanonicalValue& value, BuildManifestKind kind) {
  return require_digest(
      require_member(value.as_object(), own_digest_field(kind), "manifest"),
      own_digest_field(kind));
}

[[nodiscard]] CanonicalValue::Array sorted_text_values(
    std::initializer_list<std::string_view> values) {
  std::vector<std::string> sorted;
  sorted.reserve(values.size());
  for (const auto value : values) {
    sorted.emplace_back(value);
  }
  std::sort(sorted.begin(), sorted.end());
  if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
    throw std::logic_error("built-in manifest text table is not unique");
  }
  CanonicalValue::Array result;
  result.reserve(sorted.size());
  for (auto& value : sorted) {
    result.emplace_back(std::move(value));
  }
  return result;
}

[[nodiscard]] CanonicalValue source_media_contract() {
  CanonicalValue::Array values;
  values.reserve(detail::kModelSourceMediaContractRows.size());
  for (const auto& row : detail::kModelSourceMediaContractRows) {
    values.emplace_back(CanonicalValue::Object{
        {"artifact_kind",
         CanonicalValue(std::string(row.artifact_kind_id))},
        {"media_type", CanonicalValue(std::string(row.media_type))},
        {"source_role", CanonicalValue(std::string(row.source_role))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_rows", CanonicalValue(std::move(values))},
      {"store_key_contract", CanonicalValue("sha256/<64-lowercase-hex>")},
      {"uri_contract",
       CanonicalValue("closure-local-logical-uri-no-resolution")},
  });
}

struct LimitRow final {
  std::string_view id;
  std::uint64_t value;
};

[[nodiscard]] CanonicalValue resource_limits() {
  std::vector<LimitRow> rows{
      {"artifact.encoded_bytes", kMaxArtifactEncodedBytes},
      {"artifact.nesting_depth", kMaxArtifactNestingDepth},
      {"artifact.node_count", kMaxArtifactNodeCount},
      {"artifact.safe_integer",
       static_cast<std::uint64_t>(kMaxArtifactSafeInteger)},
      {"artifact.scalar_bytes", kMaxArtifactScalarBytes},
      {"build_manifest.compiler_build_rows",
       detail::kMaximumCompilerBuildRows},
      {"build_manifest.compiler_contract_rows",
       detail::kMaximumCompilerContractRows},
      {"build_manifest.executable_package_rows",
       detail::kMaximumExecutablePackageRows},
      {"build_manifest.executable_registry_rows",
       detail::kMaximumExecutableRegistryRows},
      {"build_manifest.manifest_text_code_points",
       detail::kMaximumManifestTextCodePoints},
      {"build_manifest.source_coverage_rows",
       detail::kMaximumSourceCoverageRows},
      {"cell_applicability.binding_claims_per_direction",
       kMaxExpandedControlInterfaceClaimsPerDirection},
      {"cell_applicability.cell_claims_per_direction",
       kMaxCellExpandedControlInterfaceClaimsPerDirection},
      {"cell_applicability.logical_rows",
       kMaxCellLogicalApplicabilityRows},
      {"cell_composition.coordinate_projections",
       kMaxCompositionCoordinateProjections},
      {"cell_composition.flattened_xml_bytes",
       kMaxCompositionFlattenedXmlBytes},
      {"cell_composition.frame_resolutions",
       kMaxCompositionFrameResolutions},
      {"cell_composition.path_segments", kMaxCompositionPathSegments},
      {"cell_composition.references", kMaxCompositionReferences},
      {"cell_composition.symbols", kMaxCompositionSymbols},
      {"cell_composition.target_projection_rows",
       kMaxCompositionTargetProjectionRows},
      {"cell_graph.portable_identity_bytes",
       detail::kMaximumPortableIdentityBytes},
      {"cell_graph.quaternion_norm_squared_tolerance_parts_per_trillion",
       detail::kQuaternionNormSquaredTolerancePartsPerTrillion},
      {"cell_graph.qualified_apply_fence_control_bindings",
       kMaximumQualifiedApplyFenceControlBindingsPerCell},
      {"cell_graph.qualified_apply_fence_control_domains",
       kMaximumQualifiedApplyFenceControlDomainsPerCell},
      {"cell_graph.qualified_apply_fence_controllable_components",
       kMaximumQualifiedApplyFenceControllableComponentsPerCell},
      {"cell_graph.qualified_apply_fence_resources",
       kMaximumQualifiedApplyFenceResourcesPerCell},
      {"closed_schema.alternatives", kMaxClosedSchemaAlternatives},
      {"closed_schema.array_items", kMaxClosedSchemaArrayItems},
      {"closed_schema.constraints", kMaxClosedSchemaConstraints},
      {"closed_schema.definitions", kMaxClosedSchemaDefinitions},
      {"closed_schema.enum_values", kMaxClosedSchemaEnumValues},
      {"closed_schema.fields", kMaxClosedSchemaFields},
      {"closed_schema.key_path_segments",
       kMaxClosedSchemaKeyPathSegments},
      {"closed_schema.key_paths", kMaxClosedSchemaKeyPaths},
      {"closed_schema.normalized_aggregate_scalar_bytes",
       kMaxNormalizedAggregateScalarBytes},
      {"closed_schema.string_code_points",
       kMaxClosedSchemaStringCodePoints},
      {"component_type.attachment_interfaces",
       core::kMaxComponentTypeAttachmentInterfaces},
      {"component_type.attachments_per_interface",
       core::kMaxComponentTypeAttachmentsPerInterface},
      {"component_type.capability_dependencies",
       core::kMaxComponentTypeCapabilityDependencies},
      {"component_type.command_targets",
       core::kMaxComponentTypeCommandTargets},
      {"component_type.model_mounts", core::kMaxComponentTypeModelMounts},
      {"component_type.model_roles", core::kMaxComponentTypeModelRoles},
      {"component_type.tags", core::kMaxComponentTypeTags},
      {"component_type.target_capabilities",
       core::kMaxComponentTypeTargetCapabilities},
      {"component_type.target_role_aliases",
       core::kMaxComponentTypeTargetRoleAliases},
      {"control_interface.items", core::kMaxControlInterfaceContractItems},
      {"controller_topology.active_chain_edges_per_activation_set",
       core::kMaxControllerTopologyActiveChainEdgesPerActivationSet},
      {"controller_topology.active_roles_per_activation_set",
       core::kMaxControllerTopologyActiveControllerRolesPerActivationSet},
      {"controller_topology.activation_sets",
       core::kMaxControllerTopologyActivationSets},
      {"controller_topology.chain_edges",
       core::kMaxControllerTopologyChainEdges},
      {"controller_topology.claimed_directions_per_claim",
       core::kMaxControllerTopologyClaimedDirectionsPerClaim},
      {"controller_topology.compatible_activation_sets_per_route",
       core::kMaxControllerTopologyCompatibleActivationSetsPerRoute},
      {"controller_topology.control_interface_claims_per_controller",
       core::kMaxControllerTopologyControlInterfaceClaimsPerController},
      {"controller_topology.control_routes",
       core::kMaxControllerTopologyControlRoutes},
      {"controller_topology.controllers",
       core::kMaxControllerTopologyControllers},
      {"controller_topology.distinct_control_interface_contracts",
       core::kMaxControllerTopologyDistinctControlInterfaceContracts},
      {"controller_topology.exported_reference_interfaces_per_controller",
       core::kMaxControllerTopologyExportedReferenceInterfacesPerController},
      {"controller_topology.exported_state_interfaces_per_controller",
       core::kMaxControllerTopologyExportedStateInterfacesPerController},
      {"controller_topology.ordered_roles_per_route",
       core::kMaxControllerTopologyOrderedControllerRolesPerRoute},
      {"controller_topology.reference_interfaces_per_chain_edge",
       core::kMaxControllerTopologyReferenceInterfacesPerChainEdge},
      {"controller_topology.state_interfaces_per_chain_edge",
       core::kMaxControllerTopologyStateInterfacesPerChainEdge},
      {"deployment.catalog_records",
       detail::kMaximumCatalogRecords},
      {"deployment_applicability.endpoint_bindings",
       core::kMaxCapabilityProviderEndpointBindings},
      {"deployment_applicability.rows",
       core::kMaxDeploymentApplicabilityRows},
      {"model_asset.byte_size", core::kMaxModelAssetByteSize},
      {"model_asset.collection_items",
       core::kMaxModelAssetCollectionItems},
      {"model_asset.logical_uri_bytes",
       core::kMaxModelAssetLogicalUriBytes},
      {"model_asset.media_type_bytes",
       core::kMaxModelAssetMediaTypeBytes},
      {"model_asset.source_symbol_code_points",
       core::kMaxModelAssetSourceSymbolCodePoints},
      {"model_source.closure_bytes", kMaxModelSourceClosureBytes},
      {"model_source.collada_attribute_count",
       kMaxModelSourceColladaXmlAttributeCount},
      {"model_source.collada_depth", kMaxModelSourceColladaXmlDepth},
      {"model_source.collada_element_count",
       kMaxModelSourceColladaXmlElementCount},
      {"model_source.expanded_srdf_semantic_rows",
       kMaxModelSourceExpandedSrdfSemanticRows},
      {"model_source.opaque_artifact_bytes",
       kMaxModelSourceOpaqueArtifactBytes},
      {"model_source.xml_attribute_count",
       kMaxModelSourceXmlAttributeCount},
      {"model_source.xml_depth", kMaxModelSourceXmlDepth},
      {"model_source.xml_element_count", kMaxModelSourceXmlElementCount},
      {"model_source.xml_entrypoint_bytes",
       kMaxModelSourceXmlEntrypointBytes},
      {"provider_manifest.capabilities",
       core::kMaxProviderManifestCapabilities},
      {"provider_manifest.contracts", core::kMaxProviderManifestContracts},
      {"provider_manifest.endpoint_roles",
       core::kMaxProviderManifestEndpointRoles},
      {"runtime_profile.role_ids", core::kMaxRuntimeProfileRoleIds},
      {"retained_bundle.portable_identity_bytes",
       detail::kMaximumPortableIdentityBytes},
      {"site_overlay.value_rows", core::kMaxSiteOverlayValueRows},
      {"stop_evidence.duration_ns",
       static_cast<std::uint64_t>(
           core::kMaxStopEvidencePolicyDurationNs)},
      {"stop_evidence.items", core::kMaxStopEvidencePolicyItems},
      {"stop_evidence.stages", core::kMaxStopEscalationStages},
  };
  std::sort(rows.begin(), rows.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.id < rhs.id;
            });
  if (rows.size() > detail::kMaximumCompilerContractRows ||
      std::adjacent_find(rows.begin(), rows.end(),
                         [](const auto& lhs, const auto& rhs) {
                           return lhs.id == rhs.id;
                         }) != rows.end()) {
    throw std::logic_error(
        "compiler resource-limit registry is unbounded or ambiguous");
  }
  CanonicalValue::Array values;
  values.reserve(rows.size());
  for (const auto& row : rows) {
    if (row.value > detail::kMaximumIJsonInteger) {
      throw std::logic_error(
          "compiler resource limit is outside the I-JSON safe range");
    }
    values.emplace_back(CanonicalValue::Object{
        {"limit_id", CanonicalValue(std::string(row.id))},
        {"value", CanonicalValue(static_cast<double>(row.value))},
    });
  }
  return CanonicalValue(std::move(values));
}

struct IdentityContractRow final {
  std::string_view identity_domain;
  std::string_view schema_api_tag;
  std::string_view omitted_field;
  std::string_view root_shape;
};

[[nodiscard]] CanonicalValue content_identity_contracts() {
  std::vector<IdentityContractRow> rows{
      {"capability-provider-binding-revision", "manyforge.deployment.v1",
       "capability_provider_binding_revision", "nested_record"},
      {"catalog-record-revision/capability_definition",
       core::kCapabilityDefinitionApiVersion,
       "capability_definition_revision", "document"},
      {"catalog-record-revision/component_type",
       core::kComponentTypeApiVersion, "component_type_revision",
       "document"},
      {"catalog-record-revision/control_interface_contract",
       core::kControlInterfaceContractApiVersion,
       "control_interface_revision", "document"},
      {"catalog-record-revision/controller_topology",
       core::kControllerTopologyApiVersion, "topology_revision",
       "document"},
      {"catalog-record-revision/model_asset", core::kModelAssetApiVersion,
       "model_revision", "document"},
      {"catalog-record-revision/provider_manifest",
       core::kProviderManifestApiVersion, "provider_manifest_revision",
       "document"},
      {"catalog-record-revision/runtime_profile",
       core::kRuntimeProfileApiVersion, "runtime_profile_revision",
       "document"},
      {"catalog-record-revision/stop_evidence_policy",
       core::kStopEvidencePolicyApiVersion,
       "stop_evidence_policy_revision", "document"},
      {"compiler-build-digest", detail::kCompilerBuildManifestApiVersion,
       "compiler_build_digest", "document"},
      {"compiler-contract-digest",
       detail::kCompilerContractManifestApiVersion,
       "compiler_contract_digest", "document"},
      {"component-attachment-revision", core::kCellGraphApiVersion,
       "component_attachment_revision", "nested_record"},
      {"control-binding-revision", core::kCellGraphApiVersion,
       "control_binding_revision", "nested_record"},
      {"control-domain-revision", core::kCellGraphApiVersion,
       "control_domain_revision", "nested_record"},
      {"deployment-revision", "manyforge.deployment.v1",
       "deployment_revision", "document"},
      {"graph-revision", core::kCellGraphApiVersion, "graph_revision",
       "document"},
      {"node-environment-digest",
       detail::kNodeEnvironmentManifestApiVersion, "environment_digest",
       "document"},
      {"product-contract-revision", core::kProductContractApiVersion,
       "product_contract_revision", "document"},
      {"retained-bundle-id", core::kRetainedBundleApiVersion,
       "retained_bundle_id", "document"},
      {"retained-product-output",
       "manyforge.retained_bundle.v1#retained_product", "",
       "complete_projection"},
      {"schema-revision", kClosedSchemaApiVersion, "schema_revision",
       "document"},
      {"site-overlay-revision", "manyforge.site_overlay.v1",
       "site_overlay_revision", "document"},
  };
  std::sort(rows.begin(), rows.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.identity_domain < rhs.identity_domain;
            });
  if (rows.size() > detail::kMaximumCompilerContractRows ||
      std::adjacent_find(rows.begin(), rows.end(),
                         [](const auto& lhs, const auto& rhs) {
                           return lhs.identity_domain ==
                                  rhs.identity_domain;
                         }) != rows.end()) {
    throw std::logic_error(
        "compiler content-identity registry is unbounded or ambiguous");
  }
  CanonicalValue::Array values;
  values.reserve(rows.size());
  for (const auto& row : rows) {
    values.emplace_back(CanonicalValue::Object{
        {"identity_domain",
         CanonicalValue(std::string(row.identity_domain))},
        {"schema_api_tag",
         CanonicalValue(std::string(row.schema_api_tag))},
        {"omitted_field",
         row.omitted_field.empty()
             ? CanonicalValue(nullptr)
             : CanonicalValue(std::string(row.omitted_field))},
        {"root_shape", CanonicalValue(std::string(row.root_shape))},
    });
  }
  return CanonicalValue(std::move(values));
}

[[nodiscard]] CanonicalValue product_contract_registry() {
  std::vector<const BuildQualifiedProductContract*> contracts;
  for (const auto& contract :
       build_qualified_product_contract_registry().contracts()) {
    contracts.push_back(&contract);
  }
  std::sort(
      contracts.begin(), contracts.end(), [](const auto* lhs,
                                             const auto* rhs) {
        return std::tie(lhs->product_contract_id().value(),
                        lhs->product_contract_revision().value()) <
               std::tie(rhs->product_contract_id().value(),
                        rhs->product_contract_revision().value());
      });
  if (contracts.empty() ||
      contracts.size() > detail::kMaximumCompilerContractRows) {
    throw std::logic_error(
        "product-contract registry is empty or unbounded");
  }
  CanonicalValue::Array values;
  values.reserve(contracts.size());
  std::string previous;
  for (const auto* contract : contracts) {
    const std::string key = contract->product_contract_id().value() + '\0' +
                            contract->product_contract_revision().value();
    if (!previous.empty() && previous >= key) {
      throw std::logic_error(
          "product-contract registry is not canonical and unique");
    }
    previous = key;
    values.push_back(contract->normalized_descriptor());
  }
  return CanonicalValue(std::move(values));
}

[[nodiscard]] CanonicalValue registered_stop_command_registry() {
  CanonicalValue::Array rows;
  const auto definitions = core::registered_stop_command_definitions();
  rows.reserve(definitions.size());
  for (const auto& definition : definitions) {
    CanonicalValue::Object row{
        {"registered_command_kind_id",
         CanonicalValue(definition.registered_command_kind_id.value())},
        {"stop_action_class",
         CanonicalValue(
             std::string(core::to_string(definition.stop_action_class)))},
    };
    if (definition.safety_system_request_action_class.has_value()) {
      row.emplace_back(
          "safety_system_request_action_class",
          CanonicalValue(std::string(core::to_string(
              *definition.safety_system_request_action_class))));
    }
    rows.emplace_back(std::move(row));
  }
  const CanonicalValue canonical_rows(std::move(rows));
  return CanonicalValue(CanonicalValue::Object{
      {"content_digest",
       CanonicalValue(sha256_digest(canonical_json(canonical_rows)))},
      {"rows", canonical_rows},
  });
}

[[nodiscard]] CanonicalValue compiler_contract_candidate(
    std::string_view own_digest) {
  const auto& schemas = build_qualified_closed_schema_registry();
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(
           std::string(detail::kCompilerContractManifestApiVersion))},
      {"compiler_contract_digest",
       CanonicalValue(std::string(own_digest))},
      {"supported_apis",
       CanonicalValue(sorted_text_values({
           "manyforge.capability_definition.v1",
           "manyforge.cell_graph.v1",
           "manyforge.closed_schema.v1",
           "manyforge.component_type.v1",
           "manyforge.control_interface_contract.v1",
           "manyforge.controller_topology.v1",
           "manyforge.deployment.v1",
           "manyforge.model_asset.v1",
           "manyforge.product_contract.v1",
           "manyforge.provider_manifest.v1",
           "manyforge.reference_runtime_manifest.v1",
           "manyforge.retained_bundle.v1",
           "manyforge.runtime_profile.v1",
           "manyforge.site_overlay.v1",
           "manyforge.stop_evidence_policy.v2",
       }))},
      {"schema_registry",
       CanonicalValue(CanonicalValue::Object{
           {"dialect_api_version",
            CanonicalValue(std::string(kClosedSchemaApiVersion))},
           {"bootstrap_contract_digest",
            CanonicalValue(
                std::string(kClosedSchemaBootstrapContractDigest))},
           {"qualification_registry_digest",
            CanonicalValue(
                schemas.qualification_registry_digest().value())},
           {"normalized_registry", schemas.normalized_projection()},
       })},
      {"product_contract_registry", product_contract_registry()},
      {"registered_stop_command_registry",
       registered_stop_command_registry()},
      {"content_identity_contracts", content_identity_contracts()},
      {"source_media_contract", source_media_contract()},
      {"resource_limits", resource_limits()},
      {"composition_contract",
       CanonicalValue(CanonicalValue::Object{
           {"naming_scheme",
            CanonicalValue(std::string(kCompositionNamingScheme))},
           {"name_preimage_prefix",
            CanonicalValue(std::string(kCompositionNamePreimagePrefixes))},
           {"generated_name_prefix",
            CanonicalValue(std::string(kCompositionGeneratedNamePrefixes))},
           {"joint_name_token_format",
            CanonicalValue(std::string(kMf2JointNameTokenFormat))},
           {"joint_parent_local_rank_decimal_width",
            CanonicalValue(
                static_cast<double>(detail::kMf2JointLocalRankWidth))},
           {"joint_parent_local_rank_decimal_base",
            CanonicalValue(static_cast<double>(
                kMf2JointParentLocalRankDecimalBase))},
           {"joint_parent_local_rank_capacity",
            CanonicalValue(static_cast<double>(
                detail::kMf2JointMaximumLocalRank + 1U))},
           {"joint_source_tie_break",
            CanonicalValue(std::string(kMf2JointSourceTieBreak))},
           {"joint_mimic_projection_policy",
            CanonicalValue(
                std::string(kMf2JointMimicProjectionPolicy))},
           {"joint_sibling_ordering_policy",
            CanonicalValue(
                std::string(kMf2JointSiblingOrderingPolicy))},
           {"fixed_connection_ordering_policy",
            CanonicalValue(
                std::string(kMf2FixedConnectionOrderingPolicy))},
           {"joint_hash_preimage_domain",
            CanonicalValue(
                std::string(detail::kMf2JointHashPreimageDomain))},
           {"joint_hash_field_encoding",
            CanonicalValue(std::string(kMf2JointHashFieldEncoding))},
           {"source_joint_hash_fields",
            CanonicalValue(std::string(kMf2SourceJointHashFields))},
           {"fixed_connection_hash_fields",
            CanonicalValue(std::string(kMf2FixedConnectionHashFields))},
           {"occurrence_symbol_rewrite_key_fields",
            CanonicalValue(
                std::string(kOccurrenceSymbolRewriteKeyFields))},
           {"cell_frame_policy",
            CanonicalValue(std::string(kCellFramePolicy))},
           {"rewrite_policy",
            CanonicalValue(std::string(kRewritePolicy))},
           {"frame_path_policy",
            CanonicalValue(std::string(kFramePathPolicy))},
       })},
      {"accepted_deployment_subset",
       CanonicalValue(CanonicalValue::Object{
           {"deployment_api_version",
            CanonicalValue("manyforge.deployment.v1")},
           {"qualified_catalog_kinds",
            CanonicalValue(sorted_text_values({
                "capability_definition",
                "command_schema",
                "component_type",
                "control_interface_contract",
                "controller_topology",
                "model_asset",
                "provider_manifest",
                "runtime_profile",
                "stop_evidence_policy",
            }))},
           {"unqualified_catalog_kinds",
            CanonicalValue(sorted_text_values({
                "contact_policy",
                "runtime_policy",
            }))},
           {"assistant",
            CanonicalValue("absent-empty-or-closed-audit-prompt-endpoint-provider-mode-"
                           "projection")},
           {"policies",
            CanonicalValue(
                "closed-stop-stage-parameters-and-commissioned-interfaces;"
                "other-policy-tables-empty")},
           {"compatibility", CanonicalValue("closed-lane-provides-tags-metadata")},
           {"i3_retention_subset",
            CanonicalValue(
                "passive-or-one-bounded-controllable-component-or-exact-"
                "reference-runtime")},
       })},
      {"retained_projection_contract",
       CanonicalValue(CanonicalValue::Object{
           {"bundle_api_version",
            CanonicalValue(
                std::string(core::kRetainedBundleApiVersion))},
           {"bundle_identity_domain",
            CanonicalValue("retained-bundle-id")},
           {"product_output_identity_domain",
            CanonicalValue("retained-product-output")},
           {"product_output_schema_tag",
            CanonicalValue(
                "manyforge.retained_bundle.v1#retained_product")},
           {"qualified_product_contract_ids",
            CanonicalValue(sorted_text_values({
                "manyforge.flattened_model",
                "manyforge.composed_model_symbol_role_map",
                "manyforge.control_domain_control",
                "manyforge.reference_runtime_unit",
                "manyforge.viewer_model",
            }))},
           {"qualified_product_placements", CanonicalValue(sorted_text_values({
                                                "cell_planning_collision_model",
                                                "cell_viewer_model",
                                                "control_domain",
                                                "runtime_unit",
                                                "symbol_role_map",
                                            }))},
           {"canonical_empty_artifact_roles",
            CanonicalValue(
                sorted_text_values({"planning_resource_map", "srdf", "urdf"}))},
           {"active_alias_owner", CanonicalValue("none")},
       })},
  });
}

[[nodiscard]] CanonicalValue current_compiler_contract_candidate() {
  return finalize_identity(
      compiler_contract_candidate(kZeroDigest),
      BuildManifestKind::kCompilerContract);
}

[[nodiscard]] CanonicalValue digest_rows(
    std::span<const detail::build_evidence::BuildEvidenceRow> rows) {
  if (rows.empty() ||
      rows.size() > detail::kMaximumCompilerBuildRows) {
    throw std::logic_error(
        "compiler build input closure is empty or unbounded");
  }
  CanonicalValue::Array result;
  result.reserve(rows.size());
  std::string_view previous;
  for (const auto& row : rows) {
    if (!is_logical_build_input_id(row.id) ||
        !is_sha256_digest(row.content_digest) ||
        (!previous.empty() && previous >= row.id) ||
        row.id.find("..") != std::string_view::npos) {
      throw std::logic_error(
          "compiler build input closure is not canonical, complete, and "
          "path-independent");
    }
    previous = row.id;
    result.emplace_back(CanonicalValue::Object{
        {"input_id", CanonicalValue(std::string(row.id))},
        {"content_digest",
         CanonicalValue(std::string(row.content_digest))},
    });
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue dependency_rows() {
  struct Row final {
    std::string_view dependency_id;
    std::string_view dependency_version;
  };
  std::array rows{
      Row{"cmake", detail::build_evidence::kCMakeVersion},
      Row{"double-conversion",
          detail::build_evidence::kDoubleConversionVersion},
      Row{"expat", detail::build_evidence::kExpatVersion},
      Row{"libyaml", detail::build_evidence::kLibYamlVersion},
      Row{"openssl", detail::build_evidence::kOpenSslVersion},
      Row{"utf8proc", detail::build_evidence::kUtf8ProcVersion},
  };
  std::sort(rows.begin(), rows.end(),
            [](const auto& lhs, const auto& rhs) {
              return lhs.dependency_id < rhs.dependency_id;
            });
  CanonicalValue::Array result;
  result.reserve(rows.size());
  std::string_view previous;
  for (const auto& row : rows) {
    if (!is_portable_identifier(row.dependency_id) ||
        row.dependency_version.empty() ||
        (!previous.empty() && previous >= row.dependency_id)) {
      throw std::logic_error(
          "compiler dependency evidence is absent or ambiguous");
    }
    previous = row.dependency_id;
    result.emplace_back(CanonicalValue::Object{
        {"dependency_id",
         CanonicalValue(std::string(row.dependency_id))},
        {"dependency_version",
         CanonicalValue(std::string(row.dependency_version))},
    });
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue compiler_build_candidate(
    std::string_view own_digest,
    std::string_view compiler_contract_digest) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(
           std::string(detail::kCompilerBuildManifestApiVersion))},
      {"compiler_build_digest", CanonicalValue(std::string(own_digest))},
      {"compiler_package",
       CanonicalValue(CanonicalValue::Object{
           {"package_id",
            CanonicalValue(
                std::string(detail::build_evidence::kCompilerPackageId))},
           {"package_version",
            CanonicalValue(std::string(
                detail::build_evidence::kCompilerPackageVersion))},
       })},
      {"compiler_contract_digest",
       CanonicalValue(std::string(compiler_contract_digest))},
      {"build_profile",
       CanonicalValue(
           std::string(detail::build_evidence::kBuildProfile))},
      {"build_configuration_digest",
       CanonicalValue(std::string(
           detail::build_evidence::kBuildConfigurationDigest))},
      {"target",
       CanonicalValue(
           std::string(detail::build_evidence::kBuildTarget))},
      {"source_inputs",
       digest_rows(detail::build_evidence::kCompilerSourceInputs)},
      {"recipe_inputs",
       digest_rows(detail::build_evidence::kCompilerRecipeInputs)},
      {"toolchain",
       CanonicalValue(CanonicalValue::Object{
           {"compiler_id",
            CanonicalValue(
                std::string(detail::build_evidence::kCompilerId))},
           {"compiler_version",
            CanonicalValue(
                std::string(detail::build_evidence::kCompilerVersion))},
           {"cxx_standard",
            CanonicalValue(
                std::string(detail::build_evidence::kCxxStandard))},
       })},
      {"dependencies", dependency_rows()},
  });
}

[[nodiscard]] CanonicalValue current_compiler_build_candidate() {
  const CanonicalValue contract = current_compiler_contract_candidate();
  const std::string contract_digest = declared_digest(
      contract, BuildManifestKind::kCompilerContract);
  return finalize_identity(
      compiler_build_candidate(kZeroDigest, contract_digest),
      BuildManifestKind::kCompilerBuild);
}

[[nodiscard]] CanonicalValue executable_package_projection(
    std::string_view package_id, std::string_view package_version,
    std::string_view package_format,
    const std::vector<std::pair<std::string, std::string>>& sources,
    std::string_view content_digest) {
  CanonicalValue::Array rows;
  rows.reserve(sources.size());
  for (const auto& [source_id, digest] : sources) {
    rows.emplace_back(CanonicalValue::Object{
        {"source_id", CanonicalValue(source_id)},
        {"content_digest", CanonicalValue(digest)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kExecutablePackageApiVersion))},
      {"content_digest", CanonicalValue(std::string(content_digest))},
      {"executable_package_id", CanonicalValue(std::string(package_id))},
      {"package_version", CanonicalValue(std::string(package_version))},
      {"package_format", CanonicalValue(std::string(package_format))},
      {"sources", CanonicalValue(std::move(rows))},
  });
}

void require_node_environment_shape(const CanonicalValue& candidate) {
  const auto& root = require_object(candidate, "node_environment_manifest");
  require_exact_fields(
      root,
      {"api_version", "environment_digest", "catalog",
       "executable_registry", "executable_packages", "source_coverage"},
      "node_environment_manifest");
  if (require_text(require_member(root, "api_version",
                                  "node_environment_manifest"),
                   "node_environment_manifest.api_version") !=
      detail::kNodeEnvironmentManifestApiVersion) {
    fail("node-environment manifest has an unsupported API version");
  }
  (void)require_digest(
      require_member(root, "environment_digest",
                     "node_environment_manifest"),
      "node_environment_manifest.environment_digest");

  const auto& catalog = require_object(
      require_member(root, "catalog", "node_environment_manifest"),
      "node_environment_manifest.catalog");
  require_exact_fields(
      catalog,
      {"api_version", "catalog_id", "content_version", "catalog_digest"},
      "node_environment_manifest.catalog");
  if (require_text(
          require_member(catalog, "api_version",
                         "node_environment_manifest.catalog"),
          "node_environment_manifest.catalog.api_version") !=
          kBuiltinCatalogApiVersion ||
      require_identifier(
          require_member(catalog, "catalog_id",
                         "node_environment_manifest.catalog"),
          "node_environment_manifest.catalog.catalog_id") !=
          kBuiltinCatalogId ||
      require_text(
          require_member(catalog, "content_version",
                         "node_environment_manifest.catalog"),
          "node_environment_manifest.catalog.content_version") !=
          kBuiltinCatalogContentVersion) {
    fail("node-environment manifest does not select the frozen v2 catalog");
  }
  (void)require_digest(
      require_member(catalog, "catalog_digest",
                     "node_environment_manifest.catalog"),
      "node_environment_manifest.catalog.catalog_digest");

  using RegistryKey =
      std::tuple<std::string, std::string, std::string, std::string>;
  std::vector<RegistryKey> registry_keys;
  std::set<std::pair<std::string, std::string>> node_engines;
  std::set<std::string> referenced_packages;
  const auto& registry = require_array(
      require_member(root, "executable_registry",
                     "node_environment_manifest"),
      "node_environment_manifest.executable_registry",
      detail::kMaximumExecutableRegistryRows, false);
  registry_keys.reserve(registry.size());
  for (std::size_t index = 0U; index < registry.size(); ++index) {
    const std::string field =
        "node_environment_manifest.executable_registry[" +
        std::to_string(index) + "]";
    const auto& row = require_object(registry[index], field);
    require_exact_fields(
        row,
        {"node_id", "engine_id", "implementation_id",
         "executable_package_id"},
        field);
    const std::string node_id =
        require_identifier(require_member(row, "node_id", field),
                           field + ".node_id");
    const std::string engine_id =
        require_identifier(require_member(row, "engine_id", field),
                           field + ".engine_id");
    const std::string implementation_id =
        require_identifier(require_member(row, "implementation_id", field),
                           field + ".implementation_id");
    const std::string package_id = require_identifier(
        require_member(row, "executable_package_id", field),
        field + ".executable_package_id");
    registry_keys.emplace_back(node_id, engine_id, implementation_id,
                               package_id);
    if (!node_engines.emplace(node_id, engine_id).second) {
      fail("node-environment executable registry duplicates node/engine");
    }
    referenced_packages.insert(package_id);
  }
  if (!std::is_sorted(registry_keys.begin(), registry_keys.end()) ||
      std::adjacent_find(registry_keys.begin(), registry_keys.end()) !=
          registry_keys.end()) {
    fail("node-environment executable registry is not canonical");
  }

  struct PackageRow final {
    std::string package_id;
    std::string package_version;
    std::string package_format;
    std::string content_digest;
  };
  std::vector<PackageRow> packages;
  const auto& package_values = require_array(
      require_member(root, "executable_packages",
                     "node_environment_manifest"),
      "node_environment_manifest.executable_packages",
      detail::kMaximumExecutablePackageRows, false);
  packages.reserve(package_values.size());
  for (std::size_t index = 0U; index < package_values.size(); ++index) {
    const std::string field =
        "node_environment_manifest.executable_packages[" +
        std::to_string(index) + "]";
    const auto& row = require_object(package_values[index], field);
    require_exact_fields(
        row,
        {"executable_package_id", "package_version", "package_format",
         "content_digest"},
        field);
    PackageRow package{
        require_identifier(
            require_member(row, "executable_package_id", field),
            field + ".executable_package_id"),
        require_text(require_member(row, "package_version", field),
                     field + ".package_version"),
        require_text(require_member(row, "package_format", field),
                     field + ".package_format"),
        require_digest(require_member(row, "content_digest", field),
                       field + ".content_digest"),
    };
    if (package.package_format != kPythonSourcePackageFormat) {
      fail("node-environment package uses an unqualified package format");
    }
    packages.push_back(std::move(package));
  }
  if (!std::is_sorted(
          packages.begin(), packages.end(),
          [](const auto& lhs, const auto& rhs) {
            return std::tie(lhs.package_id, lhs.package_version,
                            lhs.package_format, lhs.content_digest) <
                   std::tie(rhs.package_id, rhs.package_version,
                            rhs.package_format, rhs.content_digest);
          }) ||
      std::adjacent_find(
          packages.begin(), packages.end(),
          [](const auto& lhs, const auto& rhs) {
            return lhs.package_id == rhs.package_id;
          }) != packages.end()) {
    fail("node-environment package table is not canonical and unique");
  }

  using CoverageKey =
      std::tuple<std::string, std::string, std::string>;
  std::vector<CoverageKey> coverage_keys;
  std::map<std::string,
           std::vector<std::pair<std::string, std::string>>>
      coverage_by_package;
  const auto& coverage = require_array(
      require_member(root, "source_coverage",
                     "node_environment_manifest"),
      "node_environment_manifest.source_coverage",
      detail::kMaximumSourceCoverageRows, false);
  coverage_keys.reserve(coverage.size());
  for (std::size_t index = 0U; index < coverage.size(); ++index) {
    const std::string field =
        "node_environment_manifest.source_coverage[" +
        std::to_string(index) + "]";
    const auto& row = require_object(coverage[index], field);
    require_exact_fields(
        row, {"executable_package_id", "source_id", "content_digest"},
        field);
    const std::string package_id = require_identifier(
        require_member(row, "executable_package_id", field),
        field + ".executable_package_id");
    const std::string source_id =
        require_text(require_member(row, "source_id", field),
                     field + ".source_id");
    if (!is_source_id(source_id)) {
      fail("node-environment source identity is not a logical Python ID");
    }
    const std::string digest =
        require_digest(require_member(row, "content_digest", field),
                       field + ".content_digest");
    coverage_keys.emplace_back(package_id, source_id, digest);
    coverage_by_package[package_id].emplace_back(source_id, digest);
    referenced_packages.insert(package_id);
  }
  if (!std::is_sorted(coverage_keys.begin(), coverage_keys.end()) ||
      std::adjacent_find(
          coverage_keys.begin(), coverage_keys.end(),
          [](const auto& lhs, const auto& rhs) {
            return std::get<0>(lhs) == std::get<0>(rhs) &&
                   std::get<1>(lhs) == std::get<1>(rhs);
          }) != coverage_keys.end()) {
    fail("node-environment source coverage is not canonical and unique");
  }

  std::set<std::string> package_ids;
  for (const auto& package : packages) {
    package_ids.insert(package.package_id);
    const auto found = coverage_by_package.find(package.package_id);
    if (found == coverage_by_package.end() || found->second.empty()) {
      fail("node-environment package has no exact source coverage");
    }
    const CanonicalValue package_candidate = executable_package_projection(
        package.package_id, package.package_version, package.package_format,
        found->second, package.content_digest);
    const detail::ContentIdentitySpec package_spec{
        std::string(kExecutablePackageIdentityDomain),
        std::string(kExecutablePackageApiVersion),
        "content_digest",
    };
    if (!detail::verify_content_identity(package_candidate, package_spec)) {
      fail("node-environment package content identity does not match its "
           "source coverage");
    }
  }
  if (package_ids != referenced_packages) {
    fail("node-environment package joins are not exact and total");
  }

  if (!detail::verify_content_identity(
          candidate, identity_spec(BuildManifestKind::kNodeEnvironment))) {
    fail("node-environment digest does not match its exact manifest");
  }
}

[[nodiscard]] CanonicalValue current_node_environment_candidate() {
  const ParsedArtifactCandidate parsed = parse_strict_json_model_yaml(
      detail::build_evidence::kNodeEnvironmentManifest);
  require_node_environment_shape(parsed.root());
  return parsed.root();
}

void require_sorted_unique_text_array(
    const CanonicalValue& value, std::string_view field,
    std::size_t maximum, bool allow_empty = false) {
  const auto& rows = require_array(value, field, maximum, allow_empty);
  std::string previous;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string& current = require_text(
        rows[index], std::string(field) + "[" + std::to_string(index) + "]");
    if (!previous.empty() && previous >= current) {
      fail(std::string(field) + " is not canonical and unique");
    }
    previous = current;
  }
}

[[nodiscard]] std::uint64_t require_safe_integer(
    const CanonicalValue& value, std::string_view field) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      std::trunc(*number) != *number ||
      *number > static_cast<double>(detail::kMaximumIJsonInteger)) {
    fail(std::string(field) +
         " must be a non-negative I-JSON safe integer");
  }
  return static_cast<std::uint64_t>(*number);
}

void require_compiler_contract_nested_shapes(
    const CanonicalValue::Object& root) {
  require_sorted_unique_text_array(
      require_member(root, "supported_apis", "compiler_contract_manifest"),
      "compiler_contract_manifest.supported_apis",
      detail::kMaximumCompilerContractRows);

  const auto& schema_registry = require_object(
      require_member(root, "schema_registry", "compiler_contract_manifest"),
      "compiler_contract_manifest.schema_registry");
  require_exact_fields(
      schema_registry,
      {"dialect_api_version", "bootstrap_contract_digest",
       "qualification_registry_digest", "normalized_registry"},
      "compiler_contract_manifest.schema_registry");
  (void)require_text(
      require_member(schema_registry, "dialect_api_version",
                     "compiler_contract_manifest.schema_registry"),
      "compiler_contract_manifest.schema_registry.dialect_api_version");
  (void)require_digest(
      require_member(schema_registry, "bootstrap_contract_digest",
                     "compiler_contract_manifest.schema_registry"),
      "compiler_contract_manifest.schema_registry."
      "bootstrap_contract_digest");
  (void)require_digest(
      require_member(schema_registry, "qualification_registry_digest",
                     "compiler_contract_manifest.schema_registry"),
      "compiler_contract_manifest.schema_registry."
      "qualification_registry_digest");
  (void)require_object(
      require_member(schema_registry, "normalized_registry",
                     "compiler_contract_manifest.schema_registry"),
      "compiler_contract_manifest.schema_registry.normalized_registry");

  const std::string stop_registry_field =
      "compiler_contract_manifest.registered_stop_command_registry";
  const auto& stop_registry = require_object(
      require_member(root, "registered_stop_command_registry",
                     "compiler_contract_manifest"),
      stop_registry_field);
  require_exact_fields(stop_registry, {"content_digest", "rows"},
                       stop_registry_field);
  const auto& stop_rows_value = require_member(
      stop_registry, "rows", stop_registry_field);
  const auto& stop_rows = require_array(
      stop_rows_value, stop_registry_field + ".rows",
      detail::kMaximumCompilerContractRows, false);
  std::string previous_stop_id;
  for (std::size_t index = 0U; index < stop_rows.size(); ++index) {
    const std::string row_field = stop_registry_field + ".rows[" +
                                  std::to_string(index) + "]";
    const auto& row = require_object(stop_rows[index], row_field);
    const auto* safety_class =
        find_member(row, "safety_system_request_action_class");
    require_exact_fields(
        row,
        safety_class == nullptr
            ? std::initializer_list<std::string_view>{
                  "registered_command_kind_id", "stop_action_class"}
            : std::initializer_list<std::string_view>{
                  "registered_command_kind_id", "stop_action_class",
                  "safety_system_request_action_class"},
        row_field);
    const std::string& id = require_text(
        require_member(row, "registered_command_kind_id", row_field),
        row_field + ".registered_command_kind_id");
    const std::string& action = require_text(
        require_member(row, "stop_action_class", row_field),
        row_field + ".stop_action_class");
    if (!previous_stop_id.empty() && previous_stop_id >= id) {
      fail(stop_registry_field + ".rows is not canonical and unique");
    }
    previous_stop_id = id;
    if (action == "safety_system_request") {
      if (safety_class == nullptr) {
        fail(row_field + " omits its required safety action class");
      }
      const std::string& value = require_text(
          *safety_class,
          row_field + ".safety_system_request_action_class");
      if (value != "protective_stop_request" &&
          value != "emergency_stop_request") {
        fail(row_field + " has an unknown safety action class");
      }
    } else if ((action != "controlled" && action != "software_hard") ||
               safety_class != nullptr) {
      fail(row_field + " has an invalid action-class presence matrix");
    }
  }
  const std::string& stop_registry_digest = require_digest(
      require_member(stop_registry, "content_digest", stop_registry_field),
      stop_registry_field + ".content_digest");
  if (stop_registry_digest != sha256_digest(canonical_json(stop_rows_value))) {
    fail(stop_registry_field + " digest does not match its canonical rows");
  }

  const auto& product_contracts = require_array(
      require_member(root, "product_contract_registry",
                     "compiler_contract_manifest"),
      "compiler_contract_manifest.product_contract_registry",
      detail::kMaximumCompilerContractRows, false);
  std::pair<std::string, std::string> previous_product;
  bool first_product = true;
  for (std::size_t index = 0U; index < product_contracts.size(); ++index) {
    const std::string field =
        "compiler_contract_manifest.product_contract_registry[" +
        std::to_string(index) + "]";
    const auto& row = require_object(product_contracts[index], field);
    require_exact_fields(
        row,
        {"api_version", "product_contract_id",
         "product_contract_revision", "product_kind", "placement",
         "artifact_roles"},
        field);
    (void)require_text(require_member(row, "api_version", field),
                       field + ".api_version");
    const std::string id =
        require_identifier(require_member(row, "product_contract_id", field),
                           field + ".product_contract_id");
    const std::string revision = require_digest(
        require_member(row, "product_contract_revision", field),
        field + ".product_contract_revision");
    (void)require_identifier(require_member(row, "product_kind", field),
                             field + ".product_kind");
    (void)require_identifier(require_member(row, "placement", field),
                             field + ".placement");
    const auto& artifact_roles = require_array(
        require_member(row, "artifact_roles", field),
        field + ".artifact_roles",
        detail::kMaximumCompilerContractRows, false);
    std::string previous_role;
    for (std::size_t role_index = 0U;
         role_index < artifact_roles.size(); ++role_index) {
      const std::string role_field =
          field + ".artifact_roles[" + std::to_string(role_index) + "]";
      const auto& role =
          require_object(artifact_roles[role_index], role_field);
      require_exact_fields(
          role, {"artifact_role_id", "media_type", "cardinality"},
          role_field);
      const std::string role_id = require_identifier(
          require_member(role, "artifact_role_id", role_field),
          role_field + ".artifact_role_id");
      (void)require_text(require_member(role, "media_type", role_field),
                         role_field + ".media_type");
      (void)require_identifier(
          require_member(role, "cardinality", role_field),
          role_field + ".cardinality");
      if (!previous_role.empty() && previous_role >= role_id) {
        fail(field + ".artifact_roles is not canonical and unique");
      }
      previous_role = role_id;
    }
    const std::pair current_product{id, revision};
    if (!first_product && previous_product >= current_product) {
      fail("compiler product-contract registry is not canonical and unique");
    }
    first_product = false;
    previous_product = current_product;
  }

  const auto& identities = require_array(
      require_member(root, "content_identity_contracts",
                     "compiler_contract_manifest"),
      "compiler_contract_manifest.content_identity_contracts",
      detail::kMaximumCompilerContractRows, false);
  std::string previous_domain;
  for (std::size_t index = 0U; index < identities.size(); ++index) {
    const std::string field =
        "compiler_contract_manifest.content_identity_contracts[" +
        std::to_string(index) + "]";
    const auto& row = require_object(identities[index], field);
    require_exact_fields(
        row,
        {"identity_domain", "schema_api_tag", "omitted_field",
         "root_shape"},
        field);
    const std::string domain =
        require_text(require_member(row, "identity_domain", field),
                     field + ".identity_domain");
    (void)require_text(require_member(row, "schema_api_tag", field),
                       field + ".schema_api_tag");
    const auto& omitted =
        require_member(row, "omitted_field", field).storage();
    if (!std::holds_alternative<std::nullptr_t>(omitted)) {
      (void)require_identifier(
          require_member(row, "omitted_field", field),
          field + ".omitted_field");
    }
    const std::string& root_shape =
        require_identifier(require_member(row, "root_shape", field),
                           field + ".root_shape");
    if (root_shape != "document" &&
        root_shape != "nested_record" &&
        root_shape != "complete_projection") {
      fail(field + ".root_shape is outside the closed enum");
    }
    if (!previous_domain.empty() && previous_domain >= domain) {
      fail("compiler content-identity contracts are not canonical");
    }
    previous_domain = domain;
  }

  const auto& source_media = require_object(
      require_member(root, "source_media_contract",
                     "compiler_contract_manifest"),
      "compiler_contract_manifest.source_media_contract");
  require_exact_fields(
      source_media,
      {"artifact_rows", "store_key_contract", "uri_contract"},
      "compiler_contract_manifest.source_media_contract");
  (void)require_text(
      require_member(source_media, "store_key_contract",
                     "compiler_contract_manifest.source_media_contract"),
      "compiler_contract_manifest.source_media_contract."
      "store_key_contract");
  (void)require_text(
      require_member(source_media, "uri_contract",
                     "compiler_contract_manifest.source_media_contract"),
      "compiler_contract_manifest.source_media_contract.uri_contract");
  const auto& media_rows = require_array(
      require_member(source_media, "artifact_rows",
                     "compiler_contract_manifest.source_media_contract"),
      "compiler_contract_manifest.source_media_contract.artifact_rows",
      detail::kMaximumCompilerContractRows, false);
  std::string previous_kind;
  for (std::size_t index = 0U; index < media_rows.size(); ++index) {
    const std::string field =
        "compiler_contract_manifest.source_media_contract.artifact_rows[" +
        std::to_string(index) + "]";
    const auto& row = require_object(media_rows[index], field);
    require_exact_fields(
        row, {"artifact_kind", "media_type", "source_role"}, field);
    const std::string kind =
        require_identifier(require_member(row, "artifact_kind", field),
                           field + ".artifact_kind");
    (void)require_text(require_member(row, "media_type", field),
                       field + ".media_type");
    (void)require_identifier(require_member(row, "source_role", field),
                             field + ".source_role");
    if (!previous_kind.empty() && previous_kind >= kind) {
      fail("compiler source/media rows are not canonical");
    }
    previous_kind = kind;
  }

  const auto& limits = require_array(
      require_member(root, "resource_limits", "compiler_contract_manifest"),
      "compiler_contract_manifest.resource_limits",
      detail::kMaximumCompilerContractRows, false);
  std::string previous_limit;
  for (std::size_t index = 0U; index < limits.size(); ++index) {
    const std::string field =
        "compiler_contract_manifest.resource_limits[" +
        std::to_string(index) + "]";
    const auto& row = require_object(limits[index], field);
    require_exact_fields(row, {"limit_id", "value"}, field);
    const std::string id =
        require_identifier(require_member(row, "limit_id", field),
                           field + ".limit_id");
    (void)require_safe_integer(require_member(row, "value", field),
                               field + ".value");
    if (!previous_limit.empty() && previous_limit >= id) {
      fail("compiler resource limits are not canonical");
    }
    previous_limit = id;
  }

  const std::string composition_field =
      "compiler_contract_manifest.composition_contract";
  const auto& composition = require_object(
      require_member(root, "composition_contract",
                     "compiler_contract_manifest"),
      composition_field);
  require_exact_fields(
      composition,
      {"naming_scheme", "name_preimage_prefix", "generated_name_prefix",
       "joint_name_token_format",
       "joint_parent_local_rank_decimal_width",
       "joint_parent_local_rank_decimal_base",
       "joint_parent_local_rank_capacity", "joint_source_tie_break",
       "joint_mimic_projection_policy",
       "joint_sibling_ordering_policy",
       "fixed_connection_ordering_policy",
       "joint_hash_preimage_domain", "joint_hash_field_encoding",
       "source_joint_hash_fields", "fixed_connection_hash_fields",
       "occurrence_symbol_rewrite_key_fields",
       "cell_frame_policy",
       "rewrite_policy", "frame_path_policy"},
      composition_field);
  const auto require_exact_composition_text =
      [&](std::string_view name, std::string_view expected) {
        const auto& actual = require_text(
            require_member(composition, name, composition_field),
            composition_field + "." + std::string(name));
        if (actual != expected) {
          fail(composition_field + "." + std::string(name) +
               " does not match the compiler-owned naming contract");
        }
      };
  require_exact_composition_text("naming_scheme",
                                 kCompositionNamingScheme);
  require_exact_composition_text("name_preimage_prefix",
                                 kCompositionNamePreimagePrefixes);
  require_exact_composition_text("generated_name_prefix",
                                 kCompositionGeneratedNamePrefixes);
  require_exact_composition_text("joint_name_token_format",
                                 kMf2JointNameTokenFormat);
  require_exact_composition_text("joint_source_tie_break",
                                 kMf2JointSourceTieBreak);
  require_exact_composition_text("joint_mimic_projection_policy",
                                 kMf2JointMimicProjectionPolicy);
  require_exact_composition_text("joint_sibling_ordering_policy",
                                 kMf2JointSiblingOrderingPolicy);
  require_exact_composition_text("fixed_connection_ordering_policy",
                                 kMf2FixedConnectionOrderingPolicy);
  require_exact_composition_text("joint_hash_preimage_domain",
                                 detail::kMf2JointHashPreimageDomain);
  require_exact_composition_text("joint_hash_field_encoding",
                                 kMf2JointHashFieldEncoding);
  require_exact_composition_text("source_joint_hash_fields",
                                 kMf2SourceJointHashFields);
  require_exact_composition_text("fixed_connection_hash_fields",
                                 kMf2FixedConnectionHashFields);
  require_exact_composition_text("occurrence_symbol_rewrite_key_fields",
                                 kOccurrenceSymbolRewriteKeyFields);
  require_exact_composition_text("cell_frame_policy",
                                 kCellFramePolicy);
  require_exact_composition_text("rewrite_policy", kRewritePolicy);
  require_exact_composition_text("frame_path_policy",
                                 kFramePathPolicy);
  if (require_safe_integer(
          require_member(composition,
                         "joint_parent_local_rank_decimal_width",
                         composition_field),
          composition_field +
              ".joint_parent_local_rank_decimal_width") !=
      detail::kMf2JointLocalRankWidth) {
    fail(composition_field +
         ".joint_parent_local_rank_decimal_width is not the mf2 width");
  }
  if (require_safe_integer(
          require_member(composition,
                         "joint_parent_local_rank_decimal_base",
                         composition_field),
          composition_field +
              ".joint_parent_local_rank_decimal_base") !=
      kMf2JointParentLocalRankDecimalBase) {
    fail(composition_field +
         ".joint_parent_local_rank_decimal_base is not decimal");
  }
  if (require_safe_integer(
          require_member(composition,
                         "joint_parent_local_rank_capacity",
                         composition_field),
          composition_field + ".joint_parent_local_rank_capacity") !=
      detail::kMf2JointMaximumLocalRank + 1U) {
    fail(composition_field +
         ".joint_parent_local_rank_capacity is not the mf2 capacity");
  }
  const auto& accepted = require_object(
      require_member(root, "accepted_deployment_subset",
                     "compiler_contract_manifest"),
      "compiler_contract_manifest.accepted_deployment_subset");
  require_exact_fields(
      accepted,
      {"deployment_api_version", "qualified_catalog_kinds",
       "unqualified_catalog_kinds", "assistant", "policies",
       "compatibility", "i3_retention_subset"},
      "compiler_contract_manifest.accepted_deployment_subset");
  for (const std::string_view name :
       {"deployment_api_version", "assistant", "policies",
        "compatibility", "i3_retention_subset"}) {
    (void)require_text(
        require_member(accepted, name,
                       "compiler_contract_manifest."
                       "accepted_deployment_subset"),
        std::string(
            "compiler_contract_manifest.accepted_deployment_subset.") +
            std::string(name));
  }
  for (const std::string_view name :
       {"qualified_catalog_kinds", "unqualified_catalog_kinds"}) {
    require_sorted_unique_text_array(
        require_member(accepted, name,
                       "compiler_contract_manifest."
                       "accepted_deployment_subset"),
        std::string(
            "compiler_contract_manifest.accepted_deployment_subset.") +
            std::string(name),
        detail::kMaximumCompilerContractRows);
  }

  const auto& retained = require_object(
      require_member(root, "retained_projection_contract",
                     "compiler_contract_manifest"),
      "compiler_contract_manifest.retained_projection_contract");
  require_exact_fields(
      retained,
      {"bundle_api_version", "bundle_identity_domain",
       "product_output_identity_domain", "product_output_schema_tag",
       "qualified_product_contract_ids",
       "qualified_product_placements",
       "canonical_empty_artifact_roles", "active_alias_owner"},
      "compiler_contract_manifest.retained_projection_contract");
  for (const std::string_view name :
       {"bundle_api_version", "bundle_identity_domain",
        "product_output_identity_domain", "product_output_schema_tag",
        "active_alias_owner"}) {
    (void)require_text(
        require_member(retained, name,
                       "compiler_contract_manifest."
                       "retained_projection_contract"),
        std::string(
            "compiler_contract_manifest.retained_projection_contract.") +
            std::string(name));
  }
  for (const std::string_view name :
       {"qualified_product_contract_ids",
        "qualified_product_placements",
        "canonical_empty_artifact_roles"}) {
    require_sorted_unique_text_array(
        require_member(retained, name,
                       "compiler_contract_manifest."
                       "retained_projection_contract"),
        std::string(
            "compiler_contract_manifest.retained_projection_contract.") +
            std::string(name),
        detail::kMaximumCompilerContractRows);
  }
}

void require_compiler_contract_shape(const CanonicalValue& candidate) {
  const auto& root = require_object(candidate, "compiler_contract_manifest");
  require_exact_fields(
      root,
      {"api_version", "compiler_contract_digest", "supported_apis",
       "schema_registry", "product_contract_registry",
       "registered_stop_command_registry",
       "content_identity_contracts", "source_media_contract",
       "resource_limits", "composition_contract",
       "accepted_deployment_subset", "retained_projection_contract"},
      "compiler_contract_manifest");
  if (require_text(
          require_member(root, "api_version",
                         "compiler_contract_manifest"),
          "compiler_contract_manifest.api_version") !=
      detail::kCompilerContractManifestApiVersion) {
    fail("compiler-contract manifest has an unsupported API version");
  }
  (void)require_digest(
      require_member(root, "compiler_contract_digest",
                     "compiler_contract_manifest"),
      "compiler_contract_manifest.compiler_contract_digest");
  for (const std::string_view array_name :
       {"supported_apis", "product_contract_registry",
        "content_identity_contracts", "resource_limits"}) {
    (void)require_array(
        require_member(root, array_name, "compiler_contract_manifest"),
        std::string("compiler_contract_manifest.") +
            std::string(array_name),
        detail::kMaximumCompilerContractRows, false);
  }
  for (const std::string_view object_name :
       {"schema_registry", "source_media_contract",
        "registered_stop_command_registry",
        "composition_contract", "accepted_deployment_subset",
        "retained_projection_contract"}) {
    (void)require_object(
        require_member(root, object_name, "compiler_contract_manifest"),
        std::string("compiler_contract_manifest.") +
            std::string(object_name));
  }
  require_compiler_contract_nested_shapes(root);
  if (!detail::verify_content_identity(
          candidate, identity_spec(BuildManifestKind::kCompilerContract))) {
    fail("compiler-contract digest does not match its exact manifest");
  }
}

void require_compiler_build_shape(const CanonicalValue& candidate) {
  const auto& root = require_object(candidate, "compiler_build_manifest");
  require_exact_fields(
      root,
      {"api_version", "compiler_build_digest", "compiler_package",
       "compiler_contract_digest", "build_profile",
       "build_configuration_digest", "target",
       "source_inputs", "recipe_inputs", "toolchain", "dependencies"},
      "compiler_build_manifest");
  if (require_text(
          require_member(root, "api_version", "compiler_build_manifest"),
          "compiler_build_manifest.api_version") !=
      detail::kCompilerBuildManifestApiVersion) {
    fail("compiler-build manifest has an unsupported API version");
  }
  (void)require_digest(
      require_member(root, "compiler_build_digest",
                     "compiler_build_manifest"),
      "compiler_build_manifest.compiler_build_digest");
  (void)require_digest(
      require_member(root, "compiler_contract_digest",
                     "compiler_build_manifest"),
      "compiler_build_manifest.compiler_contract_digest");
  (void)require_text(
      require_member(root, "build_profile", "compiler_build_manifest"),
      "compiler_build_manifest.build_profile");
  (void)require_digest(
      require_member(root, "build_configuration_digest",
                     "compiler_build_manifest"),
      "compiler_build_manifest.build_configuration_digest");
  (void)require_text(
      require_member(root, "target", "compiler_build_manifest"),
      "compiler_build_manifest.target");
  const auto& compiler_package = require_object(
      require_member(root, "compiler_package", "compiler_build_manifest"),
      "compiler_build_manifest.compiler_package");
  require_exact_fields(
      compiler_package, {"package_id", "package_version"},
      "compiler_build_manifest.compiler_package");
  (void)require_identifier(
      require_member(compiler_package, "package_id",
                     "compiler_build_manifest.compiler_package"),
      "compiler_build_manifest.compiler_package.package_id");
  (void)require_text(
      require_member(compiler_package, "package_version",
                     "compiler_build_manifest.compiler_package"),
      "compiler_build_manifest.compiler_package.package_version");

  const auto& toolchain = require_object(
      require_member(root, "toolchain", "compiler_build_manifest"),
      "compiler_build_manifest.toolchain");
  require_exact_fields(
      toolchain, {"compiler_id", "compiler_version", "cxx_standard"},
      "compiler_build_manifest.toolchain");
  (void)require_text(
      require_member(toolchain, "compiler_id",
                     "compiler_build_manifest.toolchain"),
      "compiler_build_manifest.toolchain.compiler_id");
  (void)require_text(
      require_member(toolchain, "compiler_version",
                     "compiler_build_manifest.toolchain"),
      "compiler_build_manifest.toolchain.compiler_version");
  (void)require_text(
      require_member(toolchain, "cxx_standard",
                     "compiler_build_manifest.toolchain"),
      "compiler_build_manifest.toolchain.cxx_standard");

  for (const std::string_view array_name :
       {"source_inputs", "recipe_inputs"}) {
    const std::string array_field =
        std::string("compiler_build_manifest.") +
        std::string(array_name);
    const auto& rows = require_array(
        require_member(root, array_name, "compiler_build_manifest"),
        array_field, detail::kMaximumCompilerBuildRows, false);
    std::string previous;
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      const std::string field =
          array_field + "[" + std::to_string(index) + "]";
      const auto& row = require_object(rows[index], field);
      require_exact_fields(row, {"input_id", "content_digest"}, field);
      const std::string input_id =
          require_text(require_member(row, "input_id", field),
                       field + ".input_id");
      const bool allowed_class =
          array_name == "source_inputs"
              ? (input_id.starts_with("cpp-public-header:") ||
                 input_id.starts_with("cpp-internal-header:") ||
                 input_id.starts_with("cpp-translation-unit:") ||
                 input_id.starts_with("compiler-tool:") ||
                 input_id.starts_with("compiler-python-tool:") ||
                 input_id.starts_with("compiler-resource:"))
              : (input_id.starts_with("build-recipe:") ||
                 input_id.starts_with("build-template:"));
      if (!is_logical_build_input_id(input_id) || !allowed_class ||
          input_id.find("..") != std::string::npos ||
          (!previous.empty() && previous >= input_id)) {
        fail(array_field +
             " contains a path-dependent, duplicate, or unsorted ID");
      }
      previous = input_id;
      (void)require_digest(
          require_member(row, "content_digest", field),
          field + ".content_digest");
    }
  }

  const auto& dependencies = require_array(
      require_member(root, "dependencies", "compiler_build_manifest"),
      "compiler_build_manifest.dependencies",
      detail::kMaximumCompilerBuildRows, false);
  std::string previous_dependency;
  for (std::size_t index = 0U; index < dependencies.size(); ++index) {
    const std::string field =
        "compiler_build_manifest.dependencies[" +
        std::to_string(index) + "]";
    const auto& row = require_object(dependencies[index], field);
    require_exact_fields(
        row, {"dependency_id", "dependency_version"}, field);
    const std::string dependency_id =
        require_identifier(
            require_member(row, "dependency_id", field),
            field + ".dependency_id");
    (void)require_text(
        require_member(row, "dependency_version", field),
        field + ".dependency_version");
    if (!previous_dependency.empty() &&
        previous_dependency >= dependency_id) {
      fail("compiler-build dependencies are not canonical and unique");
    }
    previous_dependency = dependency_id;
  }
  if (!detail::verify_content_identity(
          candidate, identity_spec(BuildManifestKind::kCompilerBuild))) {
    fail("compiler-build digest does not match its exact manifest");
  }
}

void require_manifest_shape(BuildManifestKind kind,
                            const CanonicalValue& candidate) {
  try {
    switch (kind) {
      case BuildManifestKind::kNodeEnvironment:
        require_node_environment_shape(candidate);
        return;
      case BuildManifestKind::kCompilerContract:
        require_compiler_contract_shape(candidate);
        return;
      case BuildManifestKind::kCompilerBuild:
        require_compiler_build_shape(candidate);
        return;
    }
  } catch (const CanonicalValueError& error) {
    fail(std::string("invalid build manifest: ") + error.what());
  }
  throw std::logic_error("unknown build manifest kind");
}

}  // namespace

namespace detail {

CanonicalValue current_build_manifest_candidate(BuildManifestKind kind) {
  switch (kind) {
    case BuildManifestKind::kNodeEnvironment:
      return current_node_environment_candidate();
    case BuildManifestKind::kCompilerContract:
      return current_compiler_contract_candidate();
    case BuildManifestKind::kCompilerBuild:
      return current_compiler_build_candidate();
  }
  throw std::logic_error("unknown build manifest kind");
}

std::shared_ptr<const BuildQualifiedManifest>
BuildQualifiedRetentionAuthority::qualify_self_verified_candidate(
    BuildManifestKind kind, const CanonicalValue& candidate) {
  require_manifest_shape(kind, candidate);
  const std::string digest = declared_digest(candidate, kind);
  return std::shared_ptr<const BuildQualifiedManifest>(
      new BuildQualifiedManifest(
          BuildManifestKey{kind, core::ContentDigest(digest)},
          canonical_json(candidate)));
}

std::shared_ptr<const BuildQualifiedManifest>
BuildQualifiedRetentionAuthority::qualify_candidate(
    BuildManifestKind kind, const CanonicalValue& candidate) {
  const auto qualified =
      qualify_self_verified_candidate(kind, candidate);
  const CanonicalValue expected = current_build_manifest_candidate(kind);
  if (candidate != expected) {
    fail(std::string(to_string(kind)) +
         " manifest is stale or differs from this exact build");
  }
  return qualified;
}

BuildQualifiedRetentionContext
BuildQualifiedRetentionAuthority::qualify() {
  return qualify({});
}

BuildQualifiedRetentionContext
BuildQualifiedRetentionAuthority::qualify(
    std::span<const StopQualificationEvidenceCandidate>
        stop_evidence_candidates) {
  const auto node = qualify_candidate(
      BuildManifestKind::kNodeEnvironment,
      current_build_manifest_candidate(
          BuildManifestKind::kNodeEnvironment));
  const auto contract = qualify_candidate(
      BuildManifestKind::kCompilerContract,
      current_build_manifest_candidate(
          BuildManifestKind::kCompilerContract));
  const auto build = qualify_candidate(
      BuildManifestKind::kCompilerBuild,
      current_build_manifest_candidate(BuildManifestKind::kCompilerBuild));

  const auto build_root =
      parse_strict_json_model_yaml(build->canonical_bytes()).root();
  const std::string& build_contract_digest = require_digest(
      require_member(build_root.as_object(), "compiler_contract_digest",
                     "compiler_build_manifest"),
      "compiler_build_manifest.compiler_contract_digest");
  if (build_contract_digest != contract->key().content_digest.value()) {
    throw std::logic_error(
        "compiler-build manifest does not bind this compiler contract");
  }

  const core::RetainedNodeEnvironmentDTO retained_environment =
      retained_node_environment_from_manifest(*node);
  std::vector<std::shared_ptr<const BuildQualifiedManifest>> manifests{
      node, contract, build};
  return BuildQualifiedRetentionContext(
      retained_environment, contract->key().content_digest,
      build->key().content_digest, std::move(manifests),
      qualify_stop_evidence(stop_evidence_candidates));
}

BuildQualifiedRetentionContext
BuildQualifiedRetentionAuthority::qualify_persisted(
    IBuildManifestRepository& repository) {
  return qualify_persisted(repository, {});
}

BuildQualifiedRetentionContext
BuildQualifiedRetentionAuthority::qualify_persisted(
    IBuildManifestRepository& repository,
    std::span<const StopQualificationEvidenceCandidate>
        stop_evidence_candidates) {
  const BuildQualifiedRetentionContext current =
      qualify(stop_evidence_candidates);
  if (current.manifests().size() != 3U) {
    throw std::logic_error(
        "production retention authority omitted a required manifest");
  }
  for (const auto& manifest : current.manifests()) {
    (void)repository.put_if_absent(manifest);
  }

  std::vector<std::shared_ptr<const BuildQualifiedManifest>>
      reloaded_manifests;
  reloaded_manifests.reserve(current.manifests().size());
  for (const auto& expected : current.manifests()) {
    const auto reloaded = repository.get_verified(expected->key());
    if (reloaded == nullptr || reloaded->key() != expected->key() ||
        reloaded->canonical_bytes() != expected->canonical_bytes()) {
      fail("production retention authority did not reload the exact "
           "persisted manifest closure");
    }
    reloaded_manifests.push_back(reloaded);
  }
  const auto retained_environment =
      retained_node_environment_from_manifest(*reloaded_manifests[0]);
  const auto compiler_contract_digest =
      reloaded_manifests[1]->key().content_digest;
  const auto compiler_build_digest =
      reloaded_manifests[2]->key().content_digest;
  return BuildQualifiedRetentionContext(
      retained_environment, compiler_contract_digest, compiler_build_digest,
      std::move(reloaded_manifests), current.stop_evidence());
}

BuildQualifiedRetentionContext
BuildQualifiedRetentionAuthority::qualify_existing(
    const std::filesystem::path& trusted_package_root) {
  const BuildQualifiedRetentionContext current = qualify();
  if (current.manifests().size() != 3U) {
    throw std::logic_error(
        "build authority omitted a required current manifest");
  }

  const std::filesystem::path root =
      posix::normalize_existing_plain_root(
          trusted_package_root, kReadOnlyManifestPosixErrors,
          kReadOnlyManifestRootMessages);
  const std::filesystem::path manifest_root = root / "build-manifests";
  posix::require_existing_plain_directory(
      manifest_root, kReadOnlyManifestPosixErrors,
      kReadOnlyManifestDirectoryMessages);

  std::vector<std::shared_ptr<const BuildQualifiedManifest>>
      reloaded_manifests;
  reloaded_manifests.reserve(current.manifests().size());
  for (const auto& expected : current.manifests()) {
    const auto bytes = posix::read_exact_regular_file(
        existing_manifest_path(manifest_root, expected->key()),
        kMaxQualifiedManifestBytes, false,
        kReadOnlyManifestPosixErrors, kReadOnlyManifestReadMessages);
    if (!bytes.has_value()) {
      throw std::logic_error(
          "required canonical package build manifest disappeared");
    }
    const auto reloaded = reload_self_verified_build_manifest(
        expected->key().manifest_kind, *bytes);
    if (reloaded->key() != expected->key() ||
        reloaded->canonical_bytes() != expected->canonical_bytes()) {
      fail("canonical package build manifest is stale or differs from this "
           "exact build");
    }
    reloaded_manifests.push_back(reloaded);
  }

  const auto retained_environment =
      retained_node_environment_from_manifest(*reloaded_manifests[0]);
  const auto compiler_contract_digest =
      reloaded_manifests[1]->key().content_digest;
  const auto compiler_build_digest =
      reloaded_manifests[2]->key().content_digest;
  return BuildQualifiedRetentionContext(
      retained_environment, compiler_contract_digest, compiler_build_digest,
      std::move(reloaded_manifests));
}

CanonicalValue BuildManifestTestAccess::current_candidate(
    BuildManifestKind kind) {
  return current_build_manifest_candidate(kind);
}

std::shared_ptr<const BuildQualifiedManifest>
BuildManifestTestAccess::qualify_candidate(
    BuildManifestKind kind, const CanonicalValue& candidate) {
  return BuildQualifiedRetentionAuthority::qualify_candidate(kind,
                                                             candidate);
}

BuildQualifiedRetentionContext
BuildManifestTestAccess::qualify_candidates(
    const CanonicalValue& node_environment,
    const CanonicalValue& compiler_contract,
    const CanonicalValue& compiler_build) {
  const auto node = BuildQualifiedRetentionAuthority::qualify_candidate(
      BuildManifestKind::kNodeEnvironment, node_environment);
  const auto contract =
      BuildQualifiedRetentionAuthority::qualify_candidate(
          BuildManifestKind::kCompilerContract, compiler_contract);
  const auto build = BuildQualifiedRetentionAuthority::qualify_candidate(
      BuildManifestKind::kCompilerBuild, compiler_build);
  std::vector<std::shared_ptr<const BuildQualifiedManifest>> manifests{
      node, contract, build};
  return BuildQualifiedRetentionContext(
      retained_node_environment_from_manifest(*node),
      contract->key().content_digest, build->key().content_digest,
      std::move(manifests));
}

BuildQualifiedRetentionContext
BuildManifestTestAccess::qualify_persisted_with_test_stop_build_identity(
    IBuildManifestRepository& repository,
    std::string_view canonical_build_identity_bytes) {
  const auto current =
      BuildQualifiedRetentionAuthority::qualify_persisted(repository);
  std::vector<std::shared_ptr<const BuildQualifiedManifest>> manifests(
      current.manifests().begin(), current.manifests().end());
  std::vector<StopImplementationBuildIdentity> identities;
  identities.push_back(parse_stop_implementation_build_identity(
      canonical_build_identity_bytes));
  BuildQualifiedStopEvidenceClosure stop_evidence(
      std::nullopt, {}, std::nullopt, {}, std::move(identities));
  return BuildQualifiedRetentionContext(
      current.node_environment(), current.compiler_contract_digest(),
      current.compiler_build_digest(), std::move(manifests),
      std::move(stop_evidence));
}

std::shared_ptr<const RetainedBundle>
BuildManifestTestAccess::materialize_with_candidates(
    const RetentionQualifiedDeployment& qualified,
    const CanonicalValue& node_environment,
    const CanonicalValue& compiler_contract,
    const CanonicalValue& compiler_build,
    IBuildManifestRepository& manifest_repository,
    IRawArtifactStore& artifact_store) {
  const BuildQualifiedRetentionContext context = qualify_candidates(
      node_environment, compiler_contract, compiler_build);
  for (const auto& manifest : context.manifests()) {
    (void)manifest_repository.put_if_absent(manifest);
  }
  const RejectingCompilerInputArtifactSource input_source;
  RejectingCatalogArtifactStore catalog_store;
  return RetainedBundleMaterializer::materialize(
      qualified, context, input_source, {}, catalog_store,
      artifact_store);
}

std::shared_ptr<const BuildQualifiedManifest>
reload_self_verified_build_manifest(
    BuildManifestKind kind, std::string_view canonical_bytes) {
  try {
    const ParsedArtifactCandidate parsed =
        parse_strict_json_model_yaml(canonical_bytes);
    if (canonical_json(parsed.root()) != canonical_bytes) {
      fail("semantic manifest repository bytes are not RFC 8785 canonical");
    }
    return BuildQualifiedRetentionAuthority::
        qualify_self_verified_candidate(kind, parsed.root());
  } catch (const CanonicalValueError& error) {
    fail(std::string("semantic manifest repository bytes are invalid: ") +
         error.what());
  }
}

core::RetainedNodeEnvironmentDTO retained_node_environment_from_manifest(
    const BuildQualifiedManifest& manifest) {
  if (manifest.key().manifest_kind !=
      BuildManifestKind::kNodeEnvironment) {
    fail("retained node environment requires a node-environment manifest");
  }
  const CanonicalValue root =
      parse_strict_json_model_yaml(manifest.canonical_bytes()).root();
  require_node_environment_shape(root);
  const auto& object = root.as_object();
  const auto& catalog = require_object(
      require_member(object, "catalog", "node_environment_manifest"),
      "node_environment_manifest.catalog");
  return core::RetainedNodeEnvironmentDTO{
      require_identifier(
          require_member(catalog, "catalog_id",
                         "node_environment_manifest.catalog"),
          "node_environment_manifest.catalog.catalog_id"),
      require_text(
          require_member(catalog, "content_version",
                         "node_environment_manifest.catalog"),
          "node_environment_manifest.catalog.content_version"),
      core::ContentDigest(require_digest(
          require_member(catalog, "catalog_digest",
                         "node_environment_manifest.catalog"),
          "node_environment_manifest.catalog.catalog_digest")),
      core::ContentDigest(require_digest(
          require_member(object, "environment_digest",
                         "node_environment_manifest"),
          "node_environment_manifest.environment_digest")),
  };
}

}  // namespace detail

}  // namespace manyforge::deployment
