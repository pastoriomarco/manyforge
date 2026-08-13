#include <gtest/gtest.h>

#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>

namespace manyforge::core {
namespace {

constexpr std::int64_t kAcceptedAtNs = 100;
constexpr std::int64_t kUnresolvedDisableTimeoutNs = 9'000;

[[nodiscard]] std::string Sha(char value) {
  return std::string("sha256:") + std::string(64U, value);
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO ActivePublication() {
  const ProjectionRevisionDTO revision{RetainedBundleId("bundle.1"),
                                       GraphRevision("graph.1"),
                                       {SceneId(Sha('a')), 1U, ContentDigest(Sha('b'))}};
  return {{ActiveBundlePublicationTransactionRefDTO{BootstrapPublicationTransactionDTO{
               BootstrapId("bootstrap.1"), BootstrapGeneration(1U)}},
           revision, revision.retained_bundle_id, revision.graph_revision, std::nullopt,
           ContentDigest("private.1"), ContentDigest("retired.1")},
          ActiveBundlePublicationGeneration(2U),
          ContentDigest("public-pointer.1")};
}

[[nodiscard]] ExpectedControlDomainRevisionDTO ExpectedDomain(const std::string& suffix) {
  return {
      ControlDomainId("domain-" + suffix),
      ControlDomainRevision("domain-revision-" + suffix),
      ControllerTopologyId("topology-" + suffix),
      TopologyRevision("topology-revision-" + suffix),
      {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
       ActiveControlConfigurationId("configuration-" + suffix),
       ActiveControlConfigurationGeneration(7U)},
      {{ControlBindingId("binding-" + suffix), ControlBindingRevision("binding-revision-" + suffix),
        ControlRouteId("route-" + suffix)}}};
}

[[nodiscard]] ActiveControlConfigurationDTO ActiveConfiguration(
    const ExpectedControlDomainRevisionDTO& expected) {
  const auto& binding = expected.expected_control_bindings.front();
  return {
      *expected.expected_active_control_configuration.active_control_configuration_id,
      expected.control_domain_id,
      expected.control_domain_revision,
      {{binding.control_binding_id, binding.control_binding_revision, binding.control_route_id}},
      ActivationSetId("activation.1"),
      expected.controller_topology_id,
      expected.topology_revision,
      RuntimeUnitId("runtime.1"),
      RuntimeEpoch("runtime-epoch.1"),
      1U,
      {},
      *expected.expected_active_control_configuration.active_control_configuration_generation,
      ActiveControlConfigurationStateDTO::kReconciled};
}

[[nodiscard]] ResolvedLifecycleControlDomainScopeDTO CurrentDomain(
    const ExpectedControlDomainRevisionDTO& expected) {
  return {expected.control_domain_id,
          expected.control_domain_revision,
          expected.controller_topology_id,
          expected.topology_revision,
          ActiveConfiguration(expected),
          expected.expected_control_bindings,
          {ResourceId("resource-" + expected.control_domain_id.value())}};
}

[[nodiscard]] ResolvedLifecycleProviderOccurrenceDTO Provider(
    const ResolvedLifecycleControlDomainScopeDTO& domain, const std::string& suffix) {
  return {domain.control_domain_id,
          domain.control_domain_revision,
          ProviderInstanceId("provider-" + suffix),
          ProviderManifestId("manifest-" + suffix),
          ProviderManifestRevision(Sha(suffix.empty() ? '0' : suffix.front())),
          RuntimeUnitId("runtime-" + suffix),
          RuntimeEpoch("runtime-epoch-" + suffix),
          ContentDigest(Sha(suffix.empty() ? '0' : suffix.front()))};
}

[[nodiscard]] LifecyclePredicateDTO StatePredicate(ComponentLifecycleStateDTO state) {
  return {{state}, {}};
}

[[nodiscard]] LifecycleIntentPlanDTO FixedPlan(ComponentLifecycleIntentKindDTO intent,
                                               ComponentLifecycleStateDTO target,
                                               std::int64_t timeout_ns) {
  return {intent, target, std::nullopt, StatePredicate(target), timeout_ns, {}};
}

[[nodiscard]] LifecycleIntentPlanDTO RecoverPlan(ComponentLifecycleStateDTO target,
                                                 std::int64_t timeout_ns) {
  return {ComponentLifecycleIntentKindDTO::kRecover,
          target,
          StatePredicate(ComponentLifecycleStateDTO::kFaulted),
          StatePredicate(target),
          timeout_ns,
          {}};
}

[[nodiscard]] BrandLifecyclePlanCatalogDTO Catalog(ResolvedLifecycleProviderOccurrenceDTO& provider,
                                                   std::vector<LifecycleIntentPlanDTO> plans) {
  auto catalog = MakeBrandLifecyclePlanCatalog(
      provider.provider_manifest_id, provider.provider_manifest_revision, std::move(plans));
  if (!catalog.ok()) {
    ADD_FAILURE() << catalog.error().message;
    return {provider.provider_manifest_id,
            provider.provider_manifest_revision,
            provider.lifecycle_plan_catalog_digest,
            {}};
  }
  provider.lifecycle_plan_catalog_digest = catalog.value().catalog_digest;
  return std::move(catalog.value());
}

[[nodiscard]] ComponentLifecycleCurrentSnapshotDTO Snapshot(
    const std::vector<ExpectedControlDomainRevisionDTO>& domains,
    ComponentLifecycleStateDTO state = ComponentLifecycleStateDTO::kDeclared) {
  std::vector<ResolvedLifecycleControlDomainScopeDTO> current;
  std::vector<ResolvedLifecycleProviderOccurrenceDTO> providers;
  for (std::size_t index = 0U; index < domains.size(); ++index) {
    current.push_back(CurrentDomain(domains[index]));
    providers.push_back(Provider(current.back(), std::to_string(index + 1U)));
  }
  return {ContentDigest("component-snapshot.1"),
          ControllableComponentId("component.1"),
          ComponentLifecycleGeneration(3U),
          state,
          ActivePublication(),
          std::move(current),
          std::move(providers)};
}

[[nodiscard]] ComponentLifecycleIntentDTO Intent(
    ComponentLifecycleIntentKindDTO kind, std::vector<ExpectedControlDomainRevisionDTO> domains,
    ComponentLifecycleGeneration expected_generation = ComponentLifecycleGeneration(3U)) {
  return {{RequestId("request.1"), RequestOwnerEpoch("cclc-owner.1"), "test", std::nullopt},
          ControllableComponentId("component.1"),
          kind,
          GraphRevision("graph.1"),
          expected_generation,
          std::move(domains),
          std::nullopt,
          {RequestTransportDTO::kInternal, RequestAssuranceDTO::kUnverified, std::nullopt}};
}

[[nodiscard]] DisableHandoffAdmissionSnapshotDTO ScopedDisableAdmission(
    const ComponentLifecycleCurrentSnapshotDTO& snapshot,
    DisableHandoffScopeResolutionDTO resolution = DisableHandoffScopeResolutionDTO::kExact) {
  const auto expectation = BuildDisableHandoffComponentScopeExpectation(snapshot);
  EXPECT_TRUE(expectation.ok());
  return {{snapshot.controllable_component_id,
           DisableHandoffScopeRequestKindDTO::kResolvedComponentScope, expectation.value()},
          RequestOwnerEpoch("kernel-owner.1"),
          resolution,
          expectation.value().component_scope,
          expectation.value().active_publication_generation,
          expectation.value().active_public_pointer_digest,
          expectation.value().retained_bundle_id,
          expectation.value().graph_revision,
          9U,
          ConfigurationFenceGeneration(11U),
          expectation.value().current_active_control_configurations,
          ContentDigest("disable-admission.1")};
}

[[nodiscard]] DisableHandoffAdmissionSnapshotDTO FullDisableAdmission(
    const ComponentLifecycleCurrentSnapshotDTO& illustrative_cell_snapshot) {
  const auto expectation = BuildDisableHandoffComponentScopeExpectation(illustrative_cell_snapshot);
  EXPECT_TRUE(expectation.ok());
  return {{illustrative_cell_snapshot.controllable_component_id,
           DisableHandoffScopeRequestKindDTO::kFullStopRequired, std::nullopt},
          RequestOwnerEpoch("kernel-owner.1"),
          DisableHandoffScopeResolutionDTO::kPromotedToFull,
          expectation.value().component_scope,
          expectation.value().active_publication_generation,
          expectation.value().active_public_pointer_digest,
          expectation.value().retained_bundle_id,
          expectation.value().graph_revision,
          9U,
          ConfigurationFenceGeneration(11U),
          expectation.value().current_active_control_configurations,
          ContentDigest("full-disable-admission.1")};
}

[[nodiscard]] ComponentLifecycleAdmissionCandidate Candidate(
    std::optional<ComponentLifecycleCurrentSnapshotDTO> snapshot,
    std::vector<BrandLifecyclePlanCatalogDTO> catalogs,
    std::optional<DisableHandoffAdmissionSnapshotDTO> disable_admission = std::nullopt,
    ComponentLifecycleGeneration durable_predecessor = ComponentLifecycleGeneration(3U)) {
  return {LifecycleTransactionId("lifecycle-transaction.1"),
          LifecycleTransactionGeneration(8U),
          durable_predecessor,
          kAcceptedAtNs,
          kUnresolvedDisableTimeoutNs,
          std::move(snapshot),
          std::move(catalogs),
          std::move(disable_admission)};
}

TEST(ComponentLifecycleAdmission, PreparesExactFixedTargetFromCurrentProviderCatalogClosure) {
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected});
  const auto catalog = Catalog(snapshot.provider_occurrences.front(),
                               {FixedPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                          ComponentLifecycleStateDTO::kPrepared, 5'000)});

  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kPrepare, {expected}),
      Candidate(snapshot, {catalog}));

  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.timeout_duration_ns, 5'000);
  EXPECT_EQ(prepared.value().acceptance.component_lifecycle_generation,
            ComponentLifecycleGeneration(4U));
  EXPECT_EQ(prepared.value().reserved_control_domains,
            std::vector<ControlDomainId>{ControlDomainId("domain-a")});
  ASSERT_TRUE(prepared.value().qualified_lifecycle_plan_catalogs.has_value());
  EXPECT_EQ(*prepared.value().qualified_lifecycle_plan_catalogs,
            std::vector<BrandLifecyclePlanCatalogDTO>{catalog});
  EXPECT_TRUE(prepared.value().lifecycle_mutation_eligible);
  ASSERT_EQ(prepared.value().initial_status.domains.size(), 1U);
  EXPECT_EQ(prepared.value().initial_status.requested_state.state,
            ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(prepared.value().initial_status.domains.front().resolved_provider_occurrence,
            snapshot.provider_occurrences.front());
  EXPECT_EQ(prepared.value().initial_status.current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_TRUE(prepared.value().initial_status.locally_valid());
}

TEST(ComponentLifecycleAdmission, RecoverUsesMinimumAcrossEveryCandidateWithoutSelectingTarget) {
  const auto first = ExpectedDomain("a");
  const auto second = ExpectedDomain("b");
  auto snapshot = Snapshot({first, second}, ComponentLifecycleStateDTO::kFaulted);
  const auto first_catalog = Catalog(snapshot.provider_occurrences[0],
                                     {RecoverPlan(ComponentLifecycleStateDTO::kDeclared, 8'000),
                                      RecoverPlan(ComponentLifecycleStateDTO::kPrepared, 6'000)});
  const auto second_catalog = Catalog(snapshot.provider_occurrences[1],
                                      {RecoverPlan(ComponentLifecycleStateDTO::kDeclared, 7'000),
                                       RecoverPlan(ComponentLifecycleStateDTO::kDisabled, 4'000)});

  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kRecover, {first, second}),
      Candidate(snapshot, {first_catalog, second_catalog}));

  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.timeout_duration_ns, 4'000);
  EXPECT_EQ(prepared.value().initial_status.requested_state.presence,
            ComponentLifecycleRequestedStatePresenceDTO::kUnresolved);
  EXPECT_FALSE(prepared.value().initial_status.requested_state.state.has_value());
  for (const auto& domain : prepared.value().initial_status.domains) {
    EXPECT_EQ(domain.requested_state, prepared.value().initial_status.requested_state);
    EXPECT_TRUE(domain.primitive_attempts.empty());
  }
}

TEST(ComponentLifecycleAdmission, RejectsStaleSnapshotAndWrongCatalogBeforeReservation) {
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected});
  auto catalog = Catalog(snapshot.provider_occurrences.front(),
                         {FixedPlan(ComponentLifecycleIntentKindDTO::kEnable,
                                    ComponentLifecycleStateDTO::kActive, 5'000)});

  auto forged_content = catalog;
  forged_content.plans.front().timeout_duration_ns += 1;
  auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kEnable, {expected}),
      Candidate(snapshot, {forged_content}));
  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_catalog_mismatch");

  catalog.catalog_digest = ContentDigest("other-catalog");

  prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kEnable, {expected}), Candidate(snapshot, {catalog}));
  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_catalog_mismatch");

  snapshot.component_lifecycle_generation = ComponentLifecycleGeneration(4U);
  catalog.catalog_digest = snapshot.provider_occurrences.front().lifecycle_plan_catalog_digest;
  prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kEnable, {expected}), Candidate(snapshot, {catalog}));
  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_current_snapshot_mismatch");
}

TEST(ComponentLifecycleAdmission, DisableAcceptsExpectedCurrentUnionAndReservesOnlyCurrentDomains) {
  const auto expected_a = ExpectedDomain("a");
  const auto expected_b = ExpectedDomain("b");
  const auto current_c = ExpectedDomain("c");
  auto snapshot = Snapshot({expected_a, current_c}, ComponentLifecycleStateDTO::kActive);
  const auto catalog_a = Catalog(snapshot.provider_occurrences[0],
                                 {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                                            ComponentLifecycleStateDTO::kDisabled, 7'000)});
  const auto catalog_c = Catalog(snapshot.provider_occurrences[1],
                                 {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                                            ComponentLifecycleStateDTO::kDisabled, 6'000)});

  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected_a, expected_b}),
      Candidate(snapshot, {catalog_a, catalog_c},
                ScopedDisableAdmission(
                    snapshot, DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved)));

  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.timeout_duration_ns, 6'000);
  EXPECT_EQ(
      prepared.value().reserved_control_domains,
      (std::vector<ControlDomainId>{ControlDomainId("domain-a"), ControlDomainId("domain-c")}));
  ASSERT_EQ(prepared.value().initial_status.domains.size(), 3U);
  EXPECT_TRUE(prepared.value().initial_status.domains[0].expected_domain.has_value());
  EXPECT_TRUE(prepared.value().initial_status.domains[0].current_scope.has_value());
  EXPECT_EQ(prepared.value().initial_status.domains[0].action_state,
            ComponentLifecycleDomainActionStateDTO::kReserved);
  EXPECT_TRUE(prepared.value().initial_status.domains[1].expected_domain.has_value());
  EXPECT_FALSE(prepared.value().initial_status.domains[1].current_scope.has_value());
  EXPECT_FALSE(prepared.value().initial_status.domains[1].current_state.has_value());
  EXPECT_EQ(prepared.value().initial_status.domains[1].action_state,
            ComponentLifecycleDomainActionStateDTO::kNotStarted);
  EXPECT_FALSE(prepared.value().initial_status.domains[2].expected_domain.has_value());
  EXPECT_TRUE(prepared.value().initial_status.domains[2].current_scope.has_value());
  EXPECT_EQ(prepared.value().initial_status.domains[2].action_state,
            ComponentLifecycleDomainActionStateDTO::kReserved);

  std::vector<ControlDomainId> status_reserved_domains;
  for (const auto& domain : prepared.value().initial_status.domains) {
    if (domain.action_state == ComponentLifecycleDomainActionStateDTO::kReserved) {
      ASSERT_TRUE(domain.current_scope.has_value());
      status_reserved_domains.push_back(domain.control_domain_id);
    }
  }
  EXPECT_EQ(status_reserved_domains, prepared.value().reserved_control_domains);
  ASSERT_TRUE(prepared.value().qualified_lifecycle_plan_catalogs.has_value());
  EXPECT_EQ(*prepared.value().qualified_lifecycle_plan_catalogs,
            (std::vector<BrandLifecyclePlanCatalogDTO>{catalog_a, catalog_c}));
  EXPECT_FALSE(prepared.value().lifecycle_mutation_eligible);
}

TEST(ComponentLifecycleAdmission,
     ExactResolvedDisableWithUnqualifiedCatalogMayQualifyOnlyLaterUnderFence) {
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected}, ComponentLifecycleStateDTO::kActive);
  auto catalog = Catalog(snapshot.provider_occurrences.front(),
                         {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                                    ComponentLifecycleStateDTO::kDisabled, 6'000)});
  catalog.catalog_digest = ContentDigest("unqualified-catalog");

  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected}),
      Candidate(snapshot, {catalog}, ScopedDisableAdmission(snapshot)));

  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.timeout_duration_ns, kUnresolvedDisableTimeoutNs);
  EXPECT_FALSE(prepared.value().qualified_lifecycle_plan_catalogs.has_value());
  // This records that the original graph/lifecycle/domain/configuration
  // expectations were exact. It is not permission to mutate: no executable
  // catalog is retained here, and the coordinator must requalify an exact
  // catalog/plan closure under the matching held Kernel fence first.
  EXPECT_TRUE(prepared.value().lifecycle_mutation_eligible);
  EXPECT_EQ(prepared.value().reserved_control_domains,
            std::vector<ControlDomainId>{ControlDomainId("domain-a")});
}

TEST(ComponentLifecycleAdmission, StaleResolvedDisableMintsCurrentDurableSuccessor) {
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected}, ComponentLifecycleStateDTO::kActive);
  snapshot.component_lifecycle_generation = ComponentLifecycleGeneration(5U);
  const auto catalog = Catalog(snapshot.provider_occurrences.front(),
                               {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                                          ComponentLifecycleStateDTO::kDisabled, 6'000)});

  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected}),
      Candidate(snapshot, {catalog},
                ScopedDisableAdmission(
                    snapshot, DisableHandoffScopeResolutionDTO::kStaleButUnambiguouslyResolved),
                ComponentLifecycleGeneration(5U)));

  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.component_lifecycle_generation,
            ComponentLifecycleGeneration(6U));
  EXPECT_EQ(prepared.value().initial_status.accepted_intent.expected_component_lifecycle_generation,
            ComponentLifecycleGeneration(3U));
}

TEST(ComponentLifecycleAdmission, NonDisableAndFutureDisableCannotBypassDurablePredecessor) {
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected}, ComponentLifecycleStateDTO::kActive);
  snapshot.component_lifecycle_generation = ComponentLifecycleGeneration(5U);
  const auto enable_catalog = Catalog(snapshot.provider_occurrences.front(),
                                      {FixedPlan(ComponentLifecycleIntentKindDTO::kEnable,
                                                 ComponentLifecycleStateDTO::kActive, 6'000)});

  auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kEnable, {expected}),
      Candidate(snapshot, {enable_catalog}, std::nullopt, ComponentLifecycleGeneration(5U)));
  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_current_snapshot_mismatch");

  const auto disable_catalog = Catalog(snapshot.provider_occurrences.front(),
                                       {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                                                  ComponentLifecycleStateDTO::kDisabled, 6'000)});
  prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected},
             ComponentLifecycleGeneration(6U)),
      Candidate(snapshot, {disable_catalog}, ScopedDisableAdmission(snapshot),
                ComponentLifecycleGeneration(5U)));
  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_current_snapshot_mismatch");
}

TEST(ComponentLifecycleAdmission, NoSnapshotDisableIsContainmentOnlyAndInventsNoStateOrPlan) {
  const auto expected = ExpectedDomain("a");
  const auto illustrative_snapshot = Snapshot({expected}, ComponentLifecycleStateDTO::kActive);
  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected}),
      Candidate(std::nullopt, {}, FullDisableAdmission(illustrative_snapshot)));

  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.timeout_duration_ns, kUnresolvedDisableTimeoutNs);
  EXPECT_EQ(prepared.value().acceptance.component_lifecycle_generation,
            ComponentLifecycleGeneration(4U));
  EXPECT_TRUE(prepared.value().reserved_control_domains.empty());
  EXPECT_FALSE(prepared.value().qualified_lifecycle_plan_catalogs.has_value());
  EXPECT_FALSE(prepared.value().lifecycle_mutation_eligible);
  EXPECT_FALSE(prepared.value().initial_status.previous_state.has_value());
  EXPECT_FALSE(prepared.value().initial_status.current_state.has_value());
  ASSERT_EQ(prepared.value().initial_status.domains.size(), 1U);
  EXPECT_FALSE(prepared.value().initial_status.domains.front().current_scope.has_value());
  EXPECT_FALSE(prepared.value().initial_status.domains.front().previous_state.has_value());
  EXPECT_FALSE(prepared.value().initial_status.domains.front().current_state.has_value());
  EXPECT_EQ(prepared.value().initial_status.domains.front().action_state,
            ComponentLifecycleDomainActionStateDTO::kNotStarted);
  EXPECT_TRUE(prepared.value().initial_status.locally_valid());
}

TEST(ComponentLifecycleAdmission, NoSnapshotDisableUsesExistingDurablePredecessorWhenStale) {
  const auto expected = ExpectedDomain("a");
  const auto illustrative_snapshot = Snapshot({expected}, ComponentLifecycleStateDTO::kActive);
  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected}),
      Candidate(std::nullopt, {}, FullDisableAdmission(illustrative_snapshot),
                ComponentLifecycleGeneration(5U)));

  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.component_lifecycle_generation,
            ComponentLifecycleGeneration(6U));
  EXPECT_TRUE(prepared.value().reserved_control_domains.empty());
}

TEST(ComponentLifecycleAdmission, RejectsDisableAdmissionForAnotherSnapshot) {
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected}, ComponentLifecycleStateDTO::kActive);
  const auto catalog = Catalog(snapshot.provider_occurrences.front(),
                               {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                                          ComponentLifecycleStateDTO::kDisabled, 6'000)});
  auto admission = ScopedDisableAdmission(snapshot);
  admission.query.component_expectation->component_snapshot_digest =
      ContentDigest("different-snapshot");
  ASSERT_TRUE(admission.locally_valid());

  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected}),
      Candidate(snapshot, {catalog}, admission));

  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_disable_admission_mismatch");
}

}  // namespace
}  // namespace manyforge::core
