#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace manyforge::core {

/// One desired route in the complete authored configuration of a control
/// domain.
struct DesiredControlBindingRouteDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControlRouteId control_route_id;

  friend bool operator==(const DesiredControlBindingRouteDTO&,
                         const DesiredControlBindingRouteDTO&) = default;
};

/// Exact authored desired configuration for one control-domain revision.
///
/// This is desired configuration only. It does not assert that a controller
/// is installed, active, reconciled, or ready.
struct DesiredControlConfigurationDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ActivationSetId activation_set_id;
  std::vector<DesiredControlBindingRouteDTO> binding_routes;

  friend bool operator==(const DesiredControlConfigurationDTO&,
                         const DesiredControlConfigurationDTO&) = default;
};

/// Complete `cell_global` applicability target.
struct CellGlobalCapabilityProviderTargetDTO final {
  CellId cell_id;

  friend bool operator==(const CellGlobalCapabilityProviderTargetDTO&,
                         const CellGlobalCapabilityProviderTargetDTO&) =
      default;
};

/// Complete `target_binding` applicability target.
struct TargetBindingCapabilityProviderTargetDTO final {
  ControllableComponentId controllable_component_id;
  ComponentTypeRevision component_type_revision;
  CommandTargetId command_target_id;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;

  friend bool operator==(const TargetBindingCapabilityProviderTargetDTO&,
                         const TargetBindingCapabilityProviderTargetDTO&) =
      default;
};

/// Complete `control_domain` applicability target.
struct ControlDomainCapabilityProviderTargetDTO final {
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;

  friend bool operator==(const ControlDomainCapabilityProviderTargetDTO&,
                         const ControlDomainCapabilityProviderTargetDTO&) =
      default;
};

using CapabilityProviderTargetDTO =
    std::variant<CellGlobalCapabilityProviderTargetDTO,
                 TargetBindingCapabilityProviderTargetDTO,
                 ControlDomainCapabilityProviderTargetDTO>;

enum class CapabilityEndpointSourceKind : std::uint8_t {
  kRuntimeUnit = 0,
  kExternalResource,
};

[[nodiscard]] constexpr std::string_view to_string(
    CapabilityEndpointSourceKind value) noexcept {
  switch (value) {
    case CapabilityEndpointSourceKind::kRuntimeUnit:
      return "runtime_unit";
    case CapabilityEndpointSourceKind::kExternalResource:
      return "external_resource";
  }
  return {};
}

using CapabilityEndpointSourceIdDTO =
    std::variant<RuntimeUnitId, ExternalResourceAlias>;

/// One exact logical endpoint-role source for a capability-provider binding.
///
/// `source_kind` and `source_id` form one closed wire union. The strict
/// deployment codec proves that the enum and variant alternative agree before
/// it produces its revision-verified input wrapper.
struct CapabilityProviderEndpointBindingDTO final {
  EndpointRoleId endpoint_role_id;
  CapabilityEndpointSourceKind source_kind;
  CapabilityEndpointSourceIdDTO source_id;
  EndpointRoleId source_endpoint_role_id;

  friend bool operator==(const CapabilityProviderEndpointBindingDTO&,
                         const CapabilityProviderEndpointBindingDTO&) =
      default;
};

/// The reusable provider-manifest inventory has at most 256 endpoint roles.
/// A concrete binding must match one exact offered inventory and therefore
/// cannot have more rows.
inline constexpr std::size_t kMaxCapabilityProviderEndpointBindings = 256U;
inline constexpr std::size_t kMaxDeploymentApplicabilityRows = 4096U;
inline constexpr std::size_t kMaxSiteOverlayValueRows = 4096U;

/// Exact deployment-owned provider applicability record.
///
/// This value contains only logical roles and typed source references. It
/// carries no concrete endpoint, process/readiness assertion, credential, or
/// provider byte digest.
struct CapabilityProviderBindingDTO final {
  CapabilityProviderBindingId capability_provider_binding_id;
  CapabilityProviderBindingRevision capability_provider_binding_revision;
  CapabilityId capability_id;
  CapabilityDefinitionRevision capability_definition_revision;
  CapabilityScope scope;
  CapabilityProviderTargetDTO target;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  std::vector<CapabilityProviderEndpointBindingDTO> endpoint_bindings;

  friend bool operator==(const CapabilityProviderBindingDTO&,
                         const CapabilityProviderBindingDTO&) = default;
};

enum class ExternalResourceKind : std::uint8_t {
  kModelEndpoint = 0,
  kContentStore,
  kDataSource,
  kServiceEndpoint,
};

[[nodiscard]] constexpr std::string_view to_string(
    ExternalResourceKind value) noexcept {
  switch (value) {
    case ExternalResourceKind::kModelEndpoint:
      return "model_endpoint";
    case ExternalResourceKind::kContentStore:
      return "content_store";
    case ExternalResourceKind::kDataSource:
      return "data_source";
    case ExternalResourceKind::kServiceEndpoint:
      return "service_endpoint";
  }
  return {};
}

/// Exact value-free reference to one resolver-selected site overlay.
///
/// `non_secret_digest` covers the complete normalized standalone artifact,
/// including non-secret values and opaque secret-reference identities but
/// never resolved secret bytes. It is not itself a credential or endpoint.
struct SiteOverlayRefDTO final {
  SiteOverlayId site_overlay_id;
  SiteOverlayRevision site_overlay_revision;
  ContentDigest non_secret_digest;

  friend bool operator==(const SiteOverlayRefDTO&,
                         const SiteOverlayRefDTO&) = default;
};

/// One value-free site-overlay key that may satisfy an external-resource row.
///
/// Kind is part of the exact key: a reference projected for one resource kind
/// cannot be silently reused as another kind.
struct SiteOverlayExternalResourceKeyDTO final {
  SiteOverlayValueRef site_overlay_value_ref;
  ExternalResourceKind resource_kind;

  friend bool operator==(const SiteOverlayExternalResourceKeyDTO&,
                         const SiteOverlayExternalResourceKeyDTO&) = default;
};

/// One deployment-local external-resource declaration.
///
/// The overlay reference uses the portable v1 token grammar and is resolved
/// only as an exact value-free key against the selected overlay projection.
/// This DTO never contains the resolved value.
struct ExternalResourceDTO final {
  ExternalResourceAlias external_resource_alias;
  ExternalResourceKind resource_kind;
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  EndpointRoleId endpoint_role_id;
  SiteOverlayValueRef site_overlay_value_ref;

  friend bool operator==(const ExternalResourceDTO&,
                         const ExternalResourceDTO&) = default;
};

/// Grouping projection for the three deployment-level C0.2n input tables.
///
/// This is not another artifact envelope or graph kind. The deployment codec
/// retains the three independently normalized arrays used to rebuild the
/// complete deployment identity later.
struct DeploymentApplicabilityInputsDTO final {
  std::optional<SiteOverlayRefDTO> site_overlay_ref;
  std::vector<DesiredControlConfigurationDTO> desired_control_configurations;
  std::vector<CapabilityProviderBindingDTO> capability_provider_bindings;
  std::vector<ExternalResourceDTO> external_resources;

  friend bool operator==(const DeploymentApplicabilityInputsDTO&,
                         const DeploymentApplicabilityInputsDTO&) = default;
};

}  // namespace manyforge::core
