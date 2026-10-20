#include "io_retained_fixture.hpp"
#include "canonical_empty_admission_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "managed_kernel_apply_fence_owner.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_core/route_fence_record_codec_internal.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace core = manyforge::core;
namespace component_manager = manyforge::component_manager;
namespace deployment = manyforge::deployment;
using manyforge::roboplan_backend::ManagedKernelAdmissionGate;
using manyforge::roboplan_backend::ManagedKernelApplyFenceOwner;
using manyforge::roboplan_backend::ManagedKernelApplyFenceOwnerConfig;

constexpr std::string_view kSha =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

class ScopedTemporaryRoot final {
public:
  ScopedTemporaryRoot() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-route-fence-owner-test-XXXXXX")
                       .string();
    storage_.assign(pattern.begin(), pattern.end());
    storage_.push_back('\0');
    const auto *created = ::mkdtemp(storage_.data());
    if (created == nullptr) {
      throw std::runtime_error("route-fence owner test mkdtemp failed");
    }
    path_ = created;
  }

  ~ScopedTemporaryRoot() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> storage_;
  std::filesystem::path path_;
};

[[nodiscard]] std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("route-fence owner fixture file is absent");
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

void CopyDirectory(const std::filesystem::path &source,
                   const std::filesystem::path &destination) {
  std::error_code error;
  std::filesystem::copy(source, destination,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::copy_symlinks,
                        error);
  if (error) {
    throw std::runtime_error("route-fence owner fixture copy failed: " +
                             error.message());
  }
}

[[nodiscard]] const std::string &
TextMember(const deployment::CanonicalValue &root,
           const std::string_view name) {
  const auto *member = root.find_object_member(name);
  if (member == nullptr) {
    throw std::runtime_error("route-fence owner fixture is incomplete");
  }
  const auto *value = std::get_if<std::string>(&member->storage());
  if (value == nullptr || value->empty()) {
    throw std::runtime_error("route-fence owner fixture field is invalid");
  }
  return *value;
}

struct OwnerFixture final {
  explicit OwnerFixture(bool canonical_empty = false, bool io_provider = false)
      : retained_root(
            io_provider ? [](const std::filesystem::path& root) {
              if (!std::filesystem::create_directory(root))
                throw std::runtime_error("I/O fixture retained root was not created");
              return root;
            }(state_root.path() / "io-retained")
            : canonical_empty
                ? std::filesystem::path(
                      MANYFORGE_CANONICAL_EMPTY_PRODUCT_PACKAGE_ROOT)
                : std::filesystem::path(MANYFORGE_I6_NATIVE_FIXTURE_ROOT) /
                      "retained-package"),
        bundle(io_provider
                   ? deployment::test::materialize_boolean_io_retained_fixture(retained_root).bundle
                   : nullptr),
        bundle_source(retained_root) {
    if (!io_provider) {
      const auto manifest = deployment::parse_strict_json_model_yaml(ReadFile(
          canonical_empty
              ? retained_root / "canonical-empty-admission.json"
              : std::filesystem::path(MANYFORGE_I6_NATIVE_FIXTURE_ROOT) /
                    "manifest.json"));
      bundle = bundle_source.get_verified(core::RetainedBundleId(
          TextMember(manifest.root(), "retained_bundle_id")));
    }
    if (bundle == nullptr) {
      throw std::runtime_error("route-fence owner retained bundle is invalid");
    }
    component_manager::SqliteActiveBundlePublicationStore store(
        state_root.path() /
            std::string(
                component_manager::kActiveBundlePublicationStoreFilename),
        bundle->value().cell_graph.cell_id);
    const auto publication = store.PublishActiveBundle(
        {core::BootstrapPublicationTransactionDTO{
             core::BootstrapId("route-fence-owner-bootstrap"),
             core::BootstrapGeneration(1U)},
         core::ProjectionRevisionDTO{
             bundle->retained_bundle_id(),
             bundle->value().cell_graph.graph_revision,
             core::SceneRevision{core::SceneId(std::string(kSha)), 1U,
                                 core::ContentDigest(std::string(kSha))}},
         bundle->retained_bundle_id(),
         bundle->value().cell_graph.graph_revision, std::nullopt,
         core::ContentDigest(std::string(kSha))});
    if (!publication.valid()) {
      throw std::runtime_error(
          "route-fence owner active-bundle publication is invalid");
    }
    store.Close();
  }

  [[nodiscard]] ManagedKernelApplyFenceOwnerConfig
  Config(const std::string_view epoch_suffix) {
    const auto suffix = std::string(epoch_suffix);
    return {state_root.path() / "kernel-configuration-fence.sqlite",
            state_root.path(),
            retained_root,
            bundle->value().cell_graph.cell_id,
            core::RuntimeUnitId("runtime.kernel.route-fence-test"),
            core::RuntimeEpoch("runtime-epoch.route-fence-test." + suffix),
            core::RequestOwnerEpoch("kernel-owner.route-fence-test." + suffix),
            [this] { return now_ns++; }};
  }

  ScopedTemporaryRoot state_root;
  std::filesystem::path retained_root;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source;
  std::int64_t now_ns{1000};
};

[[nodiscard]] core::ConfigurationFenceScopeDTO AffectedScope() {
  return core::ConfigurationFenceScopeDTO{
      {core::ControllableComponentId("robot.1")},
      {core::ConfigurationFenceDomainScopeDTO{
          core::ControlDomainId("domain.arm"),
          core::ControlDomainRevision("domain.arm.v1"),
          core::ControlDomainRevision("domain.arm.v2")}},
      {core::ConfigurationFenceBindingScopeDTO{
          core::ControlBindingId("binding.arm"),
          core::ControlBindingRevision("binding.arm.v1"),
          core::ControlBindingRevision("binding.arm.v2")}},
      {core::ResourceId("joint.1"), core::ResourceId("joint.2")}};
}

[[nodiscard]] core::ConfigurationFenceScopeDTO DisjointScope() {
  return core::ConfigurationFenceScopeDTO{
      {core::ControllableComponentId("robot.2")},
      {core::ConfigurationFenceDomainScopeDTO{
          core::ControlDomainId("domain.other"),
          core::ControlDomainRevision("domain.other.v1"),
          core::ControlDomainRevision("domain.other.v2")}},
      {core::ConfigurationFenceBindingScopeDTO{
          core::ControlBindingId("binding.other"),
          core::ControlBindingRevision("binding.other.v1"),
          core::ControlBindingRevision("binding.other.v2")}},
      {core::ResourceId("joint.3"), core::ResourceId("joint.4")}};
}

[[nodiscard]] std::vector<core::DomainActiveControlConfigurationDTO>
ActiveConfigurations() {
  return {core::DomainActiveControlConfigurationDTO{
      core::ControlDomainId("domain.arm"),
      core::ActiveControlConfigurationId("active.arm.v1"),
      core::ActiveControlConfigurationGeneration(1U)}};
}

[[nodiscard]] std::vector<core::DomainActiveControlConfigurationDTO>
TwoActiveConfigurations() {
  return {core::DomainActiveControlConfigurationDTO{
              core::ControlDomainId("domain.arm"),
              core::ActiveControlConfigurationId("active.arm.v1"),
              core::ActiveControlConfigurationGeneration(1U)},
          core::DomainActiveControlConfigurationDTO{
              core::ControlDomainId("domain.other"),
              core::ActiveControlConfigurationId("active.other.v1"),
              core::ActiveControlConfigurationGeneration(1U)}};
}

[[nodiscard]] core::ResourceSlotOwnerDTO OperationOwner() {
  return core::ResourceSlotOwnerDTO{
      core::OperationId("operation.1"),
      core::OperationRevision(1U),
      core::OperationParticipantId("participant.1"),
      core::ControlDomainId("domain.arm"),
      core::ControlDomainRevision("domain.arm.v1"),
      core::ControlBindingId("binding.arm"),
      core::ControlBindingRevision("binding.arm.v1"),
      core::ControllerTopologyId("topology.native"),
      core::TopologyRevision("topology.native.v1"),
      core::ControlRouteId("route.native"),
      core::ActiveControlConfigurationId("active.arm.v1"),
      core::ActiveControlConfigurationGeneration(1U),
      core::ContainmentGranularity::kResource};
}

[[nodiscard]] core::ExpectedActiveControlConfigurationDTO ExpectedActive() {
  return {core::ExpectedActiveControlConfigurationPresenceDTO::kPresent,
          core::ActiveControlConfigurationId("active.arm.v1"),
          core::ActiveControlConfigurationGeneration(1U)};
}

[[nodiscard]] core::RouteConfigurationFenceAdmissionQueryDTO
RouteQuery(const OwnerFixture &fixture) {
  return {core::RouteSelectionTransactionId("route.transaction.1"),
          core::RouteSelectionTransactionGeneration(1U),
          AffectedScope(),
          fixture.bundle->retained_bundle_id(),
          fixture.bundle->value().cell_graph.graph_revision,
          ExpectedActive()};
}

[[nodiscard]] core::RouteConfigurationFenceIntentDTO
RouteIntent(const core::RouteConfigurationFenceAdmissionSnapshotDTO &snapshot) {
  return {
      {core::RequestId("request.route-fence.1"),
       snapshot.execution_host_epoch.request_owner_epoch, "cclc", std::nullopt},
      core::RequestId("request.public-route.1"),
      snapshot.query.route_selection_transaction_id,
      snapshot.query.route_selection_transaction_generation,
      snapshot.query.scope,
      snapshot.snapshot_digest,
      core::RouteConfigurationFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      snapshot.query.expected_retained_bundle_id,
      snapshot.query.expected_graph_revision,
      snapshot.operation_resource_generation,
      snapshot.configuration_fence_generation,
      snapshot.current_active_control_configuration,
      core::ActiveControlConfigurationId("active.arm.v2"),
      core::ActiveControlConfigurationGeneration(2U),
      "select the complete desired route"};
}

[[nodiscard]] core::RouteConfigurationFenceStatusQueryDTO
RouteStatusQuery(const core::RouteConfigurationFenceAcceptedDTO &accepted) {
  return {accepted.route_selection_transaction_id,
          accepted.route_selection_transaction_generation,
          accepted.configuration_fence_id,
          accepted.configuration_fence_generation, accepted.captured_scope};
}

[[nodiscard]] core::RouteConfigurationFenceReleaseRequestDTO
ReleaseRequest(const core::RouteConfigurationFenceAcceptedDTO &accepted,
               const core::RouteConfigurationFenceStatusDTO &held) {
  return {{core::RequestId("request.route-fence.release.1"),
           held.evidence.execution_host_epoch.request_owner_epoch, "cclc",
           std::nullopt},
          core::RequestId("request.public-route.1"),
          accepted.route_selection_transaction_id,
          accepted.route_selection_transaction_generation,
          accepted,
          4U,
          core::ContentDigest(std::string(kSha)),
          held.evidence.current_active_control_configuration,
          held.evidence};
}

TEST(ManagedKernelAdmissionGate,
     TerminalApplyOverlapPreservesBothBarriersAndRejectsAnActiveEffect) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  const core::ConfigurationFenceId old_id("fence.old-terminal");
  const core::ConfigurationFenceId fresh_id("fence.fresh");
  gate.RestoreFence(old_id, scope);
  const auto observed = gate.Observe(scope);
  unsigned validations = 0U;
  bool committed = false;
  const auto validate = [&](const auto &id, const auto &exact_scope) {
    ++validations;
    EXPECT_EQ(id, old_id);
    EXPECT_EQ(exact_scope, scope);
  };
  EXPECT_THROW(gate.AcquireFenceDurably(
                   fresh_id, scope, observed.operation_resource_generation,
                   ActiveConfigurations(), [&] { committed = true; }),
               std::runtime_error);
  EXPECT_THROW(gate.AcquireFenceDurably(
                   fresh_id, scope, observed.operation_resource_generation,
                   ActiveConfigurations(), [&] { committed = true; },
                   [](const auto &, const auto &) {
                     throw std::runtime_error(
                         "original Apply is live or unknown");
                   }),
               std::runtime_error);
  gate.RunWithExactHeldFence(old_id, scope, [&] {
    EXPECT_THROW(gate.AcquireFenceDurably(
                     fresh_id, scope, observed.operation_resource_generation,
                     ActiveConfigurations(), [&] { committed = true; },
                     validate),
                 std::runtime_error);
  });
  EXPECT_EQ(validations, 0U);
  EXPECT_FALSE(committed);
  gate.AcquireFenceDurably(
      fresh_id, scope, observed.operation_resource_generation,
      ActiveConfigurations(), [&] { committed = true; }, validate);
  EXPECT_EQ(validations, 1U);
  EXPECT_TRUE(committed);
  EXPECT_TRUE(gate.exact_fence_is_held(old_id, scope));
  EXPECT_TRUE(gate.exact_fence_is_held(fresh_id, scope));
  EXPECT_FALSE(gate.reserve(*table, OperationOwner(), scope.resources).ok());
  gate.ReleaseFence(fresh_id, scope);
  EXPECT_FALSE(gate.reserve(*table, OperationOwner(), scope.resources).ok());
  gate.ReleaseFence(old_id, scope);
  EXPECT_TRUE(gate.reserve(*table, OperationOwner(), scope.resources).ok());
}

TEST(ManagedKernelAdmissionGate,
     SerializesOperationFirstAndFenceFirstOnTheActualSlotTable) {
  ManagedKernelAdmissionGate gate;
  auto table = std::make_shared<core::ResourceSlotTable>(
      std::vector<core::ResourceId>{core::ResourceId("joint.1"),
                                    core::ResourceId("joint.2")},
      ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  const auto scope = AffectedScope();
  const auto owner = OperationOwner();

  ASSERT_TRUE(gate.reserve(*table, owner, scope.resources).ok());
  const auto operation_first = gate.Observe(scope);
  EXPECT_EQ(operation_first.operation_resource_generation, 1U);
  EXPECT_EQ(operation_first.affected_queued_operation_count, 1U);
  EXPECT_EQ(operation_first.affected_active_operation_count, 0U);
  bool rejected_commit_called = false;
  EXPECT_THROW(gate.AcquireFenceDurably(
                   core::ConfigurationFenceId("fence.rejected"), scope,
                   operation_first.operation_resource_generation,
                   ActiveConfigurations(),
                   [&] { rejected_commit_called = true; }),
               std::runtime_error);
  EXPECT_FALSE(rejected_commit_called);

  ASSERT_TRUE(table->release(owner, scope.resources).ok());
  const auto idle = gate.Observe(scope);
  EXPECT_EQ(idle.operation_resource_generation, 2U);
  EXPECT_EQ(idle.affected_queued_operation_count, 0U);
  EXPECT_EQ(idle.affected_active_operation_count, 0U);
  bool durable_commit_called = false;
  gate.AcquireFenceDurably(core::ConfigurationFenceId("fence.accepted"), scope,
                           idle.operation_resource_generation,
                           ActiveConfigurations(),
                           [&] { durable_commit_called = true; });
  EXPECT_TRUE(durable_commit_called);

  const auto fenced_reservation = gate.reserve(*table, owner, scope.resources);
  ASSERT_FALSE(fenced_reservation.ok());
  EXPECT_EQ(fenced_reservation.error().details.at("reason"),
            "configuration_fence_held");
  const auto still_idle = gate.Observe(scope);
  EXPECT_EQ(still_idle.operation_resource_generation, 2U);
  EXPECT_EQ(still_idle.affected_queued_operation_count, 0U);

  gate.ReleaseFence(core::ConfigurationFenceId("fence.accepted"), scope);
  ASSERT_TRUE(gate.reserve(*table, owner, scope.resources).ok());
  EXPECT_EQ(gate.Observe(scope).affected_queued_operation_count, 1U);
}

TEST(ManagedKernelAdmissionGate,
     BatchReservationRejectsOneFencedParticipantWithoutReservingAnother) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  auto first = OperationOwner();
  first.participant_id = core::OperationParticipantId("participant.1");
  auto second = first;
  second.participant_id = core::OperationParticipantId("participant.2");

  const core::ConfigurationFenceScopeDTO second_scope{
      scope.controllable_component_ids,
      scope.domains,
      scope.bindings,
      {core::ResourceId("joint.2")}};
  gate.RestoreFence(core::ConfigurationFenceId("fence.participant.2"),
                    second_scope);
  const std::vector<core::ResourceSlotReservationOwnerDTO> owners{
      {first, {core::ResourceId("joint.1")}},
      {second, {core::ResourceId("joint.2")}}};
  const auto blocked = gate.reserve_batch(*table, owners);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "configuration_fence_held");
  EXPECT_EQ(table->coherent_snapshot().operation_resource_generation, 0U);
  EXPECT_TRUE(table->active_operation_ids().empty());

  gate.ReleaseFence(core::ConfigurationFenceId("fence.participant.2"),
                    second_scope);
  ASSERT_TRUE(gate.reserve_batch(*table, owners).ok());
  EXPECT_EQ(table->coherent_snapshot().operation_resource_generation, 1U);
  EXPECT_EQ(table->active_operation_ids(),
            std::vector<core::OperationId>{core::OperationId("operation.1")});
}

TEST(ManagedKernelAdmissionGate,
     AllowsDisjointFencesButRejectsAnOverlappingFence) {
  ManagedKernelAdmissionGate gate;
  const auto first = AffectedScope();
  const auto second = DisjointScope();
  auto resources = first.resources;
  resources.insert(resources.end(), second.resources.begin(),
                   second.resources.end());
  auto table = std::make_shared<core::ResourceSlotTable>(
      std::move(resources), TwoActiveConfigurations(),
      gate.shared_admission_mutex());
  gate.BindResourceTable(table);

  const auto observation = gate.Observe(first);
  bool first_committed = false;
  gate.AcquireFenceDurably(core::ConfigurationFenceId("fence.first"), first,
                           observation.operation_resource_generation,
                           ActiveConfigurations(),
                           [&] { first_committed = true; });
  EXPECT_TRUE(first_committed);

  const auto second_observation = gate.Observe(second);
  bool second_committed = false;
  gate.AcquireFenceDurably(core::ConfigurationFenceId("fence.second"), second,
                           second_observation.operation_resource_generation,
                           {TwoActiveConfigurations().back()},
                           [&] { second_committed = true; });
  EXPECT_TRUE(second_committed);
  EXPECT_EQ(gate.current_fenced_resources(
                {core::ResourceId("joint.1"), core::ResourceId("joint.3")}),
            (std::vector<core::ResourceId>{core::ResourceId("joint.1"),
                                           core::ResourceId("joint.3")}));

  bool overlapping_commit = false;
  EXPECT_THROW(gate.AcquireFenceDurably(
                   core::ConfigurationFenceId("fence.overlap"), first,
                   observation.operation_resource_generation,
                   ActiveConfigurations(), [&] { overlapping_commit = true; }),
               std::runtime_error);
  EXPECT_FALSE(overlapping_commit);

  gate.ReleaseFence(core::ConfigurationFenceId("fence.first"), first);
  gate.ReleaseFence(core::ConfigurationFenceId("fence.second"), second);
}

TEST(ManagedKernelAdmissionGate,
     RouteFenceTreatsAnApplyStagedConfigurationAsNoPublishedPredecessor) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, std::vector<core::DomainActiveControlConfigurationDTO>{},
      gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  const auto staged = table->reconfigure_runtime_inventory(
      scope.resources, ActiveConfigurations(), {});
  ASSERT_TRUE(staged.ok()) << staged.error().message;
  const auto observation = gate.Observe(scope);
  EXPECT_EQ(observation.active_control_configurations, ActiveConfigurations());
  EXPECT_TRUE(
      observation.route_published_active_control_configurations.empty());
  const core::ExpectedActiveControlConfigurationDTO absent{
      core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
      std::nullopt, std::nullopt};
  bool committed = false;
  gate.AcquireRouteFenceDurably(
      core::ConfigurationFenceId("fence.route.absent"), scope,
      observation.operation_resource_generation, absent,
      [&] { committed = true; });
  EXPECT_TRUE(committed);
  gate.ReleaseFence(core::ConfigurationFenceId("fence.route.absent"), scope);
}

TEST(ManagedKernelRouteFenceOwner,
     ObservesAndAcquiresAbsentPredecessorOverApplyStagedInventory) {
  OwnerFixture fixture;
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, std::vector<core::DomainActiveControlConfigurationDTO>{},
      gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  ASSERT_TRUE(table
                  ->reconfigure_runtime_inventory(scope.resources,
                                                  ActiveConfigurations(), {})
                  .ok());
  ManagedKernelApplyFenceOwner owner(fixture.Config("staged-route"), gate);

  auto query = RouteQuery(fixture);
  query.expected_active_control_configuration = {
      core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
      std::nullopt, std::nullopt};
  const auto snapshot = owner.ObserveRouteConfigurationFenceAdmission(query);
  ASSERT_TRUE(snapshot.ok()) << snapshot.error().message;
  EXPECT_EQ(snapshot.value().current_active_control_configuration,
            query.expected_active_control_configuration);

  const auto intent = RouteIntent(snapshot.value());
  const auto accepted = owner.AcquireRouteConfigurationFence(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto held = owner.ObserveRouteConfigurationFenceStatus(
      RouteStatusQuery(accepted.value()));
  ASSERT_TRUE(held.ok()) << held.error().message;
  EXPECT_EQ(held.value().evidence.current_active_control_configuration,
            query.expected_active_control_configuration);
}

TEST(ManagedKernelRouteFenceOwner,
     ResolvesActivePredecessorFromCandidatePackageDuringManagedApply) {
  OwnerFixture fixture;
  ScopedTemporaryRoot package_roots;
  const auto trusted_root = package_roots.path() / "trusted";
  const auto candidate_root = package_roots.path() / "candidate";
  CopyDirectory(fixture.retained_root, trusted_root);
  CopyDirectory(fixture.retained_root, candidate_root);

  // During a managed Apply the child is started from the activation root
  // before the durable active-publication pointer is advanced.  The trusted
  // package therefore intentionally lacks the predecessor bundle; the
  // activation root still contains it and must be accepted as the bounded
  // exact-identity fallback.
  const auto predecessor_path =
      trusted_root / "retained-bundles" / "sha256" /
      (fixture.bundle->retained_bundle_id().value().substr(7U) + ".json");
  ASSERT_TRUE(std::filesystem::remove(predecessor_path));
  ASSERT_FALSE(std::filesystem::exists(predecessor_path));
  ASSERT_TRUE(std::filesystem::exists(
      candidate_root / "retained-bundles" / "sha256" /
      (fixture.bundle->retained_bundle_id().value().substr(7U) + ".json")));

  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, std::vector<core::DomainActiveControlConfigurationDTO>{},
      gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  ASSERT_TRUE(table
                  ->reconfigure_runtime_inventory(scope.resources,
                                                  ActiveConfigurations(), {})
                  .ok());

  auto config = fixture.Config("candidate-fallback");
  config.retained_package_root = trusted_root;
  config.candidate_retained_package_root = candidate_root;
  ManagedKernelApplyFenceOwner owner(config, gate);

  auto query = RouteQuery(fixture);
  query.expected_active_control_configuration = {
      core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
      std::nullopt, std::nullopt};
  const auto observed = owner.ObserveRouteConfigurationFenceAdmission(query);
  ASSERT_TRUE(observed.ok()) << observed.error().message;
  EXPECT_EQ(observed.value().query.expected_retained_bundle_id,
            fixture.bundle->retained_bundle_id());
  EXPECT_EQ(observed.value().query.expected_graph_revision,
            fixture.bundle->value().cell_graph.graph_revision);
}

TEST(ManagedKernelRouteFenceRecordCodec,
     ExactIntentRoundTripsAndRejectsAnotherRecordKind) {
  const core::ExpectedActiveControlConfigurationDTO expected_active{
      core::ExpectedActiveControlConfigurationPresenceDTO::kPresent,
      std::optional<core::ActiveControlConfigurationId>(
          core::ActiveControlConfigurationId("active.arm.v1")),
      std::optional<core::ActiveControlConfigurationGeneration>(
          core::ActiveControlConfigurationGeneration(1U))};
  const core::RouteConfigurationFenceIntentDTO intent{
      {core::RequestId("request.route.fence"),
       core::RequestOwnerEpoch("kernel-owner.1"), "test", std::nullopt},
      core::RequestId("request.public.route"),
      core::RouteSelectionTransactionId("route.transaction.1"),
      core::RouteSelectionTransactionGeneration(1U),
      AffectedScope(),
      core::ContentDigest("snapshot.digest"),
      core::RouteConfigurationFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      core::RetainedBundleId("bundle.1"),
      core::GraphRevision("graph.1"),
      0U,
      core::ConfigurationFenceGeneration(0U),
      expected_active,
      core::ActiveControlConfigurationId("active.arm.v2"),
      core::ActiveControlConfigurationGeneration(2U),
      "change the complete controller route"};
  ASSERT_TRUE(intent.locally_valid());
  const auto bytes = core::detail::EncodeRouteFenceIntentRecord(intent);
  EXPECT_EQ(core::detail::DecodeRouteFenceIntentRecord(bytes), intent);
  EXPECT_THROW(core::detail::DecodeRouteFenceAcceptedRecord(bytes),
               std::invalid_argument);
}

TEST(ManagedKernelRouteFenceOwner,
     DurableAcquireReconcileRestartReleaseAndReplayShareOneLedger) {
  OwnerFixture fixture;
  std::optional<core::RouteConfigurationFenceIntentDTO> intent;
  std::optional<core::RouteConfigurationFenceAcceptedDTO> accepted;
  std::optional<core::RouteConfigurationFenceReleaseRequestDTO> release_request;
  {
    ManagedKernelAdmissionGate gate;
    auto table = std::make_shared<core::ResourceSlotTable>(
        AffectedScope().resources, ActiveConfigurations(),
        gate.shared_admission_mutex());
    gate.BindResourceTable(table);
    ManagedKernelApplyFenceOwner owner(fixture.Config("1"), gate);

    const auto snapshot_result =
        owner.ObserveRouteConfigurationFenceAdmission(RouteQuery(fixture));
    ASSERT_TRUE(snapshot_result.ok()) << snapshot_result.error().message;
    intent = RouteIntent(snapshot_result.value());
    ASSERT_TRUE(intent->matches(snapshot_result.value()));

    auto not_seen =
        owner.ReconcileRouteConfigurationFenceAcquireAttempt(*intent);
    ASSERT_TRUE(not_seen.ok()) << not_seen.error().message;
    EXPECT_EQ(
        not_seen.value().disposition,
        core::RouteConfigurationFenceAcquireAttemptDispositionDTO::kNotSeen);

    const auto accepted_result = owner.AcquireRouteConfigurationFence(*intent);
    ASSERT_TRUE(accepted_result.ok()) << accepted_result.error().message;
    accepted = accepted_result.value();
    EXPECT_EQ(accepted->replay_status,
              core::RequestReplayStatusDTO::kAcceptedNew);
    EXPECT_TRUE(accepted->matches(*intent));

    const auto replay = owner.AcquireRouteConfigurationFence(*intent);
    ASSERT_TRUE(replay.ok()) << replay.error().message;
    EXPECT_EQ(replay.value().configuration_fence_id,
              accepted->configuration_fence_id);
    EXPECT_EQ(replay.value().replay_status,
              core::RequestReplayStatusDTO::kReplayedExact);

    auto mixed = *intent;
    mixed.reason = "conflicting exact request";
    const auto mixed_result = owner.AcquireRouteConfigurationFence(mixed);
    EXPECT_FALSE(mixed_result.ok());

    const auto held_result =
        owner.ObserveRouteConfigurationFenceStatus(RouteStatusQuery(*accepted));
    ASSERT_TRUE(held_result.ok()) << held_result.error().message;
    ASSERT_EQ(held_result.value().state,
              core::RouteConfigurationFenceStateDTO::kHeld);
    EXPECT_TRUE(held_result.value().admission_fenced);
    release_request = ReleaseRequest(*accepted, held_result.value());

    const auto acquire_reconciliation =
        owner.ReconcileRouteConfigurationFenceAcquireAttempt(*intent);
    ASSERT_TRUE(acquire_reconciliation.ok())
        << acquire_reconciliation.error().message;
    EXPECT_EQ(acquire_reconciliation.value().disposition,
              core::RouteConfigurationFenceAcquireAttemptDispositionDTO::
                  kAcceptedExact);

    const auto release_reconciliation =
        owner.ReconcileRouteConfigurationFenceReleaseAttempt(*release_request);
    ASSERT_TRUE(release_reconciliation.ok())
        << release_reconciliation.error().message;
    EXPECT_EQ(release_reconciliation.value().disposition,
              core::RouteConfigurationFenceReleaseAttemptDispositionDTO::
                  kNotReleasedHeld);

    const auto blocked =
        gate.reserve(*table, OperationOwner(), AffectedScope().resources);
    ASSERT_FALSE(blocked.ok());
    EXPECT_EQ(blocked.error().details.at("reason"), "configuration_fence_held");
  }

  ASSERT_TRUE(intent.has_value());
  ASSERT_TRUE(accepted.has_value());
  ASSERT_TRUE(release_request.has_value());
  {
    ManagedKernelAdmissionGate gate;
    auto table = std::make_shared<core::ResourceSlotTable>(
        AffectedScope().resources, ActiveConfigurations(),
        gate.shared_admission_mutex());
    gate.BindResourceTable(table);
    ManagedKernelApplyFenceOwner owner(fixture.Config("2"), gate);

    const auto acquire_reconciliation =
        owner.ReconcileRouteConfigurationFenceAcquireAttempt(*intent);
    ASSERT_TRUE(acquire_reconciliation.ok())
        << acquire_reconciliation.error().message;
    EXPECT_EQ(acquire_reconciliation.value().disposition,
              core::RouteConfigurationFenceAcquireAttemptDispositionDTO::
                  kAcceptedExact);
    ASSERT_TRUE(acquire_reconciliation.value().current_status.has_value());
    EXPECT_EQ(
        acquire_reconciliation.value()
            .current_status->evidence.execution_host_epoch.request_owner_epoch,
        core::RequestOwnerEpoch("kernel-owner.route-fence-test.2"));

    const auto blocked =
        gate.reserve(*table, OperationOwner(), AffectedScope().resources);
    ASSERT_FALSE(blocked.ok());
    EXPECT_EQ(blocked.error().details.at("reason"), "configuration_fence_held");

    const auto held_result =
        owner.ObserveRouteConfigurationFenceStatus(RouteStatusQuery(*accepted));
    ASSERT_TRUE(held_result.ok()) << held_result.error().message;
    ASSERT_EQ(held_result.value().state,
              core::RouteConfigurationFenceStateDTO::kHeld);
    *release_request = ReleaseRequest(*accepted, held_result.value());
    const auto released =
        owner.ReleaseRouteConfigurationFence(*release_request);
    ASSERT_TRUE(released.ok()) << released.error().message;
    EXPECT_EQ(released.value().configuration_fence_id,
              accepted->configuration_fence_id);

    const auto reconciled =
        owner.ReconcileRouteConfigurationFenceReleaseAttempt(*release_request);
    ASSERT_TRUE(reconciled.ok()) << reconciled.error().message;
    EXPECT_EQ(reconciled.value().disposition,
              core::RouteConfigurationFenceReleaseAttemptDispositionDTO::
                  kReleasedExact);

    const auto replayed =
        owner.ReleaseRouteConfigurationFence(*release_request);
    ASSERT_TRUE(replayed.ok()) << replayed.error().message;
    EXPECT_EQ(replayed.value().replay_status,
              core::RequestReplayStatusDTO::kReplayedExact);
  }

  {
    ManagedKernelAdmissionGate gate;
    auto table = std::make_shared<core::ResourceSlotTable>(
        AffectedScope().resources, ActiveConfigurations(),
        gate.shared_admission_mutex());
    gate.BindResourceTable(table);
    ManagedKernelApplyFenceOwner owner(fixture.Config("3"), gate);
    const auto status =
        owner.ObserveRouteConfigurationFenceStatus(RouteStatusQuery(*accepted));
    ASSERT_TRUE(status.ok()) << status.error().message;
    EXPECT_EQ(status.value().state,
              core::RouteConfigurationFenceStateDTO::kReleased);
    EXPECT_TRUE(
        gate.reserve(*table, OperationOwner(), AffectedScope().resources).ok());
  }
}

TEST(ManagedKernelAdmissionGate,
     RestoredFenceBlocksAResourceTableAttachedAfterRestart) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  gate.RestoreFence(core::ConfigurationFenceId("fence.restored"), scope);
  auto table = std::make_shared<core::ResourceSlotTable>(
      std::vector<core::ResourceId>{core::ResourceId("joint.1"),
                                    core::ResourceId("joint.2")},
      ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);

  const auto blocked = gate.reserve(*table, OperationOwner(), scope.resources);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().details.at("reason"), "configuration_fence_held");
  EXPECT_EQ(gate.Observe(scope).operation_resource_generation, 0U);
}

TEST(ManagedKernelAdmissionGate,
     RuntimeCompositionPinsFenceWithoutHoldingResourceAdmissionMutex) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  const core::ConfigurationFenceId fence_id("fence.composition-pinned");
  gate.RestoreFence(fence_id, scope);

  std::promise<void> effect_entered;
  std::promise<void> allow_effect_to_finish;
  auto allow_effect = allow_effect_to_finish.get_future();
  std::thread composition([&] {
    gate.RunWithExactHeldFence(fence_id, scope, [&] {
      effect_entered.set_value();
      allow_effect.wait();
    });
  });
  effect_entered.get_future().wait();

  auto containment_admission = std::async(std::launch::async, [&] {
    std::lock_guard lock(*gate.shared_admission_mutex());
  });
  EXPECT_EQ(containment_admission.wait_for(std::chrono::milliseconds(100)),
            std::future_status::ready);
  EXPECT_THROW(gate.ReleaseFence(fence_id, scope), std::runtime_error);

  allow_effect_to_finish.set_value();
  composition.join();
  gate.ReleaseFence(fence_id, scope);
}

TEST(ManagedKernelAdmissionGate,
     UnreconciledOrphanWorkRejectsFenceAcquisitionFailClosed) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  ASSERT_TRUE(
      table->mark_unreconciled_external_or_orphan_goal(scope.resources).ok());
  const auto observation = gate.Observe(scope);
  EXPECT_EQ(observation.operation_resource_generation, 1U);
  EXPECT_EQ(observation.affected_queued_operation_count, 0U);
  EXPECT_EQ(observation.affected_active_operation_count, 1U);
  bool commit_called = false;
  EXPECT_THROW(gate.AcquireFenceDurably(
                   core::ConfigurationFenceId("fence.orphan-rejected"), scope,
                   observation.operation_resource_generation,
                   ActiveConfigurations(), [&] { commit_called = true; }),
               std::runtime_error);
  EXPECT_FALSE(commit_called);
}

[[nodiscard]] core::SelectedBringUpConfigurationFenceIntentDTO
SelectedIntent(const core::ConfigurationFenceAdmissionSnapshotDTO &snapshot) {
  const core::RequestOwnerEpoch epoch("cclc.selected.owner");
  const core::RequestId request("request.selected.1");
  const std::string transaction("selected-lifecycle.1");
  core::CellAdmissionScopeDTO claim_scope{
      snapshot.echoed_query.affected_scope.resources, {}};
  for (const auto &active : snapshot.active_control_configurations) {
    claim_scope.configurations.push_back(
        {active.control_domain_id, active.active_control_configuration_id,
         active.active_control_configuration_generation});
  }
  core::CellAdmissionClaimIntentDTO claim_intent{
      core::CellAdmissionClaimKindDTO::kInterventionOrRecovery,
      core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
      epoch,
      transaction,
      request,
      core::ContentDigest(std::string(kSha)),
      core::AdmissionGeneration(1U),
      claim_scope,
      std::nullopt,
      core::CellAdmissionInterventionEvidenceDTO{
          core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner, epoch,
          transaction, core::ContentDigest(std::string(kSha)), std::nullopt}};
  return {{request, epoch, "selected-test", std::nullopt},
          core::LifecycleTransactionId(transaction),
          core::LifecycleTransactionGeneration(1U),
          {claim_intent, core::CellAdmissionClaimGeneration(1U),
           core::AdmissionGeneration(2U), 100},
          snapshot,
          snapshot.execution_host_epoch.publication_generation,
          snapshot.echoed_query.affected_scope,
          snapshot.active_projection_revision};
}

[[nodiscard]] core::Result<core::CellAdmissionRecoveryReceiptDTO>
HeldSelectedReceipt(const core::CellAdmissionRecoveryReceiptQueryDTO &query) {
  return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Ok(
      {core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact,
       core::RequestOwnerEpoch("kernel.selected.owner"),
       core::AdmissionGeneration(2U),
       query.expected_claim,
       std::nullopt,
       std::nullopt,
       {*query.expected_claim},
       {{query.intent.owner_role, query.intent.owner_epoch}}});
}

TEST(ManagedKernelSelectedBringUpFenceOwner,
     MissingOrNonHeldClaimRefusesBeforeEffects) {
  OwnerFixture fixture;
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  ManagedKernelApplyFenceOwner owner(fixture.Config("missing-selected-claim"),
                                     gate);
  const auto snapshot =
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.selected")});
  const auto intent = SelectedIntent(snapshot);
  ASSERT_TRUE(intent.valid());
  EXPECT_FALSE(owner.AcquireSelectedBringUpFence(intent).ok());
  EXPECT_TRUE(gate.current_fenced_resources(scope.resources).empty());
  unsigned effects = 0U;
  const core::SelectedBringUpConfigurationFenceStatusQueryDTO unknown{
      intent.owner(), core::ConfigurationFenceId("missing.selected.fence"),
      core::ConfigurationFenceGeneration(1U), scope,
      snapshot.echoed_query.manager_runtime_epoch};
  EXPECT_THROW(
      owner.RunWithExactHeldSelectedPredecessor(unknown, [&] { ++effects; }),
      std::runtime_error);
  EXPECT_EQ(effects, 0U);
}

TEST(ManagedKernelSelectedBringUpFenceOwner,
     ExactReplayClaimLossAndReleaseReconcileWithoutRepeatingEffects) {
  OwnerFixture fixture;
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  bool claim_held = true;
  auto config = fixture.Config("selected-full-claim");
  config.selected_claim_recovery_receipt = [&](const auto &query) {
    if (claim_held)
      return HeldSelectedReceipt(query);
    return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Ok(
        {core::CellAdmissionRecoveryReceiptStatusDTO::kUnknown,
         core::RequestOwnerEpoch("kernel.selected.owner"),
         core::AdmissionGeneration(3U),
         std::nullopt,
         std::nullopt,
         std::nullopt,
         {},
         {}});
  };
  ManagedKernelApplyFenceOwner owner(config, gate);
  const auto snapshot =
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.selected")});
  const auto intent = SelectedIntent(snapshot);
  ASSERT_TRUE(intent.valid());
  auto stale = intent;
  stale.expected_admission.operation_resource_generation += 1U;
  EXPECT_FALSE(owner.AcquireSelectedBringUpFence(stale).ok());
  EXPECT_TRUE(gate.current_fenced_resources(scope.resources).empty());
  auto accepted = owner.AcquireSelectedBringUpFence(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  auto replay = owner.AcquireSelectedBringUpFence(intent);
  ASSERT_TRUE(replay.ok());
  EXPECT_EQ(replay.value().configuration_fence_id,
            accepted.value().configuration_fence_id);
  EXPECT_EQ(replay.value().replay_status,
            core::RequestReplayStatusDTO::kReplayedExact);
  auto changed = intent;
  changed.candidate_projection_revision.scene_revision.revision_id += 1U;
  EXPECT_FALSE(owner.AcquireSelectedBringUpFence(changed).ok());
  const core::SelectedBringUpConfigurationFenceStatusQueryDTO query{
      intent.owner(), accepted.value().configuration_fence_id,
      accepted.value().configuration_fence_generation, scope,
      snapshot.echoed_query.manager_runtime_epoch};
  const core::SelectedBringUpFenceAcquireAttemptQueryDTO attempt{
      intent, core::ContentDigest(std::string(kSha)),
      query.manager_runtime_epoch};
  const auto reconciled =
      owner.ReconcileSelectedBringUpFenceAcquireAttempt(attempt);
  ASSERT_TRUE(reconciled.ok());
  EXPECT_EQ(reconciled.value().state,
            core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact);
  unsigned effects = 0U;
  EXPECT_NO_THROW(
      owner.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; }));
  EXPECT_EQ(effects, 1U);
  claim_held = false;
  EXPECT_THROW(
      owner.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; }),
      std::runtime_error);
  EXPECT_EQ(effects, 1U);
  const auto held = owner.ObserveSelectedBringUpFenceStatus(query);
  ASSERT_TRUE(held.ok());
  const core::SelectedBringUpConfigurationFenceReleaseDTO release{
      held.value().reconciliation_evidence, query.manager_runtime_epoch};
  EXPECT_FALSE(owner.ReleaseSelectedBringUpFence(release).ok());
  EXPECT_FALSE(gate.current_fenced_resources(scope.resources).empty());
  claim_held = true;
  const auto released = owner.ReleaseSelectedBringUpFence(release);
  ASSERT_TRUE(released.ok()) << released.error().message;
  EXPECT_TRUE(gate.current_fenced_resources(scope.resources).empty());
  const auto release_reconciled =
      owner.ReconcileSelectedBringUpFenceReleaseAttempt({release});
  ASSERT_TRUE(release_reconciled.ok());
  EXPECT_EQ(release_reconciled.value().state,
            core::SelectedBringUpFenceReleaseAttemptState::kReleasedExact);
  EXPECT_THROW(
      owner.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; }),
      std::runtime_error);
  EXPECT_EQ(effects, 1U);
}

TEST(ManagedKernelAdmissionGate,
     DurableSelectedReleaseChecksActiveEffectBeforeCommit) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  const core::ConfigurationFenceId fence("selected.effect.pin");
  gate.RestoreFence(fence, scope);
  unsigned commits = 0U;
  gate.RunWithExactHeldFence(fence, scope, [&] {
    EXPECT_THROW(gate.ReleaseFenceDurably(fence, scope, [&] { ++commits; }),
                 std::runtime_error);
  });
  EXPECT_EQ(commits, 0U);
  EXPECT_TRUE(gate.exact_fence_is_held(fence, scope));
  EXPECT_THROW(
      gate.ReleaseFenceDurably(
          fence, scope, [] { throw std::runtime_error("unknown commit"); }),
      std::runtime_error);
  EXPECT_TRUE(gate.exact_fence_is_held(fence, scope));
  gate.ReleaseFenceDurably(fence, scope, [&] { ++commits; });
  EXPECT_EQ(commits, 1U);
  EXPECT_FALSE(gate.exact_fence_is_held(fence, scope));
}

TEST(ManagedKernelSelectedBringUpFenceOwner,
     OldLedgerWithActiveFenceIsRefusedReadOnlyAndNamesFileVersionDirection) {
  OwnerFixture fixture;
  const auto config = fixture.Config("old-version");
  {
    ManagedKernelAdmissionGate gate;
    auto table = std::make_shared<core::ResourceSlotTable>(
        AffectedScope().resources, ActiveConfigurations(),
        gate.shared_admission_mutex());
    gate.BindResourceTable(table);
    ManagedKernelApplyFenceOwner owner(config, gate);
    const auto observed =
        owner.ObserveRouteConfigurationFenceAdmission(RouteQuery(fixture));
    ASSERT_TRUE(observed.ok());
    ASSERT_TRUE(
        owner.AcquireRouteConfigurationFence(RouteIntent(observed.value()))
            .ok());
  }
  sqlite3 *db = nullptr;
  ASSERT_EQ(sqlite3_open(config.ledger_path.c_str(), &db), SQLITE_OK);
  ASSERT_EQ(
      sqlite3_exec(
          db,
          "PRAGMA user_version=5; UPDATE kernel_apply_fence_meta SET "
          "schema_version=5,schema_id='manyforge.internal.kernel-configuration-"
          "fence-ledger.v5' WHERE id=1; PRAGMA wal_checkpoint(TRUNCATE);",
          nullptr, nullptr, nullptr),
      SQLITE_OK);
  ASSERT_EQ(sqlite3_close(db), SQLITE_OK);
  const auto before = ReadFile(config.ledger_path);
  ManagedKernelAdmissionGate gate;
  try {
    ManagedKernelApplyFenceOwner owner(config, gate);
    FAIL() << "old fence ledger was accepted";
  } catch (const std::runtime_error &error) {
    const std::string message(error.what());
    EXPECT_NE(message.find(config.ledger_path.string()), std::string::npos);
    EXPECT_NE(message.find("version 5"), std::string::npos);
    EXPECT_NE(message.find("version 6"), std::string::npos);
    EXPECT_NE(message.find("preserve the file"), std::string::npos);
    EXPECT_NE(message.find("reconcile any retained fences"), std::string::npos);
  }
  EXPECT_EQ(ReadFile(config.ledger_path), before);
  EXPECT_TRUE(gate.current_fenced_resources(AffectedScope().resources).empty());
}

TEST(ManagedKernelSelectedBringUpFenceOwner,
     RestartRestoresExclusionAndReceiptWithoutAuthorizingOldOccurrenceEffects) {
  OwnerFixture fixture;
  std::optional<core::SelectedBringUpConfigurationFenceIntentDTO> intent;
  std::optional<core::ConfigurationFenceAcceptedDTO> accepted;
  const auto scope = AffectedScope();
  {
    ManagedKernelAdmissionGate gate;
    auto table = std::make_shared<core::ResourceSlotTable>(
        scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
    gate.BindResourceTable(table);
    auto config = fixture.Config("selected-before-restart");
    config.selected_claim_recovery_receipt = HeldSelectedReceipt;
    ManagedKernelApplyFenceOwner owner(config, gate);
    intent = SelectedIntent(owner.ObserveAdmission(
        {scope, core::RuntimeEpoch("manager.selected")}));
    const auto result = owner.AcquireSelectedBringUpFence(*intent);
    ASSERT_TRUE(result.ok()) << result.error().message;
    accepted = result.value();
  }
  ManagedKernelAdmissionGate gate;
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  auto config = fixture.Config("selected-after-restart");
  config.selected_claim_recovery_receipt = HeldSelectedReceipt;
  ManagedKernelApplyFenceOwner owner(config, gate);
  EXPECT_TRUE(
      gate.exact_fence_is_held(accepted->configuration_fence_id, scope));
  const auto result = owner.ReconcileSelectedBringUpFenceAcquireAttempt(
      {*intent, core::ContentDigest(std::string(kSha)),
       core::RuntimeEpoch("manager.selected")});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().state,
            core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact);
  unsigned effects = 0U;
  const core::SelectedBringUpConfigurationFenceStatusQueryDTO query{
      intent->owner(), accepted->configuration_fence_id,
      accepted->configuration_fence_generation, scope,
      core::RuntimeEpoch("manager.selected")};
  EXPECT_THROW(
      owner.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; }),
      std::runtime_error);
  EXPECT_EQ(effects, 0U);
  EXPECT_TRUE(
      gate.exact_fence_is_held(accepted->configuration_fence_id, scope));
}

TEST(ManagedKernelSelectedBringUpFenceOwner,
     UnreceiptedHeldGateReportsUncertainAcquisitionAndKeepsAdmissionClosed) {
  OwnerFixture fixture;
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  ManagedKernelApplyFenceOwner owner(fixture.Config("uncertain-selected"),
                                     gate);
  const auto intent = SelectedIntent(
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.selected")}));
  ASSERT_TRUE(intent.valid());
  gate.RestoreFence(core::ConfigurationFenceId("unreceipted.selected"), scope);
  const auto result = owner.ReconcileSelectedBringUpFenceAcquireAttempt(
      {intent, core::ContentDigest(std::string(kSha)),
       core::RuntimeEpoch("manager.selected")});
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().state,
            core::ApplyFenceAcquireAttemptReconcileState::kUncertain);
  EXPECT_TRUE(result.value().admission_fenced);
  EXPECT_FALSE(result.value().accepted_fence.has_value());
  EXPECT_FALSE(
      result.value().required_compensation_or_operator_direction.empty());
  EXPECT_FALSE(gate.reserve(*table, OperationOwner(), scope.resources).ok());
}

TEST(
    ManagedKernelSelectedBringUpFenceOwner,
    DurableReleaseWithRetainedGateReportsUncertaintyWithoutReopeningAdmission) {
  OwnerFixture fixture;
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  auto config = fixture.Config("release-unknown");
  config.selected_claim_recovery_receipt = HeldSelectedReceipt;
  ManagedKernelApplyFenceOwner owner(config, gate);
  const auto intent = SelectedIntent(
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.selected")}));
  const auto acquired = owner.AcquireSelectedBringUpFence(intent);
  ASSERT_TRUE(acquired.ok()) << acquired.error().message;
  const core::SelectedBringUpConfigurationFenceStatusQueryDTO query{
      intent.owner(), acquired.value().configuration_fence_id,
      acquired.value().configuration_fence_generation, scope,
      core::RuntimeEpoch("manager.selected")};
  const auto held = owner.ObserveSelectedBringUpFenceStatus(query);
  ASSERT_TRUE(held.ok());
  const core::SelectedBringUpConfigurationFenceReleaseDTO release{
      held.value().reconciliation_evidence, query.manager_runtime_epoch};
  ASSERT_TRUE(owner.ReleaseSelectedBringUpFence(release).ok());
  // The observable result of a committed release whose return was uncertain:
  // its real durable receipt exists while the conservative gate remains held.
  gate.RestoreFence(acquired.value().configuration_fence_id, scope);
  const auto status = owner.ObserveSelectedBringUpFenceStatus(query);
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(status.value().state,
            core::ConfigurationFenceState::kReleasePending);
  EXPECT_TRUE(status.value().admission_fenced);
  EXPECT_FALSE(status.value().release_receipt.has_value());
  EXPECT_FALSE(
      status.value().required_compensation_or_operator_direction.empty());
  const auto reconciled =
      owner.ReconcileSelectedBringUpFenceReleaseAttempt({release});
  ASSERT_TRUE(reconciled.ok());
  EXPECT_EQ(reconciled.value().state,
            core::SelectedBringUpFenceReleaseAttemptState::kUncertain);
  EXPECT_FALSE(reconciled.value().release_receipt.has_value());
  EXPECT_FALSE(owner.ReleaseSelectedBringUpFence(release).ok());
  EXPECT_TRUE(
      gate.exact_fence_is_held(acquired.value().configuration_fence_id, scope));
  EXPECT_FALSE(gate.reserve(*table, OperationOwner(), scope.resources).ok());
  unsigned effects = 0U;
  EXPECT_THROW(
      owner.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; }),
      std::runtime_error);
  EXPECT_EQ(effects, 0U);
}

} // namespace

TEST(ManagedKernelApplyFenceOwner,
     TerminalOverlapUsesTheExactExistingDurableApplyRow) {
  OwnerFixture fixture(true);
  ManagedKernelAdmissionGate gate;
  ManagedKernelApplyFenceOwner owner(fixture.Config("terminal-overlap"), gate);
  auto scope = AffectedScope();
  scope.domains.front().predecessor_control_domain_revision.reset();
  scope.bindings.front().predecessor_control_binding_revision.reset();
  const core::RuntimeEpoch manager_epoch("manager.terminal-overlap");
  const auto observed = owner.ObserveAdmission({scope, manager_epoch});
  const core::ApplyConfigurationFenceIntentDTO old_intent{
      {core::RequestId("old.apply.fence"),
       core::RequestOwnerEpoch("manager.owner"), "test", std::nullopt},
      {core::ApplyId("old.apply"), core::ApplyGeneration(1U)},
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      observed.operation_resource_generation,
      observed.configuration_fence_generation,
      {},
      "old Apply"};
  const auto old = owner.Acquire(old_intent);
  auto next = old_intent;
  next.owner_request_context.request_id = core::RequestId("fresh.apply.fence");
  next.owner_transaction = {core::ApplyId("fresh.apply"),
                            core::ApplyGeneration(1U)};
  next.expected_fence_generation = old.configuration_fence_generation;
  EXPECT_THROW(static_cast<void>(owner.Acquire(next)), std::runtime_error);
  EXPECT_THROW(
      static_cast<void>(owner.Acquire(
          next,
          [](const auto &, const auto &) {
            return core::Result<void>::Err(
                {"original_apply_live", "original Apply is not terminal", {}});
          })),
      std::runtime_error);
  unsigned validations = 0U;
  const auto fresh =
      owner.Acquire(next, [&](const auto &original, const auto &accepted) {
        ++validations;
        EXPECT_EQ(original, old_intent);
        EXPECT_EQ(accepted, old);
        return core::Result<void>::Ok();
      });
  ASSERT_TRUE(fresh.matches(next));
  EXPECT_EQ(validations, 1U);
  EXPECT_TRUE(gate.exact_fence_is_held(old.configuration_fence_id, scope));
  EXPECT_TRUE(gate.exact_fence_is_held(fresh.configuration_fence_id, scope));
  const auto replay = owner.Acquire(next, [&](const auto &, const auto &) {
    ++validations;
    return core::Result<void>::Err({"unexpected_revalidation",
                                    "exact replay must remain observation",
                                    {}});
  });
  EXPECT_EQ(replay.configuration_fence_id, fresh.configuration_fence_id);
  EXPECT_EQ(replay.replay_status, core::RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(validations, 1U);
}

TEST(ManagedKernelApplyFenceOwner,
     InitialComponentPreparationPinsEmptyPredecessorBeforeCandidateStaging) {
  OwnerFixture fixture(true);
  ASSERT_TRUE(
      fixture.bundle->value().cell_graph.controllable_components.empty());
  ManagedKernelAdmissionGate gate;
  auto table = std::make_shared<core::ResourceSlotTable>(
      std::vector<core::ResourceId>{},
      std::vector<core::DomainActiveControlConfigurationDTO>{},
      gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  ManagedKernelApplyFenceOwner owner(fixture.Config("initial-add"), gate);
  auto scope = AffectedScope();
  scope.domains.front().predecessor_control_domain_revision.reset();
  scope.bindings.front().predecessor_control_binding_revision.reset();
  const core::RuntimeEpoch manager_epoch("manager.initial-add");
  const auto observed = owner.ObserveAdmission({scope, manager_epoch});
  ASSERT_TRUE(observed.presence_valid());
  ASSERT_TRUE(observed.active_control_configurations.empty());
  const core::ApplyConfigurationFenceIntentDTO intent{
      {core::RequestId("request.initial-add"),
       core::RequestOwnerEpoch("manager-owner.initial-add"), "initial-add-test",
       std::nullopt},
      {core::ApplyId("apply.initial-add"), core::ApplyGeneration(1U)},
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      observed.operation_resource_generation,
      observed.configuration_fence_generation,
      {},
      "prepare a first component under the empty predecessor fence"};
  const auto accepted = owner.Acquire(intent);
  ASSERT_TRUE(accepted.matches(intent));
  const core::ConfigurationFenceStatusQueryDTO query{
      intent.owner_transaction, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, scope, manager_epoch};
  // Status requires installed candidate configurations; preparation must not.
  EXPECT_THROW(static_cast<void>(owner.QueryStatus(query)), std::runtime_error);
  unsigned effects = 0U;
  const auto prepare = [&](const auto &selected) {
    owner.RunWithExactHeldPredecessor(
        selected, observed.execution_host_epoch.publication_generation,
        observed.active_public_pointer_digest, [&] { ++effects; });
  };
  EXPECT_NO_THROW(prepare(query));
  EXPECT_EQ(effects, 1U);
  unsigned readbacks = 0U;
  const auto readback = [&] {
    owner.RunWithExactHeldPredecessorReadback(
        query, observed.execution_host_epoch.publication_generation,
        observed.active_public_pointer_digest, [&](const auto &view) {
          EXPECT_TRUE(view.status().matches(query));
          ++readbacks;
        });
  };
  EXPECT_THROW(readback(), std::runtime_error);
  EXPECT_EQ(readbacks, 0U); // No candidate facts are invented before install.
  ASSERT_TRUE(table->reconfigure_runtime_inventory(scope.resources, ActiveConfigurations(), {}).ok());
  EXPECT_NO_THROW(readback());
  EXPECT_EQ(readbacks, 1U);
  auto wrong = query;
  wrong.expected_scope.resources.pop_back();
  EXPECT_THROW(prepare(wrong), std::runtime_error);
  wrong = query;
  wrong.configuration_fence_id =
      core::ConfigurationFenceId("missing.initial-add");
  EXPECT_THROW(prepare(wrong), std::runtime_error);
  EXPECT_EQ(effects, 1U);
  gate.ReleaseFence(query.configuration_fence_id, scope);
  EXPECT_THROW(prepare(query), std::runtime_error);
  EXPECT_EQ(effects, 1U);
}

TEST(ManagedKernelApplyFenceOwner,
     InitialHeldPresenceNeedsNoCandidateConfigurationsAndReleasedRowsRefuse) {
  OwnerFixture fixture(true);
  ASSERT_TRUE(fixture.bundle->value().cell_graph.controllable_components.empty());
  ManagedKernelAdmissionGate gate;
  auto table = std::make_shared<core::ResourceSlotTable>(
      std::vector<core::ResourceId>{},
      std::vector<core::DomainActiveControlConfigurationDTO>{},
      gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  ManagedKernelApplyFenceOwner owner(fixture.Config("initial-presence"), gate);
  auto scope = AffectedScope();
  scope.domains.front().predecessor_control_domain_revision.reset();
  scope.bindings.front().predecessor_control_binding_revision.reset();
  const core::RuntimeEpoch manager_epoch("manager.initial-presence");
  const auto observed = owner.ObserveAdmission({scope, manager_epoch});
  ASSERT_TRUE(observed.presence_valid());
  ASSERT_TRUE(observed.active_control_configurations.empty());
  const core::ApplyConfigurationFenceIntentDTO intent{
      {core::RequestId("request.initial-presence"),
       core::RequestOwnerEpoch("manager-owner.initial-presence"), "initial-presence-test",
       std::nullopt},
      {core::ApplyId("apply.initial-presence"), core::ApplyGeneration(1U)}, scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(), fixture.bundle->value().cell_graph.graph_revision,
      observed.operation_resource_generation, observed.configuration_fence_generation, {},
      "read the exact held first-component fence before installation"};
  const auto accepted = owner.Acquire(intent);
  ASSERT_TRUE(accepted.matches(intent));
  const core::ApplyFenceAcquireAttemptQueryDTO attempt{
      intent, core::ContentDigest(std::string(kSha)), manager_epoch};
  const core::ConfigurationFenceStatusQueryDTO status_query{
      intent.owner_transaction, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, scope, manager_epoch};
  const auto held = owner.ReconcileAcquireAttempt(attempt);
  ASSERT_TRUE(held.matches(attempt));
  EXPECT_EQ(held.state, core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact);
  EXPECT_EQ(held.accepted_fence, std::optional(accepted));
  EXPECT_TRUE(held.admission_fenced);
  EXPECT_EQ(held.execution_host_epoch.runtime_unit_id, observed.execution_host_epoch.runtime_unit_id);
  EXPECT_EQ(held.execution_host_epoch.runtime_epoch, observed.execution_host_epoch.runtime_epoch);
  EXPECT_EQ(held.execution_host_epoch.request_owner_epoch, observed.execution_host_epoch.request_owner_epoch);
  EXPECT_EQ(held.execution_host_epoch.publication_generation,
            observed.execution_host_epoch.publication_generation);
  EXPECT_TRUE(held.execution_host_epoch.valid());
  EXPECT_EQ(held.execution_host_epoch, owner.ReconcileAcquireAttempt(attempt).execution_host_epoch);
  EXPECT_TRUE(table->admission_snapshot(scope.resources).active_control_configurations.empty());
  EXPECT_THROW(static_cast<void>(owner.QueryStatus(status_query)), std::runtime_error);

  // The read echoes the supplied attempt digest/Manager epoch; independent
  // original Manager joins remain mandatory. Only exact durable intent bytes
  // and the actual held ledger row establish this producer's presence result.
  auto foreign = attempt;
  foreign.persisted_intent.owner_transaction.apply_id = core::ApplyId("foreign.apply");
  ASSERT_TRUE(foreign.valid());
  const auto conflicting = owner.ReconcileAcquireAttempt(foreign);
  ASSERT_TRUE(conflicting.matches(foreign));
  EXPECT_EQ(conflicting.state, core::ApplyFenceAcquireAttemptReconcileState::kConflicting);
  EXPECT_FALSE(conflicting.accepted_fence);
  auto missing = attempt;
  missing.persisted_intent.owner_request_context.request_id = core::RequestId("missing.request");
  const auto absent = owner.ReconcileAcquireAttempt(missing);
  ASSERT_TRUE(absent.matches(missing));
  EXPECT_EQ(absent.state, core::ApplyFenceAcquireAttemptReconcileState::kNotSeen);
  EXPECT_FALSE(absent.accepted_fence);
  EXPECT_TRUE(gate.exact_fence_is_held(accepted.configuration_fence_id, scope));

  // Complete release reconciliation remains separate and cannot exist until
  // the actual candidate-domain configurations are installed in the table.
  ASSERT_TRUE(table->reconfigure_runtime_inventory(scope.resources, ActiveConfigurations(), {}).ok());
  const auto complete = owner.QueryStatus(status_query);
  ASSERT_TRUE(complete.matches(status_query));
  ASSERT_TRUE(complete.reconciliation_evidence.valid());
  const auto released = owner.Release({complete.reconciliation_evidence, manager_epoch});
  ASSERT_TRUE(released.valid());
  const auto after_release = owner.ReconcileAcquireAttempt(attempt);
  ASSERT_TRUE(after_release.matches(attempt));
  EXPECT_EQ(after_release.state, core::ApplyFenceAcquireAttemptReconcileState::kConflicting);
  EXPECT_FALSE(after_release.accepted_fence);
  EXPECT_FALSE(gate.exact_fence_is_held(accepted.configuration_fence_id, scope));
}

TEST(ManagedKernelAdmissionGate,
     MissingOrChangedFenceRejectsComponentStartBeforeEffects) {
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  const core::ConfigurationFenceId fence_id("fence.initial-component");
  unsigned effects = 0U;
  EXPECT_THROW(gate.RunWithExactHeldFence(fence_id, scope, [&] { ++effects; }),
               std::runtime_error);
  EXPECT_EQ(effects, 0U);
  gate.RestoreFence(fence_id, scope);
  auto wrong_scope = scope;
  wrong_scope.resources.pop_back();
  EXPECT_THROW(
      gate.RunWithExactHeldFence(fence_id, wrong_scope, [&] { ++effects; }),
      std::runtime_error);
  EXPECT_EQ(effects, 0U);
  gate.RunWithExactHeldFence(fence_id, scope, [&] { ++effects; });
  EXPECT_EQ(effects, 1U);
  gate.ReleaseFence(fence_id, scope);
  EXPECT_THROW(gate.RunWithExactHeldFence(fence_id, scope, [&] { ++effects; }),
               std::runtime_error);
  EXPECT_EQ(effects, 1U);
}

TEST(ManagedKernelAdmissionGate,
     AnyHeldFenceIncludesEmptyApplyWhileAdmissionMutexIsRetained) {
  ManagedKernelAdmissionGate gate;
  const core::ConfigurationFenceScopeDTO empty_scope{{}, {}, {}, {}};
  const core::ConfigurationFenceId empty_id("fence.empty-apply");
  const core::ConfigurationFenceId physical_id("fence.physical-apply");
  EXPECT_FALSE(gate.has_held_configuration_fences());
  gate.RestoreFence(empty_id, empty_scope);
  {
    // The installation owner retains this same recursive mutex from the
    // observation through its commit, so an Apply cannot overtake the proof.
    std::lock_guard lock(*gate.shared_admission_mutex());
    EXPECT_TRUE(gate.has_held_configuration_fences());
    EXPECT_TRUE(gate.exact_fence_is_held(empty_id, empty_scope));
    EXPECT_TRUE(gate.current_fenced_resources(AffectedScope().resources).empty());
  }
  gate.RestoreFence(physical_id, AffectedScope());
  gate.ReleaseFence(empty_id, empty_scope);
  EXPECT_TRUE(gate.has_held_configuration_fences());
  gate.ReleaseFence(physical_id, AffectedScope());
  EXPECT_FALSE(gate.has_held_configuration_fences());
}

namespace {
core::RuntimeProviderDomainConfigurationReadbackDTO
ProviderReadback(const OwnerFixture &fixture) {
  const auto &graph = fixture.bundle->value().cell_graph;
  const auto found = std::find_if(
      graph.control_domains.begin(), graph.control_domains.end(),
      [](const auto &domain) { return domain.provider() != nullptr; });
  if (found == graph.control_domains.end())
    throw std::logic_error("sealed I/O fixture has no provider domain");
  const auto &provider = *found->provider();
  const core::RuntimeEpoch epoch("runtime-epoch.actual-io-driver");
  return {
      fixture.bundle->retained_bundle_id(),
      graph.graph_revision,
      core::ComponentLifecycleGeneration(1U),
      {found->control_domain_id, found->control_domain_revision,
       core::ResolvedProviderDomainScopeDTO{provider, epoch, std::nullopt, {}}},
      {found->control_domain_id, found->control_domain_revision,
       provider.provider_instance_id, provider.provider_manifest_id,
       provider.provider_manifest_revision, provider.provider_runtime_unit_id,
       epoch, provider.lifecycle_plan_catalog_digest},
      false,
      0U,
      0U};
}
core::ConfigurationFenceScopeDTO ProviderFenceScope(
    const core::RuntimeProviderDomainConfigurationReadbackDTO &row) {
  const auto &provider = row.domain.provider()->provider;
  const core::ProviderConfigurationFenceOccurrenceDTO occurrence{
      provider.controllable_component_id, provider.provider_instance_id,
      provider.signal_configuration_digest, row.domain.resource_ids()};
  return {{provider.controllable_component_id},
          {{row.domain.control_domain_id, row.domain.control_domain_revision,
            row.domain.control_domain_revision,
            core::ProviderConfigurationFenceDomainDTO{occurrence, occurrence}}},
          {},
          row.domain.resource_ids()};
}
} // namespace

TEST(ManagedKernelSelectedBringUpFenceOwner,
     ClosedInstalledProviderIdentityAllowsInitialAndDisabledBringUp) {
  OwnerFixture fixture(false, true);
  auto row = ProviderReadback(fixture);
  ASSERT_TRUE(row.locally_valid());
  const auto scope = ProviderFenceScope(row);
  ASSERT_TRUE(scope.valid());
  ManagedKernelAdmissionGate gate;
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, std::vector<core::DomainActiveControlConfigurationDTO>{},
      gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  auto config = fixture.Config("provider-closed-bring-up");
  config.selected_claim_recovery_receipt = HeldSelectedReceipt;
  unsigned lookups = 0U;
  config.provider_configuration_source = [&](const auto &) {
    ++lookups;
    return core::Result<std::optional<
        core::RuntimeProviderDomainConfigurationReadbackDTO>>::Ok(row);
  };
  ManagedKernelApplyFenceOwner owner(config, gate);
  const auto snapshot =
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.provider")});
  EXPECT_TRUE(snapshot.active_control_configurations.empty());
  auto intent = SelectedIntent(snapshot);
  ASSERT_TRUE(intent.valid());
  const auto accepted = owner.AcquireSelectedBringUpFence(intent);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const core::SelectedBringUpConfigurationFenceStatusQueryDTO query{
      intent.owner(), accepted.value().configuration_fence_id,
      accepted.value().configuration_fence_generation, scope,
      snapshot.echoed_query.manager_runtime_epoch};
  unsigned effects = 0U;
  const auto installed_epoch = row.provider_occurrence.provider_runtime_epoch;
  row.provider_occurrence.provider_runtime_epoch =
      core::RuntimeEpoch("runtime-epoch.stale-driver");
  EXPECT_THROW(
      owner.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; }),
      std::runtime_error);
  EXPECT_EQ(effects, 0U);
  row.provider_occurrence.provider_runtime_epoch = installed_epoch;
  owner.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; });
  EXPECT_EQ(effects, 1U);
  const auto held = owner.ObserveSelectedBringUpFenceStatus(query);
  ASSERT_TRUE(held.ok()) << held.error().message;
  EXPECT_TRUE(
      held.value()
          .reconciliation_evidence.active_control_configurations.empty());
  // No Active/open/sample fact is introduced by provider identity fencing.
  EXPECT_FALSE(row.operation_admission_open);
  EXPECT_EQ(row.driver_source_sequence, 0U);
  EXPECT_EQ(row.driver_observation_generation, 0U);
  const auto released = owner.ReleaseSelectedBringUpFence(
      {held.value().reconciliation_evidence, query.manager_runtime_epoch});
  ASSERT_TRUE(released.ok()) << released.error().message;
  // A subsequent Disable→Enable request uses the same installed CLOSED
  // identity; the lifecycle owner, rather than this fence, still enables it.
  auto again = SelectedIntent(
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.provider")}));
  again.owner_request_context.request_id =
      core::RequestId("request.provider-enable-again");
  again.lifecycle_transaction_id =
      core::LifecycleTransactionId("selected-lifecycle.2");
  again.lifecycle_transaction_generation =
      core::LifecycleTransactionGeneration(2U);
  again.selected_claim.claim_generation =
      core::CellAdmissionClaimGeneration(2U);
  again.selected_claim.intent.owner_transaction_id =
      again.lifecycle_transaction_id.value();
  again.selected_claim.intent.request_id =
      again.owner_request_context.request_id;
  again.selected_claim.intent.intervention_evidence->offer_transaction_id =
      again.lifecycle_transaction_id.value();
  ASSERT_TRUE(again.valid());
  EXPECT_TRUE(owner.AcquireSelectedBringUpFence(again).ok());
  EXPECT_GE(lookups, 5U);
}

TEST(ManagedKernelSelectedBringUpFenceOwner,
     MissingForeignAndWrongEpochProviderProofRefuseBeforeFence) {
  OwnerFixture fixture(false, true);
  auto row = ProviderReadback(fixture);
  const auto scope = ProviderFenceScope(row);
  ManagedKernelAdmissionGate gate;
  auto config = fixture.Config("provider-proof-refusal");
  config.selected_claim_recovery_receipt = HeldSelectedReceipt;
  std::optional<core::RuntimeProviderDomainConfigurationReadbackDTO> current =
      row;
  config.provider_configuration_source = [&](const auto &) {
    return core::Result<std::optional<
        core::RuntimeProviderDomainConfigurationReadbackDTO>>::Ok(current);
  };
  ManagedKernelApplyFenceOwner owner(config, gate);
  const auto snapshot =
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.provider")});
  const auto intent = SelectedIntent(snapshot);
  current.reset();
  EXPECT_FALSE(owner.AcquireSelectedBringUpFence(intent).ok());
  EXPECT_FALSE(gate.has_held_configuration_fences());
  current = row;
  current->provider_occurrence.provider_runtime_epoch =
      core::RuntimeEpoch("runtime-epoch.foreign");
  EXPECT_FALSE(owner.AcquireSelectedBringUpFence(intent).ok());
  EXPECT_FALSE(gate.has_held_configuration_fences());
  current = row;
  current->retained_bundle_id = core::RetainedBundleId("foreign.bundle");
  EXPECT_FALSE(owner.AcquireSelectedBringUpFence(intent).ok());
  EXPECT_FALSE(gate.has_held_configuration_fences());
  current = row;
  auto foreign = intent;
  auto &proof = std::get<core::ProviderConfigurationFenceDomainDTO>(
      foreign.selected_scope.domains.front().payload);
  proof.predecessor_provider->signal_configuration_digest =
      core::ContentDigest(std::string(kSha));
  foreign.expected_admission.echoed_query.affected_scope =
      foreign.selected_scope;
  EXPECT_THROW(owner.ObserveAdmission({foreign.selected_scope,
                                       core::RuntimeEpoch("manager.provider")}),
               std::runtime_error);
  EXPECT_FALSE(owner.AcquireSelectedBringUpFence(foreign).ok());
  EXPECT_FALSE(gate.has_held_configuration_fences());
}

TEST(ManagedKernelSelectedBringUpFenceOwner,
     CoherentProviderRestartRefusesCapturedAndRestoredPredecessorEffects) {
  OwnerFixture fixture(false, true);
  auto current = ProviderReadback(fixture);
  const auto predecessor = current;
  const auto scope = ProviderFenceScope(current);
  auto config = fixture.Config("provider-lineage");
  config.selected_claim_recovery_receipt = HeldSelectedReceipt;
  config.provider_configuration_source = [&](const auto &) {
    return core::Result<std::optional<
        core::RuntimeProviderDomainConfigurationReadbackDTO>>::Ok(current);
  };
  std::optional<core::SelectedBringUpConfigurationFenceIntentDTO> captured;
  std::optional<core::ConfigurationFenceAcceptedDTO> accepted;
  const auto restart = [&] {
    const core::RuntimeEpoch epoch("runtime-epoch.restarted-io-driver");
    std::get<core::ResolvedProviderDomainScopeDTO>(current.domain.payload)
        .provider_runtime_epoch = epoch;
    current.provider_occurrence.provider_runtime_epoch = epoch;
    current.component_lifecycle_generation =
        core::ComponentLifecycleGeneration(2U);
  };
  {
    ManagedKernelAdmissionGate gate;
    ManagedKernelApplyFenceOwner owner(config, gate);
    captured = SelectedIntent(owner.ObserveAdmission(
        {scope, core::RuntimeEpoch("manager.provider")}));
    ASSERT_EQ(captured->expected_admission.provider_lineages.size(), 1U);
    EXPECT_EQ(captured->expected_admission.provider_lineages.front()
                  .provider_runtime_epoch,
              predecessor.provider_occurrence.provider_runtime_epoch);
    restart();
    ASSERT_TRUE(current.locally_valid());
    EXPECT_EQ(ProviderFenceScope(current), scope);
    EXPECT_FALSE(owner.AcquireSelectedBringUpFence(*captured).ok());
    EXPECT_FALSE(gate.has_held_configuration_fences());
    current = predecessor;
    const auto result = owner.AcquireSelectedBringUpFence(*captured);
    ASSERT_TRUE(result.ok()) << result.error().message;
    accepted = result.value();
    const core::SelectedBringUpConfigurationFenceStatusQueryDTO query{
        captured->owner(), accepted->configuration_fence_id,
        accepted->configuration_fence_generation, scope,
        core::RuntimeEpoch("manager.provider")};
    restart();
    ASSERT_TRUE(current.locally_valid());
    unsigned effects = 0U;
    EXPECT_THROW(
        owner.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; }),
        std::runtime_error);
    EXPECT_EQ(effects, 0U);
    EXPECT_TRUE(
        gate.exact_fence_is_held(accepted->configuration_fence_id, scope));
  }
  // Restore the same host occurrence from its real ledger. The new provider
  // row is internally coherent; the durable captured lineage still forbids
  // using it as the old predecessor.
  ManagedKernelAdmissionGate restored_gate;
  ManagedKernelApplyFenceOwner restored(config, restored_gate);
  ASSERT_TRUE(restored_gate.exact_fence_is_held(
      accepted->configuration_fence_id, scope));
  const core::SelectedBringUpConfigurationFenceStatusQueryDTO query{
      captured->owner(), accepted->configuration_fence_id,
      accepted->configuration_fence_generation, scope,
      core::RuntimeEpoch("manager.provider")};
  unsigned effects = 0U;
  EXPECT_THROW(
      restored.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; }),
      std::runtime_error);
  EXPECT_EQ(effects, 0U);
  current = predecessor;
  EXPECT_NO_THROW(
      restored.RunWithExactHeldSelectedPredecessor(query, [&] { ++effects; }));
  EXPECT_EQ(effects, 1U);
}

TEST(ManagedKernelApplyFenceOwner,
     ProviderLineageIsRecheckedBeforeAcquireAndPredecessorEffect) {
  OwnerFixture fixture(false, true);
  auto current = ProviderReadback(fixture);
  const auto predecessor = current;
  const auto scope = ProviderFenceScope(current);
  ManagedKernelAdmissionGate gate;
  auto config = fixture.Config("provider-apply-lineage");
  config.provider_configuration_source = [&](const auto &) {
    return core::Result<std::optional<
        core::RuntimeProviderDomainConfigurationReadbackDTO>>::Ok(current);
  };
  ManagedKernelApplyFenceOwner owner(config, gate);
  const auto observed =
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.provider")});
  const core::ApplyConfigurationFenceIntentDTO intent{
      {core::RequestId("provider.apply.fence"),
       core::RequestOwnerEpoch("manager.owner"), "test", std::nullopt},
      {core::ApplyId("provider.apply"), core::ApplyGeneration(1U)},
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      observed.operation_resource_generation,
      observed.configuration_fence_generation,
      {},
      "provider Apply",
      observed.provider_lineages};
  ASSERT_TRUE(intent.valid());
  const auto restart = [&] {
    const core::RuntimeEpoch epoch("runtime-epoch.restarted-provider");
    std::get<core::ResolvedProviderDomainScopeDTO>(current.domain.payload)
        .provider_runtime_epoch = epoch;
    current.provider_occurrence.provider_runtime_epoch = epoch;
    current.component_lifecycle_generation =
        core::ComponentLifecycleGeneration(2U);
  };
  restart();
  ASSERT_TRUE(current.locally_valid());
  EXPECT_THROW((void)owner.Acquire(intent), std::runtime_error);
  EXPECT_FALSE(gate.has_held_configuration_fences());
  current = predecessor;
  const auto accepted = owner.Acquire(intent);
  const core::ConfigurationFenceStatusQueryDTO query{
      intent.owner_transaction, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, scope,
      observed.echoed_query.manager_runtime_epoch};
  restart();
  unsigned effects = 0U;
  EXPECT_THROW(owner.RunWithExactHeldPredecessor(
                   query, observed.execution_host_epoch.publication_generation,
                   observed.active_public_pointer_digest, [&] { ++effects; }),
               std::runtime_error);
  EXPECT_EQ(effects, 0U);
  current = predecessor;
  EXPECT_NO_THROW(owner.RunWithExactHeldPredecessor(
      query, observed.execution_host_epoch.publication_generation,
      observed.active_public_pointer_digest, [&] { ++effects; }));
  EXPECT_EQ(effects, 1U);
}

TEST(ManagedKernelApplyFenceOwner,
     OriginalCandidateUsesExactOwnedLineageAndRestoredParent) {
  OwnerFixture fixture(false, true);
  auto current = ProviderReadback(fixture);
  const auto scope = ProviderFenceScope(current);
  auto config = fixture.Config("owned-provider-candidate");
  config.provider_configuration_source = [&](const auto &) {
    return core::Result<std::optional<
        core::RuntimeProviderDomainConfigurationReadbackDTO>>::Ok(current);
  };
  std::optional<core::CellAdmissionClaimDTO> actual_parent_claim;
  config.selected_claim_recovery_receipt = [&](const auto &query) {
    if (!actual_parent_claim || query.expected_claim != actual_parent_claim)
      return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Err(
          {"stale_generation", "actual parent claim changed", {}});
    return HeldSelectedReceipt(query);
  };
  std::optional<core::ConfigurationFenceStatusQueryDTO> query;
  std::optional<core::ConfigurationFenceAdmissionSnapshotDTO> observed;
  std::optional<core::ApplyComponentBringUpAuthorityDTO> authority;
  std::optional<core::RuntimeProviderDomainConfigurationReadbackDTO>
      owned_candidate;
  unsigned effects = 0U;
  {
    ManagedKernelAdmissionGate gate;
    ManagedKernelApplyFenceOwner owner(config, gate);
    observed =
        owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.provider")});
    const core::ApplyConfigurationFenceIntentDTO intent{
        {core::RequestId("owned.apply.fence"),
         core::RequestOwnerEpoch("manager.owner"), "test", std::nullopt},
        {core::ApplyId("owned.apply"), core::ApplyGeneration(1U)},
        scope,
        core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
        fixture.bundle->retained_bundle_id(),
        fixture.bundle->value().cell_graph.graph_revision,
        observed->operation_resource_generation,
        observed->configuration_fence_generation,
        {},
        "owned candidate",
        observed->provider_lineages};
    const auto accepted = owner.Acquire(intent);
    query = core::ConfigurationFenceStatusQueryDTO{
        intent.owner_transaction, accepted.configuration_fence_id,
        accepted.configuration_fence_generation, scope,
        observed->echoed_query.manager_runtime_epoch};
    const core::ContentDigest plan_digest{std::string(kSha)};
    const core::CellAdmissionClaimIntentDTO claim_intent{
        core::CellAdmissionClaimKindDTO::kManagerApply,
        core::CellAdmissionOwnerRoleDTO::kManager,
        intent.owner_request_context.request_owner_epoch,
        intent.owner_transaction.apply_id.value(),
        core::RequestId("actual.parent.claim"),
        plan_digest,
        core::AdmissionGeneration(1U),
        {},
        std::nullopt,
        std::nullopt};
    actual_parent_claim = core::CellAdmissionClaimDTO{
        claim_intent, core::CellAdmissionClaimGeneration(1U),
        core::AdmissionGeneration(2U), 1};
    authority = core::ApplyComponentBringUpAuthorityDTO{
        {intent.owner_transaction, plan_digest}, *actual_parent_claim, *query};
    ASSERT_TRUE(authority->valid());
    // The trusted host producer independently joins this accepted generation
    // to the real original child and retained driver; this unit exercises the
    // fence consumer, not a fabricated public authorization path.
    current.component_lifecycle_generation =
        core::ComponentLifecycleGeneration(2U);
    owned_candidate = current;
    EXPECT_THROW(
        owner.RunWithExactHeldOriginalApplyCandidate(
            *query, observed->execution_host_epoch.publication_generation,
            observed->active_public_pointer_digest, *authority,
            observed->active_projection_revision, {}, [&] { ++effects; }),
        std::runtime_error);
    EXPECT_THROW(owner.RunWithExactHeldOriginalApplyCandidate(
                     *query,
                     observed->execution_host_epoch.publication_generation,
                     observed->active_public_pointer_digest, *authority,
                     observed->active_projection_revision,
                     {*owned_candidate, *owned_candidate}, [&] { ++effects; }),
                 std::runtime_error);
    EXPECT_EQ(effects, 0U);
    EXPECT_THROW(
        owner.RunWithExactHeldPredecessor(
            *query, observed->execution_host_epoch.publication_generation,
            observed->active_public_pointer_digest, [&] { ++effects; }),
        std::runtime_error);
    EXPECT_NO_THROW(owner.RunWithExactHeldOriginalApplyCandidate(
        *query, observed->execution_host_epoch.publication_generation,
        observed->active_public_pointer_digest, *authority,
        observed->active_projection_revision, {*owned_candidate},
        [&] { ++effects; }));
    EXPECT_EQ(effects, 1U);
    const core::RuntimeEpoch replacement("runtime-epoch.unrelated-replacement");
    std::get<core::ResolvedProviderDomainScopeDTO>(current.domain.payload)
        .provider_runtime_epoch = replacement;
    current.provider_occurrence.provider_runtime_epoch = replacement;
    current.component_lifecycle_generation =
        core::ComponentLifecycleGeneration(3U);
    ASSERT_TRUE(current.locally_valid());
    EXPECT_THROW(owner.RunWithExactHeldOriginalApplyCandidate(
                     *query,
                     observed->execution_host_epoch.publication_generation,
                     observed->active_public_pointer_digest, *authority,
                     observed->active_projection_revision, {*owned_candidate},
                     [&] { ++effects; }),
                 std::runtime_error);
    EXPECT_EQ(effects, 1U);
  }
  ManagedKernelAdmissionGate gate;
  ManagedKernelApplyFenceOwner restored(config, gate);
  EXPECT_THROW(restored.RunWithExactHeldOriginalApplyCandidate(
                   *query,
                   observed->execution_host_epoch.publication_generation,
                   observed->active_public_pointer_digest, *authority,
                   observed->active_projection_revision, {*owned_candidate},
                   [&] { ++effects; }),
               std::runtime_error);
  current = *owned_candidate;
  auto foreign = *authority;
  foreign.parent_claim.claim_generation =
      core::CellAdmissionClaimGeneration(2U);
  ASSERT_TRUE(foreign.valid());
  EXPECT_THROW(restored.RunWithExactHeldOriginalApplyCandidate(
                   *query,
                   observed->execution_host_epoch.publication_generation,
                   observed->active_public_pointer_digest, foreign,
                   observed->active_projection_revision, {*owned_candidate},
                   [&] { ++effects; }),
               std::runtime_error);
  EXPECT_NO_THROW(restored.RunWithExactHeldOriginalApplyCandidate(
      *query, observed->execution_host_epoch.publication_generation,
      observed->active_public_pointer_digest, *authority,
      observed->active_projection_revision, {*owned_candidate},
      [&] { ++effects; }));
  EXPECT_EQ(effects, 2U);
}

TEST(ManagedKernelApplyFenceOwner,
     OrdinaryInstalledCandidateStatusKeepsPredecessorAndRequiresExactFacts) {
  OwnerFixture fixture(false, true);
  const auto candidate =
      deployment::test::materialize_boolean_io_retained_fixture(
          fixture.retained_root, true, true)
          .bundle;
  ASSERT_NE(candidate, nullptr);
  ASSERT_NE(candidate->retained_bundle_id(),
            fixture.bundle->retained_bundle_id());
  const auto scope = deployment::derive_apply_configuration_fence_scope(
      *fixture.bundle, *candidate);
  auto current = ProviderReadback(fixture);
  auto config = fixture.Config("ordinary-candidate-observation");
  config.provider_configuration_source = [&](const auto &domain) {
    using Reply = core::Result<
        std::optional<core::RuntimeProviderDomainConfigurationReadbackDTO>>;
    return Reply::Ok(current.domain.control_domain_id == domain
                         ? std::optional(current)
                         : std::nullopt);
  };
  std::optional<core::CellAdmissionClaimDTO> actual_parent;
  config.selected_claim_recovery_receipt = [&](const auto &query) {
    if (!actual_parent || query.expected_claim != actual_parent)
      return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Err(
          {"stale_generation", "actual parent claim changed", {}});
    return HeldSelectedReceipt(query);
  };
  ManagedKernelAdmissionGate gate;
  ManagedKernelApplyFenceOwner owner(config, gate);
  const auto observed =
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.native")});
  const core::ApplyConfigurationFenceIntentDTO intent{
      {core::RequestId("ordinary.candidate.fence"),
       core::RequestOwnerEpoch("manager.owner"), "test", std::nullopt},
      {core::ApplyId("ordinary.candidate.apply"), core::ApplyGeneration(1U)},
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      observed.operation_resource_generation,
      observed.configuration_fence_generation,
      {},
      "ordinary candidate observation",
      observed.provider_lineages};
  const auto accepted = owner.Acquire(intent);
  const core::ConfigurationFenceStatusQueryDTO query{
      intent.owner_transaction, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, scope,
      observed.echoed_query.manager_runtime_epoch};
  const core::CellAdmissionClaimIntentDTO claim_intent{
      core::CellAdmissionClaimKindDTO::kManagerApply,
      core::CellAdmissionOwnerRoleDTO::kManager,
      intent.owner_request_context.request_owner_epoch,
      intent.owner_transaction.apply_id.value(),
      core::RequestId("ordinary.actual.parent"),
      core::ContentDigest(std::string(kSha)),
      core::AdmissionGeneration(1U),
      {},
      std::nullopt,
      std::nullopt};
  actual_parent = core::CellAdmissionClaimDTO{
      claim_intent, core::CellAdmissionClaimGeneration(1U),
      claim_intent.expected_admission_generation, 1};
  const auto parent = *actual_parent;
  ASSERT_EQ(parent.acquired_admission_generation, parent.intent.expected_admission_generation);
  const core::RequestOwnerEpoch lifecycle_owner("ordinary.actual.cclc.owner");
  core::SqliteCCLCJournal journal(fixture.state_root.path() /
                                      "ordinary-cclc.sqlite3",
                                  candidate->value().cell_graph.cell_id, 8U);
  ASSERT_TRUE(
      journal.BeginOwnerEpoch(lifecycle_owner, fixture.now_ns++, 32U).ok());
  std::vector<core::ControllableComponentId> components;
  for (const auto &component :
       candidate->value().cell_graph.controllable_components)
    components.push_back(component.controllable_component_id);
  const auto planned =
      journal.PlanNativeApply({{intent.owner_transaction.apply_id,
                                intent.owner_transaction.apply_generation},
                               candidate->retained_bundle_id(),
                               candidate->value().cell_graph.graph_revision,
                               components,
                               {},
                               {},
                               {},
                               {},
                               {},
                               core::ComponentLifecycleStateDTO::kDeclared,
                               {}});
  ASSERT_TRUE(planned.ok()) << planned.error().message;
  const auto plan = planned.value();
  ASSERT_FALSE(plan.component_bring_up);
  ASSERT_EQ(plan.components.size(), 1U);
  const auto &graph = candidate->value().cell_graph;
  const auto domain =
      std::find_if(graph.control_domains.begin(), graph.control_domains.end(),
                   [](const auto &row) { return row.provider() != nullptr; });
  ASSERT_NE(domain, graph.control_domains.end());
  const auto catalog = deployment::ResolveRetainedIoSignalCatalog(graph);
  ASSERT_TRUE(catalog.ok());
  std::vector<core::ResourceId> outputs;
  for (const auto &row : catalog.value())
    if (row.descriptor.output_resource)
      outputs.push_back(*row.descriptor.output_resource);
  std::sort(outputs.begin(), outputs.end());
  outputs.erase(std::unique(outputs.begin(), outputs.end()), outputs.end());
  ASSERT_FALSE(outputs.empty());
  unsigned initial_effects = 0U;
  EXPECT_NO_THROW(owner.RunWithExactHeldPredecessor(query,
      observed.execution_host_epoch.publication_generation,
      observed.active_public_pointer_digest, [&] { ++initial_effects; }));
  EXPECT_EQ(initial_effects, 1U);
  const auto &provider = *domain->provider();
  const core::RuntimeEpoch driver_epoch("ordinary.actual.installed.driver");
  current = {candidate->retained_bundle_id(),
             graph.graph_revision,
             plan.components.front().generation,
             {domain->control_domain_id, domain->control_domain_revision,
              core::ResolvedProviderDomainScopeDTO{provider, driver_epoch,
                                                   std::nullopt, outputs}},
             {domain->control_domain_id, domain->control_domain_revision,
              provider.provider_instance_id, provider.provider_manifest_id,
              provider.provider_manifest_revision,
              provider.provider_runtime_unit_id, driver_epoch,
              provider.lifecycle_plan_catalog_digest},
             false,
             0U,
             0U};
  ASSERT_TRUE(current.locally_valid());
  // The consumer unit supplies controlled current-provider/held-parent ports.
  // Its plan generation is genuinely reserved by the real journal; this does
  // not assert execution of the separate host/CCLC fact producer.
  const core::InstalledProviderCandidateFactsDTO facts{
      candidate->retained_bundle_id(),
      graph.graph_revision,
      plan.components.front().component_id,
      plan.components.front().generation,
      {current.domain},
      {current.provider_occurrence},
      core::InstalledNativeApplyCandidateSourceDTO{lifecycle_owner,
                                                   plan.apply}};
  component_manager::SqliteActiveBundlePublicationStore publications(
      fixture.state_root.path() /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      graph.cell_id);
  const auto head = publications.ReadActiveBundlePublication(
      observed.execution_host_epoch.publication_generation);
  ASSERT_TRUE(head.ok());
  const auto predecessor = head.value();
  ASSERT_EQ(predecessor.request.candidate_retained_bundle_id,
            fixture.bundle->retained_bundle_id());
  EXPECT_THROW(owner.QueryStatus(query), std::runtime_error);
  bool installed_attempt_current = true;
  unsigned checked_attempts = 0U;
  const auto metadata_guard = [&] {
    ++checked_attempts;
    if (!installed_attempt_current)
      throw std::runtime_error("installed attempt changed");
  };
  const auto status = owner.QueryInstalledNativeApplyCandidateStatus(
      query, predecessor, parent, plan, {facts}, metadata_guard);
  EXPECT_TRUE(status.matches(query));
  EXPECT_EQ(status.state, core::ConfigurationFenceState::kHeld);
  EXPECT_TRUE(status.admission_fenced);
  EXPECT_EQ(status.reconciliation_evidence.active_publication_generation,
            predecessor.publication_generation);
  EXPECT_EQ(status.reconciliation_evidence.active_public_pointer_digest,
            predecessor.durable_public_pointer_content_digest);
  EXPECT_EQ(checked_attempts, 1U);
  EXPECT_EQ(publications
                .ReadActiveBundlePublication(predecessor.publication_generation)
                .value(),
            predecessor);

  const core::ProjectionRevisionDTO candidate_projection{
      candidate->retained_bundle_id(), graph.graph_revision,
      predecessor.request.candidate_projection_revision.scene_revision};
  unsigned handoffs = 0U;
  const auto handoff = [&](const auto &selected_query, const auto &selected_parent,
                           const auto &selected_head, const auto &rows) {
    owner.RunWithExactHeldInstalledCandidate(selected_query,
        predecessor.publication_generation, selected_head, selected_parent,
        candidate_projection, rows, [&] {
          EXPECT_TRUE(gate.exact_fence_is_held(query.configuration_fence_id, scope));
          ++handoffs;
        });
  };
  EXPECT_THROW(owner.RunWithExactHeldPredecessor(query,
      predecessor.publication_generation, predecessor.durable_public_pointer_content_digest,
      [&] { ++handoffs; }), std::runtime_error);
  EXPECT_THROW(handoff(query, parent, predecessor.durable_public_pointer_content_digest,
                       std::vector<core::RuntimeProviderDomainConfigurationReadbackDTO>{}),
               std::runtime_error);
  EXPECT_EQ(handoffs, 0U);
  const std::vector installed_rows{current};
  EXPECT_NO_THROW(handoff(query, parent, predecessor.durable_public_pointer_content_digest,
                          installed_rows));
  EXPECT_EQ(handoffs, 1U);
  EXPECT_THROW(handoff(query, parent, core::ContentDigest(std::string(kSha)), installed_rows),
               std::runtime_error);
  auto wrong_query = query;
  wrong_query.configuration_fence_generation = core::ConfigurationFenceGeneration(
      query.configuration_fence_generation.value() + 1U);
  EXPECT_THROW(handoff(wrong_query, parent, predecessor.durable_public_pointer_content_digest,
                       installed_rows), std::runtime_error);
  EXPECT_EQ(handoffs, 1U);
  auto wrong_parent = parent;
  wrong_parent.claim_generation =
      core::CellAdmissionClaimGeneration(parent.claim_generation.value() + 1U);
  EXPECT_THROW(
      owner.QueryInstalledNativeApplyCandidateStatus(
          query, predecessor, wrong_parent, plan, {facts}, metadata_guard),
      std::runtime_error);
  EXPECT_THROW(handoff(query, wrong_parent, predecessor.durable_public_pointer_content_digest,
                       installed_rows), std::runtime_error);
  EXPECT_EQ(handoffs, 1U);
  auto stale_plan = plan;
  stale_plan.components.front().generation = core::ComponentLifecycleGeneration(
      plan.components.front().generation.value() + 1U);
  EXPECT_THROW(
      owner.QueryInstalledNativeApplyCandidateStatus(
          query, predecessor, parent, stale_plan, {facts}, metadata_guard),
      std::runtime_error);
  auto foreign_facts = facts;
  foreign_facts.retained_bundle_id = fixture.bundle->retained_bundle_id();
  EXPECT_THROW(
      owner.QueryInstalledNativeApplyCandidateStatus(
          query, predecessor, parent, plan, {foreign_facts}, metadata_guard),
      std::runtime_error);
  EXPECT_THROW(owner.QueryInstalledNativeApplyCandidateStatus(
                   query, predecessor, parent, plan, {}, metadata_guard),
               std::runtime_error);
  EXPECT_THROW(
      owner.QueryInstalledNativeApplyCandidateStatus(
          query, predecessor, parent, plan, {facts, facts}, metadata_guard),
      std::runtime_error);
  const auto installed = current;
  const core::RuntimeEpoch replacement("ordinary.replaced.driver");
  std::get<core::ResolvedProviderDomainScopeDTO>(current.domain.payload)
      .provider_runtime_epoch = replacement;
  current.provider_occurrence.provider_runtime_epoch = replacement;
  ASSERT_TRUE(current.locally_valid());
  EXPECT_THROW(owner.QueryInstalledNativeApplyCandidateStatus(
                   query, predecessor, parent, plan, {facts}, metadata_guard),
               std::runtime_error);
  EXPECT_THROW(handoff(query, parent, predecessor.durable_public_pointer_content_digest,
                       installed_rows), std::runtime_error);
  EXPECT_EQ(handoffs, 1U);
  current = installed;
  installed_attempt_current = false;
  EXPECT_THROW(owner.QueryInstalledNativeApplyCandidateStatus(
                   query, predecessor, parent, plan, {facts}, metadata_guard),
               std::runtime_error);
  installed_attempt_current = true;
  EXPECT_NO_THROW(owner.QueryInstalledNativeApplyCandidateStatus(
      query, predecessor, parent, plan, {facts}, metadata_guard));
  EXPECT_EQ(current, installed);
  EXPECT_TRUE(gate.exact_fence_is_held(query.configuration_fence_id,
                                       query.expected_scope));
  EXPECT_FALSE(current.operation_admission_open);
  EXPECT_EQ(publications
                .ReadActiveBundlePublication(predecessor.publication_generation)
                .value(),
            predecessor);
}

TEST(ManagedKernelApplyFenceOwner,
     RemovedProviderCandidateHandoffRequiresAbsenceAndExactHeldAuthoringParent) {
  OwnerFixture fixture(false, true);
  const auto empty_root = fixture.state_root.path() / "empty-candidate";
  ASSERT_TRUE(std::filesystem::create_directory(empty_root));
  const auto admission = deployment::detail::generate_ros_empty_package(
      empty_root, fixture.bundle->value().cell_graph.cell_id,
      deployment::detail::ros2_control_execution_runtime_profile_document());
  const auto &empty = admission.retained_bundle();
  ASSERT_TRUE(empty.value().cell_graph.control_domains.empty());
  const auto scope = deployment::derive_apply_configuration_fence_scope(*fixture.bundle, empty);
  const auto predecessor_row = ProviderReadback(fixture);
  std::optional<core::RuntimeProviderDomainConfigurationReadbackDTO> current = predecessor_row;
  auto config = fixture.Config("removed-provider-handoff");
  config.candidate_retained_package_root = empty_root;
  config.provider_configuration_source = [&](const auto &domain) {
    using Reply = core::Result<std::optional<core::RuntimeProviderDomainConfigurationReadbackDTO>>;
    return Reply::Ok(current && current->domain.control_domain_id == domain ? current : std::nullopt);
  };
  std::optional<core::CellAdmissionClaimDTO> actual_parent;
  config.selected_claim_recovery_receipt = [&](const auto &query) {
    if (!actual_parent || query.expected_claim != actual_parent)
      return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Err(
          {"stale_generation", "actual authoring parent changed", {}});
    return HeldSelectedReceipt(query);
  };
  ManagedKernelAdmissionGate gate;
  ManagedKernelApplyFenceOwner owner(config, gate);
  const auto observed = owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.removal")});
  const core::ApplyConfigurationFenceIntentDTO intent{
      {core::RequestId("removal.fence"), core::RequestOwnerEpoch("manager.owner"), "test", std::nullopt},
      {core::ApplyId("removal.apply"), core::ApplyGeneration(1U)}, scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(), fixture.bundle->value().cell_graph.graph_revision,
      observed.operation_resource_generation, observed.configuration_fence_generation,
      {}, "remove installed provider", observed.provider_lineages};
  const auto accepted = owner.Acquire(intent);
  const core::ConfigurationFenceStatusQueryDTO query{
      intent.owner_transaction, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, scope, observed.echoed_query.manager_runtime_epoch};
  const core::CellAdmissionClaimIntentDTO claim_intent{
      core::CellAdmissionClaimKindDTO::kAuthoringMutation,
      core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
      core::RequestOwnerEpoch("actual.authoring.owner"), "actual.close", core::RequestId("actual.close.parent"),
      core::ContentDigest(std::string(kSha)), core::AdmissionGeneration(1U), {}, std::nullopt, std::nullopt};
  actual_parent = core::CellAdmissionClaimDTO{
      claim_intent, core::CellAdmissionClaimGeneration(1U), claim_intent.expected_admission_generation, 1};
  const auto parent = *actual_parent;
  core::ProjectionRevisionDTO candidate{
      empty.retained_bundle_id(), empty.value().cell_graph.graph_revision,
      observed.active_projection_revision.scene_revision};
  unsigned effects = 0U;
  const auto initial = [&] {
    owner.RunWithExactHeldPredecessor(query, observed.execution_host_epoch.publication_generation,
        observed.active_public_pointer_digest, [&] { ++effects; });
  };
  const auto handoff = [&](const auto &selected_query, const auto &selected_parent,
                           const auto &head) {
    owner.RunWithExactHeldInstalledCandidate(selected_query,
        observed.execution_host_epoch.publication_generation, head, selected_parent, candidate, {}, [&] {
          EXPECT_TRUE(gate.exact_fence_is_held(query.configuration_fence_id, scope));
          EXPECT_THROW(gate.ReleaseFence(query.configuration_fence_id, scope), std::runtime_error);
          ++effects;
        });
  };
  EXPECT_NO_THROW(initial());
  EXPECT_EQ(effects, 1U);
  effects = 0U;
  // Absence is an actual candidate fact. A still-installed predecessor must
  // not pass merely because the candidate has no provider domains.
  EXPECT_THROW(handoff(query, parent, observed.active_public_pointer_digest), std::runtime_error);
  EXPECT_EQ(effects, 0U);
  current.reset();
  EXPECT_THROW(initial(), std::runtime_error);
  auto wrong_scope = query;
  wrong_scope.expected_scope.domains.clear();
  EXPECT_THROW(handoff(wrong_scope, parent, observed.active_public_pointer_digest), std::runtime_error);
  auto wrong_parent = parent;
  wrong_parent.claim_generation = core::CellAdmissionClaimGeneration(2U);
  EXPECT_THROW(handoff(query, wrong_parent, observed.active_public_pointer_digest), std::runtime_error);
  EXPECT_THROW(handoff(query, parent, core::ContentDigest(std::string(kSha))), std::runtime_error);
  EXPECT_EQ(effects, 0U);
  EXPECT_NO_THROW(handoff(query, parent, observed.active_public_pointer_digest));
  EXPECT_EQ(effects, 1U);
  actual_parent.reset();
  EXPECT_THROW(handoff(query, parent, observed.active_public_pointer_digest), std::runtime_error);
  EXPECT_EQ(effects, 1U);
  component_manager::SqliteActiveBundlePublicationStore publications(
      fixture.state_root.path() / std::string(component_manager::kActiveBundlePublicationStoreFilename),
      fixture.bundle->value().cell_graph.cell_id);
  const auto head = publications.ReadActiveBundlePublication(observed.execution_host_epoch.publication_generation);
  ASSERT_TRUE(head.ok());
  EXPECT_EQ(head.value().request.candidate_retained_bundle_id, fixture.bundle->retained_bundle_id());
  EXPECT_EQ(head.value().durable_public_pointer_content_digest, observed.active_public_pointer_digest);
}

TEST(ManagedKernelAdmissionGate,
     MixedProviderProofRetainsExactPhysicalConfigurationRequirement) {
  OwnerFixture fixture(false, true);
  const auto provider = ProviderFenceScope(ProviderReadback(fixture));
  auto mixed = AffectedScope();
  mixed.controllable_component_ids.insert(
      mixed.controllable_component_ids.end(),
      provider.controllable_component_ids.begin(),
      provider.controllable_component_ids.end());
  mixed.domains.insert(mixed.domains.end(), provider.domains.begin(),
                       provider.domains.end());
  std::sort(mixed.controllable_component_ids.begin(),
            mixed.controllable_component_ids.end());
  std::sort(mixed.domains.begin(), mixed.domains.end(),
            [](const auto &a, const auto &b) {
              return a.control_domain_id < b.control_domain_id;
            });
  ASSERT_TRUE(mixed.valid());
  ManagedKernelAdmissionGate gate;
  auto table = std::make_shared<core::ResourceSlotTable>(
      mixed.resources, ActiveConfigurations(), gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  const auto observed = gate.Observe(mixed);
  bool committed = false;
  EXPECT_THROW(gate.AcquireFenceDurably(
                   core::ConfigurationFenceId("fence.mixed-missing-controller"),
                   mixed, observed.operation_resource_generation, {},
                   [&] { committed = true; }),
               std::runtime_error);
  EXPECT_FALSE(committed);
  EXPECT_FALSE(gate.has_held_configuration_fences());
  gate.AcquireFenceDurably(core::ConfigurationFenceId("fence.mixed-exact"),
                           mixed, observed.operation_resource_generation,
                           ActiveConfigurations(), [&] { committed = true; });
  EXPECT_TRUE(committed);
}
