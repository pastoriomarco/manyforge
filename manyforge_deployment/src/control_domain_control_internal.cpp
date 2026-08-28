#include "control_domain_control_internal.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "content_identity_internal.hpp"
#include "manyforge_deployment/cell_compiler.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "native_execution_runtime_unit_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment::detail {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kImplementationProfileApiVersion =
    "manyforge.control_implementation_profile.v1";
constexpr std::string_view kImplementationProfileId = "manyforge.reference_ros2_control";
constexpr std::string_view kImplementationApiVersion =
    "manyforge.control_controller_implementation.v1";
constexpr std::string_view kSourceConformanceApiVersion = "manyforge.control_source_conformance.v1";

enum class ReferenceImplementation {
  kDirectJointTrajectoryController,
  kChainableJointTrajectoryController,
  kAdmittanceController,
};

struct ImplementationDescriptor final {
  std::string_view implementation_id;
  std::string_view implementation_type;
};

constexpr ImplementationDescriptor implementation_descriptor(
    ReferenceImplementation implementation) {
  switch (implementation) {
    case ReferenceImplementation::kDirectJointTrajectoryController:
      return {"manyforge.ros2_control.joint_trajectory_controller.direct",
              "joint_trajectory_controller/JointTrajectoryController"};
    case ReferenceImplementation::kChainableJointTrajectoryController:
      return {"manyforge.ros2_control.joint_trajectory_controller.chainable",
              "joint_trajectory_controller/JointTrajectoryController"};
    case ReferenceImplementation::kAdmittanceController:
      return {"manyforge.ros2_control.admittance_controller",
              "admittance_controller/AdmittanceController"};
  }
  return {};
}

[[nodiscard]] CanonicalValue text_array(std::span<const std::string> values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value);
  }
  return CanonicalValue(std::move(result));
}

template <typename Strong>
[[nodiscard]] CanonicalValue strong_id_array(std::span<const Strong> values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(value.value());
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] std::string direction_token(core::ControllerClaimDirection direction) {
  switch (direction) {
    case core::ControllerClaimDirection::kCommand:
      return "command";
    case core::ControllerClaimDirection::kState:
      return "state";
  }
  throw std::logic_error("unknown controller claim direction");
}

[[nodiscard]] bool contains_direction(std::span<const core::ControllerClaimDirection> directions,
                                      core::ControllerClaimDirection expected) {
  return std::find(directions.begin(), directions.end(), expected) != directions.end();
}

[[nodiscard]] CanonicalValue directions_projection(
    std::span<const core::ControllerClaimDirection> directions) {
  CanonicalValue::Array result;
  result.reserve(directions.size());
  for (const auto direction : directions) {
    result.emplace_back(direction_token(direction));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue claim_projection(const ExpandedControlInterfaceClaim& claim) {
  return CanonicalValue(CanonicalValue::Object{
      {"resource_role_id", CanonicalValue(claim.resource_role_id.value())},
      {"resource_id", CanonicalValue(claim.resource_id.value())},
      {"interface_name", CanonicalValue(claim.interface_name)},
      {"logical_interface_symbol", CanonicalValue(claim.logical_interface_symbol)},
  });
}

[[nodiscard]] CanonicalValue claims_projection(
    std::span<const ExpandedControlInterfaceClaim> claims) {
  CanonicalValue::Array result;
  result.reserve(claims.size());
  for (const auto& claim : claims) {
    result.push_back(claim_projection(claim));
  }
  return CanonicalValue(std::move(result));
}

[[nodiscard]] const core::ControlDomainDTO& require_domain(
    const CompilerQualifiedCellIr& compiler_ir, const core::ControlDomainId& control_domain_id) {
  const auto& domains = compiler_ir.applicability().composition().graph().control_domains;
  const auto found = std::find_if(domains.begin(), domains.end(), [&](const auto& domain) {
    return domain.control_domain_id == control_domain_id;
  });
  if (found == domains.end()) {
    fail_retained_bundle("control-domain product selected an unknown compiler domain");
  }
  return *found;
}

[[nodiscard]] const core::DesiredControlConfigurationDTO& require_desired_configuration(
    const CompilerQualifiedCellIr& compiler_ir, const core::ControlDomainId& control_domain_id) {
  const auto& desired =
      compiler_ir.applicability().deployment_inputs().value().desired_control_configurations;
  const core::DesiredControlConfigurationDTO* match = nullptr;
  for (const auto& candidate : desired) {
    if (candidate.control_domain_id != control_domain_id) {
      continue;
    }
    if (match != nullptr) {
      fail_retained_bundle("control-domain product found duplicate desired configuration");
    }
    match = &candidate;
  }
  if (match == nullptr) {
    fail_retained_bundle("control-domain product has no desired configuration");
  }
  return *match;
}

[[nodiscard]] const QualifiedControllerTopology& require_topology(
    const CompilerQualifiedCellIr& compiler_ir, const core::ControlDomainDTO& domain) {
  const auto& topologies = compiler_ir.applicability().controller_topologies();
  const auto found = std::find_if(topologies.begin(), topologies.end(), [&](const auto& topology) {
    return topology.value().controller_topology_id == domain.controller_topology_id &&
           topology.value().topology_revision == domain.topology_revision;
  });
  if (found == topologies.end()) {
    fail_retained_bundle("control-domain product selected no exact qualified topology");
  }
  return *found;
}

[[nodiscard]] const LogicalBindingInterfaceTemplate& require_binding_interface(
    const CompilerQualifiedCellIr& compiler_ir, const core::ControlBindingId& binding_id,
    const core::ControlBindingRevision& binding_revision) {
  const auto& interfaces = compiler_ir.applicability().binding_interfaces();
  const auto found = std::find_if(interfaces.begin(), interfaces.end(), [&](const auto& value) {
    return value.control_binding_id == binding_id &&
           value.control_binding_revision == binding_revision;
  });
  if (found == interfaces.end()) {
    fail_retained_bundle("control-domain product selected no exact binding interface");
  }
  return *found;
}

[[nodiscard]] CanonicalValue desired_configuration_projection(
    const core::DesiredControlConfigurationDTO& desired) {
  CanonicalValue::Array binding_routes;
  binding_routes.reserve(desired.binding_routes.size());
  for (const auto& route : desired.binding_routes) {
    binding_routes.emplace_back(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(route.control_binding_id.value())},
        {"control_binding_revision", CanonicalValue(route.control_binding_revision.value())},
        {"control_route_id", CanonicalValue(route.control_route_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"control_domain_id", CanonicalValue(desired.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(desired.control_domain_revision.value())},
      {"controller_topology_id", CanonicalValue(desired.controller_topology_id.value())},
      {"topology_revision", CanonicalValue(desired.topology_revision.value())},
      {"activation_set_id", CanonicalValue(desired.activation_set_id.value())},
      {"binding_routes", CanonicalValue(std::move(binding_routes))},
  });
}

[[nodiscard]] CanonicalValue binding_projection(const core::ControlBindingDTO& binding,
                                                const LogicalBindingInterfaceTemplate& interface) {
  return CanonicalValue(CanonicalValue::Object{
      {"control_binding_id", CanonicalValue(binding.control_binding_id.value())},
      {"control_binding_revision", CanonicalValue(binding.control_binding_revision.value())},
      {"controllable_component_id", CanonicalValue(binding.controllable_component_id.value())},
      {"command_target_id", CanonicalValue(binding.command_target_id.value())},
      {"capability_id", CanonicalValue(binding.capability_id.value())},
      {"capability_definition_revision",
       CanonicalValue(binding.capability_definition_revision.value())},
      {"control_interface_contract_id",
       CanonicalValue(binding.control_interface_contract_id.value())},
      {"control_interface_revision", CanonicalValue(binding.control_interface_revision.value())},
      {"provider_instance_id", CanonicalValue(binding.provider_instance_id.value())},
      {"provider_runtime_unit_id",
       CanonicalValue(binding.provider_runtime_unit_id.value())},
      {"provider_manifest_id", CanonicalValue(binding.provider_manifest_id.value())},
      {"provider_manifest_revision", CanonicalValue(binding.provider_manifest_revision.value())},
      {"state_authority_id", CanonicalValue(binding.state_authority_id.value())},
      {"state_authority_class",
       CanonicalValue(std::string(core::to_string(binding.state_authority_class)))},
      {"stop_evidence_policy_id", CanonicalValue(binding.stop_evidence_policy_id.value())},
      {"stop_evidence_policy_revision",
       CanonicalValue(binding.stop_evidence_policy_revision.value())},
      {"containment_granularity",
       CanonicalValue(std::string(core::to_string(binding.containment_granularity)))},
      {"command_claims", claims_projection(interface.command_claims)},
      {"state_claims", claims_projection(interface.state_claims)},
  });
}

[[nodiscard]] std::string revision_for_implementation(const ImplementationDescriptor& descriptor) {
  const CanonicalValue root(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kImplementationApiVersion))},
      {"implementation_id", CanonicalValue(std::string(descriptor.implementation_id))},
      {"implementation_revision", CanonicalValue(std::string(kZeroDigest))},
      {"implementation_type", CanonicalValue(std::string(descriptor.implementation_type))},
  });
  return compute_content_identity(
      root, ContentIdentitySpec{
                .identity_domain = "control-controller-implementation-revision",
                .api_version = std::string(kImplementationApiVersion),
                .own_field = "implementation_revision",
            });
}

[[nodiscard]] CanonicalValue implementation_projection(ReferenceImplementation implementation) {
  const auto descriptor = implementation_descriptor(implementation);
  return CanonicalValue(CanonicalValue::Object{
      {"implementation_id", CanonicalValue(std::string(descriptor.implementation_id))},
      {"implementation_revision", CanonicalValue(revision_for_implementation(descriptor))},
      {"implementation_type", CanonicalValue(std::string(descriptor.implementation_type))},
  });
}

[[nodiscard]] std::string implementation_profile_revision() {
  CanonicalValue::Array implementations;
  for (const auto implementation : {ReferenceImplementation::kAdmittanceController,
                                    ReferenceImplementation::kChainableJointTrajectoryController,
                                    ReferenceImplementation::kDirectJointTrajectoryController}) {
    implementations.push_back(implementation_projection(implementation));
  }
  const CanonicalValue root(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kImplementationProfileApiVersion))},
      {"implementation_profile_id", CanonicalValue(std::string(kImplementationProfileId))},
      {"implementation_profile_revision", CanonicalValue(std::string(kZeroDigest))},
      {"implementations", CanonicalValue(std::move(implementations))},
  });
  return compute_content_identity(root,
                                  ContentIdentitySpec{
                                      .identity_domain = "control-implementation-profile-revision",
                                      .api_version = std::string(kImplementationProfileApiVersion),
                                      .own_field = "implementation_profile_revision",
                                  });
}

[[nodiscard]] ReferenceImplementation select_implementation(
    const LogicalControllerTemplate& controller) {
  bool has_claim = false;
  bool has_export = false;
  for (const auto& projection : controller.binding_projections) {
    has_claim |= !projection.claimed_directions.empty();
    has_export |= !projection.exported_reference_interfaces.empty() ||
                  !projection.exported_state_interfaces.empty();
  }
  if (has_export) {
    if (!has_claim) {
      fail_retained_bundle(
          "reference control profile does not admit an exporting controller "
          "without an exact contract claim");
    }
    return ReferenceImplementation::kAdmittanceController;
  }
  if (has_claim) {
    return ReferenceImplementation::kDirectJointTrajectoryController;
  }
  return ReferenceImplementation::kChainableJointTrajectoryController;
}

[[nodiscard]] CanonicalValue controller_projection_without_digest(
    const CompilerQualifiedCellIr& compiler_ir, const LogicalControllerTemplate& controller) {
  CanonicalValue::Array binding_projections;
  binding_projections.reserve(controller.binding_projections.size());
  for (const auto& projection : controller.binding_projections) {
    const auto& interface = require_binding_interface(compiler_ir, projection.control_binding_id,
                                                      projection.control_binding_revision);
    const bool claims_command =
        contains_direction(projection.claimed_directions, core::ControllerClaimDirection::kCommand);
    const bool claims_state =
        contains_direction(projection.claimed_directions, core::ControllerClaimDirection::kState);
    binding_projections.emplace_back(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(projection.control_binding_id.value())},
        {"control_binding_revision", CanonicalValue(projection.control_binding_revision.value())},
        {"control_route_id", CanonicalValue(projection.control_route_id.value())},
        {"activation_set_id", CanonicalValue(projection.activation_set_id.value())},
        {"claimed_directions", directions_projection(projection.claimed_directions)},
        {"claimed_command_interfaces", claims_command ? claims_projection(interface.command_claims)
                                                      : CanonicalValue(CanonicalValue::Array{})},
        {"claimed_state_interfaces", claims_state ? claims_projection(interface.state_claims)
                                                  : CanonicalValue(CanonicalValue::Array{})},
        {"exported_reference_interfaces",
         claims_projection(projection.exported_reference_interfaces)},
        {"exported_state_interfaces", claims_projection(projection.exported_state_interfaces)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"controller_role_id", CanonicalValue(controller.controller_role_id.value())},
      {"logical_controller_symbol", CanonicalValue(controller.logical_controller_symbol)},
      {"generated_controller_name", CanonicalValue(controller.logical_controller_symbol)},
      {"implementation", implementation_projection(select_implementation(controller))},
      {"configuration_digest", CanonicalValue(std::string(kZeroDigest))},
      {"binding_projections", CanonicalValue(std::move(binding_projections))},
  });
}

[[nodiscard]] CanonicalValue controller_projection(const CompilerQualifiedCellIr& compiler_ir,
                                                   const LogicalControllerTemplate& controller) {
  CanonicalValue root = controller_projection_without_digest(compiler_ir, controller);
  const std::string digest = compute_content_identity(
      root, ContentIdentitySpec{
                .identity_domain = "control-controller-configuration-digest",
                .api_version = std::string(kControlDomainControlApiVersion) + "#controller",
                .own_field = "configuration_digest",
                .root_shape = ContentIdentityRootShape::kNestedRecord,
            });
  CanonicalValue::Object object = root.as_object();
  const auto found = std::find_if(object.begin(), object.end(), [](const auto& member) {
    return member.first == "configuration_digest";
  });
  if (found == object.end()) {
    throw std::logic_error("controller configuration projection lost its digest member");
  }
  found->second = CanonicalValue(digest);
  return CanonicalValue(std::move(object));
}

[[nodiscard]] const core::ControllerChainEdgeDTO& require_topology_edge(
    const core::ControllerTopologyDTO& topology, const core::ControllerChainEdgeRefDTO& edge) {
  const auto found = std::find_if(
      topology.chain_edges.begin(), topology.chain_edges.end(), [&](const auto& candidate) {
        return candidate.upstream_controller_role_id == edge.upstream_controller_role_id &&
               candidate.downstream_controller_role_id == edge.downstream_controller_role_id;
      });
  if (found == topology.chain_edges.end()) {
    fail_retained_bundle("control-domain route selected no exact topology edge");
  }
  return *found;
}

[[nodiscard]] const LogicalControllerBindingProjection& require_controller_binding_projection(
    const LogicalControllerTemplate& controller, const LogicalControlRouteTemplate& route) {
  const auto found = std::find_if(
      controller.binding_projections.begin(), controller.binding_projections.end(),
      [&](const auto& projection) {
        return projection.control_binding_id == route.control_binding_id &&
               projection.control_binding_revision == route.control_binding_revision &&
               projection.control_route_id == route.control_route_id &&
               std::find(route.compatible_activation_set_ids.begin(),
                         route.compatible_activation_set_ids.end(),
                         projection.activation_set_id) != route.compatible_activation_set_ids.end();
      });
  if (found == controller.binding_projections.end()) {
    fail_retained_bundle("control-domain route has no matching controller projection");
  }
  return *found;
}

[[nodiscard]] std::vector<ExpandedControlInterfaceClaim> project_edge_claims(
    std::span<const core::ResourceInterfaceClaimDTO> abstract_claims,
    std::span<const ExpandedControlInterfaceClaim> exported_claims) {
  std::vector<ExpandedControlInterfaceClaim> result;
  for (const auto& abstract : abstract_claims) {
    const auto initial_size = result.size();
    for (const auto& expanded : exported_claims) {
      if (expanded.resource_role_id == abstract.resource_role_id &&
          expanded.interface_name == abstract.interface_name) {
        result.push_back(expanded);
      }
    }
    if (result.size() == initial_size) {
      fail_retained_bundle("control-domain chain edge has no matching expanded interface");
    }
  }
  return result;
}

[[nodiscard]] const LogicalControllerTemplate& require_logical_controller(
    std::span<const LogicalControllerTemplate> controllers, const core::ControlDomainId& domain_id,
    const core::ControllerRoleId& role_id) {
  const auto found =
      std::find_if(controllers.begin(), controllers.end(), [&](const auto& controller) {
        return controller.control_domain_id == domain_id &&
               controller.controller_role_id == role_id;
      });
  if (found == controllers.end()) {
    fail_retained_bundle("control-domain route selected no exact logical controller");
  }
  return *found;
}

[[nodiscard]] CanonicalValue edge_ref_projection(const core::ControllerChainEdgeRefDTO& edge) {
  return CanonicalValue(CanonicalValue::Object{
      {"upstream_controller_role_id", CanonicalValue(edge.upstream_controller_role_id.value())},
      {"downstream_controller_role_id", CanonicalValue(edge.downstream_controller_role_id.value())},
  });
}

[[nodiscard]] CanonicalValue activation_set_projection(
    const core::ControllerActivationSetDTO& activation_set) {
  CanonicalValue::Array edges;
  edges.reserve(activation_set.active_chain_edges.size());
  for (const auto& edge : activation_set.active_chain_edges) {
    edges.push_back(edge_ref_projection(edge));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"activation_set_id", CanonicalValue(activation_set.activation_set_id.value())},
      {"active_controller_role_ids",
       strong_id_array(std::span(activation_set.active_controller_role_ids))},
      {"active_chain_edges", CanonicalValue(std::move(edges))},
  });
}

[[nodiscard]] CanonicalValue route_projection(const CompilerQualifiedCellIr& compiler_ir,
                                              const core::ControllerTopologyDTO& topology,
                                              const LogicalControlRouteTemplate& route) {
  CanonicalValue::Array path_edges;
  path_edges.reserve(route.path_edges.size());
  const auto& all_controllers = compiler_ir.applicability().controllers();
  for (const auto& edge_ref : route.path_edges) {
    const auto& edge = require_topology_edge(topology, edge_ref);
    const auto& upstream = require_logical_controller(all_controllers, route.control_domain_id,
                                                      edge.upstream_controller_role_id);
    const auto& downstream = require_logical_controller(all_controllers, route.control_domain_id,
                                                        edge.downstream_controller_role_id);
    const auto& downstream_projection = require_controller_binding_projection(downstream, route);
    const auto reference_claims = project_edge_claims(
        edge.reference_interfaces, downstream_projection.exported_reference_interfaces);
    const auto state_claims =
        project_edge_claims(edge.state_interfaces, downstream_projection.exported_state_interfaces);
    path_edges.emplace_back(CanonicalValue::Object{
        {"upstream_controller_role_id", CanonicalValue(edge.upstream_controller_role_id.value())},
        {"upstream_logical_controller_symbol", CanonicalValue(upstream.logical_controller_symbol)},
        {"downstream_controller_role_id",
         CanonicalValue(edge.downstream_controller_role_id.value())},
        {"downstream_logical_controller_symbol",
         CanonicalValue(downstream.logical_controller_symbol)},
        {"reference_interfaces", claims_projection(reference_claims)},
        {"state_interfaces", claims_projection(state_claims)},
    });
  }

  CanonicalValue::Array endpoints;
  endpoints.reserve(route.endpoint_templates.size());
  for (const auto& endpoint : route.endpoint_templates) {
    endpoints.emplace_back(CanonicalValue::Object{
        {"endpoint_role_id", CanonicalValue(endpoint.endpoint_role_id.value())},
        {"logical_endpoint_symbol", CanonicalValue(endpoint.logical_endpoint_symbol)},
        {"generated_endpoint_name", CanonicalValue(endpoint.logical_endpoint_symbol)},
        {"is_command_endpoint", CanonicalValue(endpoint.is_command_endpoint)},
        {"is_state_endpoint", CanonicalValue(endpoint.is_state_endpoint)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"control_binding_id", CanonicalValue(route.control_binding_id.value())},
      {"control_binding_revision", CanonicalValue(route.control_binding_revision.value())},
      {"control_route_id", CanonicalValue(route.control_route_id.value())},
      {"logical_route_symbol", CanonicalValue(route.logical_route_symbol)},
      {"controller_role_ids", strong_id_array(std::span(route.controller_role_ids))},
      {"logical_controller_symbols", text_array(std::span(route.logical_controller_symbols))},
      {"path_edges", CanonicalValue(std::move(path_edges))},
      {"compatible_activation_set_ids",
       strong_id_array(std::span(route.compatible_activation_set_ids))},
      {"endpoints", CanonicalValue(std::move(endpoints))},
  });
}

[[nodiscard]] CanonicalValue provider_contract_projection(
    const core::ProviderContractRefDTO& contract) {
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract_role_id",
       CanonicalValue(contract.provider_contract_role_id.value())},
      {"contract_schema_id", CanonicalValue(contract.contract_schema_id.value())},
      {"contract_schema_revision",
       CanonicalValue(contract.contract_schema_revision.value())},
  });
}

template <typename Strong>
[[nodiscard]] CanonicalValue optional_strong_projection(
    const std::optional<Strong>& value) {
  return value.has_value() ? CanonicalValue(value->value())
                           : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue optional_contract_projection(
    const std::optional<core::ProviderContractRefDTO>& contract) {
  return contract.has_value() ? provider_contract_projection(*contract)
                              : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue json_value_projection(
    const core::JsonValueDTO& value) {
  return std::visit(
      [](const auto& current) -> CanonicalValue {
        using Value = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t> ||
                      std::is_same_v<Value, bool> ||
                      std::is_same_v<Value, double> ||
                      std::is_same_v<Value, std::string>) {
          return CanonicalValue(current);
        } else if constexpr (std::is_same_v<Value,
                                            core::JsonValueDTO::Array>) {
          CanonicalValue::Array result;
          result.reserve(current.size());
          for (const auto& item : current) {
            result.push_back(json_value_projection(item));
          }
          return CanonicalValue(std::move(result));
        } else {
          CanonicalValue::Object result;
          result.reserve(current.size());
          for (const auto& [name, item] : current) {
            result.emplace_back(name, json_value_projection(item));
          }
          return CanonicalValue(std::move(result));
        }
      },
      value.value);
}

[[nodiscard]] CanonicalValue json_object_projection(
    const core::JsonObjectDTO& object) {
  return json_value_projection(core::JsonValueDTO{object});
}

[[nodiscard]] CanonicalValue retained_artifact_ref_projection(
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

[[nodiscard]] CanonicalValue commissioned_stop_interface_projection(
    const core::CommissionedStopInterfaceDTO& commissioned) {
  return CanonicalValue(CanonicalValue::Object{
      {"commissioned_stop_interface_id",
       CanonicalValue(commissioned.commissioned_stop_interface_id.value())},
      {"commissioned_stop_interface_revision",
       CanonicalValue(
           commissioned.commissioned_stop_interface_revision.value())},
      {"control_domain_id",
       CanonicalValue(commissioned.control_domain_id.value())},
      {"control_domain_revision",
       CanonicalValue(commissioned.control_domain_revision.value())},
      {"control_binding_id",
       CanonicalValue(commissioned.control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(commissioned.control_binding_revision.value())},
      {"control_route_id",
       CanonicalValue(commissioned.control_route_id.value())},
      {"provider_instance_id",
       CanonicalValue(commissioned.provider_instance_id.value())},
      {"provider_manifest_id",
       CanonicalValue(commissioned.provider_manifest_id.value())},
      {"provider_manifest_revision",
       CanonicalValue(commissioned.provider_manifest_revision.value())},
      {"registered_command_kind_id",
       CanonicalValue(commissioned.registered_command_kind_id.value())},
      {"provider_contract_ref",
       provider_contract_projection(commissioned.provider_contract_ref)},
      {"endpoint_role_id",
       CanonicalValue(commissioned.endpoint_role_id.value())},
      {"stop_action_class",
       CanonicalValue(
           std::string(core::to_string(commissioned.stop_action_class)))},
      {"safety_system_request_action_class",
       CanonicalValue(std::string(core::to_string(
           commissioned.safety_system_request_action_class)))},
      {"external_resource_alias",
       optional_strong_projection(commissioned.external_resource_alias)},
      {"external_resource_kind",
       commissioned.external_resource_kind.has_value()
           ? CanonicalValue(std::string(core::to_string(
                 *commissioned.external_resource_kind)))
           : CanonicalValue(nullptr)},
      {"site_overlay_value_ref",
       optional_strong_projection(commissioned.site_overlay_value_ref)},
      {"commissioning_evidence_artifact_ref",
       retained_artifact_ref_projection(
           commissioned.commissioning_evidence_artifact_ref)},
  });
}

[[nodiscard]] CanonicalValue stop_command_projection(
    const core::StopContainmentCommandDTO& command) {
  return CanonicalValue(CanonicalValue::Object{
      {"command_phase_id", CanonicalValue(command.command_phase_id.value())},
      {"command_kind",
       CanonicalValue(std::string(core::to_string(command.command_kind)))},
      {"provider_contract_ref",
       optional_contract_projection(command.provider_contract_ref)},
      {"registered_command_kind_id",
       optional_strong_projection(command.registered_command_kind_id)},
  });
}

[[nodiscard]] CanonicalValue stop_budget_projection(
    const core::StopDeadlineBudgetDTO& budget) {
  CanonicalValue::Array phases;
  phases.reserve(budget.phases.size());
  for (const auto& phase : budget.phases) {
    phases.emplace_back(CanonicalValue::Object{
        {"phase_id", CanonicalValue(phase.phase_id.value())},
        {"deadline_offset_ns",
         CanonicalValue(static_cast<double>(phase.deadline_offset_ns))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"outer_duration_ns",
       CanonicalValue(static_cast<double>(budget.outer_duration_ns))},
      {"kernel_completion_reserve_ns",
       CanonicalValue(static_cast<double>(budget.kernel_completion_reserve_ns))},
      {"phases", CanonicalValue(std::move(phases))},
  });
}

[[nodiscard]] CanonicalValue stop_evidence_projection(
    const core::StopEvidenceRequirementDTO& evidence) {
  std::vector<std::string> authority_classes;
  authority_classes.reserve(
      evidence.source_requirement.allowed_authority_classes.size());
  for (const auto authority_class :
       evidence.source_requirement.allowed_authority_classes) {
    authority_classes.emplace_back(core::to_string(authority_class));
  }
  std::sort(authority_classes.begin(), authority_classes.end());
  const CanonicalValue source(CanonicalValue::Object{
      {"source_kind",
       CanonicalValue(std::string(
           core::to_string(evidence.source_requirement.source_kind)))},
      {"freshness_budget_ns",
       CanonicalValue(static_cast<double>(
           evidence.source_requirement.freshness_budget_ns))},
      {"endpoint_role_id",
       optional_strong_projection(
           evidence.source_requirement.endpoint_role_id)},
      {"allowed_authority_classes",
       text_array(std::span(authority_classes))},
  });
  const CanonicalValue observation(CanonicalValue::Object{
      {"observation_kind",
       CanonicalValue(std::string(
           core::to_string(evidence.observation_semantics.observation_kind)))},
      {"minimum_dwell_ns",
       evidence.observation_semantics.minimum_dwell_ns.has_value()
           ? CanonicalValue(static_cast<double>(
                 *evidence.observation_semantics.minimum_dwell_ns))
           : CanonicalValue(nullptr)},
  });
  return CanonicalValue(CanonicalValue::Object{
      {"evidence_item_id", CanonicalValue(evidence.evidence_item_id.value())},
      {"phase_id", CanonicalValue(evidence.phase_id.value())},
      {"evidence_kind",
       CanonicalValue(std::string(core::to_string(evidence.evidence_kind)))},
      {"provider_contract_ref",
       optional_contract_projection(evidence.provider_contract_ref)},
      {"registered_evidence_kind_id",
       optional_strong_projection(evidence.registered_evidence_kind_id)},
      {"source_requirement", source},
      {"observation_semantics", observation},
  });
}

[[nodiscard]] CanonicalValue stop_scope_projection(
    const LogicalStopScopeTemplate& scope) {
  return std::visit(
      [](const auto& typed_scope) -> CanonicalValue {
        using Scope = std::decay_t<decltype(typed_scope)>;
        if constexpr (std::is_same_v<Scope,
                                     LogicalResourceStopScopeTemplate>) {
          return CanonicalValue(CanonicalValue::Object{
              {"stop_escalation_stage_id",
               CanonicalValue(
                   typed_scope.stop_escalation_stage_id.value())},
              {"scope_kind", CanonicalValue("resource")},
              {"activation_set_id",
               CanonicalValue(typed_scope.activation_set_id.value())},
              {"resource_ids",
               strong_id_array(std::span(typed_scope.resource_ids))},
          });
        } else if constexpr (
            std::is_same_v<Scope, LogicalControllerSetStopScopeTemplate>) {
          return CanonicalValue(CanonicalValue::Object{
              {"stop_escalation_stage_id",
               CanonicalValue(
                   typed_scope.stop_escalation_stage_id.value())},
              {"scope_kind", CanonicalValue("controller_set")},
              {"activation_set_id",
               CanonicalValue(typed_scope.activation_set_id.value())},
              {"active_controller_role_ids",
               strong_id_array(
                   std::span(typed_scope.active_controller_role_ids))},
              {"logical_controller_symbols",
               text_array(
                   std::span(typed_scope.logical_controller_symbols))},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"stop_escalation_stage_id",
               CanonicalValue(
                   typed_scope.stop_escalation_stage_id.value())},
              {"scope_kind", CanonicalValue("control_domain")},
              {"activation_set_id",
               CanonicalValue(typed_scope.activation_set_id.value())},
              {"control_domain_id",
               CanonicalValue(typed_scope.control_domain_id.value())},
              {"control_domain_revision",
               CanonicalValue(typed_scope.control_domain_revision.value())},
          });
        }
      },
      scope);
}

[[nodiscard]] CanonicalValue stop_escalation_stage_projection(
    const LogicalStopEscalationStageTemplate& stage) {
  CanonicalValue::Array evidence;
  evidence.reserve(stage.all_of_evidence_requirements.size());
  for (const auto& requirement : stage.all_of_evidence_requirements) {
    evidence.push_back(stop_evidence_projection(requirement));
  }

  CanonicalValue provider_command(nullptr);
  if (stage.provider_command_endpoint.has_value()) {
    const auto& endpoint = *stage.provider_command_endpoint;
    provider_command = CanonicalValue(CanonicalValue::Object{
        {"stop_escalation_stage_id",
         CanonicalValue(endpoint.stop_escalation_stage_id.value())},
        {"provider_contract_ref",
         provider_contract_projection(endpoint.provider_contract_ref)},
        {"endpoint_role_id", CanonicalValue(endpoint.endpoint_role_id.value())},
        {"logical_endpoint_symbol",
         CanonicalValue(endpoint.logical_endpoint_symbol)},
    });
  }

  CanonicalValue::Array provider_evidence;
  provider_evidence.reserve(stage.provider_evidence_endpoints.size());
  for (const auto& endpoint : stage.provider_evidence_endpoints) {
    provider_evidence.emplace_back(CanonicalValue::Object{
        {"stop_escalation_stage_id",
         CanonicalValue(endpoint.stop_escalation_stage_id.value())},
        {"evidence_item_id",
         CanonicalValue(endpoint.evidence_item_id.value())},
        {"provider_contract_ref",
         provider_contract_projection(endpoint.provider_contract_ref)},
        {"endpoint_role_id", CanonicalValue(endpoint.endpoint_role_id.value())},
        {"logical_endpoint_symbol",
         CanonicalValue(endpoint.logical_endpoint_symbol)},
    });
  }

  CanonicalValue::Array binding_sources;
  binding_sources.reserve(stage.binding_state_authority_sources.size());
  for (const auto& source : stage.binding_state_authority_sources) {
    binding_sources.emplace_back(CanonicalValue::Object{
        {"stop_escalation_stage_id",
         CanonicalValue(source.stop_escalation_stage_id.value())},
        {"evidence_item_id", CanonicalValue(source.evidence_item_id.value())},
        {"state_authority_id",
         CanonicalValue(source.state_authority_id.value())},
        {"state_authority_class",
         CanonicalValue(
             std::string(core::to_string(source.state_authority_class)))},
        {"state_endpoint_role_id",
         CanonicalValue(source.state_endpoint_role_id.value())},
        {"logical_endpoint_symbol",
         CanonicalValue(source.logical_endpoint_symbol)},
    });
  }

  CanonicalValue::Array scopes;
  scopes.reserve(stage.containment_scopes.size());
  for (const auto& scope : stage.containment_scopes) {
    scopes.push_back(stop_scope_projection(scope));
  }

  return CanonicalValue(CanonicalValue::Object{
      {"stop_escalation_stage_id",
       CanonicalValue(stage.stop_escalation_stage_id.value())},
      {"stage_kind",
       CanonicalValue(std::string(core::to_string(stage.stage_kind)))},
      {"containment_granularity",
       CanonicalValue(
           std::string(core::to_string(stage.containment_granularity)))},
      {"containment_command",
       stop_command_projection(stage.containment_command)},
      {"evidence_conjunction",
       CanonicalValue(std::string(core::to_string(stage.evidence_conjunction)))},
      {"all_of_evidence_requirements", CanonicalValue(std::move(evidence))},
      {"default_contract_parameters",
       stage.default_contract_parameters.has_value()
           ? json_object_projection(*stage.default_contract_parameters)
           : CanonicalValue(nullptr)},
      {"commissioned_stop_interface",
       stage.commissioned_stop_interface.has_value()
           ? commissioned_stop_interface_projection(
                 *stage.commissioned_stop_interface)
           : CanonicalValue(nullptr)},
      {"provider_command_endpoint", std::move(provider_command)},
      {"provider_evidence_endpoints",
       CanonicalValue(std::move(provider_evidence))},
      {"binding_state_authority_sources",
       CanonicalValue(std::move(binding_sources))},
      {"containment_scopes", CanonicalValue(std::move(scopes))},
  });
}

template <typename Stop>
[[nodiscard]] CanonicalValue::Object stop_common_projection(const Stop& stop) {
  return CanonicalValue::Object{
      {"control_binding_id", CanonicalValue(stop.control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(stop.control_binding_revision.value())},
      {"control_route_id", CanonicalValue(stop.control_route_id.value())},
      {"stop_evidence_policy_id",
       CanonicalValue(stop.stop_evidence_policy_id.value())},
      {"stop_evidence_policy_revision",
       CanonicalValue(stop.stop_evidence_policy_revision.value())},
      {"control_domain_id", CanonicalValue(stop.control_domain_id.value())},
      {"control_domain_revision",
       CanonicalValue(stop.control_domain_revision.value())},
      {"controller_topology_id",
       CanonicalValue(stop.controller_topology_id.value())},
      {"topology_revision", CanonicalValue(stop.topology_revision.value())},
      {"containment_granularity",
       CanonicalValue(
           std::string(core::to_string(stop.containment_granularity)))},
      {"disposition",
       CanonicalValue(std::string(core::to_string(stop.disposition)))},
  };
}

[[nodiscard]] CanonicalValue stop_applicability_projection(
    const LogicalStopApplicabilityTemplate& value) {
  return std::visit(
      [](const auto& stop) -> CanonicalValue {
        using Stop = std::decay_t<decltype(stop)>;
        auto fields = stop_common_projection(stop);
        if constexpr (std::is_same_v<Stop,
                                     LogicalStoppableApplicabilityTemplate>) {
          CanonicalValue::Array stages;
          stages.reserve(stop.escalation_stages.size());
          for (const auto& stage : stop.escalation_stages) {
            stages.push_back(stop_escalation_stage_projection(stage));
          }
          fields.emplace_back("logical_stop_symbol",
                              CanonicalValue(stop.logical_stop_symbol));
          fields.emplace_back("deadline_budget",
                              stop_budget_projection(stop.deadline_budget));
          fields.emplace_back("escalation_stages",
                              CanonicalValue(std::move(stages)));
        }
        return CanonicalValue(std::move(fields));
      },
      value);
}

[[nodiscard]] std::string manager_name(const core::ControlDomainDTO& domain) {
  std::string preimage = "manyforge.control-domain-manager-name.v1";
  preimage.push_back('\0');
  preimage.append(domain.control_domain_id.value());
  preimage.push_back('\0');
  preimage.append(domain.control_domain_revision.value());
  preimage.push_back('\0');
  preimage.append(domain.controller_topology_id.value());
  preimage.push_back('\0');
  preimage.append(domain.topology_revision.value());
  const std::string digest = sha256_digest(preimage);
  return "mf1_controller_manager_" + digest.substr(7U, 24U);
}

[[nodiscard]] CanonicalValue source_conformance_projection(const CanonicalValue& bindings,
                                                           const CanonicalValue& controllers) {
  CanonicalValue root(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kSourceConformanceApiVersion))},
      {"source_conformance_digest", CanonicalValue(std::string(kZeroDigest))},
      {"bindings", bindings},
      {"controllers", controllers},
  });
  const std::string digest =
      compute_content_identity(root, ContentIdentitySpec{
                                         .identity_domain = "control-source-conformance-digest",
                                         .api_version = std::string(kSourceConformanceApiVersion),
                                         .own_field = "source_conformance_digest",
                                     });
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kSourceConformanceApiVersion))},
      {"source_conformance_digest", CanonicalValue(digest)},
  });
}

[[nodiscard]] CanonicalValue control_domain_projection(
    const CompilerQualifiedCellIr& compiler_ir,
    const core::ControlDomainId& control_domain_id) {
  const auto& domain = require_domain(compiler_ir, control_domain_id);
  const auto& desired = require_desired_configuration(compiler_ir, control_domain_id);
  const auto& topology = require_topology(compiler_ir, domain).value();
  const auto& graph = compiler_ir.applicability().composition().graph();
  CanonicalValue::Array bindings;
  for (const auto& binding : graph.control_bindings) {
    if (binding.control_domain_id != control_domain_id) {
      continue;
    }
    bindings.push_back(binding_projection(
        binding, require_binding_interface(compiler_ir, binding.control_binding_id,
                                           binding.control_binding_revision)));
  }
  if (bindings.empty()) {
    fail_retained_bundle("control-domain product requires at least one exact binding");
  }

  CanonicalValue::Array controllers;
  std::vector<const LogicalControllerTemplate*> domain_controllers;
  for (const auto& controller : compiler_ir.applicability().controllers()) {
    if (controller.control_domain_id == control_domain_id) {
      domain_controllers.push_back(&controller);
      controllers.push_back(controller_projection(compiler_ir, controller));
    }
  }
  if (domain_controllers.empty() || domain_controllers.size() > 2U) {
    fail_retained_bundle(
        "reference control profile admits one direct controller or one "
        "two-controller chain");
  }

  CanonicalValue::Array activation_sets;
  activation_sets.reserve(topology.activation_sets.size());
  for (const auto& activation_set : topology.activation_sets) {
    activation_sets.push_back(activation_set_projection(activation_set));
  }

  CanonicalValue::Array routes;
  for (const auto& route : compiler_ir.applicability().routes()) {
    if (route.control_domain_id == control_domain_id) {
      routes.push_back(route_projection(compiler_ir, topology, route));
    }
  }
  if (routes.empty()) {
    fail_retained_bundle("control-domain product requires at least one exact route");
  }

  std::vector<const LogicalStopApplicabilityTemplate*> domain_stops;
  for (const auto& stop : compiler_ir.applicability().stop_applicability()) {
    const bool belongs_to_domain = std::visit(
        [&](const auto& row) {
          return row.control_domain_id == control_domain_id;
        },
        stop);
    if (belongs_to_domain) {
      domain_stops.push_back(&stop);
    }
  }
  std::sort(domain_stops.begin(), domain_stops.end(), [](const auto* left,
                                                        const auto* right) {
    const auto key = [](const auto& value) {
      return std::visit(
          [](const auto& row) {
            return std::pair{row.control_binding_id.value(),
                             row.control_route_id.value()};
          },
          value);
    };
    return key(*left) < key(*right);
  });
  if (domain_stops.size() != routes.size()) {
    fail_retained_bundle(
        "control-domain product requires one exact Stop applicability row per route");
  }
  CanonicalValue::Array stop_applicability;
  std::vector<core::CommissionedStopInterfaceDTO>
      commissioned_stop_interfaces;
  stop_applicability.reserve(domain_stops.size());
  for (const auto* stop : domain_stops) {
    stop_applicability.push_back(stop_applicability_projection(*stop));
    std::visit(
        [&](const auto& row) {
          using Stop = std::decay_t<decltype(row)>;
          if constexpr (std::is_same_v<
                            Stop, LogicalStoppableApplicabilityTemplate>) {
            for (const auto& stage : row.escalation_stages) {
              if (stage.commissioned_stop_interface.has_value()) {
                commissioned_stop_interfaces.push_back(
                    *stage.commissioned_stop_interface);
              }
            }
          }
        },
        *stop);
  }
  std::sort(commissioned_stop_interfaces.begin(),
            commissioned_stop_interfaces.end(),
            [](const auto& left, const auto& right) {
              return left.commissioned_stop_interface_id <
                     right.commissioned_stop_interface_id;
            });
  if (std::adjacent_find(
          commissioned_stop_interfaces.begin(),
          commissioned_stop_interfaces.end(),
          [](const auto& left, const auto& right) {
            return left.commissioned_stop_interface_id ==
                   right.commissioned_stop_interface_id;
          }) != commissioned_stop_interfaces.end()) {
    fail_retained_bundle(
        "control-domain product repeats a commissioned Stop interface");
  }

  CanonicalValue::Array commissioned_rows;
  commissioned_rows.reserve(commissioned_stop_interfaces.size());
  for (const auto& commissioned : commissioned_stop_interfaces) {
    commissioned_rows.push_back(
        commissioned_stop_interface_projection(commissioned));
  }

  CanonicalValue bindings_value(std::move(bindings));
  CanonicalValue controllers_value(std::move(controllers));
  CanonicalValue source_conformance =
      source_conformance_projection(bindings_value, controllers_value);

  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kControlDomainControlApiVersion))},
      {"control_domain_id", CanonicalValue(domain.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(domain.control_domain_revision.value())},
      {"controller_topology_id", CanonicalValue(domain.controller_topology_id.value())},
      {"topology_revision", CanonicalValue(domain.topology_revision.value())},
      {"control_coordinator_endpoint_role",
       CanonicalValue(domain.control_coordinator_endpoint_role.value())},
      {"containment_granularity",
       CanonicalValue(std::string(core::to_string(domain.containment_granularity)))},
      {"implementation_profile",
       CanonicalValue(CanonicalValue::Object{
           {"implementation_profile_id", CanonicalValue(std::string(kImplementationProfileId))},
           {"implementation_profile_revision", CanonicalValue(implementation_profile_revision())},
           {"generated_controller_manager_name", CanonicalValue(manager_name(domain))},
       })},
      {"desired_configuration", desired_configuration_projection(desired)},
      {"bindings", bindings_value},
      {"controllers", controllers_value},
      {"activation_sets", CanonicalValue(std::move(activation_sets))},
      {"routes", CanonicalValue(std::move(routes))},
      {"stop_applicability", CanonicalValue(std::move(stop_applicability))},
      {"commissioned_stop_interfaces",
       CanonicalValue(std::move(commissioned_rows))},
      {"source_conformance", std::move(source_conformance)},
  });
}

}  // namespace

CanonicalValue stop_policy_applicability_projection(
    const core::StopEvidencePolicyDTO& policy) {
  CanonicalValue::Object fields{
      {"disposition",
       CanonicalValue(std::string(core::to_string(policy.disposition)))},
      {"containment_granularity",
       CanonicalValue(
           std::string(core::to_string(policy.containment_granularity)))},
  };
  if (policy.disposition == core::StopEvidenceDisposition::kNotStoppable) {
    return CanonicalValue(std::move(fields));
  }
  if (!policy.deadline_budget.has_value() || policy.escalation_stages.empty()) {
    fail_retained_bundle(
        "qualified stoppable policy lost its complete declarative semantics");
  }
  CanonicalValue::Array stages;
  stages.reserve(policy.escalation_stages.size());
  for (const auto& stage : policy.escalation_stages) {
    CanonicalValue::Array evidence;
    evidence.reserve(stage.evidence_requirements.size());
    for (const auto& requirement : stage.evidence_requirements) {
      evidence.push_back(stop_evidence_projection(requirement));
    }
    stages.emplace_back(CanonicalValue::Object{
        {"stop_escalation_stage_id",
         CanonicalValue(stage.stop_escalation_stage_id.value())},
        {"stage_kind",
         CanonicalValue(std::string(core::to_string(stage.stage_kind)))},
        {"containment_granularity",
         CanonicalValue(
             std::string(core::to_string(stage.containment_granularity)))},
        {"containment_command",
         stop_command_projection(stage.containment_command)},
        {"evidence_conjunction",
         CanonicalValue(
             std::string(core::to_string(stage.evidence_conjunction)))},
        {"all_of_evidence_requirements", CanonicalValue(std::move(evidence))},
    });
  }
  fields.emplace_back("deadline_budget",
                      stop_budget_projection(*policy.deadline_budget));
  fields.emplace_back("escalation_stages", CanonicalValue(std::move(stages)));
  return CanonicalValue(std::move(fields));
}

std::string emit_control_domain_control(
    const CompilerQualifiedCellIr& compiler_ir,
    const core::ControlDomainId& control_domain_id) {
  const std::string bytes =
      canonical_json(control_domain_projection(compiler_ir, control_domain_id));
  const CanonicalValue parsed = parse_and_verify_control_domain_control(bytes);
  if (canonical_json(parsed) != bytes) {
    throw std::logic_error("control-domain control emitter produced noncanonical bytes");
  }
  return bytes;
}

void require_control_domain_control_matches(std::string_view canonical_bytes,
                                            const CompilerQualifiedCellIr& compiler_ir,
                                            const core::ControlDomainId& control_domain_id) {
  const CanonicalValue parsed = parse_and_verify_control_domain_control(canonical_bytes);
  if (parsed != control_domain_projection(compiler_ir, control_domain_id)) {
    fail_retained_bundle(
        "control-domain control artifact differs from the exact compiler "
        "projection");
  }
}

}  // namespace manyforge::deployment::detail
