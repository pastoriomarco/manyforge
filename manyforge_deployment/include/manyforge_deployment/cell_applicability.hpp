#pragma once

#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/controller_topology_codec.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {

inline constexpr std::size_t
    kMaxExpandedControlInterfaceClaimsPerDirection = 65536U;
inline constexpr std::size_t
    kMaxCellExpandedControlInterfaceClaimsPerDirection = 1000000U;
inline constexpr std::size_t kMaxCellLogicalApplicabilityRows = 1000000U;

class CellApplicabilityError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

struct ExpandedControlInterfaceClaim final {
  core::ResourceRoleId resource_role_id;
  core::ResourceId resource_id;
  std::string interface_name;
  std::string logical_interface_symbol;

  friend bool operator==(const ExpandedControlInterfaceClaim&,
                         const ExpandedControlInterfaceClaim&) = default;
};

struct LogicalBindingInterfaceTemplate final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;
  core::ControlInterfaceContractId control_interface_contract_id;
  core::ControlInterfaceRevision control_interface_revision;
  std::vector<ExpandedControlInterfaceClaim> command_claims;
  std::vector<ExpandedControlInterfaceClaim> state_claims;

  friend bool operator==(const LogicalBindingInterfaceTemplate&,
                         const LogicalBindingInterfaceTemplate&) = default;
};

/// One binding-proven projection of a logical controller's abstract
/// interfaces onto concrete ResourceId values.
///
/// Keeping the contributing binding identity prevents one binding from
/// borrowing another binding's applicability evidence when controller roles
/// or resources are shared.
struct LogicalControllerBindingProjection final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;
  core::ControlRouteId control_route_id;
  core::ActivationSetId activation_set_id;
  std::vector<core::ControllerClaimDirection> claimed_directions;
  std::vector<ExpandedControlInterfaceClaim> exported_reference_interfaces;
  std::vector<ExpandedControlInterfaceClaim> exported_state_interfaces;

  friend bool operator==(const LogicalControllerBindingProjection&,
                         const LogicalControllerBindingProjection&) = default;
};

struct LogicalControllerTemplate final {
  core::ControlDomainId control_domain_id;
  core::ControlDomainRevision control_domain_revision;
  core::ControllerTopologyId controller_topology_id;
  core::TopologyRevision topology_revision;
  core::ControllerRoleId controller_role_id;
  std::string logical_controller_symbol;
  std::vector<LogicalControllerBindingProjection> binding_projections;

  friend bool operator==(const LogicalControllerTemplate&,
                         const LogicalControllerTemplate&) = default;
};

struct LogicalRouteEndpointTemplate final {
  core::EndpointRoleId endpoint_role_id;
  std::string logical_endpoint_symbol;
  bool is_command_endpoint = false;
  bool is_state_endpoint = false;

  friend bool operator==(const LogicalRouteEndpointTemplate&,
                         const LogicalRouteEndpointTemplate&) = default;
};

struct LogicalControlRouteTemplate final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;
  core::ControlDomainId control_domain_id;
  core::ControlDomainRevision control_domain_revision;
  core::ControllerTopologyId controller_topology_id;
  core::TopologyRevision topology_revision;
  core::ControlRouteId control_route_id;
  std::string logical_route_symbol;
  std::vector<core::ControllerRoleId> controller_role_ids;
  std::vector<std::string> logical_controller_symbols;
  std::vector<core::ControllerChainEdgeRefDTO> path_edges;
  std::vector<core::ActivationSetId> compatible_activation_set_ids;
  std::vector<LogicalRouteEndpointTemplate> endpoint_templates;

  friend bool operator==(const LogicalControlRouteTemplate&,
                         const LogicalControlRouteTemplate&) = default;
};

struct ControlBindingCapabilityOwner final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;

  friend bool operator==(const ControlBindingCapabilityOwner&,
                         const ControlBindingCapabilityOwner&) = default;
};

struct CapabilityProviderCapabilityOwner final {
  core::CapabilityProviderBindingId capability_provider_binding_id;
  core::CapabilityProviderBindingRevision
      capability_provider_binding_revision;

  friend bool operator==(const CapabilityProviderCapabilityOwner&,
                         const CapabilityProviderCapabilityOwner&) = default;
};

using QualifiedCapabilityOwner =
    std::variant<ControlBindingCapabilityOwner,
                 CapabilityProviderCapabilityOwner>;

struct LogicalCapabilityTemplate final {
  QualifiedCapabilityOwner owner;
  core::CapabilityId capability_id;
  core::CapabilityDefinitionRevision capability_definition_revision;
  core::CapabilityScope scope;
  core::CapabilityProviderBindingKind provider_binding_kind;
  core::ProviderInstanceId provider_instance_id;
  core::ProviderManifestId provider_manifest_id;
  core::ProviderManifestRevision provider_manifest_revision;
  std::string logical_capability_symbol;

  friend bool operator==(const LogicalCapabilityTemplate&,
                         const LogicalCapabilityTemplate&) = default;
};

struct RuntimeEndpointSourceTemplate final {
  core::RuntimeUnitId runtime_unit_id;
  core::RuntimeProfileId runtime_profile_id;
  core::RuntimeProfileRevision runtime_profile_revision;
  core::EndpointRoleId source_endpoint_role_id;

  friend bool operator==(const RuntimeEndpointSourceTemplate&,
                         const RuntimeEndpointSourceTemplate&) = default;
};

struct ExternalEndpointSourceTemplate final {
  core::ExternalResourceAlias external_resource_alias;
  core::ExternalResourceKind resource_kind;
  core::ProviderInstanceId provider_instance_id;
  core::ProviderManifestId provider_manifest_id;
  core::ProviderManifestRevision provider_manifest_revision;
  core::EndpointRoleId endpoint_role_id;
  core::SiteOverlayValueRef site_overlay_value_ref;
  core::SiteOverlayId selected_site_overlay_id;
  core::SiteOverlayRevision selected_site_overlay_revision;
  core::ContentDigest selected_site_overlay_non_secret_digest;

  friend bool operator==(const ExternalEndpointSourceTemplate&,
                         const ExternalEndpointSourceTemplate&) = default;
};

using LogicalEndpointSourceTemplate =
    std::variant<RuntimeEndpointSourceTemplate,
                 ExternalEndpointSourceTemplate>;

struct LogicalProviderEndpointTemplate final {
  core::CapabilityProviderBindingId capability_provider_binding_id;
  core::CapabilityProviderBindingRevision
      capability_provider_binding_revision;
  core::EndpointRoleId endpoint_role_id;
  core::ProviderInstanceId provider_instance_id;
  core::ProviderManifestId provider_manifest_id;
  core::ProviderManifestRevision provider_manifest_revision;
  LogicalEndpointSourceTemplate source;
  std::string logical_endpoint_symbol;

  friend bool operator==(const LogicalProviderEndpointTemplate&,
                         const LogicalProviderEndpointTemplate&) = default;
};

struct LogicalExternalResourceTemplate final {
  core::ExternalResourceAlias external_resource_alias;
  core::ExternalResourceKind resource_kind;
  core::ProviderInstanceId provider_instance_id;
  core::ProviderManifestId provider_manifest_id;
  core::ProviderManifestRevision provider_manifest_revision;
  core::EndpointRoleId endpoint_role_id;
  core::SiteOverlayValueRef site_overlay_value_ref;
  core::SiteOverlayId selected_site_overlay_id;
  core::SiteOverlayRevision selected_site_overlay_revision;
  core::ContentDigest selected_site_overlay_non_secret_digest;
  std::string logical_external_resource_symbol;

  friend bool operator==(const LogicalExternalResourceTemplate&,
                         const LogicalExternalResourceTemplate&) = default;
};

struct LogicalStopProviderCommandEndpoint final {
  core::ProviderContractRefDTO provider_contract_ref;
  core::EndpointRoleId endpoint_role_id;
  std::string logical_endpoint_symbol;

  friend bool operator==(const LogicalStopProviderCommandEndpoint&,
                         const LogicalStopProviderCommandEndpoint&) = default;
};

struct LogicalStopProviderEvidenceEndpoint final {
  core::StopEvidenceItemId evidence_item_id;
  core::ProviderContractRefDTO provider_contract_ref;
  core::EndpointRoleId endpoint_role_id;
  std::string logical_endpoint_symbol;

  friend bool operator==(const LogicalStopProviderEvidenceEndpoint&,
                         const LogicalStopProviderEvidenceEndpoint&) = default;
};

struct LogicalStopBindingStateAuthoritySource final {
  core::StopEvidenceItemId evidence_item_id;
  core::StateAuthorityId state_authority_id;
  core::StateAuthorityClass state_authority_class;
  core::EndpointRoleId state_endpoint_role_id;
  std::string logical_endpoint_symbol;

  friend bool operator==(const LogicalStopBindingStateAuthoritySource&,
                         const LogicalStopBindingStateAuthoritySource&) =
      default;
};

struct LogicalResourceStopScopeTemplate final {
  core::ActivationSetId activation_set_id;
  std::vector<core::ResourceId> resource_ids;

  friend bool operator==(const LogicalResourceStopScopeTemplate&,
                         const LogicalResourceStopScopeTemplate&) = default;
};

struct LogicalControllerSetStopScopeTemplate final {
  core::ActivationSetId activation_set_id;
  std::vector<core::ControllerRoleId> active_controller_role_ids;
  std::vector<std::string> logical_controller_symbols;

  friend bool operator==(const LogicalControllerSetStopScopeTemplate&,
                         const LogicalControllerSetStopScopeTemplate&) =
      default;
};

struct LogicalControlDomainStopScopeTemplate final {
  core::ActivationSetId activation_set_id;
  core::ControlDomainId control_domain_id;
  core::ControlDomainRevision control_domain_revision;

  friend bool operator==(const LogicalControlDomainStopScopeTemplate&,
                         const LogicalControlDomainStopScopeTemplate&) =
      default;
};

using LogicalStopScopeTemplate =
    std::variant<LogicalResourceStopScopeTemplate,
                 LogicalControllerSetStopScopeTemplate,
                 LogicalControlDomainStopScopeTemplate>;

struct LogicalNotStoppableApplicabilityTemplate final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;
  core::ControlRouteId control_route_id;
  core::StopEvidencePolicyId stop_evidence_policy_id;
  core::StopEvidencePolicyRevision stop_evidence_policy_revision;
  core::ControlDomainId control_domain_id;
  core::ControlDomainRevision control_domain_revision;
  core::ControllerTopologyId controller_topology_id;
  core::TopologyRevision topology_revision;
  core::ContainmentGranularity containment_granularity;
  core::StopEvidenceDisposition disposition;

  friend bool operator==(
      const LogicalNotStoppableApplicabilityTemplate&,
      const LogicalNotStoppableApplicabilityTemplate&) = default;
};

struct LogicalStoppableApplicabilityTemplate final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;
  core::ControlRouteId control_route_id;
  core::StopEvidencePolicyId stop_evidence_policy_id;
  core::StopEvidencePolicyRevision stop_evidence_policy_revision;
  core::ControlDomainId control_domain_id;
  core::ControlDomainRevision control_domain_revision;
  core::ControllerTopologyId controller_topology_id;
  core::TopologyRevision topology_revision;
  core::ContainmentGranularity containment_granularity;
  core::StopEvidenceDisposition disposition;
  std::string logical_stop_symbol;
  core::StopContainmentCommandDTO containment_command;
  core::StopDeadlineBudgetDTO deadline_budget;
  core::StopEvidenceConjunction evidence_conjunction;
  std::vector<core::StopEvidenceRequirementDTO> all_of_evidence_requirements;
  std::optional<LogicalStopProviderCommandEndpoint>
      provider_command_endpoint;
  std::vector<LogicalStopProviderEvidenceEndpoint>
      provider_evidence_endpoints;
  std::vector<LogicalStopBindingStateAuthoritySource>
      binding_state_authority_sources;
  std::vector<LogicalStopScopeTemplate> containment_scopes;

  friend bool operator==(const LogicalStoppableApplicabilityTemplate&,
                         const LogicalStoppableApplicabilityTemplate&) =
      default;
};

using LogicalStopApplicabilityTemplate =
    std::variant<LogicalNotStoppableApplicabilityTemplate,
                 LogicalStoppableApplicabilityTemplate>;

/// Immutable, non-caller-constructible C0.2n result.
///
/// The value owns every exact semantic dependency and every derived logical
/// template needed by I3. It contains no executable implementation,
/// generated transport endpoint, process/readiness fact, credential, active
/// configuration, command, Stop effect, or runtime observation.
class ApplicabilityQualifiedCellIr final {
 public:
  ApplicabilityQualifiedCellIr(const ApplicabilityQualifiedCellIr&) = default;
  ApplicabilityQualifiedCellIr(ApplicabilityQualifiedCellIr&&) noexcept =
      default;
  ApplicabilityQualifiedCellIr& operator=(
      const ApplicabilityQualifiedCellIr&) = default;
  ApplicabilityQualifiedCellIr& operator=(
      ApplicabilityQualifiedCellIr&&) noexcept = default;

  [[nodiscard]] const CompositionQualifiedCellIr& composition()
      const noexcept {
    return composition_;
  }
  [[nodiscard]] const RevisionVerifiedDeploymentApplicabilityInputs&
  deployment_inputs() const noexcept {
    return deployment_inputs_;
  }
  [[nodiscard]] const std::optional<core::SiteOverlayRefDTO>&
  selected_site_overlay_ref() const noexcept {
    return selected_site_overlay_ref_;
  }
  [[nodiscard]] const std::vector<QualifiedRuntimeProfile>& runtime_profiles()
      const noexcept {
    return runtime_profiles_;
  }
  [[nodiscard]] const std::vector<QualifiedCapabilityDefinition>&
  capability_definitions() const noexcept {
    return capability_definitions_;
  }
  [[nodiscard]] const std::vector<QualifiedControlInterfaceContract>&
  control_interface_contracts() const noexcept {
    return control_interface_contracts_;
  }
  [[nodiscard]] const std::vector<QualifiedControllerTopology>&
  controller_topologies() const noexcept {
    return controller_topologies_;
  }
  [[nodiscard]] const std::vector<QualifiedProviderManifest>&
  provider_manifests() const noexcept {
    return provider_manifests_;
  }
  [[nodiscard]] const std::vector<QualifiedStopEvidencePolicy>&
  stop_evidence_policies() const noexcept {
    return stop_evidence_policies_;
  }
  [[nodiscard]] const std::vector<LogicalBindingInterfaceTemplate>&
  binding_interfaces() const noexcept {
    return binding_interfaces_;
  }
  [[nodiscard]] const std::vector<LogicalControllerTemplate>& controllers()
      const noexcept {
    return controllers_;
  }
  [[nodiscard]] const std::vector<LogicalControlRouteTemplate>& routes()
      const noexcept {
    return routes_;
  }
  [[nodiscard]] const std::vector<LogicalCapabilityTemplate>& capabilities()
      const noexcept {
    return capabilities_;
  }
  [[nodiscard]] const
      std::vector<LogicalProviderEndpointTemplate>&
      provider_endpoints() const noexcept {
    return provider_endpoints_;
  }
  [[nodiscard]] const std::vector<LogicalExternalResourceTemplate>&
  external_resources() const noexcept {
    return external_resources_;
  }
  [[nodiscard]] const std::vector<LogicalStopApplicabilityTemplate>&
  stop_applicability() const noexcept {
    return stop_applicability_;
  }

 private:
  ApplicabilityQualifiedCellIr(
      CompositionQualifiedCellIr composition,
      RevisionVerifiedDeploymentApplicabilityInputs deployment_inputs,
      std::optional<core::SiteOverlayRefDTO> selected_site_overlay_ref,
      std::vector<QualifiedRuntimeProfile> runtime_profiles,
      std::vector<QualifiedCapabilityDefinition> capability_definitions,
      std::vector<QualifiedControlInterfaceContract>
          control_interface_contracts,
      std::vector<QualifiedControllerTopology> controller_topologies,
      std::vector<QualifiedProviderManifest> provider_manifests,
      std::vector<QualifiedStopEvidencePolicy> stop_evidence_policies,
      std::vector<LogicalBindingInterfaceTemplate> binding_interfaces,
      std::vector<LogicalControllerTemplate> controllers,
      std::vector<LogicalControlRouteTemplate> routes,
      std::vector<LogicalCapabilityTemplate> capabilities,
      std::vector<LogicalProviderEndpointTemplate> provider_endpoints,
      std::vector<LogicalExternalResourceTemplate> external_resources,
      std::vector<LogicalStopApplicabilityTemplate> stop_applicability)
      : composition_(std::move(composition)),
        deployment_inputs_(std::move(deployment_inputs)),
        selected_site_overlay_ref_(std::move(selected_site_overlay_ref)),
        runtime_profiles_(std::move(runtime_profiles)),
        capability_definitions_(std::move(capability_definitions)),
        control_interface_contracts_(std::move(control_interface_contracts)),
        controller_topologies_(std::move(controller_topologies)),
        provider_manifests_(std::move(provider_manifests)),
        stop_evidence_policies_(std::move(stop_evidence_policies)),
        binding_interfaces_(std::move(binding_interfaces)),
        controllers_(std::move(controllers)),
        routes_(std::move(routes)),
        capabilities_(std::move(capabilities)),
        provider_endpoints_(std::move(provider_endpoints)),
        external_resources_(std::move(external_resources)),
        stop_applicability_(std::move(stop_applicability)) {}

  CompositionQualifiedCellIr composition_;
  RevisionVerifiedDeploymentApplicabilityInputs deployment_inputs_;
  std::optional<core::SiteOverlayRefDTO> selected_site_overlay_ref_;
  std::vector<QualifiedRuntimeProfile> runtime_profiles_;
  std::vector<QualifiedCapabilityDefinition> capability_definitions_;
  std::vector<QualifiedControlInterfaceContract>
      control_interface_contracts_;
  std::vector<QualifiedControllerTopology> controller_topologies_;
  std::vector<QualifiedProviderManifest> provider_manifests_;
  std::vector<QualifiedStopEvidencePolicy> stop_evidence_policies_;
  std::vector<LogicalBindingInterfaceTemplate> binding_interfaces_;
  std::vector<LogicalControllerTemplate> controllers_;
  std::vector<LogicalControlRouteTemplate> routes_;
  std::vector<LogicalCapabilityTemplate> capabilities_;
  std::vector<LogicalProviderEndpointTemplate> provider_endpoints_;
  std::vector<LogicalExternalResourceTemplate> external_resources_;
  std::vector<LogicalStopApplicabilityTemplate> stop_applicability_;

  friend ApplicabilityQualifiedCellIr qualify_cell_applicability(
      const CompositionQualifiedCellIr&,
      const RevisionVerifiedDeploymentApplicabilityInputs&,
      std::span<const QualifiedRuntimeProfile>,
      std::span<const QualifiedCapabilityDefinition>,
      std::span<const QualifiedControlInterfaceContract>,
      std::span<const QualifiedControllerTopology>,
      std::span<const QualifiedProviderManifest>,
      std::span<const QualifiedStopEvidencePolicy>,
      const QualifiedSelectedSiteOverlay&);
};

[[nodiscard]] ApplicabilityQualifiedCellIr qualify_cell_applicability(
    const CompositionQualifiedCellIr& composition,
    const RevisionVerifiedDeploymentApplicabilityInputs& deployment_inputs,
    std::span<const QualifiedRuntimeProfile> runtime_profiles,
    std::span<const QualifiedCapabilityDefinition> capability_definitions,
    std::span<const QualifiedControlInterfaceContract>
        control_interface_contracts,
    std::span<const QualifiedControllerTopology> controller_topologies,
    std::span<const QualifiedProviderManifest> provider_manifests,
    std::span<const QualifiedStopEvidencePolicy> stop_evidence_policies,
    const QualifiedSelectedSiteOverlay& selected_site_overlay);

}  // namespace manyforge::deployment
