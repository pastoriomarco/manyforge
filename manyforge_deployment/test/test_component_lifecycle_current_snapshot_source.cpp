#include <gtest/gtest.h>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#if defined(MANYFORGE_SNAPSHOT_TEST_HAS_SQLITE_CCLC)
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#endif
#include <manyforge_deployment/component_lifecycle_current_snapshot_source.hpp>
#include <manyforge_deployment/route_selection_current_snapshot_source.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

#include "lite6_retained_consumer_fixture.hpp"

namespace manyforge::deployment {
namespace {

class ScopedTrustedRoot final {
public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-lifecycle-snapshot-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::logic_error("test could not create its trusted root");
    }
  }

  ~ScopedTrustedRoot() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

[[nodiscard]] core::ErrorDTO test_error(std::string reason) {
  return {"test_read_failed",
          "test source rejected the read",
          {{"reason", std::move(reason)}}};
}

class ActivePublicationReader final
    : public core::IActiveBundlePublicationReader {
public:
  explicit ActivePublicationReader(
      core::ActiveBundlePublicationReceiptDTO receipt)
      : receipt_(std::move(receipt)) {}

  [[nodiscard]] core::Result<core::ActiveBundlePublicationReceiptDTO>
  ReadActiveBundlePublication(
      core::ActiveBundlePublicationGeneration generation) const override {
    if (generation != receipt_.publication_generation) {
      return core::Result<core::ActiveBundlePublicationReceiptDTO>::Err(
          test_error("generation_mismatch"));
    }
    ++read_count_;
    if (change_after_first_read_ && read_count_ > 1U) {
      auto changed = receipt_;
      changed.durable_public_pointer_content_digest =
          core::ContentDigest("changed-public-pointer");
      return core::Result<core::ActiveBundlePublicationReceiptDTO>::Ok(
          std::move(changed));
    }
    return core::Result<core::ActiveBundlePublicationReceiptDTO>::Ok(receipt_);
  }

  void change_after_first_read() noexcept { change_after_first_read_ = true; }

private:
  core::ActiveBundlePublicationReceiptDTO receipt_;
  mutable std::size_t read_count_{};
  bool change_after_first_read_{};
};

class CurrentFactReader final : public core::ICCLCCurrentFactReader {
public:
  explicit CurrentFactReader(core::CCLCCurrentFactsReadbackDTO initial_readback)
      : readback(std::move(initial_readback)) {}

  core::CCLCCurrentFactsReadbackDTO readback;

  [[nodiscard]] core::Result<core::CCLCCurrentFactsReadbackDTO>
  LoadCurrentSnapshotFacts(
      const std::vector<core::ControllableComponentId> &components,
      const std::vector<core::ControlDomainId> &domains) override {
    std::vector<core::ControlDomainId> actual;
    actual.reserve(readback.domains.size());
    for (const auto &domain : readback.domains) {
      actual.push_back(domain.control_domain_id);
    }
    std::vector<core::ControllableComponentId> actual_components;
    actual_components.reserve(readback.components.size());
    for (const auto &component : readback.components) {
      actual_components.push_back(component.controllable_component_id);
    }
    if (components != actual_components || domains != actual) {
      return core::Result<core::CCLCCurrentFactsReadbackDTO>::Err(
          test_error("query_mismatch"));
    }
    return core::Result<core::CCLCCurrentFactsReadbackDTO>::Ok(readback);
  }
};

class CatalogSource final : public core::ICCLCLifecyclePlanCatalogSource {
public:
  explicit CatalogSource(core::BrandLifecyclePlanCatalogDTO catalog)
      : catalog_(std::move(catalog)) {}

  [[nodiscard]] core::Result<core::BrandLifecyclePlanCatalogDTO>
  LoadExactLifecyclePlanCatalog(
      const core::ProviderManifestId &manifest_id,
      const core::ProviderManifestRevision &manifest_revision,
      const core::ContentDigest &digest) const override {
    if (manifest_id != catalog_.provider_manifest_id ||
        manifest_revision != catalog_.provider_manifest_revision ||
        digest != catalog_.catalog_digest) {
      return core::Result<core::BrandLifecyclePlanCatalogDTO>::Err(
          test_error("catalog_key_mismatch"));
    }
    return core::Result<core::BrandLifecyclePlanCatalogDTO>::Ok(catalog_);
  }

private:
  core::BrandLifecyclePlanCatalogDTO catalog_;
};

[[nodiscard]] core::ActiveBundlePublicationReceiptDTO
publication(const RetainedBundle &bundle) {
  const std::string scene_digest =
      "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  const core::ProjectionRevisionDTO revision{
      bundle.retained_bundle_id(),
      bundle.value().cell_graph.graph_revision,
      {core::SceneId(scene_digest), 1U, core::ContentDigest(scene_digest)}};
  return {{core::BootstrapPublicationTransactionDTO{
               core::BootstrapId("bootstrap.lifecycle"),
               core::BootstrapGeneration(1U)},
           revision, revision.retained_bundle_id, revision.graph_revision,
           std::nullopt, core::ContentDigest("private-materialization")},
          core::ActiveBundlePublicationGeneration(1U),
          core::ContentDigest("active-public-pointer")};
}

[[nodiscard]] core::BrandLifecyclePlanCatalogDTO
catalog_for(const core::ControlBindingDTO &binding) {
  const core::LifecyclePredicateDTO declared{
      {core::ComponentLifecycleStateDTO::kDeclared}, {}};
  const core::LifecyclePredicateDTO prepared{
      {core::ComponentLifecycleStateDTO::kPrepared}, {}};
  auto result = core::MakeBrandLifecyclePlanCatalog(
      binding.provider_manifest_id, binding.provider_manifest_revision,
      {{core::ComponentLifecycleIntentKindDTO::kPrepare,
        core::ComponentLifecycleStateDTO::kPrepared,
        std::nullopt,
        prepared,
        30'000'000'000LL,
        {{core::LifecyclePrimitiveId("prepare"), declared, prepared}}}});
  if (!result.ok()) {
    throw std::logic_error("test could not construct lifecycle catalog: " +
                           result.error().message);
  }
  return std::move(result.value());
}

struct SourceFacts final {
  const core::ControllableComponentDTO *component;
  const core::ControlDomainDTO *domain;
  const core::ControlBindingDTO *component_binding;
  const core::DesiredControlConfigurationDTO *desired;
  core::BrandLifecyclePlanCatalogDTO catalog;
  core::ResolvedLifecycleProviderOccurrenceDTO provider;
};

[[nodiscard]] SourceFacts source_facts(const RetainedBundle &bundle) {
  const auto &graph = bundle.value().cell_graph;
  if (graph.controllable_components.size() != 1U) {
    throw std::logic_error("test fixture must contain one component");
  }
  const auto *component = &graph.controllable_components.front();
  const auto binding =
      std::find_if(graph.control_bindings.begin(), graph.control_bindings.end(),
                   [&](const auto &candidate) {
                     return candidate.controllable_component_id ==
                            component->controllable_component_id;
                   });
  if (binding == graph.control_bindings.end()) {
    throw std::logic_error("test fixture has no component binding");
  }
  const auto domain = std::find_if(
      graph.control_domains.begin(), graph.control_domains.end(),
      [&](const auto &candidate) {
        return candidate.control_domain_id == binding->control_domain_id;
      });
  const auto desired = std::find_if(
      bundle.value().desired_control_configurations.begin(),
      bundle.value().desired_control_configurations.end(),
      [&](const auto &candidate) {
        return candidate.control_domain_id == binding->control_domain_id;
      });
  if (domain == graph.control_domains.end() ||
      desired == bundle.value().desired_control_configurations.end()) {
    throw std::logic_error("test fixture has no complete component domain");
  }
  auto catalog = catalog_for(*binding);
  core::ResolvedLifecycleProviderOccurrenceDTO provider{
      domain->control_domain_id,
      domain->control_domain_revision,
      binding->provider_instance_id,
      binding->provider_manifest_id,
      binding->provider_manifest_revision,
      binding->provider_runtime_unit_id,
      core::RuntimeEpoch("runtime-epoch.lifecycle.1"),
      catalog.catalog_digest};
  return {component, &*domain,           &*binding,
          &*desired, std::move(catalog), std::move(provider)};
}

[[nodiscard]] CurrentFactReader bootstrap_facts(const SourceFacts &facts) {
  core::ComponentLifecycleCurrentFactReadbackDTO component{
      facts.component->controllable_component_id, std::nullopt, std::nullopt};
  std::vector<core::CCLCCurrentDomainFactReadbackDTO> domains;
  domains.push_back({facts.domain->control_domain_id, std::nullopt});
  std::vector<core::ComponentLifecycleCurrentFactReadbackDTO> components;
  components.push_back(std::move(component));
  core::CCLCCurrentFactsReadbackDTO readback{std::move(components),
                                             std::move(domains)};
  return CurrentFactReader(std::move(readback));
}

[[nodiscard]] core::ActiveControlConfigurationDTO
active_configuration(const SourceFacts &facts) {
  std::vector<core::ActiveControlConfigurationBindingRouteDTO> routes;
  for (const auto &route : facts.desired->binding_routes) {
    routes.push_back({route.control_binding_id, route.control_binding_revision,
                      route.control_route_id});
  }
  return {core::ActiveControlConfigurationId("active-config.1"),
          facts.domain->control_domain_id,
          facts.domain->control_domain_revision,
          std::move(routes),
          facts.desired->activation_set_id,
          facts.domain->controller_topology_id,
          facts.domain->topology_revision,
          facts.provider.provider_runtime_unit_id,
          facts.provider.provider_runtime_epoch,
          1U,
          {},
          core::ActiveControlConfigurationGeneration(1U),
          core::ActiveControlConfigurationStateDTO::kReconciled};
}

class ComponentLifecycleCurrentSnapshotSourceTest : public ::testing::Test {
protected:
  static void SetUpTestSuite() {
    root_ = std::make_unique<ScopedTrustedRoot>();
    fixture_ = std::make_unique<test::DurableLite6RetainedConsumerFixture>(
        test::materialize_lite6_ros2_control_retained_consumer_fixture(root_->path()));
  }

  static void TearDownTestSuite() {
    fixture_.reset();
    root_.reset();
  }

  [[nodiscard]] static const RetainedBundle &bundle() {
    return *fixture_->bundle;
  }

  [[nodiscard]] static const std::filesystem::path &package_root() {
    return root_->path();
  }

private:
  inline static std::unique_ptr<ScopedTrustedRoot> root_;
  inline static std::unique_ptr<test::DurableLite6RetainedConsumerFixture>
      fixture_;
};

TEST_F(ComponentLifecycleCurrentSnapshotSourceTest,
       BootstrapsDeclaredOnlyWhenTheDurableFactIsNeverSeen) {
  const auto facts = source_facts(bundle());
  const auto receipt = publication(bundle());
  ActivePublicationReader publication_reader(receipt);
  auto current = bootstrap_facts(facts);
  CatalogSource catalogs(facts.catalog);
  VerifiedComponentLifecycleCurrentSnapshotSource source(
      bundle(), publication_reader, receipt, current, catalogs,
      {facts.provider});

  const auto snapshot = source.ReadCurrentComponentSnapshot(
      facts.component->controllable_component_id);
  ASSERT_TRUE(snapshot.ok()) << snapshot.error().message;
  EXPECT_EQ(snapshot.value().component_lifecycle_generation,
            core::ComponentLifecycleGeneration(1U));
  EXPECT_EQ(snapshot.value().component_lifecycle_state,
            core::ComponentLifecycleStateDTO::kDeclared);
  ASSERT_EQ(snapshot.value().domains.size(), 1U);
  EXPECT_FALSE(snapshot.value()
                   .domains.front()
                   .active_control_configuration.has_value());
  EXPECT_TRUE(
      core::VerifyComponentLifecycleCurrentSnapshot(snapshot.value()).ok());
}

#if defined(MANYFORGE_SNAPSHOT_TEST_HAS_SQLITE_CCLC)
TEST_F(ComponentLifecycleCurrentSnapshotSourceTest,
       GenuineBootstrapSnapshotDoesNotBecomeADurableGenerationSeedForApply) {
  ScopedTrustedRoot journal_root;
  core::SqliteCCLCJournal journal(
      journal_root.path() / std::string(core::kCCLCJournalFilename),
      bundle().value().cell_graph.cell_id);
  const auto owner = journal.BeginOwnerEpoch(
      core::RequestOwnerEpoch("bootstrap.owner"), 1, 1024U);
  ASSERT_TRUE(owner.ok()) << owner.error().message;
  const auto facts = source_facts(bundle());
  const auto receipt = publication(bundle());
  ActivePublicationReader publication_reader(receipt);
  CatalogSource catalogs(facts.catalog);
  VerifiedComponentLifecycleCurrentSnapshotSource source(
      bundle(), publication_reader, receipt, journal, catalogs, {facts.provider});
  const auto snapshot = source.ReadCurrentComponentSnapshot(
      facts.component->controllable_component_id);
  ASSERT_TRUE(snapshot.ok()) << snapshot.error().message;
  EXPECT_EQ(snapshot.value().component_lifecycle_generation,
            core::ComponentLifecycleGeneration(1U));
  EXPECT_EQ(snapshot.value().component_lifecycle_state,
            core::ComponentLifecycleStateDTO::kDeclared);
  const auto selected = journal.ReadApplyComponentGenerationSelection(
      owner.value().request_owner_epoch, facts.component->controllable_component_id);
  ASSERT_TRUE(selected.ok()) << selected.error().message;
  EXPECT_FALSE(selected.value().predecessor_generation);
  EXPECT_EQ(selected.value().candidate_generation,
            core::ComponentLifecycleGeneration(1U));

  std::vector<core::ExpectedControlDomainRevisionDTO> expected;
  for (const auto &domain : snapshot.value().domains) {
    ASSERT_FALSE(domain.active_control_configuration);
    expected.push_back({domain.control_domain_id, domain.control_domain_revision,
                        domain.controller_topology_id, domain.topology_revision,
                        {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
                         std::nullopt, std::nullopt},
                        domain.current_control_bindings});
  }
  core::ComponentLifecycleIntentDTO intent{
      {core::RequestId("bootstrap.prepare"), owner.value().request_owner_epoch,
       "test", std::nullopt},
      facts.component->controllable_component_id,
      core::ComponentLifecycleIntentKindDTO::kPrepare,
      snapshot.value().active_publication_receipt.request.candidate_graph_revision,
      snapshot.value().component_lifecycle_generation, expected, std::nullopt,
      {core::RequestTransportDTO::kInternal, core::RequestAssuranceDTO::kUnverified,
       std::nullopt}, core::AdmissionGeneration(1U), std::nullopt};
  const core::ComponentLifecycleAdmissionCandidate candidate{
      core::LifecycleTransactionId("bootstrap.lifecycle.1"),
      core::LifecycleTransactionGeneration(1U),
      snapshot.value().component_lifecycle_generation, 2, snapshot.value(),
      {facts.catalog}};
  const auto prepared = core::PrepareComponentLifecycleAdmission(intent, candidate);
  ASSERT_TRUE(prepared.ok()) << prepared.error().message;
  EXPECT_EQ(prepared.value().acceptance.component_lifecycle_generation,
            core::ComponentLifecycleGeneration(2U));

  // A naked journal-absence value cannot replace the genuine snapshot.
  intent.apply_generation_selection = selected.value();
  EXPECT_FALSE(intent.locally_valid());
  const auto qualified = journal.ReadApplyComponentGenerationSelection(
      owner.value().request_owner_epoch, facts.component->controllable_component_id,
      core::ApplyComponentQualifiedPredecessorDTO{
          snapshot.value().component_lifecycle_generation, snapshot.value().snapshot_digest});
  ASSERT_TRUE(qualified.ok()) << qualified.error().message;
  EXPECT_FALSE(qualified.value().predecessor_generation);
  EXPECT_EQ(qualified.value().candidate_generation, core::ComponentLifecycleGeneration(2U));
  ASSERT_TRUE(qualified.value().owner_qualified_predecessor);
  EXPECT_EQ(qualified.value().owner_qualified_predecessor->snapshot_digest,
            snapshot.value().snapshot_digest);
  intent.apply_generation_selection = qualified.value();
  EXPECT_TRUE(intent.locally_valid());
  EXPECT_FALSE(core::PrepareComponentLifecycleAdmission(intent, candidate).ok());
  EXPECT_EQ(snapshot.value().component_lifecycle_generation,
            core::ComponentLifecycleGeneration(1U));
  EXPECT_EQ(journal.ReadApplyComponentGenerationSelection(
                owner.value().request_owner_epoch,
                facts.component->controllable_component_id).value(), selected.value());
}
#endif

TEST_F(ComponentLifecycleCurrentSnapshotSourceTest,
       UsesOneAtomicDurableLifecycleAndActiveConfigurationReadback) {
  const auto facts = source_facts(bundle());
  const auto receipt = publication(bundle());
  ActivePublicationReader publication_reader(receipt);
  auto current = bootstrap_facts(facts);
  current.readback.components.front().durable_generation =
      core::ComponentLifecycleGeneration(4U);
  current.readback.components.front().current_state =
      core::ComponentLifecycleStateDTO::kPrepared;
  current.readback.domains.front().current_active_control_configuration =
      active_configuration(facts);
  ASSERT_TRUE(current.readback.locally_valid());
  CatalogSource catalogs(facts.catalog);
  VerifiedComponentLifecycleCurrentSnapshotSource source(
      bundle(), publication_reader, receipt, current, catalogs,
      {facts.provider});

  const auto snapshot = source.ReadCurrentComponentSnapshot(
      facts.component->controllable_component_id);
  ASSERT_TRUE(snapshot.ok()) << snapshot.error().message;
  EXPECT_EQ(snapshot.value().component_lifecycle_generation,
            core::ComponentLifecycleGeneration(4U));
  EXPECT_EQ(snapshot.value().component_lifecycle_state,
            core::ComponentLifecycleStateDTO::kPrepared);
  ASSERT_TRUE(snapshot.value()
                  .domains.front()
                  .active_control_configuration.has_value());
  EXPECT_EQ(*snapshot.value().domains.front().active_control_configuration,
            active_configuration(facts));
}

TEST_F(ComponentLifecycleCurrentSnapshotSourceTest,
       RejectsUnresolvedDurableStateAndPublicationChangeDuringJoin) {
  const auto facts = source_facts(bundle());
  const auto receipt = publication(bundle());
  ActivePublicationReader publication_reader(receipt);
  auto current = bootstrap_facts(facts);
  current.readback.components.front().durable_generation =
      core::ComponentLifecycleGeneration(4U);
  ASSERT_TRUE(current.readback.locally_valid());
  CatalogSource catalogs(facts.catalog);
  VerifiedComponentLifecycleCurrentSnapshotSource unresolved(
      bundle(), publication_reader, receipt, current, catalogs,
      {facts.provider});
  EXPECT_FALSE(unresolved
                   .ReadCurrentComponentSnapshot(
                       facts.component->controllable_component_id)
                   .ok());

  current = bootstrap_facts(facts);
  ActivePublicationReader changing_publication(receipt);
  changing_publication.change_after_first_read();
  VerifiedComponentLifecycleCurrentSnapshotSource torn(
      bundle(), changing_publication, receipt, current, catalogs,
      {facts.provider});
  EXPECT_FALSE(torn.ReadCurrentComponentSnapshot(
                       facts.component->controllable_component_id)
                   .ok());
}

TEST_F(ComponentLifecycleCurrentSnapshotSourceTest,
       RejectsForeignProviderOccurrenceWithoutSubstitution) {
  const auto facts = source_facts(bundle());
  const auto receipt = publication(bundle());
  ActivePublicationReader publication_reader(receipt);
  auto current = bootstrap_facts(facts);
  CatalogSource catalogs(facts.catalog);
  auto foreign = facts.provider;
  foreign.provider_instance_id = core::ProviderInstanceId("foreign-provider");
  VerifiedComponentLifecycleCurrentSnapshotSource source(
      bundle(), publication_reader, receipt, current, catalogs, {foreign});

  const auto snapshot = source.ReadCurrentComponentSnapshot(
      facts.component->controllable_component_id);
  EXPECT_FALSE(snapshot.ok());
}

TEST_F(ComponentLifecycleCurrentSnapshotSourceTest,
       OneComponentLifecycleProviderMayCoverMultipleControlDomains) {
  ScopedTrustedRoot root;
  const auto retained = test::
      materialize_lite6_ros2_control_motion_arm_gripper_retained_consumer_fixture(
          root.path());
  const auto &graph = retained.bundle->value().cell_graph;
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  ASSERT_EQ(graph.control_domains.size(), 2U);
  ASSERT_EQ(graph.control_bindings.size(), 2U);

  const auto lifecycle_binding =
      std::find_if(graph.control_bindings.begin(), graph.control_bindings.end(),
                   [](const auto &binding) {
                     return binding.capability_id ==
                            core::CapabilityId("execution.trajectory.execute");
                   });
  ASSERT_NE(lifecycle_binding, graph.control_bindings.end());
  const auto catalog = catalog_for(*lifecycle_binding);

  std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> providers;
  std::vector<core::CCLCCurrentDomainFactReadbackDTO> domain_facts;
  providers.reserve(graph.control_domains.size());
  domain_facts.reserve(graph.control_domains.size());
  for (const auto &domain : graph.control_domains) {
    providers.push_back({domain.control_domain_id,
                         domain.control_domain_revision,
                         lifecycle_binding->provider_instance_id,
                         lifecycle_binding->provider_manifest_id,
                         lifecycle_binding->provider_manifest_revision,
                         lifecycle_binding->provider_runtime_unit_id,
                         core::RuntimeEpoch("runtime-epoch.lifecycle.1"),
                         catalog.catalog_digest});
    domain_facts.push_back({domain.control_domain_id, std::nullopt});
  }
  std::sort(providers.begin(), providers.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.control_domain_id < rhs.control_domain_id;
            });
  std::sort(domain_facts.begin(), domain_facts.end(),
            [](const auto &lhs, const auto &rhs) {
              return lhs.control_domain_id < rhs.control_domain_id;
            });
  CurrentFactReader current(
      {{{graph.controllable_components.front().controllable_component_id,
         std::nullopt, std::nullopt}},
       std::move(domain_facts)});
  const auto receipt = publication(*retained.bundle);
  ActivePublicationReader publication_reader(receipt);
  CatalogSource catalogs(catalog);
  VerifiedComponentLifecycleCurrentSnapshotSource source(
      *retained.bundle, publication_reader, receipt, current, catalogs,
      providers);

  const auto snapshot = source.ReadCurrentComponentSnapshot(
      graph.controllable_components.front().controllable_component_id);
  ASSERT_TRUE(snapshot.ok()) << snapshot.error().message;
  EXPECT_EQ(snapshot.value().domains.size(), 2U);
  EXPECT_EQ(snapshot.value().provider_occurrences, providers);
  EXPECT_TRUE(
      core::VerifyComponentLifecycleCurrentSnapshot(snapshot.value()).ok());
}

TEST_F(ComponentLifecycleCurrentSnapshotSourceTest,
       RouteSnapshotJoinsTheCompleteDurableDomainAndRetainedProduct) {
  const auto facts = source_facts(bundle());
  const auto receipt = publication(bundle());
  ActivePublicationReader publication_reader(receipt);
  auto current = bootstrap_facts(facts);
  current.readback.components.front().durable_generation =
      core::ComponentLifecycleGeneration(4U);
  current.readback.components.front().current_state =
      core::ComponentLifecycleStateDTO::kPrepared;
  current.readback.domains.front().current_active_control_configuration =
      active_configuration(facts);
  ASSERT_TRUE(current.readback.locally_valid());
  const FilesystemVerifiedRetainedProductArtifactSource artifacts(
      package_root());
  VerifiedRouteSelectionCurrentSnapshotSource source(
      bundle(), publication_reader, receipt, current, artifacts,
      {{facts.domain->control_domain_id,
        facts.provider.provider_runtime_unit_id,
        facts.provider.provider_runtime_epoch}});

  const auto snapshot = source.ReadCurrentRouteSelectionSnapshot(
      bundle().retained_bundle_id(), bundle().value().cell_graph.graph_revision,
      *facts.desired);
  ASSERT_TRUE(snapshot.ok()) << snapshot.error().message;
  ASSERT_EQ(snapshot.value().value().component_expectations.size(), 1U);
  EXPECT_EQ(snapshot.value()
                .value()
                .component_expectations.front()
                .expected_component_lifecycle_generation,
            core::ComponentLifecycleGeneration(4U));
  EXPECT_EQ(snapshot.value().value().active_control_configuration,
            active_configuration(facts));
  std::vector<core::ResourceId> expected_resources;
  for (const auto &resource : facts.component_binding->resource_bindings) {
    expected_resources.push_back(resource.resource_id);
  }
  std::sort(expected_resources.begin(), expected_resources.end());
  expected_resources.erase(
      std::unique(expected_resources.begin(), expected_resources.end()),
      expected_resources.end());
  EXPECT_EQ(snapshot.value().value().configuration_fence_scope.resources,
            expected_resources);
  EXPECT_EQ(snapshot.value()
                .expected_controller_domain()
                .value()
                .source_runtime_unit_id,
            facts.provider.provider_runtime_unit_id);

  const auto templates = source.ReadCurrentRouteSelectionTemplates(
      facts.component_binding->controllable_component_id);
  ASSERT_TRUE(templates.ok()) << templates.error().message;
  ASSERT_EQ(templates.value().templates.size(), 1U);
  EXPECT_EQ(templates.value().retained_bundle_id,
            bundle().retained_bundle_id());
  EXPECT_EQ(templates.value().graph_revision,
            bundle().value().cell_graph.graph_revision);
  EXPECT_EQ(templates.value().templates.front().desired_configuration,
            *facts.desired);
  EXPECT_EQ(
      templates.value()
          .templates.front()
          .expected_active_control_configuration
          .active_control_configuration_generation,
      active_configuration(facts).active_control_configuration_generation);
  EXPECT_TRUE(templates.value().locally_valid());

  VerifiedRouteSelectionCurrentSnapshotSource replacement_source(
      bundle(), publication_reader, receipt, current, artifacts,
      {{facts.domain->control_domain_id,
        facts.provider.provider_runtime_unit_id,
        core::RuntimeEpoch("replacement-controller-epoch")}},
      [configuration =
           active_configuration(facts)](const core::ControlDomainId &domain_id)
          -> core::Result<std::optional<core::ActiveControlConfigurationDTO>> {
        if (domain_id != configuration.control_domain_id) {
          return core::
              Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
                  std::nullopt);
        }
        return core::
            Result<std::optional<core::ActiveControlConfigurationDTO>>::Ok(
                configuration);
      });
  const auto replacement = replacement_source.ReadCurrentRouteSelectionSnapshot(
      bundle().retained_bundle_id(), bundle().value().cell_graph.graph_revision,
      *facts.desired);
  ASSERT_TRUE(replacement.ok()) << replacement.error().message;
  EXPECT_EQ(replacement.value()
                .value()
                .active_control_configuration->source_runtime_epoch,
            facts.provider.provider_runtime_epoch);
  EXPECT_EQ(replacement.value()
                .expected_controller_domain()
                .value()
                .source_runtime_epoch,
            core::RuntimeEpoch("replacement-controller-epoch"));
}

TEST_F(ComponentLifecycleCurrentSnapshotSourceTest,
       RouteSnapshotRejectsUnpreparedTornAndForeignControllerTruth) {
  const auto facts = source_facts(bundle());
  const auto receipt = publication(bundle());
  const FilesystemVerifiedRetainedProductArtifactSource artifacts(
      package_root());

  ActivePublicationReader bootstrap_publication(receipt);
  auto bootstrap = bootstrap_facts(facts);
  VerifiedRouteSelectionCurrentSnapshotSource unprepared(
      bundle(), bootstrap_publication, receipt, bootstrap, artifacts,
      {{facts.domain->control_domain_id,
        facts.provider.provider_runtime_unit_id,
        facts.provider.provider_runtime_epoch}});
  EXPECT_FALSE(unprepared
                   .ReadCurrentRouteSelectionSnapshot(
                       bundle().retained_bundle_id(),
                       bundle().value().cell_graph.graph_revision,
                       *facts.desired)
                   .ok());

  auto prepared = bootstrap_facts(facts);
  prepared.readback.components.front().durable_generation =
      core::ComponentLifecycleGeneration(4U);
  prepared.readback.components.front().current_state =
      core::ComponentLifecycleStateDTO::kPrepared;
  prepared.readback.domains.front().current_active_control_configuration =
      active_configuration(facts);

  ActivePublicationReader changing(receipt);
  changing.change_after_first_read();
  VerifiedRouteSelectionCurrentSnapshotSource torn(
      bundle(), changing, receipt, prepared, artifacts,
      {{facts.domain->control_domain_id,
        facts.provider.provider_runtime_unit_id,
        facts.provider.provider_runtime_epoch}});
  EXPECT_FALSE(torn.ReadCurrentRouteSelectionSnapshot(
                       bundle().retained_bundle_id(),
                       bundle().value().cell_graph.graph_revision,
                       *facts.desired)
                   .ok());

  ActivePublicationReader stable(receipt);
  VerifiedRouteSelectionCurrentSnapshotSource foreign(
      bundle(), stable, receipt, prepared, artifacts,
      {{facts.domain->control_domain_id,
        core::RuntimeUnitId("foreign-controller-runtime"),
        facts.provider.provider_runtime_epoch}});
  EXPECT_FALSE(foreign
                   .ReadCurrentRouteSelectionSnapshot(
                       bundle().retained_bundle_id(),
                       bundle().value().cell_graph.graph_revision,
                       *facts.desired)
                   .ok());
}

TEST_F(ComponentLifecycleCurrentSnapshotSourceTest,
       ConfigurationObservationAcceptsDeclaredComponentsOnlyOnItsOwnRead) {
  const auto facts = source_facts(bundle());
  const auto receipt = publication(bundle());
  const FilesystemVerifiedRetainedProductArtifactSource artifacts(
      package_root());
  const auto kernel_current =
      [](std::optional<core::ActiveControlConfigurationDTO> configuration) {
        return [configuration](const core::ControlDomainId &)
                   -> core::Result<
                       std::optional<core::ActiveControlConfigurationDTO>> {
          return core::Result<
              std::optional<core::ActiveControlConfigurationDTO>>::
              Ok(configuration);
        };
      };
  const std::vector<ControllerLifecycleSourceOccurrenceDTO> sources{
      {facts.domain->control_domain_id, facts.provider.provider_runtime_unit_id,
       facts.provider.provider_runtime_epoch}};

  // An adopted ROS Apply: the component is Declared, the route is current.
  auto declared = bootstrap_facts(facts);
  declared.readback.components.front().durable_generation =
      core::ComponentLifecycleGeneration(1U);
  declared.readback.components.front().current_state =
      core::ComponentLifecycleStateDTO::kDeclared;
  declared.readback.domains.front().current_active_control_configuration =
      active_configuration(facts);
  ASSERT_TRUE(declared.readback.locally_valid());
  ActivePublicationReader declared_publication(receipt);
  VerifiedRouteSelectionCurrentSnapshotSource declared_source(
      bundle(), declared_publication, receipt, declared, artifacts, sources,
      kernel_current(active_configuration(facts)));
  const auto selection = declared_source.ReadCurrentRouteSelectionSnapshot(
      bundle().retained_bundle_id(), bundle().value().cell_graph.graph_revision,
      *facts.desired);
  ASSERT_FALSE(selection.ok());
  EXPECT_EQ(selection.error().details.at("reason"),
            "route_snapshot_verification_failed");
  const auto observed = declared_source.ReadCurrentRouteConfigurationObservation(
      bundle().retained_bundle_id(), bundle().value().cell_graph.graph_revision,
      *facts.desired);
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  EXPECT_EQ(observed.value().value().active_control_configuration,
            active_configuration(facts));
  EXPECT_EQ(observed.value().expected_controller_domain().value().source_runtime_unit_id,
            facts.provider.provider_runtime_unit_id);

  // No lifecycle fact at all (and a numbered fact without a state) is
  // still observable; the Kernel's current route is the configuration.
  auto missing = bootstrap_facts(facts);
  missing.readback.components.front().durable_generation =
      core::ComponentLifecycleGeneration(2U);
  ActivePublicationReader missing_publication(receipt);
  VerifiedRouteSelectionCurrentSnapshotSource missing_source(
      bundle(), missing_publication, receipt, missing, artifacts, sources,
      kernel_current(active_configuration(facts)));
  EXPECT_FALSE(missing_source
                   .ReadCurrentRouteSelectionSnapshot(
                       bundle().retained_bundle_id(),
                       bundle().value().cell_graph.graph_revision,
                       *facts.desired)
                   .ok());
  const auto missing_observed =
      missing_source.ReadCurrentRouteConfigurationObservation(
          bundle().retained_bundle_id(),
          bundle().value().cell_graph.graph_revision, *facts.desired);
  ASSERT_TRUE(missing_observed.ok()) << missing_observed.error().message;
  EXPECT_EQ(missing_observed.value().value().active_control_configuration,
            active_configuration(facts));

  // CCLC durable and Kernel current configuration must still agree.
  auto other = active_configuration(facts);
  other.active_control_configuration_generation =
      core::ActiveControlConfigurationGeneration(2U);
  ActivePublicationReader disagreeing_publication(receipt);
  VerifiedRouteSelectionCurrentSnapshotSource disagreeing(
      bundle(), disagreeing_publication, receipt, declared, artifacts, sources,
      kernel_current(other));
  const auto disagreement = disagreeing.ReadCurrentRouteConfigurationObservation(
      bundle().retained_bundle_id(), bundle().value().cell_graph.graph_revision,
      *facts.desired);
  ASSERT_FALSE(disagreement.ok());
  EXPECT_EQ(disagreement.error().details.at("reason"),
            "current_configuration_owner_mismatch");

  // The retained identity and the controller source stay exact.
  EXPECT_FALSE(declared_source
                   .ReadCurrentRouteConfigurationObservation(
                       core::RetainedBundleId("bundle.foreign"),
                       bundle().value().cell_graph.graph_revision,
                       *facts.desired)
                   .ok());
  ActivePublicationReader foreign_publication(receipt);
  VerifiedRouteSelectionCurrentSnapshotSource foreign(
      bundle(), foreign_publication, receipt, declared, artifacts,
      {{facts.domain->control_domain_id,
        core::RuntimeUnitId("foreign-controller-runtime"),
        facts.provider.provider_runtime_epoch}},
      kernel_current(active_configuration(facts)));
  EXPECT_FALSE(foreign
                   .ReadCurrentRouteConfigurationObservation(
                       bundle().retained_bundle_id(),
                       bundle().value().cell_graph.graph_revision,
                       *facts.desired)
                   .ok());
}

} // namespace
} // namespace manyforge::deployment
