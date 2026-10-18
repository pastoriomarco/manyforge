#include "manyforge_deployment/cell_graph_codec.hpp"
#include <manyforge_deployment/io_signal_catalog.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "closed_schema_internal.hpp"
#include "compiler_semantic_limits_internal.hpp"
#include "content_identity_internal.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/verified_retained_bundle.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"

namespace manyforge::deployment {
namespace {

using core::AttachmentInterfaceId;
using core::CapabilityDefinitionRevision;
using core::CapabilityId;
using core::CellGraphDTO;
using core::CellId;
using core::CommandTargetId;
using core::ComponentAttachmentEdgeDTO;
using core::ComponentAttachmentEndpointDTO;
using core::ComponentAttachmentId;
using core::ComponentAttachmentRevision;
using core::ComponentTypeId;
using core::ComponentTypeRevision;
using core::ContainmentGranularity;
using core::ControlBindingDTO;
using core::ControlBindingId;
using core::ControlBindingRevision;
using core::ControlDomainDTO;
using core::ControlDomainId;
using core::ControlDomainRevision;
using core::ControlInterfaceContractId;
using core::ControlInterfaceRevision;
using core::ControllableComponentDTO;
using core::ControllableComponentId;
using core::ControllerTopologyId;
using core::ControlRouteId;
using core::EndpointRoleId;
using core::GraphRevision;
using core::JsonObjectDTO;
using core::JsonValueDTO;
using core::ModelAssetId;
using core::ModelInstanceDTO;
using core::ModelInstanceId;
using core::ModelRevision;
using core::ModelRoleBindingDTO;
using core::ModelRoleId;
using core::PlacementPoseDTO;
using core::ProviderInstanceId;
using core::ProviderManifestId;
using core::ProviderManifestRevision;
using core::ResourceBindingDTO;
using core::ResourceId;
using core::ResourceRoleId;
using core::RouteEndpointProjectionDTO;
using core::RuntimeProfileId;
using core::RuntimeProfileRevision;
using core::RuntimeUnitDTO;
using core::RuntimeUnitId;
using core::StateAuthorityClass;
using core::StateAuthorityId;
using core::StopEvidencePolicyId;
using core::StopEvidencePolicyRevision;
using core::TopologyRevision;

using RuntimeProfileKey = std::pair<std::string, std::string>;

// Accept ordinary decimal serialization error while still rejecting poses
// that require semantic renormalization. The codec never changes the value.
[[nodiscard]] constexpr bool is_ascii_alphanumeric(unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool is_bounded_ascii_identity(std::string_view spelling) noexcept {
  if (spelling.empty() ||
      spelling.size() > detail::kMaximumPortableIdentityBytes ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(spelling.front()))) {
    return false;
  }
  return std::all_of(spelling.begin() + 1, spelling.end(), [](unsigned char byte) {
    return is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' || byte == '-';
  });
}

[[noreturn]] void fail(std::string_view path, std::string_view reason) {
  throw CellGraphCodecError(std::string(path) + ": " + std::string(reason));
}

[[nodiscard]] const CanonicalValue::Object& require_object(const CanonicalValue& value,
                                                           std::string_view path) {
  const auto* object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(path, "expected object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array& require_array(const CanonicalValue& value,
                                                         std::string_view path) {
  const auto* array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(path, "expected array");
  }
  return *array;
}

[[nodiscard]] const std::string& require_string(const CanonicalValue& value,
                                                std::string_view path) {
  const auto* string = std::get_if<std::string>(&value.storage());
  if (string == nullptr) {
    fail(path, "expected string");
  }
  return *string;
}

[[nodiscard]] double require_number(const CanonicalValue& value, std::string_view path) {
  const auto* number = std::get_if<double>(&value.storage());
  if (number == nullptr) {
    fail(path, "expected number");
  }
  if (!std::isfinite(*number) || (*number == 0.0 && std::signbit(*number))) {
    fail(path, "number is outside the qualified I-JSON model");
  }
  return *number;
}

[[nodiscard]] const CanonicalValue& require_member(const CanonicalValue::Object& object,
                                                   std::string_view name, std::string_view path) {
  const auto iterator = std::find_if(object.begin(), object.end(),
                                     [name](const auto& member) { return member.first == name; });
  if (iterator == object.end()) {
    fail(path, "missing required field '" + std::string(name) + "'");
  }
  return iterator->second;
}

[[nodiscard]] const CanonicalValue* find_member(const CanonicalValue::Object& object,
                                                std::string_view name) noexcept {
  const auto iterator = std::find_if(object.begin(), object.end(),
                                     [name](const auto& member) { return member.first == name; });
  return iterator == object.end() ? nullptr : &iterator->second;
}

void require_fields(const CanonicalValue::Object& object,
                    std::initializer_list<std::string_view> required,
                    std::initializer_list<std::string_view> optional, std::string_view path) {
  for (const auto& [name, unused] : object) {
    (void)unused;
    if (std::find(required.begin(), required.end(), name) == required.end() &&
        std::find(optional.begin(), optional.end(), name) == optional.end()) {
      fail(path, "unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : required) {
    (void)require_member(object, name, path);
  }
}

void require_exact_fields(const CanonicalValue::Object& object,
                          std::initializer_list<std::string_view> expected, std::string_view path) {
  require_fields(object, expected, {}, path);
}

[[nodiscard]] std::string child_path(std::string_view parent, std::string_view name) {
  return std::string(parent) + "." + std::string(name);
}

[[nodiscard]] std::string index_path(std::string_view parent, std::size_t index) {
  return std::string(parent) + "[" + std::to_string(index) + "]";
}

template <typename StrongType>
[[nodiscard]] StrongType parse_identity(const CanonicalValue& value, std::string_view path) {
  const std::string& spelling = require_string(value, path);
  if (!is_bounded_ascii_identity(spelling)) {
    fail(path,
         "identity must be 1..128 ASCII characters, start with an alphanumeric, and contain only "
         "alphanumerics, '.', '_', or '-'");
  }
  return StrongType(spelling);
}

template <typename StrongType>
[[nodiscard]] StrongType parse_digest(const CanonicalValue& value, std::string_view path) {
  const std::string& spelling = require_string(value, path);
  if (!is_sha256_digest(spelling)) {
    fail(path, "expected lowercase sha256:<64 hex> digest");
  }
  return StrongType(spelling);
}

template <typename StrongType>
[[nodiscard]] std::vector<StrongType> parse_identity_set(const CanonicalValue& value,
                                                         std::string_view path) {
  const auto& array = require_array(value, path);
  std::vector<StrongType> result;
  result.reserve(array.size());
  for (std::size_t index = 0; index < array.size(); ++index) {
    result.push_back(parse_identity<StrongType>(array[index], index_path(path, index)));
  }
  std::sort(result.begin(), result.end());
  const auto duplicate = std::adjacent_find(result.begin(), result.end());
  if (duplicate != result.end()) {
    fail(path, "duplicate canonical identity '" + duplicate->value() + "'");
  }
  return result;
}

[[nodiscard]] JsonValueDTO to_json_value(const CanonicalValue& value, std::string_view path);

[[nodiscard]] JsonObjectDTO to_json_object(const CanonicalValue& value, std::string_view path) {
  const auto& object = require_object(value, path);
  JsonObjectDTO result;
  for (const auto& [name, member] : object) {
    result.emplace(name, to_json_value(member, child_path(path, name)));
  }
  return result;
}

[[nodiscard]] JsonValueDTO to_json_value(const CanonicalValue& value, std::string_view path) {
  return std::visit(
      [&path, &value](const auto& current) -> JsonValueDTO {
        using Value = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t> || std::is_same_v<Value, bool> ||
                      std::is_same_v<Value, double> || std::is_same_v<Value, std::string>) {
          return JsonValueDTO{current};
        } else if constexpr (std::is_same_v<Value, CanonicalValue::Array>) {
          JsonValueDTO::Array array;
          array.reserve(current.size());
          for (std::size_t index = 0; index < current.size(); ++index) {
            array.push_back(to_json_value(current[index], index_path(path, index)));
          }
          return JsonValueDTO{std::move(array)};
        } else {
          return JsonValueDTO{to_json_object(value, path)};
        }
      },
      value.storage());
}

[[nodiscard]] PlacementPoseDTO parse_placement_pose(const CanonicalValue& value,
                                                    std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(object, {"position_m", "orientation_xyzw"}, path);

  const std::string position_path = child_path(path, "position_m");
  const auto& position = require_array(require_member(object, "position_m", path), position_path);
  if (position.size() != 3U) {
    fail(position_path, "expected exactly three coordinates");
  }

  const std::string orientation_path = child_path(path, "orientation_xyzw");
  const auto& orientation =
      require_array(require_member(object, "orientation_xyzw", path), orientation_path);
  if (orientation.size() != 4U) {
    fail(orientation_path, "expected exactly four quaternion coordinates");
  }

  PlacementPoseDTO pose;
  for (std::size_t index = 0; index < pose.position_m.size(); ++index) {
    pose.position_m[index] = require_number(position[index], index_path(position_path, index));
  }
  double norm_squared = 0.0;
  for (std::size_t index = 0; index < pose.orientation_xyzw.size(); ++index) {
    pose.orientation_xyzw[index] =
        require_number(orientation[index], index_path(orientation_path, index));
    norm_squared += pose.orientation_xyzw[index] * pose.orientation_xyzw[index];
  }
  if (!std::isfinite(norm_squared) ||
      std::abs(norm_squared - 1.0) >
          detail::kQuaternionNormSquaredTolerance) {
    fail(orientation_path, "quaternion norm squared must be within 1e-6 of one");
  }
  return pose;
}

[[nodiscard]] ContainmentGranularity parse_containment_granularity(const CanonicalValue& value,
                                                                   std::string_view path) {
  const std::string& spelling = require_string(value, path);
  if (spelling == "resource") {
    return ContainmentGranularity::kResource;
  }
  if (spelling == "controller_set") {
    return ContainmentGranularity::kControllerSet;
  }
  if (spelling == "control_domain") {
    return ContainmentGranularity::kControlDomain;
  }
  fail(path, "expected resource, controller_set, or control_domain");
}

[[nodiscard]] StateAuthorityClass parse_state_authority_class(const CanonicalValue& value,
                                                              std::string_view path) {
  const std::string& spelling = require_string(value, path);
  if (spelling == "measured") {
    return StateAuthorityClass::kMeasured;
  }
  if (spelling == "commanded") {
    return StateAuthorityClass::kCommanded;
  }
  if (spelling == "predicted") {
    return StateAuthorityClass::kPredicted;
  }
  fail(path, "expected measured, commanded, or predicted");
}

[[nodiscard]] RuntimeUnitDTO parse_runtime_unit(const CanonicalValue& value,
                                                std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(object,
                       {"runtime_unit_id", "runtime_profile_id", "runtime_profile_revision",
                        "dependency_runtime_unit_ids", "configuration"},
                       path);
  return RuntimeUnitDTO{
      parse_identity<RuntimeUnitId>(require_member(object, "runtime_unit_id", path),
                                    child_path(path, "runtime_unit_id")),
      parse_identity<RuntimeProfileId>(require_member(object, "runtime_profile_id", path),
                                       child_path(path, "runtime_profile_id")),
      parse_digest<RuntimeProfileRevision>(
          require_member(object, "runtime_profile_revision", path),
          child_path(path, "runtime_profile_revision")),
      parse_identity_set<RuntimeUnitId>(require_member(object, "dependency_runtime_unit_ids", path),
                                        child_path(path, "dependency_runtime_unit_ids")),
      to_json_object(require_member(object, "configuration", path),
                     child_path(path, "configuration"))};
}

[[nodiscard]] ModelInstanceDTO parse_model_instance(const CanonicalValue& value,
                                                    std::string_view path) {
  const auto& object = require_object(value, path);
  require_fields(object, {"model_instance_id", "model_asset_id", "model_revision"},
                 {"root_pose_in_cell"}, path);
  const CanonicalValue* root_pose = find_member(object, "root_pose_in_cell");
  return ModelInstanceDTO{
      parse_identity<ModelInstanceId>(require_member(object, "model_instance_id", path),
                                      child_path(path, "model_instance_id")),
      parse_identity<ModelAssetId>(require_member(object, "model_asset_id", path),
                                   child_path(path, "model_asset_id")),
      parse_digest<ModelRevision>(require_member(object, "model_revision", path),
                                  child_path(path, "model_revision")),
      root_pose == nullptr
          ? std::nullopt
          : std::optional<PlacementPoseDTO>{
                parse_placement_pose(*root_pose, child_path(path, "root_pose_in_cell"))}};
}

[[nodiscard]] std::vector<ModelRoleBindingDTO> parse_model_role_bindings(
    const CanonicalValue& value, std::string_view path) {
  const auto& array = require_array(value, path);
  std::vector<ModelRoleBindingDTO> result;
  result.reserve(array.size());
  std::set<std::string, std::less<>> model_roles;
  std::set<std::string, std::less<>> model_instances;
  for (std::size_t index = 0; index < array.size(); ++index) {
    const std::string item_path = index_path(path, index);
    const auto& object = require_object(array[index], item_path);
    require_exact_fields(object, {"model_role_id", "model_instance_id"}, item_path);
    ModelRoleBindingDTO binding{
        parse_identity<ModelRoleId>(require_member(object, "model_role_id", item_path),
                                    child_path(item_path, "model_role_id")),
        parse_identity<ModelInstanceId>(require_member(object, "model_instance_id", item_path),
                                        child_path(item_path, "model_instance_id"))};
    if (!model_roles.emplace(binding.model_role_id.value()).second) {
      fail(path, "one model role is bound more than once");
    }
    if (!model_instances.emplace(binding.model_instance_id.value()).second) {
      fail(path, "one model instance is bound more than once");
    }
    result.push_back(std::move(binding));
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.model_role_id < right.model_role_id;
  });
  return result;
}

[[nodiscard]] ControllableComponentDTO parse_controllable_component(const CanonicalValue& value,
                                                                    std::string_view path) {
  const auto& object = require_object(value, path);
  require_fields(object,
                 {"controllable_component_id", "component_type_id", "component_type_revision",
                  "model_role_bindings", "runtime_unit_ids"},
                 {"root_pose_in_cell"}, path);
  const CanonicalValue* root_pose = find_member(object, "root_pose_in_cell");
  return ControllableComponentDTO{
      parse_identity<ControllableComponentId>(
          require_member(object, "controllable_component_id", path),
          child_path(path, "controllable_component_id")),
      parse_identity<ComponentTypeId>(require_member(object, "component_type_id", path),
                                      child_path(path, "component_type_id")),
      parse_digest<ComponentTypeRevision>(require_member(object, "component_type_revision", path),
                                          child_path(path, "component_type_revision")),
      parse_model_role_bindings(require_member(object, "model_role_bindings", path),
                                child_path(path, "model_role_bindings")),
      parse_identity_set<RuntimeUnitId>(require_member(object, "runtime_unit_ids", path),
                                        child_path(path, "runtime_unit_ids")),
      root_pose == nullptr
          ? std::nullopt
          : std::optional<PlacementPoseDTO>{
                parse_placement_pose(*root_pose, child_path(path, "root_pose_in_cell"))}};
}

[[nodiscard]] std::int64_t parse_positive_int64(const CanonicalValue &value,
                                                std::string_view path);

[[nodiscard]] ControlDomainDTO parse_control_domain(const CanonicalValue& value,
                                                    std::string_view path) {
  const auto& object = require_object(value, path);
  if (const auto *kind = find_member(object, "kind")) {
    if (require_string(*kind, child_path(path, "kind")) != "provider")
      fail(path, "unknown control-domain kind");
    require_exact_fields(object,
                         {"control_domain_id", "control_domain_revision",
                          "runtime_unit_ids",
                          "control_coordinator_endpoint_role",
                          "containment_granularity", "kind", "provider"},
                         path);
    const auto provider_path = child_path(path, "provider");
    const auto &provider =
        require_object(require_member(object, "provider", path), provider_path);
    require_exact_fields(provider,
                         {"controllable_component_id",
                          "provider_runtime_unit_id", "provider_instance_id",
                          "provider_manifest_id", "provider_manifest_revision",
                          "lifecycle_plan_catalog_digest",
                          "signal_configuration_digest", "signal_names",
                          "lifecycle_command_timeout_ns"},
                         provider_path);
    std::vector<std::string> names;
    for (const auto &item :
         require_array(require_member(provider, "signal_names", provider_path),
                       child_path(provider_path, "signal_names"))) {
      names.push_back(
          parse_identity<core::ResourceRoleId>(item, provider_path).value());
    }
    if (names.empty() || !std::is_sorted(names.begin(), names.end()) ||
        std::adjacent_find(names.begin(), names.end()) != names.end())
      fail(provider_path,
           "provider signal membership must be a canonical nonempty set");
    return ControlDomainDTO{
        parse_identity<ControlDomainId>(
            require_member(object, "control_domain_id", path), path),
        parse_digest<ControlDomainRevision>(
            require_member(object, "control_domain_revision", path), path),
        parse_identity_set<RuntimeUnitId>(
            require_member(object, "runtime_unit_ids", path), path),
        parse_identity<EndpointRoleId>(
            require_member(object, "control_coordinator_endpoint_role", path),
            path),
        parse_containment_granularity(
            require_member(object, "containment_granularity", path), path),
        core::ProviderControlDomainDTO{
            parse_identity<ControllableComponentId>(
                require_member(provider, "controllable_component_id",
                               provider_path),
                provider_path),
            parse_identity<RuntimeUnitId>(
                require_member(provider, "provider_runtime_unit_id",
                               provider_path),
                provider_path),
            parse_identity<ProviderInstanceId>(
                require_member(provider, "provider_instance_id", provider_path),
                provider_path),
            parse_identity<ProviderManifestId>(
                require_member(provider, "provider_manifest_id", provider_path),
                provider_path),
            parse_digest<ProviderManifestRevision>(
                require_member(provider, "provider_manifest_revision",
                               provider_path),
                provider_path),
            parse_digest<core::ContentDigest>(
                require_member(provider, "lifecycle_plan_catalog_digest",
                               provider_path),
                provider_path),
            parse_digest<core::ContentDigest>(
                require_member(provider, "signal_configuration_digest",
                               provider_path),
                provider_path),
            std::move(names),
            parse_positive_int64(
                require_member(provider, "lifecycle_command_timeout_ns",
                               provider_path),
                child_path(provider_path, "lifecycle_command_timeout_ns"))}};
  }
  require_exact_fields(object,
                       {"control_domain_id", "control_domain_revision", "runtime_unit_ids",
                        "control_coordinator_endpoint_role", "controller_topology_id",
                        "topology_revision", "containment_granularity"},
                       path);
  return ControlDomainDTO{
      parse_identity<ControlDomainId>(require_member(object, "control_domain_id", path),
                                      child_path(path, "control_domain_id")),
      parse_digest<ControlDomainRevision>(require_member(object, "control_domain_revision", path),
                                          child_path(path, "control_domain_revision")),
      parse_identity_set<RuntimeUnitId>(require_member(object, "runtime_unit_ids", path),
                                        child_path(path, "runtime_unit_ids")),
      parse_identity<EndpointRoleId>(
          require_member(object, "control_coordinator_endpoint_role", path),
          child_path(path, "control_coordinator_endpoint_role")),
      parse_identity<ControllerTopologyId>(require_member(object, "controller_topology_id", path),
                                           child_path(path, "controller_topology_id")),
      parse_digest<TopologyRevision>(require_member(object, "topology_revision", path),
                                     child_path(path, "topology_revision")),
      parse_containment_granularity(require_member(object, "containment_granularity", path),
                                    child_path(path, "containment_granularity"))};
}

[[nodiscard]] std::vector<ResourceBindingDTO> parse_resource_bindings(const CanonicalValue& value,
                                                                      std::string_view path) {
  const auto& array = require_array(value, path);
  std::vector<ResourceBindingDTO> result;
  std::set<std::string, std::less<>> resource_ids;
  result.reserve(array.size());
  for (std::size_t index = 0; index < array.size(); ++index) {
    const std::string item_path = index_path(path, index);
    const auto& object = require_object(array[index], item_path);
    require_exact_fields(object, {"resource_role_id", "resource_id"}, item_path);
    ResourceBindingDTO binding{
        parse_identity<ResourceRoleId>(require_member(object, "resource_role_id", item_path),
                                       child_path(item_path, "resource_role_id")),
        parse_identity<ResourceId>(require_member(object, "resource_id", item_path),
                                   child_path(item_path, "resource_id"))};
    if (!resource_ids.insert(binding.resource_id.value()).second) {
      fail(path, "duplicate resource_id '" + binding.resource_id.value() + "'");
    }
    result.push_back(std::move(binding));
  }
  return result;
}

[[nodiscard]] std::vector<RouteEndpointProjectionDTO> parse_route_endpoint_projections(
    const CanonicalValue& value, std::string_view path) {
  const auto& array = require_array(value, path);
  std::vector<RouteEndpointProjectionDTO> result;
  result.reserve(array.size());
  for (std::size_t index = 0; index < array.size(); ++index) {
    const std::string item_path = index_path(path, index);
    const auto& object = require_object(array[index], item_path);
    require_exact_fields(
        object, {"control_route_id", "command_endpoint_role_ids", "state_endpoint_role_ids"},
        item_path);
    result.push_back(RouteEndpointProjectionDTO{
        parse_identity<ControlRouteId>(require_member(object, "control_route_id", item_path),
                                       child_path(item_path, "control_route_id")),
        parse_identity_set<EndpointRoleId>(
            require_member(object, "command_endpoint_role_ids", item_path),
            child_path(item_path, "command_endpoint_role_ids")),
        parse_identity_set<EndpointRoleId>(
            require_member(object, "state_endpoint_role_ids", item_path),
            child_path(item_path, "state_endpoint_role_ids"))});
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.control_route_id < right.control_route_id;
  });
  const auto duplicate =
      std::adjacent_find(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.control_route_id == right.control_route_id;
      });
  if (duplicate != result.end()) {
    fail(path, "duplicate control_route_id '" + duplicate->control_route_id.value() + "'");
  }
  return result;
}

[[nodiscard]] core::RuntimeScaleAcknowledgementKind
parse_runtime_scale_acknowledgement(const CanonicalValue& value,
                                    std::string_view path) {
  const auto& token = require_string(value, path);
  if (token == "applied_state") {
    return core::RuntimeScaleAcknowledgementKind::kAppliedState;
  }
  fail(path, "unknown runtime-scale acknowledgement kind");
}

[[nodiscard]] std::int64_t parse_positive_int64(const CanonicalValue& value,
                                                std::string_view path) {
  constexpr double kMaxSafeJsonInteger = 9007199254740991.0;
  const double number = require_number(value, path);
  if (number <= 0.0 || std::trunc(number) != number ||
      number > kMaxSafeJsonInteger) {
    fail(path, "expected a positive integral I-JSON-safe value");
  }
  return static_cast<std::int64_t>(number);
}

[[nodiscard]] std::vector<core::RuntimeScaleRouteQualificationDTO>
parse_runtime_scale_route_qualifications(const CanonicalValue& value,
                                         std::string_view path) {
  const auto& array = require_array(value, path);
  if (array.empty()) {
    fail(path, "present runtime-scale qualification table must be non-empty");
  }
  std::vector<core::RuntimeScaleRouteQualificationDTO> result;
  result.reserve(array.size());
  for (std::size_t index = 0U; index < array.size(); ++index) {
    const auto item_path = index_path(path, index);
    const auto& object = require_object(array[index], item_path);
    require_fields(object,
                   {"control_route_id", "scaling_domain_id",
                    "command_endpoint_role_id",
                    "applied_state_endpoint_role_id",
                    "acknowledgement_kind", "maximum_slew_per_second",
                    "freshness_budget_ns"},
                   {"coordinated_barrier_id"}, item_path);
    const double maximum_slew = require_number(
        require_member(object, "maximum_slew_per_second", item_path),
        child_path(item_path, "maximum_slew_per_second"));
    if (maximum_slew <= 0.0) {
      fail(child_path(item_path, "maximum_slew_per_second"),
           "must be positive");
    }
    std::optional<core::CoordinatedScalingBarrierId> barrier;
    if (const auto* member = find_member(object, "coordinated_barrier_id")) {
      barrier = parse_identity<core::CoordinatedScalingBarrierId>(
          *member, child_path(item_path, "coordinated_barrier_id"));
    }
    result.push_back(core::RuntimeScaleRouteQualificationDTO{
        parse_identity<core::ControlRouteId>(
            require_member(object, "control_route_id", item_path),
            child_path(item_path, "control_route_id")),
        parse_identity<core::ScalingDomainId>(
            require_member(object, "scaling_domain_id", item_path),
            child_path(item_path, "scaling_domain_id")),
        parse_identity<core::EndpointRoleId>(
            require_member(object, "command_endpoint_role_id", item_path),
            child_path(item_path, "command_endpoint_role_id")),
        parse_identity<core::EndpointRoleId>(
            require_member(object, "applied_state_endpoint_role_id", item_path),
            child_path(item_path, "applied_state_endpoint_role_id")),
        std::move(barrier),
        parse_runtime_scale_acknowledgement(
            require_member(object, "acknowledgement_kind", item_path),
            child_path(item_path, "acknowledgement_kind")),
        maximum_slew,
        parse_positive_int64(
            require_member(object, "freshness_budget_ns", item_path),
            child_path(item_path, "freshness_budget_ns"))});
  }
  std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
    return left.control_route_id < right.control_route_id;
  });
  if (std::adjacent_find(result.begin(), result.end(),
                         [](const auto& left, const auto& right) {
                           return left.control_route_id == right.control_route_id;
                         }) != result.end()) {
    fail(path, "duplicate runtime-scale control_route_id");
  }
  return result;
}

[[nodiscard]] ControlBindingDTO parse_control_binding(const CanonicalValue& value,
                                                      std::string_view path) {
  const auto& object = require_object(value, path);
  require_fields(object,
                 {"control_binding_id",
                        "control_binding_revision",
                        "controllable_component_id",
                        "command_target_id",
                        "capability_id",
                        "capability_definition_revision",
                        "control_domain_id",
                        "resource_bindings",
                        "control_interface_contract_id",
                        "control_interface_revision",
                        "controller_topology_id",
                        "topology_revision",
                        "legal_control_route_ids",
                        "route_endpoint_projections",
                        "provider_instance_id",
                        "provider_runtime_unit_id",
                        "provider_manifest_id",
                        "provider_manifest_revision",
                        "state_authority_id",
                        "state_authority_class",
                        "stop_evidence_policy_id",
                        "stop_evidence_policy_revision",
                        "containment_granularity"},
                 {"runtime_scale_route_qualifications"}, path);

  std::vector<ControlRouteId> legal_routes =
      parse_identity_set<ControlRouteId>(require_member(object, "legal_control_route_ids", path),
                                         child_path(path, "legal_control_route_ids"));
  if (legal_routes.empty()) {
    fail(child_path(path, "legal_control_route_ids"), "must contain at least one route");
  }
  std::vector<RouteEndpointProjectionDTO> projections =
      parse_route_endpoint_projections(require_member(object, "route_endpoint_projections", path),
                                       child_path(path, "route_endpoint_projections"));
  if (legal_routes.size() != projections.size()) {
    fail(child_path(path, "route_endpoint_projections"),
         "must contain exactly one projection per legal route");
  }
  for (std::size_t index = 0; index < legal_routes.size(); ++index) {
    if (legal_routes[index] != projections[index].control_route_id) {
      fail(child_path(path, "route_endpoint_projections"),
           "projection route IDs must exactly equal legal route IDs");
    }
  }

  std::vector<core::RuntimeScaleRouteQualificationDTO>
      runtime_scale_route_qualifications;
  if (const auto* qualifications =
          find_member(object, "runtime_scale_route_qualifications")) {
    runtime_scale_route_qualifications =
        parse_runtime_scale_route_qualifications(
            *qualifications,
            child_path(path, "runtime_scale_route_qualifications"));
  }
  for (const auto& qualification : runtime_scale_route_qualifications) {
    const auto projection = std::find_if(
        projections.begin(), projections.end(), [&](const auto& candidate) {
          return candidate.control_route_id == qualification.control_route_id;
        });
    if (projection == projections.end()) {
      fail(child_path(path, "runtime_scale_route_qualifications"),
           "qualification names a route outside the binding");
    }
    if (std::find(projection->command_endpoint_role_ids.begin(),
                  projection->command_endpoint_role_ids.end(),
                  qualification.command_endpoint_role_id) ==
            projection->command_endpoint_role_ids.end() ||
        std::find(projection->state_endpoint_role_ids.begin(),
                  projection->state_endpoint_role_ids.end(),
                  qualification.applied_state_endpoint_role_id) ==
            projection->state_endpoint_role_ids.end()) {
      fail(child_path(path, "runtime_scale_route_qualifications"),
           "runtime-scale command/applied-state roles are absent from the exact route projection");
    }
  }

  return ControlBindingDTO{
      parse_identity<ControlBindingId>(require_member(object, "control_binding_id", path),
                                       child_path(path, "control_binding_id")),
      parse_digest<ControlBindingRevision>(require_member(object, "control_binding_revision", path),
                                           child_path(path, "control_binding_revision")),
      parse_identity<ControllableComponentId>(
          require_member(object, "controllable_component_id", path),
          child_path(path, "controllable_component_id")),
      parse_identity<CommandTargetId>(require_member(object, "command_target_id", path),
                                      child_path(path, "command_target_id")),
      parse_identity<CapabilityId>(require_member(object, "capability_id", path),
                                   child_path(path, "capability_id")),
      parse_digest<CapabilityDefinitionRevision>(
          require_member(object, "capability_definition_revision", path),
          child_path(path, "capability_definition_revision")),
      parse_identity<ControlDomainId>(require_member(object, "control_domain_id", path),
                                      child_path(path, "control_domain_id")),
      parse_resource_bindings(require_member(object, "resource_bindings", path),
                              child_path(path, "resource_bindings")),
      parse_identity<ControlInterfaceContractId>(
          require_member(object, "control_interface_contract_id", path),
          child_path(path, "control_interface_contract_id")),
      parse_digest<ControlInterfaceRevision>(
          require_member(object, "control_interface_revision", path),
          child_path(path, "control_interface_revision")),
      parse_identity<ControllerTopologyId>(require_member(object, "controller_topology_id", path),
                                           child_path(path, "controller_topology_id")),
      parse_digest<TopologyRevision>(require_member(object, "topology_revision", path),
                                     child_path(path, "topology_revision")),
      std::move(legal_routes),
      std::move(projections),
      parse_identity<ProviderInstanceId>(require_member(object, "provider_instance_id", path),
                                         child_path(path, "provider_instance_id")),
      parse_identity<RuntimeUnitId>(
          require_member(object, "provider_runtime_unit_id", path),
          child_path(path, "provider_runtime_unit_id")),
      parse_identity<ProviderManifestId>(require_member(object, "provider_manifest_id", path),
                                        child_path(path, "provider_manifest_id")),
      parse_digest<ProviderManifestRevision>(
          require_member(object, "provider_manifest_revision", path),
          child_path(path, "provider_manifest_revision")),
      parse_identity<StateAuthorityId>(require_member(object, "state_authority_id", path),
                                       child_path(path, "state_authority_id")),
      parse_state_authority_class(require_member(object, "state_authority_class", path),
                                  child_path(path, "state_authority_class")),
      parse_identity<StopEvidencePolicyId>(require_member(object, "stop_evidence_policy_id", path),
                                           child_path(path, "stop_evidence_policy_id")),
      parse_digest<StopEvidencePolicyRevision>(
          require_member(object, "stop_evidence_policy_revision", path),
          child_path(path, "stop_evidence_policy_revision")),
      parse_containment_granularity(require_member(object, "containment_granularity", path),
                                    child_path(path, "containment_granularity")),
      std::move(runtime_scale_route_qualifications)};
}

[[nodiscard]] ComponentAttachmentEndpointDTO parse_attachment_endpoint(const CanonicalValue& value,
                                                                       std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(object, {"controllable_component_id", "attachment_interface_id"}, path);
  return ComponentAttachmentEndpointDTO{
      parse_identity<ControllableComponentId>(
          require_member(object, "controllable_component_id", path),
          child_path(path, "controllable_component_id")),
      parse_identity<AttachmentInterfaceId>(require_member(object, "attachment_interface_id", path),
                                            child_path(path, "attachment_interface_id"))};
}

[[nodiscard]] ComponentAttachmentEdgeDTO parse_component_attachment(const CanonicalValue& value,
                                                                    std::string_view path) {
  const auto& object = require_object(value, path);
  require_exact_fields(object,
                       {"component_attachment_id", "component_attachment_revision", "parent",
                        "child", "child_interface_pose_in_parent_interface"},
                       path);
  return ComponentAttachmentEdgeDTO{
      parse_identity<ComponentAttachmentId>(require_member(object, "component_attachment_id", path),
                                            child_path(path, "component_attachment_id")),
      parse_digest<ComponentAttachmentRevision>(
          require_member(object, "component_attachment_revision", path),
          child_path(path, "component_attachment_revision")),
      parse_attachment_endpoint(require_member(object, "parent", path), child_path(path, "parent")),
      parse_attachment_endpoint(require_member(object, "child", path), child_path(path, "child")),
      parse_placement_pose(require_member(object, "child_interface_pose_in_parent_interface", path),
                           child_path(path, "child_interface_pose_in_parent_interface"))};
}

template <typename Record, typename Parser, typename Key>
[[nodiscard]] std::vector<Record> parse_record_table(const CanonicalValue& value,
                                                     std::string_view path, Parser parser,
                                                     Key key) {
  const auto& array = require_array(value, path);
  std::vector<Record> records;
  records.reserve(array.size());
  for (std::size_t index = 0; index < array.size(); ++index) {
    records.push_back(parser(array[index], index_path(path, index)));
  }
  std::sort(records.begin(), records.end(),
            [&key](const Record& left, const Record& right) { return key(left) < key(right); });
  const auto duplicate = std::adjacent_find(
      records.begin(), records.end(),
      [&key](const Record& left, const Record& right) { return key(left) == key(right); });
  if (duplicate != records.end()) {
    fail(path, "duplicate record identity '" + key(*duplicate).value() + "'");
  }
  return records;
}

[[nodiscard]] CanonicalValue json_to_canonical(const JsonValueDTO& value);

[[nodiscard]] CanonicalValue json_object_to_canonical(const JsonObjectDTO& object) {
  CanonicalValue::Object result;
  result.reserve(object.size());
  for (const auto& [name, value] : object) {
    result.emplace_back(name, json_to_canonical(value));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue json_to_canonical(const JsonValueDTO& value) {
  return std::visit(
      [](const auto& current) -> CanonicalValue {
        using Value = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t>) {
          return CanonicalValue(nullptr);
        } else if constexpr (std::is_same_v<Value, bool>) {
          return CanonicalValue(current);
        } else if constexpr (std::is_same_v<Value, double>) {
          return CanonicalValue(current);
        } else if constexpr (std::is_same_v<Value, std::string>) {
          return CanonicalValue(current);
        } else if constexpr (std::is_same_v<Value, JsonValueDTO::Array>) {
          CanonicalValue::Array array;
          array.reserve(current.size());
          for (const auto& item : current) {
            array.push_back(json_to_canonical(item));
          }
          return CanonicalValue(std::move(array));
        } else {
          return json_object_to_canonical(current);
        }
      },
      value.value);
}

template <typename StrongType>
[[nodiscard]] CanonicalValue identity_value(const StrongType& identity) {
  return CanonicalValue(identity.value());
}

template <typename StrongType>
[[nodiscard]] CanonicalValue identity_array(const std::vector<StrongType>& identities) {
  CanonicalValue::Array array;
  array.reserve(identities.size());
  for (const auto& identity : identities) {
    array.push_back(identity_value(identity));
  }
  return CanonicalValue(std::move(array));
}

[[nodiscard]] CanonicalValue placement_pose_value(const PlacementPoseDTO& pose) {
  CanonicalValue::Array position;
  position.reserve(pose.position_m.size());
  for (const double coordinate : pose.position_m) {
    position.emplace_back(coordinate);
  }
  CanonicalValue::Array orientation;
  orientation.reserve(pose.orientation_xyzw.size());
  for (const double coordinate : pose.orientation_xyzw) {
    orientation.emplace_back(coordinate);
  }
  return CanonicalValue(
      CanonicalValue::Object{{"position_m", CanonicalValue(std::move(position))},
                             {"orientation_xyzw", CanonicalValue(std::move(orientation))}});
}

[[nodiscard]] CanonicalValue runtime_unit_value(const RuntimeUnitDTO& record) {
  return CanonicalValue(CanonicalValue::Object{
      {"runtime_unit_id", identity_value(record.runtime_unit_id)},
      {"runtime_profile_id", identity_value(record.runtime_profile_id)},
      {"runtime_profile_revision", identity_value(record.runtime_profile_revision)},
      {"dependency_runtime_unit_ids", identity_array(record.dependency_runtime_unit_ids)},
      {"configuration", json_object_to_canonical(record.configuration)}});
}

[[nodiscard]] CanonicalValue model_instance_value(const ModelInstanceDTO& record) {
  CanonicalValue::Object object{
      {"model_instance_id", identity_value(record.model_instance_id)},
      {"model_asset_id", identity_value(record.model_asset_id)},
      {"model_revision", identity_value(record.model_revision)}};
  if (record.root_pose_in_cell.has_value()) {
    object.emplace_back("root_pose_in_cell",
                        placement_pose_value(*record.root_pose_in_cell));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue controllable_component_value(const ControllableComponentDTO& record) {
  CanonicalValue::Array model_bindings;
  model_bindings.reserve(record.model_role_bindings.size());
  for (const auto& binding : record.model_role_bindings) {
    model_bindings.emplace_back(
        CanonicalValue::Object{{"model_role_id", identity_value(binding.model_role_id)},
                               {"model_instance_id", identity_value(binding.model_instance_id)}});
  }
  CanonicalValue::Object object{
      {"controllable_component_id", identity_value(record.controllable_component_id)},
      {"component_type_id", identity_value(record.component_type_id)},
      {"component_type_revision", identity_value(record.component_type_revision)},
      {"model_role_bindings", CanonicalValue(std::move(model_bindings))},
      {"runtime_unit_ids", identity_array(record.runtime_unit_ids)}};
  if (record.root_pose_in_cell.has_value()) {
    object.emplace_back("root_pose_in_cell",
                        placement_pose_value(*record.root_pose_in_cell));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue control_domain_value(const ControlDomainDTO& record) {
  if (const auto *provider = record.provider()) {
    CanonicalValue::Array names;
    for (const auto &name : provider->signal_names)
      names.emplace_back(name);
    return CanonicalValue(CanonicalValue::Object{
        {"control_domain_id", identity_value(record.control_domain_id)},
        {"control_domain_revision",
         identity_value(record.control_domain_revision)},
        {"runtime_unit_ids", identity_array(record.runtime_unit_ids)},
        {"control_coordinator_endpoint_role",
         identity_value(record.control_coordinator_endpoint_role)},
        {"containment_granularity", CanonicalValue(std::string(core::to_string(
                                        record.containment_granularity)))},
        {"kind", CanonicalValue("provider")},
        {"provider",
         CanonicalValue(CanonicalValue::Object{
             {"controllable_component_id",
              identity_value(provider->controllable_component_id)},
             {"provider_runtime_unit_id",
              identity_value(provider->provider_runtime_unit_id)},
             {"provider_instance_id",
              identity_value(provider->provider_instance_id)},
             {"provider_manifest_id",
              identity_value(provider->provider_manifest_id)},
             {"provider_manifest_revision",
              identity_value(provider->provider_manifest_revision)},
             {"lifecycle_plan_catalog_digest",
              identity_value(provider->lifecycle_plan_catalog_digest)},
             {"signal_configuration_digest",
              identity_value(provider->signal_configuration_digest)},
             {"signal_names", CanonicalValue(std::move(names))},
             {"lifecycle_command_timeout_ns",
              CanonicalValue(static_cast<double>(
                  provider->lifecycle_command_timeout_ns))}})}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"control_domain_id", identity_value(record.control_domain_id)},
      {"control_domain_revision",
       identity_value(record.control_domain_revision)},
      {"runtime_unit_ids", identity_array(record.runtime_unit_ids)},
      {"control_coordinator_endpoint_role",
       identity_value(record.control_coordinator_endpoint_role)},
      {"controller_topology_id",
       identity_value(record.controller().controller_topology_id)},
      {"topology_revision",
       identity_value(record.controller().topology_revision)},
      {"containment_granularity", CanonicalValue(std::string(core::to_string(
                                      record.containment_granularity)))}});
}

[[nodiscard]] CanonicalValue control_binding_value(const ControlBindingDTO& record) {
  CanonicalValue::Array resources;
  resources.reserve(record.resource_bindings.size());
  for (const auto& binding : record.resource_bindings) {
    resources.emplace_back(
        CanonicalValue::Object{{"resource_role_id", identity_value(binding.resource_role_id)},
                               {"resource_id", identity_value(binding.resource_id)}});
  }
  CanonicalValue::Array projections;
  projections.reserve(record.route_endpoint_projections.size());
  for (const auto& projection : record.route_endpoint_projections) {
    projections.emplace_back(CanonicalValue::Object{
        {"control_route_id", identity_value(projection.control_route_id)},
        {"command_endpoint_role_ids", identity_array(projection.command_endpoint_role_ids)},
        {"state_endpoint_role_ids", identity_array(projection.state_endpoint_role_ids)}});
  }
  CanonicalValue::Object object{
      {"control_binding_id", identity_value(record.control_binding_id)},
      {"control_binding_revision", identity_value(record.control_binding_revision)},
      {"controllable_component_id", identity_value(record.controllable_component_id)},
      {"command_target_id", identity_value(record.command_target_id)},
      {"capability_id", identity_value(record.capability_id)},
      {"capability_definition_revision",
       identity_value(record.capability_definition_revision)},
      {"control_domain_id", identity_value(record.control_domain_id)},
      {"resource_bindings", CanonicalValue(std::move(resources))},
      {"control_interface_contract_id", identity_value(record.control_interface_contract_id)},
      {"control_interface_revision", identity_value(record.control_interface_revision)},
      {"controller_topology_id", identity_value(record.controller_topology_id)},
      {"topology_revision", identity_value(record.topology_revision)},
      {"legal_control_route_ids", identity_array(record.legal_control_route_ids)},
      {"route_endpoint_projections", CanonicalValue(std::move(projections))},
      {"provider_instance_id", identity_value(record.provider_instance_id)},
      {"provider_runtime_unit_id",
       identity_value(record.provider_runtime_unit_id)},
      {"provider_manifest_id", identity_value(record.provider_manifest_id)},
      {"provider_manifest_revision", identity_value(record.provider_manifest_revision)},
      {"state_authority_id", identity_value(record.state_authority_id)},
      {"state_authority_class",
       CanonicalValue(std::string(core::to_string(record.state_authority_class)))},
      {"stop_evidence_policy_id", identity_value(record.stop_evidence_policy_id)},
      {"stop_evidence_policy_revision", identity_value(record.stop_evidence_policy_revision)},
      {"containment_granularity",
       CanonicalValue(std::string(core::to_string(record.containment_granularity)))}};
  if (!record.runtime_scale_route_qualifications.empty()) {
    CanonicalValue::Array qualifications;
    qualifications.reserve(record.runtime_scale_route_qualifications.size());
    for (const auto& qualification :
         record.runtime_scale_route_qualifications) {
      CanonicalValue::Object value{
          {"control_route_id",
           identity_value(qualification.control_route_id)},
          {"scaling_domain_id",
           identity_value(qualification.scaling_domain_id)},
          {"command_endpoint_role_id",
           identity_value(qualification.command_endpoint_role_id)},
          {"applied_state_endpoint_role_id",
           identity_value(qualification.applied_state_endpoint_role_id)},
          {"acknowledgement_kind",
           CanonicalValue(std::string(
               core::to_string(qualification.acknowledgement_kind)))},
          {"maximum_slew_per_second",
           CanonicalValue(qualification.maximum_slew_per_second)},
          {"freshness_budget_ns",
           CanonicalValue(
               static_cast<double>(qualification.freshness_budget_ns))}};
      if (qualification.coordinated_barrier_id.has_value()) {
        value.emplace_back(
            "coordinated_barrier_id",
            identity_value(*qualification.coordinated_barrier_id));
      }
      qualifications.emplace_back(std::move(value));
    }
    object.emplace_back("runtime_scale_route_qualifications",
                        CanonicalValue(std::move(qualifications)));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue attachment_endpoint_value(
    const ComponentAttachmentEndpointDTO& endpoint) {
  return CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id", identity_value(endpoint.controllable_component_id)},
      {"attachment_interface_id", identity_value(endpoint.attachment_interface_id)}});
}

[[nodiscard]] CanonicalValue component_attachment_value(const ComponentAttachmentEdgeDTO& record) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_attachment_id", identity_value(record.component_attachment_id)},
      {"component_attachment_revision", identity_value(record.component_attachment_revision)},
      {"parent", attachment_endpoint_value(record.parent)},
      {"child", attachment_endpoint_value(record.child)},
      {"child_interface_pose_in_parent_interface",
       placement_pose_value(record.child_interface_pose_in_parent_interface)}});
}

template <typename Record, typename Converter>
[[nodiscard]] CanonicalValue record_array(const std::vector<Record>& records, Converter converter) {
  CanonicalValue::Array array;
  array.reserve(records.size());
  for (const auto& record : records) {
    array.push_back(converter(record));
  }
  return CanonicalValue(std::move(array));
}

[[nodiscard]] CanonicalValue graph_value(const CellGraphDTO& graph) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(graph.api_version)},
      {"graph_revision", identity_value(graph.graph_revision)},
      {"cell_id", identity_value(graph.cell_id)},
      {"runtime_units", record_array(graph.runtime_units, runtime_unit_value)},
      {"model_instances", record_array(graph.model_instances, model_instance_value)},
      {"controllable_components",
       record_array(graph.controllable_components, controllable_component_value)},
      {"control_domains", record_array(graph.control_domains, control_domain_value)},
      {"control_bindings", record_array(graph.control_bindings, control_binding_value)},
      {"component_attachments",
       record_array(graph.component_attachments, component_attachment_value)}});
}

[[nodiscard]] detail::ContentIdentitySpec nested_identity_spec(
    std::string identity_domain, std::string own_field) {
  return detail::ContentIdentitySpec{
      .identity_domain = std::move(identity_domain),
      .api_version = std::string(core::kCellGraphApiVersion),
      .own_field = std::move(own_field),
      .root_shape = detail::ContentIdentityRootShape::kNestedRecord,
  };
}

[[nodiscard]] detail::ContentIdentitySpec graph_identity_spec() {
  return detail::ContentIdentitySpec{
      .identity_domain = "graph-revision",
      .api_version = std::string(core::kCellGraphApiVersion),
      .own_field = "graph_revision",
  };
}

[[nodiscard]] RuntimeProfileKey runtime_profile_key(
    const core::RuntimeProfileDTO& profile) {
  return {profile.runtime_profile_id.value(),
          profile.runtime_profile_revision.value()};
}

[[nodiscard]] RuntimeProfileKey runtime_profile_key(
    const RuntimeUnitDTO& unit) {
  return {unit.runtime_profile_id.value(),
          unit.runtime_profile_revision.value()};
}

[[nodiscard]] std::string profile_key_label(
    const RuntimeProfileKey& key) {
  return "'" + key.first + "' at revision '" + key.second + "'";
}

void compute_nested_graph_revisions(CellGraphDTO& graph) {
  const auto domain_spec =
      nested_identity_spec("control-domain-revision",
                           "control_domain_revision");
  for (auto& domain : graph.control_domains) {
    domain.control_domain_revision = ControlDomainRevision(
        detail::compute_content_identity(control_domain_value(domain),
                                         domain_spec));
  }

  const auto binding_spec =
      nested_identity_spec("control-binding-revision",
                           "control_binding_revision");
  for (auto& binding : graph.control_bindings) {
    binding.control_binding_revision = ControlBindingRevision(
        detail::compute_content_identity(control_binding_value(binding),
                                         binding_spec));
  }

  const auto attachment_spec =
      nested_identity_spec("component-attachment-revision",
                           "component_attachment_revision");
  for (auto& attachment : graph.component_attachments) {
    attachment.component_attachment_revision = ComponentAttachmentRevision(
        detail::compute_content_identity(
            component_attachment_value(attachment), attachment_spec));
  }
}

void verify_nested_graph_revisions(const CellGraphDTO& graph) {
  const auto domain_spec =
      nested_identity_spec("control-domain-revision",
                           "control_domain_revision");
  for (const auto& domain : graph.control_domains) {
    if (!detail::verify_content_identity(control_domain_value(domain),
                                         domain_spec)) {
      fail("$.control_domains",
           "control_domain_revision mismatch for '" +
               domain.control_domain_id.value() + "'");
    }
  }

  const auto binding_spec =
      nested_identity_spec("control-binding-revision",
                           "control_binding_revision");
  for (const auto& binding : graph.control_bindings) {
    if (!detail::verify_content_identity(control_binding_value(binding),
                                         binding_spec)) {
      fail("$.control_bindings",
           "control_binding_revision mismatch for '" +
               binding.control_binding_id.value() + "'");
    }
  }

  const auto attachment_spec =
      nested_identity_spec("component-attachment-revision",
                           "component_attachment_revision");
  for (const auto& attachment : graph.component_attachments) {
    if (!detail::verify_content_identity(
            component_attachment_value(attachment), attachment_spec)) {
      fail("$.component_attachments",
           "component_attachment_revision mismatch for '" +
               attachment.component_attachment_id.value() + "'");
    }
  }
}

template <typename Record, typename Getter>
[[nodiscard]] std::set<std::string, std::less<>> collect_ids(const std::vector<Record>& records,
                                                             Getter getter) {
  std::set<std::string, std::less<>> ids;
  for (const auto& record : records) {
    ids.insert(getter(record).value());
  }
  return ids;
}

void require_reference(const std::set<std::string, std::less<>>& known, std::string_view value,
                       std::string_view path) {
  if (!known.contains(value)) {
    fail(path, "dangling reference '" + std::string(value) + "'");
  }
}

void require_acyclic(const std::map<std::string, std::vector<std::string>, std::less<>>& edges,
                     std::string_view path) {
  std::map<std::string, std::size_t, std::less<>> in_degree;
  for (const auto& [node, children] : edges) {
    in_degree.try_emplace(node, 0U);
    for (const std::string& child : children) {
      ++in_degree[child];
    }
  }
  std::set<std::string, std::less<>> ready;
  for (const auto& [node, degree] : in_degree) {
    if (degree == 0U) {
      ready.insert(node);
    }
  }
  std::size_t visited = 0U;
  while (!ready.empty()) {
    const std::string node = *ready.begin();
    ready.erase(ready.begin());
    ++visited;
    const auto children = edges.find(node);
    if (children == edges.end()) {
      continue;
    }
    for (const std::string& child : children->second) {
      auto& degree = in_degree.at(child);
      --degree;
      if (degree == 0U) {
        ready.insert(child);
      }
    }
  }
  if (visited != in_degree.size()) {
    const auto cyclic = std::find_if(in_degree.begin(), in_degree.end(),
                                     [](const auto& entry) { return entry.second != 0U; });
    fail(path, "cycle detected at '" + cyclic->first + "'");
  }
}

void validate_intrinsic(const CellGraphDTO& graph) {
  const auto runtime_ids = collect_ids(
      graph.runtime_units,
      [](const RuntimeUnitDTO& unit) -> const RuntimeUnitId& { return unit.runtime_unit_id; });
  const auto model_ids = collect_ids(graph.model_instances,
                                     [](const ModelInstanceDTO& model) -> const ModelInstanceId& {
                                       return model.model_instance_id;
                                     });
  const auto component_ids =
      collect_ids(graph.controllable_components,
                  [](const ControllableComponentDTO& component) -> const ControllableComponentId& {
                    return component.controllable_component_id;
                  });
  const auto domain_ids = collect_ids(graph.control_domains,
                                      [](const ControlDomainDTO& domain) -> const ControlDomainId& {
                                        return domain.control_domain_id;
                                      });

  std::map<std::string, std::vector<std::string>, std::less<>> runtime_dependencies;
  for (const auto& unit : graph.runtime_units) {
    auto& dependencies = runtime_dependencies[unit.runtime_unit_id.value()];
    for (const auto& dependency : unit.dependency_runtime_unit_ids) {
      require_reference(runtime_ids, dependency.value(),
                        "runtime_units.dependency_runtime_unit_ids");
      dependencies.push_back(dependency.value());
    }
  }
  require_acyclic(runtime_dependencies, "runtime_units");

  std::map<std::string, std::string, std::less<>> model_owners;
  for (const auto& component : graph.controllable_components) {
    for (const auto& unit_id : component.runtime_unit_ids) {
      require_reference(runtime_ids, unit_id.value(), "controllable_components.runtime_unit_ids");
    }
    for (const auto& binding : component.model_role_bindings) {
      require_reference(model_ids, binding.model_instance_id.value(),
                        "controllable_components.model_role_bindings");
      const auto [iterator, inserted] = model_owners.emplace(
          binding.model_instance_id.value(), component.controllable_component_id.value());
      if (!inserted) {
        fail("controllable_components.model_role_bindings",
             "model instance '" + binding.model_instance_id.value() +
                 "' is bound more than once; first owner is '" + iterator->second + "'");
      }
    }
  }
  for (const auto& model : graph.model_instances) {
    const bool owned = model_owners.contains(model.model_instance_id.value());
    if (owned && model.root_pose_in_cell.has_value()) {
      fail("model_instances.root_pose_in_cell",
           "owned model instance '" + model.model_instance_id.value() + "' must omit root pose");
    }
    if (!owned && !model.root_pose_in_cell.has_value()) {
      fail("model_instances.root_pose_in_cell",
           "passive model instance '" + model.model_instance_id.value() +
               "' requires a root pose");
    }
  }

  std::map<std::string, const ControlDomainDTO*, std::less<>> domains;
  for (const auto& domain : graph.control_domains) {
    domains.emplace(domain.control_domain_id.value(), &domain);
    for (const auto& unit_id : domain.runtime_unit_ids) {
      require_reference(runtime_ids, unit_id.value(), "control_domains.runtime_unit_ids");
    }
    if (const auto *provider = domain.provider()) {
      if (!provider->locally_valid())
        fail("control_domains.provider", "invalid provider-domain proof");
      require_reference(component_ids,
                        provider->controllable_component_id.value(),
                        "control_domains.provider.controllable_component_id");
      require_reference(runtime_ids, provider->provider_runtime_unit_id.value(),
                        "control_domains.provider.provider_runtime_unit_id");
      const auto component = std::find_if(
          graph.controllable_components.begin(),
          graph.controllable_components.end(), [&](const auto &value) {
            return value.controllable_component_id ==
                   provider->controllable_component_id;
          });
      if (std::find(domain.runtime_unit_ids.begin(),
                    domain.runtime_unit_ids.end(),
                    provider->provider_runtime_unit_id) ==
              domain.runtime_unit_ids.end() ||
          std::find(component->runtime_unit_ids.begin(),
                    component->runtime_unit_ids.end(),
                    provider->provider_runtime_unit_id) ==
              component->runtime_unit_ids.end())
        fail("control_domains.provider",
             "provider runtime must belong to its actual component and domain");
    }
  }

  std::map<std::string,
           std::tuple<std::string, std::string, std::string>, std::less<>>
      provider_manifest_by_instance;
  for (const auto& binding : graph.control_bindings) {
    require_reference(component_ids, binding.controllable_component_id.value(),
                      "control_bindings.controllable_component_id");
    require_reference(domain_ids, binding.control_domain_id.value(),
                      "control_bindings.control_domain_id");
    require_reference(runtime_ids, binding.provider_runtime_unit_id.value(),
                      "control_bindings.provider_runtime_unit_id");
    const ControlDomainDTO& domain = *domains.at(binding.control_domain_id.value());
    if (domain.provider())
      fail("control_bindings.control_domain_id",
           "controller binding cannot target a provider-only domain");
    const auto component = std::find_if(
        graph.controllable_components.begin(),
        graph.controllable_components.end(), [&](const auto& candidate) {
          return candidate.controllable_component_id ==
                 binding.controllable_component_id;
        });
    if (component == graph.controllable_components.end() ||
        std::find(component->runtime_unit_ids.begin(),
                  component->runtime_unit_ids.end(),
                  binding.provider_runtime_unit_id) ==
            component->runtime_unit_ids.end() ||
        std::find(domain.runtime_unit_ids.begin(), domain.runtime_unit_ids.end(),
                  binding.provider_runtime_unit_id) ==
            domain.runtime_unit_ids.end()) {
      fail("control_bindings.provider_runtime_unit_id",
           "provider runtime must belong to both the binding component and "
           "control domain");
    }
    if (binding.controller_topology_id !=
            domain.controller().controller_topology_id ||
        binding.topology_revision != domain.controller().topology_revision) {
      fail("control_bindings", "binding topology identity does not match its control domain");
    }
    const auto [selected_manifest, inserted] =
        provider_manifest_by_instance.emplace(
            binding.provider_instance_id.value(),
            std::tuple{binding.provider_runtime_unit_id.value(),
                       binding.provider_manifest_id.value(),
                       binding.provider_manifest_revision.value()});
    if (!inserted &&
        selected_manifest->second !=
            std::tuple{binding.provider_runtime_unit_id.value(),
                       binding.provider_manifest_id.value(),
                       binding.provider_manifest_revision.value()}) {
      fail("control_bindings",
           "provider instance '" + binding.provider_instance_id.value() +
               "' selects more than one provider runtime/manifest identity");
    }
  }

  using Endpoint = std::pair<std::string, std::string>;
  using Edge = std::pair<Endpoint, Endpoint>;
  std::set<Edge> attachment_edges;
  std::set<std::string, std::less<>> child_components;
  std::map<std::string, std::vector<std::string>, std::less<>> attachment_dependencies;
  for (const auto& attachment : graph.component_attachments) {
    require_reference(component_ids, attachment.parent.controllable_component_id.value(),
                      "component_attachments.parent");
    require_reference(component_ids, attachment.child.controllable_component_id.value(),
                      "component_attachments.child");
    const Endpoint parent{attachment.parent.controllable_component_id.value(),
                          attachment.parent.attachment_interface_id.value()};
    const Endpoint child{attachment.child.controllable_component_id.value(),
                         attachment.child.attachment_interface_id.value()};
    if (parent.first == child.first) {
      fail("component_attachments", "a component cannot attach to itself");
    }
    if (!attachment_edges.emplace(parent, child).second) {
      fail("component_attachments", "duplicate attachment edge");
    }
    if (!child_components.insert(child.first).second) {
      fail("component_attachments", "one child component has multiple incoming attachments");
    }
    attachment_dependencies[parent.first].push_back(child.first);
  }
  require_acyclic(attachment_dependencies, "component_attachments");

  for (const auto& component : graph.controllable_components) {
    const bool has_model = !component.model_role_bindings.empty();
    const bool is_child = child_components.contains(component.controllable_component_id.value());
    if (!has_model && component.root_pose_in_cell.has_value()) {
      fail("controllable_components.root_pose_in_cell",
           "component '" + component.controllable_component_id.value() +
               "' has no model binding and must omit root pose");
    }
    if (has_model && is_child && component.root_pose_in_cell.has_value()) {
      fail("controllable_components.root_pose_in_cell",
           "attached child component '" + component.controllable_component_id.value() +
               "' must omit root pose");
    }
    if (has_model && !is_child && !component.root_pose_in_cell.has_value()) {
      fail("controllable_components.root_pose_in_cell",
           "unattached spatial component '" + component.controllable_component_id.value() +
               "' requires a root pose");
    }
  }
}

}  // namespace

TypedCellGraphCandidate decode_cell_graph_candidate(const ParsedArtifactCandidate& parsed) {
  const CanonicalValue& root = parsed.root();
  const auto& object = require_object(root, "$");
  require_exact_fields(
      object,
      {"api_version", "graph_revision", "cell_id", "runtime_units", "model_instances",
       "controllable_components", "control_domains", "control_bindings", "component_attachments"},
      "$");

  const std::string& api_version =
      require_string(require_member(object, "api_version", "$"), "$.api_version");
  if (api_version != core::kCellGraphApiVersion) {
    fail("$.api_version", "expected manyforge.cell_graph.v1");
  }

  CellGraphDTO graph{
      api_version,
      parse_digest<GraphRevision>(require_member(object, "graph_revision", "$"),
                                  "$.graph_revision"),
      parse_identity<CellId>(require_member(object, "cell_id", "$"), "$.cell_id"),
      parse_record_table<RuntimeUnitDTO>(require_member(object, "runtime_units", "$"),
                                         "$.runtime_units", parse_runtime_unit,
                                         [](const RuntimeUnitDTO& record) -> const RuntimeUnitId& {
                                           return record.runtime_unit_id;
                                         }),
      parse_record_table<ModelInstanceDTO>(
          require_member(object, "model_instances", "$"), "$.model_instances", parse_model_instance,
          [](const ModelInstanceDTO& record) -> const ModelInstanceId& {
            return record.model_instance_id;
          }),
      parse_record_table<ControllableComponentDTO>(
          require_member(object, "controllable_components", "$"), "$.controllable_components",
          parse_controllable_component,
          [](const ControllableComponentDTO& record) -> const ControllableComponentId& {
            return record.controllable_component_id;
          }),
      parse_record_table<ControlDomainDTO>(
          require_member(object, "control_domains", "$"), "$.control_domains", parse_control_domain,
          [](const ControlDomainDTO& record) -> const ControlDomainId& {
            return record.control_domain_id;
          }),
      parse_record_table<ControlBindingDTO>(
          require_member(object, "control_bindings", "$"), "$.control_bindings",
          parse_control_binding,
          [](const ControlBindingDTO& record) -> const ControlBindingId& {
            return record.control_binding_id;
          }),
      parse_record_table<ComponentAttachmentEdgeDTO>(
          require_member(object, "component_attachments", "$"), "$.component_attachments",
          parse_component_attachment,
          [](const ComponentAttachmentEdgeDTO& record) -> const ComponentAttachmentId& {
            return record.component_attachment_id;
          })};

  CanonicalValue structurally_normalized_root = graph_value(graph);
  return TypedCellGraphCandidate(std::move(graph), std::move(structurally_normalized_root));
}

IntrinsicallyValidatedCellGraphCandidate validate_intrinsic_cell_graph_candidate(
    const TypedCellGraphCandidate& candidate) {
  validate_intrinsic(candidate.graph());
  return IntrinsicallyValidatedCellGraphCandidate(candidate);
}

GraphIdentityReadyCellGraphCandidate prepare_cell_graph_identity(
    const IntrinsicallyValidatedCellGraphCandidate& candidate,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles) {
  const auto& source_graph = candidate.graph();
  std::set<RuntimeProfileKey> selected_profiles;
  for (const auto& unit : source_graph.runtime_units) {
    selected_profiles.insert(runtime_profile_key(unit));
  }

  std::map<RuntimeProfileKey, const QualifiedRuntimeProfile*> profile_index;
  for (const auto& profile : qualified_runtime_profiles) {
    const RuntimeProfileKey key = runtime_profile_key(profile.value());
    if (!selected_profiles.contains(key)) {
      const bool selected_id = std::any_of(
          selected_profiles.begin(), selected_profiles.end(),
          [&key](const RuntimeProfileKey& selected) {
            return selected.first == key.first;
          });
      fail("qualified_runtime_profiles",
           (selected_id ? "stale runtime-profile revision "
                        : "unused extra runtime profile ") +
               profile_key_label(key));
    }
    if (!profile_index.emplace(key, &profile).second) {
      fail("qualified_runtime_profiles",
           "duplicate exact runtime profile " + profile_key_label(key));
    }
  }

  CellGraphDTO graph = source_graph;
  const auto materialized_io_devices =
      MaterializeRetainedIoProviderDevices(graph);
  if (!materialized_io_devices.ok())
    fail("$.runtime_units",
         materialized_io_devices.error().details.at("reason"));
  const auto& schema_registry = build_qualified_closed_schema_registry();
  detail::CumulativeNormalizedValueBudget normalized_configuration_budget;
  for (std::size_t index = 0U; index < graph.runtime_units.size(); ++index) {
    auto& unit = graph.runtime_units[index];
    const RuntimeProfileKey key = runtime_profile_key(unit);
    const auto found = profile_index.find(key);
    if (found == profile_index.end()) {
      fail("qualified_runtime_profiles",
           "missing runtime profile " + profile_key_label(key) +
               " for runtime unit '" + unit.runtime_unit_id.value() + "'");
    }
    const auto& schema_ref =
        found->second->value().configuration_schema_ref;
    const auto* schema_entry =
        schema_registry.find_qualified_value_schema(
            schema_ref.schema_id, schema_ref.schema_revision);
    if (schema_entry == nullptr) {
      fail("qualified_runtime_profiles",
           "runtime profile " + profile_key_label(key) +
               " selects a value schema not qualified by this build");
    }

    const std::string configuration_path =
        "$.runtime_units[" + std::to_string(index) + "].configuration";
    try {
      const auto normalized =
          validate_and_normalize_build_qualified_value(
              *schema_entry,
              json_object_to_canonical(unit.configuration));
      detail::validate_canonical_value_output_budget_cumulatively(
          normalized.normalized_value(), normalized_configuration_budget);
      unit.configuration =
          to_json_object(normalized.normalized_value(), configuration_path);
      if (unit.runtime_profile_id.value() ==
          detail::kRos2ControlExecutionRuntimeProfileId) {
        static_cast<void>(detail::require_ros2_control_execution_runtime_required_rows(
            graph, unit));
      }
    } catch (const RetainedBundleError& error) {
      fail(configuration_path, error.what());
    } catch (const ClosedSchemaError& error) {
      fail(configuration_path, error.what());
    } catch (const CanonicalValueError& error) {
      fail(configuration_path, error.what());
    }
  }

  const auto io_catalog = ResolveRetainedIoSignalCatalog(graph);
  if (!io_catalog.ok())
    fail("$.runtime_units", io_catalog.error().details.at("reason"));
  const auto io_devices = ResolveRetainedIoProviderDevices(graph);
  if (!io_devices.ok())
    fail("$.runtime_units", io_devices.error().details.at("reason"));

  CanonicalValue normalized_root = graph_value(graph);
  try {
    detail::validate_canonical_value_output_budget(normalized_root);
  } catch (const ClosedSchemaError& error) {
    fail("$", error.what());
  }
  return GraphIdentityReadyCellGraphCandidate(
      std::move(graph), std::move(normalized_root));
}

GraphRevisionVerifiedCellGraph compute_graph_revision(
    const GraphIdentityReadyCellGraphCandidate& candidate) {
  CellGraphDTO graph = candidate.graph();
  compute_nested_graph_revisions(graph);

  CanonicalValue normalized_root = graph_value(graph);
  graph.graph_revision = GraphRevision(
      detail::compute_content_identity(normalized_root,
                                       graph_identity_spec()));
  normalized_root = graph_value(graph);
  return GraphRevisionVerifiedCellGraph(
      GraphIdentityReadyCellGraphCandidate(
          std::move(graph), std::move(normalized_root)));
}

GraphRevisionVerifiedCellGraph verify_graph_revision(
    const GraphIdentityReadyCellGraphCandidate& candidate) {
  verify_nested_graph_revisions(candidate.graph());
  if (!detail::verify_content_identity(candidate.normalized_root(),
                                       graph_identity_spec())) {
    fail("$.graph_revision",
         "graph_revision does not match normalized graph content");
  }
  return GraphRevisionVerifiedCellGraph(candidate);
}

}  // namespace manyforge::deployment
