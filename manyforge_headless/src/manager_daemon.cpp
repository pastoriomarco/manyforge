#include "manyforge_headless/cell_admission_readiness_reducer.hpp"
#include "manyforge_headless/manager_package_generation.hpp"
#include "manyforge_headless/owner_publication.hpp"
#include "manyforge_headless/scene_runtime_pose_projector.hpp"
#include "manyforge_headless/scene_runtime_state_owner.hpp"
#include "manyforge_headless/source_snapshot.hpp"
#include "manager_response_transport_internal.hpp"
#include "program_runner_publisher_fault_internal.hpp"
#include "scene_admission_reconciliation_internal.hpp"
#include <manyforge_deployment/execution_runtime_contract.hpp>

#include <manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_liveness_transport.hpp>
#include <manyforge_component_manager/manager_named_apply_coordinator.hpp>
#include <manyforge_deployment/initial_scene_projection.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/activation_content_collection.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include "manager_reference_diagnostics_internal.hpp"
#include <manyforge_deployment/program_binding_outcome.hpp>
#include <manyforge_component_manager/manager_runtime_convergence_coordinator.hpp>
#include <manyforge_component_manager/retained_execution_host_unit_adapter.hpp>
#include <manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp>
#include <manyforge_component_manager/runtime_unit_adapter_router.hpp>
#include <manyforge_component_manager/site_settings.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_scene/logical_scene_owner.hpp>
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <manyforge_scene/scene_transaction_codec.hpp>
#include <manyforge_core/types/carried_object_lost.hpp>

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
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <string_view>
#include <system_error>
#include <thread>
#include <type_traits>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::headless {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;
namespace scene = manyforge::scene;

using content_identity::CanonicalValue;

constexpr std::string_view kManagerCommandApiVersion =
    "manyforge.manager_command.v1";

// Preserve the scene-runtime operation's existing refusal code while keeping
// a typed lower-level cause and its producer for diagnosis.
core::ErrorDTO SceneRuntimeFailureFromException(
    std::string code, const std::exception &error,
    std::map<std::string, std::string> details = {}) {
  if (const auto *typed = dynamic_cast<const core::TypedError *>(&error)) {
    details.emplace("cause_code", typed->code());
    details.emplace("cause_producer", typed->producer().empty()
        ? "component_manager" : typed->producer());
    for (const auto &[key, value] : typed->details())
      details.emplace("cause." + key, value);
  }
  return {std::move(code), error.what(), std::move(details)};
}
constexpr std::string_view kManagerCommandResponseApiVersion =
    "manyforge.manager_command_response.v2";
// The owner named by a Manager response error unless a typed cause names
// another producer (for example a forwarded Kernel refusal).
constexpr std::string_view kManagerProducer = "component_manager";
constexpr std::string_view kManagerSourceFileName = "manager-source.json";
constexpr std::string_view kLogicalSceneSourceFileName =
    "logical-scene-source.json";
constexpr std::string_view kSceneRuntimeSourceFileName =
    "scene-runtime-source.json";
constexpr std::string_view kManagerSocketFileName = "manager-command.sock";
constexpr std::string_view kManagerDaemonLockFileName = ".manager-daemon.lock";
constexpr std::string_view kProgramRunnerLivenessApiVersion =
    "manyforge.program_runner_liveness.v1";
constexpr std::string_view kProgramRunnerLivenessPolicyId =
    "manyforge.program-runner.manager-presence.v1";
constexpr std::string_view kProgramRunnerLivenessPolicyRevision = "1";
constexpr std::string_view kProgramRunnerLocalSafeStateContractId =
    "manyforge.program-runner.manager-loss-containment.v1";
constexpr std::string_view kProgramRunnerLocalSafeStateContractRevision = "1";
constexpr std::size_t kMaximumProgramRunnerLivenessPacketBytes = 16U * 1024U;
constexpr std::size_t kMaximumKernelStopResponseBytes = 1024U * 1024U;
constexpr std::size_t kMaximumCommandBytes =
    2U * scene::kMaximumLogicalSceneTransactionBytes + 8U * 1024U;
constexpr std::size_t kMaximumResponseBytes =
    3U * scene::kMaximumLogicalSceneTransactionBytes + 16U * 1024U;
constexpr std::size_t kMaximumCommandIdentityBytes = 256U;
constexpr std::size_t kMaximumScenePoseQueryResources = 128U;
constexpr std::uint64_t kMaximumPoseObservationLifetimeNs =
    60ULL * 1000ULL * 1000ULL * 1000ULL;
constexpr std::size_t kMaximumPoseObserverSessions =
    kDefaultMaximumPoseObservationKeys;
constexpr std::size_t kOwnerEpochRandomBytes = 16U;
constexpr std::size_t kSocketBacklog = 16U;
constexpr int kClientReadTimeoutMilliseconds = 250;
constexpr int kMaximumBootstrapAdvances = 8;

volatile sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int) { g_stop_requested = 1; }

class FileDescriptor final {
public:
  explicit FileDescriptor(const int value = -1) noexcept : value_(value) {}
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

[[noreturn]] void FailErrno(const std::string_view context) {
  throw std::runtime_error(
      std::string(context) + ": " +
      std::system_error(errno, std::generic_category()).what());
}

[[noreturn]] void Fail(const std::string &detail) {
  throw std::runtime_error("Manager daemon: " + detail);
}

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    Fail(detail);
  }
}

std::filesystem::path ExistingDirectory(std::filesystem::path path,
                                        const std::string_view label) {
  Require(!path.empty(), std::string(label) + " cannot be empty");
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  Require(!error && !std::filesystem::is_symlink(status) &&
              std::filesystem::is_directory(status),
          std::string(label) + " must be an existing non-symlink directory");
  return path;
}

std::filesystem::path ExistingPrivateDirectory(std::filesystem::path path,
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
  Require(mismatch.first != canonical_directory.end() &&
              mismatch.second != canonical_path.end(),
          std::string(labels) +
              " must be disjoint from disposable run storage");
}

std::filesystem::path ProspectiveDatabasePath(std::filesystem::path path,
                                              const std::string_view label) {
  Require(!path.empty() && path.has_filename() && path.filename() != "." &&
              path.filename() != "..",
          std::string(label) + " path is invalid");
  static_cast<void>(ExistingPrivateDirectory(path.parent_path(),
                                             std::string(label) + " parent"));
  const auto status = std::filesystem::symlink_status(path);
  Require(!std::filesystem::is_symlink(status),
          std::string(label) + " path cannot be a symlink");
  return path;
}

std::filesystem::path DatabasePath(std::filesystem::path path) {
  return ProspectiveDatabasePath(std::move(path), "logical-scene database");
}

std::filesystem::path InstalledTargetRoot() {
  std::error_code error;
  const auto executable = std::filesystem::canonical("/proc/self/exe", error);
  Require(!error && executable.is_absolute() && executable.has_parent_path(),
          "Manager executable installed-target root is unavailable");
  return ExistingDirectory(executable.parent_path(),
                           "Manager installed-target root");
}

std::string RandomToken(const std::string_view prefix) {
  std::array<unsigned char, kOwnerEpochRandomBytes> bytes{};
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      FailErrno("mint owner token");
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

class DaemonLock final {
public:
  explicit DaemonLock(const std::filesystem::path &state_root) {
    const auto path = state_root / std::string(kManagerDaemonLockFileName);
    descriptor_ = FileDescriptor(
        ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
               S_IRUSR | S_IWUSR));
    if (!descriptor_) {
      FailErrno("open Manager daemon lock");
    }
    struct stat status {};
    if (::fstat(descriptor_.get(), &status) != 0) {
      FailErrno("inspect Manager daemon lock");
    }
    Require(S_ISREG(status.st_mode),
            "Manager daemon lock is not a regular file");
    if (::flock(descriptor_.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        Fail("another Manager daemon already owns this state root");
      }
      FailErrno("acquire Manager daemon lock");
    }
  }

private:
  FileDescriptor descriptor_;
};

class CommandSocket final {
public:
  explicit CommandSocket(const std::filesystem::path &state_root)
      : path_(state_root / std::string(kManagerSocketFileName)) {
    const auto spelling = path_.string();
    Require(spelling.size() < sizeof(sockaddr_un::sun_path),
            "Manager command socket path is too long");

    struct stat existing {};
    if (::lstat(path_.c_str(), &existing) == 0) {
      Require(S_ISSOCK(existing.st_mode),
              "Manager command path exists and is not a socket");
      if (::unlink(path_.c_str()) != 0) {
        FailErrno("remove stale Manager command socket");
      }
    } else if (errno != ENOENT) {
      FailErrno("inspect Manager command socket path");
    }

    descriptor_ = FileDescriptor(
        ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!descriptor_) {
      FailErrno("create Manager command socket");
    }
    const int receive_buffer_bytes = static_cast<int>(kMaximumCommandBytes);
    if (::setsockopt(descriptor_.get(), SOL_SOCKET, SO_RCVBUF,
                     &receive_buffer_bytes,
                     sizeof(receive_buffer_bytes)) != 0) {
      FailErrno("configure Manager command socket buffers");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
    if (::bind(descriptor_.get(), reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) != 0) {
      FailErrno("bind Manager command socket");
    }
    if (::chmod(path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
      FailErrno("restrict Manager command socket permissions");
    }
    if (::lstat(path_.c_str(), &bound_status_) != 0 ||
        !S_ISSOCK(bound_status_.st_mode)) {
      Fail("bound Manager command path is not a socket");
    }
    if (::listen(descriptor_.get(), static_cast<int>(kSocketBacklog)) != 0) {
      FailErrno("listen on Manager command socket");
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

struct DaemonConfig final {
  std::filesystem::path install_root;
  std::filesystem::path activation_root;
  std::filesystem::path canonical_empty_admission_filename{
      "canonical-empty-admission.json"};
  std::filesystem::path manager_state_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path execution_state_root;
  std::filesystem::path execution_run_root;
  std::filesystem::path logical_scene_database;
  std::filesystem::path program_cycle_journal;
  std::filesystem::path projection_root;
  std::filesystem::path program_runner_liveness_socket;
  /// Optional approved root for relative Program scene meshes. The execution
  /// host adapter opens it once; empty means relative meshes fail closed.
  std::filesystem::path scene_asset_root;
  /// Optional generic machine site document (`manyforge.site.v1`).
  std::optional<std::filesystem::path> site_settings_path;
  std::chrono::milliseconds reconciliation_period{500};
};

/// Reads one caller-written startup document; `label` names it in failures.
[[nodiscard]] std::string ReadStartupDocument(const std::filesystem::path &path,
                                              const std::string &label) {
  FileDescriptor file(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
  if (file.get() < 0) FailErrno("open " + label + " " + path.string());
  struct stat status {};
  if (::fstat(file.get(), &status) != 0)
    FailErrno("inspect " + label + " " + path.string());
  Require(S_ISREG(status.st_mode), label + " must be a regular file: " + path.string());
  std::string document;
  std::array<char, 4096U> buffer{};
  while (true) {
    const auto count = ::read(file.get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) continue;
    if (count < 0) FailErrno("read " + label + " " + path.string());
    if (count == 0) break;
    document.append(buffer.data(), static_cast<std::size_t>(count));
  }
  return document;
}

/// Reads and validates the generic site document once at startup; malformed
/// input refuses startup. Only the exact validated bytes reach the adapter.
[[nodiscard]] std::optional<std::string>
LoadSiteSettingsDocument(const std::optional<std::filesystem::path> &path) {
  if (!path.has_value()) return std::nullopt;
  auto document = ReadStartupDocument(*path, "site settings");
  static_cast<void>(component_manager::ParseSiteSettings(document));
  return document;
}
enum class CommandKind {
  kDeploymentCandidatePreview,
  kApply,
  kApplyStatus,
  kApplyAcknowledge,
  kApplyReleaseForNew,
  kStopComponentProcess,
  kComponentProcessStopResult,
  kComponentProcessStopOffer,
  kSceneCommit,
  kSceneTransactionStatus,
  kSceneOwnerRead,
  kSceneCandidatePreview,
  kScenePoseQuery,
  kSceneRuntimeOwnerRead,
  kSceneRuntimeObserverOpen,
  kSceneRuntimeObserve,
  kSceneRuntimeLatchBeforePlanning,
  kSceneRuntimeLatchBetweenNodes,
  kReconcile,
  kShutdownRuntime,
};

[[nodiscard]] bool
RequiresCurrentPackageGeneration(const CommandKind command) noexcept {
  switch (command) {
  case CommandKind::kDeploymentCandidatePreview:
  case CommandKind::kSceneCommit:
  case CommandKind::kSceneCandidatePreview:
  case CommandKind::kSceneRuntimeObserverOpen:
  case CommandKind::kSceneRuntimeObserve:
  case CommandKind::kSceneRuntimeLatchBeforePlanning:
  case CommandKind::kSceneRuntimeLatchBetweenNodes:
  case CommandKind::kReconcile:
    return true;
  case CommandKind::kSceneOwnerRead:
  case CommandKind::kSceneTransactionStatus:
  case CommandKind::kApply:
  case CommandKind::kApplyStatus:
  case CommandKind::kApplyAcknowledge:
  case CommandKind::kApplyReleaseForNew:
  case CommandKind::kStopComponentProcess:
  case CommandKind::kComponentProcessStopResult:
  case CommandKind::kComponentProcessStopOffer:
  case CommandKind::kScenePoseQuery:
  case CommandKind::kSceneRuntimeOwnerRead:
  case CommandKind::kShutdownRuntime:
    return false;
  }
  return true;
}

struct DeploymentCandidateReceipt final {
  core::DeploymentArtifactId deployment_artifact_id;
  core::DeploymentRevision deployment_revision;
  core::ContentDigest prepared_activation_id;
  core::RetainedBundleId candidate_retained_bundle_id;
  core::GraphRevision candidate_graph_revision;
  core::RetainedBundleId expected_current_retained_bundle_id;
  core::GraphRevision expected_current_graph_revision;
  core::AdmissionGeneration expected_admission_generation;
  std::string manager_owner_epoch;
  /// Digest of the authoritative impact shown with this preview. Apply
  /// is refused if the recomputed impact differs, and a destructive impact
  /// must be explicitly accepted by echoing this digest.
  core::ContentDigest impact_digest;
  bool destructive_impact{};
  core::ContentDigest receipt_digest;

  friend bool operator==(const DeploymentCandidateReceipt &,
                         const DeploymentCandidateReceipt &) = default;
};

constexpr std::size_t kMaximumDeploymentCandidateReceipts = 256U;

struct PoseObserverBinding final {
  std::string object_id;
  std::string policy_id;
  bool eligible{};

  friend bool operator==(const PoseObserverBinding &,
                         const PoseObserverBinding &) = default;
  friend bool operator<(const PoseObserverBinding &left,
                        const PoseObserverBinding &right) noexcept {
    return std::tie(left.object_id, left.policy_id) <
           std::tie(right.object_id, right.policy_id);
  }
};

struct ManagerCommand final {
  CommandKind kind;
  std::optional<std::string> request_id{};
  std::optional<std::string> request_owner_epoch{};
  std::optional<std::string> source_label{};
  std::optional<std::string> correlation_id{};
  std::optional<core::SceneTransactionRequestDTO> scene_transaction{};
  std::optional<core::DeploymentArtifactId> deployment_artifact_id{};
  std::optional<core::DeploymentRevision> deployment_revision{};
  std::optional<core::RetainedBundleId> expected_current_retained_bundle_id{};
  std::optional<core::GraphRevision> expected_current_graph_revision{};
  std::optional<core::AdmissionGeneration> expected_admission_generation{};
  std::optional<core::CellAdmissionClaimDTO> parent_authoring_claim{};
  std::optional<DeploymentCandidateReceipt> deployment_candidate_receipt{};
  std::optional<component_manager::ManagerApplyPreparedActivationDTO>
      prepared_activation{};
  component_manager::ManagerSceneInitialization scene_initialization{
      component_manager::ManagerSceneInitialization::kPreserve};
  std::optional<core::ProjectionRevisionDTO> expected_scene_projection{};
  std::vector<core::SceneResourceId> scene_resource_ids{};
  std::optional<core::ResolvedSceneCandidatePreviewRequestDTO>
      scene_candidate_preview{};
  std::optional<SceneRuntimeIdentity> expected_scene_runtime_identity{};
  std::optional<std::string> pose_source_id{};
  std::optional<std::string> pose_source_epoch{};
  std::optional<std::uint64_t> pose_source_sequence{};
  std::optional<std::string> pose_object_id{};
  std::optional<std::string> pose_policy_id{};
  std::optional<std::uint64_t> pose_lifetime_ns{};
  std::optional<PoseSourceQuality> pose_payload_quality{};
  std::optional<bool> pose_eligible{};
  std::optional<CanonicalValue> pose_payload{};
  std::vector<PoseObserverBinding> pose_observer_bindings{};
  std::optional<core::RetainedBundleId> candidate_retained_bundle_id{};
  std::optional<core::GraphRevision> candidate_graph_revision{};
  std::optional<core::ContentDigest> prepared_activation_id{};
  std::optional<core::ContentDigest> accepted_impact_digest{};
  std::optional<core::ApplyId> apply_id{};
  std::optional<core::ApplyGeneration> apply_generation{};
  std::optional<component_manager::ManagerComponentProcessStopRequestDTO> component_process_stop{};
  std::optional<core::ControllableComponentId> process_stop_offer_component{};
};

const CanonicalValue &RequiredMember(const CanonicalValue &root,
                                     const std::string_view name) {
  const auto *member = root.find_object_member(name);
  Require(member != nullptr, "command is missing field " + std::string(name));
  return *member;
}

std::string RequiredString(const CanonicalValue &root,
                           const std::string_view name,
                           const std::size_t maximum_bytes = kMaximumCommandIdentityBytes) {
  const auto *value =
      std::get_if<std::string>(&RequiredMember(root, name).storage());
  Require(value != nullptr && !value->empty() &&
              value->size() <= maximum_bytes,
          "command field " + std::string(name) +
              " must be one bounded non-empty string");
  return *value;
}

bool RequiredBool(const CanonicalValue &root, const std::string_view name) {
  const auto *value = std::get_if<bool>(&RequiredMember(root, name).storage());
  Require(value != nullptr,
          "command field " + std::string(name) + " must be boolean");
  return *value;
}

void RequireExactObjectSize(const CanonicalValue &value, std::size_t size,
                            std::string_view label);
std::uint64_t RequiredUnsigned(const CanonicalValue &value,
                               std::string_view label);

CanonicalValue
DeploymentCandidateReceiptPayload(const DeploymentCandidateReceipt &receipt) {
  return CanonicalValue(CanonicalValue::Object{
      {"candidate_graph_revision",
       CanonicalValue(receipt.candidate_graph_revision.value())},
      {"candidate_retained_bundle_id",
       CanonicalValue(receipt.candidate_retained_bundle_id.value())},
      {"deployment_artifact_id",
       CanonicalValue(receipt.deployment_artifact_id.value())},
      {"deployment_revision",
       CanonicalValue(receipt.deployment_revision.value())},
      {"expected_admission_generation",
       CanonicalValue(
           static_cast<double>(receipt.expected_admission_generation.value()))},
      {"expected_current",
       CanonicalValue(CanonicalValue::Object{
           {"graph_revision",
            CanonicalValue(receipt.expected_current_graph_revision.value())},
           {"retained_bundle_id",
            CanonicalValue(
                receipt.expected_current_retained_bundle_id.value())},
       })},
      {"destructive_impact", CanonicalValue(receipt.destructive_impact)},
      {"impact_digest", CanonicalValue(receipt.impact_digest.value())},
      {"manager_owner_epoch", CanonicalValue(receipt.manager_owner_epoch)},
      {"prepared_activation_id",
       CanonicalValue(receipt.prepared_activation_id.value())},
      {"schema_version",
       CanonicalValue("manyforge.manager_deployment_candidate_receipt.v2")},
  });
}

core::ContentDigest
DeploymentCandidateReceiptDigest(const DeploymentCandidateReceipt &receipt) {
  return core::ContentDigest(
      content_identity::sha256_digest(content_identity::canonical_json(
          DeploymentCandidateReceiptPayload(receipt))));
}

CanonicalValue
DeploymentCandidateReceiptValue(const DeploymentCandidateReceipt &receipt) {
  auto payload = std::get<CanonicalValue::Object>(
      DeploymentCandidateReceiptPayload(receipt).storage());
  payload.emplace_back("receipt_digest",
                       CanonicalValue(receipt.receipt_digest.value()));
  return CanonicalValue(std::move(payload));
}

DeploymentCandidateReceipt
RequiredDeploymentCandidateReceipt(const CanonicalValue &value) {
  RequireExactObjectSize(value, 12U, "deployment candidate receipt");
  Require(RequiredString(value, "schema_version") ==
              "manyforge.manager_deployment_candidate_receipt.v2",
          "deployment candidate receipt schema is unsupported");
  const auto &expected = RequiredMember(value, "expected_current");
  RequireExactObjectSize(expected, 2U, "deployment candidate expected current");
  DeploymentCandidateReceipt receipt{
      core::DeploymentArtifactId(
          RequiredString(value, "deployment_artifact_id")),
      core::DeploymentRevision(RequiredString(value, "deployment_revision")),
      core::ContentDigest(RequiredString(value, "prepared_activation_id")),
      core::RetainedBundleId(
          RequiredString(value, "candidate_retained_bundle_id")),
      core::GraphRevision(RequiredString(value, "candidate_graph_revision")),
      core::RetainedBundleId(RequiredString(expected, "retained_bundle_id")),
      core::GraphRevision(RequiredString(expected, "graph_revision")),
      core::AdmissionGeneration(RequiredUnsigned(
          RequiredMember(value, "expected_admission_generation"),
          "deployment candidate admission generation")),
      RequiredString(value, "manager_owner_epoch"),
      core::ContentDigest(RequiredString(value, "impact_digest")),
      RequiredBool(value, "destructive_impact"),
      core::ContentDigest(RequiredString(value, "receipt_digest")),
  };
  Require(
      core::IsExactLowercaseSha256Digest(receipt.impact_digest.value()) &&
      core::IsExactLowercaseSha256Digest(receipt.deployment_revision.value()) &&
          core::IsExactLowercaseSha256Digest(
              receipt.prepared_activation_id.value()) &&
          core::IsExactLowercaseSha256Digest(receipt.receipt_digest.value()) &&
          content_identity::digest_equal(
              receipt.receipt_digest.value(),
              DeploymentCandidateReceiptDigest(receipt).value()),
      "deployment candidate receipt identity is invalid");
  return receipt;
}

/// Decode the exact prepared activation accepted by one named Apply.
///
/// The Manager records these facts in the durable intent and never reopens a
/// mutable Scenario/catalog selection, draft or current-selection fact to
/// reconstruct the operation input.
component_manager::ManagerApplyPreparedActivationDTO RequiredPreparedActivation(
    const CanonicalValue &value) {
  // Development cutover: only the current prepared-activation version is
  // accepted; v3/v4 carried the removed launch-time parameter overrides.
  const auto schema = RequiredString(value, "schema_version");
  Require(schema == deployment::kPreparedActivationInputSchemaVersion,
          "Apply prepared activation schema is unsupported (only " +
              std::string(deployment::kPreparedActivationInputSchemaVersion) +
              "; launch-time Program parameter overrides were removed)");
  RequireExactObjectSize(value, 13U, "Apply prepared activation");
  const auto captured_input_digest =
      RequiredString(value, "captured_input_digest");
  const auto deployment_revision = RequiredString(value, "deployment_revision");
  const auto prepared_activation_id =
      RequiredString(value, "prepared_activation_id");
  Require(core::IsExactLowercaseSha256Digest(captured_input_digest),
          "Apply captured_input_digest must be canonical SHA-256");
  Require(core::IsExactLowercaseSha256Digest(deployment_revision),
          "Apply prepared activation deployment_revision must be canonical "
          "SHA-256");
  Require(core::IsExactLowercaseSha256Digest(prepared_activation_id),
          "Apply prepared_activation_id must be canonical SHA-256");
  // Exactly one typed Program outcome: the candidate binding, or the
  // named non-executable result for the preserved authored Program.
  const auto &program_outcome = RequiredMember(value, "program_outcome");
  std::optional<core::ProgramBindingId> program_binding_id;
  const auto outcome_kind = RequiredString(program_outcome, "kind");
  const bool no_program = outcome_kind == "no_program";
  if (no_program) {
    RequireExactObjectSize(program_outcome, 1U, "Apply Program outcome");
    Require(RequiredMember(value, "program_id").is_null() &&
                RequiredMember(value, "program_revision").is_null() &&
                RequiredString(value, "assistant_mode") == "optional",
            "no_program must have no Program identities");
  } else if (outcome_kind == "bound") {
    RequireExactObjectSize(program_outcome, 2U, "Apply Program outcome");
    const auto binding_id = RequiredString(program_outcome, "program_binding_id");
    Require(core::IsExactLowercaseSha256Digest(binding_id),
            "Apply program_binding_id must be canonical SHA-256");
    program_binding_id = core::ProgramBindingId(binding_id);
  } else {
    Require(outcome_kind == "non_executable",
            "Apply Program outcome kind is unsupported");
    RequireExactObjectSize(program_outcome, 2U, "Apply Program outcome");
    const auto outcome = [&]() {
      try {
        return deployment::DecodeProgramNonExecutableOutcome(
            content_identity::canonical_json(
                RequiredMember(program_outcome, "outcome")));
      } catch (const deployment::ProgramBindingOutcomeError &error) {
        Fail(std::string("Apply Program outcome is invalid: ") +
             error.what());
      }
    }();
    Require(outcome.program_id.value() == RequiredString(value, "program_id") &&
                outcome.program_revision.value() ==
                    RequiredString(value, "program_revision") &&
                outcome.retained_bundle_id.value() ==
                    RequiredString(value, "retained_bundle_id") &&
                outcome.graph_revision.value() ==
                    RequiredString(value, "graph_revision"),
            "Apply Program outcome differs from its prepared activation");
  }
  for (const auto field : {"retained_bundle_id",
                           "graph_revision",
                           "node_environment_digest"}) {
    Require(core::IsExactLowercaseSha256Digest(RequiredString(value, field)),
            "Apply prepared activation digest is not canonical");
  }
  const auto assistant_mode = RequiredString(value, "assistant_mode");
  Require(assistant_mode == "required" || assistant_mode == "optional" ||
              assistant_mode == "forbidden",
          "Apply prepared activation assistant_mode is unsupported");
  static_cast<void>(deployment::parse_prepared_activation_origin(
      RequiredMember(value, "origin")));
  if (!no_program) {
    static_cast<void>(RequiredString(value, "program_id"));
    Require(core::IsExactLowercaseSha256Digest(RequiredString(value, "program_revision")),
            "Apply Program revision is not canonical");
  }
  return {
      core::ContentDigest(captured_input_digest),
      core::DeploymentArtifactId(
          RequiredString(value, "deployment_artifact_id")),
      core::DeploymentRevision(deployment_revision),
      std::move(program_binding_id),
      core::ContentDigest(prepared_activation_id),
      content_identity::canonical_json(value),
  };
}

/// A new Apply is accepted only for a prepared value the preparer published
/// in the trusted activation store: its identity and content reverify, its
/// Program outcome recompiles to exactly the recorded binding or named
/// non-executable result, and the request summary equals it exactly.
/// A caller therefore cannot submit an outcome the preparer did not produce.
bool PreparedActivationIsStoredExactly(
    const std::filesystem::path &activation_root,
    const component_manager::ManagerApplyPreparedActivationDTO &activation) {
  if (activation_root.empty()) {
    return false;
  }
  const auto stored =
      deployment::FilesystemPreparedActivationRepository(activation_root)
          .get_verified(activation.prepared_activation_id);
  if (stored == nullptr) {
    return false;
  }
  CanonicalValue::Object summary;
  for (const auto &[name, value] : stored->normalized_root().as_object()) {
    if (name != "activation_payload") {
      summary.emplace_back(name, value);
    }
  }
  return content_identity::canonical_json(CanonicalValue(std::move(summary))) ==
         activation.canonical_receipt_json;
}

std::optional<std::string> OptionalString(const CanonicalValue &root,
                                          const std::string_view name) {
  const auto &member = RequiredMember(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto *value = std::get_if<std::string>(&member.storage());
  Require(value != nullptr && !value->empty() &&
              value->size() <= kMaximumCommandIdentityBytes,
          "command field " + std::string(name) +
              " must be null or one bounded non-empty string");
  return *value;
}

void RequireExactObjectSize(const CanonicalValue &value, const std::size_t size,
                            const std::string_view label) {
  Require(value.is_object() && value.as_object().size() == size,
          std::string(label) + " has an unknown or missing field");
}

std::uint64_t RequiredUnsigned(const CanonicalValue &value,
                               std::string_view label);
std::uint64_t RequiredNonnegativeUnsigned(const CanonicalValue &value,
                                          std::string_view label);

struct ProgramRunnerLivenessIdentity final {
  core::RuntimeUnitId runtime_unit_id;
  core::RuntimeEpoch runtime_epoch;
  core::ProgramCycleStatusDTO program_cycle_status;
};

struct ProgramRunnerLivenessRenewal final {
  core::ManagerLivenessObservationDTO observation;
  core::ProgramCycleStatusDTO program_cycle_status;
};

core::ProgramCycleStatusDTO
RequiredProgramCycleStatus(const CanonicalValue &value) {
  RequireExactObjectSize(value, 7U, "Program Runner cycle status");
  const auto state_name = RequiredString(value, "state");
  core::ProgramCycleStateDTO state;
  if (state_name == "idle_after_reset") {
    state = core::ProgramCycleStateDTO::kIdleAfterReset;
  } else if (state_name == "starting") {
    state = core::ProgramCycleStateDTO::kStarting;
  } else if (state_name == "running") {
    state = core::ProgramCycleStateDTO::kRunning;
  } else if (state_name == "stopping") {
    state = core::ProgramCycleStateDTO::kStopping;
  } else if (state_name == "terminal_awaiting_reset") {
    state = core::ProgramCycleStateDTO::kTerminalAwaitingReset;
  } else if (state_name == "resetting") {
    state = core::ProgramCycleStateDTO::kResetting;
  } else if (state_name == "recovery_required") {
    state = core::ProgramCycleStateDTO::kRecoveryRequired;
  } else if (state_name == "intervention_or_recovery") {
    state = core::ProgramCycleStateDTO::kInterventionOrRecovery;
  } else {
    Fail("Program Runner cycle status has an unsupported state");
  }
  const auto run_id = OptionalString(value, "program_run_id");
  const auto claim_generation =
      std::holds_alternative<std::nullptr_t>(
          RequiredMember(value, "cycle_claim_generation").storage())
          ? std::nullopt
          : std::optional<core::CellAdmissionClaimGeneration>(RequiredUnsigned(
                RequiredMember(value, "cycle_claim_generation"),
                "Program Runner cycle claim generation"));
  const auto terminal =
      OptionalString(value, "terminal_or_settlement_reference");
  core::ProgramCycleStatusDTO result{
      core::RequestOwnerEpoch(
          RequiredString(value, "program_runner_owner_epoch")),
      RequiredUnsigned(RequiredMember(value, "cycle_generation"),
                       "Program Runner cycle generation"),
      state,
      run_id.has_value()
          ? std::optional<core::ProgramRunId>(core::ProgramRunId(*run_id))
          : std::nullopt,
      claim_generation,
      terminal.has_value()
          ? std::optional<core::ContentDigest>(core::ContentDigest(*terminal))
          : std::nullopt};
  const auto& accepted_reset = RequiredMember(value, "accepted_reset");
  if (!std::holds_alternative<std::nullptr_t>(accepted_reset.storage())) {
    RequireExactObjectSize(accepted_reset, 6U, "accepted Program Reset");
    const auto& reset_claim = RequiredMember(accepted_reset, "reset_claim_generation");
    result.accepted_reset = core::AcceptedProgramResetDTO{
        core::RequestOwnerEpoch(RequiredString(accepted_reset, "request_owner_epoch")),
        core::RequestId(RequiredString(accepted_reset, "request_id")),
        core::ContentDigest(RequiredString(accepted_reset, "request_payload_digest")),
        RequiredBool(accepted_reset, "reload_initial_scene"),
        RequiredBool(accepted_reset, "reset_robot_position"),
        std::holds_alternative<std::nullptr_t>(reset_claim.storage())
            ? std::nullopt
            : std::optional<core::CellAdmissionClaimGeneration>(
                  core::CellAdmissionClaimGeneration(RequiredUnsigned(reset_claim, "Reset claim generation")))};
  }
  Require(core::ValidateProgramCycleStatus(result).ok(),
          "Program Runner cycle status is malformed");
  return result;
}

// One request and one reply over an exact same-user SEQPACKET socket.
CanonicalValue ExchangeLocalPacket(const std::filesystem::path &path,
                                   const CanonicalValue &request,
                                   const std::string_view peer_name,
                                   const content_identity::CanonicalJsonParseLimits
                                       &limits) {
  const std::string label(peer_name);
  const auto maximum_bytes = limits.maximum_bytes;
  const auto spelling = path.string();
  Require(!spelling.empty() && spelling.size() < sizeof(sockaddr_un::sun_path),
          label + " socket path is invalid");
  struct stat path_status {};
  Require(::lstat(path.c_str(), &path_status) == 0 &&
              S_ISSOCK(path_status.st_mode) &&
              path_status.st_uid == ::geteuid(),
          label + " socket is not an exact same-user socket");
  FileDescriptor connection(
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!connection) {
    FailErrno("create " + label + " connection");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
  if (::connect(connection.get(), reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0 &&
      errno != EINPROGRESS) {
    FailErrno("connect " + label + " socket");
  }
  pollfd ready{connection.get(), POLLOUT, 0};
  int polled = -1;
  do {
    polled = ::poll(&ready, 1, kClientReadTimeoutMilliseconds);
  } while (polled < 0 && errno == EINTR);
  Require(polled == 1 && (ready.revents & POLLOUT) != 0,
          label + " connection timed out");
  int socket_error = 0;
  socklen_t socket_error_size = sizeof(socket_error);
  Require(::getsockopt(connection.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                       &socket_error_size) == 0 &&
              socket_error == 0,
          label + " connection was rejected");
  struct ucred peer {};
  socklen_t peer_size = sizeof(peer);
  Require(::getsockopt(connection.get(), SOL_SOCKET, SO_PEERCRED, &peer,
                       &peer_size) == 0 &&
              peer_size == sizeof(peer) && peer.uid == ::geteuid(),
          label + " peer uid is foreign");

  const auto encoded = content_identity::canonical_json(request);
  Require(encoded.size() <= maximum_bytes,
          label + " request exceeds its byte bound");
  const auto sent =
      ::send(connection.get(), encoded.data(), encoded.size(), MSG_NOSIGNAL);
  Require(sent == static_cast<ssize_t>(encoded.size()),
          label + " request was not sent exactly");
  ready = pollfd{connection.get(), POLLIN, 0};
  do {
    polled = ::poll(&ready, 1, kClientReadTimeoutMilliseconds);
  } while (polled < 0 && errno == EINTR);
  Require(polled == 1 && (ready.revents & POLLIN) != 0,
          label + " acknowledgement timed out");
  std::vector<char> response(maximum_bytes + 1U);
  iovec vector{response.data(), response.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  const auto received = ::recvmsg(connection.get(), &message, 0);
  Require(received > 0 &&
              received <= static_cast<ssize_t>(
                              maximum_bytes) &&
              (message.msg_flags & MSG_TRUNC) == 0,
          label + " acknowledgement is empty or oversized");
  const std::string_view bytes(response.data(),
                               static_cast<std::size_t>(received));
  CanonicalValue decoded;
  try {
    decoded = content_identity::parse_canonical_json(
        bytes, limits);
  } catch (const std::exception &error) {
    Fail(label + " acknowledgement is invalid: " +
         std::string(error.what()));
  }
  return decoded;
}

CanonicalValue
ExchangeProgramRunnerLivenessPacket(const std::filesystem::path &path,
                                    const CanonicalValue &request) {
  return ExchangeLocalPacket(
      path, request, "Program Runner liveness",
      {kMaximumProgramRunnerLivenessPacketBytes, 4U, 64U});
}

ProgramRunnerLivenessIdentity
ProbeProgramRunnerLiveness(const std::filesystem::path &path,
                           const core::CellId &cell_id) {
  const auto response = ExchangeProgramRunnerLivenessPacket(
      path, CanonicalValue(CanonicalValue::Object{
                {"api_version",
                 CanonicalValue(std::string(kProgramRunnerLivenessApiVersion))},
                {"cell_id", CanonicalValue(cell_id.value())},
                {"kind", CanonicalValue("probe")},
            }));
  RequireExactObjectSize(response, 10U, "Program Runner liveness probe");
  Require(RequiredString(response, "api_version") ==
                  kProgramRunnerLivenessApiVersion &&
              RequiredString(response, "kind") == "probe_ack" &&
              RequiredString(response, "cell_id") == cell_id.value() &&
              RequiredString(response, "runtime_policy_id") ==
                  kProgramRunnerLivenessPolicyId &&
              RequiredString(response, "runtime_policy_revision") ==
                  kProgramRunnerLivenessPolicyRevision &&
              RequiredString(response, "local_safe_state_contract_id") ==
                  kProgramRunnerLocalSafeStateContractId &&
              RequiredString(response, "local_safe_state_contract_revision") ==
                  kProgramRunnerLocalSafeStateContractRevision,
          "Program Runner liveness probe returned a foreign identity");
  return {
      core::RuntimeUnitId(RequiredString(response, "receiver_runtime_unit_id")),
      core::RuntimeEpoch(RequiredString(response, "receiver_runtime_epoch")),
      RequiredProgramCycleStatus(
          RequiredMember(response, "program_cycle_status"))};
}

ProgramRunnerLivenessRenewal RenewProgramRunnerLiveness(
    const std::filesystem::path &path,
    const ProgramRunnerLivenessIdentity &receiver,
    const core::ManagerLivenessRequiredSetDTO &required_set,
    const std::uint64_t sequence,
    const std::optional<core::ContentDigest> &reconciliation_digest) {
  const auto response = ExchangeProgramRunnerLivenessPacket(
      path,
      CanonicalValue(CanonicalValue::Object{
          {"active_bundle_id",
           CanonicalValue(required_set.active_bundle_id.value())},
          {"api_version",
           CanonicalValue(std::string(kProgramRunnerLivenessApiVersion))},
          {"cell_id", CanonicalValue(required_set.cell_id.value())},
          {"heartbeat_sequence", CanonicalValue(static_cast<double>(sequence))},
          {"kind", CanonicalValue("heartbeat")},
          {"manager_owner_epoch",
           CanonicalValue(required_set.manager_owner_epoch.value())},
          {"manager_runtime_epoch",
           CanonicalValue(required_set.manager_runtime_epoch.value())},
          {"manager_runtime_unit_id",
           CanonicalValue(required_set.manager_runtime_unit_id.value())},
          {"receiver_runtime_epoch",
           CanonicalValue(receiver.runtime_epoch.value())},
          {"receiver_runtime_unit_id",
           CanonicalValue(receiver.runtime_unit_id.value())},
          {"reconciliation_digest",
           reconciliation_digest.has_value()
               ? CanonicalValue(reconciliation_digest->value())
               : CanonicalValue(nullptr)},
          {"required_runtime_set_digest",
           CanonicalValue(required_set.required_runtime_set_digest.value())},
          {"runtime_policy_id",
           CanonicalValue(std::string(kProgramRunnerLivenessPolicyId))},
          {"runtime_policy_revision",
           CanonicalValue(std::string(kProgramRunnerLivenessPolicyRevision))},
      }));
  RequireExactObjectSize(response, 24U, "Program Runner heartbeat ack");
  const auto local_age = RequiredNonnegativeUnsigned(
      RequiredMember(response, "local_heartbeat_age_ns"),
      "Program Runner local heartbeat age");
  const auto receiver_status_sequence =
      RequiredUnsigned(RequiredMember(response, "receiver_status_sequence"),
                       "Program Runner receiver status sequence");
  const auto fault_reference = OptionalString(response, "fault_reference");
  const auto state = RequiredString(response, "state");
  const auto fenced = RequiredBool(response, "command_fenced");
  const auto current = state == "current" && !fenced;
  Require(RequiredString(response, "api_version") ==
                  kProgramRunnerLivenessApiVersion &&
              RequiredString(response, "schema_version") ==
                  "manyforge.program_runner_liveness_status.v1" &&
              RequiredString(response, "kind") == "heartbeat_ack" &&
              RequiredString(response, "cell_id") ==
                  required_set.cell_id.value() &&
              RequiredString(response, "active_bundle_id") ==
                  required_set.active_bundle_id.value() &&
              RequiredString(response, "required_runtime_set_digest") ==
                  required_set.required_runtime_set_digest.value() &&
              RequiredString(response, "receiver_runtime_unit_id") ==
                  receiver.runtime_unit_id.value() &&
              RequiredString(response, "receiver_runtime_epoch") ==
                  receiver.runtime_epoch.value() &&
              RequiredString(response, "manager_runtime_epoch") ==
                  required_set.manager_runtime_epoch.value() &&
              RequiredString(response, "manager_runtime_unit_id") ==
                  required_set.manager_runtime_unit_id.value() &&
              RequiredString(response, "manager_owner_epoch") ==
                  required_set.manager_owner_epoch.value() &&
              RequiredString(response, "runtime_policy_id") ==
                  kProgramRunnerLivenessPolicyId &&
              RequiredString(response, "runtime_policy_revision") ==
                  kProgramRunnerLivenessPolicyRevision &&
              RequiredString(response, "local_safe_state_contract_id") ==
                  kProgramRunnerLocalSafeStateContractId &&
              RequiredString(response, "local_safe_state_contract_revision") ==
                  kProgramRunnerLocalSafeStateContractRevision &&
              RequiredUnsigned(
                  RequiredMember(response, "last_accepted_heartbeat_sequence"),
                  "Program Runner heartbeat sequence") == sequence &&
              (!fault_reference.has_value() ||
               (fault_reference->starts_with("sha256:") &&
                fault_reference->size() == 71U)),
          "Program Runner liveness acknowledgement is not exact");
  const auto parsed_state = detail::DecodeProgramRunnerManagerState(state);
  core::ManagerLivenessObservationDTO observation{
      {required_set.cell_id, receiver.runtime_unit_id, receiver.runtime_epoch,
       required_set.manager_runtime_unit_id, required_set.manager_runtime_epoch,
       required_set.manager_owner_epoch,
       core::RuntimePolicyId(std::string(kProgramRunnerLivenessPolicyId)),
       core::RuntimePolicyRevision(
           std::string(kProgramRunnerLivenessPolicyRevision)),
       sequence, receiver_status_sequence, static_cast<std::int64_t>(local_age),
       parsed_state, fenced,
       fault_reference.has_value() ? std::optional<core::ContentDigest>(
                                         core::ContentDigest(*fault_reference))
                                   : std::nullopt},
      required_set.active_bundle_id,
      required_set.required_runtime_set_digest,
      core::ContentDigest(RequiredString(response, "receiver_status_digest"))};
  Require(core::ValidateManagerLivenessObservation(observation).ok(),
          "Program Runner liveness observation is malformed");
  const auto cycle = RequiredProgramCycleStatus(
      RequiredMember(response, "program_cycle_status"));
  detail::CheckProgramRunnerPublicationHealth(
      RequiredMember(response, "publisher_fault"), cycle,
      receiver.runtime_epoch, fenced, fault_reference);
  Require(current || (!reconciliation_digest.has_value() &&
                      state == "unreconciled" && fenced),
          "Program Runner liveness acknowledgement is not exact");
  return {std::move(observation), cycle};
}

std::uint64_t RequiredUnsigned(const CanonicalValue &value,
                               const std::string_view label) {
  constexpr auto kMaximumSafeInteger = 9007199254740991ULL;
  const auto *number = std::get_if<double>(&value.storage());
  Require(number != nullptr && *number >= 1.0 &&
              *number <= static_cast<double>(kMaximumSafeInteger),
          std::string(label) + " must be one positive safe integer");
  const auto result = static_cast<std::uint64_t>(*number);
  Require(static_cast<double>(result) == *number,
          std::string(label) + " must be integral");
  return result;
}

std::uint64_t RequiredNonnegativeUnsigned(const CanonicalValue &value,
                                          const std::string_view label) {
  constexpr auto kMaximumSafeInteger = 9007199254740991ULL;
  const auto *number = std::get_if<double>(&value.storage());
  Require(number != nullptr && *number >= 0.0 &&
              *number <= static_cast<double>(kMaximumSafeInteger),
          std::string(label) + " must be one nonnegative safe integer");
  const auto result = static_cast<std::uint64_t>(*number);
  Require(static_cast<double>(result) == *number,
          std::string(label) + " must be integral");
  return result;
}

core::CellAdmissionClaimDTO
RequiredParentAuthoringClaim(const CanonicalValue &value) {
  RequireExactObjectSize(value, 4U, "parent authoring claim");
  const auto &intent_value = RequiredMember(value, "intent");
  RequireExactObjectSize(intent_value, 10U, "parent authoring claim intent");
  Require(RequiredString(intent_value, "claim_kind") == "authoring_mutation" &&
              RequiredString(intent_value, "owner_role") == "authoring_owner",
          "parent claim is not an authoring-owner mutation claim");
  const auto &program_cycle =
      RequiredMember(intent_value, "program_cycle_evidence");
  const auto &intervention =
      RequiredMember(intent_value, "intervention_evidence");
  Require(program_cycle.is_null() && intervention.is_null(),
          "parent authoring claim carries unsupported nested evidence");
  const auto &scope = RequiredMember(intent_value, "scope");
  RequireExactObjectSize(scope, 2U, "parent authoring claim scope");
  const auto *resources = std::get_if<CanonicalValue::Array>(
      &RequiredMember(scope, "resources").storage());
  const auto *configurations = std::get_if<CanonicalValue::Array>(
      &RequiredMember(scope, "configurations").storage());
  Require(resources != nullptr && resources->empty() &&
              configurations != nullptr && configurations->empty(),
          "parent authoring claim must have the cell-global empty scope");
  const auto request_payload_digest =
      RequiredString(intent_value, "request_payload_digest");
  Require(core::IsExactLowercaseSha256Digest(request_payload_digest),
          "parent authoring claim payload digest is invalid");
  // Decode in the original order before constructing the nested claim, so
  // malformed later fields unwind only independent owning locals.
  auto owner =
      core::RequestOwnerEpoch(RequiredString(intent_value, "owner_epoch"));
  auto transaction = RequiredString(intent_value, "owner_transaction_id");
  auto request = core::RequestId(RequiredString(intent_value, "request_id"));
  auto payload_digest = core::ContentDigest(request_payload_digest);
  const auto expected_generation = core::AdmissionGeneration(RequiredUnsigned(
      RequiredMember(intent_value, "expected_admission_generation"),
      "parent expected_admission_generation"));
  const auto claim_generation = core::CellAdmissionClaimGeneration(
      RequiredUnsigned(RequiredMember(value, "claim_generation"),
                       "parent claim_generation"));
  const auto acquired_generation = core::AdmissionGeneration(
      RequiredUnsigned(RequiredMember(value, "acquired_admission_generation"),
                       "parent acquired_admission_generation"));
  const auto accepted_at = manyforge::content_identity::parse_nonnegative_int64(
      RequiredMember(value, "accepted_at_ns"));
  core::CellAdmissionClaimDTO claim{
      {
          core::CellAdmissionClaimKindDTO::kAuthoringMutation,
          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          std::move(owner),
          std::move(transaction),
          std::move(request),
          std::move(payload_digest),
          expected_generation,
          {},
          std::nullopt,
          std::nullopt,
      },
      claim_generation,
      acquired_generation,
      accepted_at,
  };
  Require(core::ValidateCellAdmissionClaimIntent(claim.intent).ok(),
          "parent authoring claim is invalid");
  return claim;
}

SceneRuntimeIdentity RequiredSceneRuntimeIdentity(const CanonicalValue &value) {
  RequireExactObjectSize(value, 5U, "scene-runtime identity");
  SceneRuntimeIdentity result{
      RequiredString(value, "retained_bundle_id"),
      RequiredString(value, "graph_revision"),
      RequiredNonnegativeUnsigned(
          RequiredMember(value, "deployment_generation"),
          "deployment_generation"),
      RequiredNonnegativeUnsigned(RequiredMember(value, "program_generation"),
                                  "program_generation"),
      RequiredUnsigned(RequiredMember(value, "runtime_generation"),
                       "runtime_generation"),
  };
  Require(result.valid(), "scene-runtime identity is invalid");
  return result;
}

core::ProjectionRevisionDTO
RequiredProjectionRevision(const CanonicalValue &value) {
  RequireExactObjectSize(value, 3U, "projection revision");
  const auto retained_bundle_id = RequiredString(value, "retained_bundle_id");
  const auto graph_revision = RequiredString(value, "graph_revision");
  Require(core::IsExactLowercaseSha256Digest(retained_bundle_id) &&
              core::IsExactLowercaseSha256Digest(graph_revision),
          "projection structural identities must be canonical SHA-256");
  const auto &scene = RequiredMember(value, "scene_revision");
  RequireExactObjectSize(scene, 3U, "scene revision");
  const auto scene_id = RequiredString(scene, "scene_id");
  const auto content_hash = RequiredString(scene, "content_hash");
  Require(core::IsExactLowercaseSha256Digest(scene_id) &&
              core::IsExactLowercaseSha256Digest(content_hash),
          "scene identities must be canonical SHA-256");
  core::ProjectionRevisionDTO result{
      core::RetainedBundleId(retained_bundle_id),
      core::GraphRevision(graph_revision),
      core::SceneRevision{
          core::SceneId(scene_id),
          RequiredUnsigned(RequiredMember(scene, "revision_id"),
                           "scene revision_id"),
          core::ContentDigest(content_hash),
      },
  };
  Require(result.valid(), "projection revision is invalid");
  return result;
}

std::vector<core::SceneResourceId>
RequiredSceneResourceIds(const CanonicalValue &value) {
  const auto *rows = std::get_if<CanonicalValue::Array>(&value.storage());
  Require(rows != nullptr && !rows->empty() &&
              rows->size() <= kMaximumScenePoseQueryResources,
          "scene_resource_ids must be one bounded non-empty array");
  std::vector<core::SceneResourceId> result;
  result.reserve(rows->size());
  std::string previous;
  for (const auto &row : *rows) {
    const auto *identity = std::get_if<std::string>(&row.storage());
    Require(identity != nullptr && !identity->empty() &&
                identity->size() <= kMaximumCommandIdentityBytes &&
                std::all_of(identity->begin(), identity->end(),
                            [](const char character) {
                              const auto byte =
                                  static_cast<unsigned char>(character);
                              return byte >= 0x21U && byte <= 0x7EU;
                            }) &&
                (previous.empty() || previous < *identity),
            "scene_resource_ids must be canonical bounded identities");
    previous = *identity;
    result.emplace_back(*identity);
  }
  return result;
}

std::vector<PoseObserverBinding>
RequiredPoseObserverBindings(const CanonicalValue &value) {
  const auto *rows = std::get_if<CanonicalValue::Array>(&value.storage());
  Require(rows != nullptr && !rows->empty() &&
              rows->size() <= kMaximumPoseObserverSessions,
          "pose observer bindings must be one bounded non-empty array");
  std::vector<PoseObserverBinding> result;
  result.reserve(rows->size());
  std::optional<std::string> writable_object_id;
  for (const auto &row : *rows) {
    RequireExactObjectSize(row, 3U, "pose observer binding");
    PoseObserverBinding binding{RequiredString(row, "object_id"),
                                RequiredString(row, "policy_id"),
                                RequiredBool(row, "eligible")};
    if (binding.eligible) {
      // Object links observe as "scene"; the application policy is the
      // Deployment link row (EffectivePosePolicy).
      Require(binding.policy_id == "scene",
              "eligible pose observer binding uses an unsupported policy");
      Require(!writable_object_id.has_value() ||
                  *writable_object_id != binding.object_id,
              "pose observer bindings declare more than one writable policy "
              "for one object");
      writable_object_id = binding.object_id;
    }
    Require(result.empty() || result.back() < binding,
            "pose observer bindings must be unique and canonically ordered");
    result.push_back(std::move(binding));
  }
  return result;
}

component_manager::ManagerComponentProcessStopRequestDTO
RequiredComponentProcessStop(const CanonicalValue &value) {
  RequireExactObjectSize(value, 10U, "component process Stop request");
  const auto &context = RequiredMember(value, "request_context");
  const auto *correlation = context.find_object_member("correlation_id");
  RequireExactObjectSize(context, correlation ? 4U : 3U, "component Stop request context");
  core::PublicRequestContextDTO request_context{
      core::RequestId(RequiredString(context, "request_id")),
      core::RequestOwnerEpoch(RequiredString(context, "request_owner_epoch")),
      RequiredString(context, "source_label"), std::nullopt};
  if (correlation) request_context.correlation_id = RequiredString(context, "correlation_id");
  const auto pid = RequiredUnsigned(RequiredMember(value, "leader_pid"), "leader PID");
  Require(pid <= static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()),
          "component Stop leader PID exceeds int32");
  component_manager::ManagerComponentProcessStopRequestDTO result{
      std::move(request_context), RequiredProjectionRevision(RequiredMember(value, "predecessor_projection_revision")),
      core::ActiveBundlePublicationGeneration(RequiredUnsigned(
          RequiredMember(value, "expected_predecessor_publication_generation"), "predecessor publication generation")),
      core::ContentDigest(RequiredString(value, "expected_predecessor_public_pointer_digest")),
      core::RuntimeUnitId(RequiredString(value, "runtime_unit_id")),
      core::RuntimeEpoch(RequiredString(value, "runtime_epoch")),
      core::RuntimeEpoch(RequiredString(value, "manager_runtime_epoch")),
      core::ControllableComponentId(RequiredString(value, "controllable_component_id")),
      RequiredString(value, "launch_identity",
                     core::kMaximumCellAdmissionPrivateBinaryBytes),
      static_cast<std::int32_t>(pid)};
  Require(result.valid(), "component process Stop request is invalid");
  return result;
}

ManagerCommand DecodeCommand(const std::string_view bytes) {
  Require(!bytes.empty() && bytes.size() <= kMaximumCommandBytes,
          "command packet is empty or exceeds its byte bound");
  CanonicalValue root;
  try {
    root = deployment::parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception &error) {
    Fail("command is not a strict bounded JSON value: " +
         std::string(error.what()));
  }
  Require(content_identity::canonical_json(root) == bytes,
          "command must use exact canonical JSON encoding");
  Require(root.is_object(), "command root must be an object");
  Require(RequiredString(root, "api_version") == kManagerCommandApiVersion,
          "command api_version is unsupported");
  const auto command = RequiredString(root, "command");
  if (command == "component_process_stop_offer") {
    RequireExactObjectSize(root, 3U, "component process Stop offer command");
    ManagerCommand result{.kind = CommandKind::kComponentProcessStopOffer};
    result.process_stop_offer_component = core::ControllableComponentId(
        RequiredString(root, "controllable_component_id"));
    Require(result.process_stop_offer_component->value().size() <= core::kMaxLifecycleIdentityBytes,
            "component Stop offer identity is too long");
    return result;
  }
  if (command == "stop_component_process" || command == "component_process_stop_result") {
    RequireExactObjectSize(root, 3U, "component process Stop command");
    ManagerCommand result{.kind = command == "stop_component_process"
        ? CommandKind::kStopComponentProcess : CommandKind::kComponentProcessStopResult};
    result.component_process_stop = RequiredComponentProcessStop(RequiredMember(root, "request"));
    return result;
  }
  if (command == "deployment_candidate_preview") {
    RequireExactObjectSize(root, 10U, "deployment candidate preview command");
    const auto deployment_revision =
        RequiredString(root, "deployment_revision");
    Require(core::IsExactLowercaseSha256Digest(deployment_revision),
            "candidate deployment_revision must be canonical SHA-256");
    const auto &expected = RequiredMember(root, "expected_current");
    RequireExactObjectSize(expected, 2U, "candidate preview expected_current");
    ManagerCommand result{
        .kind = CommandKind::kDeploymentCandidatePreview,
        .request_owner_epoch = RequiredString(root, "request_owner_epoch"),
        .deployment_artifact_id = core::DeploymentArtifactId(
            RequiredString(root, "deployment_artifact_id")),
        .deployment_revision = core::DeploymentRevision(deployment_revision),
        .expected_current_retained_bundle_id = core::RetainedBundleId(
            RequiredString(expected, "retained_bundle_id")),
        .expected_current_graph_revision =
            core::GraphRevision(RequiredString(expected, "graph_revision")),
        .expected_admission_generation =
            core::AdmissionGeneration(RequiredUnsigned(
                RequiredMember(root, "expected_admission_generation"),
                "candidate expected_admission_generation")),
        .candidate_retained_bundle_id = core::RetainedBundleId(
            RequiredString(root, "candidate_retained_bundle_id")),
        .candidate_graph_revision = core::GraphRevision(
            RequiredString(root, "candidate_graph_revision")),
        .prepared_activation_id = core::ContentDigest(
            RequiredString(root, "prepared_activation_id")),
    };
    Require(!result.candidate_retained_bundle_id->empty() &&
                !result.candidate_graph_revision->empty() &&
                core::IsExactLowercaseSha256Digest(
                    result.prepared_activation_id->value()),
            "candidate preview prepared activation identity is invalid");
    return result;
  }
  if (command == "reconcile") {
    RequireExactObjectSize(root, 2U, "reconcile command");
    return {CommandKind::kReconcile,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
  }
  if (command == "shutdown_runtime") {
    RequireExactObjectSize(root, 2U, "shutdown command");
    return {CommandKind::kShutdownRuntime,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt};
  }
  if (command == "scene_commit" || command == "scene_transaction_status") {
    RequireExactObjectSize(root, 3U, "scene commit command");
    auto transaction = scene::DecodeCanonicalSceneTransactionRequest(
        content_identity::canonical_json(RequiredMember(root, "transaction")));
    return {
        command == "scene_commit" ? CommandKind::kSceneCommit
                                  : CommandKind::kSceneTransactionStatus,
        transaction.request_context.request_id.value(),
        transaction.request_context.request_owner_epoch.value(),
        transaction.request_context.source_label,
        transaction.request_context.correlation_id,
        std::move(transaction),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
    };
  }
  if (command == "scene_owner_read") {
    RequireExactObjectSize(root, 2U, "scene owner read command");
    return ManagerCommand{.kind = CommandKind::kSceneOwnerRead};
  }
  if (command == "scene_candidate_preview") {
    RequireExactObjectSize(root, 3U, "scene candidate preview command");
    ManagerCommand result{.kind = CommandKind::kSceneCandidatePreview};
    result.scene_candidate_preview =
        scene::DecodeResolvedSceneCandidatePreviewRequest(
            content_identity::canonical_json(
                RequiredMember(root, "candidate_request")));
    return result;
  }
  if (command == "scene_pose_query") {
    RequireExactObjectSize(root, 4U, "scene pose query command");
    ManagerCommand result{.kind = CommandKind::kScenePoseQuery};
    result.expected_scene_projection = RequiredProjectionRevision(
        RequiredMember(root, "expected_projection_revision"));
    result.scene_resource_ids =
        RequiredSceneResourceIds(RequiredMember(root, "scene_resource_ids"));
    return result;
  }
  if (command == "scene_runtime_owner_read") {
    RequireExactObjectSize(root, 2U, "scene-runtime owner read command");
    return ManagerCommand{.kind = CommandKind::kSceneRuntimeOwnerRead};
  }
  if (command == "scene_runtime_observer_open") {
    RequireExactObjectSize(root, 6U, "scene-runtime observer open command");
    ManagerCommand result{.kind = CommandKind::kSceneRuntimeObserverOpen};
    result.pose_observer_bindings =
        RequiredPoseObserverBindings(RequiredMember(root, "bindings"));
    result.expected_scene_runtime_identity = RequiredSceneRuntimeIdentity(
        RequiredMember(root, "expected_runtime_identity"));
    result.pose_source_epoch = RequiredString(root, "source_epoch");
    result.pose_source_id = RequiredString(root, "source_id");
    return result;
  }
  if (command == "scene_runtime_observe") {
    RequireExactObjectSize(root, 12U, "scene-runtime observe command");
    ManagerCommand result{.kind = CommandKind::kSceneRuntimeObserve};
    result.expected_scene_runtime_identity = RequiredSceneRuntimeIdentity(
        RequiredMember(root, "expected_runtime_identity"));
    result.pose_source_id = RequiredString(root, "source_id");
    result.pose_source_epoch = RequiredString(root, "source_epoch");
    result.pose_source_sequence = RequiredUnsigned(
        RequiredMember(root, "source_sequence"), "source_sequence");
    result.pose_object_id = RequiredString(root, "object_id");
    result.pose_policy_id = RequiredString(root, "policy_id");
    result.pose_lifetime_ns =
        RequiredUnsigned(RequiredMember(root, "lifetime_ns"), "lifetime_ns");
    Require(*result.pose_lifetime_ns <= kMaximumPoseObservationLifetimeNs,
            "lifetime_ns exceeds the scene-runtime observation bound");
    const auto quality = RequiredString(root, "payload_quality");
    Require(quality == "good" || quality == "unknown",
            "payload_quality must be good or unknown");
    result.pose_payload_quality = quality == "good" ? PoseSourceQuality::kGood
                                            : PoseSourceQuality::kUnknown;
    result.pose_eligible = RequiredBool(root, "eligible");
    const auto &payload = RequiredMember(root, "payload");
    Require(payload.is_object(), "pose observation payload must be an object");
    result.pose_payload = payload;
    return result;
  }
  if (command == "scene_runtime_latch_before_planning" ||
      command == "scene_runtime_latch_between_nodes") {
    const bool between_nodes = command == "scene_runtime_latch_between_nodes";
    RequireExactObjectSize(
        root,
        between_nodes && root.find_object_member("scene_resource_ids") ? 4U : 3U,
        "scene-runtime before-planning latch command");
    ManagerCommand result{
        .kind = between_nodes ? CommandKind::kSceneRuntimeLatchBetweenNodes
                              : CommandKind::kSceneRuntimeLatchBeforePlanning};
    if (const auto *ids = root.find_object_member("scene_resource_ids"))
      result.scene_resource_ids = RequiredSceneResourceIds(*ids);
    result.expected_scene_runtime_identity = RequiredSceneRuntimeIdentity(
        RequiredMember(root, "expected_runtime_identity"));
    return result;
  }
  if (command == "apply_status" || command == "apply_acknowledge" ||
      command == "apply_release_for_new") {
    const bool acknowledging = command != "apply_status";
    const bool for_new = command == "apply_release_for_new";
    try {
      RequireExactObjectSize(root, for_new ? 3U : acknowledging ? 5U : 4U,
                             "Apply status command");
      ManagerCommand result{.kind = command == "apply_status" ? CommandKind::kApplyStatus
                                    : for_new ? CommandKind::kApplyReleaseForNew
                                              : CommandKind::kApplyAcknowledge};
      if (!for_new) {
        result.apply_id = core::ApplyId(RequiredString(root, "apply_id"));
        result.apply_generation = core::ApplyGeneration(RequiredUnsigned(
            RequiredMember(root, "apply_generation"), "apply_generation"));
      }
      if (acknowledging) {
        const auto &context = RequiredMember(root, "request_context");
        const auto *correlation = context.find_object_member("correlation_id");
        const auto *epoch = context.find_object_member("request_owner_epoch");
        RequireExactObjectSize(context, 2U + (correlation ? 1U : 0U) + (epoch ? 1U : 0U),
                               "Apply acknowledgement request context");
        result.request_id = RequiredString(context, "request_id");
        result.source_label = RequiredString(context, "source_label");
        // Accepted for the standard wire context. Only ID/source are retained
        // as audit labels; neither optional member grants authority here.
        if (epoch) result.request_owner_epoch = RequiredString(context, "request_owner_epoch");
        if (correlation) result.correlation_id = RequiredString(context, "correlation_id");
      }
      return result;
    } catch (const std::exception &error) {
      if (!acknowledging) throw;
      auto cause = core::CauseOf(error, "invalid_request", kManagerProducer);
      if (dynamic_cast<const core::TypedError *>(&error) == nullptr) {
        cause.error.details["command"] = "invalid";
      }
      cause.error.details["effect"] = "none";
      throw core::TypedError(cause.error, std::move(cause.producer));
    }
  }
  Require(command == "apply", "command kind is unsupported");
  RequireExactObjectSize(root, 15U, "Apply command");
  const auto request_id = RequiredString(root, "request_id");
  const auto request_owner_epoch = RequiredString(root, "request_owner_epoch");
  const auto source_label = RequiredString(root, "source_label");
  const auto correlation_id = OptionalString(root, "correlation_id");
  const auto deployment_artifact_id =
      RequiredString(root, "deployment_artifact_id");
  const auto deployment_revision = RequiredString(root, "deployment_revision");
  Require(core::IsExactLowercaseSha256Digest(deployment_revision),
          "Apply deployment_revision must be canonical SHA-256");
  const auto &expected = RequiredMember(root, "expected_current");
  RequireExactObjectSize(expected, 2U, "Apply expected_current");
  const auto expected_graph_revision =
      RequiredString(expected, "graph_revision");
  const auto expected_retained_bundle_id =
      RequiredString(expected, "retained_bundle_id");
  Require(RequiredBool(root, "reject_if_affected_busy"),
          "Apply busy disposition must be reject_if_affected_busy");
  const auto &admission_context = RequiredMember(root, "admission_context");
  const auto admission_kind = RequiredString(admission_context, "kind");
  ManagerCommand result{
      CommandKind::kApply, request_id,   request_owner_epoch, source_label,
      correlation_id,      std::nullopt, std::nullopt,        std::nullopt,
      std::nullopt,        std::nullopt};
  result.deployment_artifact_id =
      core::DeploymentArtifactId(deployment_artifact_id);
  result.deployment_revision = core::DeploymentRevision(deployment_revision);
  // Explicit acceptance of a destructive preview impact; null when
  // the caller accepts none.
  if (const auto accepted = OptionalString(root, "accepted_impact_digest")) {
    Require(core::IsExactLowercaseSha256Digest(*accepted),
            "Apply accepted_impact_digest must be canonical SHA-256");
    result.accepted_impact_digest = core::ContentDigest(*accepted);
  }
  result.deployment_candidate_receipt = RequiredDeploymentCandidateReceipt(
      RequiredMember(root, "candidate_receipt"));
  result.prepared_activation =
      RequiredPreparedActivation(RequiredMember(root, "prepared_activation"));
  const auto initialization = RequiredString(root, "scene_initialization");
  Require(initialization == "preserve" || initialization == "initialize",
          "Apply Scene initialization purpose is unsupported");
  result.scene_initialization =
      initialization == "initialize"
          ? component_manager::ManagerSceneInitialization::kInitialize
          : component_manager::ManagerSceneInitialization::kPreserve;
  result.expected_current_retained_bundle_id =
      core::RetainedBundleId(expected_retained_bundle_id);
  result.expected_current_graph_revision =
      core::GraphRevision(expected_graph_revision);
  if (admission_kind == "direct") {
    RequireExactObjectSize(admission_context, 2U, "direct admission context");
    result.expected_admission_generation =
        core::AdmissionGeneration(RequiredUnsigned(
            RequiredMember(admission_context, "expected_admission_generation"),
            "expected_admission_generation"));
  } else {
    Require(admission_kind == "parent_authoring_claim",
            "Apply admission context kind is unsupported");
    RequireExactObjectSize(admission_context, 2U, "parent admission context");
    result.parent_authoring_claim = RequiredParentAuthoringClaim(
        RequiredMember(admission_context, "claim"));
  }
  return result;
}

CanonicalValue Unsigned(const std::uint64_t value,
                        const std::string_view label) {
  constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
  Require(value <= kMaximumSafeInteger,
          std::string(label) + " exceeds the I-JSON safe-integral range");
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue ProjectionRevision(const core::ProjectionRevisionDTO &value) {
  Require(value.valid(), "scene response projection revision is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"graph_revision", CanonicalValue(value.graph_revision.value())},
      {"retained_bundle_id", CanonicalValue(value.retained_bundle_id.value())},
      {"scene_revision",
       CanonicalValue(CanonicalValue::Object{
           {"content_hash",
            CanonicalValue(value.scene_revision.content_hash.value())},
           {"revision_id",
            Unsigned(value.scene_revision.revision_id, "scene revision")},
           {"scene_id", CanonicalValue(value.scene_revision.scene_id.value())},
       })},
  });
}

CanonicalValue Pose(const core::Pose3DTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"orientation_xyzw", CanonicalValue(CanonicalValue::Array{
                               CanonicalValue(value.orientation.x),
                               CanonicalValue(value.orientation.y),
                               CanonicalValue(value.orientation.z),
                               CanonicalValue(value.orientation.w),
                           })},
      {"position_m", CanonicalValue(CanonicalValue::Object{
                         {"x", CanonicalValue(value.position_m.x_m)},
                         {"y", CanonicalValue(value.position_m.y_m)},
                         {"z", CanonicalValue(value.position_m.z_m)},
                     })},
  });
}

CanonicalValue SceneRuntimeFrame(
    const core::ComponentSceneFrameRefDTO &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id",
       CanonicalValue(value.controllable_component_id.value())},
      {"frame_role_id", CanonicalValue(value.frame_role_id.value())},
      {"kind", CanonicalValue("component")},
      {"model_instance_id", CanonicalValue(value.model_instance_id.value())},
  });
}

CanonicalValue SceneRuntimePoseDistanceValue(
    const SceneRuntimePoseDistance &value) {
  return CanonicalValue(CanonicalValue::Object{
      {"rotation_rad", CanonicalValue(value.rotation_rad)},
      {"translation_m", CanonicalValue(value.translation_m)},
  });
}

void AppendSceneRuntimePoseMeasurement(
    CanonicalValue::Object &row,
    const SceneRuntimePoseMeasurement &measurement) {
  if (measurement.holder) {
    row.emplace_back(
        "holder",
        CanonicalValue(CanonicalValue::Object{
            {"command_target_id",
             CanonicalValue(measurement.holder->command_target_id.value())},
            {"controllable_component_id",
             CanonicalValue(
                 measurement.holder->controllable_component_id.value())},
            {"gripper_link", CanonicalValue(measurement.holder->gripper_link)},
        }));
  }
  const auto measured_against = std::visit(
      [](const auto &value) -> CanonicalValue {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return CanonicalValue(value);
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"entity", CanonicalValue(value.entity)},
              {"link", CanonicalValue(value.link)},
          });
        }
      },
      measurement.measured_against);
  row.emplace_back("measured_against", measured_against);
  row.emplace_back("frame", SceneRuntimeFrame(measurement.frame));
  row.emplace_back("current_offset", Pose(measurement.current_offset));
  if (measurement.source_header_frame_id) {
    row.emplace_back("source_header_frame_id",
                     CanonicalValue(*measurement.source_header_frame_id));
  }
  if (measurement.baseline_offset && measurement.drift) {
    row.emplace_back("baseline_offset", Pose(*measurement.baseline_offset));
    row.emplace_back("drift",
                     SceneRuntimePoseDistanceValue(*measurement.drift));
  }
}

CanonicalValue ScenePoseResponse(
    const core::ProjectionRevisionDTO &projection_revision,
    const std::vector<std::pair<core::SceneResourceId, core::Pose3DTO>>
        &poses) {
  CanonicalValue::Array rows;
  rows.reserve(poses.size());
  for (const auto &[scene_resource_id, pose] : poses) {
    rows.emplace_back(CanonicalValue::Object{
        {"pose_in_cell", Pose(pose)},
        {"scene_resource_id", CanonicalValue(scene_resource_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"ok", CanonicalValue(true)},
      {"poses", CanonicalValue(std::move(rows))},
      {"projection_revision", ProjectionRevision(projection_revision)},
  });
}

CanonicalValue
SceneRuntimeIdentityResponseValue(const SceneRuntimeIdentity &identity) {
  Require(identity.valid(), "scene-runtime response identity is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"deployment_generation",
       Unsigned(identity.deployment_generation, "deployment generation")},
      {"graph_revision", CanonicalValue(identity.graph_revision)},
      {"program_generation",
       Unsigned(identity.program_generation, "program generation")},
      {"retained_bundle_id", CanonicalValue(identity.retained_bundle_id)},
      {"runtime_generation",
       Unsigned(identity.runtime_generation, "runtime generation")},
  });
}

CanonicalValue
SceneRuntimeOwnerReadResponse(const std::string_view cell_id,
                              const std::string_view owner_epoch,
                              const SceneRuntimeStateSnapshot &snapshot) {
  Require(!cell_id.empty() && !owner_epoch.empty() &&
              snapshot.runtime_identity.has_value(),
          "scene-runtime owner read is internally incomplete");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"cell_id", CanonicalValue(std::string(cell_id))},
      {"initialization_generation", Unsigned(snapshot.initialization_generation,
                                             "initialization generation")},
      {"initialization_state",
       CanonicalValue(std::string(ToString(snapshot.state)))},
      {"ok", CanonicalValue(true)},
      {"runtime_identity",
       SceneRuntimeIdentityResponseValue(*snapshot.runtime_identity)},
      {"scene_runtime_owner_epoch", CanonicalValue(std::string(owner_epoch))},
  });
}

using ConfiguredPoseSource = deployment::ros2_control_execution_runtime_contract::
    Configuration::PoseSource;

/// The active bundle's pose-source component rows (source_id =
/// component id) across its ROS execution runtime units.
[[nodiscard]] std::vector<ConfiguredPoseSource>
ConfiguredPoseSources(const deployment::RetainedBundle &bundle) {
  std::vector<ConfiguredPoseSource> result;
  for (const auto &unit : bundle.value().cell_graph.runtime_units) {
    if (unit.runtime_profile_id.value() !=
        deployment::ros2_control_execution_runtime_contract::kRuntimeProfileId)
      continue;
    auto configuration =
        deployment::ros2_control_execution_runtime_contract::require_configuration(
            bundle, unit.runtime_unit_id);
    for (auto &source : configuration.pose_sources)
      result.push_back(std::move(source));
  }
  return result;
}

[[nodiscard]] bool
IsConfiguredPoseBinding(const std::vector<ConfiguredPoseSource> &sources,
                        const PoseObservationKey &key, const bool eligible) {
  return std::any_of(sources.begin(), sources.end(), [&](const auto &source) {
    return source.controllable_component_id.value() == key.source_id &&
           std::any_of(source.bindings.begin(), source.bindings.end(),
                       [&](const auto &binding) {
                         return binding.object_id == key.object_id &&
                                binding.policy_id == key.policy_id &&
                                binding.eligible == eligible;
                       });
  });
}

CanonicalValue SceneRuntimeObserverOpenResponse(
    const std::string_view cell_id, const SceneRuntimeIdentity &identity,
    const std::string_view source_id, const std::string_view source_epoch,
    const std::uint64_t source_epoch_order,
    const std::vector<PoseObserverBinding> &bindings) {
  CanonicalValue::Array binding_values;
  binding_values.reserve(bindings.size());
  for (const auto &binding : bindings) {
    binding_values.emplace_back(CanonicalValue::Object{
        {"eligible", CanonicalValue(binding.eligible)},
        {"object_id", CanonicalValue(binding.object_id)},
        {"policy_id", CanonicalValue(binding.policy_id)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"bindings", CanonicalValue(std::move(binding_values))},
      {"cell_id", CanonicalValue(std::string(cell_id))},
      {"ok", CanonicalValue(true)},
      {"runtime_identity", SceneRuntimeIdentityResponseValue(identity)},
      {"source_epoch", CanonicalValue(std::string(source_epoch))},
      {"source_epoch_order",
       Unsigned(source_epoch_order, "source epoch order")},
      {"source_id", CanonicalValue(std::string(source_id))},
  });
}

CanonicalValue SceneRuntimeObserveResponse(
    const PoseSourceObservation &observation,
    const PoseObservationAcceptance &acceptance,
    const SceneRuntimePoseApplicationResult &application,
    const std::uint64_t scene_runtime_source_generation) {
  Require(application.valid(),
          "scene-runtime observation application result is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"disposition",
       CanonicalValue(std::string(ToString(acceptance.disposition)))},
      {"fresh", CanonicalValue(acceptance.fresh)},
      {"initialization_generation",
       Unsigned(acceptance.initialization_generation,
                "initialization generation")},
      {"object_id", CanonicalValue(observation.key.object_id)},
      {"ok", CanonicalValue(true)},
      {"policy_id", CanonicalValue(observation.key.policy_id)},
      {"scene_application_status",
       CanonicalValue(std::string(ToString(application.status)))},
      {"scene_projection_revision",
       application.projection_revision.has_value()
           ? ProjectionRevision(*application.projection_revision)
           : CanonicalValue(nullptr)},
      {"scene_request_id",
       application.scene_request_id.has_value()
           ? CanonicalValue(application.scene_request_id->value())
           : CanonicalValue(nullptr)},
      {"scene_runtime_source_generation",
       Unsigned(scene_runtime_source_generation,
                "scene runtime source generation")},
      {"scene_update_status", application.update_status.has_value()
                                  ? CanonicalValue(std::string(core::to_string(
                                        *application.update_status)))
                                  : CanonicalValue(nullptr)},
      {"source_epoch", CanonicalValue(observation.source_epoch)},
      {"source_epoch_order",
       Unsigned(observation.source_epoch_order, "source epoch order")},
      {"source_id", CanonicalValue(observation.key.source_id)},
      {"source_sequence",
       Unsigned(observation.source_sequence, "source sequence")},
  });
}

struct BeforePlanningLatchResult final {
  core::ProjectionRevisionDTO initial_projection_revision;
  core::ProjectionRevisionDTO projection_revision;
  std::uint64_t latched_observation_count{};
  std::uint64_t committed_observation_count{};
  /// Observations whose exact pose was already in the logical scene; this
  /// includes unchanged samples, which leave the projection revision
  /// unchanged.
  std::uint64_t already_applied_observation_count{};
  std::uint64_t skipped_attached_observation_count{};
  std::uint64_t scene_runtime_source_generation{};
};

CanonicalValue SceneRuntimeBeforePlanningLatchResponse(
    const SceneRuntimeIdentity &runtime_identity,
    const BeforePlanningLatchResult &result) {
  Require(runtime_identity.valid() &&
              result.initial_projection_revision.valid() &&
              result.projection_revision.valid() &&
              result.latched_observation_count ==
                  result.committed_observation_count +
                      result.already_applied_observation_count +
                      result.skipped_attached_observation_count &&
              result.scene_runtime_source_generation != 0U,
          "scene-runtime before-planning latch result is invalid");
  return CanonicalValue(CanonicalValue::Object{
      {"already_applied_observation_count",
       Unsigned(result.already_applied_observation_count,
                "already applied observation count")},
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"committed_observation_count",
       Unsigned(result.committed_observation_count,
                "committed observation count")},
      {"initial_projection_revision",
       ProjectionRevision(result.initial_projection_revision)},
      {"latched_observation_count",
       Unsigned(result.latched_observation_count, "latched observation count")},
      {"ok", CanonicalValue(true)},
      {"projection_revision", ProjectionRevision(result.projection_revision)},
      {"runtime_identity", SceneRuntimeIdentityResponseValue(runtime_identity)},
      {"scene_runtime_source_generation",
       Unsigned(result.scene_runtime_source_generation,
                "scene runtime source generation")},
      {"skipped_attached_observation_count",
       Unsigned(result.skipped_attached_observation_count,
                "skipped attached observation count")},
  });
}

CanonicalValue CanonicalCodecValue(const std::string_view bytes,
                                   const std::string_view label) {
  const auto parsed = deployment::parse_strict_json_model_yaml(bytes);
  Require(content_identity::canonical_json(parsed.root()) == bytes,
          std::string(label) + " codec did not return canonical JSON");
  return parsed.root();
}

CanonicalValue
SceneOwnerReadResponse(const core::RequestOwnerEpoch &request_owner_epoch,
                       const core::ProjectionRevisionDTO &projection_revision,
                       const core::LogicalSceneSnapshotDTO &snapshot) {
  Require(!request_owner_epoch.empty() && projection_revision.valid() &&
              snapshot.locally_valid() &&
              snapshot.revision == projection_revision.scene_revision,
          "logical-scene owner read is internally inconsistent");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"logical_scene",
       CanonicalCodecValue(scene::EncodeLogicalSceneSnapshot(snapshot),
                           "logical-scene snapshot")},
      {"ok", CanonicalValue(true)},
      {"projection_revision", ProjectionRevision(projection_revision)},
      {"request_owner_epoch", CanonicalValue(request_owner_epoch.value())},
  });
}

CanonicalValue ErrorValue(const core::ErrorDTO &error) {
  Require(!error.code.empty() && !error.message.empty(),
          "scene response error is incomplete");
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

/// The error of a refused Manager response: the ErrorDTO plus its producer.
CanonicalValue ResponseErrorValue(const core::ErrorDTO &error,
                                  const std::string_view producer = kManagerProducer) {
  Require(!error.code.empty(), "Manager response error has no code");
  CanonicalValue::Object details;
  details.reserve(error.details.size());
  for (const auto &[key, value] : error.details) {
    details.emplace_back(key, CanonicalValue(value));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue(error.code)},
      {"details", CanonicalValue(std::move(details))},
      {"message", CanonicalValue(error.message)},
      {"producer", CanonicalValue(std::string(producer))},
  });
}

CanonicalValue SceneCandidatePreviewResponse(
    const core::Result<core::SceneCandidatePreviewDTO> &result) {
  if (!result.ok()) {
    return CanonicalValue(CanonicalValue::Object{
        {"api_version",
         CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
        {"error", ResponseErrorValue(result.error())},
        {"ok", CanonicalValue(false)},
    });
  }
  const auto encoded = scene::EncodeSceneCandidatePreview(result.value());
  if (encoded.size() > kMaximumResponseBytes - 1024U) {
    return CanonicalValue(CanonicalValue::Object{
        {"api_version",
         CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
        {"error",
         ResponseErrorValue(core::ErrorDTO{
             "scene_candidate_preview_oversized",
             "scene candidate preview exceeds the bounded Manager transport",
             {}})},
        {"ok", CanonicalValue(false)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"candidate_preview",
       CanonicalCodecValue(encoded, "scene candidate preview")},
      {"ok", CanonicalValue(true)},
  });
}

CanonicalValue SceneCommitResponse(const ManagerCommand &command,
                                   const core::SceneCommitResultDTO &result) {
  Require(command.scene_transaction.has_value() &&
              command.request_id.has_value() &&
              command.request_owner_epoch.has_value(),
          "decoded scene command lacks request identity");
  Require(result.presence_valid() &&
              result.request_id.value() == *command.request_id,
          "scene owner returned an invalid or mismatched result");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"committed_projection_revision",
       result.committed_projection_revision.has_value()
           ? ProjectionRevision(*result.committed_projection_revision)
           : CanonicalValue(nullptr)},
      {"error", result.error.has_value() ? ResponseErrorValue(*result.error)
                                         : CanonicalValue(nullptr)},
      {"ok", CanonicalValue(result.success)},
      {"projection_status",
       CanonicalValue(std::string(core::to_string(result.projection_status)))},
      {"replay_status",
       CanonicalValue(std::string(core::to_string(result.replay_status)))},
      {"request_id", CanonicalValue(*command.request_id)},
      {"request_owner_epoch", CanonicalValue(*command.request_owner_epoch)},
      {"transaction_sequence",
       result.transaction_sequence.has_value()
           ? Unsigned(*result.transaction_sequence, "transaction sequence")
           : CanonicalValue(nullptr)},
      {"update_status",
       result.update_status.has_value()
           ? CanonicalValue(std::string(core::to_string(*result.update_status)))
           : CanonicalValue(nullptr)},
  });
}

CanonicalValue
DetailedErrorResponse(const std::string_view code,
                      const std::string_view message,
                      const std::map<std::string, std::string> &details,
                      const std::string_view producer = kManagerProducer) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"error", ResponseErrorValue(core::ErrorDTO{std::string(code),
                                                  std::string(message), details},
                                   producer)},
      {"ok", CanonicalValue(false)},
  });
}

CanonicalValue ErrorResponse(const std::string_view code,
                             const std::string_view message) {
  return DetailedErrorResponse(code, message, {});
}

/// The refusal for one caught failure: a typed cause keeps its code, details
/// and producer; any other exception is `fallback_code` with its text.
core::TypedCause ManagerCauseOf(const std::exception &error,
                                const std::string_view fallback_code) {
  if (dynamic_cast<const deployment::ProgramReferenceValidationRefusal *>(&error)) {
    const core::TypedError typed("program_reference_validation_failed", error.what());
    return core::CauseOf(typed, fallback_code, kManagerProducer);
  }
  if (const auto *program =
          dynamic_cast<const deployment::ProgramContractError *>(&error);
      program != nullptr && !program->code().empty()) {
    const core::TypedError typed(program->code(), program->what());
    return core::CauseOf(typed, fallback_code, kManagerProducer);
  }
  return core::CauseOf(error, fallback_code, kManagerProducer);
}

CanonicalValue WithReferenceDiagnostics(CanonicalValue response,
                                        const std::exception &error) {
  const auto *refusal = dynamic_cast<const deployment::ProgramReferenceValidationRefusal *>(&error);
  if (!refusal) return response;
  return detail::AttachReferenceDiagnostics(std::move(response), refusal->diagnostics());
}

CanonicalValue CaughtErrorResponse(const std::exception &error,
                                   const std::string_view fallback_code) {
  const auto cause = ManagerCauseOf(error, fallback_code);
  return WithReferenceDiagnostics(
      DetailedErrorResponse(cause.error.code, cause.error.message,
                            cause.error.details, cause.producer), error);
}

CanonicalValue ApplyDetailedErrorResponse(
    const ManagerCommand &command, const core::ErrorDTO &error,
    const std::string_view producer = kManagerProducer) {
  Require(command.request_id.has_value() &&
              command.request_owner_epoch.has_value(),
          "decoded Apply command lacks request identity");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"error", ResponseErrorValue(error, producer)},
      {"ok", CanonicalValue(false)},
      {"request_id", CanonicalValue(*command.request_id)},
      {"request_owner_epoch", CanonicalValue(*command.request_owner_epoch)},
  });
}

CanonicalValue ApplyKnownNoEffectDetailedErrorResponse(
    const ManagerCommand &command, const core::ErrorDTO &error,
    const std::string_view producer = kManagerProducer) {
  auto marked = error;
  marked.details["effect"] = "none";
  return ApplyDetailedErrorResponse(command, marked, producer);
}

CanonicalValue ApplyKnownNoEffectErrorResponse(
    const ManagerCommand &command, const std::string_view code,
    const std::string_view message) {
  return ApplyKnownNoEffectDetailedErrorResponse(
      command, core::ErrorDTO{std::string(code), std::string(message), {}});
}

CanonicalValue ApplyUnavailableResponse(const ManagerCommand &command) {
  return ApplyKnownNoEffectErrorResponse(
      command, kKernelOwnerUnavailableReasonCode,
      "operational Apply requires one converged managed Kernel execution "
      "host");
}

template <typename Record, typename Identity>
CanonicalValue DeploymentRecordDiff(const std::vector<Record> &current,
                                    const std::vector<Record> &candidate,
                                    Identity identity) {
  std::map<std::string, const Record *> current_by_id;
  std::map<std::string, const Record *> candidate_by_id;
  for (const auto &record : current) {
    current_by_id.emplace(std::string(identity(record)), &record);
  }
  for (const auto &record : candidate) {
    candidate_by_id.emplace(std::string(identity(record)), &record);
  }
  CanonicalValue::Array added;
  CanonicalValue::Array removed;
  CanonicalValue::Array changed;
  for (const auto &[id, record] : candidate_by_id) {
    const auto present = current_by_id.find(id);
    if (present == current_by_id.end()) {
      added.emplace_back(id);
    } else if (*present->second != *record) {
      changed.emplace_back(id);
    }
  }
  for (const auto &[id, record] : current_by_id) {
    static_cast<void>(record);
    if (!candidate_by_id.contains(id)) {
      removed.emplace_back(id);
    }
  }
  return CanonicalValue(CanonicalValue::Object{
      {"added", CanonicalValue(std::move(added))},
      {"changed", CanonicalValue(std::move(changed))},
      {"removed", CanonicalValue(std::move(removed))},
  });
}

CanonicalValue DeploymentGraphDiff(const core::CellGraphDTO &current,
                                   const core::CellGraphDTO &candidate) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_attachments",
       DeploymentRecordDiff(current.component_attachments,
                            candidate.component_attachments,
                            [](const core::ComponentAttachmentEdgeDTO &record) {
                              return record.component_attachment_id.value();
                            })},
      {"controllable_components",
       DeploymentRecordDiff(current.controllable_components,
                            candidate.controllable_components,
                            [](const core::ControllableComponentDTO &record) {
                              return record.controllable_component_id.value();
                            })},
      {"control_bindings",
       DeploymentRecordDiff(current.control_bindings,
                            candidate.control_bindings,
                            [](const core::ControlBindingDTO &record) {
                              return record.control_binding_id.value();
                            })},
      {"control_domains",
       DeploymentRecordDiff(current.control_domains, candidate.control_domains,
                            [](const core::ControlDomainDTO &record) {
                              return record.control_domain_id.value();
                            })},
      {"model_instances",
       DeploymentRecordDiff(current.model_instances, candidate.model_instances,
                            [](const core::ModelInstanceDTO &record) {
                              return record.model_instance_id.value();
                            })},
      {"runtime_units",
       DeploymentRecordDiff(current.runtime_units, candidate.runtime_units,
                            [](const core::RuntimeUnitDTO &record) {
                              return record.runtime_unit_id.value();
                            })},
  });
}

/// The one authoritative impact of applying `candidate` over `current`:
/// the per-component runtime decision, shared Scene/attachment consequences
/// and the prepared Program outcome, all derived from verified facts. Clients
/// present it and bind their confirmation to it; they never re-derive it.
CanonicalValue DeploymentApplyImpact(
    const deployment::RetainedBundle &current,
    const deployment::RetainedBundle &candidate,
    const std::filesystem::path &activation_root,
    const core::ContentDigest &prepared_activation_id,
    const deployment::SiteFacts &site_facts) {
  const auto delta =
      deployment::derive_apply_component_delta(current, candidate, site_facts);
  Require(!activation_root.empty(),
          "Deployment impact requires the trusted activation store");
  const auto prepared =
      deployment::FilesystemPreparedActivationRepository(activation_root)
          .get_verified(prepared_activation_id);
  Require(prepared != nullptr &&
              prepared->retained_bundle_id() == candidate.retained_bundle_id(),
          "Deployment impact names no stored prepared activation for the "
          "candidate");
  CanonicalValue::Array components;
  bool destructive = false;
  for (const auto &change : delta.components) {
    destructive = destructive ||
                  change.disposition ==
                      deployment::ApplyComponentDisposition::kRemove ||
                  change.disposition ==
                      deployment::ApplyComponentDisposition::kReplace;
    CanonicalValue::Array reasons;
    for (const auto &reason : change.replacement_reasons) {
      reasons.emplace_back(reason);
    }
    components.emplace_back(CanonicalValue::Object{
        {"controllable_component_id",
         CanonicalValue(change.controllable_component_id.value())},
        {"disposition",
         CanonicalValue(std::string(deployment::to_string(change.disposition)))},
        {"replacement_reasons", CanonicalValue(std::move(reasons))},
    });
  }
  CanonicalValue::Array attachments;
  for (const auto &component : delta.attachment_changed_components) {
    attachments.emplace_back(component.value());
  }
  // Children that stay in the cell, detached at their authored root pose.
  CanonicalValue::Array detached;
  for (const auto &component : deployment::detached_attachment_children(
           current.value().cell_graph, candidate.value().cell_graph)) {
    detached.emplace_back(component.value());
  }
  const auto &outcome = prepared->program_outcome();
  destructive = destructive || outcome.has_value();
  return CanonicalValue(CanonicalValue::Object{
      {"attachment_changed_components", CanonicalValue(std::move(attachments))},
      {"component_delta_digest",
       CanonicalValue(deployment::DigestApplyComponentDelta(delta).value())},
      {"components", CanonicalValue(std::move(components))},
      {"destructive", CanonicalValue(destructive)},
      {"detached_components", CanonicalValue(std::move(detached))},
      {"program",
       CanonicalValue(CanonicalValue::Object{
           {"executable", CanonicalValue(prepared->program_binding_id().has_value())},
           {"non_executable",
            outcome.has_value()
                ? content_identity::parse_canonical_json(
                      deployment::EncodeProgramNonExecutableOutcome(*outcome))
                : CanonicalValue(nullptr)},
           {"program_id", prepared->program_id().has_value()
                              ? CanonicalValue(prepared->program_id()->value())
                              : CanonicalValue(nullptr)},
           {"program_revision",
            prepared->program_revision().has_value()
                ? CanonicalValue(prepared->program_revision()->value())
                : CanonicalValue(nullptr)},
       })},
      {"shared_scene_projection_changed",
       CanonicalValue(delta.shared_scene_projection_changed)},
  });
}

core::ContentDigest DeploymentApplyImpactDigest(const CanonicalValue &impact) {
  return core::ContentDigest(content_identity::sha256_digest(
      content_identity::canonical_json(impact)));
}

CanonicalValue
DeploymentCandidatePreviewResponse(const DeploymentCandidateReceipt &receipt,
                                   const core::CellGraphDTO &current_graph,
                                   const core::CellGraphDTO &candidate_graph,
                                   CanonicalValue impact) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"candidate_receipt", DeploymentCandidateReceiptValue(receipt)},
      {"converged", CanonicalValue(current_graph == candidate_graph)},
      {"diff", DeploymentGraphDiff(current_graph, candidate_graph)},
      {"impact", std::move(impact)},
      {"ok", CanonicalValue(true)},
  });
}

CanonicalValue
ApplyResponse(const ManagerCommand &command,
              const component_manager::ManagerNamedApplyResult &result) {
  Require(command.request_id.has_value() &&
              command.request_owner_epoch.has_value(),
          "decoded Apply command lacks request identity");
  const auto succeeded =
      result.state ==
      component_manager::ManagerNamedApplyResultState::kSucceeded;
  Require(succeeded ==
              (result.terminal_outcome.has_value() &&
               result.terminal_outcome->succeeded()),
          "Manager Apply result has inconsistent terminal evidence");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"apply_generation",
       Unsigned(result.apply_generation.value(), "Apply generation")},
      {"apply_id", CanonicalValue(result.apply_id.value())},
      {"candidate_projection_revision",
       succeeded
           ? ProjectionRevision(
                 *result.terminal_outcome->candidate_projection_revision)
           : CanonicalValue(nullptr)},
      {"error", CanonicalValue(nullptr)},
      {"ok", CanonicalValue(true)},
      {"replay_status",
       CanonicalValue(result.request_replayed ? "replayed_exact"
                                              : "accepted_new")},
      {"request_id", CanonicalValue(*command.request_id)},
      {"request_owner_epoch", CanonicalValue(*command.request_owner_epoch)},
      {"state",
       CanonicalValue(result.state ==
                              component_manager::ManagerNamedApplyResultState::
                                  kPending
                          ? std::string("pending")
                          : std::string(component_manager::ToString(
                                result.terminal_outcome->state)))},
  });
}

/// Compact immutable identity of the preparation accepted by one Apply.
/// The complete receipt remains in the Manager journal; observation does not
/// expand that private input into a second public status shape.
CanonicalValue ApplyPreparedActivationValue(
    const component_manager::ManagerApplyPreparedActivationDTO &activation) {
  return CanonicalValue(CanonicalValue::Object{
      {"captured_input_digest",
       CanonicalValue(activation.captured_input_digest.value())},
      {"deployment_artifact_id",
       CanonicalValue(activation.deployment_artifact_id.value())},
      {"deployment_revision",
       CanonicalValue(activation.deployment_revision.value())},
      // Null exactly for a prepared non-executable Program.
      {"program_binding_id",
       activation.program_binding_id.has_value()
           ? CanonicalValue(activation.program_binding_id->value())
           : CanonicalValue(nullptr)},
      {"prepared_activation_id",
       CanonicalValue(activation.prepared_activation_id.value())},
  });
}

/// One bounded lifecycle status for one durable Apply generation.
///
/// The status exposes only durable facts: the operation identity, the accepted
/// prepared-activation identity, the deepest durably reached milestone with its
/// exact evidence digest, and exactly one terminal outcome. `phase` and
/// `last_progress` are pure projections over immutable parent-linked records; no
/// mutable phase label, heartbeat or socket responsiveness is reported as
/// progress. A failure is reported once, with its first retained failure and
/// effect certainty, and is never repaired, retried or rolled back.
CanonicalValue ApplyStatusResponse(
    const component_manager::PersistedManagerApplyStatus &status,
    const component_manager::FailedApplyAcknowledgementState failure_acknowledgement) {
  const auto &intent = status.intent.intent;
  const auto progress =
      component_manager::DeriveManagerApplyProgress(status);
  const auto phase =
      CanonicalValue(std::string(component_manager::ToString(progress.phase)));
  const auto terminal = status.terminal_outcome.has_value()
                            ? &status.terminal_outcome->terminal_outcome
                            : nullptr;
  const bool succeeded =
      terminal != nullptr && terminal->state ==
                                 component_manager::
                                     ManagerApplyTerminalOutcomeState::kSucceeded;
  auto failure = CanonicalValue(nullptr);
  if (terminal != nullptr && terminal->failure.has_value()) {
    const auto &retained = *terminal->failure;
    CanonicalValue::Object cause_details;
    for (const auto &[key, value] : retained.cause_details) {
      cause_details.emplace_back(key, CanonicalValue(value));
    }
    failure = CanonicalValue(CanonicalValue::Object{
        {"cause_code", CanonicalValue(retained.cause_code)},
        {"cause_details", CanonicalValue(std::move(cause_details))},
        {"code", CanonicalValue(retained.failure_code)},
        {"effect_boundary",
         CanonicalValue(std::string(
             component_manager::ToString(retained.effect_boundary)))},
        {"effect_disposition",
         CanonicalValue(std::string(
             component_manager::ToString(retained.effect_disposition)))},
        {"message", CanonicalValue(retained.failure_message)},
        {"phase", CanonicalValue(
                      std::string(component_manager::ToString(retained.phase)))},
        {"producer", CanonicalValue(retained.producer)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"apply_generation",
       Unsigned(intent.apply_generation.value(), "Apply generation")},
      {"apply_id", CanonicalValue(intent.apply_id.value())},
      {"candidate_projection_revision",
       succeeded ? ProjectionRevision(*terminal->candidate_projection_revision)
                 : CanonicalValue(nullptr)},
      {"error", CanonicalValue(nullptr)},
      {"failure", std::move(failure)},
      {"failure_acknowledgement",
       CanonicalValue(std::string(component_manager::ToString(failure_acknowledgement)))},
      {"last_progress",
       CanonicalValue(CanonicalValue::Object{
           {"phase", phase},
           {"record_digest", CanonicalValue(progress.record_digest.value())},
       })},
      {"ok", CanonicalValue(true)},
      {"phase", phase},
      {"prepared_activation",
       ApplyPreparedActivationValue(intent.prepared_activation)},
      {"scene_initialization",
       CanonicalValue(std::string(
           component_manager::ToString(intent.scene_initialization)))},
      {"request_id", CanonicalValue(intent.request_context.request_id.value())},
      {"request_owner_epoch",
       CanonicalValue(intent.request_context.request_owner_epoch.value())},
      {"state", CanonicalValue(terminal == nullptr
                                   ? std::string("pending")
                                   : std::string(component_manager::ToString(
                                         terminal->state)))},
  });
}

CanonicalValue ComponentProcessStopResultResponse(
    const component_manager::ManagerComponentProcessStopResultDTO &result) {
  Require(result.valid(), "component process Stop result is invalid");
  const auto &request = result.request;
  CanonicalValue::Object context{
      {"request_id", CanonicalValue(request.request_context.request_id.value())},
      {"request_owner_epoch", CanonicalValue(request.request_context.request_owner_epoch.value())},
      {"source_label", CanonicalValue(request.request_context.source_label)}};
  if (request.request_context.correlation_id)
    context.emplace_back("correlation_id", CanonicalValue(*request.request_context.correlation_id));
  CanonicalValue request_value(CanonicalValue::Object{
      {"request_context", CanonicalValue(std::move(context))},
      {"predecessor_projection_revision", ProjectionRevision(request.predecessor_projection_revision)},
      {"expected_predecessor_publication_generation", Unsigned(request.expected_predecessor_publication_generation.value(), "publication generation")},
      {"expected_predecessor_public_pointer_digest", CanonicalValue(request.expected_predecessor_public_pointer_digest.value())},
      {"runtime_unit_id", CanonicalValue(request.runtime_unit_id.value())},
      {"runtime_epoch", CanonicalValue(request.runtime_epoch.value())},
      {"manager_runtime_epoch", CanonicalValue(request.manager_runtime_epoch.value())},
      {"controllable_component_id", CanonicalValue(request.controllable_component_id.value())},
      {"launch_identity", CanonicalValue(request.launch_identity)},
      {"leader_pid", CanonicalValue(static_cast<double>(request.leader_pid))}});
  const auto state = [&] {
    switch (result.state) {
      case core::ControllerLaunchCleanupState::kNotRequested: return "not_requested";
      case core::ControllerLaunchCleanupState::kPending: return "pending";
      case core::ControllerLaunchCleanupState::kUnknown: return "unknown";
      case core::ControllerLaunchCleanupState::kDrained: return "drained";
    }
    Fail("component process Stop result state is invalid");
  }();
  auto receipt = CanonicalValue(nullptr);
  if (result.receipt) receipt = CanonicalValue(CanonicalValue::Object{
      {"leader_pid", CanonicalValue(static_cast<double>(result.receipt->leader_pid))},
      {"controllable_component_id", CanonicalValue(result.receipt->controllable_component_id)},
      {"launch_identity", CanonicalValue(result.receipt->launch_identity)},
      {"observed_utc", CanonicalValue(result.receipt->observed_utc)}});
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"ok", CanonicalValue(true)},
      {"error", CanonicalValue(nullptr)},
      {"result", CanonicalValue(CanonicalValue::Object{
          {"request", std::move(request_value)}, {"state", CanonicalValue(state)},
          {"receipt", std::move(receipt)},
          {"error", result.error ? ErrorValue(*result.error) : CanonicalValue(nullptr)}})}});
}

CanonicalValue
SuccessResponse(const std::string_view operation,
                const std::optional<std::uint64_t> manager_gen,
                const std::optional<std::uint64_t> scene_gen,
                const std::optional<std::uint64_t> scene_runtime_gen) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"manager_source_generation",
       manager_gen.has_value()
           ? CanonicalValue(static_cast<double>(*manager_gen))
           : CanonicalValue(nullptr)},
      {"ok", CanonicalValue(true)},
      {"operation", CanonicalValue(std::string(operation))},
      {"scene_source_generation",
       scene_gen.has_value() ? CanonicalValue(static_cast<double>(*scene_gen))
                             : CanonicalValue(nullptr)},
      {"scene_runtime_source_generation",
       scene_runtime_gen.has_value()
           ? CanonicalValue(static_cast<double>(*scene_runtime_gen))
           : CanonicalValue(nullptr)},
  });
}

std::optional<std::string> ReceiveCommand(const int client) {
  pollfd descriptor{client, POLLIN, 0};
  int ready = -1;
  do {
    ready = ::poll(&descriptor, 1, kClientReadTimeoutMilliseconds);
  } while (ready < 0 && errno == EINTR);
  if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
    return std::nullopt;
  }
  std::array<char, kMaximumCommandBytes + 1U> bytes{};
  ssize_t count = -1;
  do {
    count = ::recv(client, bytes.data(), bytes.size(), MSG_TRUNC);
  } while (count < 0 && errno == EINTR);
  if (count <= 0 || static_cast<std::size_t>(count) > kMaximumCommandBytes) {
    return std::nullopt;
  }
  return std::string(bytes.data(), static_cast<std::size_t>(count));
}

void TrySendResponse(const int client,
                     const CanonicalValue &response) noexcept {
  try {
    auto bytes = content_identity::canonical_json(response);
    if (bytes.empty()) {
      return;
    }
    if (bytes.size() >
        detail::AtomicResponseCapacity(client, kMaximumResponseBytes)) {
      bytes = content_identity::canonical_json(ErrorResponse(
          "manager_command_response_oversized",
          "Manager response exceeds the bounded private transport"));
    }
    pollfd descriptor{client, POLLOUT, 0};
    int ready = -1;
    do {
      ready = ::poll(&descriptor, 1, kClientReadTimeoutMilliseconds);
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || (descriptor.revents & POLLOUT) == 0) {
      return;
    }
    ssize_t count = -1;
    do {
      count = ::send(client, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    // The command effect is authoritative once completed. A disappearing or
    // slow client loses only its response and must reconcile the unknown
    // outcome; it must not terminate the sole Manager owner.
    static_cast<void>(count);
  } catch (...) {
    // Response construction/delivery is deliberately isolated from owner
    // availability. All responses are bounded canonical objects assembled by
    // this process, but even an allocation failure cannot be reported safely
    // through a client that may already be gone.
  }
}

std::optional<component_manager::ManagerBootstrapRuntimePlanDTO>
BuildBootstrapRuntimePlan(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const deployment::IVerifiedRetainedProductArtifactSource &artifacts) {
  auto rows = component_manager::BuildVerifiedRuntimeConvergencePlan(
      admission.retained_bundle(), artifacts, "linux",
      {std::string(deployment::ros2_control_execution_runtime_contract::
                       kLaunchEnvironmentContract)});
  if (rows.empty()) {
    return std::nullopt;
  }
  return component_manager::ManagerBootstrapRuntimePlanDTO{
      admission.retained_bundle_id(), admission.graph_revision(),
      admission.admission_digest(), std::move(rows)};
}

/// One sealed, generation-scoped view of the immutable retained package.
/// Filesystem bytes are verified on the first exact lookup; live loops reuse
/// only the resulting typed values. Replacing the view requires replacing the
/// owning Manager generation.
class ManagerVerifiedPackageView final
    : public deployment::IVerifiedRetainedBundleSource,
      public deployment::IVerifiedRetainedProductArtifactSource {
public:
  explicit ManagerVerifiedPackageView(
      std::filesystem::path package_root,
      std::optional<std::filesystem::path> activation_root =
          std::nullopt)
      : package_root_(package_root), activation_root_(activation_root),
        bundles_(package_root, activation_root),
        artifacts_(package_root, activation_root) {}

  [[nodiscard]] std::vector<core::AddObjectRequestDTO> initial_scene(
      const component_manager::ManagerApplyPreparedActivationDTO &activation,
      const deployment::RetainedBundle &bundle) const {
    const auto receipt = content_identity::parse_canonical_json(
        activation.canonical_receipt_json,
        {component_manager::kMaximumManagerApplyPreparedReceiptBytes, 16U,
         4096U});
    const auto required = [&](std::string_view key) {
      const auto *field = receipt.find_object_member(key);
      if (field == nullptr || !field->is_string() ||
          field->as_string().empty()) {
        throw std::invalid_argument(
            "Apply initial Scene has no exact prepared " + std::string(key));
      }
      return field->as_string();
    };
    const auto *outcome = receipt.find_object_member("program_outcome");
    const auto *bound_id =
        outcome == nullptr ? nullptr
                           : outcome->find_object_member("program_binding_id");
    const bool outcome_matches =
        activation.program_binding_id.has_value()
            ? bound_id != nullptr && bound_id->is_string() &&
                  bound_id->as_string() == activation.program_binding_id->value()
            : outcome != nullptr && bound_id == nullptr;
    if (required("prepared_activation_id") !=
            activation.prepared_activation_id.value() ||
        !outcome_matches ||
        required("retained_bundle_id") != bundle.retained_bundle_id().value() ||
        required("graph_revision") !=
            bundle.value().cell_graph.graph_revision.value() ||
        required("deployment_artifact_id") !=
            activation.deployment_artifact_id.value() ||
        required("deployment_revision") !=
            activation.deployment_revision.value()) {
      throw std::invalid_argument(
          "Apply initial Scene prepared identity differs from candidate");
    }
    const auto &maps = bundle.value().compiled_products.symbol_role_maps;
    if (outcome != nullptr && RequiredString(*outcome, "kind") == "no_program") {
      if (!activation_root_.has_value() ||
          !PreparedActivationIsStoredExactly(*activation_root_, activation)) {
        throw std::invalid_argument("Apply empty Scene requires exact no_program evidence");
      }
      // Empty initial intents still go through the normal Scene staging and
      // attachment disposition path; they are not a direct Scene mutation.
      return {};
    }
    const bool empty_non_executable_candidate =
        !activation.program_binding_id.has_value() && maps.empty() &&
        bundle.value().cell_graph.controllable_components.empty();
    if (maps.size() != 1U && !empty_non_executable_candidate) {
      throw std::invalid_argument(
          "Apply initial Scene requires one exact retained model map");
    }
    std::optional<deployment::VerifiedRetainedModelMap> model_map;
    if (!maps.empty()) {
      deployment::FilesystemVerifiedRetainedModelMapSource map_source(
          package_root_, activation_root_);
      model_map = map_source.get_verified(
          {bundle.retained_bundle_id(), maps.front().product_id});
      if (!model_map.has_value()) {
        throw std::invalid_argument(
            "Apply initial Scene retained model map is unavailable");
      }
    }
    const auto read_program = [&](const std::filesystem::path &root) {
      return deployment::FilesystemVerifiedProgramSource(root).get_verified(
          core::ProgramId(required("program_id")),
          core::ProgramRevision(required("program_revision")), bundle);
    };
    auto program = activation_root_.has_value()
                       ? read_program(*activation_root_)
                       : nullptr;
    if (program == nullptr)
      program = read_program(package_root_);
    if (!activation.program_binding_id.has_value()) {
      // A non-executable Program is placed through the roles that still
      // resolve against this verified candidate: an object on a surviving
      // frame keeps its place, one on an unresolved frame is refused by name,
      // never dropped. Unverifiable evidence fails closed.
      if (program == nullptr || !activation_root_.has_value()) {
        throw std::invalid_argument(
            "Apply initial Scene Program or prepared evidence is not retained");
      }
      const auto prepared =
          deployment::FilesystemPreparedActivationRepository(*activation_root_)
              .get_verified(activation.prepared_activation_id);
      if (prepared == nullptr ||
          prepared->retained_bundle_id() != bundle.retained_bundle_id() ||
          prepared->program_revision() != program->value().program_revision) {
        throw std::invalid_argument(
            "Apply initial Scene prepared evidence differs from candidate");
      }
      const auto outcome =
          deployment::recompile_prepared_program_outcome(*prepared);
      if (!outcome.resolved_references.has_value()) {
        throw std::invalid_argument(
            "Apply initial Scene outcome is not non-executable");
      }
      return deployment::project_resolved_initial_scene(
          *program, *outcome.resolved_references, bundle.value().cell_graph.cell_id);
    }
    const auto &catalog_root = activation_root_.has_value() ? *activation_root_ : package_root_;
    const deployment::FilesystemVerifiedCatalogArtifactSource catalog(catalog_root);
    const auto read_binding = [&](const std::filesystem::path &root) {
      if (!model_map.has_value()) {
        throw std::invalid_argument("Apply initial Scene binding has no retained model map");
      }
      return deployment::FilesystemVerifiedProgramBindingSource(root)
          .get_verified(*activation.program_binding_id, bundle, *model_map, catalog);
    };
    auto binding = activation_root_.has_value()
                       ? read_binding(*activation_root_)
                       : nullptr;
    if (binding == nullptr)
      binding = read_binding(package_root_);
    if (program == nullptr || binding == nullptr) {
      throw std::invalid_argument(
          "Apply initial Scene Program or binding is not retained");
    }
    return deployment::project_bound_initial_scene(
        *program, *binding, bundle.value().cell_graph.cell_id);
  }

  [[nodiscard]] std::shared_ptr<const deployment::RetainedBundle> get_verified(
      const core::RetainedBundleId &retained_bundle_id) const override {
    std::lock_guard lock(mutex_);
    const auto cached = bundles_by_id_.find(retained_bundle_id.value());
    if (cached != bundles_by_id_.end()) {
      return cached->second;
    }
    auto verified = bundles_.get_verified(retained_bundle_id);
    if (verified == nullptr) {
      return nullptr;
    }
    return bundles_by_id_
        .emplace(retained_bundle_id.value(), std::move(verified))
        .first->second;
  }

  [[nodiscard]] std::optional<deployment::VerifiedRetainedProductArtifact>
  get_verified(
      const deployment::RetainedProductArtifactQuery &query) const override {
    std::lock_guard lock(mutex_);
    const auto cached =
        std::find_if(product_artifacts_.begin(), product_artifacts_.end(),
                     [&](const auto &entry) { return entry.first == query; });
    if (cached != product_artifacts_.end()) {
      return cached->second;
    }
    auto verified = artifacts_.get_verified(query);
    if (!verified.has_value()) {
      return std::nullopt;
    }
    product_artifacts_.emplace_back(query, std::move(*verified));
    return product_artifacts_.back().second;
  }

  [[nodiscard]] std::optional<deployment::VerifiedRetainedRuntimeUnitProduct>
  runtime_unit_product(const deployment::RetainedBundle &bundle,
                       const core::RuntimeUnitId &runtime_unit_id) const {
    {
      std::lock_guard lock(mutex_);
      const auto cached =
          std::find_if(runtime_products_.begin(), runtime_products_.end(),
                       [&](const auto &product) {
                         return product.retained_bundle_id() ==
                                    bundle.retained_bundle_id() &&
                                product.runtime_unit_id() == runtime_unit_id;
                       });
      if (cached != runtime_products_.end()) {
        return *cached;
      }
    }
    auto verified =
        deployment::verify_runtime_unit_product(bundle, runtime_unit_id, *this);
    if (!verified.has_value()) {
      return std::nullopt;
    }
    std::lock_guard lock(mutex_);
    const auto cached = std::find_if(
        runtime_products_.begin(), runtime_products_.end(),
        [&](const auto &product) {
          return product.retained_bundle_id() == bundle.retained_bundle_id() &&
                 product.runtime_unit_id() == runtime_unit_id;
        });
    if (cached != runtime_products_.end()) {
      return *cached;
    }
    runtime_products_.push_back(std::move(*verified));
    return runtime_products_.back();
  }

  // Fixed pose of an attachment carrier frame in a source-local link of the
  // carrier's model instance, plus the carrier's source link name. Only
  // successful resolutions are cached.
  [[nodiscard]] core::Result<SceneRuntimeReferenceLinkResolution>
  CarrierInReferenceLink(const core::RetainedBundleId &retained_bundle_id,
                         const core::ComponentSceneFrameRefDTO &carrier,
                         const std::string &reference_link) const {
    using Reply = core::Result<SceneRuntimeReferenceLinkResolution>;
    const auto refused = [&](std::string code, std::string message) {
      return Reply::Err(core::ErrorDTO{
          std::move(code), std::move(message),
          {{"reference_link", reference_link},
           {"frame_role_id", carrier.frame_role_id.value()},
           {"model_instance_id", carrier.model_instance_id.value()}}});
    };
    const auto key = std::make_tuple(
        retained_bundle_id.value(), carrier.controllable_component_id.value(),
        carrier.model_instance_id.value(), carrier.frame_role_id.value(),
        reference_link);
    {
      std::lock_guard lock(mutex_);
      if (const auto cached = carrier_reference_poses_.find(key);
          cached != carrier_reference_poses_.end())
        return Reply::Ok(cached->second);
    }
    try {
      const auto bundle = get_verified(retained_bundle_id);
      if (bundle == nullptr)
        return refused("scene_runtime_pose_reference_unresolved",
                       "retained bundle is unavailable");
      const auto &maps = bundle->value().compiled_products.symbol_role_maps;
      if (maps.size() != 1U)
        return refused("scene_runtime_pose_reference_unresolved",
                       "retained bundle has no exact model map");
      const auto model_map =
          deployment::FilesystemVerifiedRetainedModelMapSource(package_root_,
                                                               activation_root_)
              .get_verified({retained_bundle_id, maps.front().product_id});
      if (!model_map.has_value())
        return refused("scene_runtime_pose_reference_unresolved",
                       "retained model map is unavailable");
      std::vector<const std::string *> carrier_links;
      for (const auto &path : model_map->frame_paths())
        if (const auto *frame =
                std::get_if<deployment::CompositionComponentModelFrame>(
                    &path.terminal_frame);
            frame != nullptr &&
            frame->controllable_component_id ==
                carrier.controllable_component_id &&
            frame->model_instance_id == carrier.model_instance_id &&
            frame->frame_role_id == carrier.frame_role_id)
          carrier_links.push_back(&frame->compiled_frame_name);
      std::vector<const std::string *> reference_links;
      std::vector<const std::string *> carrier_source_links;
      for (const auto &symbol : model_map->symbols())
        if (symbol.source.model_instance_id == carrier.model_instance_id &&
            symbol.source.kind == deployment::OccurrenceSymbolKind::kLink &&
            symbol.source.scope_name.empty()) {
          if (symbol.source.source_name == reference_link)
            reference_links.push_back(&symbol.compiled_name);
          if (carrier_links.size() == 1U &&
              symbol.compiled_name == *carrier_links.front())
            carrier_source_links.push_back(&symbol.source.source_name);
        }
      if (carrier_links.size() != 1U || reference_links.size() != 1U ||
          carrier_source_links.size() != 1U)
        return refused("scene_runtime_pose_reference_link_unknown",
                       "reference link or carrier frame is not one retained "
                       "model link");
      const auto &planning = bundle->value().compiled_products.planning_collision_model;
      const auto urdf = get_verified(deployment::RetainedProductArtifactQuery{
          retained_bundle_id, planning.product_id, planning.product_kind,
          core::ArtifactRoleId("urdf")});
      if (!urdf.has_value())
        return refused("scene_runtime_pose_reference_unresolved",
                       "retained planning URDF is unavailable");
      auto transform = SceneRuntimeFixedLinkTransform(
          deployment::parse_model_documents_for_projection(urdf->bytes(),
                                                           std::nullopt)
              .urdf,
          *reference_links.front(), *carrier_links.front());
      if (!transform.ok())
        return Reply::Err(transform.error());
      SceneRuntimeReferenceLinkResolution result{
          transform.value(), *carrier_source_links.front()};
      std::lock_guard lock(mutex_);
      if (carrier_reference_poses_.size() >= 16U)
        carrier_reference_poses_.clear();
      carrier_reference_poses_.emplace(key, result);
      return Reply::Ok(std::move(result));
    } catch (const std::exception &error) {
      return Reply::Err(SceneRuntimeFailureFromException(
          "scene_runtime_pose_reference_unresolved", error,
          {{"reference_link", reference_link},
           {"frame_role_id", carrier.frame_role_id.value()},
           {"model_instance_id", carrier.model_instance_id.value()}}));
    }
  }

private:
  std::filesystem::path package_root_;
  std::optional<std::filesystem::path> activation_root_;
  deployment::FilesystemVerifiedRetainedBundleSource bundles_;
  deployment::FilesystemVerifiedRetainedProductArtifactSource artifacts_;
  mutable std::mutex mutex_;
  mutable std::map<std::string,
                   std::shared_ptr<const deployment::RetainedBundle>>
      bundles_by_id_;
  mutable std::vector<std::pair<deployment::RetainedProductArtifactQuery,
                                deployment::VerifiedRetainedProductArtifact>>
      product_artifacts_;
  mutable std::vector<deployment::VerifiedRetainedRuntimeUnitProduct>
      runtime_products_;
  mutable std::map<std::tuple<std::string, std::string, std::string,
                              std::string, std::string>,
                   SceneRuntimeReferenceLinkResolution>
      carrier_reference_poses_;
};

[[nodiscard]] bool HasConvergedKernelApplyOwner(
    const component_manager::ManagerActiveRuntimeConvergenceResult &runtime) {
  if (runtime.state !=
      component_manager::ManagerActiveRuntimeConvergenceState::kConverged) {
    return false;
  }
  std::size_t execution_hosts = 0U;
  for (const auto &row : runtime.plan.runtime_units) {
    if (row.process_adapter_contract_id ==
        deployment::execution_runtime_contract::
            kProcessAdapterContractId) {
      ++execution_hosts;
    }
  }
  return execution_hosts == 1U;
}

class DaemonPlannedMaintenanceGate final
    : public component_manager::IPlannedMaintenanceGate {
public:
  using StateReader =
      std::function<component_manager::ManagementIdleState()>;
  using AdmissionCloser = std::function<bool()>;

  DaemonPlannedMaintenanceGate(StateReader state_reader,
                               AdmissionCloser admission_closer)
      : state_reader_(std::move(state_reader)),
        admission_closer_(std::move(admission_closer)) {}

  [[nodiscard]] component_manager::ManagementIdleState
  ReadIdleState() const override {
    return state_reader_();
  }

  [[nodiscard]] bool CloseAdmissionAndProveClosed() override {
    return admission_closer_();
  }

private:
  StateReader state_reader_;
  AdmissionCloser admission_closer_;
};

// Only the daemon's named coordinator owns this adapter. Its entries
// are called inside the original captured named operation; a replacement
// coordinator has no such capture and can only read retained Scene evidence.
// The actual remote port supplies checked owner readback, never HeldStatusView.
class OriginalManagerSceneMaintenanceAdapter final
    : public core::IManagerSceneProjectionMaintenancePort {
public:
  OriginalManagerSceneMaintenanceAdapter(
      scene::LogicalSceneOwner &owner,
      const core::IManagerKernelApplyFencePort &original_parent_fence)
      : owner_(owner), original_parent_fence_(original_parent_fence) {}

  core::ProjectionRevisionDTO CurrentProjectionRevision() const override {
    return owner_.CurrentProjectionRevision();
  }
  core::ProjectionMaintenanceInitializationStatusDTO
  BeginMaintenanceInitialization(
      const core::ProjectionMaintenanceInitializationRequestDTO &request) override {
    return owner_.BeginMaintenanceInitialization(request);
  }
  core::ProjectionMaintenanceInitializationStatusDTO InstallInitialScene(
      const core::ProjectionMaintenanceInitializationRequestDTO &request) override {
    return owner_.InstallInitialScene(request);
  }
  core::ProjectionMaintenanceInitializationStatusDTO
  CompleteMaintenanceInitialization(
      const core::ProjectionMaintenanceCompletionDTO &completion) override {
    return owner_.CompleteMaintenanceInitialization(completion);
  }
  std::optional<core::ProjectionMaintenanceInitializationStatusDTO>
  ReadMaintenanceInitialization(
      const core::ProjectionMaintenanceInitializationQueryDTO &query) const override {
    return owner_.ReadMaintenanceInitialization(query);
  }
  std::vector<core::ProjectionHealthDTO> StageOriginalManagerProductAdoption(
      const core::QualifiedSceneProductAdoptionRequestDTO &request,
      const core::ApplyFenceAcquireAttemptQueryDTO &attempt_query) override {
    return owner_.StageOriginalManagerProductAdoption(request, original_parent_fence_, attempt_query);
  }
  std::vector<core::ProjectionHealthDTO> ReadOriginalManagerProductAdoptionProjectionHealth(
      const core::QualifiedSceneProductAdoptionRequestDTO &request,
      const core::ApplyFenceAcquireAttemptQueryDTO &attempt_query) override {
    return owner_.ReadOriginalManagerProductAdoptionProjectionHealth(request, original_parent_fence_, attempt_query);
  }
  core::ProjectionMaintenanceInitializationStatusDTO
  BeginOriginalManagerProductAdoption(
      const core::QualifiedSceneProductAdoptionRequestDTO &request) override {
    return owner_.BeginOriginalManagerProductAdoption(request, original_parent_fence_);
  }
  core::ProjectionMaintenanceInitializationStatusDTO
  CompleteOriginalManagerProductAdoption(
      const core::QualifiedSceneProductAdoptionRequestDTO &request,
      const core::ActiveBundlePublicationReceiptDTO &publication) override {
    return owner_.CompleteOriginalManagerProductAdoption(
        request, publication, original_parent_fence_);
  }
  std::vector<core::ProjectionHealthDTO> StageOriginalManagerConfigurationAdoption(
      const core::QualifiedSceneConfigurationAdoptionRequestDTO &request,
      const core::ApplyFenceAcquireAttemptQueryDTO &attempt) override {
    return owner_.StageOriginalManagerConfigurationAdoption(request, original_parent_fence_, attempt);
  }
  std::vector<core::ProjectionHealthDTO> ReadOriginalManagerConfigurationAdoptionProjectionHealth(
      const core::QualifiedSceneConfigurationAdoptionRequestDTO &request,
      const core::ApplyFenceAcquireAttemptQueryDTO &attempt) override {
    return owner_.ReadOriginalManagerConfigurationAdoptionProjectionHealth(request, original_parent_fence_, attempt);
  }
  core::ProjectionMaintenanceInitializationStatusDTO BeginOriginalManagerConfigurationAdoption(
      const core::QualifiedSceneConfigurationAdoptionRequestDTO &request) override {
    return owner_.BeginOriginalManagerConfigurationAdoption(request, original_parent_fence_);
  }
  core::ProjectionMaintenanceInitializationStatusDTO CompleteOriginalManagerConfigurationAdoption(
      const core::QualifiedSceneConfigurationAdoptionRequestDTO &request,
      const core::ActiveBundlePublicationReceiptDTO &publication) override {
    return owner_.CompleteOriginalManagerConfigurationAdoption(request, publication, original_parent_fence_);
  }
  core::ProjectionAggregateHealthDTO ActiveProjectionHealth() const override {
    return owner_.ActiveProjectionHealth();
  }

private:
  scene::LogicalSceneOwner &owner_;
  const core::IManagerKernelApplyFencePort &original_parent_fence_;
};

class ManagerDaemon final {
public:
  explicit ManagerDaemon(DaemonConfig config)
      : config_(ValidateConfig(std::move(config))),
        site_settings_document_(LoadSiteSettingsDocument(config_.site_settings_path)),
        site_facts_(component_manager::SiteFactsOfDocument(site_settings_document_)),
        started_at_(std::chrono::steady_clock::now()),
        package_generation_(config_.install_root,
                            deployment::runtime_contract_declaration_registry()
                                .package_manifest_set()),
        daemon_lock_(config_.manager_run_root),
        admission_(deployment::FilesystemCanonicalEmptyRetainedBundleSource(
                       package_generation_.descriptor_bound_root(),
                       config_.canonical_empty_admission_filename)
                       .load_verified()),
        verified_package_(
            package_generation_.descriptor_bound_root(),
            config_.activation_root.empty()
                ? std::nullopt
                : std::optional(config_.activation_root)),
        execution_host_adapter_(
            InstalledTargetRoot(), package_generation_.descriptor_bound_root(),
            config_.manager_state_root, config_.manager_run_root,
            config_.execution_state_root, config_.execution_run_root,
            verified_package_, verified_package_, "linux",
            std::vector<std::string>{
                std::string(
                    deployment::ros2_control_execution_runtime_contract::
                        kLaunchEnvironmentContract)},
            config_.logical_scene_database, config_.program_cycle_journal,
            config_.activation_root, config_.scene_asset_root,
            site_settings_document_),
        stores_(config_.manager_state_root, admission_),
        scene_owner_(config_.logical_scene_database, admission_,
                     stores_.active_bundle_publication_store(),
                     verified_package_, execution_host_adapter_,
                     execution_host_adapter_),
        original_manager_scene_maintenance_(scene_owner_, execution_host_adapter_),
        scene_runtime_pose_projector_(
            scene_owner_, admission_.cell_id(),
            [this](const core::RetainedBundleId &retained_bundle_id,
                   const core::ComponentSceneFrameRefDTO &carrier,
                   const std::string &reference_link) {
              return verified_package_.CarrierInReferenceLink(
                  retained_bundle_id, carrier, reference_link);
            },
            [this](const SceneRuntimeCarriedObjectLoss &loss) {
              return RequestCarrierStop(loss);
            },
            [this](const SceneRuntimeCarriedObjectLoss &loss) {
              return PublishCarriedObjectLostLevel(
                  loss.controllable_component_id, loss.command_target_id, loss);
            }),
        plan_source_(stores_.active_bundle_publication_store(),
                     stores_.apply_journal(),
                     BuildBootstrapRuntimePlan(admission_, verified_package_)),
        reference_runtime_adapter_(
            config_.execution_state_root, config_.execution_run_root,
            verified_package_, verified_package_, "linux", "posix.1-2017"),
        runtime_adapter_router_(
            {{std::string(
                  component_manager::kRetainedPosixProcessAdapterContractId),
              &reference_runtime_adapter_, &reference_runtime_adapter_},
             {std::string(deployment::execution_runtime_contract::
                              kProcessAdapterContractId),
              &execution_host_adapter_, &execution_host_adapter_}}),
        active_runtime_(
            plan_source_, runtime_adapter_router_, runtime_adapter_router_,
            [this] { return ElapsedNs(); },
            [this] { RequireCurrentPackageGeneration(); }),
        planned_maintenance_gate_(
            [this] { return ReadPlannedMaintenanceState(); },
            [this] { return ClosePlannedMaintenanceAdmission(); }),
        planned_maintenance_(plan_source_, planned_maintenance_gate_),
        manager_runtime_epoch_(RandomToken("manager-runtime-")),
        named_apply_(
            stores_.apply_journal(), plan_source_, verified_package_,
            verified_package_, stores_.active_bundle_publication_store(),
            original_manager_scene_maintenance_, &scene_owner_, execution_host_adapter_,
            execution_host_adapter_, execution_host_adapter_,
            runtime_adapter_router_, runtime_adapter_router_,
            planned_maintenance_,
            manager_runtime_epoch_, "linux",
            {std::string(deployment::ros2_control_execution_runtime_contract::
                             kLaunchEnvironmentContract)},
            [this] { return ElapsedNs(); },
            [] { return core::ApplyId(RandomToken("apply-")); },
            [this](const auto &activation, const auto &bundle) {
              return verified_package_.initial_scene(activation, bundle);
            },
            [this] { RequireCurrentPackageGeneration(); },
            [this](const component_manager::ManagerApplyIntentDTO &) {
              TryPublishApplyTransition();
            },
            [this](const auto &intent, const auto attempt) {
              return PrepareSuccessfulApplySettlement(intent, attempt);
            },
            site_facts_),
        scene_runtime_owner_([] { return HostMonotonicNs(); },
                             [] { return RandomToken("scene-init-"); }),
        manager_channel_(config_.projection_root,
                         std::string(kManagerSourceFileName)),
        scene_channel_(config_.projection_root,
                       std::string(kLogicalSceneSourceFileName)),
        scene_runtime_channel_(config_.projection_root,
                               std::string(kSceneRuntimeSourceFileName)),
        viewer_pose_sessions_channel_(config_.projection_root,
                                      "viewer-pose-sessions.json"),
        manager_owner_epoch_(RandomToken("manager-")),
        scene_runtime_owner_epoch_(RandomToken("scene-runtime-")),
        manager_owner_id_("component-manager." + admission_.cell_id().value()),
        scene_owner_id_("logical-scene." + admission_.cell_id().value()),
        scene_runtime_owner_id_("scene-runtime." +
                                admission_.cell_id().value()) {
    RequireCurrentPackageGeneration();
    EnsureBootstrap();
    RequireCurrentPackageGeneration();
    // Preserve native-named per-unit launch, inventory, epochs, liveness, Stop and fencing plumbing.
    // Native children are directly owned by this Manager process and therefore
    // never survive its restart. Recreate the runtime selected by the durable
    // public pointer before the progression lane resumes an incomplete Apply.
    // Constructor/bootstrap work never advances that Apply: the command socket
    // must be available while its exact durable status is observed. This is the
    // only non-Apply caller permitted to start a managed runtime unit.
    static_cast<void>(InitializeRuntimeAndPublish());
  }

  int Run() {
    CommandSocket command_socket(config_.manager_run_root);
    StartNativeManagerLivenessLane();
    StartApplyProgressLane();
    auto next_reconciliation =
        std::chrono::steady_clock::now() + config_.reconciliation_period;
    try {
      while (g_stop_requested == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_reconciliation) {
          TryPeriodicReconcile();
          next_reconciliation =
              std::chrono::steady_clock::now() + config_.reconciliation_period;
          continue;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                next_reconciliation - now);
        pollfd descriptor{command_socket.get(), POLLIN, 0};
        int ready = -1;
        do {
          ready = ::poll(
              &descriptor, 1,
              static_cast<int>(std::max<std::int64_t>(1, remaining.count())));
        } while (ready < 0 && errno == EINTR && g_stop_requested == 0);
        if (g_stop_requested != 0) {
          break;
        }
        if (ready < 0) {
          FailErrno("poll Manager command socket");
        }
        if (ready == 0) {
          TryPeriodicReconcile();
          next_reconciliation =
              std::chrono::steady_clock::now() + config_.reconciliation_period;
          continue;
        }
        if ((descriptor.revents & POLLIN) != 0 &&
            HandleOneClient(command_socket)) {
          StopApplyProgressLane();
          StopNativeManagerLivenessLane();
          return 0;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          Fail("Manager command socket became unavailable");
        }
      }
    } catch (...) {
      StopApplyProgressLane();
      StopNativeManagerLivenessLane();
      throw;
    }
    StopApplyProgressLane();
    StopNativeManagerLivenessLane();
    return 0;
  }

private:
  struct PublicationGenerations final {
    std::uint64_t manager;
    std::uint64_t scene;
    std::uint64_t scene_runtime;
  };

  struct NativeManagerLivenessSnapshot final {
    component_manager::ManagerActiveRuntimeConvergenceResult runtime;
    std::shared_ptr<const deployment::RetainedBundle> active_bundle;
    std::uint32_t unsettled_effect_count{};
  };

  struct SuccessfulManagerRenewal final {
    NativeManagerLivenessSnapshot snapshot;
    std::uint64_t started_at_ns;
  };

  struct PoseObserverSession final {
    SceneRuntimeIdentity runtime_identity;
    std::string source_epoch;
    std::uint64_t source_epoch_order{};
    std::vector<PoseObserverBinding> bindings;
  };

  struct PoseObservationReplay final {
    SceneRuntimeIdentity runtime_identity;
    std::uint64_t requested_lifetime_ns{};
    PoseSourceObservation observation;
    PoseObservationAcceptance acceptance;
    std::optional<SceneRuntimePoseApplicationResult> application;
    std::optional<std::uint64_t> publication_generation;
  };

  static DaemonConfig ValidateConfig(DaemonConfig config) {
    config.install_root =
        ExistingDirectory(std::move(config.install_root), "install root");
    std::error_code install_root_error;
    config.install_root =
        std::filesystem::canonical(config.install_root, install_root_error);
    Require(!install_root_error && config.install_root.is_absolute(),
            "install root could not be canonicalized");
    if (!config.activation_root.empty()) {
      config.activation_root = ExistingDirectory(
          std::move(config.activation_root),
          "activation root");
      std::error_code activation_root_error;
      config.activation_root = std::filesystem::canonical(
          config.activation_root, activation_root_error);
      Require(!activation_root_error &&
                  config.activation_root.is_absolute(),
              "activation root could not be canonicalized");
      RequireDisjointDirectories(config.install_root,
                                 config.activation_root,
                                 "installed/activation capabilities");
    }
    config.manager_state_root = ExistingPrivateDirectory(
        std::move(config.manager_state_root), "Manager state root");
    config.manager_run_root = ExistingPrivateDirectory(
        std::move(config.manager_run_root), "Manager run root");
    config.execution_state_root = ExistingPrivateDirectory(
        std::move(config.execution_state_root), "execution state root");
    config.execution_run_root = ExistingPrivateDirectory(
        std::move(config.execution_run_root), "execution run root");
    const std::array<const std::filesystem::path *, 4U> typed_roots{
        &config.manager_state_root, &config.manager_run_root,
        &config.execution_state_root, &config.execution_run_root};
    for (std::size_t left = 0U; left < typed_roots.size(); ++left) {
      for (std::size_t right = left + 1U; right < typed_roots.size(); ++right) {
        RequireDisjointDirectories(*typed_roots[left], *typed_roots[right],
                                   "Manager state/run capabilities");
      }
    }
    for (const auto *typed_root : typed_roots) {
      RequireDisjointDirectories(config.install_root, *typed_root,
                                 "install/runtime capabilities");
      if (!config.activation_root.empty()) {
        RequireDisjointDirectories(config.activation_root, *typed_root,
                                   "candidate/runtime capabilities");
      }
    }
    config.projection_root = ExistingPrivateDirectory(
        std::move(config.projection_root), "projection root");
    config.logical_scene_database =
        DatabasePath(std::move(config.logical_scene_database));
    config.program_cycle_journal = ProspectiveDatabasePath(
        std::move(config.program_cycle_journal), "Program-cycle journal");
    const std::array<const std::filesystem::path *, 3U> durable_paths{
        &config.logical_scene_database, &config.program_cycle_journal,
        &config.projection_root};
    const std::array<const std::filesystem::path *, 2U> run_roots{
        &config.manager_run_root, &config.execution_run_root};
    for (const auto *durable_path : durable_paths) {
      for (const auto *run_root : run_roots) {
        RequirePathOutsideDirectory(*durable_path, *run_root,
                                    "Manager durable path/run capability");
      }
    }
    if (!config.program_runner_liveness_socket.empty()) {
      Require(config.program_runner_liveness_socket.is_absolute() &&
                  config.program_runner_liveness_socket.has_filename(),
              "Program Runner liveness socket path must be absolute");
      static_cast<void>(
          ExistingDirectory(config.program_runner_liveness_socket.parent_path(),
                            "Program Runner liveness socket parent"));
      std::error_code error;
      Require(std::filesystem::equivalent(
                  config.program_runner_liveness_socket.parent_path(),
                  config.manager_run_root, error) &&
                  !error,
              "Program Runner liveness socket must belong to the Manager run "
              "capability");
    }
    Require(config.reconciliation_period >= std::chrono::milliseconds(10) &&
                config.reconciliation_period <= std::chrono::seconds(60),
            "reconciliation period must be between 10 ms and 60 s");
    return config;
  }

  std::uint64_t ElapsedNs() const {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - started_at_)
                             .count();
    Require(elapsed >= 0, "monotonic clock moved backwards");
    return static_cast<std::uint64_t>(elapsed);
  }

  /// Scene observation freshness crosses the Manager/native process boundary
  /// on one Linux host. Both receivers therefore use that host's shared
  /// CLOCK_MONOTONIC boot epoch; raw ROS/simulation stamps remain diagnostic
  /// evidence and are never compared with this value.
  static std::int64_t HostMonotonicNs() {
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    Require(now >= 0, "host monotonic clock returned a negative timestamp");
    return now;
  }

  void EnsureBootstrap() {
    const auto journal = stores_.bootstrap_journal().ReadSnapshot();
    const auto access = scene_owner_.ReadAccessStatus();
    if (access.active_projection.has_value()) {
      Require(journal.phase == component_manager::ManagerBootstrapJournalPhase::
                                   kAcceptedTargetPersisted,
              "active scene exists without durable bootstrap acceptance");
      return;
    }
    component_manager::CanonicalEmptyBootstrapCoordinator bootstrap(
        stores_, scene_owner_);
    static_cast<void>(bootstrap.BeginOrRecover(admission_));
    for (int attempt = 0; attempt < kMaximumBootstrapAdvances; ++attempt) {
      const auto status = bootstrap.AdvanceOrRecover(admission_);
      if (status.state == core::ProjectionBootstrapStatusState::kFinalized) {
        return;
      }
    }
    Fail("canonical-empty bootstrap did not finalize within its bounded steps");
  }

  struct FinalizedSceneRuntimeSelection final {
    core::ProjectionRevisionDTO projection_revision;
    core::ActiveBundlePublicationGeneration publication_generation;
  };

  [[nodiscard]] FinalizedSceneRuntimeSelection SelectFinalizedSceneRuntime(
      const core::ActiveBundlePublicationReceiptDTO &active_publication) {
    Require(active_publication.valid(),
            "scene-runtime selection received an invalid active pointer");
    const auto *named = std::get_if<core::NamedApplyPublicationTransactionDTO>(
        &active_publication.request.transaction_ref);
    if (named == nullptr) {
      Require(std::holds_alternative<core::BootstrapPublicationTransactionDTO>(
                  active_publication.request.transaction_ref),
              "scene-runtime active pointer has an unknown transaction");
      return {active_publication.request.candidate_projection_revision,
              active_publication.publication_generation};
    }

    const auto maintenance = scene_owner_.ReadMaintenanceInitialization(
        {named->apply_id, named->apply_generation});
    Require(
        maintenance.has_value() && maintenance->valid() &&
            core::SameProjectionIdentity(
                maintenance->target_projection_revision,
                active_publication.request.candidate_projection_revision) &&
            maintenance->target_projection_revision.retained_bundle_id ==
                active_publication.request.candidate_retained_bundle_id &&
            maintenance->target_projection_revision.graph_revision ==
                active_publication.request.candidate_graph_revision &&
            active_publication.request.expected_previous_publication_generation
                .has_value(),
        "scene-runtime publication lacks its exact Scene-owned operation");

    if (maintenance->state ==
        core::ProjectionMaintenanceInitializationState::kCompleted) {
      Require(
          core::SamePublicationIdentity(maintenance->publication_receipt,
                                        active_publication),
          "Scene completion differs from the active publication");
      return {maintenance->target_projection_revision,
              active_publication.publication_generation};
    }

    Require(
        maintenance->state ==
            core::ProjectionMaintenanceInitializationState::kInitialized,
        "scene-runtime publication belongs to a failed or interrupted "
        "initialization");
    return {
        maintenance->predecessor_projection_revision,
        *active_publication.request.expected_previous_publication_generation};
  }
  /// Observation-only convergence and publication. Used by periodic runtime
  /// observation and the public `reconcile` command. The independent liveness
  /// lane consumes the resulting snapshot; it does not call this method. A
  /// missing required Manager-managed unit is reported as non-converged fault
  /// evidence that closes readiness; this surface never starts or replaces a
  /// process.
  PublicationGenerations ObserveRuntimeAndPublish() {
    RequireCurrentPackageGeneration();
    auto runtime = active_runtime_.Observe();
    if (!ManagerRuntimeAccountsForCommittedApplies(
            runtime, stores_.apply_journal().ReadStatusInventory())) {
      // An Apply published after this observation read the durable pointer.
      // Its pointer is written before its journal receipt, so observing again
      // reads a publication that accounts for every receipt just seen.
      runtime = active_runtime_.Observe();
    }
    return PublishActiveRuntime(std::move(runtime));
  }

  /// Effect-permitted convergence and publication. Restricted to bootstrap and
  /// to an admitted maintenance operation that already holds the exact
  /// operation admission; it may start absent Manager-managed units in
  /// dependency order.
  PublicationGenerations InitializeRuntimeAndPublish() {
    RequireCurrentPackageGeneration();
    return PublishActiveRuntime(active_runtime_.Initialize());
  }

  [[nodiscard]] CanonicalValue ApplyErrorResponseFromJournalEvidence(
      const ManagerCommand &command, const std::exception &error,
      const std::string_view fallback_code) {
    const auto cause = ManagerCauseOf(error, fallback_code);
    try {
      const auto accepted = stores_.apply_journal().ReadRequest(
          core::RequestOwnerEpoch(*command.request_owner_epoch),
          core::RequestId(*command.request_id));
      if (!accepted.has_value()) {
        return WithReferenceDiagnostics(
            ApplyKnownNoEffectDetailedErrorResponse(command, cause.error,
                                                    cause.producer), error);
      }
    } catch (...) {
      // A failed journal observation cannot prove whether acceptance happened.
    }
    return WithReferenceDiagnostics(
        ApplyDetailedErrorResponse(command, cause.error, cause.producer), error);
  }

  // An owned replay copy may outlive a transport call. Never write its result
  // back to a session retired or replaced by the Apply progression lane.
  [[nodiscard]] bool PoseReplayIsCurrentLocked(
      const PoseObservationReplay &record) const {
    const auto runtime = scene_runtime_owner_.Snapshot();
    if (!runtime.runtime_identity ||
        *runtime.runtime_identity != record.runtime_identity)
      return false;
    const auto &observation = record.observation;
    const auto session = pose_observer_sessions_.find(observation.key.source_id);
    const auto replay = pose_observation_replays_.find(observation.key);
    return session != pose_observer_sessions_.end() &&
           session->second.runtime_identity == record.runtime_identity &&
           session->second.source_epoch == observation.source_epoch &&
           session->second.source_epoch_order == observation.source_epoch_order &&
           replay != pose_observation_replays_.end() &&
           replay->second.runtime_identity == record.runtime_identity &&
           replay->second.observation == observation;
  }

  // Only the active bundle's configured bindings, updated after actual pose
  // commits.
  void
  RetainConfiguredPoseProviderOrigin(const PoseSourceObservation &observation) {
    const auto object = scene_owner_.GetObjectState(
        core::SceneResourceId(observation.key.object_id));
    if (!object.provider_evidence || object.attached ||
        object.provider_evidence->provider_source_epoch !=
            SceneRuntimePoseProviderSourceEpoch(observation))
      return;
    std::lock_guard lock(manager_source_mutex_);
    if (IsConfiguredPoseBinding(configured_pose_sources_, observation.key,
                                observation.eligible))
      pose_provider_origins_.insert_or_assign(observation.key,
                                              *object.provider_evidence);
  }

  core::Result<CanonicalValue> ReadComponentProcessStopOffer(
      const core::ControllableComponentId &component) {
    using Offer = core::Result<CanonicalValue>;
    const auto unavailable = [](const std::string &message) {
      return Offer::Err({"component_process_stop_offer_unknown", message, {}});
    };
    const auto active = plan_source_.ReadCurrentPlan();
    std::optional<component_manager::ManagerActiveRuntimeConvergenceResult> runtime;
    {
      std::lock_guard source_lock(manager_source_mutex_);
      runtime = last_runtime_;
    }
    if (!runtime || runtime->plan.active_publication != active.active_publication)
      return unavailable("the actual current runtime observation is unavailable");
    const auto bundle = verified_package_.get_verified(active.active_publication.request.candidate_retained_bundle_id);
    if (!bundle) return unavailable("the actual current publication has no verified host membership");
    CanonicalValue::Array inventories;
    std::size_t count = 0U;
    for (const auto &unit : bundle->value().cell_graph.runtime_units) {
      if (unit.runtime_profile_id.value() != deployment::ros2_control_execution_runtime_contract::kRuntimeProfileId)
        continue;
      const auto *host = runtime->observations.observation(unit.runtime_unit_id.value());
      if (!host || host->process_presence() != component_manager::ProcessPresenceState::kPresent || host->runtime_epoch().empty())
        return unavailable("the actual component host occurrence is unavailable; a new epoch is not absence");
      const auto &publication = active.active_publication;
      const component_manager::ManagerComponentProcessInventoryRequestDTO request{
          publication.request.candidate_projection_revision, publication.publication_generation,
          publication.durable_public_pointer_content_digest, unit.runtime_unit_id,
          core::RuntimeEpoch(host->runtime_epoch()), manager_runtime_epoch_};
      const auto observed = execution_host_adapter_.InspectInstalledComponentProcesses(request);
      if (!observed.ok()) return Offer::Err(observed.error());
      if (!observed.value().valid() || observed.value().request != request)
        return unavailable("the installed process inventory has a foreign original scope");
      if (observed.value().error) return Offer::Err(*observed.value().error);
      CanonicalValue::Array rows;
      for (const auto &row : observed.value().process_occurrences) {
        if (row.controllable_component_id != component) continue;
        if (++count > component_manager::kMaximumComponentProcessInventoryEntries)
          return unavailable("the complete addressed process union exceeds its existing bound");
        if (row.process_owner_kind == component_manager::ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal) {
          const auto verified = component_manager::ValidateManagerPneumaticNoneProcessOwner(
              *bundle, site_facts_, request, row);
          if (!verified.ok()) return Offer::Err(verified.error());
          const auto& proof = *row.pneumatic_none_owner;
          // This is an installed logical owner, not a launch or a drain receipt.
          // Preserve the exact physical row shape below for every physical row.
          rows.emplace_back(CanonicalValue::Object{
              {"controllable_component_id", CanonicalValue(row.controllable_component_id.value())},
              {"process_owner_kind", CanonicalValue("pneumatic_none_local")},
              {"pneumatic_none_owner", CanonicalValue(CanonicalValue::Object{
                  {"command_target_id", CanonicalValue(proof.command_target_id.value())},
                  {"control_binding_id", CanonicalValue(proof.control_binding_id.value())},
                  {"control_binding_revision", CanonicalValue(proof.control_binding_revision.value())}})},
              {"installed_runtime_identity", CanonicalValue(row.installed_runtime_identity->value())},
              {"process_presence", CanonicalValue("absent")}});
          continue;
        }
        const auto state = [&] {
          switch (row.cleanup_state) {
            case core::ControllerLaunchCleanupState::kNotRequested: return "not_requested";
            case core::ControllerLaunchCleanupState::kPending: return "pending";
            case core::ControllerLaunchCleanupState::kUnknown: return "unknown";
            case core::ControllerLaunchCleanupState::kDrained: return "drained";
          }
          Fail("installed process cleanup state is invalid");
        }();
        auto receipt = CanonicalValue(nullptr);
        if (row.receipt) receipt = CanonicalValue(CanonicalValue::Object{
            {"leader_pid", CanonicalValue(static_cast<double>(row.receipt->leader_pid))},
            {"controllable_component_id", CanonicalValue(row.receipt->controllable_component_id)},
            {"launch_identity", CanonicalValue(row.receipt->launch_identity)},
            {"observed_utc", CanonicalValue(row.receipt->observed_utc)}});
        rows.emplace_back(CanonicalValue::Object{
            {"controllable_component_id", CanonicalValue(row.controllable_component_id.value())},
            {"launch_identity", CanonicalValue(row.launch_identity)},
            {"leader_pid", CanonicalValue(static_cast<double>(row.leader_pid))},
            {"process_presence", CanonicalValue(std::string(component_manager::ToString(row.process_presence)))},
            {"cleanup_state", CanonicalValue(state)}, {"receipt", std::move(receipt)},
            {"installed_runtime_identity", row.installed_runtime_identity ? CanonicalValue(row.installed_runtime_identity->value()) : CanonicalValue(nullptr)}});
      }
      inventories.emplace_back(CanonicalValue::Object{
          {"request", CanonicalValue(CanonicalValue::Object{
              {"predecessor_projection_revision", ProjectionRevision(request.predecessor_projection_revision)},
              {"expected_predecessor_publication_generation", Unsigned(request.expected_predecessor_publication_generation.value(), "publication generation")},
              {"expected_predecessor_public_pointer_digest", CanonicalValue(request.expected_predecessor_public_pointer_digest.value())},
              {"runtime_unit_id", CanonicalValue(request.runtime_unit_id.value())},
              {"runtime_epoch", CanonicalValue(request.runtime_epoch.value())},
              {"manager_runtime_epoch", CanonicalValue(request.manager_runtime_epoch.value())}})},
          {"process_occurrences", CanonicalValue(std::move(rows))}, {"error", CanonicalValue(nullptr)}});
      {
        std::lock_guard source_lock(manager_source_mutex_);
        const auto *current = last_runtime_ ? last_runtime_->observations.observation(unit.runtime_unit_id.value()) : nullptr;
        if (!last_runtime_ || last_runtime_->plan.active_publication != active.active_publication ||
            !current || current->process_presence() != component_manager::ProcessPresenceState::kPresent ||
            current->runtime_epoch() != host->runtime_epoch())
          return unavailable("the actual host scope changed during its passive process read");
      }
    }
    if (plan_source_.ReadCurrentPlan().active_publication != active.active_publication)
      return unavailable("the predecessor publication changed during its passive process read");
    return Offer::Ok(CanonicalValue(CanonicalValue::Object{
        {"controllable_component_id", CanonicalValue(component.value())},
        {"process_inventories", CanonicalValue(std::move(inventories))}}));
  }

  PublicationGenerations PublishActiveRuntime(
      component_manager::ManagerActiveRuntimeConvergenceResult runtime) {
    const bool kernel_apply_available = HasConvergedKernelApplyOwner(runtime);
    {
      // The Apply progression lane publishes transitions from these.
      std::lock_guard source_lock(manager_source_mutex_);
      last_runtime_ = runtime;
      kernel_apply_available_ = kernel_apply_available;
    }
    if (kernel_apply_available && !scene_admission_reconciled_.exchange(
                                       true, std::memory_order_acq_rel)) {
      // Consume before I/O: a concurrent failed Commit may mark it dirty
      // again, and must not be overwritten by this successful reconciliation.
      const auto reconciled =
          scene_owner_.ReconcileAuthoredSceneAdmissionClaims();
      if (!reconciled.ok()) {
        scene_admission_reconciled_.store(false, std::memory_order_release);
        const auto warning = reconciled.error().code + ": " +
                             reconciled.error().message.substr(0U, 1024U);
        if (scene_admission_last_warning_ != warning) {
          std::cerr << "Scene admission reconciliation remains closed: "
                    << warning << '\n';
          scene_admission_last_warning_ = warning;
        }
      } else {
        scene_admission_last_warning_.reset();
      }
    }
    const auto bundle_id =
        runtime.plan.active_publication.request.candidate_retained_bundle_id;
    const auto active_bundle = verified_package_.get_verified(bundle_id);
    Require(active_bundle != nullptr,
            "active publication has no independently verified retained bundle");
    {
      std::lock_guard source_lock(manager_source_mutex_);
      if (last_active_bundle_ == nullptr ||
          last_active_bundle_->retained_bundle_id() !=
              active_bundle->retained_bundle_id()) {
        configured_pose_sources_ = ConfiguredPoseSources(*active_bundle);
        std::erase_if(pose_provider_origins_, [&](const auto &origin) {
          return !IsConfiguredPoseBinding(configured_pose_sources_,
                                          origin.first, true);
        });
      }
      last_active_bundle_ = active_bundle;
    }
    const bool all_runtime_processes_present = std::all_of(
        runtime.observations.observations().begin(),
        runtime.observations.observations().end(), [](const auto &observation) {
          return observation.process_presence() ==
                     component_manager::ProcessPresenceState::kPresent &&
                 !observation.runtime_epoch().empty();
        });
    // Runtime observation is deliberately non-creative. A missing or replaced
    // native owner is surfaced by Scene health; it never triggers projection
    // rebuild/recovery outside an explicitly admitted maintenance operation.
    static_cast<void>(all_runtime_processes_present);
    const auto observed_at = static_cast<std::int64_t>(ElapsedNs());
    const auto finalized_scene_runtime =
        SelectFinalizedSceneRuntime(runtime.plan.active_publication);
    const auto finalized_bundle =
        finalized_scene_runtime.projection_revision.retained_bundle_id ==
                bundle_id
            ? active_bundle
            : verified_package_.get_verified(
                  finalized_scene_runtime.projection_revision
                      .retained_bundle_id);
    Require(finalized_bundle != nullptr &&
                finalized_bundle->value().cell_graph.graph_revision ==
                    finalized_scene_runtime.projection_revision.graph_revision,
            "finalized scene-runtime identity has no exact verified bundle");
    const auto publication_generation =
        finalized_scene_runtime.publication_generation.value();
    const SceneRuntimeIdentity desired_scene_runtime{
        finalized_bundle->retained_bundle_id().value(),
        finalized_bundle->value().cell_graph.graph_revision.value(),
        publication_generation,
        0U,
        publication_generation,
    };
    {
      std::lock_guard source_lock(manager_source_mutex_);
      if (scene_runtime_owner_.Snapshot().initialization_generation == 0U) {
        scene_runtime_owner_.EstablishOpenRuntime(
            desired_scene_runtime, publication_generation,
            "durable_finalized_active_projection");
        TryPublishViewerPoseSessionsLocked();
      } else {
        const auto changed = scene_runtime_owner_.AdoptFinalizedOpenRuntime(
            desired_scene_runtime, publication_generation,
            "durable_finalized_active_projection");
        if (changed) {
          pose_observer_sessions_.clear();
          pose_observation_replays_.clear();
          TryPublishViewerPoseSessionsLocked();
        }
      }
    }
    const auto manager = [&] {
      // One lock spans the journal read, the build and the write, so every
      // Manager source value pairs one active publication with the Apply
      // history read after it, and values are written in journal order.
      std::lock_guard source_lock(manager_source_mutex_);
      return manager_channel_.Publish(
          "component_manager", manager_owner_id_, manager_owner_epoch_,
          observed_at,
          BuildManagerSourcePayload(
              *active_bundle, admission_, runtime, stores_.apply_journal(),
              stores_.apply_journal().ReadStatusInventory(),
              kernel_apply_available
                  ? ManagerApplyMutationAvailability::kAvailable
                  : ManagerApplyMutationAvailability::kKernelOwnerUnavailable));
    }();

    const auto scene_generation = PublishLogicalScene(observed_at);
    const auto scene_runtime_generation = PublishSceneRuntime(observed_at);
    const auto apply_inventory = stores_.apply_journal().ReadStatusInventory();
    const auto unsettled = static_cast<std::size_t>(std::count_if(
        apply_inventory.begin(), apply_inventory.end(), [](const auto &status) {
          return !status.terminal_outcome.has_value();
        }));
    Require(unsettled <= std::numeric_limits<std::uint32_t>::max(),
            "unsettled Manager effect count exceeds readiness capacity");
    PublishNativeManagerLivenessSnapshot(
        {runtime, active_bundle, static_cast<std::uint32_t>(unsettled)});
    return {manager.source_generation, scene_generation,
            scene_runtime_generation};
  }

  core::CellAdmissionReadinessSnapshotDTO BuildCellAdmissionReadiness(
      const component_manager::ManagerActiveRuntimeConvergenceResult &runtime,
      const core::ProgramCycleStatusDTO &program_cycle_status,
      const std::uint32_t unsettled_effect_count, const bool forced_closed) {
    // These counts describe runtime-service convergence, not component
    // activation or provider command readiness. Keep the public DTO unchanged.
    const auto runtime_unit_total = runtime.plan.runtime_units.size();
    Require(runtime_unit_total <= std::numeric_limits<std::uint32_t>::max(),
            "required runtime-unit count exceeds readiness capacity");
    std::uint32_t converged_runtime_units = 0U;
    std::uint32_t faulted_runtime_units = 0U;
    for (const auto &row : runtime.plan.runtime_units) {
      const auto *observation =
          runtime.observations.observation(row.runtime_unit_id.value());
      Require(observation != nullptr,
              "required runtime-unit observation is absent");
      if (observation->converged()) {
        ++converged_runtime_units;
      }
      if (observation->source_phase() ==
              component_manager::RuntimeSourcePhase::kFaulted ||
          observation->source_health() ==
              component_manager::RuntimeSourceHealth::kFaulted) {
        ++faulted_runtime_units;
      }
    }
    return ReduceManagerCellAdmissionReadiness({
        !forced_closed,
        program_cycle_status,
        !forced_closed,
        static_cast<std::uint32_t>(runtime_unit_total),
        converged_runtime_units,
        !forced_closed,
        runtime.state ==
            component_manager::ManagerActiveRuntimeConvergenceState::kConverged,
        faulted_runtime_units,
        scene_admission_reconciled_.load(std::memory_order_acquire) ? 0U : 1U,
        unsettled_effect_count,
    });
  }

  [[nodiscard]] component_manager::ManagementIdleState
  ReadPlannedMaintenanceState() const noexcept {
    if (config_.program_runner_liveness_socket.empty()) {
      return component_manager::ManagementIdleState::kUnavailable;
    }
    try {
      const auto status = ProbeProgramRunnerLiveness(
                              config_.program_runner_liveness_socket,
                              admission_.cell_id())
                              .program_cycle_status;
      switch (status.state) {
      case core::ProgramCycleStateDTO::kIdleAfterReset:
        return component_manager::ManagementIdleState::kIdle;
      case core::ProgramCycleStateDTO::kStarting:
        return component_manager::ManagementIdleState::kStarting;
      case core::ProgramCycleStateDTO::kRunning:
        return component_manager::ManagementIdleState::kActive;
      case core::ProgramCycleStateDTO::kStopping:
        return component_manager::ManagementIdleState::kStopping;
      case core::ProgramCycleStateDTO::kTerminalAwaitingReset:
        return component_manager::ManagementIdleState::kTerminalAwaitingReset;
      case core::ProgramCycleStateDTO::kInterventionOrRecovery:
        return component_manager::ManagementIdleState::kInterventionOrRecovery;
      case core::ProgramCycleStateDTO::kResetting:
        return component_manager::ManagementIdleState::kResetting;
      case core::ProgramCycleStateDTO::kRecoveryRequired:
        return component_manager::ManagementIdleState::kRecoveryRequired;
      }
    } catch (...) {
    }
    return component_manager::ManagementIdleState::kUnavailable;
  }

  [[nodiscard]] bool ClosePlannedMaintenanceAdmission() noexcept {
    std::lock_guard attempt_lock(manager_liveness_attempt_mutex_);
    if (!last_submitted_readiness_.has_value()) {
      return false;
    }
    SubmitFailClosedReadiness(last_submitted_readiness_->program_cycle_status);
    return last_submitted_readiness_.has_value() &&
           !last_submitted_readiness_->program_cycle_status_current &&
           !last_submitted_readiness_->required_component_status_current &&
           !last_submitted_readiness_->deployment_status_current;
  }

  void SubmitCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO &snapshot) {
    const auto updated =
        execution_host_adapter_.UpdateCellAdmissionReadiness(snapshot);
    if (!updated.ok()) {
      throw std::runtime_error(
          "Kernel rejected Manager-owned cell-admission readiness: " +
          updated.error().message);
    }
    last_submitted_readiness_ = snapshot;
    if (snapshot.program_cycle_status_current && snapshot.program_cycle_status)
      established_program_cycle_status_ = snapshot.program_cycle_status;
    ClearCarriedObjectLostOnReset(snapshot);
  }

  void RetainRunnerContactFailure(const std::string_view predicate,
                                  const std::string_view message) noexcept {
    try {
      if (program_runner_contact_loss_ || !established_program_cycle_status_)
        return;
      program_runner_contact_loss_ = MakeProgramRunnerContactLoss(
          established_program_cycle_status_,
          core::RequestOwnerEpoch(manager_owner_epoch_), manager_runtime_epoch_,
          predicate, message);
    } catch (...) {
      // Failure reporting cannot terminate the existing renewal lane.
    }
  }

  void SubmitFailClosedReadiness(
      const std::optional<core::ProgramCycleStatusDTO> &status =
          std::nullopt) noexcept {
    try {
      const auto current =
          status.has_value()
              ? status
              : (last_submitted_readiness_.has_value()
                     ? last_submitted_readiness_->program_cycle_status
                     : std::nullopt);
      if (!current.has_value()) {
        return;
      }
      auto closed = last_submitted_readiness_.value_or(
          core::CellAdmissionReadinessSnapshotDTO{});
      closed.program_cycle_status_current = false;
      closed.program_cycle_status = *current;
      closed.required_component_status_current = false;
      closed.deployment_status_current = false;
      closed.program_runner_loss = program_runner_contact_loss_;
      SubmitCellAdmissionReadiness(closed);
    } catch (...) {
      // Kernel readiness remains unavailable. Independent Runner presence
      // renewal must continue even when this publication cannot be delivered.
    }
  }

  [[nodiscard]] static bool SameNativeManagerLivenessIdentity(
      const NativeManagerLivenessSnapshot &left,
      const NativeManagerLivenessSnapshot &right) {
    if (left.active_bundle == nullptr || right.active_bundle == nullptr ||
        left.runtime.plan.active_publication !=
            right.runtime.plan.active_publication ||
        left.active_bundle->retained_bundle_id() !=
            right.active_bundle->retained_bundle_id() ||
        left.runtime.state != right.runtime.state ||
        left.runtime.plan.runtime_units.size() !=
            right.runtime.plan.runtime_units.size()) {
      return false;
    }
    for (const auto &row : left.runtime.plan.runtime_units) {
      const auto right_row = std::find_if(
          right.runtime.plan.runtime_units.begin(),
          right.runtime.plan.runtime_units.end(), [&](const auto &candidate) {
            return candidate.runtime_unit_id == row.runtime_unit_id &&
                   candidate.process_adapter_contract_id ==
                       row.process_adapter_contract_id &&
                   candidate.product_identity == row.product_identity;
          });
      const auto *left_observation =
          left.runtime.observations.observation(row.runtime_unit_id.value());
      const auto *right_observation =
          right.runtime.observations.observation(row.runtime_unit_id.value());
      if (right_row == right.runtime.plan.runtime_units.end() ||
          left_observation == nullptr || right_observation == nullptr ||
          left_observation->runtime_epoch() !=
              right_observation->runtime_epoch()) {
        return false;
      }
    }
    return true;
  }

  void
  PublishNativeManagerLivenessSnapshot(NativeManagerLivenessSnapshot snapshot) {
    if (config_.program_runner_liveness_socket.empty()) {
      return;
    }
    // Verify immutable product policy on the publication lane. Even a cached
    // package lookup shares its lock with cold verification during Apply and
    // must never run on the heartbeat lane.
    const auto &retained_policy = deployment::
        execution_runtime_contract::manager_liveness_policy();
    for (const auto &row : snapshot.runtime.plan.runtime_units) {
      if (row.process_adapter_contract_id !=
          deployment::execution_runtime_contract::
              kProcessAdapterContractId)
        continue;
      const auto product = verified_package_.runtime_unit_product(
          *snapshot.active_bundle, row.runtime_unit_id);
      Require(product.has_value() &&
                  product->retained_bundle_id() ==
                      snapshot.active_bundle->retained_bundle_id() &&
                  product->product_identity() == row.product_identity &&
                  product->manager_liveness_policy().has_value() &&
                  *product->manager_liveness_policy() == retained_policy,
              "active execution host has no exact retained liveness policy");
    }
    bool wake = false;
    {
      std::lock_guard lock(manager_liveness_lane_mutex_);
      wake = !manager_liveness_snapshot_.has_value() ||
             !SameNativeManagerLivenessIdentity(*manager_liveness_snapshot_,
                                                snapshot);
      manager_liveness_snapshot_ = std::move(snapshot);
      manager_liveness_force_closed_ = false;
      if (manager_liveness_superseded_publication_.has_value() &&
          manager_liveness_snapshot_->runtime.plan.active_publication !=
              *manager_liveness_superseded_publication_) {
        manager_liveness_superseded_publication_.reset();
      }
      if (wake) {
        ++manager_liveness_wake_generation_;
      }
    }
    if (wake) {
      manager_liveness_lane_condition_.notify_one();
    }
  }

  void TrySubmitFailClosedReadiness() noexcept {
    if (config_.program_runner_liveness_socket.empty()) {
      return;
    }
    {
      std::lock_guard lock(manager_liveness_lane_mutex_);
      manager_liveness_force_closed_ = true;
      ++manager_liveness_readiness_barrier_;
      ++manager_liveness_wake_generation_;
    }
    manager_liveness_lane_condition_.notify_one();
  }

  /// Renew the independent Program Runner lease and every required native
  /// receiver, publish their observations to the Kernel and reduce one
  /// cell-admission readiness snapshot.
  ///
  /// Traced path: ProbeProgramRunnerLiveness (read-only socket probe) ->
  /// required-set construction from the currently observed epochs ->
  /// RenewProgramRunnerLiveness -> RetainedExecutionHostAdapter
  /// RenewManagerLivenessBatch -> Kernel liveness publication ->
  /// BuildCellAdmissionReadiness. This lane performs no process lifecycle call:
  /// it never reaches the active-runtime coordinator's Observe/Initialize and
  /// therefore never starts a managed unit.
  ///
  /// Detector conclusion: the existing lease/renewal path is the
  /// required-owner-loss detector. A required owner that is missing, foreign or
  /// stale makes the observation lane report non-converged evidence and makes
  /// this renewal throw; every throw path above calls SubmitFailClosedReadiness,
  /// which publishes `required_component_status_current=false` and
  /// `deployment_status_current=false` before returning. No interval was found in
  /// which the Kernel stays open after required-owner evidence has expired, so no
  /// additional progress lease, watchdog or second Stop owner is added here.
  bool TryRenewNativeManagerLiveness(
      const NativeManagerLivenessSnapshot &snapshot, const bool forced_closed,
      const std::uint64_t readiness_barrier, const bool allow_native) noexcept {
    const auto started_at = ElapsedNs();
    bool required_owners_published = false;
    const auto &runtime = snapshot.runtime;
    const auto &active_bundle = *snapshot.active_bundle;
    std::optional<core::ProgramCycleStatusDTO> probed_cycle_status;
    using BatchProgress =
        component_manager::detail::ManagerLivenessBatchProgress;
    auto native_batch = BatchProgress::kNotStarted;
    std::string_view failure_predicate = "program_runner_liveness_probe";
    std::string_view failure_scope = "cell-role:program_runner";
    std::array<char, 256U> failure_runtime_unit{};
    std::size_t failure_runtime_unit_size{};
    const auto track_failure = [&](const std::string_view predicate,
                                   const std::string_view scope,
                                   const std::string_view runtime_unit = {}) noexcept {
      failure_predicate = predicate;
      failure_scope = scope;
      failure_runtime_unit_size =
          std::min(runtime_unit.size(), failure_runtime_unit.size());
      if (failure_runtime_unit_size != 0U)
        std::copy_n(runtime_unit.data(), failure_runtime_unit_size,
                    failure_runtime_unit.data());
    };
    const auto warn_failure = [&](const std::string_view cause) noexcept {
      // Diagnostic identity only, owned by this single renewal lane. Lease
      // counters and retry decisions never consult it. Diagnostic allocation
      // or stream failure must not escape into renewal or terminate its lane.
      const std::string_view runtime_unit(failure_runtime_unit.data(),
                                          failure_runtime_unit_size);
      try {
        std::string warning =
            "Manager native liveness renewal remains fail-closed: cell=";
        warning.append(admission_.cell_id().value().substr(0U, 256U));
        warning.append(" bundle=")
            .append(active_bundle.retained_bundle_id().value().substr(0U, 256U));
        warning.append(" predicate=").append(failure_predicate);
        warning.append(" scope=").append(failure_scope);
        if (!runtime_unit.empty())
          warning.append(" runtime_unit=").append(runtime_unit);
        warning.append(" cause=").append(cause.substr(0U, 512U));
        if (manager_liveness_last_warning_.has_value() &&
            *manager_liveness_last_warning_ == warning)
          return;
        manager_liveness_last_warning_ = std::move(warning);
        std::cerr << *manager_liveness_last_warning_ << '\n';
      } catch (...) {
        try {
          std::cerr << "Manager native liveness renewal remains fail-closed: "
                    << "predicate=" << failure_predicate
                    << " scope=" << failure_scope
                    << " runtime_unit=" << runtime_unit
                    << " cause=" << cause.substr(0U, 512U) << '\n';
        } catch (...) {
          // Logging availability never changes the already recorded failure.
        }
      }
    };
    const auto record_attempt_failure = [&] {
      manager_liveness_retry_gate_.RecordAttemptFailure(
          native_batch, ElapsedNs(),
          static_cast<std::uint64_t>(
              deployment::execution_runtime_contract::
                  manager_liveness_policy()
                      .expiry_timeout_ns));
    };
    try {
      const auto runner = ProbeProgramRunnerLiveness(
          config_.program_runner_liveness_socket, admission_.cell_id());
      probed_cycle_status = runner.program_cycle_status;
      std::vector<core::RequiredManagerLivenessReceiverDTO> native_receivers;
      native_receivers.reserve(runtime.plan.runtime_units.size());
      const auto &retained_policy = deployment::
          execution_runtime_contract::manager_liveness_policy();
      for (const auto &row : runtime.plan.runtime_units) {
        if (row.process_adapter_contract_id !=
            deployment::execution_runtime_contract::
                kProcessAdapterContractId) {
          continue;
        }
        track_failure("required_native_receiver_observable",
                      "execution_host_unit",
                      row.runtime_unit_id.value());
        const auto *observation =
            runtime.observations.observation(row.runtime_unit_id.value());
        if (observation == nullptr || observation->runtime_epoch().empty()) {
          throw std::runtime_error(
              "required native liveness receiver is not observable");
        }
        native_receivers.push_back(core::RequiredManagerLivenessReceiverDTO{
            row.runtime_unit_id,
            core::RuntimeEpoch(observation->runtime_epoch()),
            retained_policy.runtime_policy_id,
            retained_policy.runtime_policy_revision});
      }
      auto receivers = native_receivers;
      receivers.push_back(core::RequiredManagerLivenessReceiverDTO{
          runner.runtime_unit_id, runner.runtime_epoch,
          core::RuntimePolicyId(std::string(kProgramRunnerLivenessPolicyId)),
          core::RuntimePolicyRevision(
              std::string(kProgramRunnerLivenessPolicyRevision))});
      std::sort(receivers.begin(), receivers.end(),
                [](const auto &left, const auto &right) {
                  return std::tie(left.runtime_unit_id, left.runtime_epoch,
                                  left.runtime_policy_id,
                                  left.runtime_policy_revision) <
                         std::tie(right.runtime_unit_id, right.runtime_epoch,
                                  right.runtime_policy_id,
                                  right.runtime_policy_revision);
                });
      track_failure("required_runtime_set_valid", "cell");
      const auto digest = core::ComputeRequiredRuntimeSetDigest(receivers);
      const core::ManagerLivenessRequiredSetDTO required_set{
          admission_.cell_id(),
          active_bundle.retained_bundle_id(),
          digest,
          core::RuntimeUnitId("component-manager." +
                              admission_.cell_id().value()),
          manager_runtime_epoch_,
          core::RequestOwnerEpoch(manager_owner_epoch_),
          receivers};
      if (!core::ValidateManagerLivenessRequiredSet(required_set).ok() ||
          manager_heartbeat_sequence_ >= core::kMaximumManagerLivenessCounter) {
        throw std::runtime_error(
            "Manager cannot mint the exact native liveness required set");
      }
      ++manager_heartbeat_sequence_;
      // Renew the independent Runner before any native retry/backoff or
      // readiness work. Apply may deliberately retire a native receiver;
      // that is not evidence that this Manager process has disappeared.
      track_failure("program_runner_liveness_renewal", "cell-role:program_runner",
                    runner.runtime_unit_id.value());
      auto runner_renewal = RenewProgramRunnerLiveness(
          config_.program_runner_liveness_socket, runner, required_set,
          manager_heartbeat_sequence_, std::nullopt);
      probed_cycle_status = runner_renewal.program_cycle_status;
      track_failure("cell_admission_readiness_reduction", "cell");
      if (!allow_native ||
          !manager_liveness_retry_gate_.attempt_allowed(ElapsedNs())) {
        SubmitFailClosedReadiness(probed_cycle_status);
        return required_owners_published;
      }
      // A replacement Runner still reports its predecessor's durable cycle
      // until the reconciliation renewal below succeeds. Keep admission
      // closed, but do not prevent the renewal that establishes ownership.
      if (!IsCurrentProgramRunnerOwner(*probed_cycle_status,
                                       runner.runtime_epoch)) {
        SubmitFailClosedReadiness(probed_cycle_status);
      } else {
        const auto probed_readiness = BuildCellAdmissionReadiness(
            runtime, *probed_cycle_status, snapshot.unsettled_effect_count,
            forced_closed);
        if (last_submitted_readiness_.has_value() &&
            *last_submitted_readiness_ != probed_readiness &&
            !IsSameProgramAdmissionProgress(*last_submitted_readiness_,
                                            probed_readiness)) {
          SubmitFailClosedReadiness(probed_cycle_status);
          Require(last_submitted_readiness_.has_value() &&
                      !last_submitted_readiness_->program_cycle_status_current,
                  "Kernel could not be closed before a readiness transition");
        }
      }
      const core::ManagerHeartbeatDTO heartbeat{
          required_set.cell_id,
          required_set.manager_runtime_unit_id,
          required_set.manager_runtime_epoch,
          required_set.manager_owner_epoch,
          manager_heartbeat_sequence_,
          retained_policy.runtime_policy_id,
          retained_policy.runtime_policy_revision};
      std::optional<core::ManagerLivenessReconciliationAck> reconciliation;
      if (runtime.state ==
          component_manager::ManagerActiveRuntimeConvergenceState::kConverged) {
        std::string material("manyforge.manager-liveness-reconciliation.v1");
        material.push_back('\0');
        material.append(active_bundle.retained_bundle_id().value());
        material.push_back('\0');
        material.append(digest.value());
        material.push_back('\0');
        material.append(manager_runtime_epoch_.value());
        material.push_back('\0');
        material.append(manager_owner_epoch_);
        for (const auto &observation : runtime.observations.observations()) {
          material.push_back('\0');
          material.append(observation.runtime_unit_id());
          material.push_back('\0');
          material.append(observation.runtime_epoch());
          material.push_back('\0');
          material.append(
              std::to_string(observation.source_report_sequence().value_or(0)));
        }
        reconciliation = core::ManagerLivenessReconciliationAck{
            required_set.cell_id,
            required_set.manager_runtime_unit_id,
            required_set.manager_runtime_epoch,
            required_set.manager_owner_epoch,
            required_set.active_bundle_id,
            required_set.required_runtime_set_digest,
            core::ContentDigest(content_identity::sha256_digest(material))};
      }
      std::vector<component_manager::ManagerLivenessTransportRequestDTO>
          requests;
      requests.reserve(native_receivers.size());
      for (const auto &receiver : native_receivers) {
        requests.push_back(
            {receiver.runtime_unit_id,
             runtime.plan.active_publication.publication_generation,
             runtime.plan.active_publication.request.candidate_graph_revision,
             runtime.plan.active_publication
                 .durable_public_pointer_content_digest,
             required_set, retained_policy, heartbeat, reconciliation});
      }
      if (!requests.empty())
        native_batch = BatchProgress::kInFlight;
      track_failure("native_liveness_batch", "required_native_receiver_batch");
      const auto responses =
          requests.empty()
              ? std::vector<core::ManagerLivenessObservationDTO>{}
              : execution_host_adapter_.RenewManagerLivenessBatch(requests);
      if (responses.size() != native_receivers.size()) {
        throw std::runtime_error(
            "native liveness batch returned the wrong receiver count");
      }
      for (std::size_t index = 0U; index < native_receivers.size(); ++index) {
        const auto &receiver = native_receivers[index];
        const auto &response = responses[index];
        track_failure("native_receiver_reconciliation_identity",
                      "execution_host_unit",
                      receiver.runtime_unit_id.value());
        if (response.active_bundle_id != required_set.active_bundle_id ||
            response.required_runtime_set_digest != digest ||
            response.status.receiver_runtime_unit_id !=
                receiver.runtime_unit_id ||
            response.status.receiver_runtime_epoch != receiver.runtime_epoch ||
            response.status.manager_runtime_epoch != manager_runtime_epoch_ ||
            response.status.manager_owner_epoch !=
                core::RequestOwnerEpoch(manager_owner_epoch_) ||
            response.status.runtime_policy_id != receiver.runtime_policy_id ||
            response.status.runtime_policy_revision !=
                receiver.runtime_policy_revision ||
            response.status.last_accepted_heartbeat_sequence !=
                manager_heartbeat_sequence_ ||
            (reconciliation.has_value() &&
             (response.status.state !=
                  core::ManagerLivenessStateDTO::kCurrent ||
              response.status.command_fenced))) {
          throw std::runtime_error(
              "native liveness receiver returned foreign reconciliation "
              "evidence");
        }
      }
      native_batch = BatchProgress::kValidated;
      track_failure("runtime_reconciliation_converged", "cell");
      if (!reconciliation.has_value()) {
        throw std::runtime_error(
            "Execution readiness requires converged reconciliation");
      }
      if (runner_renewal.observation.status.state !=
          core::ManagerLivenessStateDTO::kCurrent) {
        track_failure("program_runner_reconciliation_sequence_available",
                      "cell-role:program_runner",
                      runner.runtime_unit_id.value());
        Require(manager_heartbeat_sequence_ <
                    core::kMaximumManagerLivenessCounter,
                "Manager heartbeat sequence is exhausted");
        track_failure("program_runner_reconciliation_renewal",
                      "cell-role:program_runner",
                      runner.runtime_unit_id.value());
        runner_renewal = RenewProgramRunnerLiveness(
            config_.program_runner_liveness_socket, runner, required_set,
            ++manager_heartbeat_sequence_,
            reconciliation->reconciliation_evidence_digest);
      }
      // The Program Runner owns cycle progression. A Start or terminal event
      // may legitimately occur between the preflight probe and this renewal;
      // require the private receiver's exact process identity and publish the
      // newer owner-issued status instead of treating ordinary progress as a
      // liveness failure.
      track_failure("program_runner_cycle_owner_current",
                    "cell-role:program_runner",
                    runner.runtime_unit_id.value());
      Require(IsCurrentProgramRunnerOwner(runner_renewal.program_cycle_status,
                                          runner.runtime_epoch),
              "Program cycle is not issued by the current Program Runner");
      probed_cycle_status = runner_renewal.program_cycle_status;
      track_failure("kernel_program_runner_liveness_publication",
                    "cell-role:program_runner",
                    runner.runtime_unit_id.value());
      const auto published_runner =
          execution_host_adapter_.Publish(runner_renewal.observation);
      if (!published_runner.ok()) {
        throw std::runtime_error(
            "Kernel rejected Program Runner liveness observation: " +
            published_runner.error().message);
      }
      // The native rows and this actual Runner row now make the exact
      // successor renewal available to the existing Kernel owner guard.
      // Subsequent readiness reduction/publication is not a settlement gate.
      required_owners_published = true;
      successful_manager_renewal_ = SuccessfulManagerRenewal{snapshot, started_at};
      track_failure("cell_admission_readiness_reduction", "cell");
      const auto readiness = BuildCellAdmissionReadiness(
          runtime, *probed_cycle_status, snapshot.unsettled_effect_count,
          forced_closed);
      // Renewal and owner identity are now validated. Publish the complete
      // reduction atomically; an artificial close/reopen here would expose a
      // false outage to already-running Program children.
      if (!last_submitted_readiness_.has_value() ||
          *last_submitted_readiness_ != readiness) {
        track_failure("readiness_snapshot_current", "cell");
        {
          std::lock_guard lock(manager_liveness_lane_mutex_);
          Require(forced_closed || (readiness_barrier ==
                                        manager_liveness_readiness_barrier_ &&
                                    !manager_liveness_force_closed_),
                  "Manager liveness snapshot changed during renewal");
        }
        track_failure("kernel_cell_readiness_publication", "cell");
        SubmitCellAdmissionReadiness(readiness);
      }
      manager_liveness_retry_gate_.RecordSuccess();
      manager_liveness_last_warning_.reset();
    } catch (const component_manager::ManagerLivenessRuntimeTransition &) {
      successful_manager_renewal_.reset();
      // Runtime replacement is already fenced by Apply. No native heartbeat
      // was sent, so this is ordinary convergence rather than an uncertain
      // renewal. Keep readiness closed until the next exact snapshot arrives.
      SubmitFailClosedReadiness(probed_cycle_status);
      manager_liveness_retry_gate_.RecordSuccess();
    } catch (const component_manager::ManagerLivenessPublicationSuperseded &) {
      successful_manager_renewal_.reset();
      SubmitFailClosedReadiness(probed_cycle_status);
      {
        std::lock_guard lock(manager_liveness_lane_mutex_);
        if (!manager_liveness_snapshot_.has_value() ||
            !SameNativeManagerLivenessIdentity(snapshot,
                                               *manager_liveness_snapshot_)) {
          return required_owners_published;
        }
        manager_liveness_superseded_publication_ =
            snapshot.runtime.plan.active_publication;
      }
      // Every native receiver rejected before renewal and the adapter drained
      // the complete batch.  Preserve fail-closed readiness, but do not spend
      // the lease-expiry retry budget on this expected publication race.
      manager_liveness_retry_gate_.RecordSuccess();
    } catch (const detail::ProgramRunnerPublisherFault &error) {
      successful_manager_renewal_.reset();
      // A responding Runner is present. Its independent publisher has failed;
      // reuse required-owner containment without manufacturing process death.
      track_failure("program_runner_publication_fault",
                    "cell-role:program_runner");
      const auto &cycle = error.cycle();
      if (established_program_cycle_status_ &&
          cycle.program_runner_owner_epoch ==
              established_program_cycle_status_->program_runner_owner_epoch &&
          cycle.program_run_id ==
              established_program_cycle_status_->program_run_id &&
          cycle.cycle_claim_generation ==
              established_program_cycle_status_->cycle_claim_generation) {
        RetainRunnerContactFailure(failure_predicate, error.what());
      }
      SubmitFailClosedReadiness(probed_cycle_status);
      record_attempt_failure();
      warn_failure(error.what());
    } catch (const std::exception &error) {
      if (failure_predicate == "program_runner_liveness_probe" ||
          failure_predicate == "program_runner_liveness_renewal" ||
          failure_predicate == "program_runner_reconciliation_renewal")
        RetainRunnerContactFailure(failure_predicate, error.what());
      SubmitFailClosedReadiness(probed_cycle_status);
      {
        std::lock_guard lock(manager_liveness_lane_mutex_);
        if (manager_liveness_snapshot_.has_value() &&
            !SameNativeManagerLivenessIdentity(snapshot,
                                               *manager_liveness_snapshot_)) {
          return required_owners_published;
        }
      }
      record_attempt_failure();
      warn_failure(error.what());
    } catch (...) {
      if (failure_predicate == "program_runner_liveness_probe" ||
          failure_predicate == "program_runner_liveness_renewal" ||
          failure_predicate == "program_runner_reconciliation_renewal")
        RetainRunnerContactFailure(failure_predicate, "Program Runner contact failed");
      SubmitFailClosedReadiness(probed_cycle_status);
      {
        std::lock_guard lock(manager_liveness_lane_mutex_);
        if (manager_liveness_snapshot_.has_value() &&
            !SameNativeManagerLivenessIdentity(snapshot,
                                               *manager_liveness_snapshot_)) {
          return required_owners_published;
        }
      }
      record_attempt_failure();
      warn_failure("unknown exception");
    }
    return required_owners_published;
  }

  bool SuccessfulRenewalIsCurrent(
      const NativeManagerLivenessSnapshot &snapshot) const {
    if (!successful_manager_renewal_ || program_runner_contact_loss_ ||
        !SameNativeManagerLivenessIdentity(successful_manager_renewal_->snapshot, snapshot))
      return false;
    const auto now = ElapsedNs();
    const auto started = successful_manager_renewal_->started_at_ns;
    // Conservative reuse of the real lease: every receipt follows attempt
    // start. The final Kernel guard remains the authority for actual health.
    return now >= started && now - started < static_cast<std::uint64_t>(
        deployment::execution_runtime_contract::manager_liveness_policy().expiry_timeout_ns);
  }

  bool RenewCurrentNativeManagerLiveness(
      const component_manager::ManagerApplyIntentDTO *expected = nullptr,
      const bool reuse_only = false) {
    std::lock_guard attempt_lock(manager_liveness_attempt_mutex_);
    const auto [snapshot, superseded, forced_closed, barrier] = [&] {
      std::lock_guard lock(manager_liveness_lane_mutex_);
      return std::tuple{manager_liveness_snapshot_, manager_liveness_superseded_publication_,
                        manager_liveness_force_closed_, manager_liveness_readiness_barrier_};
    }();
    if (!snapshot) {
      if (!reuse_only) SubmitFailClosedReadiness();
      return false;
    }
    const auto &publication = snapshot->runtime.plan.active_publication;
    if (expected &&
        (publication.request.transaction_ref != core::ActiveBundlePublicationTransactionRefDTO{
             core::NamedApplyPublicationTransactionDTO{expected->apply_id, expected->apply_generation}} ||
         publication.request.candidate_retained_bundle_id != expected->candidate_retained_bundle_id ||
         publication.request.candidate_graph_revision != expected->candidate_graph_revision))
      return false;
    if (expected && SuccessfulRenewalIsCurrent(*snapshot)) return true;
    if (reuse_only) return false;
    if (!manager_liveness_attempted_publication_ ||
        *manager_liveness_attempted_publication_ != publication) {
      // The SAME publication-boundary reset covers heartbeat and initial
      // settlement, before the existing retry gate can skip a native attempt.
      manager_liveness_retry_gate_.RecordSuccess();
      manager_liveness_attempted_publication_ = publication;
    }
    const bool allow_native = !superseded || publication != *superseded;
    const bool published = TryRenewNativeManagerLiveness(*snapshot, forced_closed,
                                                        barrier, allow_native);
    return published && !program_runner_contact_loss_;
  }

  bool PrepareSuccessfulApplySettlement(
      const component_manager::ManagerApplyIntentDTO &intent,
      const component_manager::ManagerApplySettlementAttempt attempt) {
    if (config_.program_runner_liveness_socket.empty()) return true;
    if (RenewCurrentNativeManagerLiveness(&intent, true)) return true;
    if (attempt == component_manager::ManagerApplySettlementAttempt::kOrdinaryHeartbeat)
      return false;  // No second attempt: use only this ordinary heartbeat.
    try {
      // I/O stays outside renewal serialization and all host/Core locks.
      static_cast<void>(ObserveRuntimeAndPublish());
      return RenewCurrentNativeManagerLiveness(&intent);
    } catch (const std::exception &error) {
      std::cerr << "Manager Apply successor observation remains pending: " << error.what() << '\n';
    } catch (...) {
      std::cerr << "Manager Apply successor observation remains pending after an unknown error\n";
    }
    return false;
  }

  void StartNativeManagerLivenessLane() {
    if (config_.program_runner_liveness_socket.empty()) {
      return;
    }
    Require(!manager_liveness_lane_.joinable(),
            "Manager liveness renewal lane is already running");
    {
      std::lock_guard lock(manager_liveness_lane_mutex_);
      manager_liveness_lane_stop_ = false;
    }
    manager_liveness_lane_ =
        std::thread([this] { RunNativeManagerLivenessLane(); });
  }

  void StopNativeManagerLivenessLane() noexcept {
    if (!manager_liveness_lane_.joinable()) {
      return;
    }
    {
      std::lock_guard lock(manager_liveness_lane_mutex_);
      manager_liveness_lane_stop_ = true;
    }
    manager_liveness_lane_condition_.notify_one();
    manager_liveness_lane_.join();
  }

  void RunNativeManagerLivenessLane() noexcept {
    const auto heartbeat_period = std::chrono::nanoseconds(
        deployment::execution_runtime_contract::manager_liveness_policy().heartbeat_period_ns);
    std::unique_lock lock(manager_liveness_lane_mutex_);
    bool ordinary_heartbeat = true;
    auto next_heartbeat = std::chrono::steady_clock::now() + heartbeat_period;
    while (!manager_liveness_lane_stop_) {
      if (ordinary_heartbeat)
        next_heartbeat = std::chrono::steady_clock::now() + heartbeat_period;
      const auto wake_generation = manager_liveness_wake_generation_;
      lock.unlock();
      static_cast<void>(RenewCurrentNativeManagerLiveness());
      // Only tag the actual pending Apply with this ordinary heartbeat.
      // The existing progress owner performs release RPCs; this independent
      // lease lane never waits behind that owner's operation or transport.
      try {
        if (ordinary_heartbeat) {
          if (const auto pending = named_apply_.PendingSettlementForHeartbeat())
            RequestApplySettlementProgress(*pending);
        }
      } catch (const std::exception &error) {
        std::cerr << "Manager Apply heartbeat settlement stopped fail-closed: "
                  << error.what() << '\n';
      } catch (...) {
        std::cerr << "Manager Apply heartbeat settlement stopped fail-closed after an unknown error\n";
      }
      lock.lock();
      const bool woken = manager_liveness_lane_condition_.wait_until(
          lock, next_heartbeat, [&] {
            return manager_liveness_lane_stop_ ||
                   manager_liveness_wake_generation_ != wake_generation;
          });
      // Keep this SAME ordinary deadline through extra publication/control
      // wakes: they may renew leases, but cannot starve the timed retry.
      ordinary_heartbeat = !woken;
    }
  }

  std::uint64_t PublishLogicalScene(const std::int64_t observed_at) {
    const auto access = scene_owner_.ReadAccessStatus();
    const auto snapshot = scene_owner_.ReadActiveSnapshot();
    Require(snapshot.has_value(),
            "Manager daemon cannot publish an absent post-bootstrap scene");
    const auto scene_epoch = scene_owner_.CurrentRequestOwnerEpoch().value();
    return scene_channel_
        .Publish("logical_scene", scene_owner_id_, scene_epoch, observed_at,
                 BuildLogicalSceneSourcePayload(access, snapshot))
        .source_generation;
  }

  void TryPublishLogicalScene() noexcept {
    try {
      static_cast<void>(
          PublishLogicalScene(static_cast<std::int64_t>(ElapsedNs())));
    } catch (const std::exception &error) {
      std::cerr << "Manager daemon scene publication failed after durable "
                   "commit: "
                << error.what() << '\n';
    } catch (...) {
      std::cerr << "Manager daemon scene publication failed after durable "
                   "commit with an unknown error\n";
    }
  }

  void TryPublishViewerPoseSessionsLocked() noexcept {
    try {
      const auto state = SceneRuntimeStateValue(scene_runtime_owner_.Snapshot());
      const auto *identity = state.find_object_member("runtime_identity");
      Require(identity != nullptr, "scene-runtime identity is missing");
      CanonicalValue::Array sessions;
      for (const auto &[source_id, session] : pose_observer_sessions_) {
        sessions.emplace_back(CanonicalValue::Object{
            {"source_id", CanonicalValue(source_id)},
            {"source_epoch", CanonicalValue(session.source_epoch)},
            {"source_epoch_order", Unsigned(session.source_epoch_order,
                                             "source_epoch_order")},
        });
      }
      static_cast<void>(viewer_pose_sessions_channel_.Publish(
          "scene_runtime", scene_runtime_owner_id_, scene_runtime_owner_epoch_,
          static_cast<std::int64_t>(ElapsedNs()),
          CanonicalValue(CanonicalValue::Object{
              {"runtime_identity", *identity},
              {"sessions", CanonicalValue(std::move(sessions))},
          })));
    } catch (const std::exception &error) {
      std::cerr << "Manager viewer pose session publication failed: "
                << error.what() << '\n';
    } catch (...) {
      std::cerr << "Manager viewer pose session publication failed with an "
                   "unknown error\n";
    }
  }

  std::uint64_t PublishSceneRuntime(const std::int64_t observed_at) {
    const auto runtime_snapshot = scene_runtime_owner_.Snapshot();
    auto payload = BuildSceneRuntimeSourcePayload(
        admission_.cell_id().value(), runtime_snapshot);
    {
      std::lock_guard lock(manager_source_mutex_);
      auto payload_object =
          std::get<CanonicalValue::Object>(payload.storage());
      const auto state = std::find_if(
          payload_object.begin(), payload_object.end(), [](const auto &field) {
            return field.first == "state";
          });
      if (state != payload_object.end() &&
          runtime_snapshot.runtime_identity) {
        auto state_object =
            std::get<CanonicalValue::Object>(state->second.storage());
        const auto sources = std::find_if(
            state_object.begin(), state_object.end(), [](const auto &field) {
              return field.first == "pose_sources";
            });
        if (sources != state_object.end()) {
          auto rows =
              std::get<CanonicalValue::Array>(sources->second.storage());
          const auto text_field = [](const CanonicalValue::Object &object,
                                     const std::string_view name)
              -> const std::string * {
            const auto field = std::find_if(
                object.begin(), object.end(), [&](const auto &entry) {
                  return entry.first == name;
                });
            return field != object.end() && field->second.is_string()
                       ? &field->second.as_string()
                       : nullptr;
          };
          const auto number_field = [](const CanonicalValue::Object &object,
                                       const std::string_view name)
              -> const double * {
            const auto field = std::find_if(
                object.begin(), object.end(), [&](const auto &entry) {
                  return entry.first == name;
                });
            return field == object.end()
                       ? nullptr
                       : std::get_if<double>(&field->second.storage());
          };
          for (auto &source : rows) {
            if (!source.is_object())
              continue;
            auto row = source.as_object();
            const auto *source_id = text_field(row, "source_id");
            const auto *object_id = text_field(row, "object_id");
            const auto *policy_id = text_field(row, "policy_id");
            const auto *source_epoch = text_field(row, "source_epoch");
            const auto *source_epoch_order =
                number_field(row, "source_epoch_order");
            const auto *source_sequence = number_field(row, "source_sequence");
            if (source_id == nullptr || object_id == nullptr ||
                policy_id == nullptr || source_epoch == nullptr ||
                source_epoch_order == nullptr || source_sequence == nullptr)
              continue;
            const PoseObservationKey key{*source_id, *object_id, *policy_id};
            const auto replay = pose_observation_replays_.find(key);
            if (replay == pose_observation_replays_.end())
              continue;
            const auto &observation = replay->second.observation;
            if (replay->second.runtime_identity !=
                    *runtime_snapshot.runtime_identity ||
                observation.source_epoch != *source_epoch ||
                static_cast<double>(observation.source_epoch_order) !=
                    *source_epoch_order ||
                static_cast<double>(observation.source_sequence) !=
                    *source_sequence ||
                !replay->second.application ||
                !replay->second.application->valid() ||
                !replay->second.application->pose_measurement)
              continue;
            AppendSceneRuntimePoseMeasurement(
                row, *replay->second.application->pose_measurement);
            source = CanonicalValue(std::move(row));
          }
          sources->second = CanonicalValue(std::move(rows));
        }
        state->second = CanonicalValue(std::move(state_object));
      }
      CanonicalValue::Array joint_states;
      for (const auto &source : configured_pose_sources_) {
        if (!source.joint_state)
          continue;
        joint_states.emplace_back(CanonicalValue::Object{
            {"object_id", CanonicalValue(source.joint_state->object_id)},
            {"policy_id", CanonicalValue(source.joint_state->policy_id)},
            {"source_id",
             CanonicalValue(source.controllable_component_id.value())},
            {"topic", CanonicalValue(source.joint_state->topic)},
        });
      }
      payload_object.emplace_back("configured_joint_states",
                                  CanonicalValue(std::move(joint_states)));
      payload = CanonicalValue(std::move(payload_object));
    }
    return scene_runtime_channel_
        .Publish("scene_runtime", scene_runtime_owner_id_,
                 scene_runtime_owner_epoch_, observed_at, std::move(payload))
        .source_generation;
  }

  void TryPublishApplyTransition() noexcept {
    try {
      const auto observed_at = static_cast<std::int64_t>(ElapsedNs());
      {
        // Called from the command strand and the Apply progression lane.
        std::lock_guard source_lock(manager_source_mutex_);
        if (!last_runtime_.has_value() || last_active_bundle_ == nullptr) {
          return;
        }
        const auto apply_inventory =
            stores_.apply_journal().ReadStatusInventory();
        // The last observed runtime predates this Apply. Once the Apply has
        // published, pairing them would show its committed record beside the
        // predecessor publication. Keep the previous value (the Apply still
        // in progress) until the command strand publishes the next observed
        // runtime, which holds the new publication.
        if (ManagerRuntimeAccountsForCommittedApplies(*last_runtime_,
                                                      apply_inventory)) {
          static_cast<void>(manager_channel_.Publish(
              "component_manager", manager_owner_id_, manager_owner_epoch_,
              observed_at,
              BuildManagerSourcePayload(
                  *last_active_bundle_, admission_, *last_runtime_,
                  stores_.apply_journal(), apply_inventory,
                  kernel_apply_available_
                      ? ManagerApplyMutationAvailability::kAvailable
                      : ManagerApplyMutationAvailability::
                            kKernelOwnerUnavailable)));
        }
      }
      static_cast<void>(PublishLogicalScene(observed_at));
      static_cast<void>(PublishSceneRuntime(observed_at));
    } catch (const std::exception &error) {
      std::cerr << "Manager daemon Apply-transition publication failed: "
                << error.what() << '\n';
    } catch (...) {
      std::cerr << "Manager daemon Apply-transition publication failed with "
                   "an unknown error\n";
    }
  }

  void TryPeriodicReconcile() noexcept {
    try {
      RequireCurrentPackageGeneration();
      // Apply progression has one dedicated owner so slow projection work
      // cannot occupy the command/status strand. While it remains open, keep
      // readiness closed and leave ordinary active reconciliation alone.
      if (HasIncompleteApply()) {
        TrySubmitFailClosedReadiness();
        TryPublishApplyTransition();
        RequestApplyProgress();
        return;
      }
      // Steady-state observation. A required managed unit that is absent is
      // contained fault evidence: it closes readiness and is never started or
      // replaced here. Only an explicit admitted maintenance operation may
      // initialize it again.
      static_cast<void>(ObserveRuntimeAndPublish());
    } catch (const ManagerPackageGenerationRestartRequired &error) {
      TrySubmitFailClosedReadiness();
      PublishPackageRestartRequired();
      std::cerr << "Manager daemon package generation changed: " << error.what()
                << '\n';
    } catch (const std::exception &error) {
      TrySubmitFailClosedReadiness();
      std::cerr << "Manager daemon periodic reconciliation failed: "
                << error.what() << '\n';
    } catch (...) {
      TrySubmitFailClosedReadiness();
      std::cerr << "Manager daemon periodic reconciliation failed with an "
                   "unknown error\n";
    }
  }

  [[nodiscard]] bool HasIncompleteApply() {
    for (const auto &status : stores_.apply_journal().ReadStatusInventory()) {
      if (!status.terminal_outcome.has_value()) {
        return true;
      }
    }
    return false;
  }

  void StartApplyProgressLane() {
    Require(!apply_progress_lane_.joinable(),
            "Manager Apply progression lane is already running");
    {
      std::lock_guard lock(apply_progress_lane_mutex_);
      apply_progress_lane_stop_ = false;
      apply_progress_requested_ = true;
      apply_settlement_retry_requested_.reset();
      apply_progress_faulted_ = false;
    }
    apply_progress_lane_ = std::thread([this] { RunApplyProgressLane(); });
  }

  void StopApplyProgressLane() noexcept {
    if (!apply_progress_lane_.joinable()) {
      return;
    }
    {
      std::lock_guard lock(apply_progress_lane_mutex_);
      apply_progress_lane_stop_ = true;
    }
    apply_progress_lane_condition_.notify_one();
    apply_progress_lane_.join();
  }

  void RequestApplyProgress() noexcept {
    {
      std::lock_guard lock(apply_progress_lane_mutex_);
      if (apply_progress_lane_stop_ || apply_progress_faulted_) {
        return;
      }
      apply_progress_requested_ = true;
    }
    apply_progress_lane_condition_.notify_one();
  }

  void RequestApplySettlementProgress(
      const core::NamedApplyPublicationTransactionDTO &expected) {
    {
      std::lock_guard lock(apply_progress_lane_mutex_);
      if (apply_progress_lane_stop_ || apply_progress_faulted_) return;
      // One coalesced scheduling reference, never a replacement journal fact.
      apply_settlement_retry_requested_ = expected;
    }
    apply_progress_lane_condition_.notify_one();
  }

  void RunApplyProgressLane() noexcept {
    CollectUnreferencedActivationContent();
    std::unique_lock lock(apply_progress_lane_mutex_);
    while (!apply_progress_lane_stop_) {
      apply_progress_lane_condition_.wait(lock, [&] {
        return apply_progress_lane_stop_ || apply_progress_requested_ ||
               apply_settlement_retry_requested_.has_value();
      });
      if (apply_progress_lane_stop_) {
        break;
      }
      const bool progress_requested = apply_progress_requested_;
      const auto settlement_requested = std::move(apply_settlement_retry_requested_);
      apply_progress_requested_ = false;
      apply_settlement_retry_requested_.reset();
      lock.unlock();

      bool pending = false;
      bool succeeded = false;
      const auto note_success =
          [&](const std::optional<component_manager::ManagerNamedApplyResult> &result) {
            succeeded = succeeded ||
                        (result.has_value() &&
                         result->state ==
                             component_manager::ManagerNamedApplyResultState::kSucceeded);
          };
      try {
        if (settlement_requested)
          note_success(named_apply_.RetryPendingSettlementOnHeartbeat(*settlement_requested));
        if (progress_requested) {
          const auto result = named_apply_.RecoverIncompleteApply();
          pending = result.has_value() &&
                    result->state ==
                        component_manager::ManagerNamedApplyResultState::kPending &&
                    !result->settlement_pending;
          note_success(result);
        }
        if (succeeded)
          CollectUnreferencedActivationContent();
      } catch (const std::exception &error) {
        std::cerr << "Manager daemon Apply progression stopped fail-closed: "
                  << error.what() << '\n';
        lock.lock();
        apply_progress_faulted_ = true;
        continue;
      } catch (...) {
        std::cerr << "Manager daemon Apply progression stopped fail-closed "
                     "after an unknown failure\n";
        lock.lock();
        apply_progress_faulted_ = true;
        continue;
      }

      lock.lock();
      if (pending && !apply_progress_lane_stop_) {
        apply_progress_lane_condition_.wait_for(
            lock, config_.reconciliation_period,
            [&] {
              return apply_progress_lane_stop_ || apply_progress_requested_;
            });
        if (!apply_progress_lane_stop_) {
          apply_progress_requested_ = true;
        }
      }
    }
  }

  /// Delete activation-root content no live configuration references (its
  /// stores are bounded and refuse when full). Live are the current
  /// publication's bundle, Program and binding and the bundle it replaced.
  /// Runs once when the Apply lane starts and after each successful Apply, on
  /// that lane: Applies progress one at a time there, so no publication
  /// changes underneath. A failure is logged; the content stays until the
  /// next collection.
  void CollectUnreferencedActivationContent() noexcept {
    if (config_.activation_root.empty())
      return;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed_ms = [&] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - started)
          .count();
    };
    try {
      const auto publication = plan_source_.ReadCurrentPlan().active_publication;
      deployment::ActiveActivationContent active{
          {publication.request.candidate_retained_bundle_id}, std::nullopt,
          std::nullopt};
      if (const auto *named = std::get_if<core::NamedApplyPublicationTransactionDTO>(
              &publication.request.transaction_ref)) {
        const auto intent = stores_.apply_journal().ReadIntent(
            named->apply_id, named->apply_generation);
        Require(intent.has_value(), "active publication has no durable Apply intent");
        active.retained_bundle_ids.push_back(
            intent->intent.expected_active_projection_revision.retained_bundle_id);
        const auto &prepared = intent->intent.prepared_activation;
        active.program_binding_id = prepared.program_binding_id;
        const auto receipt = content_identity::parse_canonical_json(
            prepared.canonical_receipt_json,
            {component_manager::kMaximumManagerApplyPreparedReceiptBytes, 16U,
             4096U});
        if (const auto *revision = receipt.find_object_member("program_revision");
            revision != nullptr && revision->is_string())
          active.program_revision = core::ProgramRevision(revision->as_string());
      }
      const auto collected = deployment::collect_unreferenced_activation_content(
          config_.activation_root, active);
      // One line per run, also when nothing is removed or a preparation
      // deferred it: its duration is time this lane holds the writer lock.
      std::cerr << "Manager activation content collection: "
                << (collected.collected ? "collected" : "deferred")
                << " examined=" << collected.examined_entries
                << " removed=" << collected.removed_entries
                << " removed_bytes=" << collected.removed_bytes
                << " read_bytes=" << collected.read_bytes
                << " elapsed_ms=" << elapsed_ms() << '\n';
    } catch (const std::exception &error) {
      std::cerr << "Manager activation content collection skipped after "
                << elapsed_ms() << " ms: " << error.what() << '\n';
    }
  }

  void RequireCurrentPackageGeneration() {
    package_generation_.RequireCurrent(
        deployment::runtime_contract_declaration_registry()
            .package_manifest_set());
  }

  void PublishPackageRestartRequired() noexcept {
    std::lock_guard source_lock(manager_source_mutex_);
    kernel_apply_available_ = false;
    if (!last_runtime_.has_value() || last_active_bundle_ == nullptr) {
      return;
    }
    try {
      const auto apply_inventory =
          stores_.apply_journal().ReadStatusInventory();
      if (!ManagerRuntimeAccountsForCommittedApplies(*last_runtime_,
                                                     apply_inventory)) {
        // The last observed runtime predates a committed Apply; its value
        // would be inconsistent. Readers keep the previous value.
        return;
      }
      static_cast<void>(manager_channel_.Publish(
          "component_manager", manager_owner_id_, manager_owner_epoch_,
          static_cast<std::int64_t>(ElapsedNs()),
          BuildManagerSourcePayload(
              *last_active_bundle_, admission_, *last_runtime_, stores_.apply_journal(),
              apply_inventory,
              ManagerApplyMutationAvailability::kFullStackRestartRequired)));
    } catch (const std::exception &publication_error) {
      std::cerr << "Manager daemon could not publish package-generation "
                   "failure: "
                << publication_error.what() << '\n';
    } catch (...) {
      std::cerr << "Manager daemon could not publish package-generation "
                   "failure\n";
    }
  }

  // The Deployment object link is the only policy; a binding without a link
  // row is not an object link and is never applied.
  static std::string_view
  EffectivePosePolicy(const deployment::RetainedBundle &bundle,
                      const PoseObservationKey &key) {
    const auto &bindings = bundle.value().scene_pose_sources;
    const auto selected =
        std::find_if(bindings.begin(), bindings.end(), [&](const auto &row) {
          return row.source_id == key.source_id &&
                 row.scene_resource_id == key.object_id;
        });
    return selected == bindings.end()
               ? std::string_view("observe_only")
               : std::string_view(selected->apply_policy);
  }

  static SceneRuntimePoseDeadband
  PoseDeadband(const deployment::RetainedBundle &bundle,
               const PoseObservationKey &key) {
    const auto &bindings = bundle.value().scene_pose_sources;
    const auto selected =
        std::find_if(bindings.begin(), bindings.end(), [&](const auto &row) {
          return row.source_id == key.source_id &&
                 row.scene_resource_id == key.object_id;
        });
    return selected == bindings.end()
               ? SceneRuntimePoseDeadband{}
               : SceneRuntimePoseDeadband{selected->deadband_translation_m,
                                          selected->deadband_rotation_rad};
  }

  // Requests the normal operational Stop of the carrier's motion through the
  // Kernel's Stop control socket, as the public Stop route does.
  // It names the holder and every component it is mounted on, from the
  // active bundle's cell graph, so a loss during an arm move stops the arm.
  // A component with no running motion is a no-effect Stop.
  std::optional<core::ErrorDTO>
  RequestCarrierStop(const SceneRuntimeCarriedObjectLoss &loss) {
    constexpr std::string_view source = "manager.carried_object_lost";
    try {
      std::shared_ptr<const deployment::RetainedBundle> bundle;
      {
        std::lock_guard source_lock(manager_source_mutex_);
        bundle = last_active_bundle_;
      }
      Require(bundle != nullptr,
              "carried-object-lost Stop has no active bundle");
      CanonicalValue::Array components;
      for (const auto &component : SceneRuntimeCarrierStopComponents(
               bundle->value().cell_graph, loss.controllable_component_id))
        components.push_back(CanonicalValue(CanonicalValue::Object{
            {"controllable_component_id", CanonicalValue(component.value())},
            {"expected_component_lifecycle_generation", CanonicalValue(1.0)},
            {"expected_graph_revision",
             CanonicalValue(loss.graph_revision.value())}}));
      const auto response = ExchangeLocalPacket(
          config_.execution_run_root / "execution-control.sock",
          CanonicalValue(CanonicalValue::Object{
              {"api_version", CanonicalValue("manyforge.execution_command.v3")},
              {"attribution",
               CanonicalValue(CanonicalValue::Object{
                   {"assurance", CanonicalValue("unverified")},
                   {"principal_id", CanonicalValue(nullptr)},
                   {"transport", CanonicalValue("internal")}})},
              {"command", CanonicalValue("stop")},
              {"request",
               CanonicalValue(CanonicalValue::Object{
                   {"components", CanonicalValue(std::move(components))},
                   {"reason", CanonicalValue(std::string(
                                  core::kCarriedObjectLostReason))},
                   {"request_context",
                    CanonicalValue(CanonicalValue::Object{
                        {"correlation_id",
                         CanonicalValue(
                             loss.carried_object_attachment_id.value())},
                        {"request_id", CanonicalValue(loss.request_id)},
                        // The Manager does not track the Kernel's owner
                        // epoch: the Kernel records and journals a Stop with
                        // a foreign epoch under its own current epoch.
                        {"request_owner_epoch", CanonicalValue(std::string(source))},
                        {"source_label", CanonicalValue(std::string(source))}})}})}}),
          "Kernel Stop control", {kMaximumKernelStopResponseBytes, 32U});
      const auto *ok = response.find_object_member("ok");
      if (ok != nullptr && std::get_if<bool>(&ok->storage()) != nullptr &&
          std::get<bool>(ok->storage())) {
        return std::nullopt;
      }
      const auto *error = response.find_object_member("error");
      const auto *code =
          error != nullptr ? error->find_object_member("code") : nullptr;
      return core::ErrorDTO{
          code != nullptr && code->is_string() ? code->as_string()
                                               : "carrier_stop_refused",
          "Kernel refused the carried-object-lost Stop", {}};
    } catch (const std::exception &error) {
      return core::ErrorDTO{"carrier_stop_unavailable", error.what(), {}};
    }
  }

  // Publishes one gripper's `carried_object_lost.<target>` system signal level
  // into the execution host's signal ingress, on the same control socket and
  // envelope as the loss Stop. `loss` (attachment, object,
  // loss event id, stored key) is present iff the level is `true`. The
  // host records the level even while no composition holds the signal.
  std::optional<core::ErrorDTO> PublishCarriedObjectLostLevel(
      const core::ControllableComponentId &component,
      const core::CommandTargetId &target,
      const std::optional<SceneRuntimeCarriedObjectLoss> &loss) {
    try {
      const auto response = ExchangeLocalPacket(
          config_.execution_run_root / "execution-control.sock",
          CanonicalValue(CanonicalValue::Object{
              {"api_version", CanonicalValue("manyforge.execution_command.v3")},
              {"attribution",
               CanonicalValue(CanonicalValue::Object{
                   {"assurance", CanonicalValue("unverified")},
                   {"principal_id", CanonicalValue(nullptr)},
                   {"transport", CanonicalValue("internal")}})},
              {"command", CanonicalValue("set_carried_object_lost")},
              {"request",
               CanonicalValue(CanonicalValue::Object{
                   {"carried_object_attachment_id",
                    loss ? CanonicalValue(loss->carried_object_attachment_id.value())
                         : CanonicalValue(nullptr)},
                   {"command_target_id", CanonicalValue(target.value())},
                   {"controllable_component_id",
                    CanonicalValue(component.value())},
                   {"level", CanonicalValue(loss.has_value())},
                   {"loss_event_id", loss ? CanonicalValue(loss->loss_event_id)
                                          : CanonicalValue(nullptr)},
                   {"scene_resource_id",
                    loss ? CanonicalValue(loss->scene_resource_id.value())
                         : CanonicalValue(nullptr)},
                   {"set_key", loss && loss->set_key
                                   ? CanonicalValue(*loss->set_key)
                                   : CanonicalValue(nullptr)}})}}),
          "Kernel carried-object-lost signal",
          {kMaximumKernelStopResponseBytes, 32U});
      const auto *ok = response.find_object_member("ok");
      if (ok == nullptr || !ok->is_bool() || !ok->as_bool()) {
        const auto *error = response.find_object_member("error");
        const auto *code =
            error != nullptr ? error->find_object_member("code") : nullptr;
        return core::ErrorDTO{code != nullptr && code->is_string()
                                  ? code->as_string()
                                  : "carried_object_lost_signal_refused",
                              "execution host refused the signal level",
                              {}};
      }
      // Recorded; a composition that does not hold the signal yet reports it.
      const auto *publication = response.find_object_member("publication_error");
      const auto *code = publication != nullptr && publication->is_object()
                             ? publication->find_object_member("code")
                             : nullptr;
      if (code != nullptr && code->is_string())
        return core::ErrorDTO{code->as_string(),
                              "signal level recorded but not yet delivered",
                              {}};
      return std::nullopt;
    } catch (const std::exception &error) {
      return core::ErrorDTO{"carried_object_lost_signal_unavailable",
                            error.what(), {}};
    }
  }

  // `false` for one gripper: its next attach, a Program detach, or Reset.
  // Best effort; a refusal is logged (the host keeps the last level).
  void ClearCarriedObjectLost(const core::ControllableComponentId &component,
                              const core::CommandTargetId &target) noexcept {
    try {
      if (const auto refused =
              PublishCarriedObjectLostLevel(component, target, std::nullopt);
          refused && refused->code != "io_signal_service_unavailable")
        std::cerr << "carried-object-lost signal was not cleared for "
                  << component.value() << "/" << target.value() << ": "
                  << refused->code << '\n';
    } catch (...) {
    }
  }

  // Program scene commits that replace a gripper's attachment clear its
  // signal: an attach by that gripper, a Program detach.
  void ClearCarriedObjectLostAfterCommit(
      const core::SceneTransactionRequestDTO &transaction,
      const std::optional<std::pair<core::ControllableComponentId,
                                    core::CommandTargetId>> &detached) {
    if (const auto *attach = std::get_if<core::AttachCarriedObjectRequestDTO>(
            &transaction.operation)) {
      ClearCarriedObjectLost(attach->controllable_component_id,
                             attach->command_target_id);
    } else if (detached) {
      ClearCarriedObjectLost(detached->first, detached->second);
    }
  }

  // The holder of the attachment a detach request names, read before commit.
  std::optional<std::pair<core::ControllableComponentId, core::CommandTargetId>>
  DetachedHolder(const core::SceneTransactionRequestDTO &transaction) {
    std::optional<core::CarriedObjectAttachmentId> id;
    if (const auto *detach = std::get_if<core::DetachCarriedObjectRequestDTO>(
            &transaction.operation))
      id = detach->carried_object_attachment_id;
    else if (const auto *here =
                 std::get_if<core::DetachCarriedObjectAtCurrentPoseRequestDTO>(
                     &transaction.operation))
      id = here->carried_object_attachment_id;
    if (!id)
      return std::nullopt;
    const auto snapshot = scene_owner_.LogicalSnapshot();
    if (!snapshot)
      return std::nullopt;
    for (const auto &row : snapshot->carried_object_attachments)
      if (row.carried_object_attachment_id == *id)
        return std::pair{row.controllable_component_id, row.command_target_id};
    return std::nullopt;
  }

  // Cell Reset clears every gripper's signal of the active bundle.
  void ClearCarriedObjectLostOnReset(
      const core::CellAdmissionReadinessSnapshotDTO &snapshot) noexcept {
    try {
      if (!snapshot.program_cycle_status_current ||
          !snapshot.program_cycle_status ||
          !snapshot.program_cycle_status->accepted_reset)
        return;
      const auto &reset = *snapshot.program_cycle_status->accepted_reset;
      if (cleared_reset_request_id_ == reset.request_id.value())
        return;
      cleared_reset_request_id_ = reset.request_id.value();
      const auto bundle = verified_package_.get_verified(
          scene_owner_.CurrentProjectionRevision().retained_bundle_id);
      if (bundle == nullptr)
        return;
      const auto catalog =
          deployment::ResolveCompiledIoSignalCatalog(bundle->value().cell_graph);
      if (!catalog.ok())
        return;
      for (const auto &row : catalog.value())
        if (row.provider.profile == deployment::kSystemCarriedObjectProfile)
          ClearCarriedObjectLost(row.descriptor.signal_id.controllable_component_id,
                                 row.command_target_id);
    } catch (...) {
    }
  }

  core::Result<SceneRuntimePoseApplicationResult>
  ResolvePoseApplication(const SceneRuntimeIdentity &runtime_identity,
                         const PoseSourceObservation &observation,
                         const PoseObservationAcceptance &acceptance) {
    const auto no_application =
        [](const SceneRuntimePoseApplicationStatus status) {
          return core::Result<SceneRuntimePoseApplicationResult>::Ok(
              SceneRuntimePoseApplicationResult{status, std::nullopt,
                                                std::nullopt, std::nullopt,
                                                std::nullopt});
        };
    if (!observation.eligible ||
        observation.payload_quality != PoseSourceQuality::kGood || !acceptance.fresh) {
      return no_application(SceneRuntimePoseApplicationStatus::kNotRequested);
    }
    std::shared_ptr<const deployment::RetainedBundle> bundle;
    try {
      bundle = verified_package_.get_verified(
          core::RetainedBundleId(runtime_identity.retained_bundle_id));
    } catch (const std::exception &error) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          SceneRuntimeFailureFromException("scene_runtime_pose_bundle_unavailable", error));
    }
    if (bundle == nullptr ||
        bundle->retained_bundle_id().value() !=
            runtime_identity.retained_bundle_id ||
        bundle->value().cell_graph.graph_revision.value() !=
            runtime_identity.graph_revision ||
        bundle->value().cell_graph.cell_id != admission_.cell_id()) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          core::ErrorDTO{
              "scene_runtime_pose_bundle_unavailable",
              "scene-runtime identity has no exact verified active bundle",
              {}});
    }
    const auto policy = EffectivePosePolicy(*bundle, observation.key);
    if (policy == "observe_only")
      return no_application(SceneRuntimePoseApplicationStatus::kNotRequested);
    if (policy == "before_planning" || policy == "between_nodes") {
      return no_application(
          policy == "between_nodes"
              ? SceneRuntimePoseApplicationStatus::kDeferredBetweenNodes
              : SceneRuntimePoseApplicationStatus::kDeferredBeforePlanning);
    }
    if (acceptance.disposition == PoseObservationDisposition::kBuffered ||
        acceptance.disposition ==
            PoseObservationDisposition::kReplacedBuffered) {
      return no_application(SceneRuntimePoseApplicationStatus::kBuffered);
    }
    if (policy != "continuous_unattached" && policy != "continuous" &&
        policy != "continuous_relative_to_attached") {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          core::ErrorDTO{"scene_runtime_pose_policy_unsupported",
                         "eligible pose observation uses an unsupported "
                         "application policy",
                         {{"policy_id", std::string(policy)}}});
    }
    // A link to an object the Program does not declare (orphan) is ignored.
    if (!scene_owner_
             .GetObjectState(core::SceneResourceId(observation.key.object_id))
             .exists)
      return no_application(SceneRuntimePoseApplicationStatus::kNotRequested);
    const auto owner = scene_runtime_owner_.Snapshot();
    if (owner.state != SceneInitializationState::kOpen ||
        (acceptance.disposition != PoseObservationDisposition::kApplyNow &&
         acceptance.disposition != PoseObservationDisposition::kDuplicate)) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          core::ErrorDTO{
              "scene_runtime_pose_owner_not_open",
              "scene-runtime owner is not open for continuous pose "
              "application",
              {{"initialization_state", std::string(ToString(owner.state))}}});
    }

    auto application = scene_runtime_pose_projector_.Apply(
        bundle->value().capability_provider_bindings,
        bundle->retained_bundle_id(), bundle->value().cell_graph.graph_revision,
        observation, std::nullopt, policy,
        PoseDeadband(*bundle, observation.key));
    RecordCarriedObjectLoss(application, observation.key.object_id);
    return application;
  }

  void RecordCarriedObjectLoss(
      const core::Result<SceneRuntimePoseApplicationResult> &application,
      const std::string &object_id) {
    if (application.ok() ||
        application.error().code != core::kCarriedObjectLostReason)
      return;
    // Handled by the projector per `on_grasp_lost`; publish the scene and the
    // event for the operator alarm (`raise_alarm`).
    const auto &details = application.error().details;
    const auto set_key = details.find("set_key");
    const auto signal_error = details.find("signal_error");
    scene_runtime_owner_.RecordCarriedObjectLoss(
        {details.at("event_id"), object_id, application.error().message,
         details.at("carried_object_attachment_id"),
         details.at("controllable_component_id"),
         details.at("command_target_id"),
         core::GraspLostResponseDTO{
             details.at("raise_alarm") == "true",
             details.at("stop_motion") == "true",
             set_key == details.end() ? std::nullopt
                                      : std::optional(set_key->second),
             details.at("detach") == "true"},
         signal_error == details.end() ? std::nullopt
                                       : std::optional(signal_error->second)});
    TryPublishLogicalScene();
  }

  core::Result<BeforePlanningLatchResult> LatchBeforePlanningObservations(
      const SceneRuntimeIdentity &runtime_identity,
      const core::ProjectionRevisionDTO &expected_projection,
      const std::string_view latch_policy = "before_planning",
      const std::vector<core::SceneResourceId> &resources = {}) {
    const auto selected_resource = [&](const std::string &id) {
      return resources.empty() ||
             std::binary_search(resources.begin(), resources.end(),
                                core::SceneResourceId(id));
    };
    const auto failure = [](std::string code, std::string message,
                            std::map<std::string, std::string> details = {}) {
      return core::Result<BeforePlanningLatchResult>::Err(core::ErrorDTO{
          std::move(code), std::move(message), std::move(details)});
    };
    auto owner = scene_runtime_owner_.Snapshot();
    if (owner.state != SceneInitializationState::kOpen ||
        !owner.runtime_identity.has_value() ||
        *owner.runtime_identity != runtime_identity) {
      return failure(
          "scene_runtime_pose_owner_not_open",
          "scene-runtime owner is not open for before-planning pose latching",
          {{"initialization_state", std::string(ToString(owner.state))}});
    }
    const auto initial_projection = scene_owner_.CurrentProjectionRevision();
    if (initial_projection != expected_projection ||
        initial_projection.retained_bundle_id.value() !=
            runtime_identity.retained_bundle_id ||
        initial_projection.graph_revision.value() !=
            runtime_identity.graph_revision) {
      return failure("scene_runtime_pose_projection_changed",
                     "logical-scene projection changed before pose latching");
    }

    std::shared_ptr<const deployment::RetainedBundle> bundle;
    try {
      bundle = verified_package_.get_verified(
          core::RetainedBundleId(runtime_identity.retained_bundle_id));
    } catch (const std::exception &error) {
      return core::Result<BeforePlanningLatchResult>::Err(
          SceneRuntimeFailureFromException("scene_runtime_pose_bundle_unavailable", error));
    }
    if (bundle == nullptr ||
        bundle->retained_bundle_id().value() !=
            runtime_identity.retained_bundle_id ||
        bundle->value().cell_graph.graph_revision.value() !=
            runtime_identity.graph_revision ||
        bundle->value().cell_graph.cell_id != admission_.cell_id()) {
      return failure(
          "scene_runtime_pose_bundle_unavailable",
          "scene-runtime identity has no exact verified active bundle");
    }

    std::vector<ConfiguredPoseSource> configured_sources;
    try {
      configured_sources = ConfiguredPoseSources(*bundle);
    } catch (const std::exception &error) {
      return core::Result<BeforePlanningLatchResult>::Err(
          SceneRuntimeFailureFromException("scene_runtime_pose_bundle_unavailable", error));
    }
    struct LatchCandidate final {
      PoseObservationReplay *replay{};
      PoseSourceObservation observation;
      std::string apply_policy;
    };
    std::vector<LatchCandidate> candidates;
    std::map<std::string, PoseObserverSession> sessions;
    std::map<PoseObservationKey, PoseObservationReplay> replays;
    std::map<PoseObservationKey, core::SceneObjectProviderEvidenceDTO>
        provider_origins;
    {
      std::lock_guard lock(manager_source_mutex_);
      sessions = pose_observer_sessions_;
      replays = pose_observation_replays_;
      provider_origins = pose_provider_origins_;
    }
    for (const auto &source : configured_sources) {
      for (const auto &binding : source.bindings) {
        if (!binding.eligible || !selected_resource(binding.object_id))
          continue;
        core::SceneObjectStateDTO object{
            core::SceneResourceId(binding.object_id), false, false,
            std::nullopt, std::nullopt};
        try {
          object = scene_owner_.GetObjectState(
              core::SceneResourceId(binding.object_id));
        } catch (const std::exception &error) {
          return core::Result<BeforePlanningLatchResult>::Err(
              SceneRuntimeFailureFromException("scene_runtime_pose_scene_unavailable", error));
        }
        if (!object.exists ||
            (object.attached && latch_policy == "before_planning"))
          continue;
        const PoseObservationKey key{source.controllable_component_id.value(),
                                     binding.object_id, binding.policy_id};
        const auto policy = EffectivePosePolicy(*bundle, key);
        bool depends_on_source = policy == latch_policy;
        if ((policy == "continuous_unattached" || policy == "continuous" ||
             policy == "continuous_relative_to_attached") &&
            object.provider_evidence) {
          const auto origin = provider_origins.find(key);
          depends_on_source =
              origin != provider_origins.end() &&
              object.provider_evidence->provider_instance_id ==
                  origin->second.provider_instance_id &&
              object.provider_evidence->provider_manifest_id ==
                  origin->second.provider_manifest_id &&
              object.provider_evidence->provider_manifest_revision ==
                  origin->second.provider_manifest_revision &&
              object.provider_evidence->provider_source_epoch ==
                  origin->second.provider_source_epoch;
        }
        if (!depends_on_source)
          continue;
        const auto session = sessions.find(key.source_id);
        if (session == sessions.end() ||
            session->second.runtime_identity != runtime_identity)
          return failure(
              "scene_runtime_pose_observation_missing",
              "required configured binding has no current observer session",
              {{"source_id", key.source_id}, {"object_id", key.object_id}});
        const auto observation =
            std::find_if(owner.pose_sources.begin(), owner.pose_sources.end(),
                         [&](const PoseSourceLifetimeSnapshot &pose) {
                           return pose.observation.key == key;
                         });
        if (observation == owner.pose_sources.end() || !observation->fresh ||
            observation->buffered || !observation->observation.eligible ||
            observation->observation.payload_quality != PoseSourceQuality::kGood ||
            observation->observation.source_epoch !=
                session->second.source_epoch ||
            observation->observation.source_epoch_order !=
                session->second.source_epoch_order)
          return failure(
              "scene_runtime_pose_observation_stale",
              "source-dependent planning requires a fresh current-runtime "
              "observation",
              {{"source_id", key.source_id}, {"object_id", key.object_id}});
        if (policy == "continuous_unattached" || policy == "continuous" ||
            policy == "continuous_relative_to_attached") {
          const auto replay = replays.find(key);
          // The current fresh sample whose application did not complete (for
          // example a carried-object loss that detached the object at this
          // observed pose) is applied now, so planning sees it.
          if (replay != replays.end() &&
              replay->second.runtime_identity == runtime_identity &&
              replay->second.observation == observation->observation &&
              !replay->second.application) {
            candidates.push_back(LatchCandidate{
                &replay->second, observation->observation, std::string(policy)});
            continue;
          }
          if (replay == replays.end() ||
              replay->second.runtime_identity != runtime_identity ||
              replay->second.observation != observation->observation ||
              !replay->second.application ||
              !object.provider_evidence ||
              !SceneRuntimePoseApplicationRetainsPlanningEvidence(
                  replay->second.application->status))
            return failure(
                "scene_runtime_pose_observation_unretained",
                "continuous planning evidence has no successful exact pose "
                "application",
                {{"source_id", key.source_id}, {"object_id", key.object_id}});
        }
      }
    }

    for (const auto &[source_id, session] : sessions) {
      if (session.runtime_identity != runtime_identity) {
        continue;
      }
      for (const auto &binding : session.bindings) {
        if (!binding.eligible || !selected_resource(binding.object_id) ||
            EffectivePosePolicy(*bundle, {source_id, binding.object_id,
                                          binding.policy_id}) != latch_policy) {
          continue;
        }
        if (std::any_of(configured_sources.begin(), configured_sources.end(),
                        [&](const auto &source) {
                          return source.controllable_component_id.value() ==
                                 source_id;
                        })) {
          const auto object = scene_owner_.GetObjectState(
              core::SceneResourceId(binding.object_id));
          if (!object.exists ||
              (object.attached && latch_policy == "before_planning"))
            continue;
        }
        const PoseObservationKey key{source_id, binding.object_id,
                                     binding.policy_id};
        const auto observed =
            std::find_if(owner.pose_sources.begin(), owner.pose_sources.end(),
                         [&](const PoseSourceLifetimeSnapshot &candidate) {
                           return candidate.observation.key == key;
                         });
        if (observed == owner.pose_sources.end() ||
            observed->observation.source_epoch != session.source_epoch ||
            observed->observation.source_epoch_order !=
                session.source_epoch_order) {
          return failure(
              "scene_runtime_pose_observation_missing",
              "before-planning pose source has no observation for its active "
              "epoch",
              {{"object_id", binding.object_id}, {"source_id", source_id}});
        }
        if (!observed->fresh || observed->buffered ||
            !observed->observation.eligible ||
            observed->observation.payload_quality != PoseSourceQuality::kGood) {
          return failure(
              "scene_runtime_pose_observation_stale",
              "before-planning pose source has no fresh eligible observation",
              {{"object_id", binding.object_id}, {"source_id", source_id}});
        }
        const auto replay = replays.find(key);
        if (replay == replays.end() ||
            replay->second.runtime_identity != runtime_identity ||
            replay->second.observation != observed->observation) {
          return failure(
              "scene_runtime_pose_observation_unretained",
              "before-planning pose observation has no exact replay record",
              {{"object_id", binding.object_id}, {"source_id", source_id}});
        }
        candidates.push_back(LatchCandidate{
            &replay->second, observed->observation, std::string(latch_policy)});
      }
    }

    BeforePlanningLatchResult result{
        initial_projection, initial_projection, 0U, 0U, 0U, 0U, 0U};
    std::vector<PoseObservationReplay *> applied_records;
    const auto publish_results = [&] {
      if (result.committed_observation_count != 0U) {
        TryPublishLogicalScene();
      }
      const auto generation =
          PublishSceneRuntime(static_cast<std::int64_t>(ElapsedNs()));
      result.scene_runtime_source_generation = generation;
      std::lock_guard lock(manager_source_mutex_);
      for (auto *const record : applied_records) {
        record->publication_generation = generation;
        if (PoseReplayIsCurrentLocked(*record))
          pose_observation_replays_.at(record->observation.key) = *record;
      }
    };
    for (auto &candidate : candidates) {
      {
        std::lock_guard lock(manager_source_mutex_);
        if (!PoseReplayIsCurrentLocked(*candidate.replay))
          return failure("scene_runtime_observer_session_stale",
                         "observer session changed before pose latching");
      }
      // The retained application lets an already confirmed exact
      // observation be reported again without a second confirmation.
      auto application = scene_runtime_pose_projector_.Apply(
          bundle->value().capability_provider_bindings,
          bundle->retained_bundle_id(),
          bundle->value().cell_graph.graph_revision, candidate.observation,
          candidate.replay->application, candidate.apply_policy,
          PoseDeadband(*bundle, candidate.observation.key));
      RecordCarriedObjectLoss(application, candidate.observation.key.object_id);
      if (!application.ok()) {
        try {
          publish_results();
        } catch (...) {
          // The durable logical-scene result remains authoritative. Periodic
          // publication recovery is independent of the latch rejection.
        }
        return failure(application.error().code, application.error().message,
                       application.error().details);
      }
      switch (application.value().status) {
      case SceneRuntimePoseApplicationStatus::kCommitted:
        ++result.committed_observation_count;
        break;
      case SceneRuntimePoseApplicationStatus::kAlreadyApplied:
      case SceneRuntimePoseApplicationStatus::kUnchanged:
        // An unchanged sample keeps the exact current projection, so the
        // planner synchronizes against the unchanged revision.
        ++result.already_applied_observation_count;
        break;
      case SceneRuntimePoseApplicationStatus::kSkippedAttached:
        ++result.skipped_attached_observation_count;
        break;
      case SceneRuntimePoseApplicationStatus::kNotRequested:
      case SceneRuntimePoseApplicationStatus::kDeferredBeforePlanning:
      case SceneRuntimePoseApplicationStatus::kDeferredBetweenNodes:
      case SceneRuntimePoseApplicationStatus::kBuffered:
        try {
          publish_results();
        } catch (...) {
        }
        return failure(
            "scene_runtime_pose_latch_result_invalid",
            "before-planning pose projector returned a non-latched status");
      }
      {
        std::lock_guard lock(manager_source_mutex_);
        if (!PoseReplayIsCurrentLocked(*candidate.replay))
          return failure("scene_runtime_observer_session_stale",
                         "observer session changed during pose latching");
        candidate.replay->application = application.value();
        candidate.replay->publication_generation.reset();
        pose_observation_replays_.at(candidate.observation.key) =
            *candidate.replay;
      }
      if (application.value().status !=
          SceneRuntimePoseApplicationStatus::kSkippedAttached)
        RetainConfiguredPoseProviderOrigin(candidate.observation);
      candidate.replay->publication_generation.reset();
      applied_records.push_back(candidate.replay);
      ++result.latched_observation_count;
      result.projection_revision = *application.value().projection_revision;
    }
    publish_results();
    const auto final_projection = scene_owner_.CurrentProjectionRevision();
    if (final_projection != result.projection_revision) {
      return failure(
          "scene_runtime_pose_projection_changed",
          "logical-scene projection changed while pose latching completed");
    }
    {
      std::lock_guard lock(manager_source_mutex_);
      const auto current = scene_runtime_owner_.Snapshot();
      if (!current.runtime_identity ||
          *current.runtime_identity != runtime_identity)
        return failure("scene_runtime_observer_session_stale",
                       "runtime changed during pose latching");
    }
    return core::Result<BeforePlanningLatchResult>::Ok(std::move(result));
  }

  bool HandleOneClient(const CommandSocket &socket) {
    sockaddr_un peer_address{};
    socklen_t peer_address_size = sizeof(peer_address);
    const FileDescriptor client(
        ::accept4(socket.get(), reinterpret_cast<sockaddr *>(&peer_address),
                  &peer_address_size, SOCK_CLOEXEC | SOCK_NONBLOCK));
    if (!client) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return false;
      }
      FailErrno("accept Manager command client");
    }

    ucred credentials{};
    socklen_t credentials_size = sizeof(credentials);
    if (::getsockopt(client.get(), SOL_SOCKET, SO_PEERCRED, &credentials,
                     &credentials_size) != 0 ||
        credentials_size != sizeof(credentials) ||
        credentials.uid != ::geteuid()) {
      return false;
    }

    const auto packet = ReceiveCommand(client.get());
    if (!packet.has_value()) {
      return false;
    }
    try {
      // A command that does not decode is one request-validation refusal.
      const auto command = [&] {
        try {
          return DecodeCommand(*packet);
        } catch (const core::TypedError &) {
          throw;
        } catch (const std::exception &error) {
          throw core::RequestInvalid("command", "invalid", error.what());
        }
      }();
      if (RequiresCurrentPackageGeneration(command.kind)) {
        try {
          RequireCurrentPackageGeneration();
        } catch (const ManagerPackageGenerationRestartRequired &error) {
          PublishPackageRestartRequired();
          if (command.kind == CommandKind::kApply &&
              command.request_id.has_value() &&
              command.request_owner_epoch.has_value()) {
            TrySendResponse(client.get(),
                            ApplyDetailedErrorResponse(command, error.error()));
          } else {
            TrySendResponse(client.get(),
                            DetailedErrorResponse(error.error().code,
                                                  error.error().message,
                                                  error.error().details));
          }
          return false;
        }
      }
      switch (command.kind) {
      case CommandKind::kComponentProcessStopOffer: {
        Require(command.process_stop_offer_component.has_value(), "component process Stop offer lacks its addressed component");
        const auto offered = ReadComponentProcessStopOffer(*command.process_stop_offer_component);
        if (!offered.ok()) {
          auto fields = DetailedErrorResponse(offered.error().code, offered.error().message, offered.error().details).as_object();
          fields.emplace_back("result", CanonicalValue(nullptr));
          TrySendResponse(client.get(), CanonicalValue(std::move(fields)));
        } else TrySendResponse(client.get(), CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kManagerCommandResponseApiVersion))}, {"ok", CanonicalValue(true)},
            {"error", CanonicalValue(nullptr)}, {"result", offered.value()}}));
        return false;
      }
      case CommandKind::kStopComponentProcess:
      case CommandKind::kComponentProcessStopResult: {
        Require(command.component_process_stop.has_value(), "component process Stop request is absent");
        const auto &request = *command.component_process_stop;
        Require(request.manager_runtime_epoch == manager_runtime_epoch_,
                "component process Stop names another Manager occurrence");
        const auto result = command.kind == CommandKind::kStopComponentProcess
            ? execution_host_adapter_.StopComponentProcess(request)
            : execution_host_adapter_.ReadComponentProcessStop({request});
        if (!result.ok()) {
          auto fields = DetailedErrorResponse(result.error().code,
              result.error().message, result.error().details).as_object();
          fields.emplace_back("result", CanonicalValue(nullptr));
          TrySendResponse(client.get(), CanonicalValue(std::move(fields)));
        } else {
          Require(result.value().request == request, "component process Stop returned a foreign request");
          TrySendResponse(client.get(), ComponentProcessStopResultResponse(result.value()));
        }
        return false;
  }
      case CommandKind::kDeploymentCandidatePreview: {
        Require(command.request_owner_epoch.has_value() &&
                    command.deployment_artifact_id.has_value() &&
                    command.deployment_revision.has_value() &&
                    command.candidate_retained_bundle_id.has_value() &&
                    command.candidate_graph_revision.has_value() &&
                    command.prepared_activation_id.has_value() &&
                    command.expected_current_retained_bundle_id.has_value() &&
                    command.expected_current_graph_revision.has_value() &&
                    command.expected_admission_generation.has_value(),
                "decoded candidate preview lacks its exact selection");
        if (*command.request_owner_epoch != manager_owner_epoch_) {
          TrySendResponse(
              client.get(),
              ErrorResponse("stale_request_owner_epoch",
                            "candidate preview targets a stale Manager owner"));
          return false;
        }
        try {
          const auto active = plan_source_.ReadCurrentPlan();
          const auto &current_pointer = active.active_publication.request;
          if (current_pointer.candidate_retained_bundle_id !=
                  *command.expected_current_retained_bundle_id ||
              current_pointer.candidate_graph_revision !=
                  *command.expected_current_graph_revision) {
            TrySendResponse(
                client.get(),
                ErrorResponse(
                    "deployment_candidate_current_changed",
                    "Manager current Deployment changed before preview"));
            return false;
          }
          const auto current_bundle = verified_package_.get_verified(
              current_pointer.candidate_retained_bundle_id);
          const auto candidate_bundle =
              verified_package_.get_verified(
                  *command.candidate_retained_bundle_id);
          Require(
              current_bundle != nullptr && candidate_bundle != nullptr &&
                  current_bundle->retained_bundle_id() ==
                      current_pointer.candidate_retained_bundle_id &&
                  current_bundle->value().cell_graph.graph_revision ==
                      current_pointer.candidate_graph_revision &&
                  candidate_bundle->retained_bundle_id() ==
                      *command.candidate_retained_bundle_id &&
                  candidate_bundle->value().cell_graph.graph_revision ==
                      *command.candidate_graph_revision,
              "candidate preview could not resolve exact compatible graphs");
          if (candidate_bundle->value().cell_graph.cell_id !=
              current_bundle->value().cell_graph.cell_id) {
            TrySendResponse(
                client.get(),
                ErrorResponse(
                    "deployment_candidate_cell_mismatch",
                    "The compiled Deployment targets runtime '" +
                        candidate_bundle->value().cell_graph.cell_id.value() +
                        "', but this runtime owns cell '" +
                        current_bundle->value().cell_graph.cell_id.value() +
                        "'. Prepare the portable Scenario for this runtime; "
                        "foreign-runtime compiled artifacts cannot be installed."));
            return false;
          }
          const auto host_continuity =
              component_manager::VerifyExecutionHostContinuity(
                  *current_bundle, *candidate_bundle, verified_package_);
          if (!host_continuity.ok()) {
            const auto &error = host_continuity.error();
            TrySendResponse(client.get(), DetailedErrorResponse(
                                               error.code, error.message,
                                               error.details));
            return false;
          }
          auto impact = DeploymentApplyImpact(
              *current_bundle, *candidate_bundle, config_.activation_root,
              *command.prepared_activation_id, site_facts_);
          DeploymentCandidateReceipt receipt{
              *command.deployment_artifact_id,
              *command.deployment_revision,
              *command.prepared_activation_id,
              *command.candidate_retained_bundle_id,
              *command.candidate_graph_revision,
              *command.expected_current_retained_bundle_id,
              *command.expected_current_graph_revision,
              *command.expected_admission_generation,
              manager_owner_epoch_,
              DeploymentApplyImpactDigest(impact),
              RequiredBool(impact, "destructive"),
              core::ContentDigest(""),
          };
          receipt.receipt_digest = DeploymentCandidateReceiptDigest(receipt);
          const auto retained =
              std::find(deployment_candidate_receipts_.begin(),
                        deployment_candidate_receipts_.end(), receipt);
          if (retained == deployment_candidate_receipts_.end()) {
            if (deployment_candidate_receipts_.size() >=
                kMaximumDeploymentCandidateReceipts) {
              deployment_candidate_receipts_.erase(
                  deployment_candidate_receipts_.begin());
            }
            deployment_candidate_receipts_.push_back(receipt);
          }
          TrySendResponse(
              client.get(),
              DeploymentCandidatePreviewResponse(
                  receipt, current_bundle->value().cell_graph,
                  candidate_bundle->value().cell_graph, std::move(impact)));
        } catch (const std::exception &error) {
          TrySendResponse(
              client.get(),
              CaughtErrorResponse(error, "deployment_candidate_rejected"));
        }
        return false;
      }
      case CommandKind::kApply: {
        Require(command.deployment_artifact_id.has_value() &&
                    command.deployment_revision.has_value() &&
                    command.expected_current_retained_bundle_id.has_value() &&
                    command.expected_current_graph_revision.has_value() &&
                    command.request_id.has_value() &&
                    command.request_owner_epoch.has_value() &&
                    command.source_label.has_value() &&
                    command.deployment_candidate_receipt.has_value() &&
                    command.prepared_activation.has_value(),
                "decoded Apply command lacks its exact selection");
        try {
          const auto existing = stores_.apply_journal().ReadRequest(
              core::RequestOwnerEpoch(*command.request_owner_epoch),
              core::RequestId(*command.request_id));
          // A Manager epoch is process-local, while an accepted Apply request
          // and its terminal outcome are durable. Exact replay must reach the
          // journal before current-epoch admission so Composer can recover an
          // interrupted Apply/Revert after a Manager restart. Unknown stale
          // requests remain rejected and cannot create a new effect.
          if (!existing.has_value() &&
              *command.request_owner_epoch != manager_owner_epoch_) {
            TrySendResponse(
                client.get(),
                ApplyKnownNoEffectErrorResponse(
                    command, "stale_request_owner_epoch",
                    "Apply request targets a stale Manager owner epoch"));
            return false;
          }
          const auto &candidate_receipt =
              *command.deployment_candidate_receipt;
          const auto receipt_admission_generation =
              command.expected_admission_generation.has_value()
                  ? *command.expected_admission_generation
                  : command.parent_authoring_claim->intent
                        .expected_admission_generation;
          if (!existing.has_value() &&
              (candidate_receipt.manager_owner_epoch != manager_owner_epoch_ ||
               candidate_receipt.deployment_artifact_id !=
                   *command.deployment_artifact_id ||
               candidate_receipt.deployment_revision !=
                   *command.deployment_revision ||
               candidate_receipt.expected_current_retained_bundle_id !=
                   *command.expected_current_retained_bundle_id ||
               candidate_receipt.expected_current_graph_revision !=
                   *command.expected_current_graph_revision ||
               candidate_receipt.expected_admission_generation !=
                   receipt_admission_generation ||
               candidate_receipt.prepared_activation_id !=
                   command.prepared_activation->prepared_activation_id ||
               command.prepared_activation->deployment_artifact_id !=
                   *command.deployment_artifact_id ||
               command.prepared_activation->deployment_revision !=
                   *command.deployment_revision)) {
            TrySendResponse(
                client.get(),
                ApplyKnownNoEffectErrorResponse(
                    command, "deployment_candidate_receipt_mismatch",
                    "Apply does not match its exact candidate preview"));
            return false;
          }
          if (!existing.has_value()) {
            RequireCurrentPackageGeneration();
          }
          if (!existing.has_value() && !kernel_apply_available_) {
            TrySendResponse(client.get(), ApplyUnavailableResponse(command));
            return false;
          }
          if (!existing.has_value() &&
              !PreparedActivationIsStoredExactly(config_.activation_root,
                                                 *command.prepared_activation)) {
            TrySendResponse(
                client.get(),
                ApplyKnownNoEffectErrorResponse(
                    command, "prepared_activation_unverified",
                    "Apply names no stored, reverified prepared activation "
                    "equal to its summary"));
            return false;
          }
          if (!existing.has_value()) {
            if (std::find(deployment_candidate_receipts_.begin(),
                          deployment_candidate_receipts_.end(),
                          candidate_receipt) ==
                deployment_candidate_receipts_.end()) {
              TrySendResponse(
                  client.get(),
                  ApplyKnownNoEffectErrorResponse(
                      command, "deployment_candidate_receipt_unknown",
                      "Apply receipt was not issued by this Manager owner"));
              return false;
            }
            const auto active = plan_source_.ReadCurrentPlan();
            const auto &current_pointer = active.active_publication.request;
            // The accepted operation carries the exact candidate identity that
            // preview resolved. Recheck the versioned public pointer here; the
            // named-Apply coordinator performs the one authoritative verified
            // predecessor/candidate capture while accepting the operation.
            if (current_pointer.candidate_retained_bundle_id !=
                    candidate_receipt.expected_current_retained_bundle_id ||
                current_pointer.candidate_graph_revision !=
                    candidate_receipt.expected_current_graph_revision) {
              TrySendResponse(client.get(),
                              ApplyKnownNoEffectErrorResponse(
                                  command, "deployment_candidate_receipt_stale",
                                  "Deployment candidate or current graph "
                                  "changed after preview"));
              return false;
            }
            // The previewed impact is revalidated at acceptance for every
            // caller, not only the GUI: a changed impact is a stale
            // confirmation, and a destructive one must be explicitly
            // accepted by its exact digest. Both refuse before effects.
            const auto current_bundle =
                verified_package_.get_verified(current_pointer.candidate_retained_bundle_id);
            const auto candidate_bundle = verified_package_.get_verified(
                candidate_receipt.candidate_retained_bundle_id);
            Require(current_bundle != nullptr && candidate_bundle != nullptr,
                    "Apply could not resolve its exact previewed graphs");
            const auto host_continuity =
                component_manager::VerifyExecutionHostContinuity(
                    *current_bundle, *candidate_bundle, verified_package_);
            if (!host_continuity.ok()) {
              TrySendResponse(
                  client.get(),
                  ApplyKnownNoEffectDetailedErrorResponse(
                      command, host_continuity.error()));
              return false;
            }
            auto impact = DeploymentApplyImpact(
                *current_bundle, *candidate_bundle, config_.activation_root,
                candidate_receipt.prepared_activation_id, site_facts_);
            if (DeploymentApplyImpactDigest(impact) !=
                candidate_receipt.impact_digest) {
              TrySendResponse(
                  client.get(),
                  ApplyKnownNoEffectErrorResponse(
                      command, "confirmation_stale",
                      "Deployment Apply impact changed after preview; preview "
                      "and confirm again"));
              return false;
            }
            if (candidate_receipt.destructive_impact &&
                command.accepted_impact_digest != candidate_receipt.impact_digest) {
              TrySendResponse(
                  client.get(),
                  ApplyKnownNoEffectErrorResponse(
                      command, "confirmation_required",
                      "Deployment Apply impact requires acknowledgment of its "
                      "exact preview digest"));
              return false;
            }
          }
          const auto candidate_retained_bundle_id =
              existing.has_value()
                  ? existing->intent.candidate_retained_bundle_id
                  : candidate_receipt.candidate_retained_bundle_id;
          const auto result = named_apply_.AcceptOrReplay(
              {
                  core::PublicRequestContextDTO{
                      core::RequestId(*command.request_id),
                      core::RequestOwnerEpoch(*command.request_owner_epoch),
                      *command.source_label,
                      command.correlation_id,
                  },
                  *command.deployment_artifact_id,
                  *command.deployment_revision,
                  *command.expected_current_retained_bundle_id,
                  *command.expected_current_graph_revision,
                  candidate_retained_bundle_id,
                  *command.prepared_activation,
                  command.expected_admission_generation,
                  command.parent_authoring_claim,
                  command.scene_initialization,
              },
              {});
          TrySendResponse(client.get(), ApplyResponse(command, result));
          if (result.state ==
                  component_manager::ManagerNamedApplyResultState::kPending &&
              !result.settlement_pending) {
            TrySubmitFailClosedReadiness();
            RequestApplyProgress();
            return false;
          }
          // A terminal replay is a read. Ordinary periodic reconciliation
          // publishes current runtime/source state without extending this
          // command or repeating an Apply effect.
        } catch (const component_manager::ManagerNamedApplyBusyError &error) {
          TrySendResponse(
              client.get(),
              ApplyErrorResponseFromJournalEvidence(command, error, "apply_busy"));
        } catch (const ManagerPackageGenerationRestartRequired &error) {
          PublishPackageRestartRequired();
          // Publishing the package-generation fault is itself externally
          // visible, even when this Apply was never accepted into its journal.
          TrySendResponse(client.get(),
                          ApplyDetailedErrorResponse(command, error.error()));
        } catch (const std::exception &error) {
          TrySendResponse(
              client.get(),
              ApplyErrorResponseFromJournalEvidence(command, error,
                                                    "apply_rejected"));
        }
        return false;
      }
      case CommandKind::kApplyReleaseForNew: {
        // New: release whatever failed Apply still holds the cell.
        Require(command.request_id.has_value() && command.source_label.has_value(),
                "decoded Apply release for New lacks its request context");
        const auto released = named_apply_.ReleaseFailedApplyForNew(
            core::RequestId(*command.request_id), *command.source_label);
        TrySendResponse(client.get(), CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
            {"error", CanonicalValue(nullptr)},
            {"ok", CanonicalValue(true)},
            {"released", released.has_value()
                             ? CanonicalValue(CanonicalValue::Object{
                                   {"apply_generation", Unsigned(released->apply_generation.value(),
                                                                 "Apply generation")},
                                   {"apply_id", CanonicalValue(released->apply_id.value())},
                               })
                             : CanonicalValue(nullptr)},
        }));
        return false;
      }
      case CommandKind::kApplyStatus:
      case CommandKind::kApplyAcknowledge: {
        Require(command.apply_id.has_value() &&
                    command.apply_generation.has_value(),
                "decoded Apply status command lacks its durable identity");
        const auto status = [&] {
          try {
            return stores_.apply_journal().ReadStatus(
                *command.apply_id, *command.apply_generation);
          } catch (const std::exception &error) {
            if (command.kind != CommandKind::kApplyAcknowledge) throw;
            auto cause = core::CauseOf(error, "manager_command_rejected", kManagerProducer);
            cause.error.details["effect"] = "none";
            throw core::TypedError(cause.error, std::move(cause.producer));
          }
        }();
        if (!status.has_value()) {
          TrySendResponse(client.get(),
                          DetailedErrorResponse(
                              "apply_status_not_found", "Manager Apply identity is unknown",
                              command.kind == CommandKind::kApplyAcknowledge
                                  ? std::map<std::string, std::string>{{"effect", "none"}}
                                  : std::map<std::string, std::string>{}));
          return false;
        }
        if (command.kind == CommandKind::kApplyAcknowledge) {
          Require(command.request_id.has_value() && command.source_label.has_value(),
                  "decoded Apply acknowledgement lacks its request context");
          named_apply_.AcknowledgeFailedApply(*command.apply_id,
                                              *command.apply_generation,
                                              core::RequestId(*command.request_id),
                                              *command.source_label);
        }
        TrySendResponse(client.get(), ApplyStatusResponse(
            *status, named_apply_.FailedApplyAcknowledgementStatus(
                         *command.apply_id, *command.apply_generation)));
        return false;
      }
      case CommandKind::kSceneCommit: {
        Require(command.scene_transaction.has_value(),
                "decoded scene command lacks its transaction");
        const auto detached = DetachedHolder(*command.scene_transaction);
        const auto result = detail::CommitSceneWithAdmissionObservation(
            scene_owner_, *command.scene_transaction,
            scene_admission_reconciled_);
        if (result.success) {
          ClearCarriedObjectLostAfterCommit(*command.scene_transaction,
                                            detached);
          // The durable scene result remains authoritative if this projection
          // write fails. Periodic reconciliation republishes it, and exact
          // request replay returns the same owner result without a second
          // native scene effect.
          TryPublishLogicalScene();
        }
        TrySendResponse(client.get(), SceneCommitResponse(command, result));
        return false;
      }
      case CommandKind::kSceneTransactionStatus: {
        Require(command.scene_transaction.has_value(),
                "Scene result lookup lacks its request");
        const auto result =
            scene_owner_.ReadCommitResult(*command.scene_transaction);
        TrySendResponse(
            client.get(),
            CanonicalValue(CanonicalValue::Object{
                {"api_version", CanonicalValue(std::string(
                                    kManagerCommandResponseApiVersion))},
                {"ok", CanonicalValue(true)},
                {"result", result.has_value()
                               ? SceneCommitResponse(command, *result)
                               : CanonicalValue(nullptr)}}));
        return false;
      }
      case CommandKind::kSceneOwnerRead: {
        const auto projection = scene_owner_.CurrentProjectionRevision();
        const auto owner_epoch = scene_owner_.CurrentRequestOwnerEpoch();
        const auto snapshot = scene_owner_.LogicalSnapshot();
        Require(snapshot != nullptr &&
                    snapshot->revision == projection.scene_revision,
                "logical-scene read crossed a projection revision");
        TrySendResponse(client.get(), SceneOwnerReadResponse(
                                          owner_epoch, projection, *snapshot));
        return false;
      }
      case CommandKind::kSceneCandidatePreview: {
        Require(command.scene_candidate_preview.has_value(),
                "decoded scene candidate command lacks its request");
        TrySendResponse(client.get(),
                        SceneCandidatePreviewResponse(
                            scene_owner_.PreviewResolvedSceneCandidate(
                                *command.scene_candidate_preview)));
        return false;
      }
      case CommandKind::kScenePoseQuery: {
        Require(command.expected_scene_projection.has_value() &&
                    !command.scene_resource_ids.empty(),
                "decoded scene pose query lacks its exact selection");
        // Resolve against the current projection. A newer revision of the
        // same Scene is the latest truth; only a lineage change refuses.
        const auto current = scene_owner_.CurrentProjectionRevision();
        if (!core::ProjectionIsSameOrForwardInLineage(
                current, *command.expected_scene_projection)) {
          TrySendResponse(
              client.get(),
              ErrorResponse(
                  "scene_projection_changed",
                  "logical-scene projection changed before pose resolution"));
          return false;
        }
        std::vector<std::pair<core::SceneResourceId, core::Pose3DTO>> poses;
        poses.reserve(command.scene_resource_ids.size());
        for (const auto &scene_resource_id : command.scene_resource_ids) {
          const auto pose = scene_owner_.GetObjectPose(scene_resource_id);
          if (!pose.ok()) {
            const auto &error = pose.error();
            TrySendResponse(
                client.get(),
                DetailedErrorResponse(
                    error.code.empty() ? "scene_pose_unavailable" : error.code,
                    error.message.empty()
                        ? "logical-scene object pose is unavailable"
                        : error.message,
                    error.details));
            return false;
          }
          poses.emplace_back(scene_resource_id, pose.value());
        }
        TrySendResponse(client.get(), ScenePoseResponse(current, poses));
        return false;
      }
      case CommandKind::kSceneRuntimeOwnerRead: {
        const auto snapshot = scene_runtime_owner_.Snapshot();
        if (!snapshot.runtime_identity.has_value()) {
          TrySendResponse(
              client.get(),
              ErrorResponse("scene_runtime_owner_unavailable",
                            "scene-runtime owner has no active identity"));
          return false;
        }
        TrySendResponse(client.get(),
                        SceneRuntimeOwnerReadResponse(
                            admission_.cell_id().value(),
                            scene_runtime_owner_epoch_, snapshot));
        return false;
      }
      case CommandKind::kSceneRuntimeObserverOpen: {
        Require(command.expected_scene_runtime_identity.has_value() &&
                    command.pose_source_id.has_value() &&
                    command.pose_source_epoch.has_value() &&
                    !command.pose_observer_bindings.empty(),
                "decoded scene-runtime observer open lacks identity");
        std::unique_lock source_lock(manager_source_mutex_);
        const auto snapshot = scene_runtime_owner_.Snapshot();
        if (!snapshot.runtime_identity.has_value() ||
            *snapshot.runtime_identity !=
                *command.expected_scene_runtime_identity) {
          source_lock.unlock();
          TrySendResponse(
              client.get(),
              ErrorResponse(
                  "scene_runtime_identity_changed",
                  "scene-runtime observer targets an inactive runtime"));
          return false;
        }
        auto found = pose_observer_sessions_.find(*command.pose_source_id);
        if (found == pose_observer_sessions_.end() &&
            pose_observer_sessions_.size() >= kMaximumPoseObserverSessions) {
          source_lock.unlock();
          TrySendResponse(
              client.get(),
              ErrorResponse("scene_runtime_observer_capacity_exhausted",
                            "pose observer session capacity is exhausted"));
          return false;
        }
        std::uint64_t source_epoch_order = 1U;
        if (found != pose_observer_sessions_.end()) {
          if (found->second.runtime_identity == *snapshot.runtime_identity &&
              found->second.source_epoch == *command.pose_source_epoch) {
            if (found->second.bindings != command.pose_observer_bindings) {
              source_lock.unlock();
              TrySendResponse(
                  client.get(),
                  ErrorResponse(
                      "scene_runtime_observer_request_conflict",
                      "observer epoch identity was reused with changed "
                      "bindings"));
              return false;
            }
            source_epoch_order = found->second.source_epoch_order;
          } else {
            if (found->second.source_epoch_order >= 9007199254740991ULL) {
              source_lock.unlock();
              TrySendResponse(
                  client.get(),
                  ErrorResponse("scene_runtime_observer_epoch_exhausted",
                                "pose observer epoch order is exhausted"));
              return false;
            }
            source_epoch_order = found->second.source_epoch_order + 1U;
          }
        }
        for (const auto &binding : command.pose_observer_bindings) {
          if (!binding.eligible) {
            continue;
          }
          for (const auto &[other_source_id, other_session] :
               pose_observer_sessions_) {
            if (other_source_id == *command.pose_source_id ||
                other_session.runtime_identity != *snapshot.runtime_identity) {
              continue;
            }
            const auto conflict = std::find_if(
                other_session.bindings.begin(), other_session.bindings.end(),
                [&](const PoseObserverBinding &other) {
                  return other.eligible && other.object_id == binding.object_id;
                });
            if (conflict != other_session.bindings.end()) {
              source_lock.unlock();
              TrySendResponse(
                  client.get(),
                  ErrorResponse(
                      "scene_runtime_pose_writer_conflict",
                      "another observer session already declares the object "
                      "as collision-scene writable"));
              return false;
            }
          }
        }
        pose_observer_sessions_.insert_or_assign(
            *command.pose_source_id,
            PoseObserverSession{*snapshot.runtime_identity,
                                *command.pose_source_epoch, source_epoch_order,
                                command.pose_observer_bindings});
        TryPublishViewerPoseSessionsLocked();
        source_lock.unlock();
        TrySendResponse(client.get(),
                        SceneRuntimeObserverOpenResponse(
                            admission_.cell_id().value(),
                            *snapshot.runtime_identity, *command.pose_source_id,
                            *command.pose_source_epoch, source_epoch_order,
                            command.pose_observer_bindings));
        return false;
      }
      case CommandKind::kSceneRuntimeObserve: {
        Require(command.expected_scene_runtime_identity.has_value() &&
                    command.pose_source_id.has_value() &&
                    command.pose_source_epoch.has_value() &&
                    command.pose_source_sequence.has_value() &&
                    command.pose_object_id.has_value() &&
                    command.pose_policy_id.has_value() &&
                    command.pose_lifetime_ns.has_value() &&
                    command.pose_payload_quality.has_value() &&
                    command.pose_eligible.has_value() &&
                    command.pose_payload.has_value(),
                "decoded scene-runtime observation is incomplete");
        std::unique_lock source_lock(manager_source_mutex_);
        const auto snapshot = scene_runtime_owner_.Snapshot();
        if (!snapshot.runtime_identity.has_value() ||
            *snapshot.runtime_identity !=
                *command.expected_scene_runtime_identity) {
          source_lock.unlock();
          TrySendResponse(
              client.get(),
              ErrorResponse(
                  "scene_runtime_identity_changed",
                  "pose observation targets an inactive scene runtime"));
          return false;
        }
        const auto session =
            pose_observer_sessions_.find(*command.pose_source_id);
        if (session == pose_observer_sessions_.end() ||
            session->second.runtime_identity != *snapshot.runtime_identity ||
            session->second.source_epoch != *command.pose_source_epoch) {
          source_lock.unlock();
          TrySendResponse(client.get(),
                          ErrorResponse("scene_runtime_observer_session_stale",
                                        "pose observation does not match the "
                                        "active observer session"));
          return false;
        }
        const auto declared = std::lower_bound(
            session->second.bindings.begin(), session->second.bindings.end(),
            PoseObserverBinding{*command.pose_object_id,
                                *command.pose_policy_id, false});
        if (declared == session->second.bindings.end() ||
            declared->object_id != *command.pose_object_id ||
            declared->policy_id != *command.pose_policy_id ||
            (*command.pose_eligible && !declared->eligible) ||
            (*command.pose_eligible &&
             *command.pose_payload_quality != PoseSourceQuality::kGood)) {
          source_lock.unlock();
          TrySendResponse(
              client.get(),
              ErrorResponse(
                  "scene_runtime_observation_not_declared",
                  "pose observation does not match its registered binding"));
          return false;
        }
        const PoseObservationKey observation_key{*command.pose_source_id,
                                                 *command.pose_object_id,
                                                 *command.pose_policy_id};
        auto replay = pose_observation_replays_.find(observation_key);
        std::optional<PoseObservationReplay> retained;
        if (replay != pose_observation_replays_.end() &&
            replay->second.observation.source_epoch_order ==
                session->second.source_epoch_order &&
            replay->second.observation.source_sequence ==
                *command.pose_source_sequence) {
          const auto &exact_record = replay->second;
          const bool exact =
              exact_record.runtime_identity == *snapshot.runtime_identity &&
              exact_record.requested_lifetime_ns == *command.pose_lifetime_ns &&
              exact_record.observation.source_epoch ==
                  *command.pose_source_epoch &&
              exact_record.observation.payload_quality == *command.pose_payload_quality &&
              exact_record.observation.eligible == *command.pose_eligible &&
              exact_record.observation.payload == *command.pose_payload;
          if (!exact) {
            source_lock.unlock();
            TrySendResponse(
                client.get(),
                ErrorResponse("scene_runtime_observation_request_conflict",
                              "pose observation identity was reused with "
                              "changed intent"));
            return false;
          }
          retained = replay->second;
        } else {
          const auto observed_at = HostMonotonicNs();
          if (observed_at < 0 || *command.pose_lifetime_ns > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max() - observed_at)) {
            source_lock.unlock();
            TrySendResponse(
                client.get(),
                ErrorResponse(
                    "scene_runtime_observation_expiry_invalid",
                    "pose observation expiry exceeds the int64 clock range"));
            return false;
          }
          PoseSourceObservation observation{
              observation_key,
              *command.pose_source_epoch,
              session->second.source_epoch_order,
              *command.pose_source_sequence,
              observed_at,
              observed_at + static_cast<std::int64_t>(*command.pose_lifetime_ns),
              *command.pose_payload_quality,
              *command.pose_eligible,
              *command.pose_payload,
          };
          Require(last_active_bundle_ &&
                      last_active_bundle_->retained_bundle_id().value() ==
                          snapshot.runtime_identity->retained_bundle_id &&
                      last_active_bundle_->value()
                              .cell_graph.graph_revision.value() ==
                          snapshot.runtime_identity->graph_revision,
                  "pose observation has no exact current policy bundle");
          const auto acceptance = scene_runtime_owner_.ObservePose(
              observation,
              EffectivePosePolicy(*last_active_bundle_, observation.key));
          auto [stored, inserted] = pose_observation_replays_.insert_or_assign(
              observation_key,
              PoseObservationReplay{*snapshot.runtime_identity,
                                    *command.pose_lifetime_ns, observation,
                                    acceptance, std::nullopt, std::nullopt});
          static_cast<void>(inserted);
          retained = stored->second;
        }
        Require(retained.has_value(),
                "pose observation replay record was not retained");
        source_lock.unlock();
        auto &record = *retained;
        const bool applied_now = !record.application;
        if (!record.application) {
          const auto application = ResolvePoseApplication(
              record.runtime_identity, record.observation, record.acceptance);
          if (application.ok())
            record.application = application.value();
          {
            std::lock_guard lock(manager_source_mutex_);
            if (PoseReplayIsCurrentLocked(record)) {
              pose_observation_replays_.at(record.observation.key) = record;
            }
          }
          if (!application.ok()) {
            if (!record.publication_generation)
              record.publication_generation = PublishSceneRuntime(
                  static_cast<std::int64_t>(ElapsedNs()));
            {
              std::lock_guard lock(manager_source_mutex_);
              if (PoseReplayIsCurrentLocked(record))
                pose_observation_replays_.at(record.observation.key) = record;
            }
            TrySendResponse(client.get(),
                            ErrorResponse(application.error().code,
                                          application.error().message));
            return false;
          }
        }
        bool current = false;
        {
          std::lock_guard lock(manager_source_mutex_);
          current = PoseReplayIsCurrentLocked(record);
        }
        if (!current) {
          TrySendResponse(client.get(),
                          ErrorResponse("scene_runtime_observer_session_stale",
                                        "observer session changed during pose application"));
          return false;
        }
        if (applied_now && (record.application->status ==
                SceneRuntimePoseApplicationStatus::kCommitted ||
            record.application->status ==
                SceneRuntimePoseApplicationStatus::kAlreadyApplied ||
            record.application->status ==
                SceneRuntimePoseApplicationStatus::kUnchanged))
          RetainConfiguredPoseProviderOrigin(record.observation);
        if (applied_now && record.application->status ==
            SceneRuntimePoseApplicationStatus::kCommitted)
          TryPublishLogicalScene();
        if (!record.publication_generation)
          record.publication_generation =
              PublishSceneRuntime(static_cast<std::int64_t>(ElapsedNs()));
        {
          std::lock_guard lock(manager_source_mutex_);
          current = PoseReplayIsCurrentLocked(record);
          if (current)
            pose_observation_replays_.at(record.observation.key) = record;
        }
        if (!current) {
          TrySendResponse(client.get(),
                          ErrorResponse("scene_runtime_observer_session_stale",
                                        "observer session changed during pose publication"));
          return false;
        }
        TrySendResponse(
            client.get(),
            SceneRuntimeObserveResponse(record.observation, record.acceptance,
                                        *record.application,
                                        *record.publication_generation));
        return false;
      }
      case CommandKind::kSceneRuntimeLatchBeforePlanning:
      case CommandKind::kSceneRuntimeLatchBetweenNodes: {
        const bool between_nodes =
            command.kind == CommandKind::kSceneRuntimeLatchBetweenNodes;
        Require(command.expected_scene_runtime_identity.has_value(),
                "decoded scene-runtime latch is incomplete");
        const auto owner = scene_runtime_owner_.Snapshot();
        if (!owner.runtime_identity.has_value() ||
            *owner.runtime_identity !=
                *command.expected_scene_runtime_identity) {
          TrySendResponse(
              client.get(),
              ErrorResponse(
                  "scene_runtime_identity_changed",
                  "before-planning pose latch targets an inactive runtime"));
          return false;
        }
        const auto result = LatchBeforePlanningObservations(
            *command.expected_scene_runtime_identity,
            scene_owner_.CurrentProjectionRevision(),
            between_nodes ? "between_nodes" : "before_planning",
            command.scene_resource_ids);
        if (!result.ok()) {
          TrySendResponse(client.get(), ErrorResponse(result.error().code,
                                                      result.error().message));
          return false;
        }
        TrySendResponse(
            client.get(),
            SceneRuntimeBeforePlanningLatchResponse(
                *command.expected_scene_runtime_identity, result.value()));
        return false;
      }
      case CommandKind::kReconcile: {
        const auto generations = [this] {
          try {
            // The public reconcile command is a status refresh, not an
            // authorized maintenance operation, so it stays observation only:
            // a missing managed unit is reported, never restarted.
            return ObserveRuntimeAndPublish();
          } catch (...) {
            TrySubmitFailClosedReadiness();
            throw;
          }
        }();
        TrySendResponse(client.get(),
                        SuccessResponse("reconcile", generations.manager,
                                        generations.scene,
                                        generations.scene_runtime));
        return false;
      }
      case CommandKind::kShutdownRuntime:
        static_cast<void>(active_runtime_.Shutdown());
        TrySendResponse(client.get(),
                        SuccessResponse("shutdown_runtime", std::nullopt,
                                        std::nullopt, std::nullopt));
        return true;
      }
    } catch (const ManagerPackageGenerationRestartRequired &error) {
      PublishPackageRestartRequired();
      TrySendResponse(client.get(),
                      DetailedErrorResponse(error.error().code,
                                            error.error().message,
                                            error.error().details));
    } catch (const std::exception &error) {
      TrySendResponse(client.get(),
                      CaughtErrorResponse(error, "manager_command_rejected"));
    } catch (...) {
      TrySendResponse(client.get(),
                      ErrorResponse("manager_command_rejected",
                                    "unknown Manager command failure"));
    }
    return false;
  }

  DaemonConfig config_;
  // Own the exact validated site document before constructing child adapters.
  std::optional<std::string> site_settings_document_;
  // Its device addresses: every component spec is derived with them.
  deployment::SiteFacts site_facts_;
  std::chrono::steady_clock::time_point started_at_;
  ManagerPackageGeneration package_generation_;
  DaemonLock daemon_lock_;
  deployment::CanonicalEmptyRetainedBundleAdmission admission_;
  ManagerVerifiedPackageView verified_package_;
  component_manager::RetainedExecutionHostUnitAdapter execution_host_adapter_;
  component_manager::ExternallyProvisionedManagerStores stores_;
  manyforge::scene::LogicalSceneOwner scene_owner_;
  OriginalManagerSceneMaintenanceAdapter original_manager_scene_maintenance_;
  SceneRuntimePoseProjector scene_runtime_pose_projector_;
  component_manager::DurableManagerActiveRuntimePlanSource plan_source_;
  component_manager::RetainedPosixRuntimeUnitAdapter reference_runtime_adapter_;
  component_manager::RuntimeUnitAdapterRouter runtime_adapter_router_;
  component_manager::ManagerActiveRuntimeCoordinator active_runtime_;
  DaemonPlannedMaintenanceGate planned_maintenance_gate_;
  component_manager::ManagerPlannedMaintenanceCoordinator
      planned_maintenance_;
  // Guards retained runtime/source facts and observer session/replay maps
  // shared with the Apply progression lane. Never spans child transport IO.
  std::mutex manager_source_mutex_;
  bool kernel_apply_available_{};
  std::atomic<bool> scene_admission_reconciled_{false};
  std::optional<std::string> scene_admission_last_warning_;
  std::optional<component_manager::ManagerActiveRuntimeConvergenceResult>
      last_runtime_;
  std::shared_ptr<const deployment::RetainedBundle> last_active_bundle_;
  core::RuntimeEpoch manager_runtime_epoch_;
  component_manager::ManagerNamedApplyCoordinator named_apply_;
  // Guarded by manager_source_mutex_. The active bundle's pose-source rows
  // and the provider origins of their configured bindings, updated after
  // actual pose commits.
  std::vector<ConfiguredPoseSource> configured_pose_sources_;
  std::map<PoseObservationKey, core::SceneObjectProviderEvidenceDTO>
      pose_provider_origins_;
  SceneRuntimeStateOwner scene_runtime_owner_;
  AtomicSourceSnapshotChannel manager_channel_;
  AtomicSourceSnapshotChannel scene_channel_;
  AtomicSourceSnapshotChannel scene_runtime_channel_;
  AtomicSourceSnapshotChannel viewer_pose_sessions_channel_;
  std::string manager_owner_epoch_;
  std::string scene_runtime_owner_epoch_;
  std::string manager_owner_id_;
  std::string scene_owner_id_;
  std::string scene_runtime_owner_id_;
  // All heartbeat sequence/retry/readiness fields are serialized by this
  // same attempt mutex. Never take it while holding lane_mutex_.
  std::mutex manager_liveness_attempt_mutex_;
  std::optional<SuccessfulManagerRenewal> successful_manager_renewal_;
  std::mutex manager_liveness_lane_mutex_;
  std::condition_variable manager_liveness_lane_condition_;
  std::optional<NativeManagerLivenessSnapshot> manager_liveness_snapshot_;
  std::uint64_t manager_liveness_readiness_barrier_{};
  std::uint64_t manager_liveness_wake_generation_{};
  bool manager_liveness_force_closed_{true};
  bool manager_liveness_lane_stop_{};
  std::thread manager_liveness_lane_;
  std::uint64_t manager_heartbeat_sequence_{};
  component_manager::detail::ManagerLivenessRetryGate
      manager_liveness_retry_gate_;
  // Log-only, accessed under manager_liveness_attempt_mutex_.
  std::optional<std::string> manager_liveness_last_warning_;
  std::optional<core::ActiveBundlePublicationReceiptDTO>
      manager_liveness_attempted_publication_;
  std::optional<core::ActiveBundlePublicationReceiptDTO>
      manager_liveness_superseded_publication_;
  std::optional<core::CellAdmissionReadinessSnapshotDTO>
      last_submitted_readiness_;
  // Alarm evidence only, serialized with the existing renewal attempt. It does not
  // alter readiness selection, retry budgets, lease expiry or containment.
  std::optional<core::ProgramCycleStatusDTO> established_program_cycle_status_;
  /// The accepted Reset whose carried-object-lost signals were cleared.
  std::string cleared_reset_request_id_;
  std::optional<core::ProgramRunnerContactLossDTO> program_runner_contact_loss_;
  std::mutex apply_progress_lane_mutex_;
  std::condition_variable apply_progress_lane_condition_;
  bool apply_progress_lane_stop_{};
  bool apply_progress_requested_{};
  std::optional<core::NamedApplyPublicationTransactionDTO> apply_settlement_retry_requested_;
  bool apply_progress_faulted_{};
  std::thread apply_progress_lane_;
  std::map<std::string, PoseObserverSession> pose_observer_sessions_;
  std::vector<DeploymentCandidateReceipt> deployment_candidate_receipts_;
  std::map<PoseObservationKey, PoseObservationReplay> pose_observation_replays_;
};

std::uint64_t ParsePeriod(const std::string_view value) {
  Require(!value.empty(), "reconciliation period cannot be empty");
  std::uint64_t result = 0U;
  for (const char digit : value) {
    Require(digit >= '0' && digit <= '9',
            "reconciliation period must be decimal");
    const auto next = static_cast<std::uint64_t>(digit - '0');
    Require(result <= (std::numeric_limits<std::uint64_t>::max() - next) / 10U,
            "reconciliation period overflows");
    result = result * 10U + next;
  }
  return result;
}

DaemonConfig ParseArguments(const int argc, char **argv) {
  DaemonConfig config;
  bool install = false;
  bool activation = false;
  bool admission = false;
  bool manager = false;
  bool manager_run = false;
  bool execution_state = false;
  bool execution_run = false;
  bool scene = false;
  bool program_cycle = false;
  bool projection = false;
  bool program_runner_liveness = false;
  bool period = false;
  bool scene_asset = false;
  bool site_settings = false;
  for (int index = 1; index < argc; index += 2) {
    Require(index + 1 < argc, "every option requires one value");
    const std::string_view option(argv[index]);
    const std::string value(argv[index + 1]);
    if (option == "--install-root" && !install) {
      config.install_root = value;
      install = true;
    } else if (option == "--activation-root" && !activation) {
      config.activation_root = value;
      activation = true;
    } else if (option == "--canonical-empty-admission-filename" && !admission) {
      config.canonical_empty_admission_filename = value;
      admission = true;
    } else if (option == "--manager-state-root" && !manager) {
      config.manager_state_root = value;
      manager = true;
    } else if (option == "--manager-run-root" && !manager_run) {
      config.manager_run_root = value;
      manager_run = true;
    } else if (option == "--execution-state-root" && !execution_state) {
      config.execution_state_root = value;
      execution_state = true;
    } else if (option == "--execution-run-root" && !execution_run) {
      config.execution_run_root = value;
      execution_run = true;
    } else if (option == "--logical-scene-database" && !scene) {
      config.logical_scene_database = value;
      scene = true;
    } else if (option == "--program-cycle-journal" && !program_cycle) {
      config.program_cycle_journal = value;
      program_cycle = true;
    } else if (option == "--projection-root" && !projection) {
      config.projection_root = value;
      projection = true;
    } else if (option == "--program-runner-liveness-socket" &&
               !program_runner_liveness) {
      config.program_runner_liveness_socket = value;
      program_runner_liveness = true;
    } else if (option == "--scene-asset-root" && !scene_asset) {
      config.scene_asset_root = value;
      scene_asset = true;
    } else if (option == "--site-settings" && !site_settings) {
      Require(!value.empty(), "--site-settings requires one explicit path");
      config.site_settings_path = value;
      site_settings = true;
    } else if (option == "--reconciliation-period-ms" && !period) {
      config.reconciliation_period =
          std::chrono::milliseconds(ParsePeriod(value));
      period = true;
    } else {
      Fail("unknown or duplicate option " + std::string(option));
    }
  }
  Require(
      install && manager && manager_run && execution_state && execution_run &&
          scene && program_cycle && projection,
      "--install-root, --manager-state-root, --manager-run-root, "
      "--execution-state-root, --execution-run-root, "
      "--logical-scene-database, --program-cycle-journal, "
      "and --projection-root are required");
  return config;
}

void InstallSignalHandlers() {
  struct sigaction action {};
  action.sa_handler = HandleStopSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  if (::sigaction(SIGINT, &action, nullptr) != 0 ||
      ::sigaction(SIGTERM, &action, nullptr) != 0) {
    FailErrno("install Manager daemon stop signal handlers");
  }
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (::sigaction(SIGPIPE, &ignore, nullptr) != 0) {
    FailErrno("ignore SIGPIPE in Manager daemon");
  }
}

} // namespace

std::string EncodeManagerStartupError(const manyforge::core::ErrorDTO &error) {
  manyforge::content_identity::CanonicalValue::Object details;
  details.reserve(error.details.size());
  for (const auto &[key, value] : error.details) {
    details.emplace_back(key,
                         manyforge::content_identity::CanonicalValue(value));
  }
  return manyforge::content_identity::canonical_json(
      manyforge::content_identity::CanonicalValue(
          manyforge::content_identity::CanonicalValue::Object{
              {"code", manyforge::content_identity::CanonicalValue(error.code)},
              {"details",
               manyforge::content_identity::CanonicalValue(std::move(details))},
              {"message",
               manyforge::content_identity::CanonicalValue(error.message)},
          }));
}

} // namespace manyforge::headless

int main(int argc, char **argv) {
  try {
    manyforge::headless::InstallSignalHandlers();
    manyforge::headless::ManagerDaemon daemon(
        manyforge::headless::ParseArguments(argc, argv));
    return daemon.Run();
  } catch (const manyforge::component_manager::ManagerStateMarkerError &error) {
    std::cerr << manyforge::headless::EncodeManagerStartupError(error.error())
              << '\n';
    return 70;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 70;
  } catch (...) {
    std::cerr << "Manager daemon failed with an unknown error\n";
    return 71;
  }
}
