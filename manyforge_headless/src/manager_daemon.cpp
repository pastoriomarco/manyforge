#include "manyforge_headless/owner_publication.hpp"
#include "manyforge_headless/scene_runtime_state_owner.hpp"
#include "manyforge_headless/source_snapshot.hpp"

#include <manyforge_component_manager/active_native_projection_runtime_set.hpp>
#include <manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_named_apply_coordinator.hpp>
#include <manyforge_component_manager/manager_runtime_convergence_coordinator.hpp>
#include <manyforge_component_manager/retained_native_runtime_unit_adapter.hpp>
#include <manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp>
#include <manyforge_component_manager/runtime_unit_adapter_router.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/scenario_apply_candidate_source.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_scene/canonical_empty_scene_owner.hpp>
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

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
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
constexpr std::size_t kMaximumCommandBytes =
    scene::kMaximumLogicalSceneTransactionBytes + 4U * 1024U;
constexpr std::size_t kMaximumCommandIdentityBytes = 256U;
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

std::filesystem::path DatabasePath(std::filesystem::path path) {
  Require(!path.empty() && path.has_filename() && path.filename() != "." &&
              path.filename() != "..",
          "logical-scene database path is invalid");
  static_cast<void>(
      ExistingDirectory(path.parent_path(), "logical-scene database parent"));
  const auto status = std::filesystem::symlink_status(path);
  Require(!std::filesystem::is_symlink(status),
          "logical-scene database path cannot be a symlink");
  return path;
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
  std::filesystem::path package_root;
  std::filesystem::path canonical_empty_admission_filename{
      "canonical-empty-admission.json"};
  std::filesystem::path manager_state_root;
  std::filesystem::path runtime_state_root;
  std::filesystem::path logical_scene_database;
  std::filesystem::path publication_root;
  std::chrono::milliseconds reconciliation_period{250};
};

enum class CommandKind {
  kApply,
  kSceneCommit,
  kReconcile,
  kShutdownRuntime,
};

struct ManagerCommand final {
  CommandKind kind;
  std::optional<std::string> request_id;
  std::optional<std::string> request_owner_epoch;
  std::optional<std::string> source_label;
  std::optional<std::string> correlation_id;
  std::optional<core::SceneTransactionRequestDTO> scene_transaction;
  std::optional<core::DeploymentArtifactId> deployment_artifact_id;
  std::optional<core::DeploymentRevision> deployment_revision;
  std::optional<core::RetainedBundleId> expected_current_retained_bundle_id;
  std::optional<core::GraphRevision> expected_current_graph_revision;
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
  Require(command == "apply", "command kind is unsupported");
  RequireExactObjectSize(root, 10U, "Apply command");
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
        daemon_lock_(config_.manager_state_root),
        admission_(deployment::FilesystemCanonicalEmptyRetainedBundleSource(
                       config_.package_root,
                       config_.canonical_empty_admission_filename)
                       .load_verified()),
        bundle_source_(config_.package_root),
        artifact_source_(config_.package_root),
        candidate_source_(config_.package_root, "maintained"),
        native_runtime_adapter_(
            InstalledTargetRoot(), config_.package_root,
            config_.manager_state_root, config_.runtime_state_root,
            bundle_source_, artifact_source_, "linux",
            std::string(deployment::native_execution_runtime_contract::
                            kLaunchEnvironmentContract)),
        stores_(config_.manager_state_root, admission_),
        scene_owner_(config_.logical_scene_database, admission_,
                     stores_.active_bundle_publication_store(), bundle_source_,
                     native_runtime_adapter_),
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
                        [this] { return ElapsedNs(); }),
        manager_runtime_epoch_(RandomToken("manager-runtime-")),
        named_apply_(
            stores_.apply_journal(), plan_source_, bundle_source_,
            artifact_source_, stores_.active_bundle_publication_store(),
            scene_owner_, &scene_owner_, native_runtime_adapter_,
            native_runtime_adapter_, runtime_adapter_router_,
            runtime_adapter_router_, manager_runtime_epoch_, "linux",
            {std::string(deployment::native_execution_runtime_contract::
                             kLaunchEnvironmentContract)},
            [this] { return ElapsedNs(); },
            [] { return core::ApplyId(RandomToken("apply-")); }),
        scene_runtime_owner_(
            [this] { return static_cast<std::int64_t>(ElapsedNs()); },
            [] { return RandomToken("scene-init-"); }),
        manager_channel_(config_.publication_root,
                         std::string(kManagerSourceFileName)),
        scene_channel_(config_.publication_root,
                       std::string(kLogicalSceneSourceFileName)),
        scene_runtime_channel_(config_.publication_root,
                               std::string(kSceneRuntimeSourceFileName)),
        manager_owner_epoch_(RandomToken("manager-")),
        scene_runtime_owner_epoch_(RandomToken("scene-runtime-")),
        manager_owner_id_("component-manager." + admission_.cell_id().value()),
        scene_owner_id_("logical-scene." + admission_.cell_id().value()),
        scene_runtime_owner_id_("scene-runtime." +
                                admission_.cell_id().value()) {
    EnsureBootstrap();
    static_cast<void>(ReconcileAndPublish());
    if (TryRecoverIncompleteApply()) {
      static_cast<void>(ReconcileAndPublish());
    }
  }

  int Run() {
    CommandSocket command_socket(config_.manager_state_root);
    while (g_stop_requested == 0) {
      pollfd descriptor{command_socket.get(), POLLIN, 0};
      int ready = -1;
      do {
        ready = ::poll(&descriptor, 1,
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
        return 0;
      }
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        Fail("Manager command socket became unavailable");
      }
    }
    return 0;
  }

private:
  struct PublicationGenerations final {
    std::uint64_t manager;
    std::uint64_t scene;
    std::uint64_t scene_runtime;
  };

  static DaemonConfig ValidateConfig(DaemonConfig config) {
    config.package_root =
        ExistingDirectory(std::move(config.package_root), "package root");
    config.manager_state_root = ExistingDirectory(
        std::move(config.manager_state_root), "Manager state root");
    config.runtime_state_root = ExistingDirectory(
        std::move(config.runtime_state_root), "runtime state root");
    config.publication_root = ExistingDirectory(
        std::move(config.publication_root), "publication root");
    config.logical_scene_database =
        DatabasePath(std::move(config.logical_scene_database));
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
    const auto runtime = active_runtime_.Reconcile();
    kernel_apply_available_ = HasConvergedKernelApplyOwner(runtime);
    const auto bundle_id =
        runtime.plan.active_publication.request.candidate_retained_bundle_id;
    const auto active_bundle = bundle_source_.get_verified(bundle_id);
    Require(active_bundle != nullptr,
            "active publication has no independently verified retained bundle");
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
      static_cast<void>(scene_runtime_owner_.AdoptFinalizedOpenRuntime(
          desired_scene_runtime, publication_generation,
          "durable_finalized_active_projection"));
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
    const auto scene_runtime_publication = scene_runtime_channel_.Publish(
        "scene_runtime", scene_runtime_owner_id_, scene_runtime_owner_epoch_,
        observed_at,
        BuildSceneRuntimeSourcePayload(admission_.cell_id().value(),
                                       scene_runtime_owner_.Snapshot()));
    return {manager.source_generation, scene_generation,
            scene_runtime_publication.source_generation};
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

  void TryPeriodicReconcile() noexcept {
    try {
      static_cast<void>(ReconcileAndPublish());
      if (TryRecoverIncompleteApply()) {
        static_cast<void>(ReconcileAndPublish());
      }
    } catch (const std::exception &error) {
      std::cerr << "Manager daemon periodic reconciliation failed: "
                << error.what() << '\n';
    } catch (...) {
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
          });
          // Apply durability is authoritative. A source-publication failure is
          // logged and retried by the periodic strand; it cannot turn a
          // successful Apply into a second mutation on client retry.
          try {
            static_cast<void>(ReconcileAndPublish());
          } catch (const std::exception &error) {
            std::cerr << "Manager daemon publication failed after durable "
                         "Apply: "
                      << error.what() << '\n';
          }
          TrySendResponse(client.get(), ApplyResponse(command, result));
        } catch (const component_manager::ManagerNamedApplyBusyError &error) {
          TrySendResponse(
              client.get(),
              ApplyErrorResponse(command, "apply_busy", error.what()));
        } catch (const std::exception &error) {
          TrySendResponse(
              client.get(),
              ApplyErrorResponse(command, "apply_rejected", error.what()));
        }
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
      case CommandKind::kReconcile: {
        const auto generations = ReconcileAndPublish();
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
  DaemonLock daemon_lock_;
  deployment::CanonicalEmptyRetainedBundleAdmission admission_;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source_;
  deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source_;
  deployment::FilesystemScenarioApplyCandidateSource candidate_source_;
  component_manager::RetainedNativeRuntimeUnitAdapter native_runtime_adapter_;
  component_manager::ExternallyProvisionedManagerStores stores_;
  manyforge::scene::CanonicalEmptySceneOwner scene_owner_;
  component_manager::DurableManagerActiveRuntimePlanSource plan_source_;
  component_manager::RetainedPosixRuntimeUnitAdapter reference_runtime_adapter_;
  component_manager::RuntimeUnitAdapterRouter runtime_adapter_router_;
  component_manager::ManagerActiveRuntimeCoordinator active_runtime_;
  bool kernel_apply_available_{};
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
  bool package = false;
  bool admission = false;
  bool manager = false;
  bool runtime = false;
  bool scene = false;
  bool publication = false;
  bool period = false;
  for (int index = 1; index < argc; index += 2) {
    Require(index + 1 < argc, "every option requires one value");
    const std::string_view option(argv[index]);
    const std::string value(argv[index + 1]);
    if (option == "--package-root" && !package) {
      config.package_root = value;
      package = true;
    } else if (option == "--canonical-empty-admission-filename" && !admission) {
      config.canonical_empty_admission_filename = value;
      admission = true;
    } else if (option == "--manager-state-root" && !manager) {
      config.manager_state_root = value;
      manager = true;
    } else if (option == "--runtime-state-root" && !runtime) {
      config.runtime_state_root = value;
      runtime = true;
    } else if (option == "--logical-scene-database" && !scene) {
      config.logical_scene_database = value;
      scene = true;
    } else if (option == "--publication-root" && !publication) {
      config.publication_root = value;
      publication = true;
    } else if (option == "--reconciliation-period-ms" && !period) {
      config.reconciliation_period =
          std::chrono::milliseconds(ParsePeriod(value));
      period = true;
    } else {
      Fail("unknown or duplicate option " + std::string(option));
    }
  }
  Require(package && manager && runtime && scene && publication,
          "--package-root, --manager-state-root, --runtime-state-root, "
          "--logical-scene-database, and --publication-root are required");
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
} // namespace manyforge::headless

int main(int argc, char **argv) {
  try {
    manyforge::headless::InstallSignalHandlers();
    manyforge::headless::ManagerDaemon daemon(
        manyforge::headless::ParseArguments(argc, argv));
    return daemon.Run();
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 70;
  } catch (...) {
    std::cerr << "Manager daemon failed with an unknown error\n";
    return 71;
  }
}
