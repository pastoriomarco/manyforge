#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
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
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <limits>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/execution_interrupt_support.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/manager_liveness_receiver.hpp>
#include <manyforge_core/pick_place_planning_owner.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#endif
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_binding_compiler.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_runtime_policy.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/stop_evidence_policy_codec.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/retained_native_cclc_runtime.hpp>
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
#include <manyforge_scene/canonical_empty_scene_owner.hpp>
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <manyforge_scene/scene_transaction_codec.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <set>
#include <shared_mutex>
#include <span>
#include <sstream>
#include <stdexcept>
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
constexpr int kSocketBacklog = 16;
constexpr int kReconcilePeriodMilliseconds = 5;
constexpr int kClientPacketDeadlineMilliseconds = 100;
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

void HandleStopSignal(int) { g_stop_requested = 1; }

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
  std::filesystem::path package_root;
  std::filesystem::path manager_state_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path logical_scene_database;
  std::filesystem::path program_cycle_journal;
  std::filesystem::path authoring_transaction_journal;
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
    } else if (option == "--manyforge-managed-manager-state-root") {
      config.manager_state_root = value;
    } else if (option == "--manyforge-managed-manager-run-root") {
      config.manager_run_root = value;
    } else if (option == "--manyforge-managed-logical-scene-database") {
      config.logical_scene_database = value;
    } else if (option == "--manyforge-managed-program-cycle-journal") {
      config.program_cycle_journal = value;
    } else if (option == "--manyforge-managed-authoring-transaction-journal") {
      config.authoring_transaction_journal = value;
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
              config.program_cycle_journal.empty() ==
                  config.authoring_transaction_journal.empty(),
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
  const int package_flags = ::fcntl(config.package_root_fd, F_GETFD);
  Require(::fstat(config.package_root_fd, &package_status) == 0 &&
              S_ISDIR(package_status.st_mode) && package_flags >= 0 &&
              (package_flags & FD_CLOEXEC) == 0,
          "managed package-root descriptor is not a retained directory");
  config.package_root = std::filesystem::path("/proc/self/fd") /
                        std::to_string(config.package_root_fd);
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
    config.authoring_transaction_journal = validate_future_database(
        std::move(config.authoring_transaction_journal),
        "managed Composer authoring transaction journal");
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
  }
  const std::array<const std::filesystem::path *, 3U> durable_paths{
      &config.logical_scene_database, &config.program_cycle_journal,
      &config.authoring_transaction_journal};
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

using ManagedProjectionKey = std::pair<core::CapabilityProviderBindingId,
                                       core::CapabilityProviderBindingRevision>;

class ManagedProjectionRegistry final {
public:
  explicit ManagedProjectionRegistry(const ManagedRuntimeChildConfig &config)
      : config_(config), bundle_source_(config.package_root),
        map_source_(config.package_root),
        planning_source_(config.package_root) {}

  [[nodiscard]] component_manager::detail::NativeProjectionRpcResponse Dispatch(
      const component_manager::detail::NativeProjectionRpcRequest &request) {
    std::lock_guard lock(mutex_);
    return std::visit(
        [&](const auto &value)
            -> component_manager::detail::NativeProjectionRpcResponse {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<T, component_manager::detail::
                                              NativeProjectionPrepareRequest>) {
            Prepare(value);
            return component_manager::detail::NativeProjectionPrepareResult{
                value};
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionHealthQuery>) {
            return Projection(value.native_runtime_binding).ProjectionHealth();
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionTransactionRequest>) {
            return Projection(value.native_runtime_binding)
                .ApplyTransaction(value.transaction);
          } else if constexpr (std::is_same_v<
                                   T, core::NativeProjectionRebuildDTO>) {
            return Projection(value.expected_native_runtime_binding)
                .RebuildProjection(value);
          } else if constexpr (std::is_same_v<
                                   T,
                                   core::NativeProjectionBootstrapRebuildDTO>) {
            return Projection(value.native_runtime_binding)
                .RebuildBootstrapProjection(value);
          } else if constexpr (
              std::is_same_v<T,
                             core::NativeProjectionBootstrapStatusQueryDTO>) {
            return Projection(value.native_runtime_binding)
                .BootstrapProjectionStatus(value);
          } else if constexpr (std::is_same_v<T,
                                              core::NativeProjectionStageDTO>) {
            return Projection(value.transition_row.native_runtime_binding)
                .StageProjection(value);
          } else if constexpr (std::is_same_v<
                                   T, core::NativeProjectionActivationDTO>) {
            return Projection(value.transition_row.native_runtime_binding)
                .ActivateStagedProjection(value);
          } else if constexpr (std::is_same_v<
                                   T, core::NativeProjectionRetireDTO>) {
            return Projection(value.transition_row.native_runtime_binding)
                .RetireProjection(value);
          } else if constexpr (
              std::is_same_v<T, core::NativeProjectionRetireStatusQueryDTO>) {
            return Projection(
                       value.request.transition_row.native_runtime_binding)
                .ProjectionRetireStatus(value);
          } else if constexpr (std::is_same_v<
                                   T, core::NativeProjectionDiscardDTO>) {
            return Projection(value.transition_row.native_runtime_binding)
                .DiscardStagedProjection(value);
          } else if constexpr (std::is_same_v<
                                   T, core::NativeProjectionStatusQueryDTO>) {
            return Projection(value.transition_row.native_runtime_binding)
                .ProjectionStageStatus(value);
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionRetainRequest>) {
            auto retained =
                Projection(value.native_runtime_binding)
                    .RetainProjectionBinding(value.projection_revision,
                                             value.projection_lease_id);
            if (!retained.ok()) {
              return component_manager::detail::NativeProjectionRpcFailure{
                  retained.error()};
            }
            return component_manager::detail::NativeProjectionRetainResult{
                std::move(retained.value())};
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionReleaseRequest>) {
            Projection(value.native_runtime_binding)
                .ReleaseProjection(value.projection_lease_id);
            return component_manager::detail::NativeProjectionReleaseResult{
                value.projection_lease_id};
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionSceneFrameRequest>) {
            auto resolved = Projection(value.native_runtime_binding)
                                .ResolveSceneFrame(value.query);
            if (!resolved.ok()) {
              return component_manager::detail::NativeProjectionRpcFailure{
                  resolved.error()};
            }
            return std::move(resolved.value());
          } else {
            auto projected = Projection(value.native_runtime_binding)
                                 .ProjectEffectiveCollisionPolicy(
                                     value.logical_scene,
                                     value.effective_policy_revision_id);
            if (!projected.ok()) {
              return component_manager::detail::NativeProjectionRpcFailure{
                  projected.error()};
            }
            return std::move(projected.value());
          }
        },
        request);
  }

  [[nodiscard]] std::shared_ptr<RetainedPlannerSceneProjection>
  FindActiveProjection(
      const core::ProjectionRevisionDTO &expected,
      const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    std::lock_guard lock(mutex_);
    RequireLocalBinding(binding);
    const auto found = projections_.find(Key(binding));
    if (found == projections_.end() || found->second == nullptr ||
        found->second->ProjectionHealth().native_runtime_binding != binding) {
      return nullptr;
    }
    const auto inventory =
        found->second->current_scene_snapshot_lease_inventory();
    return inventory.has_value() &&
                   inventory->projection_binding.projection_revision == expected
               ? found->second
               : nullptr;
  }

  [[nodiscard]] std::optional<core::ProjectionRevisionDTO>
  FindCurrentProjectionRevision(
      const core::ProjectionRevisionDTO &expected,
      const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    std::lock_guard lock(mutex_);
    RequireLocalBinding(binding);
    const auto found = projections_.find(Key(binding));
    if (found == projections_.end() || found->second == nullptr ||
        found->second->ProjectionHealth().native_runtime_binding != binding) {
      return std::nullopt;
    }
    const auto inventory =
        found->second->current_scene_snapshot_lease_inventory();
    if (!inventory.has_value()) {
      return std::nullopt;
    }
    const auto &projection = inventory->projection_binding.projection_revision;
    if (!projection.valid() ||
        projection.retained_bundle_id != expected.retained_bundle_id ||
        projection.graph_revision != expected.graph_revision ||
        projection.scene_revision.scene_id !=
            expected.scene_revision.scene_id ||
        !found->second->ProjectionHealth().healthy_for_target(projection)) {
      return std::nullopt;
    }
    return projection;
  }

  void InstallActiveFrameResolverSet(
      const deployment::RetainedBundle &bundle,
      const core::ProjectionRevisionDTO &expected,
      std::shared_ptr<const RetainedSceneFrameResolver> frame_resolver) {
    std::lock_guard lock(mutex_);
    Require(expected.valid() && frame_resolver != nullptr &&
                frame_resolver->matches_structural_revision(
                    expected.retained_bundle_id, expected.graph_revision) &&
                bundle.retained_bundle_id() == expected.retained_bundle_id &&
                bundle.value().cell_graph.graph_revision ==
                    expected.graph_revision,
            "active frame-resolver set does not name one retained product");

    std::set<ManagedProjectionKey> required_keys;
    std::vector<std::shared_ptr<RetainedPlannerSceneProjection>> required;
    for (const auto &provider : bundle.value().capability_provider_bindings) {
      if (!core::RequiresNativeSceneProjection(
              provider.capability_id,
              provider.capability_definition_revision)) {
        continue;
      }
      std::set<core::RuntimeUnitId> runtime_sources;
      for (const auto &endpoint : provider.endpoint_bindings) {
        if (endpoint.source_kind !=
            core::CapabilityEndpointSourceKind::kRuntimeUnit) {
          continue;
        }
        const auto *runtime_unit =
            std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
        Require(runtime_unit != nullptr && !runtime_unit->empty(),
                "active native projection has a malformed runtime endpoint");
        runtime_sources.insert(*runtime_unit);
      }
      Require(runtime_sources.size() == 1U,
              "active native projection does not resolve one runtime unit");
      if (runtime_sources.begin()->value() != config_.runtime_unit_id) {
        continue;
      }

      const ManagedProjectionKey key{
          provider.capability_provider_binding_id,
          provider.capability_provider_binding_revision};
      Require(required_keys.insert(key).second,
              "active native projection binding is repeated");
      const auto found = projections_.find(key);
      Require(found != projections_.end() && found->second != nullptr,
              "active frame-resolver set lacks a required projection");
      const core::ProjectionNativeRuntimeBindingDTO expected_binding{
          provider.capability_provider_binding_id,
          provider.capability_provider_binding_revision,
          core::RuntimeUnitId(config_.runtime_unit_id),
          core::RuntimeEpoch(config_.runtime_epoch)};
      const auto health = found->second->ProjectionHealth();
      Require(health.native_runtime_binding == expected_binding &&
                  health.healthy_for_target(expected),
              "active frame-resolver set contains a stale projection");
      required.push_back(found->second);
    }
    Require(!required.empty(),
            "active retained product has no native projection set");

    for (const auto &projection : required) {
      auto installed =
          projection->InstallActiveFrameResolver(expected, frame_resolver);
      if (!installed.ok()) {
        Fail("direct native frame resolver set installation failed: " +
             installed.error().message);
      }
    }
  }

private:
  struct PreparedIdentity final {
    core::RetainedBundleId retained_bundle_id;
    core::GraphRevision graph_revision;
    core::RetainedProductDTO planning_collision_model;
  };

  [[nodiscard]] static ManagedProjectionKey
  Key(const core::ProjectionNativeRuntimeBindingDTO &binding) {
    return {binding.provider_binding_id, binding.provider_binding_revision};
  }

  void RequireLocalBinding(
      const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    Require(binding.valid() &&
                binding.runtime_unit_id.value() == config_.runtime_unit_id &&
                binding.runtime_epoch.value() == config_.runtime_epoch,
            "native projection request names another runtime child");
  }

  [[nodiscard]] RetainedPlannerSceneProjection &
  Projection(const core::ProjectionNativeRuntimeBindingDTO &binding) {
    RequireLocalBinding(binding);
    const auto found = projections_.find(Key(binding));
    Require(found != projections_.end() && found->second != nullptr &&
                found->second->ProjectionHealth().native_runtime_binding ==
                    binding,
            "native projection request names an unprepared binding");
    return *found->second;
  }

  [[nodiscard]] const core::CapabilityProviderBindingDTO &
  RequireProviderBinding(
      const deployment::RetainedBundle &bundle,
      const core::ProjectionNativeRuntimeBindingDTO &runtime_binding) const {
    const auto &bindings = bundle.value().capability_provider_bindings;
    const auto found = std::find_if(
        bindings.begin(), bindings.end(), [&](const auto &candidate) {
          return candidate.capability_provider_binding_id ==
                     runtime_binding.provider_binding_id &&
                 candidate.capability_provider_binding_revision ==
                     runtime_binding.provider_binding_revision;
        });
    Require(
        found != bindings.end() &&
            core::RequiresNativeSceneProjection(
                found->capability_id, found->capability_definition_revision),
        "prepared provider is absent or not a native projection "
        "participant");
    std::set<core::RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : found->endpoint_bindings) {
      if (endpoint.source_kind !=
          core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *runtime_unit =
          std::get_if<core::RuntimeUnitId>(&endpoint.source_id);
      Require(runtime_unit != nullptr && !runtime_unit->empty(),
              "prepared provider has a mismatched runtime endpoint");
      runtime_sources.insert(*runtime_unit);
    }
    Require(runtime_sources.size() == 1U &&
                runtime_sources.begin()->value() == config_.runtime_unit_id,
            "prepared provider is assigned to another runtime unit");
    return *found;
  }

  [[nodiscard]] static std::string
  SelectProjectionGroup(const core::CapabilityProviderBindingDTO &provider,
                        const deployment::VerifiedRetainedModelMap &model_map) {
    std::vector<std::string> groups;
    if (const auto *target =
            std::get_if<core::TargetBindingCapabilityProviderTargetDTO>(
                &provider.target)) {
      const auto *resolved = model_map.find_target(
          target->controllable_component_id, target->command_target_id);
      Require(resolved != nullptr,
              "native provider target is absent from its retained model map");
      for (const auto &group : resolved->model_groups) {
        groups.push_back(group.compiled_group_name);
      }
    } else {
      for (const auto &target : model_map.targets()) {
        for (const auto &group : target.model_groups) {
          groups.push_back(group.compiled_group_name);
        }
      }
    }
    groups.erase(std::remove(groups.begin(), groups.end(), std::string{}),
                 groups.end());
    std::sort(groups.begin(), groups.end());
    groups.erase(std::unique(groups.begin(), groups.end()), groups.end());
    Require(!groups.empty(),
            "retained native projection has no compiled planning group");
    return groups.front();
  }

  struct ProjectionAssets final {
    RetainedCollisionWorldProjector projector;
  };

  [[nodiscard]] static std::int64_t MonotonicNowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }

  [[nodiscard]] ProjectionAssets BuildProjectionAssets(
      const component_manager::detail::NativeProjectionPrepareRequest
          &request) {
    auto bundle =
        bundle_source_.get_verified(request.candidate_retained_bundle_id);
    Require(bundle != nullptr &&
                bundle->value().cell_graph.graph_revision ==
                    request.candidate_graph_revision &&
                bundle->value().compiled_products.planning_collision_model ==
                    request.planning_collision_model,
            "prepared native projection differs from its verified retained "
            "bundle");
    const auto &provider =
        RequireProviderBinding(*bundle, request.native_runtime_binding);
    const auto &products = bundle->value().compiled_products;
    Require(products.symbol_role_maps.size() == 1U,
            "native projection requires one exact retained model map");
    auto model_map = map_source_.get_verified(deployment::RetainedModelMapQuery{
        bundle->retained_bundle_id(),
        products.symbol_role_maps.front().product_id});
    Require(model_map.has_value(),
            "native projection retained model map is absent or invalid");
    const auto group = SelectProjectionGroup(provider, *model_map);
    const auto cache_key =
        std::make_tuple(request.candidate_retained_bundle_id.value(),
                        request.candidate_graph_revision.value(), group);
    if (const auto cached = projection_asset_cache_.find(cache_key);
        cached != projection_asset_cache_.end()) {
      return cached->second;
    }

    auto planning_model =
        planning_source_.get_verified(deployment::RetainedPlanningModelQuery{
            bundle->retained_bundle_id(),
            request.planning_collision_model.product_id});
    Require(planning_model.has_value(),
            "native projection retained planning model is absent or invalid");
    auto stage = RetainedPlanningModelStage::create_verified(
        *planning_model, config_.execution_run_root);
    auto package_paths = stage.package_paths();
    package_paths.insert(package_paths.begin(), config_.package_root);
    auto shared_model = collision::ManyForgeCollisionEngine::load_shared_model(
        "manyforge_managed_native_projection", stage.loader_urdf_path(),
        stage.srdf_path(), package_paths, group);
    auto projector = RetainedCollisionWorldProjector::create_verified(
        shared_model, *bundle, *model_map, *planning_model);
    ProjectionAssets assets{std::move(projector)};
    const auto [inserted, unique] =
        projection_asset_cache_.emplace(cache_key, std::move(assets));
    Require(unique, "native projection model cache insertion conflicted");
    return inserted->second;
  }

  void Prepare(const component_manager::detail::NativeProjectionPrepareRequest
                   &request) {
    Require(request.valid(), "native projection preparation is invalid");
    RequireLocalBinding(request.native_runtime_binding);
    const auto key = Key(request.native_runtime_binding);
    if (const auto prior = prepared_.find(key);
        prior != prepared_.end() &&
        prior->second.retained_bundle_id ==
            request.candidate_retained_bundle_id &&
        prior->second.graph_revision == request.candidate_graph_revision &&
        prior->second.planning_collision_model ==
            request.planning_collision_model) {
      return;
    }

    auto assets = BuildProjectionAssets(request);
    const auto found = projections_.find(key);
    if (found == projections_.end()) {
      auto created = RetainedPlannerSceneProjection::CreateUninitialized(
          std::move(assets.projector), request.planning_collision_model,
          request.native_runtime_binding,
          RetainedPlannerSceneProjectionConfig{
              "managed-native-projection-" +
                  request.native_runtime_binding.provider_binding_id.value(),
              256U, RetainedCollisionWorldPublisher::kDefaultLedgerCapacity},
          MonotonicNowNs);
      if (!created.ok()) {
        Fail("native projection construction failed: " +
             created.error().message);
      }
      const auto [inserted, unique] =
          projections_.emplace(key, std::move(created.value()));
      Require(unique && inserted->second != nullptr,
              "native projection registry insertion conflicted");
    } else {
      Require(found->second != nullptr,
              "native projection registry contains an empty participant");
      auto prepared = found->second->PrepareCandidateProjection(
          std::move(assets.projector), request.planning_collision_model);
      if (!prepared.ok()) {
        Fail("native projection candidate preparation failed: " +
             prepared.error().message);
      }
    }
    prepared_.insert_or_assign(
        key, PreparedIdentity{request.candidate_retained_bundle_id,
                              request.candidate_graph_revision,
                              request.planning_collision_model});
  }

  const ManagedRuntimeChildConfig &config_;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source_;
  deployment::FilesystemVerifiedRetainedModelMapSource map_source_;
  deployment::FilesystemVerifiedRetainedPlanningModelSource planning_source_;
  std::map<std::tuple<std::string, std::string, std::string>, ProjectionAssets>
      projection_asset_cache_;
  std::map<ManagedProjectionKey,
           std::shared_ptr<RetainedPlannerSceneProjection>>
      projections_;
  std::map<ManagedProjectionKey, PreparedIdentity> prepared_;
  mutable std::mutex mutex_;
};

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

[[nodiscard]] std::optional<core::ReferenceDTO>
DecodeOptionalReference(const CanonicalValue &root,
                        const std::string_view name) {
  const auto &value = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(value.storage())) {
    return std::nullopt;
  }
  RequireExactObjectSize(value, 2U, "motion reference");
  const auto type = Text(value, "type");
  if (type == "frame_role") {
    return core::ReferenceDTO(core::ReferenceDTO::Type::kFrameRole,
                              Text(value, "frame_role_id"));
  }
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
    RequireExactObjectSize(value, 10U, "Cartesian path");
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
    return core::CartesianPathDTO{
        std::move(waypoints),
        Number(value, "max_translation_step_m"),
        Number(value, "max_rotation_step_rad"),
        core::CartesianSpeedModeDTO::kBounded,
        Number(value, "max_linear_speed_mps"),
        Number(value, "max_angular_speed_rad_s"),
        Number(value, "max_linear_acceleration_mps2"),
        Number(value, "max_angular_acceleration_rad_s2"),
        fallback};
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
  Require(
      value.is_object() &&
          ((value.as_object().size() == 16U && !has_runtime_object_bindings) ||
           (value.as_object().size() == 17U && has_runtime_object_bindings)),
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
  auto target_reference = DecodeOptionalReference(value, "target_reference");
  auto offset_reference = DecodeOptionalReference(value, "offset_reference");
  Require(Array(value, "motion_interrupt_scopes").empty(),
          "native motion cannot carry I8 interrupt scopes");

  const auto &motion = Member(value, "motion");
  auto decoded_motion = DecodeMotionSpec(motion);
  const bool joint_goal =
      std::holds_alternative<core::JointGoalDTO>(decoded_motion);
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
      std::move(runtime_objects)};
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
  RequireExactObjectSize(value, 10U, "operation intent");
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
  return core::OperationIntentDTO{
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
      attribution};
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
      DecodeOptionalReference(value, "pick_offset_reference"),
      DecodeOptionalReference(value, "approach_offset_reference"),
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
  RequireExactObjectSize(value, 3U, "runtime-scale control selector");
  return core::RuntimeScaleControlSelectorDTO{
      core::ProgramBindingId(Text(value, "program_binding_id")),
      core::ComponentRoleId(Text(value, "component_role_id")),
      core::TargetRoleId(Text(value, "target_role_id"))};
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
  kQueryOperation,
  kControlState,
  kObservation,
  kEvents,
  kPreviewMotion,
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
  kReleaseProgramSelection,
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
  kStartPick,
  kStartPlace,
  kReadPickPlaceSequence,
};

enum class CommandLane {
  kOrdinary,
  kStop,
  kObservation,
};

struct MalformedStopCommand final {
  core::RequestId request_id;
  core::RequestAttributionDTO attribution;
  std::string recognized_route;
  std::string reason;
};

struct TargetObservationQuery final {
  core::RequestOwnerEpoch owner_epoch;
  core::ProgramBindingId program_binding_id;
  core::TargetRoleId target_role_id;
  std::optional<core::FrameRoleId> frame_role_id;
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
  std::optional<ProgramOperationQuery> operation_query;
  std::optional<core::OperationIntentDTO> operation;
  std::optional<core::ProgramRunExecutionCaptureDTO> program_capture;
  std::optional<core::ProgramExecutionSelectionRequestDTO> program_selection;
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
  std::optional<std::string> pick_place_sequence_id;
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
      command == "release_program_selection") {
    const bool release = command == "release_program_selection";
    RequireExactObjectSize(root, release ? 5U : 4U,
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
                "manyforge.program_execution_selection.v1",
            "Program selection request schema is unsupported");
    Command decoded{release ? CommandKind::kReleaseProgramSelection
                            : CommandKind::kStopProgramSelection};
    decoded.program_selection = core::ProgramExecutionSelectionRequestDTO{
        DecodeRequestContext(Member(request, "request_context")),
        core::RequestOwnerEpoch(Text(request, "program_runner_owner_epoch")),
        core::CellAdmissionClaimGeneration(
            Counter(request, "claim_generation", 1U)),
        core::ProgramRunId(Text(request, "program_run_id")),
        Text(request, "group_id"),
        attribution};
    if (release)
      decoded.expected_selection_revision =
          Counter(root, "expected_selection_revision", 0U);
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
  if (command == "query_target") {
    RequireExactObjectSize(root, 3U, "target observation command");
    const auto &query = Member(root, "query");
    const auto kind = Text(query, "kind");
    Require(kind == "frame_pose" || kind == "gripper_status",
            "target observation kind is unsupported");
    RequireExactObjectSize(query, kind == "frame_pose" ? 6U : 5U,
                           "target observation query");
    Require(Text(query, "schema_version") ==
                "manyforge.target_observation_query.v1",
            "target observation query schema is unsupported");
    Command decoded{CommandKind::kQueryTarget};
    decoded.target_query = TargetObservationQuery{
        core::RequestOwnerEpoch(Text(query, "owner_epoch")),
        core::ProgramBindingId(Text(query, "program_binding_id")),
        core::TargetRoleId(Text(query, "target_role_id")),
        kind == "frame_pose"
            ? std::optional(core::FrameRoleId(Text(query, "frame_role_id")))
            : std::nullopt};
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
  if (command == "preview_motion") {
    RequireExactObjectSize(root, 4U, "motion preview command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    const auto &operation = Member(root, "operation");
    Command decoded{CommandKind::kPreviewMotion};
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
  if (command == "start_pick") {
    RequireExactObjectSize(root, 4U, "pick sequence start command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kStartPick};
    decoded.pick_planning =
        DecodePickPlanningIntent(Member(root, "intent"), attribution);
    return decoded;
  }
  if (command == "start_place") {
    RequireExactObjectSize(root, 4U, "place sequence start command");
    const auto attribution =
        DecodeRequestAttribution(Member(root, "attribution"));
    Command decoded{CommandKind::kStartPlace};
    decoded.place_planning =
        DecodePlacePlanningIntent(Member(root, "intent"), attribution);
    return decoded;
  }
  if (command == "read_pick_place_sequence") {
    RequireExactObjectSize(root, 3U, "pick/place sequence read command");
    Command decoded{CommandKind::kReadPickPlaceSequence};
    decoded.pick_place_sequence_id = Text(root, "sequence_id");
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
    const core::RuntimeParticipantBinding &direct_control_binding,
    const core::RequestOwnerEpoch &owner_epoch) {
  const auto &binding = direct_control_binding.value();
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
              {"max_angular_speed_rad_s",
               CanonicalValue(value.max_angular_speed_rad_s)},
              {"max_linear_acceleration_mps2",
               CanonicalValue(value.max_linear_acceleration_mps2)},
              {"max_linear_speed_mps",
               CanonicalValue(value.max_linear_speed_mps)},
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
  if (value.sequence_request.has_value()) {
    const auto &request = *value.sequence_request;
    plan = CanonicalValue(CanonicalValue::Object{
        {"approach_motion", MotionSpecValue(request.plan.approach_motion)},
        {"descend_motion", MotionSpecValue(request.plan.descend_motion)},
        {"predecessor_projection_revision",
         ProjectionRevisionValue(request.plan.predecessor_projection_revision)},
        {"preview_successor_projection_revision",
         ProjectionRevisionValue(
             request.plan.preview_successor_projection_revision)},
        {"retreat_motion", MotionSpecValue(request.plan.retreat_motion)},
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

[[nodiscard]] CanonicalValue
PickPlaceSequenceStatusValue(const core::PickPlaceSequenceStatusDTO &value) {
  CanonicalValue::Array completed_operations;
  completed_operations.reserve(value.completed_operations.size());
  for (const auto &operation_id : value.completed_operations) {
    completed_operations.emplace_back(operation_id.value());
  }
  CanonicalValue active_operation_id(nullptr);
  if (value.active_operation_id.has_value()) {
    active_operation_id = CanonicalValue(value.active_operation_id->value());
  }
  CanonicalValue committed_projection_revision(nullptr);
  if (value.committed_projection_revision.has_value()) {
    committed_projection_revision =
        ProjectionRevisionValue(*value.committed_projection_revision);
  }
  CanonicalValue terminal_error(nullptr);
  if (value.terminal_error.has_value()) {
    terminal_error = ErrorValue(*value.terminal_error);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active_operation_id", std::move(active_operation_id)},
      {"committed_projection_revision",
       std::move(committed_projection_revision)},
      {"completed_operation_ids",
       CanonicalValue(std::move(completed_operations))},
      {"kind", CanonicalValue(std::string(core::to_string(value.kind)))},
      {"phase", CanonicalValue(std::string(core::to_string(value.phase)))},
      {"schema_version",
       CanonicalValue("manyforge.pick_place_sequence_status.v1")},
      {"selected_object_id", CanonicalValue(value.selected_object_id.value())},
      {"sequence_id", CanonicalValue(value.sequence_id)},
      {"terminal_error", std::move(terminal_error)},
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
  CanonicalValue::Array held, stopping, faulted, pending;
  for (const auto &node : value.held_nodes) {
    auto *indices =
        node.state == core::ProgramNodeHoldStateDTO::kHeld       ? &held
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
       CanonicalValue("manyforge.program_execution_scope.v1")},
      {"request_owner_epoch",
       CanonicalValue(value.request_owner_epoch.value())},
      {"program_runner_owner_epoch",
       CanonicalValue(value.program_runner_owner_epoch.value())},
      {"claim_generation",
       CanonicalValue(static_cast<double>(value.claim_generation.value()))},
      {"program_run_id", CanonicalValue(value.program_run_id.value())},
      {"selection_revision",
       CanonicalValue(static_cast<double>(value.selection_revision))},
      {"held_node_indices", CanonicalValue(std::move(held))},
      {"stopping_node_indices", CanonicalValue(std::move(stopping))},
      {"faulted_node_indices", CanonicalValue(std::move(faulted))},
      {"pending_effects", CanonicalValue(std::move(pending))}});
}

[[nodiscard]] CanonicalValue ProgramSelectionAcceptedValue(
    const core::ProgramExecutionSelectionAcceptedDTO &value,
    const std::string &group_id) {
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
      {"group_id", CanonicalValue(group_id)},
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
  return CanonicalValue(CanonicalValue::Object{
      {"component_role_id", CanonicalValue(value.component_role_id.value())},
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"target_role_id", CanonicalValue(value.target_role_id.value())},
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
  return CanonicalValue(CanonicalValue::Object{
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
  });
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

[[nodiscard]] CanonicalValue
CoordinateVelocityValue(const core::CoordinateVelocityDTO &velocity) {
  if (const auto *angular = std::get_if<core::AngularVelocityDTO>(&velocity)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("angular_rad_s")},
        {"value_rad_s", CanonicalValue(angular->value_rad_s)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("linear_m_s")},
      {"value_m_s",
       CanonicalValue(std::get<core::LinearVelocityDTO>(velocity).value_m_s)},
  });
}

[[nodiscard]] CanonicalValue
ObservedTargetValue(const core::TargetRoleId &target_role_id,
                    const core::ObservedStateSnapshotDTO &state) {
  CanonicalValue::Array resources;
  resources.reserve(state.resources.size());
  for (const auto &resource : state.resources) {
    resources.emplace_back(resource.value());
  }
  CanonicalValue::Array joint_names;
  joint_names.reserve(state.joint_names.size());
  for (const auto &joint_name : state.joint_names) {
    joint_names.emplace_back(joint_name);
  }
  CanonicalValue::Array positions;
  positions.reserve(state.positions.size());
  for (const auto &position : state.positions) {
    positions.push_back(CoordinatePositionValue(position));
  }
  CanonicalValue velocities(nullptr);
  if (state.velocities.has_value()) {
    CanonicalValue::Array values;
    values.reserve(state.velocities->size());
    for (const auto &velocity : *state.velocities) {
      values.push_back(CoordinateVelocityValue(velocity));
    }
    velocities = CanonicalValue(std::move(values));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active_control_configuration_generation",
       CanonicalValue(static_cast<double>(
           state.active_control_configuration_generation.value()))},
      {"active_control_configuration_id",
       CanonicalValue(state.active_control_configuration_id.value())},
      {"command_target_id", CanonicalValue(state.command_target_id.value())},
      {"complete", CanonicalValue(state.complete)},
      {"controllable_component_id",
       CanonicalValue(state.controllable_component_id.value())},
      {"control_binding_id", CanonicalValue(state.control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(state.control_binding_revision.value())},
      {"control_domain_id", CanonicalValue(state.control_domain_id.value())},
      {"control_domain_revision",
       CanonicalValue(state.control_domain_revision.value())},
      {"control_route_id", CanonicalValue(state.control_route_id.value())},
      {"joint_names", CanonicalValue(std::move(joint_names))},
      {"positions", CanonicalValue(std::move(positions))},
      {"received_timestamp_ns",
       manyforge::content_identity::nonnegative_int64_value(
           state.received_timestamp_ns)},
      {"resources", CanonicalValue(std::move(resources))},
      {"sequence", CanonicalValue(static_cast<double>(state.sequence))},
      {"source_epoch", CanonicalValue(state.source_epoch.value())},
      {"source_id", CanonicalValue(state.source_id)},
      {"source_kind",
       CanonicalValue(std::string(core::to_string(state.source_kind)))},
      {"source_timestamp_ns",
       manyforge::content_identity::nonnegative_int64_value(
           state.source_timestamp_ns)},
      {"state_authority_class", CanonicalValue(std::string(core::to_string(
                                    state.state_authority_class)))},
      {"state_authority_id", CanonicalValue(state.state_authority_id.value())},
      {"target_role_id", CanonicalValue(target_role_id.value())},
      {"velocities", std::move(velocities)},
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
                       CanonicalValue command) {
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
  [[nodiscard]] ManagerSceneRead Read() const {
    const auto response = ExchangeManagerCommand(
        manager_run_root_,
        CanonicalValue(CanonicalValue::Object{
            {"api_version",
             CanonicalValue(std::string(kManagerCommandApiVersion))},
            {"command", CanonicalValue("scene_owner_read")},
        }));
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

[[nodiscard]] ManualControlDescriptors BuildManualControlDescriptors(
    const core::RuntimeParticipantBinding &motion_binding,
    const core::RuntimeParticipantBinding &gripper_binding,
    const core::JointLimitsDTO &motion_limits,
    const std::optional<RetainedGripperPositionLimits> &gripper_limits,
    const std::optional<core::RuntimeParticipantPolicySelection>
        &motion_runtime_policy,
    const std::optional<double> &deployment_maximum_tcp_linear_speed_mps,
    const std::vector<deployment::native_execution_runtime_contract::
                          Configuration::DirectControlJointLimit>
        &direct_control_joint_limits,
    const std::vector<deployment::native_execution_runtime_contract::
                          Configuration::GripperTargetConfiguration>
        &gripper_target_configurations,
    const deployment::VerifiedRetainedModelMap &model_map,
    const deployment::ParsedModelDocuments &documents) {
  const auto &motion = motion_binding.value();
  const auto &gripper = gripper_binding.value();
  Require(
      motion.resources.size() == motion_limits.joint_names.size() &&
          motion.resources.size() == motion_limits.position_lower_rad.size() &&
          motion.resources.size() == motion_limits.position_upper_rad.size() &&
          motion.resources.size() == motion_limits.velocity_max_rad_s.size() &&
          (motion_limits.acceleration_max_rad_s2.empty() ||
           motion.resources.size() ==
               motion_limits.acceleration_max_rad_s2.size()) &&
          gripper.resources.size() == 1U,
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
  if (motion_runtime_policy.has_value()) {
    const auto &selected = motion_runtime_policy->value();
    const bool target_scoped =
        std::holds_alternative<core::RuntimePolicyCommandTargetScopeDTO>(
            selected.selected_motion_speed_limit.scope);
    operational_limit = CanonicalValue(CanonicalValue::Object{
        {"catalog_policy_maximum_tcp_linear_speed_mps",
         CanonicalValue(selected.selected_motion_speed_limit
                            .maximum_tcp_linear_speed_mps)},
        {"deployment_maximum_tcp_linear_speed_mps",
         deployment_maximum_tcp_linear_speed_mps.has_value()
             ? CanonicalValue(*deployment_maximum_tcp_linear_speed_mps)
             : CanonicalValue(nullptr)},
        {"effective_maximum_tcp_linear_speed_mps",
         CanonicalValue(selected.effective_maximum_tcp_linear_speed_mps)},
        {"policy_id",
         CanonicalValue(selected.policy.runtime_policy_id.value())},
        {"policy_revision",
         CanonicalValue(selected.policy.runtime_policy_revision.value())},
        {"selected_scope",
         CanonicalValue(target_scoped ? "command_target" : "cell")},
        {"source_content_digest",
         CanonicalValue(selected.source_artifact.content_digest.value())},
    });
  }
  CanonicalValue::Array operational_joint_limits;
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
    operational_joint_limits.emplace_back(CanonicalValue::Object{
        {"deployment_max_acceleration_rad_s2",
         CanonicalNullableNumber(authored_acceleration)},
        {"deployment_max_jerk_rad_s3", CanonicalNullableNumber(authored_jerk)},
        {"deployment_max_velocity_rad_s",
         CanonicalNullableNumber(authored_velocity)},
        {"effective_max_acceleration_rad_s2",
         CanonicalNullableNumber(effective_acceleration)},
        {"effective_max_jerk_rad_s3", CanonicalNullableNumber(authored_jerk)},
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
      {"schema_version",
       CanonicalValue("manyforge.manual_control_motion_descriptor.v3")},
      {"target_role_id", CanonicalValue(motion.target_role_id.value())},
  });

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
    Require(row != coordinate_map->resources.end() && !row->coordinates.empty(),
            "manual-control gripper descriptor has no exact resource row");
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
  CanonicalValue gripper_descriptor(CanonicalValue::Object{
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
  return {std::move(motion_descriptor), std::move(gripper_descriptor)};
}

struct Composition final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> program_binding;
  std::shared_ptr<const deployment::ProgramBinding> direct_control_binding;
  std::shared_ptr<const deployment::ProgramBinding>
      direct_control_gripper_program_binding;
  core::RuntimeParticipantBinding direct_control_motion_binding;
  core::RuntimeParticipantBinding direct_control_gripper_binding;
  core::JointLimitsDTO direct_control_joint_limits;
  CanonicalValue direct_control_motion_descriptor;
  CanonicalValue direct_control_gripper_descriptor;
  RetainedPlanningModelStage stage;
  std::shared_ptr<RetainedNativeRuntime> runtime;
  std::shared_ptr<const ProductBackedRuntimeParticipantResolver>
      target_resolver;
  core::ProjectionRevisionDTO projection_revision;
  core::ActiveBundlePublicationGeneration publication_generation;
  core::TargetRoleId status_motion_target_role_id;
  core::TargetRoleId status_gripper_target_role_id;
  std::shared_ptr<ManagerSceneAuthorityProxy> scene_authority_proxy;
  std::shared_ptr<RetainedPlannerSceneProjection> planner_scene_projection;
  std::shared_ptr<const RetainedSceneFrameResolver> scene_frame_resolver;
#if defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
  std::shared_ptr<void> pick_place_planning_fixture_lifetime;
#endif
  std::unique_ptr<core::AddressedPickPlacePlanningOwner>
      pick_place_planning_owner;
  std::unique_ptr<core::PickPlaceSequenceCoordinator>
      pick_place_sequence_coordinator;
  std::unique_ptr<core::AddressedPickPlaceExecutionOwner>
      pick_place_execution_owner;
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
  std::unique_ptr<deployment::CCLCRuntimeComposition> cclc_runtime;
  bool direct_route_configuration_published{};
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  std::unique_ptr<RetainedRos2ControlAdmissionOwner>
      ros2_control_admission_owner;
#endif

  Composition(
      std::shared_ptr<const deployment::RetainedBundle> selected_bundle,
      std::shared_ptr<const deployment::ProgramBindingContract>
          selected_program,
      std::shared_ptr<const deployment::ProgramBinding> selected_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_binding,
      std::shared_ptr<const deployment::ProgramBinding>
          selected_direct_control_gripper_program_binding,
      core::RuntimeParticipantBinding selected_direct_control_motion_binding,
      core::RuntimeParticipantBinding selected_direct_control_gripper_binding,
      core::JointLimitsDTO selected_direct_control_joint_limits,
      CanonicalValue selected_direct_control_motion_descriptor,
      CanonicalValue selected_direct_control_gripper_descriptor,
      RetainedPlanningModelStage selected_stage,
      std::shared_ptr<RetainedNativeRuntime> selected_runtime,
      core::ProjectionRevisionDTO selected_projection,
      core::ActiveBundlePublicationGeneration selected_generation,
      core::TargetRoleId selected_status_motion_target_role_id,
      core::TargetRoleId selected_status_gripper_target_role_id)
      : bundle(std::move(selected_bundle)),
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

[[maybe_unused]] void AttachProductPickPlaceOwners(Composition &composition) {
  if (composition.planner_scene_projection == nullptr) {
    return;
  }
  Require(composition.scene_authority_proxy != nullptr &&
              composition.runtime != nullptr &&
              composition.runtime->addressed_kernel != nullptr,
          "production pick/place join has no scene or addressed owner");
  if (composition.pick_place_planning_owner != nullptr) {
    Require(composition.pick_place_sequence_coordinator != nullptr &&
                composition.pick_place_execution_owner != nullptr,
            "production pick/place owner set is incomplete");
    return;
  }
  composition.pick_place_planning_owner =
      std::make_unique<core::AddressedPickPlacePlanningOwner>(
          *composition.runtime->addressed_kernel,
          *composition.scene_authority_proxy,
          *composition.scene_authority_proxy,
          *composition.planner_scene_projection,
          composition.runtime->planning_scene_synchronizer.get());
  composition.pick_place_sequence_coordinator =
      std::make_unique<core::PickPlaceSequenceCoordinator>(
          *composition.runtime->addressed_kernel,
          *composition.scene_authority_proxy);
  composition.pick_place_execution_owner =
      std::make_unique<core::AddressedPickPlaceExecutionOwner>(
          *composition.pick_place_planning_owner,
          *composition.pick_place_sequence_coordinator);
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
  core::ResourceAddressedExecutionKernel *addressed_kernel{};

  void replace_current(Composition *replacement) {
    // Caller holds mutex exclusively; current remains alive through this
    // publication. Wake the old channel once, never poll for replacement.
    if (current != nullptr && current != replacement) {
      current->runtime->motion_state_channel->notify_composition_replaced();
    }
    current = replacement;
  }
};

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
  core::TargetRoleId status_motion_target_role_id;
  core::TargetRoleId status_gripper_target_role_id;
  bool attach_addressed_owner{true};
  std::shared_ptr<WallClock> shared_clock;
  std::shared_ptr<SimpleScheduler> shared_scheduler;
  std::filesystem::path manager_state_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path logical_scene_database;
  std::filesystem::path program_cycle_journal;
  std::filesystem::path authoring_transaction_journal;
  std::shared_ptr<RetainedPlannerSceneProjection> planner_scene_projection;
};

[[nodiscard]] core::CellAdmissionSettlementValidator
BuildOwnerSettlementValidator(
    std::filesystem::path manager_apply_journal,
    std::filesystem::path logical_scene_database,
    std::filesystem::path program_cycle_journal,
    std::filesystem::path authoring_transaction_journal,
    std::filesystem::path lifecycle_journal, core::CellId cell_id) {
  if (manager_apply_journal.empty() && logical_scene_database.empty() &&
      program_cycle_journal.empty() && authoring_transaction_journal.empty() &&
      lifecycle_journal.empty()) {
    return {};
  }
  return
      [manager_apply_journal = std::move(manager_apply_journal),
       logical_scene_database = std::move(logical_scene_database),
       program_cycle_journal = std::move(program_cycle_journal),
       authoring_transaction_journal = std::move(authoring_transaction_journal),
       lifecycle_journal = std::move(lifecycle_journal),
       cell_id = std::move(cell_id)](
          const core::CellAdmissionClaimDTO &claim,
          const core::CellAdmissionClaimReleaseDTO &release) {
        switch (claim.intent.claim_kind) {
        case core::CellAdmissionClaimKindDTO::kProgramCycle:
          return detail::ValidateProgramCycleSettlementReadOnly(
              program_cycle_journal, claim, release);
        case core::CellAdmissionClaimKindDTO::kAuthoringMutation:
          return detail::ValidateAuthoringSettlementReadOnly(
              authoring_transaction_journal, claim, release);
        case core::CellAdmissionClaimKindDTO::kManagerApply:
          return component_manager::
              ValidateManagerApplyCellAdmissionSettlementReadOnly(
                  manager_apply_journal, cell_id, claim, release);
        case core::CellAdmissionClaimKindDTO::kComponentLifecycle:
          return core::ValidateLifecycleCellAdmissionSettlementReadOnly(
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
    std::filesystem::path program_cycle_journal,
    std::filesystem::path authoring_transaction_journal,
    std::filesystem::path lifecycle_journal, core::CellId cell_id) {
  return [program_cycle_journal = std::move(program_cycle_journal),
          authoring_transaction_journal =
              std::move(authoring_transaction_journal),
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
      return detail::ValidateAuthoringOwnerReconciliationReadOnly(
          authoring_transaction_journal, held_claims, current_owner_epochs,
          reconcile);
    case core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner:
      return core::ValidateLifecycleCellAdmissionReconciliationReadOnly(
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

[[nodiscard]] std::unique_ptr<Composition>
BuildProductComposition(CompositionBuildInput input) {
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
  const auto qualified_runtime_policy =
      deployment::resolve_retained_runtime_policy(*input.bundle, catalog_store);
  std::optional<core::RuntimePolicyDTO> runtime_policy;
  std::optional<core::RuntimePolicyCatalogRecordRefDTO>
      runtime_policy_source_artifact;
  if (qualified_runtime_policy.has_value()) {
    runtime_policy = qualified_runtime_policy->value();
    runtime_policy_source_artifact = qualified_runtime_policy->reference();
  }
  auto direct_control_inventory =
      deployment::compile_direct_control_program_binding_inventory(
          *input.bundle, input.model_map, catalog_store);
  Require(direct_control_inventory.motion.size() == 1U &&
              direct_control_inventory.gripper.size() == 1U,
          "N7-a4 native composition requires one physical motion and one "
          "physical gripper target");
  auto direct_control_binding = direct_control_inventory.motion.front();
  auto direct_control_gripper_program_binding =
      direct_control_inventory.gripper.front();
  const auto direct_control_motion_target =
      deployment::resolve_runtime_program_target(
          *direct_control_binding,
          core::TargetRoleId(
              std::string(deployment::kDirectControlTargetRoleId)),
          core::CapabilityId("execution.trajectory.execute"), *input.bundle,
          input.model_map);
  const auto direct_control_gripper_target =
      deployment::resolve_runtime_program_target(
          *direct_control_gripper_program_binding,
          core::TargetRoleId(
              std::string(deployment::kDirectControlTargetRoleId)),
          core::CapabilityId("tool.gripper.command"), *input.bundle,
          input.model_map);
  Require(
      direct_control_motion_target.component().controllable_component_id ==
          direct_control_gripper_target.component().controllable_component_id,
      "N7-a4 motion and gripper targets do not form one physical "
      "component");

  if (input.status_program_binding != nullptr) {
    const auto selected_motion_target =
        deployment::resolve_runtime_program_target(
            *input.status_program_binding, input.status_motion_target_role_id,
            core::CapabilityId("execution.trajectory.execute"), *input.bundle,
            input.model_map);
    Require(
        selected_motion_target.component().controllable_component_id ==
                direct_control_motion_target.component()
                    .controllable_component_id &&
            selected_motion_target.target_role().command_target_id ==
                direct_control_motion_target.target_role().command_target_id,
        "development Program selection crosses the product-derived "
        "physical motion runtime");
    const bool selected_program_has_gripper = std::any_of(
        input.status_program_binding->value().target_role_bindings.begin(),
        input.status_program_binding->value().target_role_bindings.end(),
        [](const auto &target) {
          return std::any_of(target.capability_bindings.begin(),
                             target.capability_bindings.end(),
                             [](const auto &capability) {
                               return capability.capability_id.value() ==
                                      "tool.gripper.command";
                             });
        });
    if (selected_program_has_gripper) {
      const auto selected_gripper_target =
          deployment::resolve_runtime_program_target(
              *input.status_program_binding,
              input.status_gripper_target_role_id,
              core::CapabilityId("tool.gripper.command"), *input.bundle,
              input.model_map);
      Require(
          selected_gripper_target.component().controllable_component_id ==
                  direct_control_gripper_target.component()
                      .controllable_component_id &&
              selected_gripper_target.target_role().command_target_id ==
                  direct_control_gripper_target.target_role().command_target_id,
          "development Program selection crosses the product-derived "
          "physical gripper runtime");
    } else {
      input.status_gripper_target_role_id =
          direct_control_gripper_target.target_role().target_role_id;
    }
  } else {
    input.status_motion_target_role_id = core::TargetRoleId(
        direct_control_motion_target.target_role().command_target_id.value());
    input.status_gripper_target_role_id = core::TargetRoleId(
        direct_control_gripper_target.target_role().command_target_id.value());
  }

  std::vector<deployment::RuntimeProgramTargetBinding> direct_control_support;
  const auto &direct_control_target_role =
      direct_control_binding->value().target_role_bindings.front();
  for (const std::string_view capability_id :
       {std::string_view("planning.motion.cartesian_path"),
        std::string_view("planning.motion.joint_goal"),
        std::string_view("planning.motion.pose_goal")}) {
    if (std::any_of(direct_control_target_role.capability_bindings.begin(),
                    direct_control_target_role.capability_bindings.end(),
                    [&](const auto &capability) {
                      return capability.capability_id.value() == capability_id;
                    })) {
      direct_control_support.push_back(
          deployment::resolve_runtime_program_target(
              *direct_control_binding,
              core::TargetRoleId(
                  std::string(deployment::kDirectControlTargetRoleId)),
              core::CapabilityId(std::string(capability_id)), *input.bundle,
              input.model_map));
    }
  }
  Require(std::any_of(direct_control_support.begin(),
                      direct_control_support.end(),
                      [](const auto &support) {
                        return support.capability().capability_id.value() ==
                               "planning.motion.joint_goal";
                      }),
          "direct-control motion binding has no joint planner");
  auto direct_control_motion_binding = core::verify_runtime_participant_binding(
      deployment::build_runtime_participant_binding_dto(
          direct_control_motion_target,
          ActiveFacts(
              direct_control_motion_target,
              std::span<const deployment::RuntimeProgramTargetBinding>(
                  direct_control_support),
              input.runtime_epoch,
              core::ActiveControlConfigurationId("active.i6.native.arm")),
          std::span<const deployment::RuntimeProgramTargetBinding>(
              direct_control_support)));
  if (!direct_control_motion_binding.ok()) {
    Fail("direct-control motion participant binding failed verification: " +
         direct_control_motion_binding.error().message);
  }
  auto direct_control_gripper_binding =
      core::verify_runtime_participant_binding(
          deployment::build_runtime_participant_binding_dto(
              direct_control_gripper_target,
              ActiveFacts(
                  direct_control_gripper_target,
                  std::span<const deployment::RuntimeProgramTargetBinding>{},
                  input.runtime_epoch,
                  core::ActiveControlConfigurationId(
                      "active.i6.native.gripper"))));
  if (!direct_control_gripper_binding.ok()) {
    Fail("direct-control gripper participant binding failed verification: " +
         direct_control_gripper_binding.error().message);
  }
  auto control_binding_representatives = BuildControlBindingRepresentatives(
      *direct_control_binding, direct_control_motion_binding.value(),
      *input.bundle, input.model_map, input.runtime_epoch,
      core::ActiveControlConfigurationId("active.i6.native.arm"));
  auto gripper_control_binding_representatives =
      BuildControlBindingRepresentatives(
          *direct_control_gripper_program_binding,
          direct_control_gripper_binding.value(), *input.bundle,
          input.model_map, input.runtime_epoch,
          core::ActiveControlConfigurationId("active.i6.native.gripper"));
  control_binding_representatives.insert(
      control_binding_representatives.end(),
      std::make_move_iterator(gripper_control_binding_representatives.begin()),
      std::make_move_iterator(gripper_control_binding_representatives.end()));
  std::sort(control_binding_representatives.begin(),
            control_binding_representatives.end(),
            [](const auto &left, const auto &right) {
              return left.value().control_path->control_binding_id <
                     right.value().control_path->control_binding_id;
            });

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
              contains_local_runtime(direct_control_gripper_binding.value()),
          "native execution local runtime is absent from its exact "
          "participant bindings");
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  const auto retained_runtime_configuration =
      deployment::ros2_control_execution_runtime_contract::
          require_configuration(*input.bundle, *local_runtime_unit_id);
#else
  const auto retained_runtime_configuration =
      deployment::native_execution_runtime_contract::require_configuration(
          *input.bundle, *local_runtime_unit_id);
#endif
  std::optional<double> deployment_motion_target_limit;
#if !defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
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
  Require(retained_runtime_configuration.initial_joint_positions_rad.size() ==
              direct_control_motion_binding.value().value().resources.size(),
          "native execution initial state differs from the exact motion "
          "resource order");
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
  auto stage = RetainedPlanningModelStage::create_verified(
      *retained_model, input.execution_run_root);
  auto scene = std::make_shared<roboplan::Scene>(
      "manyforge_i6_native_execution_host",
      std::string(stage.loader_urdf_bytes()), std::string(stage.srdf_bytes()),
      stage.package_paths());
  auto scene_mutex = std::make_shared<std::mutex>();
  std::vector<core::RuntimeParticipantBinding> projected_state_bindings{
      direct_control_motion_binding.value()};
  if (direct_control_gripper_binding.value()
          .value()
          .resources.front()
          .source_joint_name.has_value()) {
    projected_state_bindings.push_back(direct_control_gripper_binding.value());
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
          RoboPlanWorkerModelDTO{"manyforge_i6_native_execution_worker",
                                 stage.loader_urdf_path(),
                                 stage.srdf_path(),
                                 stage.package_paths(),
                                 std::nullopt,
                                 {},
                                 {}},
          candidate_binding,
          RoboPlanCandidateComputerConfig{
              .rng_seed = retained_runtime_configuration.planner_rng_seed},
          ProcessContainedCandidateClientConfig{
              .worker_executable = input.candidate_worker,
              .require_retained_projection = true,
              .scene_asset_root = input.package_root,
              .retained_package_root_fd =
                  input.runtime_unit_id.has_value()
                      ? std::optional<int>{kManagedPackageRootFd}
                      : std::nullopt});
  Require(candidate_client->requires_retained_projection(),
          "production native planner must consume the exact retained scene "
          "projection");
  auto planner = std::make_unique<RoboPlanPlannerBackend>(
      inventory->projection_binding, candidate_binding,
      std::move(candidate_client));
  const auto make_participant_resolver =
      [package_root = input.package_root, bundle = input.bundle,
       model_map = input.model_map,
       intrinsic_direct_bindings =
           std::vector<core::ProgramBindingId>{
               direct_control_binding->program_binding_id(),
               direct_control_gripper_program_binding->program_binding_id()}](
          std::vector<core::RuntimeParticipantBinding> physical_bindings) {
        return std::make_shared<ProductBackedRuntimeParticipantResolver>(
            package_root, bundle, model_map, std::move(physical_bindings),
            intrinsic_direct_bindings);
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
  auto target_resolver =
      make_participant_resolver({direct_control_motion_binding.value(),
                                 direct_control_gripper_binding.value()});
  RetainedRuntimeParticipantResolver participant_resolver =
      [resolver = target_resolver](const core::OperationIntentDTO &operation,
                                   const std::size_t participant_index) {
        return resolver->ResolveExecutionParticipant(operation,
                                                     participant_index);
      };
  const auto reference_collision_model = collision_model;
  const auto reference_runtime_unit_id = *local_runtime_unit_id;
  const auto reference_runtime_epoch = input.runtime_epoch;
  RetainedNativeRuntimeInput runtime_input{
      direct_control_motion_binding.value(),
      direct_control_gripper_binding.value(),
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
       reference_runtime_epoch](const auto &channels) {
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
            std::move(state_sources));
      },
      std::move(participant_resolver),
      participant_resolver_factory,
      input.operation_admission_gate,
      std::move(runtime_policy),
      std::move(runtime_policy_source_artifact),
      {},
      {},
      {},
      {},
      {}};
  runtime_input.control_binding_representatives =
      std::move(control_binding_representatives);
  runtime_input.deployment_maximum_tcp_linear_speed_mps =
      deployment_motion_target_limit;
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
      retained_runtime_configuration.initial_joint_positions_rad,
      std::nullopt,
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
          input.program_cycle_journal, input.authoring_transaction_journal,
          input.execution_state_root / std::string(core::kCCLCJournalFilename),
          input.cell_id);
  runtime_config.cell_admission_reconcile_validator =
      BuildOwnerReconciliationValidator(
          input.program_cycle_journal, input.authoring_transaction_journal,
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
  auto direct_control_joint_limits = runtime->kinematics->joint_limits(
      direct_control_motion_binding.value()
          .value()
          .controllable_component_id.value(),
      direct_control_motion_binding.value().value().command_target_id.value());
  if (!direct_control_joint_limits.ok()) {
    Fail("direct-control joint limits are unavailable: " +
         direct_control_joint_limits.error().message);
  }
  const auto retained_srdf = retained_model->srdf_bytes();
  const auto manual_control_documents =
      deployment::parse_model_documents_for_projection(
          retained_model->urdf_bytes(),
          retained_srdf.empty()
              ? std::optional<std::string_view>{}
              : std::optional<std::string_view>{retained_srdf});
  auto manual_control_descriptors = BuildManualControlDescriptors(
      direct_control_motion_binding.value(),
      direct_control_gripper_binding.value(),
      direct_control_joint_limits.value(), runtime->gripper_position_limits,
      runtime->motion_runtime_policy,
      runtime->deployment_maximum_tcp_linear_speed_mps,
      retained_runtime_configuration.direct_control_joint_limits,
      retained_runtime_configuration.gripper_target_configurations,
      input.model_map, manual_control_documents);
  auto composition = std::make_unique<Composition>(
      std::move(input.bundle), std::move(input.status_program),
      std::move(input.status_program_binding),
      std::move(direct_control_binding),
      std::move(direct_control_gripper_program_binding),
      std::move(direct_control_motion_binding.value()),
      std::move(direct_control_gripper_binding.value()),
      std::move(direct_control_joint_limits.value()),
      std::move(manual_control_descriptors.motion),
      std::move(manual_control_descriptors.gripper), std::move(stage),
      std::move(runtime), std::move(input.projection_revision),
      input.publication_generation,
      std::move(input.status_motion_target_role_id),
      std::move(input.status_gripper_target_role_id));
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
  composition->pick_place_sequence_coordinator =
      std::move(fixture.sequence_coordinator);
  composition->pick_place_execution_owner = std::move(fixture.execution_owner);
#else
  if (input.attach_addressed_owner) {
    AttachProductPickPlaceOwners(*composition);
  }
#endif
  return composition;
}

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
  Require(direct_inventory.motion.size() == 1U &&
              direct_inventory.gripper.size() == 1U,
          "native Program aliases have no sole physical target pair");
  const auto physical_motion = deployment::resolve_runtime_program_target(
      *direct_inventory.motion.front(),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::CapabilityId("execution.trajectory.execute"), *bundle, *model_map);
  const auto physical_gripper = deployment::resolve_runtime_program_target(
      *direct_inventory.gripper.front(),
      core::TargetRoleId(std::string(deployment::kDirectControlTargetRoleId)),
      core::CapabilityId("tool.gripper.command"), *bundle, *model_map);
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
    const auto selected_gripper = deployment::resolve_runtime_program_target(
        *program_binding, core::TargetRoleId(config.gripper_target_role_id),
        core::CapabilityId("tool.gripper.command"), *bundle, *model_map);
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
              physical_gripper.component().controllable_component_id ==
                  selected_motion.component().controllable_component_id,
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

#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
[[nodiscard]] std::string
Ros2ControlProviderComponentEpoch(const Composition &composition) {
  std::vector<std::string> occurrences;
  for (const auto *binding : {&composition.direct_control_motion_binding,
                              &composition.direct_control_gripper_binding}) {
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
  std::vector<core::RuntimeParticipantBinding> physical_bindings{
      composition.runtime->motion_binding};
  if (composition.runtime->gripper_binding.has_value()) {
    physical_bindings.push_back(*composition.runtime->gripper_binding);
  }
  return physical_bindings;
}

[[nodiscard]] bool RetainedProductDeclaresLifecycle(
    const Composition &composition,
    const deployment::FilesystemVerifiedRetainedProviderManifestSource
        &provider_manifest_source,
    const std::vector<core::RuntimeParticipantBinding> &physical_bindings) {
  // The ordinary maintained native products predate the lifecycle-qualified
  // CCLC product and deliberately retain the generic native-control provider
  // manifest.  They remain valid direct native runtimes, but have no lifecycle
  // product to attach.  A lifecycle declaration is the sealed package-level
  // opt-in; once one physical provider declares it, the product constructor
  // below remains fail-closed for every desired domain and exact manifest.
  std::set<std::pair<core::ProviderManifestId, core::ProviderManifestRevision>>
      provider_manifest_keys;
  bool lifecycle_product_declared = false;
  for (const auto &binding : physical_bindings) {
    Require(binding.value().control_path.has_value(),
            "native CCLC eligibility has no exact control path");
    const auto &path = *binding.value().control_path;
    const auto key =
        std::pair{path.provider_manifest_id, path.provider_manifest_revision};
    if (!provider_manifest_keys.insert(key).second) {
      continue;
    }
    const auto manifest = provider_manifest_source.get_verified(
        {composition.bundle->retained_bundle_id(), path.provider_manifest_id,
         path.provider_manifest_revision});
    Require(manifest.has_value(),
            "native CCLC eligibility cannot resolve the exact provider "
            "manifest");
    lifecycle_product_declared =
        lifecycle_product_declared ||
        manifest->value().lifecycle_plan_catalog.has_value();
  }
  return lifecycle_product_declared;
}

void PublishDirectNativeRoutes(Composition &composition,
                               const std::filesystem::path &package_root) {
  if (composition.direct_route_configuration_published ||
      composition.cclc_runtime != nullptr) {
    return;
  }
  const auto physical_bindings = PhysicalControlBindings(composition);
  const deployment::FilesystemVerifiedRetainedProviderManifestSource
      provider_manifest_source(package_root);
  if (RetainedProductDeclaresLifecycle(composition, provider_manifest_source,
                                       physical_bindings)) {
    return;
  }
  auto published =
      composition.runtime->PublishCurrentRetainedRouteConfigurations();
  if (!published.ok()) {
    Fail("direct native route publication failed: " +
         published.error().message);
  }
  composition.direct_route_configuration_published = true;
}

void AttachRetainedNativeCCLC(Composition &composition,
                              const HostConfig &config,
                              ManagedKernelApplyFenceOwner &fence_owner) {
  Require(composition.runtime != nullptr &&
              composition.runtime->addressed_kernel != nullptr &&
              composition.runtime->clock != nullptr &&
              composition.runtime->scheduler != nullptr &&
              composition.cclc_runtime == nullptr,
          "native CCLC attachment requires one stable addressed runtime");

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

  auto artifact_source = std::make_unique<
      deployment::FilesystemVerifiedRetainedProductArtifactSource>(
      config.package_root);
  auto provider_manifest_source = std::make_unique<
      deployment::FilesystemVerifiedRetainedProviderManifestSource>(
      config.package_root);
  auto physical_bindings = PhysicalControlBindings(composition);
  if (!RetainedProductDeclaresLifecycle(composition, *provider_manifest_source,
                                        physical_bindings)) {
    return;
  }
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  Require(composition.ros2_control_runtime_handle != nullptr &&
              !composition.ros2_control_binding_contract_generation_id.empty(),
          "retained Isaac ROS lifecycle has no bound execution adapter");
  auto lifecycle_product = RetainedRos2ControlLifecycleProduct::Create(
      composition.bundle, *provider_manifest_source, physical_bindings,
      composition.ros2_control_runtime_handle, *composition.runtime->clock,
      1'000'000'000LL);
#else
  auto lifecycle_product = RetainedNativeLifecycleProduct::Create(
      composition.bundle, *provider_manifest_source, physical_bindings,
      *composition.runtime->clock, 1'000'000'000LL);
#endif
  if (!lifecycle_product.ok()) {
    Fail("native lifecycle product cannot be composed: " +
         lifecycle_product.error().message);
  }

  deployment::CCLCRuntimeCompositionConfig cclc_config{
      config.execution_state_root,
      config.execution_run_root,
      core::CellId(config.cell_id),
      {core::RequestOwnerEpoch(RandomToken("cclc-owner-")), 1024U, 2U, 2U,
       10'000'000LL, 30'000'000'000LL, 30'000'000'000LL}};
  auto cclc = CreateRetainedNativeCCLCRuntime(
      RetainedNativeCCLCRuntimeConfig{std::move(cclc_config), 1'000'000'000LL},
      composition.bundle, *active_publication_reader,
      std::move(active_publication.value()), *artifact_source,
      lifecycle_product.value()->catalog_source(),
      lifecycle_product.value()->brand_ports(), *composition.runtime,
      fence_owner, *composition.runtime->addressed_kernel, fence_owner
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
      ,
      composition.ros2_control_runtime_handle->lifecycle_snapshot_source()
#endif
  );
  if (!cclc.ok()) {
    Fail("native CCLC composition failed: " + cclc.error().message);
  }

  composition.cclc_active_publication_reader =
      std::move(active_publication_reader);
  composition.cclc_artifact_source = std::move(artifact_source);
  composition.cclc_provider_manifest_source =
      std::move(provider_manifest_source);
  composition.cclc_lifecycle_product = std::move(lifecycle_product.value());
  composition.cclc_runtime = std::move(cclc.value());
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  const auto component_id = composition.direct_control_motion_binding.value()
                                .controllable_component_id;
  const auto runtime_epochs =
      composition.direct_control_motion_binding.value().runtime_unit_epochs;
  Require(runtime_epochs.size() == 1U,
          "retained Isaac ROS admission has no sole runtime epoch");
  auto admission = RetainedRos2ControlAdmissionOwner::Create(
      RetainedRos2ControlAdmissionOwnerConfig{
          component_id, runtime_epochs.front().runtime_epoch,
          composition.ros2_control_binding_contract_generation_id,
          Ros2ControlProviderComponentEpoch(composition),
          Ros2ControlCellSupervisorEpoch(composition), 50'000'000LL,
          250'000'000LL},
      composition.ros2_control_runtime_handle, *composition.runtime->clock,
      [&composition]() {
        return composition.cclc_runtime->ReadCurrentComponentSnapshot(
            composition.direct_control_motion_binding.value()
                .controllable_component_id);
      });
  if (!admission.ok()) {
    Fail("ROS execution admission owner cannot be composed: " +
         admission.error().message);
  }
  composition.ros2_control_admission_owner = std::move(admission.value());
#endif
}

void DetachRetainedCCLC(Composition &composition) {
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
  composition.ros2_control_admission_owner.reset();
#endif
  composition.cclc_runtime.reset();
  composition.cclc_lifecycle_product.reset();
  composition.cclc_provider_manifest_source.reset();
  composition.cclc_artifact_source.reset();
  composition.cclc_active_publication_reader.reset();
}

void TryAttachManagedCCLC(Composition &composition,
                          const ManagedRuntimeChildConfig &config,
                          ManagedKernelApplyFenceOwner &fence_owner) {
  if (composition.cclc_runtime != nullptr ||
      composition.direct_route_configuration_published) {
    return;
  }
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  publication_store.Close();
  if (integrity.current_generation != composition.publication_generation) {
    return;
  }
  HostConfig attachment_config;
  attachment_config.package_root = config.package_root;
  attachment_config.manager_state_root = config.manager_state_root;
  attachment_config.execution_state_root = config.execution_state_root;
  attachment_config.execution_run_root = config.execution_run_root;
  attachment_config.candidate_worker = config.candidate_worker;
  attachment_config.cell_id = config.cell_id;
  PublishDirectNativeRoutes(composition, config.package_root);
  if (composition.direct_route_configuration_published) {
    return;
  }
  AttachRetainedNativeCCLC(composition, attachment_config, fence_owner);
}

[[nodiscard]] std::optional<CompositionBuildInput>
TryBuildCurrentManagedCompositionInput(
    const ManagedRuntimeChildConfig &config,
    const ManagedProjectionRegistry &projections,
    ManagedKernelAdmissionGate &admission_gate) {
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
          "managed active publication and retained bundle disagree");
  if (bundle->value().cell_graph.controllable_components.empty()) {
    return std::nullopt;
  }

  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "managed native composition requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      config.package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "managed active retained model map is absent or invalid");

  const core::CapabilityProviderBindingDTO *joint_planner = nullptr;
  for (const auto &provider : bundle->value().capability_provider_bindings) {
    if (provider.capability_id.value() != "planning.motion.joint_goal" ||
        !core::RequiresNativeSceneProjection(
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
    if (runtime_sources.size() == 1U &&
        runtime_sources.begin()->value() == config.runtime_unit_id) {
      Require(joint_planner == nullptr,
              "managed product has ambiguous joint-planner projections");
      joint_planner = &provider;
    }
  }
  Require(joint_planner != nullptr,
          "managed product has no native joint-planner projection");
  const core::ProjectionNativeRuntimeBindingDTO joint_projection_binding{
      joint_planner->capability_provider_binding_id,
      joint_planner->capability_provider_binding_revision,
      core::RuntimeUnitId(config.runtime_unit_id),
      core::RuntimeEpoch(config.runtime_epoch),
  };
  auto projection = projections.FindActiveProjection(
      receipt.request.candidate_projection_revision, joint_projection_binding);
  auto projection_revision = receipt.request.candidate_projection_revision;
  if (!projection) {
    // The active publication stores a historical projection revision when only
    // logical-scene state advanced; rebuild against the same durable binding.
    const auto current_projection = projections.FindCurrentProjectionRevision(
        receipt.request.candidate_projection_revision,
        joint_projection_binding);
    if (!current_projection.has_value()) {
      return std::nullopt;
    }
    projection_revision = *current_projection;
    projection = projections.FindActiveProjection(projection_revision,
                                                  joint_projection_binding);
  }
  if (projection == nullptr) {
    return std::nullopt;
  }
  auto collision_model = projection->collision_shared_model();
  Require(collision_model != nullptr,
          "managed active projection has no collision model");

  auto planner_scene_projection = projection;
  return CompositionBuildInput{
      config.package_root,
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
      config.authoring_transaction_journal,
      std::move(planner_scene_projection),
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
              transaction->projection_stage_generation ==
                  request.projection_stage_generation &&
              publication.value().request.candidate_projection_revision ==
                  request.candidate_projection_revision &&
              publication.value()
                      .request.expected_previous_publication_generation ==
                  std::optional<core::ActiveBundlePublicationGeneration>{
                      request.expected_predecessor_publication_generation},
          "managed composition successor belongs to another Apply");
  return true;
}

[[nodiscard]] std::shared_ptr<const deployment::RetainedBundle>
LoadExactManagedCandidateBundle(
    const ManagedRuntimeChildConfig &config,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request) {
  Require(request.valid() &&
              request.runtime_unit_id.value() == config.runtime_unit_id &&
              request.runtime_epoch.value() == config.runtime_epoch,
          "managed composition request names another runtime child");
  const deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      config.package_root);
  auto bundle = bundle_source.get_verified(
      request.candidate_projection_revision.retained_bundle_id);
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
  return bundle;
}

struct ManagedBootstrapStopProduct final {
  std::optional<core::StopDeadlineBudgetDTO> deadline_budget;
  std::vector<core::ResourceId> resource_inventory;
};

[[nodiscard]] ManagedBootstrapStopProduct LoadManagedBootstrapStopProduct(
    const ManagedRuntimeChildConfig &config,
    const core::RetainedBundleId &retained_bundle_id,
    const core::GraphRevision &graph_revision) {
  const deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      config.package_root);
  const auto bundle = bundle_source.get_verified(retained_bundle_id);
  Require(bundle != nullptr &&
              bundle->value().cell_graph.cell_id ==
                  core::CellId(config.cell_id) &&
              bundle->retained_bundle_id() == retained_bundle_id &&
              bundle->value().cell_graph.graph_revision == graph_revision,
          "managed bootstrap Stop policy differs from the retained product");

  const deployment::FilesystemVerifiedCatalogArtifactSource catalog_store(
      config.package_root);
  std::set<core::ResourceId> resources;
  for (const auto &binding : bundle->value().cell_graph.control_bindings) {
    if (binding.provider_runtime_unit_id.value() != config.runtime_unit_id)
      continue;
    for (const auto &resource : binding.resource_bindings)
      resources.insert(resource.resource_id);
  }
  Require(resources.size() <= core::kMaxKernelResourceSlots,
          "managed bootstrap resource inventory exceeds the Kernel bound");
  return {
      deployment::resolve_retained_stop_deadline_budget(*bundle, catalog_store),
      {resources.begin(), resources.end()}};
}

[[nodiscard]] std::optional<CompositionBuildInput> BuildManagedCompositionInput(
    const ManagedRuntimeChildConfig &config,
    const ManagedProjectionRegistry &projections,
    ManagedKernelAdmissionGate &admission_gate,
    const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
        &request,
    std::shared_ptr<WallClock> shared_clock,
    std::shared_ptr<SimpleScheduler> shared_scheduler) {
  auto bundle = LoadExactManagedCandidateBundle(config, request);
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

  if (bundle->value().cell_graph.controllable_components.empty()) {
    return std::nullopt;
  }

  const auto &products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "managed native composition requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      config.package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(),
      products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(),
          "managed active retained model map is absent or invalid");

  const core::CapabilityProviderBindingDTO *joint_planner = nullptr;
  for (const auto &provider : bundle->value().capability_provider_bindings) {
    if (provider.capability_id.value() != "planning.motion.joint_goal" ||
        !core::RequiresNativeSceneProjection(
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
    if (runtime_sources.size() == 1U &&
        runtime_sources.begin()->value() == config.runtime_unit_id) {
      Require(joint_planner == nullptr,
              "managed product has ambiguous joint-planner projections");
      joint_planner = &provider;
    }
  }
  Require(joint_planner != nullptr,
          "managed product has no native joint-planner projection");
  const core::ProjectionNativeRuntimeBindingDTO joint_projection_binding{
      joint_planner->capability_provider_binding_id,
      joint_planner->capability_provider_binding_revision,
      core::RuntimeUnitId(config.runtime_unit_id),
      core::RuntimeEpoch(config.runtime_epoch),
  };
  auto projection = projections.FindActiveProjection(
      request.candidate_projection_revision, joint_projection_binding);
  Require(projection != nullptr,
          "managed selected projection is not active at the candidate "
          "revision");
  auto collision_model = projection->collision_shared_model();
  Require(collision_model != nullptr,
          "managed active projection has no collision model");

  const auto predecessor_generation =
      request.expected_predecessor_publication_generation.value();
  Require(predecessor_generation != std::numeric_limits<std::uint64_t>::max(),
          "managed composition publication generation is exhausted");
  auto planner_scene_projection = projection;
  return CompositionBuildInput{
      config.package_root,
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
      config.authoring_transaction_journal,
      std::move(planner_scene_projection),
  };
}

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
      },
      admission_gate);
  ManagedProjectionRegistry projections(config);
  std::mutex managed_composition_mutex;
  std::unique_ptr<Composition> active_managed_composition;
  std::unique_ptr<Composition> retired_managed_composition;
  std::unique_ptr<Composition> prepared_managed_composition;
  bool prepared_managed_composition_is_empty = false;
  std::shared_ptr<WallClock> detached_owner_clock;
  std::shared_ptr<SimpleScheduler> detached_owner_scheduler;
  std::unique_ptr<core::SqliteOperationJournal> detached_operation_journal;
  std::unique_ptr<core::ResourceAddressedExecutionKernel>
      detached_addressed_kernel;
  // The stable Kernel owns the Stop latch across product reconfiguration.
  // Command dispatch already pins the active composition with its shared
  // lifetime lock, so this atomic selects only that same current product's
  // facts adapter; it is not a second recovery owner.
  std::atomic<RetainedNativeRuntime *> stop_recovery_runtime{nullptr};
  std::atomic<core::ResourceAddressedExecutionKernel *> stop_recovery_kernel{
      nullptr};
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
  detached_operation_journal = std::make_unique<core::SqliteOperationJournal>(
      config.execution_state_root /
          std::string(core::kOperationJournalFilename),
      core::CellId(config.cell_id));
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
  bootstrap_kernel_config.cell_stop_deadline_budget =
      std::move(bootstrap_stop.deadline_budget);
  bootstrap_kernel_config.empty_runtime_resource_inventory =
      std::move(bootstrap_stop.resource_inventory);
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
  bootstrap_kernel_config.cell_admission_claim_capacity = 1024U;
  bootstrap_kernel_config.cell_admission_reconcile_validator =
      BuildOwnerReconciliationValidator(
          config.program_cycle_journal, config.authoring_transaction_journal,
          config.execution_state_root / std::string(core::kCCLCJournalFilename),
          core::CellId(config.cell_id));
  bootstrap_kernel_config.cell_admission_settlement_validator =
      BuildOwnerSettlementValidator(
          config.manager_state_root /
              std::string(component_manager::kManagerApplyJournalFilename),
          config.logical_scene_database, config.program_cycle_journal,
          config.authoring_transaction_journal,
          config.execution_state_root / std::string(core::kCCLCJournalFilename),
          core::CellId(config.cell_id));
  detached_addressed_kernel =
      std::make_unique<core::ResourceAddressedExecutionKernel>(
          std::vector<core::RuntimeParticipantExecutionRegistration>{},
          *detached_owner_clock, *detached_owner_scheduler,
          *detached_operation_journal, std::move(bootstrap_kernel_config));
  auto *const stable_kernel_owner = detached_addressed_kernel.get();
  stop_recovery_kernel.store(stable_kernel_owner, std::memory_order_release);
  admission_gate.BindResourceTable(
      detached_addressed_kernel->resource_admission_table());
  fence_owner.BindDisableHandoffKernel(*detached_addressed_kernel);
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
  execution_composition.addressed_kernel = detached_addressed_kernel.get();
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
  std::atomic<std::shared_ptr<
      const component_manager::ManagerSelectedRuntimeCompositionRequestDTO>>
      installed_liveness_handoff;
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
  bool current_composition_building = false;
  std::jthread managed_composition_builder;
  bool managed_composition_building = false;

  const auto stable_addressed_kernel =
      [&]() -> core::ResourceAddressedExecutionKernel * {
    if (detached_addressed_kernel != nullptr) {
      return detached_addressed_kernel.get();
    }
    if (active_managed_composition != nullptr &&
        active_managed_composition->runtime->addressed_kernel != nullptr) {
      return active_managed_composition->runtime->addressed_kernel.get();
    }
    return nullptr;
  };

  const auto bind_disable_handoff_owner = [&] {
    auto *const owner = stable_addressed_kernel();
    if (owner != nullptr) {
      fence_owner.BindDisableHandoffKernel(*owner);
    }
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

  const auto try_release_retired_composition = [&] {
    std::lock_guard lock(managed_composition_mutex);
    auto *const owner = stable_addressed_kernel();
    if (retired_managed_composition == nullptr || owner == nullptr) {
      return;
    }
    auto &registrations =
        retired_managed_composition->runtime->participant_registrations;
    std::set<core::IResolvedParticipantExecutionEngine *> engines;
    for (const auto &registration : registrations)
      engines.insert(registration.engine);
    for (auto *engine : engines) {
      if (owner->participant_engine_in_use(engine))
        return;
    }
    for (auto *engine : engines) {
      if (!owner->release_retired_engine(engine).ok())
        return;
      // Reuse the retired registration set to track successful detachments.
      // A later call resumes only unfinished work, without a second registry.
      std::erase_if(registrations,
                    [engine](const auto &row) { return row.engine == engine; });
    }
    retired_managed_composition.reset();
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
    auto input = TryBuildCurrentManagedCompositionInput(config, projections,
                                                        admission_gate);
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
      current_composition_builder_started = true;
      current_composition_building = true;
    }
    current_composition_builder =
        std::jthread([&, input = std::move(*input)]() mutable {
          try {
            auto built = BuildProductComposition(std::move(input));
            InstallDirectSceneFrameResolver(projections, *built);
            Composition *installed = nullptr;
            {
              std::lock_guard lock(managed_composition_mutex);
              std::unique_lock execution_lock(execution_composition.mutex);
              Require(detached_addressed_kernel != nullptr &&
                          detached_operation_journal != nullptr &&
                          detached_owner_clock != nullptr &&
                          detached_owner_scheduler != nullptr &&
                          built->runtime->addressed_kernel == nullptr &&
                          built->runtime->clock == detached_owner_clock &&
                          built->runtime->scheduler == detached_owner_scheduler,
                      "current managed composition duplicated the stable "
                      "addressed owner");
              auto reconfigured =
                  detached_addressed_kernel->reconfigure_runtime_participants(
                      built->runtime->participant_registrations,
                      std::move(built->runtime->participant_resolver));
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected current runtime participants: " +
                     reconfigured.error().message);
              }
              built->runtime->operation_journal =
                  std::move(detached_operation_journal);
              built->runtime->addressed_kernel =
                  std::move(detached_addressed_kernel);
              detached_owner_clock.reset();
              detached_owner_scheduler.reset();
              PublishDirectNativeRoutes(*built, config.package_root);
              fence_owner.BindDisableHandoffKernel(
                  *built->runtime->addressed_kernel);
              active_managed_composition = std::move(built);
              execution_composition.replace_current(
                  active_managed_composition.get());
              execution_composition.addressed_kernel =
                  active_managed_composition->runtime->addressed_kernel.get();
              stop_recovery_runtime.store(
                  active_managed_composition->runtime.get(),
                  std::memory_order_release);
              installed = active_managed_composition.get();
            }
            // Initial CCLC recovery is part of composing a usable runtime.  It
            // runs on the existing builder lane so the ordinary Manager
            // command channel remains responsive and cannot advertise an
            // installed composition before its exact routes are current.
            TryAttachManagedCCLC(*installed, config, fence_owner);
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
    const bool publication_is_successor =
        CurrentPublicationIsInstalledSuccessor(config, request);
    const auto successor_bundle =
        publication_is_successor
            ? LoadExactManagedCandidateBundle(config, request)
            : std::shared_ptr<const deployment::RetainedBundle>{};
    bool reconcile_installed_candidate = false;
    {
      std::lock_guard lock(managed_composition_mutex);
      if (installed_composition_request.has_value()) {
        if (*installed_composition_request == request) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kInstalled};
        }
      }
      if (installed_under_fence_composition_request.has_value()) {
        Require(*installed_under_fence_composition_request == request,
                "another selected runtime composition is installed under "
                "the held fence");
        reconcile_installed_candidate = true;
      }
      if (!reconcile_installed_candidate) {
        if (current_composition_building) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing};
        }
        if (publication_is_successor) {
          const auto expected_generation =
              core::ActiveBundlePublicationGeneration(
                  request.expected_predecessor_publication_generation.value() +
                  1U);
          if (active_managed_composition == nullptr) {
            if (successor_bundle->value()
                    .cell_graph.controllable_components.empty()) {
              installed_composition_request = request;
              publish_installed_liveness_handoff(request);
              return {request,
                      component_manager::
                          ManagerSelectedRuntimeCompositionState::kInstalled};
            }
            return {request,
                    component_manager::ManagerSelectedRuntimeCompositionState::
                        kPreparing};
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
          installed_composition_request = request;
          publish_installed_liveness_handoff(request);
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kInstalled};
        }
        if (retired_managed_composition != nullptr) {
          return {request,
                  component_manager::ManagerSelectedRuntimeCompositionState::
                      kPreparing};
        }
      }
    }
    if (reconcile_installed_candidate) {
      require_installed_candidate_configuration();
      {
        std::lock_guard lock(managed_composition_mutex);
        Require(installed_under_fence_composition_request.has_value() &&
                    *installed_under_fence_composition_request == request,
                "installed runtime composition changed during fence "
                "reconciliation");
        installed_composition_request = request;
        publish_installed_liveness_handoff(request);
        installed_under_fence_composition_request.reset();
        preparing_composition_request.reset();
      }
      try_release_retired_composition();
      return {request, component_manager::
                           ManagerSelectedRuntimeCompositionState::kInstalled};
    }

    std::unique_ptr<Composition> prepared;
    bool prepared_is_empty = false;
    bool start_builder = false;
    std::shared_ptr<WallClock> shared_clock;
    std::shared_ptr<SimpleScheduler> shared_scheduler;
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
      } else if (prepared_managed_composition_is_empty) {
        prepared_is_empty = true;
        prepared_managed_composition_is_empty = false;
      } else if (!preparing_composition_request.has_value()) {
        preparing_composition_request = request;
        managed_composition_building = true;
        start_builder = true;
        if (active_managed_composition != nullptr) {
          shared_clock = active_managed_composition->runtime->clock;
          shared_scheduler = active_managed_composition->runtime->scheduler;
        } else if (detached_addressed_kernel != nullptr) {
          shared_clock = detached_owner_clock;
          shared_scheduler = detached_owner_scheduler;
        }
      }
    }

    if (start_builder) {
      managed_composition_builder = std::jthread(
          [&, request, shared_clock = std::move(shared_clock),
           shared_scheduler = std::move(shared_scheduler)]() mutable {
            try {
              auto input = BuildManagedCompositionInput(
                  config, projections, admission_gate, request,
                  std::move(shared_clock), std::move(shared_scheduler));
              auto built = input.has_value()
                               ? BuildProductComposition(std::move(*input))
                               : std::unique_ptr<Composition>{};
              std::lock_guard lock(managed_composition_mutex);
              prepared_managed_composition = std::move(built);
              prepared_managed_composition_is_empty = !input.has_value();
              managed_composition_building = false;
            } catch (...) {
              std::lock_guard lock(managed_composition_mutex);
              managed_composition_failure = std::current_exception();
              managed_composition_building = false;
            }
          });
      return {request, component_manager::
                           ManagerSelectedRuntimeCompositionState::kPreparing};
    }
    if (prepared == nullptr && !prepared_is_empty) {
      std::lock_guard lock(managed_composition_mutex);
      if (managed_composition_failure != nullptr) {
        std::rethrow_exception(managed_composition_failure);
      }
      if (prepared_managed_composition != nullptr) {
        prepared = std::move(prepared_managed_composition);
      } else if (prepared_managed_composition_is_empty) {
        prepared_is_empty = true;
        prepared_managed_composition_is_empty = false;
      } else {
        Require(managed_composition_building,
                "managed composition has no preparation outcome");
        return {request,
                component_manager::ManagerSelectedRuntimeCompositionState::
                    kPreparing};
      }
    }
    if (managed_composition_builder.joinable()) {
      managed_composition_builder.join();
    }

    fence_owner.RunWithExactHeldPredecessor(
        fence_query, request.expected_predecessor_publication_generation,
        request.expected_predecessor_public_pointer_digest, [&] {
          std::unique_lock execution_lock(execution_composition.mutex);
          Require(retired_managed_composition == nullptr,
                  "a predecessor runtime engine remains lifetime-pinned");
          if (prepared_is_empty) {
            Require(prepared == nullptr,
                    "empty selected product has a physical composition");
            if (active_managed_composition != nullptr) {
              DetachRetainedCCLC(*active_managed_composition);
              Require(detached_addressed_kernel == nullptr &&
                          detached_operation_journal == nullptr &&
                          detached_owner_clock == nullptr &&
                          detached_owner_scheduler == nullptr,
                      "stable addressed owner is duplicated");
              auto &stable_runtime = *active_managed_composition->runtime;
              auto reconfigured = stable_runtime.addressed_kernel
                                      ->reconfigure_runtime_participants({});
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
              retired_managed_composition =
                  std::move(active_managed_composition);
            } else if (detached_addressed_kernel != nullptr) {
              auto reconfigured =
                  detached_addressed_kernel->reconfigure_runtime_participants(
                      {});
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected the empty selected runtime: " +
                     reconfigured.error().message);
              }
            }
          } else {
            Require(prepared != nullptr,
                    "physical selected product has no prepared composition");
            auto *const stable_owner = stable_addressed_kernel();
            if (stable_owner == nullptr) {
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
                          config.authoring_transaction_journal,
                          config.execution_state_root /
                              std::string(core::kCCLCJournalFilename),
                          core::CellId(config.cell_id)),
                      BuildOwnerReconciliationValidator(
                          config.program_cycle_journal,
                          config.authoring_transaction_journal,
                          config.execution_state_root /
                              std::string(core::kCCLCJournalFilename),
                          core::CellId(config.cell_id)),
                      {}});
              admission_gate.BindResourceTable(
                  prepared->runtime->addressed_kernel
                      ->resource_admission_table());
            } else {
              if (active_managed_composition != nullptr) {
                DetachRetainedCCLC(*active_managed_composition);
              }
              auto reconfigured =
                  stable_owner->reconfigure_runtime_participants(
                      prepared->runtime->participant_registrations,
                      std::move(prepared->runtime->participant_resolver));
              if (!reconfigured.ok()) {
                Fail("stable Kernel rejected the selected runtime "
                     "participants: " +
                     reconfigured.error().message);
              }
              if (active_managed_composition != nullptr) {
                auto &stable_runtime = *active_managed_composition->runtime;
                Require(prepared->runtime->clock == stable_runtime.clock &&
                            prepared->runtime->scheduler ==
                                stable_runtime.scheduler,
                        "prepared runtime crossed the stable Kernel time "
                        "capabilities");
                prepared->runtime->operation_journal =
                    std::move(stable_runtime.operation_journal);
                prepared->runtime->addressed_kernel =
                    std::move(stable_runtime.addressed_kernel);
                retired_managed_composition =
                    std::move(active_managed_composition);
              } else {
                Require(detached_operation_journal != nullptr &&
                            detached_owner_clock != nullptr &&
                            detached_owner_scheduler != nullptr &&
                            prepared->runtime->clock == detached_owner_clock &&
                            prepared->runtime->scheduler ==
                                detached_owner_scheduler,
                        "detached addressed owner is incomplete");
                prepared->runtime->operation_journal =
                    std::move(detached_operation_journal);
                prepared->runtime->addressed_kernel =
                    std::move(detached_addressed_kernel);
                detached_owner_clock.reset();
                detached_owner_scheduler.reset();
              }
            }
            PublishDirectNativeRoutes(*prepared, config.package_root);
            auto staged =
                prepared->runtime->addressed_kernel
                    ->stage_current_runtime_configuration_facts_under_apply_fence(
                        request.configuration_fence_scope);
            if (!staged.ok()) {
              Fail("stable Kernel rejected candidate configuration staging: " +
                   staged.error().message);
            }
            InstallDirectSceneFrameResolver(projections, *prepared);
#if !defined(MANYFORGE_ENABLE_FOCUSED_PICK_PLACE_PLANNING_FIXTURE)
            AttachProductPickPlaceOwners(*prepared);
#endif
            active_managed_composition = std::move(prepared);
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
    bind_disable_handoff_owner();
    require_installed_candidate_configuration();
    {
      std::lock_guard lock(managed_composition_mutex);
      Require(installed_under_fence_composition_request.has_value() &&
                  *installed_under_fence_composition_request == request,
              "installed runtime composition changed before fence "
              "reconciliation completed");
      installed_composition_request = request;
      publish_installed_liveness_handoff(request);
      installed_under_fence_composition_request.reset();
    }
    try_release_retired_composition();
    return {
        request,
        component_manager::ManagerSelectedRuntimeCompositionState::kInstalled};
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
                installed_request->projection_stage_generation ==
                    named->projection_stage_generation &&
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
        // A candidate composition is installed while its predecessor remains
        // authoritative. Attach lifecycle control only once Manager has made
        // that exact candidate current; this event is the single retry point
        // and avoids both premature acceptance and background polling.
        {
          std::lock_guard lock(managed_composition_mutex);
          Require(active_managed_composition != nullptr &&
                      active_managed_composition->publication_generation ==
                          current_publication->publication_generation,
                  "Manager liveness successor has no exact active runtime "
                  "composition");
          TryAttachManagedCCLC(*active_managed_composition, config,
                               fence_owner);
        }
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
        std::lock_guard lock(managed_composition_mutex);
        auto *const owner = stable_addressed_kernel();
        Require(owner != nullptr, "stable Kernel is not installed");
        response = component_manager::detail::DispatchKernelCellAdmissionRpc(
            *owner, decoded);
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

class ExecutionCommandSurface final {
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
        observation_socket_(run_root, kObservationSocketFileName) {}

  int Run(const std::stop_token stop_token = {}) {
    std::jthread control_thread(
        [this](std::stop_token stop_token) { RunControlLane(stop_token); });
    std::jthread observation_thread(
        [this](std::stop_token stop_token) { RunObservationLane(stop_token); });
    std::jthread reconciliation_thread([this](std::stop_token stop_token) {
      RunReconciliationLane(stop_token);
    });
    while (g_stop_requested == 0 && !stop_token.stop_requested() &&
           !reconciliation_lane_failed_.load(std::memory_order_acquire)) {
      pollfd descriptor{command_socket_.get(), POLLIN, 0};
      int ready = -1;
      do {
        ready = ::poll(&descriptor, 1, kReconcilePeriodMilliseconds);
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
      if ((descriptor.revents & POLLIN) != 0) {
        HandleOneClient(command_socket_, CommandLane::kOrdinary);
      }
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        Fail("execution command socket became unavailable");
      }
      if (control_socket_failed_.load(std::memory_order_acquire)) {
        Fail("execution Stop control socket became unavailable");
      }
    }
    reconciliation_thread.request_stop();
    control_thread.request_stop();
    observation_thread.request_stop();
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
    for (const auto *binding : {&composition.direct_control_motion_binding,
                                &composition.direct_control_gripper_binding}) {
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
    const auto &binding = composition.direct_control_motion_binding.value();
    const auto &limits = composition.direct_control_joint_limits;
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

  [[nodiscard]] CanonicalValue
  QueryTargetResponse(const TargetObservationQuery &query,
                      Composition &composition) const {
    if (query.owner_epoch != owner_epoch_)
      return ErrorResponse("execution_target_owner_stale",
                           "target query names a replaced owner");
    const bool frame_query = query.frame_role_id.has_value();
    const auto &channel = frame_query
                              ? composition.runtime->motion_state_channel
                              : composition.runtime->gripper_state_channel;
    if (!channel)
      return ErrorResponse("execution_target_observation_unavailable",
                           "target has no qualified observation channel");
    const auto observed_binding = channel->binding();
    const auto &binding = observed_binding.value();
    const auto configuration =
        composition.runtime->ReadCurrentPublishedRouteConfiguration(
            binding.control_path->control_domain_id);
    if (!configuration.ok() || !configuration.value())
      return ErrorResponse("execution_target_observation_unavailable",
                           "target has no current route configuration");
    const auto representatives =
        composition.runtime->ReadCurrentPhysicalParticipants(
            *configuration.value());
    if (!representatives.ok())
      return ErrorResponse("execution_target_observation_unavailable",
                           "target route is not reconciled");
    const auto resolved = composition.target_resolver->ResolveObservationTarget(
        query.program_binding_id, query.target_role_id,
        core::CapabilityId(frame_query ? "model.kinematics.fk"
                                       : "tool.gripper.feedback"),
        observed_binding, representatives.value());
    if (!resolved.ok())
      return ErrorResponse(resolved.error().code, resolved.error().message);
    const auto &target = resolved.value().target.target_role();
    const auto observed =
        channel->current_state(composition.runtime->clock->now());
    if (!observed.ok())
      return ErrorResponse(observed.error().code, observed.error().message);
    const auto &state = observed.value();
    const auto current_configuration =
        composition.runtime->ReadCurrentPublishedRouteConfiguration(
            state.control_domain_id);
    if (!current_configuration.ok() || !current_configuration.value() ||
        !target_observation_source_is_current(
            resolved.value().observation_binding, state,
            *current_configuration.value()))
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
      // Empty base means the composed model root, i.e. the logical cell frame.
      const auto pose = composition.runtime->kinematics->forward_kinematics(
          core::FkRequestDTO{
              core::RobotStateDTO{state.joint_names, std::move(positions), {}},
              frame->compiled_frame_name,
              {}});
      if (!pose.ok())
        return ErrorResponse(pose.error().code, pose.error().message);
      value = PoseValue(pose.value());
    } else {
      if (!composition.runtime->gripper || binding.resources.size() != 1U)
        return ErrorResponse("gripper_feedback_unavailable",
                             "target has no qualified gripper feedback");
      const auto status = composition.runtime->gripper->get_gripper_status(
          binding.command_target_id.value());
      value = CanonicalValue(CanonicalValue::Object{
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
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("query_target")},
        {"ok", CanonicalValue(true)},
        {"target_observation", CanonicalValue(std::move(result))},
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
        composition.runtime->addressed_kernel->get_resident_operations(
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
          composition.runtime->addressed_kernel->get_runtime_scale(
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
    const auto &binding = composition.direct_control_motion_binding.value();
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
        composition.runtime->kinematics->forward_kinematics(core::FkRequestDTO{
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
    const auto &binding = composition.direct_control_gripper_binding.value();
    Require(binding.resources.size() == 1U,
            "direct-control gripper status has no exact scalar resource");
    const auto &resource = binding.resources.front();
    const bool model_backed = resource.source_joint_name.has_value();
    Require(binding.command_schema.has_value() &&
                model_backed ==
                    composition.runtime->gripper_position_limits.has_value(),
            "direct-control gripper status crosses its retained binding");
    const auto &limits = composition.runtime->gripper_position_limits;
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
    const auto &binding = composition.direct_control_gripper_binding.value();
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
    core::ObservedStateSnapshotDTO motion;
    std::optional<core::ObservedStateSnapshotDTO> gripper;
  };

  [[nodiscard]] CurrentControlState
  ReadCurrentControlState(Composition &composition) const {
    const auto motion =
        composition.runtime->motion_state_channel->current_state(
            composition.runtime->clock->now());
    if (!motion.ok()) {
      Fail("selected motion observed state is unavailable: " +
           motion.error().message);
    }
    Require(composition.runtime->gripper_state_channel != nullptr,
            "selected gripper observed-state channel is absent");
    Require(
        composition.runtime->gripper_binding.has_value() &&
            composition.runtime->gripper_binding->value().resources.size() ==
                1U,
        "selected gripper observation has no exact scalar binding");
    const auto gripper =
        composition.runtime->gripper_state_channel->current_state(
            composition.runtime->clock->now());
    const auto &resource =
        composition.runtime->gripper_binding->value().resources.front();
    const bool component_local =
        !resource.source_joint_name.has_value() &&
        !composition.runtime->gripper_position_limits.has_value();
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
        motion.value(),
        gripper.ok()
            ? std::optional<core::ObservedStateSnapshotDTO>{gripper.value()}
            : std::nullopt};
  }

  [[nodiscard]] CanonicalValue::Object
  IdentityFields(Composition &composition) const {
    const auto &bundle = *composition.bundle;
    const auto &binding = composition.program_binding != nullptr
                              ? *composition.program_binding
                              : *composition.direct_control_binding;
    const auto projection =
        composition.scene_authority_proxy != nullptr
            ? composition.scene_authority_proxy->CurrentProjectionRevision()
            : composition.projection_revision;
    Require(projection.retained_bundle_id ==
                    composition.projection_revision.retained_bundle_id &&
                projection.graph_revision ==
                    composition.projection_revision.graph_revision &&
                projection.scene_revision.valid(),
            "selected execution identity crosses its active product");
    Require(composition.runtime->gripper_binding.has_value() &&
                composition.runtime->gripper_binding->value()
                    .command_schema.has_value(),
            "selected execution identity has no gripper command schema");
    const auto &gripper_schema =
        *composition.runtime->gripper_binding->value().command_schema;
    return CanonicalValue::Object{
        {"active_publication_generation",
         CanonicalValue(
             static_cast<double>(composition.publication_generation.value()))},
        {"cell_id", CanonicalValue(bundle.value().cell_graph.cell_id.value())},
        {"control_management_mode",
         CanonicalValue(
             composition.cclc_runtime != nullptr ? "component_lifecycle"
             : composition.direct_route_configuration_published ? "direct"
                                                                : "pending")},
        {"graph_revision",
         CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
        {"gripper_command_schema_id",
         CanonicalValue(gripper_schema.command_schema_id.value())},
        {"gripper_command_schema_revision",
         CanonicalValue(gripper_schema.command_schema_revision.value())},
        {"gripper_target_role_id",
         CanonicalValue(composition.status_gripper_target_role_id.value())},
        {"motion_target_role_id",
         CanonicalValue(composition.status_motion_target_role_id.value())},
        {"owner_epoch", CanonicalValue(owner_epoch_.value())},
        {"program_binding_id",
         CanonicalValue(binding.program_binding_id().value())},
        {"program_id", CanonicalValue(binding.value().program_id.value())},
        {"program_revision",
         CanonicalValue(binding.value().program_revision.value())},
        {"retained_bundle_id",
         CanonicalValue(bundle.retained_bundle_id().value())},
        {"scene_revision", SceneRevisionValue(projection.scene_revision)},
    };
  }

  [[nodiscard]] CanonicalValue IdentityValue(Composition &composition) const {
    auto fields = IdentityFields(composition);
    fields.emplace_back("schema_version",
                        CanonicalValue("manyforge.execution_identity.v1"));
    return CanonicalValue(std::move(fields));
  }

  [[nodiscard]] CanonicalValue StatusResponse(Composition &composition) const {
    const auto current = ReadCurrentControlState(composition);
    const auto cell_stop_action =
        composition.runtime->addressed_kernel->cell_stop_action_projection();
    CanonicalValue::Array observed_targets;
    observed_targets.reserve(2U);
    observed_targets.push_back(ObservedTargetValue(
        composition.status_motion_target_role_id, current.motion));
    if (current.gripper.has_value()) {
      observed_targets.push_back(ObservedTargetValue(
          composition.status_gripper_target_role_id, *current.gripper));
    }
    auto status = IdentityFields(composition);
    status.emplace_back("cell_stop_action",
                        CellStopActionValue(cell_stop_action));
    status.emplace_back("direct_control",
                        DirectControlStatusValue(composition, current.motion));
    status.emplace_back(
        "direct_control_gripper",
        DirectControlGripperStatusValue(composition, current.gripper.has_value()
                                                         ? &*current.gripper
                                                         : nullptr));
    status.emplace_back("observed_targets",
                        CanonicalValue(std::move(observed_targets)));
#if defined(MANYFORGE_ROS2_CONTROL_EXECUTION_HOST)
    status.emplace_back("ros2_control_lifecycle_observation",
                        Ros2ControlLifecycleObservationValue(composition));
    status.emplace_back("ros2_control_transport_diagnostic",
                        Ros2ControlTransportDiagnosticValue(composition));
#endif
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("status")},
        {"ok", CanonicalValue(true)},
        {"status", CanonicalValue(std::move(status))},
    });
  }

  [[nodiscard]] CanonicalValue
  IdentityResponse(Composition &composition) const {
    const auto admission =
        composition.runtime->addressed_kernel->GetAuthoringAdmission();
    return CanonicalValue(CanonicalValue::Object{
        {"admission_generation", CanonicalValue(static_cast<double>(
                                     admission.admission_generation.value()))},
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("identity")},
        {"identity", IdentityValue(composition)},
        {"ok", CanonicalValue(true)},
    });
  }

  [[nodiscard]] CanonicalValue TargetsResponse(Composition &composition) const {
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("targets")},
        {"identity", IdentityValue(composition)},
        {"ok", CanonicalValue(true)},
        {"targets",
         CanonicalValue(CanonicalValue::Object{
             {"direct_control", DirectControlTargetValue(composition)},
             {"direct_control_gripper",
              DirectControlGripperTargetValue(composition)},
             {"schema_version",
              CanonicalValue("manyforge.execution_targets.v1")},
         })},
    });
  }

  [[nodiscard]] CanonicalValue
  ControlStateResponse(Composition &composition) const {
    const auto current = ReadCurrentControlState(composition);
    const core::RuntimeParticipantBindingDTO *motion_binding =
        &composition.direct_control_motion_binding.value();
    if (composition.program_binding != nullptr) {
      const auto selected = std::find_if(
          composition.runtime->motion_binding_aliases.begin(),
          composition.runtime->motion_binding_aliases.end(),
          [&](const auto &candidate) {
            return candidate.value().program_binding_id ==
                       composition.program_binding->program_binding_id() &&
                   candidate.value().target_role_id ==
                       composition.status_motion_target_role_id;
          });
      Require(selected != composition.runtime->motion_binding_aliases.end(),
              "selected Program Binding has no runtime-scale motion alias");
      motion_binding = &selected->value();
    }
    const core::RuntimeScaleControlSelectorDTO runtime_scale_selector{
        motion_binding->program_binding_id, motion_binding->component_role_id,
        motion_binding->target_role_id};
    const auto runtime_scale_control =
        composition.runtime->addressed_kernel->get_runtime_scale_control(
            runtime_scale_selector);
    if (!runtime_scale_control.ok()) {
      Fail("runtime-scale control projection failed: " +
           runtime_scale_control.error().message);
    }
    const auto manual_operations =
        composition.runtime->addressed_kernel->get_latest_run_operations(
            {direct_control_program_run_id_,
             direct_control_gripper_program_run_id_});
    CanonicalValue::Array manual_operation_values;
    manual_operation_values.reserve(manual_operations.size());
    for (const auto &projection : manual_operations) {
      manual_operation_values.emplace_back(CanonicalValue::Object{
          {"operation", OperationStateValue(projection.operation)},
          {"resident", CanonicalValue(projection.resident)}});
    }
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("control_state")},
        {"control_state",
         CanonicalValue(CanonicalValue::Object{
             {"cell_stop_action",
              CellStopActionValue(composition.runtime->addressed_kernel
                                      ->cell_stop_action_projection())},
             {"observed_gripper",
              DirectControlGripperObservedValue(
                  composition,
                  current.gripper.has_value() ? &*current.gripper : nullptr)},
             {"observed_motion",
              DirectControlObservedValue(composition, current.motion)},
             {"manual_operations",
              CanonicalValue(std::move(manual_operation_values))},
             {"runtime_scale_control",
              RuntimeScaleControlStatusValue(runtime_scale_control.value())},
             {"schema_version",
              CanonicalValue("manyforge.execution_control_state.v4")},
         })},
        {"identity", IdentityValue(composition)},
        {"ok", CanonicalValue(true)},
    });
  }

  struct ObservationProjection final {
    CanonicalValue response;
    std::shared_ptr<RetainedObservedStateChannel> channel;
    std::string source_epoch;
    std::uint64_t source_sequence{};
    std::uint64_t change_generation{};
  };

  [[nodiscard]] ObservationProjection
  BuildObservationProjection(Composition &composition) const {
    auto channel = composition.runtime->motion_state_channel;
    Require(channel != nullptr,
            "selected motion observation has no retained state channel");
    // Read the wake generation before the snapshot. A publication between
    // these two reads is either visible in the snapshot or makes the later
    // wait predicate immediately true, so a change cannot be missed.
    const auto change_generation = channel->render_change_generation();
    const auto now = composition.runtime->clock->now();
    CanonicalValue::Array targets;
    const auto append_target =
        [&](const auto &selected_channel,
            const core::RuntimeParticipantBinding &selected_binding) {
          const auto &binding = selected_binding.value();
          // A component-local actuator has no URDF coordinate to animate.
          if (binding.model_groups.empty())
            return;
          const auto observed = selected_channel->current_state(now);
          if (!observed.ok())
            Fail("selected observation is unavailable: " +
                 observed.error().message);
          const auto &state = observed.value();
          Require(state.complete &&
                      state.resources.size() == binding.resources.size() &&
                      state.joint_names.size() == binding.resources.size() &&
                      state.positions.size() == binding.resources.size() &&
                      state.controllable_component_id ==
                          binding.controllable_component_id &&
                      state.command_target_id == binding.command_target_id,
                  "selected observation crosses its retained target");
          CanonicalValue::Array source_names, compiled_names, kinds, positions;
          for (std::size_t index = 0; index < binding.resources.size();
               ++index) {
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
          targets.emplace_back(CanonicalValue::Object{
              {"command_target_id",
               CanonicalValue(binding.command_target_id.value())},
              {"compiled_joint_names",
               CanonicalValue(std::move(compiled_names))},
              {"controllable_component_id",
               CanonicalValue(binding.controllable_component_id.value())},
              {"coordinate_kinds", CanonicalValue(std::move(kinds))},
              {"positions_si", CanonicalValue(std::move(positions))},
              {"source_epoch", CanonicalValue(state.source_epoch.value())},
              {"source_joint_names", CanonicalValue(std::move(source_names))},
              {"source_sequence",
               CanonicalValue(static_cast<double>(state.sequence))},
          });
        };
    append_target(channel, composition.direct_control_motion_binding);
    if (composition.runtime->gripper_state_channel)
      append_target(composition.runtime->gripper_state_channel,
                    composition.direct_control_gripper_binding);
    const auto stream_epoch =
        "render." + std::to_string(channel->render_incarnation());
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
              CanonicalValue("manyforge.execution_observation.v2")},
             {"source_epoch", CanonicalValue(stream_epoch)},
             {"source_sequence",
              CanonicalValue(static_cast<double>(change_generation))},
             {"targets", CanonicalValue(std::move(targets))},
         })},
        {"ok", CanonicalValue(true)},
    });
    return ObservationProjection{std::move(response), std::move(channel),
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
               result.value(), command.program_selection->group_id)}});
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

  [[nodiscard]] CanonicalValue Execute(const Command &command,
                                       Composition &composition) {
    if (command.kind == CommandKind::kReadProgramExecutionScope) {
      const auto result =
          composition.runtime->addressed_kernel->get_program_execution_scope(
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
      return StatusResponse(composition);
    }
    if (command.kind == CommandKind::kIdentity) {
      return IdentityResponse(composition);
    }
    if (command.kind == CommandKind::kTargets) {
      return TargetsResponse(composition);
    }
    if (command.kind == CommandKind::kControlState) {
      return ControlStateResponse(composition);
    }
    if (command.kind == CommandKind::kObservation) {
      return ObservationResponse(composition);
    }
    if (command.kind == CommandKind::kEvents) {
      const auto page =
          composition.runtime->operation_journal->ReadOperationalEvents(
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
          composition.runtime->addressed_kernel->GetAuthoringAdmission();
      return CanonicalValue(CanonicalValue::Object{
          {"admission_generation",
           CanonicalValue(
               static_cast<double>(admission.admission_generation.value()))},
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("events")},
          {"event_page", OperationEventPageValue(page.value())},
          {"ok", CanonicalValue(true)},
      });
    }
    if (command.kind == CommandKind::kReadStop) {
      return ReadStop(command, *composition.runtime->addressed_kernel);
    }
    if (command.kind == CommandKind::kStop ||
        command.kind == CommandKind::kStopProgramRun ||
        command.kind == CommandKind::kStopProgramSelection ||
        command.kind == CommandKind::kFullStop ||
        command.kind == CommandKind::kMalformedStop) {
      return ExecuteStop(command, *composition.runtime->addressed_kernel);
    }
    const bool read_only =
        command.kind == CommandKind::kQueryTarget ||
        command.kind == CommandKind::kQueryOperation ||
        command.kind == CommandKind::kObservation ||
        command.kind == CommandKind::kGetStopFault ||
        command.kind == CommandKind::kRead ||
        command.kind == CommandKind::kReadStop ||
        command.kind == CommandKind::kGetOperationRecovery ||
        command.kind == CommandKind::kGetRuntimeScaleControl ||
        command.kind == CommandKind::kReadPickPlaceSequence;
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
    if (command.kind == CommandKind::kGetStopFault) {
      const auto offer = composition.runtime->addressed_kernel
                             ->get_stop_fault_recovery_offer();
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
    if (command.kind == CommandKind::kRecoverStopFault) {
      Require(command.stop_fault_recovery.has_value(),
              "Stop-fault recovery command has no complete intent");
      const auto result =
          composition.runtime->addressed_kernel->recover_stop_fault(
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
          composition.runtime->addressed_kernel->get_operation_projection(
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
          composition.runtime->addressed_kernel
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
    if (command.kind == CommandKind::kPreviewCapabilityCommand) {
      auto preview =
          composition.runtime->addressed_kernel->preview_capability_command(
              *command.operation);
      if (!preview.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(preview.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("preview_capability_command")},
          {"ok", CanonicalValue(true)},
          {"preview_receipt",
           CapabilityCommandPreviewReceiptValue(
               preview.value(), *command.operation, *command.operation_digest,
               composition.runtime->gripper_position_limits, owner_epoch_)},
      });
    }
    if (command.kind == CommandKind::kPreviewMotion) {
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
          composition.runtime->addressed_kernel->begin_motion_preview_session(
              preview_intent);
      if (!session.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(session.error())},
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
      auto preview =
          session.value()->preview_motion(preview_intent, std::move(*inventory),
                                          std::move(start_state.value()));
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
           MotionPreviewReceiptValue(
               preview.value(), *command.operation, *command.operation_digest,
               composition.direct_control_motion_binding, owner_epoch_)},
      });
    }
    if (command.kind == CommandKind::kReadPickPlaceSequence) {
      if (!composition.pick_place_execution_owner) {
        return ErrorResponse("pick_place_execution_owner_unavailable",
                             "the selected native process has no joined "
                             "pick/place sequence owner");
      }
      const auto result = composition.pick_place_execution_owner->GetStatus(
          *command.pick_place_sequence_id);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("read_pick_place_sequence")},
          {"ok", CanonicalValue(true)},
          {"sequence_status", PickPlaceSequenceStatusValue(result.value())},
      });
    }
    if (command.kind == CommandKind::kStartPick ||
        command.kind == CommandKind::kStartPlace) {
      if (!composition.pick_place_execution_owner) {
        return ErrorResponse("pick_place_execution_owner_unavailable",
                             "the selected native process has no joined "
                             "pick/place sequence owner");
      }
      const auto validate_origin = [&](const auto &request) {
        for (const auto *operation :
             {&request.approach_operation, &request.descend_operation,
              &request.tool_operation, &request.retreat_operation}) {
          const auto valid =
              composition.target_resolver->ValidateExecutionOrigin(*operation);
          if (!valid.ok())
            return valid;
        }
        return core::Result<void>::Ok();
      };
      const auto origin = command.kind == CommandKind::kStartPick
                              ? validate_origin(*command.pick_planning)
                              : validate_origin(*command.place_planning);
      if (!origin.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(origin.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      const auto sequence_id = command.kind == CommandKind::kStartPick
                                   ? command.pick_planning->sequence_id
                                   : command.place_planning->sequence_id;
      const auto result =
          command.kind == CommandKind::kStartPick
              ? composition.pick_place_execution_owner->BeginPick(
                    *command.pick_planning)
              : composition.pick_place_execution_owner->BeginPlace(
                    *command.place_planning);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      const auto command_name = command.kind == CommandKind::kStartPick
                                    ? "start_pick"
                                    : "start_place";
      const auto kind = command.kind == CommandKind::kStartPick
                            ? core::PickPlaceExecutionKindDTO::kPick
                            : core::PickPlaceExecutionKindDTO::kPlace;
      CanonicalValue sequence_status(nullptr);
      if (result.value().sequence_status.has_value()) {
        sequence_status =
            PickPlaceSequenceStatusValue(*result.value().sequence_status);
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue(command_name)},
          {"ok", CanonicalValue(true)},
          {"planning_result",
           PickPlacePlanningResultValue(sequence_id, kind,
                                        result.value().planning_result)},
          {"sequence_status", std::move(sequence_status)},
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
      const auto result =
          command.kind == CommandKind::kPlanPick
              ? composition.pick_place_planning_owner->PlanPick(
                    *command.pick_planning)
              : composition.pick_place_planning_owner->PlanPlace(
                    *command.place_planning);
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
          capture.ok() ? composition.runtime->addressed_kernel
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
      const auto origin = composition.target_resolver->ValidateExecutionOrigin(
          *command.operation);
      if (!origin.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(origin.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      const auto result =
          composition.runtime->addressed_kernel->execute(*command.operation);
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
          composition.runtime->addressed_kernel->get_runtime_scale_control(
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
          composition.runtime->addressed_kernel->set_runtime_scale(
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
          composition.runtime->addressed_kernel->set_runtime_scale_control(
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
          composition.runtime->addressed_kernel
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
    if (command.kind == CommandKind::kReleaseProgramSelection) {
      const auto result =
          composition.runtime->addressed_kernel->release_program_selection(
              *command.program_selection, command.expected_selection_revision);
      if (!result.ok())
        return ErrorResponse(result.error().code, result.error().message,
                             result.error().details);
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("release_program_selection")},
          {"ok", CanonicalValue(true)},
          {"scope", ProgramExecutionScopeValue(result.value())}});
    }
    if (command.kind == CommandKind::kReleaseProgramRunStopFence) {
      const auto result =
          composition.runtime->addressed_kernel->release_program_run_stop_fence(
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
        return composition.runtime->addressed_kernel->pause(*command.control);
      case CommandKind::kResume:
        return composition.runtime->addressed_kernel->resume(*command.control);
      case CommandKind::kReset:
        return composition.runtime->addressed_kernel->reset(*command.control);
      case CommandKind::kAcknowledgeCompleted:
        return composition.runtime->addressed_kernel->acknowledge_completed(
            *command.control);
      case CommandKind::kStatus:
      case CommandKind::kIdentity:
      case CommandKind::kTargets:
      case CommandKind::kQueryTarget:
      case CommandKind::kQueryOperation:
      case CommandKind::kControlState:
      case CommandKind::kObservation:
      case CommandKind::kEvents:
      case CommandKind::kPreviewMotion:
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
      case CommandKind::kReleaseProgramSelection:
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
      case CommandKind::kStartPick:
      case CommandKind::kStartPlace:
      case CommandKind::kReadPickPlaceSequence:
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
    return composition.runtime->addressed_kernel
        ->reconcile_manual_operation_admission();
  }

  void RunReconciliationLane(std::stop_token stop_token) noexcept {
    try {
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
            auto error = [&]() -> std::optional<core::ErrorDTO> {
              const auto reconciled = composition.runtime->addressed_kernel
                                          ->reconcile_subordinate_state();
              if (!reconciled.ok()) {
                return reconciled.error();
              }
              if (composition.pick_place_execution_owner) {
                const auto pick_place_reconciled =
                    composition.pick_place_execution_owner->Reconcile();
                if (!pick_place_reconciled.ok()) {
                  return pick_place_reconciled.error();
                }
              }
              const auto manual_settlement =
                  ReconcileDirectControlOperations(composition);
              if (!manual_settlement.ok())
                return manual_settlement.error();
              return std::nullopt;
            }();
            std::lock_guard error_lock(reconcile_error_mutex_);
            last_reconcile_error_ = std::move(error);
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
    }
  }

  void RunControlLane(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
      pollfd descriptor{control_socket_.get(), POLLIN, 0};
      int ready = -1;
      do {
        ready = ::poll(&descriptor, 1, kReconcilePeriodMilliseconds);
      } while (ready < 0 && errno == EINTR && !stop_token.stop_requested());
      if (stop_token.stop_requested()) {
        return;
      }
      if (ready < 0 ||
          (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        control_socket_failed_.store(true, std::memory_order_release);
        return;
      }
      if ((descriptor.revents & POLLIN) != 0) {
        HandleOneClient(control_socket_, CommandLane::kStop);
      }
    }
  }

  void RunObservationLane(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested()) {
      pollfd descriptor{observation_socket_.get(), POLLIN, 0};
      int ready = -1;
      do {
        ready = ::poll(&descriptor, 1, kReconcilePeriodMilliseconds);
      } while (ready < 0 && errno == EINTR && !stop_token.stop_requested());
      if (stop_token.stop_requested() || ready < 0 ||
          (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        return;
      }
      if ((descriptor.revents & POLLIN) != 0) {
        HandleOneClient(observation_socket_, CommandLane::kObservation,
                        stop_token);
      }
    }
  }

  void HandleOneClient(const CommandSocket &socket, const CommandLane lane,
                       const std::stop_token stop_token = {}) noexcept {
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
                  "cell_admission_recovery_receipt"}) {
              if (bytes.find(std::string("\"command\":\"") + candidate +
                             "\"") != std::string_view::npos) {
                cell_admission_command = candidate;
                break;
              }
            }
            const auto cell_admission =
                detail::TryDecodeCellAdmissionCommand(bytes);
            std::shared_lock composition_lock(composition_access_.mutex);
            if (cell_admission.has_value() && lane != CommandLane::kOrdinary) {
              const bool stop_lane = lane == CommandLane::kStop;
              response = detail::CellAdmissionResponseValue(
                  cell_admission->command,
                  core::ErrorDTO{
                      stop_lane ? "execution_control_command_unsupported"
                                : "execution_observation_command_unsupported",
                      stop_lane
                          ? "the independent Stop control socket accepts only "
                            "Stop"
                          : "the independent observation socket accepts only "
                            "observation",
                      {}});
            } else if (cell_admission.has_value()) {
              auto *const owner = composition_access_.addressed_kernel;
              Require(owner != nullptr,
                      "stable addressed execution Kernel is not installed");
              response = detail::CellAdmissionResponseValue(
                  cell_admission->command,
                  component_manager::detail::DispatchKernelCellAdmissionRpc(
                      *owner, *cell_admission));
            } else {
              const auto command = DecodeCommand(bytes);
              if (lane == CommandLane::kObservation &&
                  command.kind != CommandKind::kObservation) {
                response = ErrorResponse(
                    "execution_observation_command_unsupported",
                    "the independent observation socket accepts only "
                    "observation");
              } else if (composition_access_.current == nullptr) {
                auto *const owner = composition_access_.addressed_kernel;
                Require(owner != nullptr,
                        "stable addressed execution Kernel is not installed");
                if (lane == CommandLane::kStop) {
                  // All containment requests reach the stable Kernel during
                  // reconstruction. It owns exact scope resolution, journal
                  // fences and unavailable evidence; the host cannot infer
                  // rest from the absence of a composed physical product.
                  response = ExecuteStop(command, *owner);
                } else if (lane == CommandLane::kOrdinary &&
                           command.kind == CommandKind::kReadStop) {
                  response = ReadStop(command, *owner);
                } else if (lane == CommandLane::kOrdinary &&
                           command.kind == CommandKind::kIdentity) {
                  response = ExecutionOwnerNotReadyIdentityResponse(
                      *owner, owner_epoch_);
                } else {
                  response =
                      ErrorResponse("execution_owner_not_ready",
                                    "the managed execution owner has no active "
                                    "physical product",
                                    {{"owner_epoch", owner_epoch_.value()}});
                }
              } else {
                if (lane == CommandLane::kObservation &&
                    command.observation_cursor_supplied) {
                  composition_lock.unlock();
                  response = ObservationResponseAfter(command, client.get(),
                                                      stop_token);
                } else {
                  response =
                      lane == CommandLane::kStop
                          ? ExecuteStop(command,
                                        *composition_access_.current->runtime
                                             ->addressed_kernel)
                          : Execute(command, *composition_access_.current);
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
        pollfd outbound{client.get(), POLLOUT, 0};
        int response_ready = -1;
        do {
          response_ready =
              ::poll(&outbound, 1, kClientPacketDeadlineMilliseconds);
        } while (response_ready < 0 && errno == EINTR);
        Require(response_ready > 0 && (outbound.revents & POLLOUT) != 0,
                "execution command client did not accept a response in time");
        ssize_t sent = -1;
        do {
          sent = ::send(client.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
        } while (sent < 0 && errno == EINTR);
        Require(sent == static_cast<ssize_t>(bytes.size()),
                "execution command response was not sent atomically");
        if (lane != CommandLane::kObservation) {
          return;
        }
      }
    } catch (...) {
      // A malformed/disappearing private client cannot retire the sole owner.
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
  std::atomic<bool> control_socket_failed_{false};
  std::atomic<bool> reconciliation_lane_failed_{false};
  mutable std::mutex reconcile_error_mutex_;
  std::optional<core::ErrorDTO> last_reconcile_error_;
  std::exception_ptr reconciliation_lane_failure_;
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
    {
      std::lock_guard lock(control.state_mutex);
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

class NativeExecutionHost final {
public:
  explicit NativeExecutionHost(HostConfig config)
      : config_(std::move(config)),
        owner_epoch_(RandomToken("execution-host-")),
        runtime_epoch_(RandomToken("native-runtime-")), admission_gate_(),
        composition_(BuildComposition(config_, owner_epoch_, runtime_epoch_,
                                      &admission_gate_)) {
    const auto &runtime_units =
        composition_->runtime->motion_binding.value().runtime_unit_epochs;
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
    fence_owner_->BindDisableHandoffKernel(
        *composition_->runtime->addressed_kernel);
#if defined(MANYFORGE_ENABLE_STANDALONE_ADMISSION_FIXTURE)
    const auto fixture_readiness =
        composition_->runtime->addressed_kernel->UpdateCellAdmissionReadiness(
            core::CellAdmissionReadinessSnapshotDTO{
                true,
                core::ProgramCycleStatusDTO{
                    core::RequestOwnerEpoch(
                        "standalone-admission-fixture.program-runner"),
                    1U, core::ProgramCycleStateDTO::kIdleAfterReset,
                    std::nullopt, std::nullopt, std::nullopt},
                true, 0U, 0U, true, true, 0U, 0U, 0U});
    Require(fixture_readiness.ok() &&
                fixture_readiness.value().authoring_allowed,
            "standalone admission fixture did not reach deterministic Idle");
#endif
    AttachRetainedNativeCCLC(*composition_, config_, *fence_owner_);
    composition_access_.current = composition_.get();
    composition_access_.addressed_kernel =
        composition_->runtime->addressed_kernel.get();
    // This legacy standalone owner has no Manager lease or liveness receiver.
    // Explicitly keep only its private admission seam open; managed children
    // use a separate, initially closed admission instance.
    standalone_command_admission_.OpenIfUnchanged(
        standalone_command_admission_.generation());
    command_surface_ = std::make_unique<ExecutionCommandSurface>(
        config_.execution_run_root, composition_access_,
        standalone_command_admission_, owner_epoch_);
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
  Require(seen.size() == 9U,
          "package, Manager state, execution state/run, Program Binding, "
          "motion/gripper target roles, candidate worker, and "
          "cell options "
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
              !config.motion_target_role_id.empty() &&
              config.motion_target_role_id.size() <= kMaximumIdentityBytes &&
              !config.gripper_target_role_id.empty() &&
              config.gripper_target_role_id.size() <= kMaximumIdentityBytes,
          "host identities must be bounded and non-empty");
  return config;
}

void InstallSignalHandlers() {
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
