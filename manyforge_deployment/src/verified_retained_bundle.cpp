#include "manyforge_deployment/verified_retained_bundle.hpp"
#include "manyforge_deployment/scene_pose_source_policy_codec.hpp"
#include <manyforge_deployment/io_provider_domain.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "compiler_semantic_limits_internal.hpp"
#include "composed_model_symbol_role_map_internal.hpp"
#include "content_identity_internal.hpp"
#include "control_domain_control_internal.hpp"
#include "deployment_catalog_codec_internal.hpp"
#include "manyforge_core/runtime_contract_manifest_codec.hpp"
#include "manyforge_core/types/command_schema.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/assistant_settings_codec.hpp"
#include "manyforge_deployment/cell_graph_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/deployment_applicability_codec.hpp"
#include "manyforge_deployment/runtime_contract_registry.hpp"
#include "manyforge_deployment/runtime_profile_codec.hpp"
#include "manyforge_deployment/structural_model_preparation.hpp"
#include "planning_resource_map_internal.hpp"
#include "reference_runtime_unit_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

#include <manyforge_core/types/stop_control.hpp>

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kFlattenedModelContractId =
    "manyforge.flattened_model";
constexpr std::string_view kFlattenedModelPlacement =
    "cell_planning_collision_model";
constexpr std::string_view kViewerModelContractId = "manyforge.viewer_model";
constexpr std::string_view kViewerModelPlacement = "cell_viewer_model";
constexpr std::string_view kControlDomainControlPlacement = "control_domain";
constexpr std::string_view kReferenceRuntimePlacement = "runtime_unit";
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
constexpr std::array<detail::QualifiedRetainedProductArtifactRole, 2U>
    kViewerModelArtifactRoles{{
        {detail::kPlanningResourceMapArtifactRole,
         detail::kPlanningResourceMapMediaType, true},
        {"urdf", "application/xml", true},
    }};
constexpr std::array<detail::QualifiedRetainedProductArtifactRole, 2U>
    kControlDomainControlArtifactRoles{{
        {detail::kControlDomainControlArtifactRole,
         detail::kControlDomainControlMediaType, true},
        {core::kStopInterfaceCommissioningArtifactRole,
         core::kStopInterfaceCommissioningMediaType, false},
    }};
constexpr std::array<detail::QualifiedRetainedProductArtifactRole, 2U>
    kReferenceRuntimeArtifactRoles{{
        {detail::kReferenceRuntimeLaunchableArtifactRole,
         detail::kReferenceRuntimeLaunchableMediaType, true},
        {detail::kReferenceRuntimeManifestArtifactRole,
         detail::kReferenceRuntimeManifestMediaType, true},
    }};
constexpr std::array<detail::QualifiedRetainedProductArtifactRole, 2U>
    kRos2ControlExecutionRuntimeArtifactRoles{{
        {detail::kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole,
         detail::kRos2ControlExecutionRuntimeLaunchDescriptorMediaType, true},
        {detail::kRos2ControlExecutionRuntimeManifestArtifactRole,
         detail::kRos2ControlExecutionRuntimeManifestMediaType, true},
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
  if (value.empty() || value.size() > detail::kMaximumPortableIdentityBytes ||
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
           return (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f');
         });
}

[[nodiscard]] const CanonicalValue::Object &
require_object(const CanonicalValue &value, std::string_view field) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    detail::fail_retained_bundle(std::string(field) + " must be an object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array &
require_array(const CanonicalValue &value, std::string_view field) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    detail::fail_retained_bundle(std::string(field) + " must be an array");
  }
  return *array;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view field) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    detail::fail_retained_bundle(std::string(field) + " must be a string");
  }
  return *text;
}

[[nodiscard]] const CanonicalValue *
find_member(const CanonicalValue::Object &object, std::string_view name) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  return found == object.end() ? nullptr : &found->second;
}

[[nodiscard]] const CanonicalValue &
require_member(const CanonicalValue::Object &object, std::string_view name,
               std::string_view field) {
  const auto found =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  if (found == object.end()) {
    detail::fail_retained_bundle(std::string(field) +
                                 " is missing required member " +
                                 std::string(name));
  }
  return found->second;
}

void require_exact_fields(const CanonicalValue::Object &object,
                          std::initializer_list<std::string_view> expected,
                          std::string_view field) {
  if (object.size() != expected.size()) {
    detail::fail_retained_bundle(std::string(field) +
                                 " has an incorrect closed field count");
  }
  for (const auto &[name, unused] : object) {
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

void require_required_and_optional_fields(
    const CanonicalValue::Object &object,
    std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional, std::string_view field) {
  for (const auto &[name, unused] : object) {
    (void)unused;
    if (std::find(required.begin(), required.end(), name) == required.end() &&
        std::find(optional.begin(), optional.end(), name) == optional.end()) {
      detail::fail_retained_bundle(std::string(field) +
                                   " contains unknown member " + name);
    }
  }
  for (const std::string_view name : required) {
    (void)require_member(object, name, field);
  }
}

[[nodiscard]] std::uint64_t require_i_json_size(const CanonicalValue &value,
                                                std::string_view field) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      std::trunc(*number) != *number ||
      *number > static_cast<double>(detail::kMaximumIJsonInteger)) {
    detail::fail_retained_bundle(std::string(field) +
                                 " must be a non-negative I-JSON safe integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] CanonicalValue artifact_role_projection(
    const detail::QualifiedRetainedProductArtifactRole &role) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(std::string(role.artifact_role_id))},
      {"media_type", CanonicalValue(std::string(role.media_type))},
      {"cardinality", CanonicalValue(role.required ? "required_exactly_one"
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
  for (const auto &role : artifact_roles) {
    roles.push_back(artifact_role_projection(role));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kProductContractApiVersion))},
      {"product_contract_id", CanonicalValue(std::string(contract_id))},
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

[[nodiscard]] const QualifiedProductContractState &
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
  if (contract_id == kViewerModelContractId &&
      product_kind == core::RetainedProductKind::kViewerModel &&
      placement == kViewerModelPlacement &&
      artifact_roles.data() == kViewerModelArtifactRoles.data() &&
      artifact_roles.size() == kViewerModelArtifactRoles.size()) {
    static const QualifiedProductContractState state = build_state();
    return state;
  }
  if (contract_id == detail::kControlDomainControlContractId &&
      product_kind == core::RetainedProductKind::kControlDomainControl &&
      placement == kControlDomainControlPlacement &&
      artifact_roles.data() == kControlDomainControlArtifactRoles.data() &&
      artifact_roles.size() == kControlDomainControlArtifactRoles.size()) {
    static const QualifiedProductContractState state = build_state();
    return state;
  }
  if (contract_id == detail::kReferenceRuntimeProductContractId &&
      product_kind == core::RetainedProductKind::kRuntimeUnitConfiguration &&
      placement == kReferenceRuntimePlacement &&
      artifact_roles.data() == kReferenceRuntimeArtifactRoles.data() &&
      artifact_roles.size() == kReferenceRuntimeArtifactRoles.size()) {
    static const QualifiedProductContractState state = build_state();
    return state;
  }
  if (contract_id == detail::kRos2ControlExecutionRuntimeProductContractId &&
      product_kind == core::RetainedProductKind::kRuntimeUnitConfiguration &&
      placement == kReferenceRuntimePlacement &&
      artifact_roles.data() ==
          kRos2ControlExecutionRuntimeArtifactRoles.data() &&
      artifact_roles.size() ==
          kRos2ControlExecutionRuntimeArtifactRoles.size()) {
    static const QualifiedProductContractState state = build_state();
    return state;
  }
  if (contract_id == detail::kComposedModelSymbolRoleMapContractId &&
      product_kind == core::RetainedProductKind::kSymbolRoleMap &&
      placement == kSymbolRoleMapPlacement &&
      artifact_roles.data() ==
          kComposedModelSymbolRoleMapArtifactRoles.data() &&
      artifact_roles.size() ==
          kComposedModelSymbolRoleMapArtifactRoles.size()) {
    static const QualifiedProductContractState state = build_state();
    return state;
  }
  throw std::logic_error("unknown built-in product contract state");
}

[[nodiscard]] CanonicalValue
retained_artifact_projection(const core::RetainedArtifactRefDTO &artifact) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(artifact.artifact_role_id.value())},
      {"media_type", CanonicalValue(artifact.media_type)},
      {"byte_size", CanonicalValue(static_cast<double>(artifact.byte_size))},
      {"content_digest", CanonicalValue(artifact.content_digest.value())},
      {"content_addressed_store_key",
       CanonicalValue(artifact.content_addressed_store_key.value())},
  });
}

[[nodiscard]] std::string
domain_separated_canonical_digest(std::string_view domain,
                                  std::string_view schema_tag,
                                  const CanonicalValue &value) {
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

[[nodiscard]] core::RetainedProductDTO
parse_retained_product(const CanonicalValue &value, std::string_view field,
                       core::RetainedProductKind expected_kind) {
  const auto &object = require_object(value, field);
  require_exact_fields(object,
                       {"product_id", "product_kind", "product_contract_id",
                        "product_contract_revision", "artifact_refs"},
                       field);

  const std::string &product_id =
      require_string(require_member(object, "product_id", field),
                     std::string(field) + ".product_id");
  const std::string &product_kind =
      require_string(require_member(object, "product_kind", field),
                     std::string(field) + ".product_kind");
  if (product_kind != core::to_string(expected_kind)) {
    detail::fail_retained_bundle(std::string(field) +
                                 " has an invalid product_kind");
  }
  const std::string &contract_id =
      require_string(require_member(object, "product_contract_id", field),
                     std::string(field) + ".product_contract_id");
  const std::string &contract_revision =
      require_string(require_member(object, "product_contract_revision", field),
                     std::string(field) + ".product_contract_revision");
  detail::require_retained_digest(contract_revision,
                                  "product_contract_revision");

  std::vector<core::RetainedArtifactRefDTO> artifacts;
  const auto &artifact_values =
      require_array(require_member(object, "artifact_refs", field),
                    std::string(field) + ".artifact_refs");
  artifacts.reserve(artifact_values.size());
  for (std::size_t index = 0U; index < artifact_values.size(); ++index) {
    const std::string path =
        std::string(field) + ".artifact_refs[" + std::to_string(index) + "]";
    const auto &artifact = require_object(artifact_values[index], path);
    require_exact_fields(artifact,
                         {"artifact_role_id", "media_type", "byte_size",
                          "content_digest", "content_addressed_store_key"},
                         path);
    artifacts.push_back(core::RetainedArtifactRefDTO{
        core::ArtifactRoleId(
            require_string(require_member(artifact, "artifact_role_id", path),
                           path + ".artifact_role_id")),
        require_string(require_member(artifact, "media_type", path),
                       path + ".media_type"),
        require_i_json_size(require_member(artifact, "byte_size", path),
                            path + ".byte_size"),
        core::ContentDigest(
            require_string(require_member(artifact, "content_digest", path),
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
    detail::fail_retained_bundle(std::string(field) +
                                 " differs from its exact typed projection");
  }
  return product;
}

[[nodiscard]] GraphRevisionVerifiedCellGraph verify_structural_graph_projection(
    const CanonicalValue &graph_value,
    std::span<const QualifiedRuntimeProfile> runtime_profiles) {
  const auto typed = decode_cell_graph_candidate(
      parse_strict_json_model_yaml(canonical_json(graph_value)));
  const auto intrinsic = validate_intrinsic_cell_graph_candidate(typed);
  const auto ready = prepare_cell_graph_identity(intrinsic, runtime_profiles);
  const auto verified = verify_graph_revision(ready);
  if (verified.normalized_root() != graph_value) {
    detail::fail_retained_bundle(
        "retained cell_graph differs from its exact verified projection");
  }
  const auto &graph = verified.graph();
  // A mounted child takes its placement from its one attachment edge; the
  // intrinsic graph validation already proves it omits a root pose.
  std::set<core::ControllableComponentId> mounted;
  for (const auto &attachment : graph.component_attachments)
    mounted.insert(attachment.child.controllable_component_id);
  const bool passive = graph.controllable_components.empty() &&
                       graph.control_domains.empty() &&
                       graph.control_bindings.empty();
  if (passive) {
    if (std::any_of(graph.model_instances.begin(), graph.model_instances.end(),
                    [](const core::ModelInstanceDTO &instance) {
                      return !instance.root_pose_in_cell.has_value();
                    })) {
      detail::fail_retained_bundle(
          "passive retained model instances require explicit placement");
    }
    return verified;
  }
  const bool has_controller_domain = std::any_of(
      graph.control_domains.begin(), graph.control_domains.end(),
      [](const auto &domain) { return domain.provider() == nullptr; });
  if (graph.controllable_components.empty() || graph.control_domains.empty() ||
      (has_controller_domain &&
       (graph.model_instances.empty() || graph.control_bindings.empty()))) {
    detail::fail_retained_bundle(
        "retained native control graph requires non-empty model, component, "
        "domain, and binding tables");
  }

  std::set<core::RuntimeUnitId> execution_runtime_units;
  for (const auto &unit : graph.runtime_units) {
    const bool ros2_control_execution =
        unit.runtime_profile_id.value() ==
            detail::kRos2ControlExecutionRuntimeProfileId &&
        unit.runtime_profile_revision.value() ==
            detail::ros2_control_execution_runtime_profile_revision() &&
        detail::ros2_control_execution_runtime_configuration_is_qualified(
            unit.configuration);
    if (!ros2_control_execution ||
        !unit.dependency_runtime_unit_ids.empty() ||
        !execution_runtime_units.insert(unit.runtime_unit_id).second) {
      detail::fail_retained_bundle(
          "retained control graph has an unqualified execution "
          "runtime");
    }
  }
  const bool has_execution_runtime = !execution_runtime_units.empty();

  std::set<core::ModelInstanceId> owned_models;
  for (const auto &component : graph.controllable_components) {
    const bool model_free_provider =
        component.model_role_bindings.empty() &&
        std::any_of(graph.control_domains.begin(), graph.control_domains.end(),
                    [&](const auto &domain) {
                      return domain.provider() &&
                             domain.provider()->controllable_component_id ==
                                 component.controllable_component_id;
                    });
    if ((!model_free_provider &&
         ((!component.root_pose_in_cell.has_value() &&
           !mounted.contains(component.controllable_component_id)) ||
          component.model_role_bindings.empty())) ||
        !has_execution_runtime || component.runtime_unit_ids.empty() ||
        !std::all_of(component.runtime_unit_ids.begin(),
                     component.runtime_unit_ids.end(),
                     [&](const auto &runtime_unit_id) {
                       return execution_runtime_units.contains(runtime_unit_id);
                     })) {
      detail::fail_retained_bundle(
          "retained native component requires a direct cell placement, at "
          "least one model role, and qualified execution-runtime owners");
    }
    for (const auto &role : component.model_role_bindings) {
      const auto model = std::find_if(
          graph.model_instances.begin(), graph.model_instances.end(),
          [&role](const core::ModelInstanceDTO &value) {
            return value.model_instance_id == role.model_instance_id;
          });
      if (model == graph.model_instances.end() ||
          model->root_pose_in_cell.has_value() ||
          !owned_models.insert(role.model_instance_id).second) {
        detail::fail_retained_bundle(
            "retained native component model roles must own distinct, "
            "component-placed model instances");
      }
    }
  }
  for (const auto &model : graph.model_instances) {
    if (!owned_models.contains(model.model_instance_id) &&
        !model.root_pose_in_cell.has_value()) {
      detail::fail_retained_bundle(
          "retained passive model beside native components requires an "
          "explicit cell placement");
    }
  }

  for (const auto &domain : graph.control_domains) {
    if (!has_execution_runtime || domain.runtime_unit_ids.empty() ||
        !std::all_of(domain.runtime_unit_ids.begin(),
                     domain.runtime_unit_ids.end(),
                     [&](const auto &runtime_unit_id) {
                       return execution_runtime_units.contains(runtime_unit_id);
                     })) {
      detail::fail_retained_bundle(
          "retained native control domain has no qualified execution-runtime "
          "owner set");
    }
  }
  for (const auto &binding : graph.control_bindings) {
    const auto component =
        std::find_if(graph.controllable_components.begin(),
                     graph.controllable_components.end(),
                     [&binding](const core::ControllableComponentDTO &value) {
                       return value.controllable_component_id ==
                              binding.controllable_component_id;
                     });
    const auto domain = std::find_if(
        graph.control_domains.begin(), graph.control_domains.end(),
        [&binding](const core::ControlDomainDTO &value) {
          return value.control_domain_id == binding.control_domain_id;
        });
    if (component == graph.controllable_components.end() ||
        domain == graph.control_domains.end() || domain->provider() ||
        binding.resource_bindings.empty() ||
        binding.legal_control_route_ids.empty() ||
        binding.route_endpoint_projections.size() !=
            binding.legal_control_route_ids.size() ||
        binding.controller_topology_id !=
            domain->controller().controller_topology_id ||
        binding.topology_revision != domain->controller().topology_revision ||
        !execution_runtime_units.contains(binding.provider_runtime_unit_id) ||
        std::find(component->runtime_unit_ids.begin(),
                  component->runtime_unit_ids.end(),
                  binding.provider_runtime_unit_id) ==
            component->runtime_unit_ids.end() ||
        std::find(domain->runtime_unit_ids.begin(),
                  domain->runtime_unit_ids.end(),
                  binding.provider_runtime_unit_id) ==
            domain->runtime_unit_ids.end()) {
      detail::fail_retained_bundle(
          "retained native control binding violates its exact component, "
          "route, or domain closure");
    }
  }
  return verified;
}

[[nodiscard]] std::vector<core::CatalogRecordRefDTO>
parse_catalog_records_projection(const CanonicalValue &records_value) {
  const CanonicalValue catalogs(CanonicalValue::Object{
      {"records", records_value},
  });
  std::vector<core::CatalogRecordRefDTO> records;
  try {
    records = detail::decode_deployment_catalogs(catalogs);
  } catch (const detail::DeploymentCatalogCodecError &error) {
    detail::fail_retained_bundle(
        std::string("retained catalog record table is invalid: ") +
        error.what());
  }
  const CanonicalValue encoded = detail::encode_deployment_catalogs(records);
  const CanonicalValue *encoded_records = encoded.find_object_member("records");
  if (encoded_records == nullptr || *encoded_records != records_value) {
    detail::fail_retained_bundle(
        "retained catalog records differ from their exact typed projection");
  }
  return records;
}

using CatalogIdentity =
    std::tuple<core::CatalogRecordKind, std::string, std::string>;

void require_structural_catalog_closure(
    const core::CellGraphDTO &graph,
    std::span<const core::CapabilityProviderBindingDTO> provider_bindings,
    std::span<const core::ExternalResourceDTO> external_resources,
    std::span<const core::CatalogRecordRefDTO> retained_policy_records,
    std::span<const core::CatalogRecordRefDTO> records) {
  std::set<CatalogIdentity> graph_records;
  for (const auto &instance : graph.model_instances) {
    graph_records.emplace(core::CatalogRecordKind::kModelAsset,
                          instance.model_asset_id.value(),
                          instance.model_revision.value());
  }
  for (const auto &unit : graph.runtime_units) {
    graph_records.emplace(core::CatalogRecordKind::kRuntimeProfile,
                          unit.runtime_profile_id.value(),
                          unit.runtime_profile_revision.value());
  }
  for (const auto &component : graph.controllable_components) {
    graph_records.emplace(core::CatalogRecordKind::kComponentType,
                          component.component_type_id.value(),
                          component.component_type_revision.value());
  }
  for (const auto &domain : graph.control_domains) {
    if (const auto *provider = domain.provider()) {
      graph_records.emplace(core::CatalogRecordKind::kProviderManifest,
                            provider->provider_manifest_id.value(),
                            provider->provider_manifest_revision.value());
    } else {
      graph_records.emplace(core::CatalogRecordKind::kControllerTopology,
                            domain.controller().controller_topology_id.value(),
                            domain.controller().topology_revision.value());
    }
  }
  for (const auto &binding : graph.control_bindings) {
    graph_records.emplace(core::CatalogRecordKind::kCapabilityDefinition,
                          binding.capability_id.value(),
                          binding.capability_definition_revision.value());
    graph_records.emplace(core::CatalogRecordKind::kControlInterfaceContract,
                          binding.control_interface_contract_id.value(),
                          binding.control_interface_revision.value());
    graph_records.emplace(core::CatalogRecordKind::kControllerTopology,
                          binding.controller_topology_id.value(),
                          binding.topology_revision.value());
    graph_records.emplace(core::CatalogRecordKind::kProviderManifest,
                          binding.provider_manifest_id.value(),
                          binding.provider_manifest_revision.value());
    graph_records.emplace(core::CatalogRecordKind::kStopEvidencePolicy,
                          binding.stop_evidence_policy_id.value(),
                          binding.stop_evidence_policy_revision.value());
    if (binding.capability_id.value() == "tool.gripper.command") {
      graph_records.emplace(
          core::CatalogRecordKind::kCommandSchema,
          std::string(core::kGripperPositionCommandSchemaId),
          std::string(core::kGripperPositionCommandSchemaRevision));
    }
  }
  for (const auto &binding : provider_bindings) {
    if (binding.capability_id.value() == "io.gpio.set_output")
      graph_records.emplace(
          core::CatalogRecordKind::kCommandSchema,
          std::string(core::kIoSignalWriteCommandSchemaId),
          std::string(core::kIoSignalWriteCommandSchemaRevision));
    graph_records.emplace(core::CatalogRecordKind::kCapabilityDefinition,
                          binding.capability_id.value(),
                          binding.capability_definition_revision.value());
    graph_records.emplace(core::CatalogRecordKind::kProviderManifest,
                          binding.provider_manifest_id.value(),
                          binding.provider_manifest_revision.value());
  }
  for (const auto &resource : external_resources) {
    graph_records.emplace(core::CatalogRecordKind::kProviderManifest,
                          resource.provider_manifest_id.value(),
                          resource.provider_manifest_revision.value());
  }
  for (const auto &policy : retained_policy_records) {
    graph_records.emplace(core::catalog_record_kind(policy),
                          core::catalog_record_id_value(policy),
                          core::catalog_record_revision_value(policy));
    if (std::find(records.begin(), records.end(), policy) == records.end()) {
      detail::fail_retained_bundle(
          "retained policy selector differs from or is absent in the exact "
          "catalog closure");
    }
  }

  std::set<CatalogIdentity> catalog_records;
  for (const auto &record : records) {
    const CatalogIdentity identity{core::catalog_record_kind(record),
                                   core::catalog_record_id_value(record),
                                   core::catalog_record_revision_value(record)};
    if (!catalog_records.insert(identity).second) {
      detail::fail_retained_bundle(
          "retained structural catalogs contain a duplicate identity");
    }
  }
  for (const auto &required : graph_records) {
    if (!catalog_records.contains(required)) {
      detail::fail_retained_bundle(
          "retained catalogs omit a graph/applicability-selected structural "
          "referent");
    }
  }
  for (const auto &selected : catalog_records) {
    if (!graph_records.contains(selected) &&
        std::get<0>(selected) !=
            core::CatalogRecordKind::kCapabilityDefinition) {
      detail::fail_retained_bundle(
          "retained catalogs contain a non-capability referent outside the "
          "exact graph/applicability closure");
    }
  }
}

[[nodiscard]] std::vector<core::RetainedSchemaArtifactRefDTO>
parse_structural_schema_artifacts(
    const CanonicalValue &value,
    std::span<const core::CatalogRecordRefDTO> records) {
  using SchemaIdentity = std::pair<std::string, std::string>;
  std::map<SchemaIdentity, core::RetainedSchemaArtifactRefDTO> expected_schemas;
  const auto &registry = build_qualified_closed_schema_registry();
  for (const auto &record : records) {
    const auto kind = core::catalog_record_kind(record);
    const auto schema_id =
        std::visit([](const auto &typed) { return typed.schema_id; }, record);
    const auto schema_revision = std::visit(
        [](const auto &typed) { return typed.schema_revision; }, record);
    const auto *schema = registry.find_qualified_catalog_schema(
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
    const SchemaIdentity identity{schema_id.value(), schema_revision.value()};
    const auto [position, inserted] =
        expected_schemas.emplace(identity, expected);
    if (!inserted && position->second != expected) {
      detail::fail_retained_bundle(
          "one retained schema identity resolved to unequal build "
          "descriptors");
    }
  }

  const auto &rows =
      require_array(value, "retained_bundle.catalogs.schema_artifacts");
  std::vector<core::RetainedSchemaArtifactRefDTO> parsed;
  parsed.reserve(rows.size());
  std::optional<SchemaIdentity> previous_identity;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = "retained_bundle.catalogs.schema_artifacts[" +
                             std::to_string(index) + "]";
    const auto &row = require_object(rows[index], path);
    require_exact_fields(row,
                         {"schema_id", "schema_revision", "content_digest",
                          "content_addressed_store_key"},
                         path);
    const std::string &schema_id = require_string(
        require_member(row, "schema_id", path), path + ".schema_id");
    const std::string &schema_revision =
        require_string(require_member(row, "schema_revision", path),
                       path + ".schema_revision");
    const std::string &content_digest = require_string(
        require_member(row, "content_digest", path), path + ".content_digest");
    const std::string &store_key =
        require_string(require_member(row, "content_addressed_store_key", path),
                       path + ".content_addressed_store_key");
    detail::require_retained_portable_identity(schema_id, path + ".schema_id");
    detail::require_retained_digest(schema_revision, path + ".schema_revision");
    detail::require_retained_digest(content_digest, path + ".content_digest");
    if (store_key != detail::retained_store_key_for_digest(content_digest)) {
      detail::fail_retained_bundle(
          path + " does not use a digest-derived retained CAS key");
    }
    const SchemaIdentity identity{schema_id, schema_revision};
    if (previous_identity.has_value() && *previous_identity >= identity) {
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
  for (const auto &schema : parsed) {
    const auto expected = expected_schemas.find(
        {schema.schema_id.value(), schema.schema_revision.value()});
    if (expected == expected_schemas.end() || expected->second != schema) {
      detail::fail_retained_bundle(
          "retained schema artifact differs from its build-qualified "
          "descriptor");
    }
  }
  return parsed;
}

[[nodiscard]] std::vector<core::RetainedSourceRecordDTO>
parse_structural_source_records(
    const CanonicalValue &value,
    std::span<const core::CatalogRecordRefDTO> records) {
  std::vector<core::RetainedSourceRecordDTO> expected;
  expected.reserve(records.size());
  for (const auto &record : records) {
    expected.push_back(std::visit(
        [](const auto &typed) -> core::RetainedSourceRecordDTO {
          using Record = std::remove_cvref_t<decltype(typed)>;
          using Source =
              core::TypedRetainedSourceRecordDTO<Record::kRecordKind,
                                                 typename Record::IdType,
                                                 typename Record::RevisionType>;
          return Source{typed.record_id, typed.record_revision};
        },
        record));
  }

  const auto &rows =
      require_array(value, "retained_bundle.provenance.source_records");
  if (rows.size() != expected.size()) {
    detail::fail_retained_bundle(
        "retained source provenance is not the exact structural catalog "
        "closure");
  }
  std::vector<core::RetainedSourceRecordDTO> parsed;
  parsed.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = "retained_bundle.provenance.source_records[" +
                             std::to_string(index) + "]";
    const auto &row = require_object(rows[index], path);
    require_exact_fields(row, {"record_kind", "record_id", "revision"}, path);
    const std::string &record_kind = require_string(
        require_member(row, "record_kind", path), path + ".record_kind");
    const std::string &record_id = require_string(
        require_member(row, "record_id", path), path + ".record_id");
    const std::string &revision = require_string(
        require_member(row, "revision", path), path + ".revision");
    detail::require_retained_portable_identity(record_id, path + ".record_id");
    detail::require_retained_digest(revision, path + ".revision");
    const auto &catalog_record = records[index];
    if (record_kind !=
            core::to_string(core::catalog_record_kind(catalog_record)) ||
        record_id != core::catalog_record_id_value(catalog_record) ||
        revision != core::catalog_record_revision_value(catalog_record)) {
      detail::fail_retained_bundle(
          "retained source provenance differs from the exact catalog "
          "projection");
    }
    parsed.push_back(expected[index]);
  }
  return parsed;
}

[[nodiscard]] std::vector<core::RetainedGeneratorRecordDTO>
parse_structural_generator_records(const CanonicalValue &value,
                                   std::size_t selected_asset_count) {
  const auto &rows =
      require_array(value, "retained_bundle.provenance.generator_records");
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
    const std::string path = "retained_bundle.provenance.generator_records[" +
                             std::to_string(index) + "]";
    const auto &row = require_object(rows[index], path);
    require_exact_fields(row,
                         {"generator_id", "generator_version", "build_digest",
                          "configuration_digest"},
                         path);
    const std::string &generator_id = require_string(
        require_member(row, "generator_id", path), path + ".generator_id");
    const std::string &generator_version =
        require_string(require_member(row, "generator_version", path),
                       path + ".generator_version");
    const std::string &build_digest = require_string(
        require_member(row, "build_digest", path), path + ".build_digest");
    const std::string &configuration_digest =
        require_string(require_member(row, "configuration_digest", path),
                       path + ".configuration_digest");
    if (generator_id != kStructuralModelPreparationGeneratorId ||
        generator_version != kStructuralModelPreparationGeneratorVersion) {
      detail::fail_retained_bundle(
          path + " selects an unqualified structural generator");
    }
    detail::require_retained_digest(build_digest, path + ".build_digest");
    detail::require_retained_digest(configuration_digest,
                                    path + ".configuration_digest");
    Identity identity{generator_id, generator_version, build_digest,
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

void verify_node_environment_projection(const CanonicalValue &value) {
  const auto &object = require_object(value, "catalogs.node_environment");
  require_exact_fields(
      object,
      {"catalog_id", "content_version", "catalog_digest", "environment_digest"},
      "catalogs.node_environment");
  detail::require_retained_portable_identity(
      require_string(
          require_member(object, "catalog_id", "catalogs.node_environment"),
          "catalogs.node_environment.catalog_id"),
      "catalogs.node_environment.catalog_id");
  detail::require_retained_portable_identity(
      require_string(require_member(object, "content_version",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.content_version"),
      "catalogs.node_environment.content_version");
  detail::require_retained_digest(
      require_string(
          require_member(object, "catalog_digest", "catalogs.node_environment"),
          "catalogs.node_environment.catalog_digest"),
      "catalogs.node_environment.catalog_digest");
  detail::require_retained_digest(
      require_string(require_member(object, "environment_digest",
                                    "catalogs.node_environment"),
                     "catalogs.node_environment.environment_digest"),
      "catalogs.node_environment.environment_digest");
}

// Normalized hard profiles contain explicit false flags that the authored
// source grammar deliberately forbids spelling. Reconstruct only that source
// omission; the caller requires exact normalization back to the retained rows.
CanonicalValue interrupt_policy_source_rows(const CanonicalValue &rows) {
  auto policies = rows.as_array();
  for (auto &policy : policies) {
    const auto *profile = policy.find_object_member("stop_profile");
    if (profile == nullptr || !profile->is_object())
      continue;
    const auto *mode = profile->find_object_member("stop_mode");
    if (mode == nullptr || !mode->is_string() ||
        mode->as_string() != "hard_deceleration")
      continue;
    auto fields = profile->as_object();
    fields.erase(std::remove_if(
                     fields.begin(), fields.end(),
                     [](const auto &field) {
                       return (field.first ==
                                   "allow_natural_completion_on_short_path" ||
                               field.first == "jerk_limits_enabled") &&
                              field.second.is_bool() && !field.second.as_bool();
                     }),
                 fields.end());
    auto members = policy.as_object();
    for (auto &[name, value] : members)
      if (name == "stop_profile")
        value = CanonicalValue(std::move(fields));
    policy = CanonicalValue(std::move(members));
  }
  return CanonicalValue(std::move(policies));
}

void require_retained_interrupt_policy_references(
    const CanonicalValue::Object &interrupts, const core::CellGraphDTO &graph,
    const std::vector<core::CapabilityProviderBindingDTO> &providers) {
  std::map<std::string, std::set<std::string>> known;
  for (const auto &component : graph.controllable_components)
    known["controllable_component_ids"].insert(
        component.controllable_component_id.value());
  for (const auto &domain : graph.control_domains)
    known["control_domain_ids"].insert(domain.control_domain_id.value());
  for (const auto &binding : graph.control_bindings) {
    known["control_binding_ids"].insert(binding.control_binding_id.value());
    known["command_target_ids"].insert(binding.command_target_id.value());
    for (const auto &resource : binding.resource_bindings)
      known["resource_ids"].insert(resource.resource_id.value());
  }
  for (const auto &provider : providers) {
    if (const auto *target =
            std::get_if<core::ComponentTargetCapabilityProviderTargetDTO>(
                &provider.target))
      known["command_target_ids"].insert(target->command_target_id.value());
  }
  const auto signals = ResolveRetainedIoSignalCatalog(graph);
  if (!signals.ok())
    detail::fail_retained_bundle(signals.error().message);
  for (const auto &signal : signals.value()) {
    known["command_target_ids"].insert(signal.command_target_id.value());
    if (signal.descriptor.output_resource)
      known["resource_ids"].insert(signal.descriptor.output_resource->value());
  }
  const auto verify_condition = [&](auto &&self,
                                    const CanonicalValue &condition) -> void {
    if (const auto *compare = condition.find_object_member("compare")) {
      const auto &operand = *compare->find_object_member("operand");
      if (const auto *signal = operand.find_object_member("signal")) {
        const auto &component =
            signal->find_object_member("controllable_component_id")
                ->as_string();
        const auto &name =
            signal->find_object_member("signal_name")->as_string();
        const auto found = std::find_if(
            signals.value().begin(), signals.value().end(),
            [&](const auto &row) {
              return row.descriptor.signal_id.controllable_component_id.value() ==
                         component &&
                     row.descriptor.signal_id.name == name;
            });
        if (found == signals.value().end() ||
            found->descriptor.direction != core::IoSignalDirection::kInput)
          detail::fail_retained_bundle(
              "interrupt condition has no exact retained input signal");
      }
      return;
    }
    if (const auto *child = condition.find_object_member("not")) {
      self(self, *child);
      return;
    }
    const auto *children = condition.find_object_member("all");
    if (children == nullptr)
      children = condition.find_object_member("any");
    for (const auto &child : children->as_array())
      self(self, child);
  };
  for (const auto &policy :
       require_member(interrupts, "interrupt_policies", "interrupts")
           .as_array()) {
    for (const std::string_view field :
         {"controllable_component_ids", "command_target_ids",
          "control_domain_ids", "control_binding_ids", "resource_ids"}) {
      for (const auto &value : policy.find_object_member(field)->as_array())
        if (!known[std::string(field)].contains(value.as_string()))
          detail::fail_retained_bundle(
              "interrupt policy references unknown retained " +
              std::string(field));
    }
    if (const auto *condition = policy.find_object_member("condition"))
      verify_condition(verify_condition, *condition);
  }
  if (const auto *rules = find_member(interrupts, "interrupt_rules"))
    for (const auto &rule : rules->as_array())
      verify_condition(verify_condition, *rule.find_object_member("condition"));
}

[[nodiscard]] RevisionVerifiedDeploymentApplicabilityInputs
verify_structural_applicability_projection(
    const CanonicalValue &desired_value, const CanonicalValue &provider_value,
    const CanonicalValue &retained_policy_records_value,
    const CanonicalValue *interrupt_policy_configuration,
    const CanonicalValue *site_overlay_ref_value,
    const CanonicalValue *external_resources_value,
    const core::CellGraphDTO &graph,
    std::span<const QualifiedRuntimeProfile> runtime_profiles) {
  const CanonicalValue::Object *interrupts = nullptr;
  if (interrupt_policy_configuration != nullptr) {
    interrupts =
        &require_object(*interrupt_policy_configuration,
                        "retained_bundle.interrupt_policy_configuration");
    auto legacy_fields = *interrupts;
    std::erase_if(legacy_fields, [](const auto &field) {
      return field.first == "interrupt_rules";
    });
    require_exact_fields(
        legacy_fields,
        {"interrupt_sources", "interrupt_policies", "interrupt_config"},
        "retained_bundle.interrupt_policy_configuration");
    const auto &sources = require_array(
        require_member(*interrupts, "interrupt_sources", "interrupts"),
        "interrupt_sources");
    const auto &policies = require_array(
        require_member(*interrupts, "interrupt_policies", "interrupts"),
        "interrupt_policies");
    const auto *rules = find_member(*interrupts, "interrupt_rules");
    if (sources.empty() && policies.empty() &&
        (rules == nullptr || require_array(*rules, "interrupt_rules").empty()))
      detail::fail_retained_bundle(
          "empty interrupt_policy_configuration must be omitted");
  }
  CanonicalValue::Object root_members{
      {"api_version", CanonicalValue("manyforge.deployment.v1")},
      {"desired_control_configurations", desired_value},
      {"capability_provider_bindings", provider_value},
      {"policies",
       CanonicalValue(CanonicalValue::Object{
           {"retained_policy_records", retained_policy_records_value},
           {"stop_stage_parameters", CanonicalValue(CanonicalValue::Array{})},
           {"commissioned_stop_interfaces",
            CanonicalValue(CanonicalValue::Array{})},
           {"interrupt_sources",
            interrupts == nullptr
                ? CanonicalValue(CanonicalValue::Array{})
                : require_member(*interrupts, "interrupt_sources",
                                 "interrupts")},
           {"interrupt_policies",
            interrupts == nullptr
                ? CanonicalValue(CanonicalValue::Array{})
                : interrupt_policy_source_rows(require_member(
                      *interrupts, "interrupt_policies", "interrupts"))},
           {"interrupt_config",
            interrupts == nullptr
                ? CanonicalValue(CanonicalValue::Object{
                      {"queue_depth", CanonicalValue(8.0)},
                      {"cancel_validation_timeout_ns", CanonicalValue(1000.0)},
                      {"motion_scope_arm_timeout_ns", CanonicalValue(1000.0)},
                      {"supervisory_poll_rate_hz", CanonicalValue(10.0)},
                  })
                : require_member(*interrupts, "interrupt_config",
                                 "interrupts")},
       })},
  };
  if (interrupts != nullptr) {
    if (const auto *rules = find_member(*interrupts, "interrupt_rules")) {
      auto policies = root_members.back().second.as_object();
      policies.emplace_back("interrupt_rules", *rules);
      root_members.back().second = CanonicalValue(std::move(policies));
    }
  }
  if (site_overlay_ref_value != nullptr) {
    root_members.emplace_back("site_overlay_ref", *site_overlay_ref_value);
  }
  if (external_resources_value != nullptr) {
    root_members.emplace_back("external_resources", *external_resources_value);
  }
  const CanonicalValue root(std::move(root_members));
  RevisionVerifiedDeploymentApplicabilityInputs verified =
      compute_capability_provider_binding_revisions(
          decode_deployment_applicability_inputs_candidate(
              parse_strict_json_model_yaml(canonical_json(root))));
  if (site_overlay_ref_value != nullptr &&
      !verified.value().site_overlay_ref.has_value()) {
    detail::fail_retained_bundle(
        "retained site_overlay_ref must be a non-null selected reference");
  }
  if (external_resources_value != nullptr &&
      verified.value().external_resources.empty()) {
    detail::fail_retained_bundle(
        "retained external_resources must be omitted when empty");
  }
  if (!verified.value().external_resources.empty() &&
      !verified.value().site_overlay_ref.has_value()) {
    detail::fail_retained_bundle(
        "retained external resources require a selected site overlay");
  }
  if (verified.normalized_desired_control_configurations() != desired_value ||
      verified.normalized_capability_provider_bindings() != provider_value ||
      (external_resources_value != nullptr &&
       verified.normalized_external_resources() != *external_resources_value) ||
      (external_resources_value == nullptr &&
       !verified.value().external_resources.empty()) ||
      (site_overlay_ref_value != nullptr &&
       verified.normalized_site_overlay_ref() !=
           std::optional<CanonicalValue>(*site_overlay_ref_value)) ||
      (site_overlay_ref_value == nullptr &&
       verified.value().site_overlay_ref.has_value()) ||
      verified.value().retained_policy_records !=
          detail::decode_deployment_catalogs(
              CanonicalValue(CanonicalValue::Object{
                  {"records", retained_policy_records_value}}))) {
    detail::fail_retained_bundle(
        "retained applicability differs from its exact revision-verified "
        "projection");
  }
  if (interrupts != nullptr) {
    for (const std::string_view field :
         {"interrupt_sources", "interrupt_policies", "interrupt_config"}) {
      if (!verified.normalized_policies().has_value() ||
          verified.normalized_policies()->find_object_member(field) ==
              nullptr ||
          *verified.normalized_policies()->find_object_member(field) !=
              require_member(*interrupts, field, "interrupts"))
        detail::fail_retained_bundle(
            "retained interrupt policy normalization mismatch");
    }
  }
  if (interrupts != nullptr) {
    const auto *rules = find_member(*interrupts, "interrupt_rules");
    const auto *normalized_rules =
        verified.normalized_policies()->find_object_member("interrupt_rules");
    if ((rules == nullptr) != (normalized_rules == nullptr) ||
        (rules && *rules != *normalized_rules))
      detail::fail_retained_bundle(
          "retained global rule normalization mismatch");
  }
  if (interrupts != nullptr)
    require_retained_interrupt_policy_references(
        *interrupts, graph, verified.value().capability_provider_bindings);
  const auto &desired = verified.value().desired_control_configurations;
  const auto &providers = verified.value().capability_provider_bindings;
  if (graph.control_domains.empty()) {
    if (!desired.empty() || std::any_of(providers.begin(), providers.end(), [](const auto& provider) {
          return !std::holds_alternative<core::ComponentTargetCapabilityProviderTargetDTO>(provider.target);
        })) {
      detail::fail_retained_bundle(
          "passive structural retention cannot select a control "
          "configuration or capability provider");
    }
  } else if (desired.size() !=
                 static_cast<std::size_t>(std::count_if(
                     graph.control_domains.begin(), graph.control_domains.end(),
                     [](const auto &domain) {
                       return domain.provider() == nullptr;
                     })) ||
             (!desired.empty() && graph.control_bindings.empty())) {
    detail::fail_retained_bundle(
        "native structural retention requires one desired control "
        "configuration per control domain");
  }

  for (const auto &domain : graph.control_domains) {
    if (domain.provider()) {
      if (std::any_of(desired.begin(), desired.end(), [&](const auto &value) {
            return value.control_domain_id == domain.control_domain_id;
          }))
        detail::fail_retained_bundle(
            "provider domain cannot select controller configuration");
      continue;
    }
    const auto selected = std::find_if(
        desired.begin(), desired.end(), [&domain](const auto &value) {
          return value.control_domain_id == domain.control_domain_id;
        });
    const auto binding_count = static_cast<std::size_t>(std::count_if(
        graph.control_bindings.begin(), graph.control_bindings.end(),
        [&domain](const core::ControlBindingDTO &binding) {
          return binding.control_domain_id == domain.control_domain_id;
        }));
    if (selected == desired.end() || binding_count == 0U ||
        selected->control_domain_revision != domain.control_domain_revision ||
        selected->controller_topology_id != domain.controller().controller_topology_id ||
        selected->topology_revision != domain.controller().topology_revision ||
        selected->binding_routes.size() != binding_count) {
      detail::fail_retained_bundle(
          "retained desired control configuration is not the exact selected "
          "domain projection");
    }

    for (const auto &binding : graph.control_bindings) {
      if (binding.control_domain_id != domain.control_domain_id) {
        continue;
      }
      const auto route = std::find_if(
          selected->binding_routes.begin(), selected->binding_routes.end(),
          [&binding](const core::DesiredControlBindingRouteDTO &value) {
            return value.control_binding_id == binding.control_binding_id;
          });
      if (route == selected->binding_routes.end() ||
          route->control_binding_revision != binding.control_binding_revision ||
          std::find(binding.legal_control_route_ids.begin(),
                    binding.legal_control_route_ids.end(),
                    route->control_route_id) ==
              binding.legal_control_route_ids.end()) {
        detail::fail_retained_bundle(
            "retained desired control configuration is not the exact "
            "selected binding route");
      }
    }
  }

  for (const auto &provider : providers) {
    for (const auto &endpoint : provider.endpoint_bindings) {
      if (endpoint.source_kind ==
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        const auto *runtime_unit_id =
            std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
        const auto runtime_unit =
            runtime_unit_id == nullptr
                ? graph.runtime_units.end()
                : std::find_if(
                      graph.runtime_units.begin(), graph.runtime_units.end(),
                      [&](const auto &unit) {
                        return unit.runtime_unit_id == *runtime_unit_id;
                      });
        const auto runtime_profile =
            runtime_unit == graph.runtime_units.end()
                ? runtime_profiles.end()
                : std::find_if(
                      runtime_profiles.begin(), runtime_profiles.end(),
                      [&](const auto &profile) {
                        return profile.value().runtime_profile_id ==
                                   runtime_unit->runtime_profile_id &&
                               profile.value().runtime_profile_revision ==
                                   runtime_unit->runtime_profile_revision;
                      });
        if (runtime_unit_id == nullptr ||
            runtime_unit == graph.runtime_units.end() ||
            runtime_profile == runtime_profiles.end() ||
            !std::binary_search(
                runtime_profile->value().source_endpoint_role_ids.begin(),
                runtime_profile->value().source_endpoint_role_ids.end(),
                endpoint.source_endpoint_role_id)) {
          detail::fail_retained_bundle(
              "retained capability endpoint is outside its exact runtime "
              "unit/profile source closure");
        }
      } else {
        const auto *alias =
            std::get_if<core::ExternalResourceAlias>(&endpoint.source_id);
        const auto resource =
            alias == nullptr
                ? verified.value().external_resources.end()
                : std::find_if(verified.value().external_resources.begin(),
                               verified.value().external_resources.end(),
                               [&](const auto &candidate) {
                                 return candidate.external_resource_alias ==
                                        *alias;
                               });
        if (alias == nullptr ||
            resource == verified.value().external_resources.end() ||
            resource->endpoint_role_id != endpoint.source_endpoint_role_id) {
          detail::fail_retained_bundle(
              "retained capability endpoint differs from its exact external "
              "resource source");
        }
      }
    }
    std::visit(
        [&](const auto &target) {
          using Target = std::decay_t<decltype(target)>;
          if constexpr (std::is_same_v<
                            Target,
                            core::CellGlobalCapabilityProviderTargetDTO>) {
            if (provider.scope != core::CapabilityScope::kCellGlobal ||
                target.cell_id != graph.cell_id) {
              detail::fail_retained_bundle(
                  "retained cell-global capability provider does not target "
                  "the exact retained cell");
            }
          } else if constexpr (
              std::is_same_v<Target,
                             core::ControlDomainCapabilityProviderTargetDTO>) {
            const auto domain = std::find_if(
                graph.control_domains.begin(), graph.control_domains.end(),
                [&](const auto &value) {
                  return value.control_domain_id == target.control_domain_id;
                });
            if (provider.scope != core::CapabilityScope::kControlDomain ||
                domain == graph.control_domains.end() ||
                domain->control_domain_revision !=
                    target.control_domain_revision ||
                target.provider() != (domain->provider() != nullptr) ||
                (domain->provider()
                     ? (provider.provider_instance_id !=
                            domain->provider()->provider_instance_id ||
                        provider.provider_manifest_id !=
                            domain->provider()->provider_manifest_id ||
                        provider.provider_manifest_revision !=
                            domain->provider()->provider_manifest_revision)
                     : (domain->controller().controller_topology_id !=
                            target.controller().controller_topology_id ||
                        domain->controller().topology_revision !=
                            target.controller().topology_revision))) {
              detail::fail_retained_bundle(
                  "retained control-domain capability provider does not "
                  "target the exact retained domain");
            }
          } else if constexpr (std::is_same_v<Target, core::ComponentTargetCapabilityProviderTargetDTO>) {
            const auto bound = ValidateNonCoordinateProviderTarget(graph, provider);
            if (!bound.ok()) detail::fail_retained_bundle(bound.error().details.at("reason"));
          } else {
            const auto component = std::find_if(
                graph.controllable_components.begin(),
                graph.controllable_components.end(), [&](const auto &value) {
                  return value.controllable_component_id ==
                         target.controllable_component_id;
                });
            const auto binding = std::find_if(
                graph.control_bindings.begin(), graph.control_bindings.end(),
                [&](const auto &value) {
                  return value.control_binding_id == target.control_binding_id;
                });
            if (provider.scope != core::CapabilityScope::kTargetBinding ||
                component == graph.controllable_components.end() ||
                binding == graph.control_bindings.end() ||
                component->component_type_revision !=
                    target.component_type_revision ||
                binding->control_binding_revision !=
                    target.control_binding_revision ||
                binding->controllable_component_id !=
                    target.controllable_component_id ||
                binding->command_target_id != target.command_target_id) {
              detail::fail_retained_bundle(
                  "retained target capability provider does not target the "
                  "exact retained component and control-binding anchor");
            }
          }
        },
        provider.target);
  }
  return verified;
}

// CP9 stream parameter overrides: the exact normalized, non-empty Deployment
// table whose rows name retained controllable components.
void verify_retained_component_streams(const CanonicalValue &value,
                                       const core::CellGraphDTO &graph) {
  std::vector<core::StreamParameterOverrideDTO> rows;
  try {
    rows = decode_component_stream_overrides(value);
  } catch (const DeploymentApplicabilityCodecError &error) {
    detail::fail_retained_bundle(
        std::string("retained component_streams is invalid: ") +
        error.what());
  }
  if (rows.empty()) {
    detail::fail_retained_bundle(
        "retained component_streams must be omitted when empty");
  }
  if (encode_component_stream_overrides(rows) != value) {
    detail::fail_retained_bundle(
        "retained component_streams differs from its normalized projection");
  }
  for (const auto &row : rows) {
    if (std::none_of(graph.controllable_components.begin(),
                     graph.controllable_components.end(),
                     [&row](const auto &component) {
                       return component.controllable_component_id ==
                              row.controllable_component_id;
                     })) {
      detail::fail_retained_bundle(
          "retained component_streams references an unknown controllable "
          "component");
    }
  }
}

[[nodiscard]] core::RetainedBundleDTO
verify_current_retained_bundle_projection(const CanonicalValue &root) {
  const auto &object = require_object(root, "retained_bundle");
  require_required_and_optional_fields(
      object,
      {"api_version", "retained_bundle_id", "deployment_artifact_id",
       "deployment_revision", "cell_graph", "catalogs",
       "desired_control_configurations", "capability_provider_bindings",
       "retained_policy_records", "runtime_contract_requirements",
       "compiled_products", "provenance"},
      {"site_overlay_ref", "external_resources", "assistant",
       "motion_takeover_preparation_budget_s",
       "motion_takeover_planning_budget_s", "interrupt_policy_configuration",
       "scene_pose_sources", "component_streams"},
      "retained_bundle");
  if (require_string(require_member(object, "api_version", "retained_bundle"),
                     "retained_bundle.api_version") !=
      core::kRetainedBundleApiVersion) {
    detail::fail_retained_bundle(
        "retained_bundle.api_version is not the exact v1 API");
  }
  const std::string &retained_bundle_id = require_string(
      require_member(object, "retained_bundle_id", "retained_bundle"),
      "retained_bundle.retained_bundle_id");
  detail::require_retained_digest(retained_bundle_id,
                                  "retained_bundle.retained_bundle_id");
  const std::string &deployment_artifact_id = require_string(
      require_member(object, "deployment_artifact_id", "retained_bundle"),
      "retained_bundle.deployment_artifact_id");
  detail::require_retained_portable_identity(
      deployment_artifact_id, "retained_bundle.deployment_artifact_id");
  const std::string &deployment_revision = require_string(
      require_member(object, "deployment_revision", "retained_bundle"),
      "retained_bundle.deployment_revision");
  detail::require_retained_digest(deployment_revision,
                                  "retained_bundle.deployment_revision");
  std::optional<double> motion_takeover_preparation_budget_s;
  if (const auto *budget =
          root.find_object_member("motion_takeover_preparation_budget_s")) {
    const auto *seconds = std::get_if<double>(&budget->storage());
    if (seconds == nullptr || !std::isfinite(*seconds) || *seconds <= 0.0) {
      detail::fail_retained_bundle(
          "retained_bundle.motion_takeover_preparation_budget_s must be "
          "finite and positive");
    }
    motion_takeover_preparation_budget_s = *seconds;
  }
  std::optional<double> motion_takeover_planning_budget_s;
  if (const auto *budget =
          root.find_object_member("motion_takeover_planning_budget_s")) {
    const auto *seconds = std::get_if<double>(&budget->storage());
    if (seconds == nullptr || !std::isfinite(*seconds) || *seconds <= 0.0 ||
        !motion_takeover_preparation_budget_s ||
        *seconds >= *motion_takeover_preparation_budget_s) {
      detail::fail_retained_bundle(
          "retained_bundle.motion_takeover_planning_budget_s must be positive "
          "and less than the preparation budget");
    }
    motion_takeover_planning_budget_s = *seconds;
  }
  const auto &catalogs =
      require_object(require_member(object, "catalogs", "retained_bundle"),
                     "retained_bundle.catalogs");
  require_exact_fields(catalogs,
                       {"dialect_api_version", "bootstrap_contract_digest",
                        "qualification_registry_digest", "records",
                        "schema_artifacts", "node_environment"},
                       "retained_bundle.catalogs");
  const std::string &dialect_api_version =
      require_string(require_member(catalogs, "dialect_api_version",
                                    "retained_bundle.catalogs"),
                     "retained_bundle.catalogs.dialect_api_version");
  if (dialect_api_version != kClosedSchemaApiVersion) {
    detail::fail_retained_bundle(
        "retained catalog dialect does not match this build");
  }
  const std::string &bootstrap_contract_digest =
      require_string(require_member(catalogs, "bootstrap_contract_digest",
                                    "retained_bundle.catalogs"),
                     "retained_bundle.catalogs.bootstrap_contract_digest");
  if (bootstrap_contract_digest != kClosedSchemaBootstrapContractDigest) {
    detail::fail_retained_bundle(
        "retained bootstrap contract does not match this build; "
        "recompile the authored Deployment with the current ManyForge version");
  }
  const std::string &qualification_registry_digest =
      require_string(require_member(catalogs, "qualification_registry_digest",
                                    "retained_bundle.catalogs"),
                     "retained_bundle.catalogs.qualification_registry_digest");
  if (qualification_registry_digest != build_qualified_closed_schema_registry()
                                           .qualification_registry_digest()
                                           .value()) {
    detail::fail_retained_bundle(
        "retained qualification registry does not match this build; "
        "recompile the authored Deployment with the current ManyForge version");
  }
  const auto catalog_records = parse_catalog_records_projection(
      require_member(catalogs, "records", "retained_bundle.catalogs"));
  std::vector<QualifiedRuntimeProfile> runtime_profiles;
  try {
    for (const auto &record : catalog_records) {
      const auto *profile =
          std::get_if<core::RuntimeProfileCatalogRecordRefDTO>(&record);
      if (profile == nullptr) {
        continue;
      }
      std::string profile_document;
      if (profile->record_id.value() == detail::kReferenceRuntimeProfileId &&
          profile->record_revision.value() ==
              detail::reference_runtime_profile_revision()) {
        profile_document = detail::reference_runtime_profile_document();
      } else if (profile->record_id.value() ==
                     detail::kRos2ControlExecutionRuntimeProfileId &&
                 profile->record_revision.value() ==
                     detail::
                         ros2_control_execution_runtime_profile_revision()) {
        profile_document =
            detail::ros2_control_execution_runtime_profile_document();
      } else {
        if (profile->record_id.value() == detail::kRos2ControlExecutionRuntimeProfileId) {
          detail::fail_retained_bundle("sealed ROS runtime profile is not qualified by this build; "
              "recompile the authored Deployment with the current ManyForge version");
        }
        detail::fail_retained_bundle(
            "retained runtime profile has no build-qualified verifier");
      }
      runtime_profiles.push_back(resolve_and_qualify_runtime_profile(
          *profile, profile->content_addressed_store_key, profile_document));
    }
  } catch (const RuntimeProfileCodecError &error) {
    detail::fail_retained_bundle(
        std::string("retained runtime profile is not build-qualified: ") +
        error.what());
  }
  const auto verified_graph = verify_structural_graph_projection(
      require_member(object, "cell_graph", "retained_bundle"),
      runtime_profiles);
  if (const auto *streams = root.find_object_member("component_streams")) {
    verify_retained_component_streams(*streams, verified_graph.graph());
  }
  const auto applicability = verify_structural_applicability_projection(
      require_member(object, "desired_control_configurations",
                     "retained_bundle"),
      require_member(object, "capability_provider_bindings", "retained_bundle"),
      require_member(object, "retained_policy_records", "retained_bundle"),
      root.find_object_member("interrupt_policy_configuration"),
      root.find_object_member("site_overlay_ref"),
      root.find_object_member("external_resources"), verified_graph.graph(),
      runtime_profiles);
  const auto provider_projection = ValidateIoProviderDomainProjection(
      verified_graph.graph(),
      applicability.value().capability_provider_bindings, runtime_profiles);
  if (!provider_projection.ok())
    detail::fail_retained_bundle(
        provider_projection.error().details.at("reason"));
  require_structural_catalog_closure(
      verified_graph.graph(),
      applicability.value().capability_provider_bindings,
      applicability.value().external_resources,
      applicability.value().retained_policy_records, catalog_records);
  const auto schema_artifacts = parse_structural_schema_artifacts(
      require_member(catalogs, "schema_artifacts", "retained_bundle.catalogs"),
      catalog_records);
  const CanonicalValue &node_environment_value =
      require_member(catalogs, "node_environment", "retained_bundle.catalogs");
  verify_node_environment_projection(node_environment_value);
  const auto &node_environment =
      require_object(node_environment_value, "catalogs.node_environment");

  std::optional<core::RetainedAssistantProjectionDTO> assistant;
  if (const CanonicalValue *retained_assistant =
          root.find_object_member("assistant");
      retained_assistant != nullptr) {
    try {
      const QualifiedAssistantSettings qualified_assistant =
          verify_retained_assistant_projection(*retained_assistant);
      assistant = qualified_assistant.retained_projection();
    } catch (const AssistantSettingsCodecError &error) {
      detail::fail_retained_bundle(
          std::string("retained assistant projection is invalid: ") +
          error.what());
    }
  }

  const auto &requirement_values =
      require_array(require_member(object, "runtime_contract_requirements",
                                   "retained_bundle"),
                    "retained_bundle.runtime_contract_requirements");
  std::vector<core::RetainedRuntimeContractRequirementDTO>
      runtime_contract_requirements;
  runtime_contract_requirements.reserve(requirement_values.size());
  for (std::size_t index = 0U; index < requirement_values.size(); ++index) {
    const std::string path = "retained_bundle.runtime_contract_requirements[" +
                             std::to_string(index) + "]";
    const auto &row = require_object(requirement_values[index], path);
    require_exact_fields(row, {"runtime_unit_id", "requirement"}, path);
    const std::string runtime_unit_id =
        require_string(require_member(row, "runtime_unit_id", path),
                       path + ".runtime_unit_id");
    detail::require_retained_portable_identity(runtime_unit_id,
                                               path + ".runtime_unit_id");
    const auto &requirement_value = require_member(row, "requirement", path);
    const auto decoded = core::DecodeRuntimeContractRequirement(
        canonical_json(requirement_value));
    if (!decoded.ok()) {
      detail::fail_retained_bundle(
          path + ".requirement is not a canonical verified requirement");
    }
    const auto reencoded =
        core::EncodeRuntimeContractRequirement(decoded.value());
    if (!reencoded.ok() ||
        reencoded.value() != canonical_json(requirement_value)) {
      detail::fail_retained_bundle(
          path + ".requirement is not a canonical verified requirement");
    }
    runtime_contract_requirements.push_back(
        {core::RuntimeUnitId(runtime_unit_id), decoded.value()});
  }

  const auto &products = require_object(
      require_member(object, "compiled_products", "retained_bundle"),
      "retained_bundle.compiled_products");
  require_exact_fields(products,
                       {"planning_collision_model", "viewer_model",
                        "control_domain_products", "symbol_role_maps",
                        "runtime_unit_products"},
                       "retained_bundle.compiled_products");
  const core::RetainedProductDTO planning = parse_retained_product(
      require_member(products, "planning_collision_model",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.planning_collision_model",
      core::RetainedProductKind::kPlanningCollisionModel);
  const core::RetainedProductDTO viewer = parse_retained_product(
      require_member(products, "viewer_model",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.viewer_model",
      core::RetainedProductKind::kViewerModel);
  if (viewer.product_id.value() != "cell_viewer_model") {
    detail::fail_retained_bundle(
        "viewer model has the wrong stable product ID");
  }
  if (viewer.artifact_refs[0] != planning.artifact_refs[0] ||
      viewer.artifact_refs[1] != planning.artifact_refs[2]) {
    detail::fail_retained_bundle(
        "viewer model does not reuse the exact planning resource map and "
        "compiled URDF");
  }
  const auto &control_group_values = require_array(
      require_member(products, "control_domain_products",
                     "retained_bundle.compiled_products"),
      "retained_bundle.compiled_products.control_domain_products");
  std::vector<const core::ControlDomainDTO *> controller_domains;
  for (const auto &domain : verified_graph.graph().control_domains)
    if (!domain.provider())
      controller_domains.push_back(&domain);
  if (control_group_values.size() != controller_domains.size()) {
    detail::fail_retained_bundle(
        "retained control-domain product subjects do not equal the graph "
        "control-domain set");
  }
  std::vector<core::RetainedControlDomainProductsDTO> control_domain_products;
  control_domain_products.reserve(control_group_values.size());
  for (std::size_t index = 0U; index < control_group_values.size(); ++index) {
    const std::string path =
        "retained_bundle.compiled_products.control_domain_products[" +
        std::to_string(index) + "]";
    const auto &group = require_object(control_group_values[index], path);
    require_exact_fields(group, {"control_domain_id", "products"}, path);
    const std::string &control_domain_id =
        require_string(require_member(group, "control_domain_id", path),
                       path + ".control_domain_id");
    detail::require_retained_portable_identity(control_domain_id,
                                               path + ".control_domain_id");
    if (control_domain_id !=
        controller_domains[index]->control_domain_id.value()) {
      detail::fail_retained_bundle(
          "retained control-domain product groups are not the exact "
          "canonical graph-domain table");
    }
    const auto &group_products = require_array(
        require_member(group, "products", path), path + ".products");
    if (group_products.size() != 1U) {
      detail::fail_retained_bundle(
          "each qualified control domain requires exactly one control "
          "product");
    }
    core::RetainedProductDTO product = parse_retained_product(
        group_products.front(), path + ".products[0]",
        core::RetainedProductKind::kControlDomainControl);
    if (product.product_id.value() !=
        detail::control_domain_control_product_id(
            core::ControlDomainId(control_domain_id))) {
      detail::fail_retained_bundle(
          "control-domain product has the wrong stable product ID");
    }
    control_domain_products.push_back(core::RetainedControlDomainProductsDTO{
        core::ControlDomainId(control_domain_id),
        std::vector<core::RetainedProductDTO>{std::move(product)}});
  }
  const auto &symbol_role_map_values =
      require_array(require_member(products, "symbol_role_maps",
                                   "retained_bundle.compiled_products"),
                    "retained_bundle.compiled_products.symbol_role_maps");
  // A model-free bundle carries either no map or the one empty map product
  // (Scenario origin, zero components). The sealed canonical-empty package
  // must have none; its admission shape check enforces that.
  std::vector<core::RetainedProductDTO> symbol_role_maps;
  const bool model_free = verified_graph.graph().model_instances.empty();
  if (model_free ? symbol_role_map_values.size() > 1U
                 : symbol_role_map_values.size() != 1U) {
    detail::fail_retained_bundle(
        model_free
            ? "model-free bundle cannot contain more than one symbol/role map"
            : "non-empty model bundle requires exactly one composed-model "
              "symbol/role map");
  }
  if (!symbol_role_map_values.empty()) {
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
  const auto &runtime_group_values =
      require_array(require_member(products, "runtime_unit_products",
                                   "retained_bundle.compiled_products"),
                    "retained_bundle.compiled_products.runtime_unit_products");
  if (runtime_group_values.size() !=
      verified_graph.graph().runtime_units.size()) {
    detail::fail_retained_bundle(
        "retained runtime-unit product subjects do not equal the graph "
        "runtime-unit set");
  }
  std::vector<core::RetainedRuntimeUnitProductsDTO> runtime_unit_products;
  runtime_unit_products.reserve(runtime_group_values.size());
  for (std::size_t index = 0U; index < runtime_group_values.size(); ++index) {
    const std::string path =
        "retained_bundle.compiled_products.runtime_unit_products[" +
        std::to_string(index) + "]";
    const auto &group = require_object(runtime_group_values[index], path);
    require_exact_fields(group, {"runtime_unit_id", "products"}, path);
    const std::string &runtime_unit_id =
        require_string(require_member(group, "runtime_unit_id", path),
                       path + ".runtime_unit_id");
    detail::require_retained_portable_identity(runtime_unit_id,
                                               path + ".runtime_unit_id");
    const auto &expected_unit = verified_graph.graph().runtime_units[index];
    const bool expected_reference_unit =
        expected_unit.runtime_profile_id.value() ==
            detail::kReferenceRuntimeProfileId &&
        expected_unit.runtime_profile_revision.value() ==
            detail::reference_runtime_profile_revision() &&
        expected_unit.configuration.empty();
    const bool expected_ros2_control_unit =
        expected_unit.runtime_profile_id.value() ==
            detail::kRos2ControlExecutionRuntimeProfileId &&
        expected_unit.runtime_profile_revision.value() ==
            detail::ros2_control_execution_runtime_profile_revision() &&
        detail::ros2_control_execution_runtime_configuration_is_qualified(
            expected_unit.configuration);
    if (runtime_unit_id != expected_unit.runtime_unit_id.value() ||
        !expected_unit.dependency_runtime_unit_ids.empty() ||
        (!expected_reference_unit && !expected_ros2_control_unit)) {
      detail::fail_retained_bundle(
          "retained runtime-unit product group differs from its graph "
          "subject");
    }
    const auto &group_products = require_array(
        require_member(group, "products", path), path + ".products");
    if (group_products.size() != 1U) {
      detail::fail_retained_bundle(
          "each qualified runtime unit requires exactly one runtime product");
    }
    core::RetainedProductDTO product = parse_retained_product(
        group_products.front(), path + ".products[0]",
        core::RetainedProductKind::kRuntimeUnitConfiguration);
    const bool reference =
        expected_unit.runtime_unit_id.value() ==
            detail::kReferenceRuntimeUnitId &&
        expected_unit.runtime_profile_id.value() ==
            detail::kReferenceRuntimeProfileId &&
        expected_unit.runtime_profile_revision.value() ==
            detail::reference_runtime_profile_revision() &&
        product.product_id.value() == detail::reference_runtime_product_id(
                                          expected_unit.runtime_unit_id) &&
        product.product_contract_id.value() ==
            detail::kReferenceRuntimeProductContractId &&
        product.product_contract_revision ==
            detail::reference_runtime_product_contract_revision();
    const bool ros2_control_execution =
        expected_unit.runtime_profile_id.value() ==
            detail::kRos2ControlExecutionRuntimeProfileId &&
        expected_unit.runtime_profile_revision.value() ==
            detail::ros2_control_execution_runtime_profile_revision() &&
        product.product_id.value() ==
            detail::ros2_control_execution_runtime_product_id(
                expected_unit.runtime_unit_id) &&
        product.product_contract_id.value() ==
            detail::kRos2ControlExecutionRuntimeProductContractId &&
        product.product_contract_revision ==
            detail::ros2_control_execution_runtime_product_contract_revision();
    if (!reference && !ros2_control_execution) {
      detail::fail_retained_bundle(
          "runtime-unit product has no exact maintained product row");
    }
    runtime_unit_products.push_back(core::RetainedRuntimeUnitProductsDTO{
        core::RuntimeUnitId(runtime_unit_id),
        std::vector<core::RetainedProductDTO>{std::move(product)}});
  }
  const auto independently_derived_requirements =
      detail::derive_runtime_contract_requirements(
          verified_graph.graph(),
          applicability.value().capability_provider_bindings, {}, {},
          runtime_profiles, runtime_unit_products);
  const bool requirements_match =
      independently_derived_requirements.size() ==
          runtime_contract_requirements.size() &&
      std::equal(independently_derived_requirements.begin(),
                 independently_derived_requirements.end(),
                 runtime_contract_requirements.begin(),
                 [](const auto &derived, const auto &retained) {
                   // Implementation identity, catalog revisions, and the digest
                   // that includes them are diagnostic provenance. The semantic
                   // requirement is the exact runtime unit plus its derived API
                   // and immutable capability-ID sets.
                   const auto &derived_capabilities =
                       derived.requirement.required_capabilities;
                   const auto &retained_capabilities =
                       retained.requirement.required_capabilities;
                   return derived.runtime_unit_id == retained.runtime_unit_id &&
                          derived.requirement.required_api_contract_ids ==
                              retained.requirement.required_api_contract_ids &&
                          derived_capabilities.size() ==
                              retained_capabilities.size() &&
                          std::equal(
                              derived_capabilities.begin(),
                              derived_capabilities.end(),
                              retained_capabilities.begin(),
                              [](const auto &derived_capability,
                                 const auto &retained_capability) {
                                return derived_capability.capability_id ==
                                       retained_capability.capability_id;
                              });
                 });
  if (!requirements_match) {
    detail::fail_retained_bundle(
        "retained runtime-contract requirements differ from independent "
        "rederivation");
  }

  const auto &provenance =
      require_object(require_member(object, "provenance", "retained_bundle"),
                     "retained_bundle.provenance");
  require_exact_fields(provenance,
                       {"compiler_contract_digest", "compiler_build_digest",
                        "source_records", "generator_records",
                        "output_records"},
                       "retained_bundle.provenance");
  const std::string &compiler_contract_digest =
      require_string(require_member(provenance, "compiler_contract_digest",
                                    "retained_bundle.provenance"),
                     "retained_bundle.provenance.compiler_contract_digest");
  detail::require_retained_digest(
      compiler_contract_digest,
      "retained_bundle.provenance.compiler_contract_digest");
  const std::string &compiler_build_digest =
      require_string(require_member(provenance, "compiler_build_digest",
                                    "retained_bundle.provenance"),
                     "retained_bundle.provenance.compiler_build_digest");
  detail::require_retained_digest(
      compiler_build_digest,
      "retained_bundle.provenance.compiler_build_digest");
  const auto source_records = parse_structural_source_records(
      require_member(provenance, "source_records",
                     "retained_bundle.provenance"),
      catalog_records);
  const std::size_t selected_model_asset_count = static_cast<std::size_t>(
      std::count_if(catalog_records.begin(), catalog_records.end(),
                    [](const core::CatalogRecordRefDTO &record) {
                      return core::catalog_record_kind(record) ==
                             core::CatalogRecordKind::kModelAsset;
                    }));
  const auto generator_records = parse_structural_generator_records(
      require_member(provenance, "generator_records",
                     "retained_bundle.provenance"),
      selected_model_asset_count);

  const auto &outputs =
      require_array(require_member(provenance, "output_records",
                                   "retained_bundle.provenance"),
                    "retained_bundle.provenance.output_records");
  std::vector<const core::RetainedProductDTO *> expected_outputs{&planning,
                                                                 &viewer};
  for (const auto &group : control_domain_products) {
    for (const auto &product : group.products) {
      expected_outputs.push_back(&product);
    }
  }
  for (const auto &product : symbol_role_maps) {
    expected_outputs.push_back(&product);
  }
  for (const auto &group : runtime_unit_products) {
    for (const auto &product : group.products) {
      expected_outputs.push_back(&product);
    }
  }
  std::sort(expected_outputs.begin(), expected_outputs.end(),
            [](const auto *left, const auto *right) {
              return left->product_id.value() < right->product_id.value();
            });
  if (outputs.size() != expected_outputs.size()) {
    detail::fail_retained_bundle(
        "structural provenance does not contain one output per product");
  }
  std::vector<core::RetainedOutputRecordDTO> output_records;
  output_records.reserve(outputs.size());
  for (std::size_t index = 0U; index < outputs.size(); ++index) {
    const std::string path = "retained_bundle.provenance.output_records[" +
                             std::to_string(index) + "]";
    const auto &output = require_object(outputs[index], path);
    require_exact_fields(
        output, {"product_id", "product_kind", "content_digest"}, path);
    const auto &expected = *expected_outputs[index];
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
              require_string(require_member(node_environment, "catalog_id",
                                            "catalogs.node_environment"),
                             "catalogs.node_environment.catalog_id"),
              require_string(require_member(node_environment, "content_version",
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
      applicability.value().retained_policy_records,
      core::RetainedCompiledProductsDTO{
          planning,
          viewer,
          control_domain_products,
          symbol_role_maps,
          runtime_unit_products,
      },
      core::RetainedBundleProvenanceDTO{
          core::ContentDigest(compiler_contract_digest),
          core::ContentDigest(compiler_build_digest),
          source_records,
          generator_records,
          output_records,
      },
      applicability.value().site_overlay_ref,
      applicability.value().external_resources,
      std::move(assistant),
      std::move(runtime_contract_requirements),
      motion_takeover_preparation_budget_s,
      motion_takeover_planning_budget_s,
      root.find_object_member("scene_pose_sources")
          ? DecodeScenePoseSources(
                *root.find_object_member("scene_pose_sources"))
          : std::vector<core::ScenePoseSourcePolicyDTO>{},
  };
}

} // namespace

namespace detail {

std::string_view flattened_model_product_contract_id() noexcept {
  return kFlattenedModelContractId;
}

const core::ProductContractRevision &
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

const CanonicalValue &flattened_model_product_contract_descriptor() {
  return qualified_product_contract_state(
             kFlattenedModelContractId,
             core::RetainedProductKind::kPlanningCollisionModel,
             kFlattenedModelPlacement, kFlattenedModelArtifactRoles)
      .descriptor;
}

std::string_view viewer_model_product_contract_id() noexcept {
  return kViewerModelContractId;
}

const core::ProductContractRevision &viewer_model_product_contract_revision() {
  return qualified_product_contract_state(
             kViewerModelContractId, core::RetainedProductKind::kViewerModel,
             kViewerModelPlacement, kViewerModelArtifactRoles)
      .revision;
}

std::span<const QualifiedRetainedProductArtifactRole>
viewer_model_product_artifact_roles() noexcept {
  return kViewerModelArtifactRoles;
}

const CanonicalValue &viewer_model_product_contract_descriptor() {
  return qualified_product_contract_state(
             kViewerModelContractId, core::RetainedProductKind::kViewerModel,
             kViewerModelPlacement, kViewerModelArtifactRoles)
      .descriptor;
}

std::string_view control_domain_control_product_contract_id() noexcept {
  return kControlDomainControlContractId;
}

const core::ProductContractRevision &
control_domain_control_product_contract_revision() {
  return qualified_product_contract_state(
             kControlDomainControlContractId,
             core::RetainedProductKind::kControlDomainControl,
             kControlDomainControlPlacement, kControlDomainControlArtifactRoles)
      .revision;
}

std::span<const QualifiedRetainedProductArtifactRole>
control_domain_control_product_artifact_roles() noexcept {
  return kControlDomainControlArtifactRoles;
}

const CanonicalValue &control_domain_control_product_contract_descriptor() {
  return qualified_product_contract_state(
             kControlDomainControlContractId,
             core::RetainedProductKind::kControlDomainControl,
             kControlDomainControlPlacement, kControlDomainControlArtifactRoles)
      .descriptor;
}

std::string_view reference_runtime_product_contract_id() noexcept {
  return kReferenceRuntimeProductContractId;
}

const core::ProductContractRevision &
reference_runtime_product_contract_revision() {
  return qualified_product_contract_state(
             kReferenceRuntimeProductContractId,
             core::RetainedProductKind::kRuntimeUnitConfiguration,
             kReferenceRuntimePlacement, kReferenceRuntimeArtifactRoles)
      .revision;
}

std::span<const QualifiedRetainedProductArtifactRole>
reference_runtime_product_artifact_roles() noexcept {
  return kReferenceRuntimeArtifactRoles;
}

const CanonicalValue &reference_runtime_product_contract_descriptor() {
  return qualified_product_contract_state(
             kReferenceRuntimeProductContractId,
             core::RetainedProductKind::kRuntimeUnitConfiguration,
             kReferenceRuntimePlacement, kReferenceRuntimeArtifactRoles)
      .descriptor;
}

std::string_view ros2_control_execution_runtime_product_contract_id() noexcept {
  return kRos2ControlExecutionRuntimeProductContractId;
}

const core::ProductContractRevision &
ros2_control_execution_runtime_product_contract_revision() {
  return qualified_product_contract_state(
             kRos2ControlExecutionRuntimeProductContractId,
             core::RetainedProductKind::kRuntimeUnitConfiguration,
             kReferenceRuntimePlacement,
             kRos2ControlExecutionRuntimeArtifactRoles)
      .revision;
}

std::span<const QualifiedRetainedProductArtifactRole>
ros2_control_execution_runtime_product_artifact_roles() noexcept {
  return kRos2ControlExecutionRuntimeArtifactRoles;
}

const CanonicalValue &
ros2_control_execution_runtime_product_contract_descriptor() {
  return qualified_product_contract_state(
             kRos2ControlExecutionRuntimeProductContractId,
             core::RetainedProductKind::kRuntimeUnitConfiguration,
             kReferenceRuntimePlacement,
             kRos2ControlExecutionRuntimeArtifactRoles)
      .descriptor;
}

std::string_view composed_model_symbol_role_map_product_contract_id() noexcept {
  return kComposedModelSymbolRoleMapContractId;
}

const core::ProductContractRevision &
composed_model_symbol_role_map_product_contract_revision() {
  return qualified_product_contract_state(
             kComposedModelSymbolRoleMapContractId,
             core::RetainedProductKind::kSymbolRoleMap, kSymbolRoleMapPlacement,
             kComposedModelSymbolRoleMapArtifactRoles)
      .revision;
}

std::span<const QualifiedRetainedProductArtifactRole>
composed_model_symbol_role_map_product_artifact_roles() noexcept {
  return kComposedModelSymbolRoleMapArtifactRoles;
}

const CanonicalValue &
composed_model_symbol_role_map_product_contract_descriptor() {
  return qualified_product_contract_state(
             kComposedModelSymbolRoleMapContractId,
             core::RetainedProductKind::kSymbolRoleMap, kSymbolRoleMapPlacement,
             kComposedModelSymbolRoleMapArtifactRoles)
      .descriptor;
}

[[noreturn]] void fail_retained_bundle(std::string_view message) {
  throw RetainedBundleError(std::string(message));
}

void require_retained_portable_identity(std::string_view value,
                                        std::string_view field) {
  if (!is_portable_identity(value)) {
    fail_retained_bundle(std::string(field) +
                         " must use the portable 1..128-byte identity grammar");
  }
}

void require_retained_digest(std::string_view value, std::string_view field) {
  if (!is_sha256_digest(value)) {
    fail_retained_bundle(std::string(field) +
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

std::optional<std::string_view>
retained_raw_key_hex(const core::ContentAddressedStoreKey &key,
                     const core::ContentDigest &expected_digest) noexcept {
  const auto hex = retained_digest_hex(expected_digest.value());
  if (!hex.has_value() ||
      key.value() != std::string(kStoreKeyPrefix) + std::string(*hex)) {
    return std::nullopt;
  }
  return hex;
}

void require_current_retained_product_conformance(
    const core::RetainedProductDTO &product) {
  require_retained_portable_identity(product.product_id.value(), "product_id");
  std::span<const QualifiedRetainedProductArtifactRole> artifact_roles;
  if (product.product_contract_id.value() == kFlattenedModelContractId &&
      product.product_contract_revision ==
          flattened_model_product_contract_revision() &&
      product.product_kind ==
          core::RetainedProductKind::kPlanningCollisionModel) {
    artifact_roles = kFlattenedModelArtifactRoles;
  } else if (product.product_contract_id.value() == kViewerModelContractId &&
             product.product_contract_revision ==
                 viewer_model_product_contract_revision() &&
             product.product_kind == core::RetainedProductKind::kViewerModel) {
    artifact_roles = kViewerModelArtifactRoles;
  } else if (product.product_contract_id.value() ==
                 kControlDomainControlContractId &&
             product.product_contract_revision ==
                 control_domain_control_product_contract_revision() &&
             product.product_kind ==
                 core::RetainedProductKind::kControlDomainControl) {
    artifact_roles = kControlDomainControlArtifactRoles;
  } else if (product.product_contract_id.value() ==
                 kComposedModelSymbolRoleMapContractId &&
             product.product_contract_revision ==
                 composed_model_symbol_role_map_product_contract_revision() &&
             product.product_kind ==
                 core::RetainedProductKind::kSymbolRoleMap) {
    artifact_roles = kComposedModelSymbolRoleMapArtifactRoles;
  } else if (product.product_contract_id.value() ==
                 kReferenceRuntimeProductContractId &&
             product.product_contract_revision ==
                 reference_runtime_product_contract_revision() &&
             product.product_kind ==
                 core::RetainedProductKind::kRuntimeUnitConfiguration) {
    artifact_roles = kReferenceRuntimeArtifactRoles;
  } else if (product.product_contract_id.value() ==
                 kRos2ControlExecutionRuntimeProductContractId &&
             product.product_contract_revision ==
                 ros2_control_execution_runtime_product_contract_revision() &&
             product.product_kind ==
                 core::RetainedProductKind::kRuntimeUnitConfiguration) {
    artifact_roles = kRos2ControlExecutionRuntimeArtifactRoles;
  } else {
    fail_retained_bundle("retained product references an unqualified contract");
  }
  if (product.artifact_refs.size() > artifact_roles.size()) {
    fail_retained_bundle(
        "retained product contains more artifacts than its contract admits");
  }

  std::set<std::string> present_roles;
  std::string previous_role;
  for (const auto &artifact : product.artifact_refs) {
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
        [&artifact](const QualifiedRetainedProductArtifactRole &role) {
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
    require_retained_digest(artifact.content_digest.value(), "content_digest");
    if (artifact.content_addressed_store_key.value() !=
        retained_store_key_for_digest(artifact.content_digest.value())) {
      fail_retained_bundle("retained artifact store key is not digest-derived");
    }
    if (artifact.byte_size > detail::kMaximumIJsonInteger) {
      fail_retained_bundle(
          "retained artifact byte size exceeds the I-JSON safe integer");
    }
    present_roles.insert(artifact.artifact_role_id.value());
  }
  for (const auto &expected : artifact_roles) {
    if (expected.required &&
        !present_roles.contains(std::string(expected.artifact_role_id))) {
      fail_retained_bundle(
          "retained product is missing a required artifact role");
    }
  }
}

CanonicalValue
retained_product_projection(const core::RetainedProductDTO &product) {
  require_current_retained_product_conformance(product);
  CanonicalValue::Array artifacts;
  artifacts.reserve(product.artifact_refs.size());
  for (const auto &artifact : product.artifact_refs) {
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
retained_product_output_digest(const core::RetainedProductDTO &product) {
  return core::ContentDigest(domain_separated_canonical_digest(
      kRetainedProductOutputDomain, kRetainedProductSchemaTag,
      retained_product_projection(product)));
}

std::shared_ptr<const RetainedBundle>
RetainedBundleConstructionAuthority::construct(
    core::RetainedBundleId retained_bundle_id, core::RetainedBundleDTO value,
    CanonicalValue normalized_root, std::string canonical_bytes) {
  return std::shared_ptr<const RetainedBundle>(new RetainedBundle(
      std::move(retained_bundle_id), std::move(value),
      std::move(normalized_root), std::move(canonical_bytes)));
}

} // namespace detail

namespace {

[[nodiscard]] bool
derive_retained_bundle_identity(const RetainedBundle &bundle) {
  try {
    const core::RetainedBundleDTO parsed =
        verify_current_retained_bundle_projection(bundle.normalized_root());
    const CanonicalValue *declared_id =
        bundle.normalized_root().find_object_member("retained_bundle_id");
    const auto *declared_id_string =
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
  } catch (const std::exception &) {
    return false;
  }
}

} // namespace

bool verify_retained_bundle_identity(const RetainedBundle &bundle) {
  constexpr std::uint8_t kVerified = 1U;
  constexpr std::uint8_t kRejected = 2U;
  const auto known =
      bundle.identity_verification_.load(std::memory_order_acquire);
  if (known != 0U) {
    return known == kVerified;
  }
  // Concurrent first checks may both derive; they reach the same outcome.
  const bool verified = derive_retained_bundle_identity(bundle);
  bundle.identity_verification_.store(verified ? kVerified : kRejected,
                                      std::memory_order_release);
  return verified;
}

std::vector<core::StreamParameterOverrideDTO>
ComponentStreamParameterOverrides(const RetainedBundle &bundle) {
  const auto *streams =
      bundle.normalized_root().find_object_member("component_streams");
  return streams == nullptr ? std::vector<core::StreamParameterOverrideDTO>{}
                            : decode_component_stream_overrides(*streams);
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
    const core::RetainedBundleId retained_bundle_id = value.retained_bundle_id;
    auto bundle = RetainedBundleConstructionAuthority::construct(
        retained_bundle_id, std::move(value), std::move(root),
        std::string(canonical_bytes));
    if (!verify_retained_bundle_identity(*bundle)) {
      fail_retained_bundle(
          "retained-bundle repository bytes have an invalid semantic "
          "identity");
    }
    return bundle;
  } catch (const CanonicalValueError &error) {
    fail_retained_bundle(
        std::string("retained-bundle repository bytes are invalid: ") +
        error.what());
  } catch (const ArtifactCodecError &error) {
    fail_retained_bundle(
        std::string("retained-bundle repository bytes are invalid: ") +
        error.what());
  } catch (const CellGraphCodecError &error) {
    fail_retained_bundle(
        std::string("retained-bundle repository bytes are invalid: ") +
        error.what());
  }
}

} // namespace manyforge::deployment
