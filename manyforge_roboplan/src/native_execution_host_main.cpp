#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <future>
#include <iostream>
#include <iterator>
#include <limits>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/time/host_boot_identity.hpp>
#include <manyforge_core/filesystem_resolved_stop_base_plan_repository.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#endif
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/resolved_stop_base_plan_resolver.hpp>
#include <manyforge_deployment/retained_stop_deadline_budget.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <manyforge_deployment/verified_controller_lifecycle_expected_domain.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/retained_native_cclc_runtime.hpp>
#include <manyforge_roboplan/retained_native_cell_command_readiness_port.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>
#include <manyforge_roboplan/retained_native_component_owner.hpp>
#include <manyforge_roboplan/retained_native_gripper_runtime.hpp>
#include <manyforge_roboplan/retained_native_gripper_route_publication_owner.hpp>
#include <manyforge_roboplan/retained_native_view_predecessor.hpp>
#include <manyforge_roboplan/retained_native_lifecycle_product.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
#include <manyforge_roboplan/retained_isaac_ros2_control_profile.hpp>
#include <manyforge_roboplan/retained_ros2_control_admission_owner.hpp>
#include <manyforge_roboplan/retained_ros2_control_lifecycle_product.hpp>
#include <manyforge_roboplan/retained_ros2_control_runtime_handle.hpp>
#endif
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <manyforge_roboplan/roboplan_scene_factory.hpp>
#include <manyforge_scene/canonical_empty_scene_owner.hpp>
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <manyforge_scene/scene_transaction_codec.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "cell_admission_command_codec.hpp"
#include "kernel_apply_fence_rpc_internal.hpp"
#include "managed_kernel_apply_fence_owner.hpp"
#include "managed_projection_registry.hpp"
#include "manager_liveness_rpc_internal.hpp"
#include "native_projection_rpc_internal.hpp"
#include "owner_settlement_readers.hpp"
#include "product_backed_runtime_participant_resolver.hpp"
#include "selected_runtime_composition_rpc_internal.hpp"
#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_core/ports/ikernel_cell_admission_port.hpp>

#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
#include "focused_pick_place_planning_fixture.hpp"
#endif

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;
namespace scene = manyforge::scene;

using content_identity::CanonicalValue;

constexpr std::string_view kCommandApiVersion =
    "manyforge.execution_command.v1";
constexpr std::string_view kResponseApiVersion =
    "manyforge.execution_command_response.v1";
constexpr std::string_view kSocketFileName = "execution-command.sock";
constexpr std::string_view kControlSocketFileName = "execution-control.sock";
constexpr std::string_view kObservationSocketFileName =
    "execution-observation.sock";
constexpr std::string_view kManagerSocketFileName = "manager-command.sock";
constexpr std::string_view kManagerCommandApiVersion =
    "manyforge.manager_command.v1";
constexpr std::string_view kManagerResponseApiVersion =
    "manyforge.manager_command_response.v1";
constexpr std::string_view kLockFileName = ".execution-host.lock";
constexpr std::size_t kMaximumPacketBytes = 1024U * 1024U;
constexpr std::size_t kMaximumResponseBytes =
    core::kMaximumCellAdmissionJsonResponseBytes;
constexpr std::size_t kMaximumManagerResponseBytes = 512U * 1024U;
constexpr std::size_t kMaximumIdentityBytes = 2048U;
constexpr std::size_t kMaximumReasonBytes = 4096U;
constexpr std::size_t kMaximumEventPage = 16U;
// Leave ample room for the historian's causal envelope while retaining the
// complete maintained-cell Stop evidence in one operational occurrence.
constexpr std::size_t kMaximumStopHistoryProjectionBytes = 192U * 1024U;
constexpr double kMaximumSafeInteger = 9'007'199'254'740'991.0;
constexpr std::size_t kOwnerEpochRandomBytes = 16U;
constexpr int kManagedPackageRootFd = 6;
constexpr int kManagedProgramArtifactRootFd = 8;
constexpr int kSocketBacklog = 16;
constexpr int kReconcilePeriodMilliseconds = 5;
// Recovery evidence is not a control deadline. Refresh it at 20 Hz on the
// existing noncritical lane, independently of browser requests.
constexpr core::Duration kRecoveryObservationPeriodNs = 50'000'000;
constexpr int kClientPacketDeadlineMilliseconds = 100;
// Requests the ordinary socket handed to one deferred lane and that are not
// yet answered: the one executing plus those queued behind it. Upstream
// admits one preview per cell target; this host-side bound keeps a
// misbehaving client from accumulating sockets behind long work.
constexpr std::size_t kMaximumPendingDeferredCommands = 8U;
constexpr int kManagerSceneDeadlineMilliseconds = 5000;
constexpr std::size_t kPlanningSceneSynchronizationAttempts = 32U;
constexpr auto kPlanningSceneSynchronizationRetryDelay =
    std::chrono::milliseconds(10);

[[nodiscard]] std::string SelectedExecutionOfferDigest() {
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  constexpr auto family =
      deployment::ros2_control_execution_runtime_contract::kImplementationId;
#else
  constexpr auto family =
      deployment::native_execution_runtime_contract::kImplementationId;
#endif
  const auto &offers = deployment::runtime_contract_declaration_registry()
                           .package_manifest_set()
                           .offers;
  const auto found =
      std::find_if(offers.begin(), offers.end(), [&](const auto &offer) {
        return offer.runtime_implementation_id.value() == family;
      });
  if (found == offers.end()) {
    throw std::runtime_error(
        "selected execution host has no qualified runtime offer");
  }
  return found->offer_digest.value();
}

[[nodiscard]] constexpr std::string_view SelectedExecutionRuntimeRole() {
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  return deployment::ros2_control_execution_runtime_contract::kRuntimeRoleId;
#else
  return deployment::native_execution_runtime_contract::kRuntimeRoleId;
#endif
}

volatile sig_atomic_t g_stop_requested = 0;

/// Readable once a stop signal arrived, so socket lanes wait without a
/// timeout. Created before the handlers are installed and never closed: the
/// handler can never write to a closed or reused descriptor.
std::atomic<int> g_stop_signal_wake_fd{-1};
static_assert(std::atomic<int>::is_always_lock_free);

void HandleStopSignal(int) {
  g_stop_requested = 1;
  const int wake = g_stop_signal_wake_fd.load(std::memory_order_relaxed);
  if (wake >= 0) {
    const int saved_errno = errno;
    const std::uint64_t one = 1U;
    static_cast<void>(::write(wake, &one, sizeof(one)));
    errno = saved_errno;
  }
}

class FileDescriptor final {
public:
  explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept
      : value_(std::exchange(other.value_, -1)) {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        static_cast<void>(::close(value_));
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] explicit operator bool() const noexcept { return value_ >= 0; }

private:
  int value_;
};

[[noreturn]] void Fail(std::string detail) {
  throw std::runtime_error("Native execution host: " + std::move(detail));
}

[[noreturn]] void FailErrno(std::string_view context) {
  Fail(std::string(context) + ": " +
       std::system_error(errno, std::generic_category()).what());
}

void Require(bool condition, std::string detail) {
  if (!condition) {
    Fail(std::move(detail));
  }
}

[[nodiscard]] std::vector<RoboPlanJointLimitInput>
RoboPlanJointLimitsFromRetainedModel(
    const deployment::UrdfDocumentIr &urdf) {
  std::vector<RoboPlanJointLimitInput> result;
  result.reserve(urdf.joints.size());
  for (const auto &joint : urdf.joints) {
    if (joint.type == deployment::UrdfJointType::kFixed ||
        joint.type == deployment::UrdfJointType::kFloating ||
        joint.type == deployment::UrdfJointType::kPlanar ||
        joint.mimic.has_value()) {
      continue;
    }
    Require(joint.limit.has_value(),
            "retained movable joint '" + joint.name +
                "' has no explicit dynamics limits");
    const auto &limit = *joint.limit;
    Require(std::isfinite(limit.velocity) && limit.velocity > 0.0,
            "retained movable joint '" + joint.name +
                "' has no positive velocity limit");

    RoboPlanJointLimitInput row;
    row.joint_name = joint.name;
    row.maximum_velocity = {limit.velocity};
    if (joint.type == deployment::UrdfJointType::kRevolute ||
        joint.type == deployment::UrdfJointType::kPrismatic) {
      Require(limit.authored_lower.has_value() &&
                  limit.authored_upper.has_value(),
              "retained bounded joint '" + joint.name +
                  "' has incomplete position limits");
      row.minimum_position = std::vector<double>{limit.lower};
      row.maximum_position = std::vector<double>{limit.upper};
    }
    if (limit.acceleration.has_value()) {
      row.maximum_acceleration =
          std::vector<double>{*limit.acceleration};
    }
    result.push_back(std::move(row));
  }
  std::sort(result.begin(), result.end(), [](const auto &left,
                                              const auto &right) {
    return left.joint_name < right.joint_name;
  });
  Require(std::adjacent_find(
              result.begin(), result.end(), [](const auto &left,
                                                const auto &right) {
                return left.joint_name == right.joint_name;
              }) == result.end(),
          "retained model repeats a movable joint limit");
  return result;
}

// The RoboPlan worker reloads this base model by path whenever it restarts;
// the stage that owns those files must outlive every worker generation.
[[nodiscard]] RoboPlanWorkerModelDTO NativeWorkerBaseModel(
    const RetainedPlanningModelStage &stage,
    std::vector<RoboPlanJointLimitInput> joint_limits) {
  return RoboPlanWorkerModelDTO{"manyforge_i6_native_execution_worker",
                                stage.loader_urdf_path(),
                                stage.srdf_path(),
                                stage.package_paths(),
                                std::move(joint_limits),
                                {},
                                {}};
}

// One candidate RoboPlan scene and its exact state projector for the given
// bindings, built from the staged candidate whole-cell planning model.
struct CandidateRetainedModelScene final {
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<RetainedKinematicStateProjector> state_projector;
};

template <typename RetainedBundle, typename ModelMap, typename PlanningModel>
[[nodiscard]] CandidateRetainedModelScene BuildCandidateRetainedModelScene(
    std::string_view scene_name, const RetainedPlanningModelStage &stage,
    const PlanningModel &planning_model, const RetainedBundle &bundle,
    const ModelMap &model_map,
    std::vector<core::RuntimeParticipantBinding> bindings) {
  const auto srdf = planning_model.srdf_bytes();
  const auto documents = deployment::parse_model_documents_for_projection(
      planning_model.urdf_bytes(),
      srdf.empty() ? std::optional<std::string_view>{}
                   : std::optional<std::string_view>{srdf});
  CandidateRetainedModelScene result;
  result.scene = make_roboplan_scene(RoboPlanSceneInput{
      std::string(scene_name), std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths(),
      RoboPlanJointLimitsFromRetainedModel(documents.urdf)});
  result.scene_mutex = std::make_shared<std::mutex>();
  result.state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          result.scene, result.scene_mutex, bundle, model_map, planning_model,
          std::move(bindings), 0));
  return result;
}

[[nodiscard]] std::filesystem::path
ExistingDirectory(std::filesystem::path path, std::string_view label) {
  Require(!path.empty(), std::string(label) + " cannot be empty");
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  Require(!error && !std::filesystem::is_symlink(status) &&
              std::filesystem::is_directory(status),
          std::string(label) + " must be an existing non-symlink directory");
  auto absolute = std::filesystem::absolute(path, error);
  Require(!error, std::string(label) + " cannot be resolved");
  return absolute.lexically_normal();
}

[[nodiscard]] std::filesystem::path
ExistingPrivateDirectory(std::filesystem::path path,
                         const std::string_view label) {
  Require(path.is_absolute(),
          std::string(label) + " must be an absolute directory capability");
  Require(path.lexically_normal() == path,
          std::string(label) + " must use its canonical lexical spelling");
  std::error_code error;
  Require(std::filesystem::canonical(path, error) == path && !error,
          std::string(label) + " must be one canonical directory capability");
  struct stat before {};
  Require(::lstat(path.c_str(), &before) == 0 && S_ISDIR(before.st_mode) &&
              !S_ISLNK(before.st_mode),
          std::string(label) + " must be one plain directory");
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  struct stat opened {};
  struct stat after {};
  const bool valid =
      descriptor >= 0 && ::fstat(descriptor, &opened) == 0 &&
      ::lstat(path.c_str(), &after) == 0 && opened.st_dev == before.st_dev &&
      opened.st_ino == before.st_ino && after.st_dev == before.st_dev &&
      after.st_ino == before.st_ino && S_ISDIR(opened.st_mode) &&
      S_ISDIR(after.st_mode) && opened.st_uid == ::geteuid() &&
      after.st_uid == ::geteuid() &&
      (opened.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
      (after.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
      (opened.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) ==
          (S_IRUSR | S_IWUSR | S_IXUSR) &&
      (after.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) ==
          (S_IRUSR | S_IWUSR | S_IXUSR);
  if (descriptor >= 0) {
    static_cast<void>(::close(descriptor));
  }
  Require(valid, std::string(label) + " identity, owner, or mode is unsafe");
  return path;
}

void RequireDisjointDirectories(const std::filesystem::path &left,
                                const std::filesystem::path &right,
                                const std::string_view labels) {
  std::error_code error;
  const auto canonical_left = std::filesystem::canonical(left, error);
  Require(!error, std::string(labels) + " left directory is unavailable");
  const auto canonical_right = std::filesystem::canonical(right, error);
  Require(!error, std::string(labels) + " right directory is unavailable");
  const auto mismatch =
      std::mismatch(canonical_left.begin(), canonical_left.end(),
                    canonical_right.begin(), canonical_right.end());
  Require(mismatch.first != canonical_left.end() &&
              mismatch.second != canonical_right.end(),
          std::string(labels) + " must be disjoint directory trees");
}

void RequirePathOutsideDirectory(const std::filesystem::path &path,
                                 const std::filesystem::path &directory,
                                 const std::string_view labels) {
  if (path.empty()) {
    return;
  }
  std::error_code error;
  const auto canonical_parent =
      std::filesystem::canonical(path.parent_path(), error);
  Require(!error, std::string(labels) + " parent is unavailable");
  const auto canonical_path = canonical_parent / path.filename();
  const auto canonical_directory = std::filesystem::canonical(directory, error);
  Require(!error, std::string(labels) + " directory is unavailable");
  const auto mismatch =
      std::mismatch(canonical_directory.begin(), canonical_directory.end(),
                    canonical_path.begin(), canonical_path.end());
  Require(mismatch.first != canonical_directory.end(),
          std::string(labels) + " must be outside disposable run storage");
}

[[nodiscard]] std::filesystem::path
ExistingRegularFile(std::filesystem::path path, std::string_view label,
                    bool executable) {
  Require(!path.empty(), std::string(label) + " cannot be empty");
  struct stat status {};
  Require(::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) &&
              !S_ISLNK(status.st_mode),
          std::string(label) + " must be an existing non-symlink file");
  Require(!executable || ::access(path.c_str(), X_OK) == 0,
          std::string(label) + " must be executable");
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  Require(!error, std::string(label) + " cannot be resolved");
  return absolute.lexically_normal();
}

[[nodiscard]] std::string RandomToken(std::string_view prefix) {
  std::array<unsigned char, kOwnerEpochRandomBytes> bytes{};
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      FailErrno("mint execution-host epoch");
    }
    offset += static_cast<std::size_t>(count);
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(prefix);
  result.reserve(result.size() + bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(kHex[(byte >> 4U) & 0x0fU]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

class HostLock final {
public:
  explicit HostLock(const std::filesystem::path &run_root) {
    const auto path = run_root / std::string(kLockFileName);
    descriptor_ = FileDescriptor(
        ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
               S_IRUSR | S_IWUSR));
    if (!descriptor_) {
      FailErrno("open execution-host lock");
    }
    struct stat status {};
    if (::fstat(descriptor_.get(), &status) != 0) {
      FailErrno("inspect execution-host lock");
    }
    Require(S_ISREG(status.st_mode),
            "execution-host lock is not a regular file");
    if (::flock(descriptor_.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        Fail("another execution host already owns this run root");
      }
      FailErrno("acquire execution-host lock");
    }
  }

private:
  FileDescriptor descriptor_;
};

class CommandSocket final {
public:
  CommandSocket(const std::filesystem::path &run_root,
                std::string_view socket_file_name)
      : path_(run_root / std::string(socket_file_name)) {
    const auto spelling = path_.string();
    Require(spelling.size() < sizeof(sockaddr_un::sun_path),
            "execution command socket path is too long");
    struct stat existing {};
    if (::lstat(path_.c_str(), &existing) == 0) {
      Require(S_ISSOCK(existing.st_mode),
              "execution command path exists and is not a socket");
      if (::unlink(path_.c_str()) != 0) {
        FailErrno("remove stale execution command socket");
      }
    } else if (errno != ENOENT) {
      FailErrno("inspect execution command socket path");
    }
    descriptor_ = FileDescriptor(
        ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!descriptor_) {
      FailErrno("create execution command socket");
    }
    const int send_buffer_bytes = static_cast<int>(kMaximumResponseBytes);
    if (::setsockopt(descriptor_.get(), SOL_SOCKET, SO_SNDBUF,
                     &send_buffer_bytes, sizeof(send_buffer_bytes)) != 0) {
      FailErrno("set execution command response buffer");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
    if (::bind(descriptor_.get(), reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0) {
      FailErrno("bind execution command socket");
    }
    if (::chmod(path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
      FailErrno("restrict execution command socket permissions");
    }
    if (::lstat(path_.c_str(), &bound_status_) != 0 ||
        !S_ISSOCK(bound_status_.st_mode)) {
      Fail("bound execution command path is not a socket");
    }
    if (::listen(descriptor_.get(), kSocketBacklog) != 0) {
      FailErrno("listen on execution command socket");
    }
  }

  ~CommandSocket() {
    struct stat current {};
    if (::lstat(path_.c_str(), &current) == 0 &&
        current.st_dev == bound_status_.st_dev &&
        current.st_ino == bound_status_.st_ino) {
      static_cast<void>(::unlink(path_.c_str()));
    }
  }

  [[nodiscard]] int get() const noexcept { return descriptor_.get(); }

private:
  std::filesystem::path path_;
  FileDescriptor descriptor_;
  struct stat bound_status_ {};
};

struct HostConfig final {
  std::filesystem::path package_root;
  std::filesystem::path manager_state_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path execution_state_root;
  std::filesystem::path execution_run_root;
  std::filesystem::path candidate_worker;
  std::string cell_id;
  std::string program_binding_id;
  std::string motion_target_role_id;
  std::string gripper_target_role_id;
};

struct ManagedRuntimeChildConfig final {
  int manager_channel_fd{-1};
  int manager_liveness_channel_fd{-1};
  int package_root_fd{-1};
  int program_artifact_root_fd{-1};
  std::filesystem::path package_root;
  std::filesystem::path program_artifact_root;
  std::filesystem::path manager_state_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path logical_scene_database;
  std::filesystem::path program_cycle_journal;
  std::filesystem::path execution_state_root;
  std::filesystem::path execution_run_root;
  std::filesystem::path candidate_worker;
  std::string cell_id;
  core::RetainedBundleId retained_bundle_id{""};
  core::GraphRevision graph_revision{""};
  std::string runtime_unit_id;
  std::string runtime_epoch;
  std::string product_identity;
  std::string configuration_identity;
};

[[nodiscard]] bool
IsPortableRuntimeIdentity(const std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U ||
      !((value.front() >= 'a' && value.front() <= 'z') ||
        (value.front() >= 'A' && value.front() <= 'Z') ||
        (value.front() >= '0' && value.front() <= '9'))) {
    return false;
  }
  return std::all_of(value.begin() + 1, value.end(), [](const char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '.' ||
           character == '_' || character == '-';
  });
}

[[nodiscard]] int ParseFileDescriptor(const std::string_view value) {
  Require(!value.empty() && (value.size() == 1U || value.front() != '0'),
          "managed runtime channel FD is not canonical");
  unsigned int result = 0U;
  for (const char digit : value) {
    Require(digit >= '0' && digit <= '9',
            "managed runtime channel FD is not decimal");
    const auto next = static_cast<unsigned int>(digit - '0');
    Require(result <= (static_cast<unsigned int>(INT_MAX) - next) / 10U,
            "managed runtime channel FD overflows");
    result = result * 10U + next;
  }
  return static_cast<int>(result);
}

[[nodiscard]] ManagedRuntimeChildConfig
ParseManagedRuntimeArguments(const int argc, char **argv) {
  ManagedRuntimeChildConfig config;
  std::set<std::string> seen;
  for (int index = 1; index < argc; index += 2) {
    Require(index + 1 < argc,
            "every managed runtime option requires one value");
    const std::string option(argv[index]);
    const std::string value(argv[index + 1]);
    Require(seen.insert(option).second,
            "duplicate managed runtime option " + option);
    if (option == "--manyforge-managed-runtime-channel-fd") {
      config.manager_channel_fd = ParseFileDescriptor(value);
    } else if (option == "--manyforge-managed-liveness-channel-fd") {
      config.manager_liveness_channel_fd = ParseFileDescriptor(value);
    } else if (option == "--manyforge-managed-package-root-fd") {
      config.package_root_fd = ParseFileDescriptor(value);
    } else if (option == "--manyforge-managed-program-artifact-root-fd") {
      config.program_artifact_root_fd = ParseFileDescriptor(value);
    } else if (option == "--manyforge-managed-manager-state-root") {
      config.manager_state_root = value;
    } else if (option == "--manyforge-managed-manager-run-root") {
      config.manager_run_root = value;
    } else if (option == "--manyforge-managed-logical-scene-database") {
      config.logical_scene_database = value;
    } else if (option == "--manyforge-managed-program-cycle-journal") {
      config.program_cycle_journal = value;
    } else if (option == "--manyforge-managed-execution-state-root") {
      config.execution_state_root = value;
    } else if (option == "--manyforge-managed-execution-run-root") {
      config.execution_run_root = value;
    } else if (option == "--manyforge-managed-candidate-worker") {
      config.candidate_worker = value;
    } else if (option == "--manyforge-managed-cell-id") {
      config.cell_id = value;
    } else if (option == "--manyforge-managed-retained-bundle-id") {
      config.retained_bundle_id = core::RetainedBundleId(value);
    } else if (option == "--manyforge-managed-graph-revision") {
      config.graph_revision = core::GraphRevision(value);
    } else if (option == "--runtime-unit-id") {
      config.runtime_unit_id = value;
    } else if (option == "--runtime-epoch") {
      config.runtime_epoch = value;
    } else if (option == "--product-identity") {
      config.product_identity = value;
    } else if (option == "--configuration-identity") {
      config.configuration_identity = value;
    } else {
      Fail("unknown managed runtime option " + option);
    }
  }
  Require(seen.size() >= 15U && seen.size() <= 18U &&
              config.manager_channel_fd >= 0 &&
              config.manager_liveness_channel_fd >= 0 &&
              config.manager_liveness_channel_fd != config.manager_channel_fd &&
              config.package_root_fd == kManagedPackageRootFd &&
              config.program_artifact_root_fd ==
                  kManagedProgramArtifactRootFd,
          "managed runtime invocation is incomplete");
  Require(IsPortableRuntimeIdentity(config.cell_id) &&
              IsPortableRuntimeIdentity(config.runtime_unit_id) &&
              IsPortableRuntimeIdentity(config.runtime_epoch),
          "managed runtime identity is invalid");
  Require(!config.retained_bundle_id.empty() && !config.graph_revision.empty(),
          "managed runtime retained descriptor is invalid");
  Require(core::IsExactLowercaseSha256Digest(config.product_identity) &&
              core::IsExactLowercaseSha256Digest(config.configuration_identity),
          "managed runtime retained identity is invalid");
  struct stat package_status {};
  struct stat program_artifact_status {};
  const int package_flags = ::fcntl(config.package_root_fd, F_GETFD);
  const int program_artifact_flags =
      ::fcntl(config.program_artifact_root_fd, F_GETFD);
  Require(::fstat(config.package_root_fd, &package_status) == 0 &&
              S_ISDIR(package_status.st_mode) && package_flags >= 0 &&
              (package_flags & FD_CLOEXEC) == 0 &&
              ::fstat(config.program_artifact_root_fd,
                      &program_artifact_status) == 0 &&
              S_ISDIR(program_artifact_status.st_mode) &&
              program_artifact_flags >= 0 &&
              (program_artifact_flags & FD_CLOEXEC) == 0,
          "managed package-root descriptor is not a retained directory");
  config.package_root = std::filesystem::path("/proc/self/fd") /
                        std::to_string(config.package_root_fd);
  config.program_artifact_root =
      std::filesystem::path("/proc/self/fd") /
      std::to_string(config.program_artifact_root_fd);
  if (program_artifact_status.st_dev == package_status.st_dev &&
      program_artifact_status.st_ino == package_status.st_ino) {
    config.program_artifact_root.clear();
  }
  config.manager_state_root = ExistingPrivateDirectory(
      std::move(config.manager_state_root), "managed Manager state root");
  config.manager_run_root = ExistingPrivateDirectory(
      std::move(config.manager_run_root), "managed Manager run root");
  if (!config.logical_scene_database.empty()) {
    config.logical_scene_database =
        ExistingRegularFile(std::move(config.logical_scene_database),
                            "managed logical-scene database", false);
  }
  const auto validate_future_database = [](std::filesystem::path path,
                                           const std::string_view label) {
    Require(!path.empty() && path.has_filename() && path.filename() != "." &&
                path.filename() != "..",
            std::string(label) + " path is invalid");
    static_cast<void>(ExistingPrivateDirectory(path.parent_path(),
                                               std::string(label) + " parent"));
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    Require((!error || error == std::errc::no_such_file_or_directory) &&
                !std::filesystem::is_symlink(status),
            std::string(label) + " path is not a safe physical leaf");
    return path;
  };
  if (!config.program_cycle_journal.empty()) {
    config.program_cycle_journal =
        validate_future_database(std::move(config.program_cycle_journal),
                                 "managed Program-cycle journal");
  }
  config.execution_state_root = ExistingPrivateDirectory(
      std::move(config.execution_state_root), "managed execution state root");
  config.execution_run_root = ExistingPrivateDirectory(
      std::move(config.execution_run_root), "managed execution run root");
  const std::array<const std::filesystem::path *, 4U> typed_roots{
      &config.manager_state_root, &config.manager_run_root,
      &config.execution_state_root, &config.execution_run_root};
  for (std::size_t left = 0U; left < typed_roots.size(); ++left) {
    for (std::size_t right = left + 1U; right < typed_roots.size(); ++right) {
      RequireDisjointDirectories(*typed_roots[left], *typed_roots[right],
                                 "managed state/run capabilities");
    }
  }
  for (const auto *typed_root : typed_roots) {
    RequireDisjointDirectories(config.package_root, *typed_root,
                               "managed install/runtime capabilities");
    if (!config.program_artifact_root.empty()) {
      RequireDisjointDirectories(config.program_artifact_root, *typed_root,
                                 "managed Program/runtime capabilities");
    }
  }
  const std::array<const std::filesystem::path *, 2U> durable_paths{
      &config.logical_scene_database, &config.program_cycle_journal};
  const std::array<const std::filesystem::path *, 2U> run_roots{
      &config.manager_run_root, &config.execution_run_root};
  for (const auto *durable_path : durable_paths) {
    for (const auto *run_root : run_roots) {
      RequirePathOutsideDirectory(*durable_path, *run_root,
                                  "managed durable path/run capability");
    }
  }
  config.candidate_worker = ExistingRegularFile(
      std::move(config.candidate_worker), "managed candidate worker", true);

  const auto require_parent_channel = [](const int descriptor,
                                         const std::string_view label) {
    struct stat status {};
    int socket_type = 0;
    socklen_t socket_type_size = sizeof(socket_type);
    ucred credentials{};
    socklen_t credentials_size = sizeof(credentials);
    Require(::fstat(descriptor, &status) == 0 && S_ISSOCK(status.st_mode) &&
                ::getsockopt(descriptor, SOL_SOCKET, SO_TYPE, &socket_type,
                             &socket_type_size) == 0 &&
                socket_type_size == sizeof(socket_type) &&
                socket_type == SOCK_SEQPACKET &&
                ::getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                             &credentials_size) == 0 &&
                credentials_size == sizeof(credentials) &&
                credentials.uid == ::geteuid() &&
                credentials.pid == ::getppid(),
            std::string(label) + " is not the direct same-user parent channel");
  };
  require_parent_channel(config.manager_channel_fd, "managed runtime channel");
  require_parent_channel(config.manager_liveness_channel_fd,
                         "managed runtime liveness channel");
  return config;
}

void SendManagedRuntimePacket(const int fd, const std::string_view bytes) {
  Require(
      !bytes.empty() &&
          bytes.size() <=
              component_manager::detail::kMaximumManagedRuntimeRpcPacketBytes,
      "managed runtime response is empty or oversized");
  pollfd descriptor{fd, POLLOUT | POLLHUP, 0};
  int ready = -1;
  do {
    ready = ::poll(&descriptor, 1U, 750);
  } while (ready < 0 && errno == EINTR && g_stop_requested == 0);
  Require(ready > 0 && (descriptor.revents & POLLOUT) != 0,
          "managed runtime parent did not accept a response");
  ssize_t sent = -1;
  do {
    sent = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  Require(sent == static_cast<ssize_t>(bytes.size()),
          "managed runtime response was not sent atomically");
}

[[nodiscard]] std::optional<std::string>
ReceiveManagedRuntimePacket(const int fd) {
  std::array<char,
             component_manager::detail::kMaximumManagedRuntimeRpcPacketBytes>
      buffer{};
  iovec vector{buffer.data(), buffer.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  ssize_t size = -1;
  do {
    size = ::recvmsg(fd, &message, 0);
  } while (size < 0 && errno == EINTR);
  if (size == 0) {
    return std::nullopt;
  }
  Require(size > 0, "managed runtime request is absent");
  if ((message.msg_flags & MSG_TRUNC) != 0) {
    // Consume and reject a foreign oversized packet without terminating the
    // sole managed child. The ordinary malformed-request path returns one
    // bounded failure packet to the caller.
    return std::string("oversized-managed-runtime-request");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(size));
}

struct ManagedComponentProjectionSet final {
  core::ProjectionRevisionDTO projection_revision;
  std::map<core::ControllableComponentId,
           std::shared_ptr<RetainedPlannerSceneProjection>>
      by_component;
};

[[nodiscard]] std::optional<ManagedComponentProjectionSet>
SelectManagedComponentProjections(const ManagedRuntimeChildConfig &config,
                                  const deployment::RetainedBundle &bundle,
                                  const ManagedProjectionRegistry &projections,
                                  const core::ProjectionRevisionDTO &expected,
                                  const bool allow_current_scene_revision) {
  const auto unavailable = [](const std::string &detail) {
    static std::mutex mutex;
    static std::string last_detail;
    std::lock_guard lock(mutex);
    if (last_detail != detail) {
      last_detail = detail;
      std::cerr << "managed component projection selection: " << detail
                << '\n';
    }
    return std::optional<ManagedComponentProjectionSet>{};
  };
  ManagedComponentProjectionSet selected;
  bool revision_selected = false;
  std::shared_ptr<RetainedPlannerSceneProjection> cell_projection;
  std::set<core::ControllableComponentId> motion_projection_components;
  const auto supported_component = [&](const core::ControllableComponentId &id) {
    return std::any_of(bundle.value().cell_graph.control_bindings.begin(),
                       bundle.value().cell_graph.control_bindings.end(),
                       [&](const auto &binding) {
                         return binding.controllable_component_id == id &&
                                binding.provider_runtime_unit_id ==
                                    core::RuntimeUnitId(config.runtime_unit_id) &&
                                (binding.capability_id.value() ==
                                     "execution.trajectory.execute" ||
                                 binding.capability_id.value() ==
                                     "tool.gripper.command");
                       });
  };
  for (const auto &provider : bundle.value().capability_provider_bindings) {
    if (!core::RequiresNativeSceneProjection(
            provider.capability_id, provider.capability_definition_revision)) {
      continue;
    }
    std::set<core::RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : provider.endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      Require(runtime != nullptr && !runtime->empty(),
              "managed joint planner has a malformed runtime endpoint");
      runtime_sources.insert(*runtime);
    }
    if (runtime_sources.size() != 1U ||
        runtime_sources.begin()->value() != config.runtime_unit_id) {
      continue;
    }
    const core::ProjectionNativeRuntimeBindingDTO binding{
        provider.capability_provider_binding_id,
        provider.capability_provider_binding_revision,
        core::RuntimeUnitId(config.runtime_unit_id),
        core::RuntimeEpoch(config.runtime_epoch)};
    auto revision = expected;
    auto projection = projections.FindActiveProjection(revision, binding);
    if (projection == nullptr && allow_current_scene_revision) {
      const auto current =
          projections.FindCurrentProjectionRevision(expected, binding);
      if (!current.has_value())
        return unavailable("no current revision for " +
                           binding.provider_binding_id.value());
      revision = *current;
      projection = projections.FindActiveProjection(revision, binding);
    }
    if (projection == nullptr)
      return unavailable("no active projection for " +
                         binding.provider_binding_id.value());
    if (cell_projection == nullptr)
      cell_projection = projection;
    if (!revision_selected) {
      selected.projection_revision = revision;
      revision_selected = true;
    } else {
      if (selected.projection_revision != revision)
        return unavailable("component projections have different revisions");
    }
    if (const auto *target =
            std::get_if<core::TargetBindingCapabilityProviderTargetDTO>(
                &provider.target);
        target != nullptr &&
        supported_component(target->controllable_component_id)) {
      // A motion owner must use its own joint-goal projection. Other native
      // providers may share that component but cannot silently substitute
      // their projection for the planner's bound one.
      if (provider.capability_id.value() == "planning.motion.joint_goal") {
        Require(motion_projection_components
                    .insert(target->controllable_component_id)
                    .second,
                "managed motion component has multiple joint planners");
        selected.by_component[target->controllable_component_id] = projection;
      } else {
        selected.by_component.try_emplace(target->controllable_component_id,
                                          projection);
      }
    }
  }
  for (const auto &binding : bundle.value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id ==
            core::RuntimeUnitId(config.runtime_unit_id) &&
        binding.capability_id.value() == "execution.trajectory.execute" &&
        !motion_projection_components.contains(
            binding.controllable_component_id))
      return unavailable("motion component has no active native projection");
  }
  if (!revision_selected) {
    selected.projection_revision = expected;
    return selected;
  }
  // A gripper with an independent controller need not have a motion planner.
  // Its frame/Scene view is the same cell projection already selected for the
  // motion owners; this does not create a planner or a projection lease for
  // the gripper.
  Require(cell_projection != nullptr,
          "selected native projection has no cell model");
  for (const auto &binding : bundle.value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id !=
            core::RuntimeUnitId(config.runtime_unit_id) ||
        (binding.capability_id.value() != "execution.trajectory.execute" &&
         binding.capability_id.value() != "tool.gripper.command"))
      continue;
    selected.by_component.try_emplace(binding.controllable_component_id,
                                      cell_projection);
  }
  return selected;
}

[[nodiscard]] int
RunManagedRuntimeChild(const ManagedRuntimeChildConfig &config);
void RequireExactObjectSize(const CanonicalValue &value, std::size_t size,
                            std::string_view label) {
  Require(value.is_object() && value.as_object().size() == size,
          std::string(label) + " has an unknown or missing field");
}

const CanonicalValue &Member(const CanonicalValue &root,
                             std::string_view name) {
  const auto *value = root.find_object_member(name);
  Require(value != nullptr, "command is missing field " + std::string(name));
  return *value;
}

std::string Text(const CanonicalValue &root, std::string_view name,
                 bool allow_empty = false) {
  const auto *value = std::get_if<std::string>(&Member(root, name).storage());
  Require(value != nullptr && (allow_empty || !value->empty()) &&
              value->size() <= kMaximumIdentityBytes,
          "command field " + std::string(name) + " must be one bounded string");
  return *value;
}

double Number(const CanonicalValue &root, std::string_view name) {
  const auto *value = std::get_if<double>(&Member(root, name).storage());
  Require(value != nullptr && std::isfinite(*value),
          "command field " + std::string(name) + " must be one finite number");
  return *value;
}

bool Boolean(const CanonicalValue &root, std::string_view name) {
  const auto *value = std::get_if<bool>(&Member(root, name).storage());
  Require(value != nullptr,
          "command field " + std::string(name) + " must be boolean");
  return *value;
}

std::uint64_t Counter(const CanonicalValue &root, std::string_view name,
                      std::uint64_t minimum) {
  const double value = Number(root, name);
  Require(value >= static_cast<double>(minimum) && std::floor(value) == value &&
              value <= kMaximumSafeInteger,
          "command field " + std::string(name) +
              " must be one safe unsigned integer");
  return static_cast<std::uint64_t>(value);
}

std::optional<double> OptionalNumber(const CanonicalValue &root,
                                     std::string_view name) {
  const auto &member = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto *value = std::get_if<double>(&member.storage());
  Require(value != nullptr && std::isfinite(*value),
          "command field " + std::string(name) + " must be null or finite");
  return *value;
}

std::optional<std::uint64_t> OptionalCounter(const CanonicalValue &root,
                                             const std::string_view name,
                                             const std::uint64_t minimum) {
  const auto value = OptionalNumber(root, name);
  if (!value.has_value()) {
    return std::nullopt;
  }
  Require(*value >= static_cast<double>(minimum) &&
              std::floor(*value) == *value && *value <= kMaximumSafeInteger,
          "command field " + std::string(name) +
              " must be null or one safe unsigned integer");
  return static_cast<std::uint64_t>(*value);
}

std::optional<std::string> OptionalText(const CanonicalValue &root,
                                        std::string_view name) {
  const auto &member = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto *value = std::get_if<std::string>(&member.storage());
  Require(value != nullptr && !value->empty() &&
              value->size() <= kMaximumIdentityBytes,
          "command field " + std::string(name) +
              " must be null or one bounded string");
  return *value;
}

std::optional<std::string> OptionalBoundedText(const CanonicalValue &root,
                                               std::string_view name,
                                               std::size_t maximum_bytes) {
  const auto &member = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto *value = std::get_if<std::string>(&member.storage());
  Require(value != nullptr && !value->empty() && value->size() <= maximum_bytes,
          "command field " + std::string(name) +
              " must be null or one bounded string");
  return *value;
}

const CanonicalValue::Array &Array(const CanonicalValue &root,
                                   std::string_view name) {
  const auto &member = Member(root, name);
  const auto *values = std::get_if<CanonicalValue::Array>(&member.storage());
  Require(values != nullptr,
          "command field " + std::string(name) + " must be an array");
  return *values;
}

std::vector<std::string> TextArray(const CanonicalValue &root,
                                   std::string_view name, std::size_t maximum) {
  const auto &values = Array(root, name);
  Require(values.size() <= maximum,
          "command text array exceeds its fixed bound");
  std::vector<std::string> result;
  result.reserve(values.size());
  for (const auto &value : values) {
    const auto *text = std::get_if<std::string>(&value.storage());
    Require(text != nullptr && !text->empty() &&
                text->size() <= kMaximumIdentityBytes,
            "command text array contains an invalid value");
    result.push_back(*text);
  }
  return result;
}

std::vector<double> NumberArray(const CanonicalValue &root,
                                std::string_view name, std::size_t maximum) {
  const auto &values = Array(root, name);
  Require(values.size() <= maximum,
          "command number array exceeds its fixed bound");
  std::vector<double> result;
  result.reserve(values.size());
  for (const auto &value : values) {
    const auto *number = std::get_if<double>(&value.storage());
    Require(number != nullptr && std::isfinite(*number),
            "command number array contains a non-finite value");
    result.push_back(*number);
  }
  return result;
}

[[nodiscard]] core::Pose3DTO DecodePose(const CanonicalValue &value) {
  RequireExactObjectSize(value, 2U, "pose");
  const auto &position = Member(value, "position_m");
  const auto &orientation = Member(value, "orientation");
  RequireExactObjectSize(position, 3U, "pose position");
  RequireExactObjectSize(orientation, 4U, "pose orientation");
  return core::Pose3DTO{
      core::Vector3DTO{Number(position, "x_m"), Number(position, "y_m"),
                       Number(position, "z_m")},
      core::QuaternionDTO{Number(orientation, "x"), Number(orientation, "y"),
                          Number(orientation, "z"), Number(orientation, "w")}};
}

/// Decode one portable reference. `frame_role` may carry an exact robot
/// qualifier `target` ({target_role_id, controllable_component_id,
/// command_target_id}); its Program-Binding agreement is checked at admission
/// by `VerifyReferenceTargets`, never inferred here.
[[nodiscard]] core::ReferenceDTO
DecodeReference(const CanonicalValue &value, const bool allow_robot_target) {
  Require(value.is_object(), "motion reference must be an object");
  const auto type = Text(value, "type");
  if (type == "cell_frame") {
    RequireExactObjectSize(value, 1U, "cell-frame reference");
    return core::ReferenceDTO(core::ReferenceDTO::Type::kCellFrame,
                              std::string{});
  }
  if (type == "frame_role") {
    const auto *target = value.find_object_member("target");
    if (target == nullptr) {
      RequireExactObjectSize(value, 2U, "motion reference");
      return core::ReferenceDTO(core::ReferenceDTO::Type::kFrameRole,
                                Text(value, "frame_role_id"));
    }
    Require(allow_robot_target,
            "robot-qualified frame reference is unsupported here");
    RequireExactObjectSize(value, 3U, "robot frame reference");
    RequireExactObjectSize(*target, 3U, "robot frame reference target");
    core::ReferenceDTO reference(
        core::FrameRoleId(Text(value, "frame_role_id")),
        core::ReferenceTargetDTO{
            core::TargetRoleId(Text(*target, "target_role_id")),
            core::ControllableComponentId(
                Text(*target, "controllable_component_id")),
            core::CommandTargetId(Text(*target, "command_target_id"))});
    Require(core::reference_identity_matches_type(reference),
            "robot frame reference is incomplete");
    return reference;
  }
  RequireExactObjectSize(value, 2U, "motion reference");
  if (type == "component_body_role") {
    return core::ReferenceDTO(core::ReferenceDTO::Type::kComponentBodyRole,
                              Text(value, "collision_body_role_id"));
  }
  if (type == "scene_resource") {
    return core::ReferenceDTO(core::ReferenceDTO::Type::kSceneResource,
                              Text(value, "scene_resource_id"));
  }
  if (type == "runtime_object") {
    return core::ReferenceDTO(core::ReferenceDTO::Type::kRuntimeObject,
                              Text(value, "runtime_object_id"));
  }
  if (type == "pose") {
    return core::ReferenceDTO(DecodePose(Member(value, "pose")));
  }
  Fail("motion reference type is unsupported");
}

[[nodiscard]] std::optional<core::ReferenceDTO>
DecodeOptionalReference(const CanonicalValue &root, const std::string_view name,
                        const bool allow_robot_target = false) {
  const auto &value = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(value.storage())) {
    return std::nullopt;
  }
  return DecodeReference(value, allow_robot_target);
}

[[nodiscard]] core::MotionSpecDTO
DecodeMotionSpec(const CanonicalValue &value) {
  const auto kind = Text(value, "kind");
  if (kind == "joint_goal") {
    RequireExactObjectSize(value, 3U, "joint goal");
    return core::JointGoalDTO{
        TextArray(value, "joint_names", core::kMaxOperationParticipants),
        NumberArray(value, "positions_rad", core::kMaxOperationParticipants)};
  }
  if (kind == "pose_goal") {
    RequireExactObjectSize(value, 2U, "pose goal");
    return core::PoseGoalDTO{DecodePose(Member(value, "goal"))};
  }
  if (kind == "cartesian_path") {
    const auto *blend = value.find_object_member("max_blend_deviation_rad");
    // A Cartesian path has nine required fields; the blend-deviation field is
    // the one optional tenth field.  Keep the decoder aligned with the
    // frontend/headless CartesianPath contract.
    RequireExactObjectSize(value, blend == nullptr ? 9U : 10U,
                           "Cartesian path");
    const auto &encoded_waypoints = Array(value, "waypoints");
    Require(!encoded_waypoints.empty() &&
                encoded_waypoints.size() <= core::kMaxOperationParticipants,
            "Cartesian path waypoint count is outside its fixed bound");
    std::vector<core::Pose3DTO> waypoints;
    waypoints.reserve(encoded_waypoints.size());
    for (const auto &waypoint : encoded_waypoints) {
      waypoints.push_back(DecodePose(waypoint));
    }
    Require(Text(value, "speed_mode") == "bounded",
            "Cartesian speed mode is unsupported");
    core::CartesianFallbackPolicyDTO fallback;
    const auto fallback_name = Text(value, "fallback_policy");
    if (fallback_name == "fail_closed") {
      fallback = core::CartesianFallbackPolicyDTO::kFailClosed;
    } else if (fallback_name == "allow_joint_space_fallback") {
      fallback = core::CartesianFallbackPolicyDTO::kAllowJointSpaceFallback;
    } else {
      Fail("Cartesian fallback policy is unsupported");
    }
    core::CartesianPathDTO result{
        std::move(waypoints),
        Number(value, "max_translation_step_m"),
        Number(value, "max_rotation_step_rad"),
        core::CartesianSpeedModeDTO::kBounded,
        Number(value, "max_angular_speed_rad_s"),
        Number(value, "max_linear_acceleration_mps2"),
        Number(value, "max_angular_acceleration_rad_s2"),
        fallback};
    if (blend != nullptr) {
      result.max_blend_deviation_rad = Number(value, "max_blend_deviation_rad");
    }
    return result;
  }
  Fail("motion specification kind is unsupported");
}

[[nodiscard]] core::SceneRevision
DecodeSceneRevision(const CanonicalValue &value) {
  RequireExactObjectSize(value, 3U, "scene revision");
  const double revision = Number(value, "revision_id");
  Require(revision >= 1.0 && std::floor(revision) == revision &&
              revision <= 9'007'199'254'740'991.0,
          "scene revision id is invalid");
  return core::SceneRevision{core::SceneId(Text(value, "scene_id")),
                             static_cast<std::uint64_t>(revision),
                             core::ContentDigest(Text(value, "content_hash"))};
}

[[nodiscard]] core::ActionSpanDTO
DecodeActionSpan(const CanonicalValue &value) {
  RequireExactObjectSize(value, 6U, "action span");
  const auto source =
      core::action_source_from_string(Text(value, "action_source"));
  Require(source.has_value(), "action span source is unsupported");
  const auto optional_or_empty = [&](std::string_view name) {
    return OptionalText(value, name).value_or(std::string{});
  };
  return core::ActionSpanDTO{*source,
                             Text(value, "action_span_id"),
                             optional_or_empty("program_run_id"),
                             optional_or_empty("node_instance_id"),
                             optional_or_empty("node_attempt_id"),
                             optional_or_empty("parent_node_attempt_id")};
}

[[nodiscard]] core::PublicRequestContextDTO
DecodeRequestContext(const CanonicalValue &value) {
  RequireExactObjectSize(value, 4U, "public request context");
  return core::PublicRequestContextDTO{
      core::RequestId(Text(value, "request_id")),
      core::RequestOwnerEpoch(Text(value, "request_owner_epoch")),
      Text(value, "source_label"), OptionalText(value, "correlation_id")};
}

[[nodiscard]] core::RequestAttributionDTO
DecodeRequestAttribution(const CanonicalValue &value) {
  RequireExactObjectSize(value, 3U, "request attribution");
  core::RequestTransportDTO transport;
  const auto transport_name = Text(value, "transport");
  if (transport_name == "ros") {
    transport = core::RequestTransportDTO::kRos;
  } else if (transport_name == "http") {
    transport = core::RequestTransportDTO::kHttp;
  } else if (transport_name == "internal") {
    transport = core::RequestTransportDTO::kInternal;
  } else {
    Fail("request attribution transport is unsupported");
  }
  core::RequestAssuranceDTO assurance;
  const auto assurance_name = Text(value, "assurance");
  if (assurance_name == "unverified") {
    assurance = core::RequestAssuranceDTO::kUnverified;
  } else if (assurance_name == "externally_authenticated") {
    assurance = core::RequestAssuranceDTO::kExternallyAuthenticated;
  } else {
    Fail("request attribution assurance is unsupported");
  }
  core::RequestAttributionDTO attribution{transport, assurance,
                                          OptionalText(value, "principal_id")};
  Require(attribution.locally_valid(),
          "request attribution is internally inconsistent");
  return attribution;
}

[[nodiscard]] core::JointDynamicsLimitDTO
DecodeDynamicsLimit(const CanonicalValue &value) {
  RequireExactObjectSize(value, 3U, "joint dynamics limit");
  return {OptionalNumber(value, "max_velocity_rad_s"),
          OptionalNumber(value, "max_acceleration_rad_s2"),
          OptionalNumber(value, "max_jerk_rad_s3")};
}

[[nodiscard]] core::MotionParticipantIntentDTO
DecodeMotionPayload(const CanonicalValue &value) {
  const bool has_runtime_object_bindings =
      value.is_object() &&
      value.find_object_member("runtime_object_bindings") != nullptr;
  // Optional nullable member; omitted by callers that predate framed goals.
  const bool has_goal_frame_member =
      value.is_object() &&
      value.find_object_member("goal_frame_reference") != nullptr;
  Require(value.is_object() &&
              value.as_object().size() ==
                  16U + (has_runtime_object_bindings ? 1U : 0U) +
                      (has_goal_frame_member ? 1U : 0U),
          "motion payload has an unknown or missing field");
  Require(Text(value, "kind") == "motion",
          "operation payload kind is unsupported");

  const auto &tolerances = Member(value, "tolerances");
  RequireExactObjectSize(tolerances, 3U, "motion tolerances");
  const auto &speed = Member(value, "speed_profile");
  RequireExactObjectSize(speed, 3U, "motion speed profile");
  const auto &timeouts = Member(value, "timeouts");
  RequireExactObjectSize(timeouts, 2U, "motion timeouts");

  core::CacheMode cache_mode;
  const auto cache = Text(value, "cache_mode");
  if (cache == "default") {
    cache_mode = core::CacheMode::kDefault;
  } else if (cache == "disable") {
    cache_mode = core::CacheMode::kDisable;
  } else {
    Fail("motion cache mode is unsupported");
  }
  core::PlanningProfileDTO planning_profile;
  const auto profile = Text(value, "planning_profile");
  if (profile == "first_feasible") {
    planning_profile = core::PlanningProfileDTO::kFirstFeasible;
  } else if (profile == "balanced") {
    planning_profile = core::PlanningProfileDTO::kBalanced;
  } else if (profile == "quality_bounded") {
    planning_profile = core::PlanningProfileDTO::kQualityBounded;
  } else {
    Fail("motion planning profile is unsupported");
  }
  core::MotionTargetModeDTO target_mode;
  const auto target_mode_name = Text(value, "target_mode");
  if (target_mode_name == "absolute") {
    target_mode = core::MotionTargetModeDTO::kAbsolute;
  } else if (target_mode_name == "relative") {
    target_mode = core::MotionTargetModeDTO::kRelative;
  } else {
    Fail("motion target mode is unsupported");
  }
  auto target_reference =
      DecodeOptionalReference(value, "target_reference", true);
  auto offset_reference =
      DecodeOptionalReference(value, "offset_reference", true);
  auto goal_frame_reference =
      has_goal_frame_member
          ? DecodeOptionalReference(value, "goal_frame_reference", true)
          : std::nullopt;
  Require(Array(value, "motion_interrupt_scopes").empty(),
          "native motion cannot carry I8 interrupt scopes");

  const auto &motion = Member(value, "motion");
  auto decoded_motion = DecodeMotionSpec(motion);
  const bool joint_goal =
      std::holds_alternative<core::JointGoalDTO>(decoded_motion);
  if (goal_frame_reference.has_value()) {
    Require(target_mode == core::MotionTargetModeDTO::kAbsolute &&
                (std::holds_alternative<core::PoseGoalDTO>(decoded_motion) ||
                 std::holds_alternative<core::CartesianPathDTO>(
                     decoded_motion)) &&
                goal_frame_reference->type != core::ReferenceDTO::Type::kPose,
            "goal frame requires an absolute pose or Cartesian goal and a "
            "named frame");
  }
  if (target_mode == core::MotionTargetModeDTO::kAbsolute) {
    Require(!target_reference.has_value() && !offset_reference.has_value(),
            "absolute motion cannot carry references");
  } else if (joint_goal) {
    Require(!target_reference.has_value() && !offset_reference.has_value(),
            "relative joint motion cannot carry spatial references");
  } else {
    Require(target_reference.has_value(),
            "relative spatial motion requires target_reference");
  }

  std::optional<core::ContactPolicyId> contact_policy;
  if (const auto contact_name = OptionalText(value, "contact_policy_id");
      contact_name.has_value()) {
    contact_policy = core::ContactPolicyId(*contact_name);
  }
  std::vector<core::RuntimeObjectReferenceBindingDTO> runtime_objects;
  if (has_runtime_object_bindings) {
    const auto &encoded_runtime_objects =
        Array(value, "runtime_object_bindings");
    Require(encoded_runtime_objects.size() <=
                core::kMaxOperationRuntimeObjectBindings,
            "runtime-object binding count exceeds its fixed bound");
    runtime_objects.reserve(encoded_runtime_objects.size());
    for (const auto &encoded : encoded_runtime_objects) {
      RequireExactObjectSize(encoded, 2U, "runtime-object binding");
      runtime_objects.push_back(core::RuntimeObjectReferenceBindingDTO{
          core::RuntimeObjectId(Text(encoded, "runtime_object_id")),
          core::SceneResourceId(Text(encoded, "scene_resource_id"))});
    }
  }
  if (joint_goal) {
    Require(
        !contact_policy.has_value() && runtime_objects.empty(),
        "joint motion cannot carry contact-policy or runtime-object bindings");
  }

  const auto &dynamics = Member(value, "per_joint_dynamics_limits");
  Require(dynamics.is_object() &&
              dynamics.as_object().size() <= core::kMaxOperationParticipants,
          "joint dynamics map exceeds its fixed bound");
  std::map<std::string, core::JointDynamicsLimitDTO> limits;
  for (const auto &[joint_name, limit] : dynamics.as_object()) {
    Require(!joint_name.empty() && joint_name.size() <= kMaximumIdentityBytes,
            "joint dynamics map contains an invalid name");
    const auto [position, inserted] =
        limits.emplace(joint_name, DecodeDynamicsLimit(limit));
    static_cast<void>(position);
    Require(inserted, "joint dynamics map repeats one name");
  }

  return core::MotionParticipantIntentDTO{
      core::MoveId(Text(value, "move_id")),
      core::FrameRoleId(Text(value, "base_frame_role_id")),
      core::FrameRoleId(Text(value, "tcp_frame_role_id")),
      core::TolerancesDTO{Number(tolerances, "joint_position_tol_rad"),
                          Number(tolerances, "translation_tol_m"),
                          Number(tolerances, "rotation_tol_rad")},
      core::SpeedProfileDTO{Number(speed, "velocity_scale"),
                            Number(speed, "acceleration_scale"),
                            OptionalNumber(speed, "max_tcp_linear_speed_mps")},
      core::TimeoutsDTO{Number(timeouts, "planning_timeout_s"),
                        Number(timeouts, "execution_timeout_s")},
      cache_mode,
      planning_profile,
      target_mode,
      std::move(target_reference),
      std::move(offset_reference),
      std::move(decoded_motion),
      std::move(contact_policy),
      std::move(limits),
      {},
      std::move(runtime_objects),
      std::move(goal_frame_reference)};
}

[[nodiscard]] core::CoordinatePositionDTO
DecodeCoordinatePosition(const CanonicalValue &value) {
  RequireExactObjectSize(value, 2U, "coordinate position");
  const auto kind = Text(value, "kind");
  if (kind == "angular_rad") {
    return core::AngularPositionDTO{Number(value, "value_rad")};
  }
  if (kind == "linear_m") {
    return core::LinearPositionDTO{Number(value, "value_m")};
  }
  Fail("coordinate position kind is unsupported");
}

[[nodiscard]] core::CoordinateEffortDTO
DecodeCoordinateEffort(const CanonicalValue &value) {
  RequireExactObjectSize(value, 2U, "coordinate effort");
  const auto kind = Text(value, "kind");
  if (kind == "angular_newton_meter") {
    return core::AngularEffortDTO{Number(value, "value_nm")};
  }
  if (kind == "linear_newton") {
    return core::LinearEffortDTO{Number(value, "value_n")};
  }
  Fail("coordinate effort kind is unsupported");
}

[[nodiscard]] core::CapabilityCommandIntentDTO
DecodeCapabilityCommandPayload(const CanonicalValue &value) {
  RequireExactObjectSize(value, 6U, "capability-command payload");
  Require(Text(value, "kind") == "capability_command",
          "operation payload kind is unsupported");
  const auto &command_value = Member(value, "command_value");
  RequireExactObjectSize(command_value, 3U, "gripper-position command value");
  Require(Text(command_value, "kind") == "gripper_position",
          "capability-command value kind is unsupported");

  std::optional<core::CoordinateEffortDTO> max_effort;
  const auto &effort = Member(command_value, "max_effort");
  if (!std::holds_alternative<std::nullptr_t>(effort.storage())) {
    max_effort = DecodeCoordinateEffort(effort);
  }

  const auto &option_values = Array(value, "caller_options");
  Require(option_values.size() <= core::kMaxOperationCommandOptions,
          "capability-command options exceed their fixed bound");
  std::vector<core::CommandDurationOptionDTO> caller_options;
  caller_options.reserve(option_values.size());
  for (const auto &option : option_values) {
    RequireExactObjectSize(option, 2U, "capability-command option");
    caller_options.push_back(core::CommandDurationOptionDTO{
        core::CommandOptionId(Text(option, "option_id")),
        Number(option, "value_s")});
  }

  return core::CapabilityCommandIntentDTO{
      core::CommandSchemaId(Text(value, "command_schema_id")),
      core::CommandSchemaRevision(Text(value, "command_schema_revision")),
      core::GripperPositionCommandValueDTO{
          DecodeCoordinatePosition(Member(command_value, "position")),
          std::move(max_effort)},
      Number(value, "completion_timeout_s"), std::move(caller_options)};
}

[[nodiscard]] core::OperationParticipantPayloadDTO
DecodeParticipantPayload(const CanonicalValue &value) {
  const auto kind = Text(value, "kind");
  if (kind == "motion") {
    return DecodeMotionPayload(value);
  }
  if (kind == "capability_command") {
    return DecodeCapabilityCommandPayload(value);
  }
  Fail("operation payload kind is unsupported");
}

[[nodiscard]] std::optional<core::ProgramNodeExecutionScopeDTO>
DecodeProgramExecutionScope(const CanonicalValue &value,
                            const core::RequestAttributionDTO &attribution) {
  const auto *scope = value.find_object_member("execution_scope");
  if (scope == nullptr)
    return std::nullopt;
  Require(attribution.transport == core::RequestTransportDTO::kInternal &&
              attribution.assurance == core::RequestAssuranceDTO::kUnverified &&
              !attribution.principal_id.has_value(),
          "Program origin requires the private owner-client path");
  RequireExactObjectSize(*scope, 2U, "Program node execution scope");
  core::ProgramNodeExecutionScopeDTO decoded{
      core::ProgramNodeInstanceId(Text(*scope, "node_instance_id")),
      Text(*scope, "node_attempt_id")};
  Require(core::valid_program_node_execution_scope(decoded),
          "Program node execution scope is malformed or oversized");
  return decoded;
}

[[nodiscard]] core::OperationIntentDTO
DecodeOperation(const CanonicalValue &value,
                const core::RequestAttributionDTO &attribution) {
  const auto *manual_context = value.find_object_member("manual_handover");
  RequireExactObjectSize(value, manual_context == nullptr ? 10U : 11U,
                         "operation intent");
  std::optional<core::ManualProgramHandoverContextDTO> manual_handover;
  if (manual_context != nullptr &&
      !std::holds_alternative<std::nullptr_t>(manual_context->storage())) {
    RequireExactObjectSize(*manual_context, 4U, "manual handover context");
    manual_handover = core::ManualProgramHandoverContextDTO{
        core::RequestOwnerEpoch(
            Text(*manual_context, "program_runner_owner_epoch")),
        core::ProgramRunId(Text(*manual_context, "program_run_id")),
        core::CellAdmissionClaimGeneration(
            Counter(*manual_context, "claim_generation", 1U)),
        Counter(*manual_context, "selection_revision", 0U)};
  }
  const auto &request = Member(value, "request_context");
  const auto &participant_values = Array(value, "participants");
  Require(!participant_values.empty() &&
              participant_values.size() <= core::kMaxOperationParticipants,
          "operation participant count exceeds its bounds");
  const auto policy_name = Text(value, "coordination_policy");
  Require(policy_name == "independent" || policy_name == "coordinated_all",
          "operation coordination policy is unsupported");
  const auto policy = policy_name == "independent"
                          ? core::OperationCoordinationPolicy::kIndependent
                          : core::OperationCoordinationPolicy::kCoordinatedAll;
  Require((participant_values.size() == 1U) ==
              (policy == core::OperationCoordinationPolicy::kIndependent),
          "operation participant count differs from its coordination policy");
  std::vector<core::OperationParticipantIntentDTO> participants;
  participants.reserve(participant_values.size());
  for (const auto &participant : participant_values) {
    auto execution_scope =
        DecodeProgramExecutionScope(participant, attribution);
    RequireExactObjectSize(participant, execution_scope.has_value() ? 6U : 5U,
                           "operation participant");
    std::optional<core::ActionSpanDTO> action_span;
    const auto &span = Member(participant, "action_span");
    if (!std::holds_alternative<std::nullptr_t>(span.storage())) {
      action_span = DecodeActionSpan(span);
    }
    participants.push_back(core::OperationParticipantIntentDTO{
        core::ComponentRoleId(Text(participant, "component_role_id")),
        core::TargetRoleId(Text(participant, "target_role_id")),
        core::CapabilityId(Text(participant, "required_capability_id")),
        std::move(action_span),
        DecodeParticipantPayload(Member(participant, "payload")),
        std::move(execution_scope)});
  }
  std::optional<core::SceneRevision> expected_scene;
  const auto &scene = Member(value, "expected_scene_revision");
  if (!std::holds_alternative<std::nullptr_t>(scene.storage())) {
    expected_scene = DecodeSceneRevision(scene);
  }
  core::OperationIntentDTO result{
      DecodeRequestContext(request),
      core::ProgramId(Text(value, "program_id")),
      core::ProgramRevision(Text(value, "program_revision")),
      core::ProgramBindingId(Text(value, "program_binding_id")),
      core::ProgramRunId(Text(value, "program_run_id")),
      core::GraphRevision(Text(value, "expected_graph_revision")),
      core::RetainedBundleId(Text(value, "expected_retained_bundle_id")),
      std::move(expected_scene),
      std::move(participants),
      policy,
      attribution,
      std::move(manual_handover)};
  if (result.manual_handover) {
    const auto valid = core::validate_operation_intent(result);
    Require(valid.ok(), valid.ok() ? "" : valid.error().message);
  }
  return result;
}

[[nodiscard]] std::array<double, 3U> DecodeTriple(const CanonicalValue &root,
                                                  const std::string_view name) {
  const auto values = NumberArray(root, name, 3U);
  Require(values.size() == 3U, "command field " + std::string(name) +
                                   " must contain exactly three numbers");
  return {values[0], values[1], values[2]};
}

[[nodiscard]] core::PickOffsetEnvelopeDTO
DecodePickEnvelope(const CanonicalValue &value) {
  RequireExactObjectSize(value, 4U, "pick offset envelope");
  const auto position_min = DecodeTriple(value, "position_min_m");
  const auto position_max = DecodeTriple(value, "position_max_m");
  const auto orientation_min = DecodeTriple(value, "orientation_rpy_min_deg");
  const auto orientation_max = DecodeTriple(value, "orientation_rpy_max_deg");
  for (std::size_t index = 0U; index < 3U; ++index) {
    Require(position_min[index] <= position_max[index] &&
                orientation_min[index] <= orientation_max[index],
            "pick offset envelope minimum exceeds its maximum");
  }
  return {position_min, position_max, orientation_min, orientation_max};
}

[[nodiscard]] core::PickCandidateSynthesisIntentDTO
DecodePickSynthesis(const CanonicalValue &value) {
  RequireExactObjectSize(value, 11U, "pick synthesis intent");
  core::PickCandidateSearchStrategyDTO strategy;
  const auto strategy_name = Text(value, "search_strategy");
  if (strategy_name == "first_feasible") {
    strategy = core::PickCandidateSearchStrategyDTO::kFirstFeasible;
  } else if (strategy_name == "best_cost") {
    strategy = core::PickCandidateSearchStrategyDTO::kBestCost;
  } else {
    Fail("pick search strategy is unsupported");
  }
  const auto maximum = Counter(value, "maximum_candidate_count", 1U);
  Require(maximum <= 4096U,
          "pick maximum candidate count exceeds its fixed bound");
  const auto translation_step = Number(value, "translation_step_m");
  const auto rotation_step = Number(value, "rotation_step_deg");
  const auto min_tcp_z = OptionalNumber(value, "min_tcp_z_m");
  Require(translation_step > 0.0 && rotation_step > 0.0 &&
              (!min_tcp_z.has_value() || *min_tcp_z >= 0.0),
          "pick synthesis step or minimum TCP height is invalid");
  return core::PickCandidateSynthesisIntentDTO{
      DecodePickEnvelope(Member(value, "pick_offset_envelope")),
      DecodePickEnvelope(Member(value, "approach_offset_envelope")),
      DecodeOptionalReference(value, "pick_offset_reference", true),
      DecodeOptionalReference(value, "approach_offset_reference", true),
      DecodeTriple(value, "base_tool_orientation_rpy_deg"),
      translation_step,
      rotation_step,
      Boolean(value, "force_vertical_pick"),
      min_tcp_z,
      static_cast<std::size_t>(maximum),
      strategy};
}

[[nodiscard]] core::PickSequencePlanningIntentDTO
DecodePickPlanningIntent(const CanonicalValue &value,
                         const core::RequestAttributionDTO &attribution) {
  RequireExactObjectSize(value, 9U, "pick planning intent");
  return core::PickSequencePlanningIntentDTO{
      Text(value, "sequence_id"),
      core::SceneResourceId(Text(value, "selected_object_id")),
      core::RuntimeObjectId(Text(value, "selected_runtime_object_id")),
      core::FrameRoleId(Text(value, "carrier_frame_role_id")),
      DecodePickSynthesis(Member(value, "synthesis")),
      DecodeOperation(Member(value, "approach_operation"), attribution),
      DecodeOperation(Member(value, "descend_operation"), attribution),
      DecodeOperation(Member(value, "tool_operation"), attribution),
      DecodeOperation(Member(value, "retreat_operation"), attribution)};
}

[[nodiscard]] core::PlaceSequencePlanningIntentDTO
DecodePlacePlanningIntent(const CanonicalValue &value,
                          const core::RequestAttributionDTO &attribution) {
  RequireExactObjectSize(value, 7U, "place planning intent");
  return core::PlaceSequencePlanningIntentDTO{
      Text(value, "sequence_id"),
      core::SceneResourceId(Text(value, "selected_object_id")),
      core::FrameRoleId(Text(value, "carrier_frame_role_id")),
      DecodeOperation(Member(value, "approach_operation"), attribution),
      DecodeOperation(Member(value, "descend_operation"), attribution),
      DecodeOperation(Member(value, "tool_operation"), attribution),
      DecodeOperation(Member(value, "retreat_operation"), attribution)};
}

[[nodiscard]] core::OperationControlRequestDTO
DecodeOperationControlRequest(const CanonicalValue &value,
                              const core::RequestAttributionDTO &attribution) {
  auto execution_scope = DecodeProgramExecutionScope(value, attribution);
  RequireExactObjectSize(value, execution_scope.has_value() ? 5U : 4U,
                         "operation control request");
  return core::OperationControlRequestDTO{
      DecodeRequestContext(Member(value, "request_context")),
      core::OperationId(Text(value, "operation_id")),
      core::OperationRevision(
          Counter(value, "expected_operation_revision", 1U)),
      OptionalBoundedText(value, "reason", kMaximumReasonBytes),
      attribution,
      std::move(execution_scope)};
}

[[nodiscard]] core::StopIntentDTO
DecodeStopIntent(const CanonicalValue &value,
                 const core::RequestAttributionDTO &attribution) {
  Require(value.is_object() && (value.as_object().size() == 2U ||
                                value.as_object().size() == 3U),
          "Stop intent has an unknown or missing field");
  const auto selector_count =
      static_cast<std::size_t>(value.find_object_member("operations") !=
                               nullptr) +
      static_cast<std::size_t>(value.find_object_member("components") !=
                               nullptr) +
      static_cast<std::size_t>(value.find_object_member("resources") !=
                               nullptr);
  Require(selector_count == 1U,
          "Stop intent must contain exactly one selector kind");
  core::StopSelectorDTO selector;
  if (value.find_object_member("operations") != nullptr) {
    std::vector<core::StopOperationTargetDTO> targets;
    const auto &rows = Array(value, "operations");
    Require(!rows.empty() && rows.size() <= core::kMaximumStopSelectorTargets,
            "Stop operation selector exceeds its row bound");
    targets.reserve(rows.size());
    for (const auto &target : rows) {
      RequireExactObjectSize(target, 3U, "Stop operation target");
      targets.push_back(core::StopOperationTargetDTO{
          core::OperationId(Text(target, "operation_id")),
          core::OperationRevision(
              Counter(target, "expected_operation_revision", 1U)),
          core::StopGeneration(
              Counter(target, "expected_stop_generation", 0U))});
    }
    selector = std::move(targets);
  } else if (value.find_object_member("components") != nullptr) {
    std::vector<core::StopComponentTargetDTO> targets;
    const auto &rows = Array(value, "components");
    Require(!rows.empty() && rows.size() <= core::kMaximumStopSelectorTargets,
            "Stop component selector exceeds its row bound");
    targets.reserve(rows.size());
    for (const auto &target : rows) {
      RequireExactObjectSize(target, 3U, "Stop component target");
      targets.push_back(core::StopComponentTargetDTO{
          core::ControllableComponentId(
              Text(target, "controllable_component_id")),
          core::GraphRevision(Text(target, "expected_graph_revision")),
          core::ComponentLifecycleGeneration(
              Counter(target, "expected_component_lifecycle_generation", 1U))});
    }
    selector = std::move(targets);
  } else {
    std::vector<core::StopResourceTargetDTO> targets;
    const auto &rows = Array(value, "resources");
    Require(!rows.empty() && rows.size() <= core::kMaximumStopSelectorTargets,
            "Stop resource selector exceeds its row bound");
    targets.reserve(rows.size());
    for (const auto &target : rows) {
      RequireExactObjectSize(target, 4U, "Stop resource target");
      targets.push_back(core::StopResourceTargetDTO{
          core::ResourceId(Text(target, "resource_id")),
          core::GraphRevision(Text(target, "expected_graph_revision")),
          Counter(target, "expected_operation_resource_generation", 0U),
          core::StopGeneration(
              Counter(target, "expected_stop_generation", 0U))});
    }
    selector = std::move(targets);
  }
  std::optional<std::string> reason;
  if (value.find_object_member("reason") != nullptr) {
    reason = OptionalBoundedText(value, "reason", kMaximumReasonBytes);
  }
  return core::StopIntentDTO{
      DecodeRequestContext(Member(value, "request_context")),
      std::move(selector), std::move(reason), attribution};
}

[[nodiscard]] core::StopIntentDTO
DecodeProgramRunStopIntent(const CanonicalValue &value,
                           const core::RequestAttributionDTO &attribution) {
  Require(value.is_object() && (value.as_object().size() == 2U ||
                                value.as_object().size() == 3U),
          "Program-run Stop intent has an unknown or missing field");
  std::optional<std::string> reason;
  if (value.find_object_member("reason") != nullptr) {
    reason = OptionalBoundedText(value, "reason", kMaximumReasonBytes);
  }
  return core::StopIntentDTO{
      DecodeRequestContext(Member(value, "request_context")),
      std::vector<core::StopProgramRunTargetDTO>{core::StopProgramRunTargetDTO{
          core::ProgramRunId(Text(value, "program_run_id"))}},
      std::move(reason), attribution};
}

[[nodiscard]] core::FullStopIntentDTO
DecodeFullStopIntent(const CanonicalValue &value,
                     const core::RequestAttributionDTO &attribution) {
  RequireExactObjectSize(value, 2U, "Full Stop intent");
  return core::FullStopIntentDTO{
      DecodeRequestContext(Member(value, "request_context")),
      core::StopGeneration(Counter(value, "expected_stop_generation", 0U)),
      attribution};
}

[[nodiscard]] core::StopFaultRecoveryIntentDTO
DecodeStopFaultRecoveryIntent(const CanonicalValue &value) {
  Require(value.is_object() && (value.as_object().size() == 10U ||
                                value.as_object().size() == 11U),
          "Stop fault-recovery intent has an unknown or missing field");
  std::vector<core::StopFaultRecoveryConfigurationExpectationDTO> expectations;
  const auto &rows = Array(value, "configuration_expectations");
  Require(!rows.empty() &&
              rows.size() <= core::kMaximumStopFaultRecoveryEvidenceRefs,
          "Stop fault-recovery configuration table exceeds its row bound");
  expectations.reserve(rows.size());
  for (const auto &row : rows) {
    RequireExactObjectSize(row, 3U,
                           "Stop fault-recovery configuration expectation");
    expectations.push_back(core::StopFaultRecoveryConfigurationExpectationDTO{
        core::ControlDomainId(Text(row, "control_domain_id")),
        core::ActiveControlConfigurationId(
            Text(row, "active_control_configuration_id")),
        core::ActiveControlConfigurationGeneration(
            Counter(row, "active_control_configuration_generation", 1U))});
  }
  std::optional<std::string> reason;
  if (value.find_object_member("reason") != nullptr) {
    reason = OptionalBoundedText(value, "reason", kMaximumReasonBytes);
  }
  return core::StopFaultRecoveryIntentDTO{
      DecodeRequestContext(Member(value, "request_context")),
      core::StopFaultLatchId(Text(value, "expected_latch_id")),
      core::StopFaultLatchGeneration(
          Counter(value, "expected_latch_generation", 1U)),
      Counter(value, "expected_recovery_assessment_revision", 1U),
      core::StopCertificateId(Text(value, "expected_stop_certificate_id")),
      core::StopCertificateRevision(
          Counter(value, "expected_stop_certificate_revision", 1U)),
      core::CellId(Text(value, "expected_cell_id")),
      core::RetainedBundleId(Text(value, "expected_retained_bundle_id")),
      core::GraphRevision(Text(value, "expected_graph_revision")),
      std::move(expectations),
      std::move(reason)};
}

[[nodiscard]] core::RuntimeScaleControlSelectorDTO
DecodeRuntimeScaleControlSelector(const CanonicalValue &value) {
  if (value.find_object_member("group_targets") == nullptr) {
    RequireExactObjectSize(value, 3U, "runtime-scale control selector");
    return core::RuntimeScaleControlSelectorDTO{
        core::ProgramBindingId(Text(value, "program_binding_id")),
        core::ComponentRoleId(Text(value, "component_role_id")),
        core::TargetRoleId(Text(value, "target_role_id"))};
  }
  RequireExactObjectSize(value, 2U, "runtime-scale group selector");
  const auto &targets = Array(value, "group_targets");
  Require(targets.size() >= 2U &&
              targets.size() <= core::kMaxOperationParticipants,
          "runtime-scale group selector target count is out of bounds");
  std::vector<core::RuntimeScaleControlTargetDTO> decoded;
  decoded.reserve(targets.size());
  for (const auto &target : targets) {
    RequireExactObjectSize(target, 2U, "runtime-scale group target");
    core::RuntimeScaleControlTargetDTO item{
        core::ComponentRoleId(Text(target, "component_role_id")),
        core::TargetRoleId(Text(target, "target_role_id"))};
    Require(std::none_of(decoded.begin(), decoded.end(),
                         [&](const auto &prior) { return prior == item; }),
            "runtime-scale group selector contains duplicate targets");
    decoded.push_back(std::move(item));
  }
  return core::RuntimeScaleControlSelectorDTO{
      core::ProgramBindingId(Text(value, "program_binding_id")),
      core::ComponentRoleId(""), core::TargetRoleId(""), std::move(decoded)};
}

[[nodiscard]] core::RuntimeScaleControlRequestDTO
DecodeRuntimeScaleControlRequest(
    const CanonicalValue &value,
    const core::RequestAttributionDTO &attribution) {
  RequireExactObjectSize(value, 4U, "runtime-scale control request");
  const double scale = Number(value, "scale");
  Require(scale >= 0.0 && scale <= 1.0,
          "runtime scale must be in the closed interval [0, 1]");
  return core::RuntimeScaleControlRequestDTO{
      DecodeRequestContext(Member(value, "request_context")),
      DecodeRuntimeScaleControlSelector(Member(value, "selector")),
      core::RuntimeScaleControlGeneration(
          Counter(value, "expected_control_generation", 0U)),
      scale, attribution};
}

[[nodiscard]] core::RecoveryAction
DecodeRecoveryAction(const std::string_view value) {
  if (value == "retry_motion_from_current_state") {
    return core::RecoveryAction::kRetryMotionFromCurrentState;
  }
  if (value == "acknowledge_and_continue") {
    return core::RecoveryAction::kAcknowledgeAndContinue;
  }
  if (value == "retry_after_scene_repair") {
    return core::RecoveryAction::kRetryAfterSceneRepair;
  }
  if (value == "replan_from_stopped_state") {
    return core::RecoveryAction::kReplanFromStoppedState;
  }
  if (value == "reset_cycle") {
    return core::RecoveryAction::kResetCycle;
  }
  Fail("operation recovery action is unsupported");
}

[[nodiscard]] core::KernelRecoveryActionRequestDTO
DecodeOperationRecoveryRequest(const CanonicalValue &value) {
  RequireExactObjectSize(value, 10U, "operation recovery request");
  return core::KernelRecoveryActionRequestDTO{
      Text(value, "request_id"),
      DecodeRecoveryAction(Text(value, "action")),
      Text(value, "principal_id", true),
      Counter(value, "expected_assessment_revision", 1U),
      Counter(value, "expected_control_command_epoch", 0U),
      Counter(value, "expected_execution_id", 1U),
      Text(value, "expected_latch_id", true),
      Text(value, "expected_stop_certificate_id", true),
      Counter(value, "expected_stop_owner_generation", 0U),
      Text(value, "reason", true)};
}

enum class CommandKind {
  kStatus,
  kIdentity,
  kTargets,
  kQueryTarget,
  kConfirmGrasp,
  kQueryOperation,
  kControlState,
  kObservation,
  kEvents,
  kPreviewMotion,
  kReadManualHandover,
  kPreviewCapabilityCommand,
  kRegisterProgramCapture,
  kSubmit,
  kRead,
  kReadStop,
  kGetOperationRecovery,
  kApplyOperationRecovery,
  kPause,
  kResume,
  kReset,
  kAcknowledgeCompleted,
  kStop,
  kStopProgramRun,
  kStopProgramSelection,
  kReadProgramExecutionScope,
  kStartProgramSelection,
  kReleaseProgramSelection,
  kCheckProgramNodeMotionAttention,
  kReleaseProgramRunStopFence,
  kFullStop,
  kMalformedStop,
  kGetStopFault,
  kRecoverStopFault,
  kSetRuntimeScaleControl,
  kSetOperationRuntimeScale,
  kGetRuntimeScaleControl,
  kPlanPick,
  kPlanPlace,
};

enum class CommandLane {
  kOrdinary,
  kStop,
  kObservation,
};

/// Commands whose addressed-Kernel entry takes its submission ordering. A
/// motion-preview session (and pick/place planning built on one) retains that
/// ordering for its whole planning transaction, so these commands run on the
/// submission-ordered lane, FIFO in accept order, and never occupy the
/// ordinary lane that serves Stop status, Reset, events, cell admission and
/// operator-alarm attention.
[[nodiscard]] bool IsSubmissionOrdered(const CommandKind kind) noexcept {
  switch (kind) {
  case CommandKind::kPreviewMotion:
  case CommandKind::kPlanPick:
  case CommandKind::kPlanPlace:
  case CommandKind::kPreviewCapabilityCommand:
  case CommandKind::kReadManualHandover:
  case CommandKind::kSubmit:
  case CommandKind::kSetRuntimeScaleControl:
    return true;
  default:
    return false;
  }
}

/// Ordinary projections that first read the live Scene revision from Manager.
/// That round trip waits on Manager (for example during an Apply), so it runs
/// on the Scene-read lane and never occupies the ordinary lane.
[[nodiscard]] bool ReadsManagerScene(const CommandKind kind) noexcept {
  return kind == CommandKind::kIdentity || kind == CommandKind::kStatus ||
         kind == CommandKind::kTargets || kind == CommandKind::kControlState;
}

/// In-memory Stop-progression reads of the stable Kernel. They are served
/// beside Stop on the independent control socket, never on the ordinary one.
[[nodiscard]] bool IsStopLaneRead(const CommandKind kind) noexcept {
  return kind == CommandKind::kReadStop || kind == CommandKind::kGetStopFault;
}

/// Every Stop form preempts advisory planning before it is dispatched: a
/// preview is effect-free and re-requestable, and containment never waits
/// for one. An accepted Reset preempts it after acceptance (a refused Reset
/// changed nothing a preview depends on).
[[nodiscard]] bool PreemptsPlanning(const CommandKind kind) noexcept {
  switch (kind) {
  case CommandKind::kStop:
  case CommandKind::kStopProgramRun:
  case CommandKind::kStopProgramSelection:
  case CommandKind::kFullStop:
  case CommandKind::kMalformedStop:
    return true;
  default:
    return false;
  }
}

/// A component-lifecycle claim is a whole-cell configuration mutation: the
/// Kernel serializes it with the submission lock a preview session retains
/// for its whole planning time. Like Stop, it preempts advisory planning
/// before it is dispatched instead of waiting behind it. Other claim kinds
/// never take that lock.
[[nodiscard]] bool PreemptsPlanning(
    const component_manager::detail::KernelCellAdmissionRpcPacket
        &packet) noexcept {
  const auto *intent =
      std::get_if<core::CellAdmissionClaimIntentDTO>(&packet.request);
  return intent != nullptr &&
         intent->claim_kind == core::CellAdmissionClaimKindDTO::kComponentLifecycle;
}

struct MalformedStopCommand final {
  core::RequestId request_id;
  core::RequestAttributionDTO attribution;
  std::string recognized_route;
  std::string reason;
};

/// `frame_transform`: express `pose` (given in `source_frame`) in
/// `target_frame` through the cell frame resolver. The canonical request
/// members are retained verbatim for the response echo.
struct FrameTransformQuery final {
  core::Pose3DTO pose;
  core::ReferenceDTO source_frame;
  core::ReferenceDTO target_frame;
  CanonicalValue pose_value;
  CanonicalValue source_frame_value;
  CanonicalValue target_frame_value;
};

struct TargetObservationQuery final {
  core::RequestOwnerEpoch owner_epoch;
  core::ProgramBindingId program_binding_id;
  core::TargetRoleId target_role_id;
  std::optional<core::FrameRoleId> frame_role_id;
  std::optional<core::FrameRoleId> relative_to_frame_role_id;
  bool grasp_confirmation{};
  std::optional<FrameTransformQuery> frame_transform{};
};

struct GripperConfirmationCommand final {
  core::PublicRequestContextDTO context;
  core::ProgramBindingId program_binding_id;
  core::TargetRoleId target_role_id;
  core::GripperGraspObservationDTO observation;
  core::RequestAttributionDTO attribution;
  core::TimePoint admission_deadline_ns;
};

struct ProgramOperationQuery final {
  core::RequestOwnerEpoch owner_epoch;
  core::ProgramBindingId program_binding_id;
  core::ProgramRunId program_run_id;
  core::TargetRoleId target_role_id;
};

struct Command final {
  explicit Command(CommandKind command_kind) : kind(command_kind) {}

  CommandKind kind;
  std::optional<TargetObservationQuery> target_query;
  std::optional<GripperConfirmationCommand> confirmation;
  std::optional<ProgramOperationQuery> operation_query;
  std::optional<core::OperationIntentDTO> operation;
  std::optional<core::ProgramRunExecutionCaptureDTO> program_capture;
  std::optional<core::ProgramExecutionSelectionRequestDTO> program_selection;
  std::optional<core::ProgramNodeMotionAttentionCheckDTO> program_node_attention;
  std::uint64_t expected_selection_revision{};
  std::optional<core::ContentDigest> operation_digest;
  std::optional<core::OperationId> operation_id;
  std::optional<core::StopId> stop_id;
  std::optional<core::ProgramRunId> program_run_id;
  std::optional<core::StopGeneration> stop_generation;
  std::optional<core::OperationControlRequestDTO> control;
  std::optional<core::OperationRevision> expected_operation_revision;
  std::optional<core::KernelRecoveryActionRequestDTO> operation_recovery;
  std::optional<core::StopIntentDTO> stop;
  std::optional<core::FullStopIntentDTO> full_stop;
  std::optional<MalformedStopCommand> malformed_stop;
  std::optional<core::StopFaultRecoveryIntentDTO> stop_fault_recovery;
  std::optional<core::RuntimeScaleControlRequestDTO> runtime_scale_control;
  std::optional<core::RuntimeScaleRequestDTO> operation_runtime_scale;
  std::optional<core::RuntimeScaleControlSelectorDTO>
      runtime_scale_control_selector;
  std::optional<core::RequestOwnerEpoch> event_owner_epoch{};
  std::uint64_t after_sequence{};
  std::size_t event_limit{};
  std::optional<core::RequestOwnerEpoch> observation_after_owner_epoch;
  std::optional<std::string> observation_after_source_epoch;
  std::optional<std::uint64_t> observation_after_source_sequence;
  bool observation_cursor_supplied{};
  std::optional<core::PickSequencePlanningIntentDTO> pick_planning;
  std::optional<core::PlaceSequencePlanningIntentDTO> place_planning;
};

[[nodiscard]] Command DecodeCommand(std::string_view bytes) {
  Require(!bytes.empty() && bytes.size() <= kMaximumPacketBytes,
          "execution command packet is empty or oversized");
  CanonicalValue root;
  try {
    root = deployment::parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception &error) {
    Fail("execution command is not strict JSON: " + std::string(error.what()));
  }
  Require(content_identity::canonical_json(root) == bytes,
          "execution command must use canonical JSON encoding");
  Require(root.is_object(), "execution command root must be an object");
  Require(Text(root, "api_version") == kCommandApiVersion,
          "execution command API version is unsupported");
  const auto command = Text(root, "command");
  if (command == "stop_program_selection" ||
      command == "start_program_selection" ||
      command == "release_program_selection") {
    const bool start = command == "start_program_selection";
    const bool release = command == "release_program_selection";
    RequireExactObjectSize(root, (start || release) ? 5U : 4U,
                           "Program selection command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Program selection requires internal owner attribution");
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 6U, "Program selection request");
    Require(Text(request, "schema_version") ==
                "manyforge.program_execution_selection.v2",
            "Program selection request schema is unsupported");
    Command decoded{start   ? CommandKind::kStartProgramSelection
                    : release ? CommandKind::kReleaseProgramSelection
                              : CommandKind::kStopProgramSelection};
    decoded.program_selection = core::ProgramExecutionSelectionRequestDTO{
        DecodeRequestContext(Member(request, "request_context")),
        core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
        core::CellAdmissionClaimGeneration(
            Counter(request, "claim_generation", 1U)),
        core::ProgramRunId(Text(request, "program_run_id")),
        TextArray(request, "group_ids", core::kMaxProgramExecutionGroups),
        attribution};
    if (start || release)
      decoded.expected_selection_revision =
          Counter(root, "expected_selection_revision", 0U);
    return decoded;
  }
  if (command == "check_program_node_motion_attention") {
    RequireExactObjectSize(root, 4U, "Program node motion-attention check");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Program node check requires internal owner attribution");
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 6U, "Program node check request");
    Require(Text(request, "schema_version") ==
                "manyforge.program_node_motion_attention_check.v1",
            "Program node check request schema is unsupported");
    Command decoded{CommandKind::kCheckProgramNodeMotionAttention};
    decoded.program_node_attention = core::ProgramNodeMotionAttentionCheckDTO{
        DecodeRequestContext(Member(request, "request_context")),
        core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
        core::CellAdmissionClaimGeneration(
            Counter(request, "claim_generation", 1U)),
        core::ProgramRunId(Text(request, "program_run_id")),
        core::ProgramNodeInstanceId(Text(request, "node_instance_id")),
        attribution};
    return decoded;
  }
  if (command == "read_program_execution_scope") {
    RequireExactObjectSize(root, 3U, "Program execution scope read");
    Command decoded{CommandKind::kReadProgramExecutionScope};
    decoded.program_run_id = core::ProgramRunId(Text(root, "program_run_id"));
    return decoded;
  }
  if (command == "register_program_run_capture") {
    RequireExactObjectSize(root, 3U, "Program capture registration command");
    const auto &capture = Member(root, "capture");
    RequireExactObjectSize(capture, 10U, "Program capture registration");
    Require(Text(capture, "schema_version") ==
                "manyforge.program_run_execution_capture.v1",
            "Program capture registration schema is unsupported");
    Command decoded{CommandKind::kRegisterProgramCapture};
    decoded.program_capture = core::ProgramRunExecutionCaptureDTO{
        core::RequestOwnerEpoch(Text(capture, "request_owner_epoch")),
        core::RequestOwnerEpoch(Text(capture, "program_runner_owner_epoch")),
        core::CellAdmissionClaimGeneration(
            Counter(capture, "claim_generation", 1U)),
        core::ProgramRunId(Text(capture, "program_run_id")),
        core::ProgramId(Text(capture, "program_id")),
        core::ProgramRevision(Text(capture, "program_revision")),
        core::ProgramBindingId(Text(capture, "program_binding_id")),
        core::RetainedBundleId(Text(capture, "retained_bundle_id")),
        core::GraphRevision(Text(capture, "graph_revision")),
        {}};
    return decoded;
  }
  if (command == "status") {
    RequireExactObjectSize(root, 2U, "status command");
    return Command{CommandKind::kStatus};
  }
  if (command == "identity") {
    RequireExactObjectSize(root, 2U, "identity command");
    return Command{CommandKind::kIdentity};
  }
  if (command == "targets") {
    RequireExactObjectSize(root, 2U, "targets command");
    return Command{CommandKind::kTargets};
  }
  if (command == "gripper_confirm") {
    RequireExactObjectSize(root, 7U, "gripper confirmation command");
    const auto &request = Member(root, "request");
    RequireExactObjectSize(request, 8U, "gripper confirmation request");
    Require(Text(request, "schema_version") ==
                "manyforge.gripper_confirmation_request.v1",
            "gripper confirmation schema is unsupported");
    auto context = DecodeRequestContext(Member(request, "request_context"));
    const auto origin = Text(root, "origin_epoch");
    Require(!origin.empty() && origin.size() <= 256U,
            "confirmation ingress origin is invalid");
    const auto observation_id = content_identity::sha256_digest(
        canonical_json(CanonicalValue(CanonicalValue::Array{
            CanonicalValue(origin),
            CanonicalValue(context.request_id.value())})));
    Command decoded{CommandKind::kConfirmGrasp};
    decoded.confirmation = GripperConfirmationCommand{
        context,
        core::ProgramBindingId(Text(request, "program_binding_id")),
        core::TargetRoleId(Text(request, "target_role_id")),
        core::GripperGraspObservationDTO{
            {},
            Counter(request, "command_id", 1U),
            Counter(request, "generation", 1U),
            Counter(request, "expected_status_revision", 1U),
            observation_id,
            Boolean(request, "confirmed"),
            content_identity::parse_nonnegative_int64(
                Member(root, "received_monotonic_ns")),
            context.source_label},
        DecodeRequestAttribution(Member(root, "attribution")),
        content_identity::parse_nonnegative_int64(
            Member(root, "valid_until_monotonic_ns"))};
    return decoded;
  }
  if (command == "query_target") {
    RequireExactObjectSize(root, 3U, "target observation command");
    const auto &query = Member(root, "query");
    const auto kind = Text(query, "kind");
    if (kind == "frame_transform") {
      RequireExactObjectSize(query, 7U, "frame transform query");
      Require(Text(query, "schema_version") ==
                  "manyforge.target_observation_query.v1",
              "target observation query schema is unsupported");
      const auto &pose = Member(query, "pose");
      const auto &source = Member(query, "source_frame");
      const auto &target = Member(query, "target_frame");
      Command decoded{CommandKind::kQueryTarget};
      decoded.target_query = TargetObservationQuery{
          core::RequestOwnerEpoch(Text(query, "owner_epoch")),
          core::ProgramBindingId(Text(query, "program_binding_id")),
          core::TargetRoleId(std::string{}),
          std::nullopt,
          std::nullopt,
          false,
          FrameTransformQuery{DecodePose(pose), DecodeReference(source, true),
                              DecodeReference(target, true), pose, source,
                              target}};
      return decoded;
    }
    Require(kind == "frame_pose" || kind == "gripper_status" ||
                kind == "gripper_confirmation",
            "target observation kind is unsupported");
    if (kind == "frame_pose") {
      Require(query.as_object().size() == 6U || query.as_object().size() == 7U,
              "target observation query has an unexpected field count");
    } else {
      RequireExactObjectSize(query, 5U, "target observation query");
    }
    Require(Text(query, "schema_version") ==
                "manyforge.target_observation_query.v1",
            "target observation query schema is unsupported");
    const auto relative_frame =
        std::find_if(query.as_object().begin(), query.as_object().end(),
                     [](const auto &member) {
                       return member.first == "relative_to_frame_role_id";
                     });
    Command decoded{CommandKind::kQueryTarget};
    decoded.target_query = TargetObservationQuery{
        core::RequestOwnerEpoch(Text(query, "owner_epoch")),
        core::ProgramBindingId(Text(query, "program_binding_id")),
        core::TargetRoleId(Text(query, "target_role_id")),
        kind == "frame_pose"
            ? std::optional(core::FrameRoleId(Text(query, "frame_role_id")))
            : std::nullopt,
        kind == "frame_pose" && relative_frame != query.as_object().end()
            ? std::optional(
                  core::FrameRoleId(Text(query, "relative_to_frame_role_id")))
            : std::nullopt,
        kind == "gripper_confirmation"};
    return decoded;
  }
  if (command == "query_operation") {
    RequireExactObjectSize(root, 3U, "Program operation query command");
    const auto &query = Member(root, "query");
    RequireExactObjectSize(query, 5U, "Program operation query");
    Require(Text(query, "schema_version") ==
                "manyforge.program_operation_query.v1",
            "Program operation query schema is unsupported");
    Command decoded{CommandKind::kQueryOperation};
    decoded.operation_query = ProgramOperationQuery{
        core::RequestOwnerEpoch(Text(query, "owner_epoch")),
        core::ProgramBindingId(Text(query, "program_binding_id")),
        core::ProgramRunId(Text(query, "program_run_id")),
        core::TargetRoleId(Text(query, "target_role_id"))};
    return decoded;
  }
  if (command == "control_state") {
    RequireExactObjectSize(root, 2U, "control-state command");
    return Command{CommandKind::kControlState};
  }
  if (command == "observation") {
    Require(root.as_object().size() == 2U || root.as_object().size() == 5U,
            "observation command has an unexpected field count");
    Command decoded{CommandKind::kObservation};
    if (root.as_object().size() == 5U) {
      decoded.observation_cursor_supplied = true;
      const auto owner_epoch = OptionalText(root, "after_owner_epoch");
      const auto source_epoch = OptionalText(root, "after_source_epoch");
      const auto source_sequence =
          OptionalCounter(root, "after_source_sequence", 0U);
      const bool initial = !owner_epoch.has_value() &&
                           !source_epoch.has_value() &&
                           !source_sequence.has_value();
      const bool complete = owner_epoch.has_value() &&
                            source_epoch.has_value() &&
                            source_sequence.has_value();
      Require(initial || complete,
              "observation cursor must be wholly null or complete");
      if (complete) {
        decoded.observation_after_owner_epoch =
            core::RequestOwnerEpoch(*owner_epoch);
        decoded.observation_after_source_epoch = *source_epoch;
        decoded.observation_after_source_sequence = *source_sequence;
      }
    }
    return decoded;
  }
  if (command == "events") {
    RequireExactObjectSize(root, 5U, "events command");
    const auto limit = Counter(root, "limit", 1U);
    Require(limit <= kMaximumEventPage,
            "events command exceeds its page bound");
    Command decoded{CommandKind::kEvents};
    decoded.event_owner_epoch =
        core::RequestOwnerEpoch(Text(root, "owner_epoch"));
    decoded.after_sequence = Counter(root, "after_sequence", 0U);
    decoded.event_limit = static_cast<std::size_t>(limit);
    return decoded;
  }
  if (command == "read") {
    RequireExactObjectSize(root, 3U, "read command");
    Command decoded{CommandKind::kRead};
    decoded.operation_id = core::OperationId(Text(root, "operation_id"));
    return decoded;
  }
  if (command == "read_stop") {
    RequireExactObjectSize(root, 4U, "Stop-status read command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Stop-status read requires internal unverified attribution");
    Command decoded{CommandKind::kReadStop};
    decoded.stop_id = core::StopId(Text(root, "stop_id"));
    return decoded;
  }
  if (command == "get_operation_recovery") {
    RequireExactObjectSize(root, 3U, "operation recovery query command");
    Command decoded{CommandKind::kGetOperationRecovery};
    decoded.operation_id = core::OperationId(Text(root, "operation_id"));
    return decoded;
  }
  if (command == "apply_operation_recovery") {
    RequireExactObjectSize(root, 5U, "operation recovery action command");
    Command decoded{CommandKind::kApplyOperationRecovery};
    decoded.operation_id = core::OperationId(Text(root, "operation_id"));
    decoded.expected_operation_revision = core::OperationRevision(
        Counter(root, "expected_operation_revision", 1U));
    decoded.operation_recovery =
        DecodeOperationRecoveryRequest(Member(root, "request"));
    return decoded;
  }
  if (command == "get_runtime_scale_control") {
    RequireExactObjectSize(root, 3U, "runtime-scale control read command");
    Command decoded{CommandKind::kGetRuntimeScaleControl};
    decoded.runtime_scale_control_selector =
        DecodeRuntimeScaleControlSelector(Member(root, "selector"));
    return decoded;
  }
  if (command == "submit") {
    RequireExactObjectSize(root, 4U, "submit command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kSubmit};
    decoded.operation = DecodeOperation(Member(root, "operation"), attribution);
    return decoded;
  }
  if (command == "preview_motion" || command == "read_manual_handover") {
    RequireExactObjectSize(root, 4U, "motion preview command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    const auto &operation = Member(root, "operation");
    Command decoded{command == "preview_motion"
                        ? CommandKind::kPreviewMotion
                        : CommandKind::kReadManualHandover};
    decoded.operation = DecodeOperation(operation, attribution);
    decoded.operation_digest =
        core::ContentDigest(content_identity::sha256_digest(
            content_identity::canonical_json(operation)));
    return decoded;
  }
  if (command == "preview_capability_command") {
    RequireExactObjectSize(root, 4U, "capability-command preview command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    const auto &operation = Member(root, "operation");
    Command decoded{CommandKind::kPreviewCapabilityCommand};
    decoded.operation = DecodeOperation(operation, attribution);
    decoded.operation_digest =
        core::ContentDigest(content_identity::sha256_digest(
            content_identity::canonical_json(operation)));
    return decoded;
  }
  if (command == "plan_pick") {
    RequireExactObjectSize(root, 4U, "pick planning command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kPlanPick};
    decoded.pick_planning =
        DecodePickPlanningIntent(Member(root, "intent"), attribution);
    return decoded;
  }
  if (command == "plan_place") {
    RequireExactObjectSize(root, 4U, "place planning command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kPlanPlace};
    decoded.place_planning =
        DecodePlacePlanningIntent(Member(root, "intent"), attribution);
    return decoded;
  }
  if (command == "stop") {
    RequireExactObjectSize(root, 4U, "Stop command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kStop};
    decoded.stop = DecodeStopIntent(Member(root, "request"), attribution);
    return decoded;
  }
  if (command == "stop_program_run") {
    RequireExactObjectSize(root, 4U, "Program-run Stop command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Program-run Stop requires internal unverified attribution");
    Command decoded{CommandKind::kStopProgramRun};
    decoded.stop =
        DecodeProgramRunStopIntent(Member(root, "request"), attribution);
    return decoded;
  }
  if (command == "release_program_run_stop_fence") {
    RequireExactObjectSize(root, 6U, "Program-run Stop fence release command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Require(attribution.transport == core::RequestTransportDTO::kInternal &&
                attribution.assurance ==
                    core::RequestAssuranceDTO::kUnverified &&
                !attribution.principal_id.has_value(),
            "Program-run Stop fence release requires internal attribution");
    Command decoded{CommandKind::kReleaseProgramRunStopFence};
    decoded.program_run_id = core::ProgramRunId(Text(root, "program_run_id"));
    decoded.stop_id = core::StopId(Text(root, "stop_id"));
    decoded.stop_generation =
        core::StopGeneration(Counter(root, "stop_generation", 1U));
    return decoded;
  }
  if (command == "full_stop") {
    RequireExactObjectSize(root, 4U, "Full Stop command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kFullStop};
    decoded.full_stop =
        DecodeFullStopIntent(Member(root, "request"), attribution);
    return decoded;
  }
  if (command == "malformed_stop") {
    RequireExactObjectSize(root, 6U, "recognized malformed Stop command");
    const auto route = Text(root, "recognized_route");
    Require(route == "/api/execution/stops" ||
                route == "/api/execution/full-stop",
            "recognized malformed Stop command names an unknown route");
    Command decoded{CommandKind::kMalformedStop};
    decoded.malformed_stop = MalformedStopCommand{
        core::RequestId(Text(root, "request_id")),
        DecodeRequestAttribution(Member(root, "attribution")), route,
        Text(root, "reason")};
    return decoded;
  }
  if (command == "get_stop_fault") {
    RequireExactObjectSize(root, 2U, "Stop-fault query command");
    return Command{CommandKind::kGetStopFault};
  }
  if (command == "recover_stop_fault") {
    RequireExactObjectSize(root, 3U, "Stop-fault recovery command");
    Command decoded{CommandKind::kRecoverStopFault};
    decoded.stop_fault_recovery =
        DecodeStopFaultRecoveryIntent(Member(root, "request"));
    return decoded;
  }
  if (command == "set_operation_runtime_scale") {
    RequireExactObjectSize(root, 4U, "operation runtime-scale command");
    const auto &value = Member(root, "request");
    RequireExactObjectSize(value, 5U, "operation runtime-scale request");
    const double scale = Number(value, "scale");
    Require(scale > 0.0 && scale <= 1.0,
            "node runtime scale must be in (0, 1]");
    Command decoded{CommandKind::kSetOperationRuntimeScale};
    decoded.operation_runtime_scale = core::RuntimeScaleRequestDTO{
        DecodeRequestContext(Member(value, "request_context")),
        core::OperationId(Text(value, "operation_id")),
        core::OperationRevision(
            Counter(value, "expected_operation_revision", 1U)),
        core::RuntimeScaleGeneration(
            Counter(value, "expected_runtime_scale_generation", 0U)),
        scale,
        DecodeRequestAttribution(Member(root, "attribution"))};
    return decoded;
  }
  if (command == "set_runtime_scale_control") {
    RequireExactObjectSize(root, 4U, "runtime-scale control command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kSetRuntimeScaleControl};
    decoded.runtime_scale_control =
        DecodeRuntimeScaleControlRequest(Member(root, "request"), attribution);
    return decoded;
  }
  CommandKind kind;
  if (command == "pause") {
    kind = CommandKind::kPause;
  } else if (command == "resume") {
    kind = CommandKind::kResume;
  } else if (command == "reset") {
    kind = CommandKind::kReset;
  } else if (command == "acknowledge_completed") {
    kind = CommandKind::kAcknowledgeCompleted;
  } else {
    Fail("execution command kind is unsupported");
  }
  RequireExactObjectSize(root, 4U, "operation control command");
  const auto attribution =
      DecodeRequestAttribution(Member(root, "attribution"));
  Command decoded{kind};
  decoded.control =
      DecodeOperationControlRequest(Member(root, "request"), attribution);
  return decoded;
}

[[nodiscard]] CanonicalValue ErrorValue(const core::ErrorDTO &error) {
  CanonicalValue::Object details;
  details.reserve(error.details.size());
  for (const auto &[key, value] : error.details) {
    details.emplace_back(key, CanonicalValue(value));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue(error.code)},
      {"details", CanonicalValue(std::move(details))},
      {"message", CanonicalValue(error.message)},
  });
}

[[nodiscard]] CanonicalValue
ErrorResponse(std::string code, std::string message,
              std::map<std::string, std::string> details = {}) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"error", ErrorValue(core::ErrorDTO{std::move(code), std::move(message),
                                          std::move(details)})},
      {"ok", CanonicalValue(false)},
  });
}

[[nodiscard]] CanonicalValue ExecutionOwnerNotReadyIdentityResponse(
    const core::ResourceAddressedExecutionKernel &owner,
    const core::RequestOwnerEpoch &owner_epoch) {
  const auto admission = owner.GetAuthoringAdmission();
  return CanonicalValue(CanonicalValue::Object{
      {"admission_generation", CanonicalValue(static_cast<double>(
                                   admission.admission_generation.value()))},
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"error",
       ErrorValue(core::ErrorDTO{
           "execution_owner_not_ready",
           "the managed execution owner has no active physical product",
           {{"owner_epoch", owner_epoch.value()}}})},
      {"ok", CanonicalValue(false)},
  });
}

[[nodiscard]] CanonicalValue PoseValue(const core::Pose3DTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"orientation", CanonicalValue(CanonicalValue::Object{
                          {"w", CanonicalValue(value.orientation.w)},
                          {"x", CanonicalValue(value.orientation.x)},
                          {"y", CanonicalValue(value.orientation.y)},
                          {"z", CanonicalValue(value.orientation.z)},
                      })},
      {"position_m", CanonicalValue(CanonicalValue::Object{
                         {"x_m", CanonicalValue(value.position_m.x_m)},
                         {"y_m", CanonicalValue(value.position_m.y_m)},
                         {"z_m", CanonicalValue(value.position_m.z_m)},
                     })},
  });
}

[[nodiscard]] CanonicalValue
SceneRevisionValue(const core::SceneRevision &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"content_hash", CanonicalValue(value.content_hash.value())},
      {"revision_id", CanonicalValue(static_cast<double>(value.revision_id))},
      {"scene_id", CanonicalValue(value.scene_id.value())},
  });
}

class CollisionLeaseRelease final {
public:
  CollisionLeaseRelease(core::ICollisionWorldSnapshotPublisher &publisher,
                        core::ProjectionLeaseId lease_id)
      : publisher_(publisher), lease_id_(std::move(lease_id)) {}
  ~CollisionLeaseRelease() {
    publisher_.release_scene_snapshot_lease_inventory(lease_id_);
  }
  CollisionLeaseRelease(const CollisionLeaseRelease &) = delete;
  CollisionLeaseRelease &operator=(const CollisionLeaseRelease &) = delete;

private:
  core::ICollisionWorldSnapshotPublisher &publisher_;
  core::ProjectionLeaseId lease_id_;
};

[[nodiscard]] CanonicalValue MotionPreviewReceiptValue(
    const core::MotionPreviewResultDTO &preview,
    const core::OperationIntentDTO &intent,
    const core::ContentDigest &intent_digest,
    const core::RuntimeParticipantBindingDTO &direct_control_binding,
    const core::RequestOwnerEpoch &owner_epoch) {
  const auto &binding = direct_control_binding;
  const auto &final_state = preview.final_controlled_state;
  Require(
      final_state.resources.size() == binding.resources.size() &&
          final_state.joint_names.size() == binding.resources.size() &&
          final_state.positions.size() == binding.resources.size() &&
          preview.start_state.joint_names.size() == binding.resources.size() &&
          preview.start_state.positions_rad.size() == binding.resources.size(),
      "motion preview result crosses its direct-control resource shape");

  CanonicalValue::Array source_joint_names;
  CanonicalValue::Array start_positions;
  CanonicalValue::Array final_positions;
  source_joint_names.reserve(binding.resources.size());
  start_positions.reserve(binding.resources.size());
  final_positions.reserve(binding.resources.size());
  for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
    const auto &resource = binding.resources[index];
    Require(resource.source_joint_name.has_value() &&
                final_state.resources[index] == resource.resource_id &&
                final_state.joint_names[index] ==
                    resource.compiled_resource_name &&
                preview.start_state.joint_names[index] ==
                    resource.compiled_resource_name,
            "motion preview result crosses its source-local binding");
    const auto *angular =
        std::get_if<core::AngularPositionDTO>(&final_state.positions[index]);
    Require(angular != nullptr,
            "joint motion preview returned a non-angular terminal state");
    source_joint_names.emplace_back(*resource.source_joint_name);
    start_positions.emplace_back(preview.start_state.positions_rad[index]);
    final_positions.emplace_back(angular->value_rad);
  }

  CanonicalValue::Object receipt{
      {"collision_world_revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(preview.collision_world_revision.content_hash)},
           {"revision_id", CanonicalValue(static_cast<double>(
                               preview.collision_world_revision.revision_id))},
       })},
      {"commit_eligible", CanonicalValue(true)},
      {"component_role_id", CanonicalValue(binding.component_role_id.value())},
      {"final_positions_rad", CanonicalValue(std::move(final_positions))},
      {"final_tcp_pose", PoseValue(preview.final_tcp_pose)},
      {"graph_revision", CanonicalValue(binding.graph_revision.value())},
      {"intent_digest", CanonicalValue(intent_digest.value())},
      {"program_binding_id",
       CanonicalValue(binding.program_binding_id.value())},
      {"program_id", CanonicalValue(binding.program_id.value())},
      {"program_revision", CanonicalValue(binding.program_revision.value())},
      {"program_run_id", CanonicalValue(intent.program_run_id.value())},
      {"request_id", CanonicalValue(intent.request_context.request_id.value())},
      {"request_owner_epoch", CanonicalValue(owner_epoch.value())},
      {"required_capability_id",
       CanonicalValue(
           intent.participants.front().required_capability_id.value())},
      {"retained_bundle_id",
       CanonicalValue(binding.retained_bundle_id.value())},
      {"scene_revision",
       SceneRevisionValue(
           preview.projection_binding.projection_revision.scene_revision)},
      {"schema_version",
       CanonicalValue("manyforge.direct_control_motion_preview.v1")},
      {"source_joint_names", CanonicalValue(std::move(source_joint_names))},
      {"start_positions_rad", CanonicalValue(std::move(start_positions))},
      {"target_role_id", CanonicalValue(binding.target_role_id.value())},
  };
  const core::ContentDigest receipt_digest(content_identity::sha256_digest(
      content_identity::canonical_json(CanonicalValue(receipt))));
  receipt.emplace_back("receipt_digest",
                       CanonicalValue(receipt_digest.value()));
  return CanonicalValue(std::move(receipt));
}

[[nodiscard]] CanonicalValue CapabilityCommandPreviewReceiptValue(
    const core::CapabilityCommandPreviewResultDTO &preview,
    const core::OperationIntentDTO &intent,
    const core::ContentDigest &intent_digest,
    const std::optional<RetainedGripperPositionLimits> &limits,
    const core::RequestOwnerEpoch &owner_epoch) {
  const auto &binding = preview.binding;
  Require(binding.resources.size() == 1U,
          "capability-command preview has no exact scalar resource");
  const auto &resource = binding.resources.front();
  const bool model_backed = resource.source_joint_name.has_value();
  Require(
      intent.expected_scene_revision.has_value() &&
          binding.command_schema.has_value() &&
          preview.command_request.gripper_id ==
              binding.command_target_id.value() &&
          preview.command_request.joint_name ==
              resource.compiled_resource_name &&
          model_backed == limits.has_value() &&
          (!limits.has_value() ||
           (limits->coordinate_kind == resource.coordinate_kind &&
            std::isfinite(limits->lower_position) &&
            std::isfinite(limits->upper_position) &&
            limits->lower_position <= limits->upper_position &&
            limits->lower_position <= preview.command_request.target_position &&
            preview.command_request.target_position <=
                limits->upper_position)) &&
          std::isfinite(preview.command_request.target_position),
      "capability-command preview crosses its direct-control binding");
  const auto coordinate_kind =
      resource.coordinate_kind == core::CoordinateKind::kAngular ? "angular_rad"
                                                                 : "linear_m";
  const auto &source_coordinate_name = model_backed
                                           ? *resource.source_joint_name
                                           : resource.compiled_resource_name;
  CanonicalValue::Object receipt{
      {"command_schema_id",
       CanonicalValue(binding.command_schema->command_schema_id.value())},
      {"command_schema_revision",
       CanonicalValue(binding.command_schema->command_schema_revision.value())},
      {"commit_eligible", CanonicalValue(true)},
      {"component_role_id", CanonicalValue(binding.component_role_id.value())},
      {"coordinate_kind", CanonicalValue(coordinate_kind)},
      {"graph_revision", CanonicalValue(binding.graph_revision.value())},
      {"intent_digest", CanonicalValue(intent_digest.value())},
      {"max_position", limits.has_value()
                           ? CanonicalValue(limits->upper_position)
                           : CanonicalValue(nullptr)},
      {"min_position", limits.has_value()
                           ? CanonicalValue(limits->lower_position)
                           : CanonicalValue(nullptr)},
      {"program_binding_id",
       CanonicalValue(binding.program_binding_id.value())},
      {"program_id", CanonicalValue(binding.program_id.value())},
      {"program_revision", CanonicalValue(binding.program_revision.value())},
      {"program_run_id", CanonicalValue(intent.program_run_id.value())},
      {"request_id", CanonicalValue(intent.request_context.request_id.value())},
      {"request_owner_epoch", CanonicalValue(owner_epoch.value())},
      {"required_capability_id",
       CanonicalValue(
           intent.participants.front().required_capability_id.value())},
      {"retained_bundle_id",
       CanonicalValue(binding.retained_bundle_id.value())},
      {"scene_revision", SceneRevisionValue(*intent.expected_scene_revision)},
      {"schema_version",
       CanonicalValue(
           "manyforge.direct_control_capability_command_preview.v1")},
      {"source_coordinate_name", CanonicalValue(source_coordinate_name)},
      {"source_kind",
       CanonicalValue(model_backed ? "model_role" : "component_local")},
      {"target_position",
       CanonicalValue(preview.command_request.target_position)},
      {"target_role_id", CanonicalValue(binding.target_role_id.value())},
  };
  const core::ContentDigest receipt_digest(content_identity::sha256_digest(
      content_identity::canonical_json(CanonicalValue(receipt))));
  receipt.emplace_back("receipt_digest",
                       CanonicalValue(receipt_digest.value()));
  return CanonicalValue(std::move(receipt));
}

[[nodiscard]] CanonicalValue
MotionSpecValue(const core::MotionSpecDTO &motion) {
  return std::visit(
      [](const auto &value) -> CanonicalValue {
        using Motion = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Motion, core::JointGoalDTO>) {
          CanonicalValue::Array joint_names;
          CanonicalValue::Array positions;
          joint_names.reserve(value.joint_names.size());
          positions.reserve(value.positions_rad.size());
          for (const auto &name : value.joint_names) {
            joint_names.emplace_back(name);
          }
          for (const auto position : value.positions_rad) {
            positions.emplace_back(position);
          }
          return CanonicalValue(CanonicalValue::Object{
              {"joint_names", CanonicalValue(std::move(joint_names))},
              {"kind", CanonicalValue("joint_goal")},
              {"positions_rad", CanonicalValue(std::move(positions))},
          });
        } else if constexpr (std::is_same_v<Motion, core::PoseGoalDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"goal", PoseValue(value.goal)},
              {"kind", CanonicalValue("pose_goal")},
          });
        } else if constexpr (std::is_same_v<Motion, core::NamedTargetDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("named_target")},
              {"name", CanonicalValue(value.name)},
          });
        } else {
          CanonicalValue::Array waypoints;
          waypoints.reserve(value.waypoints.size());
          for (const auto &waypoint : value.waypoints) {
            waypoints.push_back(PoseValue(waypoint));
          }
          return CanonicalValue(CanonicalValue::Object{
              {"fallback_policy",
               CanonicalValue(
                   value.fallback_policy ==
                           core::CartesianFallbackPolicyDTO::kFailClosed
                       ? "fail_closed"
                       : "allow_joint_space_fallback")},
              {"kind", CanonicalValue("cartesian_path")},
              {"max_angular_acceleration_rad_s2",
               CanonicalValue(value.max_angular_acceleration_rad_s2)},
              {"max_blend_deviation_rad",
               CanonicalValue(value.max_blend_deviation_rad)},
              {"max_angular_speed_rad_s",
               CanonicalValue(value.max_angular_speed_rad_s)},
              {"max_linear_acceleration_mps2",
               CanonicalValue(value.max_linear_acceleration_mps2)},
              {"max_rotation_step_rad",
               CanonicalValue(value.max_rotation_step_rad)},
              {"max_translation_step_m",
               CanonicalValue(value.max_translation_step_m)},
              {"speed_mode", CanonicalValue("bounded")},
              {"waypoints", CanonicalValue(std::move(waypoints))},
          });
        }
      },
      motion);
}

[[nodiscard]] CanonicalValue
ProjectionRevisionValue(const core::ProjectionRevisionDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"scene_revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(value.scene_revision.content_hash.value())},
           {"revision_id", CanonicalValue(static_cast<double>(
                               value.scene_revision.revision_id))},
           {"scene_id", CanonicalValue(value.scene_revision.scene_id.value())},
       })},
  });
}

[[nodiscard]] CanonicalValue
PickCandidateValue(const core::PickPoseCandidateDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"approach_pose", PoseValue(value.approach_pose)},
      {"candidate_index",
       CanonicalValue(static_cast<double>(value.candidate_index))},
      {"min_tcp_z_applied", CanonicalValue(value.min_tcp_z_applied)},
      {"pick_cartesian_path",
       MotionSpecValue(core::MotionSpecDTO{value.descend_path})},
      {"pick_pose", PoseValue(value.pick_pose)},
      {"retreat_cartesian_path",
       MotionSpecValue(core::MotionSpecDTO{value.retreat_path})},
      {"retreat_pose", PoseValue(value.retreat_pose)},
  });
}

[[nodiscard]] CanonicalValue
PickPlacePlanningResultValue(const std::string_view sequence_id,
                             const core::PickPlaceExecutionKindDTO kind,
                             const core::PickPlacePlanningResultDTO &value) {
  CanonicalValue::Array reports;
  reports.reserve(value.reports.size());
  for (const auto &report : value.reports) {
    CanonicalValue::Array stages;
    stages.reserve(report.stages.size());
    for (const auto &stage : report.stages) {
      stages.emplace_back(CanonicalValue::Object{
          {"error", stage.error.has_value() ? ErrorValue(*stage.error)
                                            : CanonicalValue(nullptr)},
          {"stage", CanonicalValue(std::string(core::to_string(stage.stage)))},
          {"succeeded", CanonicalValue(stage.succeeded)},
          {"trajectory_duration_s",
           CanonicalValue(stage.trajectory_duration_s)},
          {"trajectory_waypoint_count", CanonicalValue(static_cast<double>(
                                            stage.trajectory_waypoint_count))},
      });
    }
    reports.emplace_back(CanonicalValue::Object{
        {"candidate_index",
         CanonicalValue(static_cast<double>(report.candidate_index))},
        {"stages", CanonicalValue(std::move(stages))},
    });
  }
  CanonicalValue plan(nullptr);
  if (value.plan.has_value()) {
    const auto &planned = *value.plan;
    plan = CanonicalValue(CanonicalValue::Object{
        {"approach_motion", MotionSpecValue(planned.approach_motion)},
        {"descend_motion", MotionSpecValue(planned.descend_motion)},
        {"predecessor_projection_revision",
         ProjectionRevisionValue(planned.predecessor_projection_revision)},
        {"preview_successor_projection_revision",
         ProjectionRevisionValue(
             planned.preview_successor_projection_revision)},
        {"retreat_motion", MotionSpecValue(planned.retreat_motion)},
    });
  }
  CanonicalValue last_attempted_candidate(nullptr);
  if (value.last_attempted_pick_candidate.has_value()) {
    last_attempted_candidate =
        PickCandidateValue(*value.last_attempted_pick_candidate);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"attempted_candidate_count",
       CanonicalValue(static_cast<double>(value.attempted_candidate_count))},
      {"kind", CanonicalValue(kind == core::PickPlaceExecutionKindDTO::kPick
                                  ? "pick"
                                  : "place")},
      {"last_attempted_candidate", std::move(last_attempted_candidate)},
      {"plan", std::move(plan)},
      {"reports", CanonicalValue(std::move(reports))},
      {"schema_version",
       CanonicalValue("manyforge.pick_place_planning_result.v1")},
      {"selected_object_id", CanonicalValue(value.selected_object_id.value())},
      {"sequence_id", CanonicalValue(std::string(sequence_id))},
      {"status", CanonicalValue(std::string(core::to_string(value.status)))},
  });
}

[[nodiscard]] std::string_view StateName(core::OperationStateKind state) {
  switch (state) {
  case core::OperationStateKind::kQueued:
    return "queued";
  case core::OperationStateKind::kResolving:
    return "resolving";
  case core::OperationStateKind::kPlanning:
    return "planning";
  case core::OperationStateKind::kValidating:
    return "validating";
  case core::OperationStateKind::kPrepared:
    return "prepared";
  case core::OperationStateKind::kDispatching:
    return "dispatching";
  case core::OperationStateKind::kExecuting:
    return "executing";
  case core::OperationStateKind::kPaused:
    return "paused";
  case core::OperationStateKind::kStopping:
    return "stopping";
  case core::OperationStateKind::kCompleted:
    return "completed";
  case core::OperationStateKind::kStopped:
    return "stopped";
  case core::OperationStateKind::kFailed:
    return "failed";
  case core::OperationStateKind::kInterrupted:
    return "interrupted";
  case core::OperationStateKind::kInterventionRequired:
    return "intervention_required";
  case core::OperationStateKind::kCancelledPreDispatch:
    return "cancelled_pre_dispatch";
  }
  return "unknown";
}

[[nodiscard]] CanonicalValue
AcceptedValue(const core::OperationAcceptedDTO &value) {
  CanonicalValue::Array assignments;
  assignments.reserve(value.participant_assignments.size());
  for (const auto &assignment : value.participant_assignments) {
    assignments.emplace_back(CanonicalValue::Object{
        {"input_index",
         CanonicalValue(static_cast<double>(assignment.input_index))},
        {"participant_id", CanonicalValue(assignment.participant_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision",
       CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participant_assignments", CanonicalValue(std::move(assignments))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
  });
}

[[nodiscard]] std::string_view ControlName(core::OperationControlKind control) {
  switch (control) {
  case core::OperationControlKind::kPause:
    return "pause";
  case core::OperationControlKind::kResume:
    return "resume";
  case core::OperationControlKind::kReset:
    return "reset";
  case core::OperationControlKind::kAcknowledgeCompleted:
    return "acknowledge_completed";
  }
  return "unknown";
}

[[nodiscard]] CanonicalValue
ControlAcceptedValue(const core::OperationControlAcceptedDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_against_revision",
       CanonicalValue(
           static_cast<double>(value.accepted_against_revision.value()))},
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"control", CanonicalValue(std::string(ControlName(value.control)))},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision_after_acceptance",
       CanonicalValue(static_cast<double>(
           value.operation_revision_after_acceptance.value()))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
  });
}

[[nodiscard]] CanonicalValue
StopAcceptedValue(const core::StopAcceptedDTO &value) {
  const auto diagnostic_target = [](const auto &target) {
    return std::visit(
        [](const auto &selected) -> CanonicalValue {
          using Target = std::decay_t<decltype(selected)>;
          if constexpr (std::is_same_v<Target, std::monostate>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("cell")},
                {"value", CanonicalValue(nullptr)},
            });
          } else if constexpr (std::is_same_v<Target, core::OperationId>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("operation_id")},
                {"value", CanonicalValue(selected.value())},
            });
          } else if constexpr (std::is_same_v<Target,
                                              core::ControllableComponentId>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("controllable_component_id")},
                {"value", CanonicalValue(selected.value())},
            });
          } else {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("resource_id")},
                {"value", CanonicalValue(selected.value())},
            });
          }
        },
        target);
  };
  const auto diagnostic_value = [](const auto &candidate) {
    return std::visit(
        [](const auto &selected) -> CanonicalValue {
          using Value = std::decay_t<decltype(selected)>;
          if constexpr (std::is_same_v<Value, std::uint64_t>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("unsigned_integer")},
                {"value", CanonicalValue(static_cast<double>(selected))},
            });
          } else if constexpr (std::is_same_v<Value, core::RequestOwnerEpoch>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("request_owner_epoch")},
                {"value", CanonicalValue(selected.value())},
            });
          } else if constexpr (std::is_same_v<Value, core::GraphRevision>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("graph_revision")},
                {"value", CanonicalValue(selected.value())},
            });
          } else if constexpr (std::is_same_v<Value, core::OperationRevision>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("operation_revision")},
                {"value",
                 CanonicalValue(static_cast<double>(selected.value()))},
            });
          } else if constexpr (std::is_same_v<
                                   Value, core::ComponentLifecycleGeneration>) {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("component_lifecycle_generation")},
                {"value",
                 CanonicalValue(static_cast<double>(selected.value()))},
            });
          } else {
            return CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("stop_generation")},
                {"value",
                 CanonicalValue(static_cast<double>(selected.value()))},
            });
          }
        },
        candidate);
  };

  CanonicalValue::Array stale_diagnostics;
  stale_diagnostics.reserve(value.stale_diagnostics.size());
  for (const auto &diagnostic : value.stale_diagnostics) {
    stale_diagnostics.emplace_back(CanonicalValue::Object{
        {"current", diagnostic_value(diagnostic.current)},
        {"expected", diagnostic_value(diagnostic.expected)},
        {"field",
         CanonicalValue(std::string(core::to_string(diagnostic.field)))},
        {"target", diagnostic_target(diagnostic.target)},
    });
  }

  CanonicalValue::Array affected_operations;
  affected_operations.reserve(value.affected_operation_revisions.size());
  for (const auto &operation : value.affected_operation_revisions) {
    affected_operations.emplace_back(CanonicalValue::Object{
        {"operation_id", CanonicalValue(operation.operation_id.value())},
        {"operation_revision_after_acceptance",
         CanonicalValue(static_cast<double>(
             operation.operation_revision_after_acceptance.value()))},
    });
  }

  CanonicalValue::Array predecessors;
  predecessors.reserve(value.joined_predecessor_stops.size());
  for (const auto &predecessor : value.joined_predecessor_stops) {
    predecessors.emplace_back(CanonicalValue::Object{
        {"captured_scope_digest",
         CanonicalValue(predecessor.captured_scope_digest.value())},
        {"predecessor_stop_generation",
         CanonicalValue(static_cast<double>(
             predecessor.predecessor_stop_generation.value()))},
        {"predecessor_stop_id",
         CanonicalValue(predecessor.predecessor_stop_id.value())},
        {"remaining_outer_duration_ns_at_join",
         CanonicalValue(static_cast<double>(
             predecessor.remaining_outer_duration_ns_at_join))},
    });
  }

  CanonicalValue::Array participant_budgets;
  participant_budgets.reserve(value.participant_budget_allocations.size());
  for (const auto &participant : value.participant_budget_allocations) {
    CanonicalValue::Array phases;
    phases.reserve(participant.phases.size());
    for (const auto &phase : participant.phases) {
      phases.emplace_back(CanonicalValue::Object{
          {"fixed_deadline_offset_ns",
           manyforge::content_identity::nonnegative_int64_value(
               phase.fixed_deadline_offset_ns)},
          {"phase_id", CanonicalValue(phase.phase_id.value())},
          {"remaining_deadline_offset_ns",
           CanonicalValue(
               static_cast<double>(phase.remaining_deadline_offset_ns))},
      });
    }
    participant_budgets.emplace_back(CanonicalValue::Object{
        {"operation_id", CanonicalValue(participant.operation_id.value())},
        {"origin_stop_generation",
         CanonicalValue(
             static_cast<double>(participant.origin_stop_generation.value()))},
        {"origin_stop_id", CanonicalValue(participant.origin_stop_id.value())},
        {"participant_id", CanonicalValue(participant.participant_id.value())},
        {"participant_stop_invocation_plan_digest",
         CanonicalValue(
             participant.participant_stop_invocation_plan_digest.value())},
        {"phases", CanonicalValue(std::move(phases))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"affected_operation_revisions",
       CanonicalValue(std::move(affected_operations))},
      {"captured_scope_digest",
       CanonicalValue(value.captured_scope_digest.value())},
      {"fixed_kernel_completion_reserve_ns",
       CanonicalValue(
           static_cast<double>(value.fixed_kernel_completion_reserve_ns))},
      {"fixed_outer_duration_ns",
       manyforge::content_identity::nonnegative_int64_value(
           value.fixed_outer_duration_ns)},
      {"joined_predecessor_stops", CanonicalValue(std::move(predecessors))},
      {"participant_budget_allocations",
       CanonicalValue(std::move(participant_budgets))},
      {"remaining_outer_duration_ns",
       manyforge::content_identity::nonnegative_int64_value(
           value.remaining_outer_duration_ns)},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
      {"requested_selector_digest",
       CanonicalValue(value.requested_selector_digest.value())},
      {"resolution_disposition", CanonicalValue(std::string(core::to_string(
                                     value.resolution_disposition)))},
      {"resolution_reason",
       CanonicalValue(std::string(core::to_string(value.resolution_reason)))},
      {"stale_diagnostics", CanonicalValue(std::move(stale_diagnostics))},
      {"stop_generation",
       CanonicalValue(static_cast<double>(value.stop_generation.value()))},
      {"stop_id", CanonicalValue(value.stop_id.value())},
  });
}

[[nodiscard]] std::string_view StopProjectionStateName(
    const core::StopDurableProjectionStateDTO state) noexcept {
  switch (state) {
  case core::StopDurableProjectionStateDTO::kNotApplicable:
    return "not_applicable";
  case core::StopDurableProjectionStateDTO::kPending:
    return "pending";
  case core::StopDurableProjectionStateDTO::kRecorded:
    return "recorded";
  case core::StopDurableProjectionStateDTO::kFailed:
    return "failed";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopStatusStateName(const core::StopStatusStateDTO state) noexcept {
  switch (state) {
  case core::StopStatusStateDTO::kAccepted:
    return "accepted";
  case core::StopStatusStateDTO::kStopping:
    return "stopping";
  case core::StopStatusStateDTO::kCertifying:
    return "certifying";
  case core::StopStatusStateDTO::kSucceeded:
    return "succeeded";
  case core::StopStatusStateDTO::kPartial:
    return "partial";
  case core::StopStatusStateDTO::kFailed:
    return "failed";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopFaultTriggerName(const core::StopFaultTriggerDTO trigger) noexcept {
  switch (trigger) {
  case core::StopFaultTriggerDTO::kScopePromotion:
    return "scope_promotion";
  case core::StopFaultTriggerDTO::kEscalated:
    return "escalated";
  case core::StopFaultTriggerDTO::kPartial:
    return "partial";
  case core::StopFaultTriggerDTO::kFailedUncontained:
    return "failed_uncontained";
  case core::StopFaultTriggerDTO::kPhysicalRestUnproven:
    return "physical_rest_unproven";
  case core::StopFaultTriggerDTO::kPersistenceDegraded:
    return "persistence_degraded";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopFaultLatchStateName(const core::StopFaultLatchStateDTO state) noexcept {
  switch (state) {
  case core::StopFaultLatchStateDTO::kLatched:
    return "latched";
  case core::StopFaultLatchStateDTO::kRecoveryPending:
    return "recovery_pending";
  case core::StopFaultLatchStateDTO::kCleared:
    return "cleared";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopFaultRecoveryStatusName(
    const core::StopFaultRecoveryStatusDTO status) noexcept {
  switch (status) {
  case core::StopFaultRecoveryStatusDTO::kWaitingForObservation:
    return "waiting_for_observation";
  case core::StopFaultRecoveryStatusDTO::kWaitingForQuiescence:
    return "waiting_for_quiescence";
  case core::StopFaultRecoveryStatusDTO::kWaitingForConfiguration:
    return "waiting_for_configuration";
  case core::StopFaultRecoveryStatusDTO::kWaitingForExternalReconciliation:
    return "waiting_for_external_reconciliation";
  case core::StopFaultRecoveryStatusDTO::kWaitingForDurableRecord:
    return "waiting_for_durable_record";
  case core::StopFaultRecoveryStatusDTO::kReady:
    return "ready";
  case core::StopFaultRecoveryStatusDTO::kRecoveryPending:
    return "recovery_pending";
  case core::StopFaultRecoveryStatusDTO::kCleared:
    return "cleared";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopFaultRecoveryEvidenceKindName(
    const core::StopFaultRecoveryEvidenceKindDTO kind) noexcept {
  switch (kind) {
  case core::StopFaultRecoveryEvidenceKindDTO::kSettlement:
    return "settlement";
  case core::StopFaultRecoveryEvidenceKindDTO::kLateQuiescence:
    return "late_quiescence";
  case core::StopFaultRecoveryEvidenceKindDTO::kConfiguration:
    return "configuration";
  case core::StopFaultRecoveryEvidenceKindDTO::kExternalReconciliation:
    return "external_reconciliation";
  case core::StopFaultRecoveryEvidenceKindDTO::kBackendOrLifecycleReset:
    return "backend_or_lifecycle_reset";
  }
  return "unknown";
}

[[nodiscard]] CanonicalValue
StopCertificateRefValue(const core::StopCertificateRefDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"stop_certificate_id",
       CanonicalValue(value.stop_certificate_id.value())},
      {"stop_certificate_revision",
       CanonicalValue(
           static_cast<double>(value.stop_certificate_revision.value()))},
      {"stop_generation",
       CanonicalValue(static_cast<double>(value.stop_generation.value()))},
      {"stop_id", CanonicalValue(value.stop_id.value())},
  });
}

[[nodiscard]] CanonicalValue
ProgramExecutionScopeValue(const core::ProgramExecutionScopeStateDTO &value) {
  // Group sparse node indices by state. Repeating field names for each of
  // 100,000 captured nodes exceeds the bounded native response envelope.
  CanonicalValue::Array ready, held, stopping, faulted, pending;
  for (const auto &node : value.held_nodes) {
    auto *indices =
        node.state == core::ProgramNodeHoldStateDTO::kReady      ? &ready
        : node.state == core::ProgramNodeHoldStateDTO::kHeld       ? &held
        : node.state == core::ProgramNodeHoldStateDTO::kStopping ? &stopping
        : node.state == core::ProgramNodeHoldStateDTO::kFaulted  ? &faulted
                                                                 : nullptr;
    if (indices != nullptr)
      indices->emplace_back(static_cast<double>(node.node_index));
  }
  for (const auto &effect : value.pending_effects) {
    pending.emplace_back(CanonicalValue::Object{
        {"node_index", CanonicalValue(static_cast<double>(effect.node_index))},
        {"operation_id", CanonicalValue(effect.operation_id.value())},
        {"participant_id", CanonicalValue(effect.participant_id.value())},
        {"stop_id", CanonicalValue(effect.stop_id.value())},
        {"stop_generation",
         CanonicalValue(static_cast<double>(effect.stop_generation.value()))}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.program_execution_scope.v2")},
      {"request_owner_epoch",
       CanonicalValue(value.request_owner_epoch.value())},
      {"program_runner_owner_epoch",
       CanonicalValue(value.program_runner_owner_epoch.value())},
      {"claim_generation",
       CanonicalValue(static_cast<double>(value.claim_generation.value()))},
      {"program_run_id", CanonicalValue(value.program_run_id.value())},
      {"selection_revision",
       CanonicalValue(static_cast<double>(value.selection_revision))},
      {"ready_node_indices", CanonicalValue(std::move(ready))},
      {"held_node_indices", CanonicalValue(std::move(held))},
      {"stopping_node_indices", CanonicalValue(std::move(stopping))},
      {"faulted_node_indices", CanonicalValue(std::move(faulted))},
      {"pending_effects", CanonicalValue(std::move(pending))}});
}

[[nodiscard]] CanonicalValue ProgramSelectionAcceptedValue(
    const core::ProgramExecutionSelectionAcceptedDTO &value,
    const std::vector<std::string> &group_ids) {
  CanonicalValue::Array groups;
  for (const auto &group : group_ids)
    groups.emplace_back(group);
  const auto indices = [](const auto &source) {
    CanonicalValue::Array result;
    for (const auto index : source)
      result.emplace_back(static_cast<double>(index));
    return CanonicalValue(std::move(result));
  };
  CanonicalValue containment(nullptr);
  if (value.fresh_containment.has_value()) {
    const auto &stop = *value.fresh_containment;
    // Existing operation/Stop queries own recovery evidence. This result
    // identifies physical work without duplicating its budget/certificate.
    containment = CanonicalValue(CanonicalValue::Object{
        {"stop_id", CanonicalValue(stop.stop_id.value())},
        {"stop_generation",
         CanonicalValue(static_cast<double>(stop.stop_generation.value()))},
        {"resolution_disposition", CanonicalValue(std::string(core::to_string(
                                       stop.resolution_disposition)))}});
  }
  return CanonicalValue(CanonicalValue::Object{
      {"state", value.state.has_value()
                    ? ProgramExecutionScopeValue(*value.state)
                    : CanonicalValue(nullptr)},
      {"group_ids", CanonicalValue(std::move(groups))},
      {"widened_node_indices", indices(value.widened_node_indices)},
      {"fresh_containment", std::move(containment)},
      {"selection_error", value.selection_error.has_value()
                              ? ErrorValue(*value.selection_error)
                              : CanonicalValue(nullptr)},
      {"replay_status",
       CanonicalValue(value.replay_status ==
                              core::RequestReplayStatusDTO::kReplayedExact
                          ? "replayed_exact"
                          : "accepted_new")}});
}

[[nodiscard]] CanonicalValue
StopStatusSummaryValue(const core::StopStatusDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"request_id", CanonicalValue(value.accepted.request_id.value())},
      {"schema_version", CanonicalValue("manyforge.execution_stop_status.v1")},
      {"state", CanonicalValue(std::string(StopStatusStateName(value.state)))},
      {"stop_generation", CanonicalValue(static_cast<double>(
                              value.accepted.stop_generation.value()))},
      {"stop_id", CanonicalValue(value.accepted.stop_id.value())},
  });
}

[[nodiscard]] CanonicalValue
StopProjectionStatusValue(const core::StopDurableProjectionStatusDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"error", value.error.has_value() ? ErrorValue(*value.error)
                                        : CanonicalValue(nullptr)},
      {"retry_generation",
       value.retry_generation.has_value()
           ? CanonicalValue(static_cast<double>(*value.retry_generation))
           : CanonicalValue(nullptr)},
      {"state",
       CanonicalValue(std::string(StopProjectionStateName(value.state)))},
  });
}

[[nodiscard]] CanonicalValue
StopFaultLatchValue(const core::StopFaultLatchDTO &value) {
  CanonicalValue::Array captured_operations;
  captured_operations.reserve(value.captured_operation_ids.size());
  for (const auto &operation : value.captured_operation_ids) {
    captured_operations.emplace_back(operation.value());
  }
  CanonicalValue::Array captured_resources;
  captured_resources.reserve(value.captured_resources.size());
  for (const auto &resource : value.captured_resources) {
    captured_resources.emplace_back(resource.value());
  }
  CanonicalValue::Array evidence_refs;
  evidence_refs.reserve(value.evidence_refs.size());
  for (const auto &evidence : value.evidence_refs) {
    evidence_refs.emplace_back(CanonicalValue::Object{
        {"evidence_digest", CanonicalValue(evidence.evidence_digest.value())},
        {"kind", CanonicalValue(std::string(
                     StopFaultRecoveryEvidenceKindName(evidence.kind)))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"captured_operation_ids",
       CanonicalValue(std::move(captured_operations))},
      {"captured_resources", CanonicalValue(std::move(captured_resources))},
      {"cell_id", CanonicalValue(value.cell_id.value())},
      {"clear_projection", StopProjectionStatusValue(value.clear_projection)},
      {"evidence_refs", CanonicalValue(std::move(evidence_refs))},
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"latch_generation",
       CanonicalValue(static_cast<double>(value.latch_generation.value()))},
      {"latch_id", CanonicalValue(value.latch_id.value())},
      {"persistence_degraded", CanonicalValue(value.persistence_degraded)},
      {"reason_code", CanonicalValue(value.reason_code)},
      {"recovery_assessment_revision",
       CanonicalValue(static_cast<double>(value.recovery_assessment_revision))},
      {"recovery_status",
       CanonicalValue(
           std::string(StopFaultRecoveryStatusName(value.recovery_status)))},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"state",
       CanonicalValue(std::string(StopFaultLatchStateName(value.state)))},
      {"stop_certificate",
       value.stop_certificate.has_value()
           ? StopCertificateRefValue(*value.stop_certificate)
           : CanonicalValue(nullptr)},
      {"stop_generation",
       CanonicalValue(static_cast<double>(value.stop_generation.value()))},
      {"stop_id", CanonicalValue(value.stop_id.value())},
      {"terminal_projection",
       StopProjectionStatusValue(value.terminal_projection)},
      {"trigger",
       CanonicalValue(std::string(StopFaultTriggerName(value.trigger)))},
  });
}

[[nodiscard]] CanonicalValue
StopFaultOfferValue(const core::StopFaultRecoveryOfferDTO &offer) {
  Require(!core::stop_fault_recovery_offer_field_error(offer).has_value(),
          "Stop-fault offer is not a valid atomic owner projection");
  CanonicalValue::Array rows;
  rows.reserve(offer.configuration_expectations.size());
  for (const auto &expectation : offer.configuration_expectations) {
    rows.emplace_back(CanonicalValue::Object{
        {"active_control_configuration_generation",
         CanonicalValue(static_cast<double>(
             expectation.active_control_configuration_generation.value()))},
        {"active_control_configuration_id",
         CanonicalValue(expectation.active_control_configuration_id.value())},
        {"control_domain_id",
         CanonicalValue(expectation.control_domain_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"configuration_expectations", CanonicalValue(std::move(rows))},
      {"latch", StopFaultLatchValue(offer.latch)},
  });
}

[[nodiscard]] CanonicalValue
StopFaultRecoveryResultValue(const core::StopFaultRecoveryResultDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted", CanonicalValue(value.accepted)},
      {"error", value.error.has_value() ? ErrorValue(*value.error)
                                        : CanonicalValue(nullptr)},
      {"latch", StopFaultLatchValue(value.latch)},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
  });
}

[[nodiscard]] std::string_view
StopCertificateResultName(core::StopCertificateResultDTO result) noexcept {
  switch (result) {
  case core::StopCertificateResultDTO::kSucceededNominal:
    return "succeeded_nominal";
  case core::StopCertificateResultDTO::kSucceededEscalated:
    return "succeeded_escalated";
  case core::StopCertificateResultDTO::kPartial:
    return "partial";
  case core::StopCertificateResultDTO::kFailedUncontained:
    return "failed_uncontained";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopStageOutcomeName(core::ParticipantStopStageOutcomeDTO outcome) noexcept {
  switch (outcome) {
  case core::ParticipantStopStageOutcomeDTO::kSucceeded:
    return "succeeded";
  case core::ParticipantStopStageOutcomeDTO::kPartial:
    return "partial";
  case core::ParticipantStopStageOutcomeDTO::kFailed:
    return "failed";
  case core::ParticipantStopStageOutcomeDTO::kSupersededNoEffect:
    return "superseded_no_effect";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopAttemptDispositionName(
    const core::StopStageAttemptDispositionDTO disposition) noexcept {
  switch (disposition) {
  case core::StopStageAttemptDispositionDTO::kNotDispatched:
    return "not_dispatched";
  case core::StopStageAttemptDispositionDTO::kDispatched:
    return "dispatched";
  case core::StopStageAttemptDispositionDTO::kEffectUncertain:
    return "effect_uncertain";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopEvidenceStateName(const core::StopEvidenceItemStateDTO state) noexcept {
  switch (state) {
  case core::StopEvidenceItemStateDTO::kRequiredPending:
    return "required_pending";
  case core::StopEvidenceItemStateDTO::kSatisfied:
    return "satisfied";
  case core::StopEvidenceItemStateDTO::kMissing:
    return "missing";
  case core::StopEvidenceItemStateDTO::kFailed:
    return "failed";
  case core::StopEvidenceItemStateDTO::kInapplicable:
    return "inapplicable";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopStageAuthorityName(
    const core::StopStageResultAuthorityDTO authority) noexcept {
  switch (authority) {
  case core::StopStageResultAuthorityDTO::kAuthoritative:
    return "authoritative";
  case core::StopStageResultAuthorityDTO::kSupersededLate:
    return "superseded_late";
  }
  return "unknown";
}

[[nodiscard]] std::string_view StopParticipantSettlementName(
    const core::StopParticipantScopeFactsDTO::TerminalSettlementState
        settlement) noexcept {
  using State = core::StopParticipantScopeFactsDTO::TerminalSettlementState;
  switch (settlement) {
  case State::kNotRequired:
    return "not_required";
  case State::kPending:
    return "pending";
  case State::kSettled:
    return "settled";
  case State::kContainedUnsettled:
    return "contained_unsettled";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
StopSelectorKindName(const core::StopRequestedSelectorKindDTO kind) noexcept {
  switch (kind) {
  case core::StopRequestedSelectorKindDTO::kOrdinaryOperations:
    return "ordinary_operations";
  case core::StopRequestedSelectorKindDTO::kOrdinaryComponents:
    return "ordinary_components";
  case core::StopRequestedSelectorKindDTO::kOrdinaryResources:
    return "ordinary_resources";
  case core::StopRequestedSelectorKindDTO::kExplicitFull:
    return "explicit_full";
  case core::StopRequestedSelectorKindDTO::kMalformedIngress:
    return "malformed_ingress";
  case core::StopRequestedSelectorKindDTO::kInternalProgramRun:
    return "internal_program_run";
  }
  return "unknown";
}

[[nodiscard]] std::string_view
MotionInterruptStopModeName(const core::MotionInterruptStopMode mode) noexcept {
  switch (mode) {
  case core::MotionInterruptStopMode::kSoftOnPath:
    return "soft_on_path";
  case core::MotionInterruptStopMode::kSpringBackHold:
    return "spring_back_hold";
  case core::MotionInterruptStopMode::kHardDeceleration:
    return "hard_deceleration";
  }
  return "unknown";
}

[[nodiscard]] CanonicalValue
StopHistoryJsonValue(const core::JsonValueDTO &value) {
  return std::visit(
      [](const auto &current) -> CanonicalValue {
        using Value = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<Value, std::nullptr_t>) {
          return CanonicalValue(nullptr);
        } else if constexpr (std::is_same_v<Value, bool> ||
                             std::is_same_v<Value, double> ||
                             std::is_same_v<Value, std::string>) {
          return CanonicalValue(current);
        } else if constexpr (std::is_same_v<Value, core::JsonValueDTO::Array>) {
          CanonicalValue::Array projected;
          projected.reserve(current.size());
          for (const auto &item : current) {
            projected.emplace_back(StopHistoryJsonValue(item));
          }
          return CanonicalValue(std::move(projected));
        } else {
          CanonicalValue::Object projected;
          projected.reserve(current.size());
          for (const auto &[name, item] : current) {
            projected.emplace_back(name, StopHistoryJsonValue(item));
          }
          return CanonicalValue(std::move(projected));
        }
      },
      value.value);
}

[[nodiscard]] CanonicalValue
StopHistoryStructuredValue(const core::InterruptStructuredValue &value) {
  CanonicalValue::Object projected;
  projected.reserve(value.size());
  for (const auto &[name, item] : value) {
    projected.emplace_back(
        name,
        std::visit([](const auto &typed) { return CanonicalValue(typed); },
                   item));
  }
  return CanonicalValue(std::move(projected));
}

[[nodiscard]] CanonicalValue
StopHistoryAccelerationValue(const core::CoordinateAccelerationDTO &value) {
  return std::visit(
      [](const auto &typed) -> CanonicalValue {
        using Acceleration = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Acceleration,
                                     core::AngularAccelerationDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("angular")},
              {"value_rad_s2", CanonicalValue(typed.value_rad_s2)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("linear")},
              {"value_m_s2", CanonicalValue(typed.value_m_s2)},
          });
        }
      },
      value);
}

[[nodiscard]] CanonicalValue
StopHistoryVelocityValue(const core::CoordinateVelocityDTO &value) {
  return std::visit(
      [](const auto &typed) -> CanonicalValue {
        using Velocity = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Velocity, core::AngularVelocityDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("angular")},
              {"value_rad_s", CanonicalValue(typed.value_rad_s)},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("linear")},
              {"value_m_s", CanonicalValue(typed.value_m_s)},
          });
        }
      },
      value);
}

[[nodiscard]] CanonicalValue
StopHistoryScopeKeyValue(const core::StopScopeEntryKeyDTO &key) {
  const auto subject = std::visit(
      [](const auto &typed) -> CanonicalValue {
        using Subject = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Subject,
                                     core::StopParticipantScopeKeyDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("participant")},
              {"operation_id", CanonicalValue(typed.operation_id.value())},
              {"participant_id", CanonicalValue(typed.participant_id.value())},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("resource")},
              {"resource_id", CanonicalValue(typed.value())},
          });
        }
      },
      key.subject);
  return CanonicalValue(CanonicalValue::Object{
      {"origin_stop_generation",
       CanonicalValue(static_cast<double>(key.origin_stop_generation.value()))},
      {"origin_stop_id", CanonicalValue(key.origin_stop_id.value())},
      {"subject", subject},
  });
}

[[nodiscard]] CanonicalValue
StopHistoryScopeKeyArray(const std::vector<core::StopScopeEntryKeyDTO> &keys) {
  CanonicalValue::Array values;
  values.reserve(keys.size());
  for (const auto &key : keys) {
    values.emplace_back(StopHistoryScopeKeyValue(key));
  }
  return CanonicalValue(std::move(values));
}

[[nodiscard]] CanonicalValue
StopHistoryProfileValue(const core::MotionInterruptStopProfileDTO &profile) {
  CanonicalValue::Object jerk;
  for (const auto &[joint, limit] : profile.per_joint_max_jerk) {
    jerk.emplace_back(
        joint,
        std::visit(
            [](const auto &value) -> CanonicalValue {
              if constexpr (requires { value.value_rad_s3; })
                return CanonicalValue(CanonicalValue::Object{
                    {"coordinate_kind", CanonicalValue("angular_rad_s3")},
                    {"value_rad_s3", CanonicalValue(value.value_rad_s3)}});
              else
                return CanonicalValue(CanonicalValue::Object{
                    {"coordinate_kind", CanonicalValue("linear_m_s3")},
                    {"value_m_s3", CanonicalValue(value.value_m_s3)}});
            },
            limit));
  }
  CanonicalValue::Object deceleration;
  deceleration.reserve(profile.per_joint_max_deceleration.size());
  for (const auto &[joint, limit] : profile.per_joint_max_deceleration) {
    deceleration.emplace_back(joint, StopHistoryAccelerationValue(limit));
  }
  CanonicalValue settle(nullptr);
  if (profile.per_joint_settle_velocity_threshold.has_value()) {
    CanonicalValue::Object thresholds;
    thresholds.reserve(profile.per_joint_settle_velocity_threshold->size());
    for (const auto &[joint, limit] :
         *profile.per_joint_settle_velocity_threshold) {
      thresholds.emplace_back(joint, StopHistoryVelocityValue(limit));
    }
    settle = CanonicalValue(std::move(thresholds));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"allow_natural_completion_on_short_path",
       CanonicalValue(profile.allow_natural_completion_on_short_path)},
      {"deceleration_time_s", profile.deceleration_time_s.has_value()
                                  ? CanonicalValue(*profile.deceleration_time_s)
                                  : CanonicalValue(nullptr)},
      {"diagnostics", StopHistoryStructuredValue(profile.diagnostics)},
      {"jerk_limits_enabled", profile.jerk_limits_enabled.has_value()
                                  ? CanonicalValue(*profile.jerk_limits_enabled)
                                  : CanonicalValue(nullptr)},
      {"per_joint_max_jerk", CanonicalValue(std::move(jerk))},
      {"per_joint_max_deceleration", CanonicalValue(std::move(deceleration))},
      {"per_joint_settle_velocity_threshold", std::move(settle)},
      {"profile_id", CanonicalValue(profile.profile_id)},
      {"recoverable_substitution_mode",
       profile.recoverable_substitution_mode.has_value()
           ? CanonicalValue(std::string(MotionInterruptStopModeName(
                 *profile.recoverable_substitution_mode)))
           : CanonicalValue(nullptr)},
      {"require_backend_confirmation",
       CanonicalValue(profile.require_backend_confirmation)},
      {"stop_mode", CanonicalValue(std::string(
                        MotionInterruptStopModeName(profile.stop_mode)))},
  });
}

[[nodiscard]] CanonicalValue StopHistoryEvidenceSourceValue(
    const core::ResolvedStopEvidenceSourceDTO &source) {
  const auto &target = source.runtime_target;
  CanonicalValue contract(nullptr);
  if (source.provider_contract_ref.has_value()) {
    contract = CanonicalValue(CanonicalValue::Object{
        {"contract_schema_id",
         CanonicalValue(
             source.provider_contract_ref->contract_schema_id.value())},
        {"contract_schema_revision",
         CanonicalValue(
             source.provider_contract_ref->contract_schema_revision.value())},
        {"provider_contract_role_id",
         CanonicalValue(
             source.provider_contract_ref->provider_contract_role_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"provider_contract", std::move(contract)},
      {"runtime_target",
       CanonicalValue(CanonicalValue::Object{
           {"content_digest", CanonicalValue(target.content_digest.value())},
           {"endpoint_role_id",
            target.endpoint_role_id.has_value()
                ? CanonicalValue(target.endpoint_role_id->value())
                : CanonicalValue(nullptr)},
           {"provider_instance_id",
            CanonicalValue(target.provider_instance_id.value())},
           {"provider_manifest_id",
            CanonicalValue(target.provider_manifest_id.value())},
           {"provider_manifest_revision",
            CanonicalValue(target.provider_manifest_revision.value())},
           {"provider_runtime_epoch",
            CanonicalValue(target.provider_runtime_epoch.value())},
           {"runtime_unit_epoch",
            CanonicalValue(target.runtime_unit_epoch.value())},
           {"runtime_unit_id", CanonicalValue(target.runtime_unit_id.value())},
       })},
      {"source_kind",
       CanonicalValue(std::string(core::to_string(source.source_kind)))},
      {"state_authority_class",
       source.state_authority_class.has_value()
           ? CanonicalValue(
                 std::string(core::to_string(*source.state_authority_class)))
           : CanonicalValue(nullptr)},
      {"state_authority_id",
       source.state_authority_id.has_value()
           ? CanonicalValue(source.state_authority_id->value())
           : CanonicalValue(nullptr)},
  });
}

[[nodiscard]] CanonicalValue
StopHistoryEvidenceItemValue(const core::StopEvidenceItemDTO &item) {
  return CanonicalValue(CanonicalValue::Object{
      {"error", item.error.has_value() ? ErrorValue(*item.error)
                                       : CanonicalValue(nullptr)},
      {"evidence", item.evidence.has_value()
                       ? StopHistoryJsonValue(*item.evidence)
                       : CanonicalValue(nullptr)},
      {"evidence_item_id", CanonicalValue(item.evidence_item_id.value())},
      {"required_class",
       CanonicalValue(std::string(core::to_string(item.required_class)))},
      {"resolved_source", StopHistoryEvidenceSourceValue(item.resolved_source)},
      {"source_age_at_evaluation_ns",
       item.source_age_at_evaluation_ns.has_value()
           ? CanonicalValue(
                 static_cast<double>(*item.source_age_at_evaluation_ns))
           : CanonicalValue(nullptr)},
      {"source_epoch", item.source_epoch.has_value()
                           ? CanonicalValue(item.source_epoch->value())
                           : CanonicalValue(nullptr)},
      {"source_sequence",
       item.source_sequence.has_value()
           ? CanonicalValue(static_cast<double>(*item.source_sequence))
           : CanonicalValue(nullptr)},
      {"state", CanonicalValue(std::string(StopEvidenceStateName(item.state)))},
  });
}

[[nodiscard]] CanonicalValue
StopHistoryAttemptValue(const core::StopAttemptedStageResultDTO &attempt,
                        const std::int64_t accepted_at_ns) {
  const auto &result = attempt.result;
  const auto &request = result.request;
  Require(request.stage_index <
              request.participant_stop_plan.stage_commands.size(),
          "terminal Stop stage has no matching prepared command");
  const auto &prepared =
      request.participant_stop_plan.stage_commands[request.stage_index];
  const auto &stage = prepared.resolved_stage;
  CanonicalValue::Array resources;
  resources.reserve(request.resources.size());
  for (const auto &resource : request.resources) {
    resources.emplace_back(resource.value());
  }
  CanonicalValue::Array evidence;
  evidence.reserve(result.evidence_items.size());
  for (const auto &item : result.evidence_items) {
    evidence.emplace_back(StopHistoryEvidenceItemValue(item));
  }
  const auto acceptance_latency =
      attempt.completed_at_ns >= accepted_at_ns
          ? CanonicalValue(
                static_cast<double>(attempt.completed_at_ns - accepted_at_ns))
          : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance_to_stage_latency_ns", acceptance_latency},
      {"actual_stop_mode", result.actual_provider_contract_mode.has_value()
                               ? CanonicalValue(std::string(core::to_string(
                                     *result.actual_provider_contract_mode)))
                               : CanonicalValue(nullptr)},
      {"attempt_disposition",
       CanonicalValue(std::string(
           StopAttemptDispositionName(result.attempt_disposition)))},
      {"authority",
       CanonicalValue(std::string(StopStageAuthorityName(attempt.authority)))},
      {"completed_at_ns", manyforge::content_identity::nonnegative_int64_value(
                              attempt.completed_at_ns)},
      {"controller_acknowledgement_sequence",
       result.controller_acknowledgement_sequence.has_value()
           ? CanonicalValue(static_cast<double>(
                 *result.controller_acknowledgement_sequence))
           : CanonicalValue(nullptr)},
      {"error", result.error.has_value() ? ErrorValue(*result.error)
                                         : CanonicalValue(nullptr)},
      {"evidence_items", CanonicalValue(std::move(evidence))},
      {"operation_id", CanonicalValue(request.operation_id.value())},
      {"operation_revision",
       CanonicalValue(static_cast<double>(request.operation_revision.value()))},
      {"outcome",
       CanonicalValue(std::string(StopStageOutcomeName(result.outcome)))},
      {"participant_id", CanonicalValue(request.participant_id.value())},
      {"participant_stop_invocation_plan_digest",
       CanonicalValue(request.participant_stop_plan
                          .participant_stop_invocation_plan_digest.value())},
      {"profile",
       request.participant_stop_plan.participant_stop_profile.has_value()
           ? StopHistoryProfileValue(
                 *request.participant_stop_plan.participant_stop_profile)
           : CanonicalValue(nullptr)},
      {"profile_purpose",
       CanonicalValue(std::string(core::to_string(
           request.participant_stop_plan.participant_stop_profile_purpose)))},
      {"profile_source", prepared.parameter_source.has_value()
                             ? CanonicalValue(std::string(
                                   core::to_string(*prepared.parameter_source)))
                             : CanonicalValue(nullptr)},
      {"provider_runtime_epoch",
       CanonicalValue(request.provider_runtime_epoch.value())},
      {"receiver_elapsed_duration_ns",
       CanonicalValue(
           static_cast<double>(result.receiver_elapsed_duration_ns))},
      {"recoverable_substitution_reason",
       result.recoverable_substitution_reason.has_value()
           ? CanonicalValue(*result.recoverable_substitution_reason)
           : CanonicalValue(nullptr)},
      {"resolved_stop_base_plan_id",
       CanonicalValue(
           request.participant_stop_plan.resolved_stop_base_plan_id.value())},
      {"resolved_stop_scope_policy_digest",
       CanonicalValue(request.resolved_stop_scope_policy_digest.value())},
      {"resources", CanonicalValue(std::move(resources))},
      {"result_sequence",
       CanonicalValue(static_cast<double>(attempt.result_sequence))},
      {"source_epoch", result.source_epoch.has_value()
                           ? CanonicalValue(result.source_epoch->value())
                           : CanonicalValue(nullptr)},
      {"source_sequence",
       result.source_sequence.has_value()
           ? CanonicalValue(static_cast<double>(*result.source_sequence))
           : CanonicalValue(nullptr)},
      {"stage",
       CanonicalValue(CanonicalValue::Object{
           {"action_class",
            CanonicalValue(std::string(core::to_string(
                stage.command_qualification.stop_action_class)))},
           {"command_kind", CanonicalValue(std::string(core::to_string(
                                stage.containment_command.command_kind)))},
           {"default_contract_parameters",
            stage.default_contract_parameters.has_value()
                ? StopHistoryJsonValue(
                      core::JsonValueDTO{*stage.default_contract_parameters})
                : CanonicalValue(nullptr)},
           {"stage_id", CanonicalValue(stage.stop_escalation_stage_id.value())},
           {"stage_index",
            CanonicalValue(static_cast<double>(stage.stage_index))},
           {"stage_kind",
            CanonicalValue(std::string(core::to_string(stage.stage_kind)))},
           {"validated_contract_parameters",
            prepared.validated_contract_parameters.has_value()
                ? StopHistoryJsonValue(core::JsonValueDTO{
                      *prepared.validated_contract_parameters})
                : CanonicalValue(nullptr)},
       })},
      {"stop_evidence_policy_id",
       CanonicalValue(request.stop_evidence_policy_id.value())},
      {"stop_evidence_policy_revision",
       CanonicalValue(request.stop_evidence_policy_revision.value())},
      {"scope_key", StopHistoryScopeKeyValue(attempt.scope_key)},
  });
}

[[nodiscard]] CanonicalValue
StopHistorySettlementValue(const core::StopScopeEntryDTO &entry) {
  CanonicalValue facts = std::visit(
      [](const auto &typed) -> CanonicalValue {
        using Facts = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Facts,
                                     core::StopParticipantScopeFactsDTO>) {
          CanonicalValue::Array resources;
          resources.reserve(typed.resources.size());
          for (const auto &resource : typed.resources) {
            resources.emplace_back(resource.value());
          }
          return CanonicalValue(CanonicalValue::Object{
              {"command_target_id",
               CanonicalValue(typed.command_target_id.value())},
              {"controllable_component_id",
               CanonicalValue(typed.controllable_component_id.value())},
              {"controller_goal_generation",
               CanonicalValue(static_cast<double>(
                   typed.controller_goal_generation.value()))},
              {"controller_goal_id",
               CanonicalValue(typed.controller_goal_id.value())},
              {"kind", CanonicalValue("participant")},
              {"provider_runtime_epoch",
               CanonicalValue(typed.provider_runtime_epoch.value())},
              {"resources", CanonicalValue(std::move(resources))},
              {"terminal_settlement",
               CanonicalValue(std::string(
                   StopParticipantSettlementName(typed.terminal_settlement)))},
          });
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("resource")},
              {"resource_id", CanonicalValue(typed.value())},
              {"terminal_settlement", CanonicalValue("not_required")},
          });
        }
      },
      entry.facts);
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             entry.accepted_at_ns)},
      {"dispatch_state",
       CanonicalValue(static_cast<double>(entry.dispatch_state))},
      {"facts", std::move(facts)},
      {"participant_stop_invocation_plan_digest",
       entry.participant_stop_invocation_plan_digest.has_value()
           ? CanonicalValue(
                 entry.participant_stop_invocation_plan_digest->value())
           : CanonicalValue(nullptr)},
      {"scope_key", StopHistoryScopeKeyValue(entry.key)},
      {"stop_evidence_policy_id",
       entry.stop_evidence_policy_id.has_value()
           ? CanonicalValue(entry.stop_evidence_policy_id->value())
           : CanonicalValue(nullptr)},
      {"stop_evidence_policy_revision",
       entry.stop_evidence_policy_revision.has_value()
           ? CanonicalValue(entry.stop_evidence_policy_revision->value())
           : CanonicalValue(nullptr)},
  });
}

[[nodiscard]] CanonicalValue
StopHistoryFaultValue(const std::optional<core::StopFaultLatchDTO> &fault) {
  if (!fault.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"captured_operation_count", CanonicalValue(static_cast<double>(
                                       fault->captured_operation_ids.size()))},
      {"captured_resource_count",
       CanonicalValue(static_cast<double>(fault->captured_resources.size()))},
      {"cell_id", CanonicalValue(fault->cell_id.value())},
      {"latch_generation",
       CanonicalValue(static_cast<double>(fault->latch_generation.value()))},
      {"latch_id", CanonicalValue(fault->latch_id.value())},
      {"persistence_degraded", CanonicalValue(fault->persistence_degraded)},
      {"reason_code", CanonicalValue(fault->reason_code)},
      {"recovery_assessment_revision",
       CanonicalValue(
           static_cast<double>(fault->recovery_assessment_revision))},
      {"recovery_status",
       CanonicalValue(
           std::string(StopFaultRecoveryStatusName(fault->recovery_status)))},
      {"state",
       CanonicalValue(std::string(StopFaultLatchStateName(fault->state)))},
      {"stop_certificate",
       fault->stop_certificate.has_value()
           ? StopCertificateRefValue(*fault->stop_certificate)
           : CanonicalValue(nullptr)},
      {"stop_generation",
       CanonicalValue(static_cast<double>(fault->stop_generation.value()))},
      {"stop_id", CanonicalValue(fault->stop_id.value())},
      {"trigger",
       CanonicalValue(std::string(StopFaultTriggerName(fault->trigger)))},
  });
}

[[nodiscard]] CanonicalValue StopTerminalHistoryProjectionValue(
    const core::StopJournalTerminalProjectionDTO &projection,
    const std::string_view source_owner_epoch,
    const std::string_view durable_projection_digest,
    const bool include_details) {
  const auto &status = projection.stop_status;
  const auto &certificate = projection.certificate;
  CanonicalValue::Array attempted_stages;
  CanonicalValue::Array participant_settlement;
  CanonicalValue::Array certificate_evidence;
  CanonicalValue::Array predecessor_certificates;
  if (include_details) {
    attempted_stages.reserve(certificate.attempted_stage_results.size());
    for (const auto &attempt : certificate.attempted_stage_results) {
      attempted_stages.emplace_back(
          StopHistoryAttemptValue(attempt, status.accepted.accepted_at_ns));
    }
    participant_settlement.reserve(certificate.scope_entries.size());
    for (const auto &entry : certificate.scope_entries) {
      participant_settlement.emplace_back(StopHistorySettlementValue(entry));
    }
    certificate_evidence.reserve(certificate.evidence_items.size());
    for (const auto &item : certificate.evidence_items) {
      certificate_evidence.emplace_back(CanonicalValue::Object{
          {"evidence_item_id",
           CanonicalValue(item.evidence_item.evidence_item_id.value())},
          {"required_class", CanonicalValue(std::string(core::to_string(
                                 item.evidence_item.required_class)))},
          {"result_sequence",
           CanonicalValue(static_cast<double>(item.result_sequence))},
          {"scope_key", StopHistoryScopeKeyValue(item.scope_key)},
          {"stage_id", CanonicalValue(item.stage_id.value())},
          {"stage_index",
           CanonicalValue(static_cast<double>(item.stage_index))},
          {"state", CanonicalValue(std::string(
                        StopEvidenceStateName(item.evidence_item.state)))},
      });
    }
    predecessor_certificates.reserve(
        certificate.predecessor_certificates.size());
    for (const auto &reference : certificate.predecessor_certificates) {
      predecessor_certificates.emplace_back(StopCertificateRefValue(reference));
    }
  }
  const auto duration =
      certificate.completed_at_ns >= status.accepted.accepted_at_ns
          ? CanonicalValue(static_cast<double>(certificate.completed_at_ns -
                                               status.accepted.accepted_at_ns))
          : CanonicalValue(nullptr);
  const bool escalated =
      certificate.result == core::StopCertificateResultDTO::kSucceededEscalated;
  const bool hard_stop_floor_used = std::any_of(
      certificate.attempted_stage_results.begin(),
      certificate.attempted_stage_results.end(), [](const auto &attempt) {
        const auto &request = attempt.result.request;
        return request.stage_index <
                   request.participant_stop_plan.stage_commands.size() &&
               request.participant_stop_plan.stage_commands[request.stage_index]
                       .resolved_stage.command_qualification
                       .stop_action_class != core::StopActionClass::kControlled;
      });
  const auto acceptance =
      include_details
          ? StopAcceptedValue(status.accepted)
          : CanonicalValue(CanonicalValue::Object{
                {"accepted_at_ns",
                 manyforge::content_identity::nonnegative_int64_value(
                     status.accepted.accepted_at_ns)},
                {"request_id",
                 CanonicalValue(status.accepted.request_id.value())},
                {"stop_generation",
                 CanonicalValue(static_cast<double>(
                     status.accepted.stop_generation.value()))},
                {"stop_id", CanonicalValue(status.accepted.stop_id.value())},
            });
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance", acceptance},
      {"acceptance_to_terminal_latency_ns", duration},
      {"api_version", CanonicalValue("manyforge.stop_terminal_projection.v1")},
      {"attempted_stage_count",
       CanonicalValue(
           static_cast<double>(certificate.attempted_stage_results.size()))},
      {"attempted_stages", CanonicalValue(std::move(attempted_stages))},
      {"certificate_evidence", CanonicalValue(std::move(certificate_evidence))},
      {"certificate_evidence_count",
       CanonicalValue(static_cast<double>(certificate.evidence_items.size()))},
      {"completed_at_ns", manyforge::content_identity::nonnegative_int64_value(
                              certificate.completed_at_ns)},
      {"details_truncated", CanonicalValue(!include_details)},
      {"dispatched_stopped",
       include_details
           ? StopHistoryScopeKeyArray(certificate.dispatched_stopped)
           : CanonicalValue(CanonicalValue::Array{})},
      {"dispatched_stopped_count", CanonicalValue(static_cast<double>(
                                       certificate.dispatched_stopped.size()))},
      {"durable_projection_digest",
       CanonicalValue(std::string(durable_projection_digest))},
      {"escalated", CanonicalValue(escalated)},
      {"failed", include_details ? StopHistoryScopeKeyArray(certificate.failed)
                                 : CanonicalValue(CanonicalValue::Array{})},
      {"failed_count",
       CanonicalValue(static_cast<double>(certificate.failed.size()))},
      {"fault_latch", StopHistoryFaultValue(projection.fault_latch)},
      {"hard_stop_floor_used", CanonicalValue(hard_stop_floor_used)},
      {"owner_epoch", CanonicalValue(std::string(source_owner_epoch))},
      {"participant_settlement",
       CanonicalValue(std::move(participant_settlement))},
      {"participant_settlement_count",
       CanonicalValue(static_cast<double>(certificate.scope_entries.size()))},
      {"pre_dispatch_cancelled",
       include_details
           ? StopHistoryScopeKeyArray(certificate.pre_dispatch_cancelled)
           : CanonicalValue(CanonicalValue::Array{})},
      {"pre_dispatch_cancelled_count",
       CanonicalValue(
           static_cast<double>(certificate.pre_dispatch_cancelled.size()))},
      {"predecessor_certificates",
       CanonicalValue(std::move(predecessor_certificates))},
      {"predecessor_certificate_count",
       CanonicalValue(
           static_cast<double>(certificate.predecessor_certificates.size()))},
      {"record_kind", CanonicalValue("stop_terminal_projected")},
      {"request_id", CanonicalValue(status.accepted.request_id.value())},
      {"requested_selector",
       CanonicalValue(CanonicalValue::Object{
           {"kind", CanonicalValue(std::string(StopSelectorKindName(
                        certificate.requested_selector.kind)))},
           {"malformed_ingress_reason",
            certificate.requested_selector.malformed_ingress_reason.has_value()
                ? CanonicalValue(
                      *certificate.requested_selector.malformed_ingress_reason)
                : CanonicalValue(nullptr)},
           {"requested_selector_digest",
            CanonicalValue(certificate.requested_selector
                               .requested_selector_digest.value())},
       })},
      {"resolution_disposition", CanonicalValue(std::string(core::to_string(
                                     certificate.resolution_disposition)))},
      {"resolution_reason", CanonicalValue(std::string(core::to_string(
                                certificate.resolution_reason)))},
      {"result", CanonicalValue(std::string(
                     StopCertificateResultName(certificate.result)))},
      {"scope_entry_count",
       CanonicalValue(static_cast<double>(certificate.scope_entries.size()))},
      {"state", CanonicalValue(std::string(StopStatusStateName(status.state)))},
      {"stop_certificate_id",
       CanonicalValue(certificate.stop_certificate_id.value())},
      {"stop_certificate_revision",
       CanonicalValue(
           static_cast<double>(certificate.stop_certificate_revision.value()))},
      {"stop_generation", CanonicalValue(static_cast<double>(
                              certificate.stop_generation.value()))},
      {"stop_id", CanonicalValue(certificate.stop_id.value())},
      {"terminal_error", certificate.terminal_error.has_value()
                             ? ErrorValue(*certificate.terminal_error)
                             : CanonicalValue(nullptr)},
      {"terminal_projection",
       StopProjectionStatusValue(status.terminal_projection)},
      {"unsettled", include_details
                        ? StopHistoryScopeKeyArray(certificate.unsettled)
                        : CanonicalValue(CanonicalValue::Array{})},
      {"unsettled_count",
       CanonicalValue(static_cast<double>(certificate.unsettled.size()))},
  });
}

[[nodiscard]] CanonicalValue StopFaultClearHistoryProjectionValue(
    const core::StopJournalFaultClearProjectionDTO &projection,
    const std::string_view source_owner_epoch) {
  const auto &result = projection.recovery_result;
  const auto &latch = result.latch;
  Require(
      result.accepted && !result.error.has_value() &&
          latch.state == core::StopFaultLatchStateDTO::kCleared &&
          latch.stop_certificate.has_value(),
      "durable Stop fault-clear projection is not a successful exact clear");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue("manyforge.stop_fault_clear_projection.v1")},
      {"fault_latch",
       StopHistoryFaultValue(std::optional<core::StopFaultLatchDTO>(latch))},
      {"owner_epoch", CanonicalValue(std::string(source_owner_epoch))},
      {"record_kind", CanonicalValue("stop_fault_cleared")},
      {"request_id", CanonicalValue(result.request_id.value())},
      {"result",
       CanonicalValue(CanonicalValue::Object{
           {"accepted", CanonicalValue(true)},
           {"error", CanonicalValue(nullptr)},
           {"replay_status",
            CanonicalValue(std::string(core::to_string(result.replay_status)))},
           {"request_id", CanonicalValue(result.request_id.value())},
       })},
      {"stop_certificate_id",
       CanonicalValue(latch.stop_certificate->stop_certificate_id.value())},
      {"stop_certificate_revision",
       CanonicalValue(static_cast<double>(
           latch.stop_certificate->stop_certificate_revision.value()))},
      {"stop_generation",
       CanonicalValue(static_cast<double>(latch.stop_generation.value()))},
      {"stop_id", CanonicalValue(latch.stop_id.value())},
  });
}

[[nodiscard]] CanonicalValue BoundedStopTerminalHistoryProjectionValue(
    const core::StopJournalTerminalProjectionDTO &projection,
    const std::string_view source_owner_epoch,
    const std::string_view durable_projection_digest) {
  auto detailed = StopTerminalHistoryProjectionValue(
      projection, source_owner_epoch, durable_projection_digest, true);
  if (content_identity::canonical_json(detailed).size() <=
      kMaximumStopHistoryProjectionBytes) {
    return detailed;
  }
  auto bounded = StopTerminalHistoryProjectionValue(
      projection, source_owner_epoch, durable_projection_digest, false);
  Require(content_identity::canonical_json(bounded).size() <=
              kMaximumStopHistoryProjectionBytes,
          "bounded terminal Stop history projection exceeds its byte limit");
  return bounded;
}

[[nodiscard]] CanonicalValue
CellStopActionValue(const core::CellStopActionProjectionDTO &value) {
  CanonicalValue terminal(nullptr);
  if (value.latest_terminal.has_value()) {
    CanonicalValue::Array stages;
    stages.reserve(value.latest_terminal->executed_stages.size());
    for (const auto &stage : value.latest_terminal->executed_stages) {
      stages.emplace_back(CanonicalValue::Object{
          {"action_class",
           CanonicalValue(std::string(core::to_string(stage.action_class)))},
          {"executed_mode", stage.executed_mode.has_value()
                                ? CanonicalValue(std::string(
                                      core::to_string(*stage.executed_mode)))
                                : CanonicalValue(nullptr)},
          {"operation_id", CanonicalValue(stage.operation_id.value())},
          {"outcome",
           CanonicalValue(std::string(StopStageOutcomeName(stage.outcome)))},
          {"participant_id", CanonicalValue(stage.participant_id.value())},
          {"stage_id", CanonicalValue(stage.stage_id.value())},
          {"stage_index",
           CanonicalValue(static_cast<double>(stage.stage_index))},
          {"substitution_reason",
           stage.substitution_reason.has_value()
               ? CanonicalValue(*stage.substitution_reason)
               : CanonicalValue(nullptr)},
      });
    }
    terminal = CanonicalValue(CanonicalValue::Object{
        {"certificate",
         StopCertificateRefValue(value.latest_terminal->certificate)},
        {"escalated", CanonicalValue(value.latest_terminal->escalated)},
        {"executed_stages", CanonicalValue(std::move(stages))},
        {"hard_stop_floor_used",
         CanonicalValue(value.latest_terminal->hard_stop_floor_used)},
        {"result", CanonicalValue(std::string(StopCertificateResultName(
                       value.latest_terminal->result)))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"available", CanonicalValue(true)},
      {"expected_stop_generation",
       CanonicalValue(
           static_cast<double>(value.expected_stop_generation.value()))},
      {"fault",
       value.fault.has_value()
           ? CanonicalValue(CanonicalValue::Object{
                 {"certificate",
                  value.fault->certificate.has_value()
                      ? StopCertificateRefValue(*value.fault->certificate)
                      : CanonicalValue(nullptr)},
                 {"latch_generation",
                  CanonicalValue(static_cast<double>(
                      value.fault->latch_generation.value()))},
                 {"latch_id", CanonicalValue(value.fault->latch_id.value())},
                 {"reason_code", CanonicalValue(value.fault->reason_code)},
                 {"recovery_status",
                  CanonicalValue(std::string(StopFaultRecoveryStatusName(
                      value.fault->recovery_status)))},
                 {"state", CanonicalValue(std::string(
                               StopFaultLatchStateName(value.fault->state)))},
                 {"trigger", CanonicalValue(std::string(
                                 StopFaultTriggerName(value.fault->trigger)))},
             })
           : CanonicalValue(nullptr)},
      {"latest_terminal", std::move(terminal)},
      {"unavailable_reason", CanonicalValue(nullptr)},
  });
}

[[nodiscard]] CanonicalValue
RecoveryAssessmentValue(const core::KernelRecoveryAssessmentDTO &value) {
  CanonicalValue::Array actions;
  actions.reserve(value.allowed_actions.size());
  for (const auto action : value.allowed_actions) {
    actions.emplace_back(core::recovery_action_name(action));
  }
  CanonicalValue::Array evidence;
  evidence.reserve(value.evidence_refs.size());
  for (const auto &reference : value.evidence_refs) {
    evidence.emplace_back(reference);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"allowed_actions", CanonicalValue(std::move(actions))},
      {"assessed_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.assessed_at_ns)},
      {"assessment_revision",
       CanonicalValue(static_cast<double>(value.assessment_revision))},
      {"backend_stop_confirmed", CanonicalValue(value.backend_stop_confirmed)},
      {"control_command_epoch",
       CanonicalValue(static_cast<double>(value.control_command_epoch))},
      {"evidence_refs", CanonicalValue(std::move(evidence))},
      {"execution_id", CanonicalValue(static_cast<double>(value.execution_id))},
      {"execution_state",
       CanonicalValue(std::string(core::to_string(value.execution_state)))},
      {"latch_id", CanonicalValue(value.latch_id)},
      {"observed_state_at_rest", CanonicalValue(value.observed_state_at_rest)},
      {"observed_state_authority_profile",
       CanonicalValue(value.observed_state_authority_profile)},
      {"observed_state_available",
       CanonicalValue(value.observed_state_available)},
      {"observed_state_fresh", CanonicalValue(value.observed_state_fresh)},
      {"observed_state_measured",
       CanonicalValue(value.observed_state_measured)},
      {"observed_state_sequence",
       CanonicalValue(static_cast<double>(value.observed_state_sequence))},
      {"observed_state_source_id",
       CanonicalValue(value.observed_state_source_id)},
      {"observed_state_valid_until_ns",
       manyforge::content_identity::nonnegative_int64_value(
           value.observed_state_valid_until_ns)},
      {"policy_id", CanonicalValue(value.policy_id)},
      {"preemption_result",
       CanonicalValue(std::string(
           core::interrupt_preemption_result_name(value.preemption_result)))},
      {"queued_latch_present", CanonicalValue(value.queued_latch_present)},
      {"reason_code", CanonicalValue(value.reason_code)},
      {"recovery_class", CanonicalValue(std::string(
                             core::recovery_class_name(value.recovery_class)))},
      {"retry_admissible", CanonicalValue(value.retry_admissible)},
      {"schema_version", CanonicalValue(value.schema_version)},
      {"source_id", CanonicalValue(value.source_id)},
      {"status", CanonicalValue(std::string(
                     core::recovery_assessment_status_name(value.status)))},
      {"stop_certificate_id", CanonicalValue(value.stop_certificate_id)},
      {"stop_certificate_revision",
       CanonicalValue(static_cast<double>(value.stop_certificate_revision))},
      {"stop_owner_active", CanonicalValue(value.stop_owner_active)},
      {"stop_owner_generation",
       CanonicalValue(static_cast<double>(value.stop_owner_generation))},
      {"stronger_authority_active",
       CanonicalValue(value.stronger_authority_active)},
  });
}

[[nodiscard]] CanonicalValue RecoveryParticipantsValue(
    const std::vector<core::OperationRecoveryParticipantDTO> &values) {
  CanonicalValue::Array participants;
  participants.reserve(values.size());
  for (const auto &value : values) {
    CanonicalValue::Array resources;
    resources.reserve(value.resource_ids.size());
    for (const auto &resource : value.resource_ids) {
      resources.emplace_back(resource.value());
    }
    participants.emplace_back(CanonicalValue::Object{
        {"active_control_configuration_generation",
         CanonicalValue(static_cast<double>(
             value.active_control_configuration_generation.value()))},
        {"active_control_configuration_id",
         CanonicalValue(value.active_control_configuration_id.value())},
        {"capability_id", CanonicalValue(value.capability_id.value())},
        {"capability_provider_binding_id",
         value.capability_provider_binding_id.has_value()
             ? CanonicalValue(value.capability_provider_binding_id->value())
             : CanonicalValue(nullptr)},
        {"command_target_id", CanonicalValue(value.command_target_id.value())},
        {"controllable_component_id",
         CanonicalValue(value.controllable_component_id.value())},
        {"control_binding_id",
         CanonicalValue(value.control_binding_id.value())},
        {"control_domain_id", CanonicalValue(value.control_domain_id.value())},
        {"control_route_id", CanonicalValue(value.control_route_id.value())},
        {"operation_participant_id",
         CanonicalValue(value.operation_participant_id.value())},
        {"resource_ids", CanonicalValue(std::move(resources))},
    });
  }
  return CanonicalValue(std::move(participants));
}

[[nodiscard]] CanonicalValue
RecoveryActionResultValue(const core::KernelRecoveryActionResultDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted", CanonicalValue(value.accepted)},
      {"action",
       CanonicalValue(std::string(core::recovery_action_name(value.action)))},
      {"error", value.error.has_value() ? ErrorValue(*value.error)
                                        : CanonicalValue(nullptr)},
      {"request_id", CanonicalValue(value.request_id)},
      {"resulting_assessment",
       RecoveryAssessmentValue(value.resulting_assessment)},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleControlSelectorValue(
    const core::RuntimeScaleControlSelectorDTO &value) {
  if (value.group_targets.empty()) {
    return CanonicalValue(CanonicalValue::Object{
        {"component_role_id", CanonicalValue(value.component_role_id.value())},
        {"program_binding_id",
         CanonicalValue(value.program_binding_id.value())},
        {"target_role_id", CanonicalValue(value.target_role_id.value())},
    });
  }
  CanonicalValue::Array targets;
  targets.reserve(value.group_targets.size());
  for (const auto &target : value.group_targets) {
    targets.emplace_back(CanonicalValue::Object{
        {"component_role_id", CanonicalValue(target.component_role_id.value())},
        {"target_role_id", CanonicalValue(target.target_role_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"group_targets", CanonicalValue(std::move(targets))},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleControlAcceptedValue(
    const core::RuntimeScaleControlAcceptedDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", manyforge::content_identity::nonnegative_int64_value(
                             value.accepted_at_ns)},
      {"active_operation_id",
       value.active_operation_id.has_value()
           ? CanonicalValue(value.active_operation_id->value())
           : CanonicalValue(nullptr)},
      {"active_operation_revision",
       value.active_operation_revision.has_value()
           ? CanonicalValue(
                 static_cast<double>(value.active_operation_revision->value()))
           : CanonicalValue(nullptr)},
      {"active_operation_scale_generation",
       value.active_operation_scale_generation.has_value()
           ? CanonicalValue(static_cast<double>(
                 value.active_operation_scale_generation->value()))
           : CanonicalValue(nullptr)},
      {"control_generation",
       CanonicalValue(static_cast<double>(value.control_generation.value()))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
      {"requested_scale", CanonicalValue(value.requested_scale)},
      {"selector", RuntimeScaleControlSelectorValue(value.selector)},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleControlStatusValue(
    const core::RuntimeScaleControlStatusDTO &value) {
  CanonicalValue::Array group_members;
  group_members.reserve(value.group_members.size());
  for (const auto &member : value.group_members) {
    group_members.emplace_back(CanonicalValue::Object{
        {"active_control_configuration_generation",
         CanonicalValue(static_cast<double>(
             member.active_control_configuration_generation.value()))},
        {"active_control_configuration_id",
         CanonicalValue(member.active_control_configuration_id.value())},
        {"control_binding_id",
         CanonicalValue(member.control_binding_id.value())},
        {"control_binding_revision",
         CanonicalValue(member.control_binding_revision.value())},
        {"control_domain_id", CanonicalValue(member.control_domain_id.value())},
        {"control_domain_revision",
         CanonicalValue(member.control_domain_revision.value())},
        {"scaling_domain_id", CanonicalValue(member.scaling_domain_id.value())},
        {"component_role_id",
         CanonicalValue(member.target.component_role_id.value())},
        {"target_role_id",
         CanonicalValue(member.target.target_role_id.value())},
    });
  }
  CanonicalValue::Object object{
      {"active_control_configuration_generation",
       value.active_control_configuration_generation.has_value()
           ? CanonicalValue(static_cast<double>(
                 value.active_control_configuration_generation->value()))
           : CanonicalValue(nullptr)},
      {"active_control_configuration_id",
       value.active_control_configuration_id.has_value()
           ? CanonicalValue(value.active_control_configuration_id->value())
           : CanonicalValue(nullptr)},
      {"active_operation_id",
       value.active_operation_id.has_value()
           ? CanonicalValue(value.active_operation_id->value())
           : CanonicalValue(nullptr)},
      {"active_operation_revision",
       value.active_operation_revision.has_value()
           ? CanonicalValue(
                 static_cast<double>(value.active_operation_revision->value()))
           : CanonicalValue(nullptr)},
      {"active_operation_scale_generation",
       value.active_operation_scale_generation.has_value()
           ? CanonicalValue(static_cast<double>(
                 value.active_operation_scale_generation->value()))
           : CanonicalValue(nullptr)},
      {"application_state", value.application_state.has_value()
                                ? CanonicalValue(std::string(core::to_string(
                                      *value.application_state)))
                                : CanonicalValue(nullptr)},
      {"control_binding_id",
       value.control_binding_id.has_value()
           ? CanonicalValue(value.control_binding_id->value())
           : CanonicalValue(nullptr)},
      {"control_binding_revision",
       value.control_binding_revision.has_value()
           ? CanonicalValue(value.control_binding_revision->value())
           : CanonicalValue(nullptr)},
      {"control_domain_id",
       value.control_domain_id.has_value()
           ? CanonicalValue(value.control_domain_id->value())
           : CanonicalValue(nullptr)},
      {"control_domain_revision",
       value.control_domain_revision.has_value()
           ? CanonicalValue(value.control_domain_revision->value())
           : CanonicalValue(nullptr)},
      {"control_generation",
       CanonicalValue(static_cast<double>(value.control_generation.value()))},
      {"effective_scale", value.effective_scale.has_value()
                              ? CanonicalValue(*value.effective_scale)
                              : CanonicalValue(nullptr)},
      {"frozen", CanonicalValue(value.frozen)},
      {"reason", value.reason.has_value() ? ErrorValue(*value.reason)
                                          : CanonicalValue(nullptr)},
      {"requested_scale", CanonicalValue(value.requested_scale)},
      {"scaling_domain_id",
       value.scaling_domain_id.has_value()
           ? CanonicalValue(value.scaling_domain_id->value())
           : CanonicalValue(nullptr)},
      {"selector", RuntimeScaleControlSelectorValue(value.selector)},
      {"supported", CanonicalValue(value.supported)},
  };
  if (!group_members.empty()) {
    object.emplace_back("group_members",
                        CanonicalValue(std::move(group_members)));
  }
  return CanonicalValue(std::move(object));
}

[[nodiscard]] CanonicalValue
OperationStateValue(const core::OperationStateDTO &value) {
  CanonicalValue::Array participants;
  participants.reserve(value.participants.size());
  for (const auto &participant : value.participants) {
    CanonicalValue::Array resources;
    resources.reserve(participant.resources.size());
    for (const auto &resource : participant.resources) {
      resources.emplace_back(resource.value());
    }
    participants.emplace_back(CanonicalValue::Object{
        {"capability_id", CanonicalValue(participant.capability_id.value())},
        {"capability_provider_binding_id",
         participant.capability_provider_binding_id.has_value()
             ? CanonicalValue(
                   participant.capability_provider_binding_id->value())
             : CanonicalValue(nullptr)},
        {"capability_provider_binding_revision",
         participant.capability_provider_binding_revision.has_value()
             ? CanonicalValue(
                   participant.capability_provider_binding_revision->value())
             : CanonicalValue(nullptr)},
        {"command_target_id",
         CanonicalValue(participant.command_target_id.value())},
        {"controllable_component_id",
         CanonicalValue(participant.controllable_component_id.value())},
        {"control_binding_id",
         CanonicalValue(participant.control_binding_id.value())},
        {"control_binding_revision",
         CanonicalValue(participant.control_binding_revision.value())},
        {"control_domain_id",
         CanonicalValue(participant.control_domain_id.value())},
        {"control_domain_revision",
         CanonicalValue(participant.control_domain_revision.value())},
        {"controller_goal_generation",
         CanonicalValue(static_cast<double>(
             participant.controller_goal_generation.value()))},
        {"controller_goal_id",
         CanonicalValue(participant.controller_goal_id.value())},
        {"participant_id", CanonicalValue(participant.participant_id.value())},
        {"payload_kind",
         CanonicalValue(participant.payload_kind ==
                                core::OperationParticipantPayloadKind::kMotion
                            ? "motion"
                            : "capability_command")},
        {"progress", CanonicalValue(participant.progress)},
        {"resources", CanonicalValue(std::move(resources))},
        {"state", CanonicalValue(std::string(StateName(participant.state)))},
        {"terminal_error", participant.terminal_error.has_value()
                               ? ErrorValue(*participant.terminal_error)
                               : CanonicalValue(nullptr)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active_stop_generation",
       value.active_stop_generation.has_value()
           ? CanonicalValue(
                 static_cast<double>(value.active_stop_generation->value()))
           : CanonicalValue(nullptr)},
      {"active_stop_id", value.active_stop_id.has_value()
                             ? CanonicalValue(value.active_stop_id->value())
                             : CanonicalValue(nullptr)},
      {"coordination_policy",
       CanonicalValue(value.coordination_policy ==
                              core::OperationCoordinationPolicy::kCoordinatedAll
                          ? "coordinated_all"
                          : "independent")},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision",
       CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participants", CanonicalValue(std::move(participants))},
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"program_run_id", CanonicalValue(value.program_run_id.value())},
      {"runtime_scale_generation",
       value.runtime_scale_generation.has_value()
           ? CanonicalValue(
                 static_cast<double>(value.runtime_scale_generation->value()))
           : CanonicalValue(nullptr)},
      {"state", CanonicalValue(std::string(StateName(value.state)))},
      {"updated_at_ns", manyforge::content_identity::nonnegative_int64_value(
                            value.updated_at_ns)},
  });
}

[[nodiscard]] CanonicalValue
OperationEventPageValue(const core::OperationJournalEventPageDTO &page) {
  CanonicalValue::Array events;
  events.reserve(page.events.size());
  for (const auto &event : page.events) {
    CanonicalValue payload;
    std::string payload_digest = event.payload_digest;
    if (event.record_kind == "stop_terminal_projected") {
      const auto terminal =
          core::DecodeStopJournalTerminalProjection(event.payload_json);
      Require(terminal.ok(),
              "durable terminal Stop projection could not be decoded: " +
                  (terminal.ok() ? std::string{} : terminal.error().message));
      payload = BoundedStopTerminalHistoryProjectionValue(
          terminal.value(), event.owner_epoch.value(), event.payload_digest);
      payload_digest = content_identity::sha256_digest(
          content_identity::canonical_json(payload));
    } else if (event.record_kind == "stop_fault_cleared") {
      const auto cleared =
          core::DecodeStopJournalFaultClearProjection(event.payload_json);
      Require(cleared.ok(),
              "durable Stop fault-clear projection could not be decoded: " +
                  (cleared.ok() ? std::string{} : cleared.error().message));
      payload = StopFaultClearHistoryProjectionValue(cleared.value(),
                                                     event.owner_epoch.value());
      payload_digest = content_identity::sha256_digest(
          content_identity::canonical_json(payload));
    } else {
      try {
        payload =
            deployment::parse_strict_json_model_yaml(event.payload_json).root();
      } catch (const std::exception &error) {
        Fail("durable operation event payload is not strict JSON: " +
             std::string(error.what()));
      }
      Require(content_identity::canonical_json(payload) == event.payload_json,
              "durable operation event payload is not canonical JSON");
    }
    events.emplace_back(CanonicalValue::Object{
        {"effect_pending", CanonicalValue(event.effect_pending)},
        {"occurred_at_ns", manyforge::content_identity::nonnegative_int64_value(
                               event.occurred_at_ns)},
        {"occurred_at_wall", CanonicalValue(event.occurred_at_wall)},
        {"operation_id", event.operation_id.has_value()
                             ? CanonicalValue(event.operation_id->value())
                             : CanonicalValue(nullptr)},
        {"operation_revision", event.operation_revision.has_value()
                                   ? CanonicalValue(static_cast<double>(
                                         event.operation_revision->value()))
                                   : CanonicalValue(nullptr)},
        {"owner_epoch", CanonicalValue(event.owner_epoch.value())},
        {"payload", std::move(payload)},
        {"payload_digest", CanonicalValue(std::move(payload_digest))},
        {"record_kind", CanonicalValue(event.record_kind)},
        {"request_id", event.request_id.has_value()
                           ? CanonicalValue(event.request_id->value())
                           : CanonicalValue(nullptr)},
        {"sequence", CanonicalValue(static_cast<double>(event.sequence))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"events", CanonicalValue(std::move(events))},
      {"has_more", CanonicalValue(page.has_more)},
      {"next_after_sequence",
       CanonicalValue(static_cast<double>(page.next_after_sequence))},
      {"owner_epoch", CanonicalValue(page.owner_epoch.value())},
      {"schema_version", CanonicalValue("manyforge.execution_event_page.v1")},
  });
}

[[nodiscard]] CanonicalValue
CoordinatePositionValue(const core::CoordinatePositionDTO &position) {
  if (const auto *angular = std::get_if<core::AngularPositionDTO>(&position)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("angular_rad")},
        {"value_rad", CanonicalValue(angular->value_rad)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("linear_m")},
      {"value_m",
       CanonicalValue(std::get<core::LinearPositionDTO>(position).value_m)},
  });
}

[[nodiscard]] RoboPlanCandidateBindingDTO
CandidateBinding(const core::RuntimeParticipantBinding &runtime) {
  const auto &value = runtime.value();
  Require(value.model_groups.size() == 1U,
          "motion binding has no sole retained model group");
  const auto &group = value.model_groups.front();
  const auto model = std::find_if(
      value.model_bindings.begin(), value.model_bindings.end(),
      [&](const auto &candidate) {
        return candidate.model_instance_id == group.model_instance_id;
      });
  Require(model != value.model_bindings.end(),
          "motion model group has no retained model revision");
  std::vector<core::ResourceId> resources;
  std::vector<std::string> compiled_joint_names;
  resources.reserve(value.resources.size());
  compiled_joint_names.reserve(value.resources.size());
  for (const auto &resource : value.resources) {
    resources.push_back(resource.resource_id);
    compiled_joint_names.push_back(resource.compiled_resource_name);
  }
  std::vector<core::ResolvedFrameDTO> frames;
  frames.reserve(value.frame_bindings.size());
  for (const auto &frame : value.frame_bindings) {
    frames.emplace_back(core::ResolvedComponentModelFrameDTO{
        value.controllable_component_id, frame.model_instance_id,
        frame.frame_role_id, frame.compiled_frame_name, frame.model_revision});
  }
  return RoboPlanCandidateBindingDTO{
      value.controllable_component_id, value.command_target_id,
      group.model_instance_id,         model->model_revision,
      group.compiled_group_name,       std::move(resources),
      std::move(compiled_joint_names), std::move(frames)};
}

[[nodiscard]] core::LogicalSceneSnapshotDTO
EmptyScene(const core::CellId &cell_id, const core::SceneRevision &revision) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U,
                     std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  core::LogicalSceneSnapshotDTO snapshot{revision, cell_id,           0U, 0U,
                                         0U,       std::move(policy), {}, {}};
  Require(core::logical_scene_identity::ComputeLogicalSceneContentHash(
              snapshot) == revision.content_hash &&
              snapshot.locally_valid(),
          "active scene revision is not the exact canonical empty scene; "
          "live scene consumption starts at N5");
  return snapshot;
}

[[nodiscard]] core::ProjectionRevisionDTO
DecodeProjectionRevision(const CanonicalValue &value) {
  RequireExactObjectSize(value, 3U, "projection revision");
  core::ProjectionRevisionDTO result{
      core::RetainedBundleId(Text(value, "retained_bundle_id")),
      core::GraphRevision(Text(value, "graph_revision")),
      DecodeSceneRevision(Member(value, "scene_revision"))};
  Require(result.valid(), "projection revision is invalid");
  return result;
}

[[nodiscard]] core::ErrorDTO DecodeManagerError(const CanonicalValue &value) {
  Require(value.is_object() && (value.as_object().size() == 2U ||
                                value.as_object().size() == 3U),
          "Manager error has an unknown or missing field");
  const auto code = Text(value, "code");
  const auto message = Text(value, "message");
  std::map<std::string, std::string> details;
  if (const auto *encoded_details = value.find_object_member("details")) {
    Require(encoded_details->is_object(),
            "Manager error details must be an object");
    for (const auto &[key, encoded_value] : encoded_details->as_object()) {
      const auto *text = std::get_if<std::string>(&encoded_value.storage());
      Require(!key.empty() && text != nullptr,
              "Manager error detail is invalid");
      details.emplace(key, *text);
    }
  }
  return {code, message, std::move(details)};
}

[[nodiscard]] CanonicalValue CanonicalCodecValue(const std::string_view bytes,
                                                 const std::string_view label) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  Require(content_identity::canonical_json(parsed.root()) == bytes,
          std::string(label) + " codec did not return canonical JSON");
  return parsed.root();
}

[[nodiscard]] CanonicalValue
ExchangeManagerCommand(const std::filesystem::path &manager_run_root,
                       CanonicalValue command,
                       const std::stop_token stop_token = {}) {
  Require(!stop_token.stop_requested(), "Manager scene request cancelled");
  const auto bytes = content_identity::canonical_json(command);
  Require(!bytes.empty() && bytes.size() <= kMaximumPacketBytes,
          "Manager scene request is empty or oversized");
  const auto socket_path =
      manager_run_root / std::string(kManagerSocketFileName);
  const auto spelling = socket_path.string();
  Require(spelling.size() < sizeof(sockaddr_un::sun_path),
          "Manager command socket path is too long");
  struct stat socket_status {};
  Require(::lstat(socket_path.c_str(), &socket_status) == 0 &&
              S_ISSOCK(socket_status.st_mode) &&
              !S_ISLNK(socket_status.st_mode),
          "Manager command path is not a local socket");

  FileDescriptor descriptor(
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!descriptor) {
    FailErrno("create Manager scene client socket");
  }
  // Wake an outstanding socket wait when the hosting execution surface drains.
  // The callback is destroyed before the descriptor, so it cannot affect a
  // subsequently reused descriptor. No owner state or Scene revision is cached.
  const std::stop_callback cancel_wait(stop_token, [&descriptor] {
    static_cast<void>(::shutdown(descriptor.get(), SHUT_RDWR));
  });
  const int receive_buffer_bytes =
      static_cast<int>(kMaximumManagerResponseBytes);
  const int send_buffer_bytes = static_cast<int>(kMaximumPacketBytes);
  if (::setsockopt(descriptor.get(), SOL_SOCKET, SO_RCVBUF,
                   &receive_buffer_bytes, sizeof(receive_buffer_bytes)) != 0 ||
      ::setsockopt(descriptor.get(), SOL_SOCKET, SO_SNDBUF, &send_buffer_bytes,
                   sizeof(send_buffer_bytes)) != 0) {
    FailErrno("configure Manager scene client socket buffers");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
  if (::connect(descriptor.get(), reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
    Require(errno == EINPROGRESS || errno == EAGAIN,
            std::string("connect to Manager command socket failed: ") +
                std::system_error(errno, std::generic_category()).what());
    pollfd connected{descriptor.get(), POLLOUT, 0};
    Require(!stop_token.stop_requested(), "Manager scene request cancelled");
    int ready = -1;
    do {
      ready = ::poll(&connected, 1, kManagerSceneDeadlineMilliseconds);
    } while (ready < 0 && errno == EINTR);
    Require(ready > 0 && (connected.revents & POLLOUT) != 0,
            "Manager command socket connection timed out");
    int socket_error = 0;
    socklen_t error_size = sizeof(socket_error);
    Require(::getsockopt(descriptor.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                         &error_size) == 0 &&
                error_size == sizeof(socket_error) && socket_error == 0,
            "Manager command socket connection was rejected");
  }

  pollfd outbound{descriptor.get(), POLLOUT, 0};
  Require(!stop_token.stop_requested(), "Manager scene request cancelled");
  int ready = -1;
  do {
    ready = ::poll(&outbound, 1, kManagerSceneDeadlineMilliseconds);
  } while (ready < 0 && errno == EINTR);
  Require(ready > 0 && (outbound.revents & POLLOUT) != 0,
          "Manager did not accept a scene request in time");
  ssize_t sent = -1;
  do {
    sent = ::send(descriptor.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  Require(sent == static_cast<ssize_t>(bytes.size()),
          "Manager scene request was not sent atomically");

  pollfd inbound{descriptor.get(), POLLIN, 0};
  Require(!stop_token.stop_requested(), "Manager scene request cancelled");
  do {
    ready = ::poll(&inbound, 1, kManagerSceneDeadlineMilliseconds);
  } while (ready < 0 && errno == EINTR);
  Require(ready > 0 && (inbound.revents & POLLIN) != 0,
          "Manager scene response timed out");
  std::vector<char> buffer(kMaximumManagerResponseBytes);
  iovec vector{buffer.data(), buffer.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  ssize_t received = -1;
  do {
    received = ::recvmsg(descriptor.get(), &message, 0);
  } while (received < 0 && errno == EINTR);
  Require(received > 0 && (message.msg_flags & MSG_TRUNC) == 0,
          "Manager scene response is empty or oversized");
  const std::string_view response_bytes(buffer.data(),
                                        static_cast<std::size_t>(received));
  const auto parsed = deployment::parse_strict_json_model_yaml(response_bytes);
  Require(content_identity::canonical_json(parsed.root()) == response_bytes &&
              parsed.root().is_object() &&
              Text(parsed.root(), "api_version") == kManagerResponseApiVersion,
          "Manager scene response is not the canonical supported contract");
  static_cast<void>(Boolean(parsed.root(), "ok"));
  return parsed.root();
}

[[nodiscard]] core::RequestReplayStatusDTO
DecodeRequestReplayStatus(const CanonicalValue &root,
                          const std::string_view name) {
  const auto value = Text(root, name);
  if (value == "accepted_new") {
    return core::RequestReplayStatusDTO::kAcceptedNew;
  }
  if (value == "replayed_exact") {
    return core::RequestReplayStatusDTO::kReplayedExact;
  }
  if (value == "request_result_expired") {
    return core::RequestReplayStatusDTO::kRequestResultExpired;
  }
  if (value == "request_id_conflict") {
    return core::RequestReplayStatusDTO::kRequestIdConflict;
  }
  if (value == "request_ledger_full") {
    return core::RequestReplayStatusDTO::kRequestLedgerFull;
  }
  if (value == "stale_request_owner_epoch") {
    return core::RequestReplayStatusDTO::kStaleRequestOwnerEpoch;
  }
  Fail("Manager scene replay status is unsupported");
}

[[nodiscard]] core::SceneProjectionCommitStatusDTO
DecodeSceneProjectionCommitStatus(const CanonicalValue &root,
                                  const std::string_view name) {
  const auto value = Text(root, name);
  if (value == "not_attempted") {
    return core::SceneProjectionCommitStatusDTO::kNotAttempted;
  }
  if (value == "committed") {
    return core::SceneProjectionCommitStatusDTO::kCommitted;
  }
  if (value == "rejected_rollback_proven") {
    return core::SceneProjectionCommitStatusDTO::kRejectedRollbackProven;
  }
  if (value == "failed_projection_uncertain") {
    return core::SceneProjectionCommitStatusDTO::kFailedProjectionUncertain;
  }
  Fail("Manager scene projection status is unsupported");
}

[[nodiscard]] core::SceneUpdateStatusDTO
DecodeSceneUpdateStatus(const CanonicalValue &value) {
  const auto *text = std::get_if<std::string>(&value.storage());
  Require(text != nullptr, "Manager scene update status must be a string");
  if (*text == "geometry_changed") {
    return core::SceneUpdateStatusDTO::kGeometryChanged;
  }
  if (*text == "freshness_only") {
    return core::SceneUpdateStatusDTO::kFreshnessOnly;
  }
  if (*text == "no_op") {
    return core::SceneUpdateStatusDTO::kNoOp;
  }
  Fail("Manager scene update status is unsupported");
}

struct ManagerSceneRead final {
  core::RequestOwnerEpoch request_owner_epoch;
  core::ProjectionRevisionDTO projection_revision;
  std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene;
};

/// Process-local adapter to the sole Manager-owned logical-scene reducer.
/// It carries exact owner DTOs over the existing same-user command socket and
/// stores no scene, replay, or projection authority of its own.
class ManagerSceneAuthorityProxy final : public core::ISceneAuthority,
                                         public core::ISceneCandidatePreview {
public:
  explicit ManagerSceneAuthorityProxy(std::filesystem::path manager_run_root)
      : manager_run_root_(std::move(manager_run_root)) {}

  [[nodiscard]] core::SceneCommitResultDTO
  Commit(const core::SceneTransactionRequestDTO &request) override {
    const auto encoded = scene::EncodeCanonicalSceneTransactionRequest(request);
    const auto response = ExchangeManagerCommand(
        manager_run_root_,
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(kManagerCommandApiVersion))},
            {"command", CanonicalValue("scene_commit")},
            {"transaction",
             CanonicalCodecValue(encoded, "scene transaction request")},
        }));
    RequireExactObjectSize(response, 10U, "Manager scene commit response");
    Require(Text(response, "request_id") ==
                    request.request_context.request_id.value() &&
                Text(response, "request_owner_epoch") ==
                    request.request_context.request_owner_epoch.value(),
            "Manager scene commit response changed request identity");
    std::optional<core::ErrorDTO> error;
    const auto &encoded_error = Member(response, "error");
    if (!std::holds_alternative<std::nullptr_t>(encoded_error.storage())) {
      error = DecodeManagerError(encoded_error);
    }
    std::optional<std::uint64_t> transaction_sequence;
    const auto &encoded_sequence = Member(response, "transaction_sequence");
    if (!std::holds_alternative<std::nullptr_t>(encoded_sequence.storage())) {
      const auto *number = std::get_if<double>(&encoded_sequence.storage());
      Require(number != nullptr && *number >= 1.0 &&
                  *number <= kMaximumSafeInteger &&
                  std::floor(*number) == *number,
              "Manager scene transaction sequence is invalid");
      transaction_sequence = static_cast<std::uint64_t>(*number);
    }
    std::optional<core::ProjectionRevisionDTO> committed_projection;
    const auto &encoded_projection =
        Member(response, "committed_projection_revision");
    if (!std::holds_alternative<std::nullptr_t>(encoded_projection.storage())) {
      committed_projection = DecodeProjectionRevision(encoded_projection);
    }
    std::optional<core::SceneUpdateStatusDTO> update_status;
    const auto &encoded_update = Member(response, "update_status");
    if (!std::holds_alternative<std::nullptr_t>(encoded_update.storage())) {
      update_status = DecodeSceneUpdateStatus(encoded_update);
    }
    core::SceneCommitResultDTO result{
        request.request_context.request_id,
        DecodeRequestReplayStatus(response, "replay_status"),
        Boolean(response, "ok"),
        std::move(error),
        transaction_sequence,
        std::move(committed_projection),
        DecodeSceneProjectionCommitStatus(response, "projection_status"),
        update_status};
    Require(result.presence_valid(),
            "Manager scene commit response is internally inconsistent");
    if (result.success) {
      Require(
          result.committed_projection_revision->retained_bundle_id ==
                  request.expected_projection_revision.retained_bundle_id &&
              result.committed_projection_revision->graph_revision ==
                  request.expected_projection_revision.graph_revision &&
              result.committed_projection_revision->scene_revision.scene_id ==
                  request.expected_projection_revision.scene_revision.scene_id,
          "Manager scene commit changed structural or scene-lineage identity");
    }
    return result;
  }

  [[nodiscard]] core::Result<core::SceneCandidatePreviewDTO>
  PreviewResolvedSceneCandidate(
      const core::ResolvedSceneCandidatePreviewRequestDTO &request)
      const override {
    const auto encoded =
        scene::EncodeResolvedSceneCandidatePreviewRequest(request);
    const auto response = ExchangeManagerCommand(
        manager_run_root_,
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(kManagerCommandApiVersion))},
            {"candidate_request",
             CanonicalCodecValue(encoded, "scene candidate request")},
            {"command", CanonicalValue("scene_candidate_preview")},
        }));
    if (!Boolean(response, "ok")) {
      return core::Result<core::SceneCandidatePreviewDTO>::Err(
          DecodeManagerError(Member(response, "error")));
    }
    RequireExactObjectSize(response, 3U, "Manager scene candidate response");
    auto preview =
        scene::DecodeSceneCandidatePreview(content_identity::canonical_json(
            Member(response, "candidate_preview")));
    Require(preview.prior_projection_revision ==
                request.request.expected_projection_revision,
            "Manager scene candidate changed predecessor projection");
    return core::Result<core::SceneCandidatePreviewDTO>::Ok(std::move(preview));
  }

  [[nodiscard]] core::RequestOwnerEpoch
  CurrentRequestOwnerEpoch() const override {
    return Read().request_owner_epoch;
  }

  [[nodiscard]] core::ProjectionRevisionDTO
  CurrentProjectionRevision() const override {
    return Read().projection_revision;
  }

  [[nodiscard]] core::ProjectionRevisionDTO
  CurrentProjectionRevision(const std::stop_token stop_token) const {
    return Read(stop_token).projection_revision;
  }

  [[nodiscard]] std::shared_ptr<const core::LogicalSceneSnapshotDTO>
  LogicalSnapshot() const override {
    return Read().logical_scene;
  }

  [[nodiscard]] std::vector<core::SceneObjectSnapshotDTO>
  GetSceneObjects() const override {
    return Read().logical_scene->objects;
  }

  [[nodiscard]] core::SceneObjectStateDTO GetObjectState(
      const core::SceneResourceId &scene_resource_id) const override {
    Require(!scene_resource_id.empty() &&
                scene_resource_id.value().size() <= 256U,
            "scene resource identity is invalid");
    const auto snapshot = Read().logical_scene;
    const auto found = std::lower_bound(
        snapshot->objects.begin(), snapshot->objects.end(), scene_resource_id,
        [](const core::SceneObjectSnapshotDTO &object,
           const core::SceneResourceId &id) {
          return object.scene_resource_id < id;
        });
    if (found == snapshot->objects.end() ||
        found->scene_resource_id != scene_resource_id) {
      return {scene_resource_id, false, false, std::nullopt, std::nullopt};
    }
    const auto *carried =
        std::get_if<core::CarriedSceneObjectPlacementDTO>(&found->placement);
    return {scene_resource_id, true, carried != nullptr,
            carried == nullptr ? std::nullopt
                               : std::optional<core::CarriedObjectAttachmentId>(
                                     carried->carried_object_attachment_id),
            found->provider_evidence};
  }

  [[nodiscard]] core::Result<core::Pose3DTO>
  GetObjectPose(const core::SceneResourceId &scene_resource_id) const override {
    Require(!scene_resource_id.empty() &&
                scene_resource_id.value().size() <= 256U,
            "scene resource identity is invalid");
    const auto current = Read();
    const auto response = ExchangeManagerCommand(
        manager_run_root_,
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(kManagerCommandApiVersion))},
            {"command", CanonicalValue("scene_pose_query")},
            {"expected_projection_revision",
             ProjectionRevisionValue(current.projection_revision)},
            {"scene_resource_ids",
             CanonicalValue(CanonicalValue::Array{
                 CanonicalValue(scene_resource_id.value())})},
        }));
    if (!Boolean(response, "ok")) {
      return core::Result<core::Pose3DTO>::Err(
          DecodeManagerError(Member(response, "error")));
    }
    RequireExactObjectSize(response, 4U, "Manager scene pose response");
    Require(DecodeProjectionRevision(Member(response, "projection_revision")) ==
                current.projection_revision,
            "Manager scene pose response changed projection identity");
    const auto &poses = Array(response, "poses");
    Require(poses.size() == 1U, "Manager scene pose response is incomplete");
    const auto &row = poses.front();
    RequireExactObjectSize(row, 2U, "Manager scene pose row");
    Require(Text(row, "scene_resource_id") == scene_resource_id.value(),
            "Manager scene pose response changed resource identity");
    const auto &pose = Member(row, "pose_in_cell");
    RequireExactObjectSize(pose, 2U, "Manager scene pose");
    const auto &position = Member(pose, "position_m");
    RequireExactObjectSize(position, 3U, "Manager scene pose position");
    const auto &orientation = Array(pose, "orientation_xyzw");
    Require(orientation.size() == 4U,
            "Manager scene pose orientation is incomplete");
    const auto coordinate = [](const CanonicalValue &value,
                               const std::string_view label) {
      const auto *number = std::get_if<double>(&value.storage());
      Require(number != nullptr && std::isfinite(*number),
              std::string(label) + " must be finite");
      return *number;
    };
    core::Pose3DTO result{
        {Number(position, "x"), Number(position, "y"), Number(position, "z")},
        {coordinate(orientation[0], "orientation x"),
         coordinate(orientation[1], "orientation y"),
         coordinate(orientation[2], "orientation z"),
         coordinate(orientation[3], "orientation w")}};
    return core::Result<core::Pose3DTO>::Ok(std::move(result));
  }

private:
  [[nodiscard]] ManagerSceneRead
  Read(const std::stop_token stop_token = {}) const {
    const auto response = ExchangeManagerCommand(
        manager_run_root_,
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(kManagerCommandApiVersion))},
            {"command", CanonicalValue("scene_owner_read")},
        }),
        stop_token);
    if (!Boolean(response, "ok")) {
      const auto error = DecodeManagerError(Member(response, "error"));
      Fail("Manager scene read failed: " + error.code + ": " + error.message);
    }
    RequireExactObjectSize(response, 5U, "Manager scene owner response");
    auto snapshot = std::make_shared<const core::LogicalSceneSnapshotDTO>(
        scene::DecodeLogicalSceneSnapshot(content_identity::canonical_json(
            Member(response, "logical_scene"))));
    auto projection =
        DecodeProjectionRevision(Member(response, "projection_revision"));
    Require(snapshot->revision == projection.scene_revision,
            "Manager scene owner response crossed a projection revision");
    return {core::RequestOwnerEpoch(Text(response, "request_owner_epoch")),
            std::move(projection), std::move(snapshot)};
  }

  std::filesystem::path manager_run_root_;
};

/// Process-local, non-authoritative join from fresh planning admission to the
/// Manager-owned observation policy. It retains no pose or scene truth: every
/// call fences the local predecessor, asks Manager to latch, and requires the
/// native projection to expose the exact returned successor before planning.
class ManagerPlanningSceneSynchronizer final
    : public core::IPlanningSceneSynchronizer {
public:
  ManagerPlanningSceneSynchronizer(
      std::filesystem::path manager_run_root,
      core::RetainedBundleId retained_bundle_id,
      core::GraphRevision graph_revision,
      const core::ActiveBundlePublicationGeneration publication_generation,
      std::shared_ptr<core::ICollisionWorldSnapshotPublisher>
          collision_publisher)
      : manager_run_root_(std::move(manager_run_root)),
        retained_bundle_id_(std::move(retained_bundle_id)),
        graph_revision_(std::move(graph_revision)),
        publication_generation_(publication_generation),
        collision_publisher_(std::move(collision_publisher)) {
    Require(
        !manager_run_root_.empty() &&
            core::IsExactLowercaseSha256Digest(retained_bundle_id_.value()) &&
            core::IsExactLowercaseSha256Digest(graph_revision_.value()) &&
            publication_generation_.nonzero() &&
            collision_publisher_ != nullptr,
        "Manager planning-scene synchronizer identity is invalid");
  }

  [[nodiscard]] core::Result<core::ProjectionRevisionDTO>
  SynchronizeBeforePlanning() override {
    try {
      for (std::size_t attempt = 0U;
           attempt < kPlanningSceneSynchronizationAttempts; ++attempt) {
        const auto current =
            collision_publisher_->current_scene_snapshot_lease_inventory();
        if (!current.has_value() ||
            !current->projection_binding.projection_revision.valid() ||
            current->projection_binding.projection_revision
                    .retained_bundle_id != retained_bundle_id_ ||
            current->projection_binding.projection_revision.graph_revision !=
                graph_revision_) {
          return Failure(
              "planning_scene_projection_unavailable",
              "local native predecessor projection is unavailable or stale");
        }
        const auto predecessor =
            current->projection_binding.projection_revision;
        const auto response = ExchangeManagerCommand(
            manager_run_root_,
            CanonicalValue(CanonicalValue::Object{
                {"api_version",
                 CanonicalValue(std::string(kManagerCommandApiVersion))},
                {"command",
                 CanonicalValue("scene_runtime_latch_before_planning")},
                {"expected_projection_revision",
                 ProjectionRevisionValue(predecessor)},
                {"expected_runtime_identity", RuntimeIdentityValue()},
            }));
        if (!Boolean(response, "ok")) {
          auto error = DecodeManagerError(Member(response, "error"));
          if (error.code == "scene_runtime_pose_projection_changed" &&
              attempt + 1U < kPlanningSceneSynchronizationAttempts) {
            std::this_thread::sleep_for(
                kPlanningSceneSynchronizationRetryDelay);
            continue;
          }
          return core::Result<core::ProjectionRevisionDTO>::Err(
              std::move(error));
        }
        RequireExactObjectSize(response, 10U,
                               "Manager before-planning latch response");
        RequireRuntimeIdentity(Member(response, "runtime_identity"));
        const auto initial = DecodeProjectionRevision(
            Member(response, "initial_projection_revision"));
        const auto projection =
            DecodeProjectionRevision(Member(response, "projection_revision"));
        const auto latched = Counter(response, "latched_observation_count", 0U);
        const auto committed =
            Counter(response, "committed_observation_count", 0U);
        const auto already_applied =
            Counter(response, "already_applied_observation_count", 0U);
        const auto skipped_attached =
            Counter(response, "skipped_attached_observation_count", 0U);
        static_cast<void>(
            Counter(response, "scene_runtime_source_generation", 1U));
        Require(initial == predecessor && projection.valid() &&
                    projection.retained_bundle_id == retained_bundle_id_ &&
                    projection.graph_revision == graph_revision_ &&
                    projection.scene_revision.scene_id ==
                        predecessor.scene_revision.scene_id &&
                    latched == committed + already_applied + skipped_attached,
                "Manager before-planning latch response crossed its request");
        for (std::size_t convergence_attempt = 0U;
             convergence_attempt < kPlanningSceneSynchronizationAttempts;
             ++convergence_attempt) {
          const auto converged =
              collision_publisher_->current_scene_snapshot_lease_inventory();
          if (converged.has_value() &&
              converged->projection_binding.projection_revision == projection) {
            return core::Result<core::ProjectionRevisionDTO>::Ok(projection);
          }
          if (convergence_attempt + 1U <
              kPlanningSceneSynchronizationAttempts) {
            std::this_thread::sleep_for(
                kPlanningSceneSynchronizationRetryDelay);
          }
        }
        return Failure(
            "planning_scene_projection_not_converged",
            "local native projection did not converge to the Manager latch");
      }
      return Failure("planning_scene_projection_changed",
                     "logical scene kept changing before Manager pose latch");
    } catch (const std::exception &error) {
      return Failure("planning_scene_synchronization_failed", error.what());
    } catch (...) {
      return Failure("planning_scene_synchronization_failed",
                     "unknown Manager planning-scene synchronization failure");
    }
  }

private:
  [[nodiscard]] core::Result<core::ProjectionRevisionDTO>
  Failure(std::string code, std::string message) const {
    return core::Result<core::ProjectionRevisionDTO>::Err(
        core::ErrorDTO{std::move(code),
                       std::move(message),
                       {{"reason", "before_planning_scene_latch_failed"}}});
  }

  [[nodiscard]] CanonicalValue RuntimeIdentityValue() const {
    const auto generation =
        static_cast<double>(publication_generation_.value());
    return CanonicalValue(CanonicalValue::Object{
        {"deployment_generation", CanonicalValue(generation)},
        {"graph_revision", CanonicalValue(graph_revision_.value())},
        {"program_generation", CanonicalValue(0.0)},
        {"retained_bundle_id", CanonicalValue(retained_bundle_id_.value())},
        {"runtime_generation", CanonicalValue(generation)},
    });
  }

  void RequireRuntimeIdentity(const CanonicalValue &value) const {
    RequireExactObjectSize(value, 5U, "Manager scene-runtime latch identity");
    Require(Text(value, "retained_bundle_id") == retained_bundle_id_.value() &&
                Text(value, "graph_revision") == graph_revision_.value() &&
                Counter(value, "deployment_generation", 1U) ==
                    publication_generation_.value() &&
                Counter(value, "program_generation", 0U) == 0U &&
                Counter(value, "runtime_generation", 1U) ==
                    publication_generation_.value(),
            "Manager scene-runtime latch identity changed");
  }

  std::filesystem::path manager_run_root_;
  core::RetainedBundleId retained_bundle_id_;
  core::GraphRevision graph_revision_;
  core::ActiveBundlePublicationGeneration publication_generation_;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher_;
};

[[nodiscard]] deployment::ActiveRuntimeParticipantFactsDTO
ActiveFacts(const deployment::RuntimeProgramTargetBinding &primary,
            std::span<const deployment::RuntimeProgramTargetBinding> supporting,
            const core::RuntimeEpoch &runtime_epoch,
            core::ActiveControlConfigurationId active_configuration_id) {
  Require(primary.desired_route().has_value(),
          "runtime binding has no desired I6 route");
  std::set<core::RuntimeUnitId> runtime_units(
      primary.component().runtime_unit_ids.begin(),
      primary.component().runtime_unit_ids.end());
  if (primary.control_domain().has_value()) {
    runtime_units.insert(primary.control_domain()->runtime_unit_ids.begin(),
                         primary.control_domain()->runtime_unit_ids.end());
  }
  std::map<core::ProviderInstanceId, core::RuntimeUnitId> providers;
  const auto insert_provider = [&](const core::ProviderInstanceId &provider,
                                   const core::RuntimeUnitId &runtime_unit) {
    const auto [found, inserted] = providers.emplace(provider, runtime_unit);
    Require(inserted || found->second == runtime_unit,
            "runtime provider is assigned to conflicting runtime units");
    runtime_units.insert(runtime_unit);
  };
  if (primary.control_binding().has_value()) {
    insert_provider(primary.control_binding()->provider_instance_id,
                    primary.control_binding()->provider_runtime_unit_id);
  }
  if (primary.capability_provider_binding().has_value()) {
    insert_provider(
        primary.capability_provider_binding()->provider_instance_id,
        retained_provider_runtime_unit(*primary.capability_provider_binding()));
  }
  for (const auto &support : supporting) {
    if (support.capability_provider_binding().has_value()) {
      insert_provider(
          support.capability_provider_binding()->provider_instance_id,
          retained_provider_runtime_unit(
              *support.capability_provider_binding()));
    }
  }
  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_epochs;
  runtime_epochs.reserve(runtime_units.size());
  for (const auto &runtime_unit : runtime_units) {
    runtime_epochs.push_back({runtime_unit, runtime_epoch});
  }
  std::vector<deployment::RuntimeProviderEpochFactDTO> provider_epochs;
  provider_epochs.reserve(providers.size());
  for (const auto &[provider, runtime_unit] : providers) {
    provider_epochs.push_back({provider, runtime_unit, runtime_epoch});
  }
  return deployment::ActiveRuntimeParticipantFactsDTO{
      *primary.desired_route(), std::move(active_configuration_id),
      core::ActiveControlConfigurationGeneration(1U), std::move(runtime_epochs),
      std::move(provider_epochs)};
}

[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
BuildControlBindingRepresentatives(
    const deployment::ProgramBinding &direct_control_binding,
    const core::RuntimeParticipantBinding &primary_binding,
    const deployment::RetainedBundle &bundle,
    const deployment::VerifiedRetainedModelMap &model_map,
    const core::RuntimeEpoch &runtime_epoch,
    const core::ActiveControlConfigurationId &active_configuration_id) {
  const auto &primary = primary_binding.value();
  Require(primary.control_path.has_value(),
          "direct-control primary has no exact control path");
  const auto &primary_path = *primary.control_path;
  const auto &targets = direct_control_binding.value().target_role_bindings;
  const auto target =
      std::find_if(targets.begin(), targets.end(), [&](const auto &candidate) {
        return candidate.controllable_component_id ==
                   primary.controllable_component_id &&
               candidate.command_target_id == primary.command_target_id;
      });
  Require(target != targets.end() &&
              std::find_if(std::next(target), targets.end(),
                           [&](const auto &candidate) {
                             return candidate.controllable_component_id ==
                                        primary.controllable_component_id &&
                                    candidate.command_target_id ==
                                        primary.command_target_id;
                           }) == targets.end(),
          "direct-control physical target is absent or ambiguous");

  std::vector<core::RuntimeParticipantBinding> representatives;
  for (const auto &capability : target->capability_bindings) {
    if (!capability.control_binding.has_value()) {
      continue;
    }
    if (capability.capability_id == primary.capability_id) {
      representatives.push_back(primary_binding);
      continue;
    }
    const auto resolved = deployment::resolve_runtime_program_target(
        direct_control_binding, target->target_role_id,
        capability.capability_id, bundle, model_map);
    auto sealed = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            resolved,
            ActiveFacts(resolved, {}, runtime_epoch, active_configuration_id)));
    if (!sealed.ok()) {
      Fail("direct-control control-binding representative failed "
           "verification: " +
           sealed.error().message);
    }
    representatives.push_back(std::move(sealed.value()));
  }
  std::sort(representatives.begin(), representatives.end(),
            [](const auto &left, const auto &right) {
              return left.value().control_path->control_binding_id <
                     right.value().control_path->control_binding_id;
            });

  const auto &desired = bundle.value().desired_control_configurations;
  const auto configuration =
      std::find_if(desired.begin(), desired.end(), [&](const auto &candidate) {
        return candidate.control_domain_id == primary_path.control_domain_id;
      });
  Require(configuration != desired.end() &&
              std::find_if(std::next(configuration), desired.end(),
                           [&](const auto &candidate) {
                             return candidate.control_domain_id ==
                                    primary_path.control_domain_id;
                           }) == desired.end() &&
              representatives.size() == configuration->binding_routes.size(),
          "direct-control target does not cover its exact desired domain");
  for (const auto &route : configuration->binding_routes) {
    const auto found = std::find_if(
        representatives.begin(), representatives.end(),
        [&](const auto &candidate) {
          const auto &path = *candidate.value().control_path;
          return path.control_binding_id == route.control_binding_id &&
                 path.control_binding_revision ==
                     route.control_binding_revision &&
                 path.control_route_id == route.control_route_id;
        });
    Require(found != representatives.end(),
            "direct-control target omits one desired control binding");
  }
  return representatives;
}

[[nodiscard]] CanonicalValue
ManualControlContractValue(const core::RuntimeParticipantBindingDTO &binding) {
  CanonicalValue::Array supporting_providers;
  supporting_providers.reserve(
      binding.supporting_capability_provider_paths.size());
  for (const auto &path : binding.supporting_capability_provider_paths) {
    supporting_providers.emplace_back(CanonicalValue::Object{
        {"capability_definition_revision",
         CanonicalValue(path.capability_definition_revision.value())},
        {"capability_id", CanonicalValue(path.capability_id.value())},
        {"capability_provider_binding_id",
         CanonicalValue(path.capability_provider_binding_id.value())},
        {"capability_provider_binding_revision",
         CanonicalValue(path.capability_provider_binding_revision.value())},
        {"provider_manifest_id",
         CanonicalValue(path.provider_manifest_id.value())},
        {"provider_manifest_revision",
         CanonicalValue(path.provider_manifest_revision.value())},
    });
  }
  CanonicalValue command_schema(nullptr);
  if (binding.command_schema.has_value()) {
    command_schema = CanonicalValue(CanonicalValue::Object{
        {"command_schema_id",
         CanonicalValue(binding.command_schema->command_schema_id.value())},
        {"command_schema_revision",
         CanonicalValue(
             binding.command_schema->command_schema_revision.value())},
    });
  }
  CanonicalValue control_path(nullptr);
  if (binding.control_path.has_value()) {
    const auto &path = *binding.control_path;
    control_path = CanonicalValue(CanonicalValue::Object{
        {"control_binding_id", CanonicalValue(path.control_binding_id.value())},
        {"control_binding_revision",
         CanonicalValue(path.control_binding_revision.value())},
        {"control_interface_contract_id",
         CanonicalValue(path.control_interface_contract_id.value())},
        {"control_interface_revision",
         CanonicalValue(path.control_interface_revision.value())},
        {"controller_topology_id",
         CanonicalValue(path.controller_topology_id.value())},
        {"stop_evidence_policy_id",
         CanonicalValue(path.stop_evidence_policy_id.value())},
        {"stop_evidence_policy_revision",
         CanonicalValue(path.stop_evidence_policy_revision.value())},
        {"topology_revision", CanonicalValue(path.topology_revision.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"command_schema", std::move(command_schema)},
      {"control_path", std::move(control_path)},
      {"operation_capability_definition_revision",
       CanonicalValue(binding.capability_definition_revision.value())},
      {"operation_capability_id",
       CanonicalValue(binding.capability_id.value())},
      {"supporting_providers", CanonicalValue(std::move(supporting_providers))},
  });
}

struct ManualControlDescriptors final {
  CanonicalValue motion;
  CanonicalValue gripper;
};

[[nodiscard]] CanonicalValue
CanonicalNullableNumber(const std::optional<double> &value) {
  if (!value.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(value.value());
}

[[nodiscard]] CanonicalValue
CanonicalNullableText(const std::optional<std::string> &value) {
  if (!value.has_value()) {
    return CanonicalValue(nullptr);
  }
  return CanonicalValue(*value);
}

[[nodiscard]] CanonicalValue BuildManualControlGripperDescriptor(
    const core::RuntimeParticipantBinding &gripper_binding,
    const std::optional<RetainedGripperPositionLimits> &gripper_limits,
    const std::vector<deployment::native_execution_runtime_contract::
                          Configuration::GripperTargetConfiguration>
        &gripper_target_configurations,
    const deployment::VerifiedRetainedModelMap &model_map,
    const deployment::ParsedModelDocuments &documents) {
  const auto &gripper = gripper_binding.value();
  Require(gripper.resources.size() == 1U,
          "manual-control descriptor crosses its exact resource inventory");
  const auto &resource = gripper.resources.front();
  CanonicalValue::Array followers;
  if (gripper.control_path.has_value()) {
    const auto *coordinate_map = model_map.find_binding(
        gripper.controllable_component_id, gripper.command_target_id,
        gripper.control_path->control_binding_id);
    Require(coordinate_map != nullptr,
            "manual-control gripper descriptor has no retained coordinate map");
    const auto row = std::find_if(
        coordinate_map->resources.begin(), coordinate_map->resources.end(),
        [&](const auto &candidate) {
          return candidate.resource_id == resource.resource_id;
        });
    Require(row != coordinate_map->resources.end(),
            "manual-control gripper descriptor has no exact resource row");
    Require(resource.source_joint_name.has_value()
                ? !row->coordinates.empty() &&
                      !row->component_local_coordinate_kind.has_value()
                : row->coordinates.empty() &&
                      row->component_local_coordinate_kind ==
                          resource.coordinate_kind,
            "manual-control gripper descriptor crosses coordinate semantics");
    for (const auto &coordinate : row->coordinates) {
      if (!coordinate.derived_mimic) {
        continue;
      }
      followers.emplace_back(CanonicalValue::Object{
          {"compiled_coordinate_name",
           CanonicalValue(coordinate.compiled_joint_name)},
          {"multiplier", CanonicalValue(coordinate.multiplier)},
          {"offset", CanonicalValue(coordinate.offset)},
          {"source_coordinate_name",
           CanonicalValue(coordinate.source_joint_name)},
      });
    }
  }

  std::map<std::string, CanonicalValue> named_presets;
  std::optional<core::ModelInstanceId> gripper_model_instance_id;
  std::optional<std::string> source_group_name;
  if (documents.srdf.has_value() && resource.source_joint_name.has_value()) {
    const auto group = std::find_if(
        gripper.model_groups.begin(), gripper.model_groups.end(),
        [&](const auto &candidate) {
          return std::find(candidate.expanded_compiled_joint_names.begin(),
                           candidate.expanded_compiled_joint_names.end(),
                           resource.compiled_resource_name) !=
                 candidate.expanded_compiled_joint_names.end();
        });
    Require(
        group != gripper.model_groups.end() &&
            std::find_if(
                std::next(group), gripper.model_groups.end(),
                [&](const auto &candidate) {
                  return std::find(
                             candidate.expanded_compiled_joint_names.begin(),
                             candidate.expanded_compiled_joint_names.end(),
                             resource.compiled_resource_name) !=
                         candidate.expanded_compiled_joint_names.end();
                }) == gripper.model_groups.end(),
        "manual-control gripper resource has no unique retained group");
    const auto source_group = std::find_if(
        model_map.symbols().begin(), model_map.symbols().end(),
        [&](const auto &symbol) {
          return symbol.source.model_instance_id == group->model_instance_id &&
                 symbol.source.kind ==
                     deployment::OccurrenceSymbolKind::kSrdfGroup &&
                 symbol.compiled_name == group->compiled_group_name;
        });
    Require(source_group != model_map.symbols().end(),
            "manual-control gripper group has no retained source identity");
    gripper_model_instance_id = group->model_instance_id;
    source_group_name = source_group->source.source_name;
    std::map<std::string, std::string> source_state_names;
    for (const auto &symbol : model_map.symbols()) {
      if (symbol.source.model_instance_id != group->model_instance_id ||
          symbol.source.kind !=
              deployment::OccurrenceSymbolKind::kSrdfGroupState) {
        continue;
      }
      Require(source_state_names
                  .emplace(symbol.compiled_name, symbol.source.source_name)
                  .second,
              "manual-control preset has no unique retained source name");
    }
    std::map<std::string, std::string> source_coordinate_names;
    for (const auto &coordinate : model_map.coordinates()) {
      if (coordinate.model_instance_id != group->model_instance_id) {
        continue;
      }
      Require(source_coordinate_names
                  .emplace(coordinate.compiled_joint_name,
                           coordinate.source_joint_name)
                  .second,
              "manual-control preset coordinate is not unique");
    }
    for (const auto &state : documents.srdf->group_states) {
      if (state.group_name != group->compiled_group_name) {
        continue;
      }
      const auto master = std::find_if(
          state.joints.begin(), state.joints.end(), [&](const auto &joint) {
            return joint.joint_name == resource.compiled_resource_name;
          });
      if (master == state.joints.end()) {
        continue;
      }
      const auto symbol = source_state_names.find(state.name);
      Require(symbol != source_state_names.end(),
              "manual-control preset has no unique retained source name");
      CanonicalValue::Array coordinates;
      coordinates.reserve(state.joints.size());
      for (const auto &joint : state.joints) {
        const auto source = source_coordinate_names.find(joint.joint_name);
        Require(source != source_coordinate_names.end(),
                "manual-control preset names an unknown retained coordinate");
        coordinates.emplace_back(CanonicalValue::Object{
            {"position", CanonicalValue(joint.value)},
            {"source_coordinate_name", CanonicalValue(source->second)},
        });
      }
      Require(
          named_presets
              .emplace(
                  symbol->second,
                  CanonicalValue(CanonicalValue::Object{
                      {"coordinates", CanonicalValue(std::move(coordinates))},
                      {"master_position", CanonicalValue(master->value)},
                      {"name", CanonicalValue(symbol->second)},
                  }))
              .second,
          "manual-control preset source name is not unique");
    }
  }
  CanonicalValue::Array presets;
  presets.reserve(named_presets.size());
  for (auto &entry : named_presets) {
    presets.emplace_back(std::move(entry.second));
  }

  const auto authored_configuration = std::find_if(
      gripper_target_configurations.begin(),
      gripper_target_configurations.end(), [&](const auto &candidate) {
        return candidate.controllable_component_id ==
                   gripper.controllable_component_id &&
               candidate.command_target_id == gripper.command_target_id;
      });
  const auto has_authored_configuration =
      authored_configuration != gripper_target_configurations.end();
  if (has_authored_configuration) {
    Require(gripper_model_instance_id.has_value() &&
                source_group_name.has_value(),
            "authored gripper configuration has no exact retained model group");
    Require(
        named_presets.contains(authored_configuration->open_state_ref) &&
            named_presets.contains(authored_configuration->closed_state_ref),
        "authored gripper state reference is not a retained SRDF preset");
    const auto require_source_link = [&](const std::string &source_name,
                                         const std::string_view field) {
      Require(
          std::count_if(model_map.symbols().begin(), model_map.symbols().end(),
                        [&](const auto &symbol) {
                          return symbol.source.model_instance_id ==
                                     *gripper_model_instance_id &&
                                 symbol.source.kind ==
                                     deployment::OccurrenceSymbolKind::kLink &&
                                 symbol.source.source_name == source_name;
                        }) == 1,
          "authored gripper " + std::string(field) +
              " is not one exact retained source link");
    };
    require_source_link(authored_configuration->mount_frame, "mount frame");
    require_source_link(authored_configuration->attach_frame, "attach frame");
    require_source_link(authored_configuration->tcp_frame, "TCP frame");
    for (const auto &link : authored_configuration->touch_links) {
      require_source_link(link, "touch link");
    }
    for (const auto &frame : authored_configuration->auxiliary_frames) {
      require_source_link(frame, "auxiliary frame");
    }
  }
  CanonicalValue::Array touch_links;
  CanonicalValue::Array auxiliary_frames;
  std::optional<std::string> description;
  std::optional<std::string> actuation_kind;
  std::optional<std::string> open_state_ref;
  std::optional<std::string> closed_state_ref;
  std::optional<double> command_duration_s;
  std::optional<std::string> mount_frame;
  std::optional<std::string> attach_frame;
  std::optional<std::string> tcp_frame;
  if (has_authored_configuration) {
    description = authored_configuration->description;
    actuation_kind = authored_configuration->actuation_kind;
    open_state_ref = authored_configuration->open_state_ref;
    closed_state_ref = authored_configuration->closed_state_ref;
    command_duration_s = authored_configuration->command_duration_s;
    mount_frame = authored_configuration->mount_frame;
    attach_frame = authored_configuration->attach_frame;
    tcp_frame = authored_configuration->tcp_frame;
    touch_links.reserve(authored_configuration->touch_links.size());
    for (const auto &link : authored_configuration->touch_links) {
      touch_links.emplace_back(link);
    }
    auxiliary_frames.reserve(authored_configuration->auxiliary_frames.size());
    for (const auto &frame : authored_configuration->auxiliary_frames) {
      auxiliary_frames.emplace_back(frame);
    }
  }

  std::optional<std::string> state_authority_id;
  std::optional<std::string> state_authority_class;
  std::optional<std::string> control_interface_contract_id;
  if (gripper.control_path.has_value()) {
    state_authority_id = gripper.control_path->state_authority_id.value();
    state_authority_class = std::string(
        core::to_string(gripper.control_path->state_authority_class));
    control_interface_contract_id =
        gripper.control_path->control_interface_contract_id.value();
  }

  const auto coordinate_kind =
      resource.coordinate_kind == core::CoordinateKind::kAngular ? "angular_rad"
                                                                 : "linear_m";
  return CanonicalValue(CanonicalValue::Object{
      {"actuation_kind", CanonicalNullableText(actuation_kind)},
      {"attach_frame", CanonicalNullableText(attach_frame)},
      {"auxiliary_frames", CanonicalValue(std::move(auxiliary_frames))},
      {"closed_state_ref", CanonicalNullableText(closed_state_ref)},
      {"command_target_id", CanonicalValue(gripper.command_target_id.value())},
      {"command_duration_s", CanonicalNullableNumber(command_duration_s)},
      {"command_kind", CanonicalValue("gripper_position")},
      {"component_role_id", CanonicalValue(gripper.component_role_id.value())},
      {"completion_policy", CanonicalValue("goal_fenced_target_reached")},
      {"control_interface_contract_id",
       CanonicalNullableText(control_interface_contract_id)},
      {"contracts", ManualControlContractValue(gripper)},
      {"controllable_component_id",
       CanonicalValue(gripper.controllable_component_id.value())},
      {"coordinate_kind", CanonicalValue(coordinate_kind)},
      {"description", CanonicalNullableText(description)},
      {"grasp_confirmation_policy",
       CanonicalValue("binding_feedback_required")},
      {"mimic_followers", CanonicalValue(std::move(followers))},
      {"max_position", gripper_limits.has_value()
                           ? CanonicalValue(gripper_limits->upper_position)
                           : CanonicalValue(nullptr)},
      {"min_position", gripper_limits.has_value()
                           ? CanonicalValue(gripper_limits->lower_position)
                           : CanonicalValue(nullptr)},
      {"mount_frame", CanonicalNullableText(mount_frame)},
      {"named_presets", CanonicalValue(std::move(presets))},
      {"open_state_ref", CanonicalNullableText(open_state_ref)},
      {"schema_version",
       CanonicalValue("manyforge.manual_control_gripper_descriptor.v2")},
      {"source_coordinate_name",
       CanonicalValue(resource.source_joint_name.has_value()
                          ? *resource.source_joint_name
                          : resource.compiled_resource_name)},
      {"source_group_name", CanonicalNullableText(source_group_name)},
      {"source_kind", CanonicalValue(resource.source_joint_name.has_value()
                                         ? "model_role"
                                         : "component_local")},
      {"state_authority_class", CanonicalNullableText(state_authority_class)},
      {"state_authority_id", CanonicalNullableText(state_authority_id)},
      {"target_role_id", CanonicalValue(gripper.target_role_id.value())},
      {"tcp_frame", CanonicalNullableText(tcp_frame)},
      {"touch_links", CanonicalValue(std::move(touch_links))},
      {"transport_kind", CanonicalValue("resolved_binding")},
      {"unit", CanonicalValue(coordinate_kind == std::string_view("angular_rad")
                                  ? "rad"
                                  : "m")},
  });
}

[[nodiscard]] ManualControlDescriptors BuildManualControlDescriptors(
    const core::RuntimeParticipantBinding &motion_binding,
    const std::optional<core::RuntimeParticipantBinding> &gripper_binding,
    const core::JointLimitsDTO &motion_limits,
    const std::optional<RetainedGripperPositionLimits> &gripper_limits,
    const std::optional<core::RuntimeParticipantPolicySelection>
        &motion_participant_policy,
    const std::optional<double> &deployment_maximum_tcp_linear_speed_mps,
    const deployment::native_execution_runtime_contract::
        OrdinaryMotionJerkSettings *ordinary_jerk_settings,
    const std::vector<deployment::native_execution_runtime_contract::
                          Configuration::DirectControlJointLimit>
        &direct_control_joint_limits,
    const std::vector<deployment::native_execution_runtime_contract::
                          Configuration::GripperTargetConfiguration>
        &gripper_target_configurations,
    const deployment::VerifiedRetainedModelMap &model_map,
    const deployment::ParsedModelDocuments &documents) {
  const auto &motion = motion_binding.value();
  Require(
      motion.resources.size() == motion_limits.joint_names.size() &&
          motion.resources.size() == motion_limits.position_lower_rad.size() &&
          motion.resources.size() == motion_limits.position_upper_rad.size() &&
          motion.resources.size() == motion_limits.velocity_max_rad_s.size() &&
          (motion_limits.acceleration_max_rad_s2.empty() ||
           motion.resources.size() ==
               motion_limits.acceleration_max_rad_s2.size()),
      "manual-control descriptor crosses its exact resource inventory");

  CanonicalValue::Array frames;
  frames.reserve(motion.frame_bindings.size());
  for (const auto &frame : motion.frame_bindings) {
    frames.emplace_back(CanonicalValue::Object{
        {"compiled_frame_name", CanonicalValue(frame.compiled_frame_name)},
        {"frame_role_id", CanonicalValue(frame.frame_role_id.value())},
        {"model_instance_id", CanonicalValue(frame.model_instance_id.value())},
        {"model_revision", CanonicalValue(frame.model_revision.value())},
    });
  }
  CanonicalValue::Array joints;
  joints.reserve(motion.resources.size());
  for (std::size_t index = 0U; index < motion.resources.size(); ++index) {
    const auto &resource = motion.resources[index];
    Require(resource.source_joint_name.has_value(),
            "manual-control motion descriptor has a non-model coordinate");
    Require(motion_limits.joint_names[index] == resource.compiled_resource_name,
            "manual-control motion descriptor crosses its exact joint order");
    joints.emplace_back(CanonicalValue::Object{
        {"compiled_joint_name",
         CanonicalValue(resource.compiled_resource_name)},
        {"max_acceleration_rad_s2",
         motion_limits.acceleration_max_rad_s2.empty()
             ? CanonicalValue(nullptr)
             : CanonicalValue(motion_limits.acceleration_max_rad_s2[index])},
        {"max_position_rad",
         CanonicalValue(motion_limits.position_upper_rad[index])},
        {"max_velocity_rad_s",
         CanonicalValue(motion_limits.velocity_max_rad_s[index])},
        {"min_position_rad",
         CanonicalValue(motion_limits.position_lower_rad[index])},
        {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
        {"unit", CanonicalValue("rad")},
    });
  }
  CanonicalValue operational_limit(nullptr);
  if (deployment_maximum_tcp_linear_speed_mps.has_value()) {
    operational_limit = CanonicalValue(CanonicalValue::Object{
        {"maximum_tcp_linear_speed_mps",
         CanonicalValue(*deployment_maximum_tcp_linear_speed_mps)},
    });
  }
  CanonicalValue::Array operational_joint_limits;
  CanonicalValue::Array ordinary_jerk_joints;
  operational_joint_limits.reserve(motion.resources.size());
  for (std::size_t index = 0U; index < motion.resources.size(); ++index) {
    const auto &resource = motion.resources[index];
    const auto authored = std::find_if(
        direct_control_joint_limits.begin(), direct_control_joint_limits.end(),
        [&](const auto &candidate) {
          return candidate.controllable_component_id ==
                     motion.controllable_component_id &&
                 candidate.command_target_id == motion.command_target_id &&
                 resource.source_joint_name.has_value() &&
                 candidate.source_joint_name == *resource.source_joint_name;
        });
    std::optional<double> authored_velocity;
    std::optional<double> authored_acceleration;
    std::optional<double> authored_jerk;
    if (authored != direct_control_joint_limits.end()) {
      authored_velocity = authored->max_velocity_rad_s;
      authored_acceleration = authored->max_acceleration_rad_s2;
      authored_jerk = authored->max_jerk_rad_s3;
    }
    const auto model_acceleration =
        motion_limits.acceleration_max_rad_s2.empty()
            ? std::optional<double>{}
            : std::optional<double>{
                  motion_limits.acceleration_max_rad_s2[index]};
    auto effective_acceleration = model_acceleration;
    if (authored_acceleration.has_value() &&
        (!effective_acceleration.has_value() ||
         authored_acceleration.value() < effective_acceleration.value())) {
      effective_acceleration = authored_acceleration;
    }
    auto effective_velocity = motion_limits.velocity_max_rad_s[index];
    if (authored_velocity.has_value()) {
      effective_velocity =
          std::min(effective_velocity, authored_velocity.value());
    }
    auto effective_jerk = CanonicalNullableNumber(authored_jerk);
    if (motion_participant_policy &&
        motion_participant_policy->value().ordinary_motion_jerk) {
      const auto &profile =
          *motion_participant_policy->value().ordinary_motion_jerk;
      if (!profile.enabled) {
        effective_jerk = CanonicalValue(nullptr);
      } else {
        const double ceiling =
            std::get<core::AngularJerkDTO>(
                profile.limits.at(*resource.source_joint_name))
                .value_rad_s3;
        effective_jerk = CanonicalValue(
            authored_jerk ? std::min(*authored_jerk, ceiling) : ceiling);
      }
    }
    if (ordinary_jerk_settings != nullptr) {
      const auto &settings = *ordinary_jerk_settings;
      const auto *profile =
          motion_participant_policy &&
                  motion_participant_policy->value().ordinary_motion_jerk
              ? &*motion_participant_policy->value().ordinary_motion_jerk
              : nullptr;
      if (settings.enabled) {
        Require(profile != nullptr && profile->enabled,
                "enabled ordinary jerk descriptor requires its resolved policy");
      } else {
        effective_jerk = CanonicalValue(nullptr);
      }
      const auto setting = std::find_if(
          settings.joint_limits.begin(), settings.joint_limits.end(),
          [&](const auto &row) {
            return row.source_joint_name == *resource.source_joint_name;
          });
      const auto *imported =
          setting != settings.joint_limits.end() && setting->imported
              ? &*setting->imported
              : nullptr;
      const char *source =
          !settings.enabled ? "off"
          : setting != settings.joint_limits.end() && setting->override_rad_s3
              ? "override"
          : imported != nullptr && imported->enabled ? "imported"
                                                     : "preset";
      ordinary_jerk_joints.emplace_back(CanonicalValue::Object{
          {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
          {"source", CanonicalValue(source)},
          {"maximum_rad_s3",
           settings.enabled
               ? CanonicalValue(
                     std::get<core::AngularJerkDTO>(
                         profile->limits.at(*resource.source_joint_name))
                         .value_rad_s3)
               : CanonicalValue(nullptr)},
          {"preset_rad_s3",
           model_acceleration
               ? CanonicalValue(core::motion_jerk_preset_limit(
                     *model_acceleration, settings.preset_reference_time_s))
               : CanonicalValue(nullptr)},
          {"imported",
           imported != nullptr
               ? CanonicalValue(CanonicalValue::Object{
                     {"maximum_rad_s3",
                      CanonicalValue(imported->maximum_rad_s3)},
                     {"enabled", CanonicalValue(imported->enabled)},
                     {"machine_required",
                      CanonicalValue(imported->machine_required)},
                     {"source_id", CanonicalValue(imported->source_id)},
                     {"source_digest",
                      CanonicalValue(imported->source_digest.value())}})
               : CanonicalValue(nullptr)},
      });
    }
    operational_joint_limits.emplace_back(CanonicalValue::Object{
        {"deployment_max_acceleration_rad_s2",
         CanonicalNullableNumber(authored_acceleration)},
        {"deployment_max_jerk_rad_s3", CanonicalNullableNumber(authored_jerk)},
        {"deployment_max_velocity_rad_s",
         CanonicalNullableNumber(authored_velocity)},
        {"effective_max_acceleration_rad_s2",
         CanonicalNullableNumber(effective_acceleration)},
        {"effective_max_jerk_rad_s3", std::move(effective_jerk)},
        {"effective_max_velocity_rad_s", CanonicalValue(effective_velocity)},
        {"model_max_acceleration_rad_s2",
         CanonicalNullableNumber(model_acceleration)},
        {"model_max_velocity_rad_s",
         CanonicalValue(motion_limits.velocity_max_rad_s[index])},
        {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
    });
  }
  for (const auto &authored : direct_control_joint_limits) {
    if (authored.controllable_component_id !=
            motion.controllable_component_id ||
        authored.command_target_id != motion.command_target_id) {
      continue;
    }
    Require(std::any_of(motion.resources.begin(), motion.resources.end(),
                        [&](const auto &resource) {
                          return resource.source_joint_name.has_value() &&
                                 *resource.source_joint_name ==
                                     authored.source_joint_name;
                        }),
            "direct-control joint limit names an unknown source joint");
  }
  CanonicalValue motion_descriptor(CanonicalValue::Object{
      {"command_target_id", CanonicalValue(motion.command_target_id.value())},
      {"component_role_id", CanonicalValue(motion.component_role_id.value())},
      {"contracts", ManualControlContractValue(motion)},
      {"controllable_component_id",
       CanonicalValue(motion.controllable_component_id.value())},
      {"frames", CanonicalValue(std::move(frames))},
      {"joints", CanonicalValue(std::move(joints))},
      {"operational_joint_limits",
       CanonicalValue(std::move(operational_joint_limits))},
      {"operational_limit", std::move(operational_limit)},
      {"ordinary_jerk",
       ordinary_jerk_settings != nullptr
           ? CanonicalValue(CanonicalValue::Object{
                 {"enabled", CanonicalValue(ordinary_jerk_settings->enabled)},
                 {"preset_reference_time_s",
                  CanonicalValue(
                      ordinary_jerk_settings->preset_reference_time_s)},
                 {"joint_limits",
                  CanonicalValue(std::move(ordinary_jerk_joints))}})
           : CanonicalValue(nullptr)},
      {"schema_version",
       CanonicalValue("manyforge.manual_control_motion_descriptor.v5")},
      {"target_role_id", CanonicalValue(motion.target_role_id.value())},
  });

  if (!gripper_binding.has_value()) {
    Require(!gripper_limits.has_value(),
            "manual-control gripper limits have no physical gripper binding");
    Require(std::none_of(
                gripper_target_configurations.begin(),
                gripper_target_configurations.end(), [&](const auto &candidate) {
                  return candidate.controllable_component_id ==
                         motion.controllable_component_id;
                }),
            "manual-control gripper configuration has no physical gripper binding");
    return {std::move(motion_descriptor), CanonicalValue(nullptr)};
  }

  return {std::move(motion_descriptor),
          BuildManualControlGripperDescriptor(
              *gripper_binding, gripper_limits,
              gripper_target_configurations, model_map, documents)};
}

struct Composition final {
  core::ControllableComponentId controllable_component_id;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> program_binding;
  std::shared_ptr<const deployment::ProgramBinding> direct_control_binding;
  std::shared_ptr<const deployment::ProgramBinding>
      direct_control_gripper_program_binding;
  std::optional<core::RuntimeParticipantBinding> direct_control_motion_binding;
  std::optional<core::RuntimeParticipantBinding>
      direct_control_gripper_binding;
  std::optional<core::JointLimitsDTO> direct_control_joint_limits;
  CanonicalValue direct_control_motion_descriptor;
  CanonicalValue direct_control_gripper_descriptor;
  std::optional<core::Duration> operator_confirmation_validity_ns;
  // The staged cell planning-model files of this composition's product. The
  // runtime's RoboPlan worker reloads them by path whenever it restarts, so a
  // retained runtime shares this stage with every composition that retains it
  // while the cell planning model is unchanged.
  std::shared_ptr<const RetainedPlanningModelStage> stage;
  // Present only for a retained component whose candidate product carries
  // another cell planning model. Every model-dependent computation is
  // prepared from `stage` before the fence and installed under it: a motion
  // controller's worker base model and its complete kinematic generation
  // (scene, state projector, kinematics, validator, collision projector); an
  // independently owned model-backed gripper's state projector.
  std::optional<RoboPlanWorkerModelDTO> candidate_planner_model;
  std::shared_ptr<const RetainedKinematicModelGeneration>
      candidate_model_generation;
  std::shared_ptr<RetainedKinematicStateProjector>
      candidate_tool_state_projector;
  std::shared_ptr<RetainedNativeRuntime> runtime;
  // Managed native cells keep this owner independently of any motion engine.
  // A component view may instead hold an independently managed gripper.
  RetainedNativeCellRuntime *cell_runtime{};
  std::shared_ptr<RetainedNativeGripperRuntime> gripper_runtime;
  std::shared_ptr<const ProductBackedRuntimeParticipantResolver>
      target_resolver;
  // Candidate metadata for an owner retained from the predecessor. The
  // runtime's registrations are re-sealed only at fenced installation.
  std::vector<core::RuntimeParticipantBinding>
      candidate_control_representatives;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      candidate_component_registrations;
  RetainedRuntimeParticipantResolverFactory
      candidate_participant_resolver_factory;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher>
      candidate_collision_publisher;
  collision::ManyForgeCollisionEngine::SharedModelPtr
      candidate_collision_model;
  std::shared_ptr<core::ICollisionQueryEngine>
      candidate_runtime_collision_engine;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      candidate_validation_collision_engines;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      candidate_lookahead_collision_engines;
  core::ProjectionRevisionDTO projection_revision;
  core::ActiveBundlePublicationGeneration publication_generation;
  std::optional<core::TargetRoleId> status_motion_target_role_id;
  std::optional<core::TargetRoleId> status_gripper_target_role_id;
  std::shared_ptr<ManagerSceneAuthorityProxy> scene_authority_proxy;
  std::shared_ptr<RetainedPlannerSceneProjection> planner_scene_projection;
  std::shared_ptr<RetainedSceneFrameResolver> scene_frame_resolver;
#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
  std::shared_ptr<void> pick_place_planning_fixture_lifetime;
#endif
  std::unique_ptr<core::AddressedPickPlacePlanningOwner>
      pick_place_planning_owner;
  std::unique_ptr<component_manager::SqliteActiveBundlePublicationStore>
      cclc_active_publication_reader;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      cclc_artifact_source;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProviderManifestSource>
      cclc_provider_manifest_source;
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  std::shared_ptr<RetainedRos2ControlRuntimeHandle> ros2_control_runtime_handle;
  std::string ros2_control_binding_contract_generation_id;
  std::unique_ptr<RetainedRos2ControlLifecycleProduct> cclc_lifecycle_product;
#else
  std::unique_ptr<RetainedNativeLifecycleProduct> cclc_lifecycle_product;
#endif
  // These external CCLC dependencies are declared before the composition so
  // reverse member destruction retires CCLC first, then readiness/route owner,
  // while the runtime and stable cell Kernel remain alive.
  std::unique_ptr<deployment::IRouteConfigurationKernelPublicationOwner>
      cclc_cell_route_publication_owner;
  std::unique_ptr<RetainedNativeCellCommandReadinessPort>
      cclc_cell_command_readiness_port;
  std::unique_ptr<deployment::CCLCRuntimeComposition> cclc_runtime;
  std::function<core::Result<void>()> cclc_cell_route_reconciler;
  std::atomic_bool cclc_attached{false};
  std::atomic_bool route_recovery_pending{false};
  std::vector<std::unique_ptr<Composition>> participant_compositions;
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  std::unique_ptr<RetainedRos2ControlAdmissionOwner>
      ros2_control_admission_owner;
#endif

  [[nodiscard]] core::ResourceAddressedExecutionKernel *
  active_addressed_kernel() const noexcept {
    if (cell_runtime != nullptr) {
      return &cell_runtime->addressed_kernel();
    }
    return runtime != nullptr ? runtime->active_addressed_kernel() : nullptr;
  }

  [[nodiscard]] const void *component_runtime_identity() const noexcept {
    return runtime != nullptr
               ? static_cast<const void *>(runtime.get())
               : static_cast<const void *>(gripper_runtime.get());
  }

  [[nodiscard]] std::vector<core::RuntimeUnitEpochBindingDTO>
  host_runtime_unit_epochs() const {
    if (runtime != nullptr)
      return runtime->motion_binding.value().runtime_unit_epochs;
    if (gripper_runtime != nullptr)
      // binding() returns a value; copy its epoch list so callers never retain
      // a reference into that temporary.
      return gripper_runtime->binding().value().runtime_unit_epochs;
    Fail("native component has no runtime-unit identity source");
  }

  [[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
  component_registrations() const {
    if (runtime != nullptr)
      return RetainedNativeComponentOwner(runtime).registrations();
    if (gripper_runtime != nullptr)
      return RetainedNativeComponentOwner(gripper_runtime).registrations();
    Fail("native component has no execution registration owner");
  }

  Composition(
      core::ControllableComponentId selected_component_id,
      std::shared_ptr<const deployment::RetainedBundle> selected_bundle,
      std::shared_ptr<const deployment::ProgramBindingContract>
          selected_program,
      std::shared_ptr<const deployment::ProgramBinding> selected_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_gripper_program_binding,
      std::optional<core::RuntimeParticipantBinding>
          selected_direct_control_motion_binding,
      std::optional<core::RuntimeParticipantBinding>
          selected_direct_control_gripper_binding,
      std::optional<core::JointLimitsDTO> selected_direct_control_joint_limits,
      CanonicalValue selected_direct_control_motion_descriptor,
      CanonicalValue selected_direct_control_gripper_descriptor,
      std::shared_ptr<const RetainedPlanningModelStage> selected_stage,
      std::shared_ptr<RetainedNativeRuntime> selected_runtime,
      core::ProjectionRevisionDTO selected_projection,
      core::ActiveBundlePublicationGeneration selected_generation,
      std::optional<core::TargetRoleId> selected_status_motion_target_role_id,
      std::optional<core::TargetRoleId> selected_status_gripper_target_role_id)
      : controllable_component_id(std::move(selected_component_id)),
        bundle(std::move(selected_bundle)),
        program(std::move(selected_program)),
        program_binding(std::move(selected_binding)),
        direct_control_binding(std::move(selected_direct_control_binding)),
        direct_control_gripper_program_binding(
            std::move(selected_direct_control_gripper_program_binding)),
        direct_control_motion_binding(
            std::move(selected_direct_control_motion_binding)),
        direct_control_gripper_binding(
            std::move(selected_direct_control_gripper_binding)),
        direct_control_joint_limits(
            std::move(selected_direct_control_joint_limits)),
        direct_control_motion_descriptor(
            std::move(selected_direct_control_motion_descriptor)),
        direct_control_gripper_descriptor(
            std::move(selected_direct_control_gripper_descriptor)),
        stage(std::move(selected_stage)), runtime(std::move(selected_runtime)),
        projection_revision(std::move(selected_projection)),
        publication_generation(selected_generation),
        status_motion_target_role_id(
            std::move(selected_status_motion_target_role_id)),
        status_gripper_target_role_id(
            std::move(selected_status_gripper_target_role_id)) {}
};

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
[[nodiscard]] std::map<core::ControllableComponentId, const Composition *>
IndexNativeComponentCompositions(const Composition &cell) {
  std::map<core::ControllableComponentId, const Composition *> result;
  const auto include = [&](const Composition &composition) {
    Require(composition.component_runtime_identity() != nullptr &&
                (composition.runtime == nullptr) !=
                    (composition.gripper_runtime == nullptr),
            "native component composition has no sole runtime owner");
    Require(result.emplace(composition.controllable_component_id,
                           &composition).second,
            "native cell contains duplicate component runtime owners");
  };
  include(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr,
            "native cell contains a missing component runtime owner");
    include(*participant);
  }
  return result;
}

void VerifyRetainedNativePredecessors(
    const Composition *active,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  const auto components = active == nullptr
                              ? std::map<core::ControllableComponentId,
                                         const Composition *>{}
                              : IndexNativeComponentCompositions(*active);
  if (active != nullptr) {
    Require(active->bundle != nullptr &&
                active->bundle->retained_bundle_id() ==
                    request.predecessor_projection_revision.retained_bundle_id &&
                active->bundle->value().cell_graph.graph_revision ==
                    request.predecessor_projection_revision.graph_revision,
            "native retained controllers are sealed to another predecessor");
  }
  for (const auto &row : request.component_delta.components) {
    const auto current = components.find(row.controllable_component_id);
    const bool is_retained =
        row.disposition == deployment::ApplyComponentDisposition::kRetain;
    if (is_retained) {
      Require(current != components.end(),
              "native Apply cannot retain a component without its live owner");
    } else if (row.disposition ==
               deployment::ApplyComponentDisposition::kAdd) {
      Require(current == components.end(),
              "native Apply addition already has a live component owner");
    } else {
      Require(current != components.end(),
              "native Apply change has no predecessor component owner");
    }
  }
}

[[nodiscard]] core::NativeApplyPlanRequestDTO BuildNativeApplyPlanRequest(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO &request,
    const Composition &candidate, const Composition *predecessor,
    const std::span<const core::RuntimeParticipantBinding> representatives) {
  Require(candidate.bundle != nullptr &&
              candidate.component_runtime_identity() != nullptr &&
              candidate.active_addressed_kernel() != nullptr,
          "native Apply has no installed candidate or stable Kernel");
  const auto candidate_components = IndexNativeComponentCompositions(candidate);
  const auto predecessor_components =
      predecessor == nullptr
          ? std::map<core::ControllableComponentId, const Composition *>{}
          : IndexNativeComponentCompositions(*predecessor);
  core::NativeApplyPlanRequestDTO plan{
      {request.apply_id, request.apply_generation},
      candidate.bundle->retained_bundle_id(),
      candidate.bundle->value().cell_graph.graph_revision,
      {}, {}, {}, {}, {}, {}};
  for (const auto &row : request.component_delta.components) {
    const auto before = predecessor_components.find(row.controllable_component_id);
    const auto after = candidate_components.find(row.controllable_component_id);
    switch (row.disposition) {
    case deployment::ApplyComponentDisposition::kRetain:
      Require(before != predecessor_components.end() &&
                  after != candidate_components.end() &&
                  before->second->component_runtime_identity() ==
                      after->second->component_runtime_identity(),
              "native Apply retained component was reconstructed");
      plan.retained_components.push_back(row.controllable_component_id);
      break;
    case deployment::ApplyComponentDisposition::kRemove:
      Require(before != predecessor_components.end() &&
                  after == candidate_components.end(),
              "native Apply removed component is still installed");
      plan.removed_components.push_back(row.controllable_component_id);
      break;
    case deployment::ApplyComponentDisposition::kAdd:
      Require(before == predecessor_components.end() &&
                  after != candidate_components.end(),
              "native Apply addition did not create a component owner");
      break;
    case deployment::ApplyComponentDisposition::kReplace:
      Require(before != predecessor_components.end() &&
                  after != candidate_components.end() &&
                  before->second->component_runtime_identity() !=
                      after->second->component_runtime_identity(),
              "native Apply replacement kept its predecessor controller");
      break;
    }
  }
  for (const auto &[component_id, _] : candidate_components)
    plan.components.push_back(component_id);
  const auto &desired_routes =
      candidate.bundle->value().desired_control_configurations;
  for (const auto &desired : desired_routes) {
    const auto physical = std::find_if(
        representatives.begin(), representatives.end(), [&](const auto &binding) {
          return binding.value().control_path.has_value() &&
                 binding.value().control_path->control_domain_id ==
                     desired.control_domain_id;
        });
    Require(physical != representatives.end(),
            "native candidate domain has no compiled physical binding");
    plan.compiled_routes.push_back(
        {desired.control_domain_id,
         physical->value()
             .control_path->active_control_configuration_generation});
    if (!std::binary_search(plan.retained_components.begin(),
                            plan.retained_components.end(),
                            physical->value().controllable_component_id))
      continue;
    const auto current = candidate.active_addressed_kernel()
                             ->current_runtime_route_configuration(
                                 desired.control_domain_id);
    Require(current.ok() && current.value().has_value() &&
                current.value()->operation_admission_open,
            "native retained domain has no current Kernel route");
    plan.retained_routes.push_back(current.value()->active_configuration);
  }
  if (predecessor != nullptr) {
    for (const auto &desired :
         predecessor->bundle->value().desired_control_configurations) {
      if (std::none_of(desired_routes.begin(), desired_routes.end(),
                       [&](const auto &row) {
                         return row.control_domain_id ==
                                desired.control_domain_id;
                       }))
        plan.removed_domains.push_back(desired.control_domain_id);
    }
  }
  return plan;
}

[[nodiscard]] std::vector<
    component_manager::ManagerRuntimeComponentReceiptDTO>
ReadNativeInstalledComponentReceipts(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO &request,
    const Composition *candidate, const Composition *predecessor) {
  const auto current = candidate == nullptr
                           ? std::map<core::ControllableComponentId,
                                      const Composition *>{}
                           : IndexNativeComponentCompositions(*candidate);
  const auto previous = predecessor == nullptr
                            ? std::map<core::ControllableComponentId,
                                       const Composition *>{}
                            : IndexNativeComponentCompositions(*predecessor);
  const bool product_changed =
      request.predecessor_projection_revision.retained_bundle_id !=
          request.candidate_projection_revision.retained_bundle_id ||
      request.predecessor_projection_revision.graph_revision !=
          request.candidate_projection_revision.graph_revision;
  std::vector<component_manager::ManagerRuntimeComponentReceiptDTO> receipts;
  for (const auto &row : request.component_delta.components) {
    if (row.disposition == deployment::ApplyComponentDisposition::kRemove)
      continue;
    const auto installed = current.find(row.controllable_component_id);
    Require(candidate != nullptr && installed != current.end(),
            "native installed component receipt has no live owner");
    const auto &component_id = installed->second->controllable_component_id;
    Require(component_id == row.controllable_component_id,
            "native installed composition has another component identity");
    const auto runtime_identity = deployment::derive_component_runtime_identity(
        *candidate->bundle, component_id);
    Require(runtime_identity.has_value() &&
                runtime_identity == row.candidate_runtime_identity,
            "native installed component differs from the candidate product");
    bool has_component_registration = false;
    for (const auto &registration :
         installed->second->component_registrations()) {
      const auto &binding = registration.binding.value();
      if (binding.controllable_component_id != component_id) {
        continue;
      }
      has_component_registration = true;
      Require(binding.retained_bundle_id ==
                      request.candidate_projection_revision.retained_bundle_id &&
                  binding.graph_revision ==
                      request.candidate_projection_revision.graph_revision,
              "native component registration is sealed to another product");
      Require(std::any_of(
                  binding.runtime_unit_epochs.begin(),
                  binding.runtime_unit_epochs.end(), [&](const auto &epoch) {
                    return epoch.runtime_unit_id == request.runtime_unit_id &&
                           epoch.runtime_epoch == request.runtime_epoch;
                  }),
              "native component registration has no current execution-host epoch");
    }
    Require(has_component_registration,
            "native installed component has no runtime participant registrations");
    std::vector<core::RuntimeUnitEpochBindingDTO> epochs{
        {request.runtime_unit_id, request.runtime_epoch}};
    const bool retained =
        row.disposition == deployment::ApplyComponentDisposition::kRetain;
    const auto predecessor_owner = previous.find(row.controllable_component_id);
    Require(retained ? predecessor_owner != previous.end() &&
                           predecessor_owner->second->component_runtime_identity() ==
                               installed->second->component_runtime_identity()
                     : predecessor_owner == previous.end() ||
                           predecessor_owner->second->component_runtime_identity() !=
                               installed->second->component_runtime_identity(),
            "native component receipt disagrees with controller lifetime");
    component_manager::ManagerRuntimeComponentReceiptDTO receipt{
        row.controllable_component_id,
        row.disposition,
        runtime_identity,
        std::move(epochs),
        retained,
        retained && product_changed};
    Require(receipt.valid(), "native component receipt is invalid");
    receipts.push_back(std::move(receipt));
  }
  Require(component_manager::ComponentReceiptsMatchRequest(request, receipts),
          "native installed component receipts differ from the Apply decision");
  return receipts;
}
#endif

[[maybe_unused]] void
AttachProductPickPlacePlanningOwner(Composition &composition) {
  if (composition.planner_scene_projection == nullptr) {
    return;
  }
  Require(composition.scene_authority_proxy != nullptr &&
              composition.runtime != nullptr &&
              composition.active_addressed_kernel() != nullptr,
          "production pick/place join has no scene or addressed owner");
  if (composition.pick_place_planning_owner != nullptr) {
    return;
  }
  composition.pick_place_planning_owner =
      std::make_unique<core::AddressedPickPlacePlanningOwner>(
          *composition.active_addressed_kernel(),
          *composition.scene_authority_proxy,
          *composition.scene_authority_proxy,
          *composition.planner_scene_projection,
          composition.runtime->planning_scene_synchronizer.get());
}

void InstallDirectSceneFrameResolver(ManagedProjectionRegistry &projections,
                                     Composition &composition) {
  if (composition.planner_scene_projection == nullptr) {
    return;
  }
  Require(composition.bundle != nullptr &&
              composition.scene_frame_resolver != nullptr,
          "command-capable native projection has no direct frame resolver");
  projections.InstallActiveFrameResolverSet(*composition.bundle,
                                            composition.projection_revision,
                                            composition.scene_frame_resolver);
}

struct ExecutionCompositionAccess final {
  mutable std::shared_mutex mutex;
  Composition *current{};
  // The stable cell Kernel. It is installed once and outlives every
  // composition; the Stop lane and cell admission read it without the
  // composition lock.
  std::atomic<core::ResourceAddressedExecutionKernel *> addressed_kernel{};

  void replace_current(Composition *replacement) {
    // Caller holds mutex exclusively and keeps the predecessor alive. A new
    // Program/Scene publication may still use the same physical controllers;
    // only a controller actually retired by the component delta loses its
    // observed-state source. Waking a retained channel would falsely report
    // a controller disconnect to readers waiting on that stable source.
    if (current != nullptr && current != replacement) {
      std::set<const RetainedNativeRuntime *> retained_motion;
      std::set<const RetainedNativeGripperRuntime *> retained_grippers;
      const auto include_retained = [&](const Composition &composition) {
        if (composition.runtime != nullptr)
          retained_motion.insert(composition.runtime.get());
        if (composition.gripper_runtime != nullptr)
          retained_grippers.insert(composition.gripper_runtime.get());
      };
      if (replacement != nullptr) {
        include_retained(*replacement);
        for (const auto &participant : replacement->participant_compositions)
          include_retained(*participant);
      }
      const auto wake_if_retired = [&](const Composition &composition) {
        if (composition.runtime != nullptr &&
            !retained_motion.contains(composition.runtime.get())) {
          if (composition.runtime->motion_state_channel != nullptr)
            composition.runtime->motion_state_channel
                ->notify_composition_replaced();
          if (composition.runtime->gripper_state_channel != nullptr)
            composition.runtime->gripper_state_channel
                ->notify_composition_replaced();
        }
        if (composition.gripper_runtime != nullptr &&
            !retained_grippers.contains(composition.gripper_runtime.get())) {
          if (const auto channel = composition.gripper_runtime->state_channel();
              channel != nullptr)
            channel->notify_composition_replaced();
        }
      };
      wake_if_retired(*current);
      for (const auto &participant : current->participant_compositions)
        wake_if_retired(*participant);
    }
    current = replacement;
  }
};

class ExecutionCommandSurface;

struct ManagedExecutionSurfaceControl final {
  std::filesystem::path run_root;
  ExecutionCompositionAccess *composition_access{};
  component_manager::detail::ManagerLivenessCommandAdmission
      *manager_liveness_admission{};
  core::RequestOwnerEpoch owner_epoch;
  std::mutex state_mutex;
  std::condition_variable state_changed;
  bool ready{};
  std::exception_ptr failure;
  // The running surface, guarded by `state_mutex`; null before it is ready
  // and once it has stopped.
  ExecutionCommandSurface *surface{};

  ManagedExecutionSurfaceControl(
      std::filesystem::path selected_run_root,
      ExecutionCompositionAccess *selected_composition_access,
      component_manager::detail::ManagerLivenessCommandAdmission
          *selected_manager_liveness_admission,
      core::RequestOwnerEpoch selected_owner_epoch)
      : run_root(std::move(selected_run_root)),
        composition_access(selected_composition_access),
        manager_liveness_admission(selected_manager_liveness_admission),
        owner_epoch(std::move(selected_owner_epoch)) {}
};

void RunManagedExecutionCommandSurface(ManagedExecutionSurfaceControl &control,
                                       std::stop_token stop_token) noexcept;

/// Preempts the running surface's advisory planning; a no-op while no
/// surface runs.
void PreemptManagedExecutionSurfacePlanning(
    ManagedExecutionSurfaceControl &control) noexcept;

struct RetainedNativeComponentSnapshot final {
  std::shared_ptr<RetainedNativeRuntime> runtime;
  std::shared_ptr<RetainedNativeGripperRuntime> gripper_runtime;
  std::shared_ptr<const RetainedPlanningModelStage> stage;
  // The cell planning-model product `stage` was staged from.
  core::RetainedProductDTO planning_model_product;
  std::optional<core::JointLimitsDTO> joint_limits;
  CanonicalValue motion_descriptor;
  CanonicalValue gripper_descriptor;
  std::optional<core::Duration> operator_confirmation_validity_ns;
};

struct CompositionBuildInput final {
  std::filesystem::path package_root;
  std::filesystem::path execution_state_root;
  std::filesystem::path execution_run_root;
  std::filesystem::path candidate_worker;
  core::CellId cell_id;
  core::RequestOwnerEpoch owner_epoch;
  std::optional<core::RuntimeUnitId> runtime_unit_id;
  core::RuntimeEpoch runtime_epoch;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  deployment::VerifiedRetainedModelMap model_map;
  core::ProjectionRevisionDTO projection_revision;
  core::ActiveBundlePublicationGeneration publication_generation;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  collision::ManyForgeCollisionEngine::SharedModelPtr collision_shared_model;
  std::vector<core::RuntimeParticipantBinding> motion_program_aliases;
  std::vector<core::RuntimeParticipantBinding> gripper_program_aliases;
  ManagedKernelAdmissionGate *operation_admission_gate{};
  std::shared_ptr<const deployment::ProgramBindingContract> status_program;
  std::shared_ptr<const deployment::ProgramBinding> status_program_binding;
  std::optional<core::TargetRoleId> status_motion_target_role_id;
  std::optional<core::TargetRoleId> status_gripper_target_role_id;
  bool attach_addressed_owner{true};
  std::shared_ptr<WallClock> shared_clock;
  std::shared_ptr<SimpleScheduler> shared_scheduler;
  std::filesystem::path manager_state_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path logical_scene_database;
  std::filesystem::path program_cycle_journal;
  std::shared_ptr<RetainedPlannerSceneProjection> planner_scene_projection;
  std::optional<core::ControllableComponentId> selected_component_id{};
  std::shared_ptr<RetainedObservationWake> observation_wake{};
  std::map<core::ControllableComponentId,
           std::shared_ptr<RetainedPlannerSceneProjection>>
      component_planner_scene_projections{};
  // Immutable Programs generated by Program Apply may live outside the
  // retained Deployment package. This is a read-only point-lookup source.
  std::filesystem::path program_artifact_root{};
  // Present only for a Manager-selected managed composition. Startup and
  // direct product builds have no Apply delta.
  std::optional<deployment::ApplyComponentDeltaDTO> managed_component_delta{};
  std::map<core::ControllableComponentId, RetainedNativeComponentSnapshot>
      retained_component_snapshots{};
  // The selected package root may be either retained descriptor in a managed
  // child. Candidate workers receive whichever exact root was selected as FD 6.
  std::optional<int> retained_package_root_fd{};
};

[[nodiscard]] core::CellAdmissionSettlementValidator
BuildOwnerSettlementValidator(
    std::filesystem::path manager_apply_journal,
    std::filesystem::path logical_scene_database,
    std::filesystem::path program_cycle_journal,
    std::filesystem::path lifecycle_journal, core::CellId cell_id) {
  if (manager_apply_journal.empty() && logical_scene_database.empty() &&
      program_cycle_journal.empty() && lifecycle_journal.empty()) {
    return {};
  }
  return
      [manager_apply_journal = std::move(manager_apply_journal),
       logical_scene_database = std::move(logical_scene_database),
       program_cycle_journal = std::move(program_cycle_journal),
       lifecycle_journal = std::move(lifecycle_journal),
       cell_id = std::move(cell_id)](
          const core::CellAdmissionClaimDTO &claim,
          const core::CellAdmissionClaimReleaseDTO &release) {
        if (claim.intent.claim_kind ==
                core::CellAdmissionClaimKindDTO::kInterventionOrRecovery &&
            claim.intent.owner_role ==
                core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner) {
          return core::ValidateCCLCCellAdmissionSettlementReadOnly(
              lifecycle_journal, cell_id, claim, release);
        }
        switch (claim.intent.claim_kind) {
        case core::CellAdmissionClaimKindDTO::kProgramCycle:
          return detail::ValidateProgramCycleSettlementReadOnly(
              program_cycle_journal, claim, release);
        case core::CellAdmissionClaimKindDTO::kAuthoringMutation:
          // Composer local edits own no durable or runtime effect. Kernel's
          // exact claim/release identity validation is therefore the complete
          // settlement proof; requiring a second Composer journal would only
          // duplicate the in-memory workspace owner.
          return core::Result<void>::Ok();
        case core::CellAdmissionClaimKindDTO::kManagerApply:
          return component_manager::
              ValidateManagerApplyCellAdmissionSettlementReadOnly(
                  manager_apply_journal, cell_id, claim, release);
        case core::CellAdmissionClaimKindDTO::kComponentLifecycle:
          return core::ValidateCCLCCellAdmissionSettlementReadOnly(
              lifecycle_journal, cell_id, claim, release);
        case core::CellAdmissionClaimKindDTO::kAuthoredScene:
          return scene::ValidateAuthoredSceneCellAdmissionSettlementReadOnly(
              logical_scene_database, cell_id, claim, release);
        default:
          return core::Result<void>::Err(core::ErrorDTO{
              "invariant_violation",
              "cell-admission owner settlement reader is unavailable",
              {{"reason", "settlement_owner_reader_unavailable"}}});
        }
      };
}

[[nodiscard]] core::CellAdmissionReconcileValidator
BuildOwnerReconciliationValidator(
    std::filesystem::path manager_apply_journal,
    std::filesystem::path program_cycle_journal,
    std::filesystem::path lifecycle_journal, core::CellId cell_id) {
  return [manager_apply_journal = std::move(manager_apply_journal),
          program_cycle_journal = std::move(program_cycle_journal),
          lifecycle_journal = std::move(lifecycle_journal),
          cell_id = std::move(cell_id)](
             const std::vector<core::CellAdmissionClaimDTO> &held_claims,
             const std::vector<core::CellAdmissionCurrentOwnerEpochDTO>
                 &current_owner_epochs,
             const core::CellAdmissionReconcileDTO &reconcile) {
    std::optional<core::CellAdmissionOwnerRoleDTO> changed_role;
    const auto note_changed_role = [&](const auto role) {
      if (changed_role.has_value() && *changed_role != role) {
        return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
            core::ErrorDTO{
                "invariant_violation",
                "owner reconciliation may replace exactly one role",
                {{"reason",
                  "multiple_owner_role_reconciliation_unsupported"}}});
      }
      changed_role = role;
      return core::Result<core::CellAdmissionReconcileValidationDTO>::Ok({});
    };
    for (const auto &current : current_owner_epochs) {
      const auto requested = std::find_if(
          reconcile.current_owner_epochs.begin(),
          reconcile.current_owner_epochs.end(), [&](const auto &owner) {
            return owner.owner_role == current.owner_role;
          });
      if (requested == reconcile.current_owner_epochs.end() ||
          requested->owner_epoch != current.owner_epoch) {
        const auto noted = note_changed_role(current.owner_role);
        if (!noted.ok())
          return noted;
      }
    }
    for (const auto &requested : reconcile.current_owner_epochs) {
      const auto current =
          std::find_if(current_owner_epochs.begin(), current_owner_epochs.end(),
                       [&](const auto &owner) {
                         return owner.owner_role == requested.owner_role;
                       });
      if (current == current_owner_epochs.end()) {
        const auto noted = note_changed_role(requested.owner_role);
        if (!noted.ok())
          return noted;
      }
    }
    if (!changed_role.has_value()) {
      return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
          core::ErrorDTO{
              "invariant_violation",
              "owner reconciliation has no supported replacement",
              {{"reason", "owner_role_reconciliation_unsupported"}}});
    }
    switch (*changed_role) {
    case core::CellAdmissionOwnerRoleDTO::kProgramRunner:
      return detail::ValidateProgramRunnerOwnerReconciliationReadOnly(
          program_cycle_journal, held_claims, current_owner_epochs, reconcile);
    case core::CellAdmissionOwnerRoleDTO::kAuthoringOwner:
      return component_manager::
          ValidateManagerAuthoringOwnerReconciliationReadOnly(
              manager_apply_journal, cell_id, held_claims,
              current_owner_epochs, reconcile);
    case core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner:
      return core::ValidateCCLCCellAdmissionReconciliationReadOnly(
          lifecycle_journal, cell_id, held_claims, current_owner_epochs,
          reconcile);
    default:
      return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
          core::ErrorDTO{
              "invariant_violation",
              "owner role has no durable reconciliation reader",
              {{"reason", "owner_role_reconciliation_unsupported"}}});
    }
  };
}

struct NativePhysicalBindingSet final {
  // Each native component may provide either capability independently. The
  // managed cell aggregates these per-component records; this value must not
  // combine capabilities owned by two different components.
  std::shared_ptr<const deployment::ProgramBinding> motion_program;
  std::shared_ptr<const deployment::ProgramBinding> gripper_program;
  std::optional<core::RuntimeParticipantBinding> motion;
  std::optional<core::RuntimeParticipantBinding> gripper;
  std::vector<core::RuntimeParticipantBinding> representatives;
  std::optional<core::TargetRoleId> status_motion_role;
  std::optional<core::TargetRoleId> status_gripper_role;
};

[[nodiscard]] NativePhysicalBindingSet ResolveNativePhysicalBindings(
    const CompositionBuildInput &input,
    const deployment::DirectControlProgramBindingInventory &inventory) {
  const auto select_optional_binding =
      [&](const auto &bindings, const std::string_view capability_id) {
        std::shared_ptr<const deployment::ProgramBinding> selected;
        for (const auto &binding : bindings) {
          const auto target = deployment::resolve_runtime_program_target(
              *binding,
              core::TargetRoleId(
                  std::string(deployment::kDirectControlTargetRoleId)),
              core::CapabilityId(std::string(capability_id)), *input.bundle,
              input.model_map);
          if (input.selected_component_id.has_value() &&
              target.component().controllable_component_id !=
                  *input.selected_component_id)
            continue;
          Require(selected == nullptr,
                  "native composition selection has multiple matching targets");
          selected = binding;
        }
        return selected;
      };
  auto motion_program = select_optional_binding(
      inventory.motion, "execution.trajectory.execute");
  std::optional<deployment::RuntimeProgramTargetBinding> motion_target;
  if (motion_program != nullptr) {
    motion_target = deployment::resolve_runtime_program_target(
        *motion_program,
        core::TargetRoleId(
            std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("execution.trajectory.execute"), *input.bundle,
        input.model_map);
  }
  auto gripper_program = select_optional_binding(inventory.gripper,
                                                 "tool.gripper.command");
  std::optional<deployment::RuntimeProgramTargetBinding> gripper_target;
  if (gripper_program != nullptr) {
    gripper_target = deployment::resolve_runtime_program_target(
        *gripper_program,
        core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("tool.gripper.command"), *input.bundle,
        input.model_map);
  }

  // A component-runtime value has exactly one lifecycle owner. Motion and
  // gripper targets may be independent components in the retained cell, but
  // they must be resolved through separate component-runtime invocations.
  if (motion_target.has_value() && gripper_target.has_value()) {
    Require(motion_target->component().controllable_component_id ==
                gripper_target->component().controllable_component_id,
            "native component binding combines capabilities from different owners");
  }
  Require(!input.selected_component_id.has_value() ||
              motion_target.has_value() || gripper_target.has_value(),
          "selected native component has no supported direct-control capability");

  std::optional<core::TargetRoleId> status_motion_role;
  std::optional<core::TargetRoleId> status_gripper_role;
  if (input.status_program_binding != nullptr) {
    const auto selected_target_for =
        [&](const std::optional<core::TargetRoleId> &role,
                                         const std::string_view capability_id)
        -> std::optional<deployment::RuntimeProgramTargetBinding> {
      if (!role.has_value()) return std::nullopt;
      const auto found = std::find_if(
          input.status_program_binding->value().target_role_bindings.begin(),
          input.status_program_binding->value().target_role_bindings.end(),
          [&](const auto &target) {
            return target.target_role_id == *role &&
                   std::any_of(target.capability_bindings.begin(),
                               target.capability_bindings.end(),
                               [&](const auto &capability) {
                                 return capability.capability_id.value() ==
                                        capability_id;
                               });
          });
      if (found ==
          input.status_program_binding->value().target_role_bindings.end()) {
        return std::nullopt;
      }
      return deployment::resolve_runtime_program_target(
          *input.status_program_binding, *role,
          core::CapabilityId(std::string(capability_id)), *input.bundle,
          input.model_map);
    };
    const auto status_role_for = [&](const auto &physical_target,
                                     const auto &selected_target,
                                     const std::optional<core::TargetRoleId>
                                         &selected_role,
                                     const std::string_view capability_name)
        -> std::optional<core::TargetRoleId> {
      if (!physical_target.has_value())
        return std::nullopt;
      const auto &physical_owner =
          physical_target->component().controllable_component_id;
      const auto &physical_command_target =
          physical_target->target_role().command_target_id;
      if (!selected_target.has_value())
        return core::TargetRoleId(physical_command_target.value());
      const bool selection_addresses_this_component =
          selected_target->component().controllable_component_id ==
              (input.selected_component_id.has_value()
                   ? *input.selected_component_id
                   : physical_owner);
      if (!selection_addresses_this_component) {
        // The selected Program may legitimately use this capability on a
        // different independently owned component. Keep this component's
        // direct-control status address tied to its own verified target.
        return core::TargetRoleId(physical_command_target.value());
      }
      Require(selected_target->component().controllable_component_id ==
                  physical_owner &&
                  selected_role.has_value() &&
                  selected_target->target_role().command_target_id ==
                      physical_command_target,
              "selected Program " + std::string(capability_name) +
                  " target does not match its native component capability");
      return *selected_role;
    };
    const auto selected_motion_target = selected_target_for(
        input.status_motion_target_role_id, "execution.trajectory.execute");
    status_motion_role = status_role_for(
        motion_target, selected_motion_target,
        input.status_motion_target_role_id, "motion");
    const auto selected_gripper_target = selected_target_for(
        input.status_gripper_target_role_id, "tool.gripper.command");
    status_gripper_role = status_role_for(
        gripper_target, selected_gripper_target,
        input.status_gripper_target_role_id, "gripper");
  } else {
    if (motion_target.has_value()) {
      status_motion_role = core::TargetRoleId(
          motion_target->target_role().command_target_id.value());
    }
    if (gripper_target.has_value()) {
      status_gripper_role = core::TargetRoleId(
          gripper_target->target_role().command_target_id.value());
    }
  }

  std::optional<core::RuntimeParticipantBinding> motion;
  if (motion_target.has_value()) {
    std::vector<deployment::RuntimeProgramTargetBinding> motion_support;
    const auto &target_role = motion_program->value().target_role_bindings.front();
    for (const std::string_view capability_id :
         {std::string_view("planning.motion.cartesian_path"),
          std::string_view("planning.motion.joint_goal"),
          std::string_view("planning.motion.pose_goal")}) {
      if (std::any_of(target_role.capability_bindings.begin(),
                      target_role.capability_bindings.end(),
                      [&](const auto &capability) {
                        return capability.capability_id.value() == capability_id;
                      })) {
        motion_support.push_back(deployment::resolve_runtime_program_target(
            *motion_program,
            core::TargetRoleId(
                std::string(deployment::kDirectControlTargetRoleId)),
            core::CapabilityId(std::string(capability_id)), *input.bundle,
            input.model_map));
      }
    }
    Require(std::any_of(motion_support.begin(), motion_support.end(),
                        [](const auto &support) {
                          return support.capability().capability_id.value() ==
                                 "planning.motion.joint_goal";
                        }),
            "direct-control motion binding has no joint planner");
    auto verified_motion = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            *motion_target,
            ActiveFacts(
                *motion_target,
                std::span<const deployment::RuntimeProgramTargetBinding>(
                    motion_support),
                input.runtime_epoch,
                core::ActiveControlConfigurationId("active.i6.native.arm")),
            std::span<const deployment::RuntimeProgramTargetBinding>(
                motion_support)));
    if (!verified_motion.ok()) {
      Fail("direct-control motion participant binding failed verification: " +
           verified_motion.error().message);
    }
    motion = std::move(verified_motion.value());
  }

  std::optional<core::RuntimeParticipantBinding> gripper;
  if (gripper_target.has_value()) {
    auto verified_gripper = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            *gripper_target,
            ActiveFacts(
                *gripper_target,
                std::span<const deployment::RuntimeProgramTargetBinding>{},
                input.runtime_epoch,
                core::ActiveControlConfigurationId(
                    "active.i6.native.gripper"))));
    if (!verified_gripper.ok()) {
      Fail("direct-control gripper participant binding failed verification: " +
           verified_gripper.error().message);
    }
    gripper = std::move(verified_gripper.value());
  }
  std::vector<core::RuntimeParticipantBinding> representatives;
  if (motion.has_value()) {
    auto motion_representatives = BuildControlBindingRepresentatives(
        *motion_program, *motion, *input.bundle, input.model_map,
        input.runtime_epoch,
        core::ActiveControlConfigurationId("active.i6.native.arm"));
    representatives.insert(
        representatives.end(),
        std::make_move_iterator(motion_representatives.begin()),
        std::make_move_iterator(motion_representatives.end()));
  }
  if (gripper.has_value()) {
    auto gripper_representatives = BuildControlBindingRepresentatives(
        *gripper_program, *gripper, *input.bundle, input.model_map,
        input.runtime_epoch,
        core::ActiveControlConfigurationId("active.i6.native.gripper"));
    representatives.insert(
        representatives.end(),
        std::make_move_iterator(gripper_representatives.begin()),
        std::make_move_iterator(gripper_representatives.end()));
  }
  std::sort(representatives.begin(), representatives.end(),
            [](const auto &left, const auto &right) {
      return left.value().control_path->control_binding_id <
             right.value().control_path->control_binding_id;
    });
  return {std::move(motion_program), std::move(gripper_program),
          std::move(motion), std::move(gripper),
          std::move(representatives), std::move(status_motion_role),
          std::move(status_gripper_role)};
}

[[nodiscard]] std::unique_ptr<Composition>
BuildRetainedProductComposition(CompositionBuildInput input);

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
[[nodiscard]] std::unique_ptr<Composition>
BuildStandaloneGripperProductComposition(CompositionBuildInput input);
#endif

[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
ResealRetainedNativeCandidateRegistrations(
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &predecessor_registrations,
    const deployment::RetainedBundle &candidate_bundle,
    const std::filesystem::path &package_root,
    const std::vector<core::RuntimeParticipantBinding> &representatives) {
  const deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts(
      package_root);
  std::vector<core::RuntimeParticipantExecutionRegistration> result;
  result.reserve(representatives.size());
  for (const auto &binding : representatives) {
    const auto &value = binding.value();
    Require(value.control_path.has_value(),
            "retained native candidate binding has no control route");
    const auto old = std::find_if(
        predecessor_registrations.begin(),
        predecessor_registrations.end(),
        [&](const auto &registration) {
          const auto &before = registration.binding.value();
          return before.controllable_component_id ==
                     value.controllable_component_id &&
                 before.command_target_id == value.command_target_id &&
                 before.capability_id == value.capability_id &&
                 before.control_path.has_value() &&
                 before.control_path->control_binding_id ==
                     value.control_path->control_binding_id;
        });
    Require(old != predecessor_registrations.end(),
            "retained native candidate has no predecessor registration");
    auto registration = *old;
    registration.binding = binding;
    if (old->stop_readiness.has_value()) {
      const auto desired = std::find_if(
          candidate_bundle.value().desired_control_configurations.begin(),
          candidate_bundle.value().desired_control_configurations.end(),
          [&](const auto &row) {
            return row.control_domain_id ==
                   value.control_path->control_domain_id;
          });
      Require(desired !=
                  candidate_bundle.value().desired_control_configurations.end(),
              "retained native candidate has no desired controller domain");
      const auto expected =
          deployment::load_verified_controller_lifecycle_expected_domain(
              candidate_bundle, artifacts, *desired,
              value.control_path->provider_runtime_unit_id,
              value.control_path->provider_runtime_epoch);
      auto readiness =
          core::rebind_runtime_participant_stop_readiness_product(
              *old->stop_readiness, old->binding, binding,
              expected.sealed());
      if (!readiness.ok())
        Fail("retained native Stop readiness cannot be re-sealed: " +
             readiness.error().message);
      registration.stop_readiness = std::move(readiness.value());
    }
    result.push_back(std::move(registration));
  }
  Require(result.size() == representatives.size(),
          "retained native candidate registration set is incomplete");
  const auto predecessor_rows = std::count_if(
      predecessor_registrations.begin(),
      predecessor_registrations.end(),
      [&](const auto &registration) {
        return !representatives.empty() &&
               registration.binding.value().controllable_component_id ==
                   representatives.front().value().controllable_component_id;
      });
  Require(static_cast<std::size_t>(predecessor_rows) == result.size(),
          "retained native candidate cannot discard a predecessor registration");
  return result;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildRetainedProductComposition(CompositionBuildInput input) {
  Require(input.selected_component_id.has_value() &&
              input.runtime_unit_id.has_value() &&
              input.status_program == nullptr &&
              input.status_program_binding == nullptr &&
              input.bundle != nullptr,
          "retained native component view is not a managed physical target");
  const auto predecessor = input.retained_component_snapshots.find(
      *input.selected_component_id);
  Require(predecessor != input.retained_component_snapshots.end() &&
              (predecessor->second.runtime != nullptr ||
               predecessor->second.gripper_runtime != nullptr) &&
              predecessor->second.stage != nullptr,
          "retained native component has no pinned predecessor owner");
  Require(input.planner_scene_projection != nullptr ||
              predecessor->second.runtime == nullptr,
          "retained motion controller has no candidate Scene projection");
  if (predecessor->second.gripper_runtime != nullptr) {
    Require(predecessor->second.runtime == nullptr,
            "retained native component has two controller owners");
    const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
        input.package_root);
    const auto inventory =
        deployment::compile_direct_control_program_binding_inventory(
            *input.bundle, input.model_map, catalog_store);
    auto physical = ResolveNativePhysicalBindings(input, inventory);
    Require(!physical.motion.has_value() && physical.gripper.has_value() &&
                physical.gripper_program != nullptr &&
                retained_bindings_share_backend_owner(
                    predecessor->second.gripper_runtime->binding(),
                    *physical.gripper),
            "retained gripper changed its physical capability or controller");
    auto candidate_registrations =
        ResealRetainedNativeCandidateRegistrations(
            {predecessor->second.gripper_runtime->registration()},
            *input.bundle, input.package_root, physical.representatives);
    const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
        input.package_root);
    const auto retained_model = source.get_verified(
        deployment::RetainedPlanningModelQuery{
            input.bundle->retained_bundle_id(),
            input.bundle->value().compiled_products.planning_collision_model
                .product_id});
    Require(retained_model.has_value() && input.collision_shared_model != nullptr,
            "retained gripper has no verified candidate model");
    const auto old_channel = std::dynamic_pointer_cast<RetainedObservedStateChannel>(
        predecessor->second.gripper_runtime->state_channel());
    Require(old_channel != nullptr,
            "retained gripper has no observed-state channel");
    // A model-backed gripper projects its observations into a scene of the
    // whole-cell model. When that model changes, its candidate projector is
    // prepared here and replaces the predecessor's under the Apply fence.
    auto gripper_stage = predecessor->second.stage;
    std::shared_ptr<RetainedKinematicStateProjector> candidate_tool_projector;
    if (old_channel->kinematic_projector() != nullptr &&
        predecessor->second.planning_model_product !=
            input.bundle->value().compiled_products.planning_collision_model) {
      auto candidate_stage = std::make_shared<const RetainedPlanningModelStage>(
          RetainedPlanningModelStage::create_verified(
              *retained_model, input.execution_run_root));
      auto candidate_scene = BuildCandidateRetainedModelScene(
          "manyforge_i6_native_gripper_host", *candidate_stage,
          *retained_model, *input.bundle, input.model_map,
          {*physical.gripper});
      const auto limits =
          predecessor->second.gripper_runtime->position_limits();
      const auto &resource = physical.gripper->value().resources.front();
      const auto joint =
          candidate_scene.scene->getJointInfo(resource.compiled_resource_name);
      Require(limits.has_value() && joint.has_value() &&
                  joint->limits.min_position.size() == 1U &&
                  joint->limits.max_position.size() == 1U &&
                  limits->coordinate_kind == resource.coordinate_kind &&
                  limits->lower_position == joint->limits.min_position[0] &&
                  limits->upper_position == joint->limits.max_position[0],
              "candidate cell model changes the retained gripper limits");
      candidate_tool_projector = std::move(candidate_scene.state_projector);
      gripper_stage = std::move(candidate_stage);
    }
    std::vector<RetainedSceneFrameStateSourceInput> state_sources;
    const auto gripper_binding =
        predecessor->second.gripper_runtime->binding();
    const auto &resources = gripper_binding.value().resources;
    if (std::any_of(resources.begin(), resources.end(),
                    [](const auto &resource) {
                      return resource.source_joint_name.has_value();
                    })) {
      state_sources.push_back(RetainedSceneFrameStateSourceInput{
          old_channel->query_context(), old_channel->source_kind(),
          std::shared_ptr<const core::IObservedStateProvider>(
              old_channel, old_channel->addressed_publication())});
    }
    auto frame_resolver = RetainedSceneFrameResolver::create_verified(
        input.collision_shared_model, *input.bundle, input.model_map,
        *retained_model, *input.runtime_unit_id, input.runtime_epoch,
        std::move(state_sources), input.selected_component_id);
    Require(frame_resolver != nullptr,
            "retained gripper has no candidate frame resolver");
    const std::vector<core::ProgramBindingId> direct_binding_ids{
        physical.gripper_program->program_binding_id()};
    auto target_resolver =
        std::make_shared<ProductBackedRuntimeParticipantResolver>(
            input.package_root, input.bundle, input.model_map,
            std::vector<core::RuntimeParticipantBinding>{*physical.gripper},
            direct_binding_ids, input.program_artifact_root);
    RetainedRuntimeParticipantResolverFactory resolver_factory =
        [package_root = input.package_root, bundle = input.bundle,
         model_map = input.model_map,
         program_artifact_root = input.program_artifact_root,
         direct_binding_ids](
            std::vector<core::RuntimeParticipantBinding> bindings) {
          auto resolver =
              std::make_shared<ProductBackedRuntimeParticipantResolver>(
                  package_root, bundle, model_map, std::move(bindings),
                  direct_binding_ids, program_artifact_root);
          return RetainedRuntimeParticipantResolver{
              [resolver](const core::OperationIntentDTO &operation,
                         const std::size_t participant_index) {
                return resolver->ResolveExecutionParticipant(
                    operation, participant_index);
              }};
        };
    auto composition = std::make_unique<Composition>(
        *input.selected_component_id, std::move(input.bundle), nullptr, nullptr,
        nullptr, std::move(physical.gripper_program), std::nullopt,
        std::move(physical.gripper), std::nullopt,
        CanonicalValue(nullptr), predecessor->second.gripper_descriptor,
        std::move(gripper_stage), nullptr,
        std::move(input.projection_revision),
        input.publication_generation, std::nullopt,
        physical.status_gripper_role);
    composition->gripper_runtime = predecessor->second.gripper_runtime;
    composition->candidate_tool_state_projector =
        std::move(candidate_tool_projector);
    composition->target_resolver = std::move(target_resolver);
    composition->candidate_control_representatives =
        std::move(physical.representatives);
    composition->candidate_participant_resolver_factory =
        std::move(resolver_factory);
    composition->candidate_component_registrations =
        std::move(candidate_registrations);
    composition->scene_frame_resolver = std::move(frame_resolver);
    composition->planner_scene_projection =
        std::move(input.planner_scene_projection);
    if (composition->planner_scene_projection != nullptr) {
      composition->scene_authority_proxy =
          std::make_shared<ManagerSceneAuthorityProxy>(
              std::move(input.manager_run_root));
    }
    return composition;
  }
  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  const auto inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  auto physical = ResolveNativePhysicalBindings(input, inventory);
  Require(physical.motion.has_value() &&
              physical.status_motion_role.has_value() &&
              physical.gripper.has_value() ==
                  predecessor->second.runtime->gripper_binding.has_value(),
          "retained native controller changed its physical capability set");
  auto candidate_registrations =
      ResealRetainedNativeCandidateRegistrations(
          predecessor->second.runtime->participant_registrations,
          *input.bundle, input.package_root,
          physical.representatives);

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      planning_source(input.package_root);
  const auto retained_model = planning_source.get_verified(
      deployment::RetainedPlanningModelQuery{
          input.bundle->retained_bundle_id(),
          input.bundle->value().compiled_products.planning_collision_model
              .product_id});
  Require(retained_model.has_value() && input.collision_shared_model != nullptr,
          "retained native view has no verified candidate model");
  // Runtime identity excludes the shared cell planning model (other
  // components, passive models, root poses). The retained worker plans in
  // that whole-cell model, so a changed product stages the candidate model
  // here and replaces the worker's model under the Apply fence.
  // The same holds for every in-process model consumer (scene, state
  // projector, kinematics, validator, collision projector): all of them are
  // prepared here from the candidate model and the candidate bindings that
  // the re-seal installs, and replace the predecessor generation together.
  auto planning_stage = predecessor->second.stage;
  std::optional<RoboPlanWorkerModelDTO> candidate_planner_model;
  std::shared_ptr<const RetainedKinematicModelGeneration>
      candidate_model_generation;
  if (predecessor->second.planning_model_product !=
      input.bundle->value().compiled_products.planning_collision_model) {
    auto candidate_stage = std::make_shared<const RetainedPlanningModelStage>(
        RetainedPlanningModelStage::create_verified(*retained_model,
                                                    input.execution_run_root));
    const auto candidate_srdf = retained_model->srdf_bytes();
    const auto candidate_documents =
        deployment::parse_model_documents_for_projection(
            retained_model->urdf_bytes(),
            candidate_srdf.empty()
                ? std::optional<std::string_view>{}
                : std::optional<std::string_view>{candidate_srdf});
    candidate_planner_model = NativeWorkerBaseModel(
        *candidate_stage,
        RoboPlanJointLimitsFromRetainedModel(candidate_documents.urdf));
    std::vector<core::RuntimeParticipantBinding> projected_bindings{
        physical.motion.value()};
    if (physical.gripper.has_value() &&
        physical.gripper->value().resources.front().source_joint_name
            .has_value())
      projected_bindings.push_back(*physical.gripper);
    auto candidate_scene = BuildCandidateRetainedModelScene(
        "manyforge_i6_native_execution_host", *candidate_stage,
        *retained_model, *input.bundle, input.model_map,
        std::move(projected_bindings));
    candidate_model_generation = make_retained_motion_model_generation(
        physical.motion.value(), std::move(candidate_scene.scene),
        std::move(candidate_scene.scene_mutex),
        std::move(candidate_scene.state_projector));
    planning_stage = std::move(candidate_stage);
  }
  std::vector<RetainedSceneFrameStateSourceInput> state_sources;
  const auto add_state_source = [&](const auto &channel) {
    if (channel == nullptr)
      return;
    const auto channel_binding = channel->binding();
    const auto &resources = channel_binding.value().resources;
    if (std::none_of(resources.begin(), resources.end(),
                     [](const auto &resource) {
                       return resource.source_joint_name.has_value();
                     }))
      return;
    Require(std::all_of(resources.begin(), resources.end(),
                        [](const auto &resource) {
                          return resource.source_joint_name.has_value();
                        }),
            "retained native state channel mixes model and local resources");
    state_sources.push_back(RetainedSceneFrameStateSourceInput{
        channel->query_context(),
        core::ObservedStateSourceKind::kPredictedState,
        std::shared_ptr<const core::IObservedStateProvider>(
            channel, channel->addressed_publication())});
  };
  add_state_source(predecessor->second.runtime->motion_state_channel);
  add_state_source(predecessor->second.runtime->gripper_state_channel);
  auto frame_resolver = RetainedSceneFrameResolver::create_verified(
      input.collision_shared_model, *input.bundle, input.model_map,
      *retained_model, *input.runtime_unit_id, input.runtime_epoch,
      std::move(state_sources), input.selected_component_id);
  Require(frame_resolver != nullptr &&
              frame_resolver->matches_structural_revision(
                  input.bundle->retained_bundle_id(),
                  input.bundle->value().cell_graph.graph_revision),
          "retained native candidate frame resolver has another product");

  std::vector<core::ProgramBindingId> direct_binding_ids{
      physical.motion_program->program_binding_id()};
  if (physical.gripper_program != nullptr)
    direct_binding_ids.push_back(
        physical.gripper_program->program_binding_id());
  std::sort(direct_binding_ids.begin(), direct_binding_ids.end());
  std::vector<core::RuntimeParticipantBinding> physical_bindings{
      physical.motion.value()};
  if (physical.gripper.has_value())
    physical_bindings.push_back(*physical.gripper);
  auto target_resolver =
      std::make_shared<ProductBackedRuntimeParticipantResolver>(
          input.package_root, input.bundle, input.model_map,
          std::move(physical_bindings), direct_binding_ids,
          input.program_artifact_root);
  RetainedRuntimeParticipantResolverFactory resolver_factory =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       program_artifact_root = input.program_artifact_root,
       direct_binding_ids = std::move(direct_binding_ids)](
          std::vector<core::RuntimeParticipantBinding> bindings) {
        auto resolver =
            std::make_shared<ProductBackedRuntimeParticipantResolver>(
                package_root, bundle, model_map, std::move(bindings),
                direct_binding_ids, program_artifact_root);
        return RetainedRuntimeParticipantResolver{
            [resolver](const core::OperationIntentDTO &operation,
                       const std::size_t index) {
              return resolver->ResolveExecutionParticipant(operation, index);
            }};
      };
  auto composition = std::make_unique<Composition>(
      *input.selected_component_id, std::move(input.bundle), nullptr, nullptr,
      std::move(physical.motion_program),
      std::move(physical.gripper_program), std::move(physical.motion.value()),
      std::move(physical.gripper), predecessor->second.joint_limits,
      predecessor->second.motion_descriptor,
      predecessor->second.gripper_descriptor, std::move(planning_stage),
      predecessor->second.runtime, std::move(input.projection_revision),
      input.publication_generation, std::move(*physical.status_motion_role),
      physical.status_gripper_role);
  composition->candidate_planner_model = std::move(candidate_planner_model);
  composition->candidate_model_generation =
      std::move(candidate_model_generation);
  composition->operator_confirmation_validity_ns =
      predecessor->second.operator_confirmation_validity_ns;
  composition->target_resolver = std::move(target_resolver);
  composition->candidate_control_representatives =
      std::move(physical.representatives);
  composition->candidate_component_registrations =
      std::move(candidate_registrations);
  composition->candidate_participant_resolver_factory =
      std::move(resolver_factory);
  composition->candidate_collision_publisher = input.collision_publisher;
  composition->candidate_collision_model = input.collision_shared_model;
  composition->candidate_runtime_collision_engine =
      std::make_shared<collision::ManyForgeCollisionEngine>(
          input.collision_shared_model,
          collision::ManyForgeCollisionEngine::make_shared_scene_cache(
              input.collision_shared_model));
  const auto validation_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          input.collision_shared_model);
  for (std::size_t index = 0;
       index < predecessor->second.runtime->validation_collision_engines.size();
       ++index) {
    composition->candidate_validation_collision_engines.push_back(
        std::make_shared<collision::ManyForgeCollisionEngine>(
            input.collision_shared_model, validation_cache));
  }
  const auto lookahead_cache =
      collision::ManyForgeCollisionEngine::make_shared_scene_cache(
          input.collision_shared_model);
  for (std::size_t index = 0;
       index < predecessor->second.runtime->lookahead_collision_engines.size();
       ++index) {
    composition->candidate_lookahead_collision_engines.push_back(
        std::make_shared<collision::ManyForgeCollisionEngine>(
            input.collision_shared_model, lookahead_cache));
  }
  composition->scene_frame_resolver = std::move(frame_resolver);
  composition->planner_scene_projection =
      std::move(input.planner_scene_projection);
  composition->scene_authority_proxy =
      std::make_shared<ManagerSceneAuthorityProxy>(
          std::move(input.manager_run_root));
  return composition;
}

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
[[nodiscard]] std::unique_ptr<Composition>
BuildStandaloneGripperProductComposition(CompositionBuildInput input) {
  Require(input.selected_component_id.has_value() && input.bundle != nullptr &&
              input.bundle->value().cell_graph.cell_id == input.cell_id &&
              !input.runtime_epoch.empty() &&
              input.model_map.query().retained_bundle_id ==
                  input.bundle->retained_bundle_id() &&
              input.projection_revision.retained_bundle_id ==
                  input.bundle->retained_bundle_id() &&
              input.projection_revision.graph_revision ==
                  input.bundle->value().cell_graph.graph_revision &&
              input.publication_generation.nonzero() &&
              (input.status_program == nullptr) ==
                  (input.status_program_binding == nullptr) &&
              (input.shared_clock == nullptr) ==
                  (input.shared_scheduler == nullptr) &&
              input.collision_shared_model != nullptr &&
              (input.planner_scene_projection == nullptr ||
               !input.manager_run_root.empty()),
          "standalone native gripper inputs do not share one verified product");

  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  const auto inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  auto physical = ResolveNativePhysicalBindings(input, inventory);
  Require(!physical.motion.has_value() && physical.gripper.has_value() &&
              physical.gripper_program != nullptr &&
              physical.gripper->value().controllable_component_id ==
                  *input.selected_component_id &&
              physical.gripper->value().control_path.has_value(),
          "selected native component is not one verified gripper-only target");
  const auto gripper_binding = *physical.gripper;
  const auto &gripper_value = gripper_binding.value();
  const auto &control_path = *gripper_value.control_path;
  if (!input.runtime_unit_id.has_value()) {
    Require(gripper_value.runtime_unit_epochs.size() == 1U &&
                gripper_value.runtime_unit_epochs.front().runtime_epoch ==
                    input.runtime_epoch,
            "native gripper has no unique local runtime unit and epoch");
    input.runtime_unit_id =
        gripper_value.runtime_unit_epochs.front().runtime_unit_id;
  }
  Require(!input.runtime_unit_id->empty(),
          "native gripper runtime unit is empty");
  Require(control_path.state_authority_class ==
                  core::StateAuthorityClass::kPredicted &&
              gripper_value.resources.size() == 1U,
          "native gripper does not use the predicted-state authority");

  const auto runtime_units = gripper_value.runtime_unit_epochs;
  Require(std::any_of(runtime_units.begin(), runtime_units.end(),
                      [&](const auto &runtime_unit) {
                        return runtime_unit.runtime_unit_id ==
                                   *input.runtime_unit_id &&
                               runtime_unit.runtime_epoch == input.runtime_epoch;
                      }),
          "native gripper is not sealed to the selected runtime unit and epoch");
  const auto retained_runtime_configuration =
      deployment::native_execution_runtime_contract::require_component_configuration(
          *input.bundle, *input.runtime_unit_id, *input.selected_component_id);

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      planning_source(input.package_root);
  const auto retained_model = planning_source.get_verified(
      deployment::RetainedPlanningModelQuery{
          input.bundle->retained_bundle_id(),
          input.bundle->value().compiled_products.planning_collision_model
              .product_id});
  Require(retained_model.has_value(),
          "active retained model is absent or invalid for the gripper");
  auto stage = RetainedPlanningModelStage::create_verified(
      *retained_model, input.execution_run_root);
  const auto retained_srdf = retained_model->srdf_bytes();
  const auto retained_model_documents =
      deployment::parse_model_documents_for_projection(
          retained_model->urdf_bytes(),
          retained_srdf.empty()
              ? std::optional<std::string_view>{}
              : std::optional<std::string_view>{retained_srdf});

  const auto &resource = gripper_value.resources.front();
  const bool model_backed = resource.source_joint_name.has_value();
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<RetainedKinematicStateProjector> state_projector;
  std::optional<RetainedGripperPositionLimits> gripper_limits;
  if (model_backed) {
    scene_mutex = std::make_shared<std::mutex>();
    scene = make_roboplan_scene(RoboPlanSceneInput{
        "manyforge_i6_native_gripper_host",
        std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
        stage.package_paths(),
        RoboPlanJointLimitsFromRetainedModel(retained_model_documents.urdf)});
    state_projector = std::make_shared<RetainedKinematicStateProjector>(
        RetainedKinematicStateProjector::create_verified(
            scene, scene_mutex, *input.bundle, input.model_map, *retained_model,
            {gripper_binding}, 0));

    Require(gripper_value.resources.size() == 1U,
            "native model-backed gripper does not have one exact resource");
    const auto joint = scene->getJointInfo(resource.compiled_resource_name);
    Require(joint.has_value() && joint->num_position_dofs == 1U &&
                joint->limits.min_position.size() == 1U &&
                joint->limits.max_position.size() == 1U &&
                std::isfinite(joint->limits.min_position[0]) &&
                std::isfinite(joint->limits.max_position[0]) &&
                joint->limits.min_position[0] <=
                    joint->limits.max_position[0] &&
                ((resource.coordinate_kind == core::CoordinateKind::kAngular &&
                  joint->type == roboplan::JointType::REVOLUTE) ||
                 (resource.coordinate_kind == core::CoordinateKind::kLinear &&
                  joint->type == roboplan::JointType::PRISMATIC)),
            "native gripper has no exact retained position limits");
    gripper_limits = RetainedGripperPositionLimits{
        resource.coordinate_kind, joint->limits.min_position[0],
        joint->limits.max_position[0]};
  } else {
    Require(gripper_value.resources.size() == 1U &&
                gripper_value.model_groups.empty(),
            "native component-local gripper has an invalid physical scope");
  }

  const auto clock = input.shared_clock != nullptr
                         ? std::shared_ptr<core::IClock>(input.shared_clock)
                         : std::shared_ptr<core::IClock>(
                               std::make_shared<WallClock>());
  const auto scheduler =
      input.shared_scheduler != nullptr
          ? std::shared_ptr<core::IScheduler>(input.shared_scheduler)
          : std::shared_ptr<core::IScheduler>(std::make_shared<SimpleScheduler>(
                SimpleSchedulerConfig::from_environment()));
  const auto binding_revision = control_path.control_binding_revision.value();
  Require(!binding_revision.empty(),
          "native gripper route has no binding revision");
  RetainedObservedStateChannelConfig state_channel_config{
      core::ObservedStateSourceKind::kPredictedState,
      "roboplan_native/" + gripper_value.controllable_component_id.value() +
          "/" + gripper_value.command_target_id.value() + "/" +
          binding_revision,
      0, 1U};
  const auto update_period_ns = static_cast<core::Duration>(std::llround(
      1'000'000'000.0 /
      static_cast<double>(retained_runtime_configuration.execution_tick_hz)));
  Require(update_period_ns > 0,
          "native gripper execution cadence is invalid");

  // No Stop readiness is fabricated here. The new physical owner is admitted
  // only after CCLC has prepared and published exact lifecycle readiness.
  auto gripper_runtime =
      RetainedNativeGripperRuntime::CreateNativeWithOwnedStateChannel(
          gripper_binding, state_projector, std::move(state_channel_config),
          clock, scheduler,
          input.observation_wake != nullptr
              ? std::move(input.observation_wake)
              : std::make_shared<RetainedObservationWake>(),
          gripper_limits, std::nullopt, true, update_period_ns);

  std::vector<core::ProgramBindingId> direct_binding_ids{
      physical.gripper_program->program_binding_id()};
  std::vector<core::RuntimeParticipantBinding> resolver_bindings{
      gripper_binding};
  auto target_resolver =
      std::make_shared<ProductBackedRuntimeParticipantResolver>(
          input.package_root, input.bundle, input.model_map,
          std::move(resolver_bindings), direct_binding_ids,
          input.program_artifact_root);
  RetainedRuntimeParticipantResolverFactory resolver_factory =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       program_artifact_root = input.program_artifact_root,
       direct_binding_ids = std::move(direct_binding_ids)](
          std::vector<core::RuntimeParticipantBinding> bindings) {
        auto resolver =
            std::make_shared<ProductBackedRuntimeParticipantResolver>(
                package_root, bundle, model_map, std::move(bindings),
                direct_binding_ids, program_artifact_root);
        return RetainedRuntimeParticipantResolver{
            [resolver](const core::OperationIntentDTO &operation,
                       const std::size_t participant_index) {
              return resolver->ResolveExecutionParticipant(operation,
                                                           participant_index);
            }};
      };

  std::vector<RetainedSceneFrameStateSourceInput> state_sources;
  if (model_backed) {
    const auto channel =
        std::dynamic_pointer_cast<RetainedObservedStateChannel>(
            gripper_runtime->state_channel());
    Require(channel != nullptr,
            "native gripper has no retained route-aware observed-state channel");
    state_sources.push_back(RetainedSceneFrameStateSourceInput{
        channel->query_context(), channel->source_kind(),
        std::shared_ptr<const core::IObservedStateProvider>(
            channel, channel->addressed_publication())});
  }
  auto frame_resolver = RetainedSceneFrameResolver::create_verified(
      input.collision_shared_model, *input.bundle, input.model_map,
      *retained_model, *input.runtime_unit_id, input.runtime_epoch,
      std::move(state_sources), input.selected_component_id);
  Require(frame_resolver != nullptr &&
              frame_resolver->matches_structural_revision(
                  input.bundle->retained_bundle_id(),
                  input.bundle->value().cell_graph.graph_revision),
          "native gripper frame resolver has another product");

  auto descriptor = BuildManualControlGripperDescriptor(
      gripper_binding, gripper_limits,
      retained_runtime_configuration.gripper_target_configurations,
      input.model_map, retained_model_documents);
  const auto selected_component_id = *input.selected_component_id;
  Require(physical.status_gripper_role.has_value(),
          "native gripper has no verified status target role");
  auto composition = std::make_unique<Composition>(
      selected_component_id, std::move(input.bundle),
      std::move(input.status_program),
      std::move(input.status_program_binding), nullptr,
      std::move(physical.gripper_program), std::nullopt,
      std::optional<core::RuntimeParticipantBinding>{gripper_binding},
      std::nullopt, CanonicalValue(nullptr), std::move(descriptor),
      std::make_shared<const RetainedPlanningModelStage>(std::move(stage)),
      nullptr, std::move(input.projection_revision),
      input.publication_generation, std::nullopt,
      physical.status_gripper_role);
  composition->gripper_runtime = std::shared_ptr<RetainedNativeGripperRuntime>(
      std::move(gripper_runtime));
  composition->candidate_control_representatives =
      std::move(physical.representatives);
  composition->candidate_participant_resolver_factory =
      std::move(resolver_factory);
  composition->planner_scene_projection =
      std::move(input.planner_scene_projection);
  composition->scene_frame_resolver = std::move(frame_resolver);
  composition->target_resolver = std::move(target_resolver);
  if (composition->planner_scene_projection != nullptr) {
    composition->scene_authority_proxy =
        std::make_shared<ManagerSceneAuthorityProxy>(
            std::move(input.manager_run_root));
  }
  for (const auto &target :
       retained_runtime_configuration.gripper_target_configurations) {
    if (target.controllable_component_id ==
            gripper_value.controllable_component_id &&
        target.command_target_id == gripper_value.command_target_id) {
      composition->operator_confirmation_validity_ns =
          target.operator_confirmation_validity_ns;
    }
  }
  return composition;
}
#endif

[[nodiscard]] std::unique_ptr<Composition>
BuildSingleProductComposition(CompositionBuildInput input) {
  if (input.selected_component_id.has_value() &&
      input.retained_component_snapshots.contains(*input.selected_component_id))
    return BuildRetainedProductComposition(std::move(input));
  Require(input.bundle != nullptr &&
              input.bundle->value().cell_graph.cell_id == input.cell_id &&
              (!input.runtime_unit_id.has_value() ||
               !input.runtime_unit_id->empty()) &&
              !input.runtime_epoch.empty() &&
              input.model_map.query().retained_bundle_id ==
                  input.bundle->retained_bundle_id() &&
              input.projection_revision.retained_bundle_id ==
                  input.bundle->retained_bundle_id() &&
              input.projection_revision.graph_revision ==
                  input.bundle->value().cell_graph.graph_revision &&
              input.publication_generation.nonzero() &&
              (input.status_program == nullptr) ==
                  (input.status_program_binding == nullptr) &&
              (input.planner_scene_projection == nullptr ||
               !input.manager_run_root.empty()),
          "native physical composition inputs do not share one active "
          "product");
  const auto &products = input.bundle->value().compiled_products;
  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  auto direct_control_inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  auto physical_bindings =
      ResolveNativePhysicalBindings(input, direct_control_inventory);
  Require(physical_bindings.motion.has_value() &&
              physical_bindings.status_motion_role.has_value(),
          "motion-backed native composition has no motion target");
  auto direct_control_binding = std::move(physical_bindings.motion_program);
  auto direct_control_gripper_program_binding =
      std::move(physical_bindings.gripper_program);
  auto direct_control_motion_binding = std::move(physical_bindings.motion);
  auto direct_control_gripper_binding = std::move(physical_bindings.gripper);
  auto control_binding_representatives =
      std::move(physical_bindings.representatives);
  input.status_motion_target_role_id =
      std::move(*physical_bindings.status_motion_role);
  input.status_gripper_target_role_id =
      physical_bindings.status_gripper_role;
  auto local_runtime_unit_id = input.runtime_unit_id;
  if (!local_runtime_unit_id.has_value()) {
    const auto &runtime_units =
        direct_control_motion_binding.value().value().runtime_unit_epochs;
    Require(runtime_units.size() == 1U,
            "standalone native execution cannot infer one local runtime "
            "unit");
    local_runtime_unit_id = runtime_units.front().runtime_unit_id;
  }
  const auto contains_local_runtime = [&](const auto &binding) {
    const auto &runtime_units = binding.value().runtime_unit_epochs;
    return std::find_if(runtime_units.begin(), runtime_units.end(),
                        [&](const auto &runtime_unit) {
                          return runtime_unit.runtime_unit_id ==
                                     *local_runtime_unit_id &&
                                 runtime_unit.runtime_epoch ==
                                     input.runtime_epoch;
                        }) != runtime_units.end();
  };
  Require(contains_local_runtime(direct_control_motion_binding.value()) &&
              (!direct_control_gripper_binding.has_value() ||
               contains_local_runtime(*direct_control_gripper_binding)),
          "native execution local runtime is absent from its exact "
          "participant bindings");
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  const auto retained_runtime_configuration =
      deployment::ros2_control_execution_runtime_contract::
          require_configuration(*input.bundle, *local_runtime_unit_id);
#else
  const auto retained_runtime_configuration =
      deployment::native_execution_runtime_contract::require_component_configuration(
          *input.bundle, *local_runtime_unit_id,
          direct_control_motion_binding.value().value().controllable_component_id);
#endif
  std::optional<double> deployment_motion_target_limit;
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  std::vector<std::pair<core::ControllableComponentId,
                        core::CommandTargetId>> expected_motion_limits;
  expected_motion_limits.reserve(direct_control_inventory.motion.size());
  for (const auto &binding : direct_control_inventory.motion) {
    const auto target = deployment::resolve_runtime_program_target(
        *binding,
        core::TargetRoleId(
            std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("execution.trajectory.execute"), *input.bundle,
        input.model_map);
    if (target.component().controllable_component_id !=
        direct_control_motion_binding.value().value().controllable_component_id) {
      continue;
    }
    expected_motion_limits.emplace_back(
        target.component().controllable_component_id,
        target.target_role().command_target_id);
  }
  std::sort(expected_motion_limits.begin(), expected_motion_limits.end());
  std::vector<std::pair<core::ControllableComponentId,
                        core::CommandTargetId>> configured_motion_limits;
  configured_motion_limits.reserve(
      retained_runtime_configuration.motion_target_limits.size());
  std::transform(
      retained_runtime_configuration.motion_target_limits.begin(),
      retained_runtime_configuration.motion_target_limits.end(),
      std::back_inserter(configured_motion_limits), [](const auto &limit) {
        return std::pair{limit.controllable_component_id,
                         limit.command_target_id};
      });
  Require(configured_motion_limits == expected_motion_limits,
          "native Deployment must contain exactly one TCP-speed maximum for "
          "every motion-capable target and no unknown target");
  const auto motion_limit =
      std::find_if(retained_runtime_configuration.motion_target_limits.begin(),
                   retained_runtime_configuration.motion_target_limits.end(),
                   [&](const auto &candidate) {
                     return candidate.controllable_component_id ==
                                direct_control_motion_binding.value()
                                    .value()
                                    .controllable_component_id &&
                            candidate.command_target_id ==
                                direct_control_motion_binding.value()
                                    .value()
                                    .command_target_id;
                   });
  Require(motion_limit !=
              retained_runtime_configuration.motion_target_limits.end(),
          "native Deployment has no limit for the exact motion target");
  deployment_motion_target_limit = motion_limit->maximum_tcp_linear_speed_mps;
#endif
  auto selected_initial_joint_positions =
      retained_runtime_configuration.initial_joint_positions_rad;
  const auto selected_resource_count =
      direct_control_motion_binding.value().value().resources.size();
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  if (selected_initial_joint_positions.size() != selected_resource_count) {
    struct InitialPositionSlice final {
      core::ControllableComponentId component_id;
      std::size_t count{};
    };
    std::vector<InitialPositionSlice> slices;
    for (const auto &binding : direct_control_inventory.motion) {
      const auto target = deployment::resolve_runtime_program_target(
          *binding,
          core::TargetRoleId(
              std::string(deployment::kDirectControlTargetRoleId)),
          core::CapabilityId("execution.trajectory.execute"), *input.bundle,
          input.model_map);
      const auto verified = core::verify_runtime_participant_binding(
          deployment::build_runtime_participant_binding_dto(
              target, ActiveFacts(target, {}, input.runtime_epoch,
                                  core::ActiveControlConfigurationId(
                                      "active.i6.native.arm"))));
      Require(verified.ok(), "native initial-state target is invalid");
      slices.push_back({target.component().controllable_component_id,
                        verified.value().value().resources.size()});
    }
    std::sort(slices.begin(), slices.end(),
              [](const auto &left, const auto &right) {
                return left.component_id < right.component_id;
              });
    const auto total = std::accumulate(
        slices.begin(), slices.end(), std::size_t{0},
        [](const auto sum, const auto &slice) { return sum + slice.count; });
    Require(total == selected_initial_joint_positions.size() &&
                input.selected_component_id.has_value(),
            "native execution initial state differs from the cell motion "
            "resource order");
    std::size_t offset = 0U;
    for (const auto &slice : slices) {
      if (slice.component_id == *input.selected_component_id) {
        selected_initial_joint_positions = std::vector<double>(
            retained_runtime_configuration.initial_joint_positions_rad.begin() +
                static_cast<std::ptrdiff_t>(offset),
            retained_runtime_configuration.initial_joint_positions_rad.begin() +
                static_cast<std::ptrdiff_t>(offset + slice.count));
        break;
      }
      offset += slice.count;
    }
    Require(selected_initial_joint_positions.size() == selected_resource_count,
            "native execution initial-state slice is unavailable");
  }
#endif
  Require(selected_initial_joint_positions.size() == selected_resource_count,
          "native initial-state selection differs from the exact motion binding");
  const auto execution_tick_period_ns = static_cast<core::Duration>(
      std::llround(1'000'000'000.0 /
                   static_cast<double>(
                       retained_runtime_configuration.execution_tick_hz)));
  const auto collision_monitor_period_ns = static_cast<core::Duration>(
      std::llround(retained_runtime_configuration.collision_monitor_period_ms *
                   1'000'000.0));
  Require(execution_tick_period_ns > 0 && collision_monitor_period_ns > 0,
          "native execution retained runtime cadences are invalid");

  const deployment::FilesystemVerifiedRetainedPlanningModelSource
      planning_source(input.package_root);
  auto retained_model =
      planning_source.get_verified(deployment::RetainedPlanningModelQuery{
          input.bundle->retained_bundle_id(),
          products.planning_collision_model.product_id});
  Require(retained_model.has_value(),
          "active retained planning model is absent or invalid");
  const auto retained_srdf = retained_model->srdf_bytes();
  const auto retained_model_documents =
      deployment::parse_model_documents_for_projection(
          retained_model->urdf_bytes(),
          retained_srdf.empty()
              ? std::optional<std::string_view>{}
              : std::optional<std::string_view>{retained_srdf});
  const auto roboplan_joint_limits = RoboPlanJointLimitsFromRetainedModel(
      retained_model_documents.urdf);
  auto stage = RetainedPlanningModelStage::create_verified(
      *retained_model, input.execution_run_root);
  auto scene = make_roboplan_scene(RoboPlanSceneInput{
      "manyforge_i6_native_execution_host",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths(), roboplan_joint_limits});
  auto scene_mutex = std::make_shared<std::mutex>();
  std::vector<core::RuntimeParticipantBinding> projected_state_bindings{
      direct_control_motion_binding.value()};
  if (direct_control_gripper_binding.has_value() &&
      direct_control_gripper_binding->value()
          .resources.front()
          .source_joint_name.has_value()) {
    projected_state_bindings.push_back(*direct_control_gripper_binding);
  }
  auto state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, *input.bundle, input.model_map, *retained_model,
          std::move(projected_state_bindings), 0));

  auto publisher = std::move(input.collision_publisher);
  auto collision_model = std::move(input.collision_shared_model);
  if (publisher == nullptr) {
    const auto &group =
        direct_control_motion_binding.value().value().model_groups.front();
    auto package_paths = stage.package_paths();
    package_paths.insert(package_paths.begin(), input.package_root);
    collision_model = collision::ManyForgeCollisionEngine::load_shared_model(
        "manyforge_i6_native_execution_host", stage.loader_urdf_path(),
        stage.srdf_path(), package_paths, group.compiled_group_name);
    auto collision_projector = RetainedCollisionWorldProjector::create_verified(
        collision_model, *input.bundle, input.model_map, *retained_model);
    const auto logical_scene =
        EmptyScene(input.bundle->value().cell_graph.cell_id,
                   input.projection_revision.scene_revision);
    auto publisher_result = RetainedCollisionWorldPublisher::create(
        std::move(collision_projector), "i6-native-execution-host",
        logical_scene,
        RetainedCollisionWorldPublicationContext{
            core::CollisionWorldMutationSource::kDeploymentApply,
            {},
            WallClock{}.now(),
            {},
            "i6_native_execution_host_startup",
            std::nullopt},
        [] { return WallClock{}.now(); });
    if (!publisher_result.ok()) {
      Fail("retained collision publication failed: " +
           publisher_result.error().message);
    }
    publisher = std::move(publisher_result.value());
  }
  Require(publisher != nullptr && collision_model != nullptr,
          "native composition has no authoritative collision projection");
  const auto inventory = publisher->current_scene_snapshot_lease_inventory();
  Require(inventory.has_value() &&
              inventory->projection_binding.projection_revision ==
                  input.projection_revision,
          "native collision publisher does not expose the active product "
          "projection");

  auto candidate_binding =
      CandidateBinding(direct_control_motion_binding.value());
  auto candidate_client =
      std::make_unique<ProcessContainedRoboPlanCandidateClient>(
          NativeWorkerBaseModel(stage, roboplan_joint_limits),
          candidate_binding,
          RoboPlanCandidateComputerConfig{
              .rng_seed = retained_runtime_configuration.planner_rng_seed},
          ProcessContainedCandidateClientConfig{
              .worker_executable = input.candidate_worker,
              .require_retained_projection = true,
              .scene_asset_root = input.package_root,
              .retained_package_root_fd = input.retained_package_root_fd});
  Require(candidate_client->requires_retained_projection(),
          "production native planner must consume the exact retained scene "
          "projection");
  auto planner = std::make_unique<RoboPlanPlannerBackend>(
      inventory->projection_binding, candidate_binding,
      std::move(candidate_client));
  std::vector<core::ProgramBindingId> intrinsic_direct_bindings{
      direct_control_binding->program_binding_id()};
  if (direct_control_gripper_program_binding != nullptr) {
    intrinsic_direct_bindings.push_back(
        direct_control_gripper_program_binding->program_binding_id());
  }
  const auto make_participant_resolver =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       program_artifact_root = input.program_artifact_root,
       intrinsic_direct_bindings = std::move(intrinsic_direct_bindings)](
          std::vector<core::RuntimeParticipantBinding> physical_bindings) {
        return std::make_shared<ProductBackedRuntimeParticipantResolver>(
            package_root, bundle, model_map, std::move(physical_bindings),
            intrinsic_direct_bindings, program_artifact_root);
      };
  const RetainedRuntimeParticipantResolverFactory participant_resolver_factory =
      [make_participant_resolver](
          std::vector<core::RuntimeParticipantBinding> physical_bindings) {
        auto resolver = make_participant_resolver(std::move(physical_bindings));
        return RetainedRuntimeParticipantResolver{
            [resolver](const core::OperationIntentDTO &operation,
                       const std::size_t participant_index) {
              return resolver->ResolveExecutionParticipant(operation,
                                                           participant_index);
            }};
      };
  std::vector<core::RuntimeParticipantBinding> target_physical_bindings{
      direct_control_motion_binding.value()};
  if (direct_control_gripper_binding.has_value())
    target_physical_bindings.push_back(*direct_control_gripper_binding);
  auto target_resolver =
      make_participant_resolver(std::move(target_physical_bindings));
  RetainedRuntimeParticipantResolver participant_resolver =
      [resolver = target_resolver](const core::OperationIntentDTO &operation,
                                   const std::size_t participant_index) {
        return resolver->ResolveExecutionParticipant(operation,
                                                     participant_index);
      };
  const auto reference_collision_model = collision_model;
  const auto reference_runtime_unit_id = *local_runtime_unit_id;
  const auto reference_runtime_epoch = input.runtime_epoch;
  const auto reference_component_id = input.selected_component_id;
  RetainedNativeRuntimeInput runtime_input{
      direct_control_motion_binding.value(),
      direct_control_gripper_binding,
      {},
      {},
      std::move(input.motion_program_aliases),
      std::move(input.gripper_program_aliases),
      scene,
      scene_mutex,
      state_projector,
      publisher,
      collision_model,
      std::move(planner),
      [&, reference_collision_model, reference_runtime_unit_id,
       reference_runtime_epoch, reference_component_id](const auto &channels) {
        std::vector<RetainedSceneFrameStateSourceInput> state_sources;
        state_sources.reserve(channels.size());
        for (const auto &channel : channels) {
          const auto binding = channel->binding();
          const auto &resources = binding.value().resources;
          const bool has_model_resource = std::any_of(
              resources.begin(), resources.end(), [](const auto &resource) {
                return resource.source_joint_name.has_value();
              });
          if (!has_model_resource) {
            continue;
          }
          Require(std::all_of(resources.begin(), resources.end(),
                              [](const auto &resource) {
                                return resource.source_joint_name.has_value();
                              }),
                  "native scene-frame state channel mixes model and "
                  "component-local resources");
          state_sources.push_back(RetainedSceneFrameStateSourceInput{
              channel->query_context(), channel->source_kind(),
              std::shared_ptr<const core::IObservedStateProvider>(
                  channel, channel->addressed_publication())});
        }
        return RetainedSceneFrameResolver::create_verified(
            reference_collision_model, *input.bundle, input.model_map,
            *retained_model, reference_runtime_unit_id, reference_runtime_epoch,
            std::move(state_sources), reference_component_id);
      },
      std::move(participant_resolver),
      participant_resolver_factory,
      input.operation_admission_gate,
      {},
      {},
      {},
      {},
      {}};
  runtime_input.control_binding_representatives =
      std::move(control_binding_representatives);
  runtime_input.observation_wake = std::move(input.observation_wake);
  runtime_input.deployment_maximum_tcp_linear_speed_mps =
      deployment_motion_target_limit;
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  runtime_input.ordinary_motion_jerk = motion_limit->ordinary_motion_jerk;
#endif
  for (const auto &limit :
       retained_runtime_configuration.direct_control_joint_limits) {
    const auto &target = runtime_input.motion_binding.value();
    if (limit.controllable_component_id == target.controllable_component_id &&
        limit.command_target_id == target.command_target_id) {
      runtime_input.direct_control_joint_limits.emplace(
          limit.source_joint_name,
          core::JointDynamicsLimitDTO{limit.max_velocity_rad_s,
                                      limit.max_acceleration_rad_s2,
                                      limit.max_jerk_rad_s3});
    }
  }
  if (input.planner_scene_projection != nullptr) {
    runtime_input.planning_scene_synchronizer =
        std::make_shared<ManagerPlanningSceneSynchronizer>(
            input.manager_run_root, input.bundle->retained_bundle_id(),
            input.bundle->value().cell_graph.graph_revision,
            input.publication_generation, publisher);
  }
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  auto ros2_control_runtime_handle =
      std::make_shared<RetainedRos2ControlRuntimeHandle>();
  auto ros2_control_backend_config =
      make_retained_isaac_ros2_control_backend_config(
          retained_runtime_configuration, direct_control_motion_binding.value(),
          std::optional<core::RuntimeParticipantBinding>{
              direct_control_gripper_binding.value()});
  Require(ros2_control_backend_config.execution.controller_binding.has_value(),
          "retained Isaac ROS profile has no controller binding contract");
  const std::string ros2_control_binding_contract_generation_id =
      ros2_control_backend_config.execution.controller_binding->generation_id;
  runtime_input.motion_state_channel_config =
      retained_ros2_control_motion_state_config(
          ros2_control_backend_config.execution);
  runtime_input.gripper_state_channel_config =
      retained_ros2_control_gripper_state_config(
          direct_control_gripper_binding.value());
  runtime_input.backend_factory = make_retained_ros2_control_backend_factory(
      std::move(ros2_control_backend_config), ros2_control_runtime_handle);
#endif
  auto kernel_config = core::ExecutionKernelConfig{};
  kernel_config.collision_monitor_period_ns = collision_monitor_period_ns;
  // Every product composition built by this host executes beneath the
  // resource-addressed Kernel owner.  Managed successor compositions are
  // intentionally built without constructing a second owner and are then
  // installed beneath the retained owner during the fenced swap; they still
  // must hand runtime-collision Stop authority to that owner from their first
  // executable instant.
  kernel_config.runtime_collision_stop_owned_by_addressed_kernel = true;
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  const auto milliseconds_to_ns = [](const double value) {
    return static_cast<core::Duration>(std::llround(value * 1'000'000.0));
  };
  if (retained_runtime_configuration.runtime_collision_immediate_query_budget_ms
          .has_value()) {
    kernel_config.runtime_collision_immediate_query_budget_ns =
        milliseconds_to_ns(*retained_runtime_configuration
                                .runtime_collision_immediate_query_budget_ms);
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_query_budget_ms
          .has_value()) {
    kernel_config.runtime_collision_lookahead_query_budget_ns =
        milliseconds_to_ns(*retained_runtime_configuration
                                .runtime_collision_lookahead_query_budget_ms);
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_cycle_budget_ms
          .has_value()) {
    kernel_config.runtime_collision_lookahead_cycle_budget_ns =
        milliseconds_to_ns(*retained_runtime_configuration
                                .runtime_collision_lookahead_cycle_budget_ms);
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_s
          .has_value()) {
    kernel_config.runtime_collision_lookahead_horizon_ns =
        static_cast<core::Duration>(std::llround(
            *retained_runtime_configuration.runtime_collision_lookahead_s *
            1'000'000'000.0));
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_max_samples
          .has_value()) {
    kernel_config.runtime_collision_lookahead_max_samples =
        *retained_runtime_configuration.runtime_collision_lookahead_max_samples;
  }
  if (retained_runtime_configuration.runtime_collision_lookahead_workers
          .has_value()) {
    kernel_config.runtime_collision_lookahead_workers = static_cast<int>(
        *retained_runtime_configuration.runtime_collision_lookahead_workers);
    kernel_config.runtime_collision_lookahead_workers_declared = true;
  }
  if (retained_runtime_configuration.runtime_dynamic_body_max_age_cap_ms
          .has_value()) {
    kernel_config.runtime_dynamic_body_max_age_cap_ns = milliseconds_to_ns(
        *retained_runtime_configuration.runtime_dynamic_body_max_age_cap_ms);
  }
  if (retained_runtime_configuration.validation_collision_workers.has_value()) {
    kernel_config.validation_collision_workers = static_cast<int>(
        *retained_runtime_configuration.validation_collision_workers);
    kernel_config.validation_collision_workers_declared = true;
  }
#endif
  auto execution_config = RoboPlanExecutionBackendConfig{};
  execution_config.execution_tick_period_ns = execution_tick_period_ns;
  RetainedNativeRuntimeConfig runtime_config{
      input.owner_epoch,
      input.cell_id,
      input.execution_state_root / std::string(core::kOperationJournalFilename),
      1024U,
      core::CacheConfig{},
      std::move(kernel_config),
      std::move(execution_config),
      SimpleSchedulerConfig::from_environment(),
      execution_tick_period_ns,
      std::move(selected_initial_joint_positions),
      deployment::resolve_retained_stop_deadline_budget(*input.bundle,
                                                        catalog_store),
      kMaintainedNativeStopSurveillancePolicy,
      {},
      {},
      {},
      {}};
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
  const std::vector<core::RequiredManagerLivenessReceiverDTO> fixture_receivers;
  runtime_config.manager_liveness_required_set =
      core::ManagerLivenessRequiredSetDTO{
          input.cell_id,
          input.bundle->retained_bundle_id(),
          core::ComputeRequiredRuntimeSetDigest(fixture_receivers),
          core::RuntimeUnitId("standalone-admission-fixture.manager"),
          core::RuntimeEpoch("standalone-admission-fixture.manager-epoch"),
          core::RequestOwnerEpoch("standalone-admission-fixture.manager-owner"),
          fixture_receivers};
#endif
  const auto manager_apply_journal =
      input.manager_state_root /
      std::string(component_manager::kManagerApplyJournalFilename);
  runtime_config.cell_admission_settlement_validator =
      BuildOwnerSettlementValidator(
          manager_apply_journal, input.logical_scene_database,
          input.program_cycle_journal,
          input.execution_state_root / std::string(core::kCCLCJournalFilename),
          input.cell_id);
  runtime_config.cell_admission_reconcile_validator =
      BuildOwnerReconciliationValidator(
          manager_apply_journal, input.program_cycle_journal,
          input.execution_state_root / std::string(core::kCCLCJournalFilename),
          input.cell_id);
  auto runtime = input.attach_addressed_owner
                     ? create_retained_native_runtime(std::move(runtime_input),
                                                      std::move(runtime_config))
                     : create_retained_native_runtime_participants(
                           std::move(runtime_input), std::move(runtime_config),
                           std::move(input.shared_clock),
                           std::move(input.shared_scheduler));
  auto direct_scene_frame_resolver =
      std::dynamic_pointer_cast<RetainedSceneFrameResolver>(
          runtime->reference_resolver);
  Require(direct_scene_frame_resolver != nullptr &&
              direct_scene_frame_resolver->matches_structural_revision(
                  input.bundle->retained_bundle_id(),
                  input.bundle->value().cell_graph.graph_revision),
          "native physical composition did not retain its exact direct frame "
          "resolver");
  if (input.attach_addressed_owner &&
      input.operation_admission_gate != nullptr) {
    input.operation_admission_gate->BindResourceTable(
        runtime->addressed_kernel->resource_admission_table());
  }
  auto direct_control_joint_limits = runtime->kinematic_model->joint_limits(
      direct_control_motion_binding.value()
          .value()
          .controllable_component_id.value(),
      direct_control_motion_binding.value().value().command_target_id.value());
  if (!direct_control_joint_limits.ok()) {
    Fail("direct-control joint limits are unavailable: " +
         direct_control_joint_limits.error().message);
  }
  auto manual_control_descriptors = BuildManualControlDescriptors(
      direct_control_motion_binding.value(),
      direct_control_gripper_binding,
      direct_control_joint_limits.value(), runtime->gripper_position_limits,
      runtime->motion_participant_policy,
      runtime->deployment_maximum_tcp_linear_speed_mps,
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
      &motion_limit->ordinary_motion_jerk,
#else
      nullptr,
#endif
      retained_runtime_configuration.direct_control_joint_limits,
      retained_runtime_configuration.gripper_target_configurations,
      input.model_map, retained_model_documents);
  const auto selected_component_id =
      input.selected_component_id.value_or(
          direct_control_motion_binding.value()
              .value()
              .controllable_component_id);
  auto composition = std::make_unique<Composition>(
      selected_component_id,
      std::move(input.bundle), std::move(input.status_program),
      std::move(input.status_program_binding),
      std::move(direct_control_binding),
      std::move(direct_control_gripper_program_binding),
      std::move(direct_control_motion_binding.value()),
      std::move(direct_control_gripper_binding),
      std::move(direct_control_joint_limits.value()),
      std::move(manual_control_descriptors.motion),
      std::move(manual_control_descriptors.gripper),
      std::make_shared<const RetainedPlanningModelStage>(std::move(stage)),
      std::move(runtime), std::move(input.projection_revision),
      input.publication_generation,
      std::move(input.status_motion_target_role_id),
      std::move(input.status_gripper_target_role_id));
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  if (composition->direct_control_gripper_binding.has_value()) {
    const auto &gripper = *composition->direct_control_gripper_binding;
    for (const auto &target :
         retained_runtime_configuration.gripper_target_configurations) {
      if (target.controllable_component_id ==
              gripper.value().controllable_component_id &&
          target.command_target_id == gripper.value().command_target_id) {
        composition->operator_confirmation_validity_ns =
            target.operator_confirmation_validity_ns;
      }
    }
  }
#endif
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  composition->ros2_control_runtime_handle =
      std::move(ros2_control_runtime_handle);
  composition->ros2_control_binding_contract_generation_id =
      ros2_control_binding_contract_generation_id;
#endif
  composition->planner_scene_projection =
      std::move(input.planner_scene_projection);
  composition->scene_frame_resolver = std::move(direct_scene_frame_resolver);
  composition->target_resolver = std::move(target_resolver);
  if (composition->planner_scene_projection != nullptr) {
    composition->scene_authority_proxy =
        std::make_shared<ManagerSceneAuthorityProxy>(
            std::move(input.manager_run_root));
  }
#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
  auto fixture = process_fixture::MakeFocusedPickPlacePlanningFixture();
  composition->pick_place_planning_fixture_lifetime =
      std::move(fixture.lifetime);
  composition->pick_place_planning_owner = std::move(fixture.owner);
#else
  if (input.attach_addressed_owner) {
    AttachProductPickPlacePlanningOwner(*composition);
  }
#endif
  return composition;
}

[[nodiscard]] std::unique_ptr<Composition>
BuildProductComposition(CompositionBuildInput input) {
  if (input.managed_component_delta.has_value()) {
    Require(input.bundle != nullptr && input.managed_component_delta->valid(),
            "managed composition has an invalid component delta or bundle");
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    Require(input.observation_wake != nullptr,
            "managed composition has no stable cell observation wake");
#endif
    std::vector<core::ControllableComponentId> selected_bundle_components;
    selected_bundle_components.reserve(
        input.bundle->value().cell_graph.controllable_components.size());
    for (const auto &component :
         input.bundle->value().cell_graph.controllable_components) {
      selected_bundle_components.push_back(
          component.controllable_component_id);
    }
    std::sort(selected_bundle_components.begin(),
              selected_bundle_components.end());

    std::vector<core::ControllableComponentId> delta_candidate_components;
    for (const auto &change : input.managed_component_delta->components) {
      if (change.disposition == deployment::ApplyComponentDisposition::kRemove)
        continue;
      delta_candidate_components.push_back(change.controllable_component_id);
    }
    Require(delta_candidate_components == selected_bundle_components,
            "managed component delta rows differ from the selected bundle");
  }

  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      input.package_root);
  const auto inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  const auto sorted_component_ids = [&](const auto &bindings,
                                        const std::string_view capability_id) {
    std::vector<core::ControllableComponentId> component_ids;
    component_ids.reserve(bindings.size());
    for (const auto &binding : bindings) {
      const auto target = deployment::resolve_runtime_program_target(
          *binding,
          core::TargetRoleId(
              std::string(deployment::kDirectControlTargetRoleId)),
          core::CapabilityId(std::string(capability_id)), *input.bundle,
          input.model_map);
      component_ids.push_back(target.component().controllable_component_id);
    }
    std::sort(component_ids.begin(), component_ids.end());
    Require(std::adjacent_find(component_ids.begin(), component_ids.end()) ==
                component_ids.end(),
            "native direct-control inventory repeats a capability for one "
            "component");
    return component_ids;
  };
  const auto motion_component_ids = sorted_component_ids(
      inventory.motion, "execution.trajectory.execute");
  const auto gripper_component_ids =
      sorted_component_ids(inventory.gripper, "tool.gripper.command");
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  Require(!motion_component_ids.empty(),
          "native composition requires at least one motion binding");
  Require(std::includes(motion_component_ids.begin(), motion_component_ids.end(),
                        gripper_component_ids.begin(),
                        gripper_component_ids.end()),
          "native gripper binding has no matching motion component");
  if (inventory.motion.size() == 1U && inventory.gripper.size() <= 1U) {
    return BuildSingleProductComposition(std::move(input));
  }
#else
  if (motion_component_ids.empty() &&
      input.collision_shared_model == nullptr) {
    // A gripper-only cell has no motion-planner projection. Load the verified
    // model for frame resolution only; this creates no planner, collision
    // publisher, or extra Scene authority.
    const auto &products = input.bundle->value().compiled_products;
    const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
        input.package_root);
    auto model = source.get_verified(deployment::RetainedPlanningModelQuery{
        input.bundle->retained_bundle_id(),
        products.planning_collision_model.product_id});
    Require(model.has_value(),
            "gripper-only native cell has no verified frame model");
    std::vector<std::string> groups;
    for (const auto &target : input.model_map.targets()) {
      for (const auto &group : target.model_groups) {
        if (!group.compiled_group_name.empty())
          groups.push_back(group.compiled_group_name);
      }
    }
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    // A component-local gripper need not have a planning group. The collision
    // loader accepts an empty group for frame-only use; no planner is built.
    auto stage = RetainedPlanningModelStage::create_verified(
        *model, input.execution_run_root);
    auto package_paths = stage.package_paths();
    package_paths.insert(package_paths.begin(), input.package_root);
    input.collision_shared_model =
        collision::ManyForgeCollisionEngine::load_shared_model(
            "manyforge_native_gripper_frame_model",
            stage.loader_urdf_path(), stage.srdf_path(), package_paths,
            groups.empty() ? std::string{} : groups.front());
  }
  auto component_ids = motion_component_ids;
  component_ids.insert(component_ids.end(), gripper_component_ids.begin(),
                       gripper_component_ids.end());
  std::sort(component_ids.begin(), component_ids.end());
  component_ids.erase(std::unique(component_ids.begin(), component_ids.end()),
                      component_ids.end());
  Require(!component_ids.empty(),
          "native composition has no supported controllable component");
  if (component_ids.size() == 1U) {
    input.selected_component_id = component_ids.front();
    if (input.retained_component_snapshots.contains(component_ids.front()))
      return BuildRetainedProductComposition(std::move(input));
    if (std::binary_search(motion_component_ids.begin(),
                           motion_component_ids.end(), component_ids.front()))
      return BuildSingleProductComposition(std::move(input));
    return BuildStandaloneGripperProductComposition(std::move(input));
  }
  // Keep a motion owner as the legacy root when one exists. A gripper-only
  // cell has no motion root and is owned by the stable cell Kernel instead.
  if (!motion_component_ids.empty()) {
    const auto root = std::find(component_ids.begin(), component_ids.end(),
                                motion_component_ids.front());
    Require(root != component_ids.end(),
            "native motion root is absent from the component union");
    std::rotate(component_ids.begin(), root, std::next(root));
  }
#endif
  Require(
      input.runtime_unit_id.has_value() && !input.attach_addressed_owner &&
          (!input.component_planner_scene_projections.empty() ||
           motion_component_ids.empty()) &&
          input.shared_clock != nullptr && input.shared_scheduler != nullptr &&
          input.status_program == nullptr &&
          input.status_program_binding == nullptr,
      "multi-robot native composition requires the managed shared-owner path");

#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  auto component_ids = motion_component_ids;
  Require(component_ids.size() == inventory.motion.size() &&
              inventory.gripper.size() <= component_ids.size(),
          "multi-robot native composition requires one motion binding and "
          "optional gripper bindings per robot");
#endif

  std::vector<std::unique_ptr<Composition>> compositions;
  compositions.reserve(component_ids.size());
  if (input.observation_wake == nullptr)
    input.observation_wake = std::make_shared<RetainedObservationWake>();
  for (const auto &component_id : component_ids) {
    auto selected = input;
    selected.selected_component_id = component_id;
    const auto projection =
        selected.component_planner_scene_projections.find(component_id);
    if (projection != selected.component_planner_scene_projections.end()) {
      Require(projection->second != nullptr,
              "native component has a null Scene projection");
      selected.collision_publisher = projection->second;
      selected.collision_shared_model =
          projection->second->collision_shared_model();
      selected.planner_scene_projection = projection->second;
    } else {
      Require(motion_component_ids.empty() &&
                  selected.collision_shared_model != nullptr,
              "native motion component has no Scene projection");
      selected.collision_publisher.reset();
      selected.planner_scene_projection.reset();
    }
    const auto retain_component = [&](auto &aliases) {
      std::erase_if(aliases, [&](const auto &binding) {
        return binding.value().controllable_component_id != component_id;
      });
    };
    retain_component(selected.motion_program_aliases);
    retain_component(selected.gripper_program_aliases);
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    compositions.push_back(BuildSingleProductComposition(std::move(selected)));
#else
    if (selected.retained_component_snapshots.contains(component_id)) {
      compositions.push_back(BuildRetainedProductComposition(
          std::move(selected)));
    } else if (std::binary_search(motion_component_ids.begin(),
                                  motion_component_ids.end(), component_id)) {
      compositions.push_back(BuildSingleProductComposition(
          std::move(selected)));
    } else {
      compositions.push_back(BuildStandaloneGripperProductComposition(
          std::move(selected)));
    }
#endif
  }

  std::vector<core::ProgramBindingId> intrinsic_direct_bindings;
  intrinsic_direct_bindings.reserve(inventory.motion.size() +
                                    inventory.gripper.size());
  for (const auto &binding : inventory.motion)
    intrinsic_direct_bindings.push_back(binding->program_binding_id());
  for (const auto &binding : inventory.gripper)
    intrinsic_direct_bindings.push_back(binding->program_binding_id());
  std::sort(intrinsic_direct_bindings.begin(), intrinsic_direct_bindings.end());
  intrinsic_direct_bindings.erase(std::unique(intrinsic_direct_bindings.begin(),
                                              intrinsic_direct_bindings.end()),
                                  intrinsic_direct_bindings.end());
  const auto make_resolver =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       program_artifact_root = input.program_artifact_root,
       intrinsic_direct_bindings = std::move(intrinsic_direct_bindings)](
          std::vector<core::RuntimeParticipantBinding> physical_bindings) {
        return std::make_shared<ProductBackedRuntimeParticipantResolver>(
            package_root, bundle, model_map, std::move(physical_bindings),
            intrinsic_direct_bindings, program_artifact_root);
      };
  RetainedRuntimeParticipantResolverFactory resolver_factory =
      [make_resolver](std::vector<core::RuntimeParticipantBinding> bindings) {
        auto resolver = make_resolver(std::move(bindings));
        return RetainedRuntimeParticipantResolver{
            [resolver](const core::OperationIntentDTO &operation,
                       const std::size_t participant_index) {
              return resolver->ResolveExecutionParticipant(operation,
                                                           participant_index);
            }};
      };

  auto primary = std::move(compositions.front());
  compositions.erase(compositions.begin());
  const bool has_retained_owner =
      !input.retained_component_snapshots.empty();
  if (!has_retained_owner) {
    std::vector<std::shared_ptr<RetainedNativeRuntime>> peers;
    peers.reserve(compositions.size());
    for (const auto &composition : compositions) {
      if (composition->runtime != nullptr)
        peers.push_back(composition->runtime);
    }
    if (primary->runtime != nullptr)
      join_retained_native_runtime_participants(primary->runtime,
                                                std::move(peers),
                                                resolver_factory);
  } else {
    primary->candidate_participant_resolver_factory = resolver_factory;
  }
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  physical_bindings.reserve(component_ids.size() * 2U);
  if (primary->direct_control_motion_binding.has_value())
    physical_bindings.push_back(*primary->direct_control_motion_binding);
  if (primary->direct_control_gripper_binding.has_value())
    physical_bindings.push_back(*primary->direct_control_gripper_binding);
  for (const auto &composition : compositions) {
    if (composition->direct_control_motion_binding.has_value())
      physical_bindings.push_back(*composition->direct_control_motion_binding);
    if (composition->direct_control_gripper_binding.has_value())
      physical_bindings.push_back(*composition->direct_control_gripper_binding);
  }
  primary->target_resolver = make_resolver(std::move(physical_bindings));
  primary->participant_compositions = std::move(compositions);
  // Scene is a cell authority. Its component-relative frames may refer to
  // any installed robot, not just the component selected as this cell's
  // addressed-Kernel root. Keep each engine's local resolver separate, but
  // install one verified cell resolver over every live state source.
  {
    const deployment::FilesystemVerifiedRetainedPlanningModelSource source(
        input.package_root);
    const auto retained_model = source.get_verified(
        deployment::RetainedPlanningModelQuery{
            input.bundle->retained_bundle_id(),
            input.bundle->value().compiled_products.planning_collision_model
                .product_id});
    Require(retained_model.has_value() && input.runtime_unit_id.has_value() &&
                input.collision_shared_model != nullptr,
            "native cell frame resolver has no candidate model or owner");
    std::vector<RetainedSceneFrameStateSourceInput> state_sources;
    const auto include_channel = [&](const auto &channel) {
      if (channel == nullptr)
        return;
      const auto *retained_channel =
          dynamic_cast<const RetainedObservedStateChannel *>(channel.get());
      Require(retained_channel != nullptr,
              "native cell frame source is not route-aware");
      const auto channel_binding = retained_channel->binding();
      const auto &resources = channel_binding.value().resources;
      if (std::none_of(resources.begin(), resources.end(),
                       [](const auto &resource) {
                         return resource.source_joint_name.has_value();
                       }))
        return;
      Require(std::all_of(resources.begin(), resources.end(),
                          [](const auto &resource) {
                            return resource.source_joint_name.has_value();
                          }),
              "native cell frame source mixes local and model coordinates");
      state_sources.push_back(RetainedSceneFrameStateSourceInput{
          channel->query_context(), retained_channel->source_kind(),
          std::shared_ptr<const core::IObservedStateProvider>(
              channel, channel->addressed_publication())});
    };
    const auto include_owner = [&](const Composition &view) {
      Require(view.component_runtime_identity() != nullptr,
              "native cell frame resolver has no component owner");
      if (view.runtime != nullptr) {
        include_channel(view.runtime->motion_state_channel);
        include_channel(view.runtime->gripper_state_channel);
      } else {
        include_channel(view.gripper_runtime->state_channel());
      }
    };
    include_owner(*primary);
    for (const auto &peer : primary->participant_compositions)
      include_owner(*peer);
    auto cell_frame_resolver = RetainedSceneFrameResolver::create_verified(
        input.collision_shared_model, *input.bundle, input.model_map,
        *retained_model, *input.runtime_unit_id, input.runtime_epoch,
        std::move(state_sources));
    Require(cell_frame_resolver != nullptr,
            "native cell frame resolver could not be built");
    // A component's Program may name a frame on another installed component.
    // Candidate views all use the same verified cell source set. A retained
    // engine is re-sealed only at fenced installation; fresh engines are
    // private and may receive the resolver now.
    const auto install_candidate_resolver = [&](Composition &view) {
      view.scene_frame_resolver = cell_frame_resolver;
      if (input.retained_component_snapshots.contains(
              view.controllable_component_id))
        return;
      if (view.gripper_runtime != nullptr)
        return;
      Require(view.runtime != nullptr &&
                  view.runtime->reference_resolver_proxy != nullptr,
              "native candidate has no frame-resolution owner");
      view.runtime->reference_resolver = cell_frame_resolver;
      view.runtime->reference_resolver_proxy->ReplaceTarget(
          cell_frame_resolver);
    };
    install_candidate_resolver(*primary);
    for (auto &peer : primary->participant_compositions)
      install_candidate_resolver(*peer);
  }
  return primary;
}

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
[[nodiscard]] std::vector<core::ControllableComponentId>
RetainedNativeComponentIds(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  std::vector<core::ControllableComponentId> ids;
  for (const auto &change : request.component_delta.components) {
    if (change.disposition == deployment::ApplyComponentDisposition::kRetain)
      ids.push_back(change.controllable_component_id);
  }
  Require(std::is_sorted(ids.begin(), ids.end()) &&
              std::adjacent_find(ids.begin(), ids.end()) == ids.end(),
          "retained native Apply repeats a component identity");
  return ids;
}

void ResealRetainedNativeCandidateOwners(
    Composition &candidate, const Composition &predecessor,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    core::IResourceOperationAdmissionGate &admission_gate) {
  auto old = IndexNativeComponentCompositions(predecessor);
  auto next = IndexNativeComponentCompositions(candidate);
  for (const auto &component_id : RetainedNativeComponentIds(request)) {
    const auto previous = old.find(component_id);
    const auto selected = next.find(component_id);
    Require(previous != old.end() && selected != next.end() &&
                previous->second->component_runtime_identity() != nullptr &&
                previous->second->component_runtime_identity() ==
                    selected->second->component_runtime_identity() &&
                selected->second->scene_frame_resolver != nullptr &&
                !selected->second->candidate_component_registrations.empty(),
            "retained native owner differs from the planned predecessor");
    auto &view = *selected->second;
    Require(admission_gate.exact_fence_is_held(
                request.configuration_fence_id,
                request.configuration_fence_scope),
            "retained native component lost the exact parent Apply fence");
    if (view.gripper_runtime != nullptr) {
      Require(view.runtime == nullptr &&
                  view.candidate_component_registrations.size() == 1U &&
                  view.direct_control_gripper_binding.has_value(),
              "retained gripper candidate has no exact physical participant");
      const auto channel =
          std::dynamic_pointer_cast<RetainedObservedStateChannel>(
              view.gripper_runtime->state_channel());
      Require(channel != nullptr,
              "retained gripper has no rebindable observed-state owner");
      Require(predecessor.cell_runtime != nullptr &&
                  predecessor.cell_runtime->clock() != nullptr,
              "retained gripper has no stable cell clock");
      const auto rebound = channel->RebindRoute(
          *view.direct_control_gripper_binding,
          predecessor.cell_runtime->clock()->now());
      if (!rebound.ok())
        Fail("retained gripper state route cannot be re-sealed: " +
             rebound.error().message);
      const auto installed = view.gripper_runtime->RebindRoute(
          *view.direct_control_gripper_binding, channel,
          view.candidate_component_registrations.front().stop_readiness);
      if (!installed.ok())
        Fail("retained gripper controller cannot be re-sealed: " +
             installed.error().message);
      continue;
    }
    Require(view.runtime != nullptr &&
                view.direct_control_motion_binding.has_value(),
            "retained motion candidate has no physical owner");
    RetainedNativeRuntimeResealInput reseal{
        *view.direct_control_motion_binding,
        view.direct_control_gripper_binding,
        {},
        {},
        view.candidate_control_representatives,
        view.candidate_component_registrations,
        {},
        nullptr,
        view.scene_frame_resolver,
        request.configuration_fence_id,
        request.configuration_fence_scope,
        admission_gate};
    auto result = reseal_retained_native_runtime_for_apply(*view.runtime,
                                                           std::move(reseal));
    if (!result.ok()) {
      Fail("retained native owner cannot be re-sealed under Apply: " +
           result.error().message);
    }
  }
}

void RefreshRetainedNativeCandidateCollision(
    Composition &candidate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    core::IResourceOperationAdmissionGate &admission_gate) {
  std::map<core::ControllableComponentId, Composition *> selected;
  selected.emplace(candidate.controllable_component_id, &candidate);
  for (auto &component : candidate.participant_compositions) {
    Require(component != nullptr &&
                selected.emplace(component->controllable_component_id,
                                 component.get()).second,
            "retained native collision candidate repeats an owner");
  }
  for (const auto &component_id : RetainedNativeComponentIds(request)) {
    const auto found = selected.find(component_id);
    Require(found != selected.end() &&
                found->second->component_runtime_identity() != nullptr,
            "retained native collision owner is absent");
    auto &view = *found->second;
    // A standalone gripper has no planner or collision-engine resources to
    // reconfigure. Its geometry changes through the Scene projection below.
    if (view.gripper_runtime != nullptr)
      continue;
    auto &runtime = *view.runtime;
    Require(runtime.participant_engine != nullptr &&
                view.candidate_collision_publisher != nullptr &&
                view.candidate_collision_model != nullptr &&
                view.candidate_runtime_collision_engine != nullptr,
            "retained native candidate collision resources are incomplete");
    core::CollisionPipelineResourceOwners owners{
        view.candidate_collision_publisher,
        view.candidate_runtime_collision_engine,
        view.candidate_validation_collision_engines,
        view.candidate_lookahead_collision_engines};
    auto installed = runtime.participant_engine
                         ->replace_collision_resources_under_apply_fence(
                             request.configuration_fence_id,
                             request.configuration_fence_scope, admission_gate,
                             std::move(owners));
    if (!installed.ok())
      Fail("retained native collision refresh failed: " +
           installed.error().message);
    auto readback = runtime.participant_engine->collision_resource_set_readback();
    const auto candidate_policy =
        view.candidate_collision_publisher->collision_policy_snapshot();
    Require(readback.ok() && readback.value() == installed.value() &&
                readback.value().resources.snapshot_publisher ==
                    view.candidate_collision_publisher &&
                readback.value().resources.runtime_monitor_engine ==
                    view.candidate_runtime_collision_engine &&
                readback.value().resources.validation_engines ==
                    view.candidate_validation_collision_engines &&
                readback.value().resources.runtime_lookahead_engines ==
                    view.candidate_lookahead_collision_engines &&
                candidate_policy.has_value() &&
                readback.value().coordinator_policy_snapshot != nullptr &&
                *readback.value().coordinator_policy_snapshot ==
                    *candidate_policy,
            "retained native collision readback differs from the candidate");
    runtime.collision_publisher = view.candidate_collision_publisher;
    runtime.runtime_collision_engine =
        view.candidate_runtime_collision_engine;
    runtime.validation_collision_engines =
        view.candidate_validation_collision_engines;
    runtime.lookahead_collision_engines =
        view.candidate_lookahead_collision_engines;
  }
}

// A retained controller keeps its identities (controller, Kernel, planner
// client, observed sources) but every model-dependent computation follows the
// candidate's whole-cell planning model: the worker's base model, the
// in-process scene with its kinematics, validator and collision projector,
// and each model-backed state projector. The candidate composition owns the
// stage whose files the replacement worker loads. Collision resources were
// installed just before; this completes one coherent model generation and
// reads it back before admission can reopen. A failure is rethrown as the
// Apply failure; nothing is restored and the fence keeps admission closed.
void RefreshRetainedNativeCandidateModels(
    Composition &candidate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    core::IResourceOperationAdmissionGate &admission_gate,
    core::IClock &clock) {
  std::map<core::ControllableComponentId, Composition *> selected;
  selected.emplace(candidate.controllable_component_id, &candidate);
  for (auto &component : candidate.participant_compositions) {
    Require(component != nullptr &&
                selected.emplace(component->controllable_component_id,
                                 component.get()).second,
            "retained native model candidate repeats an owner");
  }
  for (const auto &component_id : RetainedNativeComponentIds(request)) {
    const auto found = selected.find(component_id);
    Require(found != selected.end(),
            "retained native model owner is absent");
    auto &view = *found->second;
    Require(admission_gate.exact_fence_is_held(
                request.configuration_fence_id,
                request.configuration_fence_scope),
            "retained native model refresh lost the exact parent Apply fence");
    if (view.gripper_runtime != nullptr) {
      if (view.candidate_tool_state_projector == nullptr)
        continue;
      const auto channel =
          std::dynamic_pointer_cast<RetainedObservedStateChannel>(
              view.gripper_runtime->state_channel());
      const auto *backend = view.gripper_runtime->backend();
      Require(channel != nullptr && backend != nullptr,
              "retained gripper has no model-backed observed-state owner");
      Require(backend
                      ->get_gripper_status(view.gripper_runtime->binding()
                                               .value()
                                               .command_target_id.value())
                      .state != core::GripperCommandState::kExecuting,
              "retained gripper has active command work during model refresh");
      const auto replaced = channel->ReplaceKinematicProjector(
          view.candidate_tool_state_projector, clock.now());
      if (!replaced.ok())
        Fail("retained gripper rejected the candidate cell model: " +
             replaced.error().message);
      Require(channel->kinematic_projector() ==
                  view.candidate_tool_state_projector,
              "retained gripper does not read back the candidate model");
      view.candidate_tool_state_projector.reset();
      continue;
    }
    Require(view.candidate_planner_model.has_value() ==
                (view.candidate_model_generation != nullptr),
            "retained native model candidate is incomplete");
    if (!view.candidate_planner_model.has_value())
      continue;
    Require(view.runtime != nullptr && view.runtime->planner != nullptr &&
                view.stage != nullptr &&
                view.candidate_planner_model->urdf_path ==
                    view.stage->loader_urdf_path(),
            "retained native planner has no staged candidate model");
    const auto replaced =
        view.runtime->planner->ReplaceRetainedModel(*view.candidate_planner_model);
    if (!replaced.ok())
      Fail("retained native planner rejected the candidate planning model: " +
           replaced.error().message);
    const auto refreshed = refresh_retained_native_runtime_model_for_apply(
        *view.runtime,
        RetainedNativeRuntimeModelRefreshInput{
            view.candidate_model_generation, request.configuration_fence_id,
            request.configuration_fence_scope, admission_gate});
    if (!refreshed.ok())
      Fail("retained native model consumers rejected the candidate cell "
           "model: " +
           refreshed.error().message);
    view.candidate_planner_model.reset();
    view.candidate_model_generation.reset();
  }
}
#endif

[[nodiscard]] std::unique_ptr<Composition>
BuildComposition(const HostConfig &config,
                 const core::RequestOwnerEpoch &owner_epoch,
                 const core::RuntimeEpoch &runtime_epoch,
                 ManagedKernelAdmissionGate *operation_admission_gate) {
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  Require(integrity.current_generation.nonzero(),
          "active retained-bundle publication is absent");
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  Require(publication.ok() && publication.value().valid(),
          "active retained-bundle publication cannot be verified");
  const auto receipt = publication.value();
  publication_store.Close();

  const deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      config.package_root);
  auto bundle =
      bundle_source.get_verified(receipt.request.candidate_retained_bundle_id);
  Require(bundle != nullptr &&
              bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              bundle->value().cell_graph.graph_revision ==
                  receipt.request.candidate_graph_revision &&
              bundle->retained_bundle_id() ==
                  receipt.request.candidate_projection_revision
                      .retained_bundle_id &&
              bundle->value().cell_graph.graph_revision ==
                  receipt.request.candidate_projection_revision.graph_revision,
          "active publication and retained bundle disagree");
  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "N3 native host requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      config.package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "active retained model map is absent or invalid");

  const deployment::FilesystemVerifiedProgramBindingSource binding_repository(
      config.package_root);
  auto program_binding = binding_repository.get_verified(
      core::ProgramBindingId(config.program_binding_id), *bundle, *model_map);
  Require(program_binding != nullptr,
          "exact retained Program Binding is absent or invalid");
  const deployment::FilesystemVerifiedProgramSource program_repository(
      config.package_root);
  auto program = program_repository.get_verified(
      program_binding->value().program_id,
      program_binding->value().program_revision, *bundle);
  Require(program != nullptr,
          "Program and exact retained Program Binding disagree");

  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      config.package_root);
  auto direct_inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *bundle, *model_map, catalog_store);
  if (direct_inventory.motion.empty()) {
    Require(direct_inventory.gripper.size() == 1U &&
                config.motion_target_role_id.empty() &&
                !config.gripper_target_role_id.empty(),
            "native no-motion composition requires one explicitly selected gripper target");
    // Let the component inventory choose the gripper-only owner. It creates
    // no motion engine or planner and is attached to the stable cell Kernel by
    // the same managed bootstrap path as every other component.
    return BuildProductComposition(CompositionBuildInput{
        config.package_root,
        config.execution_state_root,
        config.execution_run_root,
        config.candidate_worker,
        core::CellId(config.cell_id),
        owner_epoch,
        std::nullopt,
        runtime_epoch,
        std::move(bundle),
        std::move(*model_map),
        receipt.request.candidate_projection_revision,
        receipt.publication_generation,
        {},
        {},
        {},
        {},
        operation_admission_gate,
        std::move(program),
        std::move(program_binding),
        std::nullopt,
        core::TargetRoleId(config.gripper_target_role_id),
        true,
        {},
        {},
        {},
        {},
        {},
        {},
        {},
        {},
    });
  }
  Require(direct_inventory.motion.size() == 1U &&
              direct_inventory.gripper.size() <= 1U,
          "native Program aliases have no sole physical motion target or have "
          "ambiguous gripper targets");
  const auto physical_motion = deployment::resolve_runtime_program_target(
      *direct_inventory.motion.front(),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::CapabilityId("execution.trajectory.execute"), *bundle, *model_map);
  std::optional<deployment::RuntimeProgramTargetBinding> physical_gripper;
  if (!direct_inventory.gripper.empty()) {
    physical_gripper = deployment::resolve_runtime_program_target(
        *direct_inventory.gripper.front(),
        core::TargetRoleId(
            std::string(deployment::kDirectControlTargetRoleId)),
        core::CapabilityId("tool.gripper.command"), *bundle, *model_map);
  }
  Require(!config.motion_target_role_id.empty(),
          "native Program has no selected motion target role");
  std::vector<deployment::RuntimeProgramTargetBinding> physical_support;
  const auto &selected_target = program_binding->value().target_role_bindings;
  const auto selected_motion = deployment::resolve_runtime_program_target(
      *program_binding, core::TargetRoleId(config.motion_target_role_id),
      core::CapabilityId("execution.trajectory.execute"), *bundle, *model_map);
  const auto selected_motion_role = std::find_if(
      selected_target.begin(), selected_target.end(), [&](const auto &role) {
        return role.target_role_id ==
               core::TargetRoleId(config.motion_target_role_id);
      });
  Require(selected_motion_role != selected_target.end(),
          "native Program motion target role is absent");
  for (const auto &capability : selected_motion_role->capability_bindings) {
    if (capability.capability_id.value().starts_with("planning.motion.")) {
      physical_support.push_back(deployment::resolve_runtime_program_target(
          *program_binding, core::TargetRoleId(config.motion_target_role_id),
          capability.capability_id, *bundle, *model_map));
    }
  }
  const auto motion_alias = core::verify_runtime_participant_binding(
      deployment::build_runtime_participant_binding_dto(
          selected_motion,
          ActiveFacts(
              selected_motion,
              std::span<const deployment::RuntimeProgramTargetBinding>(
                  physical_support),
              runtime_epoch,
              core::ActiveControlConfigurationId("active.i6.native.arm")),
          std::span<const deployment::RuntimeProgramTargetBinding>(
              physical_support)));
  Require(motion_alias.ok(), "native Program motion alias failed verification");

  std::vector<core::RuntimeParticipantBinding> motion_aliases;
  motion_aliases.push_back(motion_alias.value());
  std::vector<core::RuntimeParticipantBinding> gripper_aliases;
  const bool selected_program_has_gripper = std::any_of(
      selected_target.begin(), selected_target.end(), [](const auto &role) {
        return std::any_of(role.capability_bindings.begin(),
                           role.capability_bindings.end(), [](const auto &cap) {
                             return cap.capability_id.value() ==
                                    "tool.gripper.command";
                           });
      });
  if (selected_program_has_gripper) {
    Require(!config.gripper_target_role_id.empty() &&
                physical_gripper.has_value(),
            "native Program gripper target is not physically available");
    const auto selected_gripper = deployment::resolve_runtime_program_target(
        *program_binding, core::TargetRoleId(config.gripper_target_role_id),
        core::CapabilityId("tool.gripper.command"), *bundle, *model_map);
    Require(selected_gripper.component().controllable_component_id ==
                    physical_gripper->component().controllable_component_id &&
                selected_gripper.target_role().command_target_id ==
                    physical_gripper->target_role().command_target_id,
            "native Program gripper alias does not match its physical target");
    const auto gripper_alias = core::verify_runtime_participant_binding(
        deployment::build_runtime_participant_binding_dto(
            selected_gripper, ActiveFacts(selected_gripper, {}, runtime_epoch,
                                          core::ActiveControlConfigurationId(
                                              "active.i6.native.gripper"))));
    Require(gripper_alias.ok(),
            "native Program gripper alias failed verification");
    gripper_aliases.push_back(gripper_alias.value());
  }
  Require(physical_motion.component().controllable_component_id ==
                  selected_motion.component().controllable_component_id &&
              physical_motion.target_role().command_target_id ==
                  selected_motion.target_role().command_target_id &&
              (!physical_gripper.has_value() ||
               physical_gripper->component().controllable_component_id ==
                   physical_motion.component().controllable_component_id) &&
              (!selected_program_has_gripper ||
               (physical_gripper.has_value() &&
                physical_gripper->component().controllable_component_id ==
                    selected_motion.component().controllable_component_id)),
          "native Program aliases cross their physical component");

  return BuildProductComposition(CompositionBuildInput{
      config.package_root,
      config.execution_state_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      owner_epoch,
      std::nullopt,
      runtime_epoch,
      std::move(bundle),
      std::move(*model_map),
      receipt.request.candidate_projection_revision,
      receipt.publication_generation,
      {},
      {},
      std::move(motion_aliases),
      std::move(gripper_aliases),
      operation_admission_gate,
      std::move(program),
      std::move(program_binding),
      core::TargetRoleId(config.motion_target_role_id),
      config.gripper_target_role_id.empty()
          ? std::optional<core::TargetRoleId>{}
          : std::optional<core::TargetRoleId>{
                core::TargetRoleId(config.gripper_target_role_id)},
      true,
      {},
      {},
      {},
      {},
      {},
      {},
      {},
      {},
  });
}

#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
[[nodiscard]] std::string
Ros2ControlProviderComponentEpoch(const Composition &composition) {
  std::vector<std::string> occurrences;
  std::vector<const core::RuntimeParticipantBinding *> bindings{
      &*composition.direct_control_motion_binding};
  if (composition.direct_control_gripper_binding.has_value())
    bindings.push_back(&*composition.direct_control_gripper_binding);
  for (const auto *binding : bindings) {
    Require(binding->value().control_path.has_value(),
            "retained Isaac ROS participant has no control path");
    const auto &path = *binding->value().control_path;
    occurrences.push_back(path.control_domain_id.value() + "\n" +
                          path.provider_instance_id.value() + "\n" +
                          path.provider_manifest_id.value() + "\n" +
                          path.provider_manifest_revision.value() + "\n" +
                          path.provider_runtime_unit_id.value() + "\n" +
                          path.provider_runtime_epoch.value());
  }
  std::sort(occurrences.begin(), occurrences.end());
  std::string preimage("manyforge.ros2-control-provider-component-epoch.v1");
  for (const auto &occurrence : occurrences) {
    preimage.push_back('\0');
    preimage.append(occurrence);
  }
  return content_identity::sha256_digest(preimage);
}

[[nodiscard]] std::string
Ros2ControlCellSupervisorEpoch(const Composition &composition) {
  std::string preimage("manyforge.cell-supervisor-component-epoch.v1");
  preimage.push_back('\0');
  preimage.append(composition.bundle->retained_bundle_id().value());
  preimage.push_back('\0');
  preimage.append(
      composition.bundle->value().cell_graph.graph_revision.value());
  preimage.push_back('\0');
  preimage.append(std::to_string(composition.publication_generation.value()));
  return content_identity::sha256_digest(preimage);
}
#endif

[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
PhysicalControlBindings(const Composition &composition) {
  std::vector<core::RuntimeParticipantBinding> physical_bindings;
  if (composition.runtime != nullptr) {
    physical_bindings =
        RetainedNativeComponentOwner(composition.runtime).physical_bindings();
  } else if (composition.gripper_runtime != nullptr) {
    physical_bindings = RetainedNativeComponentOwner(
                            composition.gripper_runtime)
                            .physical_bindings();
  }
  for (const auto &participant : composition.participant_compositions) {
    auto nested = PhysicalControlBindings(*participant);
    physical_bindings.insert(physical_bindings.end(),
                             std::make_move_iterator(nested.begin()),
                             std::make_move_iterator(nested.end()));
  }
  return physical_bindings;
}

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
[[nodiscard]] std::vector<core::RuntimeParticipantExecutionRegistration>
NativeCellComponentRegistrations(const Composition &cell) {
  std::vector<core::RuntimeParticipantExecutionRegistration> result;
  std::set<core::ControllableComponentId> owners;
  const auto append = [&](const Composition &view) {
    Require(owners.insert(view.controllable_component_id).second,
            "native cell repeats a component registration owner");
    auto rows = view.component_registrations();
    Require(!rows.empty(), "native cell component has no registrations");
    result.insert(result.end(), std::make_move_iterator(rows.begin()),
                  std::make_move_iterator(rows.end()));
  };
  append(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr, "native cell has a missing component owner");
    append(*participant);
  }
  return result;
}

[[nodiscard]] core::RuntimeParticipantAdmissionResolver
NativeCellParticipantResolver(
    const Composition &cell,
    const std::vector<core::RuntimeParticipantExecutionRegistration>
        &registrations) {
  Require(cell.target_resolver != nullptr,
          "native cell has no product-backed participant resolver");
  auto resolver = cell.target_resolver;
  RetainedRuntimeParticipantResolver binding_resolver =
      [resolver](const core::OperationIntentDTO &operation,
                 const std::size_t index) {
        return resolver->ResolveExecutionParticipant(operation, index);
      };
  return make_retained_cell_kernel_participant_resolver(
      std::move(binding_resolver), registrations);
}

void AppendNativeGripperStopSources(
    const Composition &cell,
    std::vector<RetainedStopSurveillanceSourceInput> &result) {
  const auto add_gripper = [&](const Composition &view) {
    if (view.gripper_runtime == nullptr)
      return;
    auto source = build_retained_gripper_stop_surveillance_source(
        view.gripper_runtime->binding(),
        view.gripper_runtime->stop_readiness(),
        view.gripper_runtime->state_channel(),
        view.gripper_runtime->participant_engine());
    if (source.has_value() &&
        std::none_of(result.begin(), result.end(), [&](const auto &row) {
          return row.binding.value() == source->binding.value();
        }))
      result.push_back(std::move(*source));
  };
  add_gripper(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr, "native cell has a missing Stop participant");
    add_gripper(*participant);
  }
}

[[nodiscard]] std::vector<RetainedStopSurveillanceSourceInput>
NativeCellStopSources(const Composition &cell) {
  std::vector<RetainedStopSurveillanceSourceInput> result;
  if (cell.runtime != nullptr && cell.runtime->stop_surveillance_owner != nullptr)
    result = cell.runtime->stop_surveillance_owner->SnapshotSources();
  std::set<core::ControllableComponentId> independent_grippers;
  const auto note_gripper = [&](const Composition &view) {
    if (view.gripper_runtime != nullptr)
      independent_grippers.insert(view.controllable_component_id);
  };
  note_gripper(cell);
  for (const auto &participant : cell.participant_compositions) {
    Require(participant != nullptr, "native cell has a missing Stop participant");
    note_gripper(*participant);
  }
  std::erase_if(result, [&](const auto &row) {
    return independent_grippers.contains(
        row.binding.value().controllable_component_id);
  });
  AppendNativeGripperStopSources(cell, result);
  if (!result.empty())
    Require(RetainedStopSurveillanceOwner::ValidateSources(result).ok(),
            "native cell has invalid Stop observation sources");
  return result;
}
#endif

[[nodiscard]] std::vector<core::RuntimeParticipantBinding>
CellControlBindingRepresentatives(const Composition &composition) {
  auto representatives =
      composition.candidate_control_representatives.empty()
          ? (composition.runtime != nullptr
                 ? composition.runtime->control_binding_representatives
                 : std::vector<core::RuntimeParticipantBinding>{
                       composition.gripper_runtime->binding()})
          : composition.candidate_control_representatives;
  for (const auto &participant : composition.participant_compositions) {
    auto nested = CellControlBindingRepresentatives(*participant);
    representatives.insert(representatives.end(),
                           std::make_move_iterator(nested.begin()),
                           std::make_move_iterator(nested.end()));
  }
  return representatives;
}

[[nodiscard]] core::Result<void> VerifyNativeEngineForDomain(
    const Composition &composition, const core::ControlDomainId &domain) {
  if (composition.runtime != nullptr) {
    for (const auto &registration :
         composition.runtime->participant_registrations) {
      const auto &path = registration.binding.value().control_path;
      if (path && path->control_domain_id == domain &&
          registration.engine != nullptr &&
          registration.stop_execution_port != nullptr) {
        return core::Result<void>::Ok();
      }
    }
  }
  if (composition.gripper_runtime != nullptr) {
    const auto registration = composition.gripper_runtime->registration();
    const auto &path = registration.binding.value().control_path;
    if (path && path->control_domain_id == domain &&
        registration.engine != nullptr &&
        registration.stop_execution_port != nullptr) {
      return core::Result<void>::Ok();
    }
  }
  for (const auto &participant : composition.participant_compositions) {
    if (VerifyNativeEngineForDomain(*participant, domain).ok()) {
      return core::Result<void>::Ok();
    }
  }
  return core::Result<void>::Err(
      {"native_lifecycle_engine_unavailable",
       "the exact native participant engine is not installed for the control domain",
       {{"reason", "native_engine_or_stop_port_missing"},
        {"control_domain_id", domain.value()}}});
}

void DetachRetainedCCLC(Composition &composition);

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
[[nodiscard]] core::NativeApplyFactAdoptionDTO ReadNativeApplyFacts(
    Composition &composition,
    const core::ActiveBundlePublicationReceiptDTO &publication,
    const core::NativeApplyPlanDTO &plan) {
  Require(publication.valid() &&
              publication.publication_generation ==
                  composition.publication_generation &&
              publication.request.candidate_retained_bundle_id ==
                  composition.bundle->retained_bundle_id() &&
              plan.candidate_retained_bundle_id ==
                  composition.bundle->retained_bundle_id(),
          "native CCLC adoption does not name the installed candidate");
  core::NativeApplyFactAdoptionDTO facts{publication, {}, {}};
  const auto planned_component = [&](const core::ControllableComponentId &id) {
    const auto found = std::find_if(
        plan.components.begin(), plan.components.end(),
        [&](const auto &row) { return row.component_id == id; });
    Require(found != plan.components.end(),
            "native CCLC adoption has an unplanned component");
    return *found;
  };
  const auto representatives = CellControlBindingRepresentatives(composition);
  std::set<core::ControllableComponentId> ready_components;
  for (const auto &desired :
       composition.bundle->value().desired_control_configurations) {
    const auto engine = VerifyNativeEngineForDomain(
        composition, desired.control_domain_id);
    if (!engine.ok()) {
      Fail("native CCLC adoption has no exact engine: " +
           engine.error().message);
    }
    const auto current = composition.active_addressed_kernel()
                             ->current_runtime_route_configuration(
                                 desired.control_domain_id);
    Require(current.ok() && current.value().has_value(),
            "native CCLC adoption has no route readback");
    const auto &active = current.value()->active_configuration;
    const auto planned_route = std::find_if(
        plan.routes.begin(), plan.routes.end(), [&](const auto &row) {
          return row.control_domain_id == desired.control_domain_id;
        });
    Require(planned_route != plan.routes.end() &&
                active.active_control_configuration_generation ==
                    planned_route->generation,
            "native CCLC adoption route generation differs from the plan");
    Require(active.locally_valid() &&
                active.state ==
                    core::ActiveControlConfigurationStateDTO::kReconciled &&
                active.control_domain_id == desired.control_domain_id &&
                active.control_domain_revision ==
                    desired.control_domain_revision &&
                active.activation_set_id == desired.activation_set_id &&
                active.controller_topology_id ==
                    desired.controller_topology_id &&
                active.topology_revision == desired.topology_revision &&
                active.binding_routes.size() == desired.binding_routes.size() &&
                current.value()->readiness_rows.size() ==
                    desired.binding_routes.size(),
            "native CCLC adoption route differs from retained intent");
    for (const auto &route : desired.binding_routes) {
      Require(std::any_of(active.binding_routes.begin(),
                          active.binding_routes.end(), [&](const auto &row) {
                            return row.control_binding_id ==
                                       route.control_binding_id &&
                                   row.control_binding_revision ==
                                       route.control_binding_revision &&
                                   row.control_route_id == route.control_route_id;
                          }),
              "native CCLC adoption is missing a retained binding route");
      const auto representative = std::find_if(
          representatives.begin(), representatives.end(), [&](const auto &row) {
            return row.value().control_path.has_value() &&
                   row.value().control_path->control_domain_id ==
                       desired.control_domain_id &&
                   row.value().control_path->control_binding_id ==
                       route.control_binding_id &&
                   row.value().control_path->control_binding_revision ==
                       route.control_binding_revision;
          });
      Require(representative != representatives.end(),
              "native CCLC adoption has no physical binding representative");
      ready_components.insert(
          representative->value().controllable_component_id);
      // Route currency and component command eligibility are independent.
      // A Disabled component keeps its verified route but cannot dispatch.
      Require(current.value()->operation_admission_open,
              "native CCLC adoption route is not current");
    }
    facts.current_routes.push_back(active);
  }
  for (const auto &component :
       composition.bundle->value().cell_graph.controllable_components) {
    Require(ready_components.erase(component.controllable_component_id) == 1U,
            "native CCLC adoption has a component without verified routes");
    const auto readiness = composition.active_addressed_kernel()
                               ->component_command_readiness(
                                   component.controllable_component_id);
    Require(readiness.ok() &&
                readiness.value().component_lifecycle_generation ==
                    planned_component(component.controllable_component_id)
                        .generation &&
                readiness.value().command_eligible ==
                    (planned_component(component.controllable_component_id)
                         .state == core::ComponentLifecycleStateDTO::kActive),
            "native CCLC adoption command eligibility differs from the plan");
    facts.components.push_back(
        planned_component(component.controllable_component_id));
  }
  Require(ready_components.empty(),
          "native CCLC adoption contains an undeclared route component");
  std::sort(facts.components.begin(), facts.components.end(),
            [](const auto &a, const auto &b) {
              return a.component_id < b.component_id;
            });
  std::sort(facts.current_routes.begin(), facts.current_routes.end(),
            [](const auto &a, const auto &b) {
              return a.control_domain_id < b.control_domain_id;
            });
  return facts;
}
#endif

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
[[nodiscard]] RetainedNativeCellRuntime &RequireNativeCellRuntime(
    Composition &composition) {
  Require(composition.cell_runtime != nullptr,
          "native cell CCLC owner requires the stable cell runtime");
  return *composition.cell_runtime;
}

class RetainedNativeCellRoutePublicationOwner final
    : public deployment::IRouteConfigurationKernelPublicationOwner {
 public:
  explicit RetainedNativeCellRoutePublicationOwner(Composition &composition)
      : composition_(composition), cell_(RequireNativeCellRuntime(composition)),
        motion_route_owner_(composition.runtime.get()) {
    Require(composition_.cell_runtime != nullptr,
            "native cell CCLC route owner requires the stable cell runtime");
    const auto clock = cell_.clock();
    Require(clock != nullptr,
            "native cell CCLC route owner has no stable cell clock");
    const auto add_gripper = [&](Composition &view) {
      if (view.gripper_runtime == nullptr) return;
      const auto binding = view.gripper_runtime->binding();
      Require(binding.value().control_path.has_value(),
              "native gripper route owner has no exact control domain");
      const auto domain =
          binding.value().control_path->control_domain_id;
      auto owner = std::make_unique<RetainedNativeGripperRoutePublicationOwner>(
          view.gripper_runtime, *clock, cell_.addressed_kernel(),
          [this](const core::ActiveControlConfigurationDTO &configuration,
                 const core::RuntimeParticipantExecutionRegistration
                     &gripper_registration) {
            return BuildCellCandidate(configuration, gripper_registration);
          });
      Require(owners_.emplace(domain, std::move(owner)).second,
              "native cell repeats a gripper physical control domain");
    };
    add_gripper(composition_);
    for (const auto &participant : composition_.participant_compositions) {
      Require(participant != nullptr,
              "gripper-only cell has a missing component owner");
      add_gripper(*participant);
    }
    Require(!owners_.empty() || motion_route_owner_ != nullptr,
            "native cell CCLC route owner has no physical route owner");
  }

  [[nodiscard]] core::Result<
      std::vector<core::RuntimeParticipantBinding>>
  ReadCurrentPhysicalParticipants(
      const core::ActiveControlConfigurationDTO &configuration) const override {
    const auto owner = owners_.find(configuration.control_domain_id);
    if (owner == owners_.end()) {
      if (motion_route_owner_ != nullptr) {
        return motion_route_owner_->ReadCurrentPhysicalParticipants(
            configuration);
      }
      return core::Result<
          std::vector<core::RuntimeParticipantBinding>>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kMissingReference)),
          "native cell has no physical owner for the requested domain",
          {{"reason", "native_gripper_route_domain_missing"},
           {"control_domain_id", configuration.control_domain_id.value()}}});
    }
    return owner->second->ReadCurrentPhysicalParticipants(configuration);
  }

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  AdoptAndReadBackRouteConfigurationPredecessor(
      const core::ActiveControlConfigurationDTO &configuration) {
    std::lock_guard publication_lock(publication_mutex_);
    pending_candidate_.reset();
    pending_stop_sources_.reset();
    const auto gripper = owners_.find(configuration.control_domain_id);
    if (gripper == owners_.end()) {
      if (motion_route_owner_ != nullptr)
        return motion_route_owner_
            ->AdoptAndReadBackRouteConfigurationPredecessor(configuration);
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kMissingReference)),
              "native cell has no predecessor route owner for the domain",
              {{"reason", "native_cell_predecessor_domain_missing"},
               {"control_domain_id", configuration.control_domain_id.value()}}});
    }
    auto adopted = gripper->second
                       ->AdoptAndReadBackRouteConfigurationPredecessor(
                           configuration);
    if (!adopted.ok()) {
      pending_candidate_.reset();
      pending_stop_sources_.reset();
      return adopted;
    }
    if (!pending_candidate_.has_value() || !pending_stop_sources_.has_value())
      return FailClosedAfterSurveillanceFailure(
          configuration, "native_gripper_predecessor_candidate_missing",
          "adopted gripper predecessor has no complete cell snapshot");
    cell_.AdoptVerifiedComponentReadinessSnapshot(
        std::move(pending_candidate_->registrations),
        std::move(pending_candidate_->participant_resolver));
    pending_candidate_.reset();
    const auto observer = cell_.stop_surveillance_owner();
    if (observer == nullptr) {
      pending_stop_sources_.reset();
      return FailClosedAfterSurveillanceFailure(
          configuration, "native_gripper_predecessor_stop_owner_missing",
          "adopted gripper predecessor has no stable Stop observer");
    }
    auto replaced = observer->ReplaceSources(std::move(*pending_stop_sources_));
    pending_stop_sources_.reset();
    if (!replaced.ok())
      return FailClosedAfterSurveillanceFailure(
          configuration, "native_gripper_predecessor_stop_source_failed",
          replaced.error().message);
    return adopted;
  }

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  PublishAndReadBackRouteConfiguration(
      const core::ActiveControlConfigurationDTO &configuration,
      std::vector<deployment::VerifiedRouteReadinessParticipant> participants)
      override {
    std::lock_guard publication_lock(publication_mutex_);
    pending_candidate_.reset();
    const auto owner = owners_.find(configuration.control_domain_id);
    if (owner == owners_.end()) {
      if (motion_route_owner_ != nullptr) {
        pending_candidate_.reset();
        pending_stop_sources_.reset();
        return motion_route_owner_->PublishAndReadBackRouteConfiguration(
            configuration, std::move(participants));
      }
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kMissingReference)),
              "gripper-only cell has no physical owner for the requested domain",
              {{"reason", "native_gripper_route_domain_missing"},
               {"control_domain_id", configuration.control_domain_id.value()}}});
    }
    auto published = owner->second->PublishAndReadBackRouteConfiguration(
        configuration, std::move(participants));
    if (!published.ok()) {
      pending_candidate_.reset();
      pending_stop_sources_.reset();
      return published;
    }
    if (configuration.state ==
        core::ActiveControlConfigurationStateDTO::kReconciled) {
      if (!pending_candidate_.has_value() ||
          !pending_stop_sources_.has_value()) {
        return FailClosedAfterSurveillanceFailure(
            configuration, "native_gripper_cell_candidate_missing",
            "verified gripper route publication has no complete cell snapshot");
      }
      // CCLC holds the exact route-configuration fence around this callback.
      // The route owner has verified Kernel readback; adopt the same full-cell
      // registration/resolver snapshot and replace surveillance before the
      // coordinator can release that fence.
      cell_.AdoptVerifiedComponentReadinessSnapshot(
          std::move(pending_candidate_->registrations),
          std::move(pending_candidate_->participant_resolver));
      pending_candidate_.reset();
      const auto observer = cell_.stop_surveillance_owner();
      if (observer == nullptr) {
        pending_stop_sources_.reset();
        return FailClosedAfterSurveillanceFailure(
            configuration, "native_gripper_cell_stop_owner_missing",
            "stable cell Stop-surveillance owner is unavailable");
      }
      auto sources = observer->ReplaceSources(
          std::move(*pending_stop_sources_));
      pending_stop_sources_.reset();
      if (!sources.ok()) {
        return FailClosedAfterSurveillanceFailure(
            configuration, "native_gripper_cell_stop_source_publish_failed",
            sources.error().message);
      }
    }
    return published;
  }

 private:
  using KernelCandidate =
      RetainedNativeGripperRoutePublicationOwner::KernelCandidate;

  [[nodiscard]] core::Result<KernelCandidate> BuildCellCandidate(
      const core::ActiveControlConfigurationDTO &,
      const core::RuntimeParticipantExecutionRegistration
          &gripper_registration) {
    try {
      auto registrations = NativeCellComponentRegistrations(composition_);
      const auto exact_gripper_count = std::count_if(
          registrations.begin(), registrations.end(), [&](const auto &row) {
            return row.binding.value() ==
                   gripper_registration.binding.value();
          });
      if (exact_gripper_count != 1U) {
        return core::Result<KernelCandidate>::Err(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
            "full-cell registration set does not contain the exact gripper once",
            {{"reason", "native_gripper_cell_registration_mismatch"}}});
      }
      auto resolver_factory =
          composition_.candidate_participant_resolver_factory;
      if (!resolver_factory && composition_.runtime != nullptr) {
        resolver_factory = composition_.runtime->participant_resolver_factory;
      }
      if (!resolver_factory) {
        return core::Result<KernelCandidate>::Err(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kMissingReference)),
            "managed native cell has no exact resolver factory for its current routes",
            {{"reason", "native_cell_resolver_factory_missing"}}});
      }
      auto physical_bindings = PhysicalControlBindings(composition_);
      for (const auto &physical_binding : physical_bindings) {
        const auto matches = std::count_if(
            registrations.begin(), registrations.end(), [&](const auto &row) {
              return row.binding.value() == physical_binding.value();
            });
        if (matches != 1U) {
          return core::Result<KernelCandidate>::Err(core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
              "full-cell physical binding has no unique staged registration",
              {{"reason", "native_cell_physical_registration_mismatch"},
               {"controllable_component_id",
                physical_binding.value().controllable_component_id.value()}}});
        }
      }
      auto binding_resolver = resolver_factory(std::move(physical_bindings));
      if (!binding_resolver) {
        return core::Result<KernelCandidate>::Err(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kMissingReference)),
            "managed native cell resolver factory returned no resolver",
            {{"reason", "native_cell_resolver_missing"}}});
      }
      auto resolver = make_retained_cell_kernel_participant_resolver(
          std::move(binding_resolver), registrations);
      auto stop_sources = NativeCellStopSources(composition_);
      const auto stop_sources_valid =
          RetainedNativeCellRuntime::ValidateStopSourceCoverage(
              stop_sources,
              RetainedStopSurveillanceEmptySourceMode::kRequireNonEmpty,
              registrations);
      if (!stop_sources_valid.ok()) {
        return core::Result<KernelCandidate>::Err(
            stop_sources_valid.error());
      }
      KernelCandidate candidate{std::move(registrations), std::move(resolver)};
      pending_candidate_ = candidate;
      pending_stop_sources_ = std::move(stop_sources);
      return core::Result<KernelCandidate>::Ok(std::move(candidate));
    } catch (const std::exception &exception) {
      return core::Result<KernelCandidate>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
          exception.what(),
          {{"reason", "native_gripper_cell_candidate_failed"}}});
    }
  }

  [[nodiscard]] core::Result<core::RuntimeRouteConfigurationReadbackDTO>
  FailClosedAfterSurveillanceFailure(
      const core::ActiveControlConfigurationDTO &configuration,
      std::string reason, std::string message) {
    auto pending = cell_.addressed_kernel().SetRouteRecoveryPending(true);
    if (!pending.ok()) {
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
              std::move(message) + "; could not close cell admission: " +
                  pending.error().message,
              {{"reason", std::move(reason)}}});
    }
    auto closed_configuration = configuration;
    closed_configuration.state =
        core::ActiveControlConfigurationStateDTO::
            kExternalOrUnknownUnreconciled;
    auto closed = cell_.addressed_kernel().publish_runtime_route_configuration(
        std::move(closed_configuration));
    if (!closed.ok()) {
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
              std::move(message) + "; could not publish a closed route: " +
                  closed.error().message,
              {{"reason", std::move(reason)}}});
    }
    const auto readback =
        cell_.addressed_kernel().runtime_route_configuration_readback(
            configuration.control_domain_id,
            configuration.active_control_configuration_id,
            configuration.active_control_configuration_generation);
    if (!readback.ok() ||
        readback.value().active_configuration.state !=
            core::ActiveControlConfigurationStateDTO::
                kExternalOrUnknownUnreconciled ||
        readback.value().operation_admission_open ||
        !readback.value().readiness_rows.empty()) {
      const auto detail = readback.ok()
                              ? "Kernel closed-route readback is inconsistent"
                              : readback.error().message;
      return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
              std::move(message) + "; fail-closed readback failed: " + detail,
              {{"reason", std::move(reason)}}});
    }
    return core::Result<core::RuntimeRouteConfigurationReadbackDTO>::Err(
        core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
            std::move(message), {{"reason", std::move(reason)}}});
  }

  Composition &composition_;
  RetainedNativeCellRuntime &cell_;
  RetainedNativeRuntime *motion_route_owner_{};
  std::map<core::ControlDomainId,
           std::unique_ptr<RetainedNativeGripperRoutePublicationOwner>>
      owners_;
  std::mutex publication_mutex_;
  std::optional<KernelCandidate> pending_candidate_;
  std::optional<std::vector<RetainedStopSurveillanceSourceInput>>
      pending_stop_sources_;
};

[[nodiscard]] std::unique_ptr<RetainedNativeCellCommandReadinessPort>
MakeNativeCellReadinessPort(Composition &composition) {
  Require(composition.cell_runtime != nullptr,
          "managed native readiness owner requires the stable cell runtime");
  const auto readiness_builder =
      [&composition](
          const core::ComponentCommandReadinessPublicationDTO &publication,
          const std::vector<core::RuntimeParticipantExecutionRegistration>
              &current)
          -> core::Result<RetainedNativeCellReadinessCandidate> {
    auto registrations = current;
    std::size_t matched = 0U;
    for (auto &registration : registrations) {
      if (registration.binding.value().controllable_component_id !=
          publication.controllable_component_id) {
        continue;
      }
      ++matched;
      if (!registration.stop_readiness.has_value()) {
        // An adopted predecessor route is deliberately command-closed and
        // carries no current Stop readiness. Its durable generation may still
        // be reconstructed; CCLC will re-issue Stop readiness with the
        // reconciled successor route before eligibility can open.
        if (!publication.command_eligible)
          continue;
        return core::Result<RetainedNativeCellReadinessCandidate>::Err(
            core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kMissingReference)),
                "component route has no current Stop readiness",
                {{"reason", "component_readiness_route_not_current"},
                 {"controllable_component_id",
                  publication.controllable_component_id.value()}}});
      }
      if (registration.stop_readiness->value()
              .component_lifecycle_generation ==
          publication.component_lifecycle_generation) {
        continue;
      }
      auto resealed = core::rebind_runtime_participant_stop_readiness_generation(
          *registration.stop_readiness,
          publication.component_lifecycle_generation);
      if (!resealed.ok()) {
        return core::Result<RetainedNativeCellReadinessCandidate>::Err(
            resealed.error());
      }
      registration.stop_readiness = std::move(resealed.value());
    }
    if (matched == 0U) {
      return core::Result<RetainedNativeCellReadinessCandidate>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kMissingReference)),
              "component readiness candidate has no cell registration",
              {{"reason", "native_cell_readiness_component_missing"},
               {"controllable_component_id",
                publication.controllable_component_id.value()}}});
    }
    auto resolver_factory = composition.candidate_participant_resolver_factory;
    if (!resolver_factory && composition.runtime != nullptr) {
      resolver_factory = composition.runtime->participant_resolver_factory;
    }
    if (!resolver_factory) {
      return core::Result<RetainedNativeCellReadinessCandidate>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kMissingReference)),
              "managed native cell has no exact resolver factory",
              {{"reason", "native_cell_resolver_factory_missing"}}});
    }
    auto physical_bindings = PhysicalControlBindings(composition);
    for (const auto &physical_binding : physical_bindings) {
      const auto matches = std::count_if(
          registrations.begin(), registrations.end(), [&](const auto &row) {
            return row.binding.value() == physical_binding.value();
          });
      if (matches != 1U) {
        return core::Result<RetainedNativeCellReadinessCandidate>::Err(
            core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
                "full-cell physical binding has no unique staged registration",
                {{"reason", "native_cell_physical_registration_mismatch"},
                 {"controllable_component_id",
                  physical_binding.value().controllable_component_id.value()}}});
      }
    }
    auto binding_resolver = resolver_factory(std::move(physical_bindings));
    if (!binding_resolver) {
      return core::Result<RetainedNativeCellReadinessCandidate>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kMissingReference)),
              "managed native cell resolver factory returned no resolver",
              {{"reason", "native_cell_resolver_missing"}}});
    }
    auto resolver = make_retained_cell_kernel_participant_resolver(
        std::move(binding_resolver), registrations);
    using RegistrationRows =
        std::vector<core::RuntimeParticipantExecutionRegistration>;
    std::vector<std::pair<std::shared_ptr<RetainedNativeRuntime>,
                          RegistrationRows>> motion_caches;
    std::vector<std::pair<
        std::shared_ptr<RetainedNativeGripperRuntime>,
        std::optional<core::RuntimeParticipantStopReadiness>>> gripper_caches;
    const auto stage_owner = [&](const Composition &owner) -> core::Result<void> {
      if (owner.runtime != nullptr) {
        auto rows = owner.runtime->participant_registrations;
        bool changed = false;
        for (auto &row : rows) {
          if (row.binding.value().controllable_component_id !=
              publication.controllable_component_id)
            continue;
          const auto exact = std::find_if(
              registrations.begin(), registrations.end(),
              [&](const auto &candidate) {
                return candidate.binding.value() == row.binding.value();
              });
          if (exact == registrations.end()) {
            return core::Result<void>::Err(core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
                "retained motion owner has no exact staged registration",
                {{"reason", "native_cell_owner_registration_mismatch"},
                 {"controllable_component_id",
                  publication.controllable_component_id.value()}}});
          }
          row.stop_readiness = exact->stop_readiness;
          changed = true;
        }
        if (changed)
          motion_caches.emplace_back(owner.runtime, std::move(rows));
      }
      if (owner.gripper_runtime != nullptr &&
          owner.controllable_component_id ==
              publication.controllable_component_id) {
        const auto binding = owner.gripper_runtime->binding();
        const auto exact = std::find_if(
            registrations.begin(), registrations.end(),
            [&](const auto &candidate) {
              return candidate.binding.value() == binding.value();
            });
        if (exact == registrations.end() ||
            (!exact->stop_readiness.has_value() &&
             publication.command_eligible)) {
          return core::Result<void>::Err(core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
              "retained gripper owner has no exact staged Stop readiness",
              {{"reason", "native_cell_owner_readiness_mismatch"},
               {"controllable_component_id",
                publication.controllable_component_id.value()}}});
        }
        gripper_caches.emplace_back(owner.gripper_runtime,
                                    exact->stop_readiness);
      }
      return core::Result<void>::Ok();
    };
    auto staged_owner = stage_owner(composition);
    if (!staged_owner.ok())
      return core::Result<RetainedNativeCellReadinessCandidate>::Err(
          staged_owner.error());
    for (const auto &participant : composition.participant_compositions) {
      Require(participant != nullptr,
              "native cell readiness has a missing component owner");
      staged_owner = stage_owner(*participant);
      if (!staged_owner.ok())
        return core::Result<RetainedNativeCellReadinessCandidate>::Err(
            staged_owner.error());
    }
    std::function<void()> adopt_component_owners =
        [motion_caches = std::move(motion_caches),
         gripper_caches = std::move(gripper_caches)]() mutable noexcept {
          for (auto &[runtime, rows] : motion_caches)
            runtime->participant_registrations.swap(rows);
          for (auto &[gripper, readiness] : gripper_caches)
            gripper->AdoptVerifiedReadiness(std::move(readiness));
        };
    return core::Result<RetainedNativeCellReadinessCandidate>::Ok(
        {std::move(registrations), std::move(resolver),
         std::move(adopt_component_owners)});
  };
  return std::make_unique<RetainedNativeCellCommandReadinessPort>(
      *composition.cell_runtime, readiness_builder);
}
#endif

void AttachRetainedNativeCCLC(Composition &composition,
                              const HostConfig &config,
                              ManagedKernelApplyFenceOwner &fence_owner,
                              const std::optional<core::ConfigurationFenceStatusQueryDTO>
                                  &parent_apply = std::nullopt) {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  std::shared_ptr<WallClock> cell_clock;
  std::shared_ptr<SimpleScheduler> cell_scheduler;
#endif
  core::IClock *owner_clock = nullptr;
  core::IScheduler *owner_scheduler = nullptr;
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  Require(composition.runtime != nullptr &&
              composition.runtime->clock != nullptr &&
              composition.runtime->scheduler != nullptr,
          "native CCLC attachment requires one stable addressed runtime");
  owner_clock = composition.runtime->clock.get();
  owner_scheduler = composition.runtime->scheduler.get();
#else
  if (composition.runtime != nullptr) {
    Require(composition.runtime->clock != nullptr &&
                composition.runtime->scheduler != nullptr,
            "native CCLC attachment requires stable time services");
    owner_clock = composition.runtime->clock.get();
    owner_scheduler = composition.runtime->scheduler.get();
  } else {
    Require(composition.gripper_runtime != nullptr &&
                composition.cell_runtime != nullptr,
            "gripper-only CCLC attachment requires the stable cell owner");
    cell_clock = composition.cell_runtime->clock();
    cell_scheduler = composition.cell_runtime->scheduler();
    Require(cell_clock != nullptr && cell_scheduler != nullptr,
            "gripper-only CCLC attachment has no stable time services");
    owner_clock = cell_clock.get();
    owner_scheduler = cell_scheduler.get();
  }
#endif
  Require(composition.active_addressed_kernel() != nullptr &&
              owner_clock != nullptr && owner_scheduler != nullptr,
          "native CCLC attachment requires one stable addressed Kernel");

  auto &kernel = *composition.active_addressed_kernel();
  if (composition.cclc_runtime) {
    Require(composition.cclc_attached.load(std::memory_order_acquire),
            "constructed native CCLC is not attached");
    return;
  }
  Require(kernel.SetRouteRecoveryPending(true).ok(),
          "route recovery could not retain closed admission");
  composition.route_recovery_pending.store(true, std::memory_order_release);

  auto active_publication_reader =
      std::make_unique<component_manager::SqliteActiveBundlePublicationStore>(
          config.manager_state_root /
              std::string(
                  component_manager::kActiveBundlePublicationStoreFilename),
          core::CellId(config.cell_id));
  const auto integrity = active_publication_reader->VerifyIntegrity();
  Require(integrity.current_generation == composition.publication_generation,
          "native CCLC active publication changed during composition");
  auto active_publication =
      active_publication_reader->ReadActiveBundlePublication(
          integrity.current_generation);
  Require(
      active_publication.ok() && active_publication.value().valid() &&
          active_publication.value().request.candidate_retained_bundle_id ==
              composition.bundle->retained_bundle_id() &&
          active_publication.value()
                  .request.candidate_projection_revision.retained_bundle_id ==
              composition.projection_revision.retained_bundle_id &&
          active_publication.value()
                  .request.candidate_projection_revision.graph_revision ==
              composition.projection_revision.graph_revision &&
          active_publication.value()
                  .request.candidate_projection_revision.scene_revision
                  .scene_id ==
              composition.projection_revision.scene_revision.scene_id &&
          active_publication.value().request.candidate_graph_revision ==
              composition.bundle->value().cell_graph.graph_revision,
      "native CCLC cannot re-read the exact active product");

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  bool parent_apply_facts_adopted = false;
  if (parent_apply.has_value()) {
    fence_owner.RunWithExactHeldSuccessor(
        *parent_apply, active_publication.value(), [&] {
          core::SqliteCCLCJournal journal(
              config.execution_state_root /
                  std::string(core::kCCLCJournalFilename),
              core::CellId(config.cell_id));
          const auto plan = journal.LoadNativeApplyPlan(
              {parent_apply->owner_transaction.apply_id,
               parent_apply->owner_transaction.apply_generation});
          if (!plan.ok() || !plan.value().has_value()) {
            Fail("native CCLC has no recorded plan for this Apply");
          }
          auto facts = ReadNativeApplyFacts(composition,
                                            active_publication.value(),
                                            *plan.value());
          const auto adopted = journal.AdoptNativeApplyFacts(facts);
          if (!adopted.ok()) {
            Fail("native CCLC could not adopt exact Apply facts: " +
                 adopted.error().message);
          }
          journal.Close();
        });
    parent_apply_facts_adopted = true;
  }
#endif

  auto artifact_source = std::make_unique<
      deployment::FilesystemVerifiedRetainedProductArtifactSource>(
      config.package_root);
  auto provider_manifest_source = std::make_unique<
      deployment::FilesystemVerifiedRetainedProviderManifestSource>(
      config.package_root);
  auto physical_bindings = PhysicalControlBindings(composition);
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  Require(composition.ros2_control_runtime_handle != nullptr &&
              !composition.ros2_control_binding_contract_generation_id.empty(),
          "retained Isaac ROS lifecycle has no bound execution adapter");
  auto lifecycle_product = RetainedRos2ControlLifecycleProduct::Create(
      composition.bundle, *provider_manifest_source, physical_bindings,
      composition.ros2_control_runtime_handle, *owner_clock,
      1'000'000'000LL);
#else
  core::SqliteCCLCJournal native_state_journal(
      config.execution_state_root /
          std::string(core::kCCLCJournalFilename),
      core::CellId(config.cell_id));
  auto lifecycle_product = RetainedNativeLifecycleProduct::Create(
      composition.bundle, *provider_manifest_source, physical_bindings,
      *owner_clock, 1'000'000'000LL,
      [&composition](const core::ControlDomainId &domain) {
        return VerifyNativeEngineForDomain(composition, domain);
      },
      [&composition](const core::ControlDomainId &domain)
          -> core::Result<bool> {
        const auto current = composition.active_addressed_kernel()
                                 ->current_runtime_route_configuration(domain);
        if (!current.ok()) return core::Result<bool>::Err(current.error());
        if (!current.value().has_value() ||
            !current.value()->operation_admission_open ||
            current.value()->active_configuration.state !=
                core::ActiveControlConfigurationStateDTO::kReconciled) {
          return core::Result<bool>::Ok(false);
        }
        const auto desired = std::find_if(
            composition.bundle->value().desired_control_configurations.begin(),
            composition.bundle->value().desired_control_configurations.end(),
            [&](const auto &row) { return row.control_domain_id == domain; });
        return core::Result<bool>::Ok(
            desired != composition.bundle->value()
                           .desired_control_configurations.end() &&
            current.value()->active_configuration.control_domain_revision ==
                desired->control_domain_revision &&
            current.value()->active_configuration.activation_set_id ==
                desired->activation_set_id &&
            current.value()->active_configuration.controller_topology_id ==
                desired->controller_topology_id &&
            current.value()->active_configuration.topology_revision ==
                desired->topology_revision &&
            current.value()->active_configuration.binding_routes.size() ==
                desired->binding_routes.size() &&
            std::all_of(desired->binding_routes.begin(),
                        desired->binding_routes.end(), [&](const auto &route) {
                          return std::any_of(
                              current.value()->active_configuration
                                  .binding_routes.begin(),
                              current.value()->active_configuration
                                  .binding_routes.end(), [&](const auto &active) {
                                return active.control_binding_id ==
                                           route.control_binding_id &&
                                       active.control_binding_revision ==
                                           route.control_binding_revision &&
                                       active.control_route_id ==
                                           route.control_route_id;
                              });
                        }) &&
            current.value()->readiness_rows.size() ==
                desired->binding_routes.size());
      },
      [&native_state_journal](const core::ControllableComponentId &component)
          -> core::Result<std::optional<core::ComponentLifecycleStateDTO>> {
        const auto retained =
            native_state_journal.LoadCurrentComponentLifecycleFact(component);
        if (!retained.ok()) {
          return core::Result<
              std::optional<core::ComponentLifecycleStateDTO>>::Err(
              retained.error());
        }
        return core::Result<
            std::optional<core::ComponentLifecycleStateDTO>>::Ok(
            retained.value().current_state);
      },
      !parent_apply.has_value() && composition.route_recovery_pending.load(
                                      std::memory_order_acquire));
  native_state_journal.Close();
#endif
  if (!lifecycle_product.ok()) {
    Fail("native lifecycle product cannot be composed: " +
         lifecycle_product.error().message);
  }

  deployment::CCLCRuntimeCompositionConfig cclc_config{
      config.execution_state_root,
      config.execution_run_root,
      core::CellId(config.cell_id),
      {core::RequestOwnerEpoch(RandomToken("cclc-owner-")), 1024U, 2U,
       10'000'000LL, 30'000'000'000LL}};
  auto cclc = [&]() -> core::Result<
      std::unique_ptr<deployment::CCLCRuntimeComposition>> {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    const bool has_independent_gripper =
        composition.gripper_runtime != nullptr ||
        std::any_of(composition.participant_compositions.begin(),
                    composition.participant_compositions.end(),
                    [](const auto &participant) {
                      return participant != nullptr &&
                             participant->gripper_runtime != nullptr;
                    });
    const bool motion_only_restart =
        composition.cell_runtime != nullptr && composition.runtime != nullptr &&
        !has_independent_gripper && !parent_apply.has_value();
    if (motion_only_restart) {
      // Keep the established motion-only recovery path. Mixed and gripper-only
      // cells use the stable cell owner's equivalent journal-recovery path.
      return CreateRetainedNativeCCLCRuntime(
          RetainedNativeCCLCRuntimeConfig{std::move(cclc_config),
                                          1'000'000'000LL},
          composition.bundle, *active_publication_reader,
          std::move(active_publication.value()), *artifact_source,
          lifecycle_product.value()->catalog_source(),
          lifecycle_product.value()->brand_ports(),
          CellControlBindingRepresentatives(composition), *composition.runtime,
          *composition.active_addressed_kernel(), fence_owner);
    }
    if (composition.cell_runtime != nullptr) {
      if (composition.cclc_cell_route_publication_owner == nullptr) {
        composition.cclc_cell_route_publication_owner =
            std::make_unique<RetainedNativeCellRoutePublicationOwner>(
                composition);
      }
      if (composition.cclc_cell_command_readiness_port == nullptr) {
        composition.cclc_cell_command_readiness_port =
            MakeNativeCellReadinessPort(composition);
      }
      if (!parent_apply.has_value()) {
        auto recovered = CreateRecoveredRetainedNativeCellCCLCRuntime(
            RetainedNativeCCLCRuntimeConfig{std::move(cclc_config),
                                            1'000'000'000LL},
            composition.bundle, *active_publication_reader,
            std::move(active_publication.value()), *artifact_source,
            lifecycle_product.value()->catalog_source(),
            lifecycle_product.value()->brand_ports(),
            CellControlBindingRepresentatives(composition),
            *composition.active_addressed_kernel(), fence_owner,
            *composition.cclc_cell_route_publication_owner,
            *composition.cclc_cell_command_readiness_port,
            *composition.active_addressed_kernel(), *owner_clock,
            *owner_scheduler,
            [&composition](const core::ActiveControlConfigurationDTO
                               &predecessor) {
              auto *owner = dynamic_cast<RetainedNativeCellRoutePublicationOwner *>(
                  composition.cclc_cell_route_publication_owner.get());
              Require(owner != nullptr,
                      "native cell CCLC route owner changed during recovery");
              return owner->AdoptAndReadBackRouteConfigurationPredecessor(
                  predecessor);
            },
            [&composition]() {
              return composition.cell_runtime->ReconcileCommandFreeOrphans();
            },
            composition.cclc_cell_route_reconciler);
        return recovered;
      }
      auto current_configuration_source =
          [&composition](const core::ControlDomainId &domain)
          -> core::Result<std::optional<
              core::ActiveControlConfigurationDTO>> {
        const auto current = composition.active_addressed_kernel()
                                 ->current_runtime_route_configuration(domain);
        if (!current.ok()) {
          return core::Result<std::optional<
              core::ActiveControlConfigurationDTO>>::Err(current.error());
        }
        if (!current.value().has_value()) {
          return core::Result<std::optional<
              core::ActiveControlConfigurationDTO>>::Ok(std::nullopt);
        }
        return core::Result<std::optional<
            core::ActiveControlConfigurationDTO>>::Ok(
            current.value()->active_configuration);
      };
      auto created = CreateRetainedNativeCCLCRuntime(
          RetainedNativeCCLCRuntimeConfig{std::move(cclc_config),
                                          1'000'000'000LL},
          composition.bundle, *active_publication_reader,
          std::move(active_publication.value()), *artifact_source,
          lifecycle_product.value()->catalog_source(),
          lifecycle_product.value()->brand_ports(),
          CellControlBindingRepresentatives(composition),
          *composition.active_addressed_kernel(), fence_owner,
          *composition.cclc_cell_route_publication_owner, *owner_clock,
          *owner_scheduler, std::move(current_configuration_source), {},
          composition.cclc_cell_command_readiness_port.get());
      if (!created.ok()) {
        return created;
      }
      auto started = created.value()->Start();
      if (!started.ok()) {
        return core::Result<
            std::unique_ptr<deployment::CCLCRuntimeComposition>>::Err(
            started.error());
      }
      return created;
    }
#endif
    return CreateRetainedNativeCCLCRuntime(
        RetainedNativeCCLCRuntimeConfig{std::move(cclc_config),
                                        1'000'000'000LL},
        composition.bundle, *active_publication_reader,
        std::move(active_publication.value()), *artifact_source,
        lifecycle_product.value()->catalog_source(),
        lifecycle_product.value()->brand_ports(),
        CellControlBindingRepresentatives(composition), *composition.runtime,
        *composition.active_addressed_kernel(), fence_owner
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
      ,
      composition.ros2_control_runtime_handle->lifecycle_snapshot_source()
#endif
    );
  }();
  if (!cclc.ok()) {
    Fail("native CCLC composition failed: " + cclc.error().message);
  }

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  if (composition.runtime != nullptr &&
      composition.runtime->reconcile_controller_routes) {
    const auto routes = composition.runtime->reconcile_controller_routes();
    if (parent_apply.has_value() && !routes.ok()) {
      composition.runtime->reconcile_controller_routes = {};
      composition.runtime->stop_recovery_configuration_source = {};
      Fail("native CCLC Apply routes did not settle: " +
           routes.error().message);
    }
    if (routes.ok()) {
      composition.route_recovery_pending.store(false,
                                               std::memory_order_release);
    }
  } else if (composition.cclc_cell_route_reconciler) {
    const auto routes = composition.cclc_cell_route_reconciler();
    if (routes.ok())
      composition.route_recovery_pending.store(false,
                                               std::memory_order_release);
  } else if (parent_apply_facts_adopted) {
    // The exact successor Apply facts were verified and adopted while its
    // Manager fence remained held. The routes/readiness were initialized by
    // the generic cell route owner before CCLC attachment.
    Require(kernel.SetRouteRecoveryPending(false).ok(),
            "native gripper Apply could not clear verified route recovery");
    composition.route_recovery_pending.store(false,
                                             std::memory_order_release);
  }
#endif

  composition.cclc_active_publication_reader =
      std::move(active_publication_reader);
  composition.cclc_artifact_source = std::move(artifact_source);
  composition.cclc_provider_manifest_source =
      std::move(provider_manifest_source);
  composition.cclc_lifecycle_product = std::move(lifecycle_product.value());
  composition.cclc_runtime = std::move(cclc.value());
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  const auto component_id = composition.direct_control_motion_binding->value()
                                .controllable_component_id;
  const auto runtime_epochs =
      composition.direct_control_motion_binding->value().runtime_unit_epochs;
  Require(runtime_epochs.size() == 1U,
          "retained Isaac ROS admission has no sole runtime epoch");
  auto admission = RetainedRos2ControlAdmissionOwner::Create(
      RetainedRos2ControlAdmissionOwnerConfig{
          component_id, runtime_epochs.front().runtime_epoch,
          composition.ros2_control_binding_contract_generation_id,
          Ros2ControlProviderComponentEpoch(composition),
          Ros2ControlCellSupervisorEpoch(composition), 50'000'000LL,
          250'000'000LL},
      composition.ros2_control_runtime_handle, *owner_clock,
      [&composition]() {
        return composition.cclc_runtime->ReadCurrentComponentSnapshot(
            composition.direct_control_motion_binding->value()
                .controllable_component_id);
      });
  if (!admission.ok()) {
    Fail("ROS execution admission owner cannot be composed: " +
         admission.error().message);
  }
  composition.ros2_control_admission_owner = std::move(admission.value());
#endif
  composition.cclc_attached.store(true, std::memory_order_release);
}

void DetachRetainedCCLC(Composition &composition) {
  composition.cclc_attached.store(false, std::memory_order_release);
  composition.route_recovery_pending.store(false, std::memory_order_release);
  composition.cclc_cell_route_reconciler = {};
  if (composition.runtime != nullptr) {
    composition.runtime->stop_recovery_configuration_source = {};
    composition.runtime->reconcile_controller_routes = {};
  }
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  composition.ros2_control_admission_owner.reset();
#endif
  composition.cclc_runtime.reset();
  composition.cclc_cell_command_readiness_port.reset();
  composition.cclc_cell_route_publication_owner.reset();
  composition.cclc_lifecycle_product.reset();
  composition.cclc_provider_manifest_source.reset();
  composition.cclc_artifact_source.reset();
  composition.cclc_active_publication_reader.reset();
}

struct ManagedBundleAtPinnedRoot final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::filesystem::path package_root;
};

/// Resolve an exact retained bundle from the two directory descriptors pinned
/// for this managed child. The selected package root has precedence; the
/// activation root is only the explicitly supplied fallback, never an
/// ambient filesystem search path.
[[nodiscard]] std::optional<ManagedBundleAtPinnedRoot>
LoadManagedBundleAtPinnedRoot(
    const ManagedRuntimeChildConfig &config,
    const core::RetainedBundleId &retained_bundle_id) {
  const std::array<std::filesystem::path, 2U> roots{
      config.package_root, config.program_artifact_root};
  for (std::size_t index = 0U; index < roots.size(); ++index) {
    const auto &root = roots[index];
    if (root.empty() || (index == 1U && root == roots.front())) continue;
    const deployment::FilesystemVerifiedRetainedBundleSource source(root);
    auto bundle = source.get_verified(retained_bundle_id);
    if (bundle != nullptr)
      return ManagedBundleAtPinnedRoot{std::move(bundle), root};
  }
  return std::nullopt;
}

[[nodiscard]] std::filesystem::path ManagedProgramArtifactRoot(
    const ManagedRuntimeChildConfig &config,
    const std::filesystem::path &selected_package_root) {
  return config.program_artifact_root == selected_package_root
             ? std::filesystem::path{}
             : config.program_artifact_root;
}

[[nodiscard]] int ManagedPackageRootDescriptor(
    const ManagedRuntimeChildConfig &config,
    const std::filesystem::path &selected_package_root) {
  if (selected_package_root == config.package_root) {
    return config.package_root_fd;
  }
  if (!config.program_artifact_root.empty() &&
      selected_package_root == config.program_artifact_root) {
    return config.program_artifact_root_fd;
  }
  Fail("managed candidate package root is not one of the pinned descriptors");
}

[[nodiscard]] bool AttachCurrentPublishedCCLCIfReady(
    Composition &composition, const ManagedRuntimeChildConfig &config,
    ManagedKernelApplyFenceOwner &fence_owner,
    const std::optional<core::ConfigurationFenceStatusQueryDTO>
        &parent_apply = std::nullopt) {
  auto &kernel = *composition.active_addressed_kernel();
  if (composition.cclc_attached.load(std::memory_order_acquire))
    return true;
  Require(kernel.SetRouteRecoveryPending(true).ok(),
          "route attachment could not close admission");
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  publication_store.Close();
  if (integrity.current_generation != composition.publication_generation) {
    return false;
  }
  Require(publication.ok() && publication.value().valid(),
          "native CCLC attachment cannot verify current publication");
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  if (!parent_apply.has_value() &&
      std::holds_alternative<core::NamedApplyPublicationTransactionDTO>(
          publication.value().request.transaction_ref)) {
    core::SqliteCCLCJournal journal(
        config.execution_state_root /
            std::string(core::kCCLCJournalFilename),
        core::CellId(config.cell_id));
    const auto adopted = journal.HasNativeApplyFacts(publication.value());
    if (!adopted.ok()) {
      Fail("native CCLC adoption verification failed: " +
           adopted.error().message);
    }
    if (!adopted.value()) return false;
  }
#endif
  const auto active_bundle = LoadManagedBundleAtPinnedRoot(
      config, publication.value().request.candidate_retained_bundle_id);
  Require(active_bundle.has_value(),
          "native CCLC attachment cannot load its published retained bundle");
  HostConfig attachment_config;
  attachment_config.package_root = active_bundle->package_root;
  attachment_config.manager_state_root = config.manager_state_root;
  attachment_config.execution_state_root = config.execution_state_root;
  attachment_config.execution_run_root = config.execution_run_root;
  attachment_config.candidate_worker = config.candidate_worker;
  attachment_config.cell_id = config.cell_id;
  AttachRetainedNativeCCLC(composition, attachment_config, fence_owner,
                           parent_apply);
  return composition.cclc_attached.load(std::memory_order_acquire);
}

[[nodiscard]] std::optional<CompositionBuildInput>
TryBuildCurrentManagedCompositionInput(
    const ManagedRuntimeChildConfig &config,
    const ManagedProjectionRegistry &projections,
    ManagedKernelAdmissionGate &admission_gate,
    bool &predecessor_composition_pending) {
  predecessor_composition_pending = false;
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  if (!integrity.current_generation.nonzero()) {
    publication_store.Close();
    return std::nullopt;
  }
  const auto publication = publication_store.ReadActiveBundlePublication(
      integrity.current_generation);
  Require(publication.ok() && publication.value().valid(),
          "managed active retained-bundle publication cannot be verified");
  const auto receipt = publication.value();
  publication_store.Close();

  // Verify the published bundle before deciding which projection providers
  // this runtime unit actually requires. A gripper-only unit may require no
  // native planner projection at all.
  auto selected_bundle = LoadManagedBundleAtPinnedRoot(
      config, receipt.request.candidate_retained_bundle_id);
  if (!selected_bundle.has_value()) {
    Fail("managed active publication names an unavailable retained bundle: " +
         receipt.request.candidate_retained_bundle_id.value());
  }
  auto bundle = std::move(selected_bundle->bundle);
  const auto &selected_package_root = selected_bundle->package_root;
  const auto &published_projection_revision =
      receipt.request.candidate_projection_revision;
  if (bundle->value().cell_graph.cell_id != core::CellId(config.cell_id) ||
      bundle->value().cell_graph.graph_revision !=
          receipt.request.candidate_graph_revision ||
      bundle->retained_bundle_id() !=
          published_projection_revision.retained_bundle_id ||
      bundle->value().cell_graph.graph_revision !=
          published_projection_revision.graph_revision) {
    Fail("managed active publication and retained bundle disagree: "
         "publication_bundle=" +
         receipt.request.candidate_retained_bundle_id.value() +
         ", publication_graph=" +
         receipt.request.candidate_graph_revision.value() +
         ", projection_bundle=" +
         published_projection_revision.retained_bundle_id.value() +
         ", projection_graph=" +
         published_projection_revision.graph_revision.value() +
         ", loaded_bundle=" + bundle->retained_bundle_id().value() +
         ", loaded_graph=" +
         bundle->value().cell_graph.graph_revision.value());
  }
  if (bundle->value().cell_graph.controllable_components.empty()) {
    return std::nullopt;
  }
  if (!projections.AllRequiredProjectionsHealthy(
          *bundle, receipt.request.candidate_projection_revision)) {
    predecessor_composition_pending = true;
    return std::nullopt;
  }

  auto component_projections = SelectManagedComponentProjections(
      config, *bundle, projections,
      receipt.request.candidate_projection_revision, true);
  if (!component_projections.has_value()) {
    predecessor_composition_pending = true;
    return std::nullopt;
  }
  const auto projection_revision = component_projections->projection_revision;
  auto projection = component_projections->by_component.empty()
                        ? nullptr
                        : component_projections->by_component.begin()->second;
  // The model map is still mandatory when no motion planner is present: it
  // seals the gripper's component/target identity and any authored frames.
  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "managed native composition requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      selected_package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "managed active retained model map is absent or invalid");
  auto collision_model = projection != nullptr
                             ? projection->collision_shared_model()
                             : collision::ManyForgeCollisionEngine::SharedModelPtr{};
  Require(projection == nullptr || collision_model != nullptr,
          "managed active projection has no collision model");

  auto planner_scene_projection = projection;
  return CompositionBuildInput{
      selected_package_root,
      config.execution_state_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
      core::RuntimeUnitId(config.runtime_unit_id),
      core::RuntimeEpoch(config.runtime_epoch),
      std::move(bundle),
      std::move(*model_map),
      projection_revision,
      receipt.publication_generation,
      std::move(projection),
      std::move(collision_model),
      {},
      {},
      &admission_gate,
      {},
      {},
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      true,
      {},
      {},
      config.manager_state_root,
      config.manager_run_root,
      config.logical_scene_database,
      config.program_cycle_journal,
      std::move(planner_scene_projection),
      {},
      {},
      std::move(component_projections->by_component),
      ManagedProgramArtifactRoot(config, selected_package_root),
      {},
      {},
      ManagedPackageRootDescriptor(config, selected_package_root),
  };
}

[[nodiscard]] bool CurrentPublicationIsInstalledSuccessor(
    const ManagedRuntimeChildConfig &config,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  const auto predecessor_generation =
      request.expected_predecessor_publication_generation.value();
  Require(predecessor_generation != std::numeric_limits<std::uint64_t>::max(),
          "managed composition publication generation is exhausted");
  const core::ActiveBundlePublicationGeneration successor_generation(
      predecessor_generation + 1U);
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  if (integrity.current_generation ==
      request.expected_predecessor_publication_generation) {
    publication_store.Close();
    return false;
  }
  Require(integrity.current_generation == successor_generation,
          "managed composition public pointer is neither predecessor nor "
          "exact successor");
  const auto publication =
      publication_store.ReadActiveBundlePublication(successor_generation);
  publication_store.Close();
  Require(publication.ok() && publication.value().valid(),
          "managed composition successor publication cannot be verified");
  const auto *transaction =
      std::get_if<core::NamedApplyPublicationTransactionDTO>(
          &publication.value().request.transaction_ref);
  Require(transaction != nullptr && transaction->apply_id == request.apply_id &&
              transaction->apply_generation == request.apply_generation &&
              publication.value().request.candidate_projection_revision ==
                  request.candidate_projection_revision &&
              publication.value()
                      .request.expected_previous_publication_generation ==
                  std::optional<core::ActiveBundlePublicationGeneration>{
                      request.expected_predecessor_publication_generation},
          "managed composition successor belongs to another Apply");
  return true;
}

[[nodiscard]] std::optional<ManagedBundleAtPinnedRoot>
LoadExactManagedCandidateBundle(
    const ManagedRuntimeChildConfig &config,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  Require(request.valid() &&
              request.runtime_unit_id.value() == config.runtime_unit_id &&
              request.runtime_epoch.value() == config.runtime_epoch,
          "managed composition request names another runtime child");
  auto selected = LoadManagedBundleAtPinnedRoot(
      config, request.candidate_projection_revision.retained_bundle_id);
  const auto bundle = selected.has_value() ? selected->bundle : nullptr;
  const auto canonical_bytes =
      bundle != nullptr ? bundle->canonical_bytes() : std::string_view{};
  Require(bundle != nullptr &&
              bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              bundle->value().cell_graph.graph_revision ==
                  request.candidate_projection_revision.graph_revision &&
              bundle->retained_bundle_id() ==
                  request.candidate_projection_revision.retained_bundle_id &&
              !canonical_bytes.empty() &&
              canonical_bytes.size() ==
                  request.retained_bundle_canonical_byte_size &&
              core::ContentDigest(
                  content_identity::sha256_digest(canonical_bytes)) ==
                  request.retained_bundle_canonical_content_digest,
          "managed selected retained bundle identity or materialization "
          "differs");
  return selected;
}

struct ManagedBootstrapStopProduct final {
  std::optional<core::StopDeadlineBudgetDTO> deadline_budget;
  std::vector<core::ResourceId> resource_inventory;
  bool requires_physical_composition{};
};

[[nodiscard]] ManagedBootstrapStopProduct LoadManagedBootstrapStopProduct(
    const ManagedRuntimeChildConfig &config,
    const core::RetainedBundleId &retained_bundle_id,
    const core::GraphRevision &graph_revision) {
  const auto selected =
      LoadManagedBundleAtPinnedRoot(config, retained_bundle_id);
  Require(selected.has_value() && selected->bundle != nullptr &&
              selected->bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              selected->bundle->retained_bundle_id() == retained_bundle_id &&
              selected->bundle->value().cell_graph.graph_revision ==
                  graph_revision,
          "managed bootstrap Stop policy differs from the retained product");

  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      selected->package_root);
  std::set<core::ResourceId> resources;
  for (const auto &binding :
       selected->bundle->value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id.value() != config.runtime_unit_id)
      continue;
    for (const auto &resource : binding.resource_bindings)
      resources.insert(resource.resource_id);
  }
  Require(resources.size() <= core::kMaxKernelResourceSlots,
          "managed bootstrap resource inventory exceeds the Kernel bound");
  return {
      deployment::resolve_retained_stop_deadline_budget(*selected->bundle,
                                                        catalog_store),
      {resources.begin(), resources.end()},
      !selected->bundle->value().cell_graph.controllable_components.empty()};
}

[[nodiscard]] std::optional<CompositionBuildInput> BuildManagedCompositionInput(
    const ManagedRuntimeChildConfig &config,
    const ManagedProjectionRegistry &projections,
    ManagedKernelAdmissionGate &admission_gate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    std::shared_ptr<WallClock> shared_clock,
    std::shared_ptr<SimpleScheduler> shared_scheduler) {
  auto selected_candidate = LoadExactManagedCandidateBundle(config, request);
  Require(selected_candidate.has_value(),
          "managed selected retained bundle is unavailable from pinned roots");
  auto bundle = std::move(selected_candidate->bundle);
  const auto &candidate_package_root = selected_candidate->package_root;
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  Require(integrity.current_generation ==
                  request.expected_predecessor_publication_generation &&
              integrity.current_pointer_digest ==
                  request.expected_predecessor_public_pointer_digest.value(),
          "managed composition predecessor publication changed before "
          "installation");
  const auto publication = publication_store.ReadActiveBundlePublication(
      request.expected_predecessor_publication_generation);
  Require(publication.ok() && publication.value().valid() &&
              publication.value().publication_generation ==
                  request.expected_predecessor_publication_generation &&
              publication.value().durable_public_pointer_content_digest ==
                  request.expected_predecessor_public_pointer_digest,
          "managed predecessor publication cannot be verified");
  publication_store.Close();

  const auto predecessor_location = LoadManagedBundleAtPinnedRoot(
      config, request.predecessor_projection_revision.retained_bundle_id);
  const auto predecessor = predecessor_location.has_value()
                               ? predecessor_location->bundle
                               : nullptr;
  Require(predecessor != nullptr &&
              predecessor->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              predecessor->value().cell_graph.graph_revision ==
                  request.predecessor_projection_revision.graph_revision &&
              deployment::derive_apply_component_delta(*predecessor, *bundle) ==
                  request.component_delta,
          "managed component delta differs from the verified predecessor and candidate");

  if (bundle->value().cell_graph.controllable_components.empty()) {
    return std::nullopt;
  }

  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "managed native composition requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      candidate_package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "managed active retained model map is absent or invalid");

  auto component_projections = SelectManagedComponentProjections(
      config, *bundle, projections, request.candidate_projection_revision,
      false);
  Require(component_projections.has_value(),
          "managed selected robot projections are not active at the candidate "
          "revision");
  auto projection = component_projections->by_component.empty()
                        ? nullptr
                        : component_projections->by_component.begin()->second;
  auto collision_model = projection != nullptr
                             ? projection->collision_shared_model()
                             : collision::ManyForgeCollisionEngine::SharedModelPtr{};
  Require(projection == nullptr || collision_model != nullptr,
          "managed active projection has no collision model");

  const auto predecessor_generation =
      request.expected_predecessor_publication_generation.value();
  Require(predecessor_generation != std::numeric_limits<std::uint64_t>::max(),
          "managed composition publication generation is exhausted");
  auto planner_scene_projection = projection;
  return CompositionBuildInput{
      candidate_package_root,
      config.execution_state_root,
      config.execution_run_root,
      config.candidate_worker,
      core::CellId(config.cell_id),
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
      core::RuntimeUnitId(config.runtime_unit_id),
      core::RuntimeEpoch(config.runtime_epoch),
      std::move(bundle),
      std::move(*model_map),
      request.candidate_projection_revision,
      core::ActiveBundlePublicationGeneration(predecessor_generation + 1U),
      std::move(projection),
      std::move(collision_model),
      {},
      {},
      &admission_gate,
      {},
      {},
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      false,
      std::move(shared_clock),
      std::move(shared_scheduler),
      config.manager_state_root,
      config.manager_run_root,
      config.logical_scene_database,
      config.program_cycle_journal,
      std::move(planner_scene_projection),
      {},
      {},
      std::move(component_projections->by_component),
      ManagedProgramArtifactRoot(config, candidate_package_root),
      request.component_delta,
      {},
      ManagedPackageRootDescriptor(config, candidate_package_root),
  };
}

[[nodiscard]] bool IsExactRetainedNativeViewApply(
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  static_cast<void>(request);
  return false;
#else
  return !request.component_delta.components.empty() &&
         request.predecessor_projection_revision.retained_bundle_id ==
             request.candidate_projection_revision.retained_bundle_id &&
         request.predecessor_projection_revision.graph_revision ==
             request.candidate_projection_revision.graph_revision &&
         std::all_of(request.component_delta.components.begin(),
                     request.component_delta.components.end(),
                     [](const auto &row) {
                       return row.disposition ==
                              deployment::ApplyComponentDisposition::kRetain;
                     });
#endif
}

#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
void RefreshRetainedNativePublicationView(
    Composition &cell,
    const std::map<core::ControllableComponentId,
                   std::shared_ptr<RetainedPlannerSceneProjection>>
        &projections,
    const core::ProjectionRevisionDTO &revision,
    const core::ActiveBundlePublicationGeneration generation,
    const std::filesystem::path &manager_run_root) {
  std::map<core::ControllableComponentId, Composition *> components;
  components.emplace(cell.controllable_component_id, &cell);
  for (auto &peer : cell.participant_compositions) {
    Require(peer != nullptr &&
                components.emplace(peer->controllable_component_id,
                                   peer.get()).second,
            "retained native publication has duplicate component owners");
  }
  if (projections.empty()) {
    // No component advertises a native planner projection. Scene revision is
    // still owned by the canonical Scene; there is no planner to re-anchor.
    for (const auto &[component_id, composition] : components) {
      static_cast<void>(component_id);
      Require(composition->gripper_runtime != nullptr &&
                  composition->runtime == nullptr &&
                  composition->planner_scene_projection == nullptr,
              "unprojected retained view contains a motion controller");
      composition->projection_revision = revision;
      composition->publication_generation = generation;
    }
    return;
  }
  Require(components.size() == projections.size(),
          "retained native publication projection set changed components");
  // First validate the entire candidate set. A failed validation does not
  // re-anchor even one planner; any later effect still runs under admission.
  for (const auto &[component_id, composition] : components) {
    const auto projection = projections.find(component_id);
    Require(projection != projections.end() && projection->second != nullptr &&
                composition->component_runtime_identity() != nullptr &&
                composition->planner_scene_projection == projection->second,
            "retained native controller has no stable Scene projection owner");
    if (composition->runtime != nullptr) {
      Require(composition->runtime->planner != nullptr &&
                  composition->runtime->planning_scene_synchronizer != nullptr,
              "retained motion controller has no stable planning owner");
    }
    const auto inventory =
        projection->second->current_scene_snapshot_lease_inventory();
    Require(inventory.has_value() &&
                inventory->projection_binding.projection_revision == revision,
            "retained native projection does not match the candidate Scene");
  }
  for (const auto &[component_id, composition] : components) {
    const auto &projection = projections.at(component_id);
    const auto inventory =
        projection->current_scene_snapshot_lease_inventory();
    if (composition->runtime != nullptr) {
      auto &runtime = *composition->runtime;
      const auto rebound = runtime.planner->RebindProjection(
          runtime.planner->projection_binding(), inventory->projection_binding);
      if (!rebound.ok())
        Fail("retained native planner rejected the candidate Scene: " +
             rebound.error().message);
      runtime.planning_scene_synchronizer->ReplaceTarget(
          std::make_shared<ManagerPlanningSceneSynchronizer>(
              manager_run_root, revision.retained_bundle_id,
              revision.graph_revision, generation, projection));
    }
    composition->projection_revision = revision;
    composition->publication_generation = generation;
  }
}
#endif

[[nodiscard]] int
RunManagedRuntimeChild(const ManagedRuntimeChildConfig &config) {
  const auto offer_digest = SelectedExecutionOfferDigest();
  const std::string ready_packet =
      "manyforge.native-execution-ready.v2 " + config.runtime_unit_id + " " +
      config.runtime_epoch + " " + offer_digest + " " +
      config.product_identity + " " + config.configuration_identity;
  std::uint64_t source_report_sequence = 0U;
  ManagedKernelAdmissionGate admission_gate;
  ManagedKernelApplyFenceOwner fence_owner(
      ManagedKernelApplyFenceOwnerConfig{
          config.execution_state_root / "kernel-apply-fence.sqlite3",
          config.manager_state_root,
          config.package_root,
          core::CellId(config.cell_id),
          core::RuntimeUnitId(config.runtime_unit_id),
          core::RuntimeEpoch(config.runtime_epoch),
          core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
          [] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                .count();
          },
          config.program_artifact_root,
      },
      admission_gate);
  const ManagedProjectionRegistryConfig projection_config{
      config.package_root, config.program_artifact_root,
      config.execution_run_root, config.runtime_unit_id,
      config.runtime_epoch};
  ManagedProjectionRegistry projections(
      projection_config,
      MakeFilesystemManagedProjectionAssetSource(projection_config),
      [] {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
      });
  std::mutex managed_composition_mutex;
  // A failed fenced installation may already have re-rooted retained
  // participants toward the private candidate. Keep that candidate alive
  // until after the old active/retired views are destroyed; discarding it
  // would strand non-owning root and Stop-reader pointers while the Apply
  // remains command-closed.
  std::unique_ptr<Composition> failed_prepared_composition;
  std::unique_ptr<Composition> active_managed_composition;
  std::unique_ptr<Composition> retired_managed_composition;
  // Snapshot engine identities separately: the runtime registration vector
  // remains authoritative and may be shared with a future active wrapper.
  std::set<core::IResolvedParticipantExecutionEngine *>
      retired_registration_engines;
  std::unique_ptr<Composition> prepared_managed_composition;
  bool prepared_managed_composition_is_empty = false;
  // A Program/Scene-only Apply keeps the exact component owners. Preparation
  // verifies the candidate product and projections without constructing
  // replacement controllers; installation refreshes only their publication
  // views beneath the Manager fence.
  std::optional<CompositionBuildInput> prepared_retained_view;
  std::shared_ptr<WallClock> detached_owner_clock;
  std::shared_ptr<SimpleScheduler> detached_owner_scheduler;
  // The cell Kernel's recovery resolver captures these by reference, so they
  // must outlive the cell owner and its Kernel at local-scope teardown.
  std::atomic<RetainedNativeRuntime *> stop_recovery_runtime{nullptr};
  std::atomic<core::ResourceAddressedExecutionKernel *> stop_recovery_kernel{
      nullptr};
  // The stable addressed Kernel borrows this observer from construction until
  // destruction, including intervals with no installed component. Declare
  // it before the Kernel so destruction retires the Kernel first.
  std::shared_ptr<RetainedStopSurveillanceOwner>
      stable_stop_surveillance_owner;
  std::unique_ptr<RetainedNativeCellRuntime> stable_cell_runtime_owner;
  std::unique_ptr<core::SqliteOperationJournal> detached_operation_journal;
  std::unique_ptr<core::ResourceAddressedExecutionKernel>
      detached_addressed_kernel;
  // The stable Kernel owns the Stop latch across product reconfiguration.
  // Command dispatch already pins the active composition with its shared
  // lifetime lock, so this atomic selects only that same current product's
  // facts adapter; it is not a second recovery owner.
  core::ResourceAddressedExecutionKernel *stable_kernel_owner{};
  core::SqliteOperationJournal *stable_operation_journal{};
  const auto read_current_publication = [&] {
    component_manager::SqliteActiveBundlePublicationStore publication_store(
        config.manager_state_root /
            std::string(
                component_manager::kActiveBundlePublicationStoreFilename),
        core::CellId(config.cell_id));
    const auto integrity = publication_store.VerifyIntegrity();
    Require(integrity.current_generation.nonzero(),
            "managed active retained-bundle publication is absent");
    auto publication = publication_store.ReadActiveBundlePublication(
        integrity.current_generation);
    publication_store.Close();
    Require(publication.ok() && publication.value().valid(),
            "managed active retained-bundle publication cannot be verified");
    return std::move(publication.value());
  };
  auto bootstrap_retained_bundle_id = config.retained_bundle_id;
  auto bootstrap_graph_revision = config.graph_revision;
  if (!config.program_cycle_journal.empty()) {
    const auto retained_run =
        detail::ReadActiveProgramRunProductForReconciliation(
            config.program_cycle_journal);
    if (!retained_run.ok()) {
      Fail(retained_run.error().message);
    }
    if (retained_run.value().has_value()) {
      bootstrap_retained_bundle_id = retained_run.value()->retained_bundle_id;
      bootstrap_graph_revision = retained_run.value()->graph_revision;
    }
  }
  detached_owner_clock = std::make_shared<WallClock>();
  detached_owner_scheduler = std::make_shared<SimpleScheduler>(
      SimpleSchedulerConfig::from_environment());
  core::ResourceAddressedExecutionKernelConfig bootstrap_kernel_config{
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch),
      core::CellId(config.cell_id),
      1024U,
      {},
      &admission_gate,
      std::nullopt};
  bootstrap_kernel_config.empty_runtime_retained_bundle_id =
      bootstrap_retained_bundle_id;
  bootstrap_kernel_config.empty_runtime_graph_revision =
      bootstrap_graph_revision;
  bootstrap_kernel_config.prequalified_max_live_stop_participants =
      kNativeMaximumLiveStopParticipants;
  auto bootstrap_stop = LoadManagedBootstrapStopProduct(
      config, bootstrap_retained_bundle_id, bootstrap_graph_revision);
  const bool bootstrap_requires_physical_composition =
      bootstrap_stop.requires_physical_composition;
  bootstrap_kernel_config.cell_stop_deadline_budget =
      std::move(bootstrap_stop.deadline_budget);
  bootstrap_kernel_config.empty_runtime_resource_inventory =
      std::move(bootstrap_stop.resource_inventory);
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  bootstrap_kernel_config.stop_fault_recovery_facts_resolver =
      [&](const core::StopStatusDTO &status, const std::int64_t assessed_at_ns)
      -> core::Result<core::StopFaultRecoveryFactsDTO> {
    auto *const runtime = stop_recovery_runtime.load(std::memory_order_acquire);
    auto *const kernel = stop_recovery_kernel.load(std::memory_order_acquire);
    if (runtime == nullptr || kernel == nullptr) {
      return core::Result<core::StopFaultRecoveryFactsDTO>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kMissingReference)),
          "managed native Stop recovery owner is unavailable",
          {{"reason", "managed_native_stop_recovery_owner_unavailable"}}});
    }
    return build_retained_native_stop_recovery_facts(
        *runtime, *kernel, &admission_gate, status, assessed_at_ns);
  };
#endif
  bootstrap_kernel_config.cell_admission_claim_capacity = 1024U;
  // The managed cell hosts the Headless Alarm Lifecycle Owner: Run/Resume
  // and motion-producing recovery evaluate its published attention. The
  // control plane is single-host: only projections from this host boot are
  // accepted, so owner incarnations are comparable (spec 530 §11).
  bootstrap_kernel_config.operator_alarm_attention =
      core::OperatorAlarmAttentionPolicyDTO{
          core::kOperatorAlarmAttentionMaximumSnapshotAgeNs,
          core::ReadHostBootStamp().boot_id};
  bootstrap_kernel_config.cell_admission_reconcile_validator =
      BuildOwnerReconciliationValidator(
          config.manager_state_root /
              std::string(component_manager::kManagerApplyJournalFilename),
          config.program_cycle_journal,
          config.execution_state_root / std::string(core::kCCLCJournalFilename),
          core::CellId(config.cell_id));
  bootstrap_kernel_config.cell_admission_settlement_validator =
      BuildOwnerSettlementValidator(
          config.manager_state_root /
              std::string(component_manager::kManagerApplyJournalFilename),
          config.logical_scene_database, config.program_cycle_journal,
          config.execution_state_root / std::string(core::kCCLCJournalFilename),
          core::CellId(config.cell_id));
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  RetainedNativeCellRuntimeInput cell_runtime_input{
      .clock = detached_owner_clock,
      .scheduler = detached_owner_scheduler,
      .operation_journal_path =
          config.execution_state_root /
          std::string(core::kOperationJournalFilename),
      .participant_registrations = {},
      .kernel_config = std::move(bootstrap_kernel_config),
      .stop_sources = {},
      .stop_source_mode =
          RetainedStopSurveillanceEmptySourceMode::kManagedCellBootstrapEmpty};
  stable_cell_runtime_owner = std::make_unique<RetainedNativeCellRuntime>(
      std::move(cell_runtime_input));
  stable_stop_surveillance_owner =
      stable_cell_runtime_owner->stop_surveillance_owner();
  stable_kernel_owner = &stable_cell_runtime_owner->addressed_kernel();
  stable_operation_journal = &stable_cell_runtime_owner->operation_journal();
#else
  detached_operation_journal = std::make_unique<core::SqliteOperationJournal>(
      config.execution_state_root /
          std::string(core::kOperationJournalFilename),
      core::CellId(config.cell_id));
  detached_addressed_kernel =
      std::make_unique<core::ResourceAddressedExecutionKernel>(
          std::vector<core::RuntimeParticipantExecutionRegistration>{},
          *detached_owner_clock, *detached_owner_scheduler,
          *detached_operation_journal, std::move(bootstrap_kernel_config));
  stable_kernel_owner = detached_addressed_kernel.get();
  stable_operation_journal = detached_operation_journal.get();
#endif
  Require(stable_kernel_owner != nullptr && stable_operation_journal != nullptr,
          "managed native cell owner could not be created");
  if (bootstrap_requires_physical_composition) {
    Require(stable_kernel_owner->SetRouteRecoveryPending(true).ok(),
            "selected physical runtime could not close admission before "
            "composition initialization");
  }
  const auto bind_stable_kernel_to_runtime =
      [&](RetainedNativeRuntime &runtime) {
        runtime.cell_addressed_kernel = stable_kernel_owner;
        runtime.cell_operation_journal = stable_operation_journal;
        runtime.addressed_request_owner_epoch =
            core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch);
        runtime.addressed_cell_id = core::CellId(config.cell_id);
        for (const auto &peer : runtime.participant_peers) {
          peer->cell_addressed_kernel = stable_kernel_owner;
          peer->cell_operation_journal = stable_operation_journal;
          peer->addressed_request_owner_epoch =
              runtime.addressed_request_owner_epoch;
          peer->addressed_cell_id = runtime.addressed_cell_id;
        }
      };
  const auto bind_cell_runtime_to_composition =
      [&](Composition &composition) {
        composition.cell_runtime = stable_cell_runtime_owner.get();
        std::vector<std::shared_ptr<RetainedNativeGripperRuntime>>
            independent_grippers;
        if (composition.gripper_runtime != nullptr)
          independent_grippers.push_back(composition.gripper_runtime);
        for (const auto &participant : composition.participant_compositions) {
          Require(participant != nullptr,
                  "native cell has a missing component composition");
          participant->cell_runtime = stable_cell_runtime_owner.get();
          if (participant->gripper_runtime != nullptr)
            independent_grippers.push_back(participant->gripper_runtime);
          if (participant->runtime != nullptr)
            participant->runtime->cell_runtime_owner =
                stable_cell_runtime_owner.get();
        }
        if (composition.runtime != nullptr) {
          composition.runtime->cell_runtime_owner =
              stable_cell_runtime_owner.get();
          composition.runtime->independent_gripper_peers =
              std::move(independent_grippers);
        }
      };
  stop_recovery_kernel.store(stable_kernel_owner, std::memory_order_release);
  admission_gate.BindResourceTable(
      stable_kernel_owner->resource_admission_table());
  component_manager::detail::ManagerLivenessCommandAdmission
      manager_liveness_admission;
  component_manager::detail::ChildOwnedManagerLivenessReceiver
      manager_liveness_receiver(std::make_shared<WallClock>(),
                                std::make_unique<SimpleScheduler>(
                                    SimpleSchedulerConfig::from_environment()));
  std::atomic<bool> manager_liveness_installed{false};
  std::optional<core::ManagerLivenessReceiverConfig>
      manager_liveness_receiver_config;
  std::optional<core::ActiveBundlePublicationGeneration>
      manager_liveness_publication_generation;
  std::optional<core::ContentDigest> manager_liveness_public_pointer_digest;
  std::optional<core::ManagerLivenessRequiredSetDTO>
      manager_liveness_required_set;
  std::set<std::pair<std::string, std::string>> retired_manager_epochs;
  ExecutionCompositionAccess execution_composition;
  execution_composition.addressed_kernel = stable_kernel_owner;
  bool current_composition_building = false;
  bool current_predecessor_composition_pending = false;

  ManagedExecutionSurfaceControl execution_surface_control(
      config.execution_run_root, &execution_composition,
      &manager_liveness_admission,
      core::RequestOwnerEpoch("kernel-owner-" + config.runtime_epoch));
  std::jthread execution_surface([&](const std::stop_token stop_token) {
    RunManagedExecutionCommandSurface(execution_surface_control, stop_token);
  });
  {
    std::unique_lock lock(execution_surface_control.state_mutex);
    const auto signaled = execution_surface_control.state_changed.wait_for(
        lock, std::chrono::seconds(5), [&] {
          return execution_surface_control.ready ||
                 execution_surface_control.failure != nullptr;
        });
    Require(signaled, "managed execution command surface startup timed out");
    if (execution_surface_control.failure != nullptr) {
      std::rethrow_exception(execution_surface_control.failure);
    }
    Require(execution_surface_control.ready,
            "managed execution command surface did not become ready");
  }
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      preparing_composition_request;
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      installed_under_fence_composition_request;
  std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
      installed_composition_request;
  std::vector<component_manager::ManagerRuntimeComponentReceiptDTO>
      installed_component_receipts;
  std::atomic<std::shared_ptr<
      const component_manager::ManagerSelectedRuntimeCompositionRequestDTO>>
      installed_liveness_handoff;
  const auto confirm_installed_projection_set = [&](const auto &request) {
    auto bundle = active_managed_composition != nullptr
                      ? active_managed_composition->bundle
                      : std::shared_ptr<const deployment::RetainedBundle>{};
    if (bundle == nullptr ||
        bundle->retained_bundle_id() !=
            request.candidate_projection_revision.retained_bundle_id) {
      const auto selected = LoadManagedBundleAtPinnedRoot(
          config, request.candidate_projection_revision.retained_bundle_id);
      Require(selected.has_value(),
              "installed projection provider set has no verified bundle");
      bundle = selected->bundle;
    }
    Require(bundle->value().cell_graph.graph_revision ==
                request.candidate_projection_revision.graph_revision,
            "installed projection provider set has a different graph");
    projections.ConfirmInstalledProjectionSet(*bundle);
  };
  const auto publish_installed_liveness_handoff = [&](const auto &request) {
    installed_liveness_handoff.store(
        std::make_shared<const component_manager::
                             ManagerSelectedRuntimeCompositionRequestDTO>(
            request),
        std::memory_order_release);
  };
  std::exception_ptr managed_composition_failure;
  std::jthread current_composition_builder;
  bool current_composition_builder_started = false;
  std::jthread managed_composition_builder;
  bool managed_composition_building = false;
  std::exception_ptr selected_composition_failure;

  const auto stable_addressed_kernel =
      [&]() -> core::ResourceAddressedExecutionKernel * {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    if (stable_cell_runtime_owner != nullptr) {
      return &stable_cell_runtime_owner->addressed_kernel();
    }
#endif
    if (detached_addressed_kernel != nullptr) {
      return detached_addressed_kernel.get();
    }
    if (active_managed_composition != nullptr &&
        active_managed_composition->active_addressed_kernel() !=
            nullptr) {
      return active_managed_composition->active_addressed_kernel();
    }
    return nullptr;
  };

  const auto await_manager_loss_containment = [&] {
    if (!manager_liveness_installed.load(std::memory_order_acquire)) {
      return;
    }
    while (g_stop_requested == 0 &&
           manager_liveness_receiver.get()->status().state !=
               core::ManagerLivenessStateDTO::kExpired) {
      int waited = -1;
      do {
        waited = ::poll(nullptr, 0U, 25);
      } while (waited < 0 && errno == EINTR && g_stop_requested == 0);
      Require(waited >= 0, "Manager-loss containment wait failed");
    }
  };

  const auto snapshot_registration_engines = [](const Composition &composition) {
    std::set<core::IResolvedParticipantExecutionEngine *> engines;
    const auto include = [&](const Composition &component) {
      if (component.runtime != nullptr) {
        for (const auto &registration :
             component.runtime->participant_registrations) {
          Require(registration.engine != nullptr,
                  "composition registration has no execution engine");
          engines.insert(registration.engine);
        }
      }
      if (component.gripper_runtime != nullptr) {
        const auto registration = component.gripper_runtime->registration();
        Require(registration.engine != nullptr,
                "gripper composition has no execution engine");
        engines.insert(registration.engine);
      }
    };
    include(composition);
    for (const auto &participant : composition.participant_compositions) {
      Require(participant != nullptr,
              "composition has a missing component owner");
      include(*participant);
    }
    Require(!engines.empty(), "physical composition has no execution engines");
    return engines;
  };

  const auto try_release_retired_composition = [&] {
    std::lock_guard lock(managed_composition_mutex);
    auto *const owner = stable_addressed_kernel();
    if (owner == nullptr || installed_under_fence_composition_request.has_value()) {
      return;
    }
    if (retired_managed_composition == nullptr) {
      Require(retired_registration_engines.empty(),
              "retired engine snapshot has no retired composition");
      projections.ReclaimRetiredProjections();
      return;
    }
    std::set<core::IResolvedParticipantExecutionEngine *> active_engines;
    if (active_managed_composition != nullptr) {
      active_engines = snapshot_registration_engines(*active_managed_composition);
    }
    for (auto *engine : retired_registration_engines) {
      if (active_engines.contains(engine)) {
        continue;
      }
      if (owner->participant_engine_in_use(engine)) {
        return;
      }
    }
    for (auto engine = retired_registration_engines.begin();
         engine != retired_registration_engines.end();) {
      if (active_engines.contains(*engine)) {
        ++engine;
        continue;
      }
      if (!owner->release_retired_engine(*engine).ok()) {
        return;
      }
      engine = retired_registration_engines.erase(engine);
    }
    retired_registration_engines.clear();
    retired_managed_composition.reset();
    projections.ReclaimRetiredProjections();
  };

  const auto rethrow_composition_failure = [&] {
    std::exception_ptr failure;
    {
      std::lock_guard lock(managed_composition_mutex);
      failure = managed_composition_failure;
    }
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
    {
      std::lock_guard lock(execution_surface_control.state_mutex);
      failure = execution_surface_control.failure;
    }
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  };

  const auto try_start_current_composition = [&] {
    {
      std::lock_guard lock(managed_composition_mutex);
      if (active_managed_composition != nullptr ||
          current_composition_builder_started ||
          preparing_composition_request.has_value()) {
        return;
      }
    }
    bool predecessor_pending = false;
    auto input = TryBuildCurrentManagedCompositionInput(
        config, projections, admission_gate, predecessor_pending);
    {
      std::lock_guard lock(managed_composition_mutex);
      current_predecessor_composition_pending = predecessor_pending;
    }
    if (!input.has_value()) {
      return;
    }
    {
      std::lock_guard lock(managed_composition_mutex);
      if (active_managed_composition != nullptr ||
          current_composition_builder_started ||
          preparing_composition_request.has_value()) {
        return;
      }
      input->attach_addressed_owner = false;
      input->shared_clock = detached_owner_clock;
      input->shared_scheduler = detached_owner_scheduler;
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
      input->observation_wake = stable_cell_runtime_owner->observation_wake();
#endif
      current_predecessor_composition_pending = false;
      current_composition_builder_started = true;
      Require(stable_kernel_owner->SetRouteRecoveryPending(true).ok(),
              "initial runtime composition could not close admission");
      current_composition_building = true;
    }
    current_composition_builder =
        std::jthread([&, input = std::move(*input)]() mutable {
          try {
            std::cerr << "native composition phase: materializing runtime\n";
            auto built = BuildProductComposition(std::move(input));
            std::cerr << "native composition phase: runtime materialized\n";
            InstallDirectSceneFrameResolver(projections, *built);
            Composition *installed = nullptr;
            {
              std::lock_guard lock(managed_composition_mutex);
              std::unique_lock execution_lock(execution_composition.mutex);
              Require(stable_kernel_owner != nullptr &&
                          stable_operation_journal != nullptr &&
                          detached_owner_clock != nullptr &&
                          detached_owner_scheduler != nullptr,
                      "current managed composition has no stable cell owner");
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              Require(stable_cell_runtime_owner != nullptr &&
                          stable_stop_surveillance_owner != nullptr &&
                          built->component_runtime_identity() != nullptr,
                      "current native composition has no stable component owner");
              Require(built->runtime == nullptr ||
                          (built->runtime->addressed_kernel == nullptr &&
                           built->runtime->operation_journal == nullptr &&
                           built->runtime->clock == detached_owner_clock &&
                           built->runtime->scheduler == detached_owner_scheduler),
                      "current managed composition duplicated the stable "
                      "addressed owner");
#else
              Require(built->runtime != nullptr &&
                          built->runtime->addressed_kernel == nullptr &&
                          built->runtime->operation_journal == nullptr &&
                          built->runtime->clock == detached_owner_clock &&
                          built->runtime->scheduler == detached_owner_scheduler,
                      "current managed composition duplicated the stable "
                      "addressed owner");
#endif
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              auto current_registrations =
                  NativeCellComponentRegistrations(*built);
              auto current_resolver = NativeCellParticipantResolver(
                  *built, current_registrations);
              auto current_stop_sources = NativeCellStopSources(*built);
              const auto stop_product = LoadManagedBootstrapStopProduct(
                  config, built->bundle->retained_bundle_id(),
                  built->bundle->value().cell_graph.graph_revision);
              auto reconfigured =
                  stable_cell_runtime_owner->reconfigure_runtime_product(
                      std::move(current_registrations),
                      std::move(current_resolver),
                      std::move(current_stop_sources),
                      stop_product.deadline_budget);
#else
              auto reconfigured =
                  detached_addressed_kernel->reconfigure_runtime_product(
                      built->runtime->participant_registrations,
                      std::move(built->runtime->participant_resolver),
                      built->runtime->cell_stop_deadline_budget);
#endif
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected current runtime participants: " +
                     reconfigured.error().message);
              }
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              built->runtime->operation_journal =
                  std::move(detached_operation_journal);
              built->runtime->addressed_kernel =
                  std::move(detached_addressed_kernel);
              detached_owner_clock.reset();
              detached_owner_scheduler.reset();
#else
              if (built->runtime != nullptr) {
                built->runtime->cell_addressed_kernel = stable_kernel_owner;
                built->runtime->cell_operation_journal =
                    stable_operation_journal;
                built->runtime->stop_surveillance_owner =
                    stable_stop_surveillance_owner;
              }
#endif
              active_managed_composition = std::move(built);
              if (active_managed_composition->runtime != nullptr)
                bind_stable_kernel_to_runtime(*active_managed_composition->runtime);
              bind_cell_runtime_to_composition(*active_managed_composition);
              execution_composition.replace_current(
                  active_managed_composition.get());
              execution_composition.addressed_kernel =
                  active_managed_composition->active_addressed_kernel();
              stop_recovery_runtime.store(
                  active_managed_composition->runtime.get(),
                  std::memory_order_release);
              installed = active_managed_composition.get();
            }
            std::cerr << "native composition phase: stable Kernel bound\n";
            // Initial CCLC recovery is part of composing a usable runtime.  It
            // runs on the existing builder lane so the ordinary Manager
            // command channel remains responsive and cannot advertise an
            // installed composition before its exact routes are current.
            const auto routes_attached =
                AttachCurrentPublishedCCLCIfReady(*installed, config, fence_owner);
            std::cerr << "native composition phase: control routes "
                      << (routes_attached ? "attached" : "deferred") << '\n';
            {
              std::lock_guard lock(managed_composition_mutex);
              Require(active_managed_composition.get() == installed,
                      "initial managed composition changed during CCLC "
                      "recovery");
              current_composition_building = false;
            }
          } catch (...) {
            std::lock_guard lock(managed_composition_mutex);
            managed_composition_failure = std::current_exception();
            current_composition_building = false;
          }
        });
  };

  const auto install_or_observe_composition =
      [&](const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
              &request)
      -> component_manager::ManagerSelectedRuntimeCompositionResultDTO {
    Require(request.valid() &&
                request.runtime_unit_id.value() == config.runtime_unit_id &&
                request.runtime_epoch.value() == config.runtime_epoch,
            "runtime-composition request names another managed child");
    if (selected_composition_failure != nullptr) {
      std::rethrow_exception(selected_composition_failure);
    }
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    // Dynamic ROS2 component ownership is deliberately not advertised by the
    // current provider. Refuse before any candidate effect; an empty receipt
    // must never be reported as a successful installed component set.
    if (!request.component_delta.components.empty())
      Fail("ros2_component_delta_not_qualified: dynamic ROS2 component "
           "installation requires a lifecycle provider receipt");
#endif
    const auto read_installed_receipts = [&] {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
      return ReadNativeInstalledComponentReceipts(
          request, active_managed_composition.get(),
          retired_managed_composition != nullptr
              ? retired_managed_composition.get()
              : (IsExactRetainedNativeViewApply(request)
                     ? active_managed_composition.get()
                     : nullptr));
#else
      return std::vector<
          component_manager::ManagerRuntimeComponentReceiptDTO>{};
#endif
    };
    const core::ConfigurationFenceStatusQueryDTO fence_query{
        {request.apply_id, request.apply_generation},
        request.configuration_fence_id,
        request.configuration_fence_generation,
        request.configuration_fence_scope,
        request.manager_runtime_epoch};
    const auto require_installed_candidate_configuration = [&] {
      const auto fence_status = fence_owner.QueryStatus(fence_query);
      Require(fence_status.matches(fence_query) &&
                  fence_status.state == core::ConfigurationFenceState::kHeld &&
                  fence_status.admission_fenced &&
                  fence_status.reconciliation_evidence.execution_host_epoch
                          .runtime_unit_id == request.runtime_unit_id &&
                  fence_status.reconciliation_evidence.execution_host_epoch
                          .runtime_epoch == request.runtime_epoch &&
                  fence_status.reconciliation_evidence
                          .active_publication_generation ==
                      request.expected_predecessor_publication_generation &&
                  fence_status.reconciliation_evidence
                          .active_public_pointer_digest ==
                      request.expected_predecessor_public_pointer_digest,
              "installed runtime composition does not reconcile to its exact "
              "durable held fence and predecessor publication");
    };
    try_release_retired_composition();
    try_start_current_composition();
    rethrow_composition_failure();
    {
      std::lock_guard lock(managed_composition_mutex);
      if (current_predecessor_composition_pending &&
          active_managed_composition == nullptr &&
          !current_composition_builder_started) {
        // The published predecessor already declares physical components,
        // but their verified Scene projections have not settled yet. Keep
        // the Manager request in preparation; a retain delta may only proceed
        // after the exact predecessor owners exist in this live Kernel.
        return {request,
                component_manager::ManagerSelectedRuntimeCompositionState::
                    kPreparing,
                {}};
      }
    }
    if (request.phase == component_manager::
                             ManagerSelectedRuntimeCompositionPhase::kFinalize) {
      std::optional<component_manager::ManagerSelectedRuntimeCompositionRequestDTO>
          prepared_request;
      {
        std::lock_guard lock(managed_composition_mutex);
        if (installed_under_fence_composition_request.has_value() &&
            installed_under_fence_composition_request->phase ==
                component_manager::ManagerSelectedRuntimeCompositionPhase::
                    kPrepare) {
          prepared_request = installed_under_fence_composition_request;
        }
      }
      if (prepared_request.has_value()) {
        auto expected = request;
        expected.phase = component_manager::
            ManagerSelectedRuntimeCompositionPhase::kPrepare;
        expected.candidate_projection_revision =
            prepared_request->candidate_projection_revision;
        expected.authoritative_scene_snapshot_digest =
            prepared_request->authoritative_scene_snapshot_digest;
        Require(expected == *prepared_request &&
                    request.candidate_projection_revision.scene_revision.scene_id ==
                        prepared_request->candidate_projection_revision
                            .scene_revision.scene_id &&
                    request.candidate_projection_revision.scene_revision.revision_id >
                        prepared_request->candidate_projection_revision
                            .scene_revision.revision_id,
                "final runtime composition differs from its exact prepared Apply");
        require_installed_candidate_configuration();
        const auto selected_bundle =
            LoadExactManagedCandidateBundle(config, request);
        Require(selected_bundle.has_value(),
                "final runtime composition candidate bundle is unavailable");
        const auto &bundle = *selected_bundle->bundle;
        const auto final_projections =
            bundle.value().cell_graph.controllable_components.empty()
                ? std::optional<ManagedComponentProjectionSet>{}
                : SelectManagedComponentProjections(
                      config, bundle, projections,
                      request.candidate_projection_revision, false);
        Require(bundle.value().cell_graph.controllable_components.empty() ||
                    final_projections.has_value(),
                "final runtime composition has no installed Scene projection");
        Composition *current = nullptr;
        {
          std::lock_guard lock(managed_composition_mutex);
          Require(installed_under_fence_composition_request == prepared_request &&
                      !current_composition_building &&
                      !managed_composition_building &&
                      (bundle.value().cell_graph.controllable_components.empty() ||
                       active_managed_composition != nullptr) &&
                      (active_managed_composition == nullptr ||
                       active_managed_composition->projection_revision ==
                           prepared_request->candidate_projection_revision),
                  "prepared runtime changed before final Scene handoff");
          current = active_managed_composition.get();
          installed_under_fence_composition_request = request;
        }
        if (current != nullptr) {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
          fence_owner.RunWithExactHeldPredecessor(
              fence_query,
              request.expected_predecessor_publication_generation,
              request.expected_predecessor_public_pointer_digest, [&] {
                std::unique_lock execution_lock(execution_composition.mutex);
                Require(active_managed_composition.get() == current &&
                            final_projections.has_value(),
                        "native composition changed before final Scene");
                RefreshRetainedNativePublicationView(
                    *current, final_projections->by_component,
                    request.candidate_projection_revision,
                    current->publication_generation,
                    config.manager_run_root);
              });
#else
          {
            std::unique_lock execution_lock(execution_composition.mutex);
            Require(active_managed_composition.get() == current,
                    "ROS composition changed before final Scene");
            current->projection_revision = request.candidate_projection_revision;
          }
#endif
          InstallDirectSceneFrameResolver(projections, *current);
        }
      }
    }
    const bool publication_is_successor =
        CurrentPublicationIsInstalledSuccessor(config, request);
    const auto successor_bundle =
        publication_is_successor
            ? LoadExactManagedCandidateBundle(config, request)
            : std::optional<ManagedBundleAtPinnedRoot>{};
    bool reconcile_installed_candidate = false;
    Composition *published_installed_composition = nullptr;
    {
      std::lock_guard lock(managed_composition_mutex);
      if (installed_composition_request.has_value()) {
        if (*installed_composition_request == request) {
          if (!publication_is_successor) {
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kInstalled,
                    installed_component_receipts};
          }
          if (current_composition_building) {
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kPreparing,
                    {}};
          }
          Require(active_managed_composition != nullptr,
                  "published installed runtime has no active composition");
          current_composition_building = true;
          published_installed_composition =
              active_managed_composition.get();
        }
      }
      if (published_installed_composition == nullptr &&
          installed_under_fence_composition_request.has_value()) {
        Require(*installed_under_fence_composition_request == request,
                "another selected runtime composition is installed under "
                "the held fence");
        if (request.phase == component_manager::
                                 ManagerSelectedRuntimeCompositionPhase::kPrepare) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kInstalled,
                  {}};
        }
        reconcile_installed_candidate = true;
      }
      if (published_installed_composition == nullptr &&
          !reconcile_installed_candidate) {
        if (current_composition_building) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing,
                  {}};
        }
        if (publication_is_successor) {
          const auto expected_generation =
              core::ActiveBundlePublicationGeneration(
                  request.expected_predecessor_publication_generation.value() +
                  1U);
          if (active_managed_composition == nullptr) {
            if (successor_bundle->bundle->value()
                    .cell_graph.controllable_components.empty()) {
              installed_component_receipts = read_installed_receipts();
              installed_composition_request = request;
              confirm_installed_projection_set(request);
              publish_installed_liveness_handoff(request);
              return {request,
                      component_manager::
                          ManagerSelectedRuntimeCompositionState::kInstalled,
                      installed_component_receipts};
            }
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kPreparing,
                    {}};
          }
          Require(active_managed_composition->bundle->retained_bundle_id() ==
                          request.candidate_projection_revision
                              .retained_bundle_id &&
                      active_managed_composition->projection_revision ==
                          request.candidate_projection_revision &&
                      active_managed_composition->publication_generation ==
                          expected_generation,
                  "recovered active composition differs from the published "
                  "Apply successor");
          current_composition_building = true;
          published_installed_composition =
              active_managed_composition.get();
        }
        Require(request.phase == component_manager::
                    ManagerSelectedRuntimeCompositionPhase::kPrepare,
                "final runtime composition has no exact prepared candidate");
        if (retired_managed_composition != nullptr) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing,
                  {}};
        }
      }
    }
    if (published_installed_composition != nullptr) {
      bool attached = false;
      try {
        attached = AttachCurrentPublishedCCLCIfReady(
            *published_installed_composition, config, fence_owner, fence_query);
        std::lock_guard lock(managed_composition_mutex);
        Require(active_managed_composition.get() ==
                    published_installed_composition,
                "published installed runtime changed during route recovery");
        if (attached) {
          if (installed_composition_request == request) {
            // This exact Apply already proved controller retention before
            // retiring its predecessor. Replay that receipt; the released
            // predecessor is no longer a live source of lifetime evidence.
            Require(component_manager::ComponentReceiptsMatchRequest(
                        request, installed_component_receipts),
                    "installed receipt replay differs from its exact Apply");
          } else {
            installed_component_receipts = read_installed_receipts();
          }
          installed_composition_request = request;
          confirm_installed_projection_set(request);
          publish_installed_liveness_handoff(request);
        }
        current_composition_building = false;
      } catch (...) {
        std::lock_guard lock(managed_composition_mutex);
        current_composition_building = false;
        throw;
      }
      return {
          request,
          attached
              ? component_manager::ManagerSelectedRuntimeCompositionState::
                    kInstalled
              : component_manager::ManagerSelectedRuntimeCompositionState::
                    kPreparing,
          attached ? installed_component_receipts
                   : std::vector<component_manager::
                         ManagerRuntimeComponentReceiptDTO>{}};
    }
    if (reconcile_installed_candidate) {
      require_installed_candidate_configuration();
      // Finalization is still a private parent-Apply effect. Publish and
      // read back every required native route before reporting the selected
      // runtime installed; a later CCLC service attachment is not a route
      // readiness substitute. The Apply fence continues to block commands.
      if (active_managed_composition != nullptr) {
        fence_owner.RunWithExactHeldPredecessor(
            fence_query, request.expected_predecessor_publication_generation,
            request.expected_predecessor_public_pointer_digest, [&] {
              const auto candidate_package = LoadManagedBundleAtPinnedRoot(
                  config, request.candidate_projection_revision
                              .retained_bundle_id);
              Require(candidate_package.has_value(),
                      "finalized native Apply bundle is unavailable");
              const deployment::FilesystemVerifiedRetainedProductArtifactSource
                  artifacts(candidate_package->package_root);
              const auto representatives =
                  CellControlBindingRepresentatives(*active_managed_composition);
              const auto &candidate = *active_managed_composition->bundle;
              // CCLC records one decision before any candidate route exists.
              // Retained facts are taken from the live Kernel only after the
              // host proves that the exact predecessor controller survived.
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              auto plan_request = BuildNativeApplyPlanRequest(
                  request, *active_managed_composition,
                  retired_managed_composition != nullptr
                      ? retired_managed_composition.get()
                      : (IsExactRetainedNativeViewApply(request)
                             ? active_managed_composition.get()
                             : nullptr),
                  representatives);
#else
              core::NativeApplyPlanRequestDTO plan_request{
                  {fence_query.owner_transaction.apply_id,
                   fence_query.owner_transaction.apply_generation},
                  candidate.retained_bundle_id(),
                  candidate.value().cell_graph.graph_revision,
                  {}, {}};
              for (const auto &component :
                   candidate.value().cell_graph.controllable_components)
                plan_request.components.push_back(
                    component.controllable_component_id);
              for (const auto &desired :
                   candidate.value().desired_control_configurations) {
                const auto compiled = std::find_if(
                    representatives.begin(), representatives.end(),
                    [&](const auto &row) {
                      return row.value().control_path.has_value() &&
                             row.value().control_path->control_domain_id ==
                                 desired.control_domain_id;
                    });
                Require(compiled != representatives.end(),
                        "candidate domain has no compiled binding");
                plan_request.compiled_routes.push_back(
                    {desired.control_domain_id,
                     compiled->value()
                         .control_path->active_control_configuration_generation});
              }
#endif
              core::SqliteCCLCJournal plan_journal(
                  config.execution_state_root /
                      std::string(core::kCCLCJournalFilename),
                  core::CellId(config.cell_id));
              const auto plan = plan_journal.PlanNativeApply(plan_request);
              plan_journal.Close();
              if (!plan.ok()) {
                Fail("native CCLC could not record the Apply plan: " +
                     plan.error().message);
              }
              auto &installed = *active_managed_composition;
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              {
                Require(installed.cell_runtime != nullptr,
                        "native Apply lost its stable cell owner");
                const bool has_independent_gripper =
                    installed.gripper_runtime != nullptr ||
                    std::any_of(
                        installed.participant_compositions.begin(),
                        installed.participant_compositions.end(),
                        [](const auto &participant) {
                          return participant != nullptr &&
                                 participant->gripper_runtime != nullptr;
                        });
                if (has_independent_gripper &&
                    installed.cclc_cell_route_publication_owner == nullptr) {
                  installed.cclc_cell_route_publication_owner =
                      std::make_unique<RetainedNativeCellRoutePublicationOwner>(
                          installed);
                }
                if (installed.runtime == nullptr && !has_independent_gripper)
                  Fail("native Apply has no installed route owner");
                if (installed.cclc_cell_command_readiness_port == nullptr) {
                  installed.cclc_cell_command_readiness_port =
                      MakeNativeCellReadinessPort(installed);
                }
              }
#endif
              auto &route_owner =
                  installed.cclc_cell_route_publication_owner != nullptr
                      ? *installed.cclc_cell_route_publication_owner
                      : static_cast<deployment::IRouteConfigurationKernelPublicationOwner &>(
                            *installed.runtime);
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              auto &readiness_owner = *installed.cclc_cell_command_readiness_port;
#else
              auto &readiness_owner =
                  static_cast<core::ICCLCComponentCommandReadinessPort &>(
                      *installed.runtime);
#endif
              const auto initialized = InitializeRetainedNativeCandidateControlRoutes(
                  candidate, *installed.active_addressed_kernel(), route_owner,
                  readiness_owner, installed.publication_generation,
                  plan.value(), artifacts, representatives,
                  config.execution_run_root,
                  fence_query.configuration_fence_id);
              if (!initialized.ok()) {
                Fail("native CCLC candidate routes are not ready: " +
                     initialized.error().message);
              }
            });
      }
      {
        std::lock_guard lock(managed_composition_mutex);
        Require(installed_under_fence_composition_request.has_value() &&
                    *installed_under_fence_composition_request == request,
                "installed runtime composition changed during fence "
                "reconciliation");
        installed_component_receipts = read_installed_receipts();
        installed_composition_request = request;
        confirm_installed_projection_set(request);
        publish_installed_liveness_handoff(request);
        installed_under_fence_composition_request.reset();
        preparing_composition_request.reset();
      }
      try_release_retired_composition();
      return {request, component_manager::
                           ManagerSelectedRuntimeCompositionState::kInstalled,
              installed_component_receipts};
    }

    std::unique_ptr<Composition> prepared;
    std::optional<CompositionBuildInput> retained_view_input;
    bool prepared_is_empty = false;
    bool start_builder = false;
    std::shared_ptr<WallClock> shared_clock;
    std::shared_ptr<SimpleScheduler> shared_scheduler;
    std::map<core::ControllableComponentId, RetainedNativeComponentSnapshot>
        retained_snapshots;
    {
      std::lock_guard lock(managed_composition_mutex);
      if (managed_composition_failure != nullptr) {
        std::rethrow_exception(managed_composition_failure);
      }
      if (preparing_composition_request.has_value() &&
          *preparing_composition_request != request) {
        Fail("another selected runtime composition is still preparing");
      }
      if (prepared_managed_composition != nullptr) {
        prepared = std::move(prepared_managed_composition);
      } else if (prepared_retained_view.has_value()) {
        retained_view_input = std::move(prepared_retained_view);
        prepared_retained_view.reset();
      } else if (prepared_managed_composition_is_empty) {
        prepared_is_empty = true;
        prepared_managed_composition_is_empty = false;
      } else if (!preparing_composition_request.has_value()) {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
        VerifyRetainedNativePredecessors(active_managed_composition.get(),
                                          request);
        if (active_managed_composition != nullptr) {
          const auto previous = IndexNativeComponentCompositions(
              *active_managed_composition);
          for (const auto &row : request.component_delta.components) {
            if (row.disposition !=
                deployment::ApplyComponentDisposition::kRetain)
              continue;
            const auto *owner = previous.at(row.controllable_component_id);
            Require(owner->component_runtime_identity() != nullptr &&
                        (owner->runtime == nullptr ||
                         owner->direct_control_joint_limits.has_value()),
                    "retained native component has no predecessor owner");
            retained_snapshots.emplace(
                row.controllable_component_id,
                RetainedNativeComponentSnapshot{
                    owner->runtime, owner->gripper_runtime, owner->stage,
                    owner->bundle->value()
                        .compiled_products.planning_collision_model,
                    owner->direct_control_joint_limits,
                    owner->direct_control_motion_descriptor,
                    owner->direct_control_gripper_descriptor,
                    owner->operator_confirmation_validity_ns});
          }
        }
#endif
        preparing_composition_request = request;
        managed_composition_building = true;
        start_builder = true;
        if (active_managed_composition != nullptr) {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
          Require(stable_cell_runtime_owner != nullptr,
                  "active native composition has no retained cell owner");
          shared_clock = stable_cell_runtime_owner->clock();
          shared_scheduler = stable_cell_runtime_owner->scheduler();
#else
          shared_clock = active_managed_composition->runtime->clock;
          shared_scheduler = active_managed_composition->runtime->scheduler;
#endif
        } else if (stable_addressed_kernel() != nullptr) {
          shared_clock = detached_owner_clock;
          shared_scheduler = detached_owner_scheduler;
        }
      }
    }

    if (start_builder) {
      managed_composition_builder = std::jthread(
          [&, request, shared_clock = std::move(shared_clock),
           shared_scheduler = std::move(shared_scheduler),
           retained_snapshots = std::move(retained_snapshots)]() mutable {
            try {
              auto input = BuildManagedCompositionInput(
                  config, projections, admission_gate, request,
                  std::move(shared_clock), std::move(shared_scheduler));
              if (input.has_value()) {
                input->retained_component_snapshots =
                    std::move(retained_snapshots);
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
                input->observation_wake =
                    stable_cell_runtime_owner->observation_wake();
#endif
              }
              const bool retain_view =
                  input.has_value() &&
                  IsExactRetainedNativeViewApply(request);
              auto built = input.has_value() && !retain_view
                               ? BuildProductComposition(std::move(*input))
                               : std::unique_ptr<Composition>{};
              std::lock_guard lock(managed_composition_mutex);
              prepared_managed_composition = std::move(built);
              if (retain_view)
                prepared_retained_view = std::move(input);
              prepared_managed_composition_is_empty = !input.has_value();
              managed_composition_building = false;
            } catch (...) {
              std::lock_guard lock(managed_composition_mutex);
              managed_composition_failure = std::current_exception();
              managed_composition_building = false;
            }
          });
      return {request, component_manager::
                           ManagerSelectedRuntimeCompositionState::kPreparing,
              {}};
    }
    if (prepared == nullptr && !prepared_is_empty &&
        !retained_view_input.has_value()) {
      std::lock_guard lock(managed_composition_mutex);
      if (managed_composition_failure != nullptr) {
        std::rethrow_exception(managed_composition_failure);
      }
      if (prepared_managed_composition != nullptr) {
        prepared = std::move(prepared_managed_composition);
      } else if (prepared_retained_view.has_value()) {
        retained_view_input = std::move(prepared_retained_view);
        prepared_retained_view.reset();
      } else if (prepared_managed_composition_is_empty) {
        prepared_is_empty = true;
        prepared_managed_composition_is_empty = false;
      } else {
        if (managed_composition_building) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing,
                  {}};
        }
        Fail("selected runtime preparation was consumed without an exact "
             "installed result; automatic replacement is forbidden");
      }
    }
    if (managed_composition_builder.joinable()) {
      managed_composition_builder.join();
    }

    try {
      fence_owner.RunWithExactHeldPredecessor(
        fence_query, request.expected_predecessor_publication_generation,
        request.expected_predecessor_public_pointer_digest, [&] {
          std::unique_lock execution_lock(execution_composition.mutex);
          Require(retired_managed_composition == nullptr &&
                      retired_registration_engines.empty(),
                  "a predecessor runtime engine remains lifetime-pinned");
          if (retained_view_input.has_value()) {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
            Require(IsExactRetainedNativeViewApply(request) &&
                        active_managed_composition != nullptr &&
                        active_managed_composition->bundle != nullptr &&
                        retained_view_input->bundle != nullptr &&
                        RetainedNativeViewFollowsPredecessor(
                            active_managed_composition->bundle
                                ->retained_bundle_id(),
                            active_managed_composition->projection_revision,
                            retained_view_input->bundle->retained_bundle_id(),
                            request.predecessor_projection_revision),
                    "retained native view crossed its verified predecessor");
            auto &candidate = *active_managed_composition;
            Require(candidate.component_runtime_identity() != nullptr &&
                        candidate.active_addressed_kernel() ==
                            stable_addressed_kernel(),
                    "retained native view lost its stable command owner");
            const auto generation =
                retained_view_input->publication_generation;
            RefreshRetainedNativePublicationView(
                candidate,
                retained_view_input->component_planner_scene_projections,
                request.candidate_projection_revision, generation,
                config.manager_run_root);
            candidate.bundle = std::move(retained_view_input->bundle);
            DetachRetainedCCLC(candidate);
            Require(candidate.active_addressed_kernel()
                        ->SetRouteRecoveryPending(true)
                        .ok(),
                    "retained native view could not close route recovery");
            candidate.route_recovery_pending.store(true,
                                                   std::memory_order_release);
            InstallDirectSceneFrameResolver(projections, candidate);
#else
            Fail("ROS retained native view is unavailable");
#endif
          } else if (prepared_is_empty) {
            Require(prepared == nullptr,
                    "empty selected product has a physical composition");
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
            Require(stable_cell_runtime_owner != nullptr,
                    "managed native cell owner is unavailable");
            if (active_managed_composition != nullptr) {
              DetachRetainedCCLC(*active_managed_composition);
              auto retired_engines =
                  snapshot_registration_engines(*active_managed_composition);
              auto reconfigured =
                  stable_cell_runtime_owner->reconfigure_runtime_component_set(
                      {}, {}, {}, std::nullopt,
                      request.candidate_projection_revision.retained_bundle_id,
                      request.candidate_projection_revision.graph_revision,
                      request.configuration_fence_id,
                      request.configuration_fence_scope, {});
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected the empty selected runtime: " +
                     reconfigured.error().message);
              }
              retired_registration_engines = std::move(retired_engines);
              retired_managed_composition =
                  std::move(active_managed_composition);
            } else {
              auto reconfigured =
                  stable_cell_runtime_owner->reconfigure_runtime_component_set(
                      {}, {}, {}, std::nullopt,
                      request.candidate_projection_revision.retained_bundle_id,
                      request.candidate_projection_revision.graph_revision,
                      request.configuration_fence_id,
                      request.configuration_fence_scope, {});
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected the empty selected runtime: " +
                     reconfigured.error().message);
              }
            }
            Require(
                stable_cell_runtime_owner->addressed_kernel()
                    .SetRouteRecoveryPending(false)
                    .ok(),
                "empty selected runtime could not release route recovery");
#else
            std::function<core::Result<void>()> commit_empty_stop_sources;
            if (active_managed_composition != nullptr) {
              DetachRetainedCCLC(*active_managed_composition);
              Require(detached_addressed_kernel == nullptr &&
                          detached_operation_journal == nullptr &&
                      detached_owner_clock == nullptr &&
                      detached_owner_scheduler == nullptr,
                      "stable addressed owner is duplicated");
              auto retired_engines =
                  snapshot_registration_engines(*active_managed_composition);
              auto &stable_runtime = *active_managed_composition->runtime;
              auto reconfigured = stable_runtime.active_addressed_kernel()
                                      ->reconfigure_runtime_component_set(
                                          {}, {}, std::nullopt,
                                          request.candidate_projection_revision
                                              .retained_bundle_id,
                                          request.candidate_projection_revision
                                              .graph_revision,
                                          request.configuration_fence_id,
                                          request.configuration_fence_scope,
                                          {}, commit_empty_stop_sources);
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected the empty selected runtime: " +
                     reconfigured.error().message);
              }
              detached_owner_clock = stable_runtime.clock;
              detached_owner_scheduler = stable_runtime.scheduler;
              detached_operation_journal =
                  std::move(stable_runtime.operation_journal);
              detached_addressed_kernel =
                  std::move(stable_runtime.addressed_kernel);
              retired_registration_engines = std::move(retired_engines);
              retired_managed_composition =
                  std::move(active_managed_composition);
            } else if (detached_addressed_kernel != nullptr) {
              auto reconfigured = detached_addressed_kernel
                                      ->reconfigure_runtime_component_set(
                                          {}, {}, std::nullopt,
                                          request.candidate_projection_revision
                                              .retained_bundle_id,
                                          request.candidate_projection_revision
                                              .graph_revision,
                                          request.configuration_fence_id,
                                          request.configuration_fence_scope,
                                          {}, commit_empty_stop_sources);
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected the empty selected runtime: " +
                     reconfigured.error().message);
              }
            }
            Require(
                detached_addressed_kernel != nullptr &&
                    detached_addressed_kernel->SetRouteRecoveryPending(false)
                        .ok(),
                "empty selected runtime could not release route recovery");
#endif
          } else {
            Require(prepared != nullptr,
                    "physical selected product has no prepared composition");
            auto *const stable_owner = stable_addressed_kernel();
            if (stable_owner == nullptr) {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              Fail("managed native candidate has no stable cell Kernel");
#else
              attach_retained_native_addressed_owner(
                  prepared->runtime,
                  RetainedNativeAddressedOwnerConfig{
                      core::RequestOwnerEpoch("kernel-owner-" +
                                              config.runtime_epoch),
                      core::CellId(config.cell_id),
                      config.execution_state_root /
                          std::string(core::kOperationJournalFilename),
                      1024U,
                      &admission_gate,
                      prepared->runtime->motion_binding.value().graph_revision,
                      prepared->runtime->cell_stop_deadline_budget,
                      {},
                      BuildOwnerSettlementValidator(
                          config.manager_state_root /
                              std::string(component_manager::
                                              kManagerApplyJournalFilename),
                          config.logical_scene_database,
                          config.program_cycle_journal,
                          config.execution_state_root /
                              std::string(core::kCCLCJournalFilename),
                          core::CellId(config.cell_id)),
                      BuildOwnerReconciliationValidator(
                          config.manager_state_root /
                              std::string(component_manager::
                                              kManagerApplyJournalFilename),
                          config.program_cycle_journal,
                          config.execution_state_root /
                              std::string(core::kCCLCJournalFilename),
                          core::CellId(config.cell_id)),
                      {}});
              admission_gate.BindResourceTable(
                  prepared->runtime->addressed_kernel
                      ->resource_admission_table());
#endif
            } else {
              if (active_managed_composition != nullptr) {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
                Require(stable_cell_runtime_owner != nullptr &&
                            detached_owner_clock != nullptr &&
                            detached_owner_scheduler != nullptr,
                        "prepared native runtime lost the stable cell time services");
#else
                Require(prepared->runtime->clock == active_managed_composition->runtime->clock &&
                            prepared->runtime->scheduler == active_managed_composition->runtime->scheduler,
                        "prepared runtime crossed the stable Kernel time capabilities");
#endif
              } else {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
                Require(stable_cell_runtime_owner != nullptr &&
                            stable_operation_journal != nullptr &&
                            detached_owner_clock != nullptr &&
                            detached_owner_scheduler != nullptr,
                        "stable native cell owner is incomplete");
#else
                Require(detached_operation_journal != nullptr &&
                            detached_owner_clock != nullptr &&
                            detached_owner_scheduler != nullptr &&
                            prepared->runtime->clock == detached_owner_clock &&
                            prepared->runtime->scheduler == detached_owner_scheduler,
                        "detached addressed owner is incomplete");
#endif
              }
              std::optional<std::set<
                  core::IResolvedParticipantExecutionEngine *>>
                  predecessor_engine_snapshot;
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              const auto retained_component_ids =
                  RetainedNativeComponentIds(request);
              auto stable_stop_observer = stable_stop_surveillance_owner;
              std::vector<RetainedStopSurveillanceSourceInput>
                  candidate_stop_sources;
#endif
              if (active_managed_composition != nullptr) {
                predecessor_engine_snapshot =
                    snapshot_registration_engines(*active_managed_composition);
                DetachRetainedCCLC(*active_managed_composition);
              }
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              if (!retained_component_ids.empty()) {
                Require(active_managed_composition != nullptr &&
                            (prepared->runtime == nullptr ||
                             prepared->candidate_participant_resolver_factory),
                        "retained native Apply has no predecessor or candidate "
                        "participant resolver");
                ResealRetainedNativeCandidateOwners(
                    *prepared, *active_managed_composition, request,
                    admission_gate);
                RefreshRetainedNativeCandidateCollision(
                    *prepared, request, admission_gate);
                RefreshRetainedNativeCandidateModels(
                    *prepared, request, admission_gate,
                    *stable_cell_runtime_owner->clock());
                if (prepared->runtime != nullptr) {
                  std::vector<std::shared_ptr<RetainedNativeRuntime>> peers;
                  peers.reserve(prepared->participant_compositions.size());
                  for (const auto &component :
                       prepared->participant_compositions) {
                    Require(component != nullptr &&
                                component->component_runtime_identity() != nullptr,
                            "retained native Apply has an incomplete peer");
                    if (component->runtime != nullptr)
                      peers.push_back(component->runtime);
                  }
                  auto rejoined = rejoin_retained_native_runtime_participants(
                      prepared->runtime, std::move(peers),
                      prepared->candidate_participant_resolver_factory,
                      stable_stop_observer, candidate_stop_sources,
                      admission_gate, request.configuration_fence_id,
                      request.configuration_fence_scope);
                  if (!rejoined.ok())
                    Fail("retained native cell participant rejoin failed: " +
                         rejoined.error().message);
                  AppendNativeGripperStopSources(*prepared,
                                                 candidate_stop_sources);
                } else {
                  candidate_stop_sources = NativeCellStopSources(*prepared);
                }
                std::map<core::ControllableComponentId,
                         std::shared_ptr<RetainedPlannerSceneProjection>>
                    selected_projections;
                const auto include_projection = [&](const Composition &view) {
                  if (view.planner_scene_projection == nullptr) {
                    Require(view.runtime == nullptr &&
                                view.gripper_runtime != nullptr,
                            "retained motion candidate has no Scene projection");
                    return;
                  }
                  Require(selected_projections
                              .emplace(view.controllable_component_id,
                                       view.planner_scene_projection)
                              .second,
                          "retained native candidate has a duplicate Scene "
                          "projection");
                };
                include_projection(*prepared);
                for (const auto &component :
                     prepared->participant_compositions)
                  include_projection(*component);
                RefreshRetainedNativePublicationView(
                    *prepared, selected_projections,
                    request.candidate_projection_revision,
                    prepared->publication_generation,
                    config.manager_run_root);
              }
              if (retained_component_ids.empty()) {
                candidate_stop_sources = NativeCellStopSources(*prepared);
              }
              auto candidate_registrations =
                  NativeCellComponentRegistrations(*prepared);
              auto candidate_resolver = NativeCellParticipantResolver(
                  *prepared, candidate_registrations);
              const auto candidate_stop_product =
                  LoadManagedBootstrapStopProduct(
                      config, prepared->bundle->retained_bundle_id(),
                      prepared->bundle->value().cell_graph.graph_revision);
#endif
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              auto reconfigured =
                  stable_cell_runtime_owner->reconfigure_runtime_component_set(
                      std::move(candidate_registrations),
                      std::move(candidate_resolver),
                      std::move(candidate_stop_sources),
                      candidate_stop_product.deadline_budget,
                      request.candidate_projection_revision.retained_bundle_id,
                      request.candidate_projection_revision.graph_revision,
                      request.configuration_fence_id,
                      request.configuration_fence_scope, retained_component_ids);
#else
              auto reconfigured = stable_owner->reconfigure_runtime_component_set(
                  prepared->runtime->participant_registrations,
                  std::move(prepared->runtime->participant_resolver),
                  prepared->runtime->cell_stop_deadline_budget,
                  request.candidate_projection_revision.retained_bundle_id,
                  request.candidate_projection_revision.graph_revision,
                  request.configuration_fence_id,
                  request.configuration_fence_scope,
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
                  retained_component_ids,
                  [&]() -> core::Result<void> {
                    if (stable_stop_observer == nullptr)
                      return core::Result<void>::Ok();
                    return stable_stop_observer->ReplaceSources(
                        std::move(candidate_stop_sources));
                  });
#else
                  {});
#endif
#endif
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected the selected runtime "
                     "participants: " +
                     reconfigured.error().message);
              }
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              if (prepared->runtime != nullptr)
                prepared->runtime->stop_surveillance_owner =
                    stable_stop_surveillance_owner;
#endif
              if (active_managed_composition != nullptr) {
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
                auto &stable_runtime = *active_managed_composition->runtime;
                if (prepared->runtime.get() != &stable_runtime) {
                  prepared->runtime->operation_journal =
                      std::move(stable_runtime.operation_journal);
                  prepared->runtime->addressed_kernel =
                      std::move(stable_runtime.addressed_kernel);
                  const bool old_root_retained = std::any_of(
                      request.component_delta.components.begin(),
                      request.component_delta.components.end(),
                      [&](const auto &row) {
                        return row.controllable_component_id ==
                                   active_managed_composition
                                       ->controllable_component_id &&
                               row.disposition == deployment::
                                                      ApplyComponentDisposition::
                                                          kRetain;
                      });
                  if (!old_root_retained) {
                    stable_runtime.cell_addressed_kernel = nullptr;
                    stable_runtime.cell_runtime_root = nullptr;
                  }
                } else {
                  Require(stable_runtime.active_operation_journal() != nullptr &&
                              stable_runtime.active_addressed_kernel() == stable_owner,
                          "retained native root lost the stable Kernel");
                }
#else
                if (active_managed_composition->runtime != nullptr &&
                    prepared->runtime != active_managed_composition->runtime) {
                  auto &stable_runtime = *active_managed_composition->runtime;
                  const bool old_root_retained = std::any_of(
                      request.component_delta.components.begin(),
                      request.component_delta.components.end(),
                      [&](const auto &row) {
                        return row.controllable_component_id ==
                                   active_managed_composition
                                       ->controllable_component_id &&
                               row.disposition == deployment::
                                                      ApplyComponentDisposition::
                                                          kRetain;
                      });
                  if (!old_root_retained) {
                    stable_runtime.cell_addressed_kernel = nullptr;
                    stable_runtime.cell_operation_journal = nullptr;
                    stable_runtime.cell_runtime_root = nullptr;
                  }
                } else if (prepared->runtime != nullptr &&
                           prepared->runtime ==
                               active_managed_composition->runtime) {
                  auto &stable_runtime = *prepared->runtime;
                  Require(stable_runtime.active_operation_journal() ==
                                  stable_operation_journal &&
                              stable_runtime.active_addressed_kernel() ==
                                  stable_owner,
                          "retained native root lost the stable cell owner");
                }
#endif
                Require(predecessor_engine_snapshot.has_value(),
                        "predecessor engine snapshot is missing");
                retired_registration_engines =
                    std::move(*predecessor_engine_snapshot);
                retired_managed_composition =
                    std::move(active_managed_composition);
              } else {
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
                prepared->runtime->operation_journal =
                    std::move(detached_operation_journal);
                prepared->runtime->addressed_kernel =
                    std::move(detached_addressed_kernel);
                detached_owner_clock.reset();
                detached_owner_scheduler.reset();
#endif
              }
            }
            // The stable Kernel and its registered engines must outlive every
            // fallible initialization below. Retain the candidate under the
            // held fence before those calls; failure must not destroy the
            // admission owner still referenced by the command surface.
            active_managed_composition = std::move(prepared);
            if (active_managed_composition->runtime != nullptr)
              bind_stable_kernel_to_runtime(*active_managed_composition->runtime);
            bind_cell_runtime_to_composition(*active_managed_composition);
            execution_composition.replace_current(active_managed_composition.get());
            execution_composition.addressed_kernel = stable_addressed_kernel();
            stop_recovery_runtime.store(active_managed_composition->runtime.get(),
                                        std::memory_order_release);
            auto &candidate = *active_managed_composition;
            Require(
                candidate.active_addressed_kernel()
                    ->SetRouteRecoveryPending(true)
                    .ok(),
                "selected runtime could not close route recovery admission");
            candidate.route_recovery_pending.store(true,
                                                   std::memory_order_release);
            // Program/Scene-only activation has no changed control domains.
            // There are no candidate configuration facts to stage in that case.
            if (!request.configuration_fence_scope.resources.empty()) {
              auto staged =
                candidate.active_addressed_kernel()
                    ->stage_current_runtime_configuration_facts_under_apply_fence(
                        request.configuration_fence_scope);
              if (!staged.ok()) {
                Fail("stable Kernel rejected candidate configuration staging: " +
                   staged.error().message);
              }
            }
            InstallDirectSceneFrameResolver(projections, candidate);
#if !defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
            if (candidate.runtime != nullptr)
              AttachProductPickPlacePlanningOwner(candidate);
#endif
          }
          execution_composition.replace_current(
              active_managed_composition.get());
          execution_composition.addressed_kernel = stable_addressed_kernel();
          stop_recovery_runtime.store(
              active_managed_composition == nullptr
                  ? nullptr
                  : active_managed_composition->runtime.get(),
              std::memory_order_release);
          installed_under_fence_composition_request = request;
          preparing_composition_request.reset();
        });
    } catch (...) {
      // Preparation was consumed. Preserve the first failure rather than
      // attempting another build on a later observation of this Apply.
      selected_composition_failure = std::current_exception();
      if (prepared != nullptr)
        failed_prepared_composition = std::move(prepared);
      throw;
    }
    require_installed_candidate_configuration();
    if (request.phase == component_manager::
                             ManagerSelectedRuntimeCompositionPhase::kPrepare) {
      return {request,
              component_manager::ManagerSelectedRuntimeCompositionState::
                  kInstalled,
              {}};
    }
    {
      std::lock_guard lock(managed_composition_mutex);
      Require(installed_under_fence_composition_request.has_value() &&
                  *installed_under_fence_composition_request == request,
              "installed runtime composition changed before fence "
              "reconciliation completed");
      installed_component_receipts = read_installed_receipts();
      installed_composition_request = request;
      confirm_installed_projection_set(request);
      publish_installed_liveness_handoff(request);
      installed_under_fence_composition_request.reset();
    }
    try_release_retired_composition();
    return {
        request,
        component_manager::ManagerSelectedRuntimeCompositionState::kInstalled,
        installed_component_receipts};
  };

  const auto handle_manager_liveness = [&](const std::string_view packet) {
    const auto decoded =
        component_manager::detail::DecodeManagerLivenessRpcRequest(packet);
    const auto required = std::find_if(
        decoded.required_set.required_receivers.begin(),
        decoded.required_set.required_receivers.end(), [&](const auto &row) {
          return row.runtime_unit_id.value() == config.runtime_unit_id;
        });
    Require(required != decoded.required_set.required_receivers.end() &&
                decoded.required_set.cell_id.value() == config.cell_id &&
                decoded.receiver_runtime_unit_id == required->runtime_unit_id &&
                required->runtime_epoch.value() == config.runtime_epoch,
            "Manager liveness request names a foreign receiver");
    Require(decoded.policy == deployment::native_execution_runtime_contract::
                                  manager_liveness_policy() &&
                required->runtime_policy_id ==
                    decoded.policy.runtime_policy_id &&
                required->runtime_policy_revision ==
                    decoded.policy.runtime_policy_revision,
            "Manager liveness request differs from retained policy");

    const auto request_matches_publication = [&](const auto &publication) {
      return publication.publication_generation ==
                 decoded.active_publication_generation &&
             publication.request.candidate_retained_bundle_id ==
                 decoded.required_set.active_bundle_id &&
             publication.request.candidate_graph_revision ==
                 decoded.active_graph_revision &&
             publication.durable_public_pointer_content_digest ==
                 decoded.active_public_pointer_digest;
    };
    const auto require_durable_current_publication = [&]() {
      auto publication = read_current_publication();
      if (!request_matches_publication(publication) &&
          decoded.active_publication_generation.value() <
              publication.publication_generation.value()) {
        return std::optional<core::ActiveBundlePublicationReceiptDTO>{};
      }
      Require(request_matches_publication(publication),
              "Manager liveness request has foreign active-publication "
              "identity");
      return std::optional<core::ActiveBundlePublicationReceiptDTO>(
          std::move(publication));
    };

    const core::ManagerLivenessReceiverConfig receiver_config{
        decoded.required_set.cell_id,
        required->runtime_unit_id,
        required->runtime_epoch,
        decoded.required_set.manager_runtime_unit_id,
        decoded.required_set.manager_runtime_epoch,
        decoded.required_set.manager_owner_epoch,
        decoded.policy,
        decoded.required_set.active_bundle_id,
        decoded.required_set.required_runtime_set_digest};
    if (manager_liveness_receiver.get() == nullptr) {
      if (!require_durable_current_publication().has_value()) {
        return component_manager::detail::EncodeManagerLivenessRpcRejection(
            "superseded_active_publication");
      }
      // Initial construction and exact claim settlement own attachment.
      // Their Kernel barrier remains closed through controller readback;
      // this independent lease handler never performs controller I/O.
      const auto installed =
          stable_kernel_owner->ReplaceManagerLivenessRequiredSet(
              decoded.required_set);
      Require(installed.ok(),
              "Kernel rejected the exact Manager-liveness required set");
      manager_liveness_receiver.Install(
          receiver_config,
          [&manager_liveness_admission](
              const core::ManagerLivenessLossIntentDTO &intent) {
            return manager_liveness_admission.CloseAndAttest(intent);
          },
          *stable_kernel_owner, *stable_kernel_owner);
      manager_liveness_installed.store(true, std::memory_order_release);
      manager_liveness_receiver_config = receiver_config;
      manager_liveness_publication_generation =
          decoded.active_publication_generation;
      manager_liveness_public_pointer_digest =
          decoded.active_public_pointer_digest;
      manager_liveness_required_set = decoded.required_set;
    } else {
      Require(manager_liveness_receiver_config.has_value() &&
                  manager_liveness_receiver_config->cell_id ==
                      receiver_config.cell_id &&
                  manager_liveness_receiver_config->receiver_runtime_unit_id ==
                      receiver_config.receiver_runtime_unit_id &&
                  manager_liveness_receiver_config->receiver_runtime_epoch ==
                      receiver_config.receiver_runtime_epoch &&
                  manager_liveness_receiver_config->policy ==
                      receiver_config.policy &&
                  manager_liveness_receiver_config->manager_runtime_unit_id ==
                      receiver_config.manager_runtime_unit_id &&
                  manager_liveness_publication_generation.has_value() &&
                  manager_liveness_public_pointer_digest.has_value(),
              "Manager liveness request changed immutable receiver evidence");
      const bool publication_changed =
          *manager_liveness_publication_generation !=
              decoded.active_publication_generation ||
          *manager_liveness_public_pointer_digest !=
              decoded.active_public_pointer_digest;
      Require(!publication_changed ||
                  (manager_liveness_receiver_config->manager_runtime_epoch ==
                       receiver_config.manager_runtime_epoch &&
                   manager_liveness_receiver_config->manager_owner_epoch ==
                       receiver_config.manager_owner_epoch),
              "Manager liveness publication rebind changed Manager identity");
      if (manager_liveness_receiver_config->manager_runtime_epoch !=
              receiver_config.manager_runtime_epoch ||
          manager_liveness_receiver_config->manager_owner_epoch !=
              receiver_config.manager_owner_epoch) {
        const auto candidate_epoch =
            std::pair{receiver_config.manager_runtime_epoch.value(),
                      receiver_config.manager_owner_epoch.value()};
        Require(!retired_manager_epochs.contains(candidate_epoch),
                "stale Manager epoch cannot replace the current Manager");
        Require(retired_manager_epochs.size() < 64U,
                "Manager epoch replacement history is exhausted");
        retired_manager_epochs.emplace(
            manager_liveness_receiver_config->manager_runtime_epoch.value(),
            manager_liveness_receiver_config->manager_owner_epoch.value());
        const auto replaced =
            manager_liveness_receiver.get()->BeginManagerEpochReplacement(
                receiver_config.manager_runtime_unit_id,
                receiver_config.manager_runtime_epoch,
                receiver_config.manager_owner_epoch);
        Require(replaced.ok(),
                "Manager liveness epoch replacement was rejected");
        manager_liveness_receiver_config->manager_runtime_unit_id =
            receiver_config.manager_runtime_unit_id;
        manager_liveness_receiver_config->manager_runtime_epoch =
            receiver_config.manager_runtime_epoch;
        manager_liveness_receiver_config->manager_owner_epoch =
            receiver_config.manager_owner_epoch;
      }
      if (publication_changed) {
        const auto current_publication = require_durable_current_publication();
        if (!current_publication.has_value()) {
          return component_manager::detail::EncodeManagerLivenessRpcRejection(
              "superseded_active_publication");
        }
        const auto installed_request =
            installed_liveness_handoff.load(std::memory_order_acquire);
        const auto *const named =
            std::get_if<core::NamedApplyPublicationTransactionDTO>(
                &current_publication->request.transaction_ref);
        Require(
            named != nullptr && installed_request != nullptr &&
                installed_request->apply_id == named->apply_id &&
                installed_request->apply_generation ==
                    named->apply_generation &&
                installed_request->candidate_projection_revision ==
                    current_publication->request
                        .candidate_projection_revision &&
                installed_request->runtime_unit_id ==
                    required->runtime_unit_id &&
                installed_request->runtime_epoch == required->runtime_epoch &&
                installed_request->manager_runtime_epoch ==
                    receiver_config.manager_runtime_epoch &&
                installed_request->expected_predecessor_publication_generation
                            .value() +
                        1U ==
                    current_publication->publication_generation.value(),
            "Manager liveness successor lacks exact installed composition "
            "evidence");
        // The exact installed handoff authorizes this lease rebind. The
        // subsequent Apply/authoring claim settlement owns attachment, with
        // ordinary execution closed by the Kernel's route-recovery barrier.
        manager_liveness_admission.CloseForRequiredSetRebind();
        const auto installed =
            stable_kernel_owner->ReplaceManagerLivenessRequiredSet(
                decoded.required_set);
        Require(installed.ok(),
                "Kernel rejected the successor Manager-liveness required set");
        const auto rebound = manager_liveness_receiver.get()->RebindRequiredSet(
            receiver_config.active_bundle_id,
            receiver_config.required_runtime_set_digest);
        Require(rebound.ok(),
                "Manager-liveness receiver rejected the successor required-set "
                "rebind");
        manager_liveness_receiver_config->active_bundle_id =
            receiver_config.active_bundle_id;
        manager_liveness_receiver_config->required_runtime_set_digest =
            receiver_config.required_runtime_set_digest;
        manager_liveness_required_set = decoded.required_set;
        manager_liveness_publication_generation =
            decoded.active_publication_generation;
        manager_liveness_public_pointer_digest =
            decoded.active_public_pointer_digest;
      } else {
        Require(
            manager_liveness_required_set.has_value() &&
                *manager_liveness_required_set == decoded.required_set &&
                manager_liveness_receiver_config->active_bundle_id ==
                    receiver_config.active_bundle_id &&
                manager_liveness_receiver_config->required_runtime_set_digest ==
                    receiver_config.required_runtime_set_digest,
            "Manager liveness request changed required-set identity "
            "without a publication transition");
      }
    }
    const auto admission_generation = manager_liveness_admission.generation();
    const auto renewed =
        manager_liveness_receiver.get()->Renew(decoded.heartbeat);
    Require(renewed.ok(), "Manager heartbeat renewal was rejected");
    if (decoded.reconciliation.has_value() &&
        manager_liveness_receiver.get()->status().command_fenced) {
      const auto reconciled =
          manager_liveness_receiver.get()->CompleteReconciliation(
              *decoded.reconciliation);
      Require(reconciled.ok(), "Manager liveness reconciliation was rejected");
    }
    const auto status = manager_liveness_receiver.get()->status();
    if (status.state == core::ManagerLivenessStateDTO::kCurrent &&
        !status.command_fenced) {
      manager_liveness_admission.OpenIfUnchanged(admission_generation);
    }
    return component_manager::detail::EncodeManagerLivenessRpcResponse(
        core::ManagerLivenessObservationDTO{
            status, decoded.required_set.active_bundle_id,
            decoded.required_set.required_runtime_set_digest,
            core::ComputeManagerLivenessStatusDigest(status)});
  };

  std::mutex manager_liveness_failure_mutex;
  std::exception_ptr manager_liveness_failure;
  const auto rethrow_manager_liveness_failure = [&] {
    std::lock_guard lock(manager_liveness_failure_mutex);
    if (manager_liveness_failure != nullptr) {
      std::rethrow_exception(manager_liveness_failure);
    }
  };
  std::jthread manager_liveness_lane([&](const std::stop_token stop_token) {
    try {
      while (!stop_token.stop_requested()) {
        pollfd descriptor{config.manager_liveness_channel_fd, POLLIN | POLLHUP,
                          0};
        int ready = -1;
        do {
          ready = ::poll(&descriptor, 1U, 100);
        } while (ready < 0 && errno == EINTR && !stop_token.stop_requested());
        Require(ready >= 0, "poll managed runtime liveness channel failed");
        if (ready == 0) {
          continue;
        }
        if ((descriptor.revents & POLLHUP) != 0 &&
            (descriptor.revents & POLLIN) == 0) {
          return;
        }
        Require((descriptor.revents & (POLLERR | POLLNVAL)) == 0,
                "managed runtime liveness channel became invalid");
        const auto packet =
            ReceiveManagedRuntimePacket(config.manager_liveness_channel_fd);
        if (!packet.has_value()) {
          return;
        }
        if (component_manager::detail::IsManagerLivenessRpcPacket(*packet)) {
          SendManagedRuntimePacket(config.manager_liveness_channel_fd,
                                   handle_manager_liveness(*packet));
          continue;
        }
        Require(
            component_manager::detail::IsKernelCellAdmissionRpcPacket(*packet),
            "dedicated Manager-liveness channel received another protocol");
        const auto decoded =
            component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                *packet);
        const bool publish_liveness =
            decoded.command == "cell_admission_publish_liveness" &&
            std::holds_alternative<core::ManagerLivenessObservationDTO>(
                decoded.request);
        const bool update_readiness =
            decoded.command == "cell_admission_update_readiness" &&
            std::holds_alternative<core::CellAdmissionReadinessSnapshotDTO>(
                decoded.request);
        // Only the ordered fail-closed stream belongs on this channel.  All
        // user/authoring admission commands remain on the ordinary channel.
        Require(publish_liveness || update_readiness,
                "dedicated Manager-liveness channel received an ordinary "
                "cell-admission command");
        component_manager::detail::KernelCellAdmissionRpcResponse response;
        try {
          response = component_manager::detail::DispatchKernelCellAdmissionRpc(
              *stable_kernel_owner, decoded);
        } catch (const std::exception &error) {
          response =
              core::ErrorDTO{"cell_admission_request_failed", error.what(), {}};
        } catch (...) {
          response = core::ErrorDTO{"cell_admission_request_failed",
                                    "cell-admission request failed",
                                    {}};
        }
        SendManagedRuntimePacket(
            config.manager_liveness_channel_fd,
            component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
                decoded.command, response));
      }
    } catch (...) {
      {
        std::lock_guard lock(manager_liveness_failure_mutex);
        manager_liveness_failure = std::current_exception();
      }
      static_cast<void>(::shutdown(config.manager_channel_fd, SHUT_RDWR));
    }
  });

  // Keep renewal servicing and captured state alive until every composition
  // builder and execution lane has drained, including exception unwinding.
  // Otherwise reverse declaration order stops renewal before that drain while
  // the receiver's independent watchdog is still armed.
  struct DrainBeforeLivenessShutdown {
    std::array<std::jthread *, 3> workers;
    ManagedProjectionRegistry *projections;
    ~DrainBeforeLivenessShutdown() {
      for (auto *worker : workers)
        worker->request_stop();
      for (auto *worker : workers) {
        if (worker->joinable())
          worker->join();
      }
      projections->Drain();
    }
  } drain_before_liveness_shutdown{{&execution_surface,
                                    &current_composition_builder,
                                    &managed_composition_builder},
                                   &projections};

  while (g_stop_requested == 0) {
    rethrow_manager_liveness_failure();
    rethrow_composition_failure();
    try_start_current_composition();
    try_release_retired_composition();
    pollfd descriptor{config.manager_channel_fd, POLLIN | POLLHUP, 0};
    int ready = -1;
    do {
      ready = ::poll(&descriptor, 1U, 1000);
    } while (ready < 0 && errno == EINTR && g_stop_requested == 0);
    if (g_stop_requested != 0) {
      break;
    }
    if (ready < 0) {
      FailErrno("poll managed runtime parent channel");
    }
    if ((descriptor.revents & POLLHUP) != 0 &&
        (descriptor.revents & POLLIN) == 0) {
      await_manager_loss_containment();
      rethrow_manager_liveness_failure();
      return 0;
    }
    Require((descriptor.revents & (POLLERR | POLLNVAL)) == 0,
            "managed runtime parent channel became invalid");
    if ((descriptor.revents & POLLIN) == 0) {
      continue;
    }
    const auto packet = ReceiveManagedRuntimePacket(config.manager_channel_fd);
    if (!packet.has_value()) {
      rethrow_manager_liveness_failure();
      await_manager_loss_containment();
      return 0;
    }
    const std::string_view request(*packet);
    if (request == "hello") {
      SendManagedRuntimePacket(config.manager_channel_fd, ready_packet);
      continue;
    }
    if (request == "status") {
      Require(source_report_sequence !=
                  std::numeric_limits<std::uint64_t>::max(),
              "managed runtime source sequence is exhausted");
      ++source_report_sequence;
      SendManagedRuntimePacket(
          config.manager_channel_fd,
          "manyforge.native-execution-status.v2 " + config.runtime_unit_id +
              " " + config.runtime_epoch + " " +
              std::to_string(source_report_sequence) + " 1 serving healthy " +
              offer_digest + " " + config.product_identity + " " +
              config.configuration_identity + " " +
              std::string(SelectedExecutionRuntimeRole()) +
              " closed_no_active_effect");
      continue;
    }
    if (request == "shutdown") {
      SendManagedRuntimePacket(config.manager_channel_fd, "shutdown-accepted");
      return 0;
    }
    if (component_manager::detail::IsManagerLivenessRpcPacket(request)) {
      Fail("Manager-liveness protocol is forbidden on the ordinary managed "
           "runtime channel");
    }
    if (component_manager::detail::IsSelectedRuntimeCompositionRpcPacket(
            request)) {
      component_manager::detail::SelectedRuntimeCompositionRpcResponse
          response =
              component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                  core::ErrorDTO{"selected_runtime_composition_request_failed",
                                 "selected runtime-composition request was not "
                                 "dispatched",
                                 {}}};
      try {
        response = install_or_observe_composition(
            component_manager::detail::
                DecodeSelectedRuntimeCompositionRpcRequest(request));
      } catch (const std::exception &error) {
        std::cerr << "native selected composition failed: " << error.what() << '\n';
        response =
            component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                core::ErrorDTO{"selected_runtime_composition_request_failed",
                               error.what(),
                               {}}};
      } catch (...) {
        response =
            component_manager::detail::SelectedRuntimeCompositionRpcFailure{
                core::ErrorDTO{"selected_runtime_composition_request_failed",
                               "selected runtime-composition request failed",
                               {}}};
      }
      SendManagedRuntimePacket(
          config.manager_channel_fd,
          component_manager::detail::
              EncodeSelectedRuntimeCompositionRpcResponse(response));
      continue;
    }
    if (component_manager::detail::IsKernelApplyFenceRpcPacket(request)) {
      component_manager::detail::KernelApplyFenceRpcResponse response =
          component_manager::detail::KernelApplyFenceRpcFailure{
              core::ErrorDTO{"kernel_apply_fence_request_failed",
                             "Kernel Apply-fence request was not dispatched",
                             {}}};
      try {
        const auto decoded =
            component_manager::detail::DecodeKernelApplyFenceRpcRequest(
                request);
        response = std::visit(
            [&](const auto &value)
                -> component_manager::detail::KernelApplyFenceRpcResponse {
              using T = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<
                                T, core::ConfigurationFenceAdmissionQueryDTO>) {
                return fence_owner.ObserveAdmission(value);
              } else if constexpr (
                  std::is_same_v<T, core::ApplyConfigurationFenceIntentDTO>) {
                return fence_owner.Acquire(value);
              } else if constexpr (
                  std::is_same_v<T, core::ApplyFenceAcquireAttemptQueryDTO>) {
                return fence_owner.ReconcileAcquireAttempt(value);
              } else if constexpr (std::is_same_v<
                                       T, core::ApplyFenceRehydrateDTO>) {
                return fence_owner.Rehydrate(value);
              } else if constexpr (
                  std::is_same_v<T, core::ConfigurationFenceStatusQueryDTO>) {
                return fence_owner.QueryStatus(value);
              } else {
                return fence_owner.Release(value);
              }
            },
            decoded);
      } catch (const std::exception &error) {
        response = component_manager::detail::KernelApplyFenceRpcFailure{
            core::ErrorDTO{
                "kernel_apply_fence_request_failed", error.what(), {}}};
      } catch (...) {
        response = component_manager::detail::KernelApplyFenceRpcFailure{
            core::ErrorDTO{"kernel_apply_fence_request_failed",
                           "Kernel Apply-fence request failed",
                           {}}};
      }
      SendManagedRuntimePacket(
          config.manager_channel_fd,
          component_manager::detail::EncodeKernelApplyFenceRpcResponse(
              response));
      continue;
    }
    if (component_manager::detail::IsKernelCellAdmissionRpcPacket(request)) {
      component_manager::detail::KernelCellAdmissionRpcResponse response =
          core::ErrorDTO{"cell_admission_request_failed",
                         "cell-admission request was not dispatched",
                         {}};
      std::string response_command = "cell_admission_get";
      try {
        const auto decoded =
            component_manager::detail::DecodeKernelCellAdmissionRpcRequest(
                request);
        response_command = decoded.command;
        if (PreemptsPlanning(decoded)) {
          PreemptManagedExecutionSurfacePlanning(execution_surface_control);
        }
        core::ResourceAddressedExecutionKernel *owner = nullptr;
        {
          std::lock_guard lock(managed_composition_mutex);
          owner = stable_addressed_kernel();
          Require(owner != nullptr, "stable Kernel is not installed");
          response = component_manager::detail::DispatchKernelCellAdmissionRpc(
              *owner, decoded);
        }
      } catch (const std::exception &error) {
        response =
            core::ErrorDTO{"cell_admission_request_failed", error.what(), {}};
      } catch (...) {
        response = core::ErrorDTO{"cell_admission_request_failed",
                                  "cell-admission request failed",
                                  {}};
      }
      std::string encoded_response;
      try {
        encoded_response =
            component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
                response_command, response);
      } catch (...) {
        encoded_response =
            component_manager::detail::EncodeKernelCellAdmissionRpcResponse(
                response_command,
                core::ErrorDTO{
                    "cell_admission_response_oversized",
                    "cell-admission response exceeds the bounded private "
                    "transport",
                    {}});
      }
      SendManagedRuntimePacket(config.manager_channel_fd, encoded_response);
      continue;
    }
    component_manager::detail::NativeProjectionRpcResponse response;
    try {
      response = projections.Dispatch(
          component_manager::detail::DecodeNativeProjectionRpcRequest(request));
    } catch (const std::exception &error) {
      response = component_manager::detail::NativeProjectionRpcFailure{
          core::ErrorDTO{"native_projection_request_failed", error.what(), {}}};
    } catch (...) {
      response = component_manager::detail::NativeProjectionRpcFailure{
          core::ErrorDTO{"native_projection_request_failed",
                         "native projection request failed",
                         {}}};
    }
    SendManagedRuntimePacket(
        config.manager_channel_fd,
        component_manager::detail::EncodeNativeProjectionRpcResponse(response));
  }
  return 0;
}

/// Admission check for robot-qualified references: each portable target
/// role must resolve through the operation's exact Program Binding to the
/// named controllable component and command target. Locality is checked by the
/// cell frame resolver when the reference is captured.
[[nodiscard]] core::Result<void>
VerifyReferenceTarget(const ProductBackedRuntimeParticipantResolver &resolver,
                      const core::ProgramBindingId &program_binding_id,
                      const core::ReferenceDTO &reference) {
  if (!reference.target.has_value())
    return core::Result<void>::Ok();
  const auto &target = *reference.target;
  const auto bound =
      resolver.ResolveOperationTarget(program_binding_id, target.target_role_id);
  // Both refusals name the failed reference (spec 210 §5.3).
  const std::string frame_role_id(core::reference_identity_value(reference));
  if (!bound.ok())
    return core::Result<void>::Err(core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kMissingReference)),
        "frame reference names a target role absent from the Program Binding",
        {{"reason", "program_reference_target_role_unbound"},
         {"reference_type", "frame_role"},
         {"reference_id", frame_role_id},
         {"target_role_id", target.target_role_id.value()}}});
  if (bound.value().controllable_component_id !=
          target.controllable_component_id ||
      bound.value().command_target_id != target.command_target_id)
    return core::Result<void>::Err(core::ErrorDTO{
        core::kErrorInvalidRequest,
        "frame reference robot differs from its Program Binding target role",
        {{"reason", "program_reference_target_role_mismatch"},
         {"reference_type", "frame_role"},
         {"reference_id", frame_role_id},
         {"target_role_id", target.target_role_id.value()}}});
  return core::Result<void>::Ok();
}

[[nodiscard]] core::Result<void>
VerifyReferenceTargets(const ProductBackedRuntimeParticipantResolver &resolver,
                       const core::OperationIntentDTO &operation) {
  for (const auto &participant : operation.participants) {
    const auto *motion =
        std::get_if<core::MotionParticipantIntentDTO>(&participant.payload);
    if (motion == nullptr)
      continue;
    for (const auto *reference :
         {&motion->target_reference, &motion->offset_reference,
          &motion->goal_frame_reference}) {
      if (!reference->has_value())
        continue;
      if (auto verified = VerifyReferenceTarget(
              resolver, operation.program_binding_id, **reference);
          !verified.ok())
        return verified;
    }
  }
  return core::Result<void>::Ok();
}

/// Pick and approach offset references accept every motion-reference form
/// (spec 475 §3.5); a robot-qualified one passes the same Program-Binding
/// check against the picking motion's binding as a motion reference.
[[nodiscard]] core::Result<void>
VerifyPickReferenceTargets(const ProductBackedRuntimeParticipantResolver &resolver,
                           const core::PickSequencePlanningIntentDTO &intent) {
  for (const auto *operation :
       {&intent.approach_operation, &intent.descend_operation,
        &intent.tool_operation, &intent.retreat_operation}) {
    if (auto verified = VerifyReferenceTargets(resolver, *operation);
        !verified.ok())
      return verified;
  }
  for (const auto *reference : {&intent.synthesis.pick_offset_reference,
                                &intent.synthesis.approach_offset_reference}) {
    if (!reference->has_value())
      continue;
    if (auto verified =
            VerifyReferenceTarget(resolver,
                                  intent.approach_operation.program_binding_id,
                                  **reference);
        !verified.ok())
      return verified;
  }
  return core::Result<void>::Ok();
}

class ExecutionCommandSurface final {
  // The Manager channel's component-lifecycle claim preempts planning as
  // the ordinary socket's does.
  friend void PreemptManagedExecutionSurfacePlanning(
      ManagedExecutionSurfaceControl &control) noexcept;

public:
  ExecutionCommandSurface(
      std::filesystem::path run_root,
      ExecutionCompositionAccess &composition_access,
      component_manager::detail::ManagerLivenessCommandAdmission
          &manager_liveness_admission,
      core::RequestOwnerEpoch owner_epoch)
      : owner_epoch_(std::move(owner_epoch)),
        direct_control_program_run_id_("direct-control-" +
                                       owner_epoch_.value()),
        direct_control_gripper_program_run_id_("direct-control-gripper-" +
                                               owner_epoch_.value()),
        composition_access_(composition_access),
        manager_liveness_admission_(manager_liveness_admission),
        host_lock_(run_root), command_socket_(run_root, kSocketFileName),
        control_socket_(run_root, kControlSocketFileName),
        observation_socket_(run_root, kObservationSocketFileName) {
    if (!lane_wake_) {
      FailErrno("create execution lane wake");
    }
  }

  int Run(const std::stop_token stop_token = {}) {
    const std::stop_callback wake_on_stop(stop_token, [this] { WakeLanes(); });
    std::jthread control_thread(
        [this](std::stop_token stop_token) { RunControlLane(stop_token); });
    std::jthread observation_thread(
        [this](std::stop_token stop_token) { RunObservationLane(stop_token); });
    std::jthread reconciliation_thread([this](std::stop_token stop_token) {
      RunReconciliationLane(stop_token);
    });
    std::jthread submission_ordered_thread(
        [this](std::stop_token stop_token) {
          RunDeferredLane(submission_ordered_lane_, stop_token);
        });
    std::jthread scene_read_thread([this](std::stop_token stop_token) {
      RunDeferredLane(scene_read_lane_, stop_token);
    });
    while (g_stop_requested == 0 && !stop_token.stop_requested() &&
           !reconciliation_lane_failed_.load(std::memory_order_acquire)) {
      short revents = 0;
      int ready = -1;
      do {
        ready = WaitForSocket(command_socket_.get(), revents);
      } while (ready < 0 && errno == EINTR && g_stop_requested == 0 &&
               !stop_token.stop_requested() &&
               !reconciliation_lane_failed_.load(std::memory_order_acquire));
      if (g_stop_requested != 0 || stop_token.stop_requested() ||
          reconciliation_lane_failed_.load(std::memory_order_acquire)) {
        break;
      }
      if (ready < 0) {
        FailErrno("poll execution command socket");
      }
      if ((revents & POLLIN) != 0) {
        HandleOneClient(command_socket_, CommandLane::kOrdinary, stop_token);
      }
      if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        Fail("execution command socket became unavailable");
      }
      if (control_socket_failed_.load(std::memory_order_acquire)) {
        Fail("execution Stop control socket became unavailable");
      }
    }
    reconciliation_thread.request_stop();
    control_thread.request_stop();
    observation_thread.request_stop();
    submission_ordered_thread.request_stop();
    scene_read_thread.request_stop();
    submission_ordered_thread.join();
    scene_read_thread.join();
    reconciliation_thread.join();
    control_thread.join();
    observation_thread.join();
    if (reconciliation_lane_failed_.load(std::memory_order_acquire)) {
      std::exception_ptr failure;
      {
        std::lock_guard lock(reconcile_error_mutex_);
        failure = reconciliation_lane_failure_;
      }
      Require(failure != nullptr,
              "execution reconciliation lane failed without an exception");
      std::rethrow_exception(failure);
    }
    Require(!control_socket_failed_.load(std::memory_order_acquire),
            "execution Stop control socket became unavailable");
    return 0;
  }

private:
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  [[nodiscard]] CanonicalValue
  Ros2ControlLifecycleObservationValue(Composition &composition) const {
    Require(composition.ros2_control_runtime_handle != nullptr &&
                composition.runtime != nullptr &&
                composition.runtime->clock != nullptr,
            "ROS lifecycle status has no selected observer or clock");

    std::vector<core::ControlDomainId> domain_ids;
    domain_ids.reserve(2U);
    std::vector<const core::RuntimeParticipantBinding *> bindings{
        &*composition.direct_control_motion_binding};
    if (composition.direct_control_gripper_binding.has_value())
      bindings.push_back(&*composition.direct_control_gripper_binding);
    for (const auto *binding : bindings) {
      Require(binding->value().control_path.has_value(),
              "ROS lifecycle status binding has no retained control path");
      domain_ids.push_back(binding->value().control_path->control_domain_id);
    }
    std::sort(domain_ids.begin(), domain_ids.end());
    domain_ids.erase(std::unique(domain_ids.begin(), domain_ids.end()),
                     domain_ids.end());
    Require(!domain_ids.empty(),
            "ROS lifecycle status has no retained control domain");

    const auto now_ns = composition.runtime->clock->now();
    bool every_domain_ready = true;
    CanonicalValue::Array domains;
    domains.reserve(domain_ids.size());
    for (const auto &domain_id : domain_ids) {
      const auto sampled =
          composition.ros2_control_runtime_handle->ReadLifecycleSnapshot(
              domain_id, now_ns);
      if (!sampled.ok()) {
        Fail("ROS lifecycle status observation failed: " +
             sampled.error().message);
      }
      const auto &observation = sampled.value();
      Require(observation.locally_valid(),
              "ROS lifecycle status observation is malformed");

      std::optional<bool> controller_configuration_ready;
      std::optional<bool> fault_present;
      std::optional<bool> transport_ready;
      CanonicalValue::Array facts;
      facts.reserve(observation.facts.size());
      for (const auto &fact : observation.facts) {
        Require(fact.locally_valid() &&
                    fact.value.kind ==
                        core::LifecycleFactValueKindDTO::kBoolean &&
                    fact.value.boolean_value.has_value(),
                "ROS lifecycle status fact is not one exact boolean");
        const auto &fact_id = fact.lifecycle_fact_id.value();
        const bool value = *fact.value.boolean_value;
        if (fact_id == "external.controller_configuration_ready") {
          controller_configuration_ready = value;
        } else if (fact_id == "external.fault_present") {
          fault_present = value;
        } else if (fact_id == "external.transport_ready") {
          transport_ready = value;
        }
        facts.emplace_back(CanonicalValue::Object{
            {"lifecycle_fact_id", CanonicalValue(fact_id)},
            {"value", CanonicalValue(value)},
        });
      }
      Require(controller_configuration_ready.has_value() &&
                  fault_present.has_value() && transport_ready.has_value(),
              "ROS lifecycle status omits one retained readiness fact");
      const bool ready = observation.component_state ==
                             core::ComponentLifecycleStateDTO::kActive &&
                         observation.exact_controller_configuration_active &&
                         *controller_configuration_ready && !*fault_present &&
                         *transport_ready;
      every_domain_ready = every_domain_ready && ready;
      domains.emplace_back(CanonicalValue::Object{
          {"component_lifecycle_state",
           CanonicalValue(
               std::string(core::to_string(observation.component_state)))},
          {"control_domain_id", CanonicalValue(domain_id.value())},
          {"exact_controller_configuration_active",
           CanonicalValue(observation.exact_controller_configuration_active)},
          {"facts", CanonicalValue(std::move(facts))},
          {"ready", CanonicalValue(ready)},
          {"source_generation",
           CanonicalValue(static_cast<double>(observation.source_generation))},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"domains", CanonicalValue(std::move(domains))},
        {"ready", CanonicalValue(every_domain_ready)},
        {"schema_version",
         CanonicalValue("manyforge.ros2_control_lifecycle_observation.v1")},
    });
  }

  [[nodiscard]] CanonicalValue
  Ros2ControlTransportDiagnosticValue(Composition &composition) const {
    Require(composition.ros2_control_runtime_handle != nullptr,
            "ROS transport diagnostics have no selected observer");
    const auto sampled = composition.ros2_control_runtime_handle
                             ->gripper_transport_readiness_snapshot();
    Require(sampled.has_value(),
            "ROS transport diagnostics have no bound gripper transport");
    const auto &snapshot = *sampled;
    constexpr std::uint64_t kMaximumSafeUnsignedInteger =
        9'007'199'254'740'991ULL;
    Require(snapshot.feedback_publisher_count <= kMaximumSafeUnsignedInteger &&
                snapshot.feedback_messages_received <=
                    kMaximumSafeUnsignedInteger &&
                snapshot.feedback_samples_matched <=
                    kMaximumSafeUnsignedInteger,
            "ROS transport diagnostic counter exceeds JSON safe integer");

    CanonicalValue::Array reason_codes;
    reason_codes.reserve(snapshot.reason_codes.size());
    for (const auto &reason : snapshot.reason_codes) {
      Require(!reason.empty() && reason.size() <= kMaximumIdentityBytes,
              "ROS transport diagnostic reason is not bounded");
      reason_codes.emplace_back(CanonicalValue(reason));
    }
    return CanonicalValue(CanonicalValue::Object{
        {"command_gate_open", CanonicalValue(snapshot.command_gate_open)},
        {"command_path_ready", CanonicalValue(snapshot.command_path_ready)},
        {"configured", CanonicalValue(snapshot.configured)},
        {"feedback_messages_received",
         CanonicalValue(
             static_cast<double>(snapshot.feedback_messages_received))},
        {"feedback_path_ready", CanonicalValue(snapshot.feedback_path_ready)},
        {"feedback_publisher_count", CanonicalValue(static_cast<double>(
                                         snapshot.feedback_publisher_count))},
        {"feedback_samples_matched", CanonicalValue(static_cast<double>(
                                         snapshot.feedback_samples_matched))},
        {"ready", CanonicalValue(snapshot.ready)},
        {"reason_codes", CanonicalValue(std::move(reason_codes))},
        {"schema_version",
         CanonicalValue("manyforge.ros2_control_transport_diagnostic.v1")},
        {"state_joint_name", CanonicalValue(snapshot.state_joint_name)},
        {"state_topic", CanonicalValue(snapshot.state_topic)},
        {"transport_kind", CanonicalValue(snapshot.transport_kind)},
    });
  }
#endif

  [[nodiscard]] CanonicalValue
  DirectControlTargetValue(Composition &composition) const {
    const auto &binding = composition.direct_control_motion_binding->value();
    const auto &limits = *composition.direct_control_joint_limits;
    Require(
        limits.joint_names.size() == binding.resources.size() &&
            limits.position_lower_rad.size() == binding.resources.size() &&
            limits.position_upper_rad.size() == binding.resources.size() &&
            limits.velocity_max_rad_s.size() == binding.resources.size() &&
            (limits.acceleration_max_rad_s2.empty() ||
             limits.acceleration_max_rad_s2.size() == binding.resources.size()),
        "direct-control joint limits cross their retained target");

    CanonicalValue::Array joints;
    joints.reserve(binding.resources.size());
    for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
      const auto &resource = binding.resources[index];
      Require(resource.source_joint_name.has_value() &&
                  resource.coordinate_kind == core::CoordinateKind::kAngular &&
                  limits.joint_names[index] == resource.compiled_resource_name,
              "direct-control joint limits cross their source-local binding");
      joints.emplace_back(CanonicalValue::Object{
          {"max_acceleration_rad_s2",
           limits.acceleration_max_rad_s2.empty()
               ? CanonicalValue(nullptr)
               : CanonicalValue(limits.acceleration_max_rad_s2[index])},
          {"max_position_rad",
           CanonicalValue(limits.position_upper_rad[index])},
          {"max_velocity_rad_s",
           CanonicalValue(limits.velocity_max_rad_s[index])},
          {"min_position_rad",
           CanonicalValue(limits.position_lower_rad[index])},
          {"source_joint_name", CanonicalValue(*resource.source_joint_name)},
      });
    }
    CanonicalValue::Array frame_roles;
    frame_roles.reserve(binding.frame_bindings.size());
    for (const auto &frame : binding.frame_bindings) {
      frame_roles.emplace_back(frame.frame_role_id.value());
    }
    CanonicalValue::Array planning_capabilities;
    for (const auto &path : binding.supporting_capability_provider_paths) {
      planning_capabilities.emplace_back(path.capability_id.value());
    }
    const auto base_frame =
        std::find_if(binding.frame_bindings.begin(),
                     binding.frame_bindings.end(), [](const auto &frame) {
                       return frame.frame_role_id.value() ==
                              deployment::kDirectControlBaseFrameRoleId;
                     });
    const auto tcp_frame =
        std::find_if(binding.frame_bindings.begin(),
                     binding.frame_bindings.end(), [](const auto &frame) {
                       return frame.frame_role_id.value() ==
                              deployment::kDirectControlTcpFrameRoleId;
                     });
    Require(base_frame != binding.frame_bindings.end() &&
                tcp_frame != binding.frame_bindings.end(),
            "direct-control status has no exact base/TCP frame binding");
    return CanonicalValue(CanonicalValue::Object{
        {"command_target_id",
         CanonicalValue(binding.command_target_id.value())},
        {"base_frame_role_id", CanonicalValue(std::string(
                                   deployment::kDirectControlBaseFrameRoleId))},
        {"component_role_id",
         CanonicalValue(binding.component_role_id.value())},
        {"controllable_component_id",
         CanonicalValue(binding.controllable_component_id.value())},
        {"frame_role_ids", CanonicalValue(std::move(frame_roles))},
        {"joints", CanonicalValue(std::move(joints))},
        {"operation_capability_id",
         CanonicalValue(binding.capability_id.value())},
        {"planning_capability_ids",
         CanonicalValue(std::move(planning_capabilities))},
        {"program_binding_id",
         CanonicalValue(binding.program_binding_id.value())},
        {"program_id", CanonicalValue(binding.program_id.value())},
        {"program_revision", CanonicalValue(binding.program_revision.value())},
        {"program_run_id",
         CanonicalValue(direct_control_program_run_id_.value())},
        {"target_role_id", CanonicalValue(binding.target_role_id.value())},
        {"target_descriptor", composition.direct_control_motion_descriptor},
        {"tcp_frame_role_id",
         CanonicalValue(std::string(deployment::kDirectControlTcpFrameRoleId))},
    });
  }

  struct ResolvedObservationOwner final {
    Composition *composition;
    std::shared_ptr<RetainedObservedStateChannel> channel;
    core::RuntimeParticipantBinding physical;
    ResolvedTargetObservation resolved;
  };

  [[nodiscard]] std::optional<ResolvedObservationOwner>
  ResolveObservationOwner(const core::ProgramBindingId &program_binding_id,
                          const core::TargetRoleId &target_role_id,
                          const core::CapabilityId &capability_id,
                          const bool motion, Composition &composition) const {
    std::optional<ResolvedObservationOwner> match;
    bool ambiguous = false;
    const auto inspect = [&](Composition &candidate) {
      const auto channel =
          motion ? (candidate.runtime != nullptr
                        ? candidate.runtime->motion_state_channel
                        : nullptr)
                 : (candidate.runtime != nullptr
                        ? candidate.runtime->gripper_state_channel
                        : candidate.gripper_runtime != nullptr
                              ? std::dynamic_pointer_cast<
                                    RetainedObservedStateChannel>(
                                    candidate.gripper_runtime->state_channel())
                              : nullptr);
      if (!channel)
        return;
      const auto physical = channel->binding();
      if (!physical.value().control_path.has_value())
        return;
      std::vector<core::RuntimeParticipantBinding> representatives;
      if (candidate.runtime != nullptr) {
        const auto configuration =
            candidate.runtime->ReadCurrentPublishedRouteConfiguration(
                physical.value().control_path->control_domain_id);
        if (!configuration.ok() || !configuration.value())
          return;
        const auto selected = candidate.runtime->ReadCurrentPhysicalParticipants(
            *configuration.value());
        if (!selected.ok())
          return;
        representatives = selected.value();
      } else {
        const auto route = candidate.active_addressed_kernel()
                               ->current_runtime_route_configuration(
                                   physical.value().control_path->control_domain_id);
        if (!route.ok() || !route.value().has_value() ||
            !route.value()->operation_admission_open ||
            route.value()->active_configuration
                    .active_control_configuration_generation !=
                physical.value()
                    .control_path->active_control_configuration_generation ||
            route.value()->active_configuration
                    .active_control_configuration_id !=
                physical.value().control_path->active_control_configuration_id)
          return;
        representatives.push_back(physical);
      }
      const auto resolved =
          composition.target_resolver->ResolveObservationTarget(
              program_binding_id, target_role_id, capability_id, physical,
              representatives);
      if (!resolved.ok())
        return;
      if (match.has_value()) {
        ambiguous = true;
        return;
      }
      match.emplace(ResolvedObservationOwner{&candidate, channel, physical,
                                             resolved.value()});
    };
    inspect(composition);
    for (const auto &participant : composition.participant_compositions)
      inspect(*participant);
    return ambiguous ? std::nullopt : match;
  }

  [[nodiscard]] Composition *
  FindBindingComposition(const core::RuntimeParticipantBindingDTO &binding,
                         Composition &composition) const {
    Composition *match = nullptr;
    bool ambiguous = false;
    const auto inspect = [&](Composition &candidate) {
      const auto same_target = [&](const auto &owned) {
        return owned.value().controllable_component_id ==
                   binding.controllable_component_id &&
               owned.value().command_target_id == binding.command_target_id;
      };
      const bool owns_target =
          (candidate.direct_control_motion_binding.has_value() &&
           same_target(*candidate.direct_control_motion_binding)) ||
          (candidate.direct_control_gripper_binding.has_value() &&
           same_target(*candidate.direct_control_gripper_binding));
      if (!owns_target)
        return;
      if (match != nullptr) {
        ambiguous = true;
        return;
      }
      match = &candidate;
    };
    inspect(composition);
    for (const auto &participant : composition.participant_compositions)
      inspect(*participant);
    return ambiguous ? nullptr : match;
  }

  [[nodiscard]] core::Result<core::GripperCommandStatusDTO>
  ReadCurrentGripperStatus(
      Composition &component,
      const core::RuntimeParticipantBinding &observation_binding) const {
    if (component.runtime != nullptr)
      return component.runtime->ReadGripperStatus(observation_binding);
    if (component.gripper_runtime == nullptr ||
        component.gripper_runtime->backend() == nullptr ||
        observation_binding.value().controllable_component_id !=
            component.controllable_component_id ||
        observation_binding.value().command_target_id !=
            component.gripper_runtime->binding().value().command_target_id) {
      return core::Result<core::GripperCommandStatusDTO>::Err(core::ErrorDTO{
          "gripper_feedback_unavailable",
          "target has no exact retained gripper feedback owner", {}});
    }
    try {
      return core::Result<core::GripperCommandStatusDTO>::Ok(
          component.gripper_runtime->backend()->get_gripper_status(
              observation_binding.value().command_target_id.value()));
    } catch (const std::exception &exception) {
      return core::Result<core::GripperCommandStatusDTO>::Err(core::ErrorDTO{
          "gripper_feedback_unavailable", exception.what(), {}});
    }
  }

  [[nodiscard]] CanonicalValue
  FrameTransformResponse(const TargetObservationQuery &query,
                         Composition &composition) const {
    const auto &transform = *query.frame_transform;
    for (const auto *reference :
         {&transform.source_frame, &transform.target_frame}) {
      const auto verified = VerifyReferenceTarget(
          *composition.target_resolver, query.program_binding_id, *reference);
      if (!verified.ok())
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(verified.error())},
            {"ok", CanonicalValue(false)},
        });
    }
    if (composition.scene_frame_resolver == nullptr)
      return ErrorResponse("execution_frame_transform_unavailable",
                           "cell has no installed frame resolver");
    std::optional<core::CollisionWorldLeaseInventory> inventory;
    if (composition.planner_scene_projection != nullptr) {
      inventory = composition.planner_scene_projection
                      ->current_scene_snapshot_lease_inventory();
    } else if (composition.runtime != nullptr &&
               composition.runtime->collision_publisher != nullptr) {
      inventory = composition.runtime->collision_publisher
                      ->current_scene_snapshot_lease_inventory();
    }
    if (!inventory.has_value() || !inventory->logical_scene)
      return ErrorResponse("execution_frame_transform_unavailable",
                           "cell has no current retained scene projection");
    const auto now_ns = composition.cell_runtime != nullptr
                            ? composition.cell_runtime->clock()->now()
                        : composition.runtime != nullptr
                            ? composition.runtime->clock->now()
                            : std::int64_t{-1};
    const auto resolved = composition.scene_frame_resolver->ResolveFrameTransform(
        RetainedSceneFrameTransformQuery{transform.pose, transform.source_frame,
                                         transform.target_frame,
                                         inventory->projection_binding},
        inventory->logical_scene, now_ns);
    if (!resolved.ok())
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(resolved.error())},
          {"ok", CanonicalValue(false)},
      });
    CanonicalValue::Array evidence;
    for (const auto &digest : resolved.value().resolution_evidence_digests)
      evidence.emplace_back(digest.value());
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("query_target")},
        {"ok", CanonicalValue(true)},
        {"target_observation",
         CanonicalValue(CanonicalValue::Object{
             {"evaluated_at_ns",
              content_identity::nonnegative_int64_value(
                  resolved.value().evaluated_at_ns)},
             {"kind", CanonicalValue("frame_transform")},
             {"owner_epoch", CanonicalValue(owner_epoch_.value())},
             {"pose", transform.pose_value},
             {"program_binding_id",
              CanonicalValue(query.program_binding_id.value())},
             {"resolution_evidence_digests", CanonicalValue(std::move(evidence))},
             {"scene_revision",
              SceneRevisionValue(resolved.value()
                                     .projection_binding.projection_revision
                                     .scene_revision)},
             {"schema_version",
              CanonicalValue("manyforge.target_observation.v1")},
             {"source_frame", transform.source_frame_value},
             {"target_frame", transform.target_frame_value},
             {"value", PoseValue(resolved.value().pose_in_target_frame)},
         })},
    });
  }

  [[nodiscard]] CanonicalValue
  QueryTargetResponse(const TargetObservationQuery &query,
                      Composition &composition) const {
    if (query.owner_epoch != owner_epoch_)
      return ErrorResponse("execution_target_owner_stale",
                           "target query names a replaced owner");
    if (query.frame_transform.has_value())
      return FrameTransformResponse(query, composition);
    const bool frame_query = query.frame_role_id.has_value();
    const auto capability_id = core::CapabilityId(
        frame_query                ? "model.kinematics.fk"
        : query.grasp_confirmation ? "tool.gripper.confirmation.operator"
                                   : "tool.gripper.feedback");
    const auto owner =
        ResolveObservationOwner(query.program_binding_id, query.target_role_id,
                                capability_id, frame_query, composition);
    if (!owner.has_value())
      return ErrorResponse("execution_target_observation_unavailable",
                           "target has no unique qualified observation owner");
    auto &selected = *owner->composition;
    const auto &channel = owner->channel;
    const auto &observed_binding = owner->physical;
    const auto &binding = observed_binding.value();
    const auto &resolved = owner->resolved;
    const auto &target = resolved.target.target_role();
    if (query.grasp_confirmation) {
      if (!selected.operator_confirmation_validity_ns)
        return ErrorResponse(
            "grasp_confirmation_unavailable",
            "target has no explicit operator confirmation policy");
      const auto current = ReadCurrentGripperStatus(
          selected, resolved.observation_binding);
      if (!current.ok())
        return ErrorResponse(current.error().code, current.error().message);
      const auto &status = current.value();
      if (status.command_id == 0U || status.revision == 0U)
        return ErrorResponse("gripper_command_unavailable",
                             "confirmation requires a current gripper command");
      const auto &source = *resolved.observation_binding.value().control_path;
      const bool fresh =
          status.grasp_evidence &&
          core::gripper_grasp_evidence_is_fresh(*status.grasp_evidence,
                                                selected.cell_runtime != nullptr
                                                    ? selected.cell_runtime->clock()->now()
                                                    : selected.runtime->clock->now());
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("query_target")},
          {"ok", CanonicalValue(true)},
          {"target_observation",
           CanonicalValue(CanonicalValue::Object{
               {"schema_version",
                CanonicalValue("manyforge.target_observation.v1")},
               {"kind", CanonicalValue("gripper_confirmation")},
               {"owner_epoch", CanonicalValue(owner_epoch_.value())},
               {"program_binding_id",
                CanonicalValue(query.program_binding_id.value())},
               {"target_role_id", CanonicalValue(query.target_role_id.value())},
               {"source_epoch",
                CanonicalValue(source.provider_runtime_epoch.value())},
               {"source_sequence",
                CanonicalValue(static_cast<double>(status.revision))},
               {"value",
                CanonicalValue(CanonicalValue::Object{
                    {"state", CanonicalValue(core::gripper_command_state_name(
                                  status.state))},
                    {"command_id",
                     CanonicalValue(static_cast<double>(status.command_id))},
                    {"generation",
                     CanonicalValue(static_cast<double>(status.generation))},
                    {"revision",
                     CanonicalValue(static_cast<double>(status.revision))},
                    {"source_id",
                     CanonicalValue(source.control_binding_id.value())},
                    {"source_revision",
                     CanonicalValue(source.control_binding_revision.value())},
                    {"source_epoch",
                     CanonicalValue(source.provider_runtime_epoch.value())},
                    {"active_control_configuration_id",
                     CanonicalValue(
                         source.active_control_configuration_id.value())},
                    {"active_control_configuration_generation",
                     CanonicalValue(static_cast<double>(
                         source.active_control_configuration_generation
                             .value()))},
                    {"grasp_confirmed",
                     fresh ? CanonicalValue(
                                 status.grasp_evidence->observation.confirmed)
                           : CanonicalValue(nullptr)},
                    {"evidence",
                     status.grasp_evidence
                         ? GraspEvidenceValue(*status.grasp_evidence)
                         : CanonicalValue(nullptr)},
                })},
           })},
      });
    }
    const auto observed = channel->current_state(
        selected.cell_runtime != nullptr ? selected.cell_runtime->clock()->now()
                                         : selected.runtime->clock->now());
    if (!observed.ok())
      return ErrorResponse(observed.error().code, observed.error().message);
    const auto &state = observed.value();
    std::optional<core::ActiveControlConfigurationDTO> current_configuration;
    if (selected.runtime != nullptr) {
      const auto read = selected.runtime->ReadCurrentPublishedRouteConfiguration(
          state.control_domain_id);
      if (read.ok())
        current_configuration = read.value();
    } else {
      const auto read = selected.active_addressed_kernel()
                            ->current_runtime_route_configuration(
                                state.control_domain_id);
      if (read.ok() && read.value().has_value() &&
          read.value()->operation_admission_open)
        current_configuration = read.value()->active_configuration;
    }
    if (!current_configuration.has_value() ||
        !target_observation_source_is_current(resolved.observation_binding,
                                              state,
                                              *current_configuration))
      return ErrorResponse("execution_target_observation_unavailable",
                           "selected read source is no longer reconciled");
    Require(state.complete &&
                state.controllable_component_id ==
                    binding.controllable_component_id &&
                state.command_target_id == binding.command_target_id &&
                state.resources.size() == binding.resources.size() &&
                state.joint_names.size() == binding.resources.size() &&
                state.positions.size() == binding.resources.size(),
            "target query observation crosses its retained binding");
    std::vector<double> positions;
    positions.reserve(binding.resources.size());
    for (std::size_t index = 0; index < binding.resources.size(); ++index) {
      const auto &resource = binding.resources[index];
      Require(state.resources[index] == resource.resource_id &&
                  state.joint_names[index] == resource.compiled_resource_name &&
                  core::coordinate_kind(state.positions[index]) ==
                      resource.coordinate_kind,
              "target query observation has mismatched coordinates");
      const auto *angular =
          std::get_if<core::AngularPositionDTO>(&state.positions[index]);
      const auto *linear =
          std::get_if<core::LinearPositionDTO>(&state.positions[index]);
      Require(angular != nullptr || linear != nullptr,
              "target query observation has unsupported coordinate units");
      const double value = angular ? angular->value_rad : linear->value_m;
      Require(std::isfinite(value), "target query observation is not finite");
      positions.push_back(value);
    }
    CanonicalValue value;
    if (frame_query) {
      if (selected.runtime == nullptr ||
          selected.runtime->kinematic_model == nullptr)
        return ErrorResponse(
            "execution_target_frame_unavailable",
            "target has no native kinematics owner for frame observation");
      const auto frame =
          std::find_if(target.frame_role_bindings.begin(),
                       target.frame_role_bindings.end(), [&](const auto &row) {
                         return row.frame_role_id == *query.frame_role_id;
                       });
      if (frame == target.frame_role_bindings.end())
        return ErrorResponse(
            "execution_target_frame_unavailable",
            "frame role is not declared by the target binding");
      if (!composition.target_resolver->FrameObservationCovered(
              *frame, observed_binding))
        return ErrorResponse(
            "execution_target_frame_state_unavailable",
            "frame depends on coordinates outside the qualified observation");
      std::string base_frame;
      if (query.relative_to_frame_role_id.has_value()) {
        const auto base = std::find_if(
            target.frame_role_bindings.begin(),
            target.frame_role_bindings.end(), [&](const auto &row) {
              return row.frame_role_id == *query.relative_to_frame_role_id;
            });
        if (base == target.frame_role_bindings.end())
          return ErrorResponse(
              "execution_target_frame_unavailable",
              "relative frame role is not declared by the target binding");
        if (!composition.target_resolver->FrameObservationCovered(
                *base, observed_binding))
          return ErrorResponse("execution_target_frame_state_unavailable",
                               "relative frame depends on coordinates outside "
                               "the qualified observation");
        base_frame = base->compiled_frame_name;
      }
      // Empty base retains the existing logical-cell-frame query semantics.
      const auto pose =
          selected.runtime->kinematic_model->forward_kinematics(core::FkRequestDTO{
              core::RobotStateDTO{state.joint_names, std::move(positions), {}},
              frame->compiled_frame_name, std::move(base_frame)});
      if (!pose.ok())
        return ErrorResponse(pose.error().code, pose.error().message);
      value = PoseValue(pose.value());
    } else {
      if ((selected.runtime == nullptr || !selected.runtime->gripper) &&
          selected.gripper_runtime == nullptr)
        return ErrorResponse("gripper_feedback_unavailable",
                             "target has no qualified gripper feedback");
      if (binding.resources.size() != 1U)
        return ErrorResponse("gripper_feedback_unavailable",
                             "target has no qualified gripper feedback");
      const auto current = ReadCurrentGripperStatus(
          selected, resolved.observation_binding);
      if (!current.ok())
        return ErrorResponse(current.error().code, current.error().message);
      const auto &status = current.value();
      const auto &source = *resolved.observation_binding.value().control_path;
      value = CanonicalValue(CanonicalValue::Object{
          {"source_id", CanonicalValue(source.control_binding_id.value())},
          {"source_revision",
           CanonicalValue(source.control_binding_revision.value())},
          {"source_epoch",
           CanonicalValue(source.provider_runtime_epoch.value())},
          {"active_control_configuration_id",
           CanonicalValue(source.active_control_configuration_id.value())},
          {"active_control_configuration_generation",
           CanonicalValue(static_cast<double>(
               source.active_control_configuration_generation.value()))},
          {"command_id",
           CanonicalValue(static_cast<double>(status.command_id))},
          {"generation",
           CanonicalValue(static_cast<double>(status.generation))},
          // No command echo or unqualified evidence label proves a grasp.
          {"grasp_confirmed", CanonicalValue(nullptr)},
          {"observed_position",
           CoordinatePositionValue(state.positions.front())},
          {"revision", CanonicalValue(static_cast<double>(status.revision))},
          {"state",
           CanonicalValue(core::gripper_command_state_name(status.state))},
      });
    }
    CanonicalValue::Object result{
        {"kind", CanonicalValue(frame_query ? "frame_pose" : "gripper_status")},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"program_binding_id",
         CanonicalValue(query.program_binding_id.value())},
        {"schema_version", CanonicalValue("manyforge.target_observation.v1")},
        {"source_epoch", CanonicalValue(state.source_epoch.value())},
        {"source_sequence",
         CanonicalValue(static_cast<double>(state.sequence))},
        {"target_role_id", CanonicalValue(query.target_role_id.value())},
        {"value", std::move(value)},
    };
    if (frame_query)
      result.emplace_back("frame_role_id",
                          CanonicalValue(query.frame_role_id->value()));
    if (query.relative_to_frame_role_id.has_value())
      result.emplace_back(
          "relative_to_frame_role_id",
          CanonicalValue(query.relative_to_frame_role_id->value()));
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("query_target")},
        {"ok", CanonicalValue(true)},
        {"target_observation", CanonicalValue(std::move(result))},
    });
  }

  [[nodiscard]] static CanonicalValue
  GraspEvidenceValue(const core::GripperGraspEvidenceDTO &evidence) {
    const auto &accepted = evidence.observation;
    return CanonicalValue(CanonicalValue::Object{
        {"source_kind", CanonicalValue("operator")},
        {"source_id", CanonicalValue(evidence.source_id.value())},
        {"source_revision", CanonicalValue(evidence.source_revision.value())},
        {"source_epoch", CanonicalValue(evidence.source_epoch.value())},
        {"observation_id", CanonicalValue(accepted.observation_id)},
        {"attribution", CanonicalValue(accepted.attribution)},
        {"command_id",
         CanonicalValue(static_cast<double>(accepted.command_id))},
        {"generation",
         CanonicalValue(static_cast<double>(accepted.generation))},
        {"confirmed", CanonicalValue(accepted.confirmed)},
        {"observed_at_ns",
         content_identity::nonnegative_int64_value(accepted.observed_at_ns)},
        {"expires_at_ns",
         content_identity::nonnegative_int64_value(evidence.expires_at_ns)},
    });
  }

  [[nodiscard]] CanonicalValue
  ConfirmGraspResponse(const GripperConfirmationCommand &command,
                       Composition &composition) const {
    if (command.context.request_owner_epoch != owner_epoch_)
      return ErrorResponse("stale_request_owner_epoch",
                           "confirmation names a replaced native owner");
    const auto owner = ResolveObservationOwner(
        command.program_binding_id, command.target_role_id,
        core::CapabilityId("tool.gripper.confirmation.operator"), false,
        composition);
    if (!owner.has_value())
      return ErrorResponse("grasp_confirmation_unavailable",
                           "target has no unique qualified gripper owner");
    auto &selected = *owner->composition;
    if (!selected.operator_confirmation_validity_ns || !owner->channel)
      return ErrorResponse(
          "grasp_confirmation_unavailable",
          "target has no explicitly configured operator source");
    const auto &physical = owner->physical;
    const auto &source = owner->resolved;
    auto observation = command.observation;
    observation.gripper_id = physical.value().command_target_id.value();
    const auto recorded = selected.runtime != nullptr
                              ? selected.runtime->RecordGraspObservation(
                                    source.observation_binding, observation,
                                    *selected.operator_confirmation_validity_ns,
                                    command.admission_deadline_ns)
                              : selected.gripper_runtime->backend()
                                    ->record_grasp_observation(
                                        source.observation_binding, observation,
                                        *selected.operator_confirmation_validity_ns,
                                        command.admission_deadline_ns);
    if (!recorded.ok())
      return ErrorResponse(recorded.error().code, recorded.error().message);
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("gripper_confirm")},
        {"ok", CanonicalValue(true)},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"program_binding_id",
         CanonicalValue(command.program_binding_id.value())},
        {"target_role_id", CanonicalValue(command.target_role_id.value())},
        {"confirmation", GraspEvidenceValue(recorded.value())},
    });
  }

  [[nodiscard]] CanonicalValue
  QueryOperationResponse(const ProgramOperationQuery &query,
                         Composition &composition) const {
    if (query.owner_epoch != owner_epoch_)
      return ErrorResponse("stale_request_owner_epoch",
                           "operation query names another native owner");
    const auto target = composition.target_resolver->ResolveOperationTarget(
        query.program_binding_id, query.target_role_id);
    if (!target.ok())
      return ErrorResponse(target.error().code, target.error().message);
    // This is one bounded immutable owner capture, not a new operation index.
    const auto operations =
        composition.active_addressed_kernel()->get_resident_operations(
            {query.program_run_id});
    const core::OperationStateDTO *selected = nullptr;
    for (const auto &operation : operations) {
      if (operation.program_binding_id != query.program_binding_id)
        continue;
      const auto matches = std::count_if(
          operation.participants.begin(), operation.participants.end(),
          [&](const auto &participant) {
            return participant.controllable_component_id ==
                       target.value().controllable_component_id &&
                   participant.command_target_id ==
                       target.value().command_target_id;
          });
      if (matches == 0)
        continue;
      if (selected || operation.participants.size() != 1U)
        return ErrorResponse(
            "execution_operation_context_ambiguous",
            "target does not select one exact operation scope");
      selected = &operation;
    }
    if (!selected)
      return ErrorResponse(
          "execution_operation_context_unavailable",
          "target has no resident operation in the exact Program run");
    CanonicalValue scale_value;
    if (selected->runtime_scale_generation.has_value()) {
      const auto scale =
          composition.active_addressed_kernel()->get_runtime_scale(
              selected->operation_id);
      if (!scale.ok())
        return ErrorResponse(scale.error().code, scale.error().message);
      if (scale.value().operation_revision != selected->operation_revision ||
          scale.value().runtime_scale_generation !=
              *selected->runtime_scale_generation)
        return ErrorResponse("execution_operation_observation_changed",
                             "operation changed during scale observation");
      scale_value = CanonicalValue(CanonicalValue::Object{
          {"generation", CanonicalValue(static_cast<double>(
                             scale.value().runtime_scale_generation.value()))},
          {"state",
           CanonicalValue(std::string(core::to_string(scale.value().state)))},
          {"requested_scale", CanonicalValue(scale.value().requested_scale)},
          {"effective_scale", CanonicalValue(scale.value().effective_scale)},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("query_operation")},
        {"ok", CanonicalValue(true)},
        {"operation", OperationStateValue(*selected)},
        {"runtime_scale", std::move(scale_value)},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"target_role_id", CanonicalValue(query.target_role_id.value())},
    });
  }

  [[nodiscard]] CanonicalValue DirectControlObservedValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO &observed_state) const {
    const auto &binding = composition.direct_control_motion_binding->value();
    Require(observed_state.complete &&
                observed_state.resources.size() == binding.resources.size() &&
                observed_state.joint_names.size() == binding.resources.size() &&
                observed_state.positions.size() == binding.resources.size(),
            "direct-control observation crosses its retained target");
    CanonicalValue::Array source_joint_names;
    CanonicalValue::Array observed_positions;
    std::vector<double> observed_positions_rad;
    source_joint_names.reserve(binding.resources.size());
    observed_positions.reserve(binding.resources.size());
    observed_positions_rad.reserve(binding.resources.size());
    for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
      const auto &resource = binding.resources[index];
      const auto *position = std::get_if<core::AngularPositionDTO>(
          &observed_state.positions[index]);
      Require(resource.source_joint_name.has_value() &&
                  resource.coordinate_kind == core::CoordinateKind::kAngular &&
                  observed_state.resources[index] == resource.resource_id &&
                  observed_state.joint_names[index] ==
                      resource.compiled_resource_name &&
                  position != nullptr && std::isfinite(position->value_rad),
              "direct-control observation crosses its source-local binding");
      source_joint_names.emplace_back(*resource.source_joint_name);
      observed_positions.emplace_back(position->value_rad);
      observed_positions_rad.push_back(position->value_rad);
    }
    const auto base_frame =
        std::find_if(binding.frame_bindings.begin(),
                     binding.frame_bindings.end(), [](const auto &frame) {
                       return frame.frame_role_id.value() ==
                              deployment::kDirectControlBaseFrameRoleId;
                     });
    const auto tcp_frame =
        std::find_if(binding.frame_bindings.begin(),
                     binding.frame_bindings.end(), [](const auto &frame) {
                       return frame.frame_role_id.value() ==
                              deployment::kDirectControlTcpFrameRoleId;
                     });
    Require(base_frame != binding.frame_bindings.end() &&
                tcp_frame != binding.frame_bindings.end(),
            "direct-control observation has no exact base/TCP frame binding");
    const auto current_tcp =
        composition.runtime->kinematic_model->forward_kinematics(core::FkRequestDTO{
            core::RobotStateDTO{observed_state.joint_names,
                                std::move(observed_positions_rad),
                                {}},
            tcp_frame->compiled_frame_name, base_frame->compiled_frame_name});
    if (!current_tcp.ok()) {
      Fail("direct-control current TCP pose is unavailable: " +
           current_tcp.error().message);
    }
    return CanonicalValue(CanonicalValue::Object{
        {"complete", CanonicalValue(true)},
        {"current_tcp_pose", PoseValue(current_tcp.value())},
        {"positions_rad", CanonicalValue(std::move(observed_positions))},
        {"sequence",
         CanonicalValue(static_cast<double>(observed_state.sequence))},
        {"source_epoch", CanonicalValue(observed_state.source_epoch.value())},
        {"source_joint_names", CanonicalValue(std::move(source_joint_names))},
    });
  }

  [[nodiscard]] CanonicalValue DirectControlStatusValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO &observed_state) const {
    auto fields = DirectControlTargetValue(composition).as_object();
    fields.emplace_back("observed_state", DirectControlObservedValue(
                                              composition, observed_state));
    return CanonicalValue(std::move(fields));
  }

  [[nodiscard]] CanonicalValue
  DirectControlGripperTargetValue(Composition &composition) const {
    const auto &binding = composition.direct_control_gripper_binding->value();
    Require(binding.resources.size() == 1U,
            "direct-control gripper status has no exact scalar resource");
    const auto &resource = binding.resources.front();
    const bool model_backed = resource.source_joint_name.has_value();
    const auto limits = composition.runtime != nullptr
                            ? composition.runtime->gripper_position_limits
                            : composition.gripper_runtime->position_limits();
    Require(binding.command_schema.has_value() &&
                model_backed == limits.has_value(),
            "direct-control gripper status crosses its retained binding");
    Require(!limits.has_value() ||
                (limits->coordinate_kind == resource.coordinate_kind &&
                 std::isfinite(limits->lower_position) &&
                 std::isfinite(limits->upper_position) &&
                 limits->lower_position <= limits->upper_position),
            "direct-control gripper status has inconsistent units or limits");
    const auto coordinate_kind =
        resource.coordinate_kind == core::CoordinateKind::kAngular
            ? "angular_rad"
            : "linear_m";
    const auto &source_coordinate_name = model_backed
                                             ? *resource.source_joint_name
                                             : resource.compiled_resource_name;
    return CanonicalValue(CanonicalValue::Object{
        {"command_schema_id",
         CanonicalValue(binding.command_schema->command_schema_id.value())},
        {"command_schema_revision",
         CanonicalValue(
             binding.command_schema->command_schema_revision.value())},
        {"command_target_id",
         CanonicalValue(binding.command_target_id.value())},
        {"component_role_id",
         CanonicalValue(binding.component_role_id.value())},
        {"controllable_component_id",
         CanonicalValue(binding.controllable_component_id.value())},
        {"operation_capability_id",
         CanonicalValue(binding.capability_id.value())},
        {"program_binding_id",
         CanonicalValue(binding.program_binding_id.value())},
        {"program_id", CanonicalValue(binding.program_id.value())},
        {"program_revision", CanonicalValue(binding.program_revision.value())},
        {"program_run_id",
         CanonicalValue(direct_control_gripper_program_run_id_.value())},
        {"resource",
         CanonicalValue(CanonicalValue::Object{
             {"coordinate_kind", CanonicalValue(coordinate_kind)},
             {"max_position", limits.has_value()
                                  ? CanonicalValue(limits->upper_position)
                                  : CanonicalValue(nullptr)},
             {"min_position", limits.has_value()
                                  ? CanonicalValue(limits->lower_position)
                                  : CanonicalValue(nullptr)},
             {"source_coordinate_name", CanonicalValue(source_coordinate_name)},
             {"source_kind",
              CanonicalValue(model_backed ? "model_role" : "component_local")},
         })},
        {"target_role_id", CanonicalValue(binding.target_role_id.value())},
        {"target_descriptor", composition.direct_control_gripper_descriptor},
    });
  }

  [[nodiscard]] CanonicalValue DirectControlGripperObservedValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO *observed_state) const {
    const auto &binding = composition.direct_control_gripper_binding->value();
    Require(binding.resources.size() == 1U,
            "direct-control gripper observation has no exact scalar resource");
    const auto &resource = binding.resources.front();
    const bool model_backed = resource.source_joint_name.has_value();
    if (observed_state == nullptr) {
      Require(!model_backed, "model-backed gripper observation is unavailable");
      return CanonicalValue(nullptr);
    }
    Require(observed_state->complete &&
                observed_state->resources.size() == 1U &&
                observed_state->joint_names.size() == 1U &&
                observed_state->positions.size() == 1U &&
                observed_state->resources.front() == resource.resource_id &&
                observed_state->joint_names.front() ==
                    resource.compiled_resource_name &&
                core::coordinate_kind(observed_state->positions.front()) ==
                    resource.coordinate_kind,
            "direct-control gripper observation crosses its retained target");
    const auto &source_coordinate_name = model_backed
                                             ? *resource.source_joint_name
                                             : resource.compiled_resource_name;
    return CanonicalValue(CanonicalValue::Object{
        {"complete", CanonicalValue(true)},
        {"position",
         CoordinatePositionValue(observed_state->positions.front())},
        {"sequence",
         CanonicalValue(static_cast<double>(observed_state->sequence))},
        {"source_coordinate_name", CanonicalValue(source_coordinate_name)},
        {"source_epoch", CanonicalValue(observed_state->source_epoch.value())},
    });
  }

  [[nodiscard]] CanonicalValue DirectControlGripperStatusValue(
      Composition &composition,
      const core::ObservedStateSnapshotDTO *observed_state) const {
    auto fields = DirectControlGripperTargetValue(composition).as_object();
    fields.emplace_back("observed_state", DirectControlGripperObservedValue(
                                              composition, observed_state));
    return CanonicalValue(std::move(fields));
  }

  struct CurrentControlState final {
    std::optional<core::ObservedStateSnapshotDTO> motion;
    std::optional<core::ObservedStateSnapshotDTO> gripper;
  };

  [[nodiscard]] CurrentControlState
  ReadCurrentControlState(Composition &composition) const {
    Require(composition.runtime != nullptr ||
                composition.gripper_runtime != nullptr,
            "selected component has no observed-state owner");
    const auto now = composition.runtime != nullptr
                         ? composition.runtime->clock->now()
                         : composition.cell_runtime->clock()->now();
    std::optional<core::ObservedStateSnapshotDTO> motion_state;
    if (composition.runtime != nullptr) {
      const auto motion =
          composition.runtime->motion_state_channel->current_state(now);
      if (!motion.ok())
        Fail("selected motion observed state is unavailable: " +
             motion.error().message);
      motion_state = motion.value();
    }
    const auto gripper_channel = composition.runtime != nullptr
                                     ? composition.runtime->gripper_state_channel
                                     : composition.gripper_runtime->state_channel();
    if (!composition.direct_control_gripper_binding.has_value()) {
      Require(gripper_channel == nullptr,
              "selected gripper observed-state channel has no binding");
      return CurrentControlState{std::move(motion_state), std::nullopt};
    }
    Require(gripper_channel != nullptr,
            "selected gripper observed-state channel is absent");
    Require(
        composition.direct_control_gripper_binding->value().resources.size() ==
            1U,
        "selected gripper observation has no exact scalar binding");
    const auto gripper = gripper_channel->current_state(now);
    const auto &resource =
        composition.direct_control_gripper_binding->value().resources.front();
    const bool component_local =
        !resource.source_joint_name.has_value() &&
        (composition.runtime != nullptr
             ? !composition.runtime->gripper_position_limits.has_value()
             : !composition.gripper_runtime->position_limits().has_value());
    const auto missing_reason =
        gripper.ok() ? std::string{}
                     : (gripper.error().details.contains("reason")
                            ? gripper.error().details.at("reason")
                            : std::string{});
    if (!gripper.ok() && (!component_local ||
                          missing_reason != "observed_state_not_published")) {
      Fail("selected gripper observed state is unavailable: " +
           gripper.error().message);
    }
    return CurrentControlState{
        std::move(motion_state),
        gripper.ok()
            ? std::optional<core::ObservedStateSnapshotDTO>{gripper.value()}
            : std::nullopt};
  }

  [[nodiscard]] CanonicalValue::Object
  IdentityFields(Composition &composition,
                 const core::ProjectionRevisionDTO &projection) const {
    const auto &bundle = *composition.bundle;
    Require(projection.retained_bundle_id ==
                    composition.projection_revision.retained_bundle_id &&
                projection.graph_revision ==
                    composition.projection_revision.graph_revision &&
                projection.scene_revision.valid(),
            "selected execution identity crosses its active product");
    return CanonicalValue::Object{
        {"active_publication_generation",
         CanonicalValue(
             static_cast<double>(composition.publication_generation.value()))},
        {"cell_id", CanonicalValue(bundle.value().cell_graph.cell_id.value())},
        {"control_management_mode",
         CanonicalValue(
             composition.cclc_attached.load(std::memory_order_acquire)
                 ? (composition.route_recovery_pending.load(
                        std::memory_order_acquire)
                        ? "pending"
                        : "component_lifecycle")
             : "pending")},
        {"graph_revision",
         CanonicalValue(projection.graph_revision.value())},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"retained_bundle_id",
         CanonicalValue(projection.retained_bundle_id.value())},
        {"scene_revision", SceneRevisionValue(projection.scene_revision)},
    };
  }

  [[nodiscard]] CanonicalValue
  IdentityValue(Composition &composition,
                const core::ProjectionRevisionDTO &projection) const {
    auto fields = IdentityFields(composition, projection);
    fields.emplace_back("schema_version",
                        CanonicalValue("manyforge.execution_identity.v2"));
    return CanonicalValue(std::move(fields));
  }

  [[nodiscard]] CanonicalValue
  StatusResponse(Composition &composition,
                 const core::ProjectionRevisionDTO &projection) const {
    // Diagnostics are a cold-path view of the same three owner projections,
    // not another status contract with primary motion/gripper aliases.
    const auto targets = TargetsResponse(composition, projection);
    const auto control_state = ControlStateResponse(composition, projection);
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("status")},
        {"ok", CanonicalValue(true)},
        {"status", CanonicalValue(CanonicalValue::Object{
                       {"schema_version",
                        CanonicalValue("manyforge.execution_status.v2")},
                       {"identity", IdentityValue(composition, projection)},
                       {"targets", Member(targets, "targets")},
                       {"control_state", Member(control_state, "control_state")},
                   })},
    });
  }

  [[nodiscard]] CanonicalValue
  IdentityResponse(Composition &composition,
                   const core::ProjectionRevisionDTO &projection) const {
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("identity")},
        {"identity", IdentityValue(composition, projection)},
        {"ok", CanonicalValue(true)},
    });
  }

  [[nodiscard]] CanonicalValue
  TargetsResponse(Composition &composition,
                  const core::ProjectionRevisionDTO &projection) const {
    Require(composition.bundle != nullptr &&
                composition.bundle->retained_bundle_id() ==
                    projection.retained_bundle_id &&
                composition.bundle->value().cell_graph.graph_revision ==
                    projection.graph_revision,
            "execution targets do not match the verified active bundle");
    struct TargetProjection final {
      std::string command_target_id;
      std::string operation_capability_id;
      CanonicalValue value;
    };
    struct ComponentProjection final {
      Composition *owner;
      std::vector<TargetProjection> targets;
    };
    using TargetKey = std::tuple<std::string, std::string, std::string>;
    std::map<core::ControllableComponentId, Composition *>
        runtime_component_owners;
    const auto register_runtime_component = [&](Composition &candidate) {
      Require(runtime_component_owners
                  .emplace(candidate.controllable_component_id, &candidate)
                  .second,
              "native execution component has multiple runtime owners");
    };
    register_runtime_component(composition);
    for (const auto &participant : composition.participant_compositions) {
      Require(participant != nullptr,
              "native execution component registry contains a missing owner");
      register_runtime_component(*participant);
    }

    std::map<core::ControllableComponentId, ComponentProjection>
        component_targets;
    std::set<core::ControllableComponentId> active_component_ids;
    for (const auto &component :
         composition.bundle->value().cell_graph.controllable_components) {
      Require(active_component_ids.insert(component.controllable_component_id)
                  .second,
              "verified active bundle contains a duplicate component identity");
      const auto runtime_owner =
          runtime_component_owners.find(component.controllable_component_id);
      auto [entry, inserted] = component_targets.try_emplace(
          component.controllable_component_id,
          ComponentProjection{runtime_owner == runtime_component_owners.end()
                                  ? nullptr
                                  : runtime_owner->second,
                              {}});
      static_cast<void>(entry);
      Require(inserted,
              "verified active bundle contains a duplicate target component row");
      Require(component_targets.size() <= 128U,
              "native execution target component list exceeds its bound");
    }
    for (const auto &[component_id, owner] : runtime_component_owners) {
      static_cast<void>(owner);
      Require(active_component_ids.contains(component_id),
              "native runtime registry contains a component outside the active bundle");
    }

    std::set<TargetKey> supported_target_keys;

    const auto append_target = [&](const CanonicalValue &source,
                                   const std::string_view presentation) {
      const auto component_id = core::ControllableComponentId(
          Text(source, "controllable_component_id"));
      const auto runtime_owner = runtime_component_owners.find(component_id);
      Require(runtime_owner != runtime_component_owners.end(),
              "execution target has no component in the runtime registry");
      const auto component = component_targets.find(component_id);
      Require(component != component_targets.end() &&
                  component->second.owner == runtime_owner->second,
              "supported execution target has no matching active runtime owner");

      const auto command_target_id = Text(source, "command_target_id");
      const auto operation_capability_id =
          Text(source, "operation_capability_id");
      Require(supported_target_keys
                  .insert({component_id.value(), command_target_id,
                           operation_capability_id})
                  .second,
              "native execution component has a duplicate supported target key");
      const auto field = [&](const std::string_view name) {
        return Member(source, name);
      };
      CanonicalValue::Object manual_control{
          {"program_binding_id", field("program_binding_id")},
          {"program_id", field("program_id")},
          {"program_revision", field("program_revision")},
          {"program_run_id", field("program_run_id")},
      };
      CanonicalValue::Object command;
      if (presentation == "motion") {
        command = {
            {"base_frame_role_id", field("base_frame_role_id")},
            {"component_role_id", field("component_role_id")},
            {"frame_role_ids", field("frame_role_ids")},
            {"joints", field("joints")},
            {"manual_control", CanonicalValue(std::move(manual_control))},
            {"planning_capability_ids", field("planning_capability_ids")},
            {"target_descriptor", field("target_descriptor")},
            {"target_role_id", field("target_role_id")},
            {"tcp_frame_role_id", field("tcp_frame_role_id")},
        };
      } else {
        Require(presentation == "gripper",
                "native execution target has an unsupported presentation");
        command = {
            {"command_schema_id", field("command_schema_id")},
            {"command_schema_revision", field("command_schema_revision")},
            {"component_role_id", field("component_role_id")},
            {"manual_control", CanonicalValue(std::move(manual_control))},
            {"resource", field("resource")},
            {"target_descriptor", field("target_descriptor")},
            {"target_role_id", field("target_role_id")},
        };
      }
      CanonicalValue target(CanonicalValue::Object{
          {"command", CanonicalValue(std::move(command))},
          {"command_target_id", CanonicalValue(command_target_id)},
          {"operation_capability_id",
           CanonicalValue(operation_capability_id)},
          {"presentation", CanonicalValue(std::string(presentation))},
      });
      const auto *descriptor = target.find_object_member("command");
      Require(descriptor != nullptr,
              "native execution target has no command projection");
      const auto *target_descriptor =
          descriptor->find_object_member("target_descriptor");
      Require(target_descriptor != nullptr &&
                  Text(*target_descriptor, "controllable_component_id") ==
                      component_id.value() &&
                  Text(*target_descriptor, "command_target_id") ==
                      command_target_id,
              "execution target descriptor identity crosses its row key");
      component->second.targets.push_back(TargetProjection{
          command_target_id, operation_capability_id, std::move(target)});
    };

    const auto append_component_targets = [&](Composition &candidate) {
      if (candidate.direct_control_motion_binding.has_value()) {
        Require(candidate.runtime != nullptr &&
                    candidate.direct_control_joint_limits.has_value(),
                "native motion target has no installed motion owner or limits");
        append_target(DirectControlTargetValue(candidate), "motion");
      }
      if (candidate.direct_control_gripper_binding.has_value()) {
        Require(candidate.runtime != nullptr ||
                    candidate.gripper_runtime != nullptr,
                "native gripper target has no installed controller owner");
        append_target(DirectControlGripperTargetValue(candidate), "gripper");
      }
    };
    append_component_targets(composition);
    for (const auto &participant : composition.participant_compositions) {
      append_component_targets(*participant);
    }

    std::set<TargetKey> active_binding_keys;
    for (const auto &binding :
         composition.bundle->value().cell_graph.control_bindings) {
      const auto &component_id = binding.controllable_component_id;
      Require(active_component_ids.contains(component_id),
              "verified active control binding names a missing component");
      const TargetKey key{component_id.value(), binding.command_target_id.value(),
                          binding.capability_id.value()};
      Require(active_binding_keys.insert(key).second,
              "verified active bundle contains an ambiguous target key");
      if (supported_target_keys.contains(key)) {
        continue;
      }
      component_targets.at(component_id).targets.push_back(TargetProjection{
          binding.command_target_id.value(), binding.capability_id.value(),
          CanonicalValue(CanonicalValue::Object{
              {"command", CanonicalValue(nullptr)},
              {"command_target_id",
               CanonicalValue(binding.command_target_id.value())},
              {"operation_capability_id",
               CanonicalValue(binding.capability_id.value())},
              {"presentation", CanonicalValue("unsupported")},
          })});
    }
    for (const auto &key : supported_target_keys) {
      Require(active_binding_keys.contains(key),
              "native manual target is absent from the verified active bundle");
    }

    CanonicalValue::Array components;
    components.reserve(component_targets.size());
    for (auto &[component_id, component] : component_targets) {
      std::sort(component.targets.begin(), component.targets.end(),
                [](const auto &left, const auto &right) {
                  return std::tie(left.command_target_id,
                                  left.operation_capability_id) <
                         std::tie(right.command_target_id,
                                  right.operation_capability_id);
                });
      Require(std::adjacent_find(
                  component.targets.begin(), component.targets.end(),
                  [](const auto &left, const auto &right) {
                    return left.command_target_id == right.command_target_id &&
                           left.operation_capability_id ==
                               right.operation_capability_id;
                  }) == component.targets.end(),
              "native execution component has a duplicate addressed target");

      bool command_eligible = false;
      CanonicalValue reason("component_command_not_eligible");
      auto *kernel = component.owner == nullptr
                         ? nullptr
                         : component.owner->active_addressed_kernel();
      if (kernel != nullptr) {
        const auto readiness = kernel->component_command_readiness(component_id);
        if (!readiness.ok()) {
          const auto existing_reason = readiness.error().details.find("reason");
          if (existing_reason != readiness.error().details.end() &&
              !existing_reason->second.empty()) {
            reason = CanonicalValue(existing_reason->second);
          }
        } else if (readiness.value().controllable_component_id == component_id &&
                   readiness.value().component_lifecycle_generation.has_value()) {
          command_eligible = readiness.value().command_eligible;
          if (command_eligible) {
            reason = CanonicalValue(nullptr);
          }
        } else {
          reason = CanonicalValue("component_command_not_eligible");
        }
      } else {
        reason = CanonicalValue("component_readiness_kernel_missing");
      }

      CanonicalValue::Array targets;
      Require(component.targets.size() <= 128U,
              "native execution component target list exceeds its bound");
      targets.reserve(component.targets.size());
      for (auto &target : component.targets) {
        targets.emplace_back(std::move(target.value));
      }
      components.emplace_back(CanonicalValue::Object{
          {"availability",
           CanonicalValue(CanonicalValue::Object{
               {"command_eligible", CanonicalValue(command_eligible)},
               {"reason", std::move(reason)},
           })},
          {"controllable_component_id",
           CanonicalValue(component_id.value())},
          {"targets", CanonicalValue(std::move(targets))},
      });
    }

    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("targets")},
        {"identity", IdentityValue(composition, projection)},
        {"ok", CanonicalValue(true)},
        {"targets",
         CanonicalValue(CanonicalValue::Object{
             {"components", CanonicalValue(std::move(components))},
             {"schema_version",
              CanonicalValue("manyforge.execution_targets.v3")},
         })},
    });
  }

  [[nodiscard]] CanonicalValue
  ControlStateResponse(Composition &composition,
                       const core::ProjectionRevisionDTO &projection) const {
    const auto current = ReadCurrentControlState(composition);
    CanonicalValue runtime_scale_control_value(nullptr);
    if (composition.direct_control_motion_binding.has_value()) {
      const core::RuntimeParticipantBindingDTO *motion_binding =
          &composition.direct_control_motion_binding->value();
      if (composition.program_binding != nullptr) {
        Require(composition.runtime != nullptr &&
                    composition.status_motion_target_role_id.has_value(),
                "selected Program has no motion runtime-scale identity");
        const auto selected = std::find_if(
            composition.runtime->motion_binding_aliases.begin(),
            composition.runtime->motion_binding_aliases.end(),
            [&](const auto &candidate) {
              return candidate.value().program_binding_id ==
                         composition.program_binding->program_binding_id() &&
                     candidate.value().target_role_id ==
                         *composition.status_motion_target_role_id;
            });
        Require(selected != composition.runtime->motion_binding_aliases.end(),
                "selected Program Binding has no runtime-scale motion alias");
        motion_binding = &selected->value();
      }
      const core::RuntimeScaleControlSelectorDTO runtime_scale_selector{
          motion_binding->program_binding_id, motion_binding->component_role_id,
          motion_binding->target_role_id};
      const auto runtime_scale_control =
          composition.active_addressed_kernel()
              ->present_runtime_scale_control(runtime_scale_selector);
      if (!runtime_scale_control.ok()) {
        Fail("runtime-scale control projection failed: " +
             runtime_scale_control.error().message);
      }
      runtime_scale_control_value =
          RuntimeScaleControlStatusValue(runtime_scale_control.value());
    }
    const auto manual_operations =
        composition.active_addressed_kernel()->get_latest_run_operations(
            {direct_control_program_run_id_,
             direct_control_gripper_program_run_id_});
    CanonicalValue::Array manual_operation_values;
    manual_operation_values.reserve(manual_operations.size());
    for (const auto &projection : manual_operations) {
      manual_operation_values.emplace_back(CanonicalValue::Object{
          {"operation", OperationStateValue(projection.operation)},
          {"resident", CanonicalValue(projection.resident)}});
    }
    struct ObservedTargetRow final {
      std::tuple<std::string, std::string, std::string> key;
      CanonicalValue value;
    };
    std::vector<ObservedTargetRow> observed_rows;
    observed_rows.reserve(2U * (1U + composition.participant_compositions.size()));
    const auto append_observed_target_state =
        [&](Composition &candidate, const CurrentControlState &state) {
          if (candidate.direct_control_motion_binding.has_value()) {
            const auto &motion =
                candidate.direct_control_motion_binding->value();
            CanonicalValue motion_observed(nullptr);
            if (state.motion.has_value()) {
              auto observed =
                  DirectControlObservedValue(candidate, *state.motion).as_object();
              observed.emplace_back("kind", CanonicalValue("joint_positions"));
              motion_observed = CanonicalValue(std::move(observed));
            }
            observed_rows.push_back(ObservedTargetRow{
                {motion.controllable_component_id.value(),
                 motion.command_target_id.value(), motion.capability_id.value()},
                CanonicalValue(CanonicalValue::Object{
                    {"controllable_component_id",
                     CanonicalValue(motion.controllable_component_id.value())},
                    {"command_target_id",
                     CanonicalValue(motion.command_target_id.value())},
                    {"operation_capability_id",
                     CanonicalValue(motion.capability_id.value())},
                    {"observed", std::move(motion_observed)},
                })});
          }
          if (candidate.direct_control_gripper_binding.has_value()) {
            const auto &gripper =
                candidate.direct_control_gripper_binding->value();
            CanonicalValue observed(nullptr);
            if (state.gripper.has_value()) {
              auto gripper_observed = DirectControlGripperObservedValue(
                  candidate, &*state.gripper).as_object();
              gripper_observed.emplace_back("kind", CanonicalValue("coordinate"));
              observed = CanonicalValue(std::move(gripper_observed));
            }
            observed_rows.push_back(ObservedTargetRow{
                {gripper.controllable_component_id.value(),
                 gripper.command_target_id.value(), gripper.capability_id.value()},
                CanonicalValue(CanonicalValue::Object{
                    {"controllable_component_id",
                     CanonicalValue(gripper.controllable_component_id.value())},
                    {"command_target_id",
                     CanonicalValue(gripper.command_target_id.value())},
                    {"operation_capability_id",
                     CanonicalValue(gripper.capability_id.value())},
                    {"observed", std::move(observed)},
                })});
          }
        };
    append_observed_target_state(composition, current);
    for (const auto &participant : composition.participant_compositions) {
      append_observed_target_state(*participant,
                                   ReadCurrentControlState(*participant));
    }
    std::sort(observed_rows.begin(), observed_rows.end(),
              [](const auto &left, const auto &right) {
                return left.key < right.key;
              });
    Require(std::adjacent_find(
                observed_rows.begin(), observed_rows.end(),
                [](const auto &left, const auto &right) {
                  return left.key == right.key;
                }) == observed_rows.end(),
            "control-state projection has duplicate observed target identities");
    CanonicalValue::Array observed_target_states;
    observed_target_states.reserve(observed_rows.size());
    for (auto &row : observed_rows)
      observed_target_states.emplace_back(std::move(row.value));
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("control_state")},
        {"control_state",
         CanonicalValue(CanonicalValue::Object{
             {"cell_stop_action",
              CellStopActionValue(composition.active_addressed_kernel()
                                      ->cell_stop_action_projection())},
             {"manual_operations",
              CanonicalValue(std::move(manual_operation_values))},
             {"observed_targets",
              CanonicalValue(std::move(observed_target_states))},
             {"runtime_scale_control",
              std::move(runtime_scale_control_value)},
             {"schema_version",
              CanonicalValue("manyforge.execution_control_state.v6")},
         })},
        {"identity", IdentityValue(composition, projection)},
        {"ok", CanonicalValue(true)},
    });
  }

  struct ObservationProjection final {
    CanonicalValue response;
    std::shared_ptr<IRetainedObservedStateChannel> channel;
    std::string source_epoch;
    std::uint64_t source_sequence{};
    std::uint64_t change_generation{};
  };

  [[nodiscard]] ObservationProjection
  BuildObservationProjection(Composition &composition) const {
    std::shared_ptr<IRetainedObservedStateChannel> wake_channel;
    std::optional<std::uint64_t> wake_incarnation;
    const auto capture_wake_channel = [&](
        const std::shared_ptr<IRetainedObservedStateChannel> &channel) {
      Require(channel != nullptr,
              "active native component has no retained observation channel");
      const auto *render_channel =
          dynamic_cast<const RetainedObservedStateChannel *>(channel.get());
      Require(render_channel != nullptr,
              "native observation channel has no shared render wake identity");
      if (!wake_channel) {
        wake_channel = channel;
        wake_incarnation = render_channel->render_incarnation();
      } else {
        Require(wake_incarnation == render_channel->render_incarnation(),
                "native component observations do not share one cell render wake");
      }
    };

    struct ObservationRow final {
      std::tuple<std::string, std::string, std::string> key;
      CanonicalValue value;
    };
    // Apply publishes routes on the live product without the composition
    // fence. The channel is therefore the sole source of each target's route
    // binding: binding and sample are read together under its route fence,
    // never as an owner binding read followed by a later state read.
    struct ObservationSource final {
      std::shared_ptr<RetainedObservedStateChannel> channel;
      core::ControllableComponentId controllable_component_id;
    };
    std::vector<ObservationSource> sources;
    sources.reserve(2U * (1U + composition.participant_compositions.size()));
    std::vector<ObservationRow> rows;
    rows.reserve(2U * (1U + composition.participant_compositions.size()));
    const auto append_target = [&](
        const std::shared_ptr<IRetainedObservedStateChannel> &selected_channel,
        const core::ControllableComponentId &owner_component_id) {
      capture_wake_channel(selected_channel);
      sources.push_back(ObservationSource{
          std::static_pointer_cast<RetainedObservedStateChannel>(
              selected_channel),
          owner_component_id});
    };
    const auto project_target = [&](const ObservationSource &source) {
      const auto now = composition.cell_runtime != nullptr
                           ? composition.cell_runtime->clock()->now()
                       : composition.runtime != nullptr &&
                                 composition.runtime->clock != nullptr
                           ? composition.runtime->clock->now()
                           : WallClock{}.now();
      const auto observed = source.channel->current_bound_state(now);
      if (!observed.ok())
        Fail("selected observation is unavailable: " +
             observed.error().message);
      const auto &binding = observed.value().binding.value();
      Require(binding.controllable_component_id ==
                      source.controllable_component_id &&
                  binding.control_path.has_value() &&
                  !binding.resources.empty(),
              "native observation target has no sealed control path or resources");
      const bool any_model_resource = std::any_of(
          binding.resources.begin(), binding.resources.end(),
          [](const auto &resource) {
            return resource.source_joint_name.has_value();
          });
      if (!any_model_resource)
        return;
      Require(std::all_of(binding.resources.begin(), binding.resources.end(),
                          [](const auto &resource) {
                            return resource.source_joint_name.has_value();
                          }),
              "native observation target mixes model and component-local resources");
      const auto &state = observed.value().state;
      const auto &path = *binding.control_path;
      std::vector<std::string_view> identity_mismatches;
      const auto check_identity = [&](const std::string_view field,
                                      const bool matches) {
        if (!matches) identity_mismatches.push_back(field);
      };
      check_identity("complete", state.complete);
      check_identity("resources", state.resources.size() ==
                                      binding.resources.size());
      check_identity("joint_names", state.joint_names.size() ==
                                        binding.resources.size());
      check_identity("positions", state.positions.size() ==
                                     binding.resources.size());
      check_identity("component_id", state.controllable_component_id ==
                                         binding.controllable_component_id);
      check_identity("command_target_id", state.command_target_id ==
                                               binding.command_target_id);
      check_identity("control_domain_id", state.control_domain_id ==
                                             path.control_domain_id);
      check_identity("control_domain_revision",
                     state.control_domain_revision ==
                         path.control_domain_revision);
      check_identity("control_binding_id", state.control_binding_id ==
                                               path.control_binding_id);
      check_identity("control_binding_revision",
                     state.control_binding_revision ==
                         path.control_binding_revision);
      check_identity("provider_instance_id", state.provider_instance_id ==
                                                 path.provider_instance_id);
      check_identity("provider_manifest_id", state.provider_manifest_id ==
                                                 path.provider_manifest_id);
      check_identity("provider_manifest_revision",
                     state.provider_manifest_revision ==
                         path.provider_manifest_revision);
      check_identity("controller_topology_id",
                     state.controller_topology_id ==
                         path.controller_topology_id);
      check_identity("topology_revision", state.topology_revision ==
                                               path.topology_revision);
      check_identity("control_route_id", state.control_route_id ==
                                              path.control_route_id);
      check_identity("active_control_configuration_id",
                     state.active_control_configuration_id ==
                         path.active_control_configuration_id);
      check_identity("active_control_configuration_generation",
                     state.active_control_configuration_generation ==
                         path.active_control_configuration_generation);
      check_identity("state_authority_id", state.state_authority_id ==
                                                path.state_authority_id);
      check_identity("state_authority_class", state.state_authority_class ==
                                                   path.state_authority_class);
      check_identity("source_epoch",
                     state.source_epoch == path.provider_runtime_epoch);
      if (!identity_mismatches.empty()) {
        std::string message =
            "selected observation crosses its retained target identity: ";
        for (const auto field : identity_mismatches) {
          if (message.back() != ' ') message.push_back(',');
          message.append(field);
        }
        message.append("; route configuration observed=");
        message.append(state.active_control_configuration_id.value());
        message.append(" expected=");
        message.append(path.active_control_configuration_id.value());
        message.append(" observed_generation=");
        message.append(std::to_string(
            state.active_control_configuration_generation.value()));
        message.append(" expected_generation=");
        message.append(std::to_string(
            path.active_control_configuration_generation.value()));
        Fail(std::move(message));
      }
      CanonicalValue::Array source_names, compiled_names, kinds, positions;
      source_names.reserve(binding.resources.size());
      compiled_names.reserve(binding.resources.size());
      kinds.reserve(binding.resources.size());
      positions.reserve(binding.resources.size());
      for (std::size_t index = 0U; index < binding.resources.size(); ++index) {
        const auto &resource = binding.resources[index];
        const auto *angular =
            std::get_if<core::AngularPositionDTO>(&state.positions[index]);
        const auto *linear =
            std::get_if<core::LinearPositionDTO>(&state.positions[index]);
        Require(resource.source_joint_name.has_value() &&
                    state.resources[index] == resource.resource_id &&
                    state.joint_names[index] ==
                        resource.compiled_resource_name &&
                    ((angular && resource.coordinate_kind ==
                                     core::CoordinateKind::kAngular) ||
                     (linear && resource.coordinate_kind ==
                                    core::CoordinateKind::kLinear)),
                "selected observation has invalid retained coordinates");
        const double value = angular ? angular->value_rad : linear->value_m;
        Require(std::isfinite(value),
                "selected observation position is not finite");
        source_names.emplace_back(*resource.source_joint_name);
        compiled_names.emplace_back(resource.compiled_resource_name);
        kinds.emplace_back(angular ? "angular" : "linear");
        positions.emplace_back(value);
      }
      rows.push_back(ObservationRow{
          {binding.controllable_component_id.value(),
           binding.command_target_id.value(), binding.capability_id.value()},
          CanonicalValue(CanonicalValue::Object{
              {"command_target_id",
               CanonicalValue(binding.command_target_id.value())},
              {"compiled_joint_names",
               CanonicalValue(std::move(compiled_names))},
              {"controllable_component_id",
               CanonicalValue(binding.controllable_component_id.value())},
              {"operation_capability_id",
               CanonicalValue(binding.capability_id.value())},
              {"coordinate_kinds", CanonicalValue(std::move(kinds))},
              {"positions_si", CanonicalValue(std::move(positions))},
              {"source_epoch", CanonicalValue(state.source_epoch.value())},
              {"source_joint_names", CanonicalValue(std::move(source_names))},
              {"source_sequence",
               CanonicalValue(static_cast<double>(state.sequence))},
          })});
    };

    const auto append_owner = [&](Composition &owner) {
      Require((owner.runtime == nullptr) != (owner.gripper_runtime == nullptr),
              "native observation component has no unique runtime owner");
      if (owner.runtime != nullptr) {
        if (owner.direct_control_motion_binding.has_value()) {
          append_target(owner.runtime->motion_state_channel,
                        owner.controllable_component_id);
        }
        if (owner.direct_control_gripper_binding.has_value()) {
          append_target(owner.runtime->gripper_state_channel,
                        owner.controllable_component_id);
        }
        Require(owner.direct_control_motion_binding.has_value() ||
                    owner.direct_control_gripper_binding.has_value(),
                "native component has no supported observed target");
        return;
      }
      Require(!owner.direct_control_motion_binding.has_value() &&
                  owner.direct_control_gripper_binding.has_value(),
              "gripper-only native owner has a non-gripper target");
      append_target(owner.gripper_runtime->state_channel(),
                    owner.controllable_component_id);
    };

    append_owner(composition);
    for (const auto &participant : composition.participant_compositions) {
      Require(participant != nullptr,
              "native observation composition has a missing component owner");
      append_owner(*participant);
    }
    Require(wake_channel != nullptr && wake_incarnation.has_value(),
            "native cell has no owner observation wake source");
    // Read the cell-wide wake generation before taking the immutable source
    // snapshots. A later publication is either visible below or makes the
    // waiter predicate immediately true.
    const auto change_generation = wake_channel->render_change_generation();
    for (const auto &source : sources)
      project_target(source);
    const auto stream_epoch =
        "render." + std::to_string(*wake_incarnation);
    std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
      return left.key < right.key;
    });
    Require(std::adjacent_find(rows.begin(), rows.end(),
                               [](const auto &left, const auto &right) {
                                 return left.key == right.key;
                               }) == rows.end(),
            "native cell observation repeats a target identity");
    CanonicalValue::Array targets;
    targets.reserve(rows.size());
    for (auto &row : rows)
      targets.emplace_back(std::move(row.value));
    const auto &bundle = *composition.bundle;
    auto response = CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("observation")},
        {"observation",
         CanonicalValue(CanonicalValue::Object{
             {"cell_id",
              CanonicalValue(bundle.value().cell_graph.cell_id.value())},
             {"graph_revision",
              CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
             {"owner_epoch", CanonicalValue(owner_epoch_.value())},
             {"retained_bundle_id",
              CanonicalValue(bundle.retained_bundle_id().value())},
             {"schema_version",
              CanonicalValue("manyforge.execution_observation.v3")},
             {"source_epoch", CanonicalValue(stream_epoch)},
             {"source_sequence",
              CanonicalValue(static_cast<double>(change_generation))},
             {"targets", CanonicalValue(std::move(targets))},
         })},
        {"ok", CanonicalValue(true)},
    });
    return ObservationProjection{std::move(response), std::move(wake_channel),
                                 stream_epoch, change_generation,
                                 change_generation};
  }

  [[nodiscard]] CanonicalValue
  ObservationResponse(Composition &composition) const {
    return BuildObservationProjection(composition).response;
  }

  [[nodiscard]] CanonicalValue
  ObservationResponseAfter(const Command &command, const int client_fd,
                           const std::stop_token stop_token) const {
    while (!stop_token.stop_requested()) {
      std::optional<ObservationProjection> projection;
      {
        // Keep the composition alive only while taking one immutable current
        // projection. Waiting never holds the Manager reconfiguration fence.
        std::shared_lock composition_lock(composition_access_.mutex);
        if (composition_access_.current == nullptr) {
          return ErrorResponse(
              "execution_owner_not_ready",
              "the managed execution owner has no active physical product",
              {{"owner_epoch", owner_epoch_.value()}});
        }
        projection.emplace(
            BuildObservationProjection(*composition_access_.current));
      }
      auto current = std::move(*projection);
      if (!command.observation_after_owner_epoch.has_value() ||
          command.observation_after_owner_epoch->value() !=
              owner_epoch_.value() ||
          *command.observation_after_source_epoch != current.source_epoch ||
          *command.observation_after_source_sequence <
              current.source_sequence) {
        return std::move(current.response);
      }
      if (*command.observation_after_source_sequence >
          current.source_sequence) {
        return ErrorResponse(
            "execution_observation_cursor_invalid",
            "the observation cursor is ahead of the current owner source");
      }

      while (!stop_token.stop_requested() &&
             !current.channel->wait_for_render_change(
                 current.change_generation, std::chrono::seconds(1))) {
        // The bounded wake is only a disconnect/shutdown check. A timeout
        // performs no owner read, projection, shaping, or encoding; only a
        // real source-generation change returns to the projection path.
        pollfd peer{client_fd, 0, 0};
        int peer_state = -1;
        do {
          peer_state = ::poll(&peer, 1, 0);
        } while (peer_state < 0 && errno == EINTR &&
                 !stop_token.stop_requested());
        if (peer_state < 0 ||
            (peer.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          Fail("execution observation client disconnected");
        }
        if ((peer.revents & POLLIN) != 0) {
          char byte{};
          const auto count =
              ::recv(client_fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
          if (count == 0) {
            Fail("execution observation client disconnected");
          }
        }
      }
    }
    Fail("execution observation source stopped");
  }

  [[nodiscard]] CanonicalValue
  ExecuteStop(const Command &command,
              core::ResourceAddressedExecutionKernel &addressed_kernel) {
    if (command.kind == CommandKind::kStopProgramSelection) {
      const auto result =
          addressed_kernel.stop_program_selection(*command.program_selection);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message,
                             result.error().details);
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("stop_program_selection")},
          {"ok", CanonicalValue(true)},
          {"accepted",
           ProgramSelectionAcceptedValue(
               result.value(), command.program_selection->group_ids)}});
    }
    const bool ordinary = (command.kind == CommandKind::kStop ||
                           command.kind == CommandKind::kStopProgramRun) &&
                          command.stop.has_value();
    const bool full =
        command.kind == CommandKind::kFullStop && command.full_stop.has_value();
    const bool malformed = command.kind == CommandKind::kMalformedStop &&
                           command.malformed_stop.has_value();
    Require(static_cast<unsigned>(ordinary) + static_cast<unsigned>(full) +
                    static_cast<unsigned>(malformed) ==
                1U,
            "execution Stop control lane accepts one complete Stop command");
    const auto result = [&] {
      if (ordinary) {
        return addressed_kernel.stop(*command.stop);
      }
      if (full) {
        return addressed_kernel.full_stop(*command.full_stop);
      }
      return addressed_kernel.recognized_malformed_stop(
          core::MalformedStopIngressDTO{
              command.malformed_stop->request_id, owner_epoch_,
              command.malformed_stop->attribution,
              command.malformed_stop->recognized_route,
              command.malformed_stop->reason});
    }();
    if (!result.ok()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(result.error())},
          {"ok", CanonicalValue(false)},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"accepted", StopAcceptedValue(result.value())},
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"command",
         CanonicalValue(full ? "full_stop"
                        : command.kind == CommandKind::kStopProgramRun
                            ? "stop_program_run"
                            : "stop")},
        {"ok", CanonicalValue(true)},
    });
  }

  [[nodiscard]] CanonicalValue
  ReadStop(const Command &command,
           core::ResourceAddressedExecutionKernel &addressed_kernel) const {
    const auto status = addressed_kernel.get_stop_status(*command.stop_id);
    if (!status.ok()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(status.error())},
          {"ok", CanonicalValue(false)},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("read_stop")},
        {"ok", CanonicalValue(true)},
        {"stop_status", StopStatusSummaryValue(status.value())},
    });
  }

  [[nodiscard]] CanonicalValue ReadStopFault(
      core::ResourceAddressedExecutionKernel &addressed_kernel) const {
    const auto offer = addressed_kernel.get_stop_fault_recovery_offer();
    if (!offer.ok()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(offer.error())},
          {"ok", CanonicalValue(false)},
      });
    }
    if (!offer.value().has_value()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("get_stop_fault")},
          {"ok", CanonicalValue(true)},
          {"stop_fault", CanonicalValue(nullptr)},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("get_stop_fault")},
        {"ok", CanonicalValue(true)},
        {"stop_fault", StopFaultOfferValue(*offer.value())},
    });
  }

  [[nodiscard]] CanonicalValue
  Execute(const Command &command, Composition &composition,
          const core::ProjectionRevisionDTO &projection,
          const std::stop_token planning_stop_token) {
    if (command.kind == CommandKind::kReadProgramExecutionScope) {
      const auto result =
          composition.active_addressed_kernel()
              ->get_program_execution_scope(
              *command.program_run_id);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message,
                             result.error().details);
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("read_program_execution_scope")},
          {"ok", CanonicalValue(true)},
          {"scope", ProgramExecutionScopeValue(result.value())}});
    }
    if (command.kind == CommandKind::kStatus) {
      return StatusResponse(composition, projection);
    }
    if (command.kind == CommandKind::kIdentity) {
      return IdentityResponse(composition, projection);
    }
    if (command.kind == CommandKind::kTargets) {
      return TargetsResponse(composition, projection);
    }
    if (command.kind == CommandKind::kControlState) {
      return ControlStateResponse(composition, projection);
    }
    if (command.kind == CommandKind::kObservation) {
      return ObservationResponse(composition);
    }
    if (command.kind == CommandKind::kEvents) {
      auto *const journal = composition.cell_runtime != nullptr
                                ? &composition.cell_runtime->operation_journal()
                                : composition.runtime == nullptr
                                      ? nullptr
                                      : composition.runtime->active_operation_journal();
      Require(journal != nullptr,
              "operational events have no active cell journal owner");
      const auto page = journal->ReadOperationalEvents(
              *command.event_owner_epoch, command.after_sequence,
              command.event_limit);
      if (!page.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(page.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      const auto admission =
          composition.active_addressed_kernel()
              ->GetAuthoringAdmission();
      // Progress is presentation-only: the journal deliberately records only
      // lifecycle changes. Sample resident manual operations on this existing
      // read, without adding durable writes or rebuilding control_state.
      CanonicalValue::Array operation_progress;
      for (const auto &operation :
           composition.active_addressed_kernel()->get_resident_operations(
               {direct_control_program_run_id_,
                direct_control_gripper_program_run_id_})) {
        if (operation.state != core::OperationStateKind::kExecuting ||
            operation.participants.size() != 1U)
          continue;
        operation_progress.emplace_back(CanonicalValue::Object{
            {"owner_epoch", CanonicalValue(command.event_owner_epoch->value())},
            {"operation_id", CanonicalValue(operation.operation_id.value())},
            {"operation_revision", CanonicalValue(static_cast<double>(
                                       operation.operation_revision.value()))},
            {"progress", CanonicalValue(operation.participants.front().progress)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"admission_generation",
           CanonicalValue(
               static_cast<double>(admission.admission_generation.value()))},
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("events")},
          {"event_page", OperationEventPageValue(page.value())},
          {"operation_progress", CanonicalValue(std::move(operation_progress))},
          {"ok", CanonicalValue(true)},
      });
    }
    if (command.kind == CommandKind::kStop ||
        command.kind == CommandKind::kStopProgramRun ||
        command.kind == CommandKind::kStopProgramSelection ||
        command.kind == CommandKind::kFullStop ||
        command.kind == CommandKind::kMalformedStop) {
      return ExecuteStop(command,
                         *composition.active_addressed_kernel());
    }
    const bool read_only = command.kind == CommandKind::kQueryTarget ||
                           command.kind == CommandKind::kQueryOperation ||
                           command.kind == CommandKind::kObservation ||
                           command.kind == CommandKind::kRead ||
                           command.kind == CommandKind::kGetOperationRecovery ||
                           command.kind == CommandKind::kGetRuntimeScaleControl ||
                           command.kind ==
                               CommandKind::kCheckProgramNodeMotionAttention;
    const auto liveness_command =
        read_only
            ? component_manager::detail::ManagerLivenessCommandClass::kReadOnly
            : component_manager::detail::ManagerLivenessCommandClass::
                  kEffectful;
    if (!manager_liveness_admission_.Allows(liveness_command)) {
      return ErrorResponse(
          "manager_liveness_not_current",
          "ordinary execution command requires an exact current unfenced "
          "Manager lease");
    }
    if (command.kind == CommandKind::kQueryTarget) {
      return QueryTargetResponse(*command.target_query, composition);
    }
    if (command.kind == CommandKind::kQueryOperation) {
      return QueryOperationResponse(*command.operation_query, composition);
    }
    if (command.kind == CommandKind::kRecoverStopFault) {
      Require(command.stop_fault_recovery.has_value(),
              "Stop-fault recovery command has no complete intent");
      const auto result =
          composition.active_addressed_kernel()->recover_stop_fault(
              *command.stop_fault_recovery);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("recover_stop_fault")},
          {"ok", CanonicalValue(true)},
          {"result", StopFaultRecoveryResultValue(result.value())},
      });
    }
    if (command.kind == CommandKind::kRead) {
      const auto projection =
          composition.active_addressed_kernel()
              ->get_operation_projection(
              *command.operation_id);
      if (!projection.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(projection.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("read")},
          {"ok", CanonicalValue(true)},
          {"owner_epoch",
           CanonicalValue(projection.value().request_owner_epoch.value())},
          {"operation", OperationStateValue(projection.value().operation)},
          {"resident", CanonicalValue(projection.value().resident)},
          {"last_control",
           projection.value().last_control.has_value()
               ? ControlAcceptedValue(*projection.value().last_control)
               : CanonicalValue(nullptr)},
          {"control_pending",
           CanonicalValue(projection.value().control_pending)},
      });
    }
    if (command.kind == CommandKind::kGetOperationRecovery) {
      const auto assessment =
          composition.active_addressed_kernel()
              ->get_operation_recovery_assessment(*command.operation_id);
      if (!assessment.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(assessment.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("get_operation_recovery")},
          {"ok", CanonicalValue(true)},
          {"operation_revision",
           CanonicalValue(static_cast<double>(
               assessment.value().operation_revision.value()))},
          {"stop_generation",
           assessment.value().stop_generation.has_value()
               ? CanonicalValue(static_cast<double>(
                     assessment.value().stop_generation->value()))
               : CanonicalValue(nullptr)},
          {"stop_id", assessment.value().stop_id.has_value()
                          ? CanonicalValue(assessment.value().stop_id->value())
                          : CanonicalValue(nullptr)},
          {"participants",
           RecoveryParticipantsValue(assessment.value().participants)},
          {"recovery_assessment",
           RecoveryAssessmentValue(assessment.value().assessment)},
      });
    }
    const auto reconcile_error = LastReconcileError();
    if (reconcile_error.has_value()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(*reconcile_error)},
          {"ok", CanonicalValue(false)},
      });
    }
    if (command.kind == CommandKind::kConfirmGrasp) {
      return ConfirmGraspResponse(*command.confirmation, composition);
    }
    if (command.kind == CommandKind::kPreviewCapabilityCommand) {
      auto preview =
          composition.active_addressed_kernel()
              ->preview_capability_command(
              *command.operation);
      if (!preview.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(preview.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      auto *const target_composition =
          FindBindingComposition(preview.value().binding, composition);
      if (target_composition == nullptr) {
        return ErrorResponse(
            "execution_target_observation_unavailable",
            "capability preview has no unique qualified target owner");
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("preview_capability_command")},
          {"ok", CanonicalValue(true)},
          {"preview_receipt",
           CapabilityCommandPreviewReceiptValue(
               preview.value(), *command.operation, *command.operation_digest,
               target_composition->runtime->gripper_position_limits,
               owner_epoch_)},
      });
    }
    if (command.kind == CommandKind::kReadManualHandover) {
      const auto result =
          composition.active_addressed_kernel()
              ->get_manual_control_availability(*command.operation);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)}});
      }
      CanonicalValue handover(nullptr);
      if (result.value().handover) {
        const auto &held = *result.value().handover;
        const auto &capture = held.capture;
        CanonicalValue::Array indices;
        indices.reserve(held.producer_node_indices.size());
        for (const auto index : held.producer_node_indices)
          indices.emplace_back(static_cast<double>(index));
        handover = CanonicalValue(CanonicalValue::Object{
            {"context",
             CanonicalValue(CanonicalValue::Object{
                 {"claim_generation", CanonicalValue(static_cast<double>(
                                          capture.claim_generation.value()))},
                 {"program_run_id",
                  CanonicalValue(capture.program_run_id.value())},
                 {"program_runner_owner_epoch",
                  CanonicalValue(capture.program_runner_owner_epoch.value())},
                 {"selection_revision", CanonicalValue(static_cast<double>(
                                            held.selection_revision))}})},
            {"producer_node_indices", CanonicalValue(std::move(indices))}});
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("read_manual_handover")},
          {"availability",
           CanonicalValue(CanonicalValue::Object{
               {"available", CanonicalValue(result.value().available)},
               {"cleanup_required",
                CanonicalValue(result.value().cleanup_required)},
               {"unavailable_reason",
                result.value().unavailable_reason
                    ? CanonicalValue(*result.value().unavailable_reason)
                    : CanonicalValue(nullptr)}})},
          {"handover", std::move(handover)},
          {"intent_digest", CanonicalValue(command.operation_digest->value())},
          {"ok", CanonicalValue(true)},
          {"request_id",
           CanonicalValue(
               command.operation->request_context.request_id.value())},
          {"request_owner_epoch", CanonicalValue(owner_epoch_.value())}});
    }
    if (command.kind == CommandKind::kPreviewMotion) {
      if (composition.runtime == nullptr) {
        return ErrorResponse(
            "motion_target_unavailable",
            "motion preview requires an installed motion-capable component");
      }
      auto preview_intent = *command.operation;
      const auto predecessor = composition.runtime->collision_publisher
                                   ->current_scene_snapshot_lease_inventory();
      if (!predecessor.has_value() ||
          !preview_intent.expected_scene_revision.has_value() ||
          *preview_intent.expected_scene_revision !=
              predecessor->projection_binding.projection_revision
                  .scene_revision) {
        return ErrorResponse(
            "scene_changed_during_validation",
            "motion preview does not name the pre-latch scene revision");
      }
      if (composition.runtime->planning_scene_synchronizer != nullptr) {
        auto synchronized = composition.runtime->planning_scene_synchronizer
                                ->SynchronizeBeforePlanning();
        if (!synchronized.ok()) {
          return CanonicalValue(CanonicalValue::Object{
              {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
              {"error", ErrorValue(synchronized.error())},
              {"ok", CanonicalValue(false)},
          });
        }
        preview_intent.expected_scene_revision =
            synchronized.value().scene_revision;
      }
      const auto preview_lease_id = core::ProjectionLeaseId{
          "motion-preview." +
          preview_intent.request_context.request_id.value()};
      auto inventory =
          composition.runtime->collision_publisher
              ->retain_scene_snapshot_lease_inventory(preview_lease_id);
      if (!inventory.has_value()) {
        return ErrorResponse(
            "motion_preview_projection_unavailable",
            "the native owner could not retain an exact collision projection");
      }
      CollisionLeaseRelease release(
          *composition.runtime->collision_publisher,
          inventory->projection_binding.projection_lease_id);
      auto session =
          composition.active_addressed_kernel()
              ->begin_motion_preview_session(
              preview_intent);
      if (!session.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(session.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      auto preview_binding = session.value()->resolve_binding(preview_intent);
      if (!preview_binding.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(preview_binding.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      auto start_state = session.value()->current_state();
      if (!start_state.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(start_state.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      auto preview = session.value()->preview_motion(
          preview_intent, std::move(*inventory),
          std::move(start_state.value()), 0U, planning_stop_token);
      if (!preview.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(preview.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("preview_motion")},
          {"ok", CanonicalValue(true)},
          {"preview_receipt",
           MotionPreviewReceiptValue(preview.value(), *command.operation,
                                     *command.operation_digest,
                                     preview_binding.value(), owner_epoch_)},
      });
    }
    if (command.kind == CommandKind::kPlanPick ||
        command.kind == CommandKind::kPlanPlace) {
      if (!composition.pick_place_planning_owner) {
        return ErrorResponse(
            "pick_place_planning_owner_unavailable",
            "the selected native process has no joined logical-scene "
            "planning owner");
      }
      const auto sequence_id = command.kind == CommandKind::kPlanPick
                                   ? command.pick_planning->sequence_id
                                   : command.place_planning->sequence_id;
      if (command.kind == CommandKind::kPlanPick) {
        if (auto verified = VerifyPickReferenceTargets(
                *composition.target_resolver, *command.pick_planning);
            !verified.ok()) {
          return CanonicalValue(CanonicalValue::Object{
              {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
              {"error", ErrorValue(verified.error())},
              {"ok", CanonicalValue(false)},
          });
        }
      }
      const auto result =
          command.kind == CommandKind::kPlanPick
              ? composition.pick_place_planning_owner->PlanPick(
                    *command.pick_planning, planning_stop_token)
              : composition.pick_place_planning_owner->PlanPlace(
                    *command.place_planning, planning_stop_token);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      const auto command_name =
          command.kind == CommandKind::kPlanPick ? "plan_pick" : "plan_place";
      const auto kind = command.kind == CommandKind::kPlanPick
                            ? core::PickPlaceExecutionKindDTO::kPick
                            : core::PickPlaceExecutionKindDTO::kPlace;
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue(command_name)},
          {"ok", CanonicalValue(true)},
          {"planning_result",
           PickPlacePlanningResultValue(sequence_id, kind, result.value())},
      });
    }
    if (command.kind == CommandKind::kRegisterProgramCapture) {
      const auto capture = composition.target_resolver->ResolveProgramCapture(
          *command.program_capture);
      const auto result =
          capture.ok() ? composition.active_addressed_kernel()
                             ->register_program_run_capture(capture.value())
                       : core::Result<void>::Err(capture.error());
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("register_program_run_capture")},
          {"ok", CanonicalValue(true)},
          {"program_run_id",
           CanonicalValue(capture.value().program_run_id.value())},
          {"claim_generation", CanonicalValue(static_cast<double>(
                                   capture.value().claim_generation.value()))},
      });
    }
    if (command.kind == CommandKind::kSubmit) {
      auto origin = composition.target_resolver->ValidateExecutionOrigin(
          *command.operation);
      if (origin.ok())
        origin = VerifyReferenceTargets(*composition.target_resolver,
                                        *command.operation);
      if (!origin.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(origin.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      const auto result =
          composition.active_addressed_kernel()->execute(
              *command.operation);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"accepted", AcceptedValue(result.value())},
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("submit")},
          {"ok", CanonicalValue(true)},
      });
    }
    if (command.kind == CommandKind::kGetRuntimeScaleControl) {
      const auto status =
          composition.active_addressed_kernel()
              ->get_runtime_scale_control(
              *command.runtime_scale_control_selector);
      if (!status.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(status.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("get_runtime_scale_control")},
          {"ok", CanonicalValue(true)},
          {"runtime_scale_control",
           RuntimeScaleControlStatusValue(status.value())},
      });
    }
    if (command.kind == CommandKind::kSetOperationRuntimeScale) {
      const auto result =
          composition.active_addressed_kernel()->set_runtime_scale(
              *command.operation_runtime_scale);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message);
      const auto &value = result.value();
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("set_operation_runtime_scale")},
          {"ok", CanonicalValue(true)},
          {"accepted",
           CanonicalValue(CanonicalValue::Object{
               {"request_id", CanonicalValue(value.request_id.value())},
               {"operation_id", CanonicalValue(value.operation_id.value())},
               {"operation_revision", CanonicalValue(static_cast<double>(
                                          value.operation_revision.value()))},
               {"runtime_scale_generation",
                CanonicalValue(static_cast<double>(
                    value.runtime_scale_generation.value()))},
               {"requested_scale", CanonicalValue(value.requested_scale)},
           })},
      });
    }
    if (command.kind == CommandKind::kSetRuntimeScaleControl) {
      const auto result =
          composition.active_addressed_kernel()
              ->set_runtime_scale_control(
              *command.runtime_scale_control);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"accepted", RuntimeScaleControlAcceptedValue(result.value())},
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("set_runtime_scale_control")},
          {"ok", CanonicalValue(true)},
      });
    }
    if (command.kind == CommandKind::kApplyOperationRecovery) {
      const auto result =
          composition.active_addressed_kernel()
              ->apply_operation_recovery_action(
                  *command.operation_id, *command.expected_operation_revision,
                  *command.operation_recovery);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("apply_operation_recovery")},
          {"ok", CanonicalValue(true)},
          {"result", RecoveryActionResultValue(result.value())},
      });
    }
    if (command.kind == CommandKind::kStartProgramSelection ||
        command.kind == CommandKind::kReleaseProgramSelection) {
      const auto result =
          command.kind == CommandKind::kStartProgramSelection
              ? composition.active_addressed_kernel()
                    ->start_program_selection(
                    *command.program_selection, command.expected_selection_revision)
              : composition.active_addressed_kernel()
                    ->release_program_selection(
                    *command.program_selection, command.expected_selection_revision);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message,
                             result.error().details);
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue(
                          command.kind == CommandKind::kStartProgramSelection
                              ? "start_program_selection"
                              : "release_program_selection")},
          {"ok", CanonicalValue(true)},
          {"scope", ProgramExecutionScopeValue(result.value())}});
    }
    if (command.kind == CommandKind::kCheckProgramNodeMotionAttention) {
      const auto result =
          composition.active_addressed_kernel()
              ->check_program_node_motion_attention(
                  *command.program_node_attention);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message,
                             result.error().details);
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command",
           CanonicalValue("check_program_node_motion_attention")},
          {"ok", CanonicalValue(true)}});
    }
    if (command.kind == CommandKind::kReleaseProgramRunStopFence) {
      const auto result =
          composition.active_addressed_kernel()
              ->release_program_run_stop_fence(
              *command.program_run_id, *command.stop_id,
              *command.stop_generation);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("release_program_run_stop_fence")},
          {"ok", CanonicalValue(true)},
      });
    }
    const auto control_result =
        [&]() -> core::Result<core::OperationControlAcceptedDTO> {
      switch (command.kind) {
      case CommandKind::kPause:
        return composition.active_addressed_kernel()->pause(
            *command.control);
      case CommandKind::kResume:
        return composition.active_addressed_kernel()->resume(
            *command.control);
      case CommandKind::kReset:
        return composition.active_addressed_kernel()->reset(
            *command.control);
      case CommandKind::kAcknowledgeCompleted:
        return composition.active_addressed_kernel()
            ->acknowledge_completed(
            *command.control);
      case CommandKind::kStatus:
      case CommandKind::kIdentity:
      case CommandKind::kTargets:
      case CommandKind::kQueryTarget:
      case CommandKind::kConfirmGrasp:
      case CommandKind::kQueryOperation:
      case CommandKind::kControlState:
      case CommandKind::kObservation:
      case CommandKind::kEvents:
      case CommandKind::kPreviewMotion:
      case CommandKind::kReadManualHandover:
      case CommandKind::kPreviewCapabilityCommand:
      case CommandKind::kRegisterProgramCapture:
      case CommandKind::kSubmit:
      case CommandKind::kRead:
      case CommandKind::kReadStop:
      case CommandKind::kGetOperationRecovery:
      case CommandKind::kApplyOperationRecovery:
      case CommandKind::kStop:
      case CommandKind::kStopProgramRun:
      case CommandKind::kStopProgramSelection:
      case CommandKind::kReadProgramExecutionScope:
      case CommandKind::kStartProgramSelection:
      case CommandKind::kReleaseProgramSelection:
      case CommandKind::kCheckProgramNodeMotionAttention:
      case CommandKind::kReleaseProgramRunStopFence:
      case CommandKind::kFullStop:
      case CommandKind::kMalformedStop:
      case CommandKind::kGetStopFault:
      case CommandKind::kRecoverStopFault:
      case CommandKind::kSetRuntimeScaleControl:
      case CommandKind::kSetOperationRuntimeScale:
      case CommandKind::kGetRuntimeScaleControl:
      case CommandKind::kPlanPick:
      case CommandKind::kPlanPlace:
        Fail("execution command dispatch is inconsistent");
      }
      Fail("execution command dispatch is unreachable");
    }();
    if (!control_result.ok()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(control_result.error())},
          {"ok", CanonicalValue(false)},
      });
    }
    return CanonicalValue(CanonicalValue::Object{
        {"accepted", ControlAcceptedValue(control_result.value())},
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue(std::string(
                        ControlName(control_result.value().control)))},
        {"ok", CanonicalValue(true)},
    });
  }

  [[nodiscard]] std::optional<core::ErrorDTO> LastReconcileError() const {
    std::lock_guard lock(reconcile_error_mutex_);
    return last_reconcile_error_;
  }

  [[nodiscard]] core::Result<void>
  ReconcileDirectControlOperations(Composition &composition) const {
    // One exact Kernel claim identifies manual work, independent of caller
    // labels. Reuse this owner lane; browsers never trigger terminal release.
    return composition.active_addressed_kernel()
        ->reconcile_manual_operation_admission();
  }

  void RunReconciliationLane(std::stop_token stop_token) noexcept {
    try {
      std::optional<core::TimePoint> last_recovery_observation;
      std::optional<core::ErrorDTO> recovery_error;
      while (!stop_token.stop_requested()) {
#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
        const char *marker =
            std::getenv("MANYFORGE_RECONCILIATION_TEST_MARKER");
        const char *gate = std::getenv("MANYFORGE_RECONCILIATION_TEST_GATE");
        if (marker != nullptr && marker[0] != '\0' && gate != nullptr &&
            gate[0] != '\0') {
          const int descriptor =
              ::open(marker, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
          if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
          }
          while (!stop_token.stop_requested() && ::access(gate, F_OK) != 0) {
            pollfd interruptible_wait{-1, 0, 0};
            static_cast<void>(
                ::poll(&interruptible_wait, 0, kReconcilePeriodMilliseconds));
          }
        }
        const char *injected_failure =
            std::getenv("MANYFORGE_RECONCILIATION_TEST_THROW");
        if (!stop_token.stop_requested() && injected_failure != nullptr &&
            injected_failure[0] != '\0') {
          throw std::runtime_error(injected_failure);
        }
#endif
        {
          // This shared lifetime lock prevents Manager reconfiguration from
          // retiring a composition while its subordinate owners are in use.
          std::shared_lock lock(composition_access_.mutex);
          if (composition_access_.current != nullptr) {
            auto &composition = *composition_access_.current;
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
            const auto now = composition.runtime->clock->now();
#else
            const auto cell_clock = composition.cell_runtime != nullptr
                                        ? composition.cell_runtime->clock()
                                    : composition.runtime != nullptr
                                        ? composition.runtime->clock
                                        : nullptr;
            Require(cell_clock != nullptr,
                    "native reconciliation has no stable cell clock");
            const auto now = cell_clock->now();
#endif
            if (!last_recovery_observation ||
                now < *last_recovery_observation ||
                now - *last_recovery_observation >=
                    kRecoveryObservationPeriodNs) {
              last_recovery_observation = now;
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
              const bool has_route_reconciler = static_cast<bool>(
                  composition.runtime->reconcile_controller_routes);
              const auto routes =
                  has_route_reconciler
                      ? composition.runtime->reconcile_controller_routes()
                      : core::Result<void>::Ok();
#else
              const bool has_route_reconciler =
                  static_cast<bool>(composition.cclc_cell_route_reconciler) ||
                  (composition.runtime != nullptr &&
                   static_cast<bool>(
                       composition.runtime->reconcile_controller_routes));
              const auto routes =
                  composition.cclc_cell_route_reconciler
                      ? composition.cclc_cell_route_reconciler()
                      : composition.runtime != nullptr &&
                                composition.runtime->reconcile_controller_routes
                            ? composition.runtime->reconcile_controller_routes()
                            : core::Result<void>::Ok();
#endif
              if (has_route_reconciler && routes.ok()) {
                composition.route_recovery_pending.store(false,
                                                         std::memory_order_release);
              }
              const auto assessed =
                  composition.active_addressed_kernel()
                      ->refresh_stop_fault_recovery_assessment();
              recovery_error = !routes.ok() ? std::optional(routes.error())
                               : !assessed.ok()
                                   ? std::optional(assessed.error())
                                   : std::nullopt;
            }
            auto error = [&]() -> std::optional<core::ErrorDTO> {
              const auto reconciled = composition.active_addressed_kernel()
                                          ->reconcile_subordinate_state();
              if (!reconciled.ok()) {
                return reconciled.error();
              }
              const auto manual_settlement =
                  ReconcileDirectControlOperations(composition);
              if (!manual_settlement.ok())
                return manual_settlement.error();
              return std::nullopt;
            }();
            std::lock_guard error_lock(reconcile_error_mutex_);
            last_reconcile_error_ = error ? std::move(error) : recovery_error;
          }
        }
        pollfd interruptible_wait{-1, 0, 0};
        int waited = -1;
        do {
          waited = ::poll(&interruptible_wait, 0, kReconcilePeriodMilliseconds);
        } while (waited < 0 && errno == EINTR && !stop_token.stop_requested());
        if (waited < 0 && !stop_token.stop_requested()) {
          return;
        }
      }
    } catch (...) {
      const auto failure = std::current_exception();
      {
        std::lock_guard lock(reconcile_error_mutex_);
        reconciliation_lane_failure_ = failure;
      }
      reconciliation_lane_failed_.store(true, std::memory_order_release);
      WakeLanes();
    }
  }

  /// Socket lanes only wait for clients; they need no periodic wakeup.
  /// Every reason to leave a lane (its stop request, a failed lane, a process
  /// stop signal) makes one of these descriptors readable.
  [[nodiscard]] int WaitForSocket(const int socket, short &revents) const {
    std::array<pollfd, 3U> descriptors{{
        {socket, POLLIN, 0},
        {lane_wake_.get(), POLLIN, 0},
        // A negative descriptor is ignored by poll().
        {g_stop_signal_wake_fd.load(std::memory_order_relaxed), POLLIN, 0},
    }};
    const int ready = ::poll(descriptors.data(), descriptors.size(), -1);
    revents = descriptors[0].revents;
    return ready;
  }

  /// Ends every socket-lane wait. The wake stays readable: it is raised only
  /// when this surface is stopping.
  void WakeLanes() noexcept {
    const std::uint64_t one = 1U;
    static_cast<void>(::write(lane_wake_.get(), &one, sizeof(one)));
  }

  [[nodiscard]] bool LanesWoken() const noexcept {
    pollfd wake{lane_wake_.get(), POLLIN, 0};
    return ::poll(&wake, 1, 0) > 0 && (wake.revents & POLLIN) != 0;
  }

  void RunControlLane(std::stop_token stop_token) noexcept {
    const std::stop_callback wake_on_stop(stop_token,
                                          [this] { WakeLanes(); });
    while (!stop_token.stop_requested() && g_stop_requested == 0 &&
           !LanesWoken()) {
      short revents = 0;
      int ready = -1;
      do {
        ready = WaitForSocket(control_socket_.get(), revents);
      } while (ready < 0 && errno == EINTR && !stop_token.stop_requested());
      if (stop_token.stop_requested()) {
        return;
      }
      if (ready < 0 || (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        control_socket_failed_.store(true, std::memory_order_release);
        WakeLanes();
        return;
      }
      if ((revents & POLLIN) != 0) {
        HandleOneClient(control_socket_, CommandLane::kStop);
      }
    }
  }

  void RunObservationLane(std::stop_token stop_token) noexcept {
    const std::stop_callback wake_on_stop(stop_token,
                                          [this] { WakeLanes(); });
    while (!stop_token.stop_requested() && g_stop_requested == 0 &&
           !LanesWoken()) {
      short revents = 0;
      int ready = -1;
      do {
        ready = WaitForSocket(observation_socket_.get(), revents);
      } while (ready < 0 && errno == EINTR && !stop_token.stop_requested());
      if (stop_token.stop_requested() || ready < 0 ||
          (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return;
      }
      if ((revents & POLLIN) != 0) {
        HandleOneClient(observation_socket_, CommandLane::kObservation,
                        stop_token);
      }
    }
  }

  void HandleOneClient(const CommandSocket &socket, const CommandLane lane,
                       const std::stop_token stop_token = {}) noexcept {
    const bool ordinary = lane == CommandLane::kOrdinary;
    try {
      FileDescriptor client(::accept4(socket.get(), nullptr, nullptr,
                                      SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (!client) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          return;
        }
        FailErrno("accept execution command client");
      }
      ucred peer{};
      socklen_t peer_size = sizeof(peer);
      Require(::getsockopt(client.get(), SOL_SOCKET, SO_PEERCRED, &peer,
                           &peer_size) == 0 &&
                  peer_size == sizeof(peer) && peer.uid == ::geteuid(),
              "execution command client is not the same user");
      bool first_packet = true;
      while (!stop_token.stop_requested()) {
        pollfd inbound{client.get(), POLLIN, 0};
        int packet_ready = -1;
        do {
          packet_ready =
              ::poll(&inbound, 1,
                     first_packet ? kClientPacketDeadlineMilliseconds : 1'000);
        } while (packet_ready < 0 && errno == EINTR);
        if (lane == CommandLane::kObservation && !first_packet &&
            packet_ready == 0) {
          continue;
        }
        Require(packet_ready > 0 && (inbound.revents & POLLIN) != 0,
                "execution command client did not send a packet in time");
        first_packet = false;
        std::array<char, kMaximumPacketBytes> buffer{};
        iovec vector{buffer.data(), buffer.size()};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1U;
        ssize_t count = -1;
        do {
          count = ::recvmsg(client.get(), &message, 0);
        } while (count < 0 && errno == EINTR);
        CanonicalValue response;
        std::optional<std::string> cell_admission_command;
        std::optional<std::pair<DeferredLane *, Command>> deferred;
        if (count <= 0 || (message.msg_flags & MSG_TRUNC) != 0) {
          response =
              ErrorResponse("execution_command_invalid",
                            "execution command packet is empty or oversized");
        } else {
          try {
            const auto bytes = std::string_view(
                buffer.data(), static_cast<std::size_t>(count));
            for (const auto candidate :
                 {"cell_admission_get", "cell_admission_update_readiness",
                  "cell_admission_acquire", "cell_admission_release",
                  "cell_admission_reconcile",
                  "cell_admission_recovery_receipt",
                  "cell_admission_publish_operator_alarm_attention"}) {
              if (bytes.find(std::string("\"command\":\"") + candidate +
                             "\"") != std::string_view::npos) {
                cell_admission_command = candidate;
                break;
              }
            }
            const auto cell_admission =
                detail::TryDecodeCellAdmissionCommand(bytes);
            if (cell_admission.has_value()) {
              const bool attention =
                  cell_admission->command ==
                  component_manager::detail::
                      kPublishOperatorAlarmAttentionCommand;
              if (lane == CommandLane::kObservation) {
                response = detail::CellAdmissionResponseValue(
                    cell_admission->command,
                    core::ErrorDTO{
                        "execution_observation_command_unsupported",
                        "the independent observation socket accepts only "
                        "observation",
                        {}});
              } else if (lane == CommandLane::kStop && !attention) {
                response = detail::CellAdmissionResponseValue(
                    cell_admission->command,
                    core::ErrorDTO{
                        "execution_control_command_unsupported",
                        "the independent Stop control socket accepts only "
                        "Stop, Stop status and operator-alarm attention",
                        {}});
              } else if (lane == CommandLane::kOrdinary && attention) {
                response = detail::CellAdmissionResponseValue(
                    cell_admission->command,
                    core::ErrorDTO{
                        "execution_ordinary_command_unsupported",
                        "operator-alarm attention is published on the "
                        "independent Stop control socket",
                        {}});
              } else {
                // Cell admission is owned by the stable cell Kernel and never
                // reads the composition, so an Apply holding the composition
                // exclusively does not delay it.
                if (PreemptsPlanning(*cell_admission)) {
                  PreemptPlanning();
                }
                auto result =
                    component_manager::detail::DispatchKernelCellAdmissionRpc(
                        StableKernel(), *cell_admission);
                response = detail::CellAdmissionResponseValue(
                    cell_admission->command, result);
              }
            } else {
              const auto command = DecodeCommand(bytes);
              if (lane != CommandLane::kObservation &&
                  PreemptsPlanning(command.kind)) {
                PreemptPlanning();
              }
              if (lane == CommandLane::kStop) {
                // Stop and its in-memory progression reads reach the stable
                // Kernel without the composition lock. It owns exact scope
                // resolution, journal fences and unavailable evidence, also
                // while no physical product is composed; the host cannot
                // infer rest from the absence of one.
                auto &owner = StableKernel();
                response = command.kind == CommandKind::kReadStop
                               ? ReadStop(command, owner)
                           : command.kind == CommandKind::kGetStopFault
                               ? ReadStopFault(owner)
                               : ExecuteStop(command, owner);
              } else if (lane == CommandLane::kObservation &&
                         command.kind != CommandKind::kObservation) {
                response = ErrorResponse(
                    "execution_observation_command_unsupported",
                    "the independent observation socket accepts only "
                    "observation");
              } else if (IsStopLaneRead(command.kind)) {
                response = ErrorResponse(
                    "execution_ordinary_command_unsupported",
                    "Stop status and Stop-fault reads are served by the "
                    "independent Stop control socket");
              } else {
                std::shared_lock composition_lock(composition_access_.mutex);
                if (composition_access_.current == nullptr) {
                  response = OwnerNotReadyResponse(command);
                } else if (lane == CommandLane::kObservation &&
                           command.observation_cursor_supplied) {
                  composition_lock.unlock();
                  response = ObservationResponseAfter(command, client.get(),
                                                      stop_token);
                } else if (ordinary && IsSubmissionOrdered(command.kind)) {
                  deferred.emplace(&submission_ordered_lane_, command);
                } else if (ordinary && ReadsManagerScene(command.kind)) {
                  deferred.emplace(&scene_read_lane_, command);
                } else {
                  response = Execute(
                      command, *composition_access_.current,
                      composition_access_.current->projection_revision,
                      std::stop_token{});
                  if (command.kind == CommandKind::kReset) {
                    const auto *accepted = response.find_object_member("ok");
                    if (accepted != nullptr && accepted->is_bool() &&
                        accepted->as_bool()) {
                      PreemptPlanning();
                    }
                  }
                }
              }
            }
          } catch (const std::exception &error) {
            response =
                cell_admission_command.has_value()
                    ? detail::CellAdmissionResponseValue(
                          *cell_admission_command,
                          core::ErrorDTO{
                              "execution_command_invalid", error.what(), {}})
                    : ErrorResponse("execution_command_invalid", error.what());
          }
        }
        if (deferred.has_value()) {
          // The composition lock is released: the deferred lane takes its own
          // while it executes.
          auto &[deferred_lane, deferred_command] = *deferred;
          if (EnqueueDeferred(*deferred_lane, client,
                              std::move(deferred_command))) {
            return;
          }
          response = ErrorResponse(
              std::string(deferred_lane->busy_code),
              "the execution owner already holds its bounded number of "
              "pending requests on this lane");
        }
        SendResponse(client.get(), response, cell_admission_command);
        if (lane != CommandLane::kObservation) {
          return;
        }
      }
    } catch (...) {
      // A malformed/disappearing private client cannot retire the sole owner.
    }
  }

  void SendResponse(
      const int client, const CanonicalValue &response,
      const std::optional<std::string> &cell_admission_command) const {
    auto bytes = content_identity::canonical_json(response);
    if (bytes.empty() || bytes.size() > kMaximumResponseBytes) {
      const core::ErrorDTO error{
          "execution_command_response_oversized",
          "execution owner result exceeds the bounded private transport",
          {}};
      bytes = content_identity::canonical_json(
          cell_admission_command.has_value()
              ? detail::CellAdmissionResponseValue(*cell_admission_command,
                                                   error)
              : ErrorResponse(error.code, error.message));
    }
    pollfd outbound{client, POLLOUT, 0};
    int response_ready = -1;
    do {
      response_ready = ::poll(&outbound, 1, kClientPacketDeadlineMilliseconds);
    } while (response_ready < 0 && errno == EINTR);
    Require(response_ready > 0 && (outbound.revents & POLLOUT) != 0,
            "execution command client did not accept a response in time");
    ssize_t sent = -1;
    do {
      sent = ::send(client, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    Require(sent == static_cast<ssize_t>(bytes.size()),
            "execution command response was not sent atomically");
  }

  /// Lane and ordering contract of the execution sockets.
  ///
  /// Every ordinary and control connection carries one request and its
  /// response, so a client that needs A before B already waits for A's
  /// response; requests on different connections or sockets are ordered by
  /// nothing but the owner fences below, never by arrival.
  ///
  /// - Control socket: Stop, Program Stop, Program-selection Stop, full and
  ///   malformed Stop, plus the in-memory reads Stop progression needs
  ///   (Stop status, Stop fault) and the operator-alarm attention heartbeat.
  ///   It reaches only the stable Kernel, without the composition lock, so
  ///   neither planning nor an Apply delays it. Each request is a bounded
  ///   copy under the Kernel's state lock.
  /// - Ordinary socket: operation controls (Reset, Pause, Resume,
  ///   acknowledgement), events, reads, Program selection and capture, cell
  ///   admission and recovery. It never waits for planning, submission
  ///   ordering or a Manager round trip; those go to deferred lanes.
  /// - Submission-ordered lane: requests whose Kernel entry takes submission
  ///   ordering (IsSubmissionOrdered). One worker, FIFO in accept order, one
  ///   in flight, as the single ordinary lane executed them before.
  /// - Scene-read lane: projections that first read the live Scene revision
  ///   from Manager (ReadsManagerScene). One worker, FIFO.
  ///
  /// Cross-lane fences: Reset, recovery and Stop-fence release name the
  /// exact operation revision, Stop id and generation, or Stop-fault latch
  /// they act on, and their clients issue them only after reading that state
  /// on the control socket. A stale name is refused (the Program owner also
  /// refuses a cycle Reset while its Stop is not terminal); a Kernel Reset of
  /// a participant still stopping hard-stops it first, so no ordering of the
  /// two sockets lets Reset restart motion or release a newer Stop. Alarm
  /// attention is fenced by owner epoch, incarnation and monotonic generation
  /// and evaluated at admission only by age, so admission on another lane
  /// observes either the previous or the new publication, both valid. Event
  /// pages are journal-sequence ordered.
  [[nodiscard]] core::ResourceAddressedExecutionKernel &StableKernel() const {
    auto *const owner = composition_access_.addressed_kernel.load();
    Require(owner != nullptr,
            "stable addressed execution Kernel is not installed");
    return *owner;
  }

  [[nodiscard]] CanonicalValue
  OwnerNotReadyResponse(const Command &command) const {
    if (command.kind == CommandKind::kIdentity) {
      return ExecutionOwnerNotReadyIdentityResponse(StableKernel(),
                                                    owner_epoch_);
    }
    return ErrorResponse("execution_owner_not_ready",
                         "the managed execution owner has no active physical "
                         "product",
                         {{"owner_epoch", owner_epoch_.value()}});
  }

  struct PendingDeferredCommand final {
    FileDescriptor client;
    Command command;
    std::stop_source cancellation;
  };

  struct DeferredLane final {
    explicit DeferredLane(const std::string_view busy) : busy_code(busy) {}
    const std::string_view busy_code;
    std::mutex mutex;
    std::condition_variable_any ready;
    std::deque<PendingDeferredCommand> queue;
    std::optional<std::stop_source> in_flight;
  };

  [[nodiscard]] bool EnqueueDeferred(DeferredLane &lane, FileDescriptor &client,
                                     Command command) {
    {
      std::lock_guard lock(lane.mutex);
      const auto pending =
          lane.queue.size() + (lane.in_flight.has_value() ? 1U : 0U);
      if (pending >= kMaximumPendingDeferredCommands) {
        return false;
      }
      lane.queue.push_back(PendingDeferredCommand{
          std::move(client), std::move(command), std::stop_source{}});
    }
    lane.ready.notify_one();
    return true;
  }

  /// Cancels every advisory planning query already accepted, executing or
  /// queued; a queued one then reaches the Kernel already cancelled. Only
  /// planning consults the token: a queued submit or read is unaffected.
  /// Called before a Stop or a component-lifecycle cell-admission claim is
  /// dispatched and after a Reset is accepted; none waits for the cancelled
  /// planning to return. (The Kernel's own
  /// Reset of a participant additionally preempts that participant's preview
  /// before taking its lifecycle lock.)
  void PreemptPlanning() noexcept {
    std::lock_guard lock(submission_ordered_lane_.mutex);
    for (auto &pending : submission_ordered_lane_.queue) {
      pending.cancellation.request_stop();
    }
    if (submission_ordered_lane_.in_flight.has_value()) {
      submission_ordered_lane_.in_flight->request_stop();
    }
  }

  [[nodiscard]] CanonicalValue
  DeferredResponse(const Command &command, const std::stop_token cancellation,
                   const std::stop_token lane_stop_token) {
    std::shared_lock composition_lock(composition_access_.mutex);
    if (composition_access_.current == nullptr) {
      return OwnerNotReadyResponse(command);
    }
    auto projection = composition_access_.current->projection_revision;
    const auto scene = composition_access_.current->scene_authority_proxy;
    if (ReadsManagerScene(command.kind) && scene != nullptr) {
      const auto generation =
          composition_access_.current->publication_generation;
      // Manager may be waiting to replace this composition. Never retain
      // its lock across a request back to Manager. The shared proxy
      // outlives the unlocked read; only the exact captured publication may
      // consume its result.
      composition_lock.unlock();
      projection = scene->CurrentProjectionRevision(lane_stop_token);
      composition_lock.lock();
      Require(composition_access_.current != nullptr &&
                  composition_access_.current->publication_generation ==
                      generation &&
                  composition_access_.current->scene_authority_proxy == scene,
              "execution identity changed during Scene read");
    }
    return Execute(command, *composition_access_.current, projection,
                   cancellation);
  }

  void RunDeferredLane(DeferredLane &lane,
                       const std::stop_token stop_token) noexcept {
    for (;;) {
      std::optional<PendingDeferredCommand> pending;
      {
        std::unique_lock lock(lane.mutex);
        if (!lane.ready.wait(lock, stop_token,
                             [&lane] { return !lane.queue.empty(); })) {
          // Closing the queued clients is their answer: the owner is leaving.
          lane.queue.clear();
          return;
        }
        pending.emplace(std::move(lane.queue.front()));
        lane.queue.pop_front();
        lane.in_flight = pending->cancellation;
      }
      const std::stop_callback cancel_on_shutdown(
          stop_token, [source = pending->cancellation]() mutable {
            source.request_stop();
          });
      try {
        CanonicalValue response;
        try {
          response = DeferredResponse(pending->command,
                                      pending->cancellation.get_token(),
                                      stop_token);
        } catch (const std::exception &error) {
          response = ErrorResponse("execution_command_invalid", error.what());
        }
        {
          std::lock_guard lock(lane.mutex);
          lane.in_flight.reset();
        }
        SendResponse(pending->client.get(), response, std::nullopt);
      } catch (...) {
        // A disappearing private client cannot retire the sole owner.
        std::lock_guard lock(lane.mutex);
        lane.in_flight.reset();
      }
    }
  }

  core::RequestOwnerEpoch owner_epoch_;
  core::ProgramRunId direct_control_program_run_id_;
  core::ProgramRunId direct_control_gripper_program_run_id_;
  ExecutionCompositionAccess &composition_access_;
  component_manager::detail::ManagerLivenessCommandAdmission
      &manager_liveness_admission_;
  HostLock host_lock_;
  CommandSocket command_socket_;
  CommandSocket control_socket_;
  CommandSocket observation_socket_;
  FileDescriptor lane_wake_{::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)};
  std::atomic<bool> control_socket_failed_{false};
  std::atomic<bool> reconciliation_lane_failed_{false};
  mutable std::mutex reconcile_error_mutex_;
  std::optional<core::ErrorDTO> last_reconcile_error_;
  std::exception_ptr reconciliation_lane_failure_;
  DeferredLane submission_ordered_lane_{"execution_submission_lane_busy"};
  DeferredLane scene_read_lane_{"execution_scene_read_lane_busy"};
};

void RunManagedExecutionCommandSurface(
    ManagedExecutionSurfaceControl &control,
    const std::stop_token stop_token) noexcept {
  try {
    Require(control.composition_access != nullptr &&
                control.manager_liveness_admission != nullptr &&
                !control.owner_epoch.empty(),
            "managed execution command surface configuration is incomplete");
    ExecutionCommandSurface surface(
        control.run_root, *control.composition_access,
        *control.manager_liveness_admission, control.owner_epoch);
    // Unpublish the surface before it is destroyed, also when Run throws.
    struct Unpublish final {
      ManagedExecutionSurfaceControl &control;
      ~Unpublish() {
        std::lock_guard lock(control.state_mutex);
        control.surface = nullptr;
      }
    } const unpublish{control};
    {
      std::lock_guard lock(control.state_mutex);
      control.surface = &surface;
      control.ready = true;
    }
    control.state_changed.notify_all();
    static_cast<void>(surface.Run(stop_token));
  } catch (...) {
    {
      std::lock_guard lock(control.state_mutex);
      control.failure = std::current_exception();
    }
    control.state_changed.notify_all();
  }
}

void PreemptManagedExecutionSurfacePlanning(
    ManagedExecutionSurfaceControl &control) noexcept {
  std::lock_guard lock(control.state_mutex);
  if (control.surface != nullptr) {
    control.surface->PreemptPlanning();
  }
}

class NativeExecutionHost final {
public:
  explicit NativeExecutionHost(HostConfig config)
      : config_(std::move(config)),
        owner_epoch_(RandomToken("execution-host-")),
        runtime_epoch_(RandomToken("native-runtime-")), admission_gate_(),
        composition_(BuildComposition(config_, owner_epoch_, runtime_epoch_,
                                      &admission_gate_)) {
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    if (composition_->runtime == nullptr) {
      Require(composition_->gripper_runtime != nullptr &&
                  composition_->cell_runtime == nullptr,
              "native standalone composition has no addressed owner");
      auto registrations = NativeCellComponentRegistrations(*composition_);
      auto participant_resolver =
          NativeCellParticipantResolver(*composition_, registrations);
      auto stop_sources = NativeCellStopSources(*composition_);
      Require(!registrations.empty() && !stop_sources.empty(),
              "native gripper-only host has no verified Kernel participants");
      core::ResourceAddressedExecutionKernelConfig kernel_config{
          owner_epoch_, core::CellId(config_.cell_id), 1024U,
          std::move(participant_resolver), &admission_gate_,
          composition_->bundle->value().cell_graph.graph_revision};
      kernel_config.prequalified_max_live_stop_participants =
          kNativeMaximumLiveStopParticipants;
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
      const std::vector<core::RequiredManagerLivenessReceiverDTO>
          fixture_receivers;
      kernel_config.manager_liveness_required_set =
          core::ManagerLivenessRequiredSetDTO{
              core::CellId(config_.cell_id),
              composition_->bundle->retained_bundle_id(),
              core::ComputeRequiredRuntimeSetDigest(fixture_receivers),
              core::RuntimeUnitId("standalone-admission-fixture.manager"),
              core::RuntimeEpoch("standalone-admission-fixture.manager-epoch"),
              core::RequestOwnerEpoch("standalone-admission-fixture.manager-owner"),
              fixture_receivers};
#endif
      const auto manager_apply_journal =
          config_.manager_state_root /
          std::string(component_manager::kManagerApplyJournalFilename);
      const auto lifecycle_journal =
          config_.execution_state_root /
          std::string(core::kCCLCJournalFilename);
      kernel_config.cell_admission_settlement_validator =
          BuildOwnerSettlementValidator(manager_apply_journal, {}, {},
                                        lifecycle_journal,
                                        core::CellId(config_.cell_id));
      kernel_config.cell_admission_reconcile_validator =
          BuildOwnerReconciliationValidator(manager_apply_journal, {},
                                            lifecycle_journal,
                                            core::CellId(config_.cell_id));
      auto cell_clock = std::make_shared<WallClock>();
      auto cell_scheduler = std::make_shared<SimpleScheduler>(
          SimpleSchedulerConfig::from_environment());
      gripper_only_cell_runtime_ =
          std::make_unique<RetainedNativeCellRuntime>(
              RetainedNativeCellRuntimeInput{
                  .clock = std::move(cell_clock),
                  .scheduler = std::move(cell_scheduler),
                  .operation_journal_path =
                      config_.execution_state_root /
                      std::string(core::kOperationJournalFilename),
                  .participant_registrations = std::move(registrations),
                  .kernel_config = std::move(kernel_config),
                  .stop_sources = std::move(stop_sources),
                  .stop_source_mode =
                      RetainedStopSurveillanceEmptySourceMode::kRequireNonEmpty});
      composition_->cell_runtime = gripper_only_cell_runtime_.get();
    }
#endif
    // Motion compositions bind this shared admission gate while constructing
    // their Kernel. The standalone gripper-only composition creates its
    // stable cell Kernel above, so bind its resource table explicitly before
    // the durable fence owner can observe or reconcile route publications.
    admission_gate_.BindResourceTable(
        composition_->active_addressed_kernel()->resource_admission_table());
    const auto runtime_units = composition_->host_runtime_unit_epochs();
    Require(runtime_units.size() == 1U,
            "standalone native host has no sole runtime-unit identity");
    fence_owner_ = std::make_unique<ManagedKernelApplyFenceOwner>(
        ManagedKernelApplyFenceOwnerConfig{
            config_.execution_state_root / "kernel-apply-fence.sqlite3",
            config_.manager_state_root, config_.package_root,
            core::CellId(config_.cell_id),
            runtime_units.front().runtime_unit_id, runtime_epoch_, owner_epoch_,
            [] {
              return std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                  .count();
            }},
        admission_gate_);
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
    const auto fixture_readiness =
        composition_->active_addressed_kernel()
            ->UpdateCellAdmissionReadiness(
            core::CellAdmissionReadinessSnapshotDTO{
                true,
                core::ProgramCycleStatusDTO{
                    core::RequestOwnerEpoch(
                        "standalone-admission-fixture.program-runner"),
                    1U, core::ProgramCycleStateDTO::kIdleAfterReset,
                    std::nullopt, std::nullopt, std::nullopt},
                true, 0U, 0U, true, true, 0U, 0U, 0U});
    // Readiness publication is not permission to move. Retained claims and
    // faults must reach real reconciliation/ingress while admission stays
    // closed; do not require authoring eligibility before recovery can run.
    Require(fixture_readiness.ok(),
            "standalone admission fixture could not publish readiness");
#endif
    AttachRetainedNativeCCLC(*composition_, config_, *fence_owner_);
    composition_access_.current = composition_.get();
    composition_access_.addressed_kernel =
        composition_->active_addressed_kernel();
    // This legacy standalone owner has no Manager lease or liveness receiver.
    // Explicitly keep only its private admission seam open; managed children
    // use a separate, initially closed admission instance.
    standalone_command_admission_.OpenIfUnchanged(
        standalone_command_admission_.generation());
    command_surface_ = std::make_unique<ExecutionCommandSurface>(
        config_.execution_run_root, composition_access_,
        standalone_command_admission_, owner_epoch_);
  }

  ~NativeExecutionHost() {
    command_surface_.reset();
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    if (gripper_only_cell_runtime_ != nullptr && composition_ != nullptr) {
      DetachRetainedCCLC(*composition_);
      composition_->cell_runtime = nullptr;
      gripper_only_cell_runtime_.reset();
    }
#endif
  }

  [[nodiscard]] int Run() { return command_surface_->Run(); }

private:
  HostConfig config_;
  core::RequestOwnerEpoch owner_epoch_;
  // The development launcher remains one process incarnation while a5.5
  // qualifies and then retires it in favor of the Manager-owned surface.
  core::RuntimeEpoch runtime_epoch_;
  ManagedKernelAdmissionGate admission_gate_;
  std::unique_ptr<ManagedKernelApplyFenceOwner> fence_owner_;
  std::unique_ptr<Composition> composition_;
  std::unique_ptr<RetainedNativeCellRuntime> gripper_only_cell_runtime_;
  ExecutionCompositionAccess composition_access_;
  component_manager::detail::ManagerLivenessCommandAdmission
      standalone_command_admission_;
  std::unique_ptr<ExecutionCommandSurface> command_surface_;
};

[[nodiscard]] HostConfig ParseArguments(int argc, char **argv) {
  HostConfig config;
  std::set<std::string> seen;
  for (int index = 1; index < argc; index += 2) {
    Require(index + 1 < argc, "every option requires one value");
    const std::string option(argv[index]);
    const std::string value(argv[index + 1]);
    Require(seen.insert(option).second, "duplicate option " + option);
    if (option == "--package-root") {
      config.package_root = value;
    } else if (option == "--manager-state-root") {
      config.manager_state_root = value;
    } else if (option == "--execution-state-root") {
      config.execution_state_root = value;
    } else if (option == "--execution-run-root") {
      config.execution_run_root = value;
    } else if (option == "--program-binding-id") {
      config.program_binding_id = value;
    } else if (option == "--motion-target-role-id") {
      config.motion_target_role_id = value;
    } else if (option == "--gripper-target-role-id") {
      config.gripper_target_role_id = value;
    } else if (option == "--candidate-worker") {
      config.candidate_worker = value;
    } else if (option == "--cell-id") {
      config.cell_id = value;
    } else {
      Fail("unknown option " + option);
    }
  }
  const auto has = [&](const std::string_view option) {
    return seen.contains(std::string(option));
  };
  Require(has("--package-root") && has("--manager-state-root") &&
              has("--execution-state-root") &&
              has("--execution-run-root") && has("--program-binding-id") &&
              has("--candidate-worker") && has("--cell-id") &&
              (has("--motion-target-role-id") ||
               has("--gripper-target-role-id")) &&
              seen.size() <= 9U,
          "package, Manager state, execution state/run, Program Binding, "
          "candidate worker, cell, and at least one supported target role "
          "are required");
  config.package_root =
      ExistingDirectory(std::move(config.package_root), "package root");
  config.manager_state_root = ExistingPrivateDirectory(
      std::move(config.manager_state_root), "Manager state root");
  config.execution_state_root = ExistingPrivateDirectory(
      std::move(config.execution_state_root), "execution state root");
  config.execution_run_root = ExistingPrivateDirectory(
      std::move(config.execution_run_root), "execution run root");
  RequireDisjointDirectories(config.manager_state_root,
                             config.execution_state_root,
                             "standalone durable state capabilities");
  RequireDisjointDirectories(
      config.manager_state_root, config.execution_run_root,
      "standalone Manager-state/execution-run capabilities");
  RequireDisjointDirectories(config.execution_state_root,
                             config.execution_run_root,
                             "standalone execution state/run capabilities");
  RequireDisjointDirectories(config.package_root, config.manager_state_root,
                             "standalone install/Manager-state capabilities");
  RequireDisjointDirectories(config.package_root, config.execution_state_root,
                             "standalone install/execution-state capabilities");
  RequireDisjointDirectories(config.package_root, config.execution_run_root,
                             "standalone install/execution-run capabilities");
  config.candidate_worker = ExistingRegularFile(
      std::move(config.candidate_worker), "candidate worker", true);
  Require(!config.cell_id.empty() &&
              config.cell_id.size() <= kMaximumIdentityBytes &&
              !config.program_binding_id.empty() &&
              config.program_binding_id.size() <= kMaximumIdentityBytes &&
              (config.motion_target_role_id.empty() ||
               config.motion_target_role_id.size() <=
                   kMaximumIdentityBytes) &&
              (config.gripper_target_role_id.empty() ||
               config.gripper_target_role_id.size() <=
                   kMaximumIdentityBytes),
          "host identities must be bounded and non-empty");
  return config;
}

void InstallSignalHandlers() {
  if (g_stop_signal_wake_fd.load() < 0) {
    const int wake = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake < 0) {
      FailErrno("create execution-host stop-signal wake");
    }
    g_stop_signal_wake_fd.store(wake);
  }
  struct sigaction action {};
  action.sa_handler = HandleStopSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (::sigaction(SIGINT, &action, nullptr) != 0 ||
      ::sigaction(SIGTERM, &action, nullptr) != 0) {
    FailErrno("install execution-host signal handlers");
  }
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (::sigaction(SIGPIPE, &ignore, nullptr) != 0) {
    FailErrno("ignore execution-host SIGPIPE");
  }
}

} // namespace
} // namespace manyforge::roboplan_backend

int main(int argc, char **argv) {
  try {
    manyforge::roboplan_backend::InstallSignalHandlers();
    if (argc > 1 &&
        std::string_view(argv[1]) == "--manyforge-managed-runtime-channel-fd") {
      return manyforge::roboplan_backend::RunManagedRuntimeChild(
          manyforge::roboplan_backend::ParseManagedRuntimeArguments(argc,
                                                                    argv));
    }
    manyforge::roboplan_backend::NativeExecutionHost host(
        manyforge::roboplan_backend::ParseArguments(argc, argv));
    return host.Run();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 70;
  } catch (...) {
    std::cerr << "Native execution host failed with an unknown error\n";
    return 71;
  }
}
