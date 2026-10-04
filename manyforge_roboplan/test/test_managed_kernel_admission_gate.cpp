#include "managed_kernel_apply_fence_owner.hpp"

#include <gtest/gtest.h>

#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_core/route_fence_record_codec_internal.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
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
  std::filesystem::copy(
      source, destination,
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
  OwnerFixture()
      : retained_root(std::filesystem::path(MANYFORGE_I6_NATIVE_FIXTURE_ROOT) /
                      "retained-package"),
        bundle_source(retained_root) {
    const auto manifest = deployment::parse_strict_json_model_yaml(
        ReadFile(std::filesystem::path(MANYFORGE_I6_NATIVE_FIXTURE_ROOT) /
                 "manifest.json"));
    bundle = bundle_source.get_verified(core::RetainedBundleId(
        TextMember(manifest.root(), "retained_bundle_id")));
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
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
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

} // namespace
