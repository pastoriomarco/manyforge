#include <gtest/gtest.h>

#include <algorithm>
#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <type_traits>

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
  for (const auto& selected : expected.expected_control_bindings) {
    routes.push_back({selected.control_binding_id, selected.control_binding_revision,
                      selected.control_route_id});
  }
  return {ActiveControlConfigurationId("configuration-1"),
          expected.control_domain_id,
          expected.control_domain_revision,
          std::move(routes),
          ActivationSetId("activation-set-1"),
          expected.controller_topology_id,
          expected.topology_revision,
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
  if (expected.expected_active_control_configuration.presence ==
      ExpectedActiveControlConfigurationPresenceDTO::kPresent) {
    configuration = active_configuration(expected);
  }
  return {expected.control_domain_id,
          expected.control_domain_revision,
          expected.controller_topology_id,
          expected.topology_revision,
          std::move(configuration),
          expected.expected_control_bindings,
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
      ContentDigest("private-materialization-1"),
      ContentDigest("retired-surfaces-1")};
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
          std::nullopt,
          std::nullopt,
          std::nullopt,
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
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
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

ConfigurationFenceScopeDTO disable_handoff_component_scope() {
  return {
      {ControllableComponentId("component-1")},
      {{ControlDomainId("domain-1"), ControlDomainRevision("domain-revision-1"), std::nullopt}},
      {{ControlBindingId("binding-1"), ControlBindingRevision("binding-revision-1"), std::nullopt}},
      {ResourceId("resource-1")}};
}

ConfigurationFenceScopeDTO disable_handoff_full_scope() {
  auto scope = disable_handoff_component_scope();
  scope.controllable_component_ids.push_back(ControllableComponentId("component-2"));
  scope.domains.push_back(
      {ControlDomainId("domain-2"), ControlDomainRevision("domain-revision-2"), std::nullopt});
  scope.bindings.push_back(
      {ControlBindingId("binding-2"), ControlBindingRevision("binding-revision-2"), std::nullopt});
  scope.resources.push_back(ResourceId("resource-2"));
  return scope;
}

DisableHandoffRequestDTO disable_handoff_request(
    ConfigurationFenceScopeDTO scope,
    DisableHandoffScopeResolutionDTO resolution = DisableHandoffScopeResolutionDTO::kExact) {
  std::vector<CurrentDomainActiveControlConfigurationDTO> configurations;
  for (const auto& domain : scope.domains) {
    configurations.push_back({domain.control_domain_id, absent_configuration()});
  }
  return {
      {RequestId("handoff-request-1"), RequestOwnerEpoch("kernel-owner-1"), "test", std::nullopt},
      RequestId("public-request-1"),
      LifecycleTransactionId("transaction-1"),
      LifecycleTransactionGeneration(4U),
      ControllableComponentId("component-1"),
      resolution,
      std::move(scope),
      ContentDigest("admission-snapshot-1"),
      ActiveBundlePublicationGeneration(2U),
      ContentDigest("public-pointer-1"),
      GraphRevision("graph-revision-1"),
      RetainedBundleId("retained-bundle-1"),
      ComponentLifecycleGeneration(4U),
      3U,
      ConfigurationFenceGeneration(5U),
      std::move(configurations)};
}

DisableHandoffStatusDTO disable_handoff_status(const DisableHandoffRequestDTO& request,
                                               ConfigurationFenceScopeDTO scope,
                                               DisableHandoffScopeResolutionDTO resolution) {
  const ConfigurationFenceHandleDTO handle{ConfigurationFenceId("configuration-fence-1"),
                                           ConfigurationFenceGeneration(6U),
                                           request.lifecycle_transaction_id,
                                           request.lifecycle_transaction_generation,
                                           request.controllable_component_id,
                                           scope,
                                           "Disable containment",
                                           ConfigurationFenceState::kHeld};
  return {request.lifecycle_transaction_id,
          request.lifecycle_transaction_generation,
          request.controllable_component_id,
          resolution,
          ContentDigest("component-scope-1"),
          handle.configuration_fence_id,
          handle.configuration_fence_generation,
          DisableHandoffStateDTO::kReady,
          std::move(scope),
          handle,
          std::nullopt,
          {},
          std::nullopt};
}

DisableHandoffAdmissionSnapshotDTO disable_handoff_admission_snapshot(
    ConfigurationFenceGeneration fence_generation = ConfigurationFenceGeneration(0U)) {
  const auto scope = disable_handoff_component_scope();
  const DisableHandoffComponentScopeExpectationDTO expectation{
      ContentDigest("component-snapshot-1"),
      ActiveBundlePublicationGeneration(2U),
      ContentDigest("public-pointer-1"),
      RetainedBundleId("retained-bundle-1"),
      GraphRevision("graph-revision-1"),
      ComponentLifecycleGeneration(4U),
      scope,
      {{ControlDomainId("domain-1"), absent_configuration()}}};
  DisableHandoffAdmissionSnapshotDTO snapshot{
      {ControllableComponentId("component-1"),
       DisableHandoffScopeRequestKindDTO::kResolvedComponentScope, expectation},
      RequestOwnerEpoch("kernel-owner-1"),
      DisableHandoffScopeResolutionDTO::kExact,
      scope,
      ActiveBundlePublicationGeneration(2U),
      ContentDigest("public-pointer-1"),
      RetainedBundleId("retained-bundle-1"),
      GraphRevision("graph-revision-1"),
      3U,
      fence_generation,
      {{ControlDomainId("domain-1"), absent_configuration()}},
      ContentDigest(std::string("sha256:") + std::string(64U, '0'))};
  const auto digest = ComputeDisableHandoffAdmissionSnapshotDigest(snapshot);
  if (!digest.ok()) {
    throw std::logic_error("test Disable admission snapshot is invalid");
  }
  snapshot.snapshot_digest = digest.value();
  return snapshot;
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
  std::swap(noncanonical.expected_control_bindings[0], noncanonical.expected_control_bindings[1]);
  EXPECT_FALSE(noncanonical.locally_valid());

  auto duplicate = domain;
  duplicate.expected_control_bindings[1] = duplicate.expected_control_bindings[0];
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

  resolved.current_control_bindings[0].control_route_id = ControlRouteId("other-route");
  EXPECT_FALSE(resolved.locally_valid());

  const auto first_expected = expected_domain("domain-1", absent_configuration());
  const auto first_resolved = current_scope(first_expected);
  ASSERT_TRUE(first_resolved.locally_valid());
  EXPECT_TRUE(ExpectedDomainMatchesResolvedScope(first_expected, first_resolved));
}

TEST(DisableHandoffScope, PreservesEveryRequestedDimensionAndMayOnlyWidenToFull) {
  const auto component_scope = disable_handoff_component_scope();
  const auto full_scope = disable_handoff_full_scope();
  const auto exact_request = disable_handoff_request(component_scope);
  const auto widened = disable_handoff_status(exact_request, full_scope,
                                              DisableHandoffScopeResolutionDTO::kPromotedToFull);
  ASSERT_TRUE(exact_request.locally_valid());
  ASSERT_TRUE(widened.locally_valid());
  EXPECT_TRUE(DisableHandoffPreservesOrWidensScope(exact_request, widened));

  auto revised_scope = component_scope;
  revised_scope.domains.front().predecessor_control_domain_revision =
      ControlDomainRevision("domain-revision-2");
  revised_scope.bindings.front().predecessor_control_binding_revision =
      ControlBindingRevision("binding-revision-2");
  const auto stale_but_resolved =
      disable_handoff_status(exact_request, revised_scope,
                             DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved);
  ASSERT_TRUE(stale_but_resolved.locally_valid());
  EXPECT_TRUE(DisableHandoffPreservesOrWidensScope(exact_request, stale_but_resolved));

  auto omitted_domain = widened;
  omitted_domain.scope.domains.erase(omitted_domain.scope.domains.begin());
  omitted_domain.configuration_fence_handle.scope = omitted_domain.scope;
  ASSERT_TRUE(omitted_domain.locally_valid());
  EXPECT_FALSE(DisableHandoffPreservesOrWidensScope(exact_request, omitted_domain));

  auto omitted_binding = widened;
  omitted_binding.scope.bindings.erase(omitted_binding.scope.bindings.begin());
  omitted_binding.configuration_fence_handle.scope = omitted_binding.scope;
  ASSERT_TRUE(omitted_binding.locally_valid());
  EXPECT_FALSE(DisableHandoffPreservesOrWidensScope(exact_request, omitted_binding));

  auto omitted_resource = widened;
  omitted_resource.scope.resources.erase(omitted_resource.scope.resources.begin());
  omitted_resource.configuration_fence_handle.scope = omitted_resource.scope;
  ASSERT_TRUE(omitted_resource.locally_valid());
  EXPECT_FALSE(DisableHandoffPreservesOrWidensScope(exact_request, omitted_resource));

  const auto full_request =
      disable_handoff_request(full_scope, DisableHandoffScopeResolutionDTO::kPromotedToFull);
  const auto narrowed = disable_handoff_status(full_request, component_scope,
                                               DisableHandoffScopeResolutionDTO::kPromotedToFull);
  ASSERT_TRUE(full_request.locally_valid());
  ASSERT_TRUE(narrowed.locally_valid());
  EXPECT_FALSE(DisableHandoffPreservesOrWidensScope(full_request, narrowed));

  const auto downgraded_resolution =
      disable_handoff_status(full_request, full_scope, DisableHandoffScopeResolutionDTO::kExact);
  ASSERT_TRUE(downgraded_resolution.locally_valid());
  EXPECT_FALSE(DisableHandoffPreservesOrWidensScope(full_request, downgraded_resolution));
}

TEST(DisableHandoffAdmissionIdentity,
     AcceptsInitialFencePredecessorAndDetectsEverySemanticMutation) {
  auto snapshot = disable_handoff_admission_snapshot();
  ASSERT_TRUE(snapshot.locally_valid());
  ASSERT_TRUE(VerifyDisableHandoffAdmissionSnapshot(snapshot).ok());

  auto request = disable_handoff_request(disable_handoff_component_scope());
  request.expected_fence_generation = ConfigurationFenceGeneration(0U);
  EXPECT_TRUE(request.locally_valid());

  const auto component_scope = ComputeDisableHandoffScopeDigest(snapshot.current_scope);
  ASSERT_TRUE(component_scope.ok());
  auto widened_scope = disable_handoff_full_scope();
  const auto widened_digest = ComputeDisableHandoffScopeDigest(widened_scope);
  ASSERT_TRUE(widened_digest.ok());
  EXPECT_NE(component_scope.value(), widened_digest.value());

  snapshot.operation_resource_generation += 1U;
  EXPECT_FALSE(VerifyDisableHandoffAdmissionSnapshot(snapshot).ok());
  const auto recomputed = ComputeDisableHandoffAdmissionSnapshotDigest(snapshot);
  ASSERT_TRUE(recomputed.ok());
  EXPECT_NE(recomputed.value(), snapshot.snapshot_digest);
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
  changed_scope.domains.front().current_resources.front() = ResourceId("resource-2");
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
      std::nullopt,
      std::nullopt,
      std::nullopt,
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
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt,
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
      std::nullopt,
      std::nullopt,
      std::nullopt,
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

TEST(ComponentLifecycleStatus, DisablePreparedHandoffMayRemainUnknownWithoutInventingAFence) {
  auto disable_intent = intent(ComponentLifecycleIntentKindDTO::kDisable, {expected_domain()});
  const auto requested = resolved_requested(ComponentLifecycleStateDTO::kDisabled);
  const ErrorDTO uncertainty{
      "disable_handoff_outcome_unknown", "prepared handoff requires reconciliation", {}};
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
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt,
                                     true,
                                     std::nullopt,
                                     uncertainty,
                                     kAcceptedAtNs,
                                     kAcceptedAtNs + 10,
                                     0};

  EXPECT_TRUE(status.locally_valid());
  status.reconciliation_required = false;
  EXPECT_FALSE(status.locally_valid());
  status.reconciliation_required = true;
  status.configuration_fence = ConfigurationFenceHandleDTO{
      ConfigurationFenceId("invented"),
      ConfigurationFenceGeneration(1U),
      LifecycleTransactionId("transaction-1"),
      LifecycleTransactionGeneration(4U),
      ControllableComponentId("component-1"),
      {{ControllableComponentId("component-1")},
       {{ControlDomainId("domain-1"), ControlDomainRevision("domain-revision-1"), std::nullopt}},
       {{ControlBindingId("binding-1"), ControlBindingRevision("binding-revision-1"),
         std::nullopt}},
       {ResourceId("resource-1")}},
      "invented",
      ConfigurationFenceState::kHeld};
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
      std::nullopt,
      std::nullopt,
      std::nullopt,
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
                                     std::nullopt,
                                     std::nullopt,
                                     std::nullopt,
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
      {}};
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
      {}};
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

}  // namespace
}  // namespace manyforge::core
