#pragma once

#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_deployment/capability_definition_codec.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/closed_schema.hpp>
#include <manyforge_deployment/command_schema_codec.hpp>
#include <manyforge_deployment/contact_policy_codec.hpp>
#include <manyforge_deployment/control_interface_contract_codec.hpp>
#include <manyforge_deployment/controller_topology_codec.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/provider_manifest_codec.hpp>
#include <manyforge_deployment/runtime_policy_codec.hpp>
#include <manyforge_deployment/runtime_profile_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment {

inline constexpr std::size_t kMaxExpandedControlInterfaceClaimsPerDirection =
    65536U;
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

  friend bool operator==(const ExpandedControlInterfaceClaim &,
                         const ExpandedControlInterfaceClaim &) = default;
};

struct LogicalBindingInterfaceTemplate final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;
  core::ControlInterfaceContractId control_interface_contract_id;
  core::ControlInterfaceRevision control_interface_revision;
  std::vector<ExpandedControlInterfaceClaim> command_claims;
  std::vector<ExpandedControlInterfaceClaim> state_claims;

  friend bool operator==(const LogicalBindingInterfaceTemplate &,
                         const LogicalBindingInterfaceTemplate &) = default;
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

  friend bool operator==(const LogicalControllerBindingProjection &,
                         const LogicalControllerBindingProjection &) = default;
};

struct LogicalControllerTemplate final {
  core::ControlDomainId control_domain_id;
  core::ControlDomainRevision control_domain_revision;
  core::ControllerTopologyId controller_topology_id;
  core::TopologyRevision topology_revision;
  core::ControllerRoleId controller_role_id;
  std::string logical_controller_symbol;
  std::vector<LogicalControllerBindingProjection> binding_projections;

  friend bool operator==(const LogicalControllerTemplate &,
                         const LogicalControllerTemplate &) = default;
};

struct LogicalRouteEndpointTemplate final {
  core::EndpointRoleId endpoint_role_id;
  std::string logical_endpoint_symbol;
  bool is_command_endpoint = false;
  bool is_state_endpoint = false;

  friend bool operator==(const LogicalRouteEndpointTemplate &,
                         const LogicalRouteEndpointTemplate &) = default;
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

  friend bool operator==(const LogicalControlRouteTemplate &,
                         const LogicalControlRouteTemplate &) = default;
};

struct ControlBindingCapabilityOwner final {
  core::ControlBindingId control_binding_id;
  core::ControlBindingRevision control_binding_revision;

  friend bool operator==(const ControlBindingCapabilityOwner &,
                         const ControlBindingCapabilityOwner &) = default;
};

struct CapabilityProviderCapabilityOwner final {
  core::CapabilityProviderBindingId capability_provider_binding_id;
  core::CapabilityProviderBindingRevision capability_provider_binding_revision;

  friend bool operator==(const CapabilityProviderCapabilityOwner &,
                         const CapabilityProviderCapabilityOwner &) = default;
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

  friend bool operator==(const LogicalCapabilityTemplate &,
                         const LogicalCapabilityTemplate &) = default;
};

struct RuntimeEndpointSourceTemplate final {
  core::RuntimeUnitId runtime_unit_id;
  core::RuntimeProfileId runtime_profile_id;
  core::RuntimeProfileRevision runtime_profile_revision;
  core::EndpointRoleId source_endpoint_role_id;

  friend bool operator==(const RuntimeEndpointSourceTemplate &,
                         const RuntimeEndpointSourceTemplate &) = default;
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

  friend bool operator==(const ExternalEndpointSourceTemplate &,
                         const ExternalEndpointSourceTemplate &) = default;
};

using LogicalEndpointSourceTemplate =
    std::variant<RuntimeEndpointSourceTemplate, ExternalEndpointSourceTemplate>;

struct LogicalProviderEndpointTemplate final {
  core::CapabilityProviderBindingId capability_provider_binding_id;
  core::CapabilityProviderBindingRevision capability_provider_binding_revision;
  core::EndpointRoleId endpoint_role_id;
  core::ProviderInstanceId provider_instance_id;
  core::ProviderManifestId provider_manifest_id;
  core::ProviderManifestRevision provider_manifest_revision;
  LogicalEndpointSourceTemplate source;
  std::string logical_endpoint_symbol;

  friend bool operator==(const LogicalProviderEndpointTemplate &,
                         const LogicalProviderEndpointTemplate &) = default;
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

  friend bool operator==(const LogicalExternalResourceTemplate &,
                         const LogicalExternalResourceTemplate &) = default;
};

struct LogicalStopProviderCommandEndpoint final {
  core::StopEscalationStageId stop_escalation_stage_id;
  core::ProviderContractRefDTO provider_contract_ref;
  core::EndpointRoleId endpoint_role_id;
  std::string logical_endpoint_symbol;

  friend bool operator==(const LogicalStopProviderCommandEndpoint &,
                         const LogicalStopProviderCommandEndpoint &) = default;
};

struct LogicalStopProviderEvidenceEndpoint final {
  core::StopEscalationStageId stop_escalation_stage_id;
  core::StopEvidenceItemId evidence_item_id;
  core::ProviderContractRefDTO provider_contract_ref;
  core::EndpointRoleId endpoint_role_id;
  std::string logical_endpoint_symbol;

  friend bool operator==(const LogicalStopProviderEvidenceEndpoint &,
                         const LogicalStopProviderEvidenceEndpoint &) = default;
};

struct LogicalStopBindingStateAuthoritySource final {
  core::StopEscalationStageId stop_escalation_stage_id;
  core::StopEvidenceItemId evidence_item_id;
  core::StateAuthorityId state_authority_id;
  core::StateAuthorityClass state_authority_class;
  core::EndpointRoleId state_endpoint_role_id;
  std::string logical_endpoint_symbol;

  friend bool
  operator==(const LogicalStopBindingStateAuthoritySource &,
             const LogicalStopBindingStateAuthoritySource &) = default;
};

struct LogicalResourceStopScopeTemplate final {
  core::StopEscalationStageId stop_escalation_stage_id;
  core::ActivationSetId activation_set_id;
  std::vector<core::ResourceId> resource_ids;

  friend bool operator==(const LogicalResourceStopScopeTemplate &,
                         const LogicalResourceStopScopeTemplate &) = default;
};

struct LogicalControllerSetStopScopeTemplate final {
  core::StopEscalationStageId stop_escalation_stage_id;
  core::ActivationSetId activation_set_id;
  std::vector<core::ControllerRoleId> active_controller_role_ids;
  std::vector<std::string> logical_controller_symbols;

  friend bool
  operator==(const LogicalControllerSetStopScopeTemplate &,
             const LogicalControllerSetStopScopeTemplate &) = default;
};

struct LogicalControlDomainStopScopeTemplate final {
  core::StopEscalationStageId stop_escalation_stage_id;
  core::ActivationSetId activation_set_id;
  core::ControlDomainId control_domain_id;
  core::ControlDomainRevision control_domain_revision;

  friend bool
  operator==(const LogicalControlDomainStopScopeTemplate &,
             const LogicalControlDomainStopScopeTemplate &) = default;
};

using LogicalStopScopeTemplate =
    std::variant<LogicalResourceStopScopeTemplate,
                 LogicalControllerSetStopScopeTemplate,
                 LogicalControlDomainStopScopeTemplate>;

struct LogicalStopEscalationStageTemplate final {
  core::StopEscalationStageId stop_escalation_stage_id;
  core::StopEscalationStageKind stage_kind;
  core::ContainmentGranularity containment_granularity;
  core::StopContainmentCommandDTO containment_command;
  core::StopEvidenceConjunction evidence_conjunction;
  std::vector<core::StopEvidenceRequirementDTO> all_of_evidence_requirements;
  std::optional<core::JsonObjectDTO> default_contract_parameters;
  std::optional<core::CommissionedStopInterfaceDTO> commissioned_stop_interface;
  std::optional<LogicalStopProviderCommandEndpoint> provider_command_endpoint;
  std::vector<LogicalStopProviderEvidenceEndpoint> provider_evidence_endpoints;
  std::vector<LogicalStopBindingStateAuthoritySource>
      binding_state_authority_sources;
  std::vector<LogicalStopScopeTemplate> containment_scopes;

  friend bool operator==(const LogicalStopEscalationStageTemplate &,
                         const LogicalStopEscalationStageTemplate &) = default;
};

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

  friend bool
  operator==(const LogicalNotStoppableApplicabilityTemplate &,
             const LogicalNotStoppableApplicabilityTemplate &) = default;
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
  core::StopDeadlineBudgetDTO deadline_budget;
  std::vector<LogicalStopEscalationStageTemplate> escalation_stages;

  friend bool
  operator==(const LogicalStoppableApplicabilityTemplate &,
             const LogicalStoppableApplicabilityTemplate &) = default;
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
  ApplicabilityQualifiedCellIr(const ApplicabilityQualifiedCellIr &) noexcept =
      default;
  ApplicabilityQualifiedCellIr &
  operator=(const ApplicabilityQualifiedCellIr &) noexcept = default;

  [[nodiscard]] const CompositionQualifiedCellIr &composition() const noexcept {
    return data_->composition;
  }
  [[nodiscard]] const RevisionVerifiedDeploymentApplicabilityInputs &
  deployment_inputs() const noexcept {
    return data_->deployment_inputs;
  }
  [[nodiscard]] const std::optional<core::SiteOverlayRefDTO> &
  selected_site_overlay_ref() const noexcept {
    return data_->selected_site_overlay_ref;
  }
  [[nodiscard]] const QualifiedAssistantSettings &
  assistant_settings() const noexcept {
    return data_->assistant_settings;
  }
  [[nodiscard]] const std::vector<QualifiedRuntimeProfile> &
  runtime_profiles() const noexcept {
    return data_->runtime_profiles;
  }
  [[nodiscard]] const std::vector<QualifiedRuntimePolicy> &
  runtime_policies() const noexcept {
    return data_->runtime_policies;
  }
  [[nodiscard]] const std::vector<QualifiedContactPolicy> &
  contact_policies() const noexcept {
    return data_->contact_policies;
  }
  [[nodiscard]] const std::vector<QualifiedCapabilityDefinition> &
  capability_definitions() const noexcept {
    return data_->capability_definitions;
  }
  [[nodiscard]] const std::vector<QualifiedCommandSchema> &
  command_schemas() const noexcept {
    return data_->command_schemas;
  }
  [[nodiscard]] const std::vector<QualifiedControlInterfaceContract> &
  control_interface_contracts() const noexcept {
    return data_->control_interface_contracts;
  }
  [[nodiscard]] const std::vector<QualifiedControllerTopology> &
  controller_topologies() const noexcept {
    return data_->controller_topologies;
  }
  [[nodiscard]] const std::vector<QualifiedProviderManifest> &
  provider_manifests() const noexcept {
    return data_->provider_manifests;
  }
  [[nodiscard]] const std::vector<QualifiedStopEvidencePolicy> &
  stop_evidence_policies() const noexcept {
    return data_->stop_evidence_policies;
  }
  [[nodiscard]] const std::vector<BuildQualifiedValueSchemaEntry> &
  value_schemas() const noexcept {
    return data_->value_schemas;
  }
  [[nodiscard]] const std::vector<QualifiedCommissioningEvidenceRef> &
  commissioning_evidence_refs() const noexcept {
    return data_->commissioning_evidence_refs;
  }
  [[nodiscard]] const std::vector<LogicalBindingInterfaceTemplate> &
  binding_interfaces() const noexcept {
    return data_->binding_interfaces;
  }
  [[nodiscard]] const std::vector<LogicalControllerTemplate> &
  controllers() const noexcept {
    return data_->controllers;
  }
  [[nodiscard]] const std::vector<LogicalControlRouteTemplate> &
  routes() const noexcept {
    return data_->routes;
  }
  [[nodiscard]] const std::vector<LogicalCapabilityTemplate> &
  capabilities() const noexcept {
    return data_->capabilities;
  }
  [[nodiscard]] const std::vector<LogicalProviderEndpointTemplate> &
  provider_endpoints() const noexcept {
    return data_->provider_endpoints;
  }
  [[nodiscard]] const std::vector<LogicalExternalResourceTemplate> &
  external_resources() const noexcept {
    return data_->external_resources;
  }
  [[nodiscard]] const std::vector<LogicalStopApplicabilityTemplate> &
  stop_applicability() const noexcept {
    return data_->stop_applicability;
  }

private:
  struct Data final {
    Data(const CompositionQualifiedCellIr &composition_in,
         const RevisionVerifiedDeploymentApplicabilityInputs
             &deployment_inputs_in,
         std::optional<core::SiteOverlayRefDTO> selected_site_overlay_ref_in,
         const QualifiedAssistantSettings &assistant_settings_in,
         std::vector<QualifiedRuntimeProfile> runtime_profiles_in,
         std::vector<QualifiedRuntimePolicy> runtime_policies_in,
         std::vector<QualifiedContactPolicy> contact_policies_in,
         std::vector<QualifiedCommandSchema> command_schemas_in,
         std::vector<QualifiedCapabilityDefinition> capability_definitions_in,
         std::vector<QualifiedControlInterfaceContract>
             control_interface_contracts_in,
         std::vector<QualifiedControllerTopology> controller_topologies_in,
         std::vector<QualifiedProviderManifest> provider_manifests_in,
         std::vector<QualifiedStopEvidencePolicy> stop_evidence_policies_in,
         std::vector<BuildQualifiedValueSchemaEntry> value_schemas_in,
         std::vector<QualifiedCommissioningEvidenceRef>
             commissioning_evidence_refs_in,
         std::vector<LogicalBindingInterfaceTemplate> binding_interfaces_in,
         std::vector<LogicalControllerTemplate> controllers_in,
         std::vector<LogicalControlRouteTemplate> routes_in,
         std::vector<LogicalCapabilityTemplate> capabilities_in,
         std::vector<LogicalProviderEndpointTemplate> provider_endpoints_in,
         std::vector<LogicalExternalResourceTemplate> external_resources_in,
         std::vector<LogicalStopApplicabilityTemplate> stop_applicability_in)
        : composition(composition_in), deployment_inputs(deployment_inputs_in),
          selected_site_overlay_ref(std::move(selected_site_overlay_ref_in)),
          assistant_settings(assistant_settings_in),
          runtime_profiles(std::move(runtime_profiles_in)),
          runtime_policies(std::move(runtime_policies_in)),
          contact_policies(std::move(contact_policies_in)),
          command_schemas(std::move(command_schemas_in)),
          capability_definitions(std::move(capability_definitions_in)),
          control_interface_contracts(
              std::move(control_interface_contracts_in)),
          controller_topologies(std::move(controller_topologies_in)),
          provider_manifests(std::move(provider_manifests_in)),
          stop_evidence_policies(std::move(stop_evidence_policies_in)),
          value_schemas(std::move(value_schemas_in)),
          commissioning_evidence_refs(
              std::move(commissioning_evidence_refs_in)),
          binding_interfaces(std::move(binding_interfaces_in)),
          controllers(std::move(controllers_in)), routes(std::move(routes_in)),
          capabilities(std::move(capabilities_in)),
          provider_endpoints(std::move(provider_endpoints_in)),
          external_resources(std::move(external_resources_in)),
          stop_applicability(std::move(stop_applicability_in)) {}

    CompositionQualifiedCellIr composition;
    RevisionVerifiedDeploymentApplicabilityInputs deployment_inputs;
    std::optional<core::SiteOverlayRefDTO> selected_site_overlay_ref;
    QualifiedAssistantSettings assistant_settings;
    std::vector<QualifiedRuntimeProfile> runtime_profiles;
    std::vector<QualifiedRuntimePolicy> runtime_policies;
    std::vector<QualifiedContactPolicy> contact_policies;
    std::vector<QualifiedCommandSchema> command_schemas;
    std::vector<QualifiedCapabilityDefinition> capability_definitions;
    std::vector<QualifiedControlInterfaceContract> control_interface_contracts;
    std::vector<QualifiedControllerTopology> controller_topologies;
    std::vector<QualifiedProviderManifest> provider_manifests;
    std::vector<QualifiedStopEvidencePolicy> stop_evidence_policies;
    std::vector<BuildQualifiedValueSchemaEntry> value_schemas;
    std::vector<QualifiedCommissioningEvidenceRef> commissioning_evidence_refs;
    std::vector<LogicalBindingInterfaceTemplate> binding_interfaces;
    std::vector<LogicalControllerTemplate> controllers;
    std::vector<LogicalControlRouteTemplate> routes;
    std::vector<LogicalCapabilityTemplate> capabilities;
    std::vector<LogicalProviderEndpointTemplate> provider_endpoints;
    std::vector<LogicalExternalResourceTemplate> external_resources;
    std::vector<LogicalStopApplicabilityTemplate> stop_applicability;
  };

  ApplicabilityQualifiedCellIr(
      const CompositionQualifiedCellIr &composition,
      const RevisionVerifiedDeploymentApplicabilityInputs &deployment_inputs,
      std::optional<core::SiteOverlayRefDTO> selected_site_overlay_ref,
      const QualifiedAssistantSettings &assistant_settings,
      std::vector<QualifiedRuntimeProfile> runtime_profiles,
      std::vector<QualifiedRuntimePolicy> runtime_policies,
      std::vector<QualifiedContactPolicy> contact_policies,
      std::vector<QualifiedCommandSchema> command_schemas,
      std::vector<QualifiedCapabilityDefinition> capability_definitions,
      std::vector<QualifiedControlInterfaceContract>
          control_interface_contracts,
      std::vector<QualifiedControllerTopology> controller_topologies,
      std::vector<QualifiedProviderManifest> provider_manifests,
      std::vector<QualifiedStopEvidencePolicy> stop_evidence_policies,
      std::vector<BuildQualifiedValueSchemaEntry> value_schemas,
      std::vector<QualifiedCommissioningEvidenceRef>
          commissioning_evidence_refs,
      std::vector<LogicalBindingInterfaceTemplate> binding_interfaces,
      std::vector<LogicalControllerTemplate> controllers,
      std::vector<LogicalControlRouteTemplate> routes,
      std::vector<LogicalCapabilityTemplate> capabilities,
      std::vector<LogicalProviderEndpointTemplate> provider_endpoints,
      std::vector<LogicalExternalResourceTemplate> external_resources,
      std::vector<LogicalStopApplicabilityTemplate> stop_applicability)
      : data_(std::make_shared<const Data>(
            composition, deployment_inputs,
            std::move(selected_site_overlay_ref), assistant_settings,
            std::move(runtime_profiles), std::move(runtime_policies),
            std::move(contact_policies), std::move(command_schemas),
            std::move(capability_definitions),
            std::move(control_interface_contracts),
            std::move(controller_topologies), std::move(provider_manifests),
            std::move(stop_evidence_policies), std::move(value_schemas),
            std::move(commissioning_evidence_refs),
            std::move(binding_interfaces), std::move(controllers),
            std::move(routes), std::move(capabilities),
            std::move(provider_endpoints), std::move(external_resources),
            std::move(stop_applicability))) {}

  std::shared_ptr<const Data> data_;

  friend ApplicabilityQualifiedCellIr qualify_cell_applicability(
      const CompositionQualifiedCellIr &,
      const RevisionVerifiedDeploymentApplicabilityInputs &,
      std::span<const QualifiedRuntimeProfile>,
      std::span<const QualifiedRuntimePolicy>,
      std::span<const QualifiedCommandSchema>,
      std::span<const QualifiedCapabilityDefinition>,
      std::span<const QualifiedControlInterfaceContract>,
      std::span<const QualifiedControllerTopology>,
      std::span<const QualifiedProviderManifest>,
      std::span<const QualifiedStopEvidencePolicy>,
      std::span<const BuildQualifiedValueSchemaEntry>,
      std::span<const QualifiedCommissioningEvidenceRef>,
      const QualifiedSelectedSiteOverlay &,
      std::span<const QualifiedContactPolicy>);
};

[[nodiscard]] ApplicabilityQualifiedCellIr qualify_cell_applicability(
    const CompositionQualifiedCellIr &composition,
    const RevisionVerifiedDeploymentApplicabilityInputs &deployment_inputs,
    std::span<const QualifiedRuntimeProfile> runtime_profiles,
    std::span<const QualifiedRuntimePolicy> runtime_policies,
    std::span<const QualifiedCommandSchema> command_schemas,
    std::span<const QualifiedCapabilityDefinition> capability_definitions,
    std::span<const QualifiedControlInterfaceContract>
        control_interface_contracts,
    std::span<const QualifiedControllerTopology> controller_topologies,
    std::span<const QualifiedProviderManifest> provider_manifests,
    std::span<const QualifiedStopEvidencePolicy> stop_evidence_policies,
    std::span<const BuildQualifiedValueSchemaEntry> value_schemas,
    std::span<const QualifiedCommissioningEvidenceRef>
        commissioning_evidence_refs,
    const QualifiedSelectedSiteOverlay &selected_site_overlay,
    std::span<const QualifiedContactPolicy> contact_policies = {});

} // namespace manyforge::deployment
