#pragma once

#include <cstdint>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/json_value.hpp>
#include <manyforge_core/types/retained_artifact.hpp>
#include <manyforge_core/types/stop_control.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::core {

/// Exact retained compiler template from which CCLC resolved one base plan.
struct ResolvedStopTemplateRefDTO final {
  ProductId product_id;
  ProductContractId product_contract_id;
  ProductContractRevision product_contract_revision;
  RetainedArtifactRefDTO artifact_ref;
  std::string logical_stop_symbol;

  friend bool operator==(const ResolvedStopTemplateRefDTO&,
                         const ResolvedStopTemplateRefDTO&) = default;
};

/// One exact managed/focused adapter used to invoke or observe a Stop stage.
/// Endpoint and site-overlay fields are closed jointly-present groups.
struct ResolvedStopRuntimeTargetDTO final {
  ProviderInstanceId provider_instance_id;
  ProviderManifestId provider_manifest_id;
  ProviderManifestRevision provider_manifest_revision;
  RuntimeUnitId runtime_unit_id;
  ProductId product_id;
  ProductContractId product_contract_id;
  ProductContractRevision product_contract_revision;
  ContentDigest content_digest;
  RuntimeEpoch provider_runtime_epoch;
  RuntimeEpoch runtime_unit_epoch;
  std::optional<EndpointRoleId> endpoint_role_id;
  std::optional<std::string> logical_endpoint_symbol;
  std::optional<ExternalResourceAlias> external_resource_alias;
  std::optional<ExternalResourceKind> external_resource_kind;
  std::optional<SiteOverlayId> site_overlay_id;
  std::optional<SiteOverlayRevision> site_overlay_revision;
  std::optional<ContentDigest> site_overlay_non_secret_digest;
  std::optional<SiteOverlayValueRef> site_overlay_value_ref;

  friend bool operator==(const ResolvedStopRuntimeTargetDTO&,
                         const ResolvedStopRuntimeTargetDTO&) = default;
};

struct ResolvedStopCommandQualificationDTO final {
  StopActionClass stop_action_class{StopActionClass::kControlled};
  std::optional<ControlledStopSubstitutionContractDTO>
      controlled_substitution_contract;
  std::optional<SafetySystemRequestActionClass> safety_system_request_action_class;
  std::optional<CommissionedStopInterfaceId> commissioned_stop_interface_id;
  std::optional<CommissionedStopInterfaceRevision> commissioned_stop_interface_revision;
  std::optional<RetainedArtifactRefDTO> commissioning_evidence_artifact_ref;

  friend bool operator==(const ResolvedStopCommandQualificationDTO&,
                         const ResolvedStopCommandQualificationDTO&) = default;
};

struct ResolvedStopResourceContainmentScopeDTO final {
  ActivationSetId activation_set_id;
  std::vector<ResourceId> resource_ids;

  friend bool operator==(const ResolvedStopResourceContainmentScopeDTO&,
                         const ResolvedStopResourceContainmentScopeDTO&) = default;
};

struct ResolvedStopControllerContainmentRowDTO final {
  ControllerRoleId controller_role_id;
  std::string logical_controller_symbol;

  friend bool operator==(const ResolvedStopControllerContainmentRowDTO&,
                         const ResolvedStopControllerContainmentRowDTO&) = default;
};

struct ResolvedStopControllerSetContainmentScopeDTO final {
  ActivationSetId activation_set_id;
  std::vector<ResolvedStopControllerContainmentRowDTO> controllers;

  friend bool operator==(const ResolvedStopControllerSetContainmentScopeDTO&,
                         const ResolvedStopControllerSetContainmentScopeDTO&) = default;
};

struct ResolvedStopControlDomainContainmentScopeDTO final {
  ActivationSetId activation_set_id;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;

  friend bool operator==(const ResolvedStopControlDomainContainmentScopeDTO&,
                         const ResolvedStopControlDomainContainmentScopeDTO&) = default;
};

using ResolvedStopContainmentScopeDTO = std::variant<ResolvedStopResourceContainmentScopeDTO,
                                                     ResolvedStopControllerSetContainmentScopeDTO,
                                                     ResolvedStopControlDomainContainmentScopeDTO>;

struct ResolvedStopEvidenceSourceDTO final {
  StopEvidenceSourceKind source_kind{StopEvidenceSourceKind::kControllerGoalStatus};
  ResolvedStopRuntimeTargetDTO runtime_target;
  std::optional<StateAuthorityId> state_authority_id;
  std::optional<StateAuthorityClass> state_authority_class;
  std::optional<ProviderContractRefDTO> provider_contract_ref;

  friend bool operator==(const ResolvedStopEvidenceSourceDTO&,
                         const ResolvedStopEvidenceSourceDTO&) = default;
};

struct ResolvedStopEvidenceRequirementDTO final {
  StopEvidenceRequirementDTO requirement;
  ResolvedStopEvidenceSourceDTO resolved_source;

  friend bool operator==(const ResolvedStopEvidenceRequirementDTO&,
                         const ResolvedStopEvidenceRequirementDTO&) = default;
};

struct ResolvedStopEscalationStageDTO final {
  std::uint64_t stage_index{};
  StopEscalationStageId stop_escalation_stage_id;
  StopEscalationStageKind stage_kind{StopEscalationStageKind::kNominal};
  ContainmentGranularity containment_granularity{ContainmentGranularity::kResource};
  StopContainmentCommandDTO containment_command;
  std::optional<JsonObjectDTO> default_contract_parameters;
  ResolvedStopCommandQualificationDTO command_qualification;
  ResolvedStopRuntimeTargetDTO command_target;
  ResolvedStopContainmentScopeDTO containment_scope;
  StopEvidenceConjunction evidence_conjunction{StopEvidenceConjunction::kAllOf};
  std::vector<ResolvedStopEvidenceRequirementDTO> evidence_requirements;

  friend bool operator==(const ResolvedStopEscalationStageDTO&,
                         const ResolvedStopEscalationStageDTO&) = default;
};

/// Content-addressed I7 reconciliation product scoped to one exact active
/// control configuration. It contains no operation-specific or live evidence.
struct ResolvedStopBasePlanDTO final {
  ResolvedStopBasePlanId resolved_stop_base_plan_id;
  CellId cell_id;
  RetainedBundleId retained_bundle_id;
  GraphRevision graph_revision;
  ResolvedStopTemplateRefDTO source_template;
  ControlDomainId control_domain_id;
  ControlDomainRevision control_domain_revision;
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  ControllerTopologyId controller_topology_id;
  TopologyRevision topology_revision;
  ControlRouteId control_route_id;
  ActivationSetId activation_set_id;
  ActiveControlConfigurationId active_control_configuration_id;
  ActiveControlConfigurationGeneration active_control_configuration_generation;
  RuntimeUnitId source_runtime_unit_id;
  RuntimeEpoch source_runtime_epoch;
  std::uint64_t source_sequence{};
  StopEvidencePolicyId stop_evidence_policy_id;
  StopEvidencePolicyRevision stop_evidence_policy_revision;
  ContainmentGranularity containment_granularity{ContainmentGranularity::kResource};
  StopDeadlineBudgetDTO deadline_budget;
  std::vector<ResolvedStopEscalationStageDTO> escalation_stages;

  friend bool operator==(const ResolvedStopBasePlanDTO&, const ResolvedStopBasePlanDTO&) = default;
};

}  // namespace manyforge::core
