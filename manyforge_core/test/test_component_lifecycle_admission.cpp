#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <map>
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <stdexcept>

namespace manyforge::core {
namespace {

constexpr std::int64_t kAcceptedAtNs = 100;

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
           ContentDigest("private.1")},
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
  const auto& binding = expected.controller().expected_control_bindings.front();
  return {
      *expected.controller().expected_active_control_configuration.active_control_configuration_id,
      expected.control_domain_id,
      expected.control_domain_revision,
      {{binding.control_binding_id, binding.control_binding_revision, binding.control_route_id}},
      ActivationSetId("activation.1"),
      expected.controller().controller_topology_id,
      expected.controller().topology_revision,
      RuntimeUnitId("runtime.1"),
      RuntimeEpoch("runtime-epoch.1"),
      1U,
      {},
      *expected.controller()
           .expected_active_control_configuration.active_control_configuration_generation,
      ActiveControlConfigurationStateDTO::kReconciled};
}

[[nodiscard]] ResolvedLifecycleControlDomainScopeDTO CurrentDomain(
    const ExpectedControlDomainRevisionDTO& expected) {
  return {expected.control_domain_id,
          expected.control_domain_revision,
          expected.controller().controller_topology_id,
          expected.controller().topology_revision,
          ActiveConfiguration(expected),
          expected.controller().expected_control_bindings,
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

[[nodiscard]] ComponentLifecycleAdmissionCandidate Candidate(
    std::optional<ComponentLifecycleCurrentSnapshotDTO> snapshot,
    std::vector<BrandLifecyclePlanCatalogDTO> catalogs,
    ComponentLifecycleGeneration durable_predecessor = ComponentLifecycleGeneration(3U)) {
  return {LifecycleTransactionId("lifecycle-transaction.1"),
          LifecycleTransactionGeneration(8U),
          durable_predecessor,
          kAcceptedAtNs,
          std::move(snapshot),
          std::move(catalogs)};
}


void SealSnapshot(ComponentLifecycleCurrentSnapshotDTO& snapshot) {
  const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot);
  if (!digest.ok()) throw std::runtime_error(digest.error().message);
  snapshot.snapshot_digest = digest.value();
}

[[nodiscard]] std::pair<ExpectedControlDomainRevisionDTO, BrandLifecyclePlanCatalogDTO>
ConfigureProviderDomain(ComponentLifecycleCurrentSnapshotDTO& snapshot, std::size_t index,
                        Duration limit, std::vector<LifecycleIntentPlanDTO> plans) {
  auto& occurrence = snapshot.provider_occurrences[index];
  auto catalog = Catalog(occurrence, std::move(plans));
  const auto id = snapshot.domains[index].control_domain_id;
  const auto revision = snapshot.domains[index].control_domain_revision;
  ProviderControlDomainDTO provider{snapshot.controllable_component_id,
                                    occurrence.provider_runtime_unit_id,
                                    occurrence.provider_instance_id,
                                    occurrence.provider_manifest_id,
                                    occurrence.provider_manifest_revision,
                                    occurrence.lifecycle_plan_catalog_digest,
                                    ContentDigest(Sha('a')),
                                    {"ready"},
                                    limit};
  snapshot.domains[index] = {id, revision,
                             ResolvedProviderDomainScopeDTO{
                                 provider, occurrence.provider_runtime_epoch, std::nullopt, {}}};
  ExpectedControlDomainRevisionDTO expected{
      id, revision,
      ExpectedProviderDomainScopeDTO{
          provider,
          {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt}}};
  return {std::move(expected), std::move(catalog)};
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
  ASSERT_EQ(prepared.value().initial_status.domains.size(), 1U);
  EXPECT_EQ(prepared.value().initial_status.requested_state.state,
            ComponentLifecycleStateDTO::kPrepared);
  EXPECT_EQ(prepared.value().initial_status.domains.front().resolved_provider_occurrence,
            snapshot.provider_occurrences.front());
  EXPECT_EQ(prepared.value().initial_status.current_state, ComponentLifecycleStateDTO::kDeclared);
  EXPECT_TRUE(prepared.value().initial_status.locally_valid());
}

// Per-component time budget: the component's Deployment budget, when present,
// bounds every intent its catalogs support instead of the plan's bound (a
// 120 s robot is not timed out at its 10 s plan); without one the plan's
// bound is unchanged; it never makes an unsupported intent admissible.
TEST(ComponentLifecycleAdmission, ComponentBudgetReplacesPlanBoundOfSupportedIntents) {
  constexpr std::int64_t kPlanNs = 10'000'000'000LL;
  constexpr std::int64_t kBudgetNs = 120'000'000'000LL;
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected});
  const auto catalog = Catalog(
      snapshot.provider_occurrences.front(),
      {FixedPlan(ComponentLifecycleIntentKindDTO::kPrepare, ComponentLifecycleStateDTO::kPrepared,
                 kPlanNs),
       RecoverPlan(ComponentLifecycleStateDTO::kDisabled, kPlanNs)});
  const auto prepare = Intent(ComponentLifecycleIntentKindDTO::kPrepare, {expected});

  const auto by_plan = PrepareComponentLifecycleAdmission(prepare, Candidate(snapshot, {catalog}));
  ASSERT_TRUE(by_plan.ok()) << by_plan.error().message;
  EXPECT_EQ(by_plan.value().acceptance.timeout_duration_ns, kPlanNs);

  auto budgeted = Candidate(snapshot, {catalog});
  budgeted.lifecycle_budget_ns = kBudgetNs;
  const auto by_budget = PrepareComponentLifecycleAdmission(prepare, budgeted);
  ASSERT_TRUE(by_budget.ok()) << by_budget.error().message;
  EXPECT_EQ(by_budget.value().acceptance.timeout_duration_ns, kBudgetNs);
  EXPECT_EQ(by_budget.value().initial_status.remaining_duration_ns, kBudgetNs);
  EXPECT_TRUE(by_budget.value().initial_status.locally_valid());

  auto faulted = Snapshot({expected}, ComponentLifecycleStateDTO::kFaulted);
  faulted.provider_occurrences = snapshot.provider_occurrences;
  auto recover = Candidate(faulted, {catalog});
  recover.lifecycle_budget_ns = kBudgetNs;
  const auto recovered = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kRecover, {expected}), recover);
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  EXPECT_EQ(recovered.value().acceptance.timeout_duration_ns, kBudgetNs);

  const auto unsupported = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kEnable, {expected}), budgeted);
  ASSERT_FALSE(unsupported.ok());
  EXPECT_EQ(unsupported.error().code, "component_lifecycle_intent_unsupported");
}

TEST(ComponentLifecycleAdmission, ConfiguredDeviceLimitsUseExactScopeWithoutChangingCatalog) {
  auto plan = FixedPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                        ComponentLifecycleStateDTO::kPrepared, 1);
  plan.timeout_policy = DeviceConfiguredLifecycleLimitDTO{};
  std::optional<ContentDigest> digest;
  for (const Duration limit : {7'000LL, 11'000LL}) {
    auto snapshot = Snapshot({ExpectedDomain("a")});
    auto [expected, catalog] = ConfigureProviderDomain(snapshot, 0U, limit, {plan});
    SealSnapshot(snapshot);
    const auto intent = Intent(ComponentLifecycleIntentKindDTO::kPrepare, {expected});
    const auto prepared =
        PrepareComponentLifecycleAdmission(intent, Candidate(snapshot, {catalog}));
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    EXPECT_EQ(prepared.value().acceptance.timeout_duration_ns, limit);
    EXPECT_EQ(prepared.value().initial_status.remaining_duration_ns, limit);
    if (digest) {
      EXPECT_EQ(*digest, catalog.catalog_digest);
    }
    digest = catalog.catalog_digest;

    auto changed = *snapshot.domains[0].provider();
    ++changed.provider.lifecycle_command_timeout_ns;
    snapshot.domains[0].payload = changed;
    SealSnapshot(snapshot);
    EXPECT_FALSE(PrepareComponentLifecycleAdmission(intent, Candidate(snapshot, {catalog})).ok());
  }
}

TEST(ComponentLifecycleAdmission, ConfiguredPolicyRequiresValidActualProviderLimit) {
  auto plan = FixedPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                        ComponentLifecycleStateDTO::kPrepared, 1);
  plan.timeout_policy = DeviceConfiguredLifecycleLimitDTO{};
  const auto physical_expected = ExpectedDomain("a");
  auto physical = Snapshot({physical_expected});
  const auto catalog = Catalog(physical.provider_occurrences[0], {plan});
  EXPECT_FALSE(PrepareComponentLifecycleAdmission(
                   Intent(ComponentLifecycleIntentKindDTO::kPrepare, {physical_expected}),
                   Candidate(physical, {catalog}))
                   .ok());
  for (const Duration invalid : {0LL, -1LL, kMaxLifecycleTransactionTimeoutNs + 1LL}) {
    auto snapshot = Snapshot({physical_expected});
    auto [expected, provider_catalog] = ConfigureProviderDomain(snapshot, 0U, invalid, {plan});
    EXPECT_FALSE(PrepareComponentLifecycleAdmission(
                     Intent(ComponentLifecycleIntentKindDTO::kPrepare, {expected}),
                     Candidate(snapshot, {provider_catalog}))
                     .ok());
  }
}

TEST(ComponentLifecycleAdmission, MixedFixedAndConfiguredRecoverKeepsMinimumBudget) {
  const auto physical_expected = ExpectedDomain("a");
  auto snapshot =
      Snapshot({physical_expected, ExpectedDomain("b")}, ComponentLifecycleStateDTO::kFaulted);
  const auto physical_catalog =
      Catalog(snapshot.provider_occurrences[0],
              {RecoverPlan(ComponentLifecycleStateDTO::kPrepared, 4'000),
               RecoverPlan(ComponentLifecycleStateDTO::kDisabled, 6'000)});
  auto configured = RecoverPlan(ComponentLifecycleStateDTO::kPrepared, 1);
  configured.timeout_policy = DeviceConfiguredLifecycleLimitDTO{};
  auto [provider_expected, provider_catalog] =
      ConfigureProviderDomain(snapshot, 1U, 9'000, {configured});
  SealSnapshot(snapshot);
  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kRecover, {physical_expected, provider_expected}),
      Candidate(snapshot, {physical_catalog, provider_catalog}));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.timeout_duration_ns, 4'000);
}

TEST(ComponentLifecycleAdmission, TypedProviderFenceAndExpectationRecordsAreClosed) {
  auto snapshot = Snapshot({ExpectedDomain("a")});
  auto [expected, catalog] =
      ConfigureProviderDomain(snapshot, 0U, 7'000,
                              {FixedPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                         ComponentLifecycleStateDTO::kPrepared, 5'000)});
  const auto expected_bytes = detail::EncodeExpectedControlDomainRevisionRecord(expected);
  EXPECT_EQ(detail::DecodeExpectedControlDomainRevisionRecord(expected_bytes), expected);
  EXPECT_THROW((void)detail::DecodeExpectedControlDomainRevisionRecord(expected_bytes + "x"),
               std::invalid_argument);
  const auto& provider = snapshot.domains[0].provider()->provider;
  const ProviderConfigurationFenceOccurrenceDTO proof{provider.controllable_component_id,
                                                      provider.provider_instance_id,
                                                      provider.signal_configuration_digest,
                                                      {}};
  ConfigurationFenceScopeDTO scope{
      {snapshot.controllable_component_id},
      {{snapshot.domains[0].control_domain_id, snapshot.domains[0].control_domain_revision,
        snapshot.domains[0].control_domain_revision,
        ProviderConfigurationFenceDomainDTO{proof, proof}}},
      {},
      {}};
  ASSERT_TRUE(scope.valid());
  const auto bytes = detail::EncodeConfigurationFenceScopeRecord(scope);
  EXPECT_EQ(detail::DecodeConfigurationFenceScopeRecord(bytes), scope);
  EXPECT_THROW((void)detail::DecodeConfigurationFenceScopeRecord(bytes + "x"),
               std::invalid_argument);
  auto invalid = scope;
  auto& typed = std::get<ProviderConfigurationFenceDomainDTO>(invalid.domains[0].payload);
  typed.candidate_provider->controllable_component_id =
      ControllableComponentId("foreign.component");
  EXPECT_FALSE(invalid.valid());
  EXPECT_THROW((void)detail::EncodeConfigurationFenceScopeRecord(invalid), std::invalid_argument);
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
  std::get<std::int64_t>(forged_content.plans.front().timeout_policy) += 1;
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

TEST(ComponentLifecycleAdmission, DisableRejectsChangedDomainSetBeforeReservation) {
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
      Candidate(snapshot, {catalog_a, catalog_c}));

  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_configuration_stale");
}

TEST(ComponentLifecycleAdmission,
     OrdinaryDisableUsesTheCurrentSnapshotUnderTheIdleClaim) {
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected}, ComponentLifecycleStateDTO::kActive);
  const auto catalog = Catalog(
      snapshot.provider_occurrences.front(),
      {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                 ComponentLifecycleStateDTO::kDisabled, 6'000)});

  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected}),
      Candidate(snapshot, {catalog}));
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().initial_status.requested_state.state,
            ComponentLifecycleStateDTO::kDisabled);
  EXPECT_EQ(prepared.value().acceptance.timeout_duration_ns, 6'000);
}

TEST(ComponentLifecycleAdmission, DisableRejectsUnqualifiedCatalogBeforeReservation) {
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected}, ComponentLifecycleStateDTO::kActive);
  auto catalog = Catalog(snapshot.provider_occurrences.front(),
                         {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                                    ComponentLifecycleStateDTO::kDisabled, 6'000)});
  catalog.catalog_digest = ContentDigest("unqualified-catalog");

  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected}),
      Candidate(snapshot, {catalog}));

  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_catalog_mismatch");
}

TEST(ComponentLifecycleAdmission, StaleResolvedDisableDoesNotMintSuccessor) {
  const auto expected = ExpectedDomain("a");
  auto snapshot = Snapshot({expected}, ComponentLifecycleStateDTO::kActive);
  snapshot.component_lifecycle_generation = ComponentLifecycleGeneration(5U);
  const auto catalog = Catalog(snapshot.provider_occurrences.front(),
                               {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                                          ComponentLifecycleStateDTO::kDisabled, 6'000)});

  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected}),
      Candidate(snapshot, {catalog},
                ComponentLifecycleGeneration(5U)));

  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_current_snapshot_mismatch");
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
      Candidate(snapshot, {enable_catalog}, ComponentLifecycleGeneration(5U)));
  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_current_snapshot_mismatch");

  const auto disable_catalog = Catalog(snapshot.provider_occurrences.front(),
                                       {FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                                                  ComponentLifecycleStateDTO::kDisabled, 6'000)});
  prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected},
             ComponentLifecycleGeneration(6U)),
      Candidate(snapshot, {disable_catalog},
                ComponentLifecycleGeneration(5U)));
  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_current_snapshot_mismatch");
}

TEST(ComponentLifecycleAdmission, NoSnapshotDisableIsRefused) {
  const auto expected = ExpectedDomain("a");
  const auto prepared = PrepareComponentLifecycleAdmission(
      Intent(ComponentLifecycleIntentKindDTO::kDisable, {expected}),
      Candidate(std::nullopt, {}));
  ASSERT_FALSE(prepared.ok());
  EXPECT_EQ(prepared.error().code, "component_lifecycle_scope_unresolved");
}


TEST(ComponentLifecycleAdmission, ProviderFenceLineageCoversOnlyExactPredecessorSides) {
  auto snapshot = Snapshot({ExpectedDomain("a")});
  auto configured =
      ConfigureProviderDomain(snapshot, 0U, 30'000'000'000LL,
                              {FixedPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                         ComponentLifecycleStateDTO::kPrepared, 5'000)});
  (void)configured;
  const auto& domain = snapshot.domains.front();
  const auto& provider = *domain.provider();
  const ProviderConfigurationFenceOccurrenceDTO proof{snapshot.controllable_component_id,
                                                      provider.provider.provider_instance_id,
                                                      provider.provider.signal_configuration_digest,
                                                      {}};
  const ConfigurationFenceScopeDTO scope{
      {snapshot.controllable_component_id},
      {{domain.control_domain_id, domain.control_domain_revision, domain.control_domain_revision,
        ProviderConfigurationFenceDomainDTO{proof, proof}}},
      {},
      {}};
  const auto& revision = snapshot.active_publication_receipt.request.candidate_projection_revision;
  const ProviderConfigurationFenceLineageDTO actual{domain.control_domain_id,
                                                    provider.provider_runtime_epoch,
                                                    snapshot.component_lifecycle_generation};
  ConfigurationFenceAdmissionSnapshotDTO admission{
      {scope, RuntimeEpoch("manager.actual")},
      {provider.provider.provider_runtime_unit_id, RuntimeEpoch("host.successor"),
       RequestOwnerEpoch("host.actual"), ContentDigest("ledger.actual"),
       snapshot.active_publication_receipt.publication_generation},
      revision,
      snapshot.active_publication_receipt.durable_public_pointer_content_digest,
      1U,
      ConfigurationFenceGeneration(1U),
      {},
      0U,
      0U,
      ContentDigest("digest.actual"),
      {actual}};
  ApplyConfigurationFenceIntentDTO intent{
      {RequestId("request.actual"), RequestOwnerEpoch("owner.actual"),
       "manager", std::nullopt},
      {ApplyId("apply.actual"), ApplyGeneration(1U)},
      scope,
      ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      revision.retained_bundle_id,
      revision.graph_revision,
      1U,
      ConfigurationFenceGeneration(1U),
      {},
      "actual Apply",
      {actual}};
  EXPECT_TRUE(admission.presence_valid());
  EXPECT_TRUE(intent.valid());
  // This is captured predecessor lineage, never a new driver's Active proof.
  EXPECT_NE(actual.provider_runtime_epoch, admission.execution_host_epoch.runtime_epoch);
  auto missing = admission;
  missing.provider_lineages.clear();
  EXPECT_FALSE(missing.presence_valid());
  auto missing_intent = intent;
  missing_intent.expected_provider_lineages.clear();
  EXPECT_FALSE(missing_intent.valid());
  auto duplicate = admission;
  duplicate.provider_lineages.push_back(actual);
  EXPECT_FALSE(duplicate.presence_valid());
  auto foreign = intent;
  foreign.expected_provider_lineages.front().control_domain_id = ControlDomainId("foreign.domain");
  EXPECT_FALSE(foreign.valid());
  auto candidate_only = admission;
  auto& fenced = candidate_only.echoed_query.affected_scope.domains.front();
  fenced.predecessor_control_domain_revision.reset();
  std::get<ProviderConfigurationFenceDomainDTO>(fenced.payload).predecessor_provider.reset();
  EXPECT_FALSE(candidate_only.presence_valid());  // Unused predecessor receipt is forbidden.
  candidate_only.provider_lineages.clear();
  EXPECT_TRUE(candidate_only.presence_valid());
  auto physical = intent;
  physical.scope.domains.front().payload = ControllerConfigurationFenceDomainDTO{};
  physical.scope.bindings.push_back(
      {ControlBindingId("binding"), ControlBindingRevision("old"), ControlBindingRevision("next")});
  physical.scope.resources.push_back(ResourceId("physical.resource"));
  physical.expected_active_control_configurations.push_back(
      {domain.control_domain_id, ActiveControlConfigurationId("actual.configuration"),
       ActiveControlConfigurationGeneration(1U)});
  EXPECT_FALSE(physical.valid());  // Provider receipt cannot masquerade as controller proof.
  physical.expected_provider_lineages.clear();
  EXPECT_TRUE(physical.valid());
}

}  // namespace
}  // namespace manyforge::core
