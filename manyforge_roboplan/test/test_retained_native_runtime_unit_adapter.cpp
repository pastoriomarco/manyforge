#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
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
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
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

class NativeProjectionWorkerControl final {
public:
  explicit NativeProjectionWorkerControl(std::string hold = {},
                                         std::string fail = {})
      : prior_root_(ReadEnvironment(
            "MANYFORGE_NATIVE_PROJECTION_TEST_CONTROL_DIR")),
        prior_hold_(ReadEnvironment("MANYFORGE_NATIVE_PROJECTION_TEST_HOLD")),
        prior_fail_(ReadEnvironment("MANYFORGE_NATIVE_PROJECTION_TEST_FAIL")) {
    SetEnvironment("MANYFORGE_NATIVE_PROJECTION_TEST_CONTROL_DIR",
                   root_.path().string());
    SetEnvironment("MANYFORGE_NATIVE_PROJECTION_TEST_HOLD", std::move(hold));
    SetEnvironment("MANYFORGE_NATIVE_PROJECTION_TEST_FAIL", std::move(fail));
  }

  ~NativeProjectionWorkerControl() {
    Release();
    RestoreEnvironment("MANYFORGE_NATIVE_PROJECTION_TEST_CONTROL_DIR",
                       prior_root_);
    RestoreEnvironment("MANYFORGE_NATIVE_PROJECTION_TEST_HOLD", prior_hold_);
    RestoreEnvironment("MANYFORGE_NATIVE_PROJECTION_TEST_FAIL", prior_fail_);
  }

  NativeProjectionWorkerControl(const NativeProjectionWorkerControl &) =
      delete;
  NativeProjectionWorkerControl &
  operator=(const NativeProjectionWorkerControl &) = delete;

  [[nodiscard]] bool WaitEntered(
      const std::string_view operation,
      const std::chrono::seconds timeout = std::chrono::seconds(30)) const {
    const auto marker =
        root_.path() / (std::string(operation) + ".entered");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!std::filesystem::exists(marker) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return std::filesystem::exists(marker);
  }

  void Release(const std::string_view operation) const {
    std::ofstream release(
        root_.path() / (std::string(operation) + ".release"),
        std::ios::binary | std::ios::trunc);
    release << "release\n";
  }

  void Release() const {
    Release("rebuild");
    Release("stage");
  }

  [[nodiscard]] std::size_t CountTracePrefix(
      const std::string_view prefix) const {
    std::ifstream trace(root_.path() / "trace.log", std::ios::binary);
    std::size_t count = 0U;
    std::string line;
    while (std::getline(trace, line)) {
      if (line.starts_with(prefix)) {
        ++count;
      }
    }
    return count;
  }

  [[nodiscard]] bool WaitForTraceCount(
      const std::string_view prefix, const std::size_t expected,
      const std::chrono::seconds timeout = std::chrono::seconds(30)) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (CountTracePrefix(prefix) < expected &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    return CountTracePrefix(prefix) >= expected;
  }

private:
  [[nodiscard]] static std::optional<std::string>
  ReadEnvironment(const char *name) {
    const char *const value = std::getenv(name);
    return value == nullptr ? std::nullopt
                            : std::optional<std::string>(value);
  }

  static void SetEnvironment(const char *name, std::string value) {
    if (value.empty()) {
      if (::unsetenv(name) != 0) {
        throw std::runtime_error("could not clear native worker test input");
      }
      return;
    }
    if (::setenv(name, value.c_str(), 1) != 0) {
      throw std::runtime_error("could not set native worker test input");
    }
  }

  static void RestoreEnvironment(
      const char *name, const std::optional<std::string> &value) noexcept {
    if (value.has_value()) {
      static_cast<void>(::setenv(name, value->c_str(), 1));
    } else {
      static_cast<void>(::unsetenv(name));
    }
  }

  ScopedTemporaryRoot root_;
  std::optional<std::string> prior_root_;
  std::optional<std::string> prior_hold_;
  std::optional<std::string> prior_fail_;
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
          "native adapter fixture has no exact native runtime product");
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
      fixture.manager_state_root.path(), fixture.manager_run_root.path(),
      fixture.execution_state_root.path(), fixture.execution_run_root.path(),
      fixture.bundle_source, fixture.artifact_source,
      std::string(contract::kOperatingSystem),
      std::string(contract::kLaunchEnvironmentContract));
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

[[nodiscard]] std::optional<core::AuthoringAdmissionDTO>
WaitForAuthoringAdmission(
    component_manager::RetainedNativeRuntimeUnitAdapter &adapter,
    const std::chrono::steady_clock::duration timeout,
    const std::optional<bool> expected_allowed = std::nullopt) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      auto admission = adapter.GetAuthoringAdmission();
      if (!expected_allowed.has_value() ||
          admission.authoring_allowed == *expected_allowed) {
        return admission;
      }
    } catch (const component_manager::RetainedNativeRuntimeUnitAdapterError &) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::nullopt;
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
  EXPECT_EQ(admission->implementation_revision(),
            "sha256:"
            "003f915c030a96347ca9f79ac4fe6fd8ead5baec3790e9b44377e1eaad6dc400");
  EXPECT_EQ(fixture.query->plan.process_adapter_contract_revision,
            "sha256:"
            "c5ee4ce0842e61b38da1c34e3979244896ae40461b5d6bb0fe3e1e81aac10186");
  EXPECT_EQ(admission->runtime_contract_offer_digest(),
            fixture.query->plan.runtime_contract_offer_digest.value());
  const core::RuntimeEpoch packet_epoch("native-runtime-test-epoch");
  const auto ready_packet = admission->expected_ready_packet(packet_epoch);
  const auto status_packet =
      admission->expected_status_packet(packet_epoch, 1U);
  EXPECT_TRUE(ready_packet.starts_with("manyforge.native-execution-ready.v2 "));
  EXPECT_TRUE(
      status_packet.starts_with("manyforge.native-execution-status.v2 "));
  EXPECT_NE(ready_packet.find(admission->runtime_contract_offer_digest()),
            std::string::npos);
  EXPECT_NE(status_packet.find(admission->runtime_contract_offer_digest()),
            std::string::npos);
  EXPECT_EQ(ready_packet.find(admission->implementation_revision()),
            std::string::npos);
  EXPECT_EQ(status_packet.find(admission->implementation_revision()),
            std::string::npos);
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
     RejectsNestedStateAndRunCapabilityTreesBeforeLaunch) {
  NativeFixture fixture;
  const auto nested_run_root = fixture.manager_state_root.path() / "run";
  ASSERT_TRUE(std::filesystem::create_directory(nested_run_root));

  EXPECT_THROW(
      static_cast<void>(component_manager::RetainedNativeRuntimeUnitAdapter(
          MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT,
          fixture.root / "retained-package", fixture.manager_state_root.path(),
          nested_run_root, fixture.execution_state_root.path(),
          fixture.execution_run_root.path(), fixture.bundle_source,
          fixture.artifact_source, std::string(contract::kOperatingSystem),
          std::string(contract::kLaunchEnvironmentContract))),
      std::runtime_error);

  EXPECT_THROW(
      static_cast<void>(component_manager::RetainedNativeRuntimeUnitAdapter(
          MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT,
          fixture.root / "retained-package", fixture.manager_state_root.path(),
          fixture.manager_run_root.path(), fixture.execution_state_root.path(),
          fixture.execution_run_root.path(), fixture.bundle_source,
          fixture.artifact_source, std::string(contract::kOperatingSystem),
          std::string(contract::kLaunchEnvironmentContract),
          fixture.execution_run_root.path() / "logical-scene.sqlite3")),
      std::runtime_error);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     ReusesOneVerifiedAdmissionForOneExactRuntimeQuery) {
  NativeFixture fixture;
  CountingBundleSource bundles(fixture.bundle_source);
  CountingArtifactSource artifacts(fixture.artifact_source);
  auto adapter = component_manager::RetainedNativeRuntimeUnitAdapter(
      MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT, fixture.root / "retained-package",
      fixture.manager_state_root.path(), fixture.manager_run_root.path(),
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

TEST(RetainedNativeRuntimeUnitAdapter,
     DirectChildOwnsStableEmptyAdmissionBeforeApplyAndRestartMintsNewEpoch) {
  NativeFixture fixture;
  ASSERT_TRUE(fixture.product.has_value());
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);

  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  ASSERT_FALSE(started.process_incarnation().empty());
  const core::RuntimeEpoch first_epoch(started.process_incarnation());
  const auto empty_admission = adapter.GetAuthoringAdmission();
  EXPECT_FALSE(empty_admission.authoring_allowed);
  EXPECT_TRUE(empty_admission.admission_generation.nonzero());

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
  EXPECT_EQ(first->offered_runtime_contract_offer_digest(),
            fixture.query->plan.runtime_contract_offer_digest.value());
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
     ReservedStopAndExactReadsRemainAvailableBeforePhysicalReconstruction) {
  NativeFixture fixture;
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);
  const auto process = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(process.state(), component_manager::ProcessPresenceState::kPresent);
  const auto ordinary =
      fixture.execution_run_root.path() / "execution-command.sock";
  const auto priority =
      fixture.execution_run_root.path() / "execution-control.sock";
  const auto read_stop_fault = [&] {
    return ExecutionPacket(
        ordinary,
        R"({"api_version":"manyforge.execution_command.v1","command":"get_stop_fault"})");
  };
  const auto identity = ExecutionPacket(
      ordinary,
      R"({"api_version":"manyforge.execution_command.v1","command":"identity"})");
  const auto *error = identity.find_object_member("error");
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(TextMember(*error, "code"), "execution_owner_not_ready");
  const auto owner =
      TextMember(*error->find_object_member("details"), "owner_epoch");
  const auto absent_offer = read_stop_fault();
  const auto absent_encoded = content_identity::canonical_json(absent_offer);
  ASSERT_TRUE(absent_offer.is_object()) << absent_encoded;
  EXPECT_EQ(absent_offer.as_object().size(), 4U) << absent_encoded;
  EXPECT_EQ(TextMember(absent_offer, "api_version"),
            "manyforge.execution_command_response.v1");
  EXPECT_EQ(TextMember(absent_offer, "command"), "get_stop_fault");
  EXPECT_EQ(absent_offer.find_object_member("error"), nullptr)
      << absent_encoded;
  const auto *absent_ok = absent_offer.find_object_member("ok");
  ASSERT_NE(absent_ok, nullptr) << absent_encoded;
  ASSERT_TRUE(absent_ok->is_bool()) << absent_encoded;
  EXPECT_TRUE(absent_ok->as_bool()) << absent_encoded;
  const auto *absent_fault = absent_offer.find_object_member("stop_fault");
  ASSERT_NE(absent_fault, nullptr) << absent_encoded;
  EXPECT_TRUE(absent_fault->is_null()) << absent_encoded;
  const std::string attribution =
      R"("attribution":{"transport":"internal","assurance":"unverified","principal_id":null})";
  const auto packet = [&](std::string_view command, std::string_view request) {
    return "{\"api_version\":\"manyforge.execution_command.v1\",\"command\":"
           "\"" +
           std::string(command) + "\"," + attribution +
           ",\"request\":" + std::string(request) + "}";
  };
  const auto context = [&](std::string_view request) {
    return "{\"request_id\":\"" + std::string(request) +
           "\",\"request_owner_epoch\":\"" + owner +
           "\",\"source_label\":\"qualification\",\"correlation_id\":null}";
  };
  const auto recovery = ExecutionPacket(
      ordinary,
      "{\"api_version\":\"manyforge.execution_command.v1\",\"command\":"
      "\"recover_stop_fault\",\"request\":{\"request_context\":" +
          context("recover.before-reconstruction") +
          ",\"expected_latch_id\":\"stop-fault.before-reconstruction\","
          "\"expected_latch_generation\":1,"
          "\"expected_recovery_assessment_revision\":1,"
          "\"expected_stop_certificate_id\":\"certificate."
          "before-reconstruction\",\"expected_stop_certificate_revision\":1,"
          "\"expected_cell_id\":\"" +
          fixture.bundle->value().cell_graph.cell_id.value() +
          "\",\"expected_retained_bundle_id\":\"" +
          fixture.bundle->retained_bundle_id().value() +
          "\",\"expected_graph_revision\":\"" +
          fixture.bundle->value().cell_graph.graph_revision.value() +
          "\",\"configuration_expectations\":[{\"control_domain_id\":"
          "\"domain.before-reconstruction\","
          "\"active_control_configuration_id\":"
          "\"configuration.before-reconstruction\","
          "\"active_control_configuration_generation\":1}]}}");
  const auto recovery_encoded = content_identity::canonical_json(recovery);
  ASSERT_TRUE(recovery.is_object()) << recovery_encoded;
  EXPECT_EQ(recovery.as_object().size(), 3U) << recovery_encoded;
  EXPECT_EQ(TextMember(recovery, "api_version"),
            "manyforge.execution_command_response.v1");
  EXPECT_EQ(recovery.find_object_member("command"), nullptr)
      << recovery_encoded;
  EXPECT_EQ(recovery.find_object_member("result"), nullptr)
      << recovery_encoded;
  const auto *recovery_ok = recovery.find_object_member("ok");
  ASSERT_NE(recovery_ok, nullptr) << recovery_encoded;
  ASSERT_TRUE(recovery_ok->is_bool()) << recovery_encoded;
  EXPECT_FALSE(recovery_ok->as_bool()) << recovery_encoded;
  const auto *recovery_error = recovery.find_object_member("error");
  ASSERT_NE(recovery_error, nullptr) << recovery_encoded;
  ASSERT_TRUE(recovery_error->is_object()) << recovery_encoded;
  EXPECT_EQ(TextMember(*recovery_error, "code"), "execution_owner_not_ready")
      << recovery_encoded;
  const auto *recovery_details =
      recovery_error->find_object_member("details");
  ASSERT_NE(recovery_details, nullptr) << recovery_encoded;
  ASSERT_TRUE(recovery_details->is_object()) << recovery_encoded;
  EXPECT_EQ(recovery_details->as_object().size(), 1U) << recovery_encoded;
  EXPECT_EQ(TextMember(*recovery_details, "owner_epoch"), owner)
      << recovery_encoded;
  for (const auto &command : {std::string("stop_program_run"),
                              std::string("stop"), std::string("full_stop")}) {
    SCOPED_TRACE(command);
    const auto target = command == "full_stop"
                            ? "\"expected_stop_generation\":0"
                        : command == "stop_program_run"
                            ? "\"program_run_id\":\"run.before-reconstruction\""
                            : "\"operations\":[{\"operation_id\":\"operation."
                              "before-reconstruction\",\"expected_operation_"
                              "revision\":1,\"expected_stop_generation\":0}]";
    const auto reply = ExecutionPacket(
        priority, packet(command, "{\"request_context\":" + context(command) +
                                      "," + target + "}"));
    const auto encoded = content_identity::canonical_json(reply);
    EXPECT_EQ(encoded.find("execution_owner_not_ready"), std::string::npos)
        << encoded;
    EXPECT_EQ(encoded.find("execution_command_invalid"), std::string::npos)
        << encoded;
    ASSERT_NE(reply.find_object_member("accepted"), nullptr) << encoded;
    const auto *accepted = reply.find_object_member("accepted");
    const auto read = ExecutionPacket(
        ordinary, "{\"api_version\":\"manyforge.execution_command.v1\","
                  "\"command\":\"read_stop\",\"stop_id\":\"" +
                      TextMember(*accepted, "stop_id") + "\"," + attribution +
                      "}");
    EXPECT_NE(read.find_object_member("stop_status"), nullptr)
        << content_identity::canonical_json(read);
  }
  const auto malformed = ExecutionPacket(priority, "{", false);
  ASSERT_NE(malformed.find_object_member("error"), nullptr);
  EXPECT_EQ(TextMember(*malformed.find_object_member("error"), "code"),
            "execution_command_invalid");
  const auto recognized = ExecutionPacket(
      priority,
      "{\"api_version\":\"manyforge.execution_command.v1\",\"command\":"
      "\"malformed_stop\",\"request_id\":\"malformed.1\",\"recognized_route\":"
      "\"/api/execution/"
      "stops\",\"reason\":\"recognized_stop_payload_invalid\"," +
          attribution + "}");
  ASSERT_NE(recognized.find_object_member("accepted"), nullptr)
      << content_identity::canonical_json(recognized);
  const auto late = ExecutionPacket(
      ordinary,
      packet("resume",
             "{\"request_context\":" + context("late.resume") +
                 ",\"operation_id\":\"operation.before-reconstruction\","
                 "\"expected_operation_revision\":1,\"reason\":null}"));
  ASSERT_NE(late.find_object_member("error"), nullptr);
  EXPECT_EQ(TextMember(*late.find_object_member("error"), "code"),
            "execution_owner_not_ready");
  EXPECT_TRUE(
      adapter
          .ObserveProgress(*fixture.query,
                           core::RuntimeEpoch(process.process_incarnation()))
          .has_value());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
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

  auto adapter = component_manager::RetainedNativeRuntimeUnitAdapter(
      MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT, fixture.root / "retained-package",
      fixture.manager_state_root.path(), fixture.manager_run_root.path(),
      fixture.execution_state_root.path(), fixture.execution_run_root.path(),
      fixture.bundle_source, fixture.artifact_source,
      std::string(contract::kOperatingSystem),
      std::string(contract::kLaunchEnvironmentContract), {}, {}, {},
      authored.path());

  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
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
  auto adapter = component_manager::RetainedNativeRuntimeUnitAdapter(
      MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT, descriptor_root,
      fixture.manager_state_root.path(), fixture.manager_run_root.path(),
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
  const auto progress = adapter.ObserveProgress(*fixture.query, epoch);
  ASSERT_TRUE(progress.has_value());
  EXPECT_EQ(progress->product_identity(),
            fixture.query->plan.product_identity.value());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  static_cast<void>(::close(package_descriptor));
}

TEST(RetainedNativeRuntimeUnitAdapter,
     CompatibleImplementationReplacementNeedsNoBinaryProof) {
  NativeFixture package;
  ASSERT_TRUE(package.product.has_value());
  ScopedTemporaryRoot target_root_a;
  ScopedTemporaryRoot target_root_b;
  const auto candidate_worker_source =
      std::filesystem::path(MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT) /
      std::string(contract::kCandidateWorkerInstalledTargetId);
  const auto target_a = InstallNativeTestTargets(
      std::filesystem::path(MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT) /
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
    ASSERT_TRUE(adapter_a
                    .ObserveProgress(
                        *package.query,
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
        adapter_b.ObserveProgress(*package.query, first_epoch).has_value());
    EXPECT_EQ(adapter_b.EnsureStopped(*package.query).state(),
              component_manager::ProcessPresenceState::kAbsent);
    EXPECT_EQ(adapter_b.ObserveProcess(*package.query).state(),
              component_manager::ProcessPresenceState::kAbsent);

    const auto second_b = adapter_b.EnsureRunning(*package.query);
    ASSERT_EQ(second_b.state(),
              component_manager::ProcessPresenceState::kPresent);
    EXPECT_NE(second_b.process_incarnation(), first_epoch.value());
    ASSERT_TRUE(
        adapter_b
            .ObserveProgress(*package.query,
                             core::RuntimeEpoch(second_b.process_incarnation()))
            .has_value());
    EXPECT_EQ(adapter_b.EnsureStopped(*package.query).state(),
              component_manager::ProcessPresenceState::kAbsent);
    EXPECT_EQ(adapter_b.ObserveProcess(*package.query).state(),
              component_manager::ProcessPresenceState::kAbsent);
  }
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
  const auto competing_release =
      adapter.Release(core::ConfigurationFenceReleaseDTO{
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
  const auto observed_authoring_admission =
      WaitForAuthoringAdmission(adapter, std::chrono::seconds(30));
  ASSERT_TRUE(observed_authoring_admission.has_value());
  EXPECT_FALSE(observed_authoring_admission->authoring_allowed);
  EXPECT_GT(observed_authoring_admission->admission_generation.value(), 0U);
  EXPECT_EQ(adapter.GetAuthoringAdmission(), *observed_authoring_admission);
  const core::CellAdmissionReadinessSnapshotDTO readiness{
      true,
      core::ProgramCycleStatusDTO{
          core::RequestOwnerEpoch("adapter-program-runner"), 1U,
          core::ProgramCycleStateDTO::kIdleAfterReset, std::nullopt,
          std::nullopt, std::nullopt},
      true,
      2U,
      2U,
      true,
      true,
      0U,
      0U,
      0U};
  const auto updated = adapter.UpdateCellAdmissionReadiness(readiness);
  ASSERT_TRUE(updated.ok()) << updated.error().message;
  EXPECT_FALSE(updated.value().authoring_allowed);

  const auto &liveness_policy = contract::manager_liveness_policy();
  const core::RuntimeUnitId program_runner_id(
      "program-runner.native-adapter-test");
  const core::RuntimeEpoch program_runner_epoch(
      "program-runner-epoch.native-adapter-test");
  const core::RuntimePolicyId program_runner_policy_id(
      "manyforge.program-runner.manager-presence.v1");
  const core::RuntimePolicyRevision program_runner_policy_revision("1");
  std::vector<core::RequiredManagerLivenessReceiverDTO> receivers{
      {fixture.query->plan.runtime_unit_id, first_epoch,
       liveness_policy.runtime_policy_id,
       liveness_policy.runtime_policy_revision},
      {program_runner_id, program_runner_epoch, program_runner_policy_id,
       program_runner_policy_revision}};
  const auto required_set_digest =
      core::ComputeRequiredRuntimeSetDigest(receivers);
  const core::RuntimeUnitId manager_runtime_unit_id(
      "component-manager.native-adapter-test");
  const core::RuntimeEpoch manager_runtime_epoch(
      "manager-runtime.native-adapter-test");
  const core::RequestOwnerEpoch manager_owner_epoch(
      "manager-owner.native-adapter-test");
  const core::ManagerLivenessRequiredSetDTO required_set{
      fixture.bundle->value().cell_graph.cell_id,
      fixture.bundle->retained_bundle_id(),
      required_set_digest,
      manager_runtime_unit_id,
      manager_runtime_epoch,
      manager_owner_epoch,
      receivers};
  const core::ManagerHeartbeatDTO heartbeat{
      required_set.cell_id,
      manager_runtime_unit_id,
      manager_runtime_epoch,
      manager_owner_epoch,
      1U,
      liveness_policy.runtime_policy_id,
      liveness_policy.runtime_policy_revision};
  const core::ManagerLivenessReconciliationAck reconciliation{
      required_set.cell_id,
      manager_runtime_unit_id,
      manager_runtime_epoch,
      manager_owner_epoch,
      required_set.active_bundle_id,
      required_set_digest,
      core::ContentDigest(std::string(kIntentDigest))};
  const auto liveness = adapter.RenewManagerLiveness(
      {fixture.query->plan.runtime_unit_id, publication.publication_generation,
       publication.request.candidate_graph_revision,
       publication.durable_public_pointer_content_digest, required_set,
       liveness_policy, heartbeat, reconciliation});
  EXPECT_EQ(liveness.active_bundle_id, required_set.active_bundle_id);
  EXPECT_EQ(liveness.required_runtime_set_digest, required_set_digest);
  EXPECT_EQ(liveness.status.state, core::ManagerLivenessStateDTO::kCurrent);
  EXPECT_FALSE(liveness.status.command_fenced);

  EXPECT_FALSE(adapter.GetAuthoringAdmission().authoring_allowed);
  core::ManagerLivenessStatusDTO program_runner_status{
      required_set.cell_id,
      program_runner_id,
      program_runner_epoch,
      manager_runtime_unit_id,
      manager_runtime_epoch,
      manager_owner_epoch,
      program_runner_policy_id,
      program_runner_policy_revision,
      1U,
      1U,
      0,
      core::ManagerLivenessStateDTO::kCurrent,
      false,
      std::nullopt};
  const core::ManagerLivenessObservationDTO program_runner_observation{
      program_runner_status, required_set.active_bundle_id, required_set_digest,
      core::ComputeManagerLivenessStatusDigest(program_runner_status)};
  const auto published = adapter.Publish(program_runner_observation);
  ASSERT_TRUE(published.ok()) << published.error().message;

  const auto authoring_admission =
      WaitForAuthoringAdmission(adapter, std::chrono::seconds(30), true);
  ASSERT_TRUE(authoring_admission.has_value());
  ASSERT_TRUE(authoring_admission->authoring_allowed);
  EXPECT_GT(authoring_admission->admission_generation,
            updated.value().admission_generation);
  const core::CellAdmissionClaimIntentDTO claim_intent{
      core::CellAdmissionClaimKindDTO::kAuthoringMutation,
      core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
      core::RequestOwnerEpoch("adapter-authoring-owner"),
      "adapter-authoring-transaction",
      core::RequestId("adapter-authoring-request"),
      core::ContentDigest(std::string(kIntentDigest)),
      authoring_admission->admission_generation,
      {},
      std::nullopt,
      std::nullopt};
  const auto claimed = adapter.AcquireCellAdmissionClaim(claim_intent);
  ASSERT_TRUE(claimed.ok()) << claimed.error().message;
  const auto replayed = adapter.AcquireCellAdmissionClaim(claim_intent);
  ASSERT_TRUE(replayed.ok()) << replayed.error().message;
  EXPECT_EQ(replayed.value(), claimed.value());
  auto stale_intent = claim_intent;
  stale_intent.request_id = core::RequestId("adapter-authoring-stale");
  const auto stale = adapter.AcquireCellAdmissionClaim(stale_intent);
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, "stale_generation");
  EXPECT_FALSE(stale.error().details.empty());
  const auto released_claim = adapter.ReleaseCellAdmissionClaim(
      {claim_intent.owner_role,
       claim_intent.owner_epoch,
       claim_intent.owner_transaction_id,
       claim_intent.request_id,
       claim_intent.request_payload_digest,
       claimed.value().acquired_admission_generation,
       claimed.value().claim_generation,
       {core::CellAdmissionSettlementKindDTO::kOwnerCommit,
        core::ContentDigest(std::string(kIntentDigest))}});
  ASSERT_FALSE(released_claim.ok());
  EXPECT_EQ(released_claim.error().code, "authoring_not_ready");
  EXPECT_EQ(released_claim.error().details.at("reason"),
            "settlement_not_proven");

  const auto physical_scope = PhysicalConfigurationScope(fixture);
  const core::RuntimeEpoch manager_epoch(
      "manager-runtime-physical-admission-test");
  const core::ConfigurationFenceAdmissionQueryDTO admission_query{
      physical_scope, manager_epoch};
  // Admission observation is an inspection service, so it remains available
  // independently of permission to perform a controller or route effect. The
  // returned snapshot is exact and does not itself acquire an Apply fence.
  const auto physical_admission = adapter.ObserveAdmission(admission_query);
  EXPECT_TRUE(physical_admission.matches(admission_query));
  EXPECT_EQ(physical_admission.active_projection_revision,
            publication.request.candidate_projection_revision);
  EXPECT_EQ(physical_admission.active_public_pointer_digest,
            publication.durable_public_pointer_content_digest);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).process_incarnation(),
            first_epoch.value());

  auto renewed_heartbeat = heartbeat;
  renewed_heartbeat.heartbeat_sequence = 2U;
  const auto renewed_liveness = adapter.RenewManagerLiveness(
      {fixture.query->plan.runtime_unit_id, publication.publication_generation,
       publication.request.candidate_graph_revision,
       publication.durable_public_pointer_content_digest, required_set,
       liveness_policy, renewed_heartbeat, reconciliation});
  ASSERT_EQ(renewed_liveness.status.state,
            core::ManagerLivenessStateDTO::kCurrent);

  ASSERT_FALSE(fixture.bundle->value().cell_graph.control_bindings.empty());
  const auto &control_binding =
      fixture.bundle->value().cell_graph.control_bindings.front();
  const core::ProjectionLeaseId frame_lease_id("native-adapter-frame-lease");
  const auto retained_binding = resolved.value()->RetainProjectionBinding(
      initial_revision, frame_lease_id);
  ASSERT_TRUE(retained_binding.ok()) << retained_binding.error().message;
  EXPECT_EQ(retained_binding.value(), (core::ProjectionLeaseBindingDTO{
                                          initial_revision, frame_lease_id}));
  const core::SceneFrameResolutionQueryDTO frame_query{
      core::TargetCarrierSceneFrameRefDTO{
          control_binding.controllable_component_id,
          control_binding.command_target_id, core::FrameRoleId("tcp")},
      retained_binding.value()};
  std::optional<core::SceneFrameResolutionReceiptDTO> frame;
  const auto frame_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < frame_deadline) {
    const auto observed_frame =
        resolved.value()->ResolveSceneFrame(frame_query);
    if (observed_frame.ok()) {
      frame = observed_frame.value();
      break;
    }
    ASSERT_EQ(observed_frame.error().details.at("reason"),
              "native_scene_frame_lease_unavailable")
        << observed_frame.error().message;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(frame.has_value())
      << "the managed child did not install the exact active frame resolver";
  ASSERT_TRUE(frame->valid());
  EXPECT_EQ(frame->projection_binding, retained_binding.value());
  const auto *component_frame =
      std::get_if<core::ResolvedComponentModelFrameDTO>(&frame->resolved_frame);
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
  EXPECT_EQ(staged.state, core::NativeProjectionStageResultState::kAccepted);
  const core::NativeProjectionStatusQueryDTO stage_query{
      stage_request.apply_id, stage_request.apply_generation,
      stage_request.projection_stage_generation, retained_row};
  auto stage_status = resolved.value()->ProjectionStageStatus(stage_query);
  const auto stage_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (stage_status.state == core::NativeProjectionStatusState::kProgressing &&
         std::chrono::steady_clock::now() < stage_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stage_status = resolved.value()->ProjectionStageStatus(stage_query);
  }
  EXPECT_EQ(stage_status.state, core::NativeProjectionStatusState::kStaged);
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
    EXPECT_TRUE(required_result.result ==
                    core::ProjectionCommitResultStateDTO::kAccepted ||
                required_result.result ==
                    core::ProjectionCommitResultStateDTO::kCommitted);
    auto required_health = required.value()->ProjectionHealth();
    const auto required_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!required_health.healthy_for_target(candidate_revision) &&
           std::chrono::steady_clock::now() < required_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      required_health = required.value()->ProjectionHealth();
    }
    EXPECT_TRUE(required_health.healthy_for_target(candidate_revision));
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
  EXPECT_TRUE(rebuilt.result == core::ProjectionCommitResultStateDTO::kAccepted ||
              rebuilt.result ==
                  core::ProjectionCommitResultStateDTO::kCommitted);
  EXPECT_FALSE(rebuilt.transaction_sequence.has_value());
  auto rebuilt_health = replacement_client.value()->ProjectionHealth();
  const auto rebuild_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!rebuilt_health.healthy_for_target(candidate_revision) &&
         std::chrono::steady_clock::now() < rebuild_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    rebuilt_health = replacement_client.value()->ProjectionHealth();
  }
  EXPECT_TRUE(rebuilt_health.healthy_for_target(candidate_revision));
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

/// One candidate logical successor for a native Stage operation.
///
/// Only the scene revision and transaction sequence move, so every candidate
/// stays a deterministic content-addressed successor of the same published
/// predecessor the managed child already projects.
[[nodiscard]] core::LogicalSceneSnapshotRoot MakeNativeStageCandidateScene(
    const core::LogicalSceneSnapshotDTO &predecessor,
    const std::uint64_t revision_id, const std::uint64_t transaction_sequence) {
  auto scene = std::make_shared<core::LogicalSceneSnapshotDTO>(predecessor);
  scene->revision.revision_id = revision_id;
  scene->transaction_sequence = transaction_sequence;
  scene->revision.content_hash =
      core::logical_scene_identity::ComputeLogicalSceneContentHash(*scene);
  if (!scene->locally_valid()) {
    throw std::logic_error("native stage fixture candidate scene is invalid");
  }
  return core::LogicalSceneSnapshotRoot(std::move(scene));
}

[[nodiscard]] core::ProjectionNativeRuntimeBindingDTO
FirstOtherNativeProjectionBinding(
    const NativeFixture &fixture, const core::RuntimeEpoch &runtime_epoch,
    const core::ProjectionNativeRuntimeBindingDTO &excluded) {
  for (const auto &candidate :
       NativeProjectionBindings(fixture, runtime_epoch)) {
    if (candidate != excluded) {
      return candidate;
    }
  }
  throw std::logic_error(
      "native stage fixture has no independent native provider");
}

/// The exact managed native child used by the Stage-lifecycle tests below.
///
/// Every required projection is bootstrapped to the published predecessor and
/// Manager liveness is renewed once, so the child's asynchronous current
/// composition builder is admitted. The scene-frame probe at the end is the
/// fixture's own barrier for that build: installing the direct frame resolver
/// is its last step. While the builder can still run, a staged candidate would
/// be a stale active row for InstallActiveFrameResolverSet; once the probe
/// resolves, no later composition install can race the Stage work under test.
struct NativeStageRuntime final {
  NativeFixture fixture;
  core::ActiveBundlePublicationReceiptDTO publication;
  component_manager::RetainedNativeRuntimeUnitAdapter adapter;
  core::RuntimeEpoch runtime_epoch;
  core::LogicalSceneSnapshotRoot initial_scene;
  core::ProjectionRevisionDTO initial_revision;
  core::RetainedProductDTO planning_collision_model;
  core::ProjectionNativeRuntimeBindingDTO binding;
  core::ProjectionNativeRuntimeBindingDTO independent_binding;
  NativeProjectionWorkerControl *worker_control{};

  explicit NativeStageRuntime(
      NativeProjectionWorkerControl *control = nullptr,
      const bool bootstrap_primary = true)
      : publication(PublishActiveFixture(fixture)),
        adapter(MakeAdapter(
            fixture, control != nullptr
                         ? MANYFORGE_NATIVE_PROJECTION_TEST_TARGET_ROOT
                         : MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT)),
        runtime_epoch(StartManagedChild(adapter, *fixture.query)),
        initial_scene(
            MakeEmptyScene(fixture.bundle->value().cell_graph.cell_id, 1U)),
        initial_revision{fixture.bundle->retained_bundle_id(),
                         fixture.bundle->value().cell_graph.graph_revision,
                         initial_scene.shared()->revision},
        planning_collision_model(
            fixture.bundle->value().compiled_products.planning_collision_model),
        binding(NativeProjectionBinding(fixture, runtime_epoch)),
        independent_binding(FirstOtherNativeProjectionBinding(
            fixture, runtime_epoch, binding)),
        worker_control(control) {
    if (!(initial_revision ==
          publication.request.candidate_projection_revision)) {
      throw std::logic_error(
          "native stage fixture predecessor is not the published projection");
    }
    for (const auto &required :
         NativeProjectionBindings(fixture, runtime_epoch)) {
      if (required == binding) {
        continue;
      }
      BootstrapProjection(required);
    }
    if (bootstrap_primary) {
      BootstrapProjection(binding);
      RenewManagerChildLiveness();
      RequireInstalledActiveFrameResolver();
    }
  }

  ~NativeStageRuntime() {
    if (worker_control != nullptr) {
      worker_control->Release();
    }
    try {
      static_cast<void>(adapter.EnsureStopped(*fixture.query));
    } catch (...) {
      // Cleanup must not turn an already-failing assertion into terminate().
    }
  }

  NativeStageRuntime(const NativeStageRuntime &) = delete;
  NativeStageRuntime &operator=(const NativeStageRuntime &) = delete;

  [[nodiscard]] std::shared_ptr<core::IPlannerSceneProjection> Client(
      const core::ProjectionNativeRuntimeBindingDTO &target) {
    auto resolved = adapter.ResolveProjection(target);
    if (!resolved.ok()) {
      throw std::runtime_error(
          "native stage fixture cannot resolve a projection client: " +
          resolved.error().message);
    }
    return resolved.value();
  }

private:
  [[nodiscard]] static core::RuntimeEpoch StartManagedChild(
      component_manager::RetainedNativeRuntimeUnitAdapter &value,
      const component_manager::RuntimeUnitConvergenceQueryDTO &query) {
    const auto started = value.EnsureRunning(query);
    if (started.state() != component_manager::ProcessPresenceState::kPresent ||
        started.process_incarnation().empty()) {
      throw std::runtime_error(
          "native stage fixture could not start the managed child");
    }
    return core::RuntimeEpoch(started.process_incarnation());
  }

  void BootstrapProjection(
      const core::ProjectionNativeRuntimeBindingDTO &target) {
    const auto client = Client(target);
    const core::NativeProjectionBootstrapRebuildDTO request{
        core::BootstrapId("native-adapter-bootstrap"),
        core::BootstrapGeneration(1U), target, initial_revision,
        planning_collision_model, initial_scene};
    const auto rebuilt = client->RebuildBootstrapProjection(request);
    if (!rebuilt.presence_valid() || !rebuilt.resulting_health.valid() ||
        !rebuilt.resulting_health.healthy_for_target(initial_revision)) {
      throw std::runtime_error(
          "native stage fixture could not bootstrap a required projection");
    }
  }

  void RenewManagerChildLiveness() {
    const auto &policy = contract::manager_liveness_policy();
    const std::vector<core::RequiredManagerLivenessReceiverDTO> receivers{
        {fixture.query->plan.runtime_unit_id, runtime_epoch,
         policy.runtime_policy_id, policy.runtime_policy_revision}};
    const core::RuntimeUnitId manager_runtime_unit_id(
        "component-manager.native-adapter-test");
    const core::RuntimeEpoch manager_runtime_epoch(
        "manager-runtime.native-adapter-test");
    const core::RequestOwnerEpoch manager_owner_epoch(
        "manager-owner.native-adapter-test");
    const auto required_set_digest =
        core::ComputeRequiredRuntimeSetDigest(receivers);
    const core::ManagerLivenessRequiredSetDTO required_set{
        fixture.bundle->value().cell_graph.cell_id,
        fixture.bundle->retained_bundle_id(),
        required_set_digest,
        manager_runtime_unit_id,
        manager_runtime_epoch,
        manager_owner_epoch,
        receivers};
    const core::ManagerHeartbeatDTO heartbeat{required_set.cell_id,
                                              manager_runtime_unit_id,
                                              manager_runtime_epoch,
                                              manager_owner_epoch,
                                              1U,
                                              policy.runtime_policy_id,
                                              policy.runtime_policy_revision};
    const core::ManagerLivenessReconciliationAck reconciliation{
        required_set.cell_id,
        manager_runtime_unit_id,
        manager_runtime_epoch,
        manager_owner_epoch,
        required_set.active_bundle_id,
        required_set_digest,
        core::ContentDigest(std::string(kIntentDigest))};
    const auto liveness = adapter.RenewManagerLiveness(
        {fixture.query->plan.runtime_unit_id,
         publication.publication_generation,
         publication.request.candidate_graph_revision,
         publication.durable_public_pointer_content_digest,
         required_set,
         policy,
         heartbeat,
         reconciliation});
    if (liveness.status.state != core::ManagerLivenessStateDTO::kCurrent) {
      throw std::runtime_error("native stage fixture liveness is not current");
    }
  }

  void RequireInstalledActiveFrameResolver() {
    if (fixture.bundle->value().cell_graph.control_bindings.empty()) {
      throw std::logic_error("native stage fixture has no control binding");
    }
    const auto &control_binding =
        fixture.bundle->value().cell_graph.control_bindings.front();
    const auto client = Client(binding);
    const core::ProjectionLeaseId probe_lease(
        "native-adapter-stage-probe-lease");
    const auto retained =
        client->RetainProjectionBinding(initial_revision, probe_lease);
    if (!retained.ok()) {
      throw std::runtime_error(
          "native stage fixture cannot retain its probe lease: " +
          retained.error().message);
    }
    const core::SceneFrameResolutionQueryDTO probe{
        core::TargetCarrierSceneFrameRefDTO{
            control_binding.controllable_component_id,
            control_binding.command_target_id, core::FrameRoleId("tcp")},
        retained.value()};
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto resolved = client->ResolveSceneFrame(probe);
      if (resolved.ok()) {
        client->ReleaseProjection(probe_lease);
        return;
      }
      std::this_thread::yield();
    }
    client->ReleaseProjection(probe_lease);
    throw std::runtime_error(
        "the managed child never installed its active frame resolver");
  }
};

[[nodiscard]] core::NativeProjectionStageDTO MakeNativeStageRequest(
    const NativeStageRuntime &runtime,
    const core::ProjectionNativeRuntimeBindingDTO &binding,
    std::string_view apply_id, const std::uint64_t apply_generation,
    const std::uint64_t stage_generation,
    const std::uint64_t candidate_scene_revision_id,
    const std::uint64_t candidate_transaction_sequence) {
  auto candidate_scene = MakeNativeStageCandidateScene(
      *runtime.initial_scene.shared(), candidate_scene_revision_id,
      candidate_transaction_sequence);
  const core::ProjectionRevisionDTO candidate_revision{
      runtime.initial_revision.retained_bundle_id,
      runtime.initial_revision.graph_revision,
      candidate_scene.shared()->revision};
  const core::ProjectionTransitionRowDTO row{
      binding, core::ProjectionTransitionDisposition::kRetained,
      runtime.initial_revision};
  const core::NativeProjectionStageDTO request{
      core::ApplyId(std::string(apply_id)),
      core::ApplyGeneration(apply_generation),
      core::ProjectionStageGeneration(stage_generation),
      row,
      runtime.initial_revision,
      candidate_revision,
      runtime.planning_collision_model,
      std::move(candidate_scene)};
  if (!request.valid()) {
    throw std::logic_error("native stage fixture request is invalid");
  }
  return request;
}

[[nodiscard]] core::NativeProjectionStatusQueryDTO MakeNativeStageStatusQuery(
    const core::NativeProjectionStageDTO &stage) {
  return {stage.apply_id, stage.apply_generation,
          stage.projection_stage_generation, stage.transition_row};
}

[[nodiscard]] core::NativeProjectionDiscardDTO MakeNativeStageDiscard(
    const core::NativeProjectionStageDTO &stage) {
  return {stage.apply_id,
          stage.apply_generation,
          stage.projection_stage_generation,
          stage.transition_row,
          stage.candidate_projection_revision};
}

[[nodiscard]] core::NativeProjectionRebuildDTO MakeNativeRebuildRequest(
    const NativeStageRuntime &runtime) {
  return {runtime.binding, runtime.initial_revision,
          runtime.planning_collision_model, runtime.initial_scene.shared()};
}

[[nodiscard]] bool SameProjectionHealth(
    const core::ProjectionHealthDTO &left,
    const core::ProjectionHealthDTO &right) {
  return left.native_runtime_binding == right.native_runtime_binding &&
         left.source_sequence == right.source_sequence &&
         left.state == right.state &&
         left.active_projection_revision == right.active_projection_revision &&
         left.staged_candidate == right.staged_candidate &&
         left.native_projection_generation ==
             right.native_projection_generation &&
         left.projection_content_digest == right.projection_content_digest &&
         left.error == right.error;
}

[[nodiscard]] bool SameProjectionCommitResult(
    const core::ProjectionCommitResultDTO &left,
    const core::ProjectionCommitResultDTO &right) {
  return left.transaction_sequence == right.transaction_sequence &&
         left.proposed_projection_revision ==
             right.proposed_projection_revision &&
         left.result == right.result &&
         left.acknowledged_projection_revision ==
             right.acknowledged_projection_revision &&
         left.native_projection_generation ==
             right.native_projection_generation &&
         left.rollback_proven == right.rollback_proven &&
         SameProjectionHealth(left.resulting_health, right.resulting_health) &&
         left.error == right.error;
}

/// Observes one accepted Stage until it reports its one terminal result.
///
/// The registry's own status answer is the synchronization point. The deadline
/// exists only to turn a deadlock into a failure, never to let an observation
/// pass that the registry has not confirmed.
[[nodiscard]] core::NativeProjectionStatusDTO WaitForNativeStageSettlement(
    const std::shared_ptr<core::IPlannerSceneProjection> &client,
    const core::NativeProjectionStatusQueryDTO &query,
    const std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  auto status = client->ProjectionStageStatus(query);
  while (status.state == core::NativeProjectionStatusState::kProgressing &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
    status = client->ProjectionStageStatus(query);
  }
  return status;
}

TEST(RetainedNativeRuntimeUnitAdapter,
     HeldNativeStageIsSingleJobResponsiveAndDiscardGated) {
  using namespace std::chrono_literals;

  NativeProjectionWorkerControl control("stage");
  NativeStageRuntime runtime(&control);
  const auto client = runtime.Client(runtime.binding);
  const auto independent = runtime.Client(runtime.independent_binding);
  const auto stage = MakeNativeStageRequest(
      runtime, runtime.binding, "native-adapter-held-stage", 2U, 1U, 2U, 1U);
  const auto query = MakeNativeStageStatusQuery(stage);
  const auto execution_prefix =
      "execute stage " + runtime.binding.provider_binding_id.value();
  const auto progress_prefix =
      "progress stage " + runtime.binding.provider_binding_id.value();

  const auto accepted = client->StageProjection(stage);
  ASSERT_TRUE(accepted.presence_valid());
  ASSERT_EQ(accepted.state, core::NativeProjectionStageResultState::kAccepted);
  ASSERT_TRUE(control.WaitEntered("stage"))
      << "the real Stage worker did not reach its construction barrier";
  ASSERT_TRUE(control.WaitForTraceCount(execution_prefix, 1U));
  ASSERT_TRUE(control.WaitForTraceCount(progress_prefix, 2U));
  const auto held_progress_count = control.CountTracePrefix(progress_prefix);
  EXPECT_EQ(held_progress_count, 2U);

  auto health = std::async(std::launch::async,
                           [&] { return client->ProjectionHealth(); });
  auto status = std::async(std::launch::async,
                           [&] { return client->ProjectionStageStatus(query); });
  const auto independent_stage = MakeNativeStageRequest(
      runtime, runtime.independent_binding, "native-adapter-independent-stage",
      2U, 1U, 2U, 1U);
  auto independent_submission = std::async(
      std::launch::async,
      [&] { return independent->StageProjection(independent_stage); });
  auto duplicate_submission = std::async(
      std::launch::async, [&] { return client->StageProjection(stage); });
  const auto conflict = MakeNativeStageRequest(
      runtime, runtime.binding, "native-adapter-held-conflict", 3U, 2U, 3U,
      2U);
  auto conflict_submission = std::async(
      std::launch::async, [&] { return client->StageProjection(conflict); });
  auto stale_process_epoch = stage;
  stale_process_epoch.transition_row.native_runtime_binding.runtime_epoch =
      core::RuntimeEpoch("foreign-native-runtime-epoch");
  auto stale_process_epoch_submission = std::async(
      std::launch::async,
      [&] { return client->StageProjection(stale_process_epoch); });
  auto discard_submission = std::async(std::launch::async, [&] {
    return client->DiscardStagedProjection(MakeNativeStageDiscard(stage));
  });
  auto repeated_observations = std::async(std::launch::async, [&] {
    std::vector<core::NativeProjectionStatusDTO> statuses;
    std::vector<core::ProjectionHealthDTO> health_rows;
    for (int observation = 0; observation < 8; ++observation) {
      statuses.push_back(client->ProjectionStageStatus(query));
      health_rows.push_back(client->ProjectionHealth());
    }
    return std::pair{std::move(statuses), std::move(health_rows)};
  });
  const auto request_deadline = std::chrono::steady_clock::now() + 10s;
  const bool health_ready =
      health.wait_until(request_deadline) == std::future_status::ready;
  const bool status_ready =
      status.wait_until(request_deadline) == std::future_status::ready;
  const bool independent_ready =
      independent_submission.wait_until(request_deadline) ==
      std::future_status::ready;
  const bool duplicate_ready =
      duplicate_submission.wait_until(request_deadline) ==
      std::future_status::ready;
  const bool conflict_ready =
      conflict_submission.wait_until(request_deadline) ==
      std::future_status::ready;
  const bool stale_process_epoch_ready =
      stale_process_epoch_submission.wait_until(request_deadline) ==
      std::future_status::ready;
  const bool discard_ready =
      discard_submission.wait_until(request_deadline) ==
      std::future_status::ready;
  const bool repeated_ready =
      repeated_observations.wait_until(request_deadline) ==
      std::future_status::ready;
  const bool requests_ready = health_ready && status_ready &&
                              independent_ready && duplicate_ready &&
                              conflict_ready && stale_process_epoch_ready &&
                              discard_ready && repeated_ready;
  if (!requests_ready) {
    control.Release();
  }
  ASSERT_TRUE(requests_ready)
      << "held Stage blocked a pending registry path or independent provider";
  const auto observed_health = health.get();
  EXPECT_EQ(observed_health.native_runtime_binding, runtime.binding);
  EXPECT_FALSE(
      observed_health.healthy_for_target(stage.candidate_projection_revision));
  const auto observed_status = status.get();
  ASSERT_TRUE(observed_status.presence_valid());
  EXPECT_EQ(observed_status.state,
            core::NativeProjectionStatusState::kProgressing);
  const auto independent_accepted = independent_submission.get();
  ASSERT_TRUE(independent_accepted.presence_valid());
  EXPECT_EQ(independent_accepted.state,
            core::NativeProjectionStageResultState::kAccepted);

  const auto duplicate = duplicate_submission.get();
  ASSERT_TRUE(duplicate.presence_valid());
  EXPECT_EQ(duplicate.state, core::NativeProjectionStageResultState::kAccepted);
  EXPECT_EQ(control.CountTracePrefix(execution_prefix), 1U);

  EXPECT_THROW(static_cast<void>(conflict_submission.get()),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_THROW(static_cast<void>(stale_process_epoch_submission.get()),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_EQ(control.CountTracePrefix(execution_prefix), 1U);
  const auto still_pending = client->ProjectionStageStatus(query);
  ASSERT_TRUE(still_pending.presence_valid());
  EXPECT_EQ(still_pending.state,
            core::NativeProjectionStatusState::kProgressing);

  const auto pending_discard = discard_submission.get();
  ASSERT_TRUE(pending_discard.presence_valid());
  EXPECT_EQ(pending_discard.state,
            core::NativeProjectionDiscardResultState::
                kFailedProjectionUncertain);
  ASSERT_TRUE(pending_discard.error.has_value());
  EXPECT_EQ(pending_discard.error->details.at("reason"),
            "native_projection_discard_progressing");

  const auto [repeated_statuses, repeated_health] =
      repeated_observations.get();
  for (const auto &repeated_status : repeated_statuses) {
    EXPECT_EQ(repeated_status.state,
              core::NativeProjectionStatusState::kProgressing);
  }
  for (const auto &repeated_health_row : repeated_health) {
    EXPECT_TRUE(SameProjectionHealth(repeated_health_row, observed_health));
  }
  EXPECT_EQ(control.CountTracePrefix(progress_prefix), held_progress_count)
      << "polling fabricated a construction milestone";

  control.Release();
  const auto settled = WaitForNativeStageSettlement(client, query);
  ASSERT_EQ(settled.state, core::NativeProjectionStatusState::kStaged);
  ASSERT_TRUE(control.WaitForTraceCount(progress_prefix, 3U));
  EXPECT_EQ(control.CountTracePrefix(execution_prefix), 1U);
  const auto terminal_replay = client->StageProjection(stage);
  ASSERT_TRUE(terminal_replay.presence_valid());
  EXPECT_EQ(terminal_replay.state,
            core::NativeProjectionStageResultState::kStaged);
  EXPECT_EQ(control.CountTracePrefix(execution_prefix), 1U);

  const auto discarded =
      client->DiscardStagedProjection(MakeNativeStageDiscard(stage));
  ASSERT_TRUE(discarded.presence_valid());
  EXPECT_EQ(discarded.state,
            core::NativeProjectionDiscardResultState::kDiscarded);
  const auto later = MakeNativeStageRequest(
      runtime, runtime.binding, "native-adapter-after-discard", 4U, 3U, 4U,
      3U);
  const auto later_accepted = client->StageProjection(later);
  ASSERT_TRUE(later_accepted.presence_valid());
  EXPECT_EQ(later_accepted.state,
            core::NativeProjectionStageResultState::kAccepted);
  EXPECT_EQ(WaitForNativeStageSettlement(
                client, MakeNativeStageStatusQuery(later))
                .state,
            core::NativeProjectionStatusState::kStaged);
  EXPECT_EQ(control.CountTracePrefix(execution_prefix), 2U)
      << "confirmed discard did not admit exactly one different Stage";
}

TEST(RetainedNativeRuntimeUnitAdapter,
     HeldNativeRebuildKeepsAllRequestsObservationalAndDefersOneStage) {
  using namespace std::chrono_literals;

  NativeProjectionWorkerControl control("rebuild,stage");
  NativeStageRuntime runtime(&control, false);
  const auto client = runtime.Client(runtime.binding);
  const auto independent = runtime.Client(runtime.independent_binding);
  const auto rebuild = MakeNativeRebuildRequest(runtime);
  const auto bootstrap_execution_prefix =
      "execute bootstrap_rebuild " +
      runtime.independent_binding.provider_binding_id.value();
  const auto rebuild_execution_prefix =
      "execute rebuild " + runtime.binding.provider_binding_id.value();
  const auto rebuild_progress_prefix =
      "progress rebuild " + runtime.binding.provider_binding_id.value();
  const auto stage_execution_prefix =
      "execute stage " + runtime.binding.provider_binding_id.value();
  const auto deferred_stage = MakeNativeStageRequest(
      runtime, runtime.binding, "native-adapter-deferred-stage", 2U, 1U, 2U,
      1U);

  ASSERT_EQ(control.CountTracePrefix(bootstrap_execution_prefix), 1U)
      << "the non-primary fixture projection did not finish bootstrap";
  const auto accepted = client->RebuildProjection(rebuild);
  ASSERT_TRUE(accepted.presence_valid());
  ASSERT_EQ(accepted.result, core::ProjectionCommitResultStateDTO::kAccepted);
  ASSERT_TRUE(control.WaitEntered("rebuild"))
      << "the real Rebuild worker did not reach its construction barrier";
  ASSERT_TRUE(control.WaitForTraceCount(rebuild_execution_prefix, 1U));
  ASSERT_TRUE(control.WaitForTraceCount(rebuild_progress_prefix, 2U));
  const auto held_progress_count =
      control.CountTracePrefix(rebuild_progress_prefix);

  const auto query = MakeNativeStageStatusQuery(deferred_stage);
  auto health = std::async(std::launch::async,
                           [&] { return client->ProjectionHealth(); });
  auto status = std::async(std::launch::async,
                           [&] { return client->ProjectionStageStatus(query); });
  auto discard = std::async(std::launch::async, [&] {
    return client->DiscardStagedProjection(
        MakeNativeStageDiscard(deferred_stage));
  });
  const auto independent_stage = MakeNativeStageRequest(
      runtime, runtime.independent_binding, "native-adapter-rebuild-independent",
      2U, 1U, 2U, 1U);
  auto independent_submission = std::async(
      std::launch::async,
      [&] { return independent->StageProjection(independent_stage); });
  const auto initial_request_deadline =
      std::chrono::steady_clock::now() + 10s;
  const bool health_ready =
      health.wait_until(initial_request_deadline) == std::future_status::ready;
  const bool status_ready =
      status.wait_until(initial_request_deadline) == std::future_status::ready;
  const bool discard_ready =
      discard.wait_until(initial_request_deadline) ==
      std::future_status::ready;
  const bool independent_ready =
      independent_submission.wait_until(initial_request_deadline) ==
      std::future_status::ready;
  const bool initial_requests_ready = health_ready && status_ready &&
                                      discard_ready && independent_ready;
  if (!initial_requests_ready) {
    control.Release();
  }
  ASSERT_TRUE(initial_requests_ready)
      << "held Rebuild entered a busy projection on the serial request strand";
  const auto rebuilding_health = health.get();
  EXPECT_EQ(rebuilding_health.state, core::ProjectionHealthState::kRebuilding);
  const auto rebuilding_status = status.get();
  ASSERT_TRUE(rebuilding_status.presence_valid());
  EXPECT_EQ(rebuilding_status.state,
            core::NativeProjectionStatusState::kFailedProjectionUncertain);
  const auto rebuilding_discard = discard.get();
  ASSERT_TRUE(rebuilding_discard.presence_valid());
  EXPECT_EQ(rebuilding_discard.state,
            core::NativeProjectionDiscardResultState::
                kFailedProjectionUncertain);
  const auto independent_accepted = independent_submission.get();
  ASSERT_TRUE(independent_accepted.presence_valid());
  EXPECT_EQ(independent_accepted.state,
            core::NativeProjectionStageResultState::kAccepted);

  auto retained_submission = std::async(
      std::launch::async,
      [&] { return client->StageProjection(deferred_stage); });
  const bool retained_ready =
      retained_submission.wait_for(10s) == std::future_status::ready;
  if (!retained_ready) {
    control.Release();
  }
  ASSERT_TRUE(retained_ready)
      << "held Rebuild blocked retention of its one deferred Stage";
  const auto retained = retained_submission.get();
  ASSERT_TRUE(retained.presence_valid());
  EXPECT_EQ(retained.state, core::NativeProjectionStageResultState::kAccepted);

  // The exact retained Rebuild wins replay ordering even after its deferred
  // Stage has been registered. It observes the original unsettled job rather
  // than entering Stage conflict handling or starting another worker.
  const auto held_rebuild_replay = client->RebuildProjection(rebuild);
  ASSERT_TRUE(held_rebuild_replay.presence_valid());
  EXPECT_TRUE(SameProjectionCommitResult(held_rebuild_replay, accepted));
  EXPECT_EQ(control.CountTracePrefix(rebuild_execution_prefix), 1U);
  auto duplicate_submission = std::async(
      std::launch::async,
      [&] { return client->StageProjection(deferred_stage); });
  const auto conflict = MakeNativeStageRequest(
      runtime, runtime.binding, "native-adapter-deferred-conflict", 3U, 2U,
      3U, 2U);
  auto conflict_submission = std::async(
      std::launch::async, [&] { return client->StageProjection(conflict); });
  auto repeated_observations = std::async(std::launch::async, [&] {
    std::vector<core::NativeProjectionStatusDTO> statuses;
    std::vector<core::ProjectionHealthDTO> health_rows;
    for (int observation = 0; observation < 8; ++observation) {
      health_rows.push_back(client->ProjectionHealth());
      statuses.push_back(client->ProjectionStageStatus(query));
    }
    return std::pair{std::move(statuses), std::move(health_rows)};
  });
  const auto deferred_request_deadline =
      std::chrono::steady_clock::now() + 10s;
  const bool duplicate_ready =
      duplicate_submission.wait_until(deferred_request_deadline) ==
      std::future_status::ready;
  const bool conflict_ready =
      conflict_submission.wait_until(deferred_request_deadline) ==
      std::future_status::ready;
  const bool repeated_ready =
      repeated_observations.wait_until(deferred_request_deadline) ==
      std::future_status::ready;
  const bool deferred_requests_ready =
      duplicate_ready && conflict_ready && repeated_ready;
  if (!deferred_requests_ready) {
    control.Release();
  }
  ASSERT_TRUE(deferred_requests_ready)
      << "held Rebuild blocked deferred Stage replay or observation paths";
  const auto duplicate = duplicate_submission.get();
  ASSERT_TRUE(duplicate.presence_valid());
  EXPECT_EQ(duplicate.state, core::NativeProjectionStageResultState::kAccepted);
  EXPECT_THROW(static_cast<void>(conflict_submission.get()),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_EQ(control.CountTracePrefix(stage_execution_prefix), 0U);
  const auto [repeated_statuses, repeated_health] =
      repeated_observations.get();
  for (const auto &repeated_health_row : repeated_health) {
    EXPECT_TRUE(
        SameProjectionHealth(repeated_health_row, rebuilding_health));
  }
  for (const auto &repeated_status : repeated_statuses) {
    EXPECT_EQ(repeated_status.state,
              core::NativeProjectionStatusState::kProgressing);
  }
  EXPECT_EQ(control.CountTracePrefix(rebuild_progress_prefix),
            held_progress_count)
      << "Rebuild polling fabricated a construction milestone";

  control.Release("rebuild");
  ASSERT_TRUE(control.WaitForTraceCount(stage_execution_prefix, 1U))
      << "the deferred Stage did not reach its construction barrier";
  const auto rebuilt_while_stage_pending =
      client->RebuildProjection(rebuild);
  ASSERT_TRUE(rebuilt_while_stage_pending.presence_valid());
  ASSERT_EQ(rebuilt_while_stage_pending.result,
            core::ProjectionCommitResultStateDTO::kCommitted);
  EXPECT_EQ(control.CountTracePrefix(rebuild_execution_prefix), 1U);
  EXPECT_EQ(control.CountTracePrefix(stage_execution_prefix), 1U);

  control.Release("stage");
  EXPECT_EQ(WaitForNativeStageSettlement(client, query).state,
            core::NativeProjectionStatusState::kStaged);
  const auto rebuilt_after_stage = client->RebuildProjection(rebuild);
  ASSERT_TRUE(rebuilt_after_stage.presence_valid());
  EXPECT_TRUE(SameProjectionCommitResult(rebuilt_after_stage,
                                         rebuilt_while_stage_pending));
  EXPECT_EQ(control.CountTracePrefix(rebuild_execution_prefix), 1U);
  EXPECT_EQ(control.CountTracePrefix(stage_execution_prefix), 1U)
      << "deferred Stage was not retained and executed exactly once";
}

TEST(RetainedNativeRuntimeUnitAdapter,
     NativeStageWorkerFailureReplaysOneStableTypedResult) {
  NativeProjectionWorkerControl control({}, "stage");
  NativeStageRuntime runtime(&control);
  const auto client = runtime.Client(runtime.binding);
  const auto stage = MakeNativeStageRequest(
      runtime, runtime.binding, "native-adapter-worker-failure", 2U, 1U, 2U,
      1U);
  const auto execution_prefix =
      "execute stage " + runtime.binding.provider_binding_id.value();

  const auto accepted = client->StageProjection(stage);
  ASSERT_TRUE(accepted.presence_valid());
  ASSERT_EQ(accepted.state, core::NativeProjectionStageResultState::kAccepted);
  const auto failed = WaitForNativeStageSettlement(
      client, MakeNativeStageStatusQuery(stage));
  ASSERT_TRUE(failed.presence_valid());
  ASSERT_EQ(failed.state,
            core::NativeProjectionStatusState::kFailedProjectionUncertain);
  ASSERT_TRUE(failed.error.has_value());
  ASSERT_TRUE(control.WaitForTraceCount(execution_prefix, 1U));

  const auto first_replay = client->StageProjection(stage);
  const auto second_replay = client->StageProjection(stage);
  ASSERT_TRUE(first_replay.presence_valid());
  ASSERT_TRUE(second_replay.presence_valid());
  EXPECT_EQ(first_replay.state,
            core::NativeProjectionStageResultState::
                kFailedProjectionUncertain);
  EXPECT_EQ(second_replay.state, first_replay.state);
  EXPECT_EQ(second_replay.error, first_replay.error);
  EXPECT_TRUE(SameProjectionHealth(second_replay.resulting_health,
                                   first_replay.resulting_health));
  EXPECT_EQ(control.CountTracePrefix(execution_prefix), 1U);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     SupersededFirstLivenessRenewalIsRejectedWithoutKillingChild) {
  NativeFixture fixture;
  const auto predecessor = PublishActiveFixture(fixture);
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);

  component_manager::SqliteActiveBundlePublicationStore store(
      fixture.manager_state_root.path() /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      fixture.bundle->value().cell_graph.cell_id);
  auto successor_projection = predecessor.request.candidate_projection_revision;
  ++successor_projection.scene_revision.revision_id;
  const auto successor = store.PublishActiveBundle(
      {core::NamedApplyPublicationTransactionDTO{
           core::ApplyId("native-adapter-successor"), core::ApplyGeneration(1U),
           core::ProjectionStageGeneration(1U)},
       successor_projection, successor_projection.retained_bundle_id,
       successor_projection.graph_revision, predecessor.publication_generation,
       core::ContentDigest(std::string(kIntentDigest)),
       core::ContentDigest(std::string(kIntentDigest))});
  store.Close();
  ASSERT_GT(successor.publication_generation,
            predecessor.publication_generation);

  const auto &policy = contract::manager_liveness_policy();
  const core::RuntimeEpoch runtime_epoch(started.process_incarnation());
  const std::vector<core::RequiredManagerLivenessReceiverDTO> receivers{
      {fixture.query->plan.runtime_unit_id, runtime_epoch,
       policy.runtime_policy_id, policy.runtime_policy_revision}};
  const auto required_set_digest =
      core::ComputeRequiredRuntimeSetDigest(receivers);
  const core::RuntimeUnitId manager_id(
      "component-manager.native-adapter-race-test");
  const core::RuntimeEpoch manager_epoch(
      "manager-runtime.native-adapter-race-test");
  const core::RequestOwnerEpoch manager_owner(
      "manager-owner.native-adapter-race-test");
  const core::ManagerLivenessRequiredSetDTO required_set{
      fixture.bundle->value().cell_graph.cell_id,
      predecessor.request.candidate_retained_bundle_id,
      required_set_digest,
      manager_id,
      manager_epoch,
      manager_owner,
      receivers};
  const core::ManagerHeartbeatDTO heartbeat{required_set.cell_id,
                                            manager_id,
                                            manager_epoch,
                                            manager_owner,
                                            1U,
                                            policy.runtime_policy_id,
                                            policy.runtime_policy_revision};

  EXPECT_THROW(static_cast<void>(adapter.RenewManagerLiveness(
                   {fixture.query->plan.runtime_unit_id,
                    predecessor.publication_generation,
                    predecessor.request.candidate_graph_revision,
                    predecessor.durable_public_pointer_content_digest,
                    required_set, policy, heartbeat, std::nullopt})),
               component_manager::ManagerLivenessPublicationSuperseded);
  const auto after = adapter.ObserveProcess(*fixture.query);
  EXPECT_EQ(after.state(), component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(after.process_incarnation(), runtime_epoch.value());
  ASSERT_TRUE(
      adapter.ObserveProgress(*fixture.query, runtime_epoch).has_value());
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
