#include <gtest/gtest.h>

#include <manyforge_core/types/component_lifecycle.hpp>
#include <string>
#include <type_traits>

namespace manyforge::core {
namespace {

ExpectedActiveControlConfigurationDTO absent_configuration() {
  return {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt, std::nullopt};
}

ExpectedActiveControlConfigurationDTO present_configuration() {
  return {ExpectedActiveControlConfigurationPresenceDTO::kPresent,
          ActiveControlConfigurationId("configuration-1"),
          ActiveControlConfigurationGeneration(7U)};
}

ExpectedControlBindingRevisionDTO binding() {
  return {ControlBindingId("binding-1"), ControlBindingRevision("binding-revision-1"),
          ControlRouteId("shared-route")};
}

ActiveControlConfigurationDTO active_configuration() {
  const auto route = binding();
  return {ActiveControlConfigurationId("configuration-1"),
          ControlDomainId("domain-1"),
          ControlDomainRevision("domain-revision-1"),
          {{route.control_binding_id, route.control_binding_revision, route.control_route_id}},
          ActivationSetId("activation-set-1"),
          ControllerTopologyId("topology-1"),
          TopologyRevision("topology-revision-1"),
          RuntimeUnitId("runtime-1"),
          RuntimeEpoch("runtime-epoch-1"),
          1U,
          {},
          ActiveControlConfigurationGeneration(7U),
          ActiveControlConfigurationStateDTO::kReconciled};
}

ResolvedLifecycleControlDomainScopeDTO current_scope() {
  return {ControlDomainId("domain-1"),         ControlDomainRevision("domain-revision-1"),
          ControllerTopologyId("topology-1"),  TopologyRevision("topology-revision-1"),
          active_configuration(),              {binding()},
          {ResourceId("resource-1")}};
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

BrandLifecycleObservationDTO observation() {
  return {LifecycleTransactionId("transaction-1"),
          LifecycleTransactionGeneration(4U),
          ControllableComponentId("component-1"),
          ControlDomainId("domain-1"),
          ControlDomainRevision("domain-revision-1"),
          ProviderInstanceId("provider-1"),
          ProviderManifestId("manifest-1"),
          ProviderManifestRevision("manifest-revision-1"),
          RuntimeUnitId("runtime-1"),
          RuntimeEpoch("runtime-epoch-1"),
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

TEST(ComponentLifecycleStrongIds, PreserveSemanticSeparation) {
  static_assert(!std::is_same_v<LifecycleFactId, LifecycleTransactionId>);
  static_assert(!std::is_same_v<ComponentLifecycleGeneration, LifecycleTransactionGeneration>);
}

TEST(ComponentLifecycleExpectation, RepresentsAbsentConfigurationWithoutSentinel) {
  EXPECT_TRUE(absent_configuration().locally_valid());
  EXPECT_TRUE(present_configuration().locally_valid());

  auto invalid = absent_configuration();
  invalid.active_control_configuration_id = ActiveControlConfigurationId("sentinel-is-not-absence");
  EXPECT_FALSE(invalid.locally_valid());
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

TEST(ComponentLifecycleObservation, RequiresCanonicalFreshFacts) {
  auto sample = observation();
  ASSERT_TRUE(sample.locally_valid());

  auto regressed_time = sample;
  regressed_time.fresh_until_ns = regressed_time.received_timestamp_ns;
  EXPECT_FALSE(regressed_time.locally_valid());

  auto noncanonical = sample;
  std::swap(noncanonical.facts[0], noncanonical.facts[1]);
  EXPECT_FALSE(noncanonical.locally_valid());
}

TEST(ComponentLifecycleResolvedScope, MatchesExactTopologyBindingRouteAndConfiguration) {
  auto resolved = current_scope();
  ASSERT_TRUE(resolved.locally_valid());

  resolved.controller().current_control_bindings[0].control_route_id = ControlRouteId("other-route");
  EXPECT_FALSE(resolved.locally_valid());
}

TEST(ComponentLifecycleCurrentSnapshot, JoinsOneCanonicalProviderOccurrenceToEveryCompleteDomain) {
  const auto scope = current_scope();
  const ResolvedLifecycleProviderOccurrenceDTO provider{
      scope.control_domain_id,
      scope.control_domain_revision,
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
  const auto scope = current_scope();
  const ResolvedLifecycleProviderOccurrenceDTO provider{
      scope.control_domain_id,
      scope.control_domain_revision,
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

}  // namespace
}  // namespace manyforge::core
