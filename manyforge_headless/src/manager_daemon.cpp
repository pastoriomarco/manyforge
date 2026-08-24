#include "manyforge_headless/cell_admission_readiness_reducer.hpp"
#include "manyforge_headless/manager_package_generation.hpp"
#include "manyforge_headless/owner_publication.hpp"
#include "manyforge_headless/scene_runtime_pose_projector.hpp"
#include "manyforge_headless/scene_runtime_state_owner.hpp"
#include "manyforge_headless/source_snapshot.hpp"

#include <manyforge_component_manager/active_native_projection_runtime_set.hpp>
#include <manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_liveness_transport.hpp>
#include <manyforge_component_manager/manager_named_apply_coordinator.hpp>
#include <manyforge_component_manager/manager_runtime_convergence_coordinator.hpp>
#include <manyforge_component_manager/retained_native_runtime_unit_adapter.hpp>
#include <manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp>
#include <manyforge_component_manager/runtime_unit_adapter_router.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/scenario_apply_candidate_source.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_scene/canonical_empty_scene_owner.hpp>
#include <manyforge_scene/logical_scene_value_codec.hpp>
#include <manyforge_scene/scene_transaction_codec.hpp>

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
#include <string_view>
#include <system_error>
#include <thread>
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
constexpr std::string_view kManagerCommandResponseApiVersion =
    "manyforge.manager_command_response.v1";
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

std::filesystem::path ProspectiveDatabasePath(std::filesystem::path path,
                                              const std::string_view label) {
  Require(!path.empty() && path.has_filename() && path.filename() != "." &&
              path.filename() != "..",
          std::string(label) + " path is invalid");
  static_cast<void>(
      ExistingDirectory(path.parent_path(), std::string(label) + " parent"));
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
    const int send_buffer_bytes = static_cast<int>(kMaximumResponseBytes);
    if (::setsockopt(descriptor_.get(), SOL_SOCKET, SO_RCVBUF,
                     &receive_buffer_bytes,
                     sizeof(receive_buffer_bytes)) != 0 ||
        ::setsockopt(descriptor_.get(), SOL_SOCKET, SO_SNDBUF,
                     &send_buffer_bytes, sizeof(send_buffer_bytes)) != 0) {
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
  std::filesystem::path canonical_empty_admission_filename{
      "canonical-empty-admission.json"};
  std::filesystem::path manager_state_root;
  std::filesystem::path manager_run_root;
  std::filesystem::path runtime_state_root;
  std::filesystem::path logical_scene_database;
  std::filesystem::path program_cycle_journal;
  std::filesystem::path authoring_transaction_journal;
  std::filesystem::path projection_root;
  std::filesystem::path program_runner_liveness_socket;
  std::chrono::milliseconds reconciliation_period{250};
};

enum class CommandKind {
  kApply,
  kApplyStatus,
  kSceneCommit,
  kSceneOwnerRead,
  kSceneCandidatePreview,
  kScenePoseQuery,
  kSceneRuntimeOwnerRead,
  kSceneRuntimeObserverOpen,
  kSceneRuntimeObserve,
  kSceneRuntimeLatchBeforePlanning,
  kReconcile,
  kShutdownRuntime,
};

[[nodiscard]] bool RequiresCurrentPackageGeneration(
    const CommandKind command) noexcept {
  switch (command) {
  case CommandKind::kApply:
  case CommandKind::kSceneCommit:
  case CommandKind::kSceneCandidatePreview:
  case CommandKind::kSceneRuntimeObserverOpen:
  case CommandKind::kSceneRuntimeObserve:
  case CommandKind::kSceneRuntimeLatchBeforePlanning:
  case CommandKind::kReconcile:
    return true;
  case CommandKind::kSceneOwnerRead:
  case CommandKind::kApplyStatus:
  case CommandKind::kScenePoseQuery:
  case CommandKind::kSceneRuntimeOwnerRead:
  case CommandKind::kShutdownRuntime:
    return false;
  }
  return true;
}

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
  std::optional<PoseSourceQuality> pose_quality{};
  std::optional<bool> pose_eligible{};
  std::optional<CanonicalValue> pose_payload{};
  std::vector<PoseObserverBinding> pose_observer_bindings{};
  std::optional<core::ApplyId> apply_id{};
  std::optional<core::ApplyGeneration> apply_generation{};
};

const CanonicalValue &RequiredMember(const CanonicalValue &root,
                                     const std::string_view name) {
  const auto *member = root.find_object_member(name);
  Require(member != nullptr, "command is missing field " + std::string(name));
  return *member;
}

std::string RequiredString(const CanonicalValue &root,
                           const std::string_view name) {
  const auto *value =
      std::get_if<std::string>(&RequiredMember(root, name).storage());
  Require(value != nullptr && !value->empty() &&
              value->size() <= kMaximumCommandIdentityBytes,
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
  RequireExactObjectSize(value, 6U, "Program Runner cycle status");
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
  } else {
    Fail("Program Runner cycle status has an unsupported state");
  }
  const auto run_id = OptionalString(value, "program_run_id");
  const auto claim_generation =
      std::holds_alternative<std::nullptr_t>(
          RequiredMember(value, "cycle_claim_generation").storage())
          ? std::nullopt
          : std::optional<core::CellAdmissionClaimGeneration>(
                RequiredUnsigned(
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
  Require(core::ValidateProgramCycleStatus(result).ok(),
          "Program Runner cycle status is malformed");
  return result;
}

CanonicalValue ExchangeProgramRunnerLivenessPacket(
    const std::filesystem::path &path, const CanonicalValue &request) {
  const auto spelling = path.string();
  Require(!spelling.empty() && spelling.size() < sizeof(sockaddr_un::sun_path),
          "Program Runner liveness socket path is invalid");
  struct stat path_status {};
  Require(::lstat(path.c_str(), &path_status) == 0 &&
              S_ISSOCK(path_status.st_mode) &&
              path_status.st_uid == ::geteuid(),
          "Program Runner liveness socket is not an exact same-user socket");
  FileDescriptor connection(
      ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
  if (!connection) {
    FailErrno("create Program Runner liveness connection");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
  if (::connect(connection.get(), reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0 &&
      errno != EINPROGRESS) {
    FailErrno("connect Program Runner liveness socket");
  }
  pollfd ready{connection.get(), POLLOUT, 0};
  int polled = -1;
  do {
    polled = ::poll(&ready, 1, kClientReadTimeoutMilliseconds);
  } while (polled < 0 && errno == EINTR);
  Require(polled == 1 && (ready.revents & POLLOUT) != 0,
          "Program Runner liveness connection timed out");
  int socket_error = 0;
  socklen_t socket_error_size = sizeof(socket_error);
  Require(::getsockopt(connection.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                       &socket_error_size) == 0 &&
              socket_error == 0,
          "Program Runner liveness connection was rejected");
  struct ucred peer {};
  socklen_t peer_size = sizeof(peer);
  Require(::getsockopt(connection.get(), SOL_SOCKET, SO_PEERCRED, &peer,
                       &peer_size) == 0 &&
              peer_size == sizeof(peer) && peer.uid == ::geteuid(),
          "Program Runner liveness peer uid is foreign");

  const auto encoded = content_identity::canonical_json(request);
  Require(encoded.size() <= kMaximumProgramRunnerLivenessPacketBytes,
          "Program Runner liveness request exceeds its byte bound");
  const auto sent =
      ::send(connection.get(), encoded.data(), encoded.size(), MSG_NOSIGNAL);
  Require(sent == static_cast<ssize_t>(encoded.size()),
          "Program Runner liveness request was not sent exactly");
  ready = pollfd{connection.get(), POLLIN, 0};
  do {
    polled = ::poll(&ready, 1, kClientReadTimeoutMilliseconds);
  } while (polled < 0 && errno == EINTR);
  Require(polled == 1 && (ready.revents & POLLIN) != 0,
          "Program Runner liveness acknowledgement timed out");
  std::array<char, kMaximumProgramRunnerLivenessPacketBytes + 1U> response{};
  iovec vector{response.data(), response.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  const auto received = ::recvmsg(connection.get(), &message, 0);
  Require(received > 0 &&
              received <= static_cast<ssize_t>(
                              kMaximumProgramRunnerLivenessPacketBytes) &&
              (message.msg_flags & MSG_TRUNC) == 0,
          "Program Runner liveness acknowledgement is empty or oversized");
  const std::string_view bytes(response.data(),
                               static_cast<std::size_t>(received));
  CanonicalValue decoded;
  try {
    decoded = content_identity::parse_canonical_json(
        bytes, {kMaximumProgramRunnerLivenessPacketBytes, 4U, 64U});
  } catch (const std::exception &error) {
    Fail("Program Runner liveness acknowledgement is invalid: " +
         std::string(error.what()));
  }
  return decoded;
}

ProgramRunnerLivenessIdentity ProbeProgramRunnerLiveness(
    const std::filesystem::path &path, const core::CellId &cell_id) {
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
              RequiredString(response,
                             "local_safe_state_contract_revision") ==
                  kProgramRunnerLocalSafeStateContractRevision,
          "Program Runner liveness probe returned a foreign identity");
  return {
      core::RuntimeUnitId(
          RequiredString(response, "receiver_runtime_unit_id")),
      core::RuntimeEpoch(RequiredString(response, "receiver_runtime_epoch")),
      RequiredProgramCycleStatus(
          RequiredMember(response, "program_cycle_status"))};
}

ProgramRunnerLivenessRenewal RenewProgramRunnerLiveness(
    const std::filesystem::path &path,
    const ProgramRunnerLivenessIdentity &receiver,
    const core::ManagerLivenessRequiredSetDTO &required_set,
    const std::uint64_t sequence,
    const core::ContentDigest &reconciliation_digest) {
  const auto response = ExchangeProgramRunnerLivenessPacket(
      path, CanonicalValue(CanonicalValue::Object{
                {"active_bundle_id",
                 CanonicalValue(required_set.active_bundle_id.value())},
                {"api_version",
                 CanonicalValue(std::string(kProgramRunnerLivenessApiVersion))},
                {"cell_id", CanonicalValue(required_set.cell_id.value())},
                {"heartbeat_sequence",
                 CanonicalValue(static_cast<double>(sequence))},
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
                 CanonicalValue(reconciliation_digest.value())},
                {"required_runtime_set_digest",
                 CanonicalValue(
                     required_set.required_runtime_set_digest.value())},
                {"runtime_policy_id",
                 CanonicalValue(
                     std::string(kProgramRunnerLivenessPolicyId))},
                {"runtime_policy_revision",
                 CanonicalValue(
                     std::string(kProgramRunnerLivenessPolicyRevision))},
            }));
  RequireExactObjectSize(response, 23U, "Program Runner heartbeat ack");
  const auto local_age = RequiredNonnegativeUnsigned(
      RequiredMember(response, "local_heartbeat_age_ns"),
      "Program Runner local heartbeat age");
  const auto receiver_status_sequence = RequiredUnsigned(
      RequiredMember(response, "receiver_status_sequence"),
      "Program Runner receiver status sequence");
  const auto fault_reference = OptionalString(response, "fault_reference");
  Require(
      RequiredString(response, "api_version") ==
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
          RequiredString(response, "state") == "current" &&
          !RequiredBool(response, "command_fenced") &&
          (!fault_reference.has_value() ||
           (fault_reference->starts_with("sha256:") &&
            fault_reference->size() == 71U)),
      "Program Runner liveness acknowledgement is not exact");
  core::ManagerLivenessObservationDTO observation{
      {required_set.cell_id,
       receiver.runtime_unit_id,
       receiver.runtime_epoch,
       required_set.manager_runtime_unit_id,
       required_set.manager_runtime_epoch,
       required_set.manager_owner_epoch,
       core::RuntimePolicyId(std::string(kProgramRunnerLivenessPolicyId)),
       core::RuntimePolicyRevision(
           std::string(kProgramRunnerLivenessPolicyRevision)),
       sequence,
       receiver_status_sequence,
       static_cast<std::int64_t>(local_age),
       core::ManagerLivenessStateDTO::kCurrent,
       false,
       fault_reference.has_value()
           ? std::optional<core::ContentDigest>(
                 core::ContentDigest(*fault_reference))
           : std::nullopt},
      required_set.active_bundle_id,
      required_set.required_runtime_set_digest,
      core::ContentDigest(
          RequiredString(response, "receiver_status_digest"))};
  Require(core::ValidateManagerLivenessObservation(observation).ok(),
          "Program Runner liveness observation is malformed");
  return {std::move(observation),
          RequiredProgramCycleStatus(
              RequiredMember(response, "program_cycle_status"))};
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
  Require(RequiredString(intent_value, "claim_kind") ==
                  "authoring_mutation" &&
              RequiredString(intent_value, "owner_role") ==
                  "authoring_owner",
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
  core::CellAdmissionClaimDTO claim{
      {
          core::CellAdmissionClaimKindDTO::kAuthoringMutation,
          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          core::RequestOwnerEpoch(RequiredString(intent_value, "owner_epoch")),
          RequiredString(intent_value, "owner_transaction_id"),
          core::RequestId(RequiredString(intent_value, "request_id")),
          core::ContentDigest(request_payload_digest),
          core::AdmissionGeneration(RequiredUnsigned(
              RequiredMember(intent_value, "expected_admission_generation"),
              "parent expected_admission_generation")),
          {},
          std::nullopt,
          std::nullopt,
      },
      core::CellAdmissionClaimGeneration(RequiredUnsigned(
          RequiredMember(value, "claim_generation"),
          "parent claim_generation")),
      core::AdmissionGeneration(RequiredUnsigned(
          RequiredMember(value, "acquired_admission_generation"),
          "parent acquired_admission_generation")),
      static_cast<std::int64_t>(RequiredNonnegativeUnsigned(
          RequiredMember(value, "accepted_at_ns"), "parent accepted_at_ns")),
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
  const auto retained_bundle_id =
      RequiredString(value, "retained_bundle_id");
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
      Require(binding.policy_id == "continuous_unattached" ||
                  binding.policy_id == "before_planning",
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
  if (command == "scene_commit") {
    RequireExactObjectSize(root, 3U, "scene commit command");
    auto transaction = scene::DecodeCanonicalSceneTransactionRequest(
        content_identity::canonical_json(RequiredMember(root, "transaction")));
    return {
        CommandKind::kSceneCommit,
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
    result.scene_resource_ids = RequiredSceneResourceIds(
        RequiredMember(root, "scene_resource_ids"));
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
    const auto quality = RequiredString(root, "quality");
    Require(quality == "good" || quality == "unknown",
            "quality must be good or unknown");
    result.pose_quality = quality == "good" ? PoseSourceQuality::kGood
                                            : PoseSourceQuality::kUnknown;
    result.pose_eligible = RequiredBool(root, "eligible");
    const auto &payload = RequiredMember(root, "payload");
    Require(payload.is_object(), "pose observation payload must be an object");
    result.pose_payload = payload;
    return result;
  }
  if (command == "scene_runtime_latch_before_planning") {
    RequireExactObjectSize(root, 4U,
                           "scene-runtime before-planning latch command");
    ManagerCommand result{.kind =
                              CommandKind::kSceneRuntimeLatchBeforePlanning};
    result.expected_scene_runtime_identity = RequiredSceneRuntimeIdentity(
        RequiredMember(root, "expected_runtime_identity"));
    result.expected_scene_projection = RequiredProjectionRevision(
        RequiredMember(root, "expected_projection_revision"));
    return result;
  }
  if (command == "apply_status") {
    RequireExactObjectSize(root, 4U, "Apply status command");
    ManagerCommand result{.kind = CommandKind::kApplyStatus};
    result.apply_id = core::ApplyId(RequiredString(root, "apply_id"));
    result.apply_generation = core::ApplyGeneration(RequiredUnsigned(
        RequiredMember(root, "apply_generation"), "apply_generation"));
    return result;
  }
  Require(command == "apply", "command kind is unsupported");
  RequireExactObjectSize(root, 11U, "Apply command");
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
  result.expected_current_retained_bundle_id =
      core::RetainedBundleId(expected_retained_bundle_id);
  result.expected_current_graph_revision =
      core::GraphRevision(expected_graph_revision);
  if (admission_kind == "direct") {
    RequireExactObjectSize(admission_context, 2U, "direct admission context");
    result.expected_admission_generation = core::AdmissionGeneration(
        RequiredUnsigned(
            RequiredMember(admission_context,
                           "expected_admission_generation"),
            "expected_admission_generation"));
  } else {
    Require(admission_kind == "parent_authoring_claim",
            "Apply admission context kind is unsupported");
    RequireExactObjectSize(admission_context, 2U,
                           "parent admission context");
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
      {"orientation_xyzw",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(value.orientation.x),
           CanonicalValue(value.orientation.y),
           CanonicalValue(value.orientation.z),
           CanonicalValue(value.orientation.w),
       })},
      {"position_m",
       CanonicalValue(CanonicalValue::Object{
           {"x", CanonicalValue(value.position_m.x_m)},
           {"y", CanonicalValue(value.position_m.y_m)},
           {"z", CanonicalValue(value.position_m.z_m)},
       })},
  });
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
      {"initialization_generation",
       Unsigned(snapshot.initialization_generation,
                "initialization generation")},
      {"initialization_state",
       CanonicalValue(std::string(ToString(snapshot.state)))},
      {"ok", CanonicalValue(true)},
      {"runtime_identity",
       SceneRuntimeIdentityResponseValue(*snapshot.runtime_identity)},
      {"scene_runtime_owner_epoch", CanonicalValue(std::string(owner_epoch))},
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

CanonicalValue SceneOwnerReadResponse(
    const core::RequestOwnerEpoch &request_owner_epoch,
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
      {"request_owner_epoch",
       CanonicalValue(request_owner_epoch.value())},
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

CanonicalValue SceneCandidatePreviewResponse(
    const core::Result<core::SceneCandidatePreviewDTO> &result) {
  if (!result.ok()) {
    return CanonicalValue(CanonicalValue::Object{
        {"api_version",
         CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
        {"error", ErrorValue(result.error())},
        {"ok", CanonicalValue(false)},
    });
  }
  const auto encoded = scene::EncodeSceneCandidatePreview(result.value());
  if (encoded.size() > kMaximumResponseBytes - 1024U) {
    return CanonicalValue(CanonicalValue::Object{
        {"api_version",
         CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
        {"error",
         ErrorValue(core::ErrorDTO{
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
      {"error", result.error.has_value() ? ErrorValue(*result.error)
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

CanonicalValue ErrorResponse(const std::string_view code,
                             const std::string_view message) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"error", CanonicalValue(CanonicalValue::Object{
                    {"code", CanonicalValue(std::string(code))},
                    {"message", CanonicalValue(std::string(message))},
                })},
      {"ok", CanonicalValue(false)},
  });
}

CanonicalValue DetailedErrorResponse(
    const std::string_view code, const std::string_view message,
    const std::map<std::string, std::string> &details) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"error", ErrorValue(core::ErrorDTO{std::string(code),
                                           std::string(message), details})},
      {"ok", CanonicalValue(false)},
  });
}

CanonicalValue ApplyErrorResponse(const ManagerCommand &command,
                                  const std::string_view code,
                                  const std::string_view message) {
  Require(command.request_id.has_value() &&
              command.request_owner_epoch.has_value(),
          "decoded Apply command lacks request identity");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"error", CanonicalValue(CanonicalValue::Object{
                    {"code", CanonicalValue(std::string(code))},
                    {"message", CanonicalValue(std::string(message))},
                })},
      {"ok", CanonicalValue(false)},
      {"request_id", CanonicalValue(*command.request_id)},
      {"request_owner_epoch", CanonicalValue(*command.request_owner_epoch)},
  });
}

CanonicalValue ApplyDetailedErrorResponse(const ManagerCommand &command,
                                          const core::ErrorDTO &error) {
  Require(command.request_id.has_value() &&
              command.request_owner_epoch.has_value(),
          "decoded Apply command lacks request identity");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"error", ErrorValue(error)},
      {"ok", CanonicalValue(false)},
      {"request_id", CanonicalValue(*command.request_id)},
      {"request_owner_epoch", CanonicalValue(*command.request_owner_epoch)},
  });
}

CanonicalValue ApplyUnavailableResponse(const ManagerCommand &command) {
  return ApplyErrorResponse(
      command, kKernelOwnerUnavailableReasonCode,
      "operational Apply requires one converged managed Kernel execution "
      "host");
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
  Require(succeeded == result.terminal_outcome.has_value(),
          "Manager Apply result has inconsistent terminal evidence");
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"apply_generation",
       Unsigned(result.apply_generation.value(), "Apply generation")},
      {"apply_id", CanonicalValue(result.apply_id.value())},
      {"candidate_projection_revision",
       succeeded ? ProjectionRevision(
                       result.terminal_outcome->candidate_projection_revision)
                 : CanonicalValue(nullptr)},
      {"error", CanonicalValue(nullptr)},
      {"ok", CanonicalValue(true)},
      {"replay_status",
       CanonicalValue(result.request_replayed ? "replayed_exact"
                                              : "accepted_new")},
      {"request_id", CanonicalValue(*command.request_id)},
      {"request_owner_epoch", CanonicalValue(*command.request_owner_epoch)},
      {"state", CanonicalValue(succeeded ? "succeeded" : "pending")},
  });
}

CanonicalValue ApplyStatusResponse(
    const core::ApplyId &apply_id,
    const core::ApplyGeneration apply_generation,
    const std::optional<component_manager::PersistedManagerApplyTerminalOutcome>
        &terminal) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kManagerCommandResponseApiVersion))},
      {"apply_generation",
       Unsigned(apply_generation.value(), "Apply generation")},
      {"apply_id", CanonicalValue(apply_id.value())},
      {"candidate_projection_revision",
       terminal.has_value()
           ? ProjectionRevision(
                 terminal->terminal_outcome.candidate_projection_revision)
           : CanonicalValue(nullptr)},
      {"error", CanonicalValue(nullptr)},
      {"ok", CanonicalValue(true)},
      {"state", CanonicalValue(terminal.has_value() ? "succeeded" : "pending")},
  });
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
    const auto bytes = content_identity::canonical_json(response);
    if (bytes.empty() || bytes.size() > kMaximumResponseBytes) {
      return;
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
      {std::string(deployment::native_execution_runtime_contract::
                       kLaunchEnvironmentContract),
       std::string(deployment::ros2_control_execution_runtime_contract::
                       kLaunchEnvironmentContract)});
  if (rows.empty()) {
    return std::nullopt;
  }
  return component_manager::ManagerBootstrapRuntimePlanDTO{
      admission.retained_bundle_id(), admission.graph_revision(),
      admission.admission_digest(), std::move(rows)};
}

[[nodiscard]] bool HasConvergedKernelApplyOwner(
    const component_manager::ManagerActiveRuntimeConvergenceResult &runtime) {
  if (runtime.state !=
      component_manager::ManagerActiveRuntimeConvergenceState::kConverged) {
    return false;
  }
  std::size_t native_execution_hosts = 0U;
  for (const auto &row : runtime.plan.runtime_units) {
    if (row.process_adapter_contract_id ==
        deployment::native_execution_runtime_contract::
            kProcessAdapterContractId) {
      ++native_execution_hosts;
    }
  }
  return native_execution_hosts == 1U;
}

class ManagerDaemon final {
public:
  explicit ManagerDaemon(DaemonConfig config)
      : config_(ValidateConfig(std::move(config))),
        started_at_(std::chrono::steady_clock::now()),
        package_generation_(
            config_.install_root,
            deployment::build_qualified_runtime_contract_registry()
                .package_manifest_set()),
        daemon_lock_(config_.manager_run_root),
        admission_(deployment::FilesystemCanonicalEmptyRetainedBundleSource(
                       package_generation_.descriptor_bound_root(),
                       config_.canonical_empty_admission_filename)
                       .load_verified()),
        bundle_source_(package_generation_.descriptor_bound_root()),
        artifact_source_(package_generation_.descriptor_bound_root()),
        candidate_source_(package_generation_.descriptor_bound_root(),
                          "maintained"),
        native_runtime_adapter_(
            InstalledTargetRoot(), package_generation_.descriptor_bound_root(),
            config_.manager_state_root, config_.manager_run_root,
            config_.runtime_state_root,
            bundle_source_, artifact_source_, "linux",
            std::vector<std::string>{
                std::string(deployment::native_execution_runtime_contract::
                                kLaunchEnvironmentContract),
                std::string(
                    deployment::ros2_control_execution_runtime_contract::
                        kLaunchEnvironmentContract)},
            config_.logical_scene_database, config_.program_cycle_journal,
            config_.authoring_transaction_journal),
        stores_(config_.manager_state_root, admission_),
        scene_owner_(config_.logical_scene_database, admission_,
                     stores_.active_bundle_publication_store(), bundle_source_,
                     native_runtime_adapter_, native_runtime_adapter_),
        scene_runtime_pose_projector_(scene_owner_, admission_.cell_id()),
        plan_source_(stores_.active_bundle_publication_store(),
                     stores_.apply_journal(),
                     BuildBootstrapRuntimePlan(admission_, artifact_source_)),
        reference_runtime_adapter_(config_.runtime_state_root, bundle_source_,
                                   artifact_source_, "linux", "posix.1-2017"),
        runtime_adapter_router_(
            {{std::string(
                  component_manager::kRetainedPosixProcessAdapterContractId),
              &reference_runtime_adapter_, &reference_runtime_adapter_},
             {std::string(deployment::native_execution_runtime_contract::
                              kProcessAdapterContractId),
              &native_runtime_adapter_, &native_runtime_adapter_}}),
        active_runtime_(plan_source_, runtime_adapter_router_,
                        runtime_adapter_router_,
                        [this] { return ElapsedNs(); },
                        [this] { RequireCurrentPackageGeneration(); }),
        manager_runtime_epoch_(RandomToken("manager-runtime-")),
        named_apply_(
            stores_.apply_journal(), plan_source_, bundle_source_,
            artifact_source_, stores_.active_bundle_publication_store(),
            scene_owner_, &scene_owner_, native_runtime_adapter_,
            native_runtime_adapter_,
            native_runtime_adapter_, runtime_adapter_router_,
            runtime_adapter_router_, manager_runtime_epoch_, "linux",
            {std::string(deployment::native_execution_runtime_contract::
                             kLaunchEnvironmentContract),
             std::string(
                 deployment::ros2_control_execution_runtime_contract::
                     kLaunchEnvironmentContract)},
            [this] { return ElapsedNs(); },
            [] { return core::ApplyId(RandomToken("apply-")); },
            [this] { RequireCurrentPackageGeneration(); }),
        scene_runtime_owner_(
            [] { return HostMonotonicNs(); },
            [] { return RandomToken("scene-init-"); }),
        manager_channel_(config_.projection_root,
                         std::string(kManagerSourceFileName)),
        scene_channel_(config_.projection_root,
                       std::string(kLogicalSceneSourceFileName)),
        scene_runtime_channel_(config_.projection_root,
                               std::string(kSceneRuntimeSourceFileName)),
        manager_owner_epoch_(RandomToken("manager-")),
        scene_runtime_owner_epoch_(RandomToken("scene-runtime-")),
        manager_owner_id_("component-manager." + admission_.cell_id().value()),
        scene_owner_id_("logical-scene." + admission_.cell_id().value()),
        scene_runtime_owner_id_("scene-runtime." +
                                admission_.cell_id().value()) {
    RequireCurrentPackageGeneration();
    EnsureBootstrap();
    RequireCurrentPackageGeneration();
    if (TryRecoverIncompleteApply()) {
      static_cast<void>(ReconcileAndPublish());
    }
    static_cast<void>(ReconcileAndPublish());
  }

  int Run() {
    CommandSocket command_socket(config_.manager_run_root);
    StartNativeManagerLivenessLane();
    try {
      while (g_stop_requested == 0) {
        pollfd descriptor{command_socket.get(), POLLIN, 0};
        int ready = -1;
        do {
          ready = ::poll(
              &descriptor, 1,
              static_cast<int>(config_.reconciliation_period.count()));
        } while (ready < 0 && errno == EINTR && g_stop_requested == 0);
        if (g_stop_requested != 0) {
          break;
        }
        if (ready < 0) {
          FailErrno("poll Manager command socket");
        }
        if (ready == 0) {
          TryPeriodicReconcile();
          continue;
        }
        if ((descriptor.revents & POLLIN) != 0 &&
            HandleOneClient(command_socket)) {
          StopNativeManagerLivenessLane();
          return 0;
        }
        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
          Fail("Manager command socket became unavailable");
        }
      }
    } catch (...) {
      StopNativeManagerLivenessLane();
      throw;
    }
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
    config.manager_state_root = ExistingDirectory(
        std::move(config.manager_state_root), "Manager state root");
    config.manager_run_root = ExistingDirectory(
        std::move(config.manager_run_root), "Manager run root");
    config.runtime_state_root = ExistingDirectory(
        std::move(config.runtime_state_root), "runtime state root");
    config.projection_root = ExistingDirectory(
        std::move(config.projection_root), "projection root");
    config.logical_scene_database =
        DatabasePath(std::move(config.logical_scene_database));
    config.program_cycle_journal = ProspectiveDatabasePath(
        std::move(config.program_cycle_journal), "Program-cycle journal");
    config.authoring_transaction_journal = ProspectiveDatabasePath(
        std::move(config.authoring_transaction_journal),
        "Composer authoring transaction journal");
    if (!config.program_runner_liveness_socket.empty()) {
      Require(config.program_runner_liveness_socket.is_absolute() &&
                  config.program_runner_liveness_socket.has_filename(),
              "Program Runner liveness socket path must be absolute");
      static_cast<void>(ExistingDirectory(
          config.program_runner_liveness_socket.parent_path(),
          "Program Runner liveness socket parent"));
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

    const auto finalized = stores_.apply_journal().ReadSceneFinalized(
        named->apply_id, named->apply_generation);
    if (finalized.has_value()) {
      const auto &result = finalized->scene_finalized.finalized_result;
      Require(
          result.state ==
                  core::ProjectionApplyFinalizeResultState::kFinalized &&
              result.apply_id == named->apply_id &&
              result.apply_generation == named->apply_generation &&
              result.projection_stage_generation ==
                  named->projection_stage_generation &&
              result.candidate_projection_revision ==
                  active_publication.request.candidate_projection_revision &&
              result.publication_receipt == active_publication,
          "scene-runtime finalized evidence differs from the active "
          "publication");
      return {active_publication.request.candidate_projection_revision,
              active_publication.publication_generation};
    }

    const auto intent = stores_.apply_journal().ReadIntent(
        named->apply_id, named->apply_generation);
    const auto stage = stores_.apply_journal().ReadSceneStageIntent(
        named->apply_id, named->apply_generation);
    const auto activation = stores_.apply_journal().ReadActivationSelection(
        named->apply_id, named->apply_generation);
    Require(intent.has_value() && stage.has_value() && activation.has_value() &&
                intent->intent.candidate_retained_bundle_id ==
                    active_publication.request.candidate_retained_bundle_id &&
                intent->intent.candidate_graph_revision ==
                    active_publication.request.candidate_graph_revision &&
                activation->activation_selection
                        .projection_stage_generation ==
                    named->projection_stage_generation &&
                activation->activation_selection
                        .candidate_projection_revision ==
                    active_publication.request
                        .candidate_projection_revision &&
                stage->scene_stage_intent.candidate_retained_bundle_id ==
                    active_publication.request.candidate_retained_bundle_id &&
                stage->scene_stage_intent.candidate_graph_revision ==
                    active_publication.request.candidate_graph_revision &&
                active_publication.request
                        .expected_previous_publication_generation ==
                    std::optional<core::ActiveBundlePublicationGeneration>{
                        stage->scene_stage_intent
                            .predecessor_publication_generation},
            "unfinalized scene-runtime publication lacks its exact durable "
            "predecessor evidence");
    return {intent->intent.expected_active_projection_revision,
            stage->scene_stage_intent.predecessor_publication_generation};
  }

  PublicationGenerations ReconcileAndPublish() {
    RequireCurrentPackageGeneration();
    const auto runtime = active_runtime_.Reconcile();
    last_runtime_ = runtime;
    kernel_apply_available_ = HasConvergedKernelApplyOwner(runtime);
    if (kernel_apply_available_ &&
        !scene_admission_reconciled_.load(std::memory_order_acquire)) {
      const auto reconciled =
          scene_owner_.ReconcileAuthoredSceneAdmissionClaims();
      if (reconciled.ok()) {
        scene_admission_reconciled_.store(true, std::memory_order_release);
      }
    }
    const auto bundle_id =
        runtime.plan.active_publication.request.candidate_retained_bundle_id;
    const auto active_bundle = bundle_source_.get_verified(bundle_id);
    Require(active_bundle != nullptr,
            "active publication has no independently verified retained bundle");
    last_active_bundle_ = active_bundle;
    if (runtime.state ==
        component_manager::ManagerActiveRuntimeConvergenceState::kConverged) {
      const auto current_bindings =
          component_manager::DeriveActiveNativeProjectionRuntimeBindings(
              active_bundle->value(), runtime.observations);
      if (!last_reconciled_native_runtime_bindings_.has_value() ||
          *last_reconciled_native_runtime_bindings_ != current_bindings) {
        // Full native rebuild/recovery is a startup or process-incarnation
        // transition effect. Unchanged observations perform no scene-store
        // read and no native recovery work.
        if (!current_bindings.empty()) {
          const auto scene_access = scene_owner_.ReadAccessStatus();
          Require(scene_access.active_projection.has_value(),
                  "active publication has no finalized logical-scene "
                  "projection");
          const auto runtime_set =
              component_manager::DeriveActiveNativeProjectionRuntimeSet(
                  active_bundle->value(), *scene_access.active_projection,
                  runtime.observations);
          const auto recovery =
              scene_owner_.ReconcileActiveProjectionRuntimes(runtime_set);
          Require(
              recovery.valid() && recovery.runtime_set == runtime_set,
              "active native projection recovery returned foreign evidence");
        }
        last_reconciled_native_runtime_bindings_ = current_bindings;
      }
    }
    const auto observed_at = static_cast<std::int64_t>(ElapsedNs());
    const auto finalized_scene_runtime =
        SelectFinalizedSceneRuntime(runtime.plan.active_publication);
    const auto finalized_bundle =
        finalized_scene_runtime.projection_revision.retained_bundle_id ==
                bundle_id
            ? active_bundle
            : bundle_source_.get_verified(
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
    if (scene_runtime_owner_.Snapshot().initialization_generation == 0U) {
      scene_runtime_owner_.EstablishOpenRuntime(
          desired_scene_runtime, publication_generation,
          "durable_finalized_active_projection");
    } else {
      const auto changed = scene_runtime_owner_.AdoptFinalizedOpenRuntime(
          desired_scene_runtime, publication_generation,
          "durable_finalized_active_projection");
      if (changed) {
        pose_observer_sessions_.clear();
        pose_observation_replays_.clear();
      }
    }
    const auto manager = manager_channel_.Publish(
        "component_manager", manager_owner_id_, manager_owner_epoch_,
        observed_at,
        BuildManagerSourcePayload(
            *active_bundle, runtime, stores_.apply_journal(),
            kernel_apply_available_
                ? ManagerApplyMutationAvailability::kAvailable
                : ManagerApplyMutationAvailability::kKernelOwnerUnavailable));

    const auto scene_generation = PublishLogicalScene(observed_at);
    const auto scene_runtime_generation = PublishSceneRuntime(observed_at);
    const auto apply_inventory =
        stores_.apply_journal().ReadStatusInventory();
    const auto unsettled = static_cast<std::size_t>(std::count_if(
        apply_inventory.begin(), apply_inventory.end(), [](const auto& status) {
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
      const component_manager::ManagerActiveRuntimeConvergenceResult& runtime,
      const core::ProgramCycleStatusDTO& program_cycle_status,
      const std::uint32_t unsettled_effect_count,
      const bool forced_closed) {
    const auto component_total = runtime.plan.runtime_units.size();
    Require(component_total <= std::numeric_limits<std::uint32_t>::max(),
            "required runtime-unit count exceeds readiness capacity");
    std::uint32_t ready = 0U;
    std::uint32_t faults = 0U;
    for (const auto& row : runtime.plan.runtime_units) {
      const auto* observation =
          runtime.observations.observation(row.runtime_unit_id.value());
      Require(observation != nullptr,
              "required runtime-unit observation is absent");
      if (observation->converged()) {
        ++ready;
      }
      if (observation->source_phase() ==
              component_manager::RuntimeSourcePhase::kFaulted ||
          observation->source_health() ==
              component_manager::RuntimeSourceHealth::kFaulted) {
        ++faults;
      }
    }
    return ReduceManagerCellAdmissionReadiness({
        !forced_closed,
        program_cycle_status,
        !forced_closed,
        static_cast<std::uint32_t>(component_total),
        ready,
        !forced_closed,
        runtime.state ==
            component_manager::ManagerActiveRuntimeConvergenceState::kConverged,
        faults,
        scene_admission_reconciled_.load(std::memory_order_acquire) ? 0U : 1U,
        unsettled_effect_count,
    });
  }

  void SubmitCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO& snapshot) {
    const auto updated =
        native_runtime_adapter_.UpdateCellAdmissionReadiness(snapshot);
    if (!updated.ok()) {
      throw std::runtime_error(
          "Kernel rejected Manager-owned cell-admission readiness: " +
          updated.error().message);
    }
    last_submitted_readiness_ = snapshot;
  }

  void SubmitFailClosedReadiness(
      const std::optional<core::ProgramCycleStatusDTO>& status =
          std::nullopt) noexcept {
    const auto current = status.has_value()
                             ? status
                             : (last_submitted_readiness_.has_value()
                                    ? last_submitted_readiness_
                                          ->program_cycle_status
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
    try {
      SubmitCellAdmissionReadiness(closed);
    } catch (...) {
      // Withhold renewal as well; Kernel liveness independently expires.
    }
  }

  [[nodiscard]] static bool SameNativeManagerLivenessIdentity(
      const NativeManagerLivenessSnapshot& left,
      const NativeManagerLivenessSnapshot& right) {
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
    for (const auto& row : left.runtime.plan.runtime_units) {
      const auto right_row = std::find_if(
          right.runtime.plan.runtime_units.begin(),
          right.runtime.plan.runtime_units.end(), [&](const auto& candidate) {
            return candidate.runtime_unit_id == row.runtime_unit_id &&
                   candidate.process_adapter_contract_id ==
                       row.process_adapter_contract_id &&
                   candidate.product_identity == row.product_identity;
          });
      const auto* left_observation =
          left.runtime.observations.observation(row.runtime_unit_id.value());
      const auto* right_observation =
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

  void PublishNativeManagerLivenessSnapshot(
      NativeManagerLivenessSnapshot snapshot) {
    if (config_.program_runner_liveness_socket.empty()) {
      return;
    }
    bool wake = false;
    {
      std::lock_guard lock(manager_liveness_lane_mutex_);
      wake = !manager_liveness_snapshot_.has_value() ||
             !SameNativeManagerLivenessIdentity(
                 *manager_liveness_snapshot_, snapshot);
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

  void TryRenewNativeManagerLiveness(
      const NativeManagerLivenessSnapshot& snapshot,
      const bool forced_closed,
      const std::uint64_t readiness_barrier) noexcept {
    const auto& runtime = snapshot.runtime;
    const auto& active_bundle = *snapshot.active_bundle;
    const auto now_ns = ElapsedNs();
    if (!manager_liveness_retry_gate_.attempt_allowed(now_ns)) {
      return;
    }
    std::optional<core::ProgramCycleStatusDTO> probed_cycle_status;
    try {
      const auto runner = ProbeProgramRunnerLiveness(
          config_.program_runner_liveness_socket, admission_.cell_id());
      probed_cycle_status = runner.program_cycle_status;
      const auto readiness =
          BuildCellAdmissionReadiness(
              runtime, *probed_cycle_status,
              snapshot.unsettled_effect_count, forced_closed);
      if (last_submitted_readiness_.has_value() &&
          *last_submitted_readiness_ != readiness) {
        // Close first so a later renewal/submission failure cannot leave the
        // previous optimistic reduction installed.
        SubmitFailClosedReadiness(probed_cycle_status);
        Require(last_submitted_readiness_.has_value() &&
                    !last_submitted_readiness_->program_cycle_status_current,
                "Kernel could not be closed before a readiness transition");
      }
      std::vector<core::RequiredManagerLivenessReceiverDTO> native_receivers;
      native_receivers.reserve(runtime.plan.runtime_units.size());
      const auto& retained_policy =
          deployment::native_execution_runtime_contract::
              manager_liveness_policy();
      for (const auto& row : runtime.plan.runtime_units) {
        if (row.process_adapter_contract_id !=
            deployment::native_execution_runtime_contract::
                kProcessAdapterContractId) {
          continue;
        }
        const auto* observation =
            runtime.observations.observation(row.runtime_unit_id.value());
        if (observation == nullptr || observation->runtime_epoch().empty()) {
          throw std::runtime_error(
              "required native liveness receiver is not observable");
        }
        const auto product = deployment::verify_runtime_unit_product(
            active_bundle, row.runtime_unit_id, artifact_source_);
        if (!product.has_value() ||
            product->retained_bundle_id() !=
                active_bundle.retained_bundle_id() ||
            product->product_identity() != row.product_identity ||
            !product->manager_liveness_policy().has_value() ||
            *product->manager_liveness_policy() != retained_policy) {
          throw std::runtime_error(
              "active native runtime has no exact retained liveness policy");
        }
        native_receivers.push_back(core::RequiredManagerLivenessReceiverDTO{
            row.runtime_unit_id, core::RuntimeEpoch(observation->runtime_epoch()),
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
                [](const auto& left, const auto& right) {
                  return std::tie(left.runtime_unit_id, left.runtime_epoch,
                                  left.runtime_policy_id,
                                  left.runtime_policy_revision) <
                         std::tie(right.runtime_unit_id, right.runtime_epoch,
                                  right.runtime_policy_id,
                                  right.runtime_policy_revision);
                });
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
          manager_heartbeat_sequence_ >=
              core::kMaximumManagerLivenessCounter) {
        throw std::runtime_error(
            "Manager cannot mint the exact native liveness required set");
      }
      ++manager_heartbeat_sequence_;
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
        for (const auto& observation :
             runtime.observations.observations()) {
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
      for (const auto& receiver : native_receivers) {
        requests.push_back(
            {receiver.runtime_unit_id,
             runtime.plan.active_publication.publication_generation,
             runtime.plan.active_publication.request.candidate_graph_revision,
             runtime.plan.active_publication
                 .durable_public_pointer_content_digest,
             required_set, retained_policy, heartbeat, reconciliation});
      }
      const auto responses =
          requests.empty()
              ? std::vector<
                    core::ManagerLivenessObservationDTO>{}
              : native_runtime_adapter_.RenewManagerLivenessBatch(requests);
      if (responses.size() != native_receivers.size()) {
        throw std::runtime_error(
            "native liveness batch returned the wrong receiver count");
      }
      for (std::size_t index = 0U; index < native_receivers.size(); ++index) {
        const auto& receiver = native_receivers[index];
        const auto& response = responses[index];
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
             (response.status.state != core::ManagerLivenessStateDTO::kCurrent ||
              response.status.command_fenced))) {
          throw std::runtime_error(
              "native liveness receiver returned foreign reconciliation "
              "evidence");
        }
      }
      if (!reconciliation.has_value()) {
        throw std::runtime_error(
            "Program Runner liveness requires converged reconciliation");
      }
      const auto runner_renewal = RenewProgramRunnerLiveness(
          config_.program_runner_liveness_socket, runner, required_set,
          manager_heartbeat_sequence_,
          reconciliation->reconciliation_evidence_digest);
      Require(runner_renewal.program_cycle_status == *probed_cycle_status,
              "Program cycle changed across one Manager liveness renewal");
      const auto published_runner =
          native_runtime_adapter_.Publish(runner_renewal.observation);
      if (!published_runner.ok()) {
        throw std::runtime_error(
            "Kernel rejected Program Runner liveness observation: " +
            published_runner.error().message);
      }
      if (!last_submitted_readiness_.has_value() ||
          *last_submitted_readiness_ != readiness) {
        {
          std::lock_guard lock(manager_liveness_lane_mutex_);
          Require(forced_closed ||
                      (readiness_barrier ==
                           manager_liveness_readiness_barrier_ &&
                       !manager_liveness_force_closed_),
                  "Manager liveness snapshot changed during renewal");
        }
        SubmitCellAdmissionReadiness(readiness);
      }
      manager_liveness_retry_gate_.RecordSuccess();
    } catch (const component_manager::
                 ManagerLivenessPublicationSuperseded&) {
      SubmitFailClosedReadiness(probed_cycle_status);
      {
        std::lock_guard lock(manager_liveness_lane_mutex_);
        if (!manager_liveness_snapshot_.has_value() ||
            !SameNativeManagerLivenessIdentity(
                snapshot, *manager_liveness_snapshot_)) {
          return;
        }
        manager_liveness_superseded_publication_ =
            snapshot.runtime.plan.active_publication;
      }
      // Every native receiver rejected before renewal and the adapter drained
      // the complete batch.  Preserve fail-closed readiness, but do not spend
      // the lease-expiry retry budget on this expected publication race.
      manager_liveness_retry_gate_.RecordSuccess();
    } catch (const std::exception& error) {
      SubmitFailClosedReadiness(probed_cycle_status);
      {
        std::lock_guard lock(manager_liveness_lane_mutex_);
        if (manager_liveness_snapshot_.has_value() &&
            !SameNativeManagerLivenessIdentity(
                snapshot, *manager_liveness_snapshot_)) {
          return;
        }
      }
      const auto failure_ns = ElapsedNs();
      const auto expiry = static_cast<std::uint64_t>(
          deployment::native_execution_runtime_contract::
              manager_liveness_policy()
                  .expiry_timeout_ns);
      manager_liveness_retry_gate_.RecordFailure(failure_ns, expiry);
      std::cerr << "Manager native liveness renewal remains fail-closed: "
                << error.what() << '\n';
    } catch (...) {
      SubmitFailClosedReadiness(probed_cycle_status);
      {
        std::lock_guard lock(manager_liveness_lane_mutex_);
        if (manager_liveness_snapshot_.has_value() &&
            !SameNativeManagerLivenessIdentity(
                snapshot, *manager_liveness_snapshot_)) {
          return;
        }
      }
      const auto failure_ns = ElapsedNs();
      const auto expiry = static_cast<std::uint64_t>(
          deployment::native_execution_runtime_contract::
              manager_liveness_policy()
                  .expiry_timeout_ns);
      manager_liveness_retry_gate_.RecordFailure(failure_ns, expiry);
      std::cerr << "Manager native liveness renewal remains fail-closed\n";
    }
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
        deployment::native_execution_runtime_contract::
            manager_liveness_policy()
                .heartbeat_period_ns);
    std::unique_lock lock(manager_liveness_lane_mutex_);
    while (!manager_liveness_lane_stop_) {
      const auto snapshot = manager_liveness_snapshot_;
      const auto superseded_publication =
          manager_liveness_superseded_publication_;
      const auto forced_closed = manager_liveness_force_closed_;
      const auto readiness_barrier = manager_liveness_readiness_barrier_;
      const auto wake_generation = manager_liveness_wake_generation_;
      lock.unlock();
      if (snapshot.has_value() &&
          (!superseded_publication.has_value() ||
           snapshot->runtime.plan.active_publication !=
               *superseded_publication)) {
        if (!manager_liveness_attempted_publication_.has_value() ||
            *manager_liveness_attempted_publication_ !=
                snapshot->runtime.plan.active_publication) {
          // A durable successor is independent work.  Never carry a
          // predecessor batch's retry delay across the publication boundary.
          manager_liveness_retry_gate_.RecordSuccess();
          manager_liveness_attempted_publication_ =
              snapshot->runtime.plan.active_publication;
        }
        TryRenewNativeManagerLiveness(*snapshot, forced_closed,
                                      readiness_barrier);
      } else {
        SubmitFailClosedReadiness();
      }
      lock.lock();
      static_cast<void>(manager_liveness_lane_condition_.wait_for(
          lock, heartbeat_period, [&] {
            return manager_liveness_lane_stop_ ||
                   manager_liveness_wake_generation_ != wake_generation;
          }));
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

  std::uint64_t PublishSceneRuntime(const std::int64_t observed_at) {
    return scene_runtime_channel_
        .Publish(
            "scene_runtime", scene_runtime_owner_id_,
            scene_runtime_owner_epoch_, observed_at,
            BuildSceneRuntimeSourcePayload(admission_.cell_id().value(),
                                           scene_runtime_owner_.Snapshot()))
        .source_generation;
  }

  void TryPeriodicReconcile() noexcept {
    try {
      RequireCurrentPackageGeneration();
      if (TryRecoverIncompleteApply()) {
        static_cast<void>(ReconcileAndPublish());
      }
      static_cast<void>(ReconcileAndPublish());
    } catch (const ManagerPackageGenerationRestartRequired &error) {
      TrySubmitFailClosedReadiness();
      PublishPackageRestartRequired();
      std::cerr << "Manager daemon package generation changed: "
                << error.what() << '\n';
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

  bool TryRecoverIncompleteApply() noexcept {
    try {
      return named_apply_.RecoverIncompleteApply().has_value();
    } catch (const std::exception &error) {
      std::cerr << "Manager daemon incomplete Apply recovery remains "
                   "fail-closed: "
                << error.what() << '\n';
    } catch (...) {
      std::cerr << "Manager daemon incomplete Apply recovery remains "
                   "fail-closed after an unknown failure\n";
    }
    return false;
  }

  void RequireCurrentPackageGeneration() {
    package_generation_.RequireCurrent(
        deployment::build_qualified_runtime_contract_registry()
            .package_manifest_set());
  }

  void PublishPackageRestartRequired() noexcept {
    kernel_apply_available_ = false;
    if (!last_runtime_.has_value() || last_active_bundle_ == nullptr) {
      return;
    }
    try {
      static_cast<void>(manager_channel_.Publish(
          "component_manager", manager_owner_id_, manager_owner_epoch_,
          static_cast<std::int64_t>(ElapsedNs()),
          BuildManagerSourcePayload(
              *last_active_bundle_, *last_runtime_, stores_.apply_journal(),
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

  core::Result<SceneRuntimePoseApplicationResult>
  ResolvePoseApplication(const SceneRuntimeIdentity &runtime_identity,
                         const PoseSourceObservation &observation,
                         const PoseObservationAcceptance &acceptance) {
    const auto no_application =
        [](const SceneRuntimePoseApplicationStatus status) {
          return core::Result<SceneRuntimePoseApplicationResult>::Ok(
              SceneRuntimePoseApplicationResult{status, std::nullopt,
                                                std::nullopt, std::nullopt});
        };
    if (!observation.eligible ||
        observation.quality != PoseSourceQuality::kGood || !acceptance.fresh) {
      return no_application(SceneRuntimePoseApplicationStatus::kNotRequested);
    }
    if (observation.key.policy_id == "before_planning") {
      return no_application(
          SceneRuntimePoseApplicationStatus::kDeferredBeforePlanning);
    }
    if (acceptance.disposition == PoseObservationDisposition::kBuffered ||
        acceptance.disposition ==
            PoseObservationDisposition::kReplacedBuffered) {
      return no_application(SceneRuntimePoseApplicationStatus::kBuffered);
    }
    if (observation.key.policy_id != "continuous_unattached") {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          core::ErrorDTO{"scene_runtime_pose_policy_unsupported",
                         "eligible pose observation uses an unsupported "
                         "application policy",
                         {{"policy_id", observation.key.policy_id}}});
    }
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

    std::shared_ptr<const deployment::RetainedBundle> bundle;
    try {
      bundle = bundle_source_.get_verified(
          core::RetainedBundleId(runtime_identity.retained_bundle_id));
    } catch (const std::exception &error) {
      return core::Result<SceneRuntimePoseApplicationResult>::Err(
          core::ErrorDTO{
              "scene_runtime_pose_bundle_unavailable", error.what(), {}});
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
    return scene_runtime_pose_projector_.Apply(
        bundle->value().capability_provider_bindings,
        bundle->retained_bundle_id(), bundle->value().cell_graph.graph_revision,
        observation);
  }

  core::Result<BeforePlanningLatchResult> LatchBeforePlanningObservations(
      const SceneRuntimeIdentity &runtime_identity,
      const core::ProjectionRevisionDTO &expected_projection) {
    const auto failure = [](std::string code, std::string message,
                            std::map<std::string, std::string> details = {}) {
      return core::Result<BeforePlanningLatchResult>::Err(core::ErrorDTO{
          std::move(code), std::move(message), std::move(details)});
    };
    const auto owner = scene_runtime_owner_.Snapshot();
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
      bundle = bundle_source_.get_verified(
          core::RetainedBundleId(runtime_identity.retained_bundle_id));
    } catch (const std::exception &error) {
      return failure("scene_runtime_pose_bundle_unavailable", error.what());
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

    struct LatchCandidate final {
      PoseObservationReplay *replay{};
      PoseSourceObservation observation;
    };
    std::vector<LatchCandidate> candidates;
    for (const auto &[source_id, session] : pose_observer_sessions_) {
      if (session.runtime_identity != runtime_identity) {
        continue;
      }
      for (const auto &binding : session.bindings) {
        if (!binding.eligible || binding.policy_id != "before_planning") {
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
            observed->observation.quality != PoseSourceQuality::kGood) {
          return failure(
              "scene_runtime_pose_observation_stale",
              "before-planning pose source has no fresh eligible observation",
              {{"object_id", binding.object_id}, {"source_id", source_id}});
        }
        const auto replay = pose_observation_replays_.find(key);
        if (replay == pose_observation_replays_.end() ||
            replay->second.runtime_identity != runtime_identity ||
            replay->second.observation != observed->observation) {
          return failure(
              "scene_runtime_pose_observation_unretained",
              "before-planning pose observation has no exact replay record",
              {{"object_id", binding.object_id}, {"source_id", source_id}});
        }
        candidates.push_back(
            LatchCandidate{&replay->second, observed->observation});
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
      for (auto *const record : applied_records) {
        record->publication_generation = generation;
      }
    };
    for (auto &candidate : candidates) {
      auto application = scene_runtime_pose_projector_.Apply(
          bundle->value().capability_provider_bindings,
          bundle->retained_bundle_id(),
          bundle->value().cell_graph.graph_revision, candidate.observation);
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
        ++result.already_applied_observation_count;
        break;
      case SceneRuntimePoseApplicationStatus::kSkippedAttached:
        ++result.skipped_attached_observation_count;
        break;
      case SceneRuntimePoseApplicationStatus::kNotRequested:
      case SceneRuntimePoseApplicationStatus::kDeferredBeforePlanning:
      case SceneRuntimePoseApplicationStatus::kBuffered:
        try {
          publish_results();
        } catch (...) {
        }
        return failure(
            "scene_runtime_pose_latch_result_invalid",
            "before-planning pose projector returned a non-latched status");
      }
      candidate.replay->application = application.value();
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
      const auto command = DecodeCommand(*packet);
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
      case CommandKind::kApply: {
        if (*command.request_owner_epoch != manager_owner_epoch_) {
          TrySendResponse(
              client.get(),
              ApplyErrorResponse(
                  command, "stale_request_owner_epoch",
                  "Apply request targets a stale Manager owner epoch"));
          return false;
        }
        Require(command.deployment_artifact_id.has_value() &&
                    command.deployment_revision.has_value() &&
                    command.expected_current_retained_bundle_id.has_value() &&
                    command.expected_current_graph_revision.has_value() &&
                    command.request_id.has_value() &&
                    command.request_owner_epoch.has_value() &&
                    command.source_label.has_value(),
                "decoded Apply command lacks its exact selection");
        try {
          const auto existing = stores_.apply_journal().ReadRequest(
              core::RequestOwnerEpoch(*command.request_owner_epoch),
              core::RequestId(*command.request_id));
          if (!existing.has_value() && !kernel_apply_available_) {
            TrySendResponse(client.get(), ApplyUnavailableResponse(command));
            return false;
          }
          const auto candidate_retained_bundle_id =
              existing.has_value()
                  ? existing->intent.candidate_retained_bundle_id
                  : candidate_source_
                        .ResolveExact({*command.deployment_artifact_id,
                                       *command.deployment_revision})
                        .retained_bundle_id;
          const auto result = named_apply_.ApplyOrRecover({
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
              command.expected_admission_generation,
              command.parent_authoring_claim,
          });
          // Apply durability is authoritative. A source-publication failure is
          // logged and retried by the periodic strand; it cannot turn a
          // successful Apply into a second mutation on client retry.
          try {
            static_cast<void>(ReconcileAndPublish());
          } catch (const std::exception &error) {
            TrySubmitFailClosedReadiness();
            std::cerr << "Manager daemon publication failed after durable "
                         "Apply: "
                      << error.what() << '\n';
          }
          TrySendResponse(client.get(), ApplyResponse(command, result));
        } catch (const component_manager::ManagerNamedApplyBusyError &error) {
          TrySendResponse(
              client.get(),
              ApplyErrorResponse(command, "apply_busy", error.what()));
        } catch (const ManagerPackageGenerationRestartRequired &error) {
          PublishPackageRestartRequired();
          TrySendResponse(client.get(),
                          DetailedErrorResponse(error.error().code,
                                                error.error().message,
                                                error.error().details));
        } catch (const std::exception &error) {
          TrySendResponse(
              client.get(),
              ApplyErrorResponse(command, "apply_rejected", error.what()));
        }
        return false;
      }
      case CommandKind::kApplyStatus: {
        Require(command.apply_id.has_value() &&
                    command.apply_generation.has_value(),
                "decoded Apply status command lacks its durable identity");
        const auto intent = stores_.apply_journal().ReadIntent(
            *command.apply_id, *command.apply_generation);
        if (!intent.has_value()) {
          TrySendResponse(client.get(),
                          ErrorResponse("apply_status_not_found",
                                        "Manager Apply identity is unknown"));
          return false;
        }
        const auto terminal = stores_.apply_journal().ReadTerminalOutcome(
            *command.apply_id, *command.apply_generation);
        TrySendResponse(client.get(),
                        ApplyStatusResponse(*command.apply_id,
                                            *command.apply_generation,
                                            terminal));
        return false;
      }
      case CommandKind::kSceneCommit: {
        Require(command.scene_transaction.has_value(),
                "decoded scene command lacks its transaction");
        const auto result = scene_owner_.Commit(*command.scene_transaction);
        if (result.success) {
          // The durable scene result remains authoritative if this projection
          // write fails. Periodic reconciliation republishes it, and exact
          // request replay returns the same owner result without a second
          // native scene effect.
          TryPublishLogicalScene();
        }
        TrySendResponse(client.get(), SceneCommitResponse(command, result));
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
        TrySendResponse(
            client.get(),
            SceneCandidatePreviewResponse(
                scene_owner_.PreviewResolvedSceneCandidate(
                    *command.scene_candidate_preview)));
        return false;
      }
      case CommandKind::kScenePoseQuery: {
        Require(command.expected_scene_projection.has_value() &&
                    !command.scene_resource_ids.empty(),
                "decoded scene pose query lacks its exact selection");
        const auto current = scene_owner_.CurrentProjectionRevision();
        if (current != *command.expected_scene_projection) {
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
        const auto snapshot = scene_runtime_owner_.Snapshot();
        if (!snapshot.runtime_identity.has_value() ||
            *snapshot.runtime_identity !=
                *command.expected_scene_runtime_identity) {
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
                    command.pose_quality.has_value() &&
                    command.pose_eligible.has_value() &&
                    command.pose_payload.has_value(),
                "decoded scene-runtime observation is incomplete");
        const auto snapshot = scene_runtime_owner_.Snapshot();
        if (!snapshot.runtime_identity.has_value() ||
            *snapshot.runtime_identity !=
                *command.expected_scene_runtime_identity) {
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
             *command.pose_quality != PoseSourceQuality::kGood)) {
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
        PoseObservationReplay *record = nullptr;
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
              exact_record.observation.quality == *command.pose_quality &&
              exact_record.observation.eligible == *command.pose_eligible &&
              exact_record.observation.payload == *command.pose_payload;
          if (!exact) {
            TrySendResponse(
                client.get(),
                ErrorResponse("scene_runtime_observation_request_conflict",
                              "pose observation identity was reused with "
                              "changed intent"));
            return false;
          }
          record = &replay->second;
          if (record->application.has_value()) {
            if (!record->publication_generation.has_value()) {
              record->publication_generation =
                  PublishSceneRuntime(static_cast<std::int64_t>(ElapsedNs()));
            }
            TrySendResponse(client.get(),
                            SceneRuntimeObserveResponse(
                                record->observation, record->acceptance,
                                *record->application,
                                *record->publication_generation));
            return false;
          }
        } else {
          const auto observed_at = HostMonotonicNs();
          constexpr auto kMaximumSafeInteger = 9007199254740991ULL;
          const auto observed_unsigned =
              static_cast<std::uint64_t>(observed_at);
          if (*command.pose_lifetime_ns >
              kMaximumSafeInteger - observed_unsigned) {
            TrySendResponse(
                client.get(),
                ErrorResponse(
                    "scene_runtime_observation_expiry_invalid",
                    "pose observation expiry exceeds the safe range"));
            return false;
          }
          PoseSourceObservation observation{
              observation_key,
              *command.pose_source_epoch,
              session->second.source_epoch_order,
              *command.pose_source_sequence,
              observed_at,
              static_cast<std::int64_t>(observed_unsigned +
                                        *command.pose_lifetime_ns),
              *command.pose_quality,
              *command.pose_eligible,
              *command.pose_payload,
          };
          const auto acceptance = scene_runtime_owner_.ObservePose(observation);
          auto [stored, inserted] = pose_observation_replays_.insert_or_assign(
              observation_key,
              PoseObservationReplay{*snapshot.runtime_identity,
                                    *command.pose_lifetime_ns, observation,
                                    acceptance, std::nullopt, std::nullopt});
          static_cast<void>(inserted);
          record = &stored->second;
        }
        Require(record != nullptr,
                "pose observation replay record was not retained");
        const auto application = ResolvePoseApplication(
            record->runtime_identity, record->observation, record->acceptance);
        if (!application.ok()) {
          if (!record->publication_generation.has_value()) {
            record->publication_generation =
                PublishSceneRuntime(static_cast<std::int64_t>(ElapsedNs()));
          }
          TrySendResponse(client.get(),
                          ErrorResponse(application.error().code,
                                        application.error().message));
          return false;
        }
        record->application = application.value();
        if (record->application->status ==
            SceneRuntimePoseApplicationStatus::kCommitted) {
          TryPublishLogicalScene();
        }
        if (!record->publication_generation.has_value()) {
          record->publication_generation =
              PublishSceneRuntime(static_cast<std::int64_t>(ElapsedNs()));
        }
        TrySendResponse(
            client.get(),
            SceneRuntimeObserveResponse(record->observation, record->acceptance,
                                        *record->application,
                                        *record->publication_generation));
        return false;
      }
      case CommandKind::kSceneRuntimeLatchBeforePlanning: {
        Require(command.expected_scene_runtime_identity.has_value() &&
                    command.expected_scene_projection.has_value(),
                "decoded scene-runtime before-planning latch is incomplete");
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
            *command.expected_scene_projection);
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
            return ReconcileAndPublish();
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
                      ErrorResponse("manager_command_rejected", error.what()));
    } catch (...) {
      TrySendResponse(client.get(),
                      ErrorResponse("manager_command_rejected",
                                    "unknown Manager command failure"));
    }
    return false;
  }

  DaemonConfig config_;
  std::chrono::steady_clock::time_point started_at_;
  ManagerPackageGeneration package_generation_;
  DaemonLock daemon_lock_;
  deployment::CanonicalEmptyRetainedBundleAdmission admission_;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source_;
  deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source_;
  deployment::FilesystemScenarioApplyCandidateSource candidate_source_;
  component_manager::RetainedNativeRuntimeUnitAdapter native_runtime_adapter_;
  component_manager::ExternallyProvisionedManagerStores stores_;
  manyforge::scene::CanonicalEmptySceneOwner scene_owner_;
  SceneRuntimePoseProjector scene_runtime_pose_projector_;
  component_manager::DurableManagerActiveRuntimePlanSource plan_source_;
  component_manager::RetainedPosixRuntimeUnitAdapter reference_runtime_adapter_;
  component_manager::RuntimeUnitAdapterRouter runtime_adapter_router_;
  component_manager::ManagerActiveRuntimeCoordinator active_runtime_;
  bool kernel_apply_available_{};
  std::atomic<bool> scene_admission_reconciled_{false};
  std::optional<component_manager::ManagerActiveRuntimeConvergenceResult>
      last_runtime_;
  std::shared_ptr<const deployment::RetainedBundle> last_active_bundle_;
  core::RuntimeEpoch manager_runtime_epoch_;
  component_manager::ManagerNamedApplyCoordinator named_apply_;
  SceneRuntimeStateOwner scene_runtime_owner_;
  AtomicSourceSnapshotChannel manager_channel_;
  AtomicSourceSnapshotChannel scene_channel_;
  AtomicSourceSnapshotChannel scene_runtime_channel_;
  std::string manager_owner_epoch_;
  std::string scene_runtime_owner_epoch_;
  std::string manager_owner_id_;
  std::string scene_owner_id_;
  std::string scene_runtime_owner_id_;
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
  std::optional<core::ActiveBundlePublicationReceiptDTO>
      manager_liveness_attempted_publication_;
  std::optional<core::ActiveBundlePublicationReceiptDTO>
      manager_liveness_superseded_publication_;
  std::optional<core::CellAdmissionReadinessSnapshotDTO>
      last_submitted_readiness_;
  std::map<std::string, PoseObserverSession> pose_observer_sessions_;
  std::map<PoseObservationKey, PoseObservationReplay> pose_observation_replays_;
  std::optional<std::vector<core::ProjectionNativeRuntimeBindingDTO>>
      last_reconciled_native_runtime_bindings_;
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
  bool admission = false;
  bool manager = false;
  bool manager_run = false;
  bool runtime = false;
  bool scene = false;
  bool program_cycle = false;
  bool authoring = false;
  bool projection = false;
  bool program_runner_liveness = false;
  bool period = false;
  for (int index = 1; index < argc; index += 2) {
    Require(index + 1 < argc, "every option requires one value");
    const std::string_view option(argv[index]);
    const std::string value(argv[index + 1]);
    if (option == "--install-root" && !install) {
      config.install_root = value;
      install = true;
    } else if (option == "--canonical-empty-admission-filename" && !admission) {
      config.canonical_empty_admission_filename = value;
      admission = true;
    } else if (option == "--manager-state-root" && !manager) {
      config.manager_state_root = value;
      manager = true;
    } else if (option == "--manager-run-root" && !manager_run) {
      config.manager_run_root = value;
      manager_run = true;
    } else if (option == "--runtime-state-root" && !runtime) {
      config.runtime_state_root = value;
      runtime = true;
    } else if (option == "--logical-scene-database" && !scene) {
      config.logical_scene_database = value;
      scene = true;
    } else if (option == "--program-cycle-journal" && !program_cycle) {
      config.program_cycle_journal = value;
      program_cycle = true;
    } else if (option == "--authoring-transaction-journal" && !authoring) {
      config.authoring_transaction_journal = value;
      authoring = true;
    } else if (option == "--projection-root" && !projection) {
      config.projection_root = value;
      projection = true;
    } else if (option == "--program-runner-liveness-socket" &&
               !program_runner_liveness) {
      config.program_runner_liveness_socket = value;
      program_runner_liveness = true;
    } else if (option == "--reconciliation-period-ms" && !period) {
      config.reconciliation_period =
          std::chrono::milliseconds(ParsePeriod(value));
      period = true;
    } else {
      Fail("unknown or duplicate option " + std::string(option));
    }
  }
  Require(install && manager && manager_run && runtime && scene &&
              program_cycle && authoring && projection,
          "--install-root, --manager-state-root, --manager-run-root, "
          "--runtime-state-root, "
          "--logical-scene-database, --program-cycle-journal, "
          "--authoring-transaction-journal, and --projection-root are required");
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

std::string EncodeManagerStartupError(const manyforge::core::ErrorDTO& error) {
  manyforge::content_identity::CanonicalValue::Object details;
  details.reserve(error.details.size());
  for (const auto& [key, value] : error.details) {
    details.emplace_back(
        key, manyforge::content_identity::CanonicalValue(value));
  }
  return manyforge::content_identity::canonical_json(
      manyforge::content_identity::CanonicalValue(
          manyforge::content_identity::CanonicalValue::Object{
              {"code",
               manyforge::content_identity::CanonicalValue(error.code)},
              {"details",
               manyforge::content_identity::CanonicalValue(
                   std::move(details))},
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
  } catch (const manyforge::component_manager::ManagerStateMarkerError&
               error) {
    std::cerr << manyforge::headless::EncodeManagerStartupError(
                     error.error())
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
