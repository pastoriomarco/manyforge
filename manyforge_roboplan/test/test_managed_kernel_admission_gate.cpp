#include "io_retained_fixture.hpp"
#include "canonical_empty_admission_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "managed_kernel_apply_fence_owner.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
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
                : std::filesystem::path(MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT) /
                      "retained-package"),
        bundle(io_provider
                   ? deployment::test::materialize_boolean_io_retained_fixture(retained_root).bundle
                   : nullptr),
        bundle_source(retained_root) {
    if (!io_provider) {
      const auto manifest = deployment::parse_strict_json_model_yaml(ReadFile(
          canonical_empty
              ? retained_root / "canonical-empty-admission.json"
              : std::filesystem::path(MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT) /
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

TEST(ManagedKernelAdmissionGate,
     DurableReleaseChecksActiveEffectBeforeCommit) {
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

} // namespace

TEST(ManagedKernelApplyFenceOwner,
     ReplacementOwnerWaitsForThePredecessorToReleaseTheLedger) {
  // A replacement execution host can start while the host of a killed
  // Manager is still tearing down: it owns the ledger only once the
  // predecessor's lock is released, instead of failing its start.
  OwnerFixture fixture(true);
  const auto config = fixture.Config("replacement");
  const auto lock_path = config.ledger_path.string() + ".lock";
  const int predecessor =
      ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  ASSERT_GE(predecessor, 0);
  ASSERT_EQ(::flock(predecessor, LOCK_EX | LOCK_NB), 0);
  ManagedKernelAdmissionGate gate;
  auto replacement = std::async(std::launch::async, [&] {
    return std::make_unique<ManagedKernelApplyFenceOwner>(config, gate);
  });
  EXPECT_EQ(replacement.wait_for(std::chrono::milliseconds(200)),
            std::future_status::timeout);
  ASSERT_EQ(::close(predecessor), 0);
  EXPECT_NE(replacement.get(), nullptr);
}

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
  // Status reports only installed configurations: none before install.
  EXPECT_TRUE(owner.QueryStatus(query).reconciliation_evidence.active_control_configurations.empty());
  unsigned effects = 0U;
  const auto prepare = [&](const auto &selected) {
    owner.RunWithExactHeldPredecessor(
        selected, observed.execution_host_epoch.publication_generation,
        observed.active_public_pointer_digest, [&] { ++effects; });
  };
  EXPECT_NO_THROW(prepare(query));
  EXPECT_EQ(effects, 1U);
  unsigned readbacks = 0U;
  std::vector<core::DomainActiveControlConfigurationDTO> rows;
  const auto readback = [&] {
    owner.RunWithExactHeldPredecessorReadback(
        query, observed.execution_host_epoch.publication_generation,
        observed.active_public_pointer_digest, [&](const auto &view) {
          EXPECT_TRUE(view.status().matches(query));
          rows = view.status().reconciliation_evidence.active_control_configurations;
          ++readbacks;
        });
  };
  EXPECT_NO_THROW(readback());
  EXPECT_TRUE(rows.empty()); // No candidate facts are invented before install.
  ASSERT_TRUE(table->reconfigure_runtime_inventory(scope.resources, ActiveConfigurations(), {}).ok());
  EXPECT_NO_THROW(readback());
  EXPECT_EQ(rows, ActiveConfigurations());
  EXPECT_EQ(readbacks, 2U);
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
     NeverEnabledPredecessorDomainPinsNoConfigurationButARoutedOneRefuses) {
  // OD-86: a Prepared-but-never-Enabled (or Disabled) component's domain has
  // neither a reconciled configuration nor a route publication. Its Apply
  // fence pins no row for it, as a release already does, so New/Load works.
  OwnerFixture fixture(true);
  ManagedKernelAdmissionGate gate;
  const auto scope = AffectedScope();
  ASSERT_TRUE(scope.domains.front().has_controller_predecessor());
  auto table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, std::vector<core::DomainActiveControlConfigurationDTO>{},
      gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  ManagedKernelApplyFenceOwner owner(fixture.Config("never-enabled"), gate);
  const core::RuntimeEpoch manager_epoch("manager.never-enabled");
  const auto observed = owner.ObserveAdmission({scope, manager_epoch});
  ASSERT_TRUE(observed.presence_valid());
  EXPECT_TRUE(observed.active_control_configurations.empty());
  const core::ApplyConfigurationFenceIntentDTO intent{
      {core::RequestId("request.never-enabled"),
       core::RequestOwnerEpoch("manager-owner.never-enabled"),
       "never-enabled-test", std::nullopt},
      {core::ApplyId("apply.never-enabled"), core::ApplyGeneration(1U)},
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      observed.operation_resource_generation,
      observed.configuration_fence_generation,
      {},
      "empty the cell while its robot was only prepared"};
  ASSERT_TRUE(intent.valid());
  const auto accepted = owner.Acquire(intent);
  ASSERT_TRUE(accepted.matches(intent));
  const core::ConfigurationFenceStatusQueryDTO query{
      intent.owner_transaction, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, scope, manager_epoch};
  EXPECT_TRUE(owner.QueryStatus(query)
                  .reconciliation_evidence.active_control_configurations.empty());
  gate.ReleaseFence(accepted.configuration_fence_id, scope);

  // Enabled: the reconciled configuration is pinned exactly, as before.
  ASSERT_TRUE(table->reconfigure_runtime_inventory(
                       scope.resources, ActiveConfigurations(),
                       ActiveConfigurations())
                  .ok());
  EXPECT_EQ(owner.ObserveAdmission({scope, manager_epoch})
                .active_control_configurations,
            ActiveConfigurations());

  // A domain that still has a route publication but no reconciled
  // configuration is live (e.g. an unreconciled external route): refuse.
  OwnerFixture routed_fixture(true);
  ManagedKernelAdmissionGate routed_gate;
  auto routed_table = std::make_shared<core::ResourceSlotTable>(
      scope.resources, std::vector<core::DomainActiveControlConfigurationDTO>{},
      routed_gate.shared_admission_mutex());
  ASSERT_TRUE(routed_table
                  ->reconfigure_runtime_inventory(scope.resources, {},
                                                  ActiveConfigurations())
                  .ok());
  routed_gate.BindResourceTable(routed_table);
  ManagedKernelApplyFenceOwner routed(routed_fixture.Config("routed"),
                                      routed_gate);
  const auto refuses = [](const auto &call) {
    try {
      call();
    } catch (const std::runtime_error &error) {
      return std::string_view(error.what()).find("route publication") !=
             std::string_view::npos;
    }
    return false;
  };
  EXPECT_TRUE(refuses(
      [&] { static_cast<void>(routed.ObserveAdmission({scope, manager_epoch})); }));
  auto routed_intent = intent;
  routed_intent.expected_retained_bundle_id =
      routed_fixture.bundle->retained_bundle_id();
  routed_intent.expected_graph_revision =
      routed_fixture.bundle->value().cell_graph.graph_revision;
  routed_intent.expected_operation_resource_generation =
      routed_gate.Observe(scope).operation_resource_generation;
  EXPECT_TRUE(refuses([&] { static_cast<void>(routed.Acquire(routed_intent)); }));
  EXPECT_FALSE(routed_gate.has_held_configuration_fences());
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
  EXPECT_TRUE(owner.QueryStatus(status_query)
                  .reconciliation_evidence.active_control_configurations.empty());

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

  // Release reconciliation reports the configurations actually installed.
  ASSERT_TRUE(table->reconfigure_runtime_inventory(scope.resources, ActiveConfigurations(), {}).ok());
  const auto complete = owner.QueryStatus(status_query);
  ASSERT_TRUE(complete.matches(status_query));
  EXPECT_EQ(complete.reconciliation_evidence.active_control_configurations,
            ActiveConfigurations());
  ASSERT_TRUE(complete.reconciliation_evidence.valid());
  const auto released = owner.Release({complete.reconciliation_evidence, manager_epoch});
  ASSERT_TRUE(released.valid());
  const auto after_release = owner.ReconcileAcquireAttempt(attempt);
  ASSERT_TRUE(after_release.matches(attempt));
  EXPECT_EQ(after_release.state, core::ApplyFenceAcquireAttemptReconcileState::kConflicting);
  EXPECT_FALSE(after_release.accepted_fence);
  EXPECT_FALSE(gate.exact_fence_is_held(accepted.configuration_fence_id, scope));
}

TEST(ManagedKernelApplyFenceOwner,
     BeforeEffectReleaseNeedsNoConfigurationForDomainsNewToTheCell) {
  // The execution host refused the Apply before any effect: the Manager
  // releases with the held status, which must equal the predecessor rows the
  // fence pinned. Domains new to the cell have no active configuration and
  // nothing to restore, so they must not block the release.
  const auto new_domains = [](core::ConfigurationFenceScopeDTO scope, const bool all) {
    for (std::size_t index = all ? 0U : 1U; index < scope.domains.size(); ++index) {
      scope.domains[index].predecessor_control_domain_revision.reset();
      scope.bindings[index].predecessor_control_binding_revision.reset();
    }
    return scope;
  };
  auto mixed = AffectedScope();
  const auto other = DisjointScope();
  mixed.controllable_component_ids.push_back(other.controllable_component_ids.front());
  mixed.domains.push_back(other.domains.front());
  mixed.bindings.push_back(other.bindings.front());
  mixed.resources.insert(mixed.resources.end(), other.resources.begin(), other.resources.end());
  struct Case final {
    const char *name;
    core::ConfigurationFenceScopeDTO scope;
    std::vector<core::DomainActiveControlConfigurationDTO> installed;
  };
  for (const auto &[name, scope, installed] :
       {Case{"all-new", new_domains(AffectedScope(), true), {}},
        Case{"one-new", new_domains(mixed, false), ActiveConfigurations()}}) {
    SCOPED_TRACE(name);
    ASSERT_TRUE(scope.valid());
    OwnerFixture fixture(true);
    ManagedKernelAdmissionGate gate;
    auto table = std::make_shared<core::ResourceSlotTable>(scope.resources, installed,
                                                           gate.shared_admission_mutex());
    gate.BindResourceTable(table);
    ManagedKernelApplyFenceOwner owner(fixture.Config(name), gate);
    const core::RuntimeEpoch manager_epoch(std::string("manager.before-effect.") + name);
    const auto observed = owner.ObserveAdmission({scope, manager_epoch});
    ASSERT_TRUE(observed.presence_valid());
    ASSERT_EQ(observed.active_control_configurations, installed);
    const core::ApplyConfigurationFenceIntentDTO intent{
        {core::RequestId("request.before-effect"),
         core::RequestOwnerEpoch("manager-owner.before-effect"), "before-effect-test",
         std::nullopt},
        {core::ApplyId("apply.before-effect"), core::ApplyGeneration(1U)}, scope,
        core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
        fixture.bundle->retained_bundle_id(), fixture.bundle->value().cell_graph.graph_revision,
        observed.operation_resource_generation, observed.configuration_fence_generation,
        installed, "Load refused by the execution host before any effect"};
    const auto accepted = owner.Acquire(intent);
    ASSERT_TRUE(accepted.matches(intent));
    EXPECT_TRUE(gate.exact_fence_is_held(accepted.configuration_fence_id, scope));
    const core::ConfigurationFenceStatusQueryDTO query{
        intent.owner_transaction, accepted.configuration_fence_id,
        accepted.configuration_fence_generation, scope, manager_epoch};
    const auto held = owner.QueryStatus(query);
    ASSERT_TRUE(held.matches(query));
    EXPECT_EQ(held.state, core::ConfigurationFenceState::kHeld);
    EXPECT_TRUE(held.admission_fenced);
    // The Manager's before-effect check: the predecessor is unchanged.
    EXPECT_EQ(held.reconciliation_evidence.active_control_configurations,
              intent.expected_active_control_configurations);
    const core::ConfigurationFenceReleaseDTO release{held.reconciliation_evidence, manager_epoch};
    ASSERT_TRUE(release.valid());
    const auto released = owner.Release(release);
    EXPECT_TRUE(released.valid());
    EXPECT_EQ(released.request, release);
    EXPECT_FALSE(gate.exact_fence_is_held(accepted.configuration_fence_id, scope));
    EXPECT_EQ(owner.QueryStatus(query).state, core::ConfigurationFenceState::kReleased);
  }
}

// The durable fence ledger is bounded by eviction, never refusal. One
// fence stays held while 256 + 8 further Apply fences are acquired and
// released: every Acquire succeeds, the oldest released fences are evicted,
// and the held fence survives.
TEST(ManagedKernelApplyFenceOwner, LedgerEvictsOldestReleasedFenceAtItsBound) {
  const auto all_new = [](core::ConfigurationFenceScopeDTO scope) {
    for (auto &domain : scope.domains)
      domain.predecessor_control_domain_revision.reset();
    for (auto &binding : scope.bindings)
      binding.predecessor_control_binding_revision.reset();
    return scope;
  };
  const auto held_scope = all_new(DisjointScope());
  const auto cycled_scope = all_new(AffectedScope());
  auto resources = cycled_scope.resources;
  resources.insert(resources.end(), held_scope.resources.begin(),
                   held_scope.resources.end());
  OwnerFixture fixture(true);
  ManagedKernelAdmissionGate gate;
  auto table = std::make_shared<core::ResourceSlotTable>(
      resources, std::vector<core::DomainActiveControlConfigurationDTO>{},
      gate.shared_admission_mutex());
  gate.BindResourceTable(table);
  const auto config = fixture.Config("ledger-bound");
  ManagedKernelApplyFenceOwner owner(config, gate);
  const core::RuntimeEpoch manager_epoch("manager.ledger-bound");
  const auto acquire = [&](const std::string &name,
                           const core::ConfigurationFenceScopeDTO &scope) {
    const auto observed = owner.ObserveAdmission({scope, manager_epoch});
    const core::ApplyConfigurationFenceIntentDTO intent{
        {core::RequestId("request." + name),
         core::RequestOwnerEpoch("manager-owner.ledger-bound"),
         "ledger-bound-test", std::nullopt},
        {core::ApplyId("apply." + name), core::ApplyGeneration(1U)},
        scope,
        core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
        fixture.bundle->retained_bundle_id(),
        fixture.bundle->value().cell_graph.graph_revision,
        observed.operation_resource_generation,
        observed.configuration_fence_generation,
        {},
        "ledger bound"};
    const auto accepted = owner.Acquire(intent);
    EXPECT_TRUE(accepted.matches(intent));
    return core::ConfigurationFenceStatusQueryDTO{
        intent.owner_transaction, accepted.configuration_fence_id,
        accepted.configuration_fence_generation, scope, manager_epoch};
  };
  const auto held = acquire("held", held_scope);
  constexpr std::size_t kCycles = 256U + 8U;
  std::optional<core::ConfigurationFenceStatusQueryDTO> first, last;
  for (std::size_t index = 0U; index < kCycles; ++index) {
    SCOPED_TRACE(index);
    const auto query = acquire("cycled." + std::to_string(index), cycled_scope);
    const auto status = owner.QueryStatus(query);
    ASSERT_EQ(status.state, core::ConfigurationFenceState::kHeld);
    static_cast<void>(
        owner.Release({status.reconciliation_evidence, manager_epoch}));
    if (!first)
      first = query;
    last = query;
  }
  EXPECT_EQ(owner.QueryStatus(held).state,
            core::ConfigurationFenceState::kHeld);
  EXPECT_EQ(owner.QueryStatus(*last).state,
            core::ConfigurationFenceState::kReleased);
  // The oldest released fence was evicted.
  EXPECT_THROW(static_cast<void>(owner.QueryStatus(*first)),
               std::runtime_error);
  EXPECT_TRUE(gate.exact_fence_is_held(held.configuration_fence_id,
                                       held_scope));
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
/// The sealed fixture's I/O provider domain on both sides of the Apply, fenced
/// by the given output resources (sorted).
core::ConfigurationFenceScopeDTO
ProviderFenceScope(const OwnerFixture &fixture,
                   std::vector<core::ResourceId> outputs = {}) {
  const auto &graph = fixture.bundle->value().cell_graph;
  const auto found = std::find_if(
      graph.control_domains.begin(), graph.control_domains.end(),
      [](const auto &domain) { return domain.provider() != nullptr; });
  if (found == graph.control_domains.end())
    throw std::logic_error("sealed I/O fixture has no provider domain");
  const auto &provider = *found->provider();
  const core::ProviderConfigurationFenceOccurrenceDTO occurrence{
      provider.controllable_component_id, provider.provider_instance_id,
      provider.signal_configuration_digest, outputs};
  return {{provider.controllable_component_id},
          {{found->control_domain_id, found->control_domain_revision,
            found->control_domain_revision,
            core::ProviderConfigurationFenceDomainDTO{occurrence, occurrence}}},
          {},
          outputs};
}
} // namespace

TEST(ManagedKernelApplyFenceOwner, IoProviderDomainIsFencedByResource) {
  OwnerFixture fixture(false, true);
  const core::ResourceId output("io.output.1");
  const auto scope = ProviderFenceScope(fixture, {output});
  ASSERT_TRUE(scope.valid());
  ManagedKernelAdmissionGate gate;
  ManagedKernelApplyFenceOwner owner(fixture.Config("provider-by-resource"), gate);
  EXPECT_TRUE(gate.current_fenced_resources({output}).empty());
  const auto observed =
      owner.ObserveAdmission({scope, core::RuntimeEpoch("manager.provider")});
  EXPECT_TRUE(observed.presence_valid());
  const core::ApplyConfigurationFenceIntentDTO intent{
      {core::RequestId("provider.fence"),
       core::RequestOwnerEpoch("manager.owner"), "test", std::nullopt},
      {core::ApplyId("provider.apply"), core::ApplyGeneration(1U)},
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      observed.operation_resource_generation,
      observed.configuration_fence_generation,
      {},
      "Apply over an I/O provider"};
  ASSERT_TRUE(intent.valid());
  const auto accepted = owner.Acquire(intent);
  EXPECT_EQ(gate.current_fenced_resources({output}),
            std::vector<core::ResourceId>{output});
  const core::ConfigurationFenceStatusQueryDTO query{
      intent.owner_transaction, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, scope,
      observed.echoed_query.manager_runtime_epoch};
  const auto status = owner.QueryStatus(query);
  EXPECT_EQ(status.state, core::ConfigurationFenceState::kHeld);
  EXPECT_TRUE(status.admission_fenced);
  unsigned effects = 0U;
  EXPECT_NO_THROW(owner.RunWithExactHeldPredecessor(
      query, observed.execution_host_epoch.publication_generation,
      observed.active_public_pointer_digest, [&] { ++effects; }));
  EXPECT_EQ(effects, 1U);
}

TEST(ManagedKernelApplyFenceOwner,
     HeldApplyFenceAndStatusFollowTheCandidateProviderBeforePublication) {
  OwnerFixture fixture(false, true);
  // An added provider: the predecessor side is absent.
  auto scope = ProviderFenceScope(fixture);
  auto &domain = scope.domains.front();
  domain.predecessor_control_domain_revision.reset();
  std::get<core::ProviderConfigurationFenceDomainDTO>(domain.payload)
      .predecessor_provider.reset();
  ASSERT_TRUE(scope.valid());
  ManagedKernelAdmissionGate gate;
  ManagedKernelApplyFenceOwner owner(fixture.Config("provider-candidate"), gate);
  const core::RuntimeEpoch manager_epoch("manager.provider-candidate");
  const auto observed = owner.ObserveAdmission({scope, manager_epoch});
  const core::ApplyConfigurationFenceIntentDTO intent{
      {core::RequestId("provider.candidate.fence"),
       core::RequestOwnerEpoch("manager.owner"), "test", std::nullopt},
      {core::ApplyId("provider.candidate"), core::ApplyGeneration(1U)},
      scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      observed.operation_resource_generation,
      observed.configuration_fence_generation,
      {},
      "add a provider"};
  ASSERT_TRUE(intent.valid());
  const core::NamedApplyPublicationTransactionDTO apply{
      intent.owner_transaction.apply_id, intent.owner_transaction.apply_generation};
  EXPECT_FALSE(owner.HeldApplyFence(apply, manager_epoch));
  const auto accepted = owner.Acquire(intent);
  const auto query = owner.HeldApplyFence(apply, manager_epoch);
  ASSERT_TRUE(query);
  EXPECT_EQ(*query, (core::ConfigurationFenceStatusQueryDTO{
                        intent.owner_transaction, accepted.configuration_fence_id,
                        accepted.configuration_fence_generation, scope, manager_epoch}));
  EXPECT_FALSE(owner.HeldApplyFence(
      {core::ApplyId("other.apply"), core::ApplyGeneration(1U)}, manager_epoch));
  const auto status = owner.QueryStatus(*query);
  EXPECT_EQ(status.state, core::ConfigurationFenceState::kHeld);
  EXPECT_TRUE(status.admission_fenced);
  unsigned readbacks = 0U;
  EXPECT_NO_THROW(owner.RunWithExactHeldPredecessorReadback(
      *query, observed.execution_host_epoch.publication_generation,
      observed.active_public_pointer_digest, [&](const auto &) { ++readbacks; }));
  EXPECT_EQ(readbacks, 1U);
}

TEST(ManagedKernelAdmissionGate,
     MixedProviderProofRetainsExactPhysicalConfigurationRequirement) {
  OwnerFixture fixture(false, true);
  const auto provider = ProviderFenceScope(fixture);
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
