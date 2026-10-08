#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/runtime_profile.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {
namespace {

constexpr const char* kDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

template <typename T>
concept HasGraphRevision = requires(T value) { value.graph_revision; };

template <typename T>
concept HasDesiredState = requires(T value) { value.desired_state; };

template <typename T>
concept HasCommandTuple = requires(T value) { value.command; };

template <typename T>
concept HasCatalogContentDigest = requires(T value) { value.runtime_profile_digest; } ||
                                  requires(T value) { value.component_type_digest; } ||
                                  requires(T value) { value.provider_digest; } ||
                                  requires(T value) { value.stop_evidence_policy_digest; };

static_assert(!std::is_same_v<CellId, ModelAssetId>);
static_assert(!std::is_same_v<ModelAssetId, ModelInstanceId>);
static_assert(!std::is_same_v<RuntimeProfileRevision, ModelRevision>);
static_assert(!std::is_same_v<RuntimeRoleId, EndpointRoleId>);
static_assert(!std::is_same_v<ModelRoleId, ModelGroupRoleId>);
static_assert(!std::is_same_v<EndpointRoleId, StateAuthorityId>);
static_assert(!std::is_same_v<ControlDomainId, ControlBindingId>);
static_assert(!std::is_same_v<ComponentAttachmentId, ComponentAttachmentRevision>);
static_assert(!std::is_same_v<ProviderInstanceId, ProviderManifestId>);
static_assert(!std::is_constructible_v<CellId, ModelAssetId>);
static_assert(!std::is_constructible_v<ModelAssetId, CellId>);
static_assert(!std::is_convertible_v<std::string, CellId>);
static_assert(!std::is_convertible_v<CellId, std::string>);

static_assert(std::is_same_v<decltype(RuntimeUnitDTO::runtime_unit_id), RuntimeUnitId>);
static_assert(
    std::is_same_v<decltype(RuntimeUnitDTO::runtime_profile_revision), RuntimeProfileRevision>);
static_assert(std::is_same_v<decltype(ModelInstanceDTO::model_revision), ModelRevision>);
static_assert(
    std::is_same_v<decltype(ModelInstanceDTO::root_pose_in_cell), std::optional<PlacementPoseDTO>>);
static_assert(std::is_same_v<decltype(ControllableComponentDTO::model_role_bindings),
                             std::vector<ModelRoleBindingDTO>>);
static_assert(std::is_same_v<decltype(ControllableComponentDTO::root_pose_in_cell),
                             std::optional<PlacementPoseDTO>>);
static_assert(
    std::is_same_v<decltype(ControlDomainDTO::containment_granularity), ContainmentGranularity>);
static_assert(
    std::is_same_v<decltype(ControlBindingDTO::state_authority_class), StateAuthorityClass>);
static_assert(std::is_same_v<decltype(ControlBindingDTO::state_authority_id), StateAuthorityId>);
static_assert(std::is_same_v<decltype(ControlBindingDTO::capability_definition_revision),
                             CapabilityDefinitionRevision>);
static_assert(
    std::is_same_v<decltype(ControlBindingDTO::provider_instance_id), ProviderInstanceId>);
static_assert(
    std::is_same_v<decltype(ControlBindingDTO::provider_manifest_id), ProviderManifestId>);
static_assert(std::is_same_v<decltype(ControlBindingDTO::provider_manifest_revision),
                             ProviderManifestRevision>);
static_assert(
    std::is_same_v<decltype(ComponentAttachmentEdgeDTO::parent), ComponentAttachmentEndpointDTO>);
static_assert(
    std::is_same_v<decltype(ComponentAttachmentEdgeDTO::child_interface_pose_in_parent_interface),
                   PlacementPoseDTO>);

static_assert(!HasGraphRevision<RuntimeUnitDTO>);
static_assert(!HasGraphRevision<ModelInstanceDTO>);
static_assert(!HasGraphRevision<ControllableComponentDTO>);
static_assert(!HasGraphRevision<ControlDomainDTO>);
static_assert(!HasGraphRevision<ControlBindingDTO>);
static_assert(!HasGraphRevision<ComponentAttachmentEdgeDTO>);
static_assert(!HasDesiredState<RuntimeUnitDTO>);
static_assert(!HasCommandTuple<RuntimeUnitDTO>);
static_assert(!HasCatalogContentDigest<RuntimeUnitDTO>);
static_assert(!HasCatalogContentDigest<ControllableComponentDTO>);
static_assert(!HasCatalogContentDigest<ControlBindingDTO>);

TEST(ComposableCellContracts, EqualTextRemainsDistinctAcrossStrongTypes) {
  const CellId cell{"same-text"};
  const ModelAssetId asset{"same-text"};
  const ModelInstanceId instance{"same-text"};

  EXPECT_EQ(cell.value(), asset.value());
  EXPECT_EQ(asset.value(), instance.value());
  EXPECT_EQ(CellId{"same-text"}, cell);
  EXPECT_LT(CellId{"a"}, CellId{"b"});
}

TEST(ComposableCellContracts, ClosedEnumsExposeExactWireSpellings) {
  EXPECT_EQ(to_string(ContainmentGranularity::kResource), "resource");
  EXPECT_EQ(to_string(ContainmentGranularity::kControllerSet), "controller_set");
  EXPECT_EQ(to_string(ContainmentGranularity::kControlDomain), "control_domain");
  EXPECT_EQ(to_string(StateAuthorityClass::kMeasured), "measured");
  EXPECT_EQ(to_string(StateAuthorityClass::kCommanded), "commanded");
  EXPECT_EQ(to_string(StateAuthorityClass::kPredicted), "predicted");
  EXPECT_EQ(to_string(CatalogRecordKind::kControlInterfaceContract), "control_interface_contract");
  EXPECT_EQ(to_string(RuntimeLifecycleControl::kManagerManaged), "manager_managed");
  EXPECT_EQ(to_string(RuntimeLifecycleControl::kExternallyManaged), "externally_managed");
}

TEST(ComposableCellContracts, RuntimeProfileSeparatesCatalogTruthFromRuntimeUnitConfiguration) {
  const RuntimeProfileDTO profile{
      RuntimeProfileId{"runtime-profile.xarm"},
      RuntimeProfileRevision{kDigest},
      ValueSchemaRefDTO{SchemaId{"manyforge.value_schema.runtime.empty.v1"},
                        SchemaRevision{kDigest}},
      RuntimeProcessContractDTO{RuntimeLifecycleControl::kManagerManaged},
      RuntimeSupervisionContractDTO{EndpointRoleId{"runtime_status"}},
      {RuntimeRoleId{"driver"}},
      {EndpointRoleId{"runtime_status"}},
  };
  EXPECT_EQ(profile.process_contract.lifecycle_control, RuntimeLifecycleControl::kManagerManaged);
  EXPECT_EQ(profile.offered_runtime_role_ids.front(), RuntimeRoleId{"driver"});
  EXPECT_EQ(profile.source_endpoint_role_ids.front(),
            profile.supervision_contract.status_endpoint_role_id);
}

TEST(ComposableCellContracts, SixGraphRecordShapesComposeOneClosedEnvelope) {
  const RuntimeUnitDTO runtime_unit{
      RuntimeUnitId{"runtime.control.a"},
      RuntimeProfileId{"runtime-profile.xarm"},
      RuntimeProfileRevision{kDigest},
      {},
      {},
  };
  const ModelInstanceDTO model_instance{
      ModelInstanceId{"model.lite6.a"},
      ModelAssetId{"asset.lite6"},
      ModelRevision{kDigest},
      std::nullopt,
  };
  const ControllableComponentDTO component{
      ControllableComponentId{"component.lite6.a"},
      ComponentTypeId{"component-type.arm"},
      ComponentTypeRevision{kDigest},
      {{ModelRoleId{"base"}, model_instance.model_instance_id}},
      {runtime_unit.runtime_unit_id},
      PlacementPoseDTO{},
  };
  const ControlDomainDTO domain{
      ControlDomainId{"domain.lite6.a"},      ControlDomainRevision{kDigest},
      {runtime_unit.runtime_unit_id},         EndpointRoleId{"controller_manager"},
      ControllerTopologyId{"topology.lite6"}, TopologyRevision{kDigest},
      ContainmentGranularity::kControllerSet,
  };
  const ControlBindingDTO binding{
      ControlBindingId{"binding.lite6.a.arm"},
      ControlBindingRevision{kDigest},
      component.controllable_component_id,
      CommandTargetId{"arm"},
      CapabilityId{"execution.trajectory.execute"},
      CapabilityDefinitionRevision{kDigest},
      domain.control_domain_id,
      {{ResourceRoleId{"arm_joint_1"}, ResourceId{"lite6_a_joint1"}}},
      ControlInterfaceContractId{"interface.arm.position"},
      ControlInterfaceRevision{kDigest},
      domain.controller_topology_id,
      domain.topology_revision,
      {ControlRouteId{"standard_trajectory"}},
      {{ControlRouteId{"standard_trajectory"},
        {EndpointRoleId{"trajectory_command"}},
        {EndpointRoleId{"joint_state"}}}},
      ProviderInstanceId{"xarm_hardware.a"},
      runtime_unit.runtime_unit_id,
      ProviderManifestId{"xarm_hardware"},
      ProviderManifestRevision{kDigest},
      StateAuthorityId{"xarm_measured_state"},
      StateAuthorityClass::kMeasured,
      StopEvidencePolicyId{"arm_measured_rest"},
      StopEvidencePolicyRevision{kDigest},
      ContainmentGranularity::kControllerSet,
      {},
  };
  const ComponentAttachmentEdgeDTO attachment{
      ComponentAttachmentId{"tool_on_lite6_a"},
      ComponentAttachmentRevision{kDigest},
      {component.controllable_component_id, AttachmentInterfaceId{"flange"}},
      {ControllableComponentId{"component.tool.a"}, AttachmentInterfaceId{"tool_mount"}},
      PlacementPoseDTO{},
  };

  const CellGraphDTO graph{
      std::string{kCellGraphApiVersion},
      GraphRevision{kDigest},
      CellId{"cell.a"},
      {runtime_unit},
      {model_instance},
      {component},
      {domain},
      {binding},
      {attachment},
  };

  ASSERT_EQ(graph.runtime_units.size(), 1U);
  ASSERT_EQ(graph.model_instances.size(), 1U);
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  ASSERT_EQ(graph.control_domains.size(), 1U);
  ASSERT_EQ(graph.control_bindings.size(), 1U);
  ASSERT_EQ(graph.component_attachments.size(), 1U);
  EXPECT_EQ(graph.api_version, kCellGraphApiVersion);
  EXPECT_EQ(graph.control_bindings.front().legal_control_route_ids.front(),
            ControlRouteId{"standard_trajectory"});
  EXPECT_EQ(graph.component_attachments.front().parent.attachment_interface_id,
            AttachmentInterfaceId{"flange"});
}

static_assert(std::variant_size_v<CatalogRecordRefDTO> == kCatalogRecordTypeRegistry.size());
static_assert(std::is_same_v<ModelAssetCatalogRecordRefDTO::IdType, ModelAssetId>);
static_assert(std::is_same_v<ModelAssetCatalogRecordRefDTO::RevisionType, ModelRevision>);
static_assert(std::is_same_v<ProviderManifestCatalogRecordRefDTO::IdType, ProviderManifestId>);
static_assert(
    std::is_same_v<ProviderManifestCatalogRecordRefDTO::RevisionType, ProviderManifestRevision>);
static_assert(std::is_same_v<ContactPolicyCatalogRecordRefDTO::IdType, ContactPolicyId>);
static_assert(
    std::is_same_v<ContactPolicyCatalogRecordRefDTO::RevisionType, ContactPolicyRevision>);

using InvalidModelAssetPair =
    TypedCatalogRecordRefDTO<CatalogRecordKind::kModelAsset, ModelAssetId, RuntimeProfileRevision>;
static_assert(!std::is_constructible_v<CatalogRecordRefDTO, InvalidModelAssetPair>);

TEST(ComposableCellContracts, CatalogRegistryIsClosedAndLookupIsOrdinalIndependent) {
  std::set<CatalogRecordKind> record_kinds;
  std::set<std::string_view> serialized_kinds;
  std::set<std::string_view> schema_families;

  for (std::size_t index = 0; index < kCatalogRecordTypeRegistry.size(); ++index) {
    const auto& descriptor = kCatalogRecordTypeRegistry[index];
    const auto* found = catalog_record_type_descriptor(descriptor.record_kind);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found, &descriptor);
    EXPECT_TRUE(record_kinds.insert(descriptor.record_kind).second);
    EXPECT_TRUE(serialized_kinds.insert(descriptor.serialized_kind).second);
    EXPECT_TRUE(schema_families.insert(descriptor.schema_family).second);
    EXPECT_FALSE(descriptor.own_id_field.empty());
    EXPECT_FALSE(descriptor.own_revision_field.empty());
  }

  EXPECT_EQ(catalog_record_type_descriptor(static_cast<CatalogRecordKind>(255)), nullptr);
}

TEST(ComposableCellContracts, CatalogCanonicalOrderUsesWireKindIdAndRevision) {
  constexpr const char* kLaterDigest =
      "sha256:0000000000000000000000000000000000000000000000000000000000000001";
  std::vector<CatalogRecordRefDTO> records{
      ModelAssetCatalogRecordRefDTO{ModelAssetId{"z_asset"}, ModelRevision{kDigest},
                                    SchemaId{"manyforge.model_asset.v1"}, SchemaRevision{kDigest},
                                    ContentDigest{kDigest},
                                    ContentAddressedStoreKey{"sha256/z_asset"}},
      ModelAssetCatalogRecordRefDTO{ModelAssetId{"a_asset"}, ModelRevision{kLaterDigest},
                                    SchemaId{"manyforge.model_asset.v1"}, SchemaRevision{kDigest},
                                    ContentDigest{kDigest},
                                    ContentAddressedStoreKey{"sha256/a_asset_1"}},
      ComponentTypeCatalogRecordRefDTO{
          ComponentTypeId{"z_component"}, ComponentTypeRevision{kDigest},
          SchemaId{"manyforge.component_type.v1"}, SchemaRevision{kDigest}, ContentDigest{kDigest},
          ContentAddressedStoreKey{"sha256/z_component"}},
      ModelAssetCatalogRecordRefDTO{ModelAssetId{"a_asset"}, ModelRevision{kDigest},
                                    SchemaId{"manyforge.model_asset.v1"}, SchemaRevision{kDigest},
                                    ContentDigest{kDigest},
                                    ContentAddressedStoreKey{"sha256/a_asset_0"}},
  };

  std::sort(records.begin(), records.end(), CanonicalCatalogRecordRefLess{});

  EXPECT_EQ(to_string(catalog_record_kind(records[0])), "component_type");
  EXPECT_EQ(catalog_record_id_value(records[1]), "a_asset");
  EXPECT_EQ(catalog_record_revision_value(records[1]), kDigest);
  EXPECT_EQ(catalog_record_revision_value(records[2]), kLaterDigest);
  EXPECT_EQ(catalog_record_id_value(records[3]), "z_asset");
}

TEST(ComposableCellContracts, CatalogReferenceAlternativeFixesIdRevisionPair) {
  CatalogRecordRefDTO record = ModelAssetCatalogRecordRefDTO{
      ModelAssetId{"asset.lite6"},
      ModelRevision{kDigest},
      SchemaId{"manyforge.model_asset.v1"},
      SchemaRevision{kDigest},
      ContentDigest{kDigest},
      ContentAddressedStoreKey{"sha256/model-asset-lite6"},
  };

  EXPECT_EQ(catalog_record_kind(record), CatalogRecordKind::kModelAsset);
  EXPECT_EQ(catalog_record_id_value(record), "asset.lite6");
  EXPECT_EQ(catalog_record_revision_value(record), kDigest);
  EXPECT_EQ(catalog_record_type_descriptor(record).own_id_field, "model_asset_id");
  EXPECT_EQ(catalog_record_type_descriptor(record).own_revision_field, "model_revision");
  EXPECT_TRUE(catalog_record_ref_matches_registry_family(record));

  record = ModelAssetCatalogRecordRefDTO{
      ModelAssetId{"asset.lite6"},
      ModelRevision{kDigest},
      SchemaId{"manyforge.runtime_profile.v1"},
      SchemaRevision{kDigest},
      ContentDigest{kDigest},
      ContentAddressedStoreKey{"sha256/model-asset-lite6"},
  };
  EXPECT_FALSE(catalog_record_ref_matches_registry_family(record));
}

TEST(ComposableCellContracts, ContactPolicyRegistryUsesReferentOwnFieldNames) {
  const CatalogRecordRefDTO record = ContactPolicyCatalogRecordRefDTO{
      ContactPolicyId{"support_contact"},
      ContactPolicyRevision{kDigest},
      SchemaId{"manyforge.contact_policy.v1"},
      SchemaRevision{kDigest},
      ContentDigest{kDigest},
      ContentAddressedStoreKey{"sha256/contact-policy"},
  };

  const auto& descriptor = catalog_record_type_descriptor(record);
  EXPECT_EQ(descriptor.own_id_field, "policy_id");
  EXPECT_EQ(descriptor.own_revision_field, "version");
  EXPECT_TRUE(catalog_record_ref_matches_registry_family(record));
}

TEST(ComposableCellContracts, ProviderManifestRegistryUsesReusableManifestIdentity) {
  const CatalogRecordRefDTO record = ProviderManifestCatalogRecordRefDTO{
      ProviderManifestId{"xarm_hardware"},
      ProviderManifestRevision{kDigest},
      SchemaId{"manyforge.provider_manifest.v1"},
      SchemaRevision{kDigest},
      ContentDigest{kDigest},
      ContentAddressedStoreKey{"sha256/provider-manifest"},
  };

  const auto& descriptor = catalog_record_type_descriptor(record);
  EXPECT_EQ(descriptor.own_id_field, "provider_manifest_id");
  EXPECT_EQ(catalog_record_id_value(record), "xarm_hardware");
  EXPECT_TRUE(catalog_record_ref_matches_registry_family(record));
}

TEST(ComposableCellContracts, StopEvidencePolicyTypesPreserveTheClosedDeclarativeVocabulary) {
  EXPECT_EQ(kStopEvidencePolicyApiVersion, "manyforge.stop_evidence_policy.v2");
  EXPECT_EQ(kMaxStopEscalationStages, 8U);
  EXPECT_EQ(kMaxStopEvidencePolicyItems, 64U);
  EXPECT_EQ(kMaxStopEvidencePolicyDurationNs, 9007199254740991LL);
  EXPECT_EQ(to_string(StopEvidenceDisposition::kStoppable), "stoppable");
  EXPECT_EQ(to_string(StopEvidenceDisposition::kNotStoppable), "not_stoppable");
  EXPECT_EQ(to_string(StopContainmentCommandKind::kRegistered), "registered");
  EXPECT_EQ(to_string(StopEscalationStageKind::kControlledFallback),
            "controlled_fallback");
  EXPECT_EQ(to_string(StopEscalationStageKind::kSoftwareHard),
            "software_hard");
  EXPECT_EQ(to_string(StopEvidenceConjunction::kAllOf), "all_of");
  EXPECT_EQ(to_string(StopEvidenceRequirementKind::kLogicalCancellationNoContinuingEffect),
            "logical_cancellation_no_continuing_effect");
  EXPECT_EQ(to_string(StopEvidenceSourceKind::kBindingStateAuthority), "binding_state_authority");
  EXPECT_EQ(to_string(StopEvidenceObservationKind::kPostCommandDwell), "post_command_dwell");

  const ProviderContractRefDTO contract{
      ProviderContractRoleId{"stop_contract"},
      SchemaId{"manyforge.stop_provider_contract.v1"},
      SchemaRevision{kDigest},
  };
  const StopEvidencePolicyDTO policy{
      StopEvidencePolicyId{"measured_rest"},
      StopEvidencePolicyRevision{kDigest},
      StopEvidenceDisposition::kStoppable,
      ContainmentGranularity::kControlDomain,
      StopDeadlineBudgetDTO{
          1'000,
          100,
          {
              {StopBudgetPhaseId{"command"}, 100},
              {StopBudgetPhaseId{"rest"}, 500},
              {StopBudgetPhaseId{"hard_command"}, 700},
          },
      },
      {
          {
              StopEscalationStageId{"nominal_controlled"},
              StopEscalationStageKind::kNominal,
              ContainmentGranularity::kResource,
              {
                  StopBudgetPhaseId{"command"},
                  StopContainmentCommandKind::kMotionControlledStop,
                  contract,
                  std::nullopt,
              },
              StopEvidenceConjunction::kAllOf,
              {{
                  StopEvidenceItemId{"rest"},
                  StopBudgetPhaseId{"rest"},
                  StopEvidenceRequirementKind::kMotionRest,
                  contract,
                  std::nullopt,
                  {
                      StopEvidenceSourceKind::kBindingStateAuthority,
                      25,
                      EndpointRoleId{"measured_state"},
                      {StateAuthorityClass::kMeasured},
                  },
                  {
                      StopEvidenceObservationKind::kPostCommandDwell,
                      200,
                  },
              }},
          },
          {
              StopEscalationStageId{"software_hard_floor"},
              StopEscalationStageKind::kSoftwareHard,
              ContainmentGranularity::kControlDomain,
              {
                  StopBudgetPhaseId{"hard_command"},
                  StopContainmentCommandKind::kRegistered,
                  contract,
                  RegisteredStopCommandKindId{"software_hard_stop"},
              },
              StopEvidenceConjunction::kAllOf,
              {{
                  StopEvidenceItemId{"hard_acknowledged"},
                  StopBudgetPhaseId{"hard_command"},
                  StopEvidenceRequirementKind::kDiscreteAcknowledgement,
                  std::nullopt,
                  std::nullopt,
                  {
                      StopEvidenceSourceKind::kCommandTargetStatus,
                      25,
                      std::nullopt,
                      {},
                  },
                  {
                      StopEvidenceObservationKind::kCommandResult,
                      std::nullopt,
                  },
              }},
          },
      },
  };
  ASSERT_TRUE(policy.deadline_budget.has_value());
  EXPECT_EQ(policy.deadline_budget->phases.size(), 3U);
  ASSERT_EQ(policy.escalation_stages.size(), 2U);
  EXPECT_EQ(policy.escalation_stages.front().stage_kind,
            StopEscalationStageKind::kNominal);
  ASSERT_EQ(policy.escalation_stages.front().evidence_requirements.size(), 1U);
  EXPECT_EQ(policy.escalation_stages.front()
                .evidence_requirements.front()
                .provider_contract_ref,
            contract);
}

TEST(ComposableCellContracts, CapabilityDefinitionRegistryIsExactTypedAndCodeOwned) {
  EXPECT_EQ(kCapabilityDefinitionApiVersion, "manyforge.capability_definition.v1");
  ASSERT_EQ(kBuiltinCapabilityDefinitionRegistry.size(), 68U);

  std::set<std::string_view> capability_ids;
  std::size_t target_binding_count = 0U;
  std::size_t cell_global_count = 0U;
  std::size_t control_domain_count = 0U;
  std::size_t control_binding_count = 0U;
  std::size_t capability_provider_binding_count = 0U;
  for (const auto& descriptor : kBuiltinCapabilityDefinitionRegistry) {
    EXPECT_TRUE(capability_ids.insert(descriptor.capability_id).second);
    EXPECT_EQ(find_builtin_capability_definition(descriptor.capability_id), &descriptor);
    EXPECT_TRUE(descriptor.capability_definition_revision.starts_with("sha256:"));
    EXPECT_EQ(descriptor.capability_definition_revision.size(), 71U);
    EXPECT_FALSE(descriptor.requirement_parameters_schema_ref.has_value());

    target_binding_count += descriptor.scope == CapabilityScope::kTargetBinding ? 1U : 0U;
    cell_global_count += descriptor.scope == CapabilityScope::kCellGlobal ? 1U : 0U;
    control_domain_count += descriptor.scope == CapabilityScope::kControlDomain ? 1U : 0U;
    control_binding_count +=
        descriptor.provider_binding_kind == CapabilityProviderBindingKind::kControlBinding ? 1U
                                                                                           : 0U;
    capability_provider_binding_count +=
        descriptor.provider_binding_kind ==
                CapabilityProviderBindingKind::kCapabilityProviderBinding
            ? 1U
            : 0U;

    const auto materialized = materialize_capability_definition(descriptor);
    EXPECT_EQ(materialized.capability_id.value(), descriptor.capability_id);
    EXPECT_EQ(materialized.capability_definition_revision.value(),
              descriptor.capability_definition_revision);
    EXPECT_EQ(materialized.scope, descriptor.scope);
    EXPECT_EQ(materialized.provider_binding_kind, descriptor.provider_binding_kind);
    EXPECT_FALSE(materialized.requirement_parameters_schema_ref.has_value());
  }
  EXPECT_EQ(target_binding_count, 32U);
  EXPECT_EQ(cell_global_count, 28U);
  EXPECT_EQ(control_domain_count, 8U);
  EXPECT_EQ(control_binding_count, 21U);
  EXPECT_EQ(capability_provider_binding_count, 47U);
  EXPECT_EQ(find_builtin_capability_definition("context.state.current"), nullptr);
  EXPECT_EQ(find_builtin_capability_definition("unknown.capability"), nullptr);
}

TEST(ComposableCellContracts, CapabilityDefinitionRegistryPinsRepresentativeSemantics) {
  const auto* observed = find_builtin_capability_definition("state.observed.current");
  ASSERT_NE(observed, nullptr);
  EXPECT_EQ(observed->scope, CapabilityScope::kTargetBinding);
  EXPECT_EQ(observed->provider_binding_kind, CapabilityProviderBindingKind::kControlBinding);
  EXPECT_EQ(observed->capability_definition_revision,
            "sha256:c1ec4dacead10e33176830d634e9b2287e4b96322a54700c1c3910bcba461c02");

  const auto* lifecycle = find_builtin_capability_definition("component.lifecycle.enable");
  ASSERT_NE(lifecycle, nullptr);
  EXPECT_EQ(lifecycle->scope, CapabilityScope::kControlDomain);
  EXPECT_EQ(lifecycle->provider_binding_kind,
            CapabilityProviderBindingKind::kCapabilityProviderBinding);

  const auto* scene = find_builtin_capability_definition("scene.object.query");
  ASSERT_NE(scene, nullptr);
  EXPECT_EQ(scene->scope, CapabilityScope::kCellGlobal);
  EXPECT_EQ(to_string(scene->scope), "cell_global");
  EXPECT_EQ(to_string(scene->provider_binding_kind), "capability_provider_binding");

  const auto* model =
      find_builtin_capability_definition("assistant.model.invoke");
  ASSERT_NE(model, nullptr);
  EXPECT_EQ(model->scope, CapabilityScope::kCellGlobal);
  EXPECT_EQ(model->provider_binding_kind,
            CapabilityProviderBindingKind::kCapabilityProviderBinding);
  EXPECT_EQ(model->capability_definition_revision,
            "sha256:f900f43ad7bbc61f49c09295a2a63e5fabe93eb57bc9c5f44c26e3c4eeeac158");

  const auto* bridge =
      find_builtin_capability_definition("assistant.bridge.request");
  ASSERT_NE(bridge, nullptr);
  EXPECT_EQ(bridge->scope, CapabilityScope::kCellGlobal);
  EXPECT_EQ(bridge->provider_binding_kind,
            CapabilityProviderBindingKind::kCapabilityProviderBinding);
}

}  // namespace
}  // namespace manyforge::core
