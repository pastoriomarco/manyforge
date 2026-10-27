#include "manyforge_deployment/deployment_applicability_codec.hpp"
#include "manyforge_deployment/scene_pose_source_policy_codec.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <manyforge_core/types/interrupt.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/program_contract.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "compiler_semantic_limits_internal.hpp"
#include "content_identity_internal.hpp"
#include "deployment_catalog_codec_internal.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"

namespace manyforge::deployment {
namespace {

using core::ActivationSetId;
using core::CapabilityDefinitionRevision;
using core::CapabilityEndpointSourceKind;
using core::CapabilityId;
using core::CapabilityProviderBindingDTO;
using core::CapabilityProviderBindingId;
using core::CapabilityProviderBindingRevision;
using core::CapabilityProviderEndpointBindingDTO;
using core::CapabilityProviderTargetDTO;
using core::CapabilityScope;
using core::CellGlobalCapabilityProviderTargetDTO;
using core::CellId;
using core::CommandTargetId;
using core::CommissionedStopInterfaceDTO;
using core::CommissionedStopInterfaceId;
using core::CommissionedStopInterfaceRevision;
using core::ComponentTypeRevision;
using core::ControlBindingId;
using core::ControlBindingRevision;
using core::ControlDomainCapabilityProviderTargetDTO;
using core::ControlDomainId;
using core::ControlDomainRevision;
using core::ControllableComponentId;
using core::ControllerTopologyId;
using core::ControlRouteId;
using core::DeploymentApplicabilityInputsDTO;
using core::DesiredControlBindingRouteDTO;
using core::DesiredControlConfigurationDTO;
using core::EndpointRoleId;
using core::ExternalResourceAlias;
using core::ExternalResourceDTO;
using core::ExternalResourceKind;
using core::ProviderContractRefDTO;
using core::ProviderInstanceId;
using core::ProviderManifestId;
using core::ProviderManifestRevision;
using core::RuntimeUnitId;
using core::SiteOverlayId;
using core::SiteOverlayRefDTO;
using core::SiteOverlayRevision;
using core::SiteOverlayValueRef;
using core::StopStageParametersDTO;
using core::TargetBindingCapabilityProviderTargetDTO;
using core::TopologyRevision;

constexpr std::string_view kDeploymentApiVersion = "manyforge.deployment.v1";
const detail::ContentIdentitySpec kCapabilityProviderBindingIdentitySpec{
    "capability-provider-binding-revision",
    std::string(kDeploymentApiVersion),
    "capability_provider_binding_revision",
    detail::ContentIdentityRootShape::kNestedRecord,
};
const detail::ContentIdentitySpec kCommissionedStopInterfaceIdentitySpec{
    "commissioned-stop-interface-revision",
    std::string(kDeploymentApiVersion),
    "commissioned_stop_interface_revision",
    detail::ContentIdentityRootShape::kNestedRecord,
};

[[noreturn]] void fail(std::string_view path, std::string_view reason) {
  throw DeploymentApplicabilityCodecError(std::string(path) + ": " +
                                          std::string(reason));
}

[[nodiscard]] std::string child_path(std::string_view parent,
                                     std::string_view child) {
  return std::string(parent) + "." + std::string(child);
}

[[nodiscard]] std::string index_path(std::string_view parent,
                                     std::size_t index) {
  return std::string(parent) + "[" + std::to_string(index) + "]";
}

[[nodiscard]] const CanonicalValue::Object &
require_object(const CanonicalValue &value, std::string_view path) {
  const auto *object = std::get_if<CanonicalValue::Object>(&value.storage());
  if (object == nullptr) {
    fail(path, "expected object");
  }
  return *object;
}

[[nodiscard]] const CanonicalValue::Array &
require_array(const CanonicalValue &value, std::string_view path) {
  const auto *array = std::get_if<CanonicalValue::Array>(&value.storage());
  if (array == nullptr) {
    fail(path, "expected array");
  }
  return *array;
}

[[nodiscard]] const std::string &require_string(const CanonicalValue &value,
                                                std::string_view path) {
  const auto *text = std::get_if<std::string>(&value.storage());
  if (text == nullptr) {
    fail(path, "expected string");
  }
  return *text;
}

[[nodiscard]] bool require_bool(const CanonicalValue &value,
                                std::string_view path) {
  const auto *boolean = std::get_if<bool>(&value.storage());
  if (boolean == nullptr) {
    fail(path, "expected boolean");
  }
  return *boolean;
}

[[nodiscard]] const CanonicalValue &
require_member(const CanonicalValue::Object &object, std::string_view name,
               std::string_view path) {
  const auto iterator =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  if (iterator == object.end()) {
    fail(path, "missing required field '" + std::string(name) + "'");
  }
  return iterator->second;
}

[[nodiscard]] const CanonicalValue *
find_member(const CanonicalValue::Object &object,
            std::string_view name) noexcept {
  const auto iterator =
      std::find_if(object.begin(), object.end(),
                   [name](const auto &member) { return member.first == name; });
  return iterator == object.end() ? nullptr : &iterator->second;
}

void require_exact_fields(const CanonicalValue::Object &object,
                          std::initializer_list<std::string_view> expected,
                          std::string_view path) {
  for (const auto &[name, unused] : object) {
    (void)unused;
    if (std::find(expected.begin(), expected.end(), name) == expected.end()) {
      fail(path, "unknown field '" + name + "'");
    }
  }
  for (const std::string_view name : expected) {
    (void)require_member(object, name, path);
  }
}

void require_closed_fields(const CanonicalValue::Object &object,
                           std::initializer_list<std::string_view> required,
                           std::initializer_list<std::string_view> optional,
                           std::string_view path) {
  for (const auto &[name, unused] : object) {
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

[[nodiscard]] std::uint64_t
require_bounded_unsigned_integer(const CanonicalValue &value,
                                 std::string_view path) {
  const auto *number = std::get_if<double>(&value.storage());
  constexpr double kMaximumIJsonInteger = 9007199254740991.0;
  if (number == nullptr || !std::isfinite(*number) || *number < 0.0 ||
      std::trunc(*number) != *number || *number > kMaximumIJsonInteger) {
    fail(path, "expected a non-negative I-JSON-safe integer");
  }
  return static_cast<std::uint64_t>(*number);
}

[[nodiscard]] std::uint64_t
require_integer_in_range(const CanonicalValue &value, std::uint64_t minimum,
                         std::uint64_t maximum, std::string_view path) {
  const std::uint64_t parsed = require_bounded_unsigned_integer(value, path);
  if (parsed < minimum || parsed > maximum) {
    fail(path, "integer is outside the supported closed range");
  }
  return parsed;
}

[[nodiscard]] double require_finite_number(const CanonicalValue &value,
                                           std::string_view path) {
  const auto *number = std::get_if<double>(&value.storage());
  if (number == nullptr || !std::isfinite(*number)) {
    fail(path, "expected finite number");
  }
  return *number;
}

[[nodiscard]] constexpr bool
is_ascii_alphanumeric(unsigned char byte) noexcept {
  return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
         (byte >= '0' && byte <= '9');
}

[[nodiscard]] bool is_portable_token(std::string_view value) noexcept {
  if (value.empty() || value.size() > detail::kMaximumPortableIdentityBytes ||
      !is_ascii_alphanumeric(static_cast<unsigned char>(value.front()))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](unsigned char byte) {
    return is_ascii_alphanumeric(byte) || byte == '.' || byte == '_' ||
           byte == '-';
  });
}

template <typename StrongType>
[[nodiscard]] StrongType parse_portable_id(const CanonicalValue &value,
                                           std::string_view path) {
  const std::string &text = require_string(value, path);
  if (!is_portable_token(text)) {
    fail(path,
         "expected a 1..128 byte portable token beginning with an ASCII "
         "alphanumeric and containing only alphanumerics, '.', '_', or '-'");
  }
  return StrongType(text);
}

template <typename StrongType>
[[nodiscard]] StrongType parse_digest(const CanonicalValue &value,
                                      std::string_view path) {
  const std::string &text = require_string(value, path);
  if (!is_sha256_digest(text)) {
    fail(path, "expected lowercase sha256:<64 hex> digest");
  }
  return StrongType(text);
}

[[nodiscard]] CapabilityScope parse_scope(const CanonicalValue &value,
                                          std::string_view path) {
  const std::string &text = require_string(value, path);
  if (text == "cell_global") {
    return CapabilityScope::kCellGlobal;
  }
  if (text == "target_binding") {
    return CapabilityScope::kTargetBinding;
  }
  if (text == "control_domain") {
    return CapabilityScope::kControlDomain;
  }
  fail(path, "scope is outside the closed v1 enum");
}

[[nodiscard]] ExternalResourceKind
parse_external_resource_kind(const CanonicalValue &value,
                             std::string_view path) {
  const std::string &text = require_string(value, path);
  if (text == "model_endpoint") {
    return ExternalResourceKind::kModelEndpoint;
  }
  if (text == "content_store") {
    return ExternalResourceKind::kContentStore;
  }
  if (text == "data_source") {
    return ExternalResourceKind::kDataSource;
  }
  if (text == "service_endpoint") {
    return ExternalResourceKind::kServiceEndpoint;
  }
  fail(path, "resource kind is outside the closed v1 enum");
}

[[nodiscard]] DesiredControlBindingRouteDTO
parse_desired_route(const CanonicalValue &value, std::string_view path) {
  const auto &object = require_object(value, path);
  require_exact_fields(
      object,
      {"control_binding_id", "control_binding_revision", "control_route_id"},
      path);
  return {
      parse_portable_id<ControlBindingId>(
          require_member(object, "control_binding_id", path),
          child_path(path, "control_binding_id")),
      parse_digest<ControlBindingRevision>(
          require_member(object, "control_binding_revision", path),
          child_path(path, "control_binding_revision")),
      parse_portable_id<ControlRouteId>(
          require_member(object, "control_route_id", path),
          child_path(path, "control_route_id")),
  };
}

[[nodiscard]] auto
desired_route_key(const DesiredControlBindingRouteDTO &route) {
  return std::tie(route.control_binding_id, route.control_binding_revision,
                  route.control_route_id);
}

[[nodiscard]] DesiredControlConfigurationDTO
parse_desired_configuration(const CanonicalValue &value,
                            std::string_view path) {
  const auto &object = require_object(value, path);
  require_exact_fields(object,
                       {"control_domain_id", "control_domain_revision",
                        "controller_topology_id", "topology_revision",
                        "activation_set_id", "binding_routes"},
                       path);

  const std::string routes_path = child_path(path, "binding_routes");
  const auto &route_values = require_array(
      require_member(object, "binding_routes", path), routes_path);
  if (route_values.size() > core::kMaxDeploymentApplicabilityRows) {
    fail(routes_path,
         "binding-route count exceeds the 4096-row deployment ceiling");
  }
  std::vector<DesiredControlBindingRouteDTO> routes;
  routes.reserve(route_values.size());
  for (std::size_t index = 0; index < route_values.size(); ++index) {
    routes.push_back(parse_desired_route(route_values[index],
                                         index_path(routes_path, index)));
  }
  std::sort(routes.begin(), routes.end(),
            [](const auto &left, const auto &right) {
              return desired_route_key(left) < desired_route_key(right);
            });
  if (std::adjacent_find(routes.begin(), routes.end(),
                         [](const auto &left, const auto &right) {
                           return desired_route_key(left) ==
                                  desired_route_key(right);
                         }) != routes.end()) {
    fail(routes_path, "duplicate complete binding-route identity");
  }

  return {
      parse_portable_id<ControlDomainId>(
          require_member(object, "control_domain_id", path),
          child_path(path, "control_domain_id")),
      parse_digest<ControlDomainRevision>(
          require_member(object, "control_domain_revision", path),
          child_path(path, "control_domain_revision")),
      parse_portable_id<ControllerTopologyId>(
          require_member(object, "controller_topology_id", path),
          child_path(path, "controller_topology_id")),
      parse_digest<TopologyRevision>(
          require_member(object, "topology_revision", path),
          child_path(path, "topology_revision")),
      parse_portable_id<ActivationSetId>(
          require_member(object, "activation_set_id", path),
          child_path(path, "activation_set_id")),
      std::move(routes),
  };
}

[[nodiscard]] CapabilityProviderTargetDTO
parse_provider_target(CapabilityScope scope, const CanonicalValue &value,
                      std::string_view path) {
  const auto &object = require_object(value, path);
  switch (scope) {
  case CapabilityScope::kCellGlobal:
    require_exact_fields(object, {"cell_id"}, path);
    return CellGlobalCapabilityProviderTargetDTO{parse_portable_id<CellId>(
        require_member(object, "cell_id", path), child_path(path, "cell_id"))};

  case CapabilityScope::kTargetBinding:
    if (object.size() == 3U) {
      require_exact_fields(object,
                           {"controllable_component_id",
                            "component_type_revision", "command_target_id"},
                           path);
      return core::ComponentTargetCapabilityProviderTargetDTO{
          parse_portable_id<ControllableComponentId>(
              require_member(object, "controllable_component_id", path),
              child_path(path, "controllable_component_id")),
          parse_digest<ComponentTypeRevision>(
              require_member(object, "component_type_revision", path),
              child_path(path, "component_type_revision")),
          parse_portable_id<CommandTargetId>(
              require_member(object, "command_target_id", path),
              child_path(path, "command_target_id"))};
    }
    require_exact_fields(object,
                         {"controllable_component_id",
                          "component_type_revision", "command_target_id",
                          "control_binding_id", "control_binding_revision"},
                         path);
    return TargetBindingCapabilityProviderTargetDTO{
        parse_portable_id<ControllableComponentId>(
            require_member(object, "controllable_component_id", path),
            child_path(path, "controllable_component_id")),
        parse_digest<ComponentTypeRevision>(
            require_member(object, "component_type_revision", path),
            child_path(path, "component_type_revision")),
        parse_portable_id<CommandTargetId>(
            require_member(object, "command_target_id", path),
            child_path(path, "command_target_id")),
        parse_portable_id<ControlBindingId>(
            require_member(object, "control_binding_id", path),
            child_path(path, "control_binding_id")),
        parse_digest<ControlBindingRevision>(
            require_member(object, "control_binding_revision", path),
            child_path(path, "control_binding_revision")),
    };

  case CapabilityScope::kControlDomain:
    if (const auto *domain_kind = find_member(object, "domain_kind")) {
      require_exact_fields(
          object,
          {"control_domain_id", "control_domain_revision", "domain_kind"},
          path);
      if (require_string(*domain_kind, path) != "provider")
        fail(path, "unknown control-domain target kind");
      return ControlDomainCapabilityProviderTargetDTO{
          parse_portable_id<ControlDomainId>(
              require_member(object, "control_domain_id", path), path),
          parse_digest<ControlDomainRevision>(
              require_member(object, "control_domain_revision", path), path),
          core::ProviderControlDomainCapabilityTargetDTO{}};
    }
    require_exact_fields(object,
                         {"control_domain_id", "control_domain_revision",
                          "controller_topology_id", "topology_revision"},
                         path);
    return ControlDomainCapabilityProviderTargetDTO{
        parse_portable_id<ControlDomainId>(
            require_member(object, "control_domain_id", path),
            child_path(path, "control_domain_id")),
        parse_digest<ControlDomainRevision>(
            require_member(object, "control_domain_revision", path),
            child_path(path, "control_domain_revision")),
        parse_portable_id<ControllerTopologyId>(
            require_member(object, "controller_topology_id", path),
            child_path(path, "controller_topology_id")),
        parse_digest<TopologyRevision>(
            require_member(object, "topology_revision", path),
            child_path(path, "topology_revision")),
    };
  }
  fail(path, "scope has no target alternative");
}

[[nodiscard]] CapabilityProviderEndpointBindingDTO
parse_endpoint_binding(const CanonicalValue &value, std::string_view path) {
  const auto &object = require_object(value, path);
  require_exact_fields(object,
                       {"endpoint_role_id", "source_kind", "source_id",
                        "source_endpoint_role_id"},
                       path);
  const std::string source_kind_path = child_path(path, "source_kind");
  const std::string &source_kind = require_string(
      require_member(object, "source_kind", path), source_kind_path);

  const EndpointRoleId endpoint_role_id = parse_portable_id<EndpointRoleId>(
      require_member(object, "endpoint_role_id", path),
      child_path(path, "endpoint_role_id"));
  const EndpointRoleId source_endpoint_role_id =
      parse_portable_id<EndpointRoleId>(
          require_member(object, "source_endpoint_role_id", path),
          child_path(path, "source_endpoint_role_id"));
  if (source_kind == "runtime_unit") {
    return {
        endpoint_role_id,
        CapabilityEndpointSourceKind::kRuntimeUnit,
        parse_portable_id<RuntimeUnitId>(
            require_member(object, "source_id", path),
            child_path(path, "source_id")),
        source_endpoint_role_id,
    };
  }
  if (source_kind == "external_resource") {
    return {
        endpoint_role_id,
        CapabilityEndpointSourceKind::kExternalResource,
        parse_portable_id<ExternalResourceAlias>(
            require_member(object, "source_id", path),
            child_path(path, "source_id")),
        source_endpoint_role_id,
    };
  }
  fail(source_kind_path, "source kind is outside the closed v1 enum");
}

[[nodiscard]] CapabilityProviderBindingDTO
parse_provider_binding(const CanonicalValue &value, std::string_view path) {
  const auto &object = require_object(value, path);
  require_exact_fields(object,
                       {"capability_provider_binding_id",
                        "capability_provider_binding_revision", "capability_id",
                        "capability_definition_revision", "scope", "target",
                        "provider_instance_id", "provider_manifest_id",
                        "provider_manifest_revision", "endpoint_bindings"},
                       path);

  const CapabilityScope scope = parse_scope(
      require_member(object, "scope", path), child_path(path, "scope"));
  const std::string endpoints_path = child_path(path, "endpoint_bindings");
  const auto &endpoint_values = require_array(
      require_member(object, "endpoint_bindings", path), endpoints_path);
  if (endpoint_values.size() > core::kMaxCapabilityProviderEndpointBindings) {
    fail(endpoints_path,
         "endpoint binding count exceeds the provider-manifest endpoint-role "
         "ceiling");
  }
  std::vector<CapabilityProviderEndpointBindingDTO> endpoints;
  endpoints.reserve(endpoint_values.size());
  for (std::size_t index = 0; index < endpoint_values.size(); ++index) {
    endpoints.push_back(parse_endpoint_binding(
        endpoint_values[index], index_path(endpoints_path, index)));
  }
  std::sort(endpoints.begin(), endpoints.end(),
            [](const auto &left, const auto &right) {
              return left.endpoint_role_id < right.endpoint_role_id;
            });
  if (std::adjacent_find(endpoints.begin(), endpoints.end(),
                         [](const auto &left, const auto &right) {
                           return left.endpoint_role_id ==
                                  right.endpoint_role_id;
                         }) != endpoints.end()) {
    fail(endpoints_path, "duplicate endpoint_role_id");
  }

  return {
      parse_portable_id<CapabilityProviderBindingId>(
          require_member(object, "capability_provider_binding_id", path),
          child_path(path, "capability_provider_binding_id")),
      parse_digest<CapabilityProviderBindingRevision>(
          require_member(object, "capability_provider_binding_revision", path),
          child_path(path, "capability_provider_binding_revision")),
      parse_portable_id<CapabilityId>(
          require_member(object, "capability_id", path),
          child_path(path, "capability_id")),
      parse_digest<CapabilityDefinitionRevision>(
          require_member(object, "capability_definition_revision", path),
          child_path(path, "capability_definition_revision")),
      scope,
      parse_provider_target(scope, require_member(object, "target", path),
                            child_path(path, "target")),
      parse_portable_id<ProviderInstanceId>(
          require_member(object, "provider_instance_id", path),
          child_path(path, "provider_instance_id")),
      parse_portable_id<ProviderManifestId>(
          require_member(object, "provider_manifest_id", path),
          child_path(path, "provider_manifest_id")),
      parse_digest<ProviderManifestRevision>(
          require_member(object, "provider_manifest_revision", path),
          child_path(path, "provider_manifest_revision")),
      std::move(endpoints),
  };
}

[[nodiscard]] ExternalResourceDTO
parse_external_resource(const CanonicalValue &value, std::string_view path) {
  const auto &object = require_object(value, path);
  require_exact_fields(object,
                       {"external_resource_alias", "resource_kind",
                        "provider_instance_id", "provider_manifest_id",
                        "provider_manifest_revision", "endpoint_role_id",
                        "site_overlay_value_ref"},
                       path);
  return {
      parse_portable_id<ExternalResourceAlias>(
          require_member(object, "external_resource_alias", path),
          child_path(path, "external_resource_alias")),
      parse_external_resource_kind(
          require_member(object, "resource_kind", path),
          child_path(path, "resource_kind")),
      parse_portable_id<ProviderInstanceId>(
          require_member(object, "provider_instance_id", path),
          child_path(path, "provider_instance_id")),
      parse_portable_id<ProviderManifestId>(
          require_member(object, "provider_manifest_id", path),
          child_path(path, "provider_manifest_id")),
      parse_digest<ProviderManifestRevision>(
          require_member(object, "provider_manifest_revision", path),
          child_path(path, "provider_manifest_revision")),
      parse_portable_id<EndpointRoleId>(
          require_member(object, "endpoint_role_id", path),
          child_path(path, "endpoint_role_id")),
      parse_portable_id<SiteOverlayValueRef>(
          require_member(object, "site_overlay_value_ref", path),
          child_path(path, "site_overlay_value_ref")),
  };
}

[[nodiscard]] ProviderContractRefDTO
parse_provider_contract_ref(const CanonicalValue &value,
                            std::string_view path) {
  const auto &object = require_object(value, path);
  require_exact_fields(object,
                       {"provider_contract_role_id", "contract_schema_id",
                        "contract_schema_revision"},
                       path);
  return {
      parse_portable_id<core::ProviderContractRoleId>(
          require_member(object, "provider_contract_role_id", path),
          child_path(path, "provider_contract_role_id")),
      parse_portable_id<core::SchemaId>(
          require_member(object, "contract_schema_id", path),
          child_path(path, "contract_schema_id")),
      parse_digest<core::SchemaRevision>(
          require_member(object, "contract_schema_revision", path),
          child_path(path, "contract_schema_revision")),
  };
}

[[nodiscard]] core::JsonValueDTO
canonical_to_json_value(const CanonicalValue &value) {
  if (std::holds_alternative<std::nullptr_t>(value.storage())) {
    return {};
  }
  if (const auto *boolean = std::get_if<bool>(&value.storage())) {
    return core::JsonValueDTO{*boolean};
  }
  if (const auto *number = std::get_if<double>(&value.storage())) {
    return core::JsonValueDTO{*number};
  }
  if (const auto *text = std::get_if<std::string>(&value.storage())) {
    return core::JsonValueDTO{*text};
  }
  if (const auto *array =
          std::get_if<CanonicalValue::Array>(&value.storage())) {
    core::JsonValueDTO::Array result;
    result.reserve(array->size());
    for (const auto &item : *array) {
      result.push_back(canonical_to_json_value(item));
    }
    return core::JsonValueDTO{std::move(result)};
  }
  const auto &object = std::get<CanonicalValue::Object>(value.storage());
  core::JsonObjectDTO result;
  for (const auto &[name, member] : object) {
    result.emplace(name, canonical_to_json_value(member));
  }
  return core::JsonValueDTO{std::move(result)};
}

[[nodiscard]] CanonicalValue
json_value_to_canonical(const core::JsonValueDTO &value) {
  return std::visit(
      [](const auto &selected) -> CanonicalValue {
        using Selected = std::decay_t<decltype(selected)>;
        if constexpr (std::is_same_v<Selected, std::nullptr_t> ||
                      std::is_same_v<Selected, bool> ||
                      std::is_same_v<Selected, double> ||
                      std::is_same_v<Selected, std::string>) {
          return CanonicalValue(selected);
        } else if constexpr (std::is_same_v<Selected,
                                            core::JsonValueDTO::Array>) {
          CanonicalValue::Array array;
          array.reserve(selected.size());
          for (const auto &item : selected) {
            array.push_back(json_value_to_canonical(item));
          }
          return CanonicalValue(std::move(array));
        } else {
          CanonicalValue::Object object;
          object.reserve(selected.size());
          for (const auto &[name, member] : selected) {
            object.emplace_back(name, json_value_to_canonical(member));
          }
          return CanonicalValue(std::move(object));
        }
      },
      value.value);
}

[[nodiscard]] CanonicalValue
json_object_to_canonical(const core::JsonObjectDTO &object) {
  return json_value_to_canonical(core::JsonValueDTO{object});
}

[[nodiscard]] core::RetainedArtifactRefDTO
parse_retained_artifact_ref(const CanonicalValue &value,
                            std::string_view path) {
  const auto &object = require_object(value, path);
  require_exact_fields(object,
                       {"artifact_role_id", "media_type", "byte_size",
                        "content_digest", "content_addressed_store_key"},
                       path);
  const auto &media_type =
      require_string(require_member(object, "media_type", path),
                     child_path(path, "media_type"));
  const auto &store_key = require_string(
      require_member(object, "content_addressed_store_key", path),
      child_path(path, "content_addressed_store_key"));
  if (media_type.empty() || media_type.size() > kMaxArtifactScalarBytes ||
      store_key.empty() || store_key.size() > kMaxArtifactScalarBytes) {
    fail(path,
         "artifact media type and store key must be non-empty and bounded");
  }
  return {
      parse_portable_id<core::ArtifactRoleId>(
          require_member(object, "artifact_role_id", path),
          child_path(path, "artifact_role_id")),
      media_type,
      require_bounded_unsigned_integer(
          require_member(object, "byte_size", path),
          child_path(path, "byte_size")),
      parse_digest<core::ContentDigest>(
          require_member(object, "content_digest", path),
          child_path(path, "content_digest")),
      core::ContentAddressedStoreKey(store_key),
  };
}

[[nodiscard]] core::StopActionClass
parse_stop_action_class(const CanonicalValue &value, std::string_view path) {
  const auto &token = require_string(value, path);
  if (token == "controlled") {
    return core::StopActionClass::kControlled;
  }
  if (token == "software_hard") {
    return core::StopActionClass::kSoftwareHard;
  }
  if (token == "safety_system_request") {
    return core::StopActionClass::kSafetySystemRequest;
  }
  fail(path, "stop action class is outside the closed v1 enum");
}

[[nodiscard]] core::SafetySystemRequestActionClass
parse_safety_system_request_action_class(const CanonicalValue &value,
                                         std::string_view path) {
  const auto &token = require_string(value, path);
  if (token == "protective_stop_request") {
    return core::SafetySystemRequestActionClass::kProtectiveStopRequest;
  }
  if (token == "emergency_stop_request") {
    return core::SafetySystemRequestActionClass::kEmergencyStopRequest;
  }
  fail(path,
       "safety-system request action class is outside the closed v1 enum");
}

[[nodiscard]] StopStageParametersDTO
parse_stop_stage_parameters(const CanonicalValue &value,
                            std::string_view path) {
  const auto &object = require_object(value, path);
  require_exact_fields(
      object,
      {"control_domain_id", "control_domain_revision", "control_binding_id",
       "control_binding_revision", "controller_topology_id",
       "topology_revision", "control_route_id", "stop_evidence_policy_id",
       "stop_evidence_policy_revision", "stop_escalation_stage_id",
       "provider_contract_ref", "default_contract_parameters"},
      path);
  const ProviderContractRefDTO contract = parse_provider_contract_ref(
      require_member(object, "provider_contract_ref", path),
      child_path(path, "provider_contract_ref"));
  const auto *schema =
      build_qualified_closed_schema_registry().find_qualified_value_schema(
          contract.contract_schema_id, contract.contract_schema_revision);
  if (schema == nullptr) {
    fail(child_path(path, "provider_contract_ref"),
         "does not select one exact build-qualified value schema");
  }
  const auto normalized = validate_and_normalize_build_qualified_value(
      *schema, require_member(object, "default_contract_parameters", path));
  const auto *normalized_object = std::get_if<CanonicalValue::Object>(
      &normalized.normalized_value().storage());
  if (normalized_object == nullptr) {
    fail(child_path(path, "default_contract_parameters"),
         "provider parameter root is not a closed object alternative");
  }
  return {
      parse_portable_id<ControlDomainId>(
          require_member(object, "control_domain_id", path),
          child_path(path, "control_domain_id")),
      parse_digest<ControlDomainRevision>(
          require_member(object, "control_domain_revision", path),
          child_path(path, "control_domain_revision")),
      parse_portable_id<ControlBindingId>(
          require_member(object, "control_binding_id", path),
          child_path(path, "control_binding_id")),
      parse_digest<ControlBindingRevision>(
          require_member(object, "control_binding_revision", path),
          child_path(path, "control_binding_revision")),
      parse_portable_id<ControllerTopologyId>(
          require_member(object, "controller_topology_id", path),
          child_path(path, "controller_topology_id")),
      parse_digest<TopologyRevision>(
          require_member(object, "topology_revision", path),
          child_path(path, "topology_revision")),
      parse_portable_id<ControlRouteId>(
          require_member(object, "control_route_id", path),
          child_path(path, "control_route_id")),
      parse_portable_id<core::StopEvidencePolicyId>(
          require_member(object, "stop_evidence_policy_id", path),
          child_path(path, "stop_evidence_policy_id")),
      parse_digest<core::StopEvidencePolicyRevision>(
          require_member(object, "stop_evidence_policy_revision", path),
          child_path(path, "stop_evidence_policy_revision")),
      parse_portable_id<core::StopEscalationStageId>(
          require_member(object, "stop_escalation_stage_id", path),
          child_path(path, "stop_escalation_stage_id")),
      contract,
      std::get<core::JsonObjectDTO>(
          canonical_to_json_value(normalized.normalized_value()).value),
  };
}

[[nodiscard]] CommissionedStopInterfaceDTO
parse_commissioned_stop_interface(const CanonicalValue &value,
                                  std::string_view path) {
  const auto &object = require_object(value, path);
  require_closed_fields(
      object,
      {"commissioned_stop_interface_id", "commissioned_stop_interface_revision",
       "control_domain_id", "control_domain_revision", "control_binding_id",
       "control_binding_revision", "control_route_id", "provider_instance_id",
       "provider_manifest_id", "provider_manifest_revision",
       "registered_command_kind_id", "provider_contract_ref",
       "endpoint_role_id", "stop_action_class",
       "safety_system_request_action_class",
       "commissioning_evidence_artifact_ref"},
      {"external_resource_alias", "external_resource_kind",
       "site_overlay_value_ref"},
      path);
  const bool has_alias =
      find_member(object, "external_resource_alias") != nullptr;
  const bool has_kind =
      find_member(object, "external_resource_kind") != nullptr;
  const bool has_value =
      find_member(object, "site_overlay_value_ref") != nullptr;
  if (has_alias != has_kind || has_alias != has_value) {
    fail(path, "external-resource commissioning fields must be jointly present "
               "or absent");
  }
  std::optional<ExternalResourceAlias> alias;
  std::optional<ExternalResourceKind> kind;
  std::optional<SiteOverlayValueRef> value_ref;
  if (has_alias) {
    alias = parse_portable_id<ExternalResourceAlias>(
        *find_member(object, "external_resource_alias"),
        child_path(path, "external_resource_alias"));
    kind = parse_external_resource_kind(
        *find_member(object, "external_resource_kind"),
        child_path(path, "external_resource_kind"));
    value_ref = parse_portable_id<SiteOverlayValueRef>(
        *find_member(object, "site_overlay_value_ref"),
        child_path(path, "site_overlay_value_ref"));
  }
  return {
      parse_portable_id<CommissionedStopInterfaceId>(
          require_member(object, "commissioned_stop_interface_id", path),
          child_path(path, "commissioned_stop_interface_id")),
      parse_digest<CommissionedStopInterfaceRevision>(
          require_member(object, "commissioned_stop_interface_revision", path),
          child_path(path, "commissioned_stop_interface_revision")),
      parse_portable_id<ControlDomainId>(
          require_member(object, "control_domain_id", path),
          child_path(path, "control_domain_id")),
      parse_digest<ControlDomainRevision>(
          require_member(object, "control_domain_revision", path),
          child_path(path, "control_domain_revision")),
      parse_portable_id<ControlBindingId>(
          require_member(object, "control_binding_id", path),
          child_path(path, "control_binding_id")),
      parse_digest<ControlBindingRevision>(
          require_member(object, "control_binding_revision", path),
          child_path(path, "control_binding_revision")),
      parse_portable_id<ControlRouteId>(
          require_member(object, "control_route_id", path),
          child_path(path, "control_route_id")),
      parse_portable_id<ProviderInstanceId>(
          require_member(object, "provider_instance_id", path),
          child_path(path, "provider_instance_id")),
      parse_portable_id<ProviderManifestId>(
          require_member(object, "provider_manifest_id", path),
          child_path(path, "provider_manifest_id")),
      parse_digest<ProviderManifestRevision>(
          require_member(object, "provider_manifest_revision", path),
          child_path(path, "provider_manifest_revision")),
      parse_portable_id<core::RegisteredStopCommandKindId>(
          require_member(object, "registered_command_kind_id", path),
          child_path(path, "registered_command_kind_id")),
      parse_provider_contract_ref(
          require_member(object, "provider_contract_ref", path),
          child_path(path, "provider_contract_ref")),
      parse_portable_id<EndpointRoleId>(
          require_member(object, "endpoint_role_id", path),
          child_path(path, "endpoint_role_id")),
      parse_stop_action_class(require_member(object, "stop_action_class", path),
                              child_path(path, "stop_action_class")),
      parse_safety_system_request_action_class(
          require_member(object, "safety_system_request_action_class", path),
          child_path(path, "safety_system_request_action_class")),
      std::move(alias),
      std::move(kind),
      std::move(value_ref),
      parse_retained_artifact_ref(
          require_member(object, "commissioning_evidence_artifact_ref", path),
          child_path(path, "commissioning_evidence_artifact_ref")),
  };
}

[[nodiscard]] SiteOverlayRefDTO
parse_site_overlay_ref(const CanonicalValue &value, std::string_view path) {
  const auto &object = require_object(value, path);
  require_exact_fields(
      object, {"site_overlay_id", "site_overlay_revision", "non_secret_digest"},
      path);
  return {
      parse_portable_id<SiteOverlayId>(
          require_member(object, "site_overlay_id", path),
          child_path(path, "site_overlay_id")),
      parse_digest<SiteOverlayRevision>(
          require_member(object, "site_overlay_revision", path),
          child_path(path, "site_overlay_revision")),
      parse_digest<core::ContentDigest>(
          require_member(object, "non_secret_digest", path),
          child_path(path, "non_secret_digest")),
  };
}

[[nodiscard]] CanonicalValue
encode_desired_route(const DesiredControlBindingRouteDTO &route) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_binding_id", CanonicalValue(route.control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(route.control_binding_revision.value())},
      {"control_route_id", CanonicalValue(route.control_route_id.value())},
  });
}

[[nodiscard]] CanonicalValue encode_desired_configuration(
    const DesiredControlConfigurationDTO &configuration) {
  CanonicalValue::Array routes;
  routes.reserve(configuration.binding_routes.size());
  for (const auto &route : configuration.binding_routes) {
    routes.push_back(encode_desired_route(route));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"control_domain_id",
       CanonicalValue(configuration.control_domain_id.value())},
      {"control_domain_revision",
       CanonicalValue(configuration.control_domain_revision.value())},
      {"controller_topology_id",
       CanonicalValue(configuration.controller_topology_id.value())},
      {"topology_revision",
       CanonicalValue(configuration.topology_revision.value())},
      {"activation_set_id",
       CanonicalValue(configuration.activation_set_id.value())},
      {"binding_routes", CanonicalValue(std::move(routes))},
  });
}

[[nodiscard]] CanonicalValue
encode_provider_target(CapabilityScope scope,
                       const CapabilityProviderTargetDTO &target) {
  switch (scope) {
  case CapabilityScope::kCellGlobal: {
    const auto *value =
        std::get_if<CellGlobalCapabilityProviderTargetDTO>(&target);
    if (value == nullptr) {
      fail("capability_provider_bindings.target",
           "scope and target alternative disagree");
    }
    return CanonicalValue(CanonicalValue::Object{
        {"cell_id", CanonicalValue(value->cell_id.value())},
    });
  }
  case CapabilityScope::kTargetBinding: {
    if (const auto *direct =
            std::get_if<core::ComponentTargetCapabilityProviderTargetDTO>(
                &target)) {
      return CanonicalValue(CanonicalValue::Object{
          {"controllable_component_id",
           CanonicalValue(direct->controllable_component_id.value())},
          {"component_type_revision",
           CanonicalValue(direct->component_type_revision.value())},
          {"command_target_id",
           CanonicalValue(direct->command_target_id.value())}});
    }
    const auto *value =
        std::get_if<TargetBindingCapabilityProviderTargetDTO>(&target);
    if (value == nullptr) {
      fail("capability_provider_bindings.target",
           "scope and target alternative disagree");
    }
    return CanonicalValue(CanonicalValue::Object{
        {"controllable_component_id",
         CanonicalValue(value->controllable_component_id.value())},
        {"component_type_revision",
         CanonicalValue(value->component_type_revision.value())},
        {"command_target_id", CanonicalValue(value->command_target_id.value())},
        {"control_binding_id",
         CanonicalValue(value->control_binding_id.value())},
        {"control_binding_revision",
         CanonicalValue(value->control_binding_revision.value())},
    });
  }
  case CapabilityScope::kControlDomain: {
    const auto *value =
        std::get_if<ControlDomainCapabilityProviderTargetDTO>(&target);
    if (value == nullptr) {
      fail("capability_provider_bindings.target",
           "scope and target alternative disagree");
    }
    if (value->provider())
      return CanonicalValue(CanonicalValue::Object{
          {"control_domain_id",
           CanonicalValue(value->control_domain_id.value())},
          {"control_domain_revision",
           CanonicalValue(value->control_domain_revision.value())},
          {"domain_kind", CanonicalValue("provider")}});
    return CanonicalValue(CanonicalValue::Object{
        {"control_domain_id", CanonicalValue(value->control_domain_id.value())},
        {"control_domain_revision",
         CanonicalValue(value->control_domain_revision.value())},
        {"controller_topology_id",
         CanonicalValue(value->controller().controller_topology_id.value())},
        {"topology_revision",
         CanonicalValue(value->controller().topology_revision.value())},
    });
  }
  }
  fail("capability_provider_bindings.target",
       "scope has no target alternative");
}

[[nodiscard]] CanonicalValue
encode_endpoint_binding(const CapabilityProviderEndpointBindingDTO &endpoint) {
  std::string source_id;
  switch (endpoint.source_kind) {
  case CapabilityEndpointSourceKind::kRuntimeUnit: {
    const auto *value = std::get_if<RuntimeUnitId>(&endpoint.source_id);
    if (value == nullptr) {
      fail("capability_provider_bindings.endpoint_bindings.source_id",
           "runtime_unit source kind has a non-runtime-unit ID");
    }
    source_id = value->value();
    break;
  }
  case CapabilityEndpointSourceKind::kExternalResource: {
    const auto *value = std::get_if<ExternalResourceAlias>(&endpoint.source_id);
    if (value == nullptr) {
      fail("capability_provider_bindings.endpoint_bindings.source_id",
           "external_resource source kind has a non-alias ID");
    }
    source_id = value->value();
    break;
  }
  }
  return CanonicalValue(CanonicalValue::Object{
      {"endpoint_role_id", CanonicalValue(endpoint.endpoint_role_id.value())},
      {"source_kind",
       CanonicalValue(std::string(core::to_string(endpoint.source_kind)))},
      {"source_id", CanonicalValue(std::move(source_id))},
      {"source_endpoint_role_id",
       CanonicalValue(endpoint.source_endpoint_role_id.value())},
  });
}

[[nodiscard]] CanonicalValue
encode_provider_binding(const CapabilityProviderBindingDTO &binding) {
  CanonicalValue::Array endpoints;
  endpoints.reserve(binding.endpoint_bindings.size());
  for (const auto &endpoint : binding.endpoint_bindings) {
    endpoints.push_back(encode_endpoint_binding(endpoint));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"capability_provider_binding_id",
       CanonicalValue(binding.capability_provider_binding_id.value())},
      {"capability_provider_binding_revision",
       CanonicalValue(binding.capability_provider_binding_revision.value())},
      {"capability_id", CanonicalValue(binding.capability_id.value())},
      {"capability_definition_revision",
       CanonicalValue(binding.capability_definition_revision.value())},
      {"scope", CanonicalValue(std::string(core::to_string(binding.scope)))},
      {"target", encode_provider_target(binding.scope, binding.target)},
      {"provider_instance_id",
       CanonicalValue(binding.provider_instance_id.value())},
      {"provider_manifest_id",
       CanonicalValue(binding.provider_manifest_id.value())},
      {"provider_manifest_revision",
       CanonicalValue(binding.provider_manifest_revision.value())},
      {"endpoint_bindings", CanonicalValue(std::move(endpoints))},
  });
}

[[nodiscard]] CanonicalValue
encode_external_resource(const ExternalResourceDTO &resource) {
  return CanonicalValue(CanonicalValue::Object{
      {"external_resource_alias",
       CanonicalValue(resource.external_resource_alias.value())},
      {"resource_kind",
       CanonicalValue(std::string(core::to_string(resource.resource_kind)))},
      {"provider_instance_id",
       CanonicalValue(resource.provider_instance_id.value())},
      {"provider_manifest_id",
       CanonicalValue(resource.provider_manifest_id.value())},
      {"provider_manifest_revision",
       CanonicalValue(resource.provider_manifest_revision.value())},
      {"endpoint_role_id", CanonicalValue(resource.endpoint_role_id.value())},
      {"site_overlay_value_ref",
       CanonicalValue(resource.site_overlay_value_ref.value())},
  });
}

[[nodiscard]] CanonicalValue
encode_provider_contract_ref(const ProviderContractRefDTO &contract) {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id",
       CanonicalValue(contract.provider_contract_role_id.value())},
      {"contract_schema_id",
       CanonicalValue(contract.contract_schema_id.value())},
      {"contract_schema_revision",
       CanonicalValue(contract.contract_schema_revision.value())},
  });
}

[[nodiscard]] CanonicalValue
encode_retained_artifact_ref(const core::RetainedArtifactRefDTO &reference) {
  return CanonicalValue(CanonicalValue::Object{
      {"artifact_role_id", CanonicalValue(reference.artifact_role_id.value())},
      {"media_type", CanonicalValue(reference.media_type)},
      {"byte_size", CanonicalValue(static_cast<double>(reference.byte_size))},
      {"content_digest", CanonicalValue(reference.content_digest.value())},
      {"content_addressed_store_key",
       CanonicalValue(reference.content_addressed_store_key.value())},
  });
}

[[nodiscard]] CanonicalValue
encode_stop_stage_parameters(const StopStageParametersDTO &row) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_domain_id", CanonicalValue(row.control_domain_id.value())},
      {"control_domain_revision",
       CanonicalValue(row.control_domain_revision.value())},
      {"control_binding_id", CanonicalValue(row.control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(row.control_binding_revision.value())},
      {"controller_topology_id",
       CanonicalValue(row.controller_topology_id.value())},
      {"topology_revision", CanonicalValue(row.topology_revision.value())},
      {"control_route_id", CanonicalValue(row.control_route_id.value())},
      {"stop_evidence_policy_id",
       CanonicalValue(row.stop_evidence_policy_id.value())},
      {"stop_evidence_policy_revision",
       CanonicalValue(row.stop_evidence_policy_revision.value())},
      {"stop_escalation_stage_id",
       CanonicalValue(row.stop_escalation_stage_id.value())},
      {"provider_contract_ref",
       encode_provider_contract_ref(row.provider_contract_ref)},
      {"default_contract_parameters",
       json_object_to_canonical(row.default_contract_parameters)},
  });
}

[[nodiscard]] CanonicalValue
encode_commissioned_stop_interface(const CommissionedStopInterfaceDTO &row) {
  CanonicalValue::Object object{
      {"commissioned_stop_interface_id",
       CanonicalValue(row.commissioned_stop_interface_id.value())},
      {"commissioned_stop_interface_revision",
       CanonicalValue(row.commissioned_stop_interface_revision.value())},
      {"control_domain_id", CanonicalValue(row.control_domain_id.value())},
      {"control_domain_revision",
       CanonicalValue(row.control_domain_revision.value())},
      {"control_binding_id", CanonicalValue(row.control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(row.control_binding_revision.value())},
      {"control_route_id", CanonicalValue(row.control_route_id.value())},
      {"provider_instance_id",
       CanonicalValue(row.provider_instance_id.value())},
      {"provider_manifest_id",
       CanonicalValue(row.provider_manifest_id.value())},
      {"provider_manifest_revision",
       CanonicalValue(row.provider_manifest_revision.value())},
      {"registered_command_kind_id",
       CanonicalValue(row.registered_command_kind_id.value())},
      {"provider_contract_ref",
       encode_provider_contract_ref(row.provider_contract_ref)},
      {"endpoint_role_id", CanonicalValue(row.endpoint_role_id.value())},
      {"stop_action_class",
       CanonicalValue(std::string(core::to_string(row.stop_action_class)))},
      {"safety_system_request_action_class",
       CanonicalValue(std::string(
           core::to_string(row.safety_system_request_action_class)))},
      {"commissioning_evidence_artifact_ref",
       encode_retained_artifact_ref(row.commissioning_evidence_artifact_ref)},
  };
  if (row.external_resource_alias.has_value()) {
    object.emplace_back("external_resource_alias",
                        CanonicalValue(row.external_resource_alias->value()));
    object.emplace_back("external_resource_kind",
                        CanonicalValue(std::string(
                            core::to_string(*row.external_resource_kind))));
    object.emplace_back("site_overlay_value_ref",
                        CanonicalValue(row.site_overlay_value_ref->value()));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue
encode_site_overlay_ref(const SiteOverlayRefDTO &reference) {
  return CanonicalValue(CanonicalValue::Object{
      {"site_overlay_id", CanonicalValue(reference.site_overlay_id.value())},
      {"site_overlay_revision",
       CanonicalValue(reference.site_overlay_revision.value())},
      {"non_secret_digest",
       CanonicalValue(reference.non_secret_digest.value())},
  });
}

template <typename Value, typename Encoder>
[[nodiscard]] CanonicalValue encode_array(const std::vector<Value> &values,
                                          Encoder encoder) {
  CanonicalValue::Array array;
  array.reserve(values.size());
  for (const auto &value : values) {
    array.push_back(encoder(value));
  }
  return CanonicalValue(std::move(array));
}

[[nodiscard]] auto
desired_configuration_key(const DesiredControlConfigurationDTO &configuration) {
  return std::tie(configuration.control_domain_id,
                  configuration.control_domain_revision);
}

[[nodiscard]] auto
provider_binding_key(const CapabilityProviderBindingDTO &binding) {
  return std::tie(binding.capability_provider_binding_id,
                  binding.capability_provider_binding_revision);
}

void normalize_desired_configurations(
    std::vector<DesiredControlConfigurationDTO> &configurations,
    std::string_view path) {
  std::sort(configurations.begin(), configurations.end(),
            [](const auto &left, const auto &right) {
              return desired_configuration_key(left) <
                     desired_configuration_key(right);
            });
  if (std::adjacent_find(configurations.begin(), configurations.end(),
                         [](const auto &left, const auto &right) {
                           return desired_configuration_key(left) ==
                                  desired_configuration_key(right);
                         }) != configurations.end()) {
    fail(path, "duplicate complete control-domain identity");
  }
  // ID-only stale/extra selection is occurrence-specific applicability and is
  // intentionally not inferred by this schema substrate.
}

void normalize_provider_bindings(
    std::vector<CapabilityProviderBindingDTO> &bindings,
    std::string_view path) {
  std::sort(bindings.begin(), bindings.end(),
            [](const auto &left, const auto &right) {
              return provider_binding_key(left) < provider_binding_key(right);
            });
  if (std::adjacent_find(bindings.begin(), bindings.end(),
                         [](const auto &left, const auto &right) {
                           return provider_binding_key(left) ==
                                  provider_binding_key(right);
                         }) != bindings.end()) {
    fail(path, "duplicate complete capability-provider-binding identity");
  }
  // Whether a complete deployment may contain two revisions under one stable
  // binding ID is an applicability/closure question, not a stronger
  // collection key silently invented by this substrate.
}

void normalize_external_resources(std::vector<ExternalResourceDTO> &resources,
                                  std::string_view path) {
  std::sort(resources.begin(), resources.end(),
            [](const auto &left, const auto &right) {
              return left.external_resource_alias <
                     right.external_resource_alias;
            });
  if (std::adjacent_find(resources.begin(), resources.end(),
                         [](const auto &left, const auto &right) {
                           return left.external_resource_alias ==
                                  right.external_resource_alias;
                         }) != resources.end()) {
    fail(path, "duplicate external_resource_alias");
  }
}

[[nodiscard]] auto
stop_stage_parameters_key(const StopStageParametersDTO &row) {
  return std::tie(row.control_domain_id, row.control_domain_revision,
                  row.control_binding_id, row.control_binding_revision,
                  row.controller_topology_id, row.topology_revision,
                  row.control_route_id, row.stop_evidence_policy_id,
                  row.stop_evidence_policy_revision,
                  row.stop_escalation_stage_id,
                  row.provider_contract_ref.provider_contract_role_id,
                  row.provider_contract_ref.contract_schema_id,
                  row.provider_contract_ref.contract_schema_revision);
}

void normalize_stop_stage_parameters(std::vector<StopStageParametersDTO> &rows,
                                     std::string_view path) {
  std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
    return stop_stage_parameters_key(left) < stop_stage_parameters_key(right);
  });
  if (std::adjacent_find(rows.begin(), rows.end(),
                         [](const auto &left, const auto &right) {
                           return stop_stage_parameters_key(left) ==
                                  stop_stage_parameters_key(right);
                         }) != rows.end()) {
    fail(path, "duplicate complete Stop-stage-parameters identity");
  }
}

[[nodiscard]] auto commissioned_stop_interface_subject_key(
    const CommissionedStopInterfaceDTO &row) {
  return std::tie(
      row.control_domain_id, row.control_domain_revision,
      row.control_binding_id, row.control_binding_revision,
      row.control_route_id, row.provider_instance_id, row.provider_manifest_id,
      row.provider_manifest_revision, row.registered_command_kind_id,
      row.provider_contract_ref.provider_contract_role_id,
      row.provider_contract_ref.contract_schema_id,
      row.provider_contract_ref.contract_schema_revision, row.endpoint_role_id);
}

void validate_commissioned_stop_interface_shape(
    const CommissionedStopInterfaceDTO &row, std::string_view path) {
  if (row.stop_action_class != core::StopActionClass::kSafetySystemRequest) {
    fail(path,
         "commissioned Stop interface must declare safety_system_request");
  }
  const auto *definition = core::find_registered_stop_command_definition(
      row.registered_command_kind_id);
  if (definition == nullptr ||
      definition->stop_action_class != row.stop_action_class ||
      definition->safety_system_request_action_class !=
          row.safety_system_request_action_class) {
    fail(
        path,
        "registered Stop command does not match the commissioned action class");
  }
  if (row.commissioning_evidence_artifact_ref.artifact_role_id.value() !=
          core::kStopInterfaceCommissioningArtifactRole ||
      row.commissioning_evidence_artifact_ref.media_type !=
          core::kStopInterfaceCommissioningMediaType) {
    fail(path, "commissioning artifact role/media type is not exact");
  }
  const bool has_external = row.external_resource_alias.has_value();
  if (has_external != row.external_resource_kind.has_value() ||
      has_external != row.site_overlay_value_ref.has_value()) {
    fail(path, "external commissioning source fields are not jointly present");
  }
}

void normalize_commissioned_stop_interfaces(
    std::vector<CommissionedStopInterfaceDTO> &rows, std::string_view path) {
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    validate_commissioned_stop_interface_shape(rows[index],
                                               index_path(path, index));
  }
  std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
    return left.commissioned_stop_interface_id <
           right.commissioned_stop_interface_id;
  });
  for (std::size_t index = 1U; index < rows.size(); ++index) {
    if (rows[index - 1U].commissioned_stop_interface_id ==
        rows[index].commissioned_stop_interface_id) {
      fail(path, "duplicate commissioned_stop_interface_id");
    }
  }
  std::vector<const CommissionedStopInterfaceDTO *> by_subject;
  by_subject.reserve(rows.size());
  for (const auto &row : rows) {
    by_subject.push_back(&row);
  }
  std::sort(by_subject.begin(), by_subject.end(),
            [](const auto *left, const auto *right) {
              return commissioned_stop_interface_subject_key(*left) <
                     commissioned_stop_interface_subject_key(*right);
            });
  if (std::adjacent_find(
          by_subject.begin(), by_subject.end(),
          [](const auto *left, const auto *right) {
            return commissioned_stop_interface_subject_key(*left) ==
                   commissioned_stop_interface_subject_key(*right);
          }) != by_subject.end()) {
    fail(path, "duplicate commissioned Stop interface subject");
  }
}

[[nodiscard]] CanonicalValue encode_desired_configurations(
    const std::vector<DesiredControlConfigurationDTO> &values) {
  return encode_array(values, encode_desired_configuration);
}

[[nodiscard]] CanonicalValue encode_provider_bindings(
    const std::vector<CapabilityProviderBindingDTO> &values) {
  return encode_array(values, encode_provider_binding);
}

[[nodiscard]] CanonicalValue
encode_external_resources(const std::vector<ExternalResourceDTO> &values) {
  return encode_array(values, encode_external_resource);
}

[[nodiscard]] CanonicalValue encode_stop_stage_parameters(
    const std::vector<StopStageParametersDTO> &values) {
  return encode_array(values, [](const auto &value) {
    return encode_stop_stage_parameters(value);
  });
}

[[nodiscard]] CanonicalValue encode_commissioned_stop_interfaces(
    const std::vector<CommissionedStopInterfaceDTO> &values) {
  return encode_array(values, [](const auto &value) {
    return encode_commissioned_stop_interface(value);
  });
}

[[nodiscard]] CanonicalValue encode_retained_policy_records(
    const std::vector<core::CatalogRecordRefDTO> &values) {
  const CanonicalValue catalogs = detail::encode_deployment_catalogs(values);
  const CanonicalValue *records = catalogs.find_object_member("records");
  if (records == nullptr) {
    fail("$.policies.retained_policy_records",
         "catalog codec omitted its canonical records member");
  }
  return *records;
}

[[nodiscard]] CanonicalValue encode_supported_policies(
    const std::vector<StopStageParametersDTO> &stop_stage_parameters,
    const std::vector<CommissionedStopInterfaceDTO>
        &commissioned_stop_interfaces,
    const std::vector<core::CatalogRecordRefDTO> &retained_policy_records,
    const CanonicalValue &interrupt_sources,
    const CanonicalValue &interrupt_policies,
    const CanonicalValue &interrupt_config,
    const std::optional<double> motion_takeover_preparation_budget_s,
    const std::optional<double> motion_takeover_planning_budget_s,
    const CanonicalValue *interrupt_rules = nullptr,
    const std::vector<core::ScenePoseSourcePolicyDTO> &scene_pose_sources =
        {},
    const CanonicalValue *component_streams = nullptr) {
  CanonicalValue::Object result{
      {"retained_policy_records",
       encode_retained_policy_records(retained_policy_records)},
      {"stop_stage_parameters",
       encode_stop_stage_parameters(stop_stage_parameters)},
      {"commissioned_stop_interfaces",
       encode_commissioned_stop_interfaces(commissioned_stop_interfaces)},
      {"interrupt_sources", interrupt_sources},
      {"interrupt_policies", interrupt_policies},
      {"interrupt_config", interrupt_config},
  };
  // Empty/absent global tables preserve the original empty-policy identity.
  if (interrupt_rules && !interrupt_rules->as_array().empty())
    result.emplace_back("interrupt_rules", *interrupt_rules);
  if (motion_takeover_preparation_budget_s.has_value()) {
    result.emplace_back("motion_takeover_preparation_budget_s",
                        CanonicalValue(*motion_takeover_preparation_budget_s));
  }
  if (motion_takeover_planning_budget_s.has_value()) {
    result.emplace_back("motion_takeover_planning_budget_s",
                        CanonicalValue(*motion_takeover_planning_budget_s));
  }
  if (!scene_pose_sources.empty())
    result.emplace_back("scene_pose_sources",
                        EncodeScenePoseSources(scene_pose_sources));
  // Absent/empty overrides preserve every existing policy identity.
  if (component_streams && !component_streams->as_array().empty())
    result.emplace_back("component_streams", *component_streams);
  return CanonicalValue(std::move(result));
}

[[nodiscard]] std::vector<core::CatalogRecordRefDTO>
parse_retained_policy_records(const CanonicalValue::Object &policies) {
  const auto &rows = require_array(
      require_member(policies, "retained_policy_records", "$.policies"),
      "$.policies.retained_policy_records");
  if (rows.size() > core::kMaxDeploymentApplicabilityRows) {
    fail("$.policies.retained_policy_records",
         "row count exceeds the 4096-row deployment ceiling");
  }
  std::vector<core::CatalogRecordRefDTO> result;
  try {
    result = detail::decode_deployment_catalogs(CanonicalValue(
        CanonicalValue::Object{{"records", CanonicalValue(rows)}}));
  } catch (const detail::DeploymentCatalogCodecError &error) {
    fail("$.policies.retained_policy_records", error.what());
  }
  for (const auto &record : result) {
    const auto kind = core::catalog_record_kind(record);
    if (kind != core::CatalogRecordKind::kContactPolicy &&
        kind != core::CatalogRecordKind::kStopEvidencePolicy) {
      fail("$.policies.retained_policy_records",
           "record kind '" + std::string(core::to_string(kind)) +
               "' is outside the closed policy-family set");
    }
  }
  return result;
}

struct ParsedInterruptSource final {
  std::string source_id;
  std::string source_class;
  int priority{};
  CanonicalValue normalized;
};

struct ParsedInterruptPolicy final {
  std::string policy_id;
  std::string source_id;
  std::string preemption_action;
  std::string post_latch_disposition;
  std::string scope_kind;
  int effective_priority{};
  std::vector<std::string> affected_phases;
  CanonicalValue normalized;
};

[[nodiscard]] std::string
require_portable_token_string(const CanonicalValue &value,
                              std::string_view path);

[[nodiscard]] CanonicalValue
parse_interrupt_structured_value(const CanonicalValue &value,
                                 std::string_view path, bool require_nonempty) {
  const auto &object = require_object(value, path);
  if (object.size() > core::kMaxInterruptStructuredValueEntries ||
      (require_nonempty && object.empty())) {
    fail(path, require_nonempty
                   ? "structured value must contain 1..64 entries"
                   : "structured value exceeds the 64-entry ceiling");
  }
  for (const auto &[key, scalar] : object) {
    if (key.size() > core::kMaxInterruptStructuredValueKeyBytes ||
        !is_portable_token(key)) {
      fail(child_path(path, key), "key must be a 1..64 byte portable token");
    }
    if (std::holds_alternative<bool>(scalar.storage())) {
      continue;
    }
    if (const auto *number = std::get_if<double>(&scalar.storage());
        number != nullptr) {
      if (!std::isfinite(*number)) {
        fail(child_path(path, key), "number must be finite");
      }
      continue;
    }
    if (const auto *text = std::get_if<std::string>(&scalar.storage());
        text != nullptr) {
      if (text->size() > core::kMaxInterruptStructuredValueStringBytes) {
        fail(child_path(path, key),
             "string exceeds the 256-byte structured-value ceiling");
      }
      continue;
    }
    fail(child_path(path, key),
         "value must be exactly boolean, finite number, or NFC string");
  }
  return value;
}

enum class InterruptCoordinateValueKind : std::uint8_t {
  kAcceleration = 0,
  kVelocity,
  kJerk,
};

[[nodiscard]] CanonicalValue
parse_interrupt_coordinate_value(const CanonicalValue &value,
                                 std::string_view path,
                                 InterruptCoordinateValueKind value_kind) {
  const auto &object = require_object(value, path);
  const auto &coordinate_kind =
      require_string(require_member(object, "coordinate_kind", path),
                     child_path(path, "coordinate_kind"));
  std::string_view value_field;
  if (value_kind == InterruptCoordinateValueKind::kAcceleration) {
    if (coordinate_kind == "angular_rad_s2") {
      value_field = "value_rad_s2";
    } else if (coordinate_kind == "linear_m_s2") {
      value_field = "value_m_s2";
    } else {
      fail(child_path(path, "coordinate_kind"),
           "value is outside the closed acceleration-coordinate enum");
    }
  } else if (value_kind == InterruptCoordinateValueKind::kJerk) {
    if (coordinate_kind == "angular_rad_s3")
      value_field = "value_rad_s3";
    else if (coordinate_kind == "linear_m_s3")
      value_field = "value_m_s3";
    else
      fail(child_path(path, "coordinate_kind"),
           "value is outside the closed jerk-coordinate enum");
  } else if (coordinate_kind == "angular_rad_s") {
    value_field = "value_rad_s";
  } else if (coordinate_kind == "linear_m_s") {
    value_field = "value_m_s";
  } else {
    fail(child_path(path, "coordinate_kind"),
         "value is outside the closed velocity-coordinate enum");
  }
  require_exact_fields(object, {"coordinate_kind", value_field}, path);
  const double magnitude = require_finite_number(
      require_member(object, value_field, path), child_path(path, value_field));
  if (magnitude <= 0.0) {
    fail(child_path(path, value_field), "must be finite and positive");
  }
  return value;
}

[[nodiscard]] CanonicalValue
parse_interrupt_source_joint_map(const CanonicalValue &value,
                                 std::string_view path,
                                 InterruptCoordinateValueKind value_kind) {
  const auto &object = require_object(value, path);
  constexpr std::size_t kMaximumSourceJointRows = 256U;
  if (object.empty() || object.size() > kMaximumSourceJointRows) {
    fail(path, "source-joint map must contain 1..256 entries");
  }
  CanonicalValue::Object normalized;
  normalized.reserve(object.size());
  for (const auto &[source_joint, coordinate] : object) {
    if (!content_identity::is_source_symbol(source_joint)) {
      fail(child_path(path, source_joint),
           "key is outside the NFC source-symbol grammar");
    }
    normalized.emplace_back(
        source_joint,
        parse_interrupt_coordinate_value(
            coordinate, child_path(path, source_joint), value_kind));
  }
  return CanonicalValue(std::move(normalized));
}

[[nodiscard]] CanonicalValue
parse_motion_interrupt_stop_profile(const CanonicalValue &value,
                                    std::string_view path) {
  const auto &profile = require_object(value, path);
  require_closed_fields(profile, {"profile_id", "stop_mode"},
                        {"deceleration_time_s", "per_joint_max_deceleration",
                         "allow_natural_completion_on_short_path",
                         "recoverable_substitution_mode", "fallback_stop_modes",
                         "require_backend_confirmation",
                         "per_joint_settle_velocity_threshold", "diagnostics",
                         "jerk_limits_enabled", "per_joint_max_jerk"},
                        path);

  const std::string profile_id =
      require_portable_token_string(require_member(profile, "profile_id", path),
                                    child_path(path, "profile_id"));
  const std::string &stop_mode =
      require_string(require_member(profile, "stop_mode", path),
                     child_path(path, "stop_mode"));
  if (stop_mode != "soft_on_path" && stop_mode != "spring_back_hold" &&
      stop_mode != "spring_back_fast" && stop_mode != "hard_deceleration") {
    fail(child_path(path, "stop_mode"), "value is outside the closed enum");
  }

  const auto *deceleration = find_member(profile, "deceleration_time_s");
  const auto *limits = find_member(profile, "per_joint_max_deceleration");
  const auto *natural_completion =
      find_member(profile, "allow_natural_completion_on_short_path");
  const auto *substitution =
      find_member(profile, "recoverable_substitution_mode");
  const auto *fallbacks = find_member(profile, "fallback_stop_modes");
  if (substitution && fallbacks)
    fail(path, "conflicting fallback fields");
  const bool controlled_profile = stop_mode == "soft_on_path" ||
                                  stop_mode == "spring_back_hold" ||
                                  stop_mode == "spring_back_fast";
  if ((stop_mode == "soft_on_path" && deceleration == nullptr) ||
      (controlled_profile && limits == nullptr)) {
    fail(path,
         "soft_on_path/spring_back_hold requires its exact duration/limit "
         "presence matrix");
  }
  if (!controlled_profile && (deceleration != nullptr || limits != nullptr)) {
    fail(path, "hard_deceleration forbids deceleration_time_s and "
               "per_joint_max_deceleration");
  }

  CanonicalValue::Object normalized{
      {"profile_id", CanonicalValue(profile_id)},
      {"stop_mode", CanonicalValue(stop_mode)},
  };
  const auto *jerk_enabled = find_member(profile, "jerk_limits_enabled");
  const auto *jerk_limits = find_member(profile, "per_joint_max_jerk");
  if (controlled_profile && jerk_enabled == nullptr)
    fail(child_path(path, "jerk_limits_enabled"),
         "review and save an explicit controlled-Stop jerk choice; legacy "
         "omission is not activation");
  if (!controlled_profile &&
      (jerk_enabled != nullptr || jerk_limits != nullptr))
    fail(path, "hard_deceleration does not declare jerk shaping");
  normalized.emplace_back(
      "jerk_limits_enabled",
      CanonicalValue(controlled_profile
                         ? require_bool(*jerk_enabled,
                                        child_path(path, "jerk_limits_enabled"))
                         : false));
  if (jerk_limits != nullptr)
    normalized.emplace_back(
        "per_joint_max_jerk",
        parse_interrupt_source_joint_map(*jerk_limits,
                                         child_path(path, "per_joint_max_jerk"),
                                         InterruptCoordinateValueKind::kJerk));
  if (deceleration != nullptr) {
    const double duration = require_finite_number(
        *deceleration, child_path(path, "deceleration_time_s"));
    if (duration <= 0.0 ||
        duration > core::kMaxControlledStopDecelerationTimeS) {
      fail(child_path(path, "deceleration_time_s"),
           "must be finite and within (0,2.0]");
    }
    normalized.emplace_back("deceleration_time_s", *deceleration);
  }
  if (limits != nullptr) {
    normalized.emplace_back("per_joint_max_deceleration",
                            parse_interrupt_source_joint_map(
                                *limits,
                                child_path(path, "per_joint_max_deceleration"),
                                InterruptCoordinateValueKind::kAcceleration));
  }

  if (stop_mode == "hard_deceleration" && natural_completion != nullptr) {
    fail(
        child_path(path, "allow_natural_completion_on_short_path"),
        "explicit presence is legal only for soft_on_path or spring_back_hold");
  }
  const bool normalized_natural_completion =
      stop_mode != "hard_deceleration"
          ? (natural_completion == nullptr
                 ? stop_mode == "soft_on_path"
                 : require_bool(
                       *natural_completion,
                       child_path(path,
                                  "allow_natural_completion_on_short_path")))
          : false;
  normalized.emplace_back("allow_natural_completion_on_short_path",
                          CanonicalValue(normalized_natural_completion));

  if (substitution != nullptr) {
    const auto &mode = require_string(
        *substitution, child_path(path, "recoverable_substitution_mode"));
    if (stop_mode != "soft_on_path" || mode != "spring_back_hold") {
      fail(child_path(path, "recoverable_substitution_mode"),
           "only soft_on_path to spring_back_hold is qualified");
    }
    normalized.emplace_back(
        "fallback_stop_modes",
        CanonicalValue(CanonicalValue::Array{CanonicalValue(mode)}));
  }

  if (fallbacks) {
    const auto &modes =
        require_array(*fallbacks, child_path(path, "fallback_stop_modes"));
    if (modes.size() > 2U)
      fail(path, "too many controlled Stop fallbacks");
    int previous = stop_mode == "soft_on_path"       ? 0
                   : stop_mode == "spring_back_hold" ? 1
                   : stop_mode == "spring_back_fast" ? 2
                                                     : 3;
    for (const auto &item : modes) {
      const auto &mode =
          require_string(item, child_path(path, "fallback_stop_modes"));
      const int current = mode == "spring_back_hold"   ? 1
                          : mode == "spring_back_fast" ? 2
                                                       : -1;
      if (current <= previous)
        fail(path, "fallbacks must be strictly ordered below the primary, "
                   "excluding hard");
      previous = current;
    }
    if (!modes.empty())
      normalized.emplace_back("fallback_stop_modes", *fallbacks);
  }

  const auto *confirmation =
      find_member(profile, "require_backend_confirmation");
  normalized.emplace_back(
      "require_backend_confirmation",
      CanonicalValue(
          confirmation == nullptr
              ? true
              : require_bool(
                    *confirmation,
                    child_path(path, "require_backend_confirmation"))));

  if (const auto *settle =
          find_member(profile, "per_joint_settle_velocity_threshold");
      settle != nullptr) {
    normalized.emplace_back(
        "per_joint_settle_velocity_threshold",
        parse_interrupt_source_joint_map(
            *settle, child_path(path, "per_joint_settle_velocity_threshold"),
            InterruptCoordinateValueKind::kVelocity));
  }
  if (const auto *diagnostics = find_member(profile, "diagnostics");
      diagnostics != nullptr) {
    normalized.emplace_back(
        "diagnostics",
        parse_interrupt_structured_value(
            *diagnostics, child_path(path, "diagnostics"), false));
  }
  return CanonicalValue(std::move(normalized));
}

[[nodiscard]] std::string
require_portable_token_string(const CanonicalValue &value,
                              std::string_view path) {
  const std::string &text = require_string(value, path);
  if (!is_portable_token(text)) {
    fail(path,
         "expected a 1..128 byte portable token beginning with an ASCII "
         "alphanumeric and containing only alphanumerics, '.', '_', or '-'");
  }
  return text;
}

[[nodiscard]] CanonicalValue
parse_canonical_token_set(const CanonicalValue &value, std::string_view path,
                          std::vector<std::string> &normalized_values,
                          bool require_nonempty = false) {
  const auto &rows = require_array(value, path);
  if (rows.size() > core::kMaxDeploymentApplicabilityRows ||
      (require_nonempty && rows.empty())) {
    fail(path, require_nonempty
                   ? "set must be non-empty and contain at most 4096 rows"
                   : "set exceeds the 4096-row ceiling");
  }
  normalized_values.clear();
  normalized_values.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    normalized_values.push_back(
        require_portable_token_string(rows[index], index_path(path, index)));
  }
  std::sort(normalized_values.begin(), normalized_values.end());
  if (std::adjacent_find(normalized_values.begin(), normalized_values.end()) !=
      normalized_values.end()) {
    fail(path, "set contains a duplicate identity");
  }
  CanonicalValue::Array normalized;
  normalized.reserve(normalized_values.size());
  for (const auto &item : normalized_values) {
    normalized.emplace_back(item);
  }
  return CanonicalValue(std::move(normalized));
}

[[nodiscard]] CanonicalValue
parse_ordered_enum_set(const CanonicalValue &value, std::string_view path,
                       std::initializer_list<std::string_view> enum_order,
                       std::vector<std::string> &normalized_values,
                       bool require_nonempty = false) {
  const auto &rows = require_array(value, path);
  if (rows.size() > enum_order.size() || (require_nonempty && rows.empty())) {
    fail(path, require_nonempty
                   ? "enum set must be non-empty"
                   : "enum set exceeds the closed enum cardinality");
  }
  normalized_values.clear();
  normalized_values.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto &token = require_string(rows[index], index_path(path, index));
    if (std::find(enum_order.begin(), enum_order.end(), token) ==
        enum_order.end()) {
      fail(index_path(path, index), "value is outside the closed enum");
    }
    normalized_values.push_back(token);
  }
  std::sort(normalized_values.begin(), normalized_values.end(),
            [&](const auto &left, const auto &right) {
              return std::find(enum_order.begin(), enum_order.end(), left) <
                     std::find(enum_order.begin(), enum_order.end(), right);
            });
  if (std::adjacent_find(normalized_values.begin(), normalized_values.end()) !=
      normalized_values.end()) {
    fail(path, "enum set contains a duplicate value");
  }
  CanonicalValue::Array normalized;
  normalized.reserve(normalized_values.size());
  for (const auto &item : normalized_values) {
    normalized.emplace_back(item);
  }
  return CanonicalValue(std::move(normalized));
}

void append_optional_copy(CanonicalValue::Object &target,
                          const CanonicalValue::Object &source,
                          std::string_view name) {
  if (const auto *value = find_member(source, name); value != nullptr) {
    target.emplace_back(std::string(name), *value);
  }
}

[[nodiscard]] CanonicalValue
validate_interrupt_config(const CanonicalValue::Object &policies) {
  const auto &value =
      require_member(policies, "interrupt_config", "$.policies");
  const auto &object = require_object(value, "$.policies.interrupt_config");
  require_exact_fields(object,
                       {"queue_depth", "cancel_validation_timeout_ns",
                        "motion_scope_arm_timeout_ns",
                        "supervisory_poll_rate_hz"},
                       "$.policies.interrupt_config");
  (void)require_integer_in_range(
      require_member(object, "queue_depth", "$.policies.interrupt_config"), 1U,
      core::kMaxDeploymentApplicabilityRows,
      "$.policies.interrupt_config.queue_depth");
  constexpr std::uint64_t kMaximumIJsonInteger = 9007199254740991ULL;
  for (const std::string_view name :
       {"cancel_validation_timeout_ns", "motion_scope_arm_timeout_ns"}) {
    (void)require_integer_in_range(
        require_member(object, name, "$.policies.interrupt_config"), 1U,
        kMaximumIJsonInteger, child_path("$.policies.interrupt_config", name));
  }
  const double supervisory_rate = require_finite_number(
      require_member(object, "supervisory_poll_rate_hz",
                     "$.policies.interrupt_config"),
      "$.policies.interrupt_config.supervisory_poll_rate_hz");
  if (supervisory_rate < 1.0 || supervisory_rate > 10.0) {
    fail("$.policies.interrupt_config.supervisory_poll_rate_hz",
         "must be within the qualified supervisory range [1,10]");
  }
  return value;
}

[[nodiscard]] CanonicalValue parse_interrupt_sources(
    const CanonicalValue::Object &policies,
    const std::vector<CapabilityProviderBindingDTO> &provider_bindings,
    std::vector<ParsedInterruptSource> &parsed_sources) {
  const auto &rows =
      require_array(require_member(policies, "interrupt_sources", "$.policies"),
                    "$.policies.interrupt_sources");
  if (rows.size() > core::kMaxDeploymentApplicabilityRows) {
    fail("$.policies.interrupt_sources",
         "row count exceeds the 4096-row ceiling");
  }
  parsed_sources.clear();
  parsed_sources.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = index_path("$.policies.interrupt_sources", index);
    const auto &source = require_object(rows[index], path);
    require_closed_fields(source,
                          {"source_id", "capability_provider_binding_id",
                           "adapter_capability_id", "signal", "source_class",
                           "kind", "priority"},
                          {"maskable", "freshness_budget_ns", "debounce_ns",
                           "hysteresis_value", "description"},
                          path);
    ParsedInterruptSource parsed;
    parsed.source_id =
        require_portable_token_string(require_member(source, "source_id", path),
                                      child_path(path, "source_id"));
    const std::string provider_binding_id = require_portable_token_string(
        require_member(source, "capability_provider_binding_id", path),
        child_path(path, "capability_provider_binding_id"));
    const auto provider_count =
        std::count_if(provider_bindings.begin(), provider_bindings.end(),
                      [&](const auto &binding) {
                        return binding.capability_provider_binding_id.value() ==
                               provider_binding_id;
                      });
    if (provider_count != 1) {
      fail(child_path(path, "capability_provider_binding_id"),
           provider_count == 0
               ? "references an unknown capability-provider binding"
               : "does not resolve to one exact binding revision");
    }
    const std::string adapter_capability = require_portable_token_string(
        require_member(source, "adapter_capability_id", path),
        child_path(path, "adapter_capability_id"));
    if (adapter_capability.find('.') == std::string::npos) {
      fail(child_path(path, "adapter_capability_id"),
           "must be a namespaced adapter capability token");
    }
    const std::string &signal = require_string(
        require_member(source, "signal", path), child_path(path, "signal"));
    constexpr std::size_t kMaximumNormalizedStringBytes = 1048576U;
    if (signal.empty() || signal.size() > kMaximumNormalizedStringBytes) {
      fail(child_path(path, "signal"),
           "must be a non-empty bounded UTF-8 string");
    }
    parsed.source_class =
        require_string(require_member(source, "source_class", path),
                       child_path(path, "source_class"));
    if (parsed.source_class != "fast_execution_relevant" &&
        parsed.source_class != "low_rate_supervisory") {
      fail(child_path(path, "source_class"),
           "value is outside the closed enum");
    }
    const std::string &kind = require_string(
        require_member(source, "kind", path), child_path(path, "kind"));
    if (kind != "edge" && kind != "level") {
      fail(child_path(path, "kind"), "value is outside the closed enum");
    }
    parsed.priority = static_cast<int>(
        require_integer_in_range(require_member(source, "priority", path), 0U,
                                 1000U, child_path(path, "priority")));
    if (const auto *maskable = find_member(source, "maskable");
        maskable != nullptr) {
      (void)require_bool(*maskable, child_path(path, "maskable"));
    }
    constexpr std::uint64_t kMaximumIJsonInteger = 9007199254740991ULL;
    for (const std::string_view field :
         {"freshness_budget_ns", "debounce_ns"}) {
      if (const auto *duration = find_member(source, field);
          duration != nullptr) {
        (void)require_integer_in_range(*duration, 0U, kMaximumIJsonInteger,
                                       child_path(path, field));
      }
    }
    if (const auto *hysteresis = find_member(source, "hysteresis_value");
        hysteresis != nullptr) {
      if (kind != "level") {
        fail(child_path(path, "hysteresis_value"),
             "is legal only for a level source");
      }
      if (const auto *number = std::get_if<double>(&hysteresis->storage());
          number != nullptr) {
        if (!std::isfinite(*number)) {
          fail(child_path(path, "hysteresis_value"), "must be finite");
        }
      } else if (std::holds_alternative<CanonicalValue::Object>(
                     hysteresis->storage())) {
        (void)parse_interrupt_structured_value(
            *hysteresis, child_path(path, "hysteresis_value"), true);
      } else if (!std::holds_alternative<bool>(hysteresis->storage())) {
        fail(child_path(path, "hysteresis_value"),
             "must be boolean, finite number, or a non-empty closed "
             "structured value");
      }
    }
    if (const auto *description = find_member(source, "description");
        description != nullptr) {
      if (require_string(*description, child_path(path, "description")).size() >
          kMaximumNormalizedStringBytes) {
        fail(child_path(path, "description"),
             "exceeds the normalized string ceiling");
      }
    }
    parsed.normalized = rows[index];
    parsed_sources.push_back(std::move(parsed));
  }
  std::sort(parsed_sources.begin(), parsed_sources.end(),
            [](const auto &left, const auto &right) {
              return left.source_id < right.source_id;
            });
  if (std::adjacent_find(parsed_sources.begin(), parsed_sources.end(),
                         [](const auto &left, const auto &right) {
                           return left.source_id == right.source_id;
                         }) != parsed_sources.end()) {
    fail("$.policies.interrupt_sources", "duplicate source_id");
  }
  CanonicalValue::Array normalized;
  normalized.reserve(parsed_sources.size());
  for (const auto &source : parsed_sources) {
    normalized.push_back(source.normalized);
  }
  return CanonicalValue(std::move(normalized));
}

[[nodiscard]] const ParsedInterruptSource &
require_interrupt_source(const std::vector<ParsedInterruptSource> &sources,
                         std::string_view source_id, std::string_view path) {
  const auto source =
      std::lower_bound(sources.begin(), sources.end(), source_id,
                       [](const auto &left, std::string_view right) {
                         return left.source_id < right;
                       });
  if (source == sources.end() || source->source_id != source_id) {
    fail(path, "references an unknown interrupt source");
  }
  return *source;
}

void validate_interrupt_condition(const CanonicalValue &condition,
                                  std::string_view path, std::size_t depth,
                                  std::size_t &nodes) {
  if (depth > 8U || ++nodes > 64U)
    fail(path, "interrupt condition exceeds authored tree bounds");
  const auto &record = require_object(condition, path);
  if (record.size() != 1U)
    fail(path, "condition requires exactly one operator");
  if (const auto *comparison = find_member(record, "compare")) {
    const auto &fields = require_object(*comparison, path);
    require_exact_fields(fields, {"operand", "op", "value"}, path);
    const auto &operand = require_member(fields, "operand", path);
    const auto &selector = require_object(operand, path);
    if (selector.size() != 1U)
      fail(path, "operand requires exactly one selector");
    bool signal_operand = false;
    if (const auto *key = find_member(selector, "key")) {
      (void)require_portable_token_string(*key, path);
    } else {
      signal_operand = true;
      const auto &signal =
          require_object(require_member(selector, "signal", path), path);
      require_exact_fields(signal, {"controllable_component_id", "signal_name"},
                           path);
      (void)require_portable_token_string(
          require_member(signal, "controllable_component_id", path), path);
      const auto &name =
          require_string(require_member(signal, "signal_name", path), path);
      if (name.empty() || name.size() > 256U ||
          name.find('\0') != std::string::npos)
        fail(path, "invalid condition signal name");
    }
    const auto &op = require_string(require_member(fields, "op", path), path);
    if (op != "==" && op != "!=" && op != "<" && op != "<=" && op != ">" &&
        op != ">=")
      fail(path, "unknown condition comparison operator");
    const auto &literal = require_member(fields, "value", path);
    if (!literal.is_bool() && !literal.is_string() && !literal.is_number())
      fail(path, "condition value requires a scalar literal");
    if (literal.is_number() &&
        std::trunc(literal.as_number()) == literal.as_number() &&
        std::fabs(literal.as_number()) > 9007199254740991.0)
      fail(path, "condition integer literal exceeds safe range");
    if ((signal_operand && !literal.is_bool()) ||
        (!literal.is_number() && op != "==" && op != "!="))
      fail(path, "condition comparison types are incompatible");
    return;
  }
  if (const auto *child = find_member(record, "not")) {
    validate_interrupt_condition(*child, path, depth + 1U, nodes);
    return;
  }
  const auto *children = find_member(record, "all");
  if (children == nullptr)
    children = &require_member(record, "any", path);
  const auto &array = require_array(*children, path);
  if (array.empty() || array.size() > 64U - nodes)
    fail(path, "invalid interrupt condition child count");
  for (const auto &child : array)
    validate_interrupt_condition(child, path, depth + 1U, nodes);
}

// Global rules have their own closed authored carrier. This normalization
// checks syntax only; Applied Program group/key and retained signal proof use
// the existing checked condition/compiler boundary before actual Apply.
[[nodiscard]] CanonicalValue
parse_interrupt_rules(const CanonicalValue::Object &policies) {
  const auto *value = find_member(policies, "interrupt_rules");
  if (!value)
    return CanonicalValue(CanonicalValue::Array{});
  const auto &rules = require_array(*value, "$.policies.interrupt_rules");
  if (rules.size() > core::kMaxDeploymentApplicabilityRows)
    fail("$.policies.interrupt_rules",
         "global rule count exceeds deployment bound");
  std::map<std::string, CanonicalValue> ordered;
  for (std::size_t index = 0; index < rules.size(); ++index) {
    const auto path = index_path("$.policies.interrupt_rules", index);
    const auto &rule = require_object(rules[index], path);
    require_closed_fields(
        rule, {"rule_id", "condition", "scope"},
        {"priority", "idle_behaviour", "stop_entry_stage_id", "latch_release"},
        path);
    const auto id = require_portable_token_string(
        require_member(rule, "rule_id", path), path);
    const auto &condition = require_member(rule, "condition", path);
    std::size_t nodes = 0;
    validate_interrupt_condition(condition, child_path(path, "condition"), 1U,
                                 nodes);
    const auto &scope = require_member(rule, "scope", path);
    CanonicalValue normalized_scope = scope;
    if (scope.is_string()) {
      if (scope.as_string() != "all_motion" &&
          scope.as_string() != "everything")
        fail(
            path,
            "global scope requires all_motion, execution_groups or everything");
    } else {
      const auto &selector = require_object(scope, child_path(path, "scope"));
      require_exact_fields(selector, {"execution_groups"}, path);
      const auto &groups = require_array(
          require_member(selector, "execution_groups", path), path);
      if (groups.empty() || groups.size() > core::kMaxProgramExecutionGroups)
        fail(path, "global execution_groups must be nonempty and bounded");
      std::set<std::string> names;
      for (const auto &group : groups) {
        const auto &name = require_string(group, path);
        if (!core::valid_program_execution_group(name) ||
            !names.insert(name).second)
          fail(path, "global execution group is invalid or duplicated");
      }
      CanonicalValue::Array normalized_groups;
      for (const auto &name : names)
        normalized_groups.emplace_back(name);
      normalized_scope = CanonicalValue(CanonicalValue::Object{
          {"execution_groups", CanonicalValue(std::move(normalized_groups))}});
    }
    double priority = 0;
    if (const auto *authored = find_member(rule, "priority")) {
      priority = require_finite_number(*authored, path);
      if (priority < 0 || priority > 100 || std::trunc(priority) != priority)
        fail(path, "global priority must be an integer in 0..100");
    }
    CanonicalValue::Object normalized{{"rule_id", CanonicalValue(id)},
                                      {"condition", condition},
                                      {"scope", std::move(normalized_scope)},
                                      {"priority", CanonicalValue(priority)}};
    std::string latch_release = "operator";
    if (const auto *authored = find_member(rule, "latch_release")) {
      latch_release =
          require_string(*authored, child_path(path, "latch_release"));
      if (latch_release != "operator" && latch_release != "auto_on_clear")
        fail(child_path(path, "latch_release"),
             "global latch_release requires operator or auto_on_clear");
    }
    normalized.emplace_back("latch_release", CanonicalValue(latch_release));
    if (const auto *idle = find_member(rule, "idle_behaviour")) {
      const auto &token = require_string(*idle, path);
      if (token != "latch" && token != "ignore")
        fail(path, "invalid global idle_behaviour");
      normalized.emplace_back("idle_behaviour", *idle);
    }
    if (const auto *entry = find_member(rule, "stop_entry_stage_id")) {
      (void)require_portable_token_string(*entry, path);
      normalized.emplace_back("stop_entry_stage_id", *entry);
    }
    if (!ordered.emplace(id, CanonicalValue(std::move(normalized))).second)
      fail(path, "duplicate global rule_id");
  }
  CanonicalValue::Array normalized;
  for (auto &[id, rule] : ordered) {
    (void)id;
    normalized.push_back(std::move(rule));
  }
  return CanonicalValue(std::move(normalized));
}

[[nodiscard]] CanonicalValue parse_interrupt_policies(
    const CanonicalValue::Object &policies,
    const std::vector<ParsedInterruptSource> &parsed_sources) {
  const auto &rows = require_array(
      require_member(policies, "interrupt_policies", "$.policies"),
      "$.policies.interrupt_policies");
  if (rows.size() > core::kMaxDeploymentApplicabilityRows) {
    fail("$.policies.interrupt_policies",
         "row count exceeds the 4096-row ceiling");
  }
  std::vector<ParsedInterruptPolicy> parsed_policies;
  parsed_policies.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = index_path("$.policies.interrupt_policies", index);
    const auto &policy = require_object(rows[index], path);
    require_closed_fields(
        policy,
        {"policy_id", "controllable_component_ids", "command_target_ids",
         "control_domain_ids", "control_binding_ids", "resource_ids",
         "affected_phases", "preemption_action", "post_latch_disposition",
         "scope_kind"},
        {"source_id", "condition", "idle_behaviour", "priority", "maskable",
         "maskable_by", "stop_scope", "scope_expiry",
         "allowed_activation_modes", "allowed_stop_modes", "stop_profile",
         "description", "stop_entry_stage_id", "fallback_outcome"},
        path);
    ParsedInterruptPolicy parsed;
    parsed.policy_id =
        require_portable_token_string(require_member(policy, "policy_id", path),
                                      child_path(path, "policy_id"));
    const auto *source_value = find_member(policy, "source_id");
    const auto *condition = find_member(policy, "condition");
    const ParsedInterruptSource *source = nullptr;
    CanonicalValue::Object normalized{
        {"policy_id", CanonicalValue(parsed.policy_id)}};
    if (source_value != nullptr) {
      parsed.source_id = require_portable_token_string(
          *source_value, child_path(path, "source_id"));
      source = &require_interrupt_source(parsed_sources, parsed.source_id,
                                         child_path(path, "source_id"));
      normalized.emplace_back("source_id", CanonicalValue(parsed.source_id));
    }
    if (condition != nullptr) {
      std::size_t nodes = 0U;
      validate_interrupt_condition(*condition, child_path(path, "condition"),
                                   1U, nodes);
      normalized.emplace_back("condition", *condition);
    }
    std::vector<std::string> values;
    bool has_required_scope = false;
    for (const std::string_view field :
         {"controllable_component_ids", "command_target_ids",
          "control_domain_ids", "control_binding_ids", "resource_ids"}) {
      normalized.emplace_back(
          std::string(field),
          parse_canonical_token_set(require_member(policy, field, path),
                                    child_path(path, field), values));
      if ((field == "control_domain_ids" || field == "resource_ids") &&
          !values.empty()) {
        has_required_scope = true;
      }
    }
    if (!has_required_scope) {
      fail(path,
           "interrupt policy requires at least one control domain or resource");
    }
    parsed.scope_kind =
        require_string(require_member(policy, "scope_kind", path),
                       child_path(path, "scope_kind"));
    if (parsed.scope_kind != "static" && parsed.scope_kind != "operation") {
      fail(child_path(path, "scope_kind"), "value is outside the closed enum");
    }
    if ((parsed.scope_kind == "static" &&
         ((source_value == nullptr) == (condition == nullptr))) ||
        (parsed.scope_kind == "operation" && condition != nullptr))
      fail(path, "static policy requires one source_id or condition; operation "
                 "condition comes from node");
    if (const auto *idle = find_member(policy, "idle_behaviour")) {
      const auto &token =
          require_string(*idle, child_path(path, "idle_behaviour"));
      if (parsed.scope_kind != "static" ||
          (token != "latch" && token != "ignore"))
        fail(path, "idle_behaviour is latch|ignore on static policies only");
      normalized.emplace_back("idle_behaviour", *idle);
    }
    normalized.emplace_back(
        "affected_phases",
        parse_ordered_enum_set(require_member(policy, "affected_phases", path),
                               child_path(path, "affected_phases"),
                               {"planning", "validating", "dispatching",
                                "executing", "stopping",
                                "intervention_required"},
                               parsed.affected_phases, true));
    parsed.preemption_action =
        require_string(require_member(policy, "preemption_action", path),
                       child_path(path, "preemption_action"));
    constexpr std::array<std::string_view, 6U> kActions{
        "cancel_planning", "cancel_validation", "preempt_trajectory",
        "latch_held",      "latch_faulted",     "complete_move_on_signal"};
    if (std::find(kActions.begin(), kActions.end(), parsed.preemption_action) ==
        kActions.end()) {
      fail(child_path(path, "preemption_action"),
           "value is outside the closed enum");
    }
    parsed.post_latch_disposition =
        require_string(require_member(policy, "post_latch_disposition", path),
                       child_path(path, "post_latch_disposition"));
    constexpr std::array<std::string_view, 5U> kDispositions{
        "await_operator_ack", "await_recovery_action",
        "auto_clear_on_signal_release", "auto_clear_after_motion_outcome",
        "fault_terminal"};
    if (std::find(kDispositions.begin(), kDispositions.end(),
                  parsed.post_latch_disposition) == kDispositions.end()) {
      fail(child_path(path, "post_latch_disposition"),
           "value is outside the closed enum");
    }
    if (const auto *entry = find_member(policy, "stop_entry_stage_id")) {
      normalized.emplace_back(
          "stop_entry_stage_id",
          CanonicalValue(require_portable_token_string(
              *entry, child_path(path, "stop_entry_stage_id"))));
    }
    if (const auto *fallback = find_member(policy, "fallback_outcome")) {
      const auto &value =
          require_string(*fallback, child_path(path, "fallback_outcome"));
      if ((value != "fail_node" && value != "count_as_success") ||
          (value == "count_as_success" &&
           (parsed.scope_kind != "operation" ||
            parsed.preemption_action != "complete_move_on_signal")))
        fail(child_path(path, "fallback_outcome"),
             "count_as_success requires an operation desired-trip rule");
      normalized.emplace_back("fallback_outcome", *fallback);
    }
    if (const auto *priority = find_member(policy, "priority");
        priority != nullptr) {
      parsed.effective_priority = static_cast<int>(require_integer_in_range(
          *priority, 0U, source == nullptr ? 100U : 1000U,
          child_path(path, "priority")));
    } else if (source != nullptr) {
      parsed.effective_priority = source->priority;
    } else {
      parsed.effective_priority = 0; // Contract 59: source-less default only.
    }
    if (const auto *maskable = find_member(policy, "maskable");
        maskable != nullptr) {
      (void)require_bool(*maskable, child_path(path, "maskable"));
    }
    if (const auto *maskable_by = find_member(policy, "maskable_by");
        maskable_by != nullptr) {
      normalized.emplace_back(
          "maskable_by",
          parse_canonical_token_set(*maskable_by,
                                    child_path(path, "maskable_by"), values));
    }
    if (const auto *stop_scope = find_member(policy, "stop_scope");
        stop_scope != nullptr) {
      const auto &token =
          require_string(*stop_scope, child_path(path, "stop_scope"));
      if ((token != "affected_resources" && token != "full_cell") ||
          (parsed.preemption_action != "preempt_trajectory" &&
           parsed.preemption_action != "complete_move_on_signal")) {
        fail(
            child_path(path, "stop_scope"),
            "is outside the closed enum or is invalid for the selected action");
      }
    }
    if (const auto *expiry = find_member(policy, "scope_expiry");
        expiry != nullptr) {
      if (parsed.scope_kind != "operation" ||
          require_string(*expiry, child_path(path, "scope_expiry")) !=
              "operation_terminal") {
        fail(child_path(path, "scope_expiry"),
             "is legal only as operation_terminal on operation scope");
      }
    }
    if (const auto *activation =
            find_member(policy, "allowed_activation_modes");
        activation != nullptr) {
      if (parsed.scope_kind != "operation") {
        fail(child_path(path, "allowed_activation_modes"),
             "is legal only on operation scope");
      }
      normalized.emplace_back(
          "allowed_activation_modes",
          parse_ordered_enum_set(
              *activation, child_path(path, "allowed_activation_modes"),
              {"require_inactive_at_arm", "fresh_edge_after_arm",
               "trip_if_already_active", "maintain_level_with_hysteresis"},
              values));
    }
    const auto *allowed_stop_modes = find_member(policy, "allowed_stop_modes");
    if (parsed.preemption_action == "complete_move_on_signal") {
      if (parsed.scope_kind != "operation" || allowed_stop_modes == nullptr) {
        fail(path, "complete_move_on_signal requires operation scope and "
                   "allowed_stop_modes");
      }
      normalized.emplace_back(
          "allowed_stop_modes",
          parse_ordered_enum_set(
              *allowed_stop_modes, child_path(path, "allowed_stop_modes"),
              {"spring_back_hold", "soft_on_path", "hard_deceleration",
               "spring_back_fast"},
              values,
              true));
      if ((source != nullptr &&
           source->source_class != "fast_execution_relevant") ||
          parsed.post_latch_disposition != "auto_clear_after_motion_outcome") {
        fail(path, "complete_move_on_signal requires a fast source and "
                   "auto_clear_after_motion_outcome");
      }
    } else if (allowed_stop_modes != nullptr) {
      fail(child_path(path, "allowed_stop_modes"),
           "is legal only on complete_move_on_signal");
    }
    if (const auto *stop_profile = find_member(policy, "stop_profile");
        stop_profile != nullptr) {
      if (parsed.preemption_action != "preempt_trajectory") {
        fail(child_path(path, "stop_profile"),
             "is legal only on preempt_trajectory");
      }
      normalized.emplace_back(
          "stop_profile", parse_motion_interrupt_stop_profile(
                              *stop_profile, child_path(path, "stop_profile")));
    }
    if (parsed.scope_kind == "static" &&
        parsed.preemption_action == "complete_move_on_signal") {
      fail(path, "static policy cannot use complete_move_on_signal");
    }
    if (parsed.scope_kind == "operation" &&
        (parsed.post_latch_disposition == "await_operator_ack" ||
         std::find(parsed.affected_phases.begin(), parsed.affected_phases.end(),
                   "executing") == parsed.affected_phases.end())) {
      fail(path, "operation policy must include executing and cannot await "
                 "operator acknowledgement");
    }
    if (parsed.post_latch_disposition == "auto_clear_after_motion_outcome" &&
        (parsed.scope_kind != "operation" ||
         parsed.preemption_action != "complete_move_on_signal")) {
      fail(path, "auto_clear_after_motion_outcome requires operation "
                 "complete_move_on_signal");
    }
    if (const auto *description = find_member(policy, "description");
        description != nullptr) {
      constexpr std::size_t kMaximumNormalizedStringBytes = 1048576U;
      if (require_string(*description, child_path(path, "description")).size() >
          kMaximumNormalizedStringBytes) {
        fail(child_path(path, "description"),
             "exceeds the normalized string ceiling");
      }
    }
    normalized.emplace_back("preemption_action",
                            CanonicalValue(parsed.preemption_action));
    normalized.emplace_back("post_latch_disposition",
                            CanonicalValue(parsed.post_latch_disposition));
    normalized.emplace_back("scope_kind", CanonicalValue(parsed.scope_kind));
    for (const std::string_view field : {"priority", "maskable", "stop_scope",
                                         "scope_expiry", "description"}) {
      append_optional_copy(normalized, policy, field);
    }
    parsed.normalized = CanonicalValue(std::move(normalized));
    parsed_policies.push_back(std::move(parsed));
  }
  std::sort(parsed_policies.begin(), parsed_policies.end(),
            [](const auto &left, const auto &right) {
              return left.policy_id < right.policy_id;
            });
  if (std::adjacent_find(parsed_policies.begin(), parsed_policies.end(),
                         [](const auto &left, const auto &right) {
                           return left.policy_id == right.policy_id;
                         }) != parsed_policies.end()) {
    fail("$.policies.interrupt_policies", "duplicate policy_id");
  }
  for (const auto &policy : parsed_policies) {
    const auto &object = policy.normalized.as_object();
    if (const auto *maskable_by = find_member(object, "maskable_by");
        maskable_by != nullptr) {
      const auto &mask_ids =
          std::get<CanonicalValue::Array>(maskable_by->storage());
      for (std::size_t index = 0U; index < mask_ids.size(); ++index) {
        const auto &id = std::get<std::string>(mask_ids[index].storage());
        const auto match =
            std::lower_bound(parsed_policies.begin(), parsed_policies.end(), id,
                             [](const auto &left, const std::string &right) {
                               return left.policy_id < right;
                             });
        if (match == parsed_policies.end() || match->policy_id != id) {
          fail(index_path("$.policies.interrupt_policies.maskable_by", index),
               "references an unknown interrupt policy");
        }
      }
    }
  }
  for (std::size_t left_index = 0U; left_index < parsed_policies.size();
       ++left_index) {
    for (std::size_t right_index = left_index + 1U;
         right_index < parsed_policies.size(); ++right_index) {
      const auto &left = parsed_policies[left_index];
      const auto &right = parsed_policies[right_index];
      if (left.source_id.empty() || right.source_id.empty() ||
          left.source_id != right.source_id) {
        continue;
      }
      const bool shares_phase =
          std::any_of(left.affected_phases.begin(), left.affected_phases.end(),
                      [&](const auto &phase) {
                        return std::find(right.affected_phases.begin(),
                                         right.affected_phases.end(),
                                         phase) != right.affected_phases.end();
                      });
      if (shares_phase && left.effective_priority == right.effective_priority &&
          (left.preemption_action != right.preemption_action ||
           left.post_latch_disposition != right.post_latch_disposition)) {
        fail("$.policies.interrupt_policies",
             "same-source equal-priority overlapping phases have ambiguous "
             "behavior");
      }
      const auto check_trip_priority = [&](const ParsedInterruptPolicy &trip,
                                           const ParsedInterruptPolicy &fault) {
        const bool fault_action =
            fault.scope_kind == "static" &&
            (fault.preemption_action == "preempt_trajectory" ||
             fault.preemption_action == "latch_held" ||
             fault.preemption_action == "latch_faulted");
        if (trip.preemption_action == "complete_move_on_signal" &&
            fault_action &&
            fault.effective_priority <= trip.effective_priority) {
          fail("$.policies.interrupt_policies",
               "same-source static fault policy must outrank "
               "complete_move_on_signal");
        }
      };
      check_trip_priority(left, right);
      check_trip_priority(right, left);
    }
  }
  CanonicalValue::Array normalized;
  normalized.reserve(parsed_policies.size());
  for (const auto &policy : parsed_policies) {
    normalized.push_back(policy.normalized);
  }
  return CanonicalValue(std::move(normalized));
}

// Same grammar as core::ValidStreamId (1..64 bytes of `[a-z0-9_./-]`).
[[nodiscard]] bool valid_stream_id(std::string_view value) noexcept {
  return !value.empty() && value.size() <= core::kMaxStreamIdBytes &&
         std::all_of(value.begin(), value.end(), [](const char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_' || c == '.' || c == '/' || c == '-';
         });
}

[[nodiscard]] std::string_view
stream_severity_text(const core::StreamSeverityDTO value) noexcept {
  switch (value) {
  case core::StreamSeverityDTO::kFault:
    return "fault";
  case core::StreamSeverityDTO::kBlocking:
    return "blocking";
  case core::StreamSeverityDTO::kWarning:
    return "warning";
  }
  return "fault";
}

[[nodiscard]] std::string_view
stream_recovery_text(const core::StreamRecoveryDTO value) noexcept {
  switch (value) {
  case core::StreamRecoveryDTO::kAck:
    return "ack";
  case core::StreamRecoveryDTO::kAuto:
    return "auto";
  }
  return "ack";
}

} // namespace

std::vector<core::StreamParameterOverrideDTO>
decode_component_stream_overrides(const CanonicalValue &value) {
  constexpr std::string_view kPath = "$.policies.component_streams";
  constexpr std::uint64_t kMaximumIJsonInteger = 9007199254740991ULL;
  const auto &rows = require_array(value, kPath);
  if (rows.size() > core::kMaxDeploymentApplicabilityRows) {
    fail(kPath, "row count exceeds the 4096-row deployment ceiling");
  }
  std::vector<core::StreamParameterOverrideDTO> result;
  result.reserve(rows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const std::string path = index_path(kPath, index);
    const auto &object = require_object(rows[index], path);
    require_closed_fields(object, {"controllable_component_id", "stream_id"},
                          {"budget_ns", "severity", "recovery"}, path);
    core::StreamParameterOverrideDTO row{
        parse_portable_id<ControllableComponentId>(
            require_member(object, "controllable_component_id", path),
            child_path(path, "controllable_component_id")),
        require_string(require_member(object, "stream_id", path),
                       child_path(path, "stream_id")),
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
    if (!valid_stream_id(row.stream_id)) {
      fail(child_path(path, "stream_id"),
           "expected 1..64 bytes of [a-z0-9_./-]");
    }
    if (const auto *budget = find_member(object, "budget_ns")) {
      row.budget_ns = static_cast<std::int64_t>(
          require_integer_in_range(*budget, 1U, kMaximumIJsonInteger,
                                   child_path(path, "budget_ns")));
    }
    if (const auto *severity = find_member(object, "severity")) {
      const std::string severity_path = child_path(path, "severity");
      const std::string &text = require_string(*severity, severity_path);
      for (const auto candidate :
           {core::StreamSeverityDTO::kFault, core::StreamSeverityDTO::kBlocking,
            core::StreamSeverityDTO::kWarning}) {
        if (text == stream_severity_text(candidate)) {
          row.severity = candidate;
        }
      }
      if (!row.severity) {
        fail(severity_path, "expected fault, blocking, or warning");
      }
    }
    if (const auto *recovery = find_member(object, "recovery")) {
      const std::string recovery_path = child_path(path, "recovery");
      const std::string &text = require_string(*recovery, recovery_path);
      for (const auto candidate :
           {core::StreamRecoveryDTO::kAck, core::StreamRecoveryDTO::kAuto}) {
        if (text == stream_recovery_text(candidate)) {
          row.recovery = candidate;
        }
      }
      if (!row.recovery) {
        fail(recovery_path, "expected ack or auto");
      }
    }
    if (!row.budget_ns && !row.severity && !row.recovery) {
      fail(path, "expected at least one of budget_ns, severity, or recovery");
    }
    // A `fault` stream opens an incident, which only the operator's Recover
    // closes: `auto` recovery cannot apply to it.
    if (row.severity == core::StreamSeverityDTO::kFault &&
        row.recovery == core::StreamRecoveryDTO::kAuto) {
      fail(child_path(path, "recovery"),
           "severity fault requires recovery ack (fault + auto is invalid)");
    }
    result.push_back(std::move(row));
  }
  const auto key = [](const core::StreamParameterOverrideDTO &row) {
    return std::tie(row.controllable_component_id, row.stream_id);
  };
  std::sort(result.begin(), result.end(),
            [&key](const auto &left, const auto &right) {
              return key(left) < key(right);
            });
  if (std::adjacent_find(result.begin(), result.end(),
                         [&key](const auto &left, const auto &right) {
                           return key(left) == key(right);
                         }) != result.end()) {
    fail(kPath, "duplicate (controllable_component_id, stream_id) row");
  }
  return result;
}

CanonicalValue encode_component_stream_overrides(
    const std::vector<core::StreamParameterOverrideDTO> &rows) {
  CanonicalValue::Array result;
  result.reserve(rows.size());
  for (const auto &row : rows) {
    CanonicalValue::Object object{
        {"controllable_component_id",
         CanonicalValue(row.controllable_component_id.value())},
        {"stream_id", CanonicalValue(row.stream_id)},
    };
    if (row.budget_ns) {
      object.emplace_back("budget_ns",
                          CanonicalValue(static_cast<double>(*row.budget_ns)));
    }
    if (row.severity) {
      object.emplace_back(
          "severity",
          CanonicalValue(std::string(stream_severity_text(*row.severity))));
    }
    if (row.recovery) {
      object.emplace_back(
          "recovery",
          CanonicalValue(std::string(stream_recovery_text(*row.recovery))));
    }
    result.emplace_back(std::move(object));
  }
  return CanonicalValue(std::move(result));
}

TypedDeploymentApplicabilityInputsCandidate
decode_deployment_applicability_inputs_candidate(
    const ParsedArtifactCandidate &parsed_deployment) {
  const auto &root = require_object(parsed_deployment.root(), "$");
  const std::string &api_version =
      require_string(require_member(root, "api_version", "$"), "$.api_version");
  if (api_version != kDeploymentApiVersion) {
    fail("$.api_version", "expected manyforge.deployment.v1");
  }

  const auto &desired_values =
      require_array(require_member(root, "desired_control_configurations", "$"),
                    "$.desired_control_configurations");
  if (desired_values.size() > core::kMaxDeploymentApplicabilityRows) {
    fail("$.desired_control_configurations",
         "configuration count exceeds the 4096-row deployment ceiling");
  }
  std::vector<DesiredControlConfigurationDTO> desired;
  desired.reserve(desired_values.size());
  for (std::size_t index = 0; index < desired_values.size(); ++index) {
    desired.push_back(parse_desired_configuration(
        desired_values[index],
        index_path("$.desired_control_configurations", index)));
  }
  normalize_desired_configurations(desired, "$.desired_control_configurations");

  const auto &provider_values =
      require_array(require_member(root, "capability_provider_bindings", "$"),
                    "$.capability_provider_bindings");
  if (provider_values.size() > core::kMaxDeploymentApplicabilityRows) {
    fail("$.capability_provider_bindings",
         "provider-binding count exceeds the 4096-row deployment ceiling");
  }
  std::vector<CapabilityProviderBindingDTO> providers;
  providers.reserve(provider_values.size());
  for (std::size_t index = 0; index < provider_values.size(); ++index) {
    providers.push_back(parse_provider_binding(
        provider_values[index],
        index_path("$.capability_provider_bindings", index)));
  }
  normalize_provider_bindings(providers, "$.capability_provider_bindings");

  std::vector<ExternalResourceDTO> external_resources;
  const CanonicalValue *external_value =
      find_member(root, "external_resources");
  const bool external_resources_was_present = external_value != nullptr;
  if (external_value != nullptr) {
    const auto &external_values =
        require_array(*external_value, "$.external_resources");
    if (external_values.size() > core::kMaxDeploymentApplicabilityRows) {
      fail("$.external_resources",
           "resource count exceeds the 4096-row deployment ceiling");
    }
    external_resources.reserve(external_values.size());
    for (std::size_t index = 0; index < external_values.size(); ++index) {
      external_resources.push_back(parse_external_resource(
          external_values[index], index_path("$.external_resources", index)));
    }
  }
  normalize_external_resources(external_resources, "$.external_resources");

  std::vector<StopStageParametersDTO> stop_stage_parameters;
  std::vector<CommissionedStopInterfaceDTO> commissioned_stop_interfaces;
  std::vector<core::CatalogRecordRefDTO> retained_policy_records;
  std::optional<double> motion_takeover_preparation_budget_s;
  std::optional<double> motion_takeover_planning_budget_s;
  std::vector<core::ScenePoseSourcePolicyDTO> scene_pose_sources;
  std::optional<CanonicalValue> normalized_policies;
  const CanonicalValue *policies_value = find_member(root, "policies");
  if (policies_value != nullptr) {
    const auto &policies = require_object(*policies_value, "$.policies");
    require_closed_fields(policies,
                          {"retained_policy_records", "stop_stage_parameters",
                           "commissioned_stop_interfaces", "interrupt_sources",
                           "interrupt_policies", "interrupt_config"},
                          {"motion_takeover_preparation_budget_s",
                           "motion_takeover_planning_budget_s",
                           "interrupt_rules", "scene_pose_sources",
                           "component_streams"},
                          "$.policies");
    if (const auto *sources = find_member(policies, "scene_pose_sources"))
      scene_pose_sources = DecodeScenePoseSources(*sources);
    std::optional<CanonicalValue> component_streams;
    if (const auto *rows = find_member(policies, "component_streams"))
      component_streams = encode_component_stream_overrides(
          decode_component_stream_overrides(*rows));
    if (const auto *budget =
            find_member(policies, "motion_takeover_preparation_budget_s")) {
      constexpr auto path = "$.policies.motion_takeover_preparation_budget_s";
      const auto seconds = require_finite_number(*budget, path);
      if (seconds <= 0.0) {
        fail(path, "expected positive seconds");
      }
      motion_takeover_preparation_budget_s = seconds;
    }
    if (const auto *budget =
            find_member(policies, "motion_takeover_planning_budget_s")) {
      constexpr auto path = "$.policies.motion_takeover_planning_budget_s";
      const auto seconds = require_finite_number(*budget, path);
      if (seconds <= 0.0 || !motion_takeover_preparation_budget_s ||
          seconds >= *motion_takeover_preparation_budget_s) {
        fail(path,
             "expected positive seconds less than the preparation budget");
      }
      motion_takeover_planning_budget_s = seconds;
    }
    retained_policy_records = parse_retained_policy_records(policies);
    const CanonicalValue interrupt_config = validate_interrupt_config(policies);
    std::vector<ParsedInterruptSource> parsed_interrupt_sources;
    CanonicalValue normalized_interrupt_sources =
        parse_interrupt_sources(policies, providers, parsed_interrupt_sources);
    CanonicalValue normalized_interrupt_policies =
        parse_interrupt_policies(policies, parsed_interrupt_sources);
    const auto normalized_interrupt_rules = parse_interrupt_rules(policies);

    const auto &parameter_values = require_array(
        require_member(policies, "stop_stage_parameters", "$.policies"),
        "$.policies.stop_stage_parameters");
    if (parameter_values.size() > core::kMaxDeploymentApplicabilityRows) {
      fail("$.policies.stop_stage_parameters",
           "row count exceeds the 4096-row deployment ceiling");
    }
    stop_stage_parameters.reserve(parameter_values.size());
    for (std::size_t index = 0U; index < parameter_values.size(); ++index) {
      stop_stage_parameters.push_back(parse_stop_stage_parameters(
          parameter_values[index],
          index_path("$.policies.stop_stage_parameters", index)));
    }
    normalize_stop_stage_parameters(stop_stage_parameters,
                                    "$.policies.stop_stage_parameters");

    const auto &commissioned_values = require_array(
        require_member(policies, "commissioned_stop_interfaces", "$.policies"),
        "$.policies.commissioned_stop_interfaces");
    constexpr std::size_t kMaxCommissionedStopInterfaces = 256U;
    if (commissioned_values.size() > kMaxCommissionedStopInterfaces) {
      fail("$.policies.commissioned_stop_interfaces",
           "row count exceeds the 256-row commissioning ceiling");
    }
    commissioned_stop_interfaces.reserve(commissioned_values.size());
    for (std::size_t index = 0U; index < commissioned_values.size(); ++index) {
      commissioned_stop_interfaces.push_back(parse_commissioned_stop_interface(
          commissioned_values[index],
          index_path("$.policies.commissioned_stop_interfaces", index)));
    }
    normalize_commissioned_stop_interfaces(
        commissioned_stop_interfaces,
        "$.policies.commissioned_stop_interfaces");
    normalized_policies = encode_supported_policies(
        stop_stage_parameters, commissioned_stop_interfaces,
        retained_policy_records, normalized_interrupt_sources,
        normalized_interrupt_policies, interrupt_config,
        motion_takeover_preparation_budget_s, motion_takeover_planning_budget_s,
        &normalized_interrupt_rules, scene_pose_sources,
        component_streams ? &*component_streams : nullptr);
  }

  std::optional<SiteOverlayRefDTO> site_overlay_ref;
  std::optional<CanonicalValue> normalized_site_overlay_ref;
  SiteOverlayRefSourcePresence site_overlay_ref_presence =
      SiteOverlayRefSourcePresence::kAbsent;
  const CanonicalValue *site_overlay_ref_value =
      find_member(root, "site_overlay_ref");
  if (site_overlay_ref_value != nullptr) {
    if (std::holds_alternative<std::nullptr_t>(
            site_overlay_ref_value->storage())) {
      site_overlay_ref_presence = SiteOverlayRefSourcePresence::kExplicitNull;
      normalized_site_overlay_ref.emplace(nullptr);
    } else {
      site_overlay_ref_presence = SiteOverlayRefSourcePresence::kValue;
      site_overlay_ref =
          parse_site_overlay_ref(*site_overlay_ref_value, "$.site_overlay_ref");
      normalized_site_overlay_ref = encode_site_overlay_ref(*site_overlay_ref);
    }
  }

  DeploymentApplicabilityInputsDTO value{
      std::move(site_overlay_ref),
      std::move(desired),
      std::move(providers),
      std::move(external_resources),
      std::move(stop_stage_parameters),
      std::move(commissioned_stop_interfaces),
      std::move(retained_policy_records),
      motion_takeover_preparation_budget_s,
      motion_takeover_planning_budget_s,
      std::move(scene_pose_sources),
  };
  CanonicalValue normalized_desired =
      encode_desired_configurations(value.desired_control_configurations);
  CanonicalValue normalized_providers =
      encode_provider_bindings(value.capability_provider_bindings);
  CanonicalValue normalized_external =
      encode_external_resources(value.external_resources);
  const TypedAssistantSettingsCandidate assistant_settings =
      decode_assistant_settings_candidate(parsed_deployment);
  return TypedDeploymentApplicabilityInputsCandidate(
      std::move(value), std::move(normalized_desired),
      std::move(normalized_providers), std::move(normalized_external),
      std::move(normalized_policies), site_overlay_ref_presence,
      std::move(normalized_site_overlay_ref), external_resources_was_present,
      assistant_settings);
}

RevisionVerifiedDeploymentApplicabilityInputs
compute_capability_provider_binding_revisions(
    const TypedDeploymentApplicabilityInputsCandidate &candidate) {
  DeploymentApplicabilityInputsDTO value = candidate.value();
  for (auto &binding : value.capability_provider_bindings) {
    const std::string revision = detail::compute_content_identity(
        encode_provider_binding(binding),
        kCapabilityProviderBindingIdentitySpec);
    binding.capability_provider_binding_revision =
        CapabilityProviderBindingRevision(revision);
  }
  normalize_provider_bindings(value.capability_provider_bindings,
                              "$.capability_provider_bindings");
  CanonicalValue normalized_providers =
      encode_provider_bindings(value.capability_provider_bindings);

  for (auto &row : value.commissioned_stop_interfaces) {
    row.commissioned_stop_interface_revision =
        CommissionedStopInterfaceRevision(detail::compute_content_identity(
            encode_commissioned_stop_interface(row),
            kCommissionedStopInterfaceIdentitySpec));
  }
  normalize_commissioned_stop_interfaces(
      value.commissioned_stop_interfaces,
      "$.policies.commissioned_stop_interfaces");
  std::optional<CanonicalValue> normalized_policies;
  if (candidate.normalized_policies().has_value()) {
    const auto require_normalized_policy_member =
        [&](std::string_view name) -> const CanonicalValue & {
      const auto *member =
          candidate.normalized_policies()->find_object_member(name);
      if (member == nullptr) {
        fail("$.policies",
             "normalized policy projection lost " + std::string(name));
      }
      return *member;
    };
    normalized_policies = encode_supported_policies(
        value.stop_stage_parameters, value.commissioned_stop_interfaces,
        value.retained_policy_records,
        require_normalized_policy_member("interrupt_sources"),
        require_normalized_policy_member("interrupt_policies"),
        require_normalized_policy_member("interrupt_config"),
        value.motion_takeover_preparation_budget_s,
        value.motion_takeover_planning_budget_s,
        candidate.normalized_policies()->find_object_member("interrupt_rules"),
        value.scene_pose_sources,
        candidate.normalized_policies()->find_object_member(
            "component_streams"));
  }

  return RevisionVerifiedDeploymentApplicabilityInputs(
      TypedDeploymentApplicabilityInputsCandidate(
          std::move(value),
          candidate.normalized_desired_control_configurations(),
          std::move(normalized_providers),
          candidate.normalized_external_resources(),
          std::move(normalized_policies), candidate.site_overlay_ref_presence(),
          candidate.normalized_site_overlay_ref(),
          candidate.external_resources_was_present(),
          candidate.assistant_settings()));
}

RevisionVerifiedDeploymentApplicabilityInputs
verify_capability_provider_binding_revisions(
    const TypedDeploymentApplicabilityInputsCandidate &candidate) {
  for (std::size_t index = 0;
       index < candidate.value().capability_provider_bindings.size(); ++index) {
    const auto &binding = candidate.value().capability_provider_bindings[index];
    if (!detail::verify_content_identity(
            encode_provider_binding(binding),
            kCapabilityProviderBindingIdentitySpec)) {
      fail(index_path("$.capability_provider_bindings", index),
           "capability_provider_binding_revision does not match the exact "
           "normalized record");
    }
  }

  for (std::size_t index = 0U;
       index < candidate.value().commissioned_stop_interfaces.size(); ++index) {
    if (!detail::verify_content_identity(
            encode_commissioned_stop_interface(
                candidate.value().commissioned_stop_interfaces[index]),
            kCommissionedStopInterfaceIdentitySpec)) {
      fail(index_path("$.policies.commissioned_stop_interfaces", index),
           "commissioned_stop_interface_revision does not match the exact "
           "normalized row");
    }
  }

  return RevisionVerifiedDeploymentApplicabilityInputs(candidate);
}

QualifiedCommissioningEvidenceRef resolve_and_verify_commissioning_evidence_ref(
    const core::CommissionedStopInterfaceDTO &commissioned_interface,
    const core::ContentAddressedStoreKey &resolved_store_key,
    std::string_view stored_bytes) {
  const auto &artifact =
      commissioned_interface.commissioning_evidence_artifact_ref;
  if (artifact.artifact_role_id.value() !=
          core::kStopInterfaceCommissioningArtifactRole ||
      artifact.media_type != core::kStopInterfaceCommissioningMediaType) {
    fail("$.policies.commissioned_stop_interfaces",
         "commissioning evidence has the wrong fixed artifact role or media "
         "type");
  }
  if (!is_sha256_digest(artifact.content_digest.value())) {
    fail("$.policies.commissioned_stop_interfaces",
         "commissioning evidence content digest is not canonical SHA-256");
  }
  const std::string expected_store_key =
      "sha256/" + artifact.content_digest.value().substr(7U);
  if (artifact.content_addressed_store_key.value() != expected_store_key ||
      resolved_store_key != artifact.content_addressed_store_key) {
    fail("$.policies.commissioned_stop_interfaces",
         "commissioning evidence did not resolve through its exact safe "
         "content-addressed key");
  }
  if (stored_bytes.size() > std::numeric_limits<std::uint64_t>::max() ||
      static_cast<std::uint64_t>(stored_bytes.size()) != artifact.byte_size ||
      sha256_digest(stored_bytes) != artifact.content_digest.value()) {
    fail("$.policies.commissioned_stop_interfaces",
         "commissioning evidence byte count or stored-byte digest mismatch");
  }
  return QualifiedCommissioningEvidenceRef(
      commissioned_interface.commissioned_stop_interface_id,
      commissioned_interface.commissioned_stop_interface_revision, artifact);
}

} // namespace manyforge::deployment
