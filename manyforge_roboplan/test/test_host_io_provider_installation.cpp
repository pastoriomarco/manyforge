#include "host_io_provider_installation.hpp"
#include "host_io_signal_runtime.hpp"
#include "io_provider_lifecycle_product.hpp"

#include "../../manyforge_deployment/test/io_retained_fixture.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/build_owned_lifecycle_plan_catalog_source.hpp>
#include <manyforge_core/component_lifecycle_coordinator.hpp>
#include <manyforge_core/io_provider_lifecycle_catalog.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_core/prepared_operation_participant_codec.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/types/command_schema.hpp>
#include <manyforge_deployment/component_lifecycle_current_snapshot_source.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <memory>
#include <optional>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {
namespace {
void checked(bool value, const std::string &message) {
  if (!value)
    throw std::logic_error(message);
}
template <class T> T take(core::Result<T> value) {
  if (!value.ok())
    throw std::logic_error(value.error().code + ": " + value.error().message);
  return std::move(value.value());
}
class TemporaryRoot final {
public:
  TemporaryRoot() {
    path = std::filesystem::temp_directory_path() /
           ("manyforge-host-io-installation-" + std::to_string(::getpid()) +
            "-" + std::to_string(next.fetch_add(1U)));
    checked(std::filesystem::create_directory(path),
            "fixture root was not created");
  }
  ~TemporaryRoot() {
    std::error_code error;
    std::filesystem::remove_all(path, error);
  }
  std::filesystem::path path;

private:
  static inline std::atomic<std::uint64_t> next{};
};

// This uses the real Composition implementation, actual qualified product,
// driver, channel/engine, stable cell and CCLC journal/current-source owner.
// It exercises the trusted helper boundary, not Manager Apply authority.
class HostIoProviderInstallationTest : public ::testing::Test {
protected:
  void Build(bool input = false, bool output = true, bool second = false,
             bool shared_channel = false,
             std::string_view output_profile = {}) {
    package = temporary.path / "package";
    checked(std::filesystem::create_directory(package), "package root missing");
    bundle = deployment::test::materialize_boolean_io_retained_fixture(
                 package, input, output, false, second, shared_channel,
                 nullptr, output_profile)
                 .bundle;
    checked(bundle != nullptr, "actual retained fixture product missing");
    checked(bundle->value().cell_graph.model_instances.empty() &&
                bundle->value().cell_graph.control_bindings.empty(),
            "helper fixture must remain genuinely model-free");
    checked(bundle->value().compiled_products.symbol_role_maps.size() == 1U,
            "actual Scenario candidate model-map product missing or repeated");
    deployment::FilesystemVerifiedRetainedModelMapSource maps(package);
    model_map =
        maps.get_verified({bundle->retained_bundle_id(),
                           bundle->value()
                               .compiled_products.symbol_role_maps.front()
                               .product_id});
    checked(model_map.has_value(), "actual model-free retained map missing");
    deployment::FilesystemCatalogArtifactStore catalogs(package);
    direct_bindings =
        deployment::compile_direct_control_program_binding_inventory(
            *bundle, *model_map, catalogs)
            .io;
    checked(direct_bindings.size() == 1U, "actual direct I/O target missing");
    clock = std::make_shared<WallClock>();
    scheduler = std::make_shared<SimpleScheduler>();
    const auto cell_id = bundle->value().cell_graph.cell_id;
    core::ResourceAddressedExecutionKernelConfig config{
        core::RequestOwnerEpoch("helper.kernel"), cell_id, 32U};
    config.empty_runtime_retained_bundle_id = bundle->retained_bundle_id();
    config.empty_runtime_graph_revision =
        bundle->value().cell_graph.graph_revision;
    config.manager_liveness_required_set = core::ManagerLivenessRequiredSetDTO{
        cell_id,
        bundle->retained_bundle_id(),
        core::ComputeRequiredRuntimeSetDigest({}),
        core::RuntimeUnitId("helper.manager"),
        core::RuntimeEpoch("helper.manager.epoch"),
        core::RequestOwnerEpoch("helper.manager.owner"),
        {}};
    // Use the same durable CCLC owner readers as the production host. The
    // coordinator writes its actual journal owner before requesting rotation;
    // neither startup nor later claim settlement accepts an unproved owner.
    const auto lifecycle_path =
        temporary.path / std::string(core::kCCLCJournalFilename);
    config.cell_admission_reconcile_validator =
        [lifecycle_path, cell_id](
            const std::vector<core::CellAdmissionClaimDTO> &held,
            const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> &owners,
            const core::CellAdmissionReconcileDTO &request) {
          return core::ValidateCCLCCellAdmissionReconciliationReadOnly(
              lifecycle_path, cell_id, held, owners, request);
        };
    config.cell_admission_settlement_validator =
        [lifecycle_path,
         cell_id](const core::CellAdmissionClaimDTO &claim,
                  const core::CellAdmissionClaimReleaseDTO &release) {
          return core::ValidateCCLCCellAdmissionSettlementReadOnly(
              lifecycle_path, cell_id, claim, release);
        };
    cell = std::make_unique<RetainedNativeCellRuntime>(
        RetainedNativeCellRuntimeInput{.clock = clock,
                                       .scheduler = scheduler,
                                       .operation_journal_path =
                                           temporary.path / "operations.sqlite",
                                       .participant_registrations = {},
                                       .kernel_config = std::move(config),
                                       .stop_sources = {}});
    const core::CellAdmissionReadinessSnapshotDTO idle{
        true,
        core::ProgramCycleStatusDTO{core::RequestOwnerEpoch("helper.runner"),
                                    1U,
                                    core::ProgramCycleStateDTO::kIdleAfterReset,
                                    {},
                                    {},
                                    {}},
        true,
        1U,
        1U,
        true,
        true,
        0U,
        0U,
        0U};
    take(cell->addressed_kernel().UpdateCellAdmissionReadiness(idle));
    publication_store =
        std::make_unique<component_manager::SqliteActiveBundlePublicationStore>(
            temporary.path / "active.sqlite", cell_id);
    const auto scene_digest = core::ContentDigest(
        content_identity::sha256_digest("helper fixture empty scene"));
    projection = {bundle->retained_bundle_id(),
                  bundle->value().cell_graph.graph_revision,
                  {core::SceneId(content_identity::sha256_digest(
                       "helper fixture scene lineage")),
                   1U, scene_digest}};
    materialization = core::ContentDigest(
        content_identity::sha256_digest(bundle->canonical_bytes()));
    publication = publication_store->PublishActiveBundle(
        {core::BootstrapPublicationTransactionDTO{
             core::BootstrapId("helper.bootstrap"),
             core::BootstrapGeneration(1U)},
         projection,
         bundle->retained_bundle_id(),
         projection.graph_revision,
         {},
         materialization});
    drivers = MakeDrivers();
    current = View(publication->publication_generation, drivers);
  }
  void TearDown() override {
    // The actual coordinator drains before its borrowed source/driver/cell.
    coordinator.reset();
    context.reset();
    snapshot_source.reset();
    port_registry.reset();
    // The stable Kernel borrows registered channel engines. Keep their actual
    // owners alive through Kernel unsubscribe while preserving source shutdown.
    auto installed_channels = current ? current->io_output_channels
                                     : std::vector<std::shared_ptr<HostIoOutputChannelEntry>>{};
    current.reset();
    cell.reset();
    installed_channels.clear();
    drivers.reset();
    scheduler.reset();
    clock.reset();
  }

  std::shared_ptr<const IoProviderDriverGroup> MakeDrivers() {
    IoProviderDriverGroup result;
    std::vector<core::BuildOwnedLifecyclePlanCatalogEntryDTO> entries;
    deployment::FilesystemVerifiedRetainedProviderManifestSource manifests(
        package);
    const auto catalog = take(
        deployment::ResolveRetainedIoSignalCatalog(bundle->value().cell_graph));
    for (const auto &domain : bundle->value().cell_graph.control_domains) {
      const auto *provider = domain.provider();
      checked(provider != nullptr,
              "fixture unexpectedly contains a controller");
      const auto manifest = manifests.get_verified(
          {bundle->retained_bundle_id(), provider->provider_manifest_id,
           provider->provider_manifest_revision});
      checked(manifest.has_value(), "actual qualified manifest missing");
      auto plans =
          take(core::MakeIoProviderLifecycleCatalog(manifest->value()));
      checked(plans.catalog_digest == provider->lifecycle_plan_catalog_digest,
              "actual lifecycle catalog anchor mismatch");
      entries.push_back({manifest->value(), plans});
      std::vector<core::ResourceId> resources;
      for (const auto &row : catalog)
        if (row.descriptor.signal_id.controllable_component_id ==
                provider->controllable_component_id &&
            row.runtime_unit_id == provider->provider_runtime_unit_id &&
            row.descriptor.output_resource)
          resources.push_back(*row.descriptor.output_resource);
      std::sort(resources.begin(), resources.end());
      resources.erase(std::unique(resources.begin(), resources.end()),
                      resources.end());
      const core::RuntimeEpoch epoch("helper.actual.host.epoch");
      core::ResolvedLifecycleControlDomainScopeDTO scope{
          domain.control_domain_id, domain.control_domain_revision,
          core::ResolvedProviderDomainScopeDTO{
              *provider, epoch, {}, resources}};
      core::ResolvedLifecycleProviderOccurrenceDTO occurrence{
          domain.control_domain_id,
          domain.control_domain_revision,
          provider->provider_instance_id,
          provider->provider_manifest_id,
          provider->provider_manifest_revision,
          provider->provider_runtime_unit_id,
          epoch,
          plans.catalog_digest};
      result.push_back(take(IoProviderDriverRecord::Create(
          std::move(scope), std::move(occurrence), manifest->value(), clock,
          1'000'000'000LL)));
    }
    if (!catalog_source)
      catalog_source =
          std::make_unique<core::BuildOwnedLifecyclePlanCatalogSource>(
              take(core::BuildOwnedLifecyclePlanCatalogSource::Make(
                  std::move(entries))));
    return std::make_shared<const IoProviderDriverGroup>(std::move(result));
  }

  std::unique_ptr<Composition>
  View(core::ActiveBundlePublicationGeneration generation,
       std::shared_ptr<const IoProviderDriverGroup> group) {
    const auto &component =
        bundle->value().cell_graph.controllable_components.front();
    auto result = std::make_unique<Composition>(
        component.controllable_component_id, bundle, nullptr, nullptr, nullptr,
        nullptr, std::nullopt, std::nullopt, std::nullopt,
        CanonicalValue(CanonicalValue::Object{}),
        CanonicalValue(CanonicalValue::Object{}), nullptr, nullptr, projection,
        generation, std::nullopt, std::nullopt);
    result->cell_runtime = cell.get();
    result->io_provider_drivers = std::move(group);
    result->component_runtime_pins =
        std::make_shared<const ComponentRuntimePins>(ComponentRuntimePins{
            nullptr, nullptr, result->io_provider_drivers});
    result->direct_control_io_program_bindings = direct_bindings;
    result->target_resolver =
        std::make_shared<ProductBackedRuntimeParticipantResolver>(
            package, bundle, *model_map,
            std::vector<core::RuntimeParticipantBinding>{},
            std::vector<core::ProgramBindingId>{});
    return result;
  }

  core::Result<void>
  Prepare(Composition &view,
          std::span<const CompositionIoResourcePredecessor> predecessors = {},
          rclcpp::Node::SharedPtr node = {}) {
    return PrepareCompositionIoProviderResources(view, *cell, std::move(node),
                                                 predecessors);
  }
  std::vector<CompositionIoResourcePredecessor>
  Capture(const Composition &view) {
    const Composition *roots[]{&view};
    return CaptureCompositionIoResourcePredecessors(roots);
  }
  core::ActiveBundlePublicationGeneration NewPublication() {
    publication = publication_store->PublishActiveBundle(
        {core::NamedApplyPublicationTransactionDTO{
             core::ApplyId(
                 "helper.apply." +
                 std::to_string(publication->publication_generation.value())),
             core::ApplyGeneration(1U)},
         projection, bundle->retained_bundle_id(), projection.graph_revision,
         publication->publication_generation, materialization});
    return publication->publication_generation;
  }

  void StartOwner() {
    if (coordinator)
      return;
    journal = std::make_unique<core::SqliteCCLCJournal>(
        temporary.path / std::string(core::kCCLCJournalFilename),
        bundle->value().cell_graph.cell_id, 8U);
    std::vector<core::BrandLifecyclePortRegistrationDTO> ports;
    std::vector<core::ResolvedLifecycleProviderOccurrenceDTO> occurrences;
    for (const auto &driver : *drivers) {
      ports.push_back(driver->brand_registration());
      occurrences.push_back(driver->occurrence());
    }
    port_registry = std::make_unique<core::FixedCCLCPortRegistry>(
        std::move(ports),
        std::vector<core::ControllerLifecyclePortRegistrationDTO>{});
    snapshot_source = std::make_unique<
        deployment::VerifiedComponentLifecycleCurrentSnapshotSource>(
        *bundle, *publication_store, *publication, *journal, *catalog_source,
        std::move(occurrences));
    std::vector<core::ControllableComponentId> components;
    for (const auto &component :
         bundle->value().cell_graph.controllable_components)
      components.push_back(component.controllable_component_id);
    // Exercise the actual lifecycle journal/driver protocol without a readiness
    // publication adapter. Kernel command eligibility stays closed below; this
    // fixture does not qualify the host's readiness/publication join.
    context = std::make_shared<const core::ComponentLifecycleProductContext>(
        core::ComponentLifecycleProductContext{
            *snapshot_source,
            *catalog_source,
            *port_registry,
            {},
            nullptr,
            core::ComponentLifecycleProductIdentity{*publication,
                                                    std::move(components)}});
    coordinator = std::make_unique<core::ComponentLifecycleCoordinator>(
        core::ComponentLifecycleCoordinatorConfig{
            core::RequestOwnerEpoch("helper.cclc"), 32U, 1U, 1'000'000},
        *journal, reservations, context, cell->addressed_kernel(), *clock,
        *scheduler);
    const auto started = coordinator->Start();
    checked(started.ok(), started.ok() ? "" : started.error().message);
  }
  core::ComponentLifecycleCurrentSnapshotDTO Snapshot() {
    checked(coordinator != nullptr, "actual CCLC owner missing");
    auto pin = take(coordinator->AcquireProductPin());
    return take(
        pin.context().current_snapshot_source.ReadCurrentComponentSnapshot(
            current->controllable_component_id));
  }
  core::ComponentLifecycleCurrentSnapshotDTO
  Step(core::ComponentLifecycleIntentKindDTO kind) {
    StartOwner();
    const auto before = Snapshot();
    std::vector<core::ExpectedControlDomainRevisionDTO> domains;
    for (const auto &domain : before.domains)
      domains.push_back(
          {domain.control_domain_id, domain.control_domain_revision,
           core::ExpectedProviderDomainScopeDTO{
               domain.provider()->provider,
               {core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
                {},
                {}}}});
    core::ComponentLifecycleIntentDTO intent{
        {core::RequestId("helper.lifecycle." +
                         std::string(core::to_string(kind))),
         coordinator->request_owner_epoch(),
         "fixture",
         {}},
        before.controllable_component_id,
        kind,
        bundle->value().cell_graph.graph_revision,
        before.component_lifecycle_generation,
        std::move(domains),
        {}};
    intent.expected_admission_generation =
        cell->addressed_kernel().GetAuthoringAdmission().admission_generation;
    const auto submitted = take(coordinator->SubmitLifecycle(intent));
    checked(submitted.acceptance.has_value(), "actual CCLC acceptance missing");
    checked(coordinator->DrainFor(std::chrono::seconds(2)),
            "actual CCLC did not drain");
    const auto status = take(coordinator->LoadLifecycleStatus(
        submitted.acceptance->lifecycle_transaction_id));
    checked(status && status->terminal_result ==
                          core::ComponentLifecycleTerminalResultDTO::kSucceeded,
            status && status->error ? status->error->message
                                    : "actual lifecycle failed");
    const auto after = Snapshot();
    checked(after.component_lifecycle_generation ==
                status->acceptance.component_lifecycle_generation,
            "production current source disagrees with actual accepted journal "
            "generation");
    return after;
  }
  IoProviderConstructionGenerations ActualGenerations() {
    const auto snapshot = Snapshot();
    return {{snapshot.controllable_component_id,
             snapshot.component_lifecycle_generation}};
  }
  std::shared_ptr<MockBooleanOutputProvider> Mock(const Composition &view) {
    checked(view.io_output_channels.size() == 1U,
            "expected one actual canonical channel");
    return std::get<std::shared_ptr<MockBooleanOutputProvider>>(
        view.io_output_channels.front()->provider);
  }
  struct Execution final {
    core::IoOutputParticipantEngine *engine;
    std::uint64_t preparation;
    core::IoOutputObservationDTO observation;
  };
  Execution Write(Composition &view, std::string role, bool value) {
    const auto row = std::find_if(
        view.io_output_registrations.begin(),
        view.io_output_registrations.end(), [&](const auto &registration) {
          return registration.binding.value()
                     .io_output_binding->resource_role_id.value() == role;
        });
    checked(row != view.io_output_registrations.end(),
            "actual output registration missing");
    auto *engine = dynamic_cast<core::IoOutputParticipantEngine *>(row->engine);
    checked(engine != nullptr, "actual subordinate output engine missing");
    const auto schema = core::builtin_io_signal_write_command_schema();
    const core::CapabilityCommandIntentDTO command{
        schema.command_schema_id,
        schema.command_schema_revision,
        core::IoSignalWriteCommandValueDTO{
            core::ResourceRoleId(std::move(role)), value},
        {},
        {}};
    auto prepared = take(core::MakePreparedIoOutputParticipant(
        core::OperationId("helper.output." + std::to_string(++write_sequence)),
        core::OperationRevision(1U),
        core::OperationParticipantId("helper.participant"),
        core::ProgramRunId("helper.private.session"), command, row->binding));
    auto sealed = std::make_shared<const core::PreparedOperationParticipantDTO>(
        std::move(prepared));
    const auto &output =
        std::get<core::PreparedIoOutputCommandDTO>(sealed->payload);
    const auto id = take(engine->PrepareCapabilityCommand(
        {sealed, row->binding,
         core::ResolvedPreparedIoOutputCommandAuthorityDTO{
             output.signal_id, output.output_resource,
             output.command_value.value, sealed->provider()->lifecycle_provider,
             output.completion_policy}}));
    const auto sent = engine->DispatchPreparedCapabilityCommand(
        id, sealed->prepared_contract_digest,
        core::ControllerGoalId("helper.goal." + std::to_string(write_sequence)),
        core::ControllerGoalGeneration(1U));
    checked(sent.ok(), sent.ok() ? "" : sent.error().message);
    checked(sent.value().accepted, sent.value().error
                                       ? sent.value().error->message
                                       : "actual output dispatch refused");
    const auto observed = take(engine->ObservePreparedCapabilityCommand(id));
    checked(observed.output_observation.has_value(),
            "actual output evidence missing");
    return {engine, id, *observed.output_observation};
  }

  TemporaryRoot temporary;
  std::filesystem::path package;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::optional<deployment::VerifiedRetainedModelMap> model_map;
  std::vector<std::shared_ptr<const deployment::ProgramBinding>>
      direct_bindings;
  std::shared_ptr<WallClock> clock;
  std::shared_ptr<SimpleScheduler> scheduler;
  std::unique_ptr<RetainedNativeCellRuntime> cell;
  std::unique_ptr<component_manager::SqliteActiveBundlePublicationStore>
      publication_store;
  core::ProjectionRevisionDTO projection;
  core::ContentDigest materialization{std::string{}};
  std::optional<core::ActiveBundlePublicationReceiptDTO> publication;
  std::shared_ptr<const IoProviderDriverGroup> drivers;
  std::unique_ptr<Composition> current;
  std::unique_ptr<core::BuildOwnedLifecyclePlanCatalogSource> catalog_source;
  std::unique_ptr<core::SqliteCCLCJournal> journal;
  core::CCLCDomainReservationTable reservations;
  std::unique_ptr<core::FixedCCLCPortRegistry> port_registry;
  std::unique_ptr<deployment::VerifiedComponentLifecycleCurrentSnapshotSource>
      snapshot_source;
  std::shared_ptr<const core::ComponentLifecycleProductContext> context;
  std::unique_ptr<core::ComponentLifecycleCoordinator> coordinator;
  std::uint64_t write_sequence{};
};

TEST_F(HostIoProviderInstallationTest, CommonProductViewBorrowsTheActualCompositionStorage) {
  Build();
  ExecutionCompositionAccess access;
  {
    std::unique_lock lock(access.mutex);
    access.replace_current(current.get());
    EXPECT_EQ(access.product(), static_cast<ExecutionProduct *>(current.get()));
    EXPECT_EQ(&access.product()->bundle, &current->bundle);
    EXPECT_EQ(&access.product()->program_binding, &current->program_binding);
    EXPECT_EQ(&access.product()->io_signal_service, &current->io_signal_service);
    EXPECT_EQ(&access.product()->publication_generation, &current->publication_generation);
    EXPECT_FALSE(access.logical_product);
    // A conflicting parallel selection cannot silently fall back to one copy.
    access.logical_product = std::make_shared<ExecutionProduct>(bundle,
        nullptr, nullptr, projection, publication->publication_generation);
    EXPECT_THROW((void)access.product(), std::exception);
    access.logical_product.reset();
    access.replace_current(nullptr);
    EXPECT_EQ(access.product(), nullptr);
  }
}

TEST_F(
    HostIoProviderInstallationTest,
    ProviderOnlyPhysicalSelectionRequiresItsActualDomainAndNoPhysicalBindings) {
  Build(true, false);
  const auto &graph = bundle->value().cell_graph;
  const auto component_id = current->controllable_component_id;
  ASSERT_TRUE(graph.control_bindings.empty());
  ASSERT_FALSE(graph.control_domains.empty());
  EXPECT_TRUE(IsProviderOnlyNativeComponent(graph, component_id));
  EXPECT_FALSE(IsProviderOnlyNativeComponent(
      graph, core::ControllableComponentId("absent.component")));

  // These raw negative projections are NOT qualified retained products.
  auto no_domain = graph;
  no_domain.control_domains.clear();
  EXPECT_FALSE(IsProviderOnlyNativeComponent(no_domain, component_id));
  auto foreign_domain = graph;
  auto *provider = std::get_if<core::ProviderControlDomainDTO>(
      &foreign_domain.control_domains.front().payload);
  ASSERT_NE(provider, nullptr);
  provider->controllable_component_id = core::ControllableComponentId("other");
  EXPECT_FALSE(IsProviderOnlyNativeComponent(foreign_domain, component_id));
  auto no_unit = graph;
  no_unit.controllable_components.front().runtime_unit_ids.clear();
  EXPECT_FALSE(IsProviderOnlyNativeComponent(no_unit, component_id));

  const auto physical_root = temporary.path / "physical-package";
  ASSERT_TRUE(std::filesystem::create_directory(physical_root));
  const auto physical =
      deployment::test::materialize_lite6_retained_consumer_fixture(
          physical_root);
  ASSERT_NE(physical.bundle, nullptr);
  const auto &physical_graph = physical.bundle->value().cell_graph;
  ASSERT_FALSE(physical_graph.control_bindings.empty());
  const auto physical_component =
      physical_graph.control_bindings.front().controllable_component_id;
  EXPECT_FALSE(
      IsProviderOnlyNativeComponent(physical_graph, physical_component));
  auto unsupported = physical_graph;
  unsupported.control_bindings.clear();
  EXPECT_FALSE(IsProviderOnlyNativeComponent(unsupported, physical_component));
  auto mixed = graph;
  auto actual_physical_binding = physical_graph.control_bindings.front();
  actual_physical_binding.controllable_component_id = component_id;
  mixed.control_bindings.push_back(std::move(actual_physical_binding));
  EXPECT_FALSE(IsProviderOnlyNativeComponent(mixed, component_id));

  // Existing actual installation still makes no motion/gripper registration or
  // Active/readiness fact for this genuine input-only component.
  ASSERT_TRUE(Prepare(*current).ok());
  EXPECT_TRUE(current->component_registrations().empty());
  EXPECT_FALSE(
      take(cell->addressed_kernel().component_command_readiness(component_id))
          .command_eligible);
}

TEST_F(HostIoProviderInstallationTest,
       GenuineInputOnlyCompositionHasNoExecutionOrFakeReadiness) {
  Build(true, false);
  ASSERT_TRUE(Prepare(*current).ok());
  ASSERT_NE(current->io_signal_runtime, nullptr);
  EXPECT_TRUE(current->io_output_channels.empty());
  EXPECT_TRUE(BindCompositionIoOutputParticipants(*current, {}).ok());
  EXPECT_TRUE(current->component_registrations().empty());
  const auto installed = cell->reconfigure_runtime_product({}, {}, {});
  ASSERT_TRUE(installed.ok()) << installed.error().message;
  EXPECT_TRUE(cell->participant_registrations().empty());
  EXPECT_EQ(current->component_runtime_identity(),
            current->component_runtime_pins.get());
  ASSERT_EQ(current->host_runtime_unit_epochs().size(), 1U);
  EXPECT_EQ(current->host_runtime_unit_epochs().front(),
            drivers->front()->runtime_unit_epoch());
  EXPECT_TRUE(drivers->front()->resources_installed());
  EXPECT_EQ(
      take(drivers->front()->ReadLifecycleSnapshot(
               drivers->front()->domain().control_domain_id, clock->now()))
          .component_state,
      core::ComponentLifecycleStateDTO::kDeclared);
  EXPECT_FALSE(take(cell->addressed_kernel().component_command_readiness(
                        current->controllable_component_id))
                   .command_eligible);
}

TEST_F(HostIoProviderInstallationTest,
       ActualSharedAliasesUseOneEngineAndJournalConstructionGeneration) {
  Build(false, true, true, true);
  ASSERT_TRUE(Prepare(*current).ok());
  ASSERT_EQ(current->io_output_channels.size(), 1U);
  EXPECT_FALSE(Mock(*current)->model_value());
  EXPECT_FALSE(BindCompositionIoOutputParticipants(*current, {}).ok());
  const auto prepared = Step(core::ComponentLifecycleIntentKindDTO::kPrepare);
  EXPECT_EQ(prepared.component_lifecycle_state,
            core::ComponentLifecycleStateDTO::kPrepared);
  ASSERT_TRUE(
      BindCompositionIoOutputParticipants(*current, ActualGenerations()).ok());
  const auto paused = Write(*current, "lamp", true);
  EXPECT_EQ(paused.observation.evidence, core::IoOutputEvidence::kNotSubmitted);
  EXPECT_FALSE(Mock(*current)->model_value());
  ASSERT_TRUE(paused.engine->get_status().fact);
  EXPECT_EQ(paused.engine->get_status().fact->effect,
            core::ParticipantEffectKnowledge::kNoEffect);
  ASSERT_TRUE(
      paused.engine
          ->reset_capability_command(current->io_output_registrations.front()
                                         .binding.value()
                                         .command_target_id.value(),
                                     paused.preparation,
                                     paused.observation.identity.command
                                         .command_generation.value())
          .ok());
  const auto enabled = Step(core::ComponentLifecycleIntentKindDTO::kEnable);
  const auto generations = ActualGenerations();
  EXPECT_FALSE(
      BindCompositionIoOutputParticipants(
          *current, {{core::ControllableComponentId("foreign.component"),
                      enabled.component_lifecycle_generation}})
          .ok());
  ASSERT_TRUE(BindCompositionIoOutputParticipants(*current, generations).ok());
  const auto rows = current->component_registrations();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].engine, rows[1].engine);
  EXPECT_EQ(rows[0].engine, current->io_output_channels.front()->engine.get());
  for (const auto &row : rows) {
    ASSERT_TRUE(row.binding.value().io_output_binding);
    EXPECT_EQ(
        row.binding.value().io_output_binding->component_lifecycle_generation,
        enabled.component_lifecycle_generation);
    EXPECT_FALSE(row.binding.value().control_path);
    EXPECT_TRUE(row.binding.value().resources.empty());
    EXPECT_FALSE(row.stop_readiness);
  }
  const auto write = Write(*current, "lamp_other", true);
  EXPECT_EQ(write.observation.evidence,
            core::IoOutputEvidence::kSourceAccepted);
  EXPECT_EQ(Mock(*current)->model_value(), std::optional<bool>(true));
  EXPECT_TRUE(write.engine->acknowledge_completed(write.preparation).accepted);
  // No helper or lifecycle-only test adapter published Kernel eligibility.
  EXPECT_FALSE(take(cell->addressed_kernel().component_command_readiness(
                        current->controllable_component_id))
                   .command_eligible);
}

TEST_F(HostIoProviderInstallationTest,
       RetainedActualChannelResetsOnceAndKeepsItsOriginalCommandEvidence) {
  Build();
  ASSERT_TRUE(Prepare(*current).ok());
  Step(core::ComponentLifecycleIntentKindDTO::kPrepare);
  Step(core::ComponentLifecycleIntentKindDTO::kEnable);
  const auto generations = ActualGenerations();
  ASSERT_TRUE(BindCompositionIoOutputParticipants(*current, generations).ok());
  const auto old = Write(*current, "lamp", true);
  ASSERT_EQ(old.observation.evidence, core::IoOutputEvidence::kSourceAccepted);
  const auto predecessor = Capture(*current); // before any publication mutation
  const auto entry = current->io_output_channels.front();
  auto candidate = View(NewPublication(), drivers);
  ASSERT_TRUE(Prepare(*candidate, predecessor).ok());
  EXPECT_EQ(candidate->io_output_channels.front(), entry);
  EXPECT_FALSE(Mock(*candidate)->model_value());
  const auto retained =
      take(old.engine->ObservePreparedCapabilityCommand(old.preparation));
  ASSERT_TRUE(retained.output_observation);
  EXPECT_EQ(*retained.output_observation, old.observation);
  ASSERT_TRUE(old.engine->acknowledge_completed(old.preparation).accepted);
  ASSERT_TRUE(
      BindCompositionIoOutputParticipants(*candidate, generations).ok());
  const auto next = Write(*candidate, "lamp", false);
  ASSERT_EQ(next.observation.evidence, core::IoOutputEvidence::kSourceAccepted);
  EXPECT_EQ(Mock(*candidate)->model_value(), std::optional<bool>(false));
  const auto replay = Capture(*candidate);
  ASSERT_TRUE(Prepare(*candidate, replay).ok());
  EXPECT_EQ(candidate->io_output_channels.front(), entry);
  EXPECT_EQ(Mock(*candidate)->model_value(), std::optional<bool>(false));
  EXPECT_EQ(
      take(next.engine->ObservePreparedCapabilityCommand(next.preparation))
          .output_observation,
      std::optional(next.observation));
  EXPECT_TRUE(next.engine->acknowledge_completed(next.preparation).accepted);
}

TEST_F(HostIoProviderInstallationTest,
       RetainedDriverRefusesMissingAndForeignPredecessorProof) {
  Build();
  ASSERT_TRUE(Prepare(*current).ok());
  const auto proof = Capture(*current);
  auto candidate = View(NewPublication(), drivers);
  EXPECT_FALSE(Prepare(*candidate).ok());
  EXPECT_TRUE(candidate->io_output_channels.empty());
  const auto foreign_root = temporary.path / "foreign";
  ASSERT_TRUE(std::filesystem::create_directory(foreign_root));
  const auto foreign =
      deployment::test::materialize_boolean_io_retained_fixture(
          foreign_root, false, true, true)
          .bundle;
  auto wrong = proof;
  wrong.front().bundle = foreign;
  EXPECT_FALSE(Prepare(*candidate, wrong).ok());
  EXPECT_TRUE(candidate->io_output_channels.empty());
  EXPECT_FALSE(Mock(*current)->model_value());
}

TEST_F(HostIoProviderInstallationTest,
       FailedCandidateRetainsRuntimeAndEntryAfterActualAliasRebind) {
  Build(true, true);
  ASSERT_TRUE(Prepare(*current).ok());
  auto proof = Capture(*current);
  auto *const original_entry = current->io_output_channels.front().get();
  std::weak_ptr<HostIoOutputChannelEntry> lifetime =
      current->io_output_channels.front();
  const auto replacement_drivers = MakeDrivers();
  replacement_drivers->front()
      ->Shutdown(); // genuine retired owner cannot install
  auto candidate = View(NewPublication(), replacement_drivers);
  const auto failed = Prepare(*candidate, proof);
  ASSERT_FALSE(failed.ok());
  ASSERT_NE(candidate->io_signal_runtime, nullptr);
  std::weak_ptr<HostIoSignalRuntime> runtime_lifetime =
      candidate->io_signal_runtime;
  EXPECT_NE(candidate->io_signal_runtime, current->io_signal_runtime);
  ASSERT_TRUE(candidate->io_resource_preparation);
  EXPECT_EQ(candidate->io_resource_preparation->publication_generation,
            candidate->publication_generation);
  EXPECT_FALSE(candidate->io_resource_preparation->complete);
  ASSERT_EQ(candidate->io_output_channels.size(), 1U);
  EXPECT_EQ(candidate->io_output_channels.front().get(), original_entry);
  const auto aliases = Mock(*candidate)->ReadAliasState();
  EXPECT_EQ(aliases.active_publication_generation,
            candidate->publication_generation);
  ASSERT_EQ(aliases.aliases.size(), 1U);
  EXPECT_TRUE(
      SameIoOutputDriverOwner(aliases.aliases.front().actual_driver_owner,
                              replacement_drivers->front()));
  current.reset();
  proof.clear();
  EXPECT_FALSE(
      lifetime.expired()); // candidate is now the sole surviving Entry owner
  EXPECT_NE(candidate->io_output_channels.front()->engine, nullptr);
  EXPECT_FALSE(runtime_lifetime.expired());
  EXPECT_FALSE(take(cell->addressed_kernel().component_command_readiness(
                        candidate->controllable_component_id))
                   .command_eligible);
  candidate.reset();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_TRUE(runtime_lifetime.expired());
}
TEST_F(HostIoProviderInstallationTest,
       GenuineOutputRemovalPermanentlyFencesOnlyItsOldOccurrence) {
  Build();
  ASSERT_TRUE(Prepare(*current).ok());
  Step(core::ComponentLifecycleIntentKindDTO::kPrepare);
  Step(core::ComponentLifecycleIntentKindDTO::kEnable);
  ASSERT_TRUE(
      BindCompositionIoOutputParticipants(*current, ActualGenerations()).ok());
  const auto old = Write(*current, "lamp", true);
  ASSERT_EQ(old.observation.evidence, core::IoOutputEvidence::kSourceAccepted);
  const auto old_provider = Mock(*current);
  const auto old_occurrence = drivers->front()->occurrence();
  const auto old_driver = drivers->front();
  const auto predecessor = Capture(*current);

  // Materialize a genuine replacement input-only product; no qualified DTO
  // is edited to manufacture absence or a new provider/domain revision.
  package = temporary.path / "input-only";
  ASSERT_TRUE(std::filesystem::create_directory(package));
  bundle = deployment::test::materialize_boolean_io_retained_fixture(
               package, true, false)
               .bundle;
  ASSERT_NE(bundle, nullptr);
  ASSERT_TRUE(bundle->value().cell_graph.model_instances.empty());
  ASSERT_EQ(bundle->value().compiled_products.symbol_role_maps.size(), 1U);
  deployment::FilesystemVerifiedRetainedModelMapSource maps(package);
  model_map = maps.get_verified(
      {bundle->retained_bundle_id(),
       bundle->value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(model_map);
  deployment::FilesystemCatalogArtifactStore catalogs(package);
  direct_bindings =
      deployment::compile_direct_control_program_binding_inventory(
          *bundle, *model_map, catalogs)
          .io;
  projection.retained_bundle_id = bundle->retained_bundle_id();
  projection.graph_revision = bundle->value().cell_graph.graph_revision;
  materialization = core::ContentDigest(
      content_identity::sha256_digest(bundle->canonical_bytes()));
  const auto replacement = MakeDrivers();
  auto candidate = View(NewPublication(), replacement);
  ASSERT_TRUE(Prepare(*candidate, predecessor).ok());
  EXPECT_TRUE(candidate->io_output_channels.empty());
  ASSERT_NE(candidate->io_signal_runtime, nullptr);
  EXPECT_FALSE(old_provider
                   ->Resume(old_occurrence,
                            std::numeric_limits<std::uint64_t>::max(),
                            old_driver)
                   .ok());
  const auto retained =
      take(old.engine->ObservePreparedCapabilityCommand(old.preparation));
  EXPECT_EQ(retained.output_observation, std::optional(old.observation));
  EXPECT_TRUE(old.engine->acknowledge_completed(old.preparation).accepted);
  EXPECT_TRUE(BindCompositionIoOutputParticipants(*candidate, {}).ok());
  EXPECT_TRUE(candidate->component_registrations().empty());
}

TEST_F(HostIoProviderInstallationTest,
       QualifiedGpioRemovalKeepsTopicAndRefusedPreparationPreservesOldTables) {
  Build(false, true, true, false, "ros2_control_gpio");
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  auto ros_context = std::make_shared<rclcpp::Context>();
  ros_context->init(0, nullptr);
  rclcpp::NodeOptions options;
  options.context(ros_context);
  options.use_global_arguments(false);
  auto node = std::make_shared<rclcpp::Node>("gpio_installation", options);
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = ros_context;
  rclcpp::executors::SingleThreadedExecutor executor(executor_options);
  executor.add_node(node);
  std::vector<Message> messages;
  auto subscriber = node->create_subscription<Message>(
      "/io_device/commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message& message) { messages.push_back(message); });
  const auto spin_until = [&](const auto& ready) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < limit)
      executor.spin_once(std::chrono::milliseconds(5));
    return ready();
  };
  ASSERT_TRUE(Prepare(*current, {}, node).ok());
  ASSERT_EQ(current->io_output_channels.size(), 2U);
  ASSERT_TRUE(spin_until([&] { return subscriber->get_publisher_count() == 1U; }));
  Step(core::ComponentLifecycleIntentKindDTO::kPrepare);
  Step(core::ComponentLifecycleIntentKindDTO::kEnable);
  ASSERT_TRUE(BindCompositionIoOutputParticipants(*current, ActualGenerations()).ok());
  const auto first = Write(*current, "lamp", true);
  ASSERT_EQ(first.observation.evidence, core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(spin_until([&] { return messages.size() == 1U; }));
  const auto removed_write = Write(*current, "lamp_other", false);
  ASSERT_EQ(removed_write.observation.evidence, core::IoOutputEvidence::kSubmitted);
  ASSERT_TRUE(spin_until([&] { return messages.size() == 2U; }));
  ASSERT_EQ(messages.back().interface_groups.size(), 1U);
  ASSERT_EQ(messages.back().interface_values.size(), 1U);
  EXPECT_EQ(messages.back().interface_groups.front(), "digital_outputs");
  const auto& payload = messages.back().interface_values.front();
  EXPECT_EQ(payload.interface_names, (std::vector<std::string>{"lamp", "lamp_other"}));
  EXPECT_EQ(payload.values, (std::vector<double>{1.0, 0.0}));

  const auto entry_for = [&](const char* name) {
    const auto rows =
        take(deployment::ResolveRetainedIoSignalCatalog(current->bundle->value().cell_graph));
    const auto row = std::find_if(rows.begin(), rows.end(), [&](const auto& value) {
      return value.descriptor.signal_id.name == name;
    });
    checked(row != rows.end() && row->descriptor.output_resource, "qualified GPIO channel missing");
    const auto entry = std::find_if(
        current->io_output_channels.begin(), current->io_output_channels.end(),
        [&](const auto& value) { return value->resource_id == *row->descriptor.output_resource; });
    checked(entry != current->io_output_channels.end(), "actual GPIO Entry missing");
    return *entry;
  };
  const auto retained_entry = entry_for("lamp");
  const auto removed_entry = entry_for("lamp_other");
  const auto retained_provider =
      std::get<std::shared_ptr<RosBooleanOutputProvider>>(retained_entry->provider);
  const auto removed_provider =
      std::get<std::shared_ptr<RosBooleanOutputProvider>>(removed_entry->provider);
  ASSERT_EQ(retained_provider->gpio_topic(), removed_provider->gpio_topic());
  const auto retained_before = retained_provider->ReadAliasState();
  const auto removed_before = removed_provider->ReadAliasState();
  const auto predecessor = Capture(*current);
  const auto original_driver = drivers->front();
  const auto same_table = [](const IoOutputAliasState& actual, const IoOutputAliasState& expected) {
    EXPECT_EQ(actual.active_publication_generation, expected.active_publication_generation);
    ASSERT_EQ(actual.aliases.size(), expected.aliases.size());
    for (std::size_t index = 0; index < actual.aliases.size(); ++index) {
      EXPECT_EQ(actual.aliases[index].signal_id, expected.aliases[index].signal_id);
      EXPECT_EQ(actual.aliases[index].provider, expected.aliases[index].provider);
      EXPECT_TRUE(SameIoOutputDriverOwner(actual.aliases[index].actual_driver_owner,
                                          expected.aliases[index].actual_driver_owner));
    }
  };

  // Remove only the second channel through a genuinely materialized product.
  // This is installer integration, not Manager Apply/readiness authority.
  package = temporary.path / "single-gpio";
  ASSERT_TRUE(std::filesystem::create_directory(package));
  bundle = deployment::test::materialize_boolean_io_retained_fixture(
               package, false, true, false, false, false, nullptr, "ros2_control_gpio")
               .bundle;
  deployment::FilesystemVerifiedRetainedModelMapSource maps(package);
  model_map =
      maps.get_verified({bundle->retained_bundle_id(),
                         bundle->value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(model_map);
  deployment::FilesystemCatalogArtifactStore catalogs(package);
  direct_bindings =
      deployment::compile_direct_control_program_binding_inventory(*bundle, *model_map, catalogs)
          .io;
  projection.retained_bundle_id = bundle->retained_bundle_id();
  projection.graph_revision = bundle->value().cell_graph.graph_revision;
  materialization = core::ContentDigest(content_identity::sha256_digest(bundle->canonical_bytes()));
  const auto replacement_drivers = MakeDrivers();
  auto candidate = View(NewPublication(), replacement_drivers);

  auto wrong_predecessor = predecessor;
  wrong_predecessor.front().bundle = bundle;  // actual product, wrong Entry provenance
  EXPECT_FALSE(Prepare(*candidate, wrong_predecessor, node).ok());
  EXPECT_TRUE(candidate->io_output_channels.empty());
  same_table(retained_provider->ReadAliasState(), retained_before);
  same_table(removed_provider->ReadAliasState(), removed_before);
  EXPECT_EQ(removed_provider->ObserveOutputResult(removed_write.observation.identity).value(),
            removed_write.observation);
  EXPECT_EQ(messages.size(), 2U);

  ASSERT_TRUE(Prepare(*candidate, predecessor, node).ok());
  ASSERT_EQ(candidate->io_output_channels.size(), 1U);
  EXPECT_EQ(candidate->io_output_channels.front(), retained_entry);
  EXPECT_EQ(retained_provider->gpio_topic(), removed_provider->gpio_topic());
  EXPECT_EQ(retained_provider->ReadAliasState().active_publication_generation,
            candidate->publication_generation);
  same_table(removed_provider->ReadAliasState(), removed_before);
  EXPECT_EQ(removed_provider->ObserveOutputResult(removed_write.observation.identity).value(),
            removed_write.observation);
  EXPECT_FALSE(removed_provider
                   ->Resume(original_driver->occurrence(),
                            std::numeric_limits<std::uint64_t>::max(), original_driver)
                   .ok());
  EXPECT_EQ(messages.size(), 2U);  // preparation/retirement never resends
  // Candidate stays paused. No old CCLC generation is passed off as its
  // readiness, and no post-Apply dispatch/physical-effect claim is made.
  EXPECT_EQ(take(replacement_drivers->front()->ReadLifecycleSnapshot(
                     replacement_drivers->front()->domain().control_domain_id, clock->now()))
                .component_state,
            core::ComponentLifecycleStateDTO::kDeclared);
  candidate.reset();
  for (const auto& driver : *replacement_drivers) driver->Shutdown();
  for (const auto& driver : *drivers) driver->Shutdown();
  executor.remove_node(node);
  ros_context->shutdown("GPIO installer integration complete");
}

} // namespace

TEST_F(
    HostIoProviderInstallationTest,
    GenuineLogicalOutputRegistrationsReachStableCellWithoutPhysicalResources) {
  Build(true, true);
  ASSERT_TRUE(Prepare(*current).ok());
  const auto prepared = Step(core::ComponentLifecycleIntentKindDTO::kPrepare);
  ASSERT_EQ(prepared.component_lifecycle_state,
            core::ComponentLifecycleStateDTO::kPrepared);
  ASSERT_TRUE(
      BindCompositionIoOutputParticipants(*current, ActualGenerations()).ok());
  const auto rows = current->component_registrations();
  ASSERT_EQ(rows.size(), 1U);
  ASSERT_TRUE(rows.front().binding.value().io_output_binding);
  EXPECT_TRUE(rows.front().binding.value().resources.empty());
  EXPECT_FALSE(rows.front().binding.value().control_path);
  EXPECT_FALSE(rows.front().stop_readiness);
  ASSERT_EQ(rows.front().engine,
            current->io_output_channels.front()->engine.get());
  const auto product_resolver = current->target_resolver;
  ASSERT_NE(product_resolver, nullptr);
  const auto resolver = make_retained_cell_kernel_participant_resolver(
      [product_resolver](const core::OperationIntentDTO &intent,
                         std::size_t index) {
        return product_resolver->ResolveExecutionParticipant(intent, index);
      },
      rows);
  const auto installed = cell->reconfigure_runtime_product(rows, resolver, {});
  ASSERT_TRUE(installed.ok()) << installed.error().message;
  ASSERT_EQ(cell->participant_registrations().size(), 1U);
  EXPECT_EQ(cell->participant_registrations().front().binding.value(),
            rows.front().binding.value());
  const auto slots = cell->addressed_kernel().resource_snapshot();
  ASSERT_EQ(slots.size(), 1U);
  EXPECT_EQ(
      slots.front().resource_id,
      *rows.front().binding.value().io_output_binding->signal.output_resource);
  EXPECT_TRUE(cell->stop_surveillance_owner()->SnapshotSources().empty());
  EXPECT_FALSE(take(cell->addressed_kernel().component_command_readiness(
                        current->controllable_component_id))
                   .command_eligible);
  EXPECT_FALSE(Mock(*current)->model_value());
}

} // namespace manyforge::roboplan_backend::host_detail
