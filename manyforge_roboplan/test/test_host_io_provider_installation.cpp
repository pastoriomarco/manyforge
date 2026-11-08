#include "host_io_provider_installation.hpp"
#include "retired_composition_release.hpp"
#include "execution_host_responses.hpp"
#include "host_io_signal_runtime.hpp"
#include "io_provider_lifecycle_product.hpp"
#include <manyforge_roboplan/passive_component_adapter.hpp>
#include <manyforge_roboplan/component_stream_sample_bridge.hpp>
#include <manyforge_roboplan/adapter_transport_consumer.hpp>
#include <manyforge_ros2_control_execution/ros2_transport_resources.hpp>
#include <diagnostic_msgs/msg/diagnostic_status.hpp>
#include <future>

#include "../../manyforge_deployment/test/io_retained_fixture.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <control_msgs/msg/dynamic_interface_group_values.hpp>
#include <cstdint>
#include <filesystem>
#include <gtest/gtest.h>
#include <limits>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <rclcpp/executors/single_threaded_executor.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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
// driver (the I/O provider component's adapter), channel and stable
// cell. It exercises the trusted helper boundary, not Manager Apply authority.
class HostIoProviderInstallationTest : public ::testing::Test {
protected:
  void Build(bool input = false, bool output = true, bool second = false,
             bool shared_channel = false,
             std::string_view output_profile = {}, bool controller_owned = false) {
    package = temporary.path / "package";
    checked(std::filesystem::create_directory(package), "package root missing");
    bundle = deployment::test::materialize_boolean_io_retained_fixture(
                 package, input, output, false, second, shared_channel,
                 nullptr, output_profile, controller_owned)
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
    cell = std::make_unique<RetainedCellRuntime>(
        RetainedCellRuntimeInput{.clock = clock,
                                       .scheduler = scheduler,
                                       .operation_journal_path =
                                           temporary.path / "operations.sqlite",
                                       .participant_registrations = {},
                                       .kernel_config = std::move(config)});
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
    // Keep the output channels alive until the cell is gone.
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
          provider->lifecycle_plan_catalog_digest};
      result.push_back(take(IoProviderDriverRecord::Create(
          std::move(scope), std::move(occurrence), manifest->value(), clock)));
    }
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

  // Runs one lifecycle action on the I/O provider component's adapter, as
  // the host's component runtime does.
  void Step(core::ComponentAction action) {
    const auto result = drivers->front()->Run(action, {});
    checked(result.outcome == core::ActionOutcome::kSucceeded,
            result.error ? result.error->message : "lifecycle action failed");
  }
  core::ComponentLifecycle Lifecycle(const IoProviderDriverRecord &driver) {
    return driver.State(clock->now()).lifecycle;
  }
  std::shared_ptr<MockBooleanOutputProvider> Mock(const Composition &view) {
    checked(view.io_output_channels.size() == 1U,
            "expected one actual canonical channel");
    return std::get<std::shared_ptr<MockBooleanOutputProvider>>(
        view.io_output_channels.front()->provider);
  }
  // The one write path: the I/O component's output writer (`set_io_output`).
  core::ActionOutcome Write(const std::string &signal, bool value) {
    return drivers->front()->SetOutput(signal, value, {}).outcome;
  }
  TemporaryRoot temporary;
  std::filesystem::path package;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::optional<deployment::VerifiedRetainedModelMap> model_map;
  std::vector<std::shared_ptr<const deployment::ProgramBinding>>
      direct_bindings;
  std::shared_ptr<WallClock> clock;
  std::shared_ptr<SimpleScheduler> scheduler;
  std::unique_ptr<RetainedCellRuntime> cell;
  std::unique_ptr<component_manager::SqliteActiveBundlePublicationStore>
      publication_store;
  core::ProjectionRevisionDTO projection;
  core::ContentDigest materialization{std::string{}};
  std::optional<core::ActiveBundlePublicationReceiptDTO> publication;
  std::shared_ptr<const IoProviderDriverGroup> drivers;
  std::unique_ptr<Composition> current;
};

TEST_F(HostIoProviderInstallationTest, ApplySnapshotGuardKeepsReusedQualifiedIoOnSuccessAndUnwind) {
  Build();
  ASSERT_TRUE(Prepare(*current).ok());
  ASSERT_NE(current->io_signal_runtime, nullptr);
  current->io_signal_runtime->Start();
  Step(core::ComponentAction::kPrepare);
  Step(core::ComponentAction::kEnable);
  auto retained_snapshot = [&] {
    return RetainedComponentSnapshot{
        current->runtime, current->gripper_runtime, current->stage,
        current->bundle->value().compiled_products.planning_collision_model,
        current->direct_control_joint_limits, current->direct_control_motion_descriptor,
        current->direct_control_gripper_descriptor, current->ros2_control_runtime_handle,
        current->controller_process, current->io_provider_drivers, current->component_runtime_pins,
        current->component_adapter, current->owner_component, current->owner_handle,
        current->owner_family, current->owner_id, current->member_role,
        current->adapter_transport_consumers};
  };
  std::vector<PendingDiscard> pending;
  const auto registrations = [](const Composition &view) {
    EXPECT_FALSE(view.runtime);
    EXPECT_FALSE(view.gripper_runtime);
    return std::set<core::IResolvedParticipantExecutionEngine *>{};
  };
  const auto collect = [&](const PendingDiscard &entry, auto &pins, auto &engines) {
    CollectRetainedPendingProtection(entry, pins, engines, registrations);
  };
  const std::function<void(std::vector<PendingDiscard> &)> settle = [&](auto &snapshots) {
    SettleApplyPendingSnapshots(snapshots, pending, {current.get(), nullptr, nullptr, nullptr},
        [&] { return &cell->addressed_kernel(); }, registrations, collect);
  };
  const auto *original = drivers.get();
  const std::weak_ptr<const IoProviderDriverGroup> weak = drivers;
  {
    ApplyPendingSnapshotCleanup guard{{}, settle};
    guard.snapshots.push_back(PendingDiscard{nullptr, {retained_snapshot()}, "io-owner", false});
  }
  EXPECT_TRUE(pending.empty());
  EXPECT_EQ(current->io_provider_drivers.get(), original);
  EXPECT_FALSE(weak.expired());
  try {
    ApplyPendingSnapshotCleanup guard{{}, settle};
    guard.snapshots.push_back(PendingDiscard{nullptr, {retained_snapshot()}, "io-owner", false});
    if (Write("lamp", true) != core::ActionOutcome::kSucceeded)
      throw std::runtime_error("fixture-write-failed");
    throw std::runtime_error("original-apply-failure");
  } catch (const std::runtime_error &error) {
    EXPECT_STREQ(error.what(), "original-apply-failure");
  }
  EXPECT_TRUE(pending.empty());
  EXPECT_EQ(current->io_provider_drivers.get(), original);
  EXPECT_FALSE(weak.expired());
  EXPECT_EQ(Mock(*current)->model_value(), std::optional<bool>(true));

  // The local snapshot may settle while another pending entry and the final
  // root both retain the same qualified I/O provider owners.
  pending.push_back(PendingDiscard{nullptr, {retained_snapshot()}, "other-pending", false});
  {
    ApplyPendingSnapshotCleanup guard{{}, settle};
    guard.snapshots.push_back(PendingDiscard{nullptr, {retained_snapshot()}, "io-owner", false});
  }
  ASSERT_EQ(pending.size(), 1U);
  ASSERT_EQ(pending.front().snapshots.size(), 1U);
  EXPECT_EQ(pending.front().snapshots.front().io_provider_drivers.get(), original);
  EXPECT_EQ(current->io_provider_drivers.get(), original);
  EXPECT_FALSE(weak.expired());
}

TEST(HostTransportAdmission, ApplyFirstRefusesRecoveryWithoutBlockingOtherEffects) {
  ExecutionCompositionAccess access;
  auto apply = access.TryTransportEffect();
  ASSERT_TRUE(apply);
  auto other_effect = access.TryTransportEffect();
  ASSERT_TRUE(other_effect);
  EXPECT_FALSE(access.TryTransportRecovery());
  apply.reset();
  EXPECT_FALSE(access.TryTransportRecovery());
  other_effect.reset();
  EXPECT_TRUE(access.TryTransportRecovery());
}

TEST(HostTransportAdmission, RecoveryFirstRefusesApplyAndOtherEffects) {
  ExecutionCompositionAccess access;
  auto recovery = access.TryTransportRecovery();
  ASSERT_TRUE(recovery);
  EXPECT_FALSE(access.TryTransportEffect());
  EXPECT_FALSE(access.TryTransportRecovery());
  recovery.reset();
  EXPECT_TRUE(access.TryTransportEffect());
}

TEST(HostTransportAdmission, FailedRecoveryReleasesOnEveryExit) {
  ExecutionCompositionAccess access;
  EXPECT_THROW(([&] {
    auto recovery = access.TryTransportRecovery();
    ASSERT_TRUE(recovery);
    throw std::runtime_error("injected rebuild failure");
  }()), std::runtime_error);
  EXPECT_TRUE(access.TryTransportEffect());
  EXPECT_TRUE(access.TryTransportRecovery());
}

TEST_F(HostIoProviderInstallationTest, ArmRecoveryRefusesAllMemberResetBeforeEffect) {
  Build();
  const auto first = current->controllable_component_id;
  auto peer = View(publication->publication_generation, drivers);
  peer->controllable_component_id = core::ControllableComponentId("other.arm");
  current->ros2_control_runtime_handle = RetainedRos2ControlRuntimeHandle::Create();
  peer->ros2_control_runtime_handle = RetainedRos2ControlRuntimeHandle::Create();
  current->ros2_control_runtime_handle->InstallActiveTcp(first, {}, {}, false);
  peer->ros2_control_runtime_handle->InstallActiveTcp(peer->controllable_component_id, {}, {}, false);
  int reset_claims = 0;
  current->ros2_control_runtime_handle->InstallTransportEffectClaim([&] {
    ++reset_claims;
    return core::Result<std::shared_ptr<void>>::Ok(nullptr);
  });
  const auto before = current->ros2_control_runtime_handle->ReadActiveTcp();
  const auto peer_before = peer->ros2_control_runtime_handle->ReadActiveTcp();
  auto retained = std::make_shared<InstalledRetainedTransportMembership>();
  retained->entries.push_back({peer->controllable_component_id.value(), peer->ros2_control_runtime_handle, {}});
  current->participant_compositions.push_back(std::move(peer));
  ExecutionCompositionAccess access;
  access.enter_transport_effect = [&](const std::string &id) {
    return EnterInstalledTransportEffect(access, id, nullptr, nullptr, retained);
  };
  auto recovery = access.TryTransportRecovery();
  ASSERT_TRUE(recovery);
  const auto result = ResetCompositionActiveTcps(*current, access);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "component_action_in_progress");
  EXPECT_EQ(result.error().details.at("effect_disposition"), "known_no_effect");
  EXPECT_EQ(reset_claims, 0);
  EXPECT_EQ(current->ros2_control_runtime_handle->ReadActiveTcp().sequence, before.sequence);
  EXPECT_EQ(current->participant_compositions.front()->ros2_control_runtime_handle->ReadActiveTcp().sequence,
            peer_before.sequence);
}

TEST_F(HostIoProviderInstallationTest, AdmittedResetDoesNotReenterTransportClaim) {
  Build();
  current->ros2_control_runtime_handle = RetainedRos2ControlRuntimeHandle::Create();
  current->ros2_control_runtime_handle->InstallActiveTcp(current->controllable_component_id, {}, {}, false);
  int claims = 0;
  current->ros2_control_runtime_handle->InstallTransportEffectClaim([&] {
    ++claims;
    return core::Result<std::shared_ptr<void>>::Err({"component_busy", "duplicate claim", {}});
  });
  auto retained = std::make_shared<InstalledRetainedTransportMembership>();
  retained->entries.push_back({current->controllable_component_id.value(),
                               current->ros2_control_runtime_handle, {}});
  ExecutionCompositionAccess access;
  access.enter_transport_effect = [&](const std::string &id) {
    return EnterInstalledTransportEffect(access, id, nullptr, nullptr, retained);
  };
  EXPECT_TRUE(ResetCompositionActiveTcps(*current, access).ok());
  EXPECT_EQ(claims, 0);
  EXPECT_TRUE(access.TryTransportRecovery());
}

TEST_F(HostIoProviderInstallationTest, ResetMemberRefusalMapsToTypedApiError) {
  Build();
  current->ros2_control_runtime_handle = RetainedRos2ControlRuntimeHandle::Create();
  current->ros2_control_runtime_handle->InstallActiveTcp(current->controllable_component_id, {}, {}, false);
  int first_claims = 0;
  current->ros2_control_runtime_handle->InstallTransportEffectClaim([&] {
    ++first_claims;
    return core::Result<std::shared_ptr<void>>::Ok(nullptr);
  });
  auto peer = View(publication->publication_generation, drivers);
  peer->controllable_component_id = core::ControllableComponentId("other.arm");
  peer->ros2_control_runtime_handle = RetainedRos2ControlRuntimeHandle::Create();
  peer->ros2_control_runtime_handle->InstallActiveTcp(peer->controllable_component_id, {}, {}, false);
  const auto before = peer->ros2_control_runtime_handle->ReadActiveTcp();
  peer->ros2_control_runtime_handle->InstallTransportEffectClaim([] {
    return core::Result<std::shared_ptr<void>>::Err(
        {"component_busy", "member refused", {{"effect_disposition", "known_no_effect"}}});
  });
  current->participant_compositions.push_back(std::move(peer));
  ExecutionCompositionAccess access;
  const auto result = ResetCompositionActiveTcps(*current, access);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(first_claims, 1);
  const auto response = ErrorResponse(result.error().code, result.error().message, result.error().details);
  const auto *ok = response.find_object_member("ok");
  const auto *error = response.find_object_member("error");
  ASSERT_NE(ok, nullptr);
  EXPECT_FALSE(ok->as_bool());
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->find_object_member("code")->as_string(), "component_busy");
  EXPECT_EQ(error->find_object_member("message")->as_string(), "member refused");
  EXPECT_EQ(error->find_object_member("details")->find_object_member("effect_disposition")->as_string(),
            "known_no_effect");
  EXPECT_EQ(current->participant_compositions.front()->ros2_control_runtime_handle->ReadActiveTcp().sequence,
            before.sequence);
}

TEST_F(HostIoProviderInstallationTest, AdapterRecoveryStagesSharedConsumerOnceBeforeCommit) {
  struct Consumer final : AdapterTransportConsumer {
    int staged{};
    int committed{};
    bool valid{true};
    Gate gate;
    std::shared_ptr<void> Stage(const std::shared_ptr<Owner> &,
                                const std::shared_ptr<Owner> &, Gate active) override {
      ++staged;
      gate = std::move(active);
      return std::make_shared<int>(1);
    }
    bool Validate(const std::shared_ptr<void> &) const override { return valid; }
    void Commit(const std::shared_ptr<void> &) noexcept override { ++committed; }
  };
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  const auto provider = [context] { return context; };
  auto old_owner = ros2_control_execution::Ros2TransportResources::create(
      "adapter_recovery_old", "", 1U, provider);
  auto new_owner = ros2_control_execution::Ros2TransportResources::create(
      "adapter_recovery_new", "", 1U, provider);
  old_owner->quiesce();
  auto shared = std::make_shared<Consumer>();
  auto other = std::make_shared<Consumer>();
  std::vector<std::pair<std::string, std::shared_ptr<AdapterTransportConsumer>>> rows{
      {"machine", shared}, {"arm", shared}, {"sensor", other}};
  auto staged = StageAdapterTransportRecovery(rows, old_owner, new_owner);
  ASSERT_TRUE(staged.ok());
  EXPECT_EQ(shared->staged, 1);
  EXPECT_EQ(other->staged, 1);
  EXPECT_FALSE(shared->gate->load());
  EXPECT_EQ(shared->gate, other->gate);
  EXPECT_FALSE(ActivateAdapterTransportRecovery(*staged.value(), new_owner).ok());
  new_owner->start();
  other->valid = false;
  EXPECT_FALSE(CommitAdapterTransportRecovery(*staged.value()).ok());
  EXPECT_EQ(shared->committed, 0);
  EXPECT_EQ(other->committed, 0);
  other->valid = true;
  ASSERT_TRUE(CommitAdapterTransportRecovery(*staged.value()).ok());
  EXPECT_EQ(shared->committed, 1);
  EXPECT_EQ(other->committed, 1);
  EXPECT_FALSE(shared->gate->load());
  EXPECT_FALSE(ActivateAdapterTransportRecovery(*staged.value(), old_owner).ok());
  ASSERT_TRUE(ActivateAdapterTransportRecovery(*staged.value(), new_owner).ok());
  EXPECT_TRUE(shared->gate->load());
  new_owner->quiesce();
  context->shutdown("adapter recovery stage test complete");
}

TEST_F(HostIoProviderInstallationTest, CommonProductViewBorrowsTheActualCompositionStorage) {
  Build();
  ExecutionCompositionAccess access;
  {
    std::unique_lock lock(access.mutex);
    static_cast<void>(access.replace_current(current.get()));
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
    static_cast<void>(access.replace_current(nullptr));
    EXPECT_EQ(access.product(), nullptr);
  }
}

// The owner of a machine with its arm deactivated is a view of
// its own (its adapter and its `owner_component` are the same): it is
// coordinated once, as an owner, and its I/O members carry it.
TEST_F(HostIoProviderInstallationTest, AnOwnerViewIsCoordinatedOnceAsTheOwner) {
  Build();
  const auto make_view = [&](const std::string &id) {
    return std::make_unique<Composition>(
        core::ControllableComponentId(id), bundle, nullptr, nullptr, nullptr, nullptr,
        std::nullopt, std::nullopt, std::nullopt, CanonicalValue(nullptr),
        CanonicalValue(nullptr), nullptr, nullptr, projection,
        publication->publication_generation, std::nullopt, std::nullopt);
  };
  auto owner = make_view("lite6");
  owner->component_adapter = std::make_shared<PassiveComponentAdapter>();
  owner->owner_component = core::CoordinatedComponent{"lite6", owner->component_adapter};
  auto member = make_view("lite6_controller_io");
  member->component_adapter = std::make_shared<PassiveComponentAdapter>();
  member->owner_id = "lite6";
  member->member_role = "controller_io";
  owner->participant_compositions.push_back(std::move(member));

  const auto components = CoordinatedComponentsOf(owner.get());
  ASSERT_EQ(components.size(), 2U);
  EXPECT_EQ(components[0].component_id, "lite6");
  EXPECT_TRUE(components[0].owner);
  EXPECT_EQ(components[0].adapter, owner->component_adapter);
  EXPECT_EQ(components[1].component_id, "lite6_controller_io");
  EXPECT_FALSE(components[1].owner);
  EXPECT_EQ(components[1].owner_id, std::optional<std::string>("lite6"));
  EXPECT_EQ(components[1].role, std::optional<std::string>("controller_io"));
}

TEST_F(HostIoProviderInstallationTest, TcpRegistryFollowsSelectedComposition) {
  Build();
  const core::ControllableComponentId first("tcp.robot.first");
  const core::ControllableComponentId second("tcp.robot.second");
  const core::ControllableComponentId third("tcp.robot.third");
  const auto make_composition = [&](const core::ControllableComponentId &id) {
    return std::make_unique<Composition>(
        id, bundle, nullptr, nullptr, nullptr, nullptr, std::nullopt,
        std::nullopt, std::nullopt, CanonicalValue(nullptr),
        CanonicalValue(nullptr), nullptr, nullptr, projection,
        publication->publication_generation, std::nullopt, std::nullopt);
  };
  auto predecessor = make_composition(first);
  auto peer = make_composition(second);
  auto candidate = make_composition(first);
  auto retained_peer = make_composition(second);
  auto added_peer = make_composition(third);
  auto first_owner = std::make_shared<int>(0);
  auto retained_owner = std::make_shared<int>(0);
  auto candidate_owner = std::make_shared<int>(0);
  auto added_owner = std::make_shared<int>(0);
  const auto reader = [](const std::shared_ptr<int> &owner) -> HostRobotTcpReader {
    return [weak = std::weak_ptr(owner)]() -> std::optional<ActiveTcp> {
      const auto live = weak.lock();
      if (!live) return std::nullopt;
      ++*live;
      return ActiveTcp{};
    };
  };
  predecessor->staged_robot_tcp_reader = reader(first_owner);
  peer->staged_robot_tcp_reader = reader(retained_owner);
  predecessor->participant_compositions.push_back(std::move(peer));
  candidate->staged_robot_tcp_reader = reader(candidate_owner);
  retained_peer->staged_robot_tcp_reader = reader(retained_owner);
  added_peer->staged_robot_tcp_reader = reader(added_owner);
  candidate->participant_compositions.push_back(std::move(retained_peer));
  candidate->participant_compositions.push_back(std::move(added_peer));

  ExecutionCompositionAccess access;
  struct ClearSelection {
    ExecutionCompositionAccess &access;
    ~ClearSelection() {
      std::unique_lock lock(access.mutex);
      static_cast<void>(access.replace_current(nullptr));
    }
  } cleanup{access};
  const auto select = [&](Composition *selected) {
    std::unique_lock lock(access.mutex);
    static_cast<void>(access.replace_current(selected));
  };

  // Startup selection publishes every robot; merely building a candidate
  // cannot overwrite a predecessor callback, even if it is discarded.
  select(predecessor.get());
  ASSERT_TRUE(ReadHostRobotTcps(first));
  ASSERT_TRUE(ReadHostRobotTcps(second));
  EXPECT_FALSE(ReadHostRobotTcps(third));
  EXPECT_EQ(*first_owner, 1);
  EXPECT_EQ(*retained_owner, 1);
  EXPECT_EQ(*candidate_owner, 0);

  auto discarded = make_composition(first);
  discarded->staged_robot_tcp_reader = reader(candidate_owner);
  discarded.reset();
  ASSERT_TRUE(ReadHostRobotTcps(first));
  EXPECT_EQ(*first_owner, 2);
  EXPECT_EQ(*candidate_owner, 0);

  // Once selected, a later setup failure leaves this exact inventory in
  // place; selection itself does not restore the predecessor.
  EXPECT_THROW({ select(candidate.get()); throw std::runtime_error("setup failed"); },
               std::runtime_error);
  EXPECT_EQ(access.current, candidate.get());
  ASSERT_TRUE(ReadHostRobotTcps(first));
  ASSERT_TRUE(ReadHostRobotTcps(second));
  ASSERT_TRUE(ReadHostRobotTcps(third));
  EXPECT_EQ(*candidate_owner, 1);
  EXPECT_EQ(*retained_owner, 2);
  EXPECT_EQ(*added_owner, 1);

  // Expiration is unavailable, never a fallback to the old owner.
  candidate_owner.reset();
  EXPECT_FALSE(ReadHostRobotTcps(first));
  EXPECT_EQ(*first_owner, 2);
  candidate->participant_compositions.clear();
  select(candidate.get());
  EXPECT_FALSE(ReadHostRobotTcps(second));
  EXPECT_FALSE(ReadHostRobotTcps(third));

  // The production staged reader owns only a weak ROS handle.
  candidate->ros2_control_runtime_handle = RetainedRos2ControlRuntimeHandle::Create();
  candidate->ros2_control_runtime_handle->InstallActiveTcp(first, {}, {}, false);
  candidate->StageRobotTcpReader();
  select(candidate.get());
  ASSERT_TRUE(ReadHostRobotTcps(first));
  candidate->ros2_control_runtime_handle.reset();
  EXPECT_FALSE(ReadHostRobotTcps(first));
  select(nullptr);
  EXPECT_FALSE(ReadHostRobotTcps(first));
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

  // Existing actual installation still makes no motion/gripper registration
  // for this genuine input-only component.
  ASSERT_TRUE(Prepare(*current).ok());
  EXPECT_TRUE(current->component_registrations().empty());
}

TEST_F(HostIoProviderInstallationTest,
       GenuineInputOnlyCompositionHasNoExecutionAndStaysNotPrepared) {
  Build(true, false);
  ASSERT_TRUE(Prepare(*current).ok());
  ASSERT_NE(current->io_signal_runtime, nullptr);
  EXPECT_TRUE(current->io_output_channels.empty());
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
  EXPECT_EQ(Lifecycle(*drivers->front()), core::ComponentLifecycle::kNotPrepared);
}

TEST_F(HostIoProviderInstallationTest,
       ActualSharedAliasesUseOneChannelAndWriteOnlyWhileEnabled) {
  Build(false, true, true, true);
  ASSERT_TRUE(Prepare(*current).ok());
  ASSERT_EQ(current->io_output_channels.size(), 1U);
  EXPECT_FALSE(Mock(*current)->model_value());
  Step(core::ComponentAction::kPrepare);
  EXPECT_EQ(Lifecycle(*drivers->front()), core::ComponentLifecycle::kPrepared);
  EXPECT_EQ(Write("lamp", true), core::ActionOutcome::kFailed);
  EXPECT_FALSE(Mock(*current)->model_value());
  Step(core::ComponentAction::kEnable);
  EXPECT_EQ(Lifecycle(*drivers->front()), core::ComponentLifecycle::kEnabled);
  EXPECT_TRUE(current->component_registrations().empty());
  EXPECT_EQ(Write("lamp_other", true), core::ActionOutcome::kSucceeded);
  EXPECT_EQ(Mock(*current)->model_value(), std::optional<bool>(true));
}

TEST_F(HostIoProviderInstallationTest,
       MockOutputWriteIsPublishedBackAsItsSampleWithItsOutsideChangeSignal) {
  // A Mock output reports its written value back, source
  // `manyforge_write`, and has the core-held `changed_outside` signal.
  Build();
  ASSERT_TRUE(Prepare(*current).ok());
  const auto runtime = current->io_signal_runtime;
  ASSERT_NE(runtime, nullptr);
  runtime->Start();
  Step(core::ComponentAction::kPrepare);
  Step(core::ComponentAction::kEnable);
  const auto capabilities = drivers->front()->capabilities();
  EXPECT_NE(std::find(capabilities.begin(), capabilities.end(),
                      core::Capability::kIoOutputReadback),
            capabilities.end());
  ASSERT_EQ(Write("lamp", true), core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(runtime->DrainPending().ok());
  const core::IoSignalId lamp{
      drivers->front()->domain().provider()->provider.controllable_component_id, "lamp"};
  const auto observed = runtime->service()->ReadSignal(lamp);
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  const auto sample = core::ToSignalSample(observed.value());
  EXPECT_EQ(sample.value, (std::optional<std::variant<bool, double>>(true)));
  EXPECT_EQ(sample.source, core::IoSignalSampleSource::kManyforgeWrite);
  EXPECT_TRUE(sample.fresh);
  const auto outside = runtime->service()->ReadSignal(core::ChangedOutsideSignalId(lamp));
  ASSERT_TRUE(outside.ok()) << outside.error().message;
  EXPECT_EQ(core::ToSignalSample(outside.value()).value,
            (std::optional<std::variant<bool, double>>(false)));
  runtime->Shutdown();
}

TEST_F(HostIoProviderInstallationTest, ControllerOwnedOutputIsRefusedBeforeSending) {
  Build(false, true, false, false, {}, true);
  ASSERT_TRUE(Prepare(*current).ok());
  Step(core::ComponentAction::kPrepare);
  Step(core::ComponentAction::kEnable);
  const auto refused = drivers->front()->SetOutput("lamp", true, {});
  EXPECT_EQ(refused.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "io_output_controller_owned");
  EXPECT_FALSE(Mock(*current)->model_value());
}

TEST_F(HostIoProviderInstallationTest,
       RetainedActualChannelResetsOnceAndKeepsItsEnabledGate) {
  Build();
  ASSERT_TRUE(Prepare(*current).ok());
  Step(core::ComponentAction::kPrepare);
  Step(core::ComponentAction::kEnable);
  ASSERT_EQ(Write("lamp", true), core::ActionOutcome::kSucceeded);
  const auto predecessor = Capture(*current); // before any publication mutation
  const auto entry = current->io_output_channels.front();
  auto candidate = View(NewPublication(), drivers);
  ASSERT_TRUE(Prepare(*candidate, predecessor).ok());
  EXPECT_EQ(candidate->io_output_channels.front(), entry);
  EXPECT_FALSE(Mock(*candidate)->model_value());
  ASSERT_EQ(Write("lamp", false), core::ActionOutcome::kSucceeded);
  EXPECT_EQ(Mock(*candidate)->model_value(), std::optional<bool>(false));
  const auto replay = Capture(*candidate);
  ASSERT_TRUE(Prepare(*candidate, replay).ok());
  EXPECT_EQ(candidate->io_output_channels.front(), entry);
  EXPECT_EQ(Mock(*candidate)->model_value(), std::optional<bool>(false));
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
  EXPECT_FALSE(runtime_lifetime.expired());
  candidate.reset();
  EXPECT_TRUE(lifetime.expired());
  EXPECT_TRUE(runtime_lifetime.expired());
}
TEST_F(HostIoProviderInstallationTest,
       GenuineOutputRemovalPermanentlyFencesOnlyItsOldOccurrence) {
  Build();
  ASSERT_TRUE(Prepare(*current).ok());
  Step(core::ComponentAction::kPrepare);
  Step(core::ComponentAction::kEnable);
  ASSERT_EQ(Write("lamp", true), core::ActionOutcome::kSucceeded);
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
  EXPECT_TRUE(candidate->component_registrations().empty());
}

TEST_F(HostIoProviderInstallationTest,
       RetainedRosOutputRejectsDifferentOwnerBeforeRebind) {
  Build(false, true, false, false, "ros2_control_gpio");
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  auto owner = ros2_control_execution::Ros2TransportResources::create(
      "io_metadata_owner", "", 1U, [context] { return context; });
  auto other = ros2_control_execution::Ros2TransportResources::create(
      "io_metadata_other", "", 1U, [context] { return context; });
  ASSERT_TRUE(PrepareCompositionIoProviderResources(*current, *cell,
                                                    owner->node(), {}, owner)
                  .ok());
  ASSERT_FALSE(drivers->empty());
  EXPECT_EQ(drivers->front()->RosOwner(), owner);
  auto predecessors = Capture(*current);
  auto candidate = View(NewPublication(), drivers);
  const auto result = PrepareCompositionIoProviderResources(
      *candidate, *cell, other->node(), predecessors, other);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "io_ros_owner_mismatch");
  EXPECT_EQ(drivers->front()->RosOwner(), owner);
  for (const auto &driver : *drivers)
    driver->Shutdown();
  candidate.reset();
  current.reset();
  predecessors.clear();
  drivers.reset();
  other.reset();
  owner.reset();
  context->shutdown("I/O owner metadata fixture complete");
}

TEST_F(HostIoProviderInstallationTest, FaultedGenericOwnerRebuildsInstalledGpioDriver) {
  Build(false, true, false, false, "ros2_control_gpio");
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  const auto provider = [context] { return context; };
  auto old_owner = ros2_control_execution::Ros2TransportResources::create(
      "gpio_recovery_old", "", 1U, provider);
  ASSERT_TRUE(PrepareCompositionIoProviderResources(*current, *cell, old_owner->node(), {}, old_owner).ok());
  auto faulted = std::make_shared<std::promise<void>>();
  auto observed = faulted->get_future();
  old_owner->set_owner_notification([] { throw std::runtime_error("injected GPIO owner failure"); });
  old_owner->set_executor_fault_observer([faulted](std::string_view) { faulted->set_value(); });
  old_owner->start();
  ASSERT_TRUE(old_owner->notify_owner());
  ASSERT_EQ(observed.wait_for(std::chrono::seconds(3)), std::future_status::ready);
  const std::array<const Composition *, 1> roots{current.get()};
  auto snapshot = CaptureCompositionIoOwnerRecovery(roots, old_owner);
  ResolveCompositionIoOwnerRecoveryServed(*snapshot);
  ASSERT_FALSE(CompositionIoOwnerRecoveryServed(*snapshot).empty());
  ASSERT_TRUE(CloseCompositionIoOwnerRecovery(*snapshot).ok());
  std::shared_ptr<CompositionIoOwnerRecoveryStage> stage;
  std::shared_ptr<ros2_control_execution::Ros2TransportResources> candidate;
  candidate = ros2_control_execution::Ros2TransportResources::rebuild_faulted(
      *old_owner, std::chrono::steady_clock::now() + std::chrono::seconds(2), [] {}, [&] {
        candidate = ros2_control_execution::Ros2TransportResources::create(
            "gpio_recovery_new", "", 1U, provider);
        return candidate;
      }, [&](auto &fresh) {
        auto staged = StageCompositionIoOwnerRecovery(snapshot, candidate);
        ASSERT_TRUE(staged.ok());
        stage = std::move(staged.value());
        fresh.start();
      });
  ASSERT_TRUE(stage);
  ASSERT_TRUE(old_owner->joined());
  ASSERT_TRUE(CommitCompositionIoOwnerRecovery(*stage).ok());
  ActivateCompositionIoOwnerRecovery(*stage);
  ASSERT_FALSE(drivers->empty());
  EXPECT_EQ(drivers->front()->RosOwner(), candidate);
  EXPECT_EQ(Lifecycle(*drivers->front()), core::ComponentLifecycle::kPrepared);
  for (const auto &driver : *drivers) driver->Shutdown();
  stage.reset();
  snapshot.reset();
  current.reset();
  drivers.reset();
  candidate->quiesce();
  candidate.reset();
  old_owner.reset();
  context->shutdown("GPIO owner recovery test complete");
}

TEST_F(HostIoProviderInstallationTest,
       QualifiedGpioRemovalKeepsTopicAndRefusedPreparationPreservesOldTables) {
  Build(false, true, true, false, "ros2_control_gpio");
  using Message = control_msgs::msg::DynamicInterfaceGroupValues;
  auto ros_context = std::make_shared<rclcpp::Context>();
  ros_context->init(0, nullptr);
  const auto context_provider = [ros_context] { return ros_context; };
  // Qualified GPIO drivers are installed through a ROS transport owner, as in
  // the other GPIO installation tests; its node is the generic node.
  auto owner = ros2_control_execution::Ros2TransportResources::create(
      "gpio_installation", "", 1U, context_provider);
  // The owner adds its node to its own executor at construction and spins it
  // after start(); the node is never added to a second executor here.
  auto node = owner->node();
  std::mutex messages_mutex;
  std::vector<Message> messages;  // guarded by messages_mutex
  auto subscriber = node->create_subscription<Message>(
      "/io_device/commands", rclcpp::SystemDefaultsQoS(),
      [&](const Message& message) {
        std::lock_guard lock(messages_mutex);
        messages.push_back(message);
      });
  const auto received_messages = [&] {
    std::lock_guard lock(messages_mutex);
    return messages;
  };
  const auto spin_until = [&](const auto& ready) {
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!ready() && std::chrono::steady_clock::now() < limit)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return ready();
  };
  ASSERT_TRUE(PrepareCompositionIoProviderResources(*current, *cell, node, {}, owner).ok());
  owner->start();
  ASSERT_EQ(current->io_output_channels.size(), 2U);
  ASSERT_TRUE(spin_until([&] { return subscriber->get_publisher_count() == 1U; }));
  Step(core::ComponentAction::kPrepare);
  Step(core::ComponentAction::kEnable);
  ASSERT_EQ(Write("lamp", true), core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(spin_until([&] { return received_messages().size() == 1U; }));
  ASSERT_EQ(Write("lamp_other", false), core::ActionOutcome::kSucceeded);
  ASSERT_TRUE(spin_until([&] { return received_messages().size() == 2U; }));
  const auto received = received_messages();
  ASSERT_EQ(received.back().interface_groups.size(), 1U);
  ASSERT_EQ(received.back().interface_values.size(), 1U);
  EXPECT_EQ(received.back().interface_groups.front(), "digital_outputs");
  const auto& payload = received.back().interface_values.front();
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
  EXPECT_FALSE(PrepareCompositionIoProviderResources(*candidate, *cell, node, wrong_predecessor, owner).ok());
  EXPECT_TRUE(candidate->io_output_channels.empty());
  same_table(retained_provider->ReadAliasState(), retained_before);
  same_table(removed_provider->ReadAliasState(), removed_before);
  EXPECT_EQ(received_messages().size(), 2U);

  ASSERT_TRUE(PrepareCompositionIoProviderResources(*candidate, *cell, node, predecessor, owner).ok());
  ASSERT_EQ(candidate->io_output_channels.size(), 1U);
  EXPECT_EQ(candidate->io_output_channels.front(), retained_entry);
  EXPECT_EQ(retained_provider->gpio_topic(), removed_provider->gpio_topic());
  EXPECT_EQ(retained_provider->ReadAliasState().active_publication_generation,
            candidate->publication_generation);
  same_table(removed_provider->ReadAliasState(), removed_before);
  EXPECT_FALSE(removed_provider
                   ->Resume(original_driver->occurrence(),
                            std::numeric_limits<std::uint64_t>::max(), original_driver)
                   .ok());
  EXPECT_EQ(received_messages().size(), 2U);  // preparation/retirement never resends
  // Candidate stays paused: its new driver is not prepared, and no
  // post-Apply dispatch/physical-effect claim is made.
  EXPECT_EQ(Lifecycle(*replacement_drivers->front()), core::ComponentLifecycle::kNotPrepared);
  candidate.reset();
  for (const auto& driver : *replacement_drivers) driver->Shutdown();
  for (const auto& driver : *drivers) driver->Shutdown();
  owner->quiesce();
  owner.reset();
  ros_context->shutdown("GPIO installer integration complete");
}

TEST_F(HostIoProviderInstallationTest, OwnedStreamBridgeRetainsSubscriptionUntilForeignDrop) {
  Build();
  auto context = std::make_shared<rclcpp::Context>();
  context->init(0, nullptr);
  rclcpp::NodeOptions options;
  options.context(context);
  options.use_global_arguments(false);
  auto publisher_node = std::make_shared<rclcpp::Node>("bridge_test_publisher", options);
  rclcpp::ExecutorOptions executor_options;
  executor_options.context = context;
  rclcpp::executors::SingleThreadedExecutor executor(executor_options);
  executor.add_node(publisher_node);
  auto transport = ros2_control_execution::Ros2TransportResources::create(
      "bridge_test_owner", "", 1U, [context] { return context; });
  auto health = std::make_shared<core::ComponentStreamHealth>(clock, *scheduler);
  const core::ControllableComponentId component("bridge.component");
  auto declared = health->Declare(component, {"inputs", 10'000'000'000LL});
  ASSERT_TRUE(declared.ok());
  auto recorder = std::move(declared.value());
  auto bridge = InstallComponentStreamSampleBridge(transport->node(), health, transport);
  ASSERT_TRUE(bridge);
  auto copy = bridge;
  transport->start();
  auto publisher = publisher_node->create_publisher<diagnostic_msgs::msg::DiagnosticStatus>(
      "/manyforge/component_stream_samples", rclcpp::QoS(rclcpp::KeepLast(64)).reliable());
  diagnostic_msgs::msg::DiagnosticStatus message;
  message.hardware_id = component.value();
  message.name = "inputs";
  message.level = diagnostic_msgs::msg::DiagnosticStatus::OK;
  diagnostic_msgs::msg::KeyValue stamp;
  stamp.key = "stamp_ns";
  stamp.value = "0";
  message.values.push_back(stamp);
  auto drop = std::async(std::launch::async, [&] { bridge.reset(); });
  drop.get();
  const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < until) {
    publisher->publish(message);
    health->Evaluate();
    const auto current = health->Read(component);
    if (!current.empty() && current.front().state == core::StreamLivenessStateDTO::kLive)
      break;
    executor.spin_once(std::chrono::milliseconds(5));
  }
  const auto current = health->Read(component);
  ASSERT_EQ(current.size(), 1U);
  EXPECT_EQ(current.front().state, core::StreamLivenessStateDTO::kLive);
  copy.reset();
  transport->quiesce();
  publisher.reset();
  executor.remove_node(publisher_node);
  publisher_node.reset();
  transport.reset();
  recorder.reset();
  health.reset();
  context->shutdown("stream bridge test complete");
}

} // namespace

} // namespace manyforge::roboplan_backend::host_detail
