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
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
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

#include "../../manyforge_component_manager/src/manager_liveness_rpc_internal.hpp"
#include "../../manyforge_component_manager/src/native_projection_rpc_internal.hpp"
#include "../../manyforge_component_manager/src/selected_runtime_composition_rpc_internal.hpp"

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace contract = manyforge::deployment::ros2_control_execution_runtime_contract;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;
namespace scene = manyforge::scene;
using content_identity::CanonicalValue;

constexpr std::string_view kIntentDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

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

/// The host intentionally asks the Manager for the current Scene revision
/// before publishing identity/status. This small test authority supplies that
/// read-only Manager-owned projection for the adapter-only arm fixture; it
/// does not replace Scene admission or command authority.
class ManagerSceneReadServer final {
public:
  ManagerSceneReadServer(
      const std::filesystem::path &manager_run_root,
      const core::ProjectionRevisionDTO &projection,
      const core::LogicalSceneSnapshotDTO &scene_snapshot) {
    const auto socket_path = manager_run_root / "manager-command.sock";
    const auto spelling = socket_path.string();
    if (spelling.size() >= sizeof(sockaddr_un::sun_path))
      throw std::runtime_error("Manager scene test socket path exceeds bound");
    descriptor_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (descriptor_ < 0)
      throw std::runtime_error("Manager scene test socket creation failed");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::copy(spelling.begin(), spelling.end(), address.sun_path);
    if (::bind(descriptor_, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != 0 ||
        ::listen(descriptor_, 8) != 0) {
      ::close(descriptor_);
      descriptor_ = -1;
      throw std::runtime_error("Manager scene test socket setup failed");
    }
    response_ = content_identity::canonical_json(CanonicalValue(
        CanonicalValue::Object{
            {"api_version",
             CanonicalValue("manyforge.manager_command_response.v1")},
            {"logical_scene",
             content_identity::parse_canonical_json(
                 scene::EncodeLogicalSceneSnapshot(scene_snapshot))},
            {"ok", CanonicalValue(true)},
            {"projection_revision",
             CanonicalValue(CanonicalValue::Object{
                 {"graph_revision",
                  CanonicalValue(projection.graph_revision.value())},
                 {"retained_bundle_id",
                  CanonicalValue(projection.retained_bundle_id.value())},
                 {"scene_revision",
                  CanonicalValue(CanonicalValue::Object{
                      {"content_hash", CanonicalValue(
                                           projection.scene_revision.content_hash
                                               .value())},
                      {"revision_id", CanonicalValue(static_cast<double>(
                                               projection.scene_revision
                                                   .revision_id))},
                      {"scene_id", CanonicalValue(
                                       projection.scene_revision.scene_id.value())},
                  })},
             })},
            {"request_owner_epoch",
             CanonicalValue("native-adapter-scene-owner")},
        }));
    thread_ = std::thread([this] { Serve(); });
  }

  ~ManagerSceneReadServer() {
    stopping_.store(true, std::memory_order_release);
    if (thread_.joinable()) thread_.join();
    if (descriptor_ >= 0) ::close(descriptor_);
  }

  ManagerSceneReadServer(const ManagerSceneReadServer &) = delete;
  ManagerSceneReadServer &operator=(const ManagerSceneReadServer &) = delete;

private:
  void Serve() noexcept {
    while (!stopping_.load(std::memory_order_acquire)) {
      pollfd ready{descriptor_, POLLIN, 0};
      const auto polled = ::poll(&ready, 1, 50);
      if (polled <= 0 || (ready.revents & POLLIN) == 0) continue;
      const int client = ::accept4(descriptor_, nullptr, nullptr, SOCK_CLOEXEC);
      if (client < 0) continue;
      std::array<char, 4096> request{};
      const auto received = ::recv(client, request.data(), request.size(), 0);
      if (received > 0) {
        const auto parsed = content_identity::parse_canonical_json(
            std::string_view(request.data(),
                             static_cast<std::size_t>(received)));
        const auto *command = parsed.find_object_member("command");
        if (command != nullptr && command->is_string() &&
            command->as_string() == "scene_owner_read") {
          static_cast<void>(::send(client, response_.data(), response_.size(),
                                   MSG_NOSIGNAL));
        }
      }
      ::close(client);
    }
  }

  int descriptor_{-1};
  std::atomic_bool stopping_{};
  std::string response_;
  std::thread thread_;
};

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
        // This fixture publishes a bundle without an Apply, so it has no
        // predecessor route configuration to reconcile.
        domain_id, std::nullopt,
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

[[nodiscard]] bool BootstrapNativeProjectionSet(
    NativeFixture &fixture,
    component_manager::RetainedNativeRuntimeUnitAdapter &adapter,
    const core::NativeProjectionBootstrapStatusQueryDTO &bootstrap_query,
    const core::LogicalSceneSnapshotRoot &initial_scene,
    core::IPlannerSceneProjection &primary_projection) {
  for (const auto &required_binding : NativeProjectionBindings(
           fixture, bootstrap_query.native_runtime_binding.runtime_epoch)) {
    if (required_binding == bootstrap_query.native_runtime_binding) {
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
      bootstrap_query.native_runtime_binding,
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

// The Manager's periodic liveness lane for one exact required set.
//
// A native receiver lease stays current for `expiry_timeout_ns` after its last
// accepted heartbeat, and the Manager owns renewing it every
// `heartbeat_period_ns` for as long as anything relies on it. A test standing
// in for the Manager must keep that cadence: one heartbeat followed by a wait
// on asynchronous child work (composition, Kernel attachment, route recovery)
// lets the lease expire by contract once that work outlasts the expiry under
// load. Expiry fences admission for the whole Manager epoch, and the receiver
// then rejects every later renewal of that epoch.
class ManagerHeartbeatLane final {
public:
  // `accepted` is the request whose heartbeat the receiver already accepted;
  // the lane continues that Manager epoch with the next sequence.
  ManagerHeartbeatLane(
      component_manager::RetainedNativeRuntimeUnitAdapter &adapter,
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

  component_manager::RetainedNativeRuntimeUnitAdapter &adapter_;
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
  // The installed offer is generated from this build's implementation. Its
  // ROS process-adapter revision uses the ROS contract domain with the same
  // adapter description as the retired native fixture; admission must match
  // that exact selected ROS offer, not the native-domain digest.
  EXPECT_EQ(fixture.query->plan.process_adapter_contract_revision,
            "sha256:"
            "7656d6dc0a4c97c941e6c87e3a9fcf77d23f209748b8400159aa092cd838ac6b");
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
        component_manager::NativeExecutionRuntimeUnitAdmissionError);
  }
  EXPECT_THROW(admission->parse_status_packet(
                   core::RuntimeEpoch("foreign-epoch"), status_packet),
               component_manager::NativeExecutionRuntimeUnitAdmissionError);
  EXPECT_THROW(
      admission->parse_status_packet(packet_epoch, status_packet + " "),
      component_manager::NativeExecutionRuntimeUnitAdmissionError);
  EXPECT_THROW(
      admission->parse_status_packet(
          packet_epoch, admission->expected_ready_packet(packet_epoch)),
      component_manager::NativeExecutionRuntimeUnitAdmissionError);
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
  // Stop-progression reads are served beside Stop on the control socket.
  const auto read_stop_fault = [&] {
    return ExecutionPacket(
        priority,
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
        priority, "{\"api_version\":\"manyforge.execution_command.v1\","
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
      packet("reset",
             "{\"request_context\":" + context("late.reset") +
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
      std::string(contract::kLaunchEnvironmentContract), {}, {},
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
     Ur5ArmOnlyManagedCompositionProjectsExplicitAbsentGripper) {
  NativeFixture fixture(MANYFORGE_UR5_ARM_ONLY_FIXTURE_ROOT);
  const auto publication = PublishActiveFixture(fixture);
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch runtime_epoch(started.process_incarnation());
  const auto binding = NativeProjectionBinding(fixture, runtime_epoch);
  const auto resolved = adapter.ResolveProjection(binding);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;

  const auto initial_scene =
      MakeEmptyScene(fixture.bundle->value().cell_graph.cell_id, 1U);
  const core::ProjectionRevisionDTO initial_revision{
      fixture.bundle->retained_bundle_id(),
      fixture.bundle->value().cell_graph.graph_revision,
      initial_scene.shared()->revision};
  ASSERT_EQ(initial_revision,
            publication.request.candidate_projection_revision);
  const core::NativeProjectionBootstrapStatusQueryDTO bootstrap_query{
      core::BootstrapId("native-adapter-bootstrap"),
      core::BootstrapGeneration(1U), binding, initial_revision};
  ASSERT_TRUE(BootstrapNativeProjectionSet(
      fixture, adapter, bootstrap_query, initial_scene, *resolved.value()));

  ManagerSceneReadServer manager_scene_authority(
      fixture.manager_run_root.path(), initial_revision,
      *initial_scene.shared());

  const auto command_socket =
      fixture.execution_run_root.path() / "execution-command.sock";
  const auto identity_packet =
      R"({"api_version":"manyforge.execution_command.v1","command":"identity"})";
  std::optional<content_identity::CanonicalValue> identity_response;
  std::optional<content_identity::CanonicalValue> last_identity_response;
  const auto identity_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < identity_deadline &&
         !identity_response.has_value()) {
    try {
      auto response = ExecutionPacket(command_socket, identity_packet);
      last_identity_response = response;
      const auto *ok = response.find_object_member("ok");
      if (ok != nullptr && ok->is_bool() && ok->as_bool() &&
          response.find_object_member("identity") != nullptr) {
        identity_response = std::move(response);
      }
    } catch (const std::runtime_error &) {
      // The child may not have opened its command socket yet.
    }
    if (!identity_response.has_value()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (!identity_response.has_value()) {
    static_cast<void>(adapter.EnsureStopped(*fixture.query));
    FAIL() << "arm-only execution identity did not become ready"
           << (last_identity_response.has_value()
                   ? ": " + content_identity::canonical_json(
                               *last_identity_response)
                   : "; no command response was received");
    return;
  }

  const auto query = [&](const std::string_view command) {
    return ExecutionPacket(
        command_socket,
        "{\"api_version\":\"manyforge.execution_command.v1\","
        "\"command\":\"" +
            std::string(command) + "\"}");
  };
  const auto status_response = query("status");
  const auto targets_response = query("targets");
  const auto control_state_response = query("control_state");
  const auto observation_response = query("observation");
  const auto *identity = identity_response->find_object_member("identity");
  ASSERT_NE(identity, nullptr);
  EXPECT_EQ(TextMember(*identity, "schema_version"),
            "manyforge.execution_identity.v2");
  EXPECT_EQ(identity->as_object().size(), 7U);
  for (const auto field : {"gripper_command_schema_id",
                           "gripper_command_schema_revision",
                           "gripper_target_role_id", "motion_target_role_id",
                           "program_binding_id", "program_id",
                           "program_revision", "scene_revision"}) {
    EXPECT_EQ(identity->find_object_member(field), nullptr) << field;
  }

  const auto *status = status_response.find_object_member("status");
  ASSERT_NE(status, nullptr);
  EXPECT_EQ(TextMember(*status, "schema_version"),
            "manyforge.execution_status.v2");
  const auto *status_identity = status->find_object_member("identity");
  ASSERT_NE(status_identity, nullptr);
  EXPECT_EQ(*status_identity, *identity);
  const auto *status_targets = status->find_object_member("targets");
  ASSERT_NE(status_targets, nullptr);
  EXPECT_EQ(TextMember(*status_targets, "schema_version"),
            "manyforge.execution_targets.v3");
  const auto *status_components =
      status_targets->find_object_member("components");
  ASSERT_NE(status_components, nullptr);
  ASSERT_TRUE(status_components->is_array());
  ASSERT_EQ(status_components->as_array().size(), 1U);
  const auto &status_component = status_components->as_array().front();
  EXPECT_EQ(TextMember(status_component, "controllable_component_id"),
            "ur5_arm");
  const auto *status_component_targets =
      status_component.find_object_member("targets");
  ASSERT_NE(status_component_targets, nullptr);
  ASSERT_TRUE(status_component_targets->is_array());
  ASSERT_EQ(status_component_targets->as_array().size(), 1U);
  const auto &status_target = status_component_targets->as_array().front();
  EXPECT_EQ(TextMember(status_target, "command_target_id"), "arm");
  EXPECT_EQ(TextMember(status_target, "operation_capability_id"),
            "execution.trajectory.execute");
  EXPECT_EQ(TextMember(status_target, "presentation"), "motion");
  ASSERT_NE(status_target.find_object_member("command"), nullptr);
  const auto *status_control_state =
      status->find_object_member("control_state");
  ASSERT_NE(status_control_state, nullptr);
  EXPECT_EQ(TextMember(*status_control_state, "schema_version"),
            "manyforge.execution_control_state.v6");
  const auto *status_observed_targets =
      status_control_state->find_object_member("observed_targets");
  ASSERT_NE(status_observed_targets, nullptr);
  ASSERT_TRUE(status_observed_targets->is_array());
  ASSERT_EQ(status_observed_targets->as_array().size(), 1U);

  const auto *targets = targets_response.find_object_member("targets");
  ASSERT_NE(targets, nullptr);
  EXPECT_EQ(TextMember(*targets, "schema_version"),
            "manyforge.execution_targets.v3");
  ASSERT_EQ(targets->as_object().size(), 2U);
  const auto *components = targets->find_object_member("components");
  ASSERT_NE(components, nullptr);
  ASSERT_TRUE(components->is_array());
  ASSERT_EQ(components->as_array().size(), 1U);
  const auto &component = components->as_array().front();
  ASSERT_EQ(component.as_object().size(), 3U);
  const auto *component_id =
      component.find_object_member("controllable_component_id");
  ASSERT_NE(component_id, nullptr);
  const auto *availability = component.find_object_member("availability");
  ASSERT_NE(availability, nullptr);
  ASSERT_EQ(availability->as_object().size(), 2U);
  const auto *command_eligible =
      availability->find_object_member("command_eligible");
  ASSERT_NE(command_eligible, nullptr);
  ASSERT_TRUE(command_eligible->is_bool());
  const auto *availability_reason = availability->find_object_member("reason");
  ASSERT_NE(availability_reason, nullptr);
  EXPECT_EQ(command_eligible->as_bool(), availability_reason->is_null());
  if (!command_eligible->as_bool()) {
    EXPECT_TRUE(availability_reason->is_string());
    EXPECT_FALSE(TextMember(*availability, "reason").empty());
  }
  const auto *component_targets = component.find_object_member("targets");
  ASSERT_NE(component_targets, nullptr);
  ASSERT_TRUE(component_targets->is_array());
  ASSERT_EQ(component_targets->as_array().size(), 1U);
  const auto &target = component_targets->as_array().front();
  ASSERT_EQ(target.as_object().size(), 4U);
  EXPECT_EQ(TextMember(target, "command_target_id"), "arm");
  EXPECT_EQ(TextMember(target, "operation_capability_id"),
            "execution.trajectory.execute");
  EXPECT_EQ(TextMember(target, "presentation"), "motion");
  const auto *target_command = target.find_object_member("command");
  ASSERT_NE(target_command, nullptr);
  ASSERT_TRUE(target_command->is_object());
  ASSERT_EQ(target_command->as_object().size(), 9U);
  EXPECT_EQ(target_command->find_object_member("controllable_component_id"),
            nullptr);
  EXPECT_EQ(target_command->find_object_member("command_target_id"), nullptr);
  EXPECT_EQ(target_command->find_object_member("operation_capability_id"),
            nullptr);
  EXPECT_EQ(target_command->find_object_member("observed_state"), nullptr);
  const auto *manual_control =
      target_command->find_object_member("manual_control");
  ASSERT_NE(manual_control, nullptr);
  ASSERT_EQ(manual_control->as_object().size(), 4U);
  const auto *target_descriptor =
      target_command->find_object_member("target_descriptor");
  ASSERT_NE(target_descriptor, nullptr);
  EXPECT_EQ(TextMember(*target_command, "target_role_id"),
            TextMember(*target_descriptor, "target_role_id"));
  EXPECT_EQ(TextMember(*target_descriptor, "controllable_component_id"),
            TextMember(component, "controllable_component_id"));
  EXPECT_EQ(TextMember(*target_descriptor, "command_target_id"),
            TextMember(target, "command_target_id"));
  const auto *targets_identity =
      targets_response.find_object_member("identity");
  ASSERT_NE(targets_identity, nullptr);
  EXPECT_EQ(*targets_identity, *identity);

  const auto *control_state =
      control_state_response.find_object_member("control_state");
  ASSERT_NE(control_state, nullptr);
  EXPECT_EQ(TextMember(*control_state, "schema_version"),
            "manyforge.execution_control_state.v6");
  EXPECT_EQ(control_state->find_object_member("observed_motion"), nullptr);
  EXPECT_EQ(control_state->find_object_member("observed_gripper"), nullptr);
  const auto *observed_targets =
      control_state->find_object_member("observed_targets");
  ASSERT_NE(observed_targets, nullptr);
  ASSERT_TRUE(observed_targets->is_array());
  ASSERT_EQ(observed_targets->as_array().size(), 1U);
  const auto &observed_target = observed_targets->as_array().front();
  EXPECT_EQ(TextMember(observed_target, "controllable_component_id"),
            "ur5_arm");
  EXPECT_EQ(TextMember(observed_target, "command_target_id"), "arm");
  EXPECT_EQ(TextMember(observed_target, "operation_capability_id"),
            "execution.trajectory.execute");
  const auto *observed = observed_target.find_object_member("observed");
  ASSERT_NE(observed, nullptr);
  EXPECT_FALSE(observed->is_null());
  EXPECT_EQ(TextMember(*observed, "kind"), "joint_positions");
  const auto *control_state_identity =
      control_state_response.find_object_member("identity");
  ASSERT_NE(control_state_identity, nullptr);
  EXPECT_EQ(*control_state_identity, *identity);

  const auto *observation =
      observation_response.find_object_member("observation");
  ASSERT_NE(observation, nullptr);
  const auto *observation_ok =
      observation_response.find_object_member("ok");
  ASSERT_NE(observation_ok, nullptr);
  ASSERT_TRUE(observation_ok->is_bool());
  EXPECT_TRUE(observation_ok->as_bool());
  EXPECT_EQ(TextMember(*observation, "schema_version"),
            "manyforge.execution_observation.v4");
  // A native commanded owner without a measured source adds no comparison row.
  const auto *measured_targets =
      observation->find_object_member("measured_targets");
  ASSERT_NE(measured_targets, nullptr);
  ASSERT_TRUE(measured_targets->is_array());
  EXPECT_TRUE(measured_targets->as_array().empty());
  const auto *observation_targets =
      observation->find_object_member("targets");
  ASSERT_NE(observation_targets, nullptr);
  ASSERT_TRUE(observation_targets->is_array());
  ASSERT_EQ(observation_targets->as_array().size(), 1U);
  const auto &observation_target = observation_targets->as_array().front();
  EXPECT_EQ(TextMember(observation_target, "controllable_component_id"),
            TextMember(observed_target, "controllable_component_id"));
  EXPECT_EQ(TextMember(observation_target, "command_target_id"),
            TextMember(observed_target, "command_target_id"));
  EXPECT_EQ(TextMember(observation_target, "operation_capability_id"),
            TextMember(observed_target, "operation_capability_id"));

  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     StandaloneRobotiqPositionCompositionNeedsNoArmProjectionAcrossRestart) {
  NativeFixture fixture(MANYFORGE_I6_NATIVE_GRIPPER_ONLY_FIXTURE_ROOT);
  const auto manifest = deployment::parse_strict_json_model_yaml(
      ReadFile(fixture.root / "manifest.json"));
  const auto *gripper_only =
      manifest.root().find_object_member("gripper_only");
  ASSERT_NE(gripper_only, nullptr);
  ASSERT_TRUE(gripper_only->is_bool());
  ASSERT_TRUE(gripper_only->as_bool());
  ASSERT_EQ(TextMember(manifest.root(), "gripper_actuation_kind"), "position_commanded");
  ASSERT_EQ(TextMember(manifest.root(), "gripper_output_mode"), "commanded");
  for (const auto field : {"target_role_id", "execution_capability_id",
                           "planning_capability_ids", "source_joint_names"}) {
    EXPECT_EQ(manifest.root().find_object_member(field), nullptr) << field;
  }
  // Cold readers must recover only the genuine standalone Robotiq source,
  // one independent angular command resource and its five real mimic joints.
  const auto &graph = fixture.bundle->value().cell_graph;
  ASSERT_EQ(graph.model_instances.size(), 1U);
  EXPECT_EQ(graph.model_instances.front().model_asset_id.value(), "robotiq_2f_85");
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  EXPECT_EQ(graph.controllable_components.front().controllable_component_id.value(), "robotiq_gripper");
  ASSERT_EQ(graph.control_bindings.size(), 2U);
  const auto &command_binding = graph.control_bindings.front();
  const auto &feedback_binding = graph.control_bindings.back();
  EXPECT_EQ(feedback_binding.capability_id.value(), "tool.gripper.feedback");
  EXPECT_EQ(feedback_binding.controllable_component_id, command_binding.controllable_component_id);
  EXPECT_EQ(feedback_binding.command_target_id, command_binding.command_target_id);
  EXPECT_EQ(feedback_binding.control_domain_id, command_binding.control_domain_id);
  EXPECT_EQ(feedback_binding.resource_bindings, command_binding.resource_bindings);
  EXPECT_EQ(feedback_binding.provider_instance_id, command_binding.provider_instance_id);
  EXPECT_EQ(feedback_binding.provider_runtime_unit_id, command_binding.provider_runtime_unit_id);
  EXPECT_EQ(feedback_binding.controller_topology_id, command_binding.controller_topology_id);
  EXPECT_EQ(feedback_binding.control_interface_contract_id, command_binding.control_interface_contract_id);
  EXPECT_EQ(feedback_binding.control_interface_revision, command_binding.control_interface_revision);
  EXPECT_EQ(feedback_binding.topology_revision, command_binding.topology_revision);
  EXPECT_EQ(feedback_binding.provider_manifest_id, command_binding.provider_manifest_id);
  EXPECT_EQ(feedback_binding.provider_manifest_revision, command_binding.provider_manifest_revision);
  EXPECT_EQ(feedback_binding.legal_control_route_ids, command_binding.legal_control_route_ids);
  EXPECT_EQ(feedback_binding.route_endpoint_projections, command_binding.route_endpoint_projections);
  EXPECT_EQ(feedback_binding.state_authority_id, command_binding.state_authority_id);
  EXPECT_EQ(feedback_binding.state_authority_class, command_binding.state_authority_class);
  EXPECT_EQ(feedback_binding.stop_evidence_policy_id, command_binding.stop_evidence_policy_id);
  EXPECT_EQ(feedback_binding.stop_evidence_policy_revision, command_binding.stop_evidence_policy_revision);
  EXPECT_EQ(feedback_binding.containment_granularity, command_binding.containment_granularity);
  EXPECT_EQ(graph.control_bindings.front().capability_id.value(), "tool.gripper.command");
  EXPECT_EQ(graph.control_bindings.front().stop_evidence_policy_id.value(),
            "manyforge.gripper_position_cancellation");
  ASSERT_EQ(graph.control_domains.size(), 1U);
  EXPECT_EQ(command_binding.control_domain_id, graph.control_domains.front().control_domain_id);
  EXPECT_EQ(command_binding.provider_runtime_unit_id, graph.runtime_units.front().runtime_unit_id);
  EXPECT_TRUE(graph.component_attachments.empty());
  const auto configuration = contract::require_configuration(
      *fixture.bundle, graph.runtime_units.front().runtime_unit_id);
  EXPECT_TRUE(configuration.initial_joint_positions_rad.empty());
  EXPECT_TRUE(configuration.motion_target_limits.empty());
  ASSERT_EQ(configuration.controller_profiles.size(), 1U);
  EXPECT_EQ(configuration.controller_profiles.front().controllable_component_id,
            command_binding.controllable_component_id);
  EXPECT_EQ(configuration.controller_profiles.front().mock_profile,
            std::optional<std::string>("ur5e_robotiq_2f_85"));
  ASSERT_EQ(configuration.gripper_target_configurations.size(), 1U);
  const auto &authored = configuration.gripper_target_configurations.front();
  EXPECT_EQ(authored.actuation_kind, "position_commanded");
  EXPECT_EQ(authored.output_mode, core::GripperOutputMode::kCommanded);
  EXPECT_EQ(authored.command_duration_s, 1.0);
  EXPECT_FALSE(authored.delivery_result_timeout_s.has_value());
  EXPECT_EQ(authored.mount_frame, "robotiq_85_base_link");
  EXPECT_EQ(authored.attach_frame, "robotiq_85_base_link");
  EXPECT_EQ(authored.tcp_frame, "robotiq_85_base_link");
  EXPECT_EQ(authored.touch_links.size(), 9U);
  const auto &products = fixture.bundle->value().compiled_products;
  ASSERT_EQ(products.symbol_role_maps.size(), 1U);
  deployment::FilesystemVerifiedRetainedModelMapSource maps(fixture.root / "retained-package");
  const auto model_map = maps.get_verified({fixture.bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(model_map.has_value());
  ASSERT_EQ(model_map->targets().size(), 1U);
  ASSERT_EQ(model_map->control_bindings().size(), 2U);
  EXPECT_EQ(model_map->control_bindings().front().control_binding_id, command_binding.control_binding_id);
  EXPECT_EQ(model_map->control_bindings().back().control_binding_id, feedback_binding.control_binding_id);
  EXPECT_EQ(model_map->control_bindings().front().resources,
            model_map->control_bindings().back().resources);
  ASSERT_EQ(model_map->model_occurrences().size(), 1U);
  EXPECT_EQ(model_map->model_occurrences().front().owner_model_role_id,
            std::optional(core::ModelRoleId("gripper_model")));
  const auto &resources = model_map->control_bindings().front().resources;
  ASSERT_EQ(resources.size(), 1U);
  ASSERT_EQ(resources.front().coordinates.size(), 6U);
  EXPECT_EQ(resources.front().coordinates.front().source_joint_name,
            "robotiq_85_left_knuckle_joint");
  EXPECT_FALSE(resources.front().coordinates.front().derived_mimic);
  EXPECT_EQ(resources.front().coordinates.front().coordinate_kind,
            deployment::UrdfCoordinateKind::kAngular);
  EXPECT_EQ(std::count_if(resources.front().coordinates.begin(), resources.front().coordinates.end(),
      [](const auto &row) { return row.derived_mimic; }), 5);
  for (const auto &row : resources.front().coordinates)
    EXPECT_EQ(row.coordinate_kind, deployment::UrdfCoordinateKind::kAngular);
  ASSERT_EQ(model_map->coordinates().size(), 8U);
  EXPECT_EQ(std::count_if(model_map->coordinates().begin(), model_map->coordinates().end(),
      [](const auto &row) { return row.coordinate_kind == deployment::UrdfCoordinateKind::kAngular; }), 6);
  EXPECT_EQ(std::count_if(model_map->coordinates().begin(), model_map->coordinates().end(),
      [](const auto &row) { return row.coordinate_kind == deployment::UrdfCoordinateKind::kNone &&
                                 row.joint_type == deployment::UrdfJointType::kFixed; }), 2);
  EXPECT_EQ(std::count_if(model_map->coordinates().begin(), model_map->coordinates().end(),
      [](const auto &row) { return row.coordinate_kind != deployment::UrdfCoordinateKind::kNone &&
                                 !row.mimic_source_compiled_joint_name.has_value(); }), 1);
  EXPECT_EQ(std::count_if(model_map->coordinates().begin(), model_map->coordinates().end(),
      [](const auto &row) { return row.mimic_source_compiled_joint_name.has_value(); }), 5);
  for (const auto &row : model_map->coordinates())
    EXPECT_TRUE(row.source_joint_name.starts_with("robotiq_85_")) << row.source_joint_name;
  deployment::FilesystemVerifiedRetainedPlanningModelSource planning_source(fixture.root / "retained-package");
  const auto model = planning_source.get_verified({fixture.bundle->retained_bundle_id(),
      products.planning_collision_model.product_id});
  ASSERT_TRUE(model.has_value());
  ASSERT_EQ(model->model_source_urdfs().size(), 1U);
  const auto source_documents = deployment::parse_model_documents_for_projection(
      model->model_source_urdfs().front().urdf_bytes, std::nullopt);
  EXPECT_EQ(source_documents.urdf.links.size(), 9U);
  ASSERT_EQ(source_documents.urdf.joints.size(), 8U);
  const auto master = std::find_if(source_documents.urdf.joints.begin(), source_documents.urdf.joints.end(),
      [](const auto &joint) { return joint.name == "robotiq_85_left_knuckle_joint"; });
  ASSERT_NE(master, source_documents.urdf.joints.end());
  ASSERT_TRUE(master->limit.has_value());
  EXPECT_EQ(master->limit->lower, 0.0);
  EXPECT_EQ(master->limit->upper, 0.8);
  EXPECT_EQ(master->limit->velocity, 0.5);
  EXPECT_EQ(master->limit->effort, 50.0);
  EXPECT_FALSE(master->mimic.has_value());
  EXPECT_EQ(std::count_if(source_documents.urdf.joints.begin(), source_documents.urdf.joints.end(),
      [](const auto &joint) { return joint.mimic.has_value(); }), 5);
  for (const auto &joint : source_documents.urdf.joints) {
    const auto coordinate = std::find_if(model_map->coordinates().begin(), model_map->coordinates().end(),
        [&](const auto &row) { return row.source_joint_name == joint.name; });
    ASSERT_NE(coordinate, model_map->coordinates().end()) << joint.name;
    EXPECT_EQ(coordinate->joint_type, joint.type) << joint.name;
    EXPECT_EQ(coordinate->coordinate_kind, joint.coordinate_kind) << joint.name;
    ASSERT_EQ(coordinate->mimic_source_compiled_joint_name.has_value(), joint.mimic.has_value()) << joint.name;
    if (joint.mimic) {
      const auto parent = std::find_if(model_map->coordinates().begin(), model_map->coordinates().end(),
          [&](const auto &row) { return row.source_joint_name == joint.mimic->joint_name; });
      ASSERT_NE(parent, model_map->coordinates().end()) << joint.name;
      EXPECT_EQ(*coordinate->mimic_source_compiled_joint_name, parent->compiled_joint_name);
      EXPECT_EQ(coordinate->mimic_multiplier, joint.mimic->multiplier);
      EXPECT_EQ(coordinate->mimic_offset, joint.mimic->offset);
    }
  }
  for (const auto &link : source_documents.urdf.links)
    EXPECT_TRUE(link.name.starts_with("robotiq_85_")) << link.name;
  deployment::FilesystemVerifiedCatalogArtifactSource catalogs(fixture.root / "retained-package");
  const core::StopEvidencePolicyCatalogRecordRefDTO *policy = nullptr;
  for (const auto &record : fixture.bundle->value().catalogs.records) {
    const auto *row = std::get_if<core::StopEvidencePolicyCatalogRecordRefDTO>(&record);
    if (row == nullptr || row->record_id != graph.control_bindings.front().stop_evidence_policy_id) continue;
    ASSERT_EQ(policy, nullptr);
    policy = row;
  }
  ASSERT_NE(policy, nullptr);
  const auto policy_bytes = catalogs.get_verified(policy->content_addressed_store_key, policy->content_digest);
  ASSERT_TRUE(policy_bytes.has_value());
  const auto policy_document = deployment::parse_strict_json_model_yaml(*policy_bytes);
  const auto *stages = policy_document.root().find_object_member("escalation_stages");
  ASSERT_NE(stages, nullptr);
  ASSERT_EQ(stages->as_array().size(), 2U);
  EXPECT_EQ(std::count_if(stages->as_array().begin(), stages->as_array().end(), [](const auto &stage) {
    return TextMember(stage, "stage_kind") == "software_hard";
  }), 1);
  const auto program = deployment::parse_strict_json_model_yaml(ReadFile(fixture.root / "program.json"));
  const auto *tree = program.root().find_object_member("tree");
  ASSERT_NE(tree, nullptr);
  const auto *children = tree->find_object_member("children");
  ASSERT_NE(children, nullptr);
  ASSERT_EQ(children->as_array().size(), 2U);
  for (std::size_t index = 0; index < 2U; ++index) {
    const auto &node = children->as_array()[index];
    EXPECT_EQ(TextMember(node, "node_id"), "gripper.command_position");
    const auto *parameters = node.find_object_member("parameters");
    ASSERT_NE(parameters, nullptr);
    EXPECT_EQ(TextMember(*parameters, "target_role"), "gripper");
    const auto *position = parameters->find_object_member("position");
    ASSERT_NE(position, nullptr);
    EXPECT_EQ(TextMember(*position, "kind"), "named_preset");
    EXPECT_EQ(TextMember(*position, "name"), index == 0U ? "closed" : "open");
    EXPECT_EQ(parameters->find_object_member("command_duration_s")->as_number(), 1.0);
  }
  static_cast<void>(PublishActiveFixture(fixture));
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT);
  const auto socket = fixture.execution_run_root.path() /
                      "execution-command.sock";
  const auto query = [&](const std::string_view command) {
    return ExecutionPacket(
        socket,
        "{\"api_version\":\"manyforge.execution_command.v1\","
        "\"command\":\"" + std::string(command) + "\"}");
  };
  const auto check_status = [&] {
    std::optional<content_identity::CanonicalValue> result;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline && !result.has_value()) {
      try {
        auto response = query("status");
        const auto *ok = response.find_object_member("ok");
        if (ok != nullptr && ok->is_bool() && ok->as_bool())
          result = std::move(response);
      } catch (const std::runtime_error &) {
        // The managed child can still be opening its command socket.
      }
      if (!result.has_value())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return result;
  };

  for (int attempt = 0; attempt != 2; ++attempt) {
    const auto started = adapter.EnsureRunning(*fixture.query);
    ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
    const auto response = check_status();
    ASSERT_TRUE(response.has_value())
        << "gripper-only managed status did not become ready";
    const auto *status = response->find_object_member("status");
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(TextMember(*status, "schema_version"),
              "manyforge.execution_status.v2");
    const auto *identity = status->find_object_member("identity");
    ASSERT_NE(identity, nullptr);
    EXPECT_EQ(TextMember(*identity, "schema_version"),
              "manyforge.execution_identity.v2");
    for (const auto field : {"motion_target_role_id", "program_binding_id",
                             "gripper_target_role_id"})
      EXPECT_EQ(identity->find_object_member(field), nullptr) << field;

    const auto *targets = status->find_object_member("targets");
    ASSERT_NE(targets, nullptr);
    const auto *components = targets->find_object_member("components");
    ASSERT_NE(components, nullptr);
    ASSERT_TRUE(components->is_array());
    ASSERT_EQ(components->as_array().size(), 1U);
    EXPECT_EQ(TextMember(components->as_array().front(), "controllable_component_id"),
              command_binding.controllable_component_id.value());
    const auto *target_rows =
        components->as_array().front().find_object_member("targets");
    ASSERT_NE(target_rows, nullptr);
    ASSERT_TRUE(target_rows->is_array());
    // Raw catalog aliases remain visible: feedback has no second manual
    // actuator. Select the command by capability, independently of row order.
    ASSERT_EQ(target_rows->as_array().size(), 2U);
    std::map<std::string, const CanonicalValue *> aliases;
    for (const auto &row : target_rows->as_array()) {
      EXPECT_EQ(TextMember(row, "command_target_id"), command_binding.command_target_id.value());
      const auto capability = TextMember(row, "operation_capability_id");
      ASSERT_TRUE(capability == "tool.gripper.command" || capability == "tool.gripper.feedback");
      ASSERT_TRUE(aliases.emplace(capability, &row).second);
    }
    ASSERT_EQ(aliases.size(), 2U);
    const auto &target = *aliases.at("tool.gripper.command");
    const auto &feedback_target = *aliases.at("tool.gripper.feedback");
    EXPECT_EQ(TextMember(feedback_target, "presentation"), "unsupported");
    const auto *feedback_command = feedback_target.find_object_member("command");
    ASSERT_NE(feedback_command, nullptr);
    EXPECT_TRUE(feedback_command->is_null());
    EXPECT_EQ(TextMember(target, "presentation"), "gripper");
    EXPECT_EQ(TextMember(target, "operation_capability_id"),
              "tool.gripper.command");
    const auto *command = target.find_object_member("command");
    ASSERT_NE(command, nullptr);
    ASSERT_TRUE(command->is_object());
    const auto *descriptor = command->find_object_member("target_descriptor");
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(TextMember(*descriptor, "controllable_component_id"),
              command_binding.controllable_component_id.value());
    EXPECT_EQ(TextMember(*descriptor, "command_target_id"), command_binding.command_target_id.value());
    EXPECT_EQ(TextMember(*descriptor, "schema_version"),
              "manyforge.manual_control_gripper_descriptor.v3");
    EXPECT_EQ(TextMember(*descriptor, "actuation_kind"), "position_commanded");
    EXPECT_EQ(TextMember(*descriptor, "output_mode"), "commanded");
    EXPECT_EQ(TextMember(*descriptor, "completion_policy"), "goal_fenced_target_reached");
    EXPECT_EQ(TextMember(*descriptor, "grasp_confirmation_policy"),
              "binding_feedback_required");
    EXPECT_EQ(TextMember(*descriptor, "coordinate_kind"), "angular_rad");
    EXPECT_EQ(TextMember(*descriptor, "source_coordinate_name"), "robotiq_85_left_knuckle_joint");
    EXPECT_EQ(descriptor->find_object_member("min_position")->as_number(), 0.0);
    EXPECT_EQ(descriptor->find_object_member("max_position")->as_number(), 0.8);
    const auto *followers = descriptor->find_object_member("mimic_followers");
    ASSERT_NE(followers, nullptr);
    EXPECT_EQ(followers->as_array().size(), 5U);
    EXPECT_EQ(TextMember(*descriptor, "source_group_name"), "robotiq_2f_85_gripper");
    const auto *presets = descriptor->find_object_member("named_presets");
    ASSERT_NE(presets, nullptr);
    ASSERT_EQ(presets->as_array().size(), 2U);
    std::set<std::string> preset_names;
    for (const auto &preset : presets->as_array()) {
      const auto name = TextMember(preset, "name");
      ASSERT_TRUE(name == "closed" || name == "open");
      EXPECT_TRUE(preset_names.insert(name).second);
      EXPECT_EQ(preset.find_object_member("master_position")->as_number(),
                name == "closed" ? 0.8 : 0.0);
    }
    const auto *manual = command->find_object_member("manual_control");
    ASSERT_NE(manual, nullptr);
    ASSERT_TRUE(manual->is_object());

    const auto *control = status->find_object_member("control_state");
    ASSERT_NE(control, nullptr);
    const auto *scale = control->find_object_member("runtime_scale_control");
    ASSERT_NE(scale, nullptr);
    EXPECT_TRUE(scale->is_null());
    const auto *observed = control->find_object_member("observed_targets");
    ASSERT_NE(observed, nullptr);
    ASSERT_TRUE(observed->is_array());
    // Observations follow the one installed physical command channel;
    // the raw feedback alias does not create a second state owner.
    ASSERT_EQ(observed->as_array().size(), 1U);
    EXPECT_EQ(TextMember(observed->as_array().front(), "controllable_component_id"),
              command_binding.controllable_component_id.value());
    EXPECT_EQ(TextMember(observed->as_array().front(), "command_target_id"),
              command_binding.command_target_id.value());
    EXPECT_EQ(TextMember(observed->as_array().front(),
                         "operation_capability_id"),
              "tool.gripper.command");
    const auto *coordinate_observation =
        observed->as_array().front().find_object_member("observed");
    ASSERT_NE(coordinate_observation, nullptr);
    // This is the retained Position observation channel. Initial projection
    // is not a receipt of a command or measured reached feedback; the separate
    // Manager/CCLC qualification owns those actual effects.
    ASSERT_FALSE(coordinate_observation->is_null());
    EXPECT_EQ(TextMember(*coordinate_observation, "kind"), "coordinate");
    EXPECT_EQ(TextMember(*coordinate_observation, "source_coordinate_name"),
              "robotiq_85_left_knuckle_joint");
    EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
              component_manager::ProcessPresenceState::kAbsent);
  }
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
  ASSERT_TRUE(BootstrapNativeProjectionSet(
      fixture, adapter, bootstrap_query, initial_scene, *resolved.value()));
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

  const auto &liveness_policy = deployment::execution_runtime_contract::manager_liveness_policy();
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
  const component_manager::ManagerLivenessTransportRequestDTO liveness_request{
      fixture.query->plan.runtime_unit_id, publication.publication_generation,
      publication.request.candidate_graph_revision,
      publication.durable_public_pointer_content_digest, required_set,
      liveness_policy, heartbeat, reconciliation};
  const auto liveness = adapter.RenewManagerLiveness(liveness_request);
  EXPECT_EQ(liveness.active_bundle_id, required_set.active_bundle_id);
  EXPECT_EQ(liveness.required_runtime_set_digest, required_set_digest);
  ASSERT_EQ(liveness.status.state, core::ManagerLivenessStateDTO::kCurrent);
  ASSERT_FALSE(liveness.status.command_fenced);
  // From here on the test is the Manager of this required set. The child keeps
  // attaching its composition asynchronously, so the lease is renewed at the
  // policy cadence rather than assumed to outlast that work.
  ManagerHeartbeatLane heartbeat_lane(adapter, liveness_request);

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
  ASSERT_TRUE(authoring_admission.has_value()) << heartbeat_lane.Failure();
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
  ASSERT_TRUE(released_claim.ok()) << released_claim.error().message;
  EXPECT_TRUE(released_claim.value().authoring_allowed);
  EXPECT_GT(released_claim.value().admission_generation,
            claimed.value().acquired_admission_generation);

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

  // The same Manager epoch keeps renewing the receiver it reconciled.
  const auto renewed_liveness =
      heartbeat_lane.WaitForAccepted(heartbeat.heartbeat_sequence + 1U,
                                     std::chrono::seconds(30));
  ASSERT_TRUE(renewed_liveness.has_value()) << heartbeat_lane.Failure();
  ASSERT_EQ(renewed_liveness->status.state,
            core::ManagerLivenessStateDTO::kCurrent);
  EXPECT_FALSE(renewed_liveness->status.command_fenced);

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
  const core::NativeProjectionRebuildDTO rebuild_request{
      binding, candidate_revision,
      fixture.bundle->value().compiled_products.planning_collision_model,
      candidate_scene_root.shared()};
  const auto rebuilt_candidate =
      resolved.value()->RebuildProjection(rebuild_request);
  ASSERT_TRUE(rebuilt_candidate.presence_valid());
  EXPECT_EQ(rebuilt_candidate.result,
            core::ProjectionCommitResultStateDTO::kRejectedRollbackProven);
  ASSERT_TRUE(rebuilt_candidate.error.has_value());
  EXPECT_EQ(rebuilt_candidate.error->details.at("reason"),
            "native_projection_rebuild_not_required");
  EXPECT_TRUE(resolved.value()->ProjectionHealth().healthy_for_target(
      initial_revision));

  auto foreign = binding;
  foreign.provider_binding_revision =
      core::CapabilityProviderBindingRevision("foreign-binding-revision");
  EXPECT_FALSE(adapter.ResolveProjection(foreign).ok());

  // The restarted child below is a new receiver epoch outside this required
  // set; this Manager lane ends with the epoch it renewed.
  heartbeat_lane.Stop();
  EXPECT_EQ(heartbeat_lane.Failure(), "Manager liveness lane has not failed");
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

TEST(RetainedNativeRuntimeUnitAdapter,
     SameProviderCandidateRebuildKeepsPredecessorLeaseUntilRelease) {
  // Merge immutable CAS entries in a disposable package, leaving both generated
  // fixture roots untouched. The ordinary verifier admits each exact bundle.
  ScopedTemporaryRoot merged;
  const auto fixture_root = merged.path() / "fixture";
  std::filesystem::copy(MANYFORGE_I6_NATIVE_FIXTURE_ROOT, fixture_root,
                        std::filesystem::copy_options::recursive);
  NativeFixture successor(MANYFORGE_NATIVE_PROJECTION_SUCCESSOR_FIXTURE_ROOT);
  std::filesystem::copy(
      successor.root / "retained-package", fixture_root / "retained-package",
      std::filesystem::copy_options::recursive |
          std::filesystem::copy_options::skip_existing);
  NativeFixture fixture(fixture_root);
  ASSERT_NE(fixture.bundle->retained_bundle_id(),
            successor.bundle->retained_bundle_id());
  ASSERT_NE(fixture.bundle->value().cell_graph.graph_revision,
            successor.bundle->value().cell_graph.graph_revision);
  ASSERT_EQ(fixture.bundle->value().cell_graph.cell_id,
            successor.bundle->value().cell_graph.cell_id);
  ASSERT_EQ(fixture.query->plan.product_identity,
            successor.query->plan.product_identity);
  ASSERT_EQ(fixture.query->plan.configuration_identity,
            successor.query->plan.configuration_identity);
  ASSERT_EQ(fixture.query->plan.runtime_contract_offer_digest,
            successor.query->plan.runtime_contract_offer_digest);

  const auto publication = PublishActiveFixture(fixture);
  ScopedTemporaryRoot projection_control;
  auto adapter = MakeAdapter(fixture, MANYFORGE_NATIVE_PROJECTION_TEST_TARGET_ROOT);
  ProjectionConstructionHold projection_hold(projection_control.path(),
                                             "rebuild");
  ASSERT_TRUE(projection_hold.Configure());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const auto binding = NativeProjectionBinding(fixture, epoch);
  ASSERT_EQ(binding, NativeProjectionBinding(successor, epoch));
  ASSERT_EQ(NativeProjectionBindings(fixture, epoch),
            NativeProjectionBindings(successor, epoch));
  const auto resolved = adapter.ResolveProjection(binding);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  auto &projection = *resolved.value();
  const auto initial_scene =
      MakeEmptyScene(fixture.bundle->value().cell_graph.cell_id, 1U);
  const auto initial_revision = publication.request.candidate_projection_revision;
  const core::NativeProjectionBootstrapStatusQueryDTO bootstrap{
      core::BootstrapId("native-adapter-bootstrap"),
      core::BootstrapGeneration(1U), binding, initial_revision};
  ASSERT_TRUE(BootstrapNativeProjectionSet(
      fixture, adapter, bootstrap, initial_scene, projection));

  const auto published_health =
      projection.ProjectionHealthForRevision(initial_revision);
  ASSERT_TRUE(published_health.healthy_for_target(initial_revision));
  EXPECT_THROW(static_cast<void>(projection.ProjectionHealthForRevision(
                   core::ProjectionRevisionDTO{})),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  auto missing_revision = initial_revision;
  missing_revision.retained_bundle_id =
      core::RetainedBundleId("unprepared-bundle");
  try {
    static_cast<void>(projection.ProjectionHealthForRevision(missing_revision));
    ADD_FAILURE() << "missing selected model fell back to current";
  } catch (
      const component_manager::RetainedNativeRuntimeUnitAdapterError &error) {
    EXPECT_NE(std::string(error.what()).find("unprepared retained model"),
              std::string::npos)
        << error.what();
  }

  const core::ProjectionLeaseId old_lease("native-adapter.predecessor-lease");
  const auto retained =
      projection.RetainProjectionBinding(initial_revision, old_lease);
  ASSERT_TRUE(retained.ok()) << retained.error().message;
  const auto &control =
      fixture.bundle->value().cell_graph.control_bindings.front();
  const core::SceneFrameResolutionQueryDTO frame_query{
      core::TargetCarrierSceneFrameRefDTO{
          control.controllable_component_id, control.command_target_id,
          core::FrameRoleId("tcp")},
      retained.value()};
  std::optional<core::SceneFrameResolutionReceiptDTO> initial_frame;
  const auto frame_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < frame_deadline) {
    const auto frame = projection.ResolveSceneFrame(frame_query);
    if (frame.ok()) {
      initial_frame = frame.value();
      break;
    }
    ASSERT_EQ(frame.error().details.at("reason"),
              "native_scene_frame_lease_unavailable")
        << frame.error().message;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(initial_frame.has_value());
  ASSERT_TRUE(initial_frame->valid());

  // EnsureRunning prepares a successor under the existing provider key without
  // replacing the host. It is private until its exact rebuild becomes healthy.
  const auto prepared = adapter.EnsureRunning(*successor.query);
  ASSERT_EQ(prepared.state(), component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(prepared.process_incarnation(), started.process_incarnation());
  const auto frame_after_prepare = projection.ResolveSceneFrame(frame_query);
  ASSERT_TRUE(frame_after_prepare.ok()) << frame_after_prepare.error().message;
  EXPECT_EQ(frame_after_prepare.value().projection_binding, retained.value());
  EXPECT_EQ(frame_after_prepare.value().resolved_frame,
            initial_frame->resolved_frame);

  const core::ProjectionRevisionDTO candidate_revision{
      successor.bundle->retained_bundle_id(),
      successor.bundle->value().cell_graph.graph_revision,
      initial_scene.shared()->revision};
  const auto prepared_published_health =
      projection.ProjectionHealthForRevision(initial_revision);
  EXPECT_TRUE(prepared_published_health.healthy_for_target(initial_revision));
  EXPECT_EQ(prepared_published_health.native_projection_generation,
            published_health.native_projection_generation);
  const auto prepared_candidate_health =
      projection.ProjectionHealthForRevision(candidate_revision);
  EXPECT_EQ(prepared_candidate_health.native_runtime_binding, binding);
  EXPECT_EQ(prepared_candidate_health.state,
            core::ProjectionHealthState::kUninitialized);
  EXPECT_FALSE(
      prepared_candidate_health.healthy_for_target(candidate_revision));

  const core::NativeProjectionRebuildDTO rebuild{
      binding, candidate_revision,
      successor.bundle->value().compiled_products.planning_collision_model,
      initial_scene.shared()};
  const auto accepted = projection.RebuildProjection(rebuild);
  ASSERT_TRUE(accepted.presence_valid());
  ASSERT_TRUE(accepted.result == core::ProjectionCommitResultStateDTO::kAccepted ||
              accepted.result == core::ProjectionCommitResultStateDTO::kCommitted);
  const auto rebuild_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  const auto entered = projection_hold.Entered();
  while (!std::filesystem::exists(entered) &&
         std::chrono::steady_clock::now() < rebuild_deadline) {
    std::this_thread::yield();
  }
  ASSERT_TRUE(std::filesystem::exists(entered));
  const auto pending_published_health =
      projection.ProjectionHealthForRevision(initial_revision);
  EXPECT_TRUE(pending_published_health.healthy_for_target(initial_revision));
  EXPECT_EQ(pending_published_health.native_projection_generation,
            published_health.native_projection_generation);
  const auto pending_candidate_health =
      projection.ProjectionHealthForRevision(candidate_revision);
  EXPECT_EQ(pending_candidate_health.native_runtime_binding, binding);
  EXPECT_EQ(pending_candidate_health.state,
            core::ProjectionHealthState::kRebuilding);
  EXPECT_FALSE(pending_candidate_health.healthy_for_target(candidate_revision));

  // Exact preparation replay must not conflict with its own progressing
  // rebuild, while a different model remains refused.
  const auto pending_preparation = adapter.EnsureRunning(*successor.query);
  EXPECT_EQ(pending_preparation.state(),
            component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(pending_preparation.process_incarnation(),
            started.process_incarnation());
  // The child itself refuses the other model; an adapter-side failure such as
  // a response timeout would stop the child and is not this refusal.
  try {
    static_cast<void>(adapter.EnsureRunning(*fixture.query));
    ADD_FAILURE() << "conflicting projection preparation was admitted";
  } catch (const component_manager::RetainedNativeRuntimeUnitAdapterError &error) {
    EXPECT_NE(std::string(error.what()).find(
                  "predecessor is still retained"),
              std::string::npos)
        << error.what();
  }
  EXPECT_EQ(adapter.ObserveProcess(*successor.query).process_incarnation(),
            started.process_incarnation());
  ASSERT_TRUE(projection_hold.Release());

  auto health = projection.ProjectionHealth();
  while (!health.healthy_for_target(candidate_revision) &&
         std::chrono::steady_clock::now() < rebuild_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    health = projection.ProjectionHealth();
  }
  ASSERT_TRUE(health.healthy_for_target(candidate_revision));
  EXPECT_TRUE(projection.ProjectionHealthForRevision(initial_revision)
                  .healthy_for_target(initial_revision));
  const auto selected_candidate_health =
      projection.ProjectionHealthForRevision(candidate_revision);
  EXPECT_TRUE(selected_candidate_health.healthy_for_target(candidate_revision));
  EXPECT_EQ(selected_candidate_health.native_runtime_binding, binding);
  EXPECT_EQ(selected_candidate_health.native_projection_generation,
            health.native_projection_generation);
  // Same model, different Scene revision: ordinary maintenance must receive
  // the truthful existing row, never a fabricated target revision.
  auto future_scene_revision = candidate_revision;
  ++future_scene_revision.scene_revision.revision_id;
  EXPECT_EQ(projection.ProjectionHealthForRevision(future_scene_revision)
                .active_projection_revision,
            std::optional<core::ProjectionRevisionDTO>(candidate_revision));
  const auto old_frame = projection.ResolveSceneFrame(frame_query);
  ASSERT_TRUE(old_frame.ok()) << old_frame.error().message;
  EXPECT_EQ(old_frame.value().projection_binding, retained.value());
  EXPECT_EQ(old_frame.value().resolved_frame, initial_frame->resolved_frame);
  const auto retained_replay =
      projection.RetainProjectionBinding(initial_revision, old_lease);
  ASSERT_TRUE(retained_replay.ok()) << retained_replay.error().message;
  EXPECT_EQ(retained_replay.value(), retained.value());

  const auto preparation_replay = adapter.EnsureRunning(*successor.query);
  EXPECT_EQ(preparation_replay.process_incarnation(),
            started.process_incarnation());
  const auto rebuilt_replay = projection.RebuildProjection(rebuild);
  ASSERT_TRUE(rebuilt_replay.presence_valid());
  EXPECT_EQ(rebuilt_replay.result, core::ProjectionCommitResultStateDTO::kCommitted);
  EXPECT_EQ(rebuilt_replay.native_projection_generation,
            health.native_projection_generation);

  const core::ProjectionLeaseId candidate_lease("native-adapter.candidate-lease");
  const auto candidate_retained =
      projection.RetainProjectionBinding(candidate_revision, candidate_lease);
  ASSERT_TRUE(candidate_retained.ok()) << candidate_retained.error().message;
  EXPECT_EQ(candidate_retained.value(), (core::ProjectionLeaseBindingDTO{
                                           candidate_revision, candidate_lease}));
  const auto mismatched = projection.ResolveSceneFrame(
      {frame_query.selector, {candidate_revision, old_lease}});
  ASSERT_FALSE(mismatched.ok());
  EXPECT_EQ(mismatched.error().details.at("reason"),
            "native_scene_frame_lease_unavailable");

  // A further preparation cannot allocate a third projection while the exact
  // predecessor remains leased or owned by the active composition.
  try {
    static_cast<void>(adapter.EnsureRunning(*fixture.query));
    ADD_FAILURE() << "third projection preparation was admitted";
  } catch (const component_manager::RetainedNativeRuntimeUnitAdapterError &error) {
    EXPECT_NE(std::string(error.what()).find("predecessor is still retained"),
              std::string::npos) << error.what();
  }
  EXPECT_EQ(adapter.ObserveProcess(*successor.query).process_incarnation(),
            started.process_incarnation());
  projection.ReleaseProjection(old_lease);
  const auto released = projection.ResolveSceneFrame(frame_query);
  ASSERT_FALSE(released.ok());
  EXPECT_EQ(released.error().details.at("reason"),
            "native_scene_frame_lease_unavailable");
  EXPECT_TRUE(projection.ProjectionHealth().healthy_for_target(candidate_revision));
  const auto candidate_lease_replay =
      projection.RetainProjectionBinding(candidate_revision, candidate_lease);
  ASSERT_TRUE(candidate_lease_replay.ok()) << candidate_lease_replay.error().message;
  EXPECT_EQ(candidate_lease_replay.value(), candidate_retained.value());
  projection.ReleaseProjection(candidate_lease);
  EXPECT_EQ(adapter.EnsureStopped(*successor.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     FailedSameBindingCandidateHealthNeverReplacesPublishedModel) {
  ScopedTemporaryRoot merged;
  const auto fixture_root = merged.path() / "fixture";
  std::filesystem::copy(MANYFORGE_I6_NATIVE_FIXTURE_ROOT, fixture_root,
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
  EXPECT_EQ(failed_health.native_runtime_binding, binding);
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
      const component_manager::RetainedNativeRuntimeUnitAdapterError &error) {
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
TEST(RetainedNativeRuntimeUnitAdapter,
     TimedOutExchangeForgetsItsReapedChildSoConvergenceRelaunches) {
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
  // The held synchronous bootstrap never answers within the adapter's bounded
  // response budget. The adapter stops and reaps that exact child itself.
  try {
    static_cast<void>(resolved.value()->RebuildBootstrapProjection(bootstrap));
    ADD_FAILURE() << "held bootstrap rebuild returned a response";
  } catch (
      const component_manager::RetainedNativeRuntimeUnitAdapterError &error) {
    EXPECT_NE(std::string(error.what()).find("response timed out"),
              std::string::npos)
        << error.what();
  }
  EXPECT_TRUE(std::filesystem::exists(bootstrap_hold.Entered()));
  ASSERT_TRUE(bootstrap_hold.Release());

  // A reaped PID is no longer the adapter's child and may be reused by the
  // system. Observation must report the stopped epoch absent instead of
  // waiting on that PID again, so ordinary convergence starts a fresh epoch.
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_FALSE(adapter.ResolveProjection(binding).ok());
  EXPECT_THROW(static_cast<void>(resolved.value()->ProjectionHealth()),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  const auto relaunched = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(relaunched.state(),
            component_manager::ProcessPresenceState::kPresent);
  EXPECT_NE(relaunched.process_incarnation(), started.process_incarnation());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
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
           core::ApplyId("native-adapter-successor"), core::ApplyGeneration(1U)},
       successor_projection, successor_projection.retained_bundle_id,
       successor_projection.graph_revision, predecessor.publication_generation,
       core::ContentDigest(std::string(kIntentDigest))});
  store.Close();
  ASSERT_GT(successor.publication_generation,
            predecessor.publication_generation);

  const auto &policy = deployment::execution_runtime_contract::manager_liveness_policy();
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

  const auto converged = coordinator.Initialize();
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

[[nodiscard]] component_manager::ManagerSelectedComponentBringUpRequestDTO
SelectedAdapterRequest(const NativeFixture &fixture,
                       const core::RuntimeEpoch &epoch,
                       const std::string &fixture_response) {
  const core::ExpectedActiveControlConfigurationDTO absent_configuration{
      core::ExpectedActiveControlConfigurationPresenceDTO::kAbsent,
      std::nullopt, std::nullopt};
  const core::ExpectedControlDomainRevisionDTO domain{
      core::ControlDomainId("selected.domain"),
      core::ControlDomainRevision("domain.1"),
      core::ControllerTopologyId("topology.1"),
      core::TopologyRevision("topology.1"),
      absent_configuration,
      {{core::ControlBindingId("binding.1"),
        core::ControlBindingRevision("binding.1"),
        core::ControlRouteId("route.1")}}};
  return {{{core::RequestId("selected.request"),
            core::RequestOwnerEpoch("original.cclc.1"), "adapter-test",
            std::nullopt},
           core::ControllableComponentId("selected.component"),
           core::ComponentLifecycleIntentKindDTO::kEnable,
           fixture.query->candidate_graph_revision,
           core::ComponentLifecycleGeneration(1U),
           {domain},
           fixture_response,
           {}},
          {fixture.bundle->retained_bundle_id(),
           fixture.query->candidate_graph_revision,
           {core::SceneId(std::string(kIntentDigest)), 1U,
            core::ContentDigest(std::string(kIntentDigest))}},
          core::ContentDigest(std::string(kIntentDigest)),
          1024U,
          fixture.query->plan.runtime_unit_id,
          epoch,
          core::RuntimeEpoch("original.manager.1")};
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
        if (receiver == set.required_receivers.end() ||
            receiver->runtime_epoch != epoch)
          throw std::runtime_error("shutdown probe renewal identity mismatch");
        const core::ManagerLivenessStatusDTO status{
            set.cell_id,
            receiver->runtime_unit_id,
            epoch,
            set.manager_runtime_unit_id,
            set.manager_runtime_epoch,
            set.manager_owner_epoch,
            receiver->runtime_policy_id,
            receiver->runtime_policy_revision,
            request.heartbeat.heartbeat_sequence,
            request.heartbeat.heartbeat_sequence,
            0,
            core::ManagerLivenessStateDTO::kCurrent,
            false,
            std::nullopt};
        response = rpc::EncodeManagerLivenessRpcResponse(
            core::ManagerLivenessObservationDTO{
                status, set.active_bundle_id, set.required_runtime_set_digest,
                core::ComputeManagerLivenessStatusDigest(status)});
      } else if (rpc::IsNativeShutdownRpcPacket(packet)) {
        ++status_requests;
        if (rpc::DecodeNativeShutdownRpcRequest(packet) != epoch.value())
          throw std::runtime_error("shutdown packet probe epoch mismatch");
        if (mode == "silent")
          continue;
        auto observed = progress;
        if (mode == "foreign")
          observed.runtime_epoch = "foreign.host.epoch";
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
  return 0;
}

// This is only a packet peer: production admission/codec values are reused,
// but no planner, CCLC, robot, engine or effect owner is implemented here.
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
      component_manager::VerifyNativeExecutionRuntimeUnitForHost(
          *bundle, core::RuntimeUnitId(arguments.at("--runtime-unit-id")),
          artifacts, contract::kOperatingSystem,
          contract::kLaunchEnvironmentContract);
  if (!admission)
    throw std::runtime_error("managed packet fixture admission missing");
  const core::RuntimeEpoch epoch(arguments.at("--runtime-epoch"));
  const auto run_root = std::filesystem::path(
      arguments.at("--manyforge-managed-execution-run-root"));
  if (std::filesystem::exists(run_root / "shutdown-probe.mode")) {
    return RunShutdownPacketProbe(
        channel,
        std::stoi(arguments.at("--manyforge-managed-liveness-channel-fd")),
        run_root, epoch, admission->expected_ready_packet(epoch));
  }
  std::uint64_t sequence = 0U;
  bool delayed_process_read_consumed = false;
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
      response = admission->expected_ready_packet(epoch);
    } else if (packet == "status") {
      response = admission->expected_status_packet(epoch, ++sequence);
    } else if (!rpc::IsSelectedRuntimeCompositionRpcPacket(packet)) {
      const auto request = rpc::DecodeNativeProjectionRpcRequest(packet);
      const auto *prepare =
          std::get_if<rpc::NativeProjectionPrepareRequest>(&request);
      if (!prepare)
        throw std::invalid_argument(
            "managed packet fixture only acknowledges Prepare");
      response = rpc::EncodeNativeProjectionRpcResponse(
          rpc::NativeProjectionPrepareResult{*prepare});
    } else {
      const auto request =
          rpc::DecodeSelectedRuntimeCompositionRpcRequestVariant(packet);
      const auto *process_stop =
          std::get_if<component_manager::ManagerComponentProcessStopRequestDTO>(
              &request);
      const auto *process_read = std::get_if<
          component_manager::ManagerComponentProcessStopReadRequestDTO>(
          &request);
      if (process_stop || process_read) {
        const auto &original =
            process_stop ? *process_stop : process_read->request;
        std::ofstream observed(run_root / "selected-test-last-request.bin",
                               std::ios::binary);
        observed.write(packet.data(),
                       static_cast<std::streamsize>(packet.size()));
        observed.close();
        std::ofstream calls(run_root / "process-stop.requests", std::ios::app);
        calls << (process_stop ? "stop\n" : "read\n");
        calls.close();
        if (!observed || !calls)
          throw std::runtime_error("process Stop peer capture failed");
        const bool delayed_consuming_read =
            process_read && original.launch_identity == "stop.read-delay" &&
            !delayed_process_read_consumed;
        // Model a host cache READ that copies and erases its result before
        // sending. This packet fixture proves correlation, not physical drain.
        if (delayed_consuming_read) delayed_process_read_consumed = true;
        if ((process_stop && original.launch_identity == "stop.delay") ||
            delayed_consuming_read) {
          const auto deadline =
              std::chrono::steady_clock::now() + std::chrono::seconds(5);
          while (!std::filesystem::exists(run_root / "process-stop.release")) {
            if (std::chrono::steady_clock::now() >= deadline)
              throw std::runtime_error("process Stop test release missing");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
          }
        }
        auto echo = original;
        if (echo.launch_identity == "stop.foreign")
          ++echo.leader_pid;
        if (delayed_consuming_read) {
          response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
              component_manager::ManagerComponentProcessStopResultDTO{
                  echo, core::ControllerLaunchCleanupState::kDrained,
                  core::ControllerLaunchGroupDrainReceipt{
                      echo.leader_pid, echo.controllable_component_id.value(),
                      echo.launch_identity, "2026-10-13T00:00:00Z"},
                  std::nullopt});
        } else {
          response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
              component_manager::ManagerComponentProcessStopResultDTO{
                  echo, core::ControllerLaunchCleanupState::kUnknown,
                  std::nullopt,
                  core::ErrorDTO{"packet_peer_unknown",
                                 "fixture has no physical drain proof",
                                 {}}});
        }
        if (::send(channel, response.data(), response.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(response.size()))
          throw std::runtime_error("process Stop peer response was not atomic");
        std::ofstream(run_root / "process-stop.replied") << "reply\n";
        if (delayed_consuming_read)
          std::ofstream(run_root / "process-stop.read-replied") << "reply\n";
        continue;
      }
      if (const auto *inspection = std::get_if<
              component_manager::ManagerComponentBringUpInspectionRequestDTO>(
              &request)) {
        auto echo = *inspection;
        if (echo.manager_runtime_epoch.value() == "inspection.delay")
          std::this_thread::sleep_for(std::chrono::milliseconds(11000));
        if (echo.manager_runtime_epoch.value() == "inspection.foreign")
          echo.manager_runtime_epoch = core::RuntimeEpoch("inspection.foreign-response");
        if (echo.manager_runtime_epoch.value() == "inspection.failure")
          response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
              rpc::SelectedRuntimeCompositionRpcFailure{{"inspection_unavailable", "fixture observation unavailable", {}}});
        else
          response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
              component_manager::ManagerComponentBringUpInspectionResultDTO{
                  echo, {{core::ControllableComponentId("component.absent"), component_manager::ProcessPresenceState::kAbsent},
                         {core::ControllableComponentId("component.present"), component_manager::ProcessPresenceState::kPresent},
                         {core::ControllableComponentId("component.unknown"), component_manager::ProcessPresenceState::kUnknown}}});
        if (::send(channel, response.data(), response.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(response.size()))
          throw std::runtime_error("inspection packet response was not atomic");
        continue;
      }
      const auto *apply_submit = std::get_if<
          component_manager::ManagerApplyComponentBringUpRequestDTO>(&request);
      const auto *apply_read = std::get_if<
          component_manager::ManagerApplyComponentBringUpReadRequestDTO>(&request);
      if (apply_submit || apply_read) {
        std::ofstream observed(run_root / "selected-test-last-request.bin", std::ios::binary);
        observed.write(packet.data(), static_cast<std::streamsize>(packet.size()));
        observed.close();
        if (!observed) throw std::runtime_error("delegated packet observation failed");
        auto echo = apply_submit ? *apply_submit : apply_read->request;
        const auto marker = echo.intent.reason.value_or("");
        if (marker == "close") return 0;
        if (marker == "rpc-failure") {
          response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
              rpc::SelectedRuntimeCompositionRpcFailure{{"packet_peer_failure", "response unavailable", {}}});
        } else {
          if (marker == "foreign-parent-receipt")
            echo.parent_authority.parent_claim.claim_generation = core::CellAdmissionClaimGeneration(999U);
          if (marker == "foreign-attribution")
            echo.intent.attribution = {core::RequestTransportDTO::kHttp,
                core::RequestAssuranceDTO::kExternallyAuthenticated, std::string("foreign.principal")};
          std::optional<core::CCLCLifecycleRequestResultDTO> result;
          if (marker != "unknown")
            result = core::CCLCLifecycleRequestResultDTO{
                core::RequestReplayStatusDTO::kRequestIdConflict, std::nullopt, std::nullopt};
          response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
              component_manager::ManagerApplyComponentBringUpResultDTO{echo, result});
        }
        if (::send(channel, response.data(), response.size(), MSG_NOSIGNAL) !=
            static_cast<ssize_t>(response.size()))
          throw std::runtime_error("delegated packet response was not atomic");
        continue;
      }
      const auto *submit = std::get_if<
          component_manager::ManagerSelectedComponentBringUpRequestDTO>(
          &request);
      const auto *read = std::get_if<
          component_manager::ManagerSelectedComponentBringUpReadRequestDTO>(
          &request);
      if (!submit && !read)
        throw std::invalid_argument(
            "managed packet fixture expected selected request");
      std::ofstream observed(run_root / "selected-test-last-request.bin",
                             std::ios::binary);
      observed.write(packet.data(),
                     static_cast<std::streamsize>(packet.size()));
      observed.close();
      if (!observed)
        throw std::runtime_error("managed packet fixture observation failed");
      auto echo = submit ? *submit : read->request;
      const auto marker = echo.intent.reason.value_or("");
      if (marker == "close")
        return 0; // Sent request, lost response: outcome unknown.
      if (marker == "rpc-failure") {
        response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
            rpc::SelectedRuntimeCompositionRpcFailure{
                {"packet_peer_failure",
                 "response unavailable",
                 {{"reason", "fixture"}}}});
      } else if (marker == "malformed") {
        response = "malformed selected response";
      } else if (marker == "wrong-kind") {
        const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
            ordinary{core::ContentDigest(std::string(kIntentDigest)),
                     core::ApplyId("fixture.apply"),
                     core::ApplyGeneration(1U),
                     echo.candidate_projection_revision,
                     core::ActiveBundlePublicationGeneration(1U),
                     core::ContentDigest(std::string(kIntentDigest)),
                     echo.candidate_retained_bundle_digest,
                     echo.candidate_retained_bundle_bytes,
                     echo.runtime_unit_id,
                     echo.runtime_epoch,
                     echo.manager_runtime_epoch,
                     core::ConfigurationFenceId("fixture.fence"),
                     core::ConfigurationFenceGeneration(1U),
                     {},
                     component_manager::ManagerSelectedRuntimeCompositionPhase::
                         kPrepare,
                     {},
                     echo.candidate_projection_revision};
        response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
            component_manager::ManagerSelectedRuntimeCompositionResultDTO{
                ordinary,
                component_manager::ManagerSelectedRuntimeCompositionState::
                    kPreparing,
                {}});
      } else {
        if (marker == "foreign-unit")
          echo.runtime_unit_id = core::RuntimeUnitId("foreign.unit");
        if (marker == "foreign-host")
          echo.runtime_epoch = core::RuntimeEpoch("foreign.host");
        if (marker == "foreign-manager")
          echo.manager_runtime_epoch = core::RuntimeEpoch("foreign.manager");
        if (marker == "foreign-intent")
          echo.intent.request_context.request_id =
              core::RequestId("foreign.request");
        if (marker == "foreign-attribution")
          echo.intent.attribution = {
              core::RequestTransportDTO::kHttp,
              core::RequestAssuranceDTO::kExternallyAuthenticated,
              std::string("foreign.principal")};
        if (marker == "foreign-candidate")
          ++echo.candidate_retained_bundle_bytes;
        std::optional<core::CCLCLifecycleRequestResultDTO> result;
        if (marker != "unknown") {
          result = core::CCLCLifecycleRequestResultDTO{
              core::RequestReplayStatusDTO::kRequestIdConflict, std::nullopt,
              std::nullopt};
          if (marker == "rejection") {
            result = core::CCLCLifecycleRequestResultDTO{
                core::RequestReplayStatusDTO::kReplayedExact, std::nullopt,
                std::nullopt,
                core::ErrorDTO{"selected_refused",
                               "no admission",
                               {{"effect", "not_started"}}}};
          }
        }
        response = rpc::EncodeSelectedRuntimeCompositionRpcResponse(
            component_manager::ManagerSelectedComponentBringUpResultDTO{
                echo, result});
      }
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
      std::filesystem::path(MANYFORGE_NATIVE_EXECUTION_TARGET_ROOT) /
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
      core::RuntimeEpoch("manager.shutdown.epoch"),
      core::RequestOwnerEpoch("manager.shutdown.owner"),
      receivers};
  const core::ManagerHeartbeatDTO heartbeat{
      required_set.cell_id,
      required_set.manager_runtime_unit_id,
      required_set.manager_runtime_epoch,
      required_set.manager_owner_epoch,
      1U,
      policy.runtime_policy_id,
      policy.runtime_policy_revision};
  const component_manager::ManagerLivenessTransportRequestDTO renewal{
      fixture.query->plan.runtime_unit_id,
      core::ActiveBundlePublicationGeneration(1U),
      fixture.bundle->value().cell_graph.graph_revision,
      core::ContentDigest(std::string(kIntentDigest)),
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
TEST(RetainedNativeRuntimeUnitAdapter,
     ShutdownWaitsForActualExitWithUnchangedProgress) {
  CheckShutdownPacketProbe("responsive", false);
}
TEST(RetainedNativeRuntimeUnitAdapter,
     DelayedShutdownStatusNeverRequestsSigterm) {
  CheckShutdownPacketProbe("silent", false);
}
TEST(RetainedNativeRuntimeUnitAdapter,
     ForeignShutdownProgressNeverRequestsSigterm) {
  CheckShutdownPacketProbe("foreign", false);
}
TEST(RetainedNativeRuntimeUnitAdapter,
     LateShutdownStatusCannotPoisonConcurrentRenewal) {
  CheckShutdownPacketProbe("late", false);
}
TEST(RetainedNativeRuntimeUnitAdapter,
     InitialShutdownSendFailureRequestsSigtermOnlyOnce) {
  CheckShutdownPacketProbe("responsive", true);
}

[[nodiscard]] component_manager::detail::SelectedRuntimeCompositionRpcRequest
ReadObservedSelectedRequest(const NativeFixture &fixture) {
  return component_manager::detail::
      DecodeSelectedRuntimeCompositionRpcRequestVariant(
          ReadFile(fixture.execution_run_root.path() /
                   "selected-test-last-request.bin"));
}

[[nodiscard]] component_manager::ManagerApplyComponentBringUpRequestDTO
ApplyAdapterRequest(const NativeFixture &fixture, const core::RuntimeEpoch &epoch,
                    const std::string &marker) {
  auto common = SelectedAdapterRequest(fixture, epoch, marker);
  const core::ApplyFenceOwnerDTO owner{core::ApplyId("parent.apply"), core::ApplyGeneration(8U)};
  const core::ApplyComponentBringUpReferenceDTO parent{
      owner, core::ContentDigest(std::string(kIntentDigest))};
  const core::CellAdmissionClaimIntentDTO claim{
      core::CellAdmissionClaimKindDTO::kManagerApply, core::CellAdmissionOwnerRoleDTO::kManager,
      core::RequestOwnerEpoch("parent.manager"), owner.apply_id.value(), core::RequestId("parent.request"),
      parent.plan_digest, core::AdmissionGeneration(9U), {}, std::nullopt, std::nullopt, std::nullopt};
  const core::ConfigurationFenceScopeDTO scope{};
  return {common.intent, common.candidate_projection_revision,
          common.candidate_retained_bundle_digest, common.candidate_retained_bundle_bytes,
          common.runtime_unit_id, common.runtime_epoch, common.manager_runtime_epoch,
          {parent, {claim, core::CellAdmissionClaimGeneration(17U), core::AdmissionGeneration(9U), 1000},
           {owner, core::ConfigurationFenceId("parent.fence"), core::ConfigurationFenceGeneration(22U),
            scope, common.manager_runtime_epoch}}};
}

component_manager::ManagerComponentBringUpInspectionRequestDTO InspectionAdapterRequest(
    const NativeFixture &fixture, const core::RuntimeEpoch &epoch,
    const std::string &manager_epoch) {
  const auto selected = SelectedAdapterRequest(fixture, epoch, "");
  return {selected.candidate_projection_revision, selected.candidate_retained_bundle_digest,
          selected.candidate_retained_bundle_bytes, selected.candidate_projection_revision,
          core::ActiveBundlePublicationGeneration(1U), core::ContentDigest(std::string(kIntentDigest)),
          selected.runtime_unit_id, epoch, core::RuntimeEpoch(manager_epoch)};
}

component_manager::ManagerComponentProcessStopRequestDTO
ProcessStopAdapterRequest(const NativeFixture &fixture,
                          const core::RuntimeEpoch &epoch,
                          const std::string &launch) {
  const auto selected = SelectedAdapterRequest(fixture, epoch, "");
  return {{core::RequestId("process.stop.1"),
           core::RequestOwnerEpoch("original.cclc.1"), "adapter-test",
           std::nullopt},
          selected.candidate_projection_revision,
          core::ActiveBundlePublicationGeneration(1U),
          core::ContentDigest(std::string(kIntentDigest)),
          selected.runtime_unit_id,
          epoch,
          selected.manager_runtime_epoch,
          core::ControllableComponentId("selected.component"),
          launch,
          1234};
}

TEST(RetainedNativeRuntimeUnitAdapter,
     ProcessStopReadPreservesExactUnknownWithoutResubmission) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const auto request = ProcessStopAdapterRequest(
      fixture, core::RuntimeEpoch(started.process_incarnation()),
      "stop.current");
  ASSERT_TRUE(request.valid());
  const auto submitted = adapter.StopComponentProcess(request);
  ASSERT_TRUE(submitted.ok());
  EXPECT_EQ(submitted.value().request, request);
  EXPECT_EQ(submitted.value().state,
            core::ControllerLaunchCleanupState::kUnknown);
  EXPECT_FALSE(submitted.value().receipt);
  const auto read = adapter.ReadComponentProcessStop({request});
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(read.value().request, request);
  ASSERT_TRUE(read.value().error);
  EXPECT_EQ(read.value().error->code, "packet_peer_unknown");
  EXPECT_FALSE(read.value().receipt);
  EXPECT_EQ(
      ReadFile(fixture.execution_run_root.path() / "process-stop.requests"),
      "stop\nread\n");
  EXPECT_EQ(
      std::get<component_manager::ManagerComponentProcessStopReadRequestDTO>(
          ReadObservedSelectedRequest(fixture))
          .request,
      request);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).process_incarnation(),
            started.process_incarnation());
}

TEST(RetainedNativeRuntimeUnitAdapter,
     ProcessStopForeignEchoCannotQualifyOrResubmit) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const auto request = ProcessStopAdapterRequest(
      fixture, core::RuntimeEpoch(started.process_incarnation()),
      "stop.foreign");
  EXPECT_THROW(static_cast<void>(adapter.StopComponentProcess(request)),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  const auto read = adapter.ReadComponentProcessStop({request});
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(read.value().state, core::ControllerLaunchCleanupState::kUnknown);
  EXPECT_FALSE(read.value().receipt);
  EXPECT_EQ(
      ReadFile(fixture.execution_run_root.path() / "process-stop.requests"),
      "stop\n");
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).process_incarnation(),
            started.process_incarnation());
}

TEST(RetainedNativeRuntimeUnitAdapter,
     ProcessStopTimeoutRetainsHostAndReadsOnlyExactLateReply) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  struct ReplyRelease final {
    std::filesystem::path root;
    ~ReplyRelease() {
      try {
        std::ofstream(root / "process-stop.release") << "release\n";
      } catch (...) {
      }
    }
  } release{fixture.execution_run_root.path()};
  const auto request = ProcessStopAdapterRequest(
      fixture, core::RuntimeEpoch(started.process_incarnation()), "stop.delay");
  const auto submitted = adapter.StopComponentProcess(request);
  ASSERT_TRUE(submitted.ok());
  EXPECT_EQ(submitted.value().state,
            core::ControllerLaunchCleanupState::kUnknown);
  EXPECT_FALSE(submitted.value().receipt);
  const auto pending = adapter.ReadComponentProcessStop({request});
  ASSERT_TRUE(pending.ok());
  ASSERT_TRUE(pending.value().error);
  EXPECT_EQ(pending.value().error->code, "component_process_stop_pending");
  EXPECT_EQ(ReadFile(release.root / "process-stop.requests"), "stop\n");
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).process_incarnation(),
            started.process_incarnation());
  std::ofstream(release.root / "process-stop.release") << "release\n";
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!std::filesystem::exists(release.root / "process-stop.replied") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(std::filesystem::exists(release.root / "process-stop.replied"));
  const auto read = adapter.ReadComponentProcessStop({request});
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(read.value().request, request);
  ASSERT_TRUE(read.value().error);
  EXPECT_EQ(read.value().error->code, "packet_peer_unknown");
  EXPECT_FALSE(read.value().receipt);
  EXPECT_EQ(ReadFile(release.root / "process-stop.requests"), "stop\nread\n");
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).process_incarnation(),
            started.process_incarnation());
}

// Scope: real private-channel late READ delivery and exact association only.
// The peer's receipt is authored transport data, not a physical drain proof.
TEST(RetainedNativeRuntimeUnitAdapter,
     ProcessStopLateConsumingReadReturnsExactReceiptOnce) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  struct ReplyRelease final {
    std::filesystem::path root;
    ~ReplyRelease() {
      try { std::ofstream(root / "process-stop.release") << "release\n"; }
      catch (...) {}
    }
  } release{fixture.execution_run_root.path()};
  const auto request = ProcessStopAdapterRequest(
      fixture, core::RuntimeEpoch(started.process_incarnation()), "stop.read-delay");
  ASSERT_TRUE(adapter.StopComponentProcess(request).ok());
  const auto timed_out = adapter.ReadComponentProcessStop({request});
  ASSERT_TRUE(timed_out.ok());
  EXPECT_EQ(timed_out.value().state, core::ControllerLaunchCleanupState::kUnknown);
  EXPECT_FALSE(timed_out.value().receipt);
  ASSERT_TRUE(timed_out.value().error);
  EXPECT_EQ(timed_out.value().error->code, "component_process_stop_response_unavailable");
  std::ofstream(release.root / "process-stop.release") << "release\n";
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!std::filesystem::exists(release.root / "process-stop.read-replied") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(std::filesystem::exists(release.root / "process-stop.read-replied"));
  // An unrelated status observation settles the packet without consuming
  // the exact READ association's value.
  EXPECT_TRUE(adapter.ObserveProgress(
      *fixture.query, core::RuntimeEpoch(started.process_incarnation())).has_value());
  auto foreign = request;
  foreign.request_context.request_id = core::RequestId("process.stop.foreign");
  const auto foreign_read = adapter.ReadComponentProcessStop({foreign});
  ASSERT_TRUE(foreign_read.ok());
  EXPECT_EQ(foreign_read.value().request, foreign);
  EXPECT_FALSE(foreign_read.value().receipt);
  EXPECT_EQ(ReadFile(release.root / "process-stop.requests"), "stop\nread\n");
  const auto duplicate = adapter.StopComponentProcess(request);
  ASSERT_TRUE(duplicate.ok());
  EXPECT_EQ(duplicate.value().request, request);
  EXPECT_EQ(duplicate.value().state, core::ControllerLaunchCleanupState::kDrained);
  ASSERT_TRUE(duplicate.value().receipt);
  EXPECT_EQ(ReadFile(release.root / "process-stop.requests"), "stop\nread\n");
  const auto exact = adapter.ReadComponentProcessStop({request});
  ASSERT_TRUE(exact.ok());
  EXPECT_EQ(exact.value().request, request);
  EXPECT_EQ(exact.value().state, core::ControllerLaunchCleanupState::kDrained);
  ASSERT_TRUE(exact.value().receipt);
  EXPECT_EQ(*exact.value().receipt,
            (core::ControllerLaunchGroupDrainReceipt{
                request.leader_pid, request.controllable_component_id.value(),
                request.launch_identity, "2026-10-13T00:00:00Z"}));
  EXPECT_FALSE(exact.value().error);
  EXPECT_EQ(ReadFile(release.root / "process-stop.requests"), "stop\nread\n");
  const auto consumed = adapter.ReadComponentProcessStop({request});
  ASSERT_TRUE(consumed.ok());
  EXPECT_EQ(consumed.value().state, core::ControllerLaunchCleanupState::kUnknown);
  EXPECT_FALSE(consumed.value().receipt);
  EXPECT_EQ(ReadFile(release.root / "process-stop.requests"), "stop\nread\nread\n");
}

TEST(RetainedNativeRuntimeUnitAdapter,
     ProcessStopNewExplicitRequestCannotReceiveOldLateReadReceipt) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  struct ReplyRelease final {
    std::filesystem::path root;
    ~ReplyRelease() {
      try { std::ofstream(root / "process-stop.release") << "release\n"; }
      catch (...) {}
    }
  } release{fixture.execution_run_root.path()};
  const auto original = ProcessStopAdapterRequest(
      fixture, core::RuntimeEpoch(started.process_incarnation()), "stop.read-delay");
  ASSERT_TRUE(adapter.StopComponentProcess(original).ok());
  const auto timed_out = adapter.ReadComponentProcessStop({original});
  ASSERT_TRUE(timed_out.ok());
  ASSERT_TRUE(timed_out.value().error);
  EXPECT_EQ(timed_out.value().error->code, "component_process_stop_response_unavailable");
  std::ofstream(release.root / "process-stop.release") << "release\n";
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!std::filesystem::exists(release.root / "process-stop.read-replied") &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  ASSERT_TRUE(std::filesystem::exists(release.root / "process-stop.read-replied"));
  auto successor = original;
  successor.request_context.request_id = core::RequestId("process.stop.successor");
  successor.launch_identity = "stop.current";
  const auto submitted = adapter.StopComponentProcess(successor);
  ASSERT_TRUE(submitted.ok());
  EXPECT_EQ(submitted.value().request, successor);
  EXPECT_FALSE(submitted.value().receipt);
  const auto current = adapter.ReadComponentProcessStop({successor});
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().request, successor);
  EXPECT_FALSE(current.value().receipt);
  const auto stale = adapter.ReadComponentProcessStop({original});
  ASSERT_TRUE(stale.ok());
  EXPECT_EQ(stale.value().state, core::ControllerLaunchCleanupState::kUnknown);
  EXPECT_FALSE(stale.value().receipt);
  EXPECT_EQ(ReadFile(release.root / "process-stop.requests"),
            "stop\nread\nstop\nread\nread\n");
}

TEST(RetainedNativeRuntimeUnitAdapter, ComponentInspectionPreservesExactOccurrenceAndUnavailableFacts) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const auto request = InspectionAdapterRequest(fixture, epoch, "inspection.current");
  const auto inspected = adapter.InspectComponentBringUp(request);
  ASSERT_TRUE(inspected.ok());
  EXPECT_EQ(inspected.value().request, request);
  ASSERT_EQ(inspected.value().configured_components.size(), 3U);
  EXPECT_EQ(inspected.value().configured_components[0].process_presence, component_manager::ProcessPresenceState::kAbsent);
  EXPECT_EQ(inspected.value().configured_components[1].process_presence, component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(inspected.value().configured_components[2].process_presence, component_manager::ProcessPresenceState::kUnknown);
  const auto refused = adapter.InspectComponentBringUp(InspectionAdapterRequest(fixture, epoch, "inspection.failure"));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "inspection_unavailable");
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).process_incarnation(), epoch.value());
  EXPECT_TRUE(adapter.ObserveProgress(*fixture.query, epoch).has_value());
  EXPECT_THROW((void)adapter.InspectComponentBringUp(InspectionAdapterRequest(fixture, epoch, "inspection.foreign")),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(), component_manager::ProcessPresenceState::kPresent);
  // A consumed foreign echo cannot restore correlation, or permit an effect.
  EXPECT_FALSE(adapter.ObserveProgress(*fixture.query, epoch));
  EXPECT_FALSE(adapter.SubmitApplyComponentBringUp(ApplyAdapterRequest(fixture, epoch, "present")).ok());
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(), component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter, ComponentInspectionTimeoutKeepsHostAndSettlesOnlyExactLateReply) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const auto timed_out = adapter.InspectComponentBringUp(InspectionAdapterRequest(fixture, epoch, "inspection.delay"));
  ASSERT_FALSE(timed_out.ok());
  EXPECT_EQ(timed_out.error().code, "component_bring_up_inspection_timed_out");
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(), component_manager::ProcessPresenceState::kPresent);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).process_incarnation(), epoch.value());
  EXPECT_FALSE(adapter.ObserveProgress(*fixture.query, epoch));
  EXPECT_FALSE(adapter.SubmitApplyComponentBringUp(ApplyAdapterRequest(fixture, epoch, "present")).ok());
  const auto blocked = adapter.InspectComponentBringUp(InspectionAdapterRequest(fixture, epoch, "inspection.current"));
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().code, "component_bring_up_inspection_pending");
  bool settled = false;
  for (int attempt = 0; attempt < 200 && !settled; ++attempt) {
    settled = adapter.ObserveProgress(*fixture.query, epoch).has_value();
    if (!settled) std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(settled);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).process_incarnation(), epoch.value());
  EXPECT_TRUE(adapter.InspectComponentBringUp(InspectionAdapterRequest(fixture, epoch, "inspection.current")).ok());
}

TEST(RetainedNativeRuntimeUnitAdapter, ApplyDelegationPreservesOriginalRequestOnReplacementChild) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto first = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(first.state(), component_manager::ProcessPresenceState::kPresent);
  const auto original = ApplyAdapterRequest(fixture, core::RuntimeEpoch(first.process_incarnation()), "present");
  ASSERT_TRUE(original.valid());
  ASSERT_TRUE(adapter.SubmitApplyComponentBringUp(original).ok());
  ASSERT_EQ(adapter.EnsureStopped(*fixture.query).state(), component_manager::ProcessPresenceState::kAbsent);
  const auto replacement = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(replacement.state(), component_manager::ProcessPresenceState::kPresent);
  ASSERT_NE(replacement.process_incarnation(), first.process_incarnation());
  for (const bool read : {false, true}) {
    const auto result = read ? adapter.ReadApplyComponentBringUp({original})
                             : adapter.SubmitApplyComponentBringUp(original);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().request, original);
    ASSERT_TRUE(result.value().result.has_value());
    const auto received = ReadObservedSelectedRequest(fixture);
    if (read)
      EXPECT_EQ(std::get<component_manager::ManagerApplyComponentBringUpReadRequestDTO>(received).request, original);
    else
      EXPECT_EQ(std::get<component_manager::ManagerApplyComponentBringUpRequestDTO>(received), original);
  }
}

TEST(RetainedNativeRuntimeUnitAdapter, ApplyDelegationRequiresExactEchoAndPresentSubmit) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const auto unknown = ApplyAdapterRequest(fixture, epoch, "unknown");
  const auto read = adapter.ReadApplyComponentBringUp({unknown});
  ASSERT_TRUE(read.ok());
  EXPECT_FALSE(read.value().result);
  EXPECT_THROW((void)adapter.SubmitApplyComponentBringUp(unknown),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  for (const auto &marker : {"foreign-parent-receipt", "foreign-attribution"}) {
    const auto request = ApplyAdapterRequest(fixture, epoch, marker);
    ASSERT_TRUE(request.valid());
    EXPECT_THROW((void)adapter.SubmitApplyComponentBringUp(request),
                 component_manager::RetainedNativeRuntimeUnitAdapterError);
    EXPECT_THROW((void)adapter.ReadApplyComponentBringUp({request}),
                 component_manager::RetainedNativeRuntimeUnitAdapterError);
  }
  const auto failed = adapter.SubmitApplyComponentBringUp(ApplyAdapterRequest(fixture, epoch, "rpc-failure"));
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "packet_peer_failure");
  EXPECT_FALSE(failed.error().details.contains("effect"));
}

TEST(RetainedNativeRuntimeUnitAdapter,
     SelectedPacketRoundTripsSubmitReadAndUnknownWithoutEffects) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const auto request = SelectedAdapterRequest(fixture, epoch, "present");
  ASSERT_TRUE(request.valid());
  const auto submit = adapter.SubmitSelectedComponentBringUp(request);
  ASSERT_TRUE(submit.ok());
  EXPECT_EQ(submit.value().request, request);
  ASSERT_TRUE(submit.value().result.has_value());
  EXPECT_EQ(submit.value().result->replay_status,
            core::RequestReplayStatusDTO::kRequestIdConflict);
  EXPECT_EQ(
      std::get<component_manager::ManagerSelectedComponentBringUpRequestDTO>(
          ReadObservedSelectedRequest(fixture)),
      request);
  const auto read = adapter.ReadSelectedComponentBringUp({request});
  ASSERT_TRUE(read.ok());
  EXPECT_EQ(read.value().request, request);
  ASSERT_TRUE(read.value().result.has_value());
  EXPECT_EQ(
      std::get<
          component_manager::ManagerSelectedComponentBringUpReadRequestDTO>(
          ReadObservedSelectedRequest(fixture))
          .request,
      request);
  const auto unknown_request =
      SelectedAdapterRequest(fixture, epoch, "unknown");
  const auto unknown = adapter.ReadSelectedComponentBringUp({unknown_request});
  ASSERT_TRUE(unknown.ok());
  EXPECT_EQ(unknown.value().request, unknown_request);
  EXPECT_FALSE(unknown.value().result.has_value());
  EXPECT_THROW((void)adapter.SubmitSelectedComponentBringUp(unknown_request),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  const auto rejection_request =
      SelectedAdapterRequest(fixture, epoch, "rejection");
  const auto rejected =
      adapter.SubmitSelectedComponentBringUp(rejection_request);
  ASSERT_TRUE(rejected.ok());
  ASSERT_TRUE(rejected.value().result->rejection.has_value());
  EXPECT_EQ(rejected.value().result->rejection->details.at("effect"),
            "not_started");
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     SelectedObservationPreservesOriginalEpochsAcrossChildReplacement) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto first = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(first.state(), component_manager::ProcessPresenceState::kPresent);
  const auto original = SelectedAdapterRequest(
      fixture, core::RuntimeEpoch(first.process_incarnation()), "present");
  ASSERT_TRUE(adapter.SubmitSelectedComponentBringUp(original).ok());
  ASSERT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  const auto second = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(second.state(), component_manager::ProcessPresenceState::kPresent);
  ASSERT_NE(second.process_incarnation(), first.process_incarnation());
  for (const bool read_only : {false, true}) {
    const auto result = read_only
                            ? adapter.ReadSelectedComponentBringUp({original})
                            : adapter.SubmitSelectedComponentBringUp(original);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().request, original);
    const auto received = ReadObservedSelectedRequest(fixture);
    if (read_only)
      EXPECT_EQ(
          std::get<
              component_manager::ManagerSelectedComponentBringUpReadRequestDTO>(
              received)
              .request,
          original);
    else
      EXPECT_EQ(
          std::get<
              component_manager::ManagerSelectedComponentBringUpRequestDTO>(
              received),
          original);
  }
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     SelectedExchangeRefusesForeignFullEchoAndWrongResultKind) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  for (const auto marker :
       {"foreign-unit", "foreign-host", "foreign-manager", "foreign-intent",
        "foreign-attribution", "foreign-candidate", "wrong-kind"}) {
    SCOPED_TRACE(marker);
    const auto request = SelectedAdapterRequest(fixture, epoch, marker);
    EXPECT_THROW((void)adapter.SubmitSelectedComponentBringUp(request),
                 component_manager::RetainedNativeRuntimeUnitAdapterError);
    EXPECT_THROW((void)adapter.ReadSelectedComponentBringUp({request}),
                 component_manager::RetainedNativeRuntimeUnitAdapterError);
  }
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     SelectedRpcFailureMalformedAndLostReplyAreNotNoAdmissionResults) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  const auto started = adapter.EnsureRunning(*fixture.query);
  ASSERT_EQ(started.state(), component_manager::ProcessPresenceState::kPresent);
  const core::RuntimeEpoch epoch(started.process_incarnation());
  const auto failed = adapter.SubmitSelectedComponentBringUp(
      SelectedAdapterRequest(fixture, epoch, "rpc-failure"));
  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.error().code, "packet_peer_failure");
  EXPECT_FALSE(failed.error().details.contains("effect"));
  const auto read_failed = adapter.ReadSelectedComponentBringUp(
      {SelectedAdapterRequest(fixture, epoch, "rpc-failure")});
  ASSERT_FALSE(read_failed.ok());
  EXPECT_FALSE(read_failed.error().details.contains("effect"));
  EXPECT_THROW((void)adapter.SubmitSelectedComponentBringUp(
                   SelectedAdapterRequest(fixture, epoch, "malformed")),
               std::invalid_argument);
  const auto lost = SelectedAdapterRequest(fixture, epoch, "close");
  EXPECT_THROW((void)adapter.SubmitSelectedComponentBringUp(lost),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_EQ(
      std::get<component_manager::ManagerSelectedComponentBringUpRequestDTO>(
          ReadObservedSelectedRequest(fixture)),
      lost);
  EXPECT_EQ(adapter.EnsureStopped(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
}

TEST(RetainedNativeRuntimeUnitAdapter,
     SelectedInvalidAndAbsentChildRequestsNeverStartAChild) {
  NativeFixture fixture;
  ScopedTemporaryRoot targets;
  static_cast<void>(InstallSelectedPacketTestTargets(targets.path()));
  auto adapter = MakeAdapter(fixture, targets.path());
  auto request = SelectedAdapterRequest(
      fixture, core::RuntimeEpoch("original.host"), "present");
  EXPECT_FALSE(adapter.SubmitSelectedComponentBringUp(request).ok());
  EXPECT_FALSE(adapter.ReadSelectedComponentBringUp({request}).ok());
  request.candidate_retained_bundle_bytes = 0;
  EXPECT_THROW((void)adapter.SubmitSelectedComponentBringUp(request),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_THROW((void)adapter.ReadSelectedComponentBringUp({request}),
               component_manager::RetainedNativeRuntimeUnitAdapterError);
  EXPECT_EQ(adapter.ObserveProcess(*fixture.query).state(),
            component_manager::ProcessPresenceState::kAbsent);
  EXPECT_FALSE(std::filesystem::exists(fixture.execution_run_root.path() /
                                       "selected-test-last-request.bin"));
}

} // namespace

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
