#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>

#include "../src/component_lifecycle_journal_codec_internal.hpp"

namespace manyforge::core {
namespace {

constexpr std::int64_t kAcceptedAtNs = 1'000'000;
constexpr std::int64_t kTimeoutNs = 5'000'000;

ExpectedActiveControlConfigurationDTO absent_configuration() {
  return {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
}

ExpectedActiveControlConfigurationDTO present_configuration() {
  return {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
          ActiveControlConfigurationId("configuration-1"),
          ActiveControlConfigurationGeneration(7U)};
}

ExpectedControlBindingRevisionDTO binding(std::string id = "binding-1",
                                          std::string route = "shared-route") {
  return {ControlBindingId(std::move(id)), ControlBindingRevision("binding-revision-1"),
          ControlRouteId(std::move(route))};
}

ExpectedControlDomainRevisionDTO expected_domain(
    std::string id = "domain-1",
    ExpectedActiveControlConfigurationDTO configuration = present_configuration(),
    std::vector<ExpectedControlBindingRevisionDTO> bindings = {binding()}) {
  return {ControlDomainId(std::move(id)),     ControlDomainRevision("domain-revision-1"),
          ControllerTopologyId("topology-1"), TopologyRevision("topology-revision-1"),
          std::move(configuration),           std::move(bindings)};
}

ActiveControlConfigurationDTO active_configuration(
    const ExpectedControlDomainRevisionDTO& expected) {
  std::vector<ActiveControlConfigurationBindingRouteDTO> routes;
  for (const auto& selected : expected.controller().expected_control_bindings) {
    routes.push_back({selected.control_binding_id, selected.control_binding_revision,
                      selected.control_route_id});
  }
  return {ActiveControlConfigurationId("configuration-1"),
          expected.control_domain_id,
          expected.control_domain_revision,
          std::move(routes),
          ActivationSetId("activation-set-1"),
          expected.controller().controller_topology_id,
          expected.controller().topology_revision,
          RuntimeUnitId("runtime-1"),
          RuntimeEpoch("runtime-epoch-1"),
          1U,
          {},
          ActiveControlConfigurationGeneration(7U),
          ActiveControlConfigurationStateDTO::kReconciled};
}

ResolvedLifecycleControlDomainScopeDTO current_scope(
    const ExpectedControlDomainRevisionDTO& expected,
    std::vector<ResourceId> resources = {ResourceId("resource-1")}) {
  std::optional<ActiveControlConfigurationDTO> configuration;
  if (expected.controller().expected_active_control_configuration.presence ==
      ExpectedActiveControlConfigurationPresenceDTO::kPresent) {
    configuration = active_configuration(expected);
  }
  return {expected.control_domain_id,
          expected.control_domain_revision,
          expected.controller().controller_topology_id,
          expected.controller().topology_revision,
          std::move(configuration),
          expected.controller().expected_control_bindings,
          std::move(resources)};
}

ResolvedLifecycleProviderOccurrenceDTO provider_occurrence(
    const ResolvedLifecycleControlDomainScopeDTO& scope) {
  return {scope.control_domain_id,
          scope.control_domain_revision,
          ProviderInstanceId("provider-1"),
          ProviderManifestId("manifest-1"),
          ProviderManifestRevision("manifest-revision-1"),
          RuntimeUnitId("runtime-1"),
          RuntimeEpoch("runtime-epoch-1"),
          ContentDigest("catalog-digest-1")};
}

ComponentLifecycleIntentDTO intent(
    ComponentLifecycleIntentKindDTO kind = ComponentLifecycleIntentKindDTO::kPrepare,
    std::vector<ExpectedControlDomainRevisionDTO> domains = {expected_domain()}) {
  return {{RequestId("request-1"), RequestOwnerEpoch("owner-epoch-1"), "test", std::nullopt},
          ControllableComponentId("component-1"),
          kind,
          GraphRevision("graph-revision-1"),
          ComponentLifecycleGeneration(3U),
          std::move(domains),
          std::string("operator request"),
          {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt}};
}

ComponentLifecycleAcceptedDTO acceptance() {
  return {RequestId("request-1"),
          RequestReplayStatusDTO::kAcceptedNew,
          LifecycleTransactionId("transaction-1"),
          LifecycleTransactionGeneration(4U),
          ComponentLifecycleGeneration(4U),
          kAcceptedAtNs,
          kTimeoutNs};
}

ComponentLifecycleRequestedStateDTO resolved_requested(ComponentLifecycleStateDTO state) {
  return {ComponentLifecycleRequestedStatePresenceDTO::kResolved, state};
}

ComponentLifecycleRequestedStateDTO unresolved_requested() {
  return {ComponentLifecycleRequestedStatePresenceDTO::kUnresolved, std::nullopt};
}

ActiveBundlePublicationReceiptDTO active_publication_receipt() {
  const auto digest = std::string("sha256:") + std::string(64U, 'a');
  const ProjectionRevisionDTO revision{RetainedBundleId("retained-bundle-1"),
                                       GraphRevision("graph-revision-1"),
                                       {SceneId(digest), 1U, ContentDigest(digest)}};
  const ActiveBundlePublicationRequestDTO request{
      ActiveBundlePublicationTransactionRefDTO{
          BootstrapPublicationTransactionDTO{BootstrapId("bootstrap-1"), BootstrapGeneration(1U)}},
      revision,
      revision.retained_bundle_id,
      revision.graph_revision,
      std::nullopt,
      ContentDigest("private-materialization-1")};
  return {request, ActiveBundlePublicationGeneration(1U), ContentDigest("public-pointer-1")};
}

LifecyclePredicateDTO predicate(ComponentLifecycleStateDTO state,
                                std::vector<LifecycleFactPredicateDTO> facts = {}) {
  return {{state}, std::move(facts)};
}

BrandLifecycleObservationQueryDTO observation_query(std::string domain_id = "domain-1") {
  return {LifecycleTransactionId("transaction-1"),
          LifecycleTransactionGeneration(4U),
          ControllableComponentId("component-1"),
          ControlDomainId(std::move(domain_id)),
          ControlDomainRevision("domain-revision-1"),
          ProviderInstanceId("provider-1"),
          ProviderManifestId("manifest-1"),
          ProviderManifestRevision("manifest-revision-1"),
          RuntimeUnitId("runtime-1"),
          RuntimeEpoch("runtime-epoch-1")};
}

BrandLifecycleObservationDTO observation() {
  const auto query = observation_query();
  return {query.lifecycle_transaction_id,
          query.lifecycle_transaction_generation,
          query.controllable_component_id,
          query.control_domain_id,
          query.control_domain_revision,
          query.provider_instance_id,
          query.provider_manifest_id,
          query.provider_manifest_revision,
          query.provider_runtime_unit_id,
          query.provider_runtime_epoch,
          8U,
          5U,
          100,
          110,
          200,
          ComponentLifecycleStateDTO::kPrepared,
          {{LifecycleFactId("enabled"),
            {LifecycleFactValueKindDTO::kBoolean, true, std::nullopt, std::nullopt}},
           {LifecycleFactId("mode"),
            {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
             std::string("automatic")}}}};
}

BrandLifecyclePrimitiveIntentDTO primitive_intent() {
  const auto query = observation_query();
  return {query.lifecycle_transaction_id,
          query.lifecycle_transaction_generation,
          query.controllable_component_id,
          query.control_domain_id,
          query.control_domain_revision,
          query.provider_instance_id,
          query.provider_manifest_id,
          query.provider_manifest_revision,
          query.provider_runtime_unit_id,
          query.provider_runtime_epoch,
          LifecyclePrimitiveId("enable-control"),
          LifecyclePrimitiveAttemptId("attempt-1"),
          5U,
          1'000'000};
}

BrandLifecyclePrimitiveResultDTO known_success_result() {
  const auto primitive = primitive_intent();
  return {primitive.lifecycle_transaction_id,
          primitive.lifecycle_transaction_generation,
          primitive.controllable_component_id,
          primitive.control_domain_id,
          primitive.control_domain_revision,
          primitive.provider_instance_id,
          primitive.provider_manifest_id,
          primitive.provider_manifest_revision,
          primitive.provider_runtime_unit_id,
          primitive.provider_runtime_epoch,
          primitive.lifecycle_primitive_id,
          primitive.lifecycle_primitive_attempt_id,
          primitive.expected_observation_generation,
          LifecyclePrimitiveDispatchStateDTO::kDispatchedKnown,
          LifecyclePrimitiveEffectDTO::kObserved,
          LifecyclePrimitiveKnownResultDTO::kSucceeded,
          std::nullopt};
}

ComponentLifecycleStatusDTO prepared_recovery_status() {
  const auto lifecycle_intent = intent();
  const auto requested = resolved_requested(ComponentLifecycleStateDTO::kPrepared);
  const auto scope = current_scope(lifecycle_intent.expected_control_domains.front());
  ComponentLifecycleDomainStatusDTO domain{scope.control_domain_id,
                                           lifecycle_intent.expected_control_domains.front(),
                                           scope,
                                           provider_occurrence(scope),
                                           ComponentLifecycleStateDTO::kDeclared,
                                           ComponentLifecycleStateDTO::kDeclared,
                                           requested,
                                           ComponentLifecycleDomainActionStateDTO::kExecuting,
                                           ComponentLifecycleDomainEffectDTO::kNone,
                                           {},
                                           std::nullopt,
                                           std::nullopt};
  return {acceptance(),
          lifecycle_intent,
          2U,
          lifecycle_intent.request_context.request_owner_epoch,
          ComponentLifecycleStateDTO::kDeclared,
          ComponentLifecycleStateDTO::kDeclared,
          requested,
          ComponentLifecycleTransactionPhaseDTO::kExecuting,
          {std::move(domain)},
          false,
          std::nullopt,
          std::nullopt,
          kAcceptedAtNs,
          kAcceptedAtNs + 10,
          kTimeoutNs - 10};
}

CCLCEffectAttemptDTO prepared_recovery_attempt() {
  return {CCLCRequestKindDTO::kComponentLifecycle,
          LifecycleTransactionId("transaction-1"),
          LifecycleTransactionGeneration(4U),
          1U,
          CCLCEffectKindDTO::kBrandLifecyclePrimitive,
          CCLCEffectAttemptStateDTO::kPrepared,
          primitive_intent(),
          std::nullopt};
}

ComponentLifecycleDomainStatusDTO successful_domain_status(
    const ExpectedControlDomainRevisionDTO& expected,
    ComponentLifecycleRequestedStateDTO requested) {
  const auto scope = current_scope(expected);
  return {expected.control_domain_id,
          expected,
          scope,
          provider_occurrence(scope),
          ComponentLifecycleStateDTO::kDeclared,
          *requested.state,
          requested,
          ComponentLifecycleDomainActionStateDTO::kSucceeded,
          ComponentLifecycleDomainEffectDTO::kKnownNoEffect,
          {},
          std::nullopt,
          std::nullopt};
}

ComponentLifecycleStatusDTO selected_bring_up_status() {
  auto status = prepared_recovery_status();
  status.acceptance.lifecycle_transaction_id = LifecycleTransactionId("selected-lifecycle.4");
  status.accepted_intent.intent = ComponentLifecycleIntentKindDTO::kEnable;
  status.requested_state = resolved_requested(ComponentLifecycleStateDTO::kActive);
  status.domains.front().requested_state = status.requested_state;
  // Prepared is an actual predecessor observation; no fabricated Faulted state.
  status.previous_state = ComponentLifecycleStateDTO::kPrepared;
  status.current_state = ComponentLifecycleStateDTO::kPrepared;
  status.domains.front().previous_state = ComponentLifecycleStateDTO::kPrepared;
  status.domains.front().current_state = ComponentLifecycleStateDTO::kPrepared;
  const ContentDigest digest(std::string("sha256:") + std::string(64U, 'a'));
  CellAdmissionClaimIntentDTO claim{
      CellAdmissionClaimKindDTO::kInterventionOrRecovery,
      CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
      status.accepted_intent.request_context.request_owner_epoch,
      status.acceptance.lifecycle_transaction_id.value(), status.acceptance.request_id,
      digest, status.accepted_intent.expected_admission_generation,
      {{ResourceId("resource-1")}, {{ControlDomainId("domain-1"),
          ActiveControlConfigurationId("configuration-1"), ActiveControlConfigurationGeneration(7U)}}},
      std::nullopt,
      CellAdmissionInterventionEvidenceDTO{CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
          status.accepted_intent.request_context.request_owner_epoch,
          status.acceptance.lifecycle_transaction_id.value(), digest, std::nullopt},
      std::nullopt};
  status.selected_bring_up = SelectedComponentBringUpEvidenceDTO{
      digest, digest, "incident-1", StopId("stop-1"), StopGeneration(9U),
      {RetainedBundleId("candidate-bundle"), GraphRevision("candidate-graph"),
       {SceneId(digest.value()), 1U, digest}}, digest, 1024U,
      {ExecutionBindingEpoch(12U), {{ControlDomainId("domain-1"), 30U}}},
      {ExecutionBindingEpoch(13U), {{ControlDomainId("domain-1"), 31U}}},
      {claim, CellAdmissionClaimGeneration(15U), claim.expected_admission_generation, kAcceptedAtNs},
      {ConfigurationFenceId("fence-1"), ConfigurationFenceGeneration(22U),
       {{ControllableComponentId("component-1")},
        {{ControlDomainId("domain-1"), ControlDomainRevision("domain-revision-1"),
           ControlDomainRevision("domain-revision-1")}},
        {{ControlBindingId("binding-1"), ControlBindingRevision("binding-revision-1"),
           ControlBindingRevision("binding-revision-1")}}, {ResourceId("resource-1")}},
       kAcceptedAtNs, RequestReplayStatusDTO::kAcceptedNew},
      ContentDigest(std::string("sha256:") + std::string(64U, 'b'))};
  return status;
}

ComponentLifecycleStatusDTO apply_bring_up_status(
    ComponentLifecycleIntentKindDTO kind = ComponentLifecycleIntentKindDTO::kPrepare,
    bool recovery = false) {
  auto status = prepared_recovery_status();
  status.acceptance.lifecycle_transaction_id = LifecycleTransactionId("apply-component.4");
  status.accepted_intent.intent = kind;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kAccepted;
  status.requested_state = resolved_requested(kind == ComponentLifecycleIntentKindDTO::kDisable
      ? ComponentLifecycleStateDTO::kDisabled
      : (kind == ComponentLifecycleIntentKindDTO::kPrepare
          ? ComponentLifecycleStateDTO::kPrepared : ComponentLifecycleStateDTO::kActive));
  status.domains.front().requested_state = status.requested_state;
  status.domains.front().action_state = ComponentLifecycleDomainActionStateDTO::kReserved;
  const ContentDigest digest(std::string("sha256:") + std::string(64U, 'a'));
  const ContentDigest plan_digest(std::string("sha256:") + std::string(64U, 'c'));
  const ConfigurationFenceScopeDTO scope{
      {ControllableComponentId("component-1")},
      {{ControlDomainId("domain-1"), ControlDomainRevision("domain-revision-1"),
         ControlDomainRevision("domain-revision-1")}},
      {{ControlBindingId("binding-1"), ControlBindingRevision("binding-revision-1"),
         ControlBindingRevision("binding-revision-1")}}, {ResourceId("resource-1")}};
  const ApplyFenceOwnerDTO parent_owner{ApplyId("apply-1"), ApplyGeneration(8U)};
  const ApplyComponentBringUpReferenceDTO parent{parent_owner, plan_digest};
  const CellAdmissionClaimIntentDTO parent_intent{
      CellAdmissionClaimKindDTO::kManagerApply, CellAdmissionOwnerRoleDTO::kManager,
      RequestOwnerEpoch("manager-owner"), "apply-1", RequestId("parent-request"),
      plan_digest, AdmissionGeneration(9U), {{}, {}}, std::nullopt, std::nullopt, std::nullopt};
  status.apply_bring_up = ApplyComponentBringUpEvidenceDTO{
      {parent, {parent_intent, CellAdmissionClaimGeneration(17U), AdmissionGeneration(9U), kAcceptedAtNs},
       {parent.owner, ConfigurationFenceId("parent-fence"), ConfigurationFenceGeneration(22U),
        scope, RuntimeEpoch("manager-runtime-epoch")}},
      scope, digest,
      {RetainedBundleId("candidate-bundle"), GraphRevision("candidate-graph"),
       {SceneId(digest.value()), 1U, digest}}, digest, 1024U,
      {RuntimeUnitId("runtime-1"), RuntimeEpoch("runtime-epoch-1")},
      std::nullopt, std::nullopt, std::nullopt};
  auto& evidence = *status.apply_bring_up;
  if (recovery) {
    evidence.recovery = ApplyComponentBringUpRecoveryEvidenceDTO{
        digest, "incident-1", StopId("stop-1"), StopGeneration(9U)};
    evidence.predecessor_occurrence = SelectedControllerOccurrenceDTO{
        ExecutionBindingEpoch(12U), {{ControlDomainId("domain-1"), 30U}}};
    evidence.successor_occurrence_floor = SelectedControllerOccurrenceDTO{
        ExecutionBindingEpoch(13U), {{ControlDomainId("domain-1"), 31U}}};
  }
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      evidence.original_execution_host.runtime_unit_id, evidence.original_execution_host.runtime_epoch,
      evidence.parent_authority.held_fence_query.manager_runtime_epoch, evidence.parent_authority.parent);
  return status;
}

ComponentLifecycleStatusDTO ordinary_codec_status() {
  const auto lifecycle_intent = intent(ComponentLifecycleIntentKindDTO::kDisable,
                                      {expected_domain("domain-1", absent_configuration())});
  const auto requested = resolved_requested(ComponentLifecycleStateDTO::kDisabled);
  return {acceptance(), lifecycle_intent, 1U, lifecycle_intent.request_context.request_owner_epoch,
          std::nullopt, std::nullopt, requested, ComponentLifecycleTransactionPhaseDTO::kAccepted,
          {{ControlDomainId("domain-1"), lifecycle_intent.expected_control_domains.front(),
            std::nullopt, std::nullopt, std::nullopt, std::nullopt, requested,
            ComponentLifecycleDomainActionStateDTO::kReserved,
            ComponentLifecycleDomainEffectDTO::kNone, {}, std::nullopt, std::nullopt}},
          false, std::nullopt, std::nullopt, kAcceptedAtNs, kAcceptedAtNs, kTimeoutNs};
}

TEST(ComponentLifecycleStrongIds, PreserveSemanticSeparation) {
  static_assert(!std::is_same_v<LifecyclePrimitiveId, LifecyclePrimitiveAttemptId>);
  static_assert(!std::is_same_v<LifecycleFactId, LifecyclePrimitiveId>);
  static_assert(!std::is_same_v<ComponentLifecycleGeneration, LifecycleTransactionGeneration>);
}

TEST(ComponentLifecycleExpectation, RepresentsFirstPrepareWithoutSentinel) {
  EXPECT_TRUE(absent_configuration().locally_valid());
  EXPECT_TRUE(present_configuration().locally_valid());

  auto invalid = absent_configuration();
  invalid.active_control_configuration_id = ActiveControlConfigurationId("sentinel-is-not-absence");
  EXPECT_FALSE(invalid.locally_valid());

  const auto first = expected_domain("domain-1", absent_configuration());
  EXPECT_TRUE(first.locally_valid());
  EXPECT_TRUE(intent(ComponentLifecycleIntentKindDTO::kPrepare, {first}).locally_valid());
}

TEST(ComponentLifecycleExpectation,
     AllowsOneRouteForSeveralBindingsButRequiresCanonicalBindingIds) {
  const auto domain =
      expected_domain("domain-1", present_configuration(),
                      {binding("binding-1", "shared-route"), binding("binding-2", "shared-route")});
  EXPECT_TRUE(domain.locally_valid());

  auto noncanonical = domain;
  std::swap(noncanonical.controller().expected_control_bindings[0], noncanonical.controller().expected_control_bindings[1]);
  EXPECT_FALSE(noncanonical.locally_valid());

  auto duplicate = domain;
  duplicate.controller().expected_control_bindings[1] = duplicate.controller().expected_control_bindings[0];
  EXPECT_FALSE(duplicate.locally_valid());
}

TEST(ComponentLifecycleIntent, AttributionDoesNotChangeReplayMeaning) {
  auto first = intent();
  auto second = first;
  second.attribution = {RequestTransportDTO::kHttp, RequestAssuranceDTO::kExternallyAuthenticated,
                        std::string("operator")};

  EXPECT_TRUE(first.locally_valid());
  EXPECT_TRUE(second.locally_valid());
  EXPECT_NE(first, second);
  EXPECT_TRUE(first.replay_equal(second));

  second.reason = "different request";
  EXPECT_FALSE(first.replay_equal(second));
}

TEST(ComponentLifecycleFacts, EnforcesClosedValuePresenceAndPortableTokens) {
  EXPECT_TRUE((LifecycleFactValueDTO{LifecycleFactValueKindDTO::kUnknown, std::nullopt,
                                     std::nullopt, std::nullopt}
                   .locally_valid()));
  EXPECT_TRUE(
      (LifecycleFactValueDTO{LifecycleFactValueKindDTO::kInteger, std::nullopt, -4, std::nullopt}
           .locally_valid()));
  EXPECT_TRUE((LifecycleFactValueDTO{LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
                                     std::string("mode.ready-1")}
                   .locally_valid()));
  EXPECT_FALSE(
      (LifecycleFactValueDTO{LifecycleFactValueKindDTO::kUnknown, false, std::nullopt, std::nullopt}
           .locally_valid()));
  EXPECT_FALSE((LifecycleFactValueDTO{LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
                                      std::string("not portable")}
                    .locally_valid()));
}

TEST(ComponentLifecyclePredicate, DistinguishesSatisfiedNotSatisfiedAndInconclusiveWithoutEffects) {
  const LifecyclePredicateDTO requires_prepared_automatic{
      {ComponentLifecycleStateDTO::kPrepared},
      {{LifecycleFactId("mode"),
        {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
         std::string("automatic")}}}};
  auto sample = observation();
  ASSERT_TRUE(requires_prepared_automatic.locally_valid());
  ASSERT_TRUE(sample.locally_valid());
  EXPECT_EQ(EvaluateLifecyclePredicate(requires_prepared_automatic, sample),
            LifecyclePostconditionResultDTO::kSatisfied);

  sample.observed_state = ComponentLifecycleStateDTO::kActive;
  EXPECT_EQ(EvaluateLifecyclePredicate(requires_prepared_automatic, sample),
            LifecyclePostconditionResultDTO::kNotSatisfied);

  sample = observation();
  sample.facts.erase(sample.facts.begin() + 1);
  ASSERT_TRUE(sample.locally_valid());
  EXPECT_EQ(EvaluateLifecyclePredicate(requires_prepared_automatic, sample),
            LifecyclePostconditionResultDTO::kInconclusive);

  sample = observation();
  sample.facts[1].value.token_value = "manual";
  ASSERT_TRUE(sample.locally_valid());
  EXPECT_EQ(EvaluateLifecyclePredicate(requires_prepared_automatic, sample),
            LifecyclePostconditionResultDTO::kNotSatisfied);

  sample = observation();
  sample.facts[1].value = {LifecycleFactValueKindDTO::kUnknown, std::nullopt, std::nullopt,
                           std::nullopt};
  ASSERT_TRUE(sample.locally_valid());
  EXPECT_EQ(EvaluateLifecyclePredicate(requires_prepared_automatic, sample),
            LifecyclePostconditionResultDTO::kInconclusive);
}

TEST(ComponentLifecyclePlanCatalog, RequiresCanonicalManifestScopedPredicatesAndUniqueTransitions) {
  const LifecycleFactPredicateDTO automatic{
      LifecycleFactId("mode"),
      {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt, std::string("automatic")}};
  const LifecyclePrimitiveTransitionDTO transition{
      LifecyclePrimitiveId("prepare-control"),
      predicate(ComponentLifecycleStateDTO::kDeclared, {automatic}),
      predicate(ComponentLifecycleStateDTO::kPrepared, {automatic})};
  const LifecycleIntentPlanDTO prepare{
      ComponentLifecycleIntentKindDTO::kPrepare,
      ComponentLifecycleStateDTO::kPrepared,
      std::nullopt,
      predicate(ComponentLifecycleStateDTO::kPrepared, {automatic}),
      1'000'000,
      {transition}};
  BrandLifecyclePlanCatalogDTO catalog{ProviderManifestId("manifest-1"),
                                       ProviderManifestRevision("manifest-revision-1"),
                                       ContentDigest("catalog-digest-1"),
                                       {prepare}};

  ASSERT_TRUE(automatic.locally_valid());
  ASSERT_TRUE(prepare.locally_valid());
  ASSERT_TRUE(catalog.locally_valid());

  auto duplicate = catalog;
  duplicate.plans.front().transitions.push_back(transition);
  EXPECT_FALSE(duplicate.locally_valid());

  auto unknown_predicate = catalog;
  unknown_predicate.plans.front().requested_postcondition.fact_predicates.front().expected_value = {
      LifecycleFactValueKindDTO::kUnknown, std::nullopt, std::nullopt, std::nullopt};
  EXPECT_FALSE(unknown_predicate.locally_valid());

  auto recover_without_target = prepare;
  recover_without_target.intent = ComponentLifecycleIntentKindDTO::kRecover;
  recover_without_target.requested_state = ComponentLifecycleStateDTO::kDeclared;
  EXPECT_FALSE(recover_without_target.locally_valid());
}

TEST(ComponentLifecycleObservation, RequiresCanonicalFreshFactsAndExactProviderRuntimeEcho) {
  const auto query = observation_query();
  auto sample = observation();
  ASSERT_TRUE(sample.locally_valid());
  EXPECT_TRUE(sample.matches(query));

  auto wrong_epoch = query;
  wrong_epoch.provider_runtime_epoch = RuntimeEpoch("replacement");
  EXPECT_FALSE(sample.matches(wrong_epoch));

  auto regressed_time = sample;
  regressed_time.fresh_until_ns = regressed_time.received_timestamp_ns;
  EXPECT_FALSE(regressed_time.locally_valid());

  auto noncanonical = sample;
  std::swap(noncanonical.facts[0], noncanonical.facts[1]);
  EXPECT_FALSE(noncanonical.locally_valid());
}

TEST(ComponentLifecyclePrimitiveResult, DistinguishesNoDispatchKnownAndUncertainEffects) {
  const auto primitive = primitive_intent();
  auto known = known_success_result();
  ASSERT_TRUE(known.locally_valid());
  EXPECT_TRUE(known.matches(primitive));

  auto refused = known;
  refused.dispatch_state = LifecyclePrimitiveDispatchStateDTO::kNotDispatched;
  refused.effect = LifecyclePrimitiveEffectDTO::kNone;
  refused.known_result.reset();
  refused.error = ErrorDTO{"primitive_refused", "not accepted", {}};
  EXPECT_TRUE(refused.locally_valid());

  auto uncertain = refused;
  uncertain.dispatch_state = LifecyclePrimitiveDispatchStateDTO::kDispatchedUncertain;
  uncertain.effect = LifecyclePrimitiveEffectDTO::kPossible;
  uncertain.error = ErrorDTO{"dispatch_uncertain", "response lost", {}};
  EXPECT_TRUE(uncertain.locally_valid());

  uncertain.effect = LifecyclePrimitiveEffectDTO::kNone;
  EXPECT_FALSE(uncertain.locally_valid());

  refused.error.reset();
  EXPECT_FALSE(refused.locally_valid());

  known.provider_manifest_revision = ProviderManifestRevision("stale");
  EXPECT_TRUE(known.locally_valid());
  EXPECT_FALSE(known.matches(primitive));
}

TEST(ComponentLifecycleResolvedScope, MatchesExactTopologyBindingRouteAndConfiguration) {
  const auto expected = expected_domain();
  auto resolved = current_scope(expected);
  ASSERT_TRUE(resolved.locally_valid());
  EXPECT_TRUE(ExpectedDomainMatchesResolvedScope(expected, resolved));

  resolved.controller().current_control_bindings[0].control_route_id = ControlRouteId("other-route");
  EXPECT_FALSE(resolved.locally_valid());

  const auto first_expected = expected_domain("domain-1", absent_configuration());
  const auto first_resolved = current_scope(first_expected);
  ASSERT_TRUE(first_resolved.locally_valid());
  EXPECT_TRUE(ExpectedDomainMatchesResolvedScope(first_expected, first_resolved));
}

TEST(ComponentLifecycleCurrentSnapshot, JoinsOneCanonicalProviderOccurrenceToEveryCompleteDomain) {
  const auto expected = expected_domain();
  const auto scope = current_scope(expected);
  const ResolvedLifecycleProviderOccurrenceDTO provider{
      expected.control_domain_id,
      expected.control_domain_revision,
      ProviderInstanceId("provider-1"),
      ProviderManifestId("manifest-1"),
      ProviderManifestRevision("manifest-revision-1"),
      RuntimeUnitId("runtime-1"),
      RuntimeEpoch("runtime-epoch-1"),
      ContentDigest("catalog-digest-1")};
  ComponentLifecycleCurrentSnapshotDTO snapshot{ContentDigest("snapshot-digest-1"),
                                                ControllableComponentId("component-1"),
                                                ComponentLifecycleGeneration(4U),
                                                ComponentLifecycleStateDTO::kPrepared,
                                                active_publication_receipt(),
                                                {scope},
                                                {provider}};

  ASSERT_TRUE(snapshot.locally_valid());

  auto missing_provider = snapshot;
  missing_provider.provider_occurrences.clear();
  EXPECT_FALSE(missing_provider.locally_valid());

  auto wrong_domain = snapshot;
  wrong_domain.provider_occurrences.front().control_domain_id = ControlDomainId("other-domain");
  EXPECT_FALSE(wrong_domain.locally_valid());
}

TEST(ComponentLifecycleCurrentSnapshot, CanonicalDigestCoversPublicationScopeAndProviderTruth) {
  const auto expected = expected_domain();
  const auto scope = current_scope(expected);
  const ResolvedLifecycleProviderOccurrenceDTO provider{
      expected.control_domain_id,
      expected.control_domain_revision,
      ProviderInstanceId("provider-1"),
      ProviderManifestId("manifest-1"),
      ProviderManifestRevision("manifest-revision-1"),
      RuntimeUnitId("runtime-1"),
      RuntimeEpoch("runtime-epoch-1"),
      ContentDigest("catalog-digest-1")};
  ComponentLifecycleCurrentSnapshotDTO snapshot{ContentDigest("pending"),
                                                ControllableComponentId("component-1"),
                                                ComponentLifecycleGeneration(4U),
                                                ComponentLifecycleStateDTO::kPrepared,
                                                active_publication_receipt(),
                                                {scope},
                                                {provider}};

  const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot);
  ASSERT_TRUE(digest.ok());
  snapshot.snapshot_digest = digest.value();
  EXPECT_TRUE(VerifyComponentLifecycleCurrentSnapshot(snapshot).ok());

  auto changed_publication = snapshot;
  changed_publication.active_publication_receipt.publication_generation =
      ActiveBundlePublicationGeneration(2U);
  EXPECT_FALSE(VerifyComponentLifecycleCurrentSnapshot(changed_publication).ok());

  auto changed_scope = snapshot;
  changed_scope.domains.front().controller().current_resources.front() = ResourceId("resource-2");
  EXPECT_FALSE(VerifyComponentLifecycleCurrentSnapshot(changed_scope).ok());

  auto changed_provider = snapshot;
  changed_provider.provider_occurrences.front().provider_runtime_epoch =
      RuntimeEpoch("runtime-epoch-2");
  EXPECT_FALSE(VerifyComponentLifecycleCurrentSnapshot(changed_provider).ok());
}

TEST(ComponentLifecycleStatus, AcceptsAlreadySatisfiedSuccessWithoutPrimitiveAttempt) {
  const auto lifecycle_intent = intent();
  const auto requested = resolved_requested(ComponentLifecycleStateDTO::kPrepared);
  ComponentLifecycleStatusDTO status{
      acceptance(),
      lifecycle_intent,
      1U,
      lifecycle_intent.request_context.request_owner_epoch,
      ComponentLifecycleStateDTO::kDeclared,
      ComponentLifecycleStateDTO::kPrepared,
      requested,
      ComponentLifecycleTransactionPhaseDTO::kSucceeded,
      {successful_domain_status(lifecycle_intent.expected_control_domains[0], requested)},
      false,
      ComponentLifecycleTerminalResultDTO::kSucceeded,
      std::nullopt,
      kAcceptedAtNs,
      kAcceptedAtNs + 10,
      0};

  EXPECT_TRUE(status.locally_valid());
  EXPECT_TRUE(status.domains[0].primitive_attempts.empty());

  status.acceptance.component_lifecycle_generation = ComponentLifecycleGeneration(5U);
  EXPECT_FALSE(status.locally_valid());
  status.acceptance.component_lifecycle_generation = ComponentLifecycleGeneration(4U);

  status.domains[0].current_state = ComponentLifecycleStateDTO::kDeclared;
  EXPECT_FALSE(status.locally_valid());
  status.domains[0].current_state = ComponentLifecycleStateDTO::kPrepared;
  status.domains[0].effect = ComponentLifecycleDomainEffectDTO::kEffectUncertain;
  EXPECT_FALSE(status.locally_valid());
}

TEST(ComponentLifecycleStatus, RecoverMayFailUnresolvedOnlyWithKnownNoEffect) {
  const auto lifecycle_intent = intent(ComponentLifecycleIntentKindDTO::kRecover);
  const auto requested = unresolved_requested();
  const auto scope = current_scope(lifecycle_intent.expected_control_domains[0]);
  ComponentLifecycleDomainStatusDTO domain{
      lifecycle_intent.expected_control_domains[0].control_domain_id,
      lifecycle_intent.expected_control_domains[0],
      scope,
      provider_occurrence(scope),
      ComponentLifecycleStateDTO::kFaulted,
      ComponentLifecycleStateDTO::kFaulted,
      requested,
      ComponentLifecycleDomainActionStateDTO::kFailed,
      ComponentLifecycleDomainEffectDTO::kNone,
      {},
      std::nullopt,
      ErrorDTO{"recover_target_ambiguous", "no unique plan", {}}};
  ComponentLifecycleStatusDTO status{acceptance(),
                                     lifecycle_intent,
                                     1U,
                                     lifecycle_intent.request_context.request_owner_epoch,
                                     ComponentLifecycleStateDTO::kFaulted,
                                     ComponentLifecycleStateDTO::kFaulted,
                                     requested,
                                     ComponentLifecycleTransactionPhaseDTO::kFailed,
                                     {domain},
                                     false,
                                     ComponentLifecycleTerminalResultDTO::kFailedKnownNoEffect,
                                     ErrorDTO{"recover_target_ambiguous", "no unique plan", {}},
                                     kAcceptedAtNs,
                                     kAcceptedAtNs + 10,
                                     0};

  EXPECT_TRUE(status.locally_valid());

  status.domains[0].effect = ComponentLifecycleDomainEffectDTO::kEffectObserved;
  EXPECT_FALSE(status.locally_valid());
}

TEST(ComponentLifecycleStatus, DisableUsesExpectedAndCurrentDomainUnionWithoutWideningIdleRows) {
  const auto expected_a = expected_domain("domain-a");
  const auto expected_b =
      expected_domain("domain-b", present_configuration(), {binding("binding-b", "shared-route")});
  const auto current_c_expected =
      expected_domain("domain-c", present_configuration(), {binding("binding-c", "shared-route")});
  auto disable_intent = intent(ComponentLifecycleIntentKindDTO::kDisable, {expected_a, expected_b});
  const auto requested = resolved_requested(ComponentLifecycleStateDTO::kDisabled);

  const auto status_row = [&](const ControlDomainId& id,
                              std::optional<ExpectedControlDomainRevisionDTO> expected,
                              std::optional<ResolvedLifecycleControlDomainScopeDTO> current) {
    std::optional<ResolvedLifecycleProviderOccurrenceDTO> provider;
    if (current.has_value()) {
      provider = provider_occurrence(*current);
    }
    const std::optional<ComponentLifecycleStateDTO> observed_state =
        current.has_value() ? std::optional(ComponentLifecycleStateDTO::kActive) : std::nullopt;
    return ComponentLifecycleDomainStatusDTO{id,
                                             std::move(expected),
                                             std::move(current),
                                             std::move(provider),
                                             observed_state,
                                             observed_state,
                                             requested,
                                             ComponentLifecycleDomainActionStateDTO::kReserved,
                                             ComponentLifecycleDomainEffectDTO::kNone,
                                             {},
                                             std::nullopt,
                                             std::nullopt};
  };

  ComponentLifecycleStatusDTO status{
      acceptance(),
      disable_intent,
      1U,
      disable_intent.request_context.request_owner_epoch,
      ComponentLifecycleStateDTO::kActive,
      ComponentLifecycleStateDTO::kDisabling,
      requested,
      ComponentLifecycleTransactionPhaseDTO::kAccepted,
      {status_row(expected_a.control_domain_id, expected_a, current_scope(expected_a)),
       status_row(expected_b.control_domain_id, expected_b, std::nullopt),
       status_row(current_c_expected.control_domain_id, std::nullopt,
                  current_scope(current_c_expected))},
      false,
      std::nullopt,
      std::nullopt,
      kAcceptedAtNs,
      kAcceptedAtNs,
      kTimeoutNs};
  EXPECT_TRUE(status.locally_valid());

  status.acceptance.component_lifecycle_generation = ComponentLifecycleGeneration(6U);
  EXPECT_TRUE(status.locally_valid());
  status.acceptance.component_lifecycle_generation = ComponentLifecycleGeneration(3U);
  EXPECT_FALSE(status.locally_valid());
  status.acceptance.component_lifecycle_generation = ComponentLifecycleGeneration(4U);

  status.domains.erase(status.domains.begin() + 1);
  EXPECT_FALSE(status.locally_valid());
}

TEST(ComponentLifecycleStatus, UnknownDisableOutcomeRequiresReconciliation) {
  auto disable_intent = intent(ComponentLifecycleIntentKindDTO::kDisable, {expected_domain()});
  const auto requested = resolved_requested(ComponentLifecycleStateDTO::kDisabled);
  const ErrorDTO uncertainty{
      "disable_outcome_unknown", "uncertain Disable primitive requires reconciliation", {}};
  const auto scope = current_scope(disable_intent.expected_control_domains.front());
  ComponentLifecycleDomainStatusDTO domain{ControlDomainId("domain-1"),
                                           disable_intent.expected_control_domains.front(),
                                           scope,
                                           provider_occurrence(scope),
                                           ComponentLifecycleStateDTO::kActive,
                                           ComponentLifecycleStateDTO::kActive,
                                           requested,
                                           ComponentLifecycleDomainActionStateDTO::kOutcomeUnknown,
                                           ComponentLifecycleDomainEffectDTO::kEffectUncertain,
                                           {},
                                           std::nullopt,
                                           uncertainty};
  ComponentLifecycleStatusDTO status{acceptance(),
                                     disable_intent,
                                     2U,
                                     RequestOwnerEpoch("replacement-owner-epoch"),
                                     ComponentLifecycleStateDTO::kActive,
                                     ComponentLifecycleStateDTO::kDisabling,
                                     requested,
                                     ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown,
                                     {domain},
                                     true,
                                     std::nullopt,
                                     uncertainty,
                                     kAcceptedAtNs,
                                     kAcceptedAtNs + 10,
                                     0};

  EXPECT_TRUE(status.locally_valid());
  status.reconciliation_required = false;
  EXPECT_FALSE(status.locally_valid());
}

TEST(ComponentLifecycleStatus, RequiresTheExactAcceptedProviderOccurrenceForCurrentScope) {
  const auto lifecycle_intent = intent();
  const auto requested = resolved_requested(ComponentLifecycleStateDTO::kPrepared);
  auto status = ComponentLifecycleStatusDTO{
      acceptance(),
      lifecycle_intent,
      1U,
      lifecycle_intent.request_context.request_owner_epoch,
      ComponentLifecycleStateDTO::kDeclared,
      ComponentLifecycleStateDTO::kPrepared,
      requested,
      ComponentLifecycleTransactionPhaseDTO::kSucceeded,
      {successful_domain_status(lifecycle_intent.expected_control_domains[0], requested)},
      false,
      ComponentLifecycleTerminalResultDTO::kSucceeded,
      std::nullopt,
      kAcceptedAtNs,
      kAcceptedAtNs + 10,
      0};
  ASSERT_TRUE(status.locally_valid());

  status.domains.front().resolved_provider_occurrence.reset();
  EXPECT_FALSE(status.locally_valid());
  status.domains.front() =
      successful_domain_status(lifecycle_intent.expected_control_domains[0], requested);
  status.domains.front().resolved_provider_occurrence->control_domain_revision =
      ControlDomainRevision("different-domain-revision");
  EXPECT_FALSE(status.locally_valid());
}

TEST(ComponentLifecycleStatus, UnresolvedScopeDisableDoesNotInventObservedState) {
  const auto lifecycle_intent =
      intent(ComponentLifecycleIntentKindDTO::kDisable, {expected_domain()});
  const auto requested = resolved_requested(ComponentLifecycleStateDTO::kDisabled);
  ComponentLifecycleDomainStatusDTO domain{
      lifecycle_intent.expected_control_domains.front().control_domain_id,
      lifecycle_intent.expected_control_domains.front(),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      requested,
      ComponentLifecycleDomainActionStateDTO::kReserved,
      ComponentLifecycleDomainEffectDTO::kNone,
      {},
      std::nullopt,
      std::nullopt};
  ComponentLifecycleStatusDTO status{acceptance(),
                                     lifecycle_intent,
                                     1U,
                                     lifecycle_intent.request_context.request_owner_epoch,
                                     std::nullopt,
                                     std::nullopt,
                                     requested,
                                     ComponentLifecycleTransactionPhaseDTO::kAccepted,
                                     {domain},
                                     false,
                                     std::nullopt,
                                     std::nullopt,
                                     kAcceptedAtNs,
                                     kAcceptedAtNs,
                                     kTimeoutNs};

  ASSERT_TRUE(status.locally_valid());
  status.current_state = ComponentLifecycleStateDTO::kActive;
  EXPECT_FALSE(status.locally_valid());
  status.current_state.reset();
  status.domains.front().current_state = ComponentLifecycleStateDTO::kActive;
  EXPECT_FALSE(status.locally_valid());
}

TEST(CCLCJournalRecovery, RequiresOneExactPreparedEffectAtTheTransactionWatermark) {
  const auto status = prepared_recovery_status();
  const auto prepared = prepared_recovery_attempt();
  CCLCJournalRecoveryDTO recovery{
      4U,
      {{ControllableComponentId("component-1"), ComponentLifecycleGeneration(4U)}},
      {status},
      {{status.acceptance.lifecycle_transaction_id,
        status.acceptance.lifecycle_transaction_generation, 1U}},
      {prepared},
      0U,
      {},
      {},
      {},
      {},
      {},
      {},
      RequestOwnerEpoch("owner-epoch-1")};
  ASSERT_TRUE(recovery.locally_valid());

  auto completed = prepared;
  completed.state = CCLCEffectAttemptStateDTO::kCompleted;
  completed.lifecycle_primitive_result = known_success_result();
  ASSERT_TRUE(completed.locally_valid());
  recovery.unresolved_effect_attempts = {completed};
  EXPECT_FALSE(recovery.locally_valid());

  recovery.unresolved_effect_attempts = {prepared};
  recovery.effect_sequence_watermarks.front().highest_effect_sequence = 2U;
  EXPECT_FALSE(recovery.locally_valid());
}

TEST(CCLCJournalRecovery, RejectsProviderMismatchAndBoundedCollectionOverflow) {
  const auto status = prepared_recovery_status();
  auto prepared = prepared_recovery_attempt();
  CCLCJournalRecoveryDTO recovery{
      4U,
      {{ControllableComponentId("component-1"), ComponentLifecycleGeneration(4U)}},
      {status},
      {{status.acceptance.lifecycle_transaction_id,
        status.acceptance.lifecycle_transaction_generation, 1U}},
      {prepared},
      0U,
      {},
      {},
      {},
      {},
      {},
      {},
      RequestOwnerEpoch("owner-epoch-1")};
  ASSERT_TRUE(recovery.locally_valid());

  prepared.lifecycle_primitive_intent->provider_instance_id =
      ProviderInstanceId("different-provider");
  ASSERT_TRUE(prepared.locally_valid());
  recovery.unresolved_effect_attempts = {prepared};
  EXPECT_FALSE(recovery.locally_valid());

  recovery.unresolved_effect_attempts.clear();
  recovery.component_watermarks.clear();
  for (std::size_t index = 0U; index <= kMaximumCCLCComponentWatermarks; ++index) {
    recovery.component_watermarks.push_back(
        {ControllableComponentId("component-" + std::to_string(index)),
         ComponentLifecycleGeneration(1U)});
  }
  std::sort(recovery.component_watermarks.begin(), recovery.component_watermarks.end(),
            [](const auto& left, const auto& right) {
              return left.controllable_component_id < right.controllable_component_id;
            });
  EXPECT_FALSE(recovery.locally_valid());
}

class FakeBrandLifecyclePort final : public IBrandLifecyclePort {
 public:
  Result<BrandLifecycleObservationDTO> Observe(
      const BrandLifecycleObservationQueryDTO& query) override {
    auto sample = observation();
    if (!sample.matches(query)) {
      return Result<BrandLifecycleObservationDTO>::Err(
          ErrorDTO{"identity_mismatch", "wrong provider subject", {}});
    }
    return Result<BrandLifecycleObservationDTO>::Ok(std::move(sample));
  }

  BrandLifecyclePrimitiveResultDTO Dispatch(const BrandLifecyclePrimitiveIntentDTO& request,
                                            std::stop_token cancellation) override {
    auto result = known_success_result();
    if (cancellation.stop_requested()) {
      result.dispatch_state = LifecyclePrimitiveDispatchStateDTO::kNotDispatched;
      result.effect = LifecyclePrimitiveEffectDTO::kNone;
      result.known_result.reset();
      result.error = ErrorDTO{"cancelled_before_dispatch", "cancelled", {}};
    }
    EXPECT_TRUE(result.matches(request));
    return result;
  }
};

TEST(ComponentLifecycleCodec, PreservesExactOrdinaryProgramAndRouteBytes) {
  // Frozen independently spelled V1/V2 payload fingerprints; adding selected
  // records must not append even an absent optional tag to ordinary bytes.
  auto ordinary = ordinary_codec_status();
  ASSERT_TRUE(ordinary.locally_valid());
  const auto ordinary_bytes = detail::EncodeComponentLifecycleStatus(ordinary);
  EXPECT_EQ(ordinary_bytes.size(), 549U);
  EXPECT_EQ(content_identity::sha256_digest(ordinary_bytes),
            "sha256:62766c852d31bb58ae019f76ea484829d9931c4474c65a286b8adeebef4dc816");
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(ordinary_bytes), ordinary);
  ordinary.accepted_intent.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{
      RequestOwnerEpoch("runner-epoch"), ProgramRunId("run-1"), CellAdmissionClaimGeneration(11U),
      "node-1", "attempt-1", "cclc.lifecycle"};
  ASSERT_TRUE(ordinary.locally_valid());
  const auto program_bytes = detail::EncodeComponentLifecycleStatus(ordinary);
  EXPECT_EQ(program_bytes.size(), 655U);
  EXPECT_EQ(content_identity::sha256_digest(program_bytes),
            "sha256:fe100787f2b80b06c190e3ad4a4e642413f138215071b087879333110a76c6c4");
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(program_bytes), ordinary);
  const CellAdmissionConfigurationScopeDTO configuration{ControlDomainId("domain-1"),
      ActiveControlConfigurationId("configuration-1"), ActiveControlConfigurationGeneration(7U)};
  const auto route_bytes = detail::EncodeCCLCRouteRecoveryConfiguration(configuration);
  EXPECT_EQ(route_bytes.size(), 68U);
  EXPECT_EQ(content_identity::sha256_digest(route_bytes),
            "sha256:3805d27b92bf168372cb1ba938b512bfe872707d67c1ad84d04ebec55b2b9245");
  EXPECT_EQ(detail::DecodeCCLCRouteRecoveryConfiguration(route_bytes), configuration);
}

TEST(ComponentLifecycleValueCodec, PreservesIntentAttributionAndProgramEvidenceAsClosedValues) {
  auto ordinary = intent();
  ordinary.attribution = {RequestTransportDTO::kHttp, RequestAssuranceDTO::kExternallyAuthenticated,
                          std::string("principal")};
  auto bytes = EncodeComponentLifecycleIntent(ordinary);
  EXPECT_EQ(DecodeComponentLifecycleIntent(bytes), ordinary);
  ordinary.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{
      RequestOwnerEpoch("runner-epoch"), ProgramRunId("run-1"), CellAdmissionClaimGeneration(11U),
      "node-1", "attempt-1", "cclc.lifecycle"};
  bytes = EncodeComponentLifecycleIntent(ordinary);
  EXPECT_EQ(DecodeComponentLifecycleIntent(bytes), ordinary);
  EXPECT_THROW((void)DecodeComponentLifecycleIntent(bytes + "extra"), std::invalid_argument);
  EXPECT_THROW((void)DecodeComponentLifecycleIntent(bytes.substr(0U, bytes.size() - 1U)),
               std::invalid_argument);
  EXPECT_THROW((void)DecodeComponentLifecycleIntent(detail::EncodeComponentLifecycleStatus(
      ordinary_codec_status())), std::invalid_argument);
  ordinary.intent = static_cast<ComponentLifecycleIntentKindDTO>(255U);
  EXPECT_THROW((void)EncodeComponentLifecycleIntent(ordinary), std::invalid_argument);
}

TEST(ComponentLifecycleValueCodec, CloseRecoveryRequiresExactIncidentAndPreservesClosedEncoding) {
  auto close = intent();
  close.intent = ComponentLifecycleIntentKindDTO::kRecover;
  close.reason = "cell closed by operator";
  close.close_incident_recovery = CloseComponentIncidentRecoveryDTO{"incident.1"};
  ASSERT_TRUE(close.locally_valid());
  const auto bytes = EncodeComponentLifecycleIntent(close);
  EXPECT_EQ(DecodeComponentLifecycleIntent(bytes), close);
  EXPECT_THROW((void)DecodeComponentLifecycleIntent(bytes + "tail"), std::invalid_argument);
  auto other = close;
  other.close_incident_recovery->expected_incident_id = "incident.2";
  EXPECT_FALSE(close.replay_equal(other));
  other = close;
  other.close_incident_recovery.reset();
  EXPECT_FALSE(close.replay_equal(other));
  other = close;
  other.reason = "operator-confirmed";
  EXPECT_FALSE(other.locally_valid());
  other = close;
  other.intent = ComponentLifecycleIntentKindDTO::kEnable;
  EXPECT_FALSE(other.locally_valid());
  other = close;
  other.close_incident_recovery->expected_incident_id.clear();
  EXPECT_FALSE(other.locally_valid());
}

TEST(ComponentLifecycleValueCodec, SelectedResultsKeepExactClosedAcceptanceAndNoAdmissionShapes) {
  const auto status = selected_bring_up_status();
  const CCLCLifecycleRequestResultDTO accepted{RequestReplayStatusDTO::kAcceptedNew,
      status.acceptance, status, std::nullopt, std::nullopt};
  ASSERT_TRUE(accepted.locally_valid());
  const auto bytes = EncodeSelectedComponentBringUpRequestResult(accepted);
  const auto decoded = DecodeSelectedComponentBringUpRequestResult(bytes);
  EXPECT_EQ(decoded.replay_status, accepted.replay_status);
  EXPECT_EQ(decoded.acceptance, accepted.acceptance);
  EXPECT_EQ(decoded.status, accepted.status);
  EXPECT_EQ(decoded.rejection, accepted.rejection);
  EXPECT_FALSE(decoded.incident_recovery);
  auto replay = accepted;
  replay.replay_status = RequestReplayStatusDTO::kReplayedExact;
  replay.acceptance->replay_status = replay.replay_status;
  EXPECT_EQ(DecodeSelectedComponentBringUpRequestResult(
      EncodeSelectedComponentBringUpRequestResult(replay)).acceptance, replay.acceptance);
  const CCLCLifecycleRequestResultDTO rejected{RequestReplayStatusDTO::kAcceptedNew,
      std::nullopt, std::nullopt,
      ErrorDTO{"selected_refused", "effect never started", {{"effect", "not_started"}}},
      std::nullopt};
  ASSERT_TRUE(rejected.locally_valid());
  EXPECT_EQ(DecodeSelectedComponentBringUpRequestResult(
      EncodeSelectedComponentBringUpRequestResult(rejected)).rejection, rejected.rejection);
  for (const auto replay_status : {RequestReplayStatusDTO::kRequestResultExpired,
      RequestReplayStatusDTO::kRequestIdConflict, RequestReplayStatusDTO::kStaleRequestOwnerEpoch}) {
    const CCLCLifecycleRequestResultDTO no_admission{replay_status, std::nullopt, std::nullopt,
                                                   std::nullopt, std::nullopt};
    const auto empty = DecodeSelectedComponentBringUpRequestResult(
        EncodeSelectedComponentBringUpRequestResult(no_admission));
    EXPECT_EQ(empty.replay_status, replay_status);
    EXPECT_FALSE(empty.acceptance);
    EXPECT_FALSE(empty.status);
  }
  const auto ordinary = ordinary_codec_status();
  EXPECT_THROW((void)EncodeSelectedComponentBringUpRequestResult(
      {RequestReplayStatusDTO::kAcceptedNew, ordinary.acceptance, ordinary,
       std::nullopt, std::nullopt}), std::invalid_argument);
  const ComponentIncidentRecoveryDTO incident_recovery{"incident-1",
      ControllableComponentId("component-1"), LifecycleBlockingCauseClassDTO::kUnknown,
      "operator", kAcceptedAtNs};
  const CCLCLifecycleRequestResultDTO recovered_incident{RequestReplayStatusDTO::kAcceptedNew,
      std::nullopt, std::nullopt, std::nullopt, incident_recovery};
  ASSERT_TRUE(recovered_incident.locally_valid());
  EXPECT_THROW((void)EncodeSelectedComponentBringUpRequestResult(recovered_incident),
               std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedComponentBringUpRequestResult(bytes + "extra"),
               std::invalid_argument);
  EXPECT_THROW((void)DecodeSelectedComponentBringUpRequestResult(bytes.substr(0U, bytes.size() - 1U)),
               std::invalid_argument);
  auto malformed = bytes;
  malformed[4U + std::string("MF-CCLC-SELECTED-RESULT-V1").size()] = static_cast<char>(255U);
  EXPECT_THROW((void)DecodeSelectedComponentBringUpRequestResult(malformed), std::invalid_argument);
}

TEST(ComponentLifecycleValueCodec, StableSelectedRequestDigestCoversOriginalFieldsAndOmitsAttribution) {
  const auto status = selected_bring_up_status();
  const auto& evidence = *status.selected_bring_up;
  const RuntimeUnitId unit("child-unit");
  const RuntimeEpoch runtime("child-epoch");
  const RuntimeEpoch manager("manager-epoch");
  const auto digest = [&](const auto& request, const auto& projection, const auto& bundle,
                          std::uint64_t count, const auto& child, const auto& child_epoch,
                          const auto& manager_epoch) {
    return SelectedComponentBringUpSemanticRequestDigest(request, projection, bundle, count,
                                                         child, child_epoch, manager_epoch);
  };
  const auto original = digest(status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      unit, runtime, manager);
  EXPECT_TRUE(IsExactLowercaseSha256Digest(original.value()));
  EXPECT_NE(original, evidence.offer_digest);
  auto attributed = status.accepted_intent;
  attributed.attribution = {RequestTransportDTO::kHttp, RequestAssuranceDTO::kExternallyAuthenticated,
                            std::string("principal")};
  EXPECT_EQ(digest(attributed, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      unit, runtime, manager), original);
  attributed.reason = "different selected settings";
  EXPECT_NE(digest(attributed, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      unit, runtime, manager), original);
  auto projection = evidence.candidate_projection_revision;
  ++projection.scene_revision.revision_id;
  EXPECT_NE(digest(status.accepted_intent, projection, evidence.candidate_retained_bundle_digest,
      evidence.candidate_retained_bundle_bytes, unit, runtime, manager), original);
  EXPECT_NE(digest(status.accepted_intent, evidence.candidate_projection_revision,
      ContentDigest(std::string("sha256:") + std::string(64U, 'c')),
      evidence.candidate_retained_bundle_bytes, unit, runtime, manager), original);
  EXPECT_NE(digest(status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes + 1U,
      unit, runtime, manager), original);
  EXPECT_NE(digest(status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      RuntimeUnitId("other-child"), runtime, manager), original);
  EXPECT_NE(digest(status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      unit, RuntimeEpoch("replacement-child"), manager), original);
  EXPECT_NE(digest(status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      unit, runtime, RuntimeEpoch("replacement-manager")), original);
  EXPECT_THROW((void)digest(status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, 0U, unit, runtime, manager), std::invalid_argument);
  EXPECT_THROW((void)digest(status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      unit, RuntimeEpoch(""), manager), std::invalid_argument);
  auto program = status.accepted_intent;
  program.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{
      RequestOwnerEpoch("runner"), ProgramRunId("run"), CellAdmissionClaimGeneration(1U),
      "node", "attempt", "cclc.lifecycle"};
  EXPECT_THROW((void)digest(program, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      unit, runtime, manager), std::invalid_argument);
}

TEST(SelectedBringUpCodec, RefusesMissingOrMalformedOriginalRequestDigestWithoutGuessing) {
  const auto status = selected_bring_up_status();
  const detail::SelectedCCLCClaimIntentRecordDTO record{
      status.selected_bring_up->selected_claim.intent,
      status.selected_bring_up->original_request_payload_digest};
  const auto bytes = detail::EncodeCCLCSelectedBringUpClaimIntent(record);
  const auto original_field_bytes = 4U + record.original_request_payload_digest.value().size();
  EXPECT_THROW((void)detail::DecodeCCLCSelectedBringUpClaimIntent(
      bytes.substr(0U, bytes.size() - original_field_bytes)), std::invalid_argument);
  auto malformed = bytes;
  malformed.back() = 'X';
  EXPECT_THROW((void)detail::DecodeCCLCSelectedBringUpClaimIntent(malformed), std::invalid_argument);
  const auto decoded = detail::DecodeCCLCSelectedBringUpClaimIntent(bytes);
  EXPECT_EQ(decoded.full_claim_intent, record.full_claim_intent);
  EXPECT_EQ(decoded.original_request_payload_digest, record.original_request_payload_digest);
}

TEST(SelectedBringUpCodec, RoundTripsFullClaimAndStatusWithoutConferringEffectAuthority) {
  const auto status = selected_bring_up_status();
  ASSERT_TRUE(status.locally_valid());
  const auto& claim = status.selected_bring_up->selected_claim.intent;
  const detail::SelectedCCLCClaimIntentRecordDTO record{
      claim, status.selected_bring_up->original_request_payload_digest};
  const auto claim_bytes = detail::EncodeCCLCSelectedBringUpClaimIntent(record);
  EXPECT_TRUE(detail::IsCCLCSelectedBringUpClaimIntentEncoding(claim_bytes));
  EXPECT_EQ(detail::DecodeCCLCSelectedBringUpClaimIntent(claim_bytes), record);
  EXPECT_NE(record.original_request_payload_digest, claim.request_payload_digest);
  const auto status_bytes = detail::EncodeComponentLifecycleStatus(status);
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(status_bytes), status);
  EXPECT_NE(status_bytes.find("MF-CCLC-SELECTED-STATUS-V1"), std::string::npos);
  EXPECT_THROW((void)detail::DecodeCCLCSelectedBringUpClaimIntent(claim_bytes + "extra"),
               std::invalid_argument);
  EXPECT_THROW((void)detail::DecodeComponentLifecycleStatus(status_bytes + "extra"),
               std::invalid_argument);
  EXPECT_THROW((void)detail::DecodeCCLCSelectedBringUpClaimIntent(
      claim_bytes.substr(0U, claim_bytes.size() - 1U)), std::invalid_argument);
  auto recovered = status;
  recovered.status_owner_epoch = RequestOwnerEpoch("replacement-owner");
  recovered.remaining_duration_ns = 0;
  EXPECT_TRUE(recovered.locally_valid());  // Data validity is not redispatch permission.
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(detail::EncodeComponentLifecycleStatus(recovered)),
            recovered);
}

TEST(SelectedBringUpCodec, RefusesOrdinaryRouteAndMalformedSelectedClaimShapes) {
  const auto status = selected_bring_up_status();
  const auto claim = status.selected_bring_up->selected_claim.intent;
  const auto original_digest = status.selected_bring_up->original_request_payload_digest;
  const auto route_bytes = detail::EncodeCCLCRouteRecoveryConfiguration(claim.scope.configurations[0]);
  EXPECT_FALSE(detail::IsCCLCSelectedBringUpClaimIntentEncoding(route_bytes));
  EXPECT_FALSE(detail::IsCCLCSelectedBringUpClaimIntentEncoding(""));
  EXPECT_THROW((void)detail::DecodeCCLCSelectedBringUpClaimIntent(route_bytes), std::invalid_argument);
  const auto check_invalid = [&](auto change) {
    auto changed = claim;
    change(changed);
    EXPECT_THROW((void)detail::EncodeCCLCSelectedBringUpClaimIntent({changed, original_digest}),
                 std::invalid_argument);
  };
  check_invalid([](auto& value) { value.owner_transaction_id = "selected-lifecycle.04";
    value.intervention_evidence->offer_transaction_id = value.owner_transaction_id; });
  check_invalid([](auto& value) { value.scope.resources.clear(); });
  check_invalid([](auto& value) { value.scope.configurations.clear(); });
  check_invalid([](auto& value) { value.request_payload_digest = ContentDigest(std::string(64U, 'b')); });
  check_invalid([](auto& value) { value.intervention_evidence->parent_program_cycle_claim_generation =
      CellAdmissionClaimGeneration(1U); });
  check_invalid([](auto& value) { value.program_cycle_evidence = ProgramCycleAdmissionEvidenceDTO{
      ProgramRevision("p"), ProgramRevision("p")}; });
  check_invalid([](auto& value) { value.program_node_evidence = ProgramNodeAdmissionEvidenceDTO{
      RequestOwnerEpoch("runner"), ProgramRunId("run"), CellAdmissionClaimGeneration(1U),
      "node", "attempt", "cclc.lifecycle"}; });
  EXPECT_THROW((void)detail::EncodeCCLCSelectedBringUpClaimIntent({claim, ContentDigest("invalid")}),
               std::invalid_argument);
  auto malformed = detail::EncodeCCLCSelectedBringUpClaimIntent({claim, original_digest});
  const auto transaction = malformed.find("selected-lifecycle.4");
  ASSERT_NE(transaction, std::string::npos);
  malformed[transaction + std::string("selected-lifecycle.").size()] = '0';
  EXPECT_THROW((void)detail::DecodeCCLCSelectedBringUpClaimIntent(malformed), std::invalid_argument);
}

TEST(SelectedBringUpStatus, RefusesBrokenClaimFenceOccurrenceAndCandidateJoins) {
  const auto status = selected_bring_up_status();
  ASSERT_TRUE(status.locally_valid());
  const auto check_invalid = [&](auto change) {
    auto changed = status;
    change(changed);
    EXPECT_FALSE(changed.locally_valid());
    EXPECT_THROW((void)detail::EncodeComponentLifecycleStatus(changed), std::invalid_argument);
  };
  check_invalid([](auto& value) { value.accepted_intent.intent = ComponentLifecycleIntentKindDTO::kRecover; });
  check_invalid([](auto& value) { value.selected_bring_up->offer_digest =
      ContentDigest(std::string("sha256:") + std::string(64U, 'b')); });
  check_invalid([](auto& value) { value.selected_bring_up->selected_claim.intent.request_id =
      RequestId("other"); });
  check_invalid([](auto& value) { value.selected_bring_up->selected_claim.intent.owner_epoch =
      RequestOwnerEpoch("other"); });
  check_invalid([](auto& value) { value.selected_bring_up->selected_claim.claim_generation =
      CellAdmissionClaimGeneration(0U); });
  check_invalid([](auto& value) { value.selected_bring_up->selected_claim.acquired_admission_generation =
      AdmissionGeneration(2U); });
  check_invalid([](auto& value) { value.selected_bring_up->accepted_fence.captured_scope.resources =
      {ResourceId("other")}; });
  check_invalid([](auto& value) { value.selected_bring_up->accepted_fence.captured_scope.controllable_component_ids =
      {ControllableComponentId("other")}; });
  check_invalid([](auto& value) { value.selected_bring_up->accepted_fence.captured_scope.domains.front().candidate_control_domain_revision =
      ControlDomainRevision("other"); });
  check_invalid([](auto& value) { value.selected_bring_up->accepted_fence.captured_scope.bindings.front().candidate_control_binding_revision =
      ControlBindingRevision("other"); });
  check_invalid([](auto& value) { ++value.selected_bring_up->successor_occurrence.domains.front().observation_generation; });
  check_invalid([](auto& value) { value.selected_bring_up->predecessor_occurrence.domains.front().observation_generation =
      std::numeric_limits<std::uint64_t>::max(); });
  check_invalid([](auto& value) { value.selected_bring_up->successor_occurrence.arm_binding_epoch =
      ExecutionBindingEpoch(12U); });
  check_invalid([](auto& value) { value.selected_bring_up->candidate_retained_bundle_bytes = 0U; });
  check_invalid([](auto& value) { value.selected_bring_up->candidate_retained_bundle_digest = ContentDigest("invalid"); });
  check_invalid([](auto& value) { value.selected_bring_up->candidate_projection_revision.scene_revision.revision_id = 0U; });
  check_invalid([](auto& value) { value.selected_bring_up->containment_stop_generation = StopGeneration(0U); });
  check_invalid([](auto& value) { value.selected_bring_up->original_request_payload_digest = ContentDigest("invalid"); });
}

TEST(SelectedBringUpStatus, AcceptedRequiresExactPredecessorConfigurationAndObservation) {
  auto accepted = selected_bring_up_status();
  accepted.phase = ComponentLifecycleTransactionPhaseDTO::kAccepted;
  ASSERT_TRUE(accepted.locally_valid());  // Same predecessor/candidate revision.
  const auto check_invalid = [&](auto change) {
    auto changed = accepted;
    change(changed);
    ASSERT_TRUE(changed.domains.front().locally_valid());
    EXPECT_FALSE(changed.locally_valid());
    EXPECT_THROW((void)detail::EncodeComponentLifecycleStatus(changed), std::invalid_argument);
  };
  check_invalid([](auto& value) {
    value.domains.front()
        .current_scope->controller()
        .active_control_configuration->active_control_configuration_id =
        ActiveControlConfigurationId("foreign-configuration");
  });
  check_invalid([](auto& value) {
    value.domains.front()
        .current_scope->controller()
        .active_control_configuration->active_control_configuration_generation =
        ActiveControlConfigurationGeneration(8U);
  });
  check_invalid([](auto& value) {
    value.domains.front().current_scope->controller().active_control_configuration.reset();
  });
  check_invalid([](auto& value) {
    auto& domain = value.domains.front();
    domain.current_scope.reset();
    domain.resolved_provider_occurrence.reset();
    domain.previous_state.reset();
    domain.current_state.reset();
  });
  check_invalid([](auto& value) {
    auto& domain = value.domains.front();
    const auto changed_revision = ControlDomainRevision("candidate-revision");
    value.accepted_intent.expected_control_domains.front().control_domain_revision = changed_revision;
    domain.expected_domain->control_domain_revision = changed_revision;
    value.selected_bring_up->accepted_fence.captured_scope.domains.front().candidate_control_domain_revision =
        changed_revision;
    domain.current_scope = current_scope(*domain.expected_domain);
    domain.resolved_provider_occurrence = provider_occurrence(*domain.current_scope);
  });

  // A second adopted predecessor has no active configuration. Its later
  // appearance at Accepted must fail without emptying the full selected claim.
  const auto expected = expected_domain("domain-2", absent_configuration(), {binding("binding-2")});
  auto scope = current_scope(expected);
  accepted.accepted_intent.expected_control_domains.push_back(expected);
  accepted.domains.push_back({expected.control_domain_id, expected, scope, provider_occurrence(scope),
      ComponentLifecycleStateDTO::kPrepared, ComponentLifecycleStateDTO::kPrepared,
      accepted.requested_state, ComponentLifecycleDomainActionStateDTO::kNotStarted,
      ComponentLifecycleDomainEffectDTO::kNone, {}, std::nullopt, std::nullopt});
  auto& evidence = *accepted.selected_bring_up;
  evidence.predecessor_occurrence.domains.push_back({expected.control_domain_id, 40U});
  evidence.successor_occurrence.domains.push_back({expected.control_domain_id, 41U});
  evidence.accepted_fence.captured_scope.domains.push_back({expected.control_domain_id,
      expected.control_domain_revision, expected.control_domain_revision});
  evidence.accepted_fence.captured_scope.bindings.push_back({ControlBindingId("binding-2"),
      ControlBindingRevision("binding-revision-1"), ControlBindingRevision("binding-revision-1")});
  ASSERT_TRUE(accepted.locally_valid());
  accepted.domains.back().current_scope->controller().active_control_configuration =
      active_configuration(expected);
  ASSERT_TRUE(accepted.domains.back().locally_valid());
  EXPECT_FALSE(accepted.locally_valid());
}

TEST(SelectedBringUpStatus, AcceptedNewDomainHasNoObservationAndLaterSameRevisionProgressIsValid) {
  auto accepted = selected_bring_up_status();
  accepted.phase = ComponentLifecycleTransactionPhaseDTO::kAccepted;
  const auto added = expected_domain("domain-2", absent_configuration(), {binding("binding-2")});
  accepted.accepted_intent.expected_control_domains.push_back(added);
  accepted.domains.push_back({added.control_domain_id, added, std::nullopt, std::nullopt,
      std::nullopt, std::nullopt, accepted.requested_state,
      ComponentLifecycleDomainActionStateDTO::kNotStarted, ComponentLifecycleDomainEffectDTO::kNone,
      {}, std::nullopt, std::nullopt});
  auto& evidence = *accepted.selected_bring_up;
  evidence.successor_occurrence.domains.push_back({added.control_domain_id, 1U});
  evidence.accepted_fence.captured_scope.domains.push_back({added.control_domain_id,
      std::nullopt, added.control_domain_revision});
  evidence.accepted_fence.captured_scope.bindings.push_back({ControlBindingId("binding-2"),
      std::nullopt, ControlBindingRevision("binding-revision-1")});
  ASSERT_TRUE(accepted.locally_valid());
  accepted.domains.back().current_scope = current_scope(added);
  accepted.domains.back().resolved_provider_occurrence =
      provider_occurrence(*accepted.domains.back().current_scope);
  accepted.domains.back().previous_state = ComponentLifecycleStateDTO::kPrepared;
  accepted.domains.back().current_state = ComponentLifecycleStateDTO::kPrepared;
  ASSERT_TRUE(accepted.domains.back().locally_valid());
  EXPECT_FALSE(accepted.locally_valid());

  // Actual successor progress can use a fresh adopted configuration with the
  // same domain revision. The immutable selected claim keeps the predecessor.
  accepted.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  auto& current =
      *accepted.domains.front().current_scope->controller().active_control_configuration;
  current.active_control_configuration_id = ActiveControlConfigurationId("successor-configuration");
  current.active_control_configuration_generation = ActiveControlConfigurationGeneration(8U);
  ASSERT_TRUE(accepted.locally_valid());
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(detail::EncodeComponentLifecycleStatus(accepted)),
            accepted);
}

TEST(SelectedBringUpStatus, RetainsGripperRemovalAdditionAndChangedPredecessorFacts) {
  auto status = selected_bring_up_status();
  auto& evidence = *status.selected_bring_up;
  auto& fenced = evidence.accepted_fence.captured_scope;
  // Changed candidate revision does not rewrite truthful predecessor facts.
  status.accepted_intent.expected_control_domains.front().control_domain_revision = ControlDomainRevision("candidate-revision");
  status.domains.front().expected_domain = status.accepted_intent.expected_control_domains.front();
  fenced.domains.front().candidate_control_domain_revision = ControlDomainRevision("candidate-revision");
  const auto removed = expected_domain("domain-2", present_configuration(), {binding("binding-2")});
  const auto removed_scope = current_scope(removed, {ResourceId("resource-2")});
  status.domains.push_back({removed.control_domain_id, std::nullopt, removed_scope,
      provider_occurrence(removed_scope), ComponentLifecycleStateDTO::kPrepared,
      ComponentLifecycleStateDTO::kPrepared, status.requested_state,
      ComponentLifecycleDomainActionStateDTO::kNotStarted, ComponentLifecycleDomainEffectDTO::kKnownNoEffect,
      {}, std::nullopt, std::nullopt});
  evidence.predecessor_occurrence.domains.push_back({removed.control_domain_id, 40U});
  fenced.domains.push_back({removed.control_domain_id, removed.control_domain_revision, std::nullopt});
  fenced.bindings.push_back({ControlBindingId("binding-2"), ControlBindingRevision("binding-revision-1"), std::nullopt});
  fenced.resources.push_back(ResourceId("resource-2"));
  evidence.selected_claim.intent.scope.resources = fenced.resources;
  evidence.selected_claim.intent.scope.configurations.push_back({removed.control_domain_id,
      ActiveControlConfigurationId("configuration-1"), ActiveControlConfigurationGeneration(7U)});
  const auto added = expected_domain("domain-3", absent_configuration(), {binding("binding-3")});
  status.accepted_intent.expected_control_domains.push_back(added);
  status.domains.push_back({added.control_domain_id, added, std::nullopt, std::nullopt, std::nullopt,
      std::nullopt, status.requested_state, ComponentLifecycleDomainActionStateDTO::kNotStarted,
      ComponentLifecycleDomainEffectDTO::kNone, {}, std::nullopt, std::nullopt});
  evidence.successor_occurrence.domains.push_back({added.control_domain_id, 1U});
  fenced.domains.push_back({added.control_domain_id, std::nullopt, added.control_domain_revision});
  fenced.bindings.push_back({ControlBindingId("binding-3"), std::nullopt, ControlBindingRevision("binding-revision-1")});
  ASSERT_TRUE(status.locally_valid());
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(detail::EncodeComponentLifecycleStatus(status)), status);
  auto ordinary = status;
  ordinary.selected_bring_up.reset();
  EXPECT_FALSE(ordinary.locally_valid());
  auto dropped = status;
  dropped.selected_bring_up->accepted_fence.captured_scope.domains.erase(
      dropped.selected_bring_up->accepted_fence.captured_scope.domains.begin() + 1);
  EXPECT_FALSE(dropped.locally_valid());
  dropped = status;
  dropped.selected_bring_up->successor_occurrence.domains[1].observation_generation = 2U;
  EXPECT_FALSE(dropped.locally_valid());
  // After actual installation, successor scopes can replace predecessor data.
  for (auto& domain : status.domains) {
    if (!domain.expected_domain) continue;
    domain.current_scope = current_scope(*domain.expected_domain);
    domain.resolved_provider_occurrence = provider_occurrence(*domain.current_scope);
    domain.previous_state = ComponentLifecycleStateDTO::kPrepared;
    domain.current_state = ComponentLifecycleStateDTO::kActive;
    domain.action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
    domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
  }
  status.current_state = ComponentLifecycleStateDTO::kActive;
  status.phase = ComponentLifecycleTransactionPhaseDTO::kSucceeded;
  status.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
  status.remaining_duration_ns = 0;
  EXPECT_TRUE(status.locally_valid());
}

TEST(CCLCJournalRecovery, RequiresExactSelectedClaimForObservationOnlyRecovery) {
  const auto status = selected_bring_up_status();
  CCLCJournalRecoveryDTO recovery{4U,
      {{ControllableComponentId("component-1"), ComponentLifecycleGeneration(4U)}}, {status},
      {{status.acceptance.lifecycle_transaction_id, status.acceptance.lifecycle_transaction_generation, 0U}},
      {}, 0U, {}, {}, {}, {}, {}, {}, RequestOwnerEpoch("owner-epoch-1")};
  EXPECT_FALSE(recovery.locally_valid());
  const auto claim = status.selected_bring_up->selected_claim;
  recovery.cell_claims.push_back({claim.intent, claim, std::nullopt, false, true,
      status.selected_bring_up->original_request_payload_digest});
  ASSERT_TRUE(recovery.locally_valid());
  recovery.cell_claims[0].transaction_accepted = false;
  EXPECT_FALSE(recovery.locally_valid());
  recovery.cell_claims[0].transaction_accepted = true;
  recovery.cell_claims[0].claim->claim_generation = CellAdmissionClaimGeneration(16U);
  EXPECT_FALSE(recovery.locally_valid());
  recovery.cell_claims[0].claim = claim;
  recovery.cell_claims[0].original_request_payload_digest.reset();
  EXPECT_FALSE(recovery.locally_valid());
  recovery.cell_claims[0].original_request_payload_digest =
      ContentDigest(std::string("sha256:") + std::string(64U, 'c'));
  EXPECT_FALSE(recovery.locally_valid());
  recovery.cell_claims[0].original_request_payload_digest =
      status.selected_bring_up->original_request_payload_digest;
  recovery.cell_claims.push_back(recovery.cell_claims[0]);
  EXPECT_FALSE(recovery.locally_valid());
}

TEST(CCLCJournalRecovery, OriginalRequestDigestPresentExactlyForSelectedAttemptRows) {
  const auto status = selected_bring_up_status();
  const auto selected_intent = status.selected_bring_up->selected_claim.intent;
  auto ordinary_intent = selected_intent;
  ordinary_intent.claim_kind = CellAdmissionClaimKindDTO::kComponentLifecycle;
  ordinary_intent.owner_transaction_id = "lifecycle.4";
  ordinary_intent.intervention_evidence.reset();
  ASSERT_TRUE(ValidateCellAdmissionClaimIntent(ordinary_intent).ok());
  CCLCJournalRecoveryDTO recovery{0U, {}, {}, {}, {}, 0U, {}, {}, {}, {}, {}, {},
                                 RequestOwnerEpoch("owner-epoch-1")};
  recovery.cell_claims.push_back({ordinary_intent, std::nullopt, std::nullopt, false, false,
                                 std::nullopt});
  ASSERT_TRUE(recovery.locally_valid());
  recovery.cell_claims.front().original_request_payload_digest =
      status.selected_bring_up->original_request_payload_digest;
  EXPECT_FALSE(recovery.locally_valid());
  recovery.cell_claims.front().intent = selected_intent;
  ASSERT_TRUE(recovery.locally_valid());  // Pre-acquisition selected attempt, no status minted.
  recovery.cell_claims.front().original_request_payload_digest.reset();
  EXPECT_FALSE(recovery.locally_valid());
  recovery.cell_claims.front().original_request_payload_digest = ContentDigest("invalid");
  EXPECT_FALSE(recovery.locally_valid());
}

TEST(ApplyDelegatedComponentLifecycleCodec, RoundTripsPrepareEnableDisableAndGenuineRecoverySeparately) {
  for (const auto kind : {ComponentLifecycleIntentKindDTO::kPrepare, ComponentLifecycleIntentKindDTO::kEnable,
                          ComponentLifecycleIntentKindDTO::kDisable}) {
    for (const bool recovery : {false, true}) {
      const auto status = apply_bring_up_status(kind, recovery);
      ASSERT_TRUE(status.locally_valid());
      EXPECT_FALSE(status.selected_bring_up);
      EXPECT_EQ(status.apply_bring_up->recovery.has_value(), recovery);
      EXPECT_EQ(status.apply_bring_up->predecessor_occurrence.has_value(), recovery);
      const auto bytes = detail::EncodeComponentLifecycleStatus(status);
      EXPECT_NE(bytes.find("MF-CCLC-APPLY-DELEGATED-STATUS-V1"), std::string::npos);
      EXPECT_EQ(detail::DecodeComponentLifecycleStatus(bytes), status);
      const CCLCLifecycleRequestResultDTO result{RequestReplayStatusDTO::kAcceptedNew,
          status.acceptance, status, std::nullopt, std::nullopt};
      ASSERT_TRUE(result.locally_valid());
      const auto encoded_result = EncodeApplyDelegatedComponentLifecycleRequestResult(result);
      const auto decoded = DecodeApplyDelegatedComponentLifecycleRequestResult(encoded_result);
      EXPECT_EQ(decoded.acceptance, result.acceptance);
      EXPECT_EQ(decoded.status, result.status);
      EXPECT_THROW((void)EncodeSelectedComponentBringUpRequestResult(result), std::invalid_argument);
      EXPECT_THROW((void)DecodeApplyDelegatedComponentLifecycleRequestResult(encoded_result + "extra"),
                   std::invalid_argument);
      EXPECT_THROW((void)DecodeApplyDelegatedComponentLifecycleRequestResult(
          encoded_result.substr(0U, encoded_result.size() - 1U)), std::invalid_argument);
      auto observed = status;
      observed.status_owner_epoch = RequestOwnerEpoch("replacement-owner");
      observed.remaining_duration_ns = 0;
      ASSERT_TRUE(observed.locally_valid());
      EXPECT_EQ(detail::DecodeComponentLifecycleStatus(detail::EncodeComponentLifecycleStatus(observed)),
                observed);  // No live continuation or readiness is reconstructed.
    }
  }
}

TEST(ApplyDelegatedComponentLifecycleCodec, RejectsOtherStatusAndRetainsNoAdmissionResults) {
  const auto selected = selected_bring_up_status();
  const auto ordinary = ordinary_codec_status();
  for (const auto& status : {selected, ordinary}) {
    EXPECT_THROW((void)EncodeApplyDelegatedComponentLifecycleRequestResult(
        {RequestReplayStatusDTO::kAcceptedNew, status.acceptance, status, std::nullopt, std::nullopt}),
        std::invalid_argument);
  }
  const CCLCLifecycleRequestResultDTO rejected{RequestReplayStatusDTO::kAcceptedNew, std::nullopt,
      std::nullopt, ErrorDTO{"refused", "not started", {{"effect", "not_started"}}}, std::nullopt};
  EXPECT_EQ(DecodeApplyDelegatedComponentLifecycleRequestResult(
      EncodeApplyDelegatedComponentLifecycleRequestResult(rejected)).rejection, rejected.rejection);
  const CCLCLifecycleRequestResultDTO expired{RequestReplayStatusDTO::kRequestResultExpired,
      std::nullopt, std::nullopt, std::nullopt, std::nullopt};
  EXPECT_EQ(DecodeApplyDelegatedComponentLifecycleRequestResult(
      EncodeApplyDelegatedComponentLifecycleRequestResult(expired)).replay_status, expired.replay_status);
  const CCLCLifecycleRequestResultDTO incident{RequestReplayStatusDTO::kAcceptedNew,
      std::nullopt, std::nullopt, std::nullopt,
      ComponentIncidentRecoveryDTO{"incident", ControllableComponentId("component-1"),
          LifecycleBlockingCauseClassDTO::kFault, "operator", kAcceptedAtNs}};
  ASSERT_TRUE(incident.locally_valid());
  EXPECT_THROW((void)EncodeApplyDelegatedComponentLifecycleRequestResult(incident), std::invalid_argument);
  // New evidence absent: no new magic, tag, or delegated payload is appended.
  const auto direct_bytes = detail::EncodeComponentLifecycleStatus(selected);
  EXPECT_NE(direct_bytes.find("MF-CCLC-SELECTED-STATUS-V1"), std::string::npos);
  EXPECT_EQ(direct_bytes.find("MF-CCLC-APPLY-DELEGATED"), std::string::npos);
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(direct_bytes), selected);
}

TEST(ApplyDelegatedComponentLifecycleStatus, RefusesForeignParentScopeAndOriginalIdentity) {
  const auto status = apply_bring_up_status();
  ASSERT_TRUE(status.locally_valid());
  const auto invalid = [&](auto change) {
    auto changed = status;
    change(changed);
    EXPECT_FALSE(changed.locally_valid());
    const CCLCLifecycleRequestResultDTO result{RequestReplayStatusDTO::kAcceptedNew,
        changed.acceptance, changed, std::nullopt, std::nullopt};
    EXPECT_FALSE(result.locally_valid());
    EXPECT_THROW((void)detail::EncodeComponentLifecycleStatus(changed), std::invalid_argument);
    EXPECT_THROW((void)EncodeApplyDelegatedComponentLifecycleRequestResult(result), std::invalid_argument);
  };
  invalid([](auto& value) { value.apply_bring_up->parent_authority.parent.plan_digest =
      ContentDigest(std::string("sha256:") + std::string(64U, 'd')); });
  invalid([](auto& value) { value.apply_bring_up->parent_authority.parent_claim.acquired_admission_generation =
      AdmissionGeneration(10U); });
  invalid([](auto& value) { value.apply_bring_up->parent_authority.parent_claim.intent.claim_kind =
      CellAdmissionClaimKindDTO::kInterventionOrRecovery; });
  invalid([](auto& value) { value.apply_bring_up->parent_authority.held_fence_query.owner_transaction.apply_id =
      ApplyId("foreign-parent"); });
  invalid([](auto& value) { value.apply_bring_up->parent_authority.held_fence_query.expected_scope.resources =
      {ResourceId("foreign-resource")}; });
  invalid([](auto& value) { value.apply_bring_up->selected_scope.controllable_component_ids =
      {ControllableComponentId("component-1"), ControllableComponentId("component-2")}; });
  invalid([](auto& value) { value.apply_bring_up->selected_scope.bindings.front().predecessor_control_binding_revision =
      ControlBindingRevision("foreign-binding-revision"); });
  invalid([](auto& value) { value.apply_bring_up->original_request_payload_digest =
      ContentDigest(std::string("sha256:") + std::string(64U, 'b')); });
  invalid([](auto& value) { value.apply_bring_up->original_request_payload_digest = ContentDigest("invalid"); });
  invalid([](auto& value) { value.apply_bring_up->candidate_retained_bundle_bytes = 0U; });
  invalid([](auto& value) { value.apply_bring_up->original_execution_host.runtime_epoch = RuntimeEpoch("other"); });
  invalid([](auto& value) {
    value.domains.front()
        .current_scope->controller()
        .active_control_configuration->active_control_configuration_id =
        ActiveControlConfigurationId("foreign-active");
  });
  invalid([](auto& value) {
    value.domains.front().current_scope->controller().active_control_configuration.reset();
  });
  invalid([](auto& value) { value.selected_bring_up = selected_bring_up_status().selected_bring_up; });
}

TEST(ApplyDelegatedComponentLifecycleStatus, ValidatesRecoveryPairWithoutInventingCleanStartupFacts) {
  auto status = apply_bring_up_status(ComponentLifecycleIntentKindDTO::kEnable, true);
  ASSERT_TRUE(status.locally_valid());
  auto changed = status;
  changed.apply_bring_up->successor_occurrence_floor.reset();
  EXPECT_FALSE(changed.locally_valid());
  changed = status;
  changed.apply_bring_up->predecessor_occurrence.reset();
  EXPECT_FALSE(changed.locally_valid());
  changed = status;
  changed.apply_bring_up->recovery->containment_stop_generation = StopGeneration(0U);
  EXPECT_FALSE(changed.locally_valid());
  changed = status;
  ++changed.apply_bring_up->successor_occurrence_floor->domains.front().observation_generation;
  EXPECT_FALSE(changed.locally_valid());
  changed = status;
  changed.apply_bring_up->predecessor_occurrence->domains.front().observation_generation =
      std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(changed.locally_valid());
  auto clean = apply_bring_up_status();
  clean.apply_bring_up->successor_occurrence_floor = status.apply_bring_up->successor_occurrence_floor;
  EXPECT_FALSE(clean.locally_valid());  // A floor-only startup is not an approved data shape.
  status.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  auto& configuration =
      *status.domains.front().current_scope->controller().active_control_configuration;
  configuration.active_control_configuration_id = ActiveControlConfigurationId("actual-successor");
  configuration.active_control_configuration_generation = ActiveControlConfigurationGeneration(8U);
  status.domains.front().current_state = ComponentLifecycleStateDTO::kActive;
  EXPECT_TRUE(status.locally_valid());  // Floor does not replace actual successor observations.
}

TEST(ApplyDelegatedComponentLifecycleStatus, NewAcceptedDomainHasNoInventedObservation) {
  auto status = apply_bring_up_status(ComponentLifecycleIntentKindDTO::kEnable);
  auto& domain = status.domains.front();
  auto& expected = status.accepted_intent.expected_control_domains.front();
  expected.controller().expected_active_control_configuration = absent_configuration();
  domain.expected_domain = expected;
  domain.current_scope.reset();
  domain.resolved_provider_occurrence.reset();
  domain.previous_state.reset();
  domain.current_state.reset();
  status.previous_state.reset();
  status.current_state.reset();
  auto& evidence = *status.apply_bring_up;
  evidence.selected_scope.domains.front().predecessor_control_domain_revision.reset();
  evidence.selected_scope.bindings.front().predecessor_control_binding_revision.reset();
  evidence.parent_authority.held_fence_query.expected_scope = evidence.selected_scope;
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      evidence.original_execution_host.runtime_unit_id, evidence.original_execution_host.runtime_epoch,
      evidence.parent_authority.held_fence_query.manager_runtime_epoch, evidence.parent_authority.parent);
  ASSERT_TRUE(status.locally_valid());
  EXPECT_FALSE(evidence.recovery);
  EXPECT_FALSE(evidence.predecessor_occurrence);
  EXPECT_FALSE(evidence.successor_occurrence_floor);
  domain.current_scope = current_scope(expected);
  domain.resolved_provider_occurrence = provider_occurrence(*domain.current_scope);
  domain.previous_state = ComponentLifecycleStateDTO::kDeclared;
  domain.current_state = ComponentLifecycleStateDTO::kDeclared;
  status.previous_state = ComponentLifecycleStateDTO::kDeclared;
  status.current_state = ComponentLifecycleStateDTO::kDeclared;
  ASSERT_TRUE(domain.locally_valid());
  EXPECT_FALSE(status.locally_valid());
  status.previous_state.reset();
  status.phase = ComponentLifecycleTransactionPhaseDTO::kSucceeded;
  status.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
  status.remaining_duration_ns = 0;
  status.current_state = ComponentLifecycleStateDTO::kActive;
  domain.current_state = ComponentLifecycleStateDTO::kActive;
  domain.action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
  domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
  ASSERT_TRUE(status.locally_valid());
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(detail::EncodeComponentLifecycleStatus(status)), status);
}

TEST(ApplyDelegatedComponentLifecycleStatus, InitialInstalledDomainsKeepUnknownAggregateAndNoPredecessor) {
  auto status = apply_bring_up_status();
  auto& evidence = *status.apply_bring_up;
  auto second_expected = expected_domain("domain-2", absent_configuration(), {binding("binding-2")});
  status.accepted_intent.expected_control_domains.front()
      .controller()
      .expected_active_control_configuration = absent_configuration();
  status.accepted_intent.expected_control_domains.push_back(second_expected);
  auto second_domain = status.domains.front();
  second_domain.control_domain_id = second_expected.control_domain_id;
  status.domains.push_back(second_domain);
  evidence.selected_scope.domains.front().predecessor_control_domain_revision.reset();
  evidence.selected_scope.bindings.front().predecessor_control_binding_revision.reset();
  evidence.selected_scope.domains.push_back({second_expected.control_domain_id, std::nullopt,
                                           second_expected.control_domain_revision});
  evidence.selected_scope.bindings.push_back({ControlBindingId("binding-2"), std::nullopt,
                                            ControlBindingRevision("binding-revision-1")});
  evidence.parent_authority.held_fence_query.expected_scope = evidence.selected_scope;
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      status.accepted_intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      evidence.original_execution_host.runtime_unit_id, evidence.original_execution_host.runtime_epoch,
      evidence.parent_authority.held_fence_query.manager_runtime_epoch, evidence.parent_authority.parent);
  status.previous_state.reset();
  status.current_state.reset();
  for (std::size_t index = 0U; index < status.domains.size(); ++index) {
    auto& domain = status.domains[index];
    domain.expected_domain = status.accepted_intent.expected_control_domains[index];
    domain.current_scope.reset();
    domain.resolved_provider_occurrence.reset();
    domain.previous_state.reset();
    domain.current_state.reset();
  }
  ASSERT_TRUE(status.locally_valid());
  status.phase = ComponentLifecycleTransactionPhaseDTO::kVerifying;
  for (std::size_t index = 0U; index < status.domains.size(); ++index) {
    auto& domain = status.domains[index];
    domain.current_scope = current_scope(*domain.expected_domain);
    domain.resolved_provider_occurrence = provider_occurrence(*domain.current_scope);
    const auto observed = index == 0U ? ComponentLifecycleStateDTO::kDeclared
                                     : ComponentLifecycleStateDTO::kPrepared;
    domain.previous_state = observed;
    domain.current_state = observed;
  }
  ASSERT_TRUE(status.locally_valid());
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(detail::EncodeComponentLifecycleStatus(status)), status);
  auto fabricated = status;
  fabricated.current_state = ComponentLifecycleStateDTO::kDeclared;
  EXPECT_FALSE(fabricated.locally_valid());
  fabricated = status;
  fabricated.previous_state = ComponentLifecycleStateDTO::kDeclared;
  EXPECT_FALSE(fabricated.locally_valid());
  fabricated = status;
  fabricated.apply_bring_up.reset();
  EXPECT_FALSE(fabricated.locally_valid());
  // A coherent aggregate is established only from the installed observations;
  // it never supplies a predecessor for this initial transaction.
  status.domains.front().current_state = ComponentLifecycleStateDTO::kPrepared;
  status.current_state = ComponentLifecycleStateDTO::kPrepared;
  ASSERT_TRUE(status.locally_valid());
  EXPECT_FALSE(status.previous_state);
  status.phase = ComponentLifecycleTransactionPhaseDTO::kSucceeded;
  status.terminal_result = ComponentLifecycleTerminalResultDTO::kSucceeded;
  status.remaining_duration_ns = 0;
  for (auto& domain : status.domains) {
    domain.action_state = ComponentLifecycleDomainActionStateDTO::kSucceeded;
    domain.effect = ComponentLifecycleDomainEffectDTO::kKnownNoEffect;
  }
  ASSERT_TRUE(status.locally_valid());
  status.current_state.reset();
  EXPECT_FALSE(status.locally_valid());
}

TEST(ApplyDelegatedComponentLifecycleStatus, ParentClosureCanContainPeersWithoutWideningChildScope) {
  auto status = apply_bring_up_status();
  auto& scope = status.apply_bring_up->parent_authority.held_fence_query.expected_scope;
  scope.controllable_component_ids.push_back(ControllableComponentId("component-2"));
  scope.domains.push_back({ControlDomainId("domain-2"), ControlDomainRevision("peer-revision"),
                          ControlDomainRevision("peer-revision")});
  scope.bindings.push_back({ControlBindingId("binding-2"), ControlBindingRevision("peer-binding"),
                           ControlBindingRevision("peer-binding")});
  scope.resources.push_back(ResourceId("resource-2"));
  ASSERT_TRUE(status.locally_valid());
  EXPECT_EQ(status.domains.size(), 1U);
  EXPECT_EQ(status.apply_bring_up->selected_scope.controllable_component_ids.size(), 1U);
  EXPECT_EQ(detail::DecodeComponentLifecycleStatus(detail::EncodeComponentLifecycleStatus(status)), status);
}

TEST(ApplyDelegatedComponentLifecycleCodec, StableDigestSealsTypedParentAndOmitsTransportAttribution) {
  const auto status = apply_bring_up_status();
  const auto& evidence = *status.apply_bring_up;
  const auto digest = [&](const auto& request, const auto& parent) {
    return ApplyDelegatedComponentLifecycleSemanticRequestDigest(request,
        evidence.candidate_projection_revision, evidence.candidate_retained_bundle_digest,
        evidence.candidate_retained_bundle_bytes, evidence.original_execution_host.runtime_unit_id,
        evidence.original_execution_host.runtime_epoch,
        evidence.parent_authority.held_fence_query.manager_runtime_epoch, parent);
  };
  const auto original = digest(status.accepted_intent, evidence.parent_authority.parent);
  EXPECT_EQ(original, evidence.original_request_payload_digest);
  auto request = status.accepted_intent;
  request.attribution = {RequestTransportDTO::kHttp, RequestAssuranceDTO::kExternallyAuthenticated,
                        std::string("principal")};
  EXPECT_EQ(digest(request, evidence.parent_authority.parent), original);
  request.intent = ComponentLifecycleIntentKindDTO::kEnable;
  EXPECT_NE(digest(request, evidence.parent_authority.parent), original);
  auto parent = evidence.parent_authority.parent;
  parent.owner.apply_id = ApplyId("other-parent");
  EXPECT_NE(digest(status.accepted_intent, parent), original);
  parent = evidence.parent_authority.parent;
  parent.owner.apply_generation = ApplyGeneration(9U);
  EXPECT_NE(digest(status.accepted_intent, parent), original);
  parent = evidence.parent_authority.parent;
  parent.plan_digest = ContentDigest(std::string("sha256:") + std::string(64U, 'e'));
  EXPECT_NE(digest(status.accepted_intent, parent), original);
  parent.plan_digest = ContentDigest("invalid");
  EXPECT_THROW((void)digest(status.accepted_intent, parent), std::invalid_argument);
  request.intent = ComponentLifecycleIntentKindDTO::kDisable;
  EXPECT_NE(digest(request, evidence.parent_authority.parent), original);
  request.intent = ComponentLifecycleIntentKindDTO::kRecover;
  EXPECT_THROW((void)digest(request, evidence.parent_authority.parent), std::invalid_argument);
}

TEST(ApplyComponentBringUpAuthorityCodec, ReusesExactParentGrammarAndRejectsIncompleteFrames) {
  const auto authority = apply_bring_up_status().apply_bring_up->parent_authority;
  const auto encoded = EncodeApplyComponentBringUpAuthority(authority);
  EXPECT_EQ(DecodeApplyComponentBringUpAuthority(encoded), authority);
  EXPECT_THROW((void)DecodeApplyComponentBringUpAuthority(encoded + "trailing"),
               std::invalid_argument);
  EXPECT_THROW((void)DecodeApplyComponentBringUpAuthority(
                   std::string_view(encoded).substr(0U, encoded.size() - 1U)),
               std::invalid_argument);
  auto foreign = authority;
  foreign.parent_claim.intent.owner_transaction_id = "another-Apply";
  EXPECT_THROW((void)EncodeApplyComponentBringUpAuthority(foreign), std::invalid_argument);
  foreign = authority;
  foreign.parent_claim.acquired_admission_generation = AdmissionGeneration(
      foreign.parent_claim.acquired_admission_generation.value() + 1U);
  EXPECT_THROW((void)EncodeApplyComponentBringUpAuthority(foreign), std::invalid_argument);
}

TEST(SelectedControllerOccurrence, AdvancesContinuingDomainsAndSeedsNewMembership) {
  const SelectedControllerOccurrenceDTO predecessor{
      ExecutionBindingEpoch(7U), {{ControlDomainId("arm"), 31U}, {ControlDomainId("removed"), 8U}}};
  const auto result = ComputeSelectedControllerSuccessorOccurrence(
      predecessor, {ControlDomainId("arm"), ControlDomainId("gripper")}, true);
  ASSERT_TRUE(result.ok()) << result.error().message;
  EXPECT_EQ(result.value().arm_binding_epoch, ExecutionBindingEpoch(8U));
  EXPECT_EQ(result.value().domains,
            (std::vector<SelectedDomainOccurrenceDTO>{{ControlDomainId("arm"), 32U},
                                                       {ControlDomainId("gripper"), 1U}}));
  EXPECT_EQ(predecessor.domains.back().control_domain_id, ControlDomainId("removed"));
}

TEST(SelectedControllerOccurrence, RefusesOverflowBeforeAnyOccurrenceCanBeInstalled) {
  auto predecessor = SelectedControllerOccurrenceDTO{
      ExecutionBindingEpoch(std::numeric_limits<std::uint64_t>::max()),
      {{ControlDomainId("arm"), 4U}}};
  EXPECT_FALSE(ComputeSelectedControllerSuccessorOccurrence(
                   predecessor, {ControlDomainId("arm")}, true).ok());
  predecessor.arm_binding_epoch = ExecutionBindingEpoch(7U);
  predecessor.domains.front().observation_generation = std::numeric_limits<std::uint64_t>::max();
  EXPECT_FALSE(ComputeSelectedControllerSuccessorOccurrence(
                   predecessor, {ControlDomainId("arm")}, true).ok());
  // An exhausted removed domain does not invent a successor or prevent a
  // genuinely new domain from using its existing initial convention.
  const auto removed = ComputeSelectedControllerSuccessorOccurrence(
      predecessor, {ControlDomainId("gripper")}, false);
  ASSERT_TRUE(removed.ok());
  EXPECT_FALSE(removed.value().arm_binding_epoch);
  EXPECT_EQ(removed.value().domains.front().observation_generation, 1U);
}

TEST(SelectedControllerOccurrence, RequiresCanonicalExactDomainMembership) {
  const SelectedControllerOccurrenceDTO predecessor{
      ExecutionBindingEpoch(3U), {{ControlDomainId("arm"), 0U}}};
  EXPECT_FALSE(ComputeSelectedControllerSuccessorOccurrence(predecessor, {}, true).ok());
  EXPECT_FALSE(ComputeSelectedControllerSuccessorOccurrence(
                   predecessor, {ControlDomainId("gripper"), ControlDomainId("arm")}, true).ok());
  EXPECT_FALSE(ComputeSelectedControllerSuccessorOccurrence(
                   predecessor, {ControlDomainId("arm"), ControlDomainId("arm")}, true).ok());
  const auto result = ComputeSelectedControllerSuccessorOccurrence(
      predecessor, {ControlDomainId("arm")}, true);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().domains.front().observation_generation, 1U);
}

TEST(BrandLifecyclePort, PreservesExactObservationAndCancellationBoundaries) {
  FakeBrandLifecyclePort port;
  const auto observed = port.Observe(observation_query());
  ASSERT_TRUE(observed.ok());
  EXPECT_TRUE(observed.value().locally_valid());

  std::stop_source source;
  source.request_stop();
  const auto result = port.Dispatch(primitive_intent(), source.get_token());
  EXPECT_EQ(result.dispatch_state, LifecyclePrimitiveDispatchStateDTO::kNotDispatched);
  EXPECT_EQ(result.effect, LifecyclePrimitiveEffectDTO::kNone);
  EXPECT_TRUE(result.locally_valid());
}

TEST(ComponentLifecycleHealth, ValidatesBoundedNormalizedFactsWithoutExecutionIdentity) {
  const auto sample = observation();
  const LifecycleDomainHealthDTO health{sample.observed_state, sample.facts, sample.diagnostic};
  ASSERT_TRUE(health.locally_valid());
  for (int mutation = 0; mutation < 7; ++mutation) {
    auto invalid = health;
    switch (mutation) {
      case 0: invalid.observed_state = static_cast<ComponentLifecycleStateDTO>(255); break;
      case 1: invalid.facts.push_back(invalid.facts.front()); break;
      case 2: std::reverse(invalid.facts.begin(), invalid.facts.end()); break;
      case 3: invalid.facts.front().value.boolean_value.reset(); break;
      case 4: invalid.diagnostic = ErrorDTO{"", "invalid", {}}; break;
      case 5:
        invalid.diagnostic = ErrorDTO{"fault", std::string(kMaxLifecycleErrorMessageBytes + 1, 'x'), {}};
        break;
      case 6: invalid.facts.resize(kMaxLifecycleFacts + 1, health.facts.front()); break;
    }
    EXPECT_FALSE(invalid.locally_valid()) << mutation;
  }
  const auto requires_prepared = predicate(ComponentLifecycleStateDTO::kPrepared);
  const auto before = health;
  EXPECT_EQ(EvaluateLifecyclePredicate(requires_prepared, health),
            EvaluateLifecyclePredicate(requires_prepared, sample));
  EXPECT_EQ(health, before);
  auto invalid_execution = sample;
  invalid_execution.source_sequence = 0;
  EXPECT_FALSE(invalid_execution.locally_valid());
  EXPECT_EQ(EvaluateLifecyclePredicate(requires_prepared, invalid_execution),
            LifecyclePostconditionResultDTO::kInconclusive);
  EXPECT_EQ(EvaluateLifecyclePredicate(requires_prepared, health),
            LifecyclePostconditionResultDTO::kSatisfied);
}

}  // namespace
}  // namespace manyforge::core
