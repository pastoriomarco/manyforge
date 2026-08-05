#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/component_type_codec.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/controller_topology_codec.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

template <typename Values>
[[nodiscard]] py::tuple immutable_snapshot(const Values& values) {
  py::tuple snapshot{values.size()};
  for (std::size_t index = 0U; index < values.size(); ++index) {
    snapshot[index] =
        py::cast(values[index], py::return_value_policy::copy);
  }
  return snapshot;
}

template <typename Value, typename Error>
[[nodiscard]] std::vector<Value> bounded_values_from_python(
    const py::iterable& values, std::size_t maximum,
    std::string_view argument_name, std::string_view expected_type) {
  std::vector<Value> bounded;
  for (const auto& item : values) {
    if (bounded.size() == maximum) {
      throw Error(std::string(argument_name) +
                  " exceeds its public compiler-ingress bound");
    }
    if (!py::isinstance<Value>(item)) {
      throw py::type_error(std::string(argument_name) +
                           " must contain only " +
                           std::string(expected_type) + " values");
    }
    bounded.push_back(item.cast<const Value&>());
  }
  return bounded;
}

template <typename Projection>
[[nodiscard]] std::optional<std::string>
normalized_site_overlay_ref_json(const Projection& projection) {
  std::optional<std::string> result;
  {
    py::gil_scoped_release release;
    if (projection.normalized_site_overlay_ref().has_value()) {
      result = deployment::canonical_json(
          *projection.normalized_site_overlay_ref());
    }
  }
  return result;
}

[[nodiscard]] CapabilityEndpointSourceIdDTO
capability_endpoint_source_id_from_python(const py::handle& value) {
  if (py::isinstance<RuntimeUnitId>(value)) {
    return value.cast<RuntimeUnitId>();
  }
  if (py::isinstance<ExternalResourceAlias>(value)) {
    return value.cast<ExternalResourceAlias>();
  }
  throw py::type_error(
      "source_id must be RuntimeUnitId or ExternalResourceAlias");
}

[[nodiscard]] CapabilityProviderTargetDTO
capability_provider_target_from_python(const py::handle& value) {
  if (py::isinstance<CellGlobalCapabilityProviderTargetDTO>(value)) {
    return value.cast<CellGlobalCapabilityProviderTargetDTO>();
  }
  if (py::isinstance<TargetBindingCapabilityProviderTargetDTO>(value)) {
    return value.cast<TargetBindingCapabilityProviderTargetDTO>();
  }
  if (py::isinstance<ControlDomainCapabilityProviderTargetDTO>(value)) {
    return value.cast<ControlDomainCapabilityProviderTargetDTO>();
  }
  throw py::type_error(
      "target must be CellGlobalCapabilityProviderTargetDTO, "
      "TargetBindingCapabilityProviderTargetDTO, or "
      "ControlDomainCapabilityProviderTargetDTO");
}

}  // namespace

void register_deployment_candidate_bindings(py::module_& module) {
  using deployment::IntrinsicallyValidatedCellGraphCandidate;
  using deployment::GraphIdentityReadyCellGraphCandidate;
  using deployment::GraphRevisionVerifiedCellGraph;
  using deployment::ParsedArtifactCandidate;
  using deployment::TypedCellGraphCandidate;
  using deployment::UnqualifiedSchemaNormalizedArtifactCandidate;

  py::register_exception<deployment::ArtifactCodecError>(module, "ArtifactCodecError",
                                                         PyExc_ValueError);
  py::register_exception<deployment::CellGraphCodecError>(module, "CellGraphCodecError",
                                                          PyExc_ValueError);
  py::register_exception<deployment::ClosedSchemaError>(module, "ClosedSchemaError",
                                                        PyExc_ValueError);
  py::register_exception<deployment::CapabilityDefinitionCodecError>(
      module, "CapabilityDefinitionCodecError", PyExc_ValueError);
  py::register_exception<deployment::ControlInterfaceContractCodecError>(
      module, "ControlInterfaceContractCodecError", PyExc_ValueError);
  py::register_exception<deployment::ControllerTopologyCodecError>(
      module, "ControllerTopologyCodecError", PyExc_ValueError);
  py::register_exception<deployment::ComponentTypeCodecError>(
      module, "ComponentTypeCodecError", PyExc_ValueError);
  py::register_exception<deployment::ModelAssetCodecError>(
      module, "ModelAssetCodecError", PyExc_ValueError);
  py::register_exception<deployment::ProviderManifestCodecError>(
      module, "ProviderManifestCodecError", PyExc_ValueError);
  py::register_exception<deployment::RuntimeProfileCodecError>(
      module, "RuntimeProfileCodecError", PyExc_ValueError);
  py::register_exception<deployment::StopEvidencePolicyCodecError>(
      module, "StopEvidencePolicyCodecError", PyExc_ValueError);
  py::register_exception<deployment::DeploymentApplicabilityCodecError>(
      module, "DeploymentApplicabilityCodecError", PyExc_ValueError);
  py::register_exception<deployment::AssistantSettingsCodecError>(
      module, "AssistantSettingsCodecError", PyExc_ValueError);
  py::register_exception<deployment::DeploymentArtifactCodecError>(
      module, "DeploymentArtifactCodecError", PyExc_ValueError);
  py::register_exception<deployment::SiteOverlayResolutionError>(
      module, "SiteOverlayResolutionError", PyExc_ValueError);

  py::enum_<CapabilityEndpointSourceKind>(module,
                                          "CapabilityEndpointSourceKind")
      .value("kRuntimeUnit", CapabilityEndpointSourceKind::kRuntimeUnit)
      .value("kExternalResource",
             CapabilityEndpointSourceKind::kExternalResource);
  py::enum_<ExternalResourceKind>(module, "ExternalResourceKind")
      .value("kModelEndpoint", ExternalResourceKind::kModelEndpoint)
      .value("kContentStore", ExternalResourceKind::kContentStore)
      .value("kDataSource", ExternalResourceKind::kDataSource)
      .value("kServiceEndpoint", ExternalResourceKind::kServiceEndpoint);
  py::enum_<deployment::SiteOverlayRefSourcePresence>(
      module, "SiteOverlayRefSourcePresence")
      .value("kAbsent",
             deployment::SiteOverlayRefSourcePresence::kAbsent)
      .value("kExplicitNull",
             deployment::SiteOverlayRefSourcePresence::kExplicitNull)
      .value("kValue", deployment::SiteOverlayRefSourcePresence::kValue);

  py::class_<deployment::TypedAssistantSettingsCandidate>(
      module, "TypedAssistantSettingsCandidate", py::is_final())
      .def_property_readonly(
          "source_was_present",
          &deployment::TypedAssistantSettingsCandidate::source_was_present)
      .def_property_readonly(
          "enabled", &deployment::TypedAssistantSettingsCandidate::enabled)
      .def_property_readonly(
          "normalized_source_canonical_json",
          [](const deployment::TypedAssistantSettingsCandidate& candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(candidate.normalized_source());
          });

  py::class_<deployment::BuildQualifiedAssistantInstrumentRegistry>(
      module, "BuildQualifiedAssistantInstrumentRegistry", py::is_final())
      .def_property_readonly(
          "registry_digest",
          [](const deployment::BuildQualifiedAssistantInstrumentRegistry&
                 registry) { return registry.value().registry_digest; })
      .def_property_readonly(
          "normalized_projection_canonical_json",
          [](const deployment::BuildQualifiedAssistantInstrumentRegistry&
                 registry) {
            py::gil_scoped_release release;
            return deployment::canonical_json(registry.normalized_root());
          });

  py::class_<deployment::QualifiedAssistantSettings>(
      module, "QualifiedAssistantSettings", py::is_final())
      .def_property_readonly(
          "enabled", &deployment::QualifiedAssistantSettings::enabled)
      .def_property_readonly(
          "projection_digest",
          [](const deployment::QualifiedAssistantSettings& settings) {
            return settings.projection_digest();
          })
      .def_property_readonly(
          "instrument_registry_digest",
          [](const deployment::QualifiedAssistantSettings& settings) {
            return settings.instruments().registry_digest;
          })
      .def_property_readonly(
          "normalized_projection_canonical_json",
          [](const deployment::QualifiedAssistantSettings& settings) {
            py::gil_scoped_release release;
            return deployment::canonical_json(
                settings.normalized_projection());
          });

  py::class_<DesiredControlBindingRouteDTO>(
      module, "DesiredControlBindingRouteDTO", py::is_final())
      .def(py::init([](ControlBindingId control_binding_id,
                       ControlBindingRevision control_binding_revision,
                       ControlRouteId control_route_id) {
             return DesiredControlBindingRouteDTO{
                 std::move(control_binding_id),
                 std::move(control_binding_revision),
                 std::move(control_route_id)};
           }),
           py::arg("control_binding_id"),
           py::arg("control_binding_revision"),
           py::arg("control_route_id"))
      .def_readonly("control_binding_id",
                    &DesiredControlBindingRouteDTO::control_binding_id)
      .def_readonly(
          "control_binding_revision",
          &DesiredControlBindingRouteDTO::control_binding_revision)
      .def_readonly("control_route_id",
                    &DesiredControlBindingRouteDTO::control_route_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DesiredControlConfigurationDTO>(
      module, "DesiredControlConfigurationDTO", py::is_final())
      .def(py::init([](
               ControlDomainId control_domain_id,
               ControlDomainRevision control_domain_revision,
               ControllerTopologyId controller_topology_id,
               TopologyRevision topology_revision,
               ActivationSetId activation_set_id,
               const py::iterable& binding_routes) {
             auto bounded_routes = bounded_values_from_python<
                 DesiredControlBindingRouteDTO,
                 deployment::DeploymentApplicabilityCodecError>(
                 binding_routes, kMaxDeploymentApplicabilityRows,
                 "binding_routes", "DesiredControlBindingRouteDTO");
             return DesiredControlConfigurationDTO{
                 std::move(control_domain_id),
                 std::move(control_domain_revision),
                 std::move(controller_topology_id),
                 std::move(topology_revision),
                 std::move(activation_set_id), std::move(bounded_routes)};
           }),
           py::arg("control_domain_id"),
           py::arg("control_domain_revision"),
           py::arg("controller_topology_id"),
           py::arg("topology_revision"), py::arg("activation_set_id"),
           py::arg("binding_routes"))
      .def_readonly("control_domain_id",
                    &DesiredControlConfigurationDTO::control_domain_id)
      .def_readonly(
          "control_domain_revision",
          &DesiredControlConfigurationDTO::control_domain_revision)
      .def_readonly(
          "controller_topology_id",
          &DesiredControlConfigurationDTO::controller_topology_id)
      .def_readonly("topology_revision",
                    &DesiredControlConfigurationDTO::topology_revision)
      .def_readonly("activation_set_id",
                    &DesiredControlConfigurationDTO::activation_set_id)
      .def_readonly("binding_routes",
                    &DesiredControlConfigurationDTO::binding_routes)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<CellGlobalCapabilityProviderTargetDTO>(
      module, "CellGlobalCapabilityProviderTargetDTO", py::is_final())
      .def(py::init<CellId>(), py::arg("cell_id"))
      .def_readonly("cell_id",
                    &CellGlobalCapabilityProviderTargetDTO::cell_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<TargetBindingCapabilityProviderTargetDTO>(
      module, "TargetBindingCapabilityProviderTargetDTO", py::is_final())
      .def(py::init([](
               ControllableComponentId controllable_component_id,
               ComponentTypeRevision component_type_revision,
               CommandTargetId command_target_id,
               ControlBindingId control_binding_id,
               ControlBindingRevision control_binding_revision) {
             return TargetBindingCapabilityProviderTargetDTO{
                 std::move(controllable_component_id),
                 std::move(component_type_revision),
                 std::move(command_target_id),
                 std::move(control_binding_id),
                 std::move(control_binding_revision)};
           }),
           py::arg("controllable_component_id"),
           py::arg("component_type_revision"),
           py::arg("command_target_id"), py::arg("control_binding_id"),
           py::arg("control_binding_revision"))
      .def_readonly(
          "controllable_component_id",
          &TargetBindingCapabilityProviderTargetDTO::
              controllable_component_id)
      .def_readonly(
          "component_type_revision",
          &TargetBindingCapabilityProviderTargetDTO::component_type_revision)
      .def_readonly(
          "command_target_id",
          &TargetBindingCapabilityProviderTargetDTO::command_target_id)
      .def_readonly(
          "control_binding_id",
          &TargetBindingCapabilityProviderTargetDTO::control_binding_id)
      .def_readonly(
          "control_binding_revision",
          &TargetBindingCapabilityProviderTargetDTO::
              control_binding_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ControlDomainCapabilityProviderTargetDTO>(
      module, "ControlDomainCapabilityProviderTargetDTO", py::is_final())
      .def(py::init([](
               ControlDomainId control_domain_id,
               ControlDomainRevision control_domain_revision,
               ControllerTopologyId controller_topology_id,
               TopologyRevision topology_revision) {
             return ControlDomainCapabilityProviderTargetDTO{
                 std::move(control_domain_id),
                 std::move(control_domain_revision),
                 std::move(controller_topology_id),
                 std::move(topology_revision)};
           }),
           py::arg("control_domain_id"),
           py::arg("control_domain_revision"),
           py::arg("controller_topology_id"),
           py::arg("topology_revision"))
      .def_readonly(
          "control_domain_id",
          &ControlDomainCapabilityProviderTargetDTO::control_domain_id)
      .def_readonly(
          "control_domain_revision",
          &ControlDomainCapabilityProviderTargetDTO::control_domain_revision)
      .def_readonly(
          "controller_topology_id",
          &ControlDomainCapabilityProviderTargetDTO::controller_topology_id)
      .def_readonly(
          "topology_revision",
          &ControlDomainCapabilityProviderTargetDTO::topology_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::tuple capability_provider_target_types{3U};
  capability_provider_target_types[0] =
      module.attr("CellGlobalCapabilityProviderTargetDTO");
  capability_provider_target_types[1] =
      module.attr("TargetBindingCapabilityProviderTargetDTO");
  capability_provider_target_types[2] =
      module.attr("ControlDomainCapabilityProviderTargetDTO");
  module.attr("CapabilityProviderTargetDTO") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(capability_provider_target_types);

  py::tuple capability_endpoint_source_id_types{2U};
  capability_endpoint_source_id_types[0] = module.attr("RuntimeUnitId");
  capability_endpoint_source_id_types[1] =
      module.attr("ExternalResourceAlias");
  module.attr("CapabilityEndpointSourceIdDTO") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(capability_endpoint_source_id_types);

  py::class_<CapabilityProviderEndpointBindingDTO>(
      module, "CapabilityProviderEndpointBindingDTO", py::is_final())
      .def(py::init([](
               EndpointRoleId endpoint_role_id,
               CapabilityEndpointSourceKind source_kind,
               const py::object& source_id,
               EndpointRoleId source_endpoint_role_id) {
             return CapabilityProviderEndpointBindingDTO{
                 std::move(endpoint_role_id), source_kind,
                 capability_endpoint_source_id_from_python(source_id),
                 std::move(source_endpoint_role_id)};
           }),
           py::arg("endpoint_role_id"), py::arg("source_kind"),
           py::arg("source_id"), py::arg("source_endpoint_role_id"))
      .def_readonly(
          "endpoint_role_id",
          &CapabilityProviderEndpointBindingDTO::endpoint_role_id)
      .def_readonly("source_kind",
                    &CapabilityProviderEndpointBindingDTO::source_kind)
      .def_readonly("source_id",
                    &CapabilityProviderEndpointBindingDTO::source_id)
      .def_readonly(
          "source_endpoint_role_id",
          &CapabilityProviderEndpointBindingDTO::source_endpoint_role_id)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<CapabilityProviderBindingDTO>(
      module, "CapabilityProviderBindingDTO", py::is_final())
      .def(py::init([](
               CapabilityProviderBindingId capability_provider_binding_id,
               CapabilityProviderBindingRevision
                   capability_provider_binding_revision,
               CapabilityId capability_id,
               CapabilityDefinitionRevision capability_definition_revision,
               CapabilityScope scope, const py::object& target,
               ProviderInstanceId provider_instance_id,
               ProviderManifestId provider_manifest_id,
               ProviderManifestRevision provider_manifest_revision,
               const py::iterable& endpoint_bindings) {
             auto bounded_endpoints = bounded_values_from_python<
                 CapabilityProviderEndpointBindingDTO,
                 deployment::DeploymentApplicabilityCodecError>(
                 endpoint_bindings, kMaxCapabilityProviderEndpointBindings,
                 "endpoint_bindings",
                 "CapabilityProviderEndpointBindingDTO");
             return CapabilityProviderBindingDTO{
                 std::move(capability_provider_binding_id),
                 std::move(capability_provider_binding_revision),
                 std::move(capability_id),
                 std::move(capability_definition_revision), scope,
                 capability_provider_target_from_python(target),
                 std::move(provider_instance_id),
                 std::move(provider_manifest_id),
                 std::move(provider_manifest_revision),
                 std::move(bounded_endpoints)};
           }),
           py::arg("capability_provider_binding_id"),
           py::arg("capability_provider_binding_revision"),
           py::arg("capability_id"),
           py::arg("capability_definition_revision"), py::arg("scope"),
           py::arg("target"), py::arg("provider_instance_id"),
           py::arg("provider_manifest_id"),
           py::arg("provider_manifest_revision"),
           py::arg("endpoint_bindings"))
      .def_readonly(
          "capability_provider_binding_id",
          &CapabilityProviderBindingDTO::capability_provider_binding_id)
      .def_readonly(
          "capability_provider_binding_revision",
          &CapabilityProviderBindingDTO::
              capability_provider_binding_revision)
      .def_readonly("capability_id",
                    &CapabilityProviderBindingDTO::capability_id)
      .def_readonly(
          "capability_definition_revision",
          &CapabilityProviderBindingDTO::capability_definition_revision)
      .def_readonly("scope", &CapabilityProviderBindingDTO::scope)
      .def_readonly("target", &CapabilityProviderBindingDTO::target)
      .def_readonly("provider_instance_id",
                    &CapabilityProviderBindingDTO::provider_instance_id)
      .def_readonly("provider_manifest_id",
                    &CapabilityProviderBindingDTO::provider_manifest_id)
      .def_readonly(
          "provider_manifest_revision",
          &CapabilityProviderBindingDTO::provider_manifest_revision)
      .def_readonly("endpoint_bindings",
                    &CapabilityProviderBindingDTO::endpoint_bindings)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<SiteOverlayRefDTO>(module, "SiteOverlayRefDTO",
                                py::is_final())
      .def(py::init<SiteOverlayId, SiteOverlayRevision, ContentDigest>(),
           py::arg("site_overlay_id"), py::arg("site_overlay_revision"),
           py::arg("non_secret_digest"))
      .def_readonly("site_overlay_id", &SiteOverlayRefDTO::site_overlay_id)
      .def_readonly("site_overlay_revision",
                    &SiteOverlayRefDTO::site_overlay_revision)
      .def_readonly("non_secret_digest",
                    &SiteOverlayRefDTO::non_secret_digest)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<SiteOverlayExternalResourceKeyDTO>(
      module, "SiteOverlayExternalResourceKeyDTO", py::is_final())
      .def(py::init<SiteOverlayValueRef, ExternalResourceKind>(),
           py::arg("site_overlay_value_ref"), py::arg("resource_kind"))
      .def_readonly(
          "site_overlay_value_ref",
          &SiteOverlayExternalResourceKeyDTO::site_overlay_value_ref)
      .def_readonly("resource_kind",
                    &SiteOverlayExternalResourceKeyDTO::resource_kind)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<ExternalResourceDTO>(module, "ExternalResourceDTO",
                                  py::is_final())
      .def(py::init([](
               ExternalResourceAlias external_resource_alias,
               ExternalResourceKind resource_kind,
               ProviderInstanceId provider_instance_id,
               ProviderManifestId provider_manifest_id,
               ProviderManifestRevision provider_manifest_revision,
               EndpointRoleId endpoint_role_id,
               SiteOverlayValueRef site_overlay_value_ref) {
             return ExternalResourceDTO{
                 std::move(external_resource_alias), resource_kind,
                 std::move(provider_instance_id),
                 std::move(provider_manifest_id),
                 std::move(provider_manifest_revision),
                 std::move(endpoint_role_id),
                 std::move(site_overlay_value_ref)};
           }),
           py::arg("external_resource_alias"), py::arg("resource_kind"),
           py::arg("provider_instance_id"),
           py::arg("provider_manifest_id"),
           py::arg("provider_manifest_revision"),
           py::arg("endpoint_role_id"),
           py::arg("site_overlay_value_ref"))
      .def_readonly("external_resource_alias",
                    &ExternalResourceDTO::external_resource_alias)
      .def_readonly("resource_kind", &ExternalResourceDTO::resource_kind)
      .def_readonly("provider_instance_id",
                    &ExternalResourceDTO::provider_instance_id)
      .def_readonly("provider_manifest_id",
                    &ExternalResourceDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision",
                    &ExternalResourceDTO::provider_manifest_revision)
      .def_readonly("endpoint_role_id",
                    &ExternalResourceDTO::endpoint_role_id)
      .def_readonly("site_overlay_value_ref",
                    &ExternalResourceDTO::site_overlay_value_ref)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<DeploymentApplicabilityInputsDTO>(
      module, "DeploymentApplicabilityInputsDTO", py::is_final())
      .def(py::init([](
               std::optional<SiteOverlayRefDTO> site_overlay_ref,
               const py::iterable& desired_control_configurations,
               const py::iterable& capability_provider_bindings,
               const py::iterable& external_resources) {
             auto bounded_configurations = bounded_values_from_python<
                 DesiredControlConfigurationDTO,
                 deployment::DeploymentApplicabilityCodecError>(
                 desired_control_configurations,
                 kMaxDeploymentApplicabilityRows,
                 "desired_control_configurations",
                 "DesiredControlConfigurationDTO");
             auto bounded_providers = bounded_values_from_python<
                 CapabilityProviderBindingDTO,
                 deployment::DeploymentApplicabilityCodecError>(
                 capability_provider_bindings,
                 kMaxDeploymentApplicabilityRows,
                 "capability_provider_bindings",
                 "CapabilityProviderBindingDTO");
             auto bounded_resources = bounded_values_from_python<
                 ExternalResourceDTO,
                 deployment::DeploymentApplicabilityCodecError>(
                 external_resources, kMaxDeploymentApplicabilityRows,
                 "external_resources", "ExternalResourceDTO");
             return DeploymentApplicabilityInputsDTO{
                 std::move(site_overlay_ref),
                 std::move(bounded_configurations),
                 std::move(bounded_providers),
                 std::move(bounded_resources)};
           }),
           py::arg("site_overlay_ref"),
           py::arg("desired_control_configurations"),
           py::arg("capability_provider_bindings"),
           py::arg("external_resources"))
      .def_readonly("site_overlay_ref",
                    &DeploymentApplicabilityInputsDTO::site_overlay_ref)
      .def_readonly(
          "desired_control_configurations",
          &DeploymentApplicabilityInputsDTO::desired_control_configurations)
      .def_readonly(
          "capability_provider_bindings",
          &DeploymentApplicabilityInputsDTO::capability_provider_bindings)
      .def_readonly("external_resources",
                    &DeploymentApplicabilityInputsDTO::external_resources)
      .def(py::self == py::self)
      .def(py::self != py::self);

  py::class_<deployment::TypedDeploymentApplicabilityInputsCandidate>(
      module, "TypedDeploymentApplicabilityInputsCandidate", py::is_final())
      .def_property_readonly(
          "value",
          [](const deployment::TypedDeploymentApplicabilityInputsCandidate&
                 candidate) { return candidate.value(); })
      .def_property_readonly(
          "normalized_desired_control_configurations_canonical_json",
          [](const deployment::TypedDeploymentApplicabilityInputsCandidate&
                 candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(
                candidate.normalized_desired_control_configurations());
          })
      .def_property_readonly(
          "normalized_capability_provider_bindings_canonical_json",
          [](const deployment::TypedDeploymentApplicabilityInputsCandidate&
                 candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(
                candidate.normalized_capability_provider_bindings());
          })
      .def_property_readonly(
          "normalized_external_resources_canonical_json",
          [](const deployment::TypedDeploymentApplicabilityInputsCandidate&
                 candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(
                candidate.normalized_external_resources());
          })
      .def_property_readonly(
          "site_overlay_ref_presence",
          &deployment::TypedDeploymentApplicabilityInputsCandidate::
              site_overlay_ref_presence)
      .def_property_readonly(
          "normalized_site_overlay_ref_canonical_json",
          [](const deployment::TypedDeploymentApplicabilityInputsCandidate&
                 candidate) {
            return normalized_site_overlay_ref_json(candidate);
          })
      .def_property_readonly(
          "external_resources_was_present",
          &deployment::TypedDeploymentApplicabilityInputsCandidate::
              external_resources_was_present)
      .def_property_readonly(
          "assistant_settings",
          [](const deployment::TypedDeploymentApplicabilityInputsCandidate&
                 candidate) { return candidate.assistant_settings(); });

  py::class_<deployment::RevisionVerifiedDeploymentApplicabilityInputs>(
      module, "RevisionVerifiedDeploymentApplicabilityInputs",
      py::is_final())
      .def_property_readonly(
          "value",
          [](const deployment::RevisionVerifiedDeploymentApplicabilityInputs&
                 inputs) { return inputs.value(); })
      .def_property_readonly(
          "normalized_desired_control_configurations_canonical_json",
          [](const deployment::RevisionVerifiedDeploymentApplicabilityInputs&
                 inputs) {
            py::gil_scoped_release release;
            return deployment::canonical_json(
                inputs.normalized_desired_control_configurations());
          })
      .def_property_readonly(
          "normalized_capability_provider_bindings_canonical_json",
          [](const deployment::RevisionVerifiedDeploymentApplicabilityInputs&
                 inputs) {
            py::gil_scoped_release release;
            return deployment::canonical_json(
                inputs.normalized_capability_provider_bindings());
          })
      .def_property_readonly(
          "normalized_external_resources_canonical_json",
          [](const deployment::RevisionVerifiedDeploymentApplicabilityInputs&
                 inputs) {
            py::gil_scoped_release release;
            return deployment::canonical_json(
                inputs.normalized_external_resources());
          })
      .def_property_readonly(
          "site_overlay_ref_presence",
          &deployment::RevisionVerifiedDeploymentApplicabilityInputs::
              site_overlay_ref_presence)
      .def_property_readonly(
          "normalized_site_overlay_ref_canonical_json",
          [](const deployment::RevisionVerifiedDeploymentApplicabilityInputs&
                 inputs) {
            return normalized_site_overlay_ref_json(inputs);
          })
      .def_property_readonly(
          "external_resources_was_present",
          &deployment::RevisionVerifiedDeploymentApplicabilityInputs::
              external_resources_was_present)
      .def_property_readonly(
          "assistant_settings",
          [](const deployment::RevisionVerifiedDeploymentApplicabilityInputs&
                 inputs) { return inputs.assistant_settings(); });

  py::class_<deployment::VerifiedSiteOverlayArtifactProjection>(
      module, "VerifiedSiteOverlayArtifactProjection", py::is_final())
      .def_property_readonly(
          "reference",
          [](const deployment::VerifiedSiteOverlayArtifactProjection&
                 projection) { return projection.reference(); })
      .def_property_readonly(
          "external_resource_keys",
          [](const deployment::VerifiedSiteOverlayArtifactProjection&
                 projection) {
            return immutable_snapshot(projection.external_resource_keys());
          });

  py::class_<deployment::QualifiedSelectedSiteOverlay>(
      module, "QualifiedSelectedSiteOverlay", py::is_final())
      .def_property_readonly(
          "has_selection",
          &deployment::QualifiedSelectedSiteOverlay::has_selection)
      .def_property_readonly(
          "reference",
          [](const deployment::QualifiedSelectedSiteOverlay& selection)
              -> std::optional<SiteOverlayRefDTO> {
            const auto* reference = selection.reference();
            if (reference == nullptr) {
              return std::nullopt;
            }
            return *reference;
          })
      .def_property_readonly(
          "external_resource_keys",
          [](const deployment::QualifiedSelectedSiteOverlay& selection) {
            return immutable_snapshot(selection.external_resource_keys());
          });

  py::class_<deployment::RevisionVerifiedDeploymentArtifact>(
      module, "RevisionVerifiedDeploymentArtifact", py::is_final())
      .def_property_readonly(
          "deployment_artifact_id",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            return artifact.deployment_artifact_id();
          })
      .def_property_readonly(
          "deployment_revision",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            return artifact.deployment_revision();
          })
      .def_property_readonly(
          "cell_id",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            return artifact.cell_id();
          })
      .def_property_readonly(
          "name",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            return artifact.name();
          })
      .def_property_readonly(
          "description",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            return artifact.description();
          })
      .def_property_readonly(
          "catalog_records",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            return immutable_snapshot(artifact.catalog_records());
          })
      .def_property_readonly(
          "graph",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            return artifact.graph();
          })
      .def_property_readonly(
          "applicability_inputs",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            return artifact.applicability_inputs();
          })
      .def_property_readonly(
          "assistant_was_present",
          &deployment::RevisionVerifiedDeploymentArtifact::
              assistant_was_present)
      .def_property_readonly(
          "normalized_authored_canonical_json",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            py::gil_scoped_release release;
            return deployment::canonical_json(
                artifact.normalized_authored_root());
          })
      .def_property_readonly(
          "normalized_authored_graph_canonical_json",
          [](const deployment::RevisionVerifiedDeploymentArtifact& artifact) {
            py::gil_scoped_release release;
            return deployment::canonical_json(
                artifact.normalized_authored_graph());
          });

  module.def(
      "build_qualified_assistant_instrument_registry",
      &deployment::build_qualified_assistant_instrument_registry,
      py::return_value_policy::reference);

  module.def(
      "qualify_assistant_settings",
      [](const deployment::TypedAssistantSettingsCandidate& candidate,
         const py::iterable& external_resources) {
        auto bounded_resources = bounded_values_from_python<
            ExternalResourceDTO,
            deployment::AssistantSettingsCodecError>(
            external_resources, kMaxDeploymentApplicabilityRows,
            "external_resources", "ExternalResourceDTO");
        py::gil_scoped_release release;
        return deployment::qualify_assistant_settings(
            candidate,
            std::span<const ExternalResourceDTO>{bounded_resources});
      },
      py::arg("candidate"), py::arg("external_resources"));

  py::class_<ParsedArtifactCandidate>(module, "ParsedArtifactCandidate", py::is_final())
      .def_property_readonly("canonical_json", [](const ParsedArtifactCandidate& candidate) {
        py::gil_scoped_release release;
        return deployment::canonical_json(candidate.root());
      });

  py::class_<TypedCellGraphCandidate>(module, "TypedCellGraphCandidate", py::is_final())
      .def_property_readonly("graph",
                             [](const TypedCellGraphCandidate& candidate) {
                               py::gil_scoped_release release;
                               return candidate.graph();
                             })
      .def_property_readonly(
          "structurally_normalized_canonical_json", [](const TypedCellGraphCandidate& candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(candidate.structurally_normalized_root());
          });

  py::class_<IntrinsicallyValidatedCellGraphCandidate>(
      module, "IntrinsicallyValidatedCellGraphCandidate", py::is_final())
      .def_property_readonly("graph",
                             [](const IntrinsicallyValidatedCellGraphCandidate& candidate) {
                               py::gil_scoped_release release;
                               return candidate.graph();
                             })
      .def_property_readonly(
          "structurally_normalized_canonical_json",
          [](const IntrinsicallyValidatedCellGraphCandidate& candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(candidate.structurally_normalized_root());
          });

  py::class_<GraphIdentityReadyCellGraphCandidate>(
      module, "GraphIdentityReadyCellGraphCandidate", py::is_final())
      .def_property_readonly(
          "graph",
          [](const GraphIdentityReadyCellGraphCandidate& candidate) {
            py::gil_scoped_release release;
            return candidate.graph();
          })
      .def_property_readonly(
          "normalized_canonical_json",
          [](const GraphIdentityReadyCellGraphCandidate& candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(candidate.normalized_root());
          });

  py::class_<GraphRevisionVerifiedCellGraph>(
      module, "GraphRevisionVerifiedCellGraph", py::is_final())
      .def_property_readonly(
          "graph",
          [](const GraphRevisionVerifiedCellGraph& graph) {
            py::gil_scoped_release release;
            return graph.graph();
          })
      .def_property_readonly(
          "normalized_canonical_json",
          [](const GraphRevisionVerifiedCellGraph& graph) {
            py::gil_scoped_release release;
            return deployment::canonical_json(graph.normalized_root());
          });

  py::enum_<deployment::ClosedSchemaPurpose>(module, "ClosedSchemaPurpose")
      .value("kArtifactDocument", deployment::ClosedSchemaPurpose::kArtifactDocument)
      .value("kValue", deployment::ClosedSchemaPurpose::kValue);

  py::enum_<deployment::ClosedSchemaRootShape>(module, "ClosedSchemaRootShape")
      .value("kScalar", deployment::ClosedSchemaRootShape::kScalar)
      .value("kObject", deployment::ClosedSchemaRootShape::kObject)
      .value("kArray", deployment::ClosedSchemaRootShape::kArray)
      .value("kTaggedUnion", deployment::ClosedSchemaRootShape::kTaggedUnion);

  py::enum_<deployment::CatalogSchemaQualification>(module, "CatalogSchemaQualification")
      .value("kUnqualified", deployment::CatalogSchemaQualification::kUnqualified)
      .value("kQualified", deployment::CatalogSchemaQualification::kQualified);

  py::class_<deployment::CompiledClosedSchema>(module, "CompiledClosedSchema", py::is_final())
      .def_property_readonly("schema_id", &deployment::CompiledClosedSchema::schema_id)
      .def_property_readonly("schema_revision",
                             &deployment::CompiledClosedSchema::schema_revision)
      .def_property_readonly("schema_purpose",
                             &deployment::CompiledClosedSchema::schema_purpose)
      .def_property_readonly("root_shape", &deployment::CompiledClosedSchema::root_shape)
      .def_property_readonly(
          "normalized_descriptor_canonical_json",
          [](const deployment::CompiledClosedSchema& schema) {
            py::gil_scoped_release release;
            return deployment::canonical_json(schema.normalized_descriptor());
          });

  py::class_<UnqualifiedSchemaNormalizedArtifactCandidate>(
      module, "UnqualifiedSchemaNormalizedArtifactCandidate", py::is_final())
      .def_property_readonly(
          "schema_id", &UnqualifiedSchemaNormalizedArtifactCandidate::schema_id)
      .def_property_readonly(
          "schema_revision",
          &UnqualifiedSchemaNormalizedArtifactCandidate::schema_revision)
      .def_property_readonly(
          "schema_purpose",
          &UnqualifiedSchemaNormalizedArtifactCandidate::schema_purpose)
      .def_property_readonly("normalized_canonical_json",
                             [](const UnqualifiedSchemaNormalizedArtifactCandidate&
                                    artifact) {
                               py::gil_scoped_release release;
                               return deployment::canonical_json(artifact.normalized_root());
                             });

  py::class_<deployment::BuildCatalogSchemaEntry>(module, "BuildCatalogSchemaEntry",
                                                   py::is_final())
      .def_property_readonly("record_kind",
                             &deployment::BuildCatalogSchemaEntry::record_kind)
      .def_property_readonly("schema_id", &deployment::BuildCatalogSchemaEntry::schema_id)
      .def_property_readonly("qualification",
                             &deployment::BuildCatalogSchemaEntry::qualification)
      .def_property_readonly(
          "relative_path",
          [](const deployment::BuildCatalogSchemaEntry& entry) {
            return std::string(entry.relative_path());
          })
      .def_property_readonly(
          "compiled_schema",
          [](const deployment::BuildCatalogSchemaEntry& entry) -> py::object {
            const auto* schema = entry.compiled_schema();
            if (schema == nullptr) {
              return py::none();
            }
            return py::cast(schema, py::return_value_policy::reference);
          });

  py::class_<deployment::BuildQualifiedValueSchemaEntry>(
      module, "BuildQualifiedValueSchemaEntry", py::is_final())
      .def_property_readonly("schema_id",
                             &deployment::BuildQualifiedValueSchemaEntry::schema_id)
      .def_property_readonly("schema_revision",
                             &deployment::BuildQualifiedValueSchemaEntry::schema_revision)
      .def_property_readonly(
          "relative_path",
          [](const deployment::BuildQualifiedValueSchemaEntry& entry) {
            return std::string(entry.relative_path());
          })
      .def_property_readonly(
          "compiled_schema",
          [](const deployment::BuildQualifiedValueSchemaEntry& entry) {
            return &entry.compiled_schema();
          },
          py::return_value_policy::reference);

  py::class_<deployment::BuildQualifiedClosedSchemaRegistry>(
      module, "BuildQualifiedClosedSchemaRegistry", py::is_final())
      .def_property_readonly(
          "catalog_schemas",
          [](const deployment::BuildQualifiedClosedSchemaRegistry& registry) {
            const auto entries = registry.catalog_schemas();
            py::tuple snapshot{entries.size()};
            for (std::size_t index = 0; index < entries.size(); ++index) {
              snapshot[index] =
                  py::cast(&entries[index], py::return_value_policy::reference);
            }
            return snapshot;
          })
      .def_property_readonly(
          "value_schemas",
          [](const deployment::BuildQualifiedClosedSchemaRegistry& registry) {
            const auto entries = registry.value_schemas();
            py::tuple snapshot{entries.size()};
            for (std::size_t index = 0; index < entries.size(); ++index) {
              snapshot[index] =
                  py::cast(&entries[index], py::return_value_policy::reference);
            }
            return snapshot;
          })
      .def_property_readonly(
          "normalized_projection_canonical_json",
          [](const deployment::BuildQualifiedClosedSchemaRegistry& registry) {
            py::gil_scoped_release release;
            return deployment::canonical_json(registry.normalized_projection());
          })
      .def_property_readonly(
          "qualification_registry_digest",
          &deployment::BuildQualifiedClosedSchemaRegistry::
              qualification_registry_digest);

  py::class_<deployment::QualifiedCapabilityDefinition>(
      module, "QualifiedCapabilityDefinition", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedCapabilityDefinition::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedCapabilityDefinition& definition) {
            py::gil_scoped_release release;
            return deployment::canonical_json(definition.normalized_root());
          });

  py::class_<deployment::QualifiedControlInterfaceContract>(
      module, "QualifiedControlInterfaceContract", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedControlInterfaceContract::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedControlInterfaceContract& contract) {
            py::gil_scoped_release release;
            return deployment::canonical_json(contract.normalized_root());
          });

  py::class_<deployment::QualifiedControllerTopology>(
      module, "QualifiedControllerTopology", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedControllerTopology::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedControllerTopology& topology) {
            py::gil_scoped_release release;
            return deployment::canonical_json(topology.normalized_root());
          });

  py::class_<deployment::QualifiedComponentType>(
      module, "QualifiedComponentType", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedComponentType::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedComponentType& type) {
            py::gil_scoped_release release;
            return deployment::canonical_json(type.normalized_root());
          });

  py::class_<deployment::QualifiedModelAsset>(
      module, "QualifiedModelAsset", py::is_final())
      .def_property_readonly("value",
                             &deployment::QualifiedModelAsset::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedModelAsset& asset) {
            py::gil_scoped_release release;
            return deployment::canonical_json(asset.normalized_root());
          });

  py::class_<deployment::QualifiedProviderManifest>(
      module, "QualifiedProviderManifest", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedProviderManifest::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedProviderManifest& manifest) {
            py::gil_scoped_release release;
            return deployment::canonical_json(manifest.normalized_root());
          });

  py::class_<deployment::QualifiedRuntimeProfile>(
      module, "QualifiedRuntimeProfile", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedRuntimeProfile::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedRuntimeProfile& profile) {
            py::gil_scoped_release release;
            return deployment::canonical_json(profile.normalized_root());
          });

  py::class_<deployment::QualifiedStopEvidencePolicy>(
      module, "QualifiedStopEvidencePolicy", py::is_final())
      .def_property_readonly(
          "value", &deployment::QualifiedStopEvidencePolicy::value)
      .def_property_readonly(
          "normalized_canonical_json",
          [](const deployment::QualifiedStopEvidencePolicy& policy) {
            py::gil_scoped_release release;
            return deployment::canonical_json(policy.normalized_root());
          });

  module.def(
      "parse_strict_json_model_yaml",
      [](const std::string& yaml_bytes) {
        return deployment::parse_strict_json_model_yaml(yaml_bytes);
      },
      py::arg("yaml_bytes"), py::call_guard<py::gil_scoped_release>());
  module.def("decode_cell_graph_candidate", &deployment::decode_cell_graph_candidate,
             py::arg("parsed"), py::call_guard<py::gil_scoped_release>());
  module.def("validate_intrinsic_cell_graph_candidate",
             &deployment::validate_intrinsic_cell_graph_candidate, py::arg("candidate"),
             py::call_guard<py::gil_scoped_release>());
  module.def(
      "prepare_cell_graph_identity",
      [](const IntrinsicallyValidatedCellGraphCandidate& candidate,
         const py::iterable& qualified_runtime_profiles) {
        std::set<std::pair<std::string, std::string>>
            selected_runtime_profiles;
        {
          py::gil_scoped_release release;
          for (const auto& unit : candidate.graph().runtime_units) {
            selected_runtime_profiles.emplace(
                unit.runtime_profile_id.value(),
                unit.runtime_profile_revision.value());
          }
        }
        const std::size_t maximum_closure_size =
            selected_runtime_profiles.size();
        std::set<std::pair<std::string, std::string>> seen_runtime_profiles;
        std::vector<deployment::QualifiedRuntimeProfile> bounded_profiles;
        bounded_profiles.reserve(maximum_closure_size);
        for (const auto& profile : qualified_runtime_profiles) {
          if (!py::isinstance<deployment::QualifiedRuntimeProfile>(profile)) {
            throw py::type_error(
                "qualified_runtime_profiles must contain only "
                "QualifiedRuntimeProfile values");
          }
          const auto& qualified_profile =
              profile.cast<const deployment::QualifiedRuntimeProfile&>();
          const auto key = std::pair{
              qualified_profile.value().runtime_profile_id.value(),
              qualified_profile.value().runtime_profile_revision.value()};
          if (!selected_runtime_profiles.contains(key)) {
            const bool selected_id = std::any_of(
                selected_runtime_profiles.begin(),
                selected_runtime_profiles.end(),
                [&key](const auto& selected) {
                  return selected.first == key.first;
                });
            throw deployment::CellGraphCodecError(
                std::string(selected_id
                                ? "stale runtime-profile revision "
                                : "unused extra runtime profile ") +
                "'" + key.first + "' at revision '" + key.second + "'");
          }
          if (!seen_runtime_profiles.insert(key).second) {
            throw deployment::CellGraphCodecError(
                "duplicate exact runtime profile '" + key.first +
                "' at revision '" + key.second + "'");
          }
          bounded_profiles.push_back(qualified_profile);
        }
        py::gil_scoped_release release;
        return deployment::prepare_cell_graph_identity(
            candidate,
            std::span<const deployment::QualifiedRuntimeProfile>{
                bounded_profiles});
      },
      py::arg("candidate"), py::arg("qualified_runtime_profiles"));
  module.def("compute_graph_revision", &deployment::compute_graph_revision,
             py::arg("candidate"),
             py::call_guard<py::gil_scoped_release>());
  module.def("verify_graph_revision", &deployment::verify_graph_revision,
             py::arg("candidate"),
             py::call_guard<py::gil_scoped_release>());
  module.def("compile_closed_schema_descriptor",
             &deployment::compile_closed_schema_descriptor, py::arg("candidate"),
             py::call_guard<py::gil_scoped_release>());
  module.def("validate_and_normalize_unqualified_artifact_candidate",
             &deployment::validate_and_normalize_unqualified_artifact_candidate,
             py::arg("schema"), py::arg("candidate"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_capability_definition",
             &deployment::resolve_and_qualify_capability_definition,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_control_interface_contract",
             &deployment::resolve_and_qualify_control_interface_contract,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def(
      "resolve_and_qualify_controller_topology",
      [](const core::ControllerTopologyCatalogRecordRefDTO& reference,
         const core::ContentAddressedStoreKey& resolved_store_key,
         const std::string& stored_bytes,
         const std::vector<deployment::QualifiedControlInterfaceContract>&
             qualified_contracts) {
        return deployment::resolve_and_qualify_controller_topology(
            reference, resolved_store_key, stored_bytes,
            std::span<const deployment::QualifiedControlInterfaceContract>{
                qualified_contracts});
      },
      py::arg("reference"), py::arg("resolved_store_key"),
      py::arg("stored_bytes"), py::arg("qualified_contracts"),
      py::call_guard<py::gil_scoped_release>());
  module.def(
      "resolve_and_qualify_component_type",
      [](const core::ComponentTypeCatalogRecordRefDTO& reference,
         const core::ContentAddressedStoreKey& resolved_store_key,
         const std::string& stored_bytes,
         const py::iterable& qualified_capabilities) {
        std::vector<deployment::QualifiedCapabilityDefinition>
            bounded_capabilities;
        bounded_capabilities.reserve(
            core::kMaxComponentTypeCapabilityDependencies);
        for (const auto& capability : qualified_capabilities) {
          if (bounded_capabilities.size() ==
              core::kMaxComponentTypeCapabilityDependencies) {
            throw py::value_error(
                "qualified_capabilities exceeds the ComponentType v1 "
                "dependency bound");
          }
          if (!py::isinstance<
                  deployment::QualifiedCapabilityDefinition>(
                  capability)) {
            throw py::type_error(
                "qualified_capabilities must contain only "
                "QualifiedCapabilityDefinition values");
          }
          bounded_capabilities.push_back(
              capability.cast<
                  deployment::QualifiedCapabilityDefinition>());
        }
        py::gil_scoped_release release;
        return deployment::resolve_and_qualify_component_type(
            reference, resolved_store_key, stored_bytes,
            std::span<const deployment::QualifiedCapabilityDefinition>{
                bounded_capabilities});
      },
      py::arg("reference"), py::arg("resolved_store_key"),
      py::arg("stored_bytes"), py::arg("qualified_capabilities"));
  module.def("resolve_and_qualify_model_asset",
             &deployment::resolve_and_qualify_model_asset,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_provider_manifest",
             &deployment::resolve_and_qualify_provider_manifest,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_runtime_profile",
             &deployment::resolve_and_qualify_runtime_profile,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def("resolve_and_qualify_stop_evidence_policy",
             &deployment::resolve_and_qualify_stop_evidence_policy,
             py::arg("reference"), py::arg("resolved_store_key"),
             py::arg("stored_bytes"),
             py::call_guard<py::gil_scoped_release>());
  module.def(
      "decode_deployment_applicability_inputs_candidate",
      &deployment::decode_deployment_applicability_inputs_candidate,
      py::arg("parsed_deployment"),
      py::call_guard<py::gil_scoped_release>());
  module.def(
      "compute_capability_provider_binding_revisions",
      &deployment::compute_capability_provider_binding_revisions,
      py::arg("candidate"), py::call_guard<py::gil_scoped_release>());
  module.def(
      "verify_capability_provider_binding_revisions",
      &deployment::verify_capability_provider_binding_revisions,
      py::arg("candidate"), py::call_guard<py::gil_scoped_release>());
  module.def(
      "resolve_and_verify_site_overlay_artifact_projection",
      [](const SiteOverlayRefDTO& expected_reference,
         const std::string& stored_bytes) {
        py::gil_scoped_release release;
        return deployment::resolve_and_verify_site_overlay_artifact_projection(
            expected_reference, stored_bytes);
      },
      py::arg("expected_reference"), py::arg("stored_bytes"));
  module.def(
      "select_site_overlay_for_deployment",
      [](const std::optional<SiteOverlayRefDTO>& deployment_reference,
         const py::iterable& external_resources,
         const py::object& selected_projection) {
        auto bounded_resources =
            bounded_values_from_python<ExternalResourceDTO,
                                       deployment::SiteOverlayResolutionError>(
                external_resources, kMaxDeploymentApplicabilityRows,
                "external_resources", "ExternalResourceDTO");
        std::optional<deployment::VerifiedSiteOverlayArtifactProjection>
            projection;
        if (!selected_projection.is_none()) {
          if (!py::isinstance<
                  deployment::VerifiedSiteOverlayArtifactProjection>(
                  selected_projection)) {
            throw py::type_error(
                "selected_projection must be None or a "
                "VerifiedSiteOverlayArtifactProjection");
          }
          projection.emplace(
              selected_projection
                  .cast<const deployment::
                            VerifiedSiteOverlayArtifactProjection&>());
        }
        py::gil_scoped_release release;
        return deployment::select_site_overlay_for_deployment(
            deployment_reference,
            std::span<const ExternalResourceDTO>{bounded_resources},
            projection.has_value() ? &*projection : nullptr);
      },
      py::arg("deployment_reference"), py::arg("external_resources"),
      py::arg("selected_projection"));
  module.def(
      "compute_deployment_artifact_revision",
      [](const ParsedArtifactCandidate& parsed_deployment,
         const py::iterable& qualified_runtime_profiles) {
        auto bounded_profiles =
            bounded_values_from_python<
                deployment::QualifiedRuntimeProfile,
                deployment::DeploymentArtifactCodecError>(
                qualified_runtime_profiles,
                deployment::kMaxArtifactNodeCount,
                "qualified_runtime_profiles", "QualifiedRuntimeProfile");
        py::gil_scoped_release release;
        return deployment::compute_deployment_artifact_revision(
            parsed_deployment,
            std::span<const deployment::QualifiedRuntimeProfile>{
                bounded_profiles});
      },
      py::arg("parsed_deployment"), py::arg("qualified_runtime_profiles"));
  module.def(
      "verify_deployment_artifact_revision",
      [](const ParsedArtifactCandidate& parsed_deployment,
         const py::iterable& qualified_runtime_profiles) {
        auto bounded_profiles =
            bounded_values_from_python<
                deployment::QualifiedRuntimeProfile,
                deployment::DeploymentArtifactCodecError>(
                qualified_runtime_profiles,
                deployment::kMaxArtifactNodeCount,
                "qualified_runtime_profiles", "QualifiedRuntimeProfile");
        py::gil_scoped_release release;
        return deployment::verify_deployment_artifact_revision(
            parsed_deployment,
            std::span<const deployment::QualifiedRuntimeProfile>{
                bounded_profiles});
      },
      py::arg("parsed_deployment"), py::arg("qualified_runtime_profiles"));
  module.def(
      "build_qualified_closed_schema_registry",
      []() -> const deployment::BuildQualifiedClosedSchemaRegistry& {
        return deployment::build_qualified_closed_schema_registry();
      },
      py::return_value_policy::reference);
  module.attr("CLOSED_SCHEMA_API_VERSION") =
      py::str(deployment::kClosedSchemaApiVersion);
  module.attr("CLOSED_SCHEMA_BOOTSTRAP_CONTRACT_DIGEST") =
      py::str(deployment::kClosedSchemaBootstrapContractDigest);
  module.attr("MAX_CAPABILITY_PROVIDER_ENDPOINT_BINDINGS") =
      py::int_(kMaxCapabilityProviderEndpointBindings);
  module.attr("MAX_DEPLOYMENT_APPLICABILITY_ROWS") =
      py::int_(kMaxDeploymentApplicabilityRows);
  module.attr("MAX_SITE_OVERLAY_VALUE_ROWS") =
      py::int_(kMaxSiteOverlayValueRows);
}

}  // namespace manyforge::core::python
