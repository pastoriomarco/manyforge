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

struct ApplyAdmissionFixture final {
  ComponentLifecycleIntentDTO intent;
  ComponentLifecycleAdmissionCandidate candidate;
  ApplyComponentBringUpEvidenceDTO evidence;
  std::vector<ResolvedLifecycleControlDomainScopeDTO> candidate_domains;
  std::vector<ResolvedLifecycleProviderOccurrenceDTO> candidate_providers;
};

void SealSnapshot(ComponentLifecycleCurrentSnapshotDTO& snapshot) {
  const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot);
  if (!digest.ok()) throw std::runtime_error(digest.error().message);
  snapshot.snapshot_digest = digest.value();
}

[[nodiscard]] ApplyAdmissionFixture ApplyFixture(bool recovery = false, bool changed = false) {
  auto first = ExpectedDomain("a");
  if (!recovery || changed) {
    first.expected_active_control_configuration = {
        ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
  }
  if (changed) {
    first.control_domain_revision = ControlDomainRevision("candidate-domain-a");
    first.expected_control_bindings.front().control_binding_revision =
        ControlBindingRevision("candidate-binding-a");
  }
  std::vector<ExpectedControlDomainRevisionDTO> expected{first};
  if (changed) {
    auto added = ExpectedDomain("b");
    added.expected_active_control_configuration = {
        ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
    expected.push_back(added);
  }
  std::vector<ResolvedLifecycleControlDomainScopeDTO> candidate_domains;
  std::vector<ResolvedLifecycleProviderOccurrenceDTO> candidate_providers;
  std::vector<BrandLifecyclePlanCatalogDTO> catalogs;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const auto& domain = expected[index];
    candidate_domains.push_back(
        {domain.control_domain_id,
         domain.control_domain_revision,
         domain.controller_topology_id,
         domain.topology_revision,
         domain.expected_active_control_configuration.presence ==
                 ExpectedActiveControlConfigurationPresenceDTO::kPresent
             ? std::optional(ActiveConfiguration(domain))
             : std::nullopt,
         domain.expected_control_bindings,
         {ResourceId("candidate-resource-" + domain.control_domain_id.value())}});
    candidate_providers.push_back(Provider(candidate_domains.back(), index == 0U ? "c" : "d"));
    catalogs.push_back(
        Catalog(candidate_providers.back(),
                {FixedPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                           ComponentLifecycleStateDTO::kPrepared, index == 0U ? 5'000 : 3'000),
                 FixedPlan(ComponentLifecycleIntentKindDTO::kEnable,
                           ComponentLifecycleStateDTO::kActive, index == 0U ? 7'000 : 4'000),
                 FixedPlan(ComponentLifecycleIntentKindDTO::kDisable,
                           ComponentLifecycleStateDTO::kDisabled, index == 0U ? 6'000 : 2'000)}));
  }
  std::optional<ComponentLifecycleCurrentSnapshotDTO> predecessor;
  if (recovery) {
    std::vector<ExpectedControlDomainRevisionDTO> previous{ExpectedDomain("a")};
    if (changed) previous.push_back(ExpectedDomain("c"));
    predecessor = Snapshot(previous, ComponentLifecycleStateDTO::kActive);
    for (auto& provider : predecessor->provider_occurrences) {
      static_cast<void>(
          Catalog(provider, {FixedPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                       ComponentLifecycleStateDTO::kPrepared, 9'000),
                             FixedPlan(ComponentLifecycleIntentKindDTO::kEnable,
                                       ComponentLifecycleStateDTO::kActive, 10'000)}));
    }
    SealSnapshot(*predecessor);
  }
  auto intent = Intent(ComponentLifecycleIntentKindDTO::kPrepare, expected);
  auto candidate = Candidate(predecessor, catalogs);
  const auto predecessor_generation =
      predecessor ? std::optional(predecessor->component_lifecycle_generation) : std::nullopt;
  const auto old_generation = predecessor_generation ? predecessor_generation->value() : 0U;
  intent.expected_component_lifecycle_generation = ComponentLifecycleGeneration(old_generation);
  intent.apply_generation_selection = ApplyComponentGenerationSelectionDTO{
      predecessor_generation, ComponentLifecycleGeneration(old_generation + 1U), 0U,
      predecessor ? std::optional(ApplyComponentQualifiedPredecessorDTO{
                        predecessor->component_lifecycle_generation, predecessor->snapshot_digest})
                  : std::nullopt};
  candidate.durable_component_generation_predecessor =
      intent.expected_component_lifecycle_generation;
  candidate.apply_generation_selection = intent.apply_generation_selection;
  ConfigurationFenceScopeDTO scope{{intent.controllable_component_id}, {}, {}, {}};
  std::map<ControlDomainId, ConfigurationFenceDomainScopeDTO> domains;
  std::map<ControlBindingId, ConfigurationFenceBindingScopeDTO> bindings;
  const auto collect = [&](const std::vector<ResolvedLifecycleControlDomainScopeDTO>& values,
                           bool successor) {
    for (const auto& value : values) {
      auto domain = domains
                        .try_emplace(value.control_domain_id,
                                     ConfigurationFenceDomainScopeDTO{value.control_domain_id,
                                                                      std::nullopt, std::nullopt})
                        .first;
      (successor ? domain->second.candidate_control_domain_revision
                 : domain->second.predecessor_control_domain_revision) =
          value.control_domain_revision;
      for (const auto& binding : value.current_control_bindings) {
        auto entry = bindings
                         .try_emplace(binding.control_binding_id,
                                      ConfigurationFenceBindingScopeDTO{binding.control_binding_id,
                                                                        std::nullopt, std::nullopt})
                         .first;
        (successor ? entry->second.candidate_control_binding_revision
                   : entry->second.predecessor_control_binding_revision) =
            binding.control_binding_revision;
      }
      scope.resources.insert(scope.resources.end(), value.current_resources.begin(),
                             value.current_resources.end());
    }
  };
  if (predecessor) collect(predecessor->domains, false);
  collect(candidate_domains, true);
  for (const auto& [id, domain] : domains) scope.domains.push_back(domain);
  for (const auto& [id, binding] : bindings) scope.bindings.push_back(binding);
  std::sort(scope.resources.begin(), scope.resources.end());
  scope.resources.erase(std::unique(scope.resources.begin(), scope.resources.end()),
                        scope.resources.end());
  const ApplyFenceOwnerDTO parent_owner{ApplyId("apply.1"), ApplyGeneration(1U)};
  const ApplyComponentBringUpReferenceDTO parent{parent_owner,
                                                 ContentDigest(Sha('c'))};
  const ApplyComponentBringUpAuthorityDTO authority{
      parent,
      {{CellAdmissionClaimKindDTO::kManagerApply,
        CellAdmissionOwnerRoleDTO::kManager,
        RequestOwnerEpoch("manager.1"),
        parent.owner.apply_id.value(),
        RequestId("parent.request.1"),
        parent.plan_digest,
        intent.expected_admission_generation,
        {},
        std::nullopt,
        std::nullopt},
       CellAdmissionClaimGeneration(1U),
       AdmissionGeneration(1U),
       50},
      {parent.owner, ConfigurationFenceId("fence.1"), ConfigurationFenceGeneration(1U), scope,
       RuntimeEpoch("manager-runtime.1")}};
  const ProjectionRevisionDTO projection{
      RetainedBundleId("candidate.bundle.1"),
      GraphRevision(changed ? "candidate.graph.1" : "graph.1"),
      ActivePublication().request.candidate_projection_revision.scene_revision};
  ApplyComponentBringUpEvidenceDTO evidence{authority,
                                            scope,
                                            ContentDigest(Sha('e')),
                                            projection,
                                            ContentDigest(Sha('f')),
                                            512U,
                                            {RuntimeUnitId("host.1"), RuntimeEpoch("host-epoch.1")},
                                            std::nullopt,
                                            std::nullopt,
                                            std::nullopt};
  if (predecessor) {
    evidence.recovery = ApplyComponentBringUpRecoveryEvidenceDTO{
        predecessor->snapshot_digest, "incident.1", StopId("stop.1"), StopGeneration(1U)};
    SelectedControllerOccurrenceDTO occurrence{ExecutionBindingEpoch(2U), {}};
    std::vector<ControlDomainId> successor_domains;
    for (const auto& domain : predecessor->domains)
      occurrence.domains.push_back({domain.control_domain_id, 4U});
    for (const auto& domain : candidate_domains)
      successor_domains.push_back(domain.control_domain_id);
    const auto successor =
        ComputeSelectedControllerSuccessorOccurrence(occurrence, successor_domains, true);
    if (!successor.ok()) throw std::runtime_error(successor.error().message);
    evidence.predecessor_occurrence = occurrence;
    evidence.successor_occurrence_floor = successor.value();
  }
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      intent, projection, evidence.candidate_retained_bundle_digest,
      evidence.candidate_retained_bundle_bytes, evidence.original_execution_host.runtime_unit_id,
      evidence.original_execution_host.runtime_epoch,
      authority.held_fence_query.manager_runtime_epoch, parent);
  return {std::move(intent), std::move(candidate), std::move(evidence),
          std::move(candidate_domains), std::move(candidate_providers)};
}

[[nodiscard]] Result<PreparedComponentLifecycleAdmission> PrepareApply(
    const ApplyAdmissionFixture& fixture) {
  return PrepareApplyComponentBringUpAdmission(fixture.intent, fixture.candidate, fixture.evidence,
                                               fixture.candidate_domains,
                                               fixture.candidate_providers);
}

void SealApplyRequest(ApplyAdmissionFixture& fixture) {
  auto& evidence = fixture.evidence;
  evidence.original_request_payload_digest = ApplyDelegatedComponentLifecycleSemanticRequestDigest(
      fixture.intent, evidence.candidate_projection_revision,
      evidence.candidate_retained_bundle_digest, evidence.candidate_retained_bundle_bytes,
      evidence.original_execution_host.runtime_unit_id,
      evidence.original_execution_host.runtime_epoch,
      evidence.parent_authority.held_fence_query.manager_runtime_epoch,
      evidence.parent_authority.parent);
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

TEST(ApplyComponentBringUpAdmission, InitialPrepareEnableDisableDoNotInventCurrentObservations) {
  for (const auto kind :
       {ComponentLifecycleIntentKindDTO::kPrepare, ComponentLifecycleIntentKindDTO::kEnable,
        ComponentLifecycleIntentKindDTO::kDisable}) {
    auto fixture = ApplyFixture();
    fixture.intent.intent = kind;
    SealApplyRequest(fixture);
    const auto prepared = PrepareApply(fixture);
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    const auto& value = prepared.value();
    EXPECT_EQ(value.acceptance.lifecycle_transaction_id,
              fixture.candidate.lifecycle_transaction_id);
    EXPECT_EQ(value.acceptance.lifecycle_transaction_generation,
              fixture.candidate.lifecycle_transaction_generation);
    EXPECT_EQ(value.acceptance.component_lifecycle_generation, ComponentLifecycleGeneration(1U));
    EXPECT_EQ(fixture.intent.expected_component_lifecycle_generation.value(), 0U);
    EXPECT_FALSE(fixture.intent.apply_generation_selection->predecessor_generation);
    const auto& catalog = fixture.candidate.lifecycle_plan_catalogs.front();
    const auto plan = std::find_if(catalog.plans.begin(), catalog.plans.end(),
                                   [&](const auto& entry) { return entry.intent == kind; });
    ASSERT_NE(plan, catalog.plans.end());
    EXPECT_EQ(value.acceptance.timeout_duration_ns, plan->timeout_duration_ns);
    EXPECT_EQ(value.initial_status.requested_state.state, plan->requested_state);
    EXPECT_EQ(plan->requested_postcondition, StatePredicate(plan->requested_state));
    if (kind == ComponentLifecycleIntentKindDTO::kDisable) {
      EXPECT_EQ(plan->requested_state, ComponentLifecycleStateDTO::kDisabled);
    }
    EXPECT_EQ(value.initial_status.phase, ComponentLifecycleTransactionPhaseDTO::kAccepted);
    EXPECT_FALSE(value.initial_status.terminal_result);
    EXPECT_EQ(value.initial_status.apply_bring_up, fixture.evidence);
    EXPECT_FALSE(value.initial_status.previous_state);
    EXPECT_FALSE(value.initial_status.current_state);
    EXPECT_FALSE(value.initial_status.apply_bring_up->recovery);
    EXPECT_EQ(value.qualified_lifecycle_plan_catalogs, fixture.candidate.lifecycle_plan_catalogs);
    EXPECT_EQ(value.reserved_control_domains,
              std::vector<ControlDomainId>{ControlDomainId("domain-a")});
    ASSERT_EQ(value.initial_status.domains.size(), 1U);
    const auto& domain = value.initial_status.domains.front();
    EXPECT_EQ(domain.requested_state, value.initial_status.requested_state);
    EXPECT_EQ(domain.expected_domain, fixture.intent.expected_control_domains.front());
    EXPECT_FALSE(domain.current_scope);
    EXPECT_FALSE(domain.resolved_provider_occurrence);
    EXPECT_FALSE(domain.current_state);
    EXPECT_EQ(domain.action_state, ComponentLifecycleDomainActionStateDTO::kNotStarted);
    EXPECT_EQ(domain.effect, ComponentLifecycleDomainEffectDTO::kNone);
    EXPECT_TRUE(domain.primitive_attempts.empty());
    EXPECT_TRUE(value.initial_status.locally_valid());
  }
}

TEST(ApplyComponentBringUpAdmission, ContainedRecoveryDisableKeepsActivePredecessorBeforeEffects) {
  for (const bool changed : {false, true}) {
    auto fixture = ApplyFixture(true, changed);
    fixture.intent.intent = ComponentLifecycleIntentKindDTO::kDisable;
    SealApplyRequest(fixture);
    const auto prepared = PrepareApply(fixture);
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    const auto& value = prepared.value();
    EXPECT_EQ(value.initial_status.requested_state.state, ComponentLifecycleStateDTO::kDisabled);
    EXPECT_EQ(value.initial_status.previous_state, ComponentLifecycleStateDTO::kActive);
    EXPECT_EQ(value.initial_status.current_state, ComponentLifecycleStateDTO::kActive);
    EXPECT_EQ(value.initial_status.phase, ComponentLifecycleTransactionPhaseDTO::kAccepted);
    EXPECT_FALSE(value.initial_status.terminal_result);
    EXPECT_EQ(value.acceptance.timeout_duration_ns, changed ? 2'000 : 6'000);
    EXPECT_EQ(value.initial_status.apply_bring_up, fixture.evidence);
    EXPECT_EQ(value.qualified_lifecycle_plan_catalogs, fixture.candidate.lifecycle_plan_catalogs);
    ASSERT_TRUE(value.qualified_lifecycle_plan_catalogs);
    ASSERT_FALSE(value.initial_status.domains.empty());
    for (const auto& catalog : *value.qualified_lifecycle_plan_catalogs) {
      const auto plan =
          std::find_if(catalog.plans.begin(), catalog.plans.end(), [](const auto& entry) {
            return entry.intent == ComponentLifecycleIntentKindDTO::kDisable;
          });
      ASSERT_NE(plan, catalog.plans.end());
      EXPECT_EQ(plan->requested_state, ComponentLifecycleStateDTO::kDisabled);
      EXPECT_EQ(plan->requested_postcondition,
                StatePredicate(ComponentLifecycleStateDTO::kDisabled));
    }
    EXPECT_EQ(value.initial_status.domains.front().current_scope,
              fixture.candidate.current_snapshot->domains.front());
    EXPECT_EQ(value.initial_status.domains.front().resolved_provider_occurrence,
              fixture.candidate.current_snapshot->provider_occurrences.front());
    EXPECT_NE(value.initial_status.domains.front().resolved_provider_occurrence,
              fixture.candidate_providers.front());
    EXPECT_EQ(value.reserved_control_domains,
              changed ? (std::vector<ControlDomainId>{ControlDomainId("domain-a"),
                                                      ControlDomainId("domain-b"),
                                                      ControlDomainId("domain-c")})
                      : std::vector<ControlDomainId>{ControlDomainId("domain-a")});
    for (const auto& domain : value.initial_status.domains) {
      EXPECT_EQ(domain.requested_state, value.initial_status.requested_state);
      EXPECT_EQ(domain.effect, ComponentLifecycleDomainEffectDTO::kNone);
      EXPECT_TRUE(domain.primitive_attempts.empty());
      if (domain.current_scope) {
        EXPECT_EQ(domain.current_state, ComponentLifecycleStateDTO::kActive);
      } else {
        EXPECT_FALSE(domain.current_state);
        EXPECT_FALSE(domain.resolved_provider_occurrence);
        EXPECT_EQ(domain.action_state, ComponentLifecycleDomainActionStateDTO::kNotStarted);
      }
    }
    EXPECT_TRUE(value.initial_status.locally_valid());
  }
}

TEST(ApplyComponentBringUpAdmission, RecoveryKeepsTruthfulActivePredecessorAndWholeChangedUnion) {
  for (const bool changed : {false, true}) {
    const auto fixture = ApplyFixture(true, changed);
    const auto prepared = PrepareApply(fixture);
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    const auto& value = prepared.value();
    EXPECT_EQ(value.initial_status.current_state, ComponentLifecycleStateDTO::kActive);
    EXPECT_EQ(value.initial_status.previous_state, ComponentLifecycleStateDTO::kActive);
    EXPECT_EQ(value.acceptance.timeout_duration_ns, changed ? 3'000 : 5'000);
    EXPECT_EQ(value.initial_status.apply_bring_up, fixture.evidence);
    EXPECT_EQ(value.qualified_lifecycle_plan_catalogs, fixture.candidate.lifecycle_plan_catalogs);
    EXPECT_EQ(value.initial_status.domains.front().current_scope,
              fixture.candidate.current_snapshot->domains.front());
    EXPECT_EQ(value.initial_status.domains.front().resolved_provider_occurrence,
              fixture.candidate.current_snapshot->provider_occurrences.front());
    EXPECT_NE(value.initial_status.domains.front().resolved_provider_occurrence,
              fixture.candidate_providers.front());
    if (changed) {
      EXPECT_EQ(
          value.reserved_control_domains,
          (std::vector<ControlDomainId>{ControlDomainId("domain-a"), ControlDomainId("domain-b"),
                                        ControlDomainId("domain-c")}));
      ASSERT_EQ(value.initial_status.domains.size(), 3U);
      const auto& added = value.initial_status.domains[1];
      EXPECT_EQ(added.expected_domain, fixture.intent.expected_control_domains[1]);
      EXPECT_FALSE(added.current_scope);
      EXPECT_FALSE(added.resolved_provider_occurrence);
      EXPECT_FALSE(added.current_state);
      const auto& retired = value.initial_status.domains[2];
      EXPECT_FALSE(retired.expected_domain);
      EXPECT_EQ(retired.current_scope, fixture.candidate.current_snapshot->domains[1]);
      EXPECT_EQ(retired.current_state, ComponentLifecycleStateDTO::kActive);
    }
    for (const auto& domain : value.initial_status.domains) {
      EXPECT_EQ(domain.effect, ComponentLifecycleDomainEffectDTO::kNone);
      EXPECT_TRUE(domain.primitive_attempts.empty());
    }
    EXPECT_TRUE(value.initial_status.locally_valid());
  }
}

TEST(ApplyComponentBringUpAdmission,
     UsesOwnerSelectedGenerationWithoutInventingAnInitialPredecessorOrReusingReservations) {
  const std::vector<ApplyComponentGenerationSelectionDTO> selections{
      {std::nullopt, ComponentLifecycleGeneration(1U), 0U},
      {ComponentLifecycleGeneration(7U), ComponentLifecycleGeneration(8U), 7U},
      {ComponentLifecycleGeneration(7U), ComponentLifecycleGeneration(10U), 9U},
      {std::nullopt, ComponentLifecycleGeneration(9U), 8U}};
  for (const auto& selection : selections) {
    ASSERT_TRUE(selection.locally_valid());
    auto fixture = ApplyFixture();
    fixture.intent.apply_generation_selection = selection;
    fixture.intent.expected_component_lifecycle_generation = ComponentLifecycleGeneration(
        selection.predecessor_generation ? selection.predecessor_generation->value() : 0U);
    fixture.candidate.apply_generation_selection = selection;
    fixture.candidate.durable_component_generation_predecessor =
        fixture.intent.expected_component_lifecycle_generation;
    SealApplyRequest(fixture);
    const auto prepared = PrepareApply(fixture);
    ASSERT_TRUE(prepared.ok()) << prepared.error().message;
    EXPECT_EQ(prepared.value().acceptance.component_lifecycle_generation,
              selection.candidate_generation);
    EXPECT_FALSE(prepared.value().initial_status.previous_state);
    EXPECT_FALSE(prepared.value().initial_status.current_state);
    EXPECT_FALSE(PrepareComponentLifecycleAdmission(fixture.intent, fixture.candidate).ok());

    fixture.candidate.apply_generation_selection->candidate_generation =
        ComponentLifecycleGeneration(selection.candidate_generation.value() + 1U);
    const auto changed = PrepareApply(fixture);
    ASSERT_FALSE(changed.ok());
    EXPECT_EQ(changed.error().details.at("effect"), "not_started");
  }
}

TEST(ApplyComponentBringUpAdmission,
     QualifiedBootstrapPreservesTheActualSnapshotPredecessorWithoutInventingAWatermark) {
  auto fixture = ApplyFixture(true);
  auto& snapshot = *fixture.candidate.current_snapshot;
  snapshot.component_lifecycle_generation = ComponentLifecycleGeneration(1U);
  snapshot.component_lifecycle_state = ComponentLifecycleStateDTO::kDeclared;
  SealSnapshot(snapshot);
  fixture.evidence.recovery.reset();
  fixture.intent.expected_component_lifecycle_generation = ComponentLifecycleGeneration(1U);
  fixture.intent.apply_generation_selection = ApplyComponentGenerationSelectionDTO{
      std::nullopt, ComponentLifecycleGeneration(2U), 0U,
      ApplyComponentQualifiedPredecessorDTO{snapshot.component_lifecycle_generation,
                                            snapshot.snapshot_digest}};
  fixture.candidate.durable_component_generation_predecessor = ComponentLifecycleGeneration(1U);
  fixture.candidate.apply_generation_selection = fixture.intent.apply_generation_selection;
  SealApplyRequest(fixture);
  const auto prepared = PrepareApply(fixture);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.component_lifecycle_generation,
            ComponentLifecycleGeneration(2U));
  EXPECT_EQ(prepared.value().initial_status.previous_state,
            std::optional(ComponentLifecycleStateDTO::kDeclared));
  EXPECT_EQ(prepared.value().initial_status.current_state,
            std::optional(ComponentLifecycleStateDTO::kDeclared));
  EXPECT_FALSE(fixture.intent.apply_generation_selection->predecessor_generation);
  ASSERT_TRUE(fixture.intent.apply_generation_selection->owner_qualified_predecessor);
  EXPECT_EQ(fixture.intent.apply_generation_selection->owner_qualified_predecessor->snapshot_digest,
            snapshot.snapshot_digest);

  auto forged = fixture;
  forged.intent.apply_generation_selection->owner_qualified_predecessor->snapshot_digest =
      ContentDigest(Sha('9'));
  forged.candidate.apply_generation_selection = forged.intent.apply_generation_selection;
  SealApplyRequest(forged);
  EXPECT_FALSE(PrepareApply(forged).ok());
  auto missing = fixture;
  missing.candidate.current_snapshot.reset();
  EXPECT_FALSE(PrepareApply(missing).ok());
  auto missing_seal = fixture;
  missing_seal.intent.apply_generation_selection->owner_qualified_predecessor.reset();
  missing_seal.candidate.apply_generation_selection = missing_seal.intent.apply_generation_selection;
  EXPECT_FALSE(missing_seal.intent.locally_valid());
  EXPECT_FALSE(PrepareApply(missing_seal).ok());
}

TEST(ApplyComponentBringUpAdmission, RejectsForeignSnapshotProviderCatalogAndPhysicalUnion) {
  const auto refused = [](const ApplyAdmissionFixture& fixture, const std::string& code) {
    const auto result = PrepareApply(fixture);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, code);
    EXPECT_EQ(result.error().details.at("effect"), "not_started");
  };
  auto fixture = ApplyFixture(true, true);
  fixture.candidate.current_snapshot->controllable_component_id =
      ControllableComponentId("foreign.component");
  SealSnapshot(*fixture.candidate.current_snapshot);
  refused(fixture, "component_lifecycle_current_snapshot_mismatch");
  fixture = ApplyFixture(true, true);
  fixture.candidate.current_snapshot->component_lifecycle_generation =
      ComponentLifecycleGeneration(5U);
  SealSnapshot(*fixture.candidate.current_snapshot);
  refused(fixture, "component_lifecycle_current_snapshot_mismatch");
  fixture = ApplyFixture(true, true);
  fixture.candidate_providers.front().control_domain_revision =
      ControlDomainRevision("foreign.revision");
  refused(fixture, "component_lifecycle_configuration_stale");
  fixture = ApplyFixture();
  fixture.candidate.lifecycle_plan_catalogs.front().plans.front().timeout_duration_ns += 1;
  refused(fixture, "component_lifecycle_catalog_mismatch");
  fixture = ApplyFixture();
  fixture.candidate_domains.front().current_resources.push_back(ResourceId("unfenced.resource"));
  std::sort(fixture.candidate_domains.front().current_resources.begin(),
            fixture.candidate_domains.front().current_resources.end());
  refused(fixture, "component_lifecycle_configuration_stale");
  fixture = ApplyFixture(true, true);
  fixture.evidence.recovery->predecessor_snapshot_digest = ContentDigest(Sha('0'));
  refused(fixture, "component_lifecycle_current_snapshot_mismatch");
  fixture = ApplyFixture(true);
  fixture.candidate.current_snapshot.reset();
  // An owner-qualified predecessor requires its exact snapshot before recovery.
  const auto missing_qualified = PrepareApply(fixture);
  ASSERT_FALSE(missing_qualified.ok());
  EXPECT_EQ(missing_qualified.error().code, "component_lifecycle_configuration_stale");
  EXPECT_EQ(missing_qualified.error().details.at("reason"),
            "qualified_predecessor_snapshot_missing");
  EXPECT_EQ(missing_qualified.error().details.at("effect"), "not_started");

  // A durable predecessor without the optional owner qualification still
  // reaches the separate contained-recovery missing-snapshot refusal.
  fixture.intent.apply_generation_selection->owner_qualified_predecessor.reset();
  fixture.candidate.apply_generation_selection = fixture.intent.apply_generation_selection;
  SealApplyRequest(fixture);
  ASSERT_TRUE(fixture.intent.locally_valid());
  const auto missing_recovery = PrepareApply(fixture);
  ASSERT_FALSE(missing_recovery.ok());
  EXPECT_EQ(missing_recovery.error().code, "component_lifecycle_current_snapshot_mismatch");
  EXPECT_EQ(missing_recovery.error().details.at("reason"), "current_snapshot_missing");
  EXPECT_EQ(missing_recovery.error().details.at("effect"), "not_started");
  fixture = ApplyFixture();
  fixture.evidence.parent_authority.parent_claim.intent.owner_transaction_id = "foreign.apply";
  refused(fixture, "component_lifecycle_admission_invariant_failed");
  fixture = ApplyFixture();
  fixture.evidence.original_request_payload_digest = ContentDigest(Sha('0'));
  refused(fixture, "component_lifecycle_admission_invariant_failed");
}

TEST(ApplyComponentBringUpAdmission, RejectsUnsupportedProgramAndOverflowingAdmission) {
  auto fixture = ApplyFixture();
  fixture.candidate.lifecycle_plan_catalogs = {
      Catalog(fixture.candidate_providers.front(),
              {FixedPlan(ComponentLifecycleIntentKindDTO::kEnable,
                         ComponentLifecycleStateDTO::kActive, 7'000)})};
  const auto unsupported = PrepareApply(fixture);
  ASSERT_FALSE(unsupported.ok());
  EXPECT_EQ(unsupported.error().code, "component_lifecycle_intent_unsupported");
  EXPECT_EQ(unsupported.error().details.at("effect"), "not_started");
  fixture = ApplyFixture();
  fixture.intent.intent = ComponentLifecycleIntentKindDTO::kRecover;
  EXPECT_FALSE(PrepareApply(fixture).ok());
  fixture = ApplyFixture();
  fixture.intent.program_node_evidence =
      ProgramNodeAdmissionEvidenceDTO{RequestOwnerEpoch("program.owner"),
                                      ProgramRunId("program.run"),
                                      CellAdmissionClaimGeneration(2U),
                                      "node.1",
                                      "attempt.1",
                                      "cclc.lifecycle"};
  EXPECT_FALSE(fixture.intent.locally_valid());
  EXPECT_FALSE(PrepareApply(fixture).ok());
  fixture = ApplyFixture();
  fixture.intent.expected_component_lifecycle_generation = ComponentLifecycleGeneration(5U);
  EXPECT_FALSE(PrepareApply(fixture).ok());
  fixture = ApplyFixture();
  fixture.candidate.durable_component_generation_predecessor =
      ComponentLifecycleGeneration(std::numeric_limits<std::uint64_t>::max());
  fixture.intent.expected_component_lifecycle_generation =
      fixture.candidate.durable_component_generation_predecessor;
  EXPECT_FALSE(PrepareApply(fixture).ok());
  fixture = ApplyFixture();
  fixture.candidate.accepted_at_ns = std::numeric_limits<std::int64_t>::max();
  const auto overflow = PrepareApply(fixture);
  ASSERT_FALSE(overflow.ok());
  EXPECT_EQ(overflow.error().details.at("reason"), "deadline_overflow");
}

}  // namespace
}  // namespace manyforge::core
