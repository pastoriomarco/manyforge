#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/execution_host_exit.hpp>
#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_fence_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/manager_runtime_convergence_coordinator.hpp>
#include <manyforge_component_manager/execution_host_unit_admission.hpp>
#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_component_manager/retained_execution_host_unit_adapter.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_core/types/request_key.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/retained_execution_host.hpp>
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "../../manyforge_component_manager/src/kernel_apply_fence_rpc_internal.hpp"
#include "../../manyforge_component_manager/src/manager_liveness_rpc_internal.hpp"
#include "../../manyforge_component_manager/src/native_projection_rpc_internal.hpp"
#include "../../manyforge_component_manager/src/selected_runtime_composition_rpc_internal.hpp"
#include "../src/ros_managed_runtime_host.hpp"

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace contract = manyforge::deployment::ros2_control_execution_runtime_contract;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;
namespace scene = manyforge::scene;
using content_identity::CanonicalValue;

// One application-start identity for every adapter in this suite.
constexpr core::ApplicationStartId kTestStartId{0x3f9a00000000c0deULL};
constexpr std::string_view kTestStartIdText = "3f9a00000000c0de";

// The suite's N-th distinct request key (contract 1.4).
constexpr core::RequestKey TestRequestKey(const std::uint64_t sequence) {
  return core::RequestKey::Make(
      core::RequestKeyClass::kInternal, 0U,
      static_cast<std::uint64_t>(core::RequestKeyMinter::kManager), sequence);
}

constexpr std::string_view kIntentDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

TEST(RetainedExecutionHostExit,
     ShutdownIncompleteStatusUsesDedicatedCatalogReasonOnly) {
  EXPECT_EQ(component_manager::execution_host_child_exit_reason(
                component_manager::kExecutionHostShutdownIncompleteExitStatus),
            "execution_host_shutdown_incomplete");
  EXPECT_EQ(component_manager::execution_host_child_exit_reason(0),
            "execution_host_child_exited");
  EXPECT_EQ(component_manager::execution_host_child_exit_reason(1),
            "execution_host_child_exited");
  EXPECT_EQ(component_manager::execution_host_child_exit_reason(std::nullopt),
            "execution_host_child_exited");
}

// The prepare builder stores a construction throw as the typed refusal. The
// main loop keeps running for it (other stored failures stay fatal), and the
// next Prepare poll replies with its code and cause over the actual RPC codec.
TEST(ManagedRuntimeCompositionRefusal, BuilderThrowRefusesApplyKeepsRuntimeAndCarriesCause) {
  namespace rpc = component_manager::detail;
  const auto build = [](const auto &construct) {
    std::exception_ptr stored;
    std::jthread builder([&] {
      try {
        try {
          construct();
        } catch (const std::exception &error) {
          throw host_detail::ScenarioCompositionRefusal(error, "arm.1");
        }
      } catch (...) {
        stored = std::current_exception();
      }
    });
    builder.join();
    return stored;
  };
  const auto reply = [](const std::exception_ptr &stored) {
    try {
      std::rethrow_exception(stored);
    } catch (const std::exception &error) {
      const auto decoded = rpc::DecodeSelectedRuntimeCompositionRpcResponse(
          rpc::EncodeSelectedRuntimeCompositionRpcResponse(
              rpc::SelectedRuntimeCompositionRpcFailure{
                  host_detail::SelectedRuntimeCompositionRpcError(error)}));
      return std::get<rpc::SelectedRuntimeCompositionRpcFailure>(decoded).error;
    }
  };

  const auto refused = build([] {
    throw std::invalid_argument("RoboPlan addressed planner construction is inconsistent");
  });
  EXPECT_TRUE(host_detail::IsScenarioCompositionRefusal(refused));
  const auto error = reply(refused);
  EXPECT_EQ(error.code, component_manager::kScenarioCompositionRefusedCode);
  EXPECT_EQ(error.message,
            "Scenario refused: RoboPlan addressed planner construction is inconsistent");
  EXPECT_EQ(error.details,
            (std::map<std::string, std::string>{
                {"cause", "RoboPlan addressed planner construction is inconsistent"},
                {"component_id", "arm.1"}}));

  // A typed inner cause keeps its own code; a long cause fits the Manager's
  // retained bounds on a UTF-8 boundary.
  std::string utf8_cause = "x";
  for (int index = 0; index < 400; ++index) utf8_cause += "\xC3\xA9";
  const auto typed = build([&] {
    throw core::TypedError("pneumatic_io_unsupported_on_isaac", utf8_cause);
  });
  const auto bounded = reply(typed);
  EXPECT_EQ(bounded.details.at("cause_code"), "pneumatic_io_unsupported_on_isaac");
  EXPECT_LE(bounded.details.at("cause").size(),
            component_manager::kMaximumManagerApplyFailureCauseDetailValueBytes);
  EXPECT_EQ(bounded.details.at("cause"),
            utf8_cause.substr(0U, bounded.details.at("cause").size()));
  EXPECT_EQ(bounded.details.at("cause").size() % 2U, 1U);
  EXPECT_LE(bounded.message.size(),
            component_manager::kMaximumManagerApplyFailureMessageBytes);

  // Any other stored failure stays fatal and keeps the generic RPC code.
  const auto fatal = std::make_exception_ptr(
      std::runtime_error("actual_installed_state_unknown: restart ManyForge"));
  EXPECT_FALSE(host_detail::IsScenarioCompositionRefusal(fatal));
  EXPECT_FALSE(host_detail::IsScenarioCompositionRefusal(nullptr));
  const auto generic = reply(fatal);
  EXPECT_EQ(generic.code, "selected_runtime_composition_request_failed");
  EXPECT_EQ(generic.message,
            "actual_installed_state_unknown: restart ManyForge");
  EXPECT_TRUE(generic.details.empty());
}

TEST(ManagedRuntimeCompositionRefusal, DirectControlTargetCodesSurviveRpcCodec) {
  namespace rpc = component_manager::detail;
  for (const std::string code : {"direct_control_primary_target_missing",
                                 "direct_control_primary_target_ambiguous"}) {
    const core::TypedError failure(code, "direct-control target unavailable");
    const auto mapped = host_detail::SelectedRuntimeCompositionRpcError(failure);
    const auto decoded = rpc::DecodeSelectedRuntimeCompositionRpcResponse(
        rpc::EncodeSelectedRuntimeCompositionRpcResponse(
            rpc::SelectedRuntimeCompositionRpcFailure{mapped}));
    const auto &error = std::get<rpc::SelectedRuntimeCompositionRpcFailure>(decoded).error;
    EXPECT_EQ(error.code, "selected_runtime_composition_request_failed");
    ASSERT_TRUE(error.details.contains("code"));
    EXPECT_EQ(error.details.at("code"), code);
  }
}

// A host Apply failure caught before anything installed changed is a
// failed result that says `effect: none` over the actual codec, so the
// Manager releases the Apply (the predecessor is still installed).
TEST(ManagedRuntimeCompositionRefusal, FailureBeforeEffectSaysEffectNoneOverTheCodec) {
  namespace rpc = component_manager::detail;
  const auto failure = host_detail::HostApplyFailureBeforeEffect(std::make_exception_ptr(
      std::runtime_error("retained native owner differs from the planned predecessor")));
  const auto decoded = rpc::DecodeSelectedRuntimeCompositionRpcResponse(
      rpc::EncodeSelectedRuntimeCompositionRpcResponse(component_manager::HostApplyResult{
          component_manager::HostApplyOutcome::kFailed, failure}));
  const auto &result = std::get<component_manager::HostApplyResult>(decoded);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "selected_runtime_composition_request_failed");
  EXPECT_EQ(result.error->message, "retained native owner differs from the planned predecessor");
  EXPECT_EQ(result.error->details,
            (std::map<std::string, std::string>{{"effect", "none"}}));
  // A non-standard throw keeps the generic code and still says so.
  const auto unknown = host_detail::HostApplyFailureBeforeEffect(std::make_exception_ptr(7));
  EXPECT_EQ(unknown.code, "selected_runtime_composition_request_failed");
  EXPECT_EQ(unknown.details.at("effect"), "none");
}

// A leaving component still enabled because its Disable failed is
// accepted (its process is stopped like a dead one); an enabled one without
// a failed Disable, or one whose action runs or that moves, is still refused.
TEST(ManagedRuntimeCompositionRefusal, LeavingComponentWithAFailedDisableDoesNotRefuseTheApply) {
  core::ComponentState state;
  state.component_id = "robot.1";
  state.lifecycle = core::ComponentLifecycle::kEnabled;
  state.activity = core::ComponentActivity::kIdle;
  EXPECT_TRUE(host_detail::LeavingComponentRefusesApply(state));
  state.last_action = core::ActionResult{};
  state.last_action->action = core::ComponentAction::kEnable;
  state.last_action->outcome = core::ActionOutcome::kSucceeded;
  EXPECT_TRUE(host_detail::LeavingComponentRefusesApply(state));
  state.last_action->action = core::ComponentAction::kDisable;
  state.last_action->outcome = core::ActionOutcome::kFailed;
  EXPECT_FALSE(host_detail::LeavingComponentRefusesApply(state));
  auto running = state;
  running.action = core::ComponentActionInProgress{core::ComponentAction::kDisable, 1};
  EXPECT_TRUE(host_detail::LeavingComponentRefusesApply(running));
  auto moving = state;
  moving.activity = core::ComponentActivity::kBusy;
  EXPECT_TRUE(host_detail::LeavingComponentRefusesApply(moving));
  // Dead or faulted components were accepted before and still are.
  auto faulted = core::ComponentState{};
  faulted.lifecycle = core::ComponentLifecycle::kOffline;
  EXPECT_FALSE(host_detail::LeavingComponentRefusesApply(faulted));
}

// An unresolved site address is typed and names its component; the
// composition refusal keeps its code and that component.
TEST(ManagedRuntimeCompositionRefusal, SiteAddressRefusalKeepsCodeAndComponent) {
  const auto site = host_detail::SiteAddressRefusal(
      core::ErrorDTO{"site_settings_unavailable",
                     "no site settings file is configured for this machine",
                     {{"device_address_ref", "lite6_main"}}},
      "lite6_robot");
  EXPECT_EQ(site.code(), "site_settings_unavailable");
  EXPECT_STREQ(site.what(),
               "component lite6_robot: no site settings file is configured for this machine");
  EXPECT_EQ(site.details().at("component_id"), "lite6_robot");
  EXPECT_EQ(site.details().at("device_address_ref"), "lite6_main");

  const auto refusal = host_detail::ScenarioCompositionRefusal(site).error();
  EXPECT_EQ(refusal.code, component_manager::kScenarioCompositionRefusedCode);
  EXPECT_EQ(refusal.details,
            (std::map<std::string, std::string>{
                {"cause",
                 "component lite6_robot: no site settings file is configured for this machine"},
                {"cause_code", "site_settings_unavailable"},
                {"component_id", "lite6_robot"}}));
}

class FixedPlanningSceneSynchronizer final
    : public core::IPlanningSceneSynchronizer {
public:
  explicit FixedPlanningSceneSynchronizer(const std::uint64_t revision_id)
      : revision_{core::RetainedBundleId("bundle"),
                  core::GraphRevision("graph"),
                  core::SceneRevision{
                      core::SceneId(std::string(kIntentDigest)), revision_id,
                      core::ContentDigest(std::string(kIntentDigest))}} {}

  [[nodiscard]] core::Result<core::ProjectionRevisionDTO>
  SynchronizeBeforePlanning() override {
    ++calls_;
    return core::Result<core::ProjectionRevisionDTO>::Ok(revision_);
  }

  [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

private:
  core::ProjectionRevisionDTO revision_;
  std::size_t calls_{};
};

class TaggedReferencePoseResolver final : public core::IReferencePoseResolver {
public:
  explicit TaggedReferencePoseResolver(std::string tag)
      : tag_(std::move(tag)) {}

  core::Result<core::ProgramFrameResolutionContextDTO>
  capture_program_frame_resolution_context(
      core::ProgramFrameResolutionCaptureDTO) const override {
    return core::Result<core::ProgramFrameResolutionContextDTO>::Err(
        core::ErrorDTO{tag_ + ".capture", "tagged resolver capture", {}});
  }

  core::Result<core::Pose3DTO> resolve_reference_pose(
      const core::ReferenceDTO &,
      const core::ProgramFrameResolutionContextDTO &) const override {
    return core::Result<core::Pose3DTO>::Err(
        core::ErrorDTO{tag_ + ".reference", "tagged resolver reference", {}});
  }

  core::Result<core::ResolvedSceneFramePoseDTO> resolve_scene_frame(
      const core::SceneFrameRefDTO &,
      const core::SceneFrameResolutionContextDTO &) const override {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(
        core::ErrorDTO{tag_ + ".scene", "tagged resolver scene", {}});
  }

private:
  std::string tag_;
};

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

[[nodiscard]] std::filesystem::path
InstallNativeTestTargets(const std::filesystem::path &source,
                         const std::filesystem::path &candidate_worker_source,
                         const std::filesystem::path &target_root) {
  const auto target = target_root / std::string(contract::kInstalledTargetId);
  const auto candidate_worker =
      target_root / std::string(contract::kCandidateWorkerInstalledTargetId);
  if (!std::filesystem::copy_file(source, target,
                                  std::filesystem::copy_options::none) ||
      !std::filesystem::copy_file(candidate_worker_source, candidate_worker,
                                  std::filesystem::copy_options::none)) {
    throw std::runtime_error("could not install native adapter test targets");
  }
  std::filesystem::permissions(target,
                               std::filesystem::status(source).permissions(),
                               std::filesystem::perm_options::replace);
  std::filesystem::permissions(
      candidate_worker,
      std::filesystem::status(candidate_worker_source).permissions(),
      std::filesystem::perm_options::replace);
  return target;
}

[[nodiscard]] const std::string &
TextMember(const deployment::CanonicalValue &root,
           const std::string_view name) {
  const auto *member = root.find_object_member(name);
  if (member == nullptr) {
    throw std::runtime_error("native adapter fixture is missing text member '" +
                             std::string(name) + "'");
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
      std::filesystem::path fixture_root = MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT)
      : root(std::move(fixture_root)), bundle_source(root / "retained-package"),
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
          "adapter fixture has no exact selected ROS runtime product");
    }
    const auto requirement =
        std::find_if(bundle->value().runtime_contract_requirements.begin(),
                     bundle->value().runtime_contract_requirements.end(),
                     [&](const auto &candidate) {
                       return candidate.runtime_unit_id == unit.runtime_unit_id;
                     });
    const auto *qualified =
        deployment::runtime_contract_declaration_registry().find(
            unit.runtime_profile_id, unit.runtime_profile_revision,
            product->product_contract_id(),
            product->product_contract_revision());
    if (requirement == bundle->value().runtime_contract_requirements.end() ||
        qualified == nullptr) {
      throw std::runtime_error(
          "native adapter fixture has no contract admission");
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
        requirement->requirement,
        qualified->offer().offer_digest,
    };
    query.emplace(component_manager::RuntimeUnitConvergenceQueryDTO{
        core::ApplyId(1U),
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
  ScopedTemporaryRoot manager_state_root;
  ScopedTemporaryRoot manager_run_root;
  ScopedTemporaryRoot execution_state_root;
  ScopedTemporaryRoot execution_run_root;
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
                  owner->apply_id, owner->apply_generation},
              projection,
              query.candidate_retained_bundle_id,
              query.candidate_graph_revision,
              core::ActiveBundlePublicationGeneration(1U),
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

// The fixture's bootstrap publication, as the Manager holds it in memory.
[[nodiscard]] core::ActiveBundlePublicationReceiptDTO
FixturePublication(const NativeFixture &fixture) {
  const auto active_scene =
      MakeEmptyScene(fixture.bundle->value().cell_graph.cell_id, 1U);
  return {{core::BootstrapPublicationTransactionDTO{
               core::BootstrapId("native-adapter-bootstrap"),
               core::BootstrapGeneration(1U)},
           core::ProjectionRevisionDTO{
               fixture.bundle->retained_bundle_id(),
               fixture.bundle->value().cell_graph.graph_revision,
               active_scene.shared()->revision},
           fixture.bundle->retained_bundle_id(),
           fixture.bundle->value().cell_graph.graph_revision, std::nullopt,
           core::ContentDigest(std::string(kIntentDigest))},
          core::ActiveBundlePublicationGeneration(1U),
          core::ContentDigest(std::string(kIntentDigest))};
}

// The adapter states `publication` (the fixture's bootstrap one by default)
// to the host with every fence request.
[[nodiscard]] component_manager::RetainedExecutionHostUnitAdapter
MakeAdapter(NativeFixture &fixture, const std::filesystem::path &target_root,
            std::optional<core::ActiveBundlePublicationReceiptDTO> publication =
                std::nullopt) {
  auto active = publication.value_or(FixturePublication(fixture));
  return component_manager::RetainedExecutionHostUnitAdapter(
      kTestStartId, target_root, fixture.root / "retained-package",
      fixture.manager_run_root.path(),
      fixture.execution_state_root.path(), fixture.execution_run_root.path(),
      fixture.bundle_source, fixture.artifact_source,
      std::string(contract::kOperatingSystem),
      std::string(contract::kLaunchEnvironmentContract), {}, {}, std::nullopt,
      [active] { return active; });
}

// Repeats `call` until `done` holds for its result, 10 ms apart. The status
// exchange never waits (OD-290 (a)), so an observation reports only a reply
// to a status an earlier observation sent. The deadline only fails the test:
// the last result is returned and the caller's assertion reports it.
template <typename Call, typename Done>
[[nodiscard]] auto PollUntil(Call call, Done done) -> decltype(call()) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (true) {
    auto result = call();
    if (done(result) || std::chrono::steady_clock::now() >= deadline)
      return result;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

// Observes until a status reply is consumed: the host answers again.
[[nodiscard]] std::optional<component_manager::RuntimeUnitSourceReport>
ObserveUntilReport(component_manager::RetainedExecutionHostUnitAdapter &adapter,
                   const component_manager::RuntimeUnitConvergenceQueryDTO &query,
                   const core::RuntimeEpoch &epoch) {
  return PollUntil(
      [&] { return adapter.ObserveProgress(query, epoch, 0U); },
      [](const auto &report) { return report.has_value(); });
}

[[nodiscard]] bool WaitUntilAnswering(
    component_manager::RetainedExecutionHostUnitAdapter &adapter,
    const NativeFixture &fixture, const core::RuntimeEpoch &epoch) {
  return ObserveUntilReport(adapter, *fixture.query, epoch).has_value();
}

[[nodiscard]] content_identity::CanonicalValue
ExecutionPacket(const std::filesystem::path &socket_path,
                std::string_view packet, bool normalize = true) {
  const auto encoded =
      normalize ? deployment::canonical_json(
                      deployment::parse_strict_json_model_yaml(packet).root())
                : std::string(packet);
  struct Socket final {
    int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    ~Socket() {
      if (fd >= 0)
        ::close(fd);
    }
  } socket;
  if (socket.fd < 0)
    throw std::runtime_error("qualification socket failed");
  timeval bound{2, 0};
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = socket_path.string();
  if (path.size() >= sizeof(address.sun_path))
    throw std::runtime_error("qualification socket path exceeds bound");
  std::copy(path.begin(), path.end(), address.sun_path);
  if (::setsockopt(socket.fd, SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound)) !=
          0 ||
      ::setsockopt(socket.fd, SOL_SOCKET, SO_SNDTIMEO, &bound, sizeof(bound)) !=
          0 ||
      ::connect(socket.fd, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0 ||
      ::send(socket.fd, encoded.data(), encoded.size(), MSG_NOSIGNAL) !=
          static_cast<ssize_t>(encoded.size())) {
    throw std::runtime_error("qualification packet dispatch failed");
  }
  std::array<char, 131072> bytes{};
  const auto count = ::recv(socket.fd, bytes.data(), bytes.size(), MSG_TRUNC);
  if (count <= 0 || static_cast<std::size_t>(count) > bytes.size())
    throw std::runtime_error("qualification reply absent/oversized");
  return content_identity::parse_canonical_json(
      std::string_view(bytes.data(), static_cast<std::size_t>(count)));
}

class CountingBundleSource final
    : public deployment::IVerifiedRetainedBundleSource {
public:
  explicit CountingBundleSource(
      const deployment::IVerifiedRetainedBundleSource &source)
      : source_(source) {}

  std::shared_ptr<const deployment::RetainedBundle> get_verified(
      const core::RetainedBundleId &retained_bundle_id) const override {
    ++reads_;
    return source_.get_verified(retained_bundle_id);
  }

  [[nodiscard]] std::size_t reads() const noexcept { return reads_; }

private:
  const deployment::IVerifiedRetainedBundleSource &source_;
  mutable std::size_t reads_{};
};

class CountingArtifactSource final
    : public deployment::IVerifiedRetainedProductArtifactSource {
public:
  explicit CountingArtifactSource(
      const deployment::IVerifiedRetainedProductArtifactSource &source)
      : source_(source) {}

  std::optional<deployment::VerifiedRetainedProductArtifact> get_verified(
      const deployment::RetainedProductArtifactQuery &query) const override {
    ++reads_;
    return source_.get_verified(query);
  }

  [[nodiscard]] std::size_t reads() const noexcept { return reads_; }

private:
  const deployment::IVerifiedRetainedProductArtifactSource &source_;
  mutable std::size_t reads_{};
};

[[nodiscard]] core::ActiveBundlePublicationReceiptDTO
PublishActiveFixture(NativeFixture &fixture) {
  component_manager::SqliteActiveBundlePublicationStore store(
      fixture.manager_state_root.path() /
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
  });
  store.Close();
  return receipt;
}

[[nodiscard]] core::ProjectionExecutionHostBindingDTO
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

[[nodiscard]] std::vector<core::ProjectionExecutionHostBindingDTO>
NativeProjectionBindings(const NativeFixture &fixture,
                         const core::RuntimeEpoch &runtime_epoch) {
  std::vector<core::ProjectionExecutionHostBindingDTO> result;
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

[[nodiscard]] bool BootstrapNativeProjectionSet(
    NativeFixture &fixture,
    component_manager::RetainedExecutionHostUnitAdapter &adapter,
    const core::NativeProjectionBootstrapStatusQueryDTO &bootstrap_query,
    const core::LogicalSceneSnapshotRoot &initial_scene,
    core::IPlannerSceneProjection &primary_projection) {
  for (const auto &required_binding : NativeProjectionBindings(
           fixture, bootstrap_query.execution_host_binding.runtime_epoch)) {
    if (required_binding == bootstrap_query.execution_host_binding) {
      continue;
    }
    const auto required = adapter.ResolveProjection(required_binding);
    if (!required.ok()) {
      ADD_FAILURE() << required.error().message;
      return false;
    }
    const core::NativeProjectionBootstrapRebuildDTO required_request{
        bootstrap_query.bootstrap_id,
        bootstrap_query.bootstrap_generation,
        required_binding,
        bootstrap_query.canonical_empty_target,
        fixture.bundle->value().compiled_products.planning_collision_model,
        initial_scene};
    const auto required_result =
        required.value()->RebuildBootstrapProjection(required_request);
    if (!required_result.presence_valid()) {
      ADD_FAILURE() << "required native projection bootstrap is invalid";
      return false;
    }
    EXPECT_TRUE(required_result.resulting_health.healthy_for_target(
        bootstrap_query.canonical_empty_target));
  }

  const core::NativeProjectionBootstrapRebuildDTO bootstrap_request{
      bootstrap_query.bootstrap_id,
      bootstrap_query.bootstrap_generation,
      bootstrap_query.execution_host_binding,
      bootstrap_query.canonical_empty_target,
      fixture.bundle->value().compiled_products.planning_collision_model,
      initial_scene};
  const auto bootstrapped =
      primary_projection.RebuildBootstrapProjection(bootstrap_request);
  if (!bootstrapped.presence_valid()) {
    ADD_FAILURE() << "primary native projection bootstrap is invalid";
    return false;
  }
  EXPECT_EQ(bootstrapped.state,
            core::NativeProjectionBootstrapRebuildState::kRebuilt);
  EXPECT_TRUE(bootstrapped.resulting_health.healthy_for_target(
      bootstrap_query.canonical_empty_target));
  return true;
}

// Holds one native projection construction operation of the projection test
// target at its construction boundary until Release(). Declare it after the
// adapter: assertion unwinding then releases construction before the adapter
// drains its child, and restores the inherited environment.
class ProjectionConstructionHold final {
public:
  ProjectionConstructionHold(std::filesystem::path root, std::string operation)
      : root_(std::move(root)), operation_(std::move(operation)),
        previous_control_(
            Environment("MANYFORGE_NATIVE_PROJECTION_TEST_CONTROL_DIR")),
        previous_hold_(Environment("MANYFORGE_NATIVE_PROJECTION_TEST_HOLD")) {}

  ProjectionConstructionHold(const ProjectionConstructionHold &) = delete;
  ProjectionConstructionHold &
  operator=(const ProjectionConstructionHold &) = delete;

  ~ProjectionConstructionHold() {
    static_cast<void>(Release());
    Restore("MANYFORGE_NATIVE_PROJECTION_TEST_CONTROL_DIR", previous_control_);
    Restore("MANYFORGE_NATIVE_PROJECTION_TEST_HOLD", previous_hold_);
  }

  [[nodiscard]] bool Configure() const {
    return ::setenv("MANYFORGE_NATIVE_PROJECTION_TEST_CONTROL_DIR",
                    root_.string().c_str(), 1) == 0 &&
           ::setenv("MANYFORGE_NATIVE_PROJECTION_TEST_HOLD",
                    operation_.c_str(), 1) == 0;
  }

  [[nodiscard]] std::filesystem::path Entered() const {
    return root_ / (operation_ + ".entered");
  }

  [[nodiscard]] bool Release() const noexcept {
    try {
      std::ofstream marker(root_ / (operation_ + ".release"));
      marker << "release\n";
      return static_cast<bool>(marker);
    } catch (...) {
      return false;
    }
  }

private:
  static std::optional<std::string> Environment(const char *name) {
    const auto *value = std::getenv(name);
    return value != nullptr ? std::optional<std::string>(value) : std::nullopt;
  }

  static void Restore(const char *name,
                      const std::optional<std::string> &value) noexcept {
    if (value.has_value())
      static_cast<void>(::setenv(name, value->c_str(), 1));
    else
      static_cast<void>(::unsetenv(name));
  }

  std::filesystem::path root_;
  std::string operation_;
  std::optional<std::string> previous_control_;
  std::optional<std::string> previous_hold_;
};

// The Manager's periodic report lane for one exact required set, at the
// policy's `heartbeat_period_ns`. Reports never expire; the lane lets a test
// check that the child keeps serving them while other work runs.
class ManagerHeartbeatLane final {
public:
  // `accepted` is the request whose heartbeat the receiver already accepted;
  // the lane continues that Manager epoch with the next sequence.
  ManagerHeartbeatLane(
      component_manager::RetainedExecutionHostUnitAdapter &adapter,
      component_manager::ManagerLivenessTransportRequestDTO accepted)
      : adapter_(adapter), request_(std::move(accepted)),
        accepted_sequence_(request_.heartbeat.heartbeat_sequence),
        worker_([this](const std::stop_token stop) { Run(stop); }) {}

  ManagerHeartbeatLane(const ManagerHeartbeatLane &) = delete;
  ManagerHeartbeatLane &operator=(const ManagerHeartbeatLane &) = delete;

  ~ManagerHeartbeatLane() { Stop(); }

  void Stop() {
    worker_.request_stop();
    if (worker_.joinable()) {
      worker_.join();
    }
  }

  // Waits for the receiver's own acknowledgement of `sequence`. The bound is
  // diagnostic only; a rejected renewal ends the wait immediately.
  [[nodiscard]] std::optional<core::ManagerLivenessObservationDTO>
  WaitForAccepted(const std::uint64_t sequence,
                  const std::chrono::steady_clock::duration diagnostic_bound) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, diagnostic_bound, [&] {
      return failure_.has_value() || accepted_sequence_ >= sequence;
    });
    if (accepted_sequence_ < sequence) {
      return std::nullopt;
    }
    return last_;
  }

  [[nodiscard]] std::string Failure() const {
    std::lock_guard lock(mutex_);
    return failure_.value_or("Manager liveness lane has not failed");
  }

private:
  void Run(const std::stop_token stop) {
    const std::chrono::nanoseconds period(request_.policy.heartbeat_period_ns);
    auto request = request_;
    auto due = std::chrono::steady_clock::now() + period;
    for (;;) {
      {
        std::unique_lock lock(mutex_);
        static_cast<void>(
            changed_.wait_until(lock, stop, due, [] { return false; }));
        if (stop.stop_requested()) {
          return;
        }
      }
      due += period;
      ++request.heartbeat.heartbeat_sequence;
      std::optional<std::string> failure;
      std::optional<core::ManagerLivenessObservationDTO> observation;
      try {
        observation = adapter_.RenewManagerLiveness(request);
        if (observation->status.state !=
                core::ManagerLivenessStateDTO::kCurrent ||
            observation->status.command_fenced ||
            observation->status.last_accepted_heartbeat_sequence !=
                request.heartbeat.heartbeat_sequence) {
          failure = "Manager liveness renewal " +
                    std::to_string(request.heartbeat.heartbeat_sequence) +
                    " left the receiver non-current";
        }
      } catch (const std::exception &error) {
        failure = "Manager liveness renewal " +
                  std::to_string(request.heartbeat.heartbeat_sequence) +
                  " failed: " + error.what();
      }
      const bool failed = failure.has_value();
      {
        std::lock_guard lock(mutex_);
        if (failed) {
          failure_ = std::move(failure);
        } else {
          accepted_sequence_ = request.heartbeat.heartbeat_sequence;
          last_ = std::move(observation);
        }
      }
      changed_.notify_all();
      if (failed) {
        return;
      }
    }
  }

  component_manager::RetainedExecutionHostUnitAdapter &adapter_;
  const component_manager::ManagerLivenessTransportRequestDTO request_;
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::uint64_t accepted_sequence_;
  std::optional<core::ManagerLivenessObservationDTO> last_;
  std::optional<std::string> failure_;
  // Last member: the worker starts only after every field it reads exists.
  std::jthread worker_;
};

TEST(RetainedPlanningSceneSynchronizer,
     StableOwnerForwardsToItsCurrentTarget) {
  auto first = std::make_shared<FixedPlanningSceneSynchronizer>(1U);
  auto second = std::make_shared<FixedPlanningSceneSynchronizer>(2U);
  RetainedPlanningSceneSynchronizer owner(first);
  auto *const stable_owner =
      static_cast<core::IPlanningSceneSynchronizer *>(&owner);

  const auto first_result = owner.SynchronizeBeforePlanning();
  ASSERT_TRUE(first_result.ok());
  EXPECT_EQ(first_result.value().scene_revision.revision_id, 1U);

  owner.ReplaceTarget(second);
  const auto second_result = owner.SynchronizeBeforePlanning();
  ASSERT_TRUE(second_result.ok());
  EXPECT_EQ(second_result.value().scene_revision.revision_id, 2U);
  EXPECT_EQ(stable_owner,
            static_cast<core::IPlanningSceneSynchronizer *>(&owner));
  EXPECT_EQ(first->calls(), 1U);
  EXPECT_EQ(second->calls(), 1U);
}

TEST(RetainedReferencePoseResolverProxy,
     StableKernelPortReplacesItsTargetWithoutChangingItsAddress) {
  auto first = std::make_shared<TaggedReferencePoseResolver>("first");
  auto second = std::make_shared<TaggedReferencePoseResolver>("second");
  RetainedReferencePoseResolverProxy owner(first);
  auto *const stable_owner =
      static_cast<core::IReferencePoseResolver *>(&owner);

  EXPECT_NO_THROW(owner.ReplaceTarget(second));
  EXPECT_EQ(stable_owner,
            static_cast<core::IReferencePoseResolver *>(&owner));
  EXPECT_THROW(owner.ReplaceTarget(nullptr), std::invalid_argument);
}

TEST(ExecutionHostUnitAdmission,
     SelectsOnlyTheBuildOwnedInstalledTargetAndFixedArguments) {
  NativeFixture fixture;
  const auto admission =
      component_manager::VerifyExecutionHostUnitForHost(
          *fixture.bundle, fixture.query->plan.runtime_unit_id,
          fixture.artifact_source, contract::kOperatingSystem,
          contract::kLaunchEnvironmentContract);
  ASSERT_TRUE(admission.has_value());
  EXPECT_EQ(admission->installed_target_id(), contract::kInstalledTargetId);
  EXPECT_EQ(admission->implementation_revision(),
            contract::implementation_revision());
  // The installed offer is generated from this build's implementation. Its
  // ROS process-adapter revision uses the ROS contract domain with the same
  // adapter description as the retired native fixture; admission must match
  // that exact selected ROS offer, not the native-domain digest.
  EXPECT_EQ(fixture.query->plan.process_adapter_contract_revision,
            "sha256:"
            "7656d6dc0a4c97c941e6c87e3a9fcf77d23f209748b8400159aa092cd838ac6b");
  EXPECT_EQ(admission->runtime_contract_offer_digest(),
            fixture.query->plan.runtime_contract_offer_digest.value());
  const core::RuntimeEpoch packet_epoch("execution-host-test-epoch");
  const auto ready_packet = admission->expected_ready_packet(packet_epoch);
  const auto status_packet =
      admission->expected_status_packet(packet_epoch, 1U);
  EXPECT_TRUE(ready_packet.starts_with("manyforge.execution-host-ready.v2 "));
  EXPECT_TRUE(
      status_packet.starts_with("manyforge.execution-host-status.v2 "));
  EXPECT_NE(ready_packet.find(admission->runtime_contract_offer_digest()),
            std::string::npos);
  EXPECT_NE(status_packet.find(admission->runtime_contract_offer_digest()),
            std::string::npos);
  EXPECT_EQ(ready_packet.find(admission->implementation_revision()),
            std::string::npos);
  EXPECT_EQ(status_packet.find(admission->implementation_revision()),
            std::string::npos);
  // Exercise the production parser used by ObserveProgress, including the
  // absent older packet and counts beyond the I-JSON integer width.
  const auto old_report =
      admission->parse_status_packet(packet_epoch, status_packet);
  EXPECT_FALSE(old_report.dropped_host_notice_lines().has_value());
  EXPECT_FALSE(old_report.dropped_host_notice_bytes().has_value());
  const auto full_report = admission->parse_status_packet(
      packet_epoch, status_packet +
                        " dropped_host_notice_lines=9007199254740992"
                        " dropped_host_notice_bytes=18446744073709551615");
  EXPECT_EQ(full_report.dropped_host_notice_lines(), 9007199254740992ULL);
  EXPECT_EQ(full_report.dropped_host_notice_bytes(), 18446744073709551615ULL);
  EXPECT_EQ(full_report.source_health(), old_report.source_health());
  EXPECT_EQ(full_report.local_phase(), old_report.local_phase());
  EXPECT_EQ(full_report.ready_runtime_role_ids(),
            old_report.ready_runtime_role_ids());
  EXPECT_EQ(full_report.reason_codes(), old_report.reason_codes());
  const auto zero_report = admission->parse_status_packet(
      packet_epoch,
      status_packet +
          " dropped_host_notice_lines=0 dropped_host_notice_bytes=0");
  EXPECT_EQ(zero_report.dropped_host_notice_lines(), 0U);
  for (const std::string suffix :
       {" dropped_host_notice_lines=1",
        " dropped_host_notice_bytes=1 dropped_host_notice_lines=1",
        " dropped_host_notice_lines=01 dropped_host_notice_bytes=1",
        " dropped_host_notice_lines=-1 dropped_host_notice_bytes=1",
        " dropped_host_notice_lines=1 dropped_host_notice_bytes=+1",
        " dropped_host_notice_lines=1 "
        "dropped_host_notice_bytes=18446744073709551616",
        " dropped_host_notice_lines= dropped_host_notice_bytes=1",
        " dropped_host_notice_lines=1 dropped_host_notice_bytes=1 extra",
        "  dropped_host_notice_lines=1 dropped_host_notice_bytes=1",
        " dropped_host_notice_lines=1 dropped_host_notice_bytes=1 "}) {
    SCOPED_TRACE(suffix);
    EXPECT_THROW(
        admission->parse_status_packet(packet_epoch, status_packet + suffix),
        component_manager::ExecutionHostUnitAdmissionError);
  }
  EXPECT_THROW(admission->parse_status_packet(
                   core::RuntimeEpoch("foreign-epoch"), status_packet),
               component_manager::ExecutionHostUnitAdmissionError);
  EXPECT_THROW(
      admission->parse_status_packet(packet_epoch, status_packet + " "),
      component_manager::ExecutionHostUnitAdmissionError);
  EXPECT_THROW(
      admission->parse_status_packet(
          packet_epoch, admission->expected_ready_packet(packet_epoch)),
      component_manager::ExecutionHostUnitAdmissionError);
  const auto arguments = admission->launch_arguments(3, packet_epoch);
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
                   component_manager::VerifyExecutionHostUnitForHost(
                       *fixture.bundle, fixture.query->plan.runtime_unit_id,
                       fixture.artifact_source, "darwin",
                       contract::kLaunchEnvironmentContract)),
               component_manager::ExecutionHostUnitAdmissionError);
  EXPECT_THROW(static_cast<void>(
                   component_manager::VerifyExecutionHostUnitForHost(
                       *fixture.bundle, fixture.query->plan.runtime_unit_id,
                       fixture.artifact_source, contract::kOperatingSystem,
                       "foreign.native.abi")),
               component_manager::ExecutionHostUnitAdmissionError);
}

TEST(RetainedExecutionHostUnitAdapter,
     RejectsNestedStateAndRunCapabilityTreesBeforeLaunch) {
  NativeFixture fixture;
  const auto nested_run_root = fixture.execution_state_root.path() / "run";
  ASSERT_TRUE(std::filesystem::create_directory(nested_run_root));

  EXPECT_THROW(
      static_cast<void>(component_manager::RetainedExecutionHostUnitAdapter(
          kTestStartId, MANYFORGE_EXECUTION_HOST_TARGET_ROOT,
          fixture.root / "retained-package",
          nested_run_root, fixture.execution_state_root.path(),
          fixture.execution_run_root.path(), fixture.bundle_source,
          fixture.artifact_source, std::string(contract::kOperatingSystem),
          std::string(contract::kLaunchEnvironmentContract))),
      std::runtime_error);
}

TEST(RetainedExecutionHostUnitAdapter,
     ReusesOneVerifiedAdmissionForOneExactRuntimeQuery) {
  NativeFixture fixture;
  CountingBundleSource bundles(fixture.bundle_source);
  CountingArtifactSource artifacts(fixture.artifact_source);
  auto adapter = component_manager::RetainedExecutionHostUnitAdapter(
      kTestStartId, MANYFORGE_EXECUTION_HOST_TARGET_ROOT, fixture.root / "retained-package",
      fixture.manager_run_root.path(),
      fixture.execution_state_root.path(), fixture.execution_run_root.path(),
      bundles, artifacts, std::string(contract::kOperatingSystem),
      std::string(contract::kLaunchEnvironmentContract));

  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  const auto bundle_reads = bundles.reads();
  const auto artifact_reads = artifacts.reads();
  EXPECT_GT(bundle_reads, 0U);
  EXPECT_GT(artifact_reads, 0U);

  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_EQ(bundles.reads(), bundle_reads);
  EXPECT_EQ(artifacts.reads(), artifact_reads);
}

TEST(RetainedExecutionHostUnitAdapter,
     DirectChildOwnsStableEmptyAdmissionBeforeApplyAndIsNeverRelaunched) {
  NativeFixture fixture;
  ASSERT_TRUE(fixture.product.has_value());
  auto adapter = MakeAdapter(fixture, MANYFORGE_EXECUTION_HOST_TARGET_ROOT);

  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  // The host occurrence carries the application-start identity (contract 1.1).
  ASSERT_EQ(started.process_incarnation(), kTestStartIdText);
  const core::RuntimeEpoch first_epoch(started.process_incarnation());
  const auto empty_admission = adapter.GetAuthoringAdmission();
  EXPECT_FALSE(empty_admission.authoring_allowed);
  EXPECT_TRUE(empty_admission.admission_generation.nonzero());

  const auto first = ObserveUntilReport(adapter, *fixture.query, first_epoch);
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
  EXPECT_EQ(first->offered_runtime_contract_offer_digest(),
            fixture.query->plan.runtime_contract_offer_digest.value());
  EXPECT_EQ(first->ready_runtime_role_ids(),
            std::vector<std::string>{std::string(contract::kRuntimeRoleId)});
  EXPECT_EQ(
      first->command_surface_state(),
      component_manager::ManagedCommandSurfaceState::kClosedNoActiveEffect);

  const auto replay = adapter.EnsureRunning(*fixture.query);
  EXPECT_EQ(replay.process_incarnation(), first_epoch.value());
  // The observation that consumed reply 1 sent status 2.
  const auto second = ObserveUntilReport(adapter, *fixture.query, first_epoch);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->source_report_sequence(), 2U);

  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_FALSE(
      adapter.ObserveProgress(*fixture.query, first_epoch, 0U).has_value());
  // One host per application start (Q1): its exit is reported to the Manager
  // and it is never launched again (OD-122 (1)).
  const auto exit = adapter.HostExit();
  ASSERT_TRUE(exit.has_value());
  EXPECT_FALSE(exit->signaled);
  EXPECT_EQ(exit->value, 0);
  EXPECT_THROW(static_cast<void>(adapter.EnsureRunning(*fixture.query)),
               core::TypedError);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedExecutionHostUnitAdapter,
     ReservedStopAndExactReadsRemainAvailableBeforePhysicalReconstruction) {
  NativeFixture fixture;
  auto adapter = MakeAdapter(fixture, MANYFORGE_EXECUTION_HOST_TARGET_ROOT);
  const auto process = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(process.state(), component_manager::ProcessPresenceState::kPresent);
  const auto ordinary =
      fixture.execution_run_root.path() / "execution-command.sock";
  const auto priority =
      fixture.execution_run_root.path() / "execution-control.sock";
  const auto identity = ExecutionPacket(
      ordinary,
      R"({"api_version":"manyforge.execution_command.v4","command":"identity"})");
  const auto *error = identity.find_object_member("error");
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(TextMember(*error, "code"), "execution_owner_not_ready");
  const std::string attribution =
      R"("attribution":{"transport":"internal","assurance":"unverified","principal_id":null})";
  const auto packet = [&](std::string_view command, std::string_view request) {
    return "{\"api_version\":\"manyforge.execution_command.v4\",\"command\":"
           "\"" +
           std::string(command) + "\"," + attribution +
           ",\"request\":" + std::string(request) + "}";
  };
  const auto context = [&](const std::uint64_t request) {
    return "{\"request_key\":\"" +
           core::RequestKeyText(TestRequestKey(request)) +
           "\",\"source_label\":\"qualification\"}";
  };
  std::uint64_t request_sequence = 0U;
  for (const auto &command :
       {std::string("stop_program_run"), std::string("stop"),
        std::string("stop_all"), std::string("halt"), std::string("cell_stop")}) {
    SCOPED_TRACE(command);
    // An absent operation number (contract 1.5); the cell-wide forms name
    // no target.
    const auto target = command == "stop_program_run"
                            ? ",\"program_run_id\":154"
                        : command == "stop"
                            ? ",\"operations\":[{\"operation_id\":999999,"
                              "\"expected_operation_revision\":1}]"
                            : "";
    const auto reply = ExecutionPacket(
        priority, packet(command, "{\"request_context\":" +
                                      context(++request_sequence) +
                                      std::string(target) + "}"));
    const auto encoded = content_identity::canonical_json(reply);
    EXPECT_EQ(encoded.find("execution_owner_not_ready"), std::string::npos)
        << encoded;
    EXPECT_EQ(encoded.find("execution_command_invalid"), std::string::npos)
        << encoded;
    ASSERT_NE(reply.find_object_member("accepted"), nullptr) << encoded;
    const auto *accepted = reply.find_object_member("accepted");
    // Stop event ids are integers (contract 1.5).
    const auto *event_id = accepted->find_object_member("event_id");
    ASSERT_NE(event_id, nullptr) << encoded;
    ASSERT_TRUE(event_id->is_number()) << encoded;
    const auto read = ExecutionPacket(
        priority, "{\"api_version\":\"manyforge.execution_command.v4\","
                  "\"command\":\"read_stop\",\"event_id\":" +
                      std::to_string(
                          static_cast<std::uint64_t>(event_id->as_number())) +
                      "," + attribution + "}");
    EXPECT_NE(read.find_object_member("stop_status"), nullptr)
        << content_identity::canonical_json(read);
  }
  const auto malformed = ExecutionPacket(priority, "{", false);
  ASSERT_NE(malformed.find_object_member("error"), nullptr);
  EXPECT_EQ(TextMember(*malformed.find_object_member("error"), "code"),
            "execution_command_invalid");
  const auto recognized = ExecutionPacket(
      priority,
      "{\"api_version\":\"manyforge.execution_command.v4\",\"command\":"
      "\"recognized_stop\",\"request_key\":\"" +
          core::RequestKeyText(TestRequestKey(++request_sequence)) +
          "\",\"recognized_route\":"
      "\"/api/execution/"
      "stops\",\"reason\":\"recognized_stop_payload_invalid\","
      "\"source_label\":null," +
          attribution + "}");
  ASSERT_NE(recognized.find_object_member("accepted"), nullptr)
      << content_identity::canonical_json(recognized);
  const auto late = ExecutionPacket(
      ordinary,
      packet("reset",
             "{\"request_context\":" + context(++request_sequence) +
                 ",\"operation_id\":999999,"
                 "\"expected_operation_revision\":1,\"reason\":null}"));
  ASSERT_NE(late.find_object_member("error"), nullptr);
  EXPECT_EQ(TextMember(*late.find_object_member("error"), "code"),
            "execution_owner_not_ready");
  EXPECT_TRUE(ObserveUntilReport(adapter, *fixture.query,
                                 core::RuntimeEpoch(process.process_incarnation()))
                  .has_value());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

/// One raw connection to an execution socket: it may send one packet and
/// reads its reply later, so a test can hold a lane or queue behind it.
class LaneClient final {
public:
  explicit LaneClient(const std::filesystem::path &socket_path) {
    if (fd_ < 0)
      throw std::runtime_error("lane client socket failed");
    timeval bound{2, 0};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = socket_path.string();
    if (path.size() >= sizeof(address.sun_path))
      throw std::runtime_error("lane client socket path exceeds bound");
    std::copy(path.begin(), path.end(), address.sun_path);
    if (::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound)) != 0 ||
        ::connect(fd_, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
      throw std::runtime_error("lane client connect failed");
  }
  LaneClient(const LaneClient &) = delete;
  LaneClient &operator=(const LaneClient &) = delete;
  ~LaneClient() { ::close(fd_); }

  void Send(std::string_view packet) const {
    const auto encoded = deployment::canonical_json(
        deployment::parse_strict_json_model_yaml(packet).root());
    if (::send(fd_, encoded.data(), encoded.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(encoded.size()))
      throw std::runtime_error("lane client send failed");
  }

  /// A reply arrived or the host closed the connection.
  [[nodiscard]] bool Answered() const {
    pollfd inbound{fd_, POLLIN, 0};
    return ::poll(&inbound, 1, 0) > 0;
  }

  /// The reply, or nullopt when the host closed without one.
  [[nodiscard]] std::optional<content_identity::CanonicalValue> Reply() const {
    std::array<char, 131072> bytes{};
    const auto count = ::recv(fd_, bytes.data(), bytes.size(), MSG_TRUNC);
    if (count < 0 || static_cast<std::size_t>(count) > bytes.size())
      throw std::runtime_error("lane client reply absent/oversized");
    if (count == 0)
      return std::nullopt;
    return content_identity::parse_canonical_json(
        std::string_view(bytes.data(), static_cast<std::size_t>(count)));
  }

private:
  int fd_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
};

TEST(RetainedExecutionHostUnitAdapter,
     ComponentAndStopLanesRouteTheirKindsAndServeIndependently) {
  NativeFixture fixture;
  auto adapter = MakeAdapter(fixture, MANYFORGE_EXECUTION_HOST_TARGET_ROOT);
  const auto process = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(process.state(), component_manager::ProcessPresenceState::kPresent);
  const auto run_root = fixture.execution_run_root.path();
  const auto ordinary = run_root / "execution-command.sock";
  const auto control = run_root / "execution-control.sock";
  const auto component = run_root / "execution-component.sock";
  ASSERT_TRUE(std::filesystem::is_socket(component));
  const std::string attribution =
      R"("attribution":{"transport":"internal","assurance":"unverified","principal_id":null})";
  const auto packet = [&](std::string_view command, std::string_view request) {
    return "{\"api_version\":\"manyforge.execution_command.v4\",\"command\":"
           "\"" +
           std::string(command) + "\"," + attribution +
           ",\"request\":" + std::string(request) + "}";
  };
  std::uint64_t request_sequence = 0U;
  const auto context = [&] {
    return "{\"request_key\":\"" +
           core::RequestKeyText(TestRequestKey(++request_sequence)) +
           "\",\"source_label\":\"qualification\"}";
  };
  const auto code = [](const content_identity::CanonicalValue &reply) {
    const auto *error = reply.find_object_member("error");
    return error == nullptr ? std::string() : TextMember(*error, "code");
  };
  const auto routed = [&](const content_identity::CanonicalValue &reply) {
    const auto refusal = code(reply);
    return refusal != "execution_component_command_unsupported" &&
           refusal != "execution_control_command_unsupported" &&
           refusal != "execution_ordinary_command_unsupported" &&
           refusal != "execution_command_invalid";
  };

  // Every Stop form, the Stop status read and carried-object-lost stay on
  // the control socket; the component socket refuses them unexecuted.
  const std::vector<std::string> stop_lane{
      packet("stop_program_run", "{\"request_context\":" + context() +
                                     ",\"program_run_id\":154}"),
      packet("stop", "{\"request_context\":" + context() +
                         ",\"operations\":[{\"operation_id\":999999,"
                         "\"expected_operation_revision\":1}]}"),
      packet("stop_all", "{\"request_context\":" + context() + "}"),
      packet("halt", "{\"request_context\":" + context() + "}"),
      packet("cell_stop", "{\"request_context\":" + context() + "}"),
      packet("stop_program_selection",
             "{\"schema_version\":\"manyforge.program_execution_selection.v3\","
             "\"request_context\":" + context() +
                 ",\"program_run_id\":154,\"group_ids\":[\"group.1\"]}"),
      "{\"api_version\":\"manyforge.execution_command.v4\",\"command\":"
      "\"recognized_stop\",\"request_key\":\"" +
          core::RequestKeyText(TestRequestKey(++request_sequence)) +
          "\",\"recognized_route\":\"/api/execution/stops\",\"reason\":"
          "\"recognized_stop_payload_invalid\",\"source_label\":null," + attribution + "}",
      "{\"api_version\":\"manyforge.execution_command.v4\",\"command\":"
      "\"read_stop\",\"event_id\":1," + attribution + "}",
      packet("set_carried_object_lost",
             "{\"controllable_component_id\":\"robot\",\"command_target_id\":"
             "\"arm\",\"level\":false,\"carried_object_attachment_id\":null,"
             "\"scene_resource_id\":null,\"loss_event_id\":null,\"set_key\":null}")};
  for (const auto &request : stop_lane) {
    SCOPED_TRACE(request);
    EXPECT_EQ(code(ExecutionPacket(component, request)),
              "execution_component_command_unsupported");
  }

  // Component actions (plain and recover-enable), their preflight and the
  // component model are served on the component socket only.
  const std::vector<std::string> component_lane{
      R"({"api_version":"manyforge.execution_command.v4","command":"read_component_model"})",
      R"({"api_version":"manyforge.execution_command.v4","command":"component_recovery_preflight","request":{"component_id":"absent.component"}})",
      R"({"api_version":"manyforge.execution_command.v4","command":"component_action","request":{"action":"enable","component_id":"absent.component"}})",
      R"({"api_version":"manyforge.execution_command.v4","command":"component_action","request":{"action":"enable","component_id":"absent.component","recovery_preflight":{"confirmed":[{"cause_code":"fault_vendor","component_id":"absent.component","eligibility":"recover","stop_code":null,"stop_vendor_code":null,"vendor_code":"C31"}],"snapshot_id":"snapshot"}}})"};
  for (const auto &request : component_lane) {
    SCOPED_TRACE(request);
    EXPECT_EQ(code(ExecutionPacket(control, request)),
              "execution_control_command_unsupported");
    EXPECT_EQ(code(ExecutionPacket(ordinary, request)),
              "execution_ordinary_command_unsupported");
    const auto served = ExecutionPacket(component, request);
    EXPECT_TRUE(routed(served)) << content_identity::canonical_json(served);
  }

  // Operator-alarm attention: component socket only.
  const std::string attention =
      "{\"api_version\":\"manyforge.execution_command.v4\",\"command\":"
      "\"cell_admission_publish_operator_alarm_attention\",\"request\":{"
      "\"active_configuration\":null,\"cell_id\":\"cell.test\","
      "\"disk_cannot_keep_records\":false,\"generation\":1,\"members\":[],\"owner_host_boot_id\":\"boot.test\","
      "\"reconciled\":true,\"schema_version\":\"" +
      std::string(core::kOperatorAlarmAttentionSchemaVersion) + "\"}}";
  EXPECT_EQ(code(ExecutionPacket(control, attention)),
            "execution_control_command_unsupported");
  EXPECT_EQ(code(ExecutionPacket(ordinary, attention)),
            "execution_ordinary_command_unsupported");
  const auto published = ExecutionPacket(component, attention);
  EXPECT_TRUE(routed(published)) << content_identity::canonical_json(published);
  const std::string admission_get =
      R"({"api_version":"manyforge.execution_command.v4","command":"cell_admission_get"})";
  EXPECT_EQ(code(ExecutionPacket(component, admission_get)),
            "execution_component_command_unsupported");
  EXPECT_EQ(code(ExecutionPacket(control, admission_get)),
            "execution_control_command_unsupported");

  // No lane times a request: a connection that has not sent its request
  // holds up no other connection of its lane, its request is served whenever
  // it arrives, and a connection closed without one is dropped.
  const auto observation = run_root / "execution-observation.sock";
  ASSERT_TRUE(std::filesystem::is_socket(observation));
  const std::string model_read =
      R"({"api_version":"manyforge.execution_command.v4","command":"read_component_model"})";
  const std::string observation_read =
      R"({"api_version":"manyforge.execution_command.v4","command":"observation"})";
  // Served on the observation lane (this fixture may answer it with an
  // owner error; the lane's own refusal would mean it was misrouted).
  const auto observed = [&](const content_identity::CanonicalValue &reply) {
    return reply.find_object_member("ok") != nullptr &&
           code(reply) != "execution_observation_command_unsupported";
  };
  std::uint64_t event_id = 0U;
  const auto read_stop = [&] {
    return "{\"api_version\":\"manyforge.execution_command.v4\","
           "\"command\":\"read_stop\",\"event_id\":" +
           std::to_string(event_id) + "," + attribution + "}";
  };
  {
    const LaneClient silent_component(component);
    const LaneClient silent_control(control);
    const LaneClient silent_ordinary(ordinary);
    const LaneClient silent_observation(observation);
    // Each later connection is served while the silent one before it on
    // the same lane still waits (a reply is bounded by the client's 2 s).
    const LaneClient queued(component);
    queued.Send(model_read);
    const auto model = queued.Reply();
    ASSERT_TRUE(model.has_value());
    EXPECT_TRUE(routed(*model)) << content_identity::canonical_json(*model);
    const auto stopped =
        ExecutionPacket(control, packet("stop_all", "{\"request_context\":" + context() + "}"));
    const auto *accepted = stopped.find_object_member("accepted");
    ASSERT_NE(accepted, nullptr) << content_identity::canonical_json(stopped);
    const auto *id = accepted->find_object_member("event_id");
    ASSERT_NE(id, nullptr);
    event_id = static_cast<std::uint64_t>(id->as_number());
    const auto read = ExecutionPacket(control, read_stop());
    EXPECT_NE(read.find_object_member("stop_status"), nullptr)
        << content_identity::canonical_json(read);
    const auto admission = ExecutionPacket(ordinary, admission_get);
    EXPECT_TRUE(routed(admission)) << content_identity::canonical_json(admission);
    // An observation session stays open between its requests and holds up
    // no other observation connection either.
    const LaneClient session(observation);
    session.Send(observation_read);
    const auto first_observation = session.Reply();
    ASSERT_TRUE(first_observation.has_value());
    EXPECT_TRUE(observed(*first_observation))
        << content_identity::canonical_json(*first_observation);
    const auto other_observation = ExecutionPacket(observation, observation_read);
    EXPECT_TRUE(observed(other_observation))
        << content_identity::canonical_json(other_observation);
    // No silent connection was answered or closed meanwhile.
    EXPECT_FALSE(silent_component.Answered());
    EXPECT_FALSE(silent_control.Answered());
    EXPECT_FALSE(silent_ordinary.Answered());
    EXPECT_FALSE(silent_observation.Answered());
    // The lateness under test: well beyond the former limits (100 ms, and
    // 1 s on the ordinary socket). Not a synchronisation.
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    silent_component.Send(model_read);
    const auto late_model = silent_component.Reply();
    ASSERT_TRUE(late_model.has_value());
    EXPECT_TRUE(routed(*late_model)) << content_identity::canonical_json(*late_model);
    silent_control.Send(read_stop());
    const auto late_read = silent_control.Reply();
    ASSERT_TRUE(late_read.has_value());
    EXPECT_NE(late_read->find_object_member("stop_status"), nullptr)
        << content_identity::canonical_json(*late_read);
    silent_ordinary.Send(admission_get);
    const auto late_admission = silent_ordinary.Reply();
    ASSERT_TRUE(late_admission.has_value());
    EXPECT_TRUE(routed(*late_admission))
        << content_identity::canonical_json(*late_admission);
    silent_observation.Send(observation_read);
    const auto late_observation = silent_observation.Reply();
    ASSERT_TRUE(late_observation.has_value());
    EXPECT_TRUE(observed(*late_observation))
        << content_identity::canonical_json(*late_observation);
    // The first session goes on on its same connection.
    session.Send(observation_read);
    const auto next_observation = session.Reply();
    ASSERT_TRUE(next_observation.has_value());
    EXPECT_TRUE(observed(*next_observation))
        << content_identity::canonical_json(*next_observation);
  }
  // A connection closed without a request is dropped; its lane goes on.
  {
    { const LaneClient closed(component); }
    { const LaneClient closed(control); }
    { const LaneClient closed(ordinary); }
    { const LaneClient closed(observation); }
    EXPECT_TRUE(routed(ExecutionPacket(component, model_read)));
    EXPECT_NE(ExecutionPacket(control, read_stop()).find_object_member("stop_status"),
              nullptr);
    EXPECT_TRUE(routed(ExecutionPacket(ordinary, admission_get)));
    EXPECT_TRUE(observed(ExecutionPacket(observation, observation_read)));
  }
  // The host stops while connections still wait for their request: each
  // lane ends at once and closes them unanswered. A request served after
  // each silent connection shows that the lane had accepted it.
  const LaneClient waiting_component(component);
  const LaneClient waiting_control(control);
  const LaneClient waiting_ordinary(ordinary);
  const LaneClient waiting_observation(observation);
  EXPECT_TRUE(routed(ExecutionPacket(component, model_read)));
  EXPECT_NE(ExecutionPacket(control, read_stop()).find_object_member("stop_status"),
            nullptr);
  EXPECT_TRUE(routed(ExecutionPacket(ordinary, admission_get)));
  EXPECT_TRUE(observed(ExecutionPacket(observation, observation_read)));
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_FALSE(waiting_component.Reply().has_value());
  EXPECT_FALSE(waiting_control.Reply().has_value());
  EXPECT_FALSE(waiting_ordinary.Reply().has_value());
  EXPECT_FALSE(waiting_observation.Reply().has_value());
}

TEST(RetainedExecutionHostUnitAdapter,
     PartialAuthoredDuplicateCannotShadowInstalledChildPackage) {
  NativeFixture fixture;
  ScopedTemporaryRoot authored;
  const auto bundle_filename =
      fixture.bundle->retained_bundle_id().value().substr(7U) + ".json";
  const auto authored_bundle_directory =
      authored.path() / "retained-bundles" / "sha256";
  ASSERT_TRUE(std::filesystem::create_directories(authored_bundle_directory));
  ASSERT_TRUE(std::filesystem::copy_file(
      fixture.root / "retained-package" / "retained-bundles" / "sha256" /
          bundle_filename,
      authored_bundle_directory / bundle_filename));

  auto adapter = component_manager::RetainedExecutionHostUnitAdapter(
      kTestStartId, MANYFORGE_EXECUTION_HOST_TARGET_ROOT, fixture.root / "retained-package",
      fixture.manager_run_root.path(),
      fixture.execution_state_root.path(), fixture.execution_run_root.path(),
      fixture.bundle_source, fixture.artifact_source,
      std::string(contract::kOperatingSystem),
      std::string(contract::kLaunchEnvironmentContract), authored.path());

  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedExecutionHostUnitAdapter,
     ManagedChildReadsThePinnedPackageAfterPathReplacement) {
  NativeFixture fixture;
  ScopedTemporaryRoot package_parent;
  const auto mutable_root = package_parent.path() / "retained-package";
  std::error_code copy_error;
  std::filesystem::copy(fixture.root / "retained-package", mutable_root,
                        std::filesystem::copy_options::recursive, copy_error);
  ASSERT_FALSE(copy_error) << copy_error.message();

  const int package_descriptor =
      ::open(mutable_root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  ASSERT_GE(package_descriptor, 0);
  const auto descriptor_root = std::filesystem::path("/proc/self/fd") /
                               std::to_string(package_descriptor);
  auto adapter = component_manager::RetainedExecutionHostUnitAdapter(
      kTestStartId, MANYFORGE_EXECUTION_HOST_TARGET_ROOT, descriptor_root,
      fixture.manager_run_root.path(),
      fixture.execution_state_root.path(), fixture.execution_run_root.path(),
      fixture.bundle_source, fixture.artifact_source,
      std::string(contract::kOperatingSystem),
      std::string(contract::kLaunchEnvironmentContract));

  const auto displaced_root = package_parent.path() / "retained-package-a";
  std::filesystem::rename(mutable_root, displaced_root);
  ASSERT_TRUE(std::filesystem::create_directory(mutable_root));

  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const auto progress = ObserveUntilReport(adapter, *fixture.query, epoch);
  ASSERT_TRUE(progress.has_value());
  EXPECT_EQ(progress->product_identity(),
            fixture.query->plan.product_identity.value());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  static_cast<void>(::close(package_descriptor));
}

TEST(RetainedExecutionHostUnitAdapter,
     CompatibleImplementationReplacementNeedsNoBinaryProof) {
  NativeFixture package;
  ASSERT_TRUE(package.product.has_value());
  ScopedTemporaryRoot target_root_a;
  ScopedTemporaryRoot target_root_b;
  const auto candidate_worker_source =
      std::filesystem::path(MANYFORGE_EXECUTION_HOST_TARGET_ROOT) /
      std::string(contract::kCandidateWorkerInstalledTargetId);
  const auto target_a = InstallNativeTestTargets(
      std::filesystem::path(MANYFORGE_EXECUTION_HOST_TARGET_ROOT) /
          std::string(contract::kInstalledTargetId),
      candidate_worker_source, target_root_a.path());
  const auto target_b =
      InstallNativeTestTargets(MANYFORGE_NATIVE_PICK_PLACE_PLANNING_TARGET,
                               candidate_worker_source, target_root_b.path());
  ASSERT_NE(content_identity::sha256_digest(ReadFile(target_a)),
            content_identity::sha256_digest(ReadFile(target_b)));

  // Both distinct binaries embed the same semantic runtime offer. Replacement
  // remains a clean Stop/start boundary, not a live executable swap.
  {
    auto adapter_a = MakeAdapter(package, target_root_a.path());
    const auto started_a = adapter_a.EnsureRunning(*package.query);
    ASSERT_EQ(started_a.state(),
              component_manager::ProcessPresenceState::kPresent);
    ASSERT_TRUE(ObserveUntilReport(
                    adapter_a, *package.query,
                    core::RuntimeEpoch(started_a.process_incarnation()))
                    .has_value());
    EXPECT_EQ(adapter_a.EnsureStopped(*package.query).state(),
              component_manager::ProcessPresenceState::kAbsent);
    EXPECT_EQ(adapter_a.ObserveProcess(*package.query).state(),
              component_manager::ProcessPresenceState::kAbsent);
  }

  {
    auto adapter_b = MakeAdapter(package, target_root_b.path());
    const auto first_b = adapter_b.EnsureRunning(*package.query);
    ASSERT_EQ(first_b.state(),
              component_manager::ProcessPresenceState::kPresent);
    const core::RuntimeEpoch first_epoch(first_b.process_incarnation());
    ASSERT_TRUE(
        ObserveUntilReport(adapter_b, *package.query, first_epoch).has_value());
    EXPECT_EQ(adapter_b.EnsureStopped(*package.query).state(),
              component_manager::ProcessPresenceState::kAbsent);
    EXPECT_EQ(adapter_b.ObserveProcess(*package.query).state(),
              component_manager::ProcessPresenceState::kAbsent);

  }
}

TEST(RetainedExecutionHostUnitAdapter,
     PrivateManagedChildOwnsDurableExactApplyFenceLifecycle) {
  NativeFixture fixture;
  const auto publication = PublishActiveFixture(fixture);
  auto adapter =
      MakeAdapter(fixture, MANYFORGE_EXECUTION_HOST_TARGET_ROOT, publication);
  const auto first = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(first.state(), component_manager::ProcessPresenceState::kPresent);

  const core::ConfigurationFenceAdmissionQueryDTO observation_query{{}};
  const auto observation = adapter.ObserveAdmission(observation_query);
  ASSERT_TRUE(observation.matches(observation_query));
  EXPECT_EQ(observation.active_projection_revision,
            publication.request.candidate_projection_revision);
  EXPECT_EQ(observation.active_public_pointer_digest,
            publication.durable_public_pointer_content_digest);
  EXPECT_EQ(observation.configuration_fence_generation.value(), 0U);
  EXPECT_TRUE(observation.active_control_configurations.empty());

  const core::ApplyConfigurationFenceIntentDTO intent{
      core::PublicRequestContextDTO{TestRequestKey(1U),
                                    "retained-adapter-test"},
      core::ApplyFenceOwnerDTO{core::ApplyId(1U), core::ApplyGeneration(1U)},
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
  competing_intent.owner_request_context.request_key = TestRequestKey(2U);
  competing_intent.owner_transaction.apply_id = core::ApplyId(2U);
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
      competing.configuration_fence_generation, competing.captured_scope};
  const auto competing_status = adapter.QueryStatus(competing_status_query);
  ASSERT_EQ(competing_status.state, core::ConfigurationFenceState::kHeld);
  const auto competing_release =
      adapter.Release(core::ConfigurationFenceReleaseDTO{
          competing_status.reconciliation_evidence});
  EXPECT_EQ(competing_release.request.reconciliation_evidence,
            competing_status.reconciliation_evidence);

  const core::ApplyFenceAcquireAttemptQueryDTO attempt_query{
      intent, core::ContentDigest(std::string(kIntentDigest))};
  const auto attempt = adapter.ReconcileAcquireAttempt(attempt_query);
  ASSERT_TRUE(attempt.matches(attempt_query));
  EXPECT_EQ(attempt.state,
            core::ApplyFenceAcquireAttemptReconcileState::kAcceptedExact);
  ASSERT_TRUE(attempt.accepted_fence.has_value());
  EXPECT_EQ(attempt.accepted_fence->configuration_fence_id,
            accepted.configuration_fence_id);

  const core::ConfigurationFenceStatusQueryDTO status_query{
      intent.owner_transaction, accepted.configuration_fence_id,
      accepted.configuration_fence_generation, accepted.captured_scope};
  const auto held = adapter.QueryStatus(status_query);
  ASSERT_TRUE(held.matches(status_query));
  EXPECT_EQ(held.state, core::ConfigurationFenceState::kHeld);
  EXPECT_TRUE(held.admission_fenced);
  EXPECT_EQ(held.reconciliation_evidence.active_projection_revision,
            publication.request.candidate_projection_revision);

  const core::ConfigurationFenceReleaseDTO release_request{
      held.reconciliation_evidence};
  const auto released = adapter.Release(release_request);
  ASSERT_TRUE(released.valid());
  EXPECT_EQ(released.request, release_request);
  const auto released_status = adapter.QueryStatus(status_query);
  ASSERT_TRUE(released_status.matches(status_query));
  EXPECT_EQ(released_status.state, core::ConfigurationFenceState::kReleased);
  ASSERT_TRUE(released_status.release_receipt.has_value());
  EXPECT_EQ(*released_status.release_receipt, released);

  EXPECT_EQ(adapter.Release(release_request), released);
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedExecutionHostUnitAdapter,
     FailedSameBindingCandidateHealthNeverReplacesPublishedModel) {
  ScopedTemporaryRoot merged;
  const auto fixture_root = merged.path() / "fixture";
  std::filesystem::copy(MANYFORGE_NATIVE_EXECUTION_FIXTURE_ROOT, fixture_root,
                        std::filesystem::copy_options::recursive);
  NativeFixture successor(MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT);
  std::filesystem::copy(successor.root / "retained-package",
                        fixture_root / "retained-package",
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::skip_existing);
  NativeFixture fixture(fixture_root);
  const auto publication = PublishActiveFixture(fixture);
  ScopedTemporaryRoot projection_control;
  auto adapter =
      MakeAdapter(fixture, MANYFORGE_NATIVE_PROJECTION_TEST_TARGET_ROOT);
  ProjectionConstructionHold projection_hold(projection_control.path(),
                                             "rebuild");
  ASSERT_TRUE(projection_hold.Configure());
  ASSERT_TRUE(projection_hold.Release());
  // Reuse the existing construction-failure boundary, present only in this
  // fixture host. Restore the parent test environment on every exit.
  struct RestoreFailureSelection final {
    std::optional<std::string> previous;
    ~RestoreFailureSelection() {
      if (previous.has_value()) {
        static_cast<void>(::setenv("MANYFORGE_NATIVE_PROJECTION_TEST_FAIL",
                                   previous->c_str(), 1));
      } else {
        static_cast<void>(::unsetenv("MANYFORGE_NATIVE_PROJECTION_TEST_FAIL"));
      }
    }
  };
  const char *const previous_failure =
      std::getenv("MANYFORGE_NATIVE_PROJECTION_TEST_FAIL");
  const RestoreFailureSelection restore_failure{
      previous_failure != nullptr ? std::optional<std::string>(previous_failure)
                                  : std::nullopt};
  ASSERT_EQ(::setenv("MANYFORGE_NATIVE_PROJECTION_TEST_FAIL", "rebuild", 1), 0);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const auto binding = NativeProjectionBinding(fixture, epoch);
  ASSERT_EQ(binding, NativeProjectionBinding(successor, epoch));
  const auto resolved = adapter.ResolveProjection(binding);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  auto &projection = *resolved.value();
  const auto initial_scene =
      MakeEmptyScene(fixture.bundle->value().cell_graph.cell_id, 1U);
  const auto initial_revision =
      publication.request.candidate_projection_revision;
  const core::NativeProjectionBootstrapStatusQueryDTO bootstrap{
      core::BootstrapId("failed-stage-bootstrap"),
      core::BootstrapGeneration(1U), binding, initial_revision};
  ASSERT_TRUE(BootstrapNativeProjectionSet(fixture, adapter, bootstrap,
                                           initial_scene, projection));
  const auto published_health =
      projection.ProjectionHealthForRevision(initial_revision);
  ASSERT_TRUE(published_health.healthy_for_target(initial_revision));
  const core::ProjectionLeaseId old_lease("failed-stage.published-lease");
  const auto old_retained =
      projection.RetainProjectionBinding(initial_revision, old_lease);
  ASSERT_TRUE(old_retained.ok()) << old_retained.error().message;

  const auto prepared = adapter.EnsureRunning(*successor.query);
  ASSERT_EQ(prepared.state(),
            component_manager::ProcessPresenceState::kPresent);
  ASSERT_EQ(prepared.process_incarnation(), started.process_incarnation());
  // The candidate uses the genuine predecessor Scene unchanged. Failure is
  // at the actual Rebuild worker boundary, before candidate readiness; the
  // parent Manager exclusions remain a separate Scene/host qualification.
  const core::ProjectionRevisionDTO candidate_revision{
      successor.bundle->retained_bundle_id(),
      successor.bundle->value().cell_graph.graph_revision,
      initial_scene.shared()->revision};
  const core::NativeProjectionRebuildDTO rebuild{
      binding, candidate_revision,
      successor.bundle->value().compiled_products.planning_collision_model,
      initial_scene.shared()};
  ASSERT_TRUE(rebuild.valid());
  const auto accepted = projection.RebuildProjection(rebuild);
  ASSERT_TRUE(accepted.presence_valid());
  ASSERT_EQ(accepted.result, core::ProjectionCommitResultStateDTO::kAccepted);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  auto failed_health =
      projection.ProjectionHealthForRevision(candidate_revision);
  while (failed_health.state != core::ProjectionHealthState::kUnavailable &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    failed_health = projection.ProjectionHealthForRevision(candidate_revision);
  }
  ASSERT_EQ(failed_health.state, core::ProjectionHealthState::kUnavailable);
  EXPECT_EQ(failed_health.execution_host_binding, binding);
  ASSERT_TRUE(failed_health.error.has_value());
  EXPECT_EQ(failed_health.error->details.at("reason"),
            "native_projection_rebuild_worker_failed");
  EXPECT_NE(failed_health.error->message.find(
                "injected native projection construction failure"),
            std::string::npos);
  EXPECT_FALSE(failed_health.active_projection_revision.has_value());
  EXPECT_FALSE(failed_health.healthy_for_target(candidate_revision));
  EXPECT_TRUE(projection.ProjectionHealthForRevision(initial_revision)
                  .healthy_for_target(initial_revision));
  EXPECT_EQ(projection.ProjectionHealthForRevision(initial_revision)
                .native_projection_generation,
            published_health.native_projection_generation);
  const auto retained_replay =
      projection.RetainProjectionBinding(initial_revision, old_lease);
  ASSERT_TRUE(retained_replay.ok()) << retained_replay.error().message;
  EXPECT_EQ(retained_replay.value(), old_retained.value());
  const auto failed_replay = projection.RebuildProjection(rebuild);
  EXPECT_EQ(failed_replay.result,
            core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain);
  EXPECT_EQ(failed_replay.resulting_health.state,
            core::ProjectionHealthState::kUnavailable);
  const auto candidate_retained = projection.RetainProjectionBinding(
      candidate_revision, core::ProjectionLeaseId("failed-stage.candidate"));
  EXPECT_FALSE(candidate_retained.ok());
  try {
    static_cast<void>(adapter.EnsureRunning(*fixture.query));
    ADD_FAILURE()
        << "failed staged slot silently replaced the retained predecessor";
  } catch (
      const component_manager::RetainedExecutionHostUnitAdapterError &error) {
    EXPECT_NE(std::string(error.what()).find("predecessor is still retained"),
              std::string::npos)
        << error.what();
  }
  EXPECT_EQ(adapter.ObserveProcess(*successor.query).process_incarnation(),
            started.process_incarnation());
  EXPECT_EQ(projection.ProjectionHealthForRevision(candidate_revision).state,
            core::ProjectionHealthState::kUnavailable);
  const auto trace = ReadFile(projection_control.path() / "trace.log");
  const auto execution =
      "execute rebuild " + binding.provider_binding_id.value() + "\n";
  const auto first_execution = trace.find(execution);
  ASSERT_NE(first_execution, std::string::npos);
  EXPECT_EQ(trace.find(execution, first_execution + execution.size()),
            std::string::npos)
      << "exact failed replay repeated construction";
  projection.ReleaseProjection(old_lease);
  EXPECT_EQ(adapter.EnsureStopped(*successor.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}
// OD-270: a projection rebuild has no time limit. One slower than the former
// 10 s bound is answered and the host is not late; meanwhile the unit is
// `rebuilding` and other calls answer busy at once instead of waiting.
TEST(RetainedExecutionHostUnitAdapter,
     SlowProjectionRebuildIsAnsweredWithoutATimeLimitAndOtherCallsAreBusy) {
  NativeFixture fixture;
  static_cast<void>(PublishActiveFixture(fixture));
  ScopedTemporaryRoot projection_control;
  auto adapter =
      MakeAdapter(fixture, MANYFORGE_NATIVE_PROJECTION_TEST_TARGET_ROOT);
  ProjectionConstructionHold bootstrap_hold(projection_control.path(),
                                            "bootstrap_rebuild");
  ASSERT_TRUE(bootstrap_hold.Configure());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch first_epoch(started.process_incarnation());
  const auto binding = NativeProjectionBinding(fixture, first_epoch);
  const auto resolved = adapter.ResolveProjection(binding);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;

  const auto initial_scene =
      MakeEmptyScene(fixture.bundle->value().cell_graph.cell_id, 1U);
  const core::ProjectionRevisionDTO initial_revision{
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      initial_scene.shared()->revision};
  const core::NativeProjectionBootstrapRebuildDTO bootstrap{
      core::BootstrapId("native-adapter-bootstrap"),
      core::BootstrapGeneration(1U),
      binding,
      initial_revision,
      fixture.bundle->value().compiled_products.planning_collision_model,
      initial_scene};
  auto rebuilt = std::async(std::launch::async, [&] {
    return resolved.value()->RebuildBootstrapProjection(bootstrap);
  });
  const auto entered_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!std::filesystem::exists(bootstrap_hold.Entered()) &&
         std::chrono::steady_clock::now() < entered_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(std::filesystem::exists(bootstrap_hold.Entered()));
  const auto entered = std::chrono::steady_clock::now();

  const auto busy = adapter.ResolveProjection(binding);
  ASSERT_FALSE(busy.ok());
  EXPECT_EQ(busy.error().code, component_manager::kRuntimeUnitLaunchInProgressCode);
  EXPECT_EQ(busy.error().details.at("phase"), "rebuilding");
  try {
    static_cast<void>(adapter.ObserveAdmission(
        core::ConfigurationFenceAdmissionQueryDTO{{}}));
    ADD_FAILURE() << "fence observation waited for or passed the rebuild";
  } catch (const core::TypedError &error) {
    EXPECT_EQ(error.code(), component_manager::kRuntimeUnitLaunchInProgressCode);
  }
  EXPECT_TRUE(adapter.HasHostChild());
  EXPECT_LT(std::chrono::steady_clock::now() - entered, std::chrono::seconds(1));

  // Past the former 10 s bound the rebuild still waits and the host is not
  // late; its reply then answers the call.
  std::this_thread::sleep_until(entered + std::chrono::milliseconds(10500));
  EXPECT_EQ(rebuilt.wait_for(std::chrono::seconds(0)),
            std::future_status::timeout);
  EXPECT_FALSE(adapter.HostReplyOutstanding().has_value());
  ASSERT_TRUE(bootstrap_hold.Release());
  const auto result = rebuilt.get();
  EXPECT_EQ(result.execution_host_binding, binding);
  EXPECT_NO_THROW(static_cast<void>(resolved.value()->ProjectionHealth()));
  EXPECT_FALSE(adapter.HostReplyOutstanding().has_value());
  EXPECT_FALSE(adapter.HostExit().has_value());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedExecutionHostUnitAdapter,
     SupersededFirstLivenessRenewalIsRejectedWithoutKillingChild) {
  NativeFixture fixture;
  const auto predecessor = PublishActiveFixture(fixture);
  auto successor_projection = predecessor.request.candidate_projection_revision;
  ++successor_projection.scene_revision.revision_id;
  const core::ActiveBundlePublicationReceiptDTO successor{
      {core::NamedApplyPublicationTransactionDTO{
           core::ApplyId(2U), core::ApplyGeneration(1U)},
       successor_projection, successor_projection.retained_bundle_id,
       successor_projection.graph_revision, predecessor.publication_generation,
       core::ContentDigest(std::string(kIntentDigest))},
      core::ActiveBundlePublicationGeneration(
          predecessor.publication_generation.value() + 1U),
      core::ContentDigest(std::string(kIntentDigest))};
  ASSERT_TRUE(successor.valid());
  auto adapter =
      MakeAdapter(fixture, MANYFORGE_EXECUTION_HOST_TARGET_ROOT, successor);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  // A fence request states the successor to the host first.
  static_cast<void>(
      adapter.ObserveAdmission(core::ConfigurationFenceAdmissionQueryDTO{{}}));

  const auto &policy = deployment::execution_runtime_contract::manager_liveness_policy();
  const core::RuntimeEpoch runtime_epoch(started.process_incarnation());
  const std::vector<core::RequiredManagerLivenessReceiverDTO> receivers{
      {fixture.query->plan.runtime_unit_id, runtime_epoch,
       policy.runtime_policy_id, policy.runtime_policy_revision}};
  const auto required_set_digest =
      core::ComputeRequiredRuntimeSetDigest(receivers);
  const core::RuntimeUnitId manager_id(
      "component-manager.native-adapter-race-test");
  const core::ManagerLivenessRequiredSetDTO required_set{
      fixture.bundle->value().cell_graph.cell_id,
      predecessor.request.candidate_retained_bundle_id,
      required_set_digest,
      manager_id,
      receivers};
  const core::ManagerHeartbeatDTO heartbeat{required_set.cell_id,
                                            manager_id,
                                            1U,
                                            policy.runtime_policy_id,
                                            policy.runtime_policy_revision};

  EXPECT_THROW(static_cast<void>(adapter.RenewManagerLiveness(
                   {fixture.query->plan.runtime_unit_id, predecessor,
                    required_set, policy, heartbeat, std::nullopt})),
               component_manager::ManagerLivenessPublicationSuperseded);
  const auto after = adapter.ObserveProcess(*fixture.query);
  EXPECT_EQ(after.state(), component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(after.process_incarnation(), runtime_epoch.value());
  ASSERT_TRUE(
      ObserveUntilReport(adapter, *fixture.query, runtime_epoch).has_value());
}

TEST(RetainedExecutionHostUnitAdapter,
     RejectsForeignProductOrMissingFixedTargetBeforeStartingAChild) {
  NativeFixture fixture;
  auto adapter = MakeAdapter(fixture, MANYFORGE_EXECUTION_HOST_TARGET_ROOT);
  auto foreign = *fixture.query;
  foreign.plan.product_identity = core::ContentDigest(
      "sha256:"
      "2222222222222222222222222222222222222222222222222222222222222222");
  ASSERT_TRUE(foreign.valid());
  EXPECT_THROW(static_cast<void>(adapter.EnsureRunning(foreign)),
               component_manager::RetainedExecutionHostUnitAdapterError);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);

  ScopedTemporaryRoot empty_target_root;
  auto missing = MakeAdapter(fixture, empty_target_root.path());
  EXPECT_THROW(static_cast<void>(missing.EnsureRunning(*fixture.query)),
               component_manager::RetainedExecutionHostUnitAdapterError);
  EXPECT_EQ(missing.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);

  ScopedTemporaryRoot linked_target_root;
  std::filesystem::create_symlink(
      std::filesystem::path(MANYFORGE_EXECUTION_HOST_TARGET_ROOT) /
          std::string(contract::kInstalledTargetId),
      linked_target_root.path() / std::string(contract::kInstalledTargetId));
  auto linked = MakeAdapter(fixture, linked_target_root.path());
  EXPECT_THROW(static_cast<void>(linked.EnsureRunning(*fixture.query)),
               component_manager::RetainedExecutionHostUnitAdapterError);
  EXPECT_EQ(linked.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedExecutionHostUnitAdapter,
     ProductionActiveCoordinatorStartsObservesAndStopsTheNativeChild) {
  NativeFixture fixture;
  auto native = MakeAdapter(fixture, MANYFORGE_EXECUTION_HOST_TARGET_ROOT);
  FixedActivePlanSource source(MakeActivePlan(*fixture.query));
  std::int64_t now = 1'000'000;
  component_manager::ManagerActiveRuntimeCoordinator coordinator(
      source, native, native, [&now] { return ++now; });

  // The start's first observation has no consumed status reply yet.
  const auto initialized = coordinator.Initialize();
  EXPECT_EQ(initialized.state,
            component_manager::ManagerActiveRuntimeConvergenceState::kPending);
  const auto converged = PollUntil(
      [&] { return coordinator.Observe(); },
      [](const auto &result) {
        return result.state ==
               component_manager::ManagerActiveRuntimeConvergenceState::
                   kConverged;
      });
  EXPECT_EQ(
      converged.state,
      component_manager::ManagerActiveRuntimeConvergenceState::kConverged);
  ASSERT_EQ(converged.observations.observations().size(), 1U);
  EXPECT_TRUE(converged.observations.observations().front().converged());

  const auto stopped = coordinator.Shutdown();
  ASSERT_EQ(stopped.final_process_observations.size(), 1U);
  EXPECT_EQ(stopped.final_process_observations.front().state(),
            component_manager::ProcessPresenceState::kAbsent);
}

// Accepts exactly one fresh Apply fence; the real-process convergence run
// below needs nothing else from the Kernel.
class SingleAcquireFencePort final : public core::IManagerKernelApplyFencePort {
public:
  [[nodiscard]] core::ConfigurationFenceAdmissionSnapshotDTO ObserveAdmission(
      const core::ConfigurationFenceAdmissionQueryDTO &) const override {
    throw std::runtime_error("admission observation is outside this fixture");
  }

  [[nodiscard]] core::ConfigurationFenceAcceptedDTO
  Acquire(const core::ApplyConfigurationFenceIntentDTO &intent) override {
    ++acquire_calls;
    return {
        core::ConfigurationFenceId("fence-1"),
        core::ConfigurationFenceGeneration(
            intent.expected_fence_generation.value() + 1U),
        intent.scope,
        100,
        core::RequestReplayStatusDTO::kAcceptedNew,
    };
  }

  [[nodiscard]] core::ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const core::ApplyFenceAcquireAttemptQueryDTO &) const override {
    throw std::runtime_error("lost-acquire reconciliation is outside this fixture");
  }

  [[nodiscard]] core::ConfigurationFenceStatusDTO
  QueryStatus(const core::ConfigurationFenceStatusQueryDTO &) const override {
    throw std::runtime_error("release status is outside this fixture");
  }

  [[nodiscard]] core::ConfigurationFenceReleasedDTO
  Release(const core::ConfigurationFenceReleaseDTO &) override {
    throw std::runtime_error("release is outside this fixture");
  }

  int acquire_calls{0};
};

class FixedPublicationReader final
    : public core::IActiveBundlePublicationReader {
public:
  explicit FixedPublicationReader(core::ActiveBundlePublicationReceiptDTO receipt)
      : receipt_(std::move(receipt)) {}

  [[nodiscard]] core::Result<core::ActiveBundlePublicationReceiptDTO>
  ReadActiveBundlePublication(
      const core::ActiveBundlePublicationGeneration generation) const override {
    if (generation != receipt_.publication_generation) {
      return core::Result<core::ActiveBundlePublicationReceiptDTO>::Err(
          core::ErrorDTO{"publication_not_current",
                         "publication is not current", {}});
    }
    return core::Result<core::ActiveBundlePublicationReceiptDTO>::Ok(receipt_);
  }

private:
  core::ActiveBundlePublicationReceiptDTO receipt_;
};

// The only real-process run of the Apply convergence coordinator: the
// production adapter starts the actual execution host, reaches durable staged
// readiness, replays it without a second effect, and stops the child.
TEST(RetainedExecutionHostUnitAdapter,
     ConvergenceCoordinatorReachesDurableStagedReadyWithTheRealHost) {
  static constexpr std::string_view kOtherDigest =
      "sha256:2222222222222222222222222222222222222222222222222222222222222222";
  NativeFixture fixture;
  ScopedTemporaryRoot state_root;
  auto adapter = MakeAdapter(fixture, MANYFORGE_EXECUTION_HOST_TARGET_ROOT);
  const auto &bundle = *fixture.bundle;
  const auto &graph = bundle.value().cell_graph;
  const core::ContentDigest digest{std::string(kIntentDigest)};
  const core::ProjectionRevisionDTO projection{
      bundle.retained_bundle_id(), graph.graph_revision,
      core::SceneRevision{core::SceneId(std::string(kIntentDigest)), 1U,
                          digest}};
  const component_manager::ManagerApplyIntentDTO apply_intent{
      core::PublicRequestContextDTO{
          TestRequestKey(1U),
          "runtime-convergence-test",
      },
      core::ApplyId(1U),
      core::ApplyGeneration(1U),
      graph.cell_id,
      projection,
      bundle.retained_bundle_id(),
      graph.graph_revision,
      component_manager::ManagerApplyPreparedActivationDTO{
          digest,
          bundle.value().deployment_artifact_id,
          bundle.value().deployment_revision,
          core::ProgramBindingId(std::string(kIntentDigest)),
          core::ContentDigest(std::string(kOtherDigest)),
          "{}",
      },
      core::AdmissionGeneration(7U),
      std::nullopt,
  };
  const core::ActiveBundlePublicationReceiptDTO predecessor{
      core::ActiveBundlePublicationRequestDTO{
          core::BootstrapPublicationTransactionDTO{
              core::BootstrapId("bootstrap-1"), core::BootstrapGeneration(1U)},
          projection,
          bundle.retained_bundle_id(),
          graph.graph_revision,
          std::nullopt,
          digest,
      },
      core::ActiveBundlePublicationGeneration(3U),
      core::ContentDigest(std::string(kOtherDigest)),
  };
  component_manager::SqliteManagerApplyJournal journal(
      state_root.path() / "concrete-apply.sqlite3", graph.cell_id);
  FixedPublicationReader publication(predecessor);
  SingleAcquireFencePort kernel;
  component_manager::ManagerApplyFenceCoordinator fence_coordinator(journal, kernel);
  std::uint64_t now_ns = 1'000U;
  component_manager::ManagerApplyRuntimeConvergenceCoordinator coordinator(
      journal, publication, fixture.artifact_source, adapter, adapter,
      std::string(contract::kOperatingSystem),
      std::string(contract::kLaunchEnvironmentContract),
      [&now_ns] { return now_ns++; });
  // Each run observes once and never waits; the same Apply keeps its
  // progress request mark, so it is repeated until staged ready.
  const auto run = [&] {
    return PollUntil(
        [&] {
          return coordinator.ConvergeOrRecover(
              apply_intent, bundle, bundle,
              core::ActiveBundlePublicationGeneration(3U));
        },
        [](const auto &result) {
          return result.state == component_manager::
                                     ManagerRuntimeConvergenceResultState::
                                         kStagedReady;
        });
  };

  static_cast<void>(fence_coordinator.AcquireOrRecoverFence(
      apply_intent, bundle, bundle,
      component_manager::ManagerApplyFenceExpectationDTO{
          0U, core::ConfigurationFenceGeneration(0U), {}}));
  const auto first = run();
  ASSERT_EQ(first.state,
            component_manager::ManagerRuntimeConvergenceResultState::kStagedReady);
  ASSERT_TRUE(first.durable_staged_ready.has_value());
  ASSERT_EQ(
      first.durable_staged_ready->record.staged_ready.runtime_units.size(), 1U);
  const auto first_evidence =
      first.durable_staged_ready->record.staged_ready.runtime_units.front();
  EXPECT_EQ(first_evidence.source_report_sequence, 1U);

  const auto second = run();
  ASSERT_EQ(second.state,
            component_manager::ManagerRuntimeConvergenceResultState::kStagedReady);
  ASSERT_TRUE(second.durable_staged_ready.has_value());
  ASSERT_EQ(
      second.durable_staged_ready->record.staged_ready.runtime_units.size(),
      1U);
  const auto &second_evidence =
      second.durable_staged_ready->record.staged_ready.runtime_units.front();
  EXPECT_EQ(second_evidence.runtime_epoch, first_evidence.runtime_epoch);
  EXPECT_EQ(second_evidence.source_report_sequence, 1U);
  EXPECT_EQ(
      second.durable_staged_ready->record.staged_ready.staged_ready_generation,
      1U);
  EXPECT_FALSE(second.durable_staged_ready->inserted);
  EXPECT_EQ(kernel.acquire_calls, 1);
  const auto &durable_runtime = second.durable_runtime_intent.record;
  const component_manager::RuntimeUnitConvergenceQueryDTO cleanup_query{
      apply_intent.apply_id,
      apply_intent.apply_generation,
      bundle.retained_bundle_id(),
      graph.graph_revision,
      durable_runtime.runtime_convergence_intent_digest,
      durable_runtime.runtime_convergence_intent.runtime_units.front(),
  };
  EXPECT_EQ(adapter.EnsureStopped(cleanup_query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

// The peer's behaviour, carried as the Apply number.
enum class HostApplyMarker : std::uint64_t {
  kSucceed = 1U,
  kClose = 2U,
  kRpcFailure = 3U,
  kMalformed = 4U,
  kRefused = 5U,
};

// One Manager Apply on the host; the peer reads its marker
// from the Apply number.
[[nodiscard]] component_manager::HostApplyRequest
HostApplyAdapterRequest(const NativeFixture &fixture,
                        const HostApplyMarker marker) {
  return {{core::ApplyId(static_cast<std::uint64_t>(marker)),
           core::ApplyGeneration(1U)},
          fixture.bundle->retained_bundle_id(),
          fixture.query->candidate_graph_revision,
          {core::ControllableComponentId("selected.component")},
          {},
          {},
          {},
          std::nullopt};
}

volatile std::sig_atomic_t shutdown_probe_terms = 0;
void ShutdownProbeTerm(int) { shutdown_probe_terms = shutdown_probe_terms + 1; }

// Exercise the production Manager adapter using an actual private socket and
// direct child. This peer reports facts only; it has no Kernel/robot authority.
int RunShutdownPacketProbe(const int ordinary, const int private_channel,
                           const std::filesystem::path &root,
                           const core::RuntimeEpoch &epoch,
                           const std::string &ready_packet) {
  namespace rpc = component_manager::detail;
  const auto mode = ReadFile(root / "shutdown-probe.mode");
  struct sigaction action {};
  action.sa_handler = ShutdownProbeTerm;
  sigemptyset(&action.sa_mask);
  if (::sigaction(SIGTERM, &action, nullptr) != 0)
    throw std::runtime_error("shutdown packet probe handler failed");
  unsigned shutdown_packets = 0;
  bool ordinary_cut = false;
  bool stopping = false;
  rpc::NativeShutdownProgress progress{epoch.value()};
  std::optional<std::pair<std::chrono::steady_clock::time_point, std::string>>
      delayed_progress;
  unsigned status_requests = 0;
  while (!std::filesystem::exists(root / "shutdown-probe.release")) {
    if (!ordinary_cut && std::filesystem::exists(root / "shutdown-probe.cut")) {
      if (::shutdown(ordinary, SHUT_RD) != 0)
        throw std::runtime_error("shutdown packet probe cut failed");
      ordinary_cut = true;
      std::ofstream(root / "shutdown-probe.cut-ready") << "cut";
    }
    if (delayed_progress &&
        std::chrono::steady_clock::now() >= delayed_progress->first) {
      const auto &reply = delayed_progress->second;
      if (::send(ordinary, reply.data(), reply.size(), MSG_NOSIGNAL) !=
          static_cast<ssize_t>(reply.size()))
        throw std::runtime_error("delayed shutdown probe response failed");
      delayed_progress.reset();
    }
    if (shutdown_probe_terms != 0)
      stopping = true;
    if (stopping)
      progress.phase = rpc::NativeShutdownPhase::kExecutionDrain;
    {
      {
        std::ofstream facts(root / "shutdown-probe.facts.tmp");
        facts << shutdown_packets << ' ' << shutdown_probe_terms << ' '
              << stopping << ' ' << status_requests;
      }
      std::filesystem::rename(root / "shutdown-probe.facts.tmp",
                              root / "shutdown-probe.facts");
    }
    std::array<pollfd, 2> fds{{{ordinary_cut ? -1 : ordinary, POLLIN, 0},
                               {private_channel, POLLIN, 0}}};
    const auto ready = ::poll(fds.data(), fds.size(), 20);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      throw std::runtime_error("shutdown packet probe poll failed");
    for (std::size_t index = 0; index < fds.size(); ++index) {
      if ((fds[index].revents & POLLIN) == 0)
        continue;
      std::string packet(rpc::kMaximumManagerLivenessRpcPacketBytes, '\0');
      const auto bytes =
          ::recv(fds[index].fd, packet.data(), packet.size(), MSG_TRUNC);
      if (bytes <= 0 || static_cast<std::size_t>(bytes) > packet.size())
        throw std::runtime_error("shutdown packet probe request invalid");
      packet.resize(static_cast<std::size_t>(bytes));
      std::string response;
      if (index == 1U) {
        // The peer exercises actual adapter framing/ownership only. This
        // serialized acknowledgement is not a Kernel/expiry qualification.
        const auto request = rpc::DecodeManagerLivenessRpcRequest(packet);
        const auto &set = request.required_set;
        const auto receiver = std::find_if(
            set.required_receivers.begin(), set.required_receivers.end(),
            [&](const auto &row) {
              return row.runtime_unit_id == request.receiver_runtime_unit_id;
            });
        if (receiver == set.required_receivers.end())
          throw std::runtime_error("shutdown probe renewal identity mismatch");
        const core::ManagerLivenessStatusDTO status{
            set.cell_id,
            receiver->runtime_unit_id,
            receiver->runtime_epoch,
            set.manager_runtime_unit_id,
            receiver->runtime_policy_id,
            receiver->runtime_policy_revision,
            request.heartbeat.heartbeat_sequence,
            request.heartbeat.heartbeat_sequence,
            core::ManagerLivenessStateDTO::kCurrent,
            false,
            std::nullopt};
        response = rpc::EncodeManagerLivenessRpcResponse(
            core::ManagerLivenessObservationDTO{
                status, set.active_bundle_id, set.required_runtime_set_digest});
      } else if (rpc::IsNativeShutdownRpcPacket(packet)) {
        ++status_requests;
        if (rpc::DecodeNativeShutdownRpcRequest(packet) != epoch.value())
          throw std::runtime_error("shutdown packet probe epoch mismatch");
        if (mode == "silent")
          continue;
        auto observed = progress;
        response = rpc::EncodeNativeShutdownRpcResponse(observed);
        if (mode == "late") {
          if (delayed_progress)
            throw std::runtime_error(
                "shutdown status was queued while its reply remained pending");
          delayed_progress = std::pair{std::chrono::steady_clock::now() +
                                           std::chrono::milliseconds(1100),
                                       response};
          continue;
        }
      } else if (packet == "shutdown") {
        ++shutdown_packets;
        stopping = true;
        response = "shutdown-accepted";
      } else if (packet == "hello") {
        response = ready_packet;
      } else {
        const auto decoded = rpc::DecodeNativeProjectionRpcRequest(packet);
        const auto *prepare =
            std::get_if<rpc::NativeProjectionPrepareRequest>(&decoded);
        if (!prepare)
          throw std::runtime_error(
              "unexpected shutdown probe projection request");
        response = rpc::EncodeNativeProjectionRpcResponse(
            rpc::NativeProjectionPrepareResult{*prepare});
      }
      if (::send(fds[index].fd, response.data(), response.size(),
                 MSG_NOSIGNAL) != static_cast<ssize_t>(response.size()))
        throw std::runtime_error("shutdown packet probe response failed");
    }
  }
  std::ofstream(root / "shutdown-probe.final")
      << shutdown_packets << ' ' << shutdown_probe_terms << ' ' << stopping;
  return mode == "incomplete"
      ? component_manager::kExecutionHostShutdownIncompleteExitStatus : 0;
}

// This is only a packet peer: production admission/codec values are reused,
// but no planner, robot, engine or effect owner is implemented here.
int RunSelectedAdapterPacketChild(const int argc, char **argv) {
  namespace rpc = component_manager::detail;
  std::map<std::string, std::string> arguments;
  for (int index = 1; index + 1 < argc; index += 2) {
    if (!arguments.emplace(argv[index], argv[index + 1]).second)
      throw std::invalid_argument("duplicate managed packet fixture argument");
  }
  const auto channel =
      std::stoi(arguments.at("--manyforge-managed-runtime-channel-fd"));
  const auto package_fd =
      std::stoi(arguments.at("--manyforge-managed-package-root-fd"));
  const auto package_root =
      std::filesystem::path("/proc/self/fd") / std::to_string(package_fd);
  deployment::FilesystemVerifiedRetainedBundleSource bundles(package_root);
  deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(
      package_root);
  const auto bundle = bundles.get_verified(core::RetainedBundleId(
      arguments.at("--manyforge-managed-retained-bundle-id")));
  if (!bundle)
    throw std::runtime_error("managed packet fixture bundle missing");
  const auto admission =
      component_manager::VerifyExecutionHostUnitForHost(
          *bundle, core::RuntimeUnitId(arguments.at("--runtime-unit-id")),
          artifacts, contract::kOperatingSystem,
          contract::kLaunchEnvironmentContract);
  if (!admission)
    throw std::runtime_error("managed packet fixture admission missing");
  const core::RuntimeEpoch epoch(arguments.at("--application-start-id"));
  const auto run_root = std::filesystem::path(
      arguments.at("--manyforge-managed-execution-run-root"));
  if (arguments.contains("--manyforge-managed-site-settings")) {
    std::ofstream captured(run_root / "site.arguments", std::ios::binary);
    for (int index = 1; index < argc; ++index) {
      captured << argv[index];
      captured.put('\0');
    }
    if (!captured) throw std::runtime_error("site argv fixture capture failed");
  }
  if (std::filesystem::exists(run_root / "shutdown-probe.mode")) {
    return RunShutdownPacketProbe(
        channel,
        std::stoi(arguments.at("--manyforge-managed-liveness-channel-fd")),
        run_root, epoch, admission->expected_ready_packet(epoch));
  }
  std::uint64_t sequence = 0U;
  // Reply-late mode: the first request of the kind named in
  // `reply-late.mode` is answered only once the test releases it, so the
  // Manager's bounded exchange times out and the reply arrives late. In this
  // mode every received request kind is logged, so a test sees what was sent.
  bool late_reply_held = false;
  const auto hold_late_reply = [&](const std::string_view kind) {
    if (!std::filesystem::exists(run_root / "reply-late.mode")) return;
    std::ofstream(run_root / "packets.log", std::ios::app) << kind << '\n';
    if (late_reply_held || ReadFile(run_root / "reply-late.mode") != kind)
      return;
    late_reply_held = true;
    std::ofstream(run_root / "reply-late.entered") << "entered\n";
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!std::filesystem::exists(run_root / "reply-late.release") &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  };
  for (;;) {
    std::string packet(rpc::kMaximumSelectedRuntimeCompositionRpcPacketBytes,
                       '\0');
    const auto count = ::recv(channel, packet.data(), packet.size(), MSG_TRUNC);
    if (count <= 0)
      return 0;
    if (static_cast<std::size_t>(count) > packet.size())
      throw std::runtime_error("managed packet fixture input oversized");
    packet.resize(static_cast<std::size_t>(count));
    if (packet == "shutdown")
      return 0;
    std::string response;
    if (packet == "hello") {
      // Holds the ready handshake until the test releases it, so the
      // Manager-side launch stays in its "launching" phase.
      if (std::filesystem::exists(run_root / "ready-hold.mode")) {
        std::ofstream(run_root / "ready-hold.entered") << "entered\n";
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!std::filesystem::exists(run_root / "ready-hold.release") &&
               std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      }
      response = admission->expected_ready_packet(epoch);
    } else if (packet == "status") {
      // Status-fail mode: "close" closes the channel and stays alive until
      // signalled; "exit" exits without a reply.
      if (std::filesystem::exists(run_root / "status-fail.mode")) {
        if (ReadFile(run_root / "status-fail.mode") == "exit") return 0;
        ::close(channel);
        for (;;) ::pause();
      }
      response = admission->expected_status_packet(epoch, ++sequence);
      hold_late_reply("status");
    } else if (rpc::IsKernelCellAdmissionRpcPacket(packet)) {
      // Packet peer only: a Reset authorization is accepted, anything else
      // is refused with a fixture code; no Kernel is modelled.
      const auto request = rpc::DecodeKernelCellAdmissionRpcRequest(packet);
      response =
          request.command == "cell_admission_validate_program_reset_scene"
              ? rpc::EncodeKernelCellAdmissionRpcResponse(request.command,
                                                          std::monostate{})
              : rpc::EncodeKernelCellAdmissionRpcResponse(
                    request.command,
                    core::ErrorDTO{"packet_peer_refused",
                                   "fixture has no Kernel", {}});
      hold_late_reply("cell_admission");
    } else if (rpc::IsKernelApplyFenceRpcPacket(packet)) {
      response = rpc::EncodeKernelApplyFenceRpcResponse(
          rpc::KernelApplyFenceRpcFailure{
              {"packet_peer_refused", "fixture has no Kernel", {}}});
      hold_late_reply("fence");
    } else if (rpc::IsSelectedRuntimeCompositionRpcPacket(packet) &&
               std::holds_alternative<
                   component_manager::ManagerComponentProcessInventoryRequestDTO>(
                   rpc::DecodeSelectedRuntimeCompositionRpcRequestVariant(
                       packet))) {
      // Inspection mode: "exit" exits without a reply; "hold" answers a
      // fixture failure only once the test releases it.
      const auto mode = std::filesystem::exists(run_root / "inspection.mode")
                            ? ReadFile(run_root / "inspection.mode")
                            : std::string{};
      if (mode == "exit") return 0;
      if (mode == "hold") {
        std::ofstream(run_root / "inspection.entered") << "entered\n";
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (!std::filesystem::exists(run_root / "inspection.release") &&
               std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
          rpc::SelectedRuntimeCompositionRpcFailure{
              {"packet_peer_failure", "fixture has no component processes", {}}});
    } else if (!rpc::IsSelectedRuntimeCompositionRpcPacket(packet)) {
      const auto request = rpc::DecodeNativeProjectionRpcRequest(packet);
      const auto *prepare =
          std::get_if<rpc::NativeProjectionPrepareRequest>(&request);
      if (prepare) {
        response = rpc::EncodeNativeProjectionRpcResponse(
            rpc::NativeProjectionPrepareResult{*prepare});
      } else {
        throw std::invalid_argument("unexpected packet fixture projection request");
      }
    } else {
      const auto request =
          rpc::DecodeSelectedRuntimeCompositionRpcRequestVariant(packet);
      const auto *apply = std::get_if<component_manager::HostApplyRequest>(&request);
      if (apply == nullptr)
        throw std::invalid_argument("managed packet fixture expected a host Apply");
      std::ofstream observed(run_root / "selected-test-last-request.bin",
                             std::ios::binary);
      observed.write(packet.data(),
                     static_cast<std::streamsize>(packet.size()));
      observed.close();
      if (!observed)
        throw std::runtime_error("managed packet fixture observation failed");
      const auto marker =
          static_cast<HostApplyMarker>(apply->apply.apply_id.value());
      if (marker == HostApplyMarker::kClose)
        return 0; // Sent request, lost response: outcome unknown.
      if (marker == HostApplyMarker::kRpcFailure) {
        response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
            rpc::SelectedRuntimeCompositionRpcFailure{
                {"packet_peer_failure", "response unavailable", {{"reason", "fixture"}}}});
      } else if (marker == HostApplyMarker::kMalformed) {
        response = "malformed selected response";
      } else if (marker == HostApplyMarker::kRefused) {
        response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
            component_manager::HostApplyResult{
                component_manager::HostApplyOutcome::kFailed,
                core::ErrorDTO{"apply_requires_inactive_component",
                               "Disable selected.component before this Apply replaces it.",
                               {{"component_id", "selected.component"}}}});
      } else {
        response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
            component_manager::HostApplyResult{
                component_manager::HostApplyOutcome::kSucceeded, std::nullopt});
      }
      hold_late_reply("host_apply");
    }
    if (::send(channel, response.data(), response.size(), MSG_NOSIGNAL) !=
        static_cast<ssize_t>(response.size()))
      throw std::runtime_error(
          "managed packet fixture response not sent atomically");
  }
}

[[nodiscard]] std::filesystem::path
InstallSelectedPacketTestTargets(const std::filesystem::path &root) {
  return InstallNativeTestTargets(
      "/proc/self/exe",
      std::filesystem::path(MANYFORGE_EXECUTION_HOST_TARGET_ROOT) /
          std::string(contract::kCandidateWorkerInstalledTargetId),
      root);
}

void CheckShutdownPacketProbe(const std::string &mode,
                              const bool cut_initial_send) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  const auto root = fixture.execution_run_root.path();
  std::ofstream(root / "shutdown-probe.mode") << mode;
  auto adapter = MakeAdapter(fixture, targets.path());
  struct ReleaseProbe {
    const std::filesystem::path &root;
    ~ReleaseProbe() {
      try {
        std::ofstream(root / "shutdown-probe.release") << "release";
      } catch (...) {
      } // Failure unwinding still attempts to release the child.
    }
  } release_before_adapter_destruction{root};
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const auto policy = deployment::execution_runtime_contract::manager_liveness_policy();
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const std::vector<core::RequiredManagerLivenessReceiverDTO> receivers{
      {fixture.query->plan.runtime_unit_id, epoch, policy.runtime_policy_id,
       policy.runtime_policy_revision}};
  const core::ManagerLivenessRequiredSetDTO required_set{
      fixture.bundle->value().cell_graph.cell_id,
      fixture.bundle->retained_bundle_id(),
      core::ComputeRequiredRuntimeSetDigest(receivers),
      core::RuntimeUnitId("manager.shutdown.probe"),
      receivers};
  const core::ManagerHeartbeatDTO heartbeat{
      required_set.cell_id,
      required_set.manager_runtime_unit_id,
      1U,
      policy.runtime_policy_id,
      policy.runtime_policy_revision};
  const component_manager::ManagerLivenessTransportRequestDTO renewal{
      fixture.query->plan.runtime_unit_id,
      FixturePublication(fixture),
      required_set,
      policy,
      heartbeat,
      std::nullopt};
  const auto first_renewal = adapter.RenewManagerLiveness(renewal);
  ASSERT_EQ(first_renewal.status.last_accepted_heartbeat_sequence, 1U);
  ManagerHeartbeatLane heartbeat_lane(adapter, renewal);
  if (cut_initial_send) {
    std::ofstream(root / "shutdown-probe.cut") << "cut";
    bool cut = false;
    for (int i = 0; i < 200 && !cut; ++i) {
      cut = std::filesystem::exists(root / "shutdown-probe.cut-ready");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(cut);
  }
  auto stopped = std::async(std::launch::async, [&] {
    return adapter.EnsureStopped(*fixture.query);
  });
  ReleaseProbe release_before_future_destruction{root};
  // Beyond the old automatic 2s escalation: unchanged/silent status is not
  // permission to signal, kill or declare the actual child absent.
  EXPECT_EQ(stopped.wait_for(std::chrono::milliseconds(2500)),
            std::future_status::timeout);
  const auto continuing_renewal =
      heartbeat_lane.WaitForAccepted(5U, std::chrono::seconds(2));
  EXPECT_TRUE(continuing_renewal.has_value()) << heartbeat_lane.Failure();
  heartbeat_lane.Stop();
  unsigned packets = 99;
  int terms = 99;
  bool stopping = false;
  unsigned status_requests = 99;
  std::istringstream(ReadFile(root / "shutdown-probe.facts")) >> packets >>
      terms >> stopping >> status_requests;
  EXPECT_EQ(packets, cut_initial_send ? 0U : 1U);
  EXPECT_EQ(terms, cut_initial_send ? 1 : 0);
  EXPECT_TRUE(stopping);
  if (cut_initial_send)
    EXPECT_EQ(status_requests, 0U);
  else if (mode == "silent")
    EXPECT_EQ(status_requests, 1U);
  else if (mode == "late")
    EXPECT_GE(status_requests, 2U);
  else
    EXPECT_GE(status_requests, 1U);
  std::ofstream(root / "shutdown-probe.release") << "release";
  EXPECT_EQ(stopped.get().state(),
            component_manager::ProcessPresenceState::kAbsent);
  packets = 99;
  terms = 99;
  stopping = false;
  std::istringstream(ReadFile(root / "shutdown-probe.final")) >> packets >>
      terms >> stopping;
  EXPECT_EQ(packets, cut_initial_send ? 0U : 1U);
  EXPECT_EQ(terms, cut_initial_send ? 1 : 0);
  EXPECT_TRUE(stopping);
}
// Packet peer only: verifies Manager's actual child reaping and status
// observation for both exit codes, not a real gripper shutdown.
TEST(RetainedExecutionHostUnitAdapter, ShutdownReaperDistinguishesCleanAndIncompleteExit) {
  for (const auto *mode : {"responsive", "incomplete"}) {
    SCOPED_TRACE(mode);
    testing::internal::CaptureStderr();
    std::string output;
    try {
      CheckShutdownPacketProbe(mode, false);
      output = testing::internal::GetCapturedStderr();
    } catch (...) {
      static_cast<void>(testing::internal::GetCapturedStderr());
      throw;
    }
    const auto reaped = output.find("execution host child exited: pid=");
    ASSERT_NE(reaped, std::string::npos) << output;
    const auto end = output.find('\n', reaped);
    ASSERT_NE(end, std::string::npos) << output;
    const auto line = output.substr(reaped, end - reaped);
    const auto expected = std::string(" status=") +
        (std::string_view(mode) == "incomplete"
             ? std::to_string(component_manager::kExecutionHostShutdownIncompleteExitStatus)
             : "0");
    EXPECT_TRUE(line.ends_with(expected)) << line;
    const auto exit_code = std::string_view(mode) == "incomplete"
        ? component_manager::kExecutionHostShutdownIncompleteExitStatus : 0;
    EXPECT_EQ(component_manager::execution_host_child_exit_reason(exit_code),
              std::string_view(mode) == "incomplete"
                  ? "execution_host_shutdown_incomplete" : "execution_host_child_exited");
  }
}

TEST(RetainedExecutionHostUnitAdapter,
     ShutdownWaitsForActualExitWithUnchangedProgress) {
  CheckShutdownPacketProbe("responsive", false);
}
TEST(RetainedExecutionHostUnitAdapter,
     DelayedShutdownStatusNeverRequestsSigterm) {
  CheckShutdownPacketProbe("silent", false);
}
TEST(RetainedExecutionHostUnitAdapter,
     LateShutdownStatusCannotPoisonConcurrentRenewal) {
  CheckShutdownPacketProbe("late", false);
}
TEST(RetainedExecutionHostUnitAdapter,
     InitialShutdownSendFailureRequestsSigtermOnlyOnce) {
  CheckShutdownPacketProbe("responsive", true);
}

std::atomic<bool> g_shutdown_force_requested{false};
bool ShutdownForceRequested() noexcept {
  return g_shutdown_force_requested.load(std::memory_order_acquire);
}

// OD-187 (1): the host's stop has no time limit; a second stop request (the
// Manager's force answer) ends the waiting host with SIGKILL, and the adapter
// reports the killed PID for the Manager's typed line.
TEST(RetainedExecutionHostUnitAdapter, SecondStopRequestKillsTheWaitingHost) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  const auto root = fixture.execution_run_root.path();
  // "responsive": the probe accepts the shutdown, answers status and never
  // exits by itself until released.
  std::ofstream(root / "shutdown-probe.mode") << "responsive";
  auto adapter = MakeAdapter(fixture, targets.path());
  g_shutdown_force_requested.store(false, std::memory_order_release);
  adapter.SetShutdownForce(&ShutdownForceRequested);
  struct ReleaseProbe {
    const std::filesystem::path &root;
    ~ReleaseProbe() {
      try {
        std::ofstream(root / "shutdown-probe.release") << "release";
      } catch (...) {
      }
    }
  } release_before_adapter_destruction{root};
  ASSERT_EQ(adapter.EnsureRunning(*fixture.query).state(),
            component_manager::ProcessPresenceState::kPresent);
  auto stopped = std::async(std::launch::async, [&] {
    return adapter.EnsureStopped(*fixture.query);
  });
  ReleaseProbe release_before_future_destruction{root};
  // The host accepted its shutdown and is followed through its status.
  bool followed = false;
  for (int i = 0; i < 500 && !followed; ++i) {
    unsigned packets = 0;
    int terms = 0;
    bool stopping = false;
    unsigned status_requests = 0;
    std::istringstream(ReadFile(root / "shutdown-probe.facts")) >> packets >>
        terms >> stopping >> status_requests;
    followed = packets == 1U && stopping && status_requests >= 1U;
    if (!followed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(followed);
  EXPECT_FALSE(adapter.HostForceKilledPid().has_value());
  EXPECT_FALSE(adapter.HostExit().has_value());
  g_shutdown_force_requested.store(true, std::memory_order_release);
  EXPECT_EQ(stopped.get().state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_TRUE(adapter.HostForceKilledPid().has_value());
  const auto exit = adapter.HostExit();
  ASSERT_TRUE(exit.has_value());
  EXPECT_TRUE(exit->signaled);
  EXPECT_EQ(exit->value, SIGKILL);
  // The probe never wrote its final facts: it was killed, not released.
  EXPECT_FALSE(std::filesystem::exists(root / "shutdown-probe.final"));
}

[[nodiscard]] component_manager::detail::SelectedRuntimeCompositionRpcRequest
ReadObservedSelectedRequest(const NativeFixture &fixture) {
  return component_manager::detail::
      DecodeSelectedRuntimeCompositionRpcRequestVariant(
          ReadFile(fixture.execution_run_root.path() /
                   "selected-test-last-request.bin"));
}

TEST(RetainedExecutionHostUnitAdapter,
     HostApplyRoundTripsTheRequestAndTheHostDecision) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  ASSERT_EQ(adapter.EnsureRunning(*fixture.query).state(),
            component_manager::ProcessPresenceState::kPresent);
  const auto unit = fixture.query->plan.runtime_unit_id;
  const auto request = HostApplyAdapterRequest(fixture, HostApplyMarker::kSucceed);
  const auto applied = adapter.ApplyOnHost(unit, request);
  ASSERT_TRUE(applied.ok()) << applied.error().message;
  EXPECT_EQ(applied.value().outcome, component_manager::HostApplyOutcome::kSucceeded);
  EXPECT_EQ(std::get<component_manager::HostApplyRequest>(ReadObservedSelectedRequest(fixture)),
            request);
  // A host refusal is a decision (known failure), not a transport error.
  const auto refused = adapter.ApplyOnHost(unit, HostApplyAdapterRequest(fixture, HostApplyMarker::kRefused));
  ASSERT_TRUE(refused.ok()) << refused.error().message;
  EXPECT_EQ(refused.value().outcome, component_manager::HostApplyOutcome::kFailed);
  ASSERT_TRUE(refused.value().error.has_value());
  EXPECT_EQ(refused.value().error->code, "apply_requires_inactive_component");
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedExecutionHostUnitAdapter,
     HostApplyRpcFailureMalformedAndLostReplyAreNotHostDecisions) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  ASSERT_EQ(adapter.EnsureRunning(*fixture.query).state(),
            component_manager::ProcessPresenceState::kPresent);
  const auto unit = fixture.query->plan.runtime_unit_id;
  const auto failed = adapter.ApplyOnHost(unit, HostApplyAdapterRequest(fixture, HostApplyMarker::kRpcFailure));
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "packet_peer_failure");
  EXPECT_ANY_THROW((void)adapter.ApplyOnHost(unit, HostApplyAdapterRequest(fixture, HostApplyMarker::kMalformed)));
  // The peer exits without a reply: the exchange ends on the host's exit,
  // typed (OD-290 (a)).
  const auto lost = adapter.ApplyOnHost(unit, HostApplyAdapterRequest(fixture, HostApplyMarker::kClose));
  ASSERT_FALSE(lost.ok());
  EXPECT_EQ(lost.error().code, component_manager::kExecutionHostExitedCode);
  EXPECT_EQ(lost.error().details.at("exchange"), "host_apply");
  EXPECT_TRUE(adapter.HostExit().has_value());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

// OD-290 (a): the launch handshake has no time limit; past the former 10 s
// bound it still completes on the host's readiness.
TEST(RetainedExecutionHostUnitAdapter,
     SlowLaunchHandshakeAnswersFenceAndClaimCallsBusyWithoutWaiting) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  const auto run_root = fixture.execution_run_root.path();
  std::ofstream(run_root / "ready-hold.mode") << "hold\n";
  auto adapter = MakeAdapter(fixture, targets.path());
  auto launch = std::async(std::launch::async, [&] {
    return adapter.EnsureRunning(*fixture.query);
  });
  const auto entered_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!std::filesystem::exists(run_root / "ready-hold.entered") &&
         std::chrono::steady_clock::now() < entered_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(std::filesystem::exists(run_root / "ready-hold.entered"));

  // While the child holds its handshake, every call answers at once with the
  // typed busy answer (B13, contract 1.3) instead of waiting on the launch.
  const auto started = std::chrono::steady_clock::now();
  try {
    static_cast<void>(adapter.ObserveAdmission(
        core::ConfigurationFenceAdmissionQueryDTO{{}}));
    ADD_FAILURE() << "fence observation waited for or passed the launch";
  } catch (const core::TypedError &error) {
    EXPECT_EQ(error.code(), component_manager::kRuntimeUnitLaunchInProgressCode);
    EXPECT_EQ(error.details().at("phase"), "launching");
    EXPECT_EQ(error.details().at("runtime_unit_id"),
              fixture.query->plan.runtime_unit_id.value());
  }
  try {
    static_cast<void>(adapter.GetAuthoringAdmission());
    ADD_FAILURE() << "cell-admission call waited for or passed the launch";
  } catch (const component_manager::RetainedExecutionHostUnitAdapterError &error) {
    EXPECT_NE(std::string(error.what()).find(
                  component_manager::kRuntimeUnitLaunchInProgressCode),
              std::string::npos)
        << error.what();
  }
  EXPECT_THROW(static_cast<void>(adapter.ObserveProcess(*fixture.query)),
               core::TypedError);
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::seconds(1));

  std::this_thread::sleep_until(started + std::chrono::milliseconds(10500));
  EXPECT_EQ(launch.wait_for(std::chrono::seconds(0)),
            std::future_status::timeout);
  std::ofstream(run_root / "ready-hold.release") << "release\n";
  ASSERT_EQ(launch.get().state(),
            component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

// The packet child's reply-late mode for one request kind. Declare it after
// the adapter: unwinding then releases the held reply before the adapter
// drains its child.
class LateReplyHold final {
public:
  LateReplyHold(std::filesystem::path run_root, const std::string_view kind)
      : root_(std::move(run_root)) {
    std::ofstream(root_ / "reply-late.mode") << kind;
  }
  LateReplyHold(const LateReplyHold &) = delete;
  LateReplyHold &operator=(const LateReplyHold &) = delete;
  ~LateReplyHold() { Release(); }

  void Release() const noexcept {
    try {
      std::ofstream(root_ / "reply-late.release") << "release\n";
    } catch (...) {
    }
  }
  [[nodiscard]] bool Entered() const {
    return std::filesystem::exists(root_ / "reply-late.entered");
  }
  // How many requests of `kind` the child received.
  [[nodiscard]] std::size_t Received(const std::string_view kind) const {
    if (!std::filesystem::exists(root_ / "packets.log")) return 0U;
    std::istringstream log(ReadFile(root_ / "packets.log"));
    std::size_t count = 0U;
    for (std::string line; std::getline(log, line);)
      if (line == kind) ++count;
    return count;
  }

private:
  std::filesystem::path root_;
};

[[nodiscard]] core::ProgramResetSceneAuthorizationDTO LateTestResetAuthorization() {
  return {core::ProgramRunNumber(7U), core::CellAdmissionClaimGeneration(1U),
          core::ContentDigest(std::string(kIntentDigest)), std::nullopt};
}

// While an exchange's reply is outstanding every other management call is
// answered execution_host_late at once and sends nothing (OD-290 (a)).
void ExpectEveryManagementCallAnswersOutstanding(
    component_manager::RetainedExecutionHostUnitAdapter &adapter,
    const NativeFixture &fixture, const core::RuntimeEpoch &epoch,
    const std::string_view exchange) {
  const auto started = std::chrono::steady_clock::now();
  const auto reset = adapter.ValidateProgramResetSceneAuthorization(
      LateTestResetAuthorization());
  ASSERT_FALSE(reset.ok());
  EXPECT_EQ(reset.error().code, component_manager::kExecutionHostLateCode);
  EXPECT_EQ(reset.error().details.at("exchange"), exchange);
  // Nothing was sent, so the refusal is before any effect (the Apply
  // journal reads `effect: none` as known).
  EXPECT_EQ(reset.error().details.at("effect"), "none");
  const auto apply = adapter.ApplyOnHost(
      fixture.query->plan.runtime_unit_id,
      HostApplyAdapterRequest(fixture, HostApplyMarker::kSucceed));
  ASSERT_FALSE(apply.ok());
  EXPECT_EQ(apply.error().code, component_manager::kExecutionHostLateCode);
  EXPECT_EQ(apply.error().details.at("effect"), "none");
  try {
    static_cast<void>(adapter.ObserveAdmission(
        core::ConfigurationFenceAdmissionQueryDTO{{}}));
    ADD_FAILURE() << "fence observation was sent behind an outstanding reply";
  } catch (const core::TypedError &error) {
    EXPECT_EQ(error.code(), component_manager::kExecutionHostLateCode);
  }
  // The status observation sends nothing and reports no new evidence.
  EXPECT_FALSE(adapter.ObserveProgress(*fixture.query, epoch, 0U).has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(1));
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kPresent);
  EXPECT_FALSE(adapter.HostExit().has_value());
}

// OD-290 (a): a status reply slower than the former 750 ms bound is consumed
// by a later observation. The observation never waits, nothing is halted or
// faulted, and a call made meanwhile is sent behind the status and answered.
TEST(RetainedExecutionHostUnitAdapter,
     SlowStatusReplyIsConsumedLaterWithoutWaiting) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  const auto run_root = fixture.execution_run_root.path();
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  ASSERT_TRUE(WaitUntilAnswering(adapter, fixture, epoch));
  LateReplyHold hold(run_root, "status");

  // The observation that consumes the last reply sends the held status.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!hold.Entered() && std::chrono::steady_clock::now() < deadline) {
    static_cast<void>(adapter.ObserveProgress(*fixture.query, epoch, 0U));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(hold.Entered());
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  // Past the former bound: each observation returns at once, without new
  // evidence; the host is kept and the outstanding reply is published.
  const auto observed = std::chrono::steady_clock::now();
  for (int tick = 0; tick < 3; ++tick)
    EXPECT_FALSE(adapter.ObserveProgress(*fixture.query, epoch, 0U).has_value());
  EXPECT_LT(std::chrono::steady_clock::now() - observed,
            std::chrono::milliseconds(100));
  const auto outstanding = adapter.HostReplyOutstanding();
  ASSERT_TRUE(outstanding.has_value());
  EXPECT_EQ(outstanding->exchange, "status");
  EXPECT_GE(outstanding->waited, std::chrono::milliseconds(1000));
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(hold.Received("status"), 1U);

  // A call is sent behind the status and answered once the host answers.
  auto reset = std::async(std::launch::async, [&] {
    return adapter.ValidateProgramResetSceneAuthorization(
        LateTestResetAuthorization());
  });
  EXPECT_EQ(reset.wait_for(std::chrono::milliseconds(200)),
            std::future_status::timeout);
  hold.Release();
  ASSERT_EQ(reset.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_TRUE(reset.get().ok());
  // The status reply read behind it is the next observation's evidence.
  EXPECT_TRUE(adapter.ObserveProgress(*fixture.query, epoch, 0U).has_value());
  EXPECT_EQ(hold.Received("cell_admission"), 1U);
  EXPECT_FALSE(adapter.HostExit().has_value());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

// OD-290 (a): fence, cell-admission and Host Apply exchanges slower than the
// former 750 ms bound are answered by the host's reply. Meanwhile every
// other call is answered at once and sends nothing.
TEST(RetainedExecutionHostUnitAdapter,
     SlowFenceCellAdmissionAndHostApplyAreAnsweredWithoutATimeLimit) {
  for (const std::string_view kind : {"fence", "cell_admission", "host_apply"}) {
    SCOPED_TRACE(kind);
    NativeFixture fixture;
    ScopedTemporaryRoot targets;
    static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
    const auto run_root = fixture.execution_run_root.path();
    auto adapter = MakeAdapter(fixture, targets.path());
    const auto started = adapter.EnsureRunning(*fixture.query);
    ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
    const core::RuntimeEpoch epoch(started.process_incarnation());
    LateReplyHold hold(run_root, kind);

    std::string_view exchange;
    std::future<std::string> answered;
    if (kind == "fence") {
      exchange = "fence_observe_admission";
      answered = std::async(std::launch::async, [&]() -> std::string {
        try {
          static_cast<void>(adapter.ObserveAdmission(
              core::ConfigurationFenceAdmissionQueryDTO{{}}));
          return "answered";
        } catch (const std::exception &error) {
          return error.what();
        }
      });
    } else if (kind == "cell_admission") {
      exchange = "cell_admission_validate_program_reset_scene";
      answered = std::async(std::launch::async, [&]() -> std::string {
        const auto reset = adapter.ValidateProgramResetSceneAuthorization(
            LateTestResetAuthorization());
        return reset.ok() ? "answered" : reset.error().code;
      });
    } else {
      exchange = "host_apply";
      answered = std::async(std::launch::async, [&]() -> std::string {
        const auto apply = adapter.ApplyOnHost(
            fixture.query->plan.runtime_unit_id,
            HostApplyAdapterRequest(fixture, HostApplyMarker::kSucceed));
        return apply.ok() ? "answered" : apply.error().code;
      });
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!hold.Entered() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_TRUE(hold.Entered());
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    EXPECT_EQ(answered.wait_for(std::chrono::seconds(0)),
              std::future_status::timeout);
    const auto outstanding = adapter.HostReplyOutstanding();
    ASSERT_TRUE(outstanding.has_value());
    EXPECT_EQ(outstanding->exchange, exchange);
    ExpectEveryManagementCallAnswersOutstanding(adapter, fixture, epoch,
                                                exchange);
    EXPECT_EQ(hold.Received(kind), 1U);

    hold.Release();
    ASSERT_EQ(answered.wait_for(std::chrono::seconds(10)),
              std::future_status::ready);
    const auto answer = answered.get();
    if (kind == "fence") {
      // The fixture's fence peer refuses; its refusal is the host's answer.
      EXPECT_NE(answer.find("packet_peer_refused"), std::string::npos) << answer;
    } else {
      EXPECT_EQ(answer, "answered");
    }
    EXPECT_EQ(hold.Received(kind), 1U);
    EXPECT_FALSE(adapter.HostReplyOutstanding().has_value());
    ASSERT_TRUE(WaitUntilAnswering(adapter, fixture, epoch));
    EXPECT_FALSE(adapter.HostExit().has_value());
    EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
              component_manager::ProcessPresenceState::kAbsent);
  }
}

TEST(RetainedExecutionHostUnitAdapter,
     FailedChannelWithTheHostAliveKeepsItsExchangeOutstandingUntilItIsStopped) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  std::ofstream(fixture.execution_run_root.path() / "status-fail.mode")
      << "close";

  // The child closes its channel and stays alive: its status stays
  // outstanding; the host is not stopped, reaped or erased.
  std::optional<component_manager::ExecutionHostReplyOutstandingDTO> outstanding;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    EXPECT_FALSE(adapter.ObserveProgress(*fixture.query, epoch, 0U).has_value());
    outstanding = adapter.HostReplyOutstanding();
    if (outstanding && outstanding->waited > std::chrono::milliseconds(100))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ASSERT_TRUE(outstanding.has_value());
  EXPECT_EQ(outstanding->exchange, "status");
  ExpectEveryManagementCallAnswersOutstanding(adapter, fixture, epoch, "status");

  // No reply can settle it: later observations send nothing.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  EXPECT_FALSE(adapter.ObserveProgress(*fixture.query, epoch, 0U).has_value());
  const auto still = adapter.HostReplyOutstanding();
  ASSERT_TRUE(still.has_value());
  EXPECT_EQ(still->episode, outstanding->episode);
  EXPECT_FALSE(adapter.HostExit().has_value());

  // The operator's stop still ends the host (its shutdown cannot be
  // delivered on the closed channel, so it is signalled).
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  const auto exit = adapter.HostExit();
  ASSERT_TRUE(exit.has_value());
  EXPECT_TRUE(exit->signaled);
  EXPECT_EQ(exit->value, SIGTERM);
}

TEST(RetainedExecutionHostUnitAdapter,
     FailedChannelOfAnExitedHostReportsItsExit) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  std::ofstream(fixture.execution_run_root.path() / "status-fail.mode")
      << "exit";

  // The exit is reaped at this observation, or at the next one when the
  // closed channel is seen before the child is waitable.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!adapter.HostExit().has_value() &&
         std::chrono::steady_clock::now() < deadline) {
    EXPECT_FALSE(adapter.ObserveProgress(*fixture.query, epoch, 0U).has_value());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto exit = adapter.HostExit();
  ASSERT_TRUE(exit.has_value());
  EXPECT_FALSE(exit->signaled);
  EXPECT_EQ(exit->value, 0);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_THROW(static_cast<void>(adapter.EnsureRunning(*fixture.query)),
               core::TypedError);
}

TEST(RetainedExecutionHostUnitAdapter, HostApplyWithoutARunningChildNeverStartsOne) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  EXPECT_ANY_THROW((void)adapter.ApplyOnHost(fixture.query->plan.runtime_unit_id,
                                              HostApplyAdapterRequest(fixture, HostApplyMarker::kSucceed)));
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_FALSE(std::filesystem::exists(fixture.execution_run_root.path() /
                                       "selected-test-last-request.bin"));
}

} // namespace

TEST(RetainedExecutionHostUnitAdapter,
     GenericSiteDocumentAddsOnlyItsRetainedSnapshotArgumentWhenConfigured) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  const auto arguments_path = fixture.execution_run_root.path() / "site.arguments";
  const std::string site_document =
      R"({"schema":"manyforge.site.v2","device_addresses":{"lite6_main":"192.168.1.31"}})";
  const auto launch = [&](std::optional<std::string> site) {
    std::filesystem::remove(arguments_path);
    auto adapter = component_manager::RetainedExecutionHostUnitAdapter(
        kTestStartId, targets.path(), fixture.root / "retained-package",
        fixture.manager_run_root.path(),
        fixture.execution_state_root.path(), fixture.execution_run_root.path(),
        fixture.bundle_source, fixture.artifact_source,
        std::string(contract::kOperatingSystem),
        std::string(contract::kLaunchEnvironmentContract), {}, {},
        std::move(site));
    EXPECT_EQ(adapter.EnsureRunning(*fixture.query).state(),
              component_manager::ProcessPresenceState::kPresent);
    for (int attempt = 0; attempt < 200 &&
         !std::filesystem::exists(arguments_path); ++attempt) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::vector<std::string> arguments;
    if (std::filesystem::exists(arguments_path)) {
      std::istringstream captured(ReadFile(arguments_path));
      for (std::string argument; std::getline(captured, argument, '\0');)
        arguments.push_back(std::move(argument));
    }
    EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
              component_manager::ProcessPresenceState::kAbsent);
    return arguments;
  };
  // Unconfigured: the child never sees the option (the fixture captures argv
  // only when the option is present).
  EXPECT_TRUE(launch(std::nullopt).empty());
  const auto configured = launch(site_document);
  ASSERT_EQ(std::count(configured.begin(), configured.end(),
                       "--manyforge-managed-site-settings"), 1);
  // OD-139: the host never receives the Program runner's cycle journal.
  EXPECT_TRUE(std::none_of(configured.begin(), configured.end(), [](const auto &argument) {
    return argument.find("program-cycle") != std::string::npos;
  }));
  const auto option = std::find(configured.begin(), configured.end(),
                                "--manyforge-managed-site-settings");
  ASSERT_NE(std::next(option), configured.end());
  const std::filesystem::path snapshot_path = *std::next(option);
  EXPECT_EQ(snapshot_path.parent_path(), fixture.manager_run_root.path());
  EXPECT_TRUE(snapshot_path.filename().string().starts_with("site-settings-"));
  EXPECT_EQ(snapshot_path.extension(), ".json");
  struct stat snapshot_status {};
  ASSERT_EQ(::lstat(snapshot_path.c_str(), &snapshot_status), 0);
  EXPECT_TRUE(S_ISREG(snapshot_status.st_mode));
  EXPECT_EQ(snapshot_status.st_mode & 0777, 0600);
  // Child retirement and adapter destruction keep the run evidence.
  EXPECT_EQ(ReadFile(snapshot_path), site_document);
}

// The packet child's inspection mode: "hold" answers only once released,
// "exit" exits without a reply. Declare it after the adapter.
class InspectionHold final {
public:
  InspectionHold(std::filesystem::path run_root, const std::string_view mode)
      : root_(std::move(run_root)) {
    std::ofstream(root_ / "inspection.mode") << mode;
  }
  InspectionHold(const InspectionHold &) = delete;
  InspectionHold &operator=(const InspectionHold &) = delete;
  ~InspectionHold() { Release(); }

  void Release() const noexcept {
    try {
      std::ofstream(root_ / "inspection.release") << "release\n";
    } catch (...) {
    }
  }
  [[nodiscard]] bool WaitEntered() const {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!std::filesystem::exists(root_ / "inspection.entered") &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return std::filesystem::exists(root_ / "inspection.entered");
  }

private:
  std::filesystem::path root_;
};

[[nodiscard]] component_manager::ManagerComponentProcessInventoryRequestDTO
InspectionRequest(const NativeFixture &fixture, const core::RuntimeEpoch &epoch) {
  return {{fixture.bundle->retained_bundle_id(),
           fixture.bundle->value().cell_graph.graph_revision,
           MakeEmptyScene(fixture.bundle->value().cell_graph.cell_id, 1U)
               .shared()
               ->revision},
          core::ActiveBundlePublicationGeneration(1U),
          core::ContentDigest(std::string(kIntentDigest)),
          fixture.query->plan.runtime_unit_id,
          epoch,
          true};
}

// OD-270: the process inspection has no time limit and holds no adapter lock;
// meanwhile other calls answer busy at once. The stop's abandon signal ends it
// typed at once; its reply, when it comes, is still settled; a later one is
// refused before it is sent.
TEST(RetainedExecutionHostUnitAdapter,
     HeldInspectionAnswersOtherCallsBusyAndTheStopAbandonsIt) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  const auto run_root = fixture.execution_run_root.path();
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const auto request = InspectionRequest(fixture, epoch);
  ASSERT_TRUE(request.valid());
  InspectionHold hold(run_root, "hold");
  auto inspected = std::async(std::launch::async, [&] {
    return adapter.InspectInstalledComponentProcesses(request);
  });
  ASSERT_TRUE(hold.WaitEntered());

  const auto busy_started = std::chrono::steady_clock::now();
  const auto resolved =
      adapter.ResolveProjection(NativeProjectionBinding(fixture, epoch));
  ASSERT_FALSE(resolved.ok());
  EXPECT_EQ(resolved.error().code,
            component_manager::kRuntimeUnitLaunchInProgressCode);
  EXPECT_EQ(resolved.error().details.at("phase"), "inspecting");
  try {
    static_cast<void>(adapter.ObserveAdmission(
        core::ConfigurationFenceAdmissionQueryDTO{{}}));
    ADD_FAILURE() << "fence observation waited for or passed the inspection";
  } catch (const core::TypedError &error) {
    EXPECT_EQ(error.code(), component_manager::kRuntimeUnitLaunchInProgressCode);
  }
  EXPECT_TRUE(adapter.HasHostChild());
  EXPECT_LT(std::chrono::steady_clock::now() - busy_started,
            std::chrono::seconds(1));

  adapter.AbandonExchanges();
  ASSERT_EQ(inspected.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  const auto abandoned = inspected.get();
  ASSERT_FALSE(abandoned.ok());
  EXPECT_EQ(abandoned.error().code, component_manager::kApplicationStoppingCode);
  EXPECT_EQ(abandoned.error().details.at("exchange"),
            "component_process_inspection");
  EXPECT_FALSE(abandoned.error().details.contains("effect"));
  // Its reply is still outstanding (information only).
  const auto outstanding = adapter.HostReplyOutstanding();
  ASSERT_TRUE(outstanding.has_value());
  EXPECT_EQ(outstanding->exchange, "component_process_inspection");
  EXPECT_FALSE(adapter.HostExit().has_value());

  // While the abandoned exchange is unsettled, other host calls send nothing
  // and answer the typed stopping refusal.
  const auto reset = adapter.ValidateProgramResetSceneAuthorization(
      LateTestResetAuthorization());
  ASSERT_FALSE(reset.ok());
  EXPECT_EQ(reset.error().code, component_manager::kApplicationStoppingCode);
  EXPECT_EQ(reset.error().details.at("effect"), "none");
  const auto apply = adapter.ApplyOnHost(
      fixture.query->plan.runtime_unit_id,
      HostApplyAdapterRequest(fixture, HostApplyMarker::kSucceed));
  ASSERT_FALSE(apply.ok());
  EXPECT_EQ(apply.error().code, component_manager::kApplicationStoppingCode);
  EXPECT_EQ(apply.error().details.at("effect"), "none");
  try {
    static_cast<void>(adapter.ObserveAdmission(
        core::ConfigurationFenceAdmissionQueryDTO{{}}));
    ADD_FAILURE() << "fence observation was sent behind the abandoned exchange";
  } catch (const core::TypedError &error) {
    EXPECT_EQ(error.code(), component_manager::kApplicationStoppingCode);
    EXPECT_EQ(error.details().at("exchange"), "component_process_inspection");
  }

  // The reply that comes later is recognised and settled: the status read
  // then sends again and answers.
  hold.Release();
  bool settled = false;
  const auto settle_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!settled && std::chrono::steady_clock::now() < settle_deadline) {
    settled = adapter.ObserveProgress(*fixture.query, epoch, 0U).has_value();
    if (!settled) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(settled);
  const auto refused = adapter.InspectInstalledComponentProcesses(request);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, component_manager::kApplicationStoppingCode);
  EXPECT_EQ(refused.error().details.at("effect"), "none");
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

// OD-270 with OD-187 (1): after the abandon signal the stop is not held
// behind the exchange, and a second stop request kills a host that does not
// stop.
TEST(RetainedExecutionHostUnitAdapter,
     AfterTheStopAbandonsAHeldExchangeASecondStopKillsTheHost) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  const auto run_root = fixture.execution_run_root.path();
  auto adapter = MakeAdapter(fixture, targets.path());
  g_shutdown_force_requested.store(false, std::memory_order_release);
  adapter.SetShutdownForce(&ShutdownForceRequested);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  InspectionHold hold(run_root, "hold");
  auto inspected = std::async(std::launch::async, [&] {
    return adapter.InspectInstalledComponentProcesses(
        InspectionRequest(fixture, epoch));
  });
  ASSERT_TRUE(hold.WaitEntered());
  adapter.AbandonExchanges();
  auto stopped = std::async(std::launch::async, [&] {
    return adapter.EnsureStopped(*fixture.query);
  });
  ASSERT_EQ(inspected.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(inspected.get().error().code,
            component_manager::kApplicationStoppingCode);
  // The held host does not read its shutdown; the stop waits for its exit.
  EXPECT_EQ(stopped.wait_for(std::chrono::milliseconds(300)),
            std::future_status::timeout);
  g_shutdown_force_requested.store(true, std::memory_order_release);
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  EXPECT_EQ(stopped.get().state(),
            component_manager::ProcessPresenceState::kAbsent);
  g_shutdown_force_requested.store(false, std::memory_order_release);
  EXPECT_TRUE(adapter.HostForceKilledPid().has_value());
  const auto exit = adapter.HostExit();
  ASSERT_TRUE(exit.has_value());
  EXPECT_TRUE(exit->signaled);
  EXPECT_EQ(exit->value, SIGKILL);
}

// OD-308 (a): the stop drains the host with no adapter lock held. While a
// host that does not read its shutdown drains, other calls answer the typed
// busy `stopping` at once, a second stop included, and the host still counts
// as present; a second stop request then ends it.
TEST(RetainedExecutionHostUnitAdapter,
     StopDrainsWithoutTheLockAndConcurrentCallsAreBusyStopping) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  const auto run_root = fixture.execution_run_root.path();
  auto adapter = MakeAdapter(fixture, targets.path());
  g_shutdown_force_requested.store(false, std::memory_order_release);
  adapter.SetShutdownForce(&ShutdownForceRequested);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  // The held inspection keeps the host from reading its shutdown, so the
  // drain lasts until the second stop request.
  InspectionHold hold(run_root, "hold");
  auto inspected = std::async(std::launch::async, [&] {
    return adapter.InspectInstalledComponentProcesses(
        InspectionRequest(fixture, epoch));
  });
  ASSERT_TRUE(hold.WaitEntered());
  adapter.AbandonExchanges();
  auto stopped = std::async(std::launch::async, [&] {
    return adapter.EnsureStopped(*fixture.query);
  });
  ASSERT_EQ(inspected.wait_for(std::chrono::seconds(2)),
            std::future_status::ready);
  EXPECT_EQ(inspected.get().error().code,
            component_manager::kApplicationStoppingCode);

  // The busy phase of a call made during the drain; empty when the call was
  // not answered busy. The deadline only ends a failed wait.
  const auto busy_phase = [](const auto &call) -> std::string {
    try {
      call();
    } catch (const core::TypedError &error) {
      if (error.code() == component_manager::kRuntimeUnitLaunchInProgressCode)
        return error.details().at("phase");
    }
    return {};
  };
  auto observed = std::async(std::launch::async, [&] {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string phase;
    while (phase.empty() && std::chrono::steady_clock::now() < deadline) {
      phase = busy_phase(
          [&] { static_cast<void>(adapter.ObserveProcess(*fixture.query)); });
      if (phase.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return phase;
  });
  // A call that waited for the lock instead would never return during the
  // drain: release the host then, so the test ends.
  const auto answered = [&](auto &future) {
    const bool ready = future.wait_for(std::chrono::seconds(2)) ==
                       std::future_status::ready;
    if (!ready) g_shutdown_force_requested.store(true, std::memory_order_release);
    return ready;
  };
  ASSERT_TRUE(answered(observed));
  EXPECT_EQ(observed.get(), "stopping");
  auto second_stop = std::async(std::launch::async, [&] {
    return busy_phase(
        [&] { static_cast<void>(adapter.EnsureStopped(*fixture.query)); });
  });
  ASSERT_TRUE(answered(second_stop));
  EXPECT_EQ(second_stop.get(), "stopping");
  auto present = std::async(std::launch::async,
                            [&] { return adapter.HasHostChild(); });
  ASSERT_TRUE(answered(present));
  EXPECT_TRUE(present.get());
  EXPECT_EQ(stopped.wait_for(std::chrono::seconds(0)),
            std::future_status::timeout);

  g_shutdown_force_requested.store(true, std::memory_order_release);
  ASSERT_EQ(stopped.wait_for(std::chrono::seconds(10)),
            std::future_status::ready);
  EXPECT_EQ(stopped.get().state(),
            component_manager::ProcessPresenceState::kAbsent);
  g_shutdown_force_requested.store(false, std::memory_order_release);
  EXPECT_FALSE(adapter.HasHostChild());
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

// OD-270: the host's exit ends an exchange without a time limit at once,
// typed, and HostExit is set when it is answered.
TEST(RetainedExecutionHostUnitAdapter,
     HostExitDuringAnExchangeWithoutLimitIsTypedAndReportedAtOnce) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  InspectionHold exits(fixture.execution_run_root.path(), "exit");
  const auto result =
      adapter.InspectInstalledComponentProcesses(InspectionRequest(fixture, epoch));
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, component_manager::kExecutionHostExitedCode);
  EXPECT_EQ(result.error().details.at("exchange"),
            "component_process_inspection");
  const auto exit = adapter.HostExit();
  ASSERT_TRUE(exit.has_value());
  EXPECT_FALSE(exit->signaled);
  EXPECT_EQ(exit->value, 0);
  EXPECT_FALSE(adapter.HostReplyOutstanding().has_value());
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}
} // namespace manyforge::roboplan_backend

int main(int argc, char **argv) {
  for (int index = 1; index < argc; ++index) {
    if (std::string_view(argv[index]) ==
        "--manyforge-managed-runtime-channel-fd") {
      try {
        return manyforge::roboplan_backend::RunSelectedAdapterPacketChild(argc,
                                                                          argv);
      } catch (const std::exception &error) {
        std::cerr << "selected adapter packet fixture: " << error.what()
                  << '\n';
        return 2;
      }
    }
  }
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
