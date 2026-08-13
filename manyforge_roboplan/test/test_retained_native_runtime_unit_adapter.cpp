#include <gtest/gtest.h>

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/native_execution_runtime_unit_admission.hpp>
#include <manyforge_component_manager/retained_native_runtime_unit_adapter.hpp>
#include <manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp>
#include <manyforge_component_manager/runtime_unit_adapter_router.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace contract = manyforge::deployment::native_execution_runtime_contract;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::string_view kIntentDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[nodiscard]] core::LogicalSceneSnapshotRoot
MakeEmptyScene(const core::CellId &cell_id, const std::uint64_t revision_id) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U,
                     std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  auto scene = std::make_shared<core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{
          {core::logical_scene_identity::MintSceneId(
               {cell_id, core::BootstrapId("native_adapter_bootstrap"),
                core::BootstrapGeneration(1U)}),
           revision_id, core::ContentDigest(std::string(kZeroDigest))},
          cell_id,
          revision_id - 1U,
          0U,
          0U,
          std::move(policy),
          {},
          {},
      });
  scene->revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(*scene);
  if (!scene->locally_valid()) {
    throw std::logic_error("native adapter empty scene is invalid");
  }
  return core::LogicalSceneSnapshotRoot(std::move(scene));
}

class ScopedTemporaryRoot final {
public:
  ScopedTemporaryRoot() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-native-adapter-test-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("native adapter test mkdtemp failed");
    }
    path_ = created;
  }

  ~ScopedTemporaryRoot() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  ScopedTemporaryRoot(const ScopedTemporaryRoot &) = delete;
  ScopedTemporaryRoot &operator=(const ScopedTemporaryRoot &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

[[nodiscard]] std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("could not read native adapter fixture file");
  }
  return {std::istreambuf_iterator<char>(stream),
          std::istreambuf_iterator<char>()};
}

[[nodiscard]] const std::string &
TextMember(const deployment::CanonicalValue &root,
           const std::string_view name) {
  const auto *member = root.find_object_member(name);
  if (member == nullptr) {
    throw std::runtime_error("native adapter fixture manifest is incomplete");
  }
  const auto *text = std::get_if<std::string>(&member->storage());
  if (text == nullptr || text->empty()) {
    throw std::runtime_error(
        "native adapter fixture manifest field is invalid");
  }
  return *text;
}

struct NativeFixture final {
  explicit NativeFixture(
      std::filesystem::path fixture_root = MANYFORGE_I6_NATIVE_FIXTURE_ROOT)
      : root(std::move(fixture_root)),
        bundle_source(root / "retained-package"),
        artifact_source(root / "retained-package") {
    const auto manifest = deployment::parse_strict_json_model_yaml(
        ReadFile(root / "manifest.json"));
    bundle = bundle_source.get_verified(core::RetainedBundleId(
        TextMember(manifest.root(), "retained_bundle_id")));
    if (bundle == nullptr ||
        bundle->value().cell_graph.runtime_units.size() != 1U) {
      throw std::runtime_error(
          "native adapter fixture has no sole retained runtime");
    }
    const auto &unit = bundle->value().cell_graph.runtime_units.front();
    product = deployment::verify_runtime_unit_product(
        *bundle, unit.runtime_unit_id, artifact_source);
    if (!product.has_value() || product->product_contract_id().value() !=
                                    contract::kProductContractId) {
      throw std::runtime_error(
          "native adapter fixture has no exact native runtime product");
    }
    component_manager::RuntimeUnitConvergencePlanRowDTO plan{
        unit.runtime_unit_id,
        unit.runtime_profile_id,
        unit.runtime_profile_revision,
        core::RuntimeLifecycleControl::kManagerManaged,
        component_manager::RuntimeUnitCandidateDisposition::kRetainedExact,
        product->product_id(),
        product->product_contract_id(),
        product->product_contract_revision(),
        product->product_identity(),
        product->configuration_identity(),
        unit.dependency_runtime_unit_ids,
        {core::RuntimeRoleId(std::string(product->runtime_role_id()))},
        core::EndpointRoleId(std::string(product->status_endpoint_role_id())),
        std::string(product->status_endpoint_contract_id()),
        std::string(product->status_endpoint_contract_revision()),
        std::string(product->status_transport()),
        std::string(product->process_adapter_contract_id()),
        std::string(product->process_adapter_contract_revision()),
        std::string(product->retrieval_contract_id()),
        std::string(product->retrieval_contract_revision()),
        product->launchable_artifact_ref().content_digest,
        product->launchable_artifact_ref().byte_size,
        std::string(product->operating_system()),
        std::string(product->launch_environment_contract()),
    };
    query.emplace(component_manager::RuntimeUnitConvergenceQueryDTO{
        core::ApplyId("native-adapter-apply"),
        core::ApplyGeneration(1U),
        bundle->retained_bundle_id(),
        bundle->value().cell_graph.graph_revision,
        core::ContentDigest(std::string(kIntentDigest)),
        std::move(plan),
    });
    if (!query->valid()) {
      throw std::runtime_error("native adapter fixture query is invalid");
    }
  }

  std::filesystem::path root;
  ScopedTemporaryRoot staging_root;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source;
  deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::optional<deployment::VerifiedRetainedRuntimeUnitProduct> product;
  std::optional<component_manager::RuntimeUnitConvergenceQueryDTO> query;
};

class FixedActivePlanSource final
    : public component_manager::IManagerActiveRuntimePlanSource {
public:
  explicit FixedActivePlanSource(
      component_manager::ManagerActiveRuntimePlanDTO plan)
      : plan_(std::move(plan)) {}

  component_manager::ManagerActiveRuntimePlanDTO
  ReadCurrentPlan() const override {
    return plan_;
  }

private:
  component_manager::ManagerActiveRuntimePlanDTO plan_;
};

[[nodiscard]] component_manager::ManagerActiveRuntimePlanDTO
MakeActivePlan(const component_manager::RuntimeUnitConvergenceQueryDTO &query) {
  const auto *owner =
      std::get_if<component_manager::RuntimeUnitConvergenceApplyOwnerDTO>(
          &query.owner);
  if (owner == nullptr) {
    throw std::invalid_argument(
        "active Apply fixture received bootstrap owner");
  }
  const core::ContentDigest digest{std::string(kIntentDigest)};
  const core::ProjectionRevisionDTO projection{
      query.candidate_retained_bundle_id,
      query.candidate_graph_revision,
      core::SceneRevision{core::SceneId(std::string(kIntentDigest)), 1U,
                          digest},
  };
  component_manager::ManagerActiveRuntimePlanDTO plan{
      core::ActiveBundlePublicationReceiptDTO{
          core::ActiveBundlePublicationRequestDTO{
              core::NamedApplyPublicationTransactionDTO{
                  owner->apply_id, owner->apply_generation,
                  core::ProjectionStageGeneration(1U)},
              projection,
              query.candidate_retained_bundle_id,
              query.candidate_graph_revision,
              core::ActiveBundlePublicationGeneration(1U),
              digest,
              digest,
          },
          core::ActiveBundlePublicationGeneration(2U),
          digest,
      },
      query.runtime_convergence_intent_digest,
      {query.plan},
  };
  if (!plan.valid()) {
    throw std::logic_error("native adapter active plan is invalid");
  }
  return plan;
}

[[nodiscard]] component_manager::RetainedNativeRuntimeUnitAdapter
MakeAdapter(NativeFixture &fixture, const std::filesystem::path &target_root) {
  return component_manager::RetainedNativeRuntimeUnitAdapter(
      target_root, fixture.root / "retained-package",
      fixture.staging_root.path(), fixture.staging_root.path(),
      fixture.bundle_source, fixture.artifact_source,
      std::string(contract::kOperatingSystem),
      std::string(contract::kLaunchEnvironmentContract));
}

[[nodiscard]] core::ActiveBundlePublicationReceiptDTO
PublishActiveFixture(NativeFixture &fixture) {
  component_manager::SqliteActiveBundlePublicationStore store(
      fixture.staging_root.path() /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      fixture.bundle->value().cell_graph.cell_id);
  const core::ContentDigest digest{std::string(kIntentDigest)};
  const auto active_scene =
      MakeEmptyScene(fixture.bundle->value().cell_graph.cell_id, 1U);
  auto receipt = store.PublishActiveBundle({
      core::BootstrapPublicationTransactionDTO{
          core::BootstrapId("native-adapter-bootstrap"),
          core::BootstrapGeneration(1U)},
      core::ProjectionRevisionDTO{
          fixture.bundle->retained_bundle_id(),
          fixture.bundle->value().cell_graph.graph_revision,
          active_scene.shared()->revision},
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      std::nullopt,
      digest,
      digest,
  });
  store.Close();
  return receipt;
}

[[nodiscard]] core::ProjectionNativeRuntimeBindingDTO
NativeProjectionBinding(const NativeFixture &fixture,
                        const core::RuntimeEpoch &runtime_epoch) {
  const auto found = std::find_if(
      fixture.bundle->value().capability_provider_bindings.begin(),
      fixture.bundle->value().capability_provider_bindings.end(),
      [&](const auto &binding) {
        if (binding.capability_id.value() != "planning.motion.joint_goal" ||
            !core::RequiresNativeSceneProjection(
                binding.capability_id,
                binding.capability_definition_revision)) {
          return false;
        }
        return std::any_of(
            binding.endpoint_bindings.begin(), binding.endpoint_bindings.end(),
            [&](const auto &endpoint) {
              const auto *runtime_unit =
                  std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
              return endpoint.source_kind ==
                         core::CapabilityEndpointSourceKind::kRuntimeUnit &&
                     runtime_unit != nullptr &&
                     *runtime_unit == fixture.query->plan.runtime_unit_id;
            });
      });
  if (found == fixture.bundle->value().capability_provider_bindings.end()) {
    throw std::logic_error(
        "native adapter fixture has no projection provider binding");
  }
  return {
      found->capability_provider_binding_id,
      found->capability_provider_binding_revision,
      fixture.query->plan.runtime_unit_id,
      runtime_epoch,
  };
}

[[nodiscard]] std::vector<core::ProjectionNativeRuntimeBindingDTO>
NativeProjectionBindings(const NativeFixture &fixture,
                         const core::RuntimeEpoch &runtime_epoch) {
  std::vector<core::ProjectionNativeRuntimeBindingDTO> result;
  for (const auto &binding :
       fixture.bundle->value().capability_provider_bindings) {
    if (!core::RequiresNativeSceneProjection(
            binding.capability_id, binding.capability_definition_revision)) {
      continue;
    }
    const bool belongs_to_runtime = std::any_of(
        binding.endpoint_bindings.begin(), binding.endpoint_bindings.end(),
        [&](const auto &endpoint) {
          const auto *runtime_unit =
              std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
          return endpoint.source_kind ==
                     core::CapabilityEndpointSourceKind::kRuntimeUnit &&
                 runtime_unit != nullptr &&
                 *runtime_unit == fixture.query->plan.runtime_unit_id;
        });
    if (belongs_to_runtime) {
      result.push_back({binding.capability_provider_binding_id,
                        binding.capability_provider_binding_revision,
                        fixture.query->plan.runtime_unit_id, runtime_epoch});
    }
  }
  if (result.empty()) {
    throw std::logic_error(
        "native adapter fixture has no projection provider bindings");
  }
  return result;
}

[[nodiscard]] core::ConfigurationFenceScopeDTO
PhysicalConfigurationScope(const NativeFixture &fixture) {
  const auto &graph = fixture.bundle->value().cell_graph;
  std::set<core::ControllableComponentId> components;
  std::set<core::ControlDomainId> domain_ids;
  std::set<core::ResourceId> resources;
  std::vector<core::ConfigurationFenceBindingScopeDTO> bindings;
  for (const auto &binding : graph.control_bindings) {
    components.insert(binding.controllable_component_id);
    domain_ids.insert(binding.control_domain_id);
    bindings.push_back(core::ConfigurationFenceBindingScopeDTO{
        binding.control_binding_id, binding.control_binding_revision,
        binding.control_binding_revision});
    for (const auto &resource : binding.resource_bindings) {
      resources.insert(resource.resource_id);
    }
  }
  std::vector<core::ConfigurationFenceDomainScopeDTO> domains;
  for (const auto &domain_id : domain_ids) {
    const auto found =
        std::find_if(graph.control_domains.begin(), graph.control_domains.end(),
                     [&](const auto &domain) {
                       return domain.control_domain_id == domain_id;
                     });
    if (found == graph.control_domains.end()) {
      throw std::logic_error(
          "native adapter fixture control domain is unresolved");
    }
    domains.push_back(core::ConfigurationFenceDomainScopeDTO{
        domain_id, found->control_domain_revision,
        found->control_domain_revision});
  }
  std::sort(bindings.begin(), bindings.end(),
            [](const auto &left, const auto &right) {
              return left.control_binding_id < right.control_binding_id;
            });
  core::ConfigurationFenceScopeDTO scope{{components.begin(), components.end()},
                                         std::move(domains),
                                         std::move(bindings),
                                         {resources.begin(), resources.end()}};
  if (!scope.valid()) {
    throw std::logic_error(
        "native adapter physical configuration scope is invalid");
  }
  return scope;
}

[[nodiscard]] bool
WaitForRegularFile(const std::filesystem::path &path,
                   const std::chrono::steady_clock::duration timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    std::error_code error;
    if (std::filesystem::is_regular_file(path, error) && !error) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

TEST(NativeExecutionRuntimeUnitAdmission,
     SelectsOnlyTheBuildOwnedInstalledTargetAndFixedArguments) {
  NativeFixture fixture;
  const auto admission =
      component_manager::VerifyNativeExecutionRuntimeUnitForHost(
          *fixture.bundle, fixture.query->plan.runtime_unit_id,
          fixture.artifact_source, contract::kOperatingSystem,
          contract::kLaunchEnvironmentContract);
  ASSERT_TRUE(admission.has_value());
  EXPECT_EQ(admission->installed_target_id(), contract::kInstalledTargetId);
  EXPECT_EQ(admission->implementation_revision(),
            contract::implementation_revision());
  const auto arguments = admission->launch_arguments(
      3, core::RuntimeEpoch("native-runtime-test-epoch"));
  ASSERT_EQ(arguments.size(), 10U);
  EXPECT_EQ(arguments.front(), "--manyforge-managed-runtime-channel-fd");
  EXPECT_EQ(arguments[1], "3");
  EXPECT_EQ(arguments[2], "--runtime-unit-id");
  EXPECT_EQ(arguments[3], fixture.query->plan.runtime_unit_id.value());
  EXPECT_TRUE(std::none_of(arguments.begin(), arguments.end(),
                           [](const auto &argument) {
                             return argument.starts_with('/') ||
                                    argument.find('=') != std::string::npos;
                           }));

  EXPECT_THROW(static_cast<void>(
                   component_manager::VerifyNativeExecutionRuntimeUnitForHost(
                       *fixture.bundle, fixture.query->plan.runtime_unit_id,
                       fixture.artifact_source, "darwin",
                       contract::kLaunchEnvironmentContract)),
               component_manager::NativeExecutionRuntimeUnitAdmissionError);
  EXPECT_THROW(static_cast<void>(
                   component_manager::VerifyNativeExecutionRuntimeUnitForHost(
                       *fixture.bundle, fixture.query->plan.runtime_unit_id,
                       fixture.artifact_source, contract::kOperatingSystem,
                       "foreign.native.abi")),
               component_manager::NativeExecutionRuntimeUnitAdmissionError);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     DirectChildReportsClosedReadinessAndRestartMintsAnotherEpoch) {
  NativeFixture fixture;
  ASSERT_TRUE(fixture.product.has_value());
  EXPECT_FALSE(fixture.product->implementation_build_identity().has_value());
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);

  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  ASSERT_FALSE(started.process_incarnation().empty());
  const core::RuntimeEpoch first_epoch(started.process_incarnation());

  const auto first = adapter.ObserveProgress(*fixture.query, first_epoch);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->runtime_unit_id(),
            fixture.query->plan.runtime_unit_id.value());
  EXPECT_EQ(first->process_incarnation(), first_epoch.value());
  EXPECT_EQ(first->source_report_sequence(), 1U);
  EXPECT_EQ(first->critical_progress_sequence(), 1U);
  EXPECT_EQ(first->local_phase(),
            component_manager::RuntimeSourcePhase::kServing);
  EXPECT_EQ(first->source_health(),
            component_manager::RuntimeSourceHealth::kHealthy);
  EXPECT_EQ(first->product_identity(),
            fixture.query->plan.product_identity.value());
  EXPECT_EQ(first->configuration_identity(),
            fixture.query->plan.configuration_identity.value());
  EXPECT_EQ(first->ready_runtime_role_ids(),
            std::vector<std::string>{std::string(contract::kRuntimeRoleId)});
  EXPECT_EQ(
      first->command_surface_state(),
      component_manager::ManagedCommandSurfaceState::kClosedNoActiveEffect);

  const auto replay = adapter.EnsureRunning(*fixture.query);
  EXPECT_EQ(replay.process_incarnation(), first_epoch.value());
  const auto second = adapter.ObserveProgress(*fixture.query, first_epoch);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->source_report_sequence(), 2U);

  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_FALSE(
      adapter.ObserveProgress(*fixture.query, first_epoch).has_value());
  const auto restarted = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(restarted.state(),
            component_manager::ProcessPresenceState::kPresent);
  EXPECT_NE(restarted.process_incarnation(), first_epoch.value());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     RetainedBuildIdentityMatchesOnceAndRejectsTargetDigestMismatchBeforeLaunch) {
  NativeFixture fixture(MANYFORGE_I7_BUILD_IDENTIFIED_NATIVE_FIXTURE_ROOT);
  ASSERT_TRUE(fixture.product.has_value());
  ASSERT_TRUE(fixture.product->implementation_build_identity().has_value());

  ScopedTemporaryRoot target_root;
  const auto source =
      std::filesystem::path(MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT) /
      std::string(contract::kInstalledTargetId);
  const auto target =
      target_root.path() / std::string(contract::kInstalledTargetId);
  ASSERT_TRUE(std::filesystem::copy_file(
      source, target, std::filesystem::copy_options::none));
  std::filesystem::permissions(
      target, std::filesystem::status(source).permissions(),
      std::filesystem::perm_options::replace);
  const auto candidate_worker_source =
      std::filesystem::path(MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT) /
      std::string(contract::kCandidateWorkerInstalledTargetId);
  const auto candidate_worker_target =
      target_root.path() /
      std::string(contract::kCandidateWorkerInstalledTargetId);
  ASSERT_TRUE(std::filesystem::copy_file(
      candidate_worker_source, candidate_worker_target,
      std::filesystem::copy_options::none));
  std::filesystem::permissions(
      candidate_worker_target,
      std::filesystem::status(candidate_worker_source).permissions(),
      std::filesystem::perm_options::replace);

  auto adapter = MakeAdapter(fixture, target_root.path());
  EXPECT_EQ(adapter.EnsureRunning(*fixture.query).state(),
            component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);

  {
    std::fstream stream(target,
                        std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(stream.good());
    char first = 0;
    stream.read(&first, 1U);
    ASSERT_EQ(stream.gcount(), 1);
    first = static_cast<char>(static_cast<unsigned char>(first) ^ 0x01U);
    stream.seekp(0);
    stream.write(&first, 1U);
    ASSERT_TRUE(stream.good());
  }

  EXPECT_THROW(static_cast<void>(adapter.EnsureRunning(*fixture.query)),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     PrivateManagedChildOwnsDurableExactApplyFenceLifecycle) {
  NativeFixture fixture;
  const auto publication = PublishActiveFixture(fixture);
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);
  const auto first = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(first.state(), component_manager::ProcessPresenceState::kPresent);

  const core::RuntimeEpoch manager_epoch("manager-runtime-fence-test");
  const core::ConfigurationFenceAdmissionQueryDTO observation_query{
      {}, manager_epoch};
  const auto observation = adapter.ObserveAdmission(observation_query);
  ASSERT_TRUE(observation.matches(observation_query));
  EXPECT_EQ(observation.active_projection_revision,
            publication.request.candidate_projection_revision);
  EXPECT_EQ(observation.active_public_pointer_digest,
            publication.durable_public_pointer_content_digest);
  EXPECT_EQ(observation.configuration_fence_generation.value(), 0U);
  EXPECT_TRUE(observation.active_control_configurations.empty());

  const core::ApplyConfigurationFenceIntentDTO intent{
      core::PublicRequestContextDTO{
          core::RequestId("native-fence-request-1"),
          core::RequestOwnerEpoch("manager-owner-fence-test"),
          "retained-native-adapter-test", std::nullopt},
      core::ApplyFenceOwnerDTO{core::ApplyId("native-fence-apply-1"),
                               core::ApplyGeneration(1U)},
      {},
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      observation.operation_resource_generation,
      observation.configuration_fence_generation,
      {},
      "verify the direct-child durable fence lifecycle"};
  ASSERT_TRUE(intent.valid());
  const auto accepted = adapter.Acquire(intent);
  ASSERT_TRUE(accepted.matches(intent));
  EXPECT_EQ(accepted.replay_status, core::RequestReplayStatusDTO::kAcceptedNew);
  const auto replayed = adapter.Acquire(intent);
  EXPECT_EQ(replayed.configuration_fence_id, accepted.configuration_fence_id);
  EXPECT_EQ(replayed.configuration_fence_generation,
            accepted.configuration_fence_generation);
  EXPECT_EQ(replayed.replay_status,
            core::RequestReplayStatusDTO::kReplayedExact);

  auto competing_intent = intent;
  competing_intent.owner_request_context.request_id =
      core::RequestId("native-fence-request-2");
  competing_intent.owner_transaction.apply_id =
      core::ApplyId("native-fence-apply-2");
  competing_intent.expected_fence_generation =
      accepted.configuration_fence_generation;
  ASSERT_TRUE(competing_intent.valid());
  const auto competing = adapter.Acquire(competing_intent);
  EXPECT_EQ(competing.replay_status,
            core::RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_TRUE(competing.captured_scope.resources.empty());
  EXPECT_EQ(adapter.ObserveAdmission(observation_query)
                .configuration_fence_generation,
            competing.configuration_fence_generation);

  const core::ConfigurationFenceStatusQueryDTO competing_status_query{
      competing_intent.owner_transaction, competing.configuration_fence_id,
      competing.configuration_fence_generation, competing.captured_scope,
      manager_epoch};
  const auto competing_status = adapter.QueryStatus(competing_status_query);
  ASSERT_EQ(competing_status.state, core::ConfigurationFenceState::kHeld);
  const auto competing_release = adapter.Release(
      core::ConfigurationFenceReleaseDTO{
          competing_status.reconciliation_evidence, manager_epoch});
  EXPECT_EQ(competing_release.request.reconciliation_evidence,
            competing_status.reconciliation_evidence);

  const core::ApplyFenceAcquireAttemptQueryDTO attempt_query{
      intent, core::ContentDigest(std::string(kIntentDigest)), manager_epoch};
  const auto attempt = adapter.ReconcileAcquireAttempt(attempt_query);
  ASSERT_TRUE(attempt.matches(attempt_query));
  EXPECT_EQ(attempt.state,
            core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact);
  ASSERT_TRUE(attempt.accepted_fence.has_value());
  EXPECT_EQ(attempt.accepted_fence->configuration_fence_id,
            accepted.configuration_fence_id);

  const core::ApplyFenceRehydrateDTO rehydrate{
      intent.owner_transaction.apply_id,
      intent.owner_transaction.apply_generation,
      intent.owner_request_context.request_id,
      intent.owner_request_context.request_owner_epoch,
      core::ContentDigest(std::string(kIntentDigest)),
      intent.expected_retained_bundle_id,
      intent.expected_graph_revision,
      intent.expected_retained_bundle_id,
      intent.expected_graph_revision,
      accepted.configuration_fence_id,
      accepted.configuration_fence_generation,
      accepted.captured_scope,
      "fence_accepted",
      manager_epoch};
  const auto rehydrated = adapter.Rehydrate(rehydrate);
  ASSERT_TRUE(rehydrated.matches(rehydrate));
  EXPECT_FALSE(rehydrated.admission_fenced);
  EXPECT_EQ(rehydrated.fence_results.front().state,
            core::ApplyFenceReconcileState::kMatched);

  const core::ConfigurationFenceStatusQueryDTO status_query{
      intent.owner_transaction, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, accepted.captured_scope,
      manager_epoch};
  const auto held = adapter.QueryStatus(status_query);
  ASSERT_TRUE(held.matches(status_query));
  EXPECT_EQ(held.state, core::ConfigurationFenceState::kHeld);
  EXPECT_TRUE(held.admission_fenced);
  EXPECT_EQ(held.reconciliation_evidence.active_projection_revision,
            publication.request.candidate_projection_revision);

  static_cast<void>(adapter.EnsureStopped(*fixture.query));
  const auto second = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(second.state(), component_manager::ProcessPresenceState::kPresent);
  EXPECT_NE(second.process_incarnation(), first.process_incarnation());
  const auto restored_held = adapter.QueryStatus(status_query);
  ASSERT_TRUE(restored_held.matches(status_query));
  EXPECT_EQ(restored_held.state, core::ConfigurationFenceState::kHeld);
  EXPECT_TRUE(restored_held.admission_fenced);

  const core::ConfigurationFenceReleaseDTO release_request{
      restored_held.reconciliation_evidence, manager_epoch};
  const auto released = adapter.Release(release_request);
  ASSERT_TRUE(released.valid());
  EXPECT_EQ(released.request, release_request);
  const auto released_status = adapter.QueryStatus(status_query);
  ASSERT_TRUE(released_status.matches(status_query));
  EXPECT_EQ(released_status.state, core::ConfigurationFenceState::kReleased);
  ASSERT_TRUE(released_status.release_receipt.has_value());
  EXPECT_EQ(*released_status.release_receipt, released);

  static_cast<void>(adapter.EnsureStopped(*fixture.query));
  const auto third = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(third.state(), component_manager::ProcessPresenceState::kPresent);
  EXPECT_NE(third.process_incarnation(), second.process_incarnation());
  const auto restored_status = adapter.QueryStatus(status_query);
  ASSERT_TRUE(restored_status.matches(status_query));
  EXPECT_EQ(restored_status.state, core::ConfigurationFenceState::kReleased);
  EXPECT_EQ(adapter.Release(release_request), released);
  const auto next_observation = adapter.ObserveAdmission(observation_query);
  EXPECT_EQ(next_observation.configuration_fence_generation.value(), 2U);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     ExactIdentityResolverReturnsPrivateChildProjectionClient) {
  NativeFixture fixture;
  const auto publication = PublishActiveFixture(fixture);
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch first_epoch(started.process_incarnation());
  const auto binding = NativeProjectionBinding(fixture, first_epoch);

  const auto resolved = adapter.ResolveProjection(binding);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  const auto health = resolved.value()->ProjectionHealth();
  EXPECT_EQ(health.native_runtime_binding, binding);
  EXPECT_EQ(health.state, core::ProjectionHealthState::kUninitialized);
  EXPECT_FALSE(health.active_projection_revision.has_value());
  EXPECT_FALSE(health.staged_candidate.has_value());
  EXPECT_FALSE(health.error.has_value());
  EXPECT_GT(health.source_sequence, 0U);

  const auto initial_scene =
      MakeEmptyScene(fixture.bundle->value().cell_graph.cell_id, 1U);
  const core::ProjectionRevisionDTO initial_revision{
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      initial_scene.shared()->revision};
  EXPECT_EQ(initial_revision,
            publication.request.candidate_projection_revision);
  const core::NativeProjectionBootstrapStatusQueryDTO bootstrap_query{
      core::BootstrapId("native-adapter-bootstrap"),
      core::BootstrapGeneration(1U), binding, initial_revision};
  EXPECT_EQ(resolved.value()->BootstrapProjectionStatus(bootstrap_query).state,
            core::NativeProjectionBootstrapStatusState::kAbsent);
  // Prepare every projection required by the retained product before the
  // joint-goal projection makes the active composition buildable. This keeps
  // the asynchronous child from observing a partially initialized provider
  // set as a stale active composition.
  for (const auto &required_binding :
       NativeProjectionBindings(fixture, first_epoch)) {
    if (required_binding == binding) {
      continue;
    }
    const auto required = adapter.ResolveProjection(required_binding);
    ASSERT_TRUE(required.ok()) << required.error().message;
    const core::NativeProjectionBootstrapRebuildDTO required_request{
        bootstrap_query.bootstrap_id,
        bootstrap_query.bootstrap_generation,
        required_binding,
        initial_revision,
        fixture.bundle->value().compiled_products.planning_collision_model,
        initial_scene};
    const auto required_result =
        required.value()->RebuildBootstrapProjection(required_request);
    ASSERT_TRUE(required_result.presence_valid());
    EXPECT_TRUE(
        required_result.resulting_health.healthy_for_target(initial_revision));
  }
  const core::NativeProjectionBootstrapRebuildDTO bootstrap_request{
      bootstrap_query.bootstrap_id,
      bootstrap_query.bootstrap_generation,
      binding,
      initial_revision,
      fixture.bundle->value().compiled_products.planning_collision_model,
      initial_scene};
  const auto bootstrapped =
      resolved.value()->RebuildBootstrapProjection(bootstrap_request);
  ASSERT_TRUE(bootstrapped.presence_valid());
  EXPECT_EQ(bootstrapped.state,
            core::NativeProjectionBootstrapRebuildState::kRebuilt);
  EXPECT_TRUE(
      bootstrapped.resulting_health.healthy_for_target(initial_revision));
  EXPECT_TRUE(
      WaitForRegularFile(fixture.staging_root.path() /
                             std::string(core::kOperationJournalFilename),
                         std::chrono::seconds(10)))
      << "the managed child did not compose the active physical product";

  const auto physical_scope = PhysicalConfigurationScope(fixture);
  const core::RuntimeEpoch manager_epoch(
      "manager-runtime-physical-admission-test");
  const core::ConfigurationFenceAdmissionQueryDTO admission_query{
      physical_scope, manager_epoch};
  auto admission = adapter.ObserveAdmission(admission_query);
  const auto admission_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (admission.active_control_configurations.size() !=
             physical_scope.domains.size() &&
         std::chrono::steady_clock::now() < admission_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    admission = adapter.ObserveAdmission(admission_query);
  }
  ASSERT_TRUE(admission.matches(admission_query));
  ASSERT_EQ(admission.active_control_configurations.size(),
            physical_scope.domains.size());
  EXPECT_EQ(admission.active_projection_revision, initial_revision);
  EXPECT_EQ(admission.operation_resource_generation, 0U);
  EXPECT_EQ(admission.affected_queued_operation_count, 0U);
  EXPECT_EQ(admission.affected_active_operation_count, 0U);
  EXPECT_EQ(admission.active_control_configurations.size(),
            physical_scope.domains.size());
  const core::ApplyConfigurationFenceIntentDTO physical_fence_intent{
      core::PublicRequestContextDTO{
          core::RequestId("native-physical-fence-request-1"),
          core::RequestOwnerEpoch("manager-owner-physical-fence-test"),
          "retained-native-adapter-test", std::nullopt},
      core::ApplyFenceOwnerDTO{core::ApplyId("native-physical-fence-apply-1"),
                               core::ApplyGeneration(1U)},
      physical_scope,
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      admission.operation_resource_generation,
      admission.configuration_fence_generation,
      admission.active_control_configurations,
      "verify physical Kernel table and durable fence admission share one "
      "truth"};
  ASSERT_TRUE(physical_fence_intent.valid());
  const auto physical_fence = adapter.Acquire(physical_fence_intent);
  ASSERT_TRUE(physical_fence.matches(physical_fence_intent));
  const core::ConfigurationFenceStatusQueryDTO physical_status_query{
      physical_fence_intent.owner_transaction,
      physical_fence.configuration_fence_id,
      physical_fence.configuration_fence_generation,
      physical_fence.captured_scope, manager_epoch};
  const auto physical_held = adapter.QueryStatus(physical_status_query);
  ASSERT_TRUE(physical_held.matches(physical_status_query));
  EXPECT_EQ(physical_held.reconciliation_evidence.active_control_configurations,
            admission.active_control_configurations);
  const auto canonical_bundle = fixture.bundle->canonical_bytes();
  ASSERT_FALSE(canonical_bundle.empty());
  const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
      composition_request{
          initial_scene.shared()->revision.content_hash,
          physical_fence_intent.owner_transaction.apply_id,
          physical_fence_intent.owner_transaction.apply_generation,
          core::ProjectionStageGeneration(1U),
          initial_revision,
          publication.publication_generation,
          publication.durable_public_pointer_content_digest,
          core::ContentDigest(
              content_identity::sha256_digest(canonical_bundle)),
          static_cast<std::uint64_t>(canonical_bundle.size()),
          fixture.query->plan.runtime_unit_id,
          first_epoch,
          manager_epoch,
          physical_fence.configuration_fence_id,
          physical_fence.configuration_fence_generation,
          physical_fence.captured_scope,
      };
  ASSERT_TRUE(composition_request.valid());
  auto composition = adapter.InstallOrObserve(composition_request);
  const auto composition_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (composition.state ==
             component_manager::ManagerSelectedRuntimeCompositionState::
                 kPreparing &&
         std::chrono::steady_clock::now() < composition_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    composition = adapter.InstallOrObserve(composition_request);
  }
  ASSERT_EQ(
      composition.state,
      component_manager::ManagerSelectedRuntimeCompositionState::kInstalled);
  const auto physical_reconfigured = adapter.QueryStatus(physical_status_query);
  ASSERT_TRUE(physical_reconfigured.matches(physical_status_query));
  EXPECT_GT(
      physical_reconfigured.reconciliation_evidence
          .operation_resource_generation,
      physical_held.reconciliation_evidence.operation_resource_generation);
  const core::ConfigurationFenceReleaseDTO physical_release{
      physical_reconfigured.reconciliation_evidence, manager_epoch};
  EXPECT_TRUE(adapter.Release(physical_release).valid());

  ASSERT_FALSE(fixture.bundle->value().cell_graph.control_bindings.empty());
  const auto &control_binding =
      fixture.bundle->value().cell_graph.control_bindings.front();
  const core::ProjectionLeaseId frame_lease_id("native-adapter-frame-lease");
  const auto retained_binding = resolved.value()->RetainProjectionBinding(
      initial_revision, frame_lease_id);
  ASSERT_TRUE(retained_binding.ok()) << retained_binding.error().message;
  EXPECT_EQ(retained_binding.value(), (core::ProjectionLeaseBindingDTO{
                                          initial_revision, frame_lease_id}));
  const auto frame =
      resolved.value()->ResolveSceneFrame(core::SceneFrameResolutionQueryDTO{
          core::TargetCarrierSceneFrameRefDTO{
              control_binding.controllable_component_id,
              control_binding.command_target_id, core::FrameRoleId("tcp")},
          retained_binding.value()});
  ASSERT_TRUE(frame.ok()) << frame.error().message;
  ASSERT_TRUE(frame.value().valid());
  EXPECT_EQ(frame.value().projection_binding, retained_binding.value());
  const auto *component_frame =
      std::get_if<core::ResolvedComponentModelFrameDTO>(
          &frame.value().resolved_frame);
  ASSERT_NE(component_frame, nullptr);
  EXPECT_EQ(component_frame->controllable_component_id,
            control_binding.controllable_component_id);
  EXPECT_EQ(component_frame->frame_role_id, core::FrameRoleId("tcp"));

  const auto missing_frame =
      resolved.value()->ResolveSceneFrame(core::SceneFrameResolutionQueryDTO{
          core::TargetCarrierSceneFrameRefDTO{
              control_binding.controllable_component_id,
              control_binding.command_target_id,
              core::FrameRoleId("missing-frame-role")},
          retained_binding.value()});
  ASSERT_FALSE(missing_frame.ok());
  EXPECT_EQ(missing_frame.error().details.at("reason"),
            "scene_frame_reference_missing");
  resolved.value()->ReleaseProjection(frame_lease_id);
  const auto released_frame =
      resolved.value()->ResolveSceneFrame(core::SceneFrameResolutionQueryDTO{
          core::TargetCarrierSceneFrameRefDTO{
              control_binding.controllable_component_id,
              control_binding.command_target_id, core::FrameRoleId("tcp")},
          retained_binding.value()});
  ASSERT_FALSE(released_frame.ok());
  EXPECT_EQ(released_frame.error().details.at("reason"),
            "native_scene_frame_lease_unavailable");

  const auto effective_policy =
      resolved.value()->ProjectEffectiveCollisionPolicy(
          *initial_scene.shared(),
          initial_scene.shared()->revision.revision_id);
  ASSERT_TRUE(effective_policy.ok()) << effective_policy.error().message;
  EXPECT_EQ(effective_policy.value(),
            initial_scene.shared()->configured_collision_policy);

  auto candidate_scene =
      std::make_shared<core::LogicalSceneSnapshotDTO>(*initial_scene.shared());
  candidate_scene->revision.revision_id = 2U;
  candidate_scene->transaction_sequence = 1U;
  candidate_scene->revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(
          *candidate_scene);
  const core::LogicalSceneSnapshotRoot candidate_scene_root(candidate_scene);
  const core::ProjectionRevisionDTO candidate_revision{
      initial_revision.retained_bundle_id, initial_revision.graph_revision,
      candidate_scene->revision};
  const core::ProjectionTransitionRowDTO retained_row{
      binding, core::ProjectionTransitionDisposition::kRetained,
      initial_revision};
  const core::NativeProjectionStageDTO stage_request{
      core::ApplyId("native-adapter-transition"),
      core::ApplyGeneration(2U),
      core::ProjectionStageGeneration(1U),
      retained_row,
      initial_revision,
      candidate_revision,
      fixture.bundle->value().compiled_products.planning_collision_model,
      candidate_scene_root};
  const auto staged = resolved.value()->StageProjection(stage_request);
  ASSERT_TRUE(staged.presence_valid());
  EXPECT_EQ(staged.state, core::NativeProjectionStageResultState::kStaged);
  const core::NativeProjectionStatusQueryDTO stage_query{
      stage_request.apply_id, stage_request.apply_generation,
      stage_request.projection_stage_generation, retained_row};
  EXPECT_EQ(resolved.value()->ProjectionStageStatus(stage_query).state,
            core::NativeProjectionStatusState::kStaged);
  const core::NativeProjectionActivationDTO activation_request{
      stage_request.apply_id,
      stage_request.apply_generation,
      stage_request.projection_stage_generation,
      retained_row,
      initial_revision,
      candidate_revision};
  const auto activated =
      resolved.value()->ActivateStagedProjection(activation_request);
  ASSERT_TRUE(activated.presence_valid());
  EXPECT_EQ(activated.state,
            core::NativeProjectionActivationResultState::kActivated);
  EXPECT_TRUE(
      activated.resulting_health.healthy_for_target(candidate_revision));

  const core::ProjectionTransitionRowDTO removed_row{
      binding, core::ProjectionTransitionDisposition::kRemoved,
      candidate_revision};
  const core::NativeProjectionRetireDTO retire_request{
      core::ApplyId("native-adapter-retire"),
      core::ApplyGeneration(3U),
      core::ProjectionStageGeneration(2U),
      removed_row,
      candidate_revision,
      candidate_revision};
  EXPECT_EQ(resolved.value()
                ->ProjectionRetireStatus(
                    core::NativeProjectionRetireStatusQueryDTO{retire_request})
                .state,
            core::NativeProjectionRetireStatusState::kPending);
  const auto retired = resolved.value()->RetireProjection(retire_request);
  ASSERT_TRUE(retired.presence_valid());
  EXPECT_EQ(retired.state, core::NativeProjectionRetireResultState::kRetired);

  auto foreign = binding;
  foreign.provider_binding_revision =
      core::CapabilityProviderBindingRevision("foreign-binding-revision");
  EXPECT_FALSE(adapter.ResolveProjection(foreign).ok());

  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_THROW(static_cast<void>(resolved.value()->ProjectionHealth()),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_FALSE(adapter.ResolveProjection(binding).ok());

  const auto restarted = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(restarted.state(),
            component_manager::ProcessPresenceState::kPresent);
  const auto replacement = NativeProjectionBinding(
      fixture, core::RuntimeEpoch(restarted.process_incarnation()));
  const auto replacement_client = adapter.ResolveProjection(replacement);
  ASSERT_TRUE(replacement_client.ok()) << replacement_client.error().message;
  EXPECT_EQ(
      replacement_client.value()->ProjectionHealth().native_runtime_binding,
      replacement);
  for (const auto &required_binding : NativeProjectionBindings(
           fixture, core::RuntimeEpoch(restarted.process_incarnation()))) {
    if (required_binding == replacement) {
      continue;
    }
    const auto required = adapter.ResolveProjection(required_binding);
    ASSERT_TRUE(required.ok()) << required.error().message;
    const core::NativeProjectionRebuildDTO required_rebuild{
        required_binding,
        candidate_revision,
        fixture.bundle->value().compiled_products.planning_collision_model,
        candidate_scene_root.shared(),
    };
    const auto required_result =
        required.value()->RebuildProjection(required_rebuild);
    ASSERT_TRUE(required_result.presence_valid());
    EXPECT_TRUE(required_result.resulting_health.healthy_for_target(
        candidate_revision));
  }
  const core::NativeProjectionRebuildDTO replacement_rebuild{
      replacement,
      candidate_revision,
      fixture.bundle->value().compiled_products.planning_collision_model,
      candidate_scene_root.shared(),
  };
  const auto rebuilt =
      replacement_client.value()->RebuildProjection(replacement_rebuild);
  ASSERT_TRUE(rebuilt.presence_valid());
  EXPECT_EQ(rebuilt.result, core::ProjectionCommitResultStateDTO::kCommitted);
  EXPECT_FALSE(rebuilt.transaction_sequence.has_value());
  EXPECT_TRUE(rebuilt.resulting_health.healthy_for_target(candidate_revision));
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     RejectsForeignProductOrMissingFixedTargetBeforeStartingAChild) {
  NativeFixture fixture;
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);
  auto foreign = *fixture.query;
  foreign.plan.product_identity = core::ContentDigest(
      "sha256:"
      "2222222222222222222222222222222222222222222222222222222222222222");
  ASSERT_TRUE(foreign.valid());
  EXPECT_THROW(static_cast<void>(adapter.EnsureRunning(foreign)),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);

  ScopedTemporaryRoot empty_target_root;
  auto missing = MakeAdapter(fixture, empty_target_root.path());
  EXPECT_THROW(static_cast<void>(missing.EnsureRunning(*fixture.query)),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_EQ(missing.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);

  ScopedTemporaryRoot linked_target_root;
  std::filesystem::create_symlink(
      std::filesystem::path(MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT) /
          std::string(contract::kInstalledTargetId),
      linked_target_root.path() / std::string(contract::kInstalledTargetId));
  auto linked = MakeAdapter(fixture, linked_target_root.path());
  EXPECT_THROW(static_cast<void>(linked.EnsureRunning(*fixture.query)),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_EQ(linked.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

class RejectingRuntimeUnitAdapter final
    : public component_manager::IRuntimeUnitProcessPort,
      public component_manager::IRuntimeUnitProgressPort {
public:
  component_manager::ProcessPresenceObservation ObserveProcess(
      const component_manager::RuntimeUnitConvergenceQueryDTO &) override {
    ++calls;
    throw std::logic_error("foreign adapter was called");
  }

  component_manager::ProcessPresenceObservation EnsureRunning(
      const component_manager::RuntimeUnitConvergenceQueryDTO &) override {
    ++calls;
    throw std::logic_error("foreign adapter was called");
  }

  component_manager::ProcessPresenceObservation EnsureStopped(
      const component_manager::RuntimeUnitConvergenceQueryDTO &) override {
    ++calls;
    throw std::logic_error("foreign adapter was called");
  }

  std::optional<component_manager::RuntimeUnitSourceReport>
  ObserveProgress(const component_manager::RuntimeUnitConvergenceQueryDTO &,
                  const core::RuntimeEpoch &) override {
    ++calls;
    throw std::logic_error("foreign adapter was called");
  }

  std::size_t calls{};
};

TEST(RuntimeUnitAdapterRouter,
     DispatchesOnlyTheExactRegisteredContractAndRejectsUnknownBeforeEffect) {
  NativeFixture fixture;
  RejectingRuntimeUnitAdapter reference;
  auto native = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);
  component_manager::RuntimeUnitAdapterRouter router(
      {{std::string(component_manager::kRetainedPosixProcessAdapterContractId),
        &reference, &reference},
       {std::string(contract::kProcessAdapterContractId), &native, &native}});

  const auto started = router.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  EXPECT_TRUE(router.ObserveProgress(*fixture.query, epoch).has_value());
  EXPECT_EQ(router.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_EQ(reference.calls, 0U);

  auto unknown = *fixture.query;
  unknown.plan.process_adapter_contract_id = "manyforge.unknown_adapter";
  ASSERT_TRUE(unknown.valid());
  EXPECT_THROW(static_cast<void>(router.EnsureRunning(unknown)),
               component_manager::RuntimeUnitAdapterRouterError);
  EXPECT_EQ(reference.calls, 0U);
  EXPECT_EQ(native.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RuntimeUnitAdapterRouter,
     ProductionActiveCoordinatorStartsObservesAndStopsTheNativeChild) {
  NativeFixture fixture;
  RejectingRuntimeUnitAdapter reference;
  auto native = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);
  component_manager::RuntimeUnitAdapterRouter router(
      {{std::string(component_manager::kRetainedPosixProcessAdapterContractId),
        &reference, &reference},
       {std::string(contract::kProcessAdapterContractId), &native, &native}});
  FixedActivePlanSource source(MakeActivePlan(*fixture.query));
  std::int64_t now = 1'000'000;
  component_manager::ManagerActiveRuntimeCoordinator coordinator(
      source, router, router, [&now] { return ++now; });

  const auto converged = coordinator.Reconcile();
  EXPECT_EQ(
      converged.state,
      component_manager::ManagerActiveRuntimeConvergenceState::kConverged);
  ASSERT_EQ(converged.observations.observations().size(), 1U);
  EXPECT_TRUE(converged.observations.observations().front().converged());
  EXPECT_EQ(reference.calls, 0U);

  const auto stopped = coordinator.Shutdown();
  ASSERT_EQ(stopped.final_process_observations.size(), 1U);
  EXPECT_EQ(stopped.final_process_observations.front().state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_EQ(reference.calls, 0U);
}

} // namespace
} // namespace manyforge::roboplan_backend
