#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "native_projection_rpc_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_component_manager/active_bundle_publication_store.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_roboplan/process_contained_candidate_client.hpp>
#include <manyforge_roboplan/retained_collision_world_projector.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_kinematic_state_projector.hpp>
#include <manyforge_roboplan/retained_native_runtime.hpp>
#include <manyforge_roboplan/retained_planner_scene_projection.hpp>
#include <manyforge_roboplan/retained_planning_model_stage.hpp>
#include <map>
#include <memory>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

using content_identity::CanonicalValue;

constexpr std::string_view kCommandApiVersion = "manyforge.execution_command.v1";
constexpr std::string_view kResponseApiVersion = "manyforge.execution_command_response.v1";
constexpr std::string_view kSocketFileName = "execution-command.sock";
constexpr std::string_view kLockFileName = ".execution-host.lock";
constexpr std::size_t kMaximumPacketBytes = 1024U * 1024U;
constexpr std::size_t kMaximumProgramBytes = 16U * 1024U * 1024U;
constexpr std::size_t kMaximumIdentityBytes = 2048U;
constexpr std::size_t kMaximumReasonBytes = 4096U;
constexpr std::size_t kMaximumEventPage = 16U;
constexpr double kMaximumSafeInteger = 9'007'199'254'740'991.0;
constexpr std::size_t kOwnerEpochRandomBytes = 16U;
constexpr int kSocketBacklog = 16;
constexpr int kReconcilePeriodMilliseconds = 5;
constexpr int kClientPacketDeadlineMilliseconds = 100;

volatile sig_atomic_t g_stop_requested = 0;

void HandleStopSignal(int) {
  g_stop_requested = 1;
}

class FileDescriptor final {
 public:
  explicit FileDescriptor(int value = -1) noexcept : value_(value) {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }
  FileDescriptor(const FileDescriptor&) = delete;
  FileDescriptor& operator=(const FileDescriptor&) = delete;
  FileDescriptor(FileDescriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
  FileDescriptor& operator=(FileDescriptor&& other) noexcept {
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
  Fail(std::string(context) + ": " + std::system_error(errno, std::generic_category()).what());
}

void Require(bool condition, std::string detail) {
  if (!condition) {
    Fail(std::move(detail));
  }
}

[[nodiscard]] std::filesystem::path ExistingDirectory(std::filesystem::path path,
                                                      std::string_view label) {
  Require(!path.empty(), std::string(label) + " cannot be empty");
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  Require(!error && !std::filesystem::is_symlink(status) && std::filesystem::is_directory(status),
          std::string(label) + " must be an existing non-symlink directory");
  auto absolute = std::filesystem::absolute(path, error);
  Require(!error, std::string(label) + " cannot be resolved");
  return absolute.lexically_normal();
}

[[nodiscard]] std::filesystem::path ExistingRegularFile(std::filesystem::path path,
                                                        std::string_view label, bool executable) {
  Require(!path.empty(), std::string(label) + " cannot be empty");
  struct stat status {};
  Require(
      ::lstat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) && !S_ISLNK(status.st_mode),
      std::string(label) + " must be an existing non-symlink file");
  Require(!executable || ::access(path.c_str(), X_OK) == 0,
          std::string(label) + " must be executable");
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  Require(!error, std::string(label) + " cannot be resolved");
  return absolute.lexically_normal();
}

[[nodiscard]] std::string ReadBoundedFile(const std::filesystem::path& path,
                                          std::size_t maximum_bytes) {
  FileDescriptor descriptor(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!descriptor) {
    FailErrno("open Program file");
  }
  struct stat before {};
  if (::fstat(descriptor.get(), &before) != 0) {
    FailErrno("inspect Program file");
  }
  Require(S_ISREG(before.st_mode) && before.st_size > 0 &&
              static_cast<std::uintmax_t>(before.st_size) <= maximum_bytes,
          "Program file is not regular, is empty, or exceeds its byte bound");
  std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count = ::read(descriptor.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      FailErrno("read complete Program file");
    }
    Require(count != 0, "Program file ended before its inspected size");
    offset += static_cast<std::size_t>(count);
  }
  char extra = '\0';
  ssize_t trailing = 0;
  do {
    trailing = ::read(descriptor.get(), &extra, 1U);
  } while (trailing < 0 && errno == EINTR);
  if (trailing < 0) {
    FailErrno("check Program file boundary");
  }
  struct stat after {};
  if (::fstat(descriptor.get(), &after) != 0) {
    FailErrno("reinspect Program file");
  }
  Require(trailing == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
              before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
              before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
              before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
              before.st_ctim.tv_nsec == after.st_ctim.tv_nsec,
          "Program file changed while it was being read");
  return bytes;
}

[[nodiscard]] std::string RandomToken(std::string_view prefix) {
  std::array<unsigned char, kOwnerEpochRandomBytes> bytes{};
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
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
  explicit HostLock(const std::filesystem::path& state_root) {
    const auto path = state_root / std::string(kLockFileName);
    descriptor_ = FileDescriptor(
        ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
    if (!descriptor_) {
      FailErrno("open execution-host lock");
    }
    struct stat status {};
    if (::fstat(descriptor_.get(), &status) != 0) {
      FailErrno("inspect execution-host lock");
    }
    Require(S_ISREG(status.st_mode), "execution-host lock is not a regular file");
    if (::flock(descriptor_.get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        Fail("another execution host already owns this state root");
      }
      FailErrno("acquire execution-host lock");
    }
  }

 private:
  FileDescriptor descriptor_;
};

class CommandSocket final {
 public:
  explicit CommandSocket(const std::filesystem::path& state_root)
      : path_(state_root / std::string(kSocketFileName)) {
    const auto spelling = path_.string();
    Require(spelling.size() < sizeof(sockaddr_un::sun_path),
            "execution command socket path is too long");
    struct stat existing {};
    if (::lstat(path_.c_str(), &existing) == 0) {
      Require(S_ISSOCK(existing.st_mode), "execution command path exists and is not a socket");
      if (::unlink(path_.c_str()) != 0) {
        FailErrno("remove stale execution command socket");
      }
    } else if (errno != ENOENT) {
      FailErrno("inspect execution command socket path");
    }
    descriptor_ =
        FileDescriptor(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!descriptor_) {
      FailErrno("create execution command socket");
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
    if (::bind(descriptor_.get(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      FailErrno("bind execution command socket");
    }
    if (::chmod(path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
      FailErrno("restrict execution command socket permissions");
    }
    if (::lstat(path_.c_str(), &bound_status_) != 0 || !S_ISSOCK(bound_status_.st_mode)) {
      Fail("bound execution command path is not a socket");
    }
    if (::listen(descriptor_.get(), kSocketBacklog) != 0) {
      FailErrno("listen on execution command socket");
    }
  }

  ~CommandSocket() {
    struct stat current {};
    if (::lstat(path_.c_str(), &current) == 0 && current.st_dev == bound_status_.st_dev &&
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
  std::filesystem::path execution_state_root;
  std::filesystem::path staging_root;
  std::filesystem::path program_path;
  std::filesystem::path candidate_worker;
  std::string cell_id;
  std::string program_binding_id;
  std::string motion_target_role_id;
  std::string gripper_target_role_id;
};

struct ManagedRuntimeChildConfig final {
  int manager_channel_fd{-1};
  std::filesystem::path package_root;
  std::filesystem::path staging_root;
  std::string runtime_unit_id;
  std::string runtime_epoch;
  std::string product_identity;
  std::string configuration_identity;
};

[[nodiscard]] bool IsPortableRuntimeIdentity(
    const std::string_view value) noexcept {
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
    Require(result <=
                (static_cast<unsigned int>(INT_MAX) - next) / 10U,
            "managed runtime channel FD overflows");
    result = result * 10U + next;
  }
  return static_cast<int>(result);
}

[[nodiscard]] ManagedRuntimeChildConfig ParseManagedRuntimeArguments(
    const int argc, char **argv) {
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
    } else if (option == "--manyforge-managed-package-root") {
      config.package_root = value;
    } else if (option == "--manyforge-managed-staging-root") {
      config.staging_root = value;
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
  Require(seen.size() == 7U && config.manager_channel_fd >= 0,
          "managed runtime invocation is incomplete");
  Require(IsPortableRuntimeIdentity(config.runtime_unit_id) &&
              IsPortableRuntimeIdentity(config.runtime_epoch),
          "managed runtime identity is invalid");
  Require(core::IsExactLowercaseSha256Digest(config.product_identity) &&
              core::IsExactLowercaseSha256Digest(
                  config.configuration_identity),
          "managed runtime retained identity is invalid");
  config.package_root = ExistingDirectory(
      std::move(config.package_root), "managed retained-package root");
  config.staging_root = ExistingDirectory(
      std::move(config.staging_root), "managed native staging root");

  struct stat status {};
  int socket_type = 0;
  socklen_t socket_type_size = sizeof(socket_type);
  ucred credentials{};
  socklen_t credentials_size = sizeof(credentials);
  Require(::fstat(config.manager_channel_fd, &status) == 0 &&
              S_ISSOCK(status.st_mode) &&
              ::getsockopt(config.manager_channel_fd, SOL_SOCKET, SO_TYPE,
                           &socket_type, &socket_type_size) == 0 &&
              socket_type_size == sizeof(socket_type) &&
              socket_type == SOCK_SEQPACKET &&
              ::getsockopt(config.manager_channel_fd, SOL_SOCKET, SO_PEERCRED,
                           &credentials, &credentials_size) == 0 &&
              credentials_size == sizeof(credentials) &&
              credentials.uid == ::geteuid() &&
              credentials.pid == ::getppid(),
          "managed runtime channel is not the direct same-user parent "
          "channel");
  return config;
}

void SendManagedRuntimePacket(const int fd, const std::string_view bytes) {
  Require(!bytes.empty() &&
              bytes.size() <= component_manager::detail::
                                  kMaximumNativeProjectionRpcPacketBytes,
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
  char probe = '\0';
  ssize_t size = -1;
  do {
    size = ::recv(fd, &probe, 1U, MSG_PEEK | MSG_TRUNC);
  } while (size < 0 && errno == EINTR);
  if (size == 0) {
    return std::nullopt;
  }
  Require(size > 0 &&
              static_cast<std::size_t>(size) <=
                  component_manager::detail::
                      kMaximumNativeProjectionRpcPacketBytes,
          "managed runtime request is absent or oversized");
  std::string bytes(static_cast<std::size_t>(size), '\0');
  ssize_t received = -1;
  do {
    received = ::recv(fd, bytes.data(), bytes.size(), 0);
  } while (received < 0 && errno == EINTR);
  Require(received == size,
          "managed runtime request changed while being received");
  return bytes;
}

using ManagedProjectionKey =
    std::pair<core::CapabilityProviderBindingId,
              core::CapabilityProviderBindingRevision>;

class ManagedProjectionRegistry final {
 public:
  explicit ManagedProjectionRegistry(const ManagedRuntimeChildConfig &config)
      : config_(config), bundle_source_(config.package_root),
        map_source_(config.package_root), planning_source_(config.package_root) {
  }

  [[nodiscard]] component_manager::detail::NativeProjectionRpcResponse
  Dispatch(const component_manager::detail::NativeProjectionRpcRequest
               &request) {
    return std::visit(
        [&](const auto &value)
            -> component_manager::detail::NativeProjectionRpcResponse {
          using T = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<
                            T, component_manager::detail::
                                   NativeProjectionPrepareRequest>) {
            Prepare(value);
            return component_manager::detail::NativeProjectionPrepareResult{
                value};
          } else if constexpr (std::is_same_v<
                                   T, component_manager::detail::
                                          NativeProjectionHealthQuery>) {
            return Projection(value.native_runtime_binding)
                .ProjectionHealth();
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
          } else if constexpr (std::is_same_v<
                                   T, core::
                                          NativeProjectionBootstrapStatusQueryDTO>) {
            return Projection(value.native_runtime_binding)
                .BootstrapProjectionStatus(value);
          } else if constexpr (std::is_same_v<
                                   T, core::NativeProjectionStageDTO>) {
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
          } else if constexpr (std::is_same_v<
                                   T,
                                   core::NativeProjectionRetireStatusQueryDTO>) {
            return Projection(
                       value.request.transition_row.native_runtime_binding)
                .ProjectionRetireStatus(value);
          } else if constexpr (std::is_same_v<
                                   T, core::NativeProjectionDiscardDTO>) {
            return Projection(value.transition_row.native_runtime_binding)
                .DiscardStagedProjection(value);
          } else {
            return Projection(value.transition_row.native_runtime_binding)
                .ProjectionStageStatus(value);
          }
        },
        request);
  }

 private:
  struct PreparedIdentity final {
    core::RetainedBundleId retained_bundle_id;
    core::GraphRevision graph_revision;
    core::RetainedProductDTO planning_collision_model;
  };

  [[nodiscard]] static ManagedProjectionKey Key(
      const core::ProjectionNativeRuntimeBindingDTO &binding) {
    return {binding.provider_binding_id,
            binding.provider_binding_revision};
  }

  void RequireLocalBinding(
      const core::ProjectionNativeRuntimeBindingDTO &binding) const {
    Require(binding.valid() &&
                binding.runtime_unit_id.value() == config_.runtime_unit_id &&
                binding.runtime_epoch.value() == config_.runtime_epoch,
            "native projection request names another runtime child");
  }

  [[nodiscard]] RetainedPlannerSceneProjection &Projection(
      const core::ProjectionNativeRuntimeBindingDTO &binding) {
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
    Require(found != bindings.end() &&
                core::RequiresNativeSceneProjection(
                    found->capability_id,
                    found->capability_definition_revision),
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

  [[nodiscard]] static std::string SelectProjectionGroup(
      const core::CapabilityProviderBindingDTO &provider,
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

  [[nodiscard]] RetainedCollisionWorldProjector BuildProjector(
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
    auto model_map = map_source_.get_verified(
        deployment::RetainedModelMapQuery{
            bundle->retained_bundle_id(),
            products.symbol_role_maps.front().product_id});
    Require(model_map.has_value(),
            "native projection retained model map is absent or invalid");
    const auto group = SelectProjectionGroup(provider, *model_map);
    const auto cache_key = std::make_tuple(
        request.candidate_retained_bundle_id.value(),
        request.candidate_graph_revision.value(), group);
    if (const auto cached = projector_cache_.find(cache_key);
        cached != projector_cache_.end()) {
      return cached->second;
    }

    auto planning_model = planning_source_.get_verified(
        deployment::RetainedPlanningModelQuery{
            bundle->retained_bundle_id(),
            request.planning_collision_model.product_id});
    Require(planning_model.has_value(),
            "native projection retained planning model is absent or invalid");
    auto stage = RetainedPlanningModelStage::create_verified(
        *planning_model, config_.staging_root);
    auto shared_model = collision::ManyForgeCollisionEngine::load_shared_model(
        "manyforge_managed_native_projection", stage.loader_urdf_path(),
        stage.srdf_path(), stage.package_paths(), group);
    auto projector = RetainedCollisionWorldProjector::create_verified(
        std::move(shared_model), *bundle, *model_map, *planning_model);
    const auto [inserted, unique] =
        projector_cache_.emplace(cache_key, projector);
    Require(unique, "native projection model cache insertion conflicted");
    return inserted->second;
  }

  void Prepare(
      const component_manager::detail::NativeProjectionPrepareRequest
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

    auto projector = BuildProjector(request);
    const auto found = projections_.find(key);
    if (found == projections_.end()) {
      auto created = RetainedPlannerSceneProjection::CreateUninitialized(
          std::move(projector), request.planning_collision_model,
          request.native_runtime_binding,
          RetainedPlannerSceneProjectionConfig{
              "managed-native-projection-" +
                  request.native_runtime_binding.provider_binding_id.value(),
              256U,
              RetainedCollisionWorldPublisher::kDefaultLedgerCapacity},
          [] {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
          });
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
          std::move(projector), request.planning_collision_model);
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
  std::map<std::tuple<std::string, std::string, std::string>,
           RetainedCollisionWorldProjector>
      projector_cache_;
  std::map<ManagedProjectionKey,
           std::shared_ptr<RetainedPlannerSceneProjection>>
      projections_;
  std::map<ManagedProjectionKey, PreparedIdentity> prepared_;
};

[[nodiscard]] int RunManagedRuntimeChild(
    const ManagedRuntimeChildConfig &config) {
  const auto implementation_revision =
      deployment::native_execution_runtime_contract::implementation_revision();
  const std::string ready_packet =
      "manyforge.native-execution-ready.v1 " + config.runtime_unit_id + " " +
      config.runtime_epoch + " " + implementation_revision + " " +
      config.product_identity + " " + config.configuration_identity;
  std::uint64_t source_report_sequence = 0U;
  ManagedProjectionRegistry projections(config);

  while (g_stop_requested == 0) {
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
      return 0;
    }
    Require((descriptor.revents & (POLLERR | POLLNVAL)) == 0,
            "managed runtime parent channel became invalid");
    if ((descriptor.revents & POLLIN) == 0) {
      continue;
    }
    const auto packet = ReceiveManagedRuntimePacket(config.manager_channel_fd);
    if (!packet.has_value()) {
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
          "manyforge.native-execution-status.v1 " + config.runtime_unit_id +
              " " + config.runtime_epoch + " " +
              std::to_string(source_report_sequence) +
              " 1 serving healthy " + implementation_revision + " " +
              config.product_identity + " " +
              config.configuration_identity +
              " native_execution closed_no_active_effect");
      continue;
    }
    if (request == "shutdown") {
      SendManagedRuntimePacket(config.manager_channel_fd,
                               "shutdown-accepted");
      return 0;
    }
    component_manager::detail::NativeProjectionRpcResponse response;
    try {
      response = projections.Dispatch(
          component_manager::detail::DecodeNativeProjectionRpcRequest(
              request));
    } catch (const std::exception &error) {
      response = component_manager::detail::NativeProjectionRpcFailure{
          core::ErrorDTO{"native_projection_request_failed", error.what(),
                         {}}};
    } catch (...) {
      response = component_manager::detail::NativeProjectionRpcFailure{
          core::ErrorDTO{"native_projection_request_failed",
                         "native projection request failed", {}}};
    }
    SendManagedRuntimePacket(
        config.manager_channel_fd,
        component_manager::detail::EncodeNativeProjectionRpcResponse(
            response));
  }
  return 0;
}

void RequireExactObjectSize(const CanonicalValue& value, std::size_t size, std::string_view label) {
  Require(value.is_object() && value.as_object().size() == size,
          std::string(label) + " has an unknown or missing field");
}

const CanonicalValue& Member(const CanonicalValue& root, std::string_view name) {
  const auto* value = root.find_object_member(name);
  Require(value != nullptr, "command is missing field " + std::string(name));
  return *value;
}

std::string Text(const CanonicalValue& root, std::string_view name, bool allow_empty = false) {
  const auto* value = std::get_if<std::string>(&Member(root, name).storage());
  Require(value != nullptr && (allow_empty || !value->empty()) &&
              value->size() <= kMaximumIdentityBytes,
          "command field " + std::string(name) + " must be one bounded string");
  return *value;
}

double Number(const CanonicalValue& root, std::string_view name) {
  const auto* value = std::get_if<double>(&Member(root, name).storage());
  Require(value != nullptr && std::isfinite(*value),
          "command field " + std::string(name) + " must be one finite number");
  return *value;
}

std::uint64_t Counter(const CanonicalValue& root, std::string_view name, std::uint64_t minimum) {
  const double value = Number(root, name);
  Require(value >= static_cast<double>(minimum) && std::floor(value) == value &&
              value <= kMaximumSafeInteger,
          "command field " + std::string(name) + " must be one safe unsigned integer");
  return static_cast<std::uint64_t>(value);
}

std::optional<double> OptionalNumber(const CanonicalValue& root, std::string_view name) {
  const auto& member = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto* value = std::get_if<double>(&member.storage());
  Require(value != nullptr && std::isfinite(*value),
          "command field " + std::string(name) + " must be null or finite");
  return *value;
}

std::optional<std::string> OptionalText(const CanonicalValue& root, std::string_view name) {
  const auto& member = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto* value = std::get_if<std::string>(&member.storage());
  Require(value != nullptr && !value->empty() && value->size() <= kMaximumIdentityBytes,
          "command field " + std::string(name) + " must be null or one bounded string");
  return *value;
}

std::optional<std::string> OptionalBoundedText(const CanonicalValue& root, std::string_view name,
                                               std::size_t maximum_bytes) {
  const auto& member = Member(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto* value = std::get_if<std::string>(&member.storage());
  Require(value != nullptr && !value->empty() && value->size() <= maximum_bytes,
          "command field " + std::string(name) + " must be null or one bounded string");
  return *value;
}

const CanonicalValue::Array& Array(const CanonicalValue& root, std::string_view name) {
  const auto& member = Member(root, name);
  const auto* values = std::get_if<CanonicalValue::Array>(&member.storage());
  Require(values != nullptr, "command field " + std::string(name) + " must be an array");
  return *values;
}

std::vector<std::string> TextArray(const CanonicalValue& root, std::string_view name,
                                   std::size_t maximum) {
  const auto& values = Array(root, name);
  Require(values.size() <= maximum, "command text array exceeds its fixed bound");
  std::vector<std::string> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    const auto* text = std::get_if<std::string>(&value.storage());
    Require(text != nullptr && !text->empty() && text->size() <= kMaximumIdentityBytes,
            "command text array contains an invalid value");
    result.push_back(*text);
  }
  return result;
}

std::vector<double> NumberArray(const CanonicalValue& root, std::string_view name,
                                std::size_t maximum) {
  const auto& values = Array(root, name);
  Require(values.size() <= maximum, "command number array exceeds its fixed bound");
  std::vector<double> result;
  result.reserve(values.size());
  for (const auto& value : values) {
    const auto* number = std::get_if<double>(&value.storage());
    Require(number != nullptr && std::isfinite(*number),
            "command number array contains a non-finite value");
    result.push_back(*number);
  }
  return result;
}

[[nodiscard]] core::SceneRevision DecodeSceneRevision(const CanonicalValue& value) {
  RequireExactObjectSize(value, 3U, "scene revision");
  const double revision = Number(value, "revision_id");
  Require(
      revision >= 1.0 && std::floor(revision) == revision && revision <= 9'007'199'254'740'991.0,
      "scene revision id is invalid");
  return core::SceneRevision{core::SceneId(Text(value, "scene_id")),
                             static_cast<std::uint64_t>(revision),
                             core::ContentDigest(Text(value, "content_hash"))};
}

[[nodiscard]] core::ActionSpanDTO DecodeActionSpan(const CanonicalValue& value) {
  RequireExactObjectSize(value, 6U, "action span");
  const auto source = core::action_source_from_string(Text(value, "action_source"));
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

[[nodiscard]] core::PublicRequestContextDTO DecodeRequestContext(const CanonicalValue& value) {
  RequireExactObjectSize(value, 4U, "public request context");
  return core::PublicRequestContextDTO{core::RequestId(Text(value, "request_id")),
                                       core::RequestOwnerEpoch(Text(value, "request_owner_epoch")),
                                       Text(value, "source_label"),
                                       OptionalText(value, "correlation_id")};
}

[[nodiscard]] core::RequestAttributionDTO DecodeRequestAttribution(const CanonicalValue& value) {
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
  Require(attribution.locally_valid(), "request attribution is internally inconsistent");
  return attribution;
}

[[nodiscard]] core::JointDynamicsLimitDTO DecodeDynamicsLimit(const CanonicalValue& value) {
  RequireExactObjectSize(value, 3U, "joint dynamics limit");
  return {OptionalNumber(value, "max_velocity_rad_s"),
          OptionalNumber(value, "max_acceleration_rad_s2"),
          OptionalNumber(value, "max_jerk_rad_s3")};
}

[[nodiscard]] core::MotionParticipantIntentDTO DecodeMotionPayload(const CanonicalValue& value) {
  RequireExactObjectSize(value, 16U, "motion payload");
  Require(Text(value, "kind") == "motion", "operation payload kind is unsupported");

  const auto& tolerances = Member(value, "tolerances");
  RequireExactObjectSize(tolerances, 3U, "motion tolerances");
  const auto& speed = Member(value, "speed_profile");
  RequireExactObjectSize(speed, 3U, "motion speed profile");
  const auto& timeouts = Member(value, "timeouts");
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
  Require(Text(value, "target_mode") == "absolute", "N3 admits only absolute motion");
  Require(std::holds_alternative<std::nullptr_t>(Member(value, "target_reference").storage()) &&
              std::holds_alternative<std::nullptr_t>(Member(value, "offset_reference").storage()),
          "N3 joint motion cannot carry references");
  Require(std::holds_alternative<std::nullptr_t>(Member(value, "contact_policy_id").storage()),
          "N3 joint motion cannot carry a contact policy");
  Require(Array(value, "motion_interrupt_scopes").empty(),
          "N3 joint motion cannot carry interrupt scopes");

  const auto& motion = Member(value, "motion");
  RequireExactObjectSize(motion, 3U, "joint goal");
  Require(Text(motion, "kind") == "joint_goal", "N3 admits only joint-goal motion");
  auto joint_names = TextArray(motion, "joint_names", core::kMaxOperationParticipants);
  auto positions = NumberArray(motion, "positions_rad", core::kMaxOperationParticipants);

  const auto& dynamics = Member(value, "per_joint_dynamics_limits");
  Require(dynamics.is_object() && dynamics.as_object().size() <= core::kMaxOperationParticipants,
          "joint dynamics map exceeds its fixed bound");
  std::map<std::string, core::JointDynamicsLimitDTO> limits;
  for (const auto& [joint_name, limit] : dynamics.as_object()) {
    Require(!joint_name.empty() && joint_name.size() <= kMaximumIdentityBytes,
            "joint dynamics map contains an invalid name");
    const auto [position, inserted] = limits.emplace(joint_name, DecodeDynamicsLimit(limit));
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
      core::SpeedProfileDTO{Number(speed, "velocity_scale"), Number(speed, "acceleration_scale"),
                            OptionalNumber(speed, "max_tcp_linear_speed_mps")},
      core::TimeoutsDTO{Number(timeouts, "planning_timeout_s"),
                        Number(timeouts, "execution_timeout_s")},
      cache_mode,
      planning_profile,
      core::MotionTargetModeDTO::kAbsolute,
      std::nullopt,
      std::nullopt,
      core::JointGoalDTO{std::move(joint_names), std::move(positions)},
      std::nullopt,
      std::move(limits),
      {}};
}

[[nodiscard]] core::CoordinatePositionDTO DecodeCoordinatePosition(const CanonicalValue& value) {
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

[[nodiscard]] core::CoordinateEffortDTO DecodeCoordinateEffort(const CanonicalValue& value) {
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

[[nodiscard]] core::CapabilityCommandIntentDTO DecodeCapabilityCommandPayload(
    const CanonicalValue& value) {
  RequireExactObjectSize(value, 6U, "capability-command payload");
  Require(Text(value, "kind") == "capability_command", "operation payload kind is unsupported");
  const auto& command_value = Member(value, "command_value");
  RequireExactObjectSize(command_value, 3U, "gripper-position command value");
  Require(Text(command_value, "kind") == "gripper_position",
          "capability-command value kind is unsupported");

  std::optional<core::CoordinateEffortDTO> max_effort;
  const auto& effort = Member(command_value, "max_effort");
  if (!std::holds_alternative<std::nullptr_t>(effort.storage())) {
    max_effort = DecodeCoordinateEffort(effort);
  }

  const auto& option_values = Array(value, "caller_options");
  Require(option_values.size() <= core::kMaxOperationCommandOptions,
          "capability-command options exceed their fixed bound");
  std::vector<core::CommandDurationOptionDTO> caller_options;
  caller_options.reserve(option_values.size());
  for (const auto& option : option_values) {
    RequireExactObjectSize(option, 2U, "capability-command option");
    caller_options.push_back(core::CommandDurationOptionDTO{
        core::CommandOptionId(Text(option, "option_id")), Number(option, "value_s")});
  }

  return core::CapabilityCommandIntentDTO{
      core::CommandSchemaId(Text(value, "command_schema_id")),
      core::CommandSchemaRevision(Text(value, "command_schema_revision")),
      core::GripperPositionCommandValueDTO{
          DecodeCoordinatePosition(Member(command_value, "position")), std::move(max_effort)},
      Number(value, "completion_timeout_s"), std::move(caller_options)};
}

[[nodiscard]] core::OperationParticipantPayloadDTO DecodeParticipantPayload(
    const CanonicalValue& value) {
  const auto kind = Text(value, "kind");
  if (kind == "motion") {
    return DecodeMotionPayload(value);
  }
  if (kind == "capability_command") {
    return DecodeCapabilityCommandPayload(value);
  }
  Fail("operation payload kind is unsupported");
}

[[nodiscard]] core::OperationIntentDTO DecodeOperation(
    const CanonicalValue& value, const core::RequestAttributionDTO& attribution) {
  RequireExactObjectSize(value, 10U, "operation intent");
  const auto& request = Member(value, "request_context");
  const auto& participants = Array(value, "participants");
  Require(participants.size() == 1U, "N3 admits exactly one operation participant");
  const auto& participant = participants.front();
  RequireExactObjectSize(participant, 5U, "operation participant");
  std::optional<core::ActionSpanDTO> action_span;
  const auto& span = Member(participant, "action_span");
  if (!std::holds_alternative<std::nullptr_t>(span.storage())) {
    action_span = DecodeActionSpan(span);
  }
  std::optional<core::SceneRevision> expected_scene;
  const auto& scene = Member(value, "expected_scene_revision");
  if (!std::holds_alternative<std::nullptr_t>(scene.storage())) {
    expected_scene = DecodeSceneRevision(scene);
  }
  Require(Text(value, "coordination_policy") == "independent",
          "N3 admits only independent operation coordination");
  return core::OperationIntentDTO{
      DecodeRequestContext(request),
      core::ProgramId(Text(value, "program_id")),
      core::ProgramRevision(Text(value, "program_revision")),
      core::ProgramBindingId(Text(value, "program_binding_id")),
      core::ProgramRunId(Text(value, "program_run_id")),
      core::GraphRevision(Text(value, "expected_graph_revision")),
      core::RetainedBundleId(Text(value, "expected_retained_bundle_id")),
      std::move(expected_scene),
      {core::OperationParticipantIntentDTO{
          core::ComponentRoleId(Text(participant, "component_role_id")),
          core::TargetRoleId(Text(participant, "target_role_id")),
          core::CapabilityId(Text(participant, "required_capability_id")), std::move(action_span),
          DecodeParticipantPayload(Member(participant, "payload"))}},
      core::OperationCoordinationPolicy::kIndependent,
      attribution};
}

[[nodiscard]] core::OperationControlRequestDTO DecodeOperationControlRequest(
    const CanonicalValue& value, const core::RequestAttributionDTO& attribution) {
  RequireExactObjectSize(value, 4U, "operation control request");
  return core::OperationControlRequestDTO{
      DecodeRequestContext(Member(value, "request_context")),
      core::OperationId(Text(value, "operation_id")),
      core::OperationRevision(Counter(value, "expected_operation_revision", 1U)),
      OptionalBoundedText(value, "reason", kMaximumReasonBytes), attribution};
}

[[nodiscard]] core::StopIntentDTO DecodeStopIntent(const CanonicalValue& value,
                                                   const core::RequestAttributionDTO& attribution) {
  RequireExactObjectSize(value, 3U, "Stop intent");
  const auto& operations = Array(value, "operations");
  Require(operations.size() == 1U, "N4 admits exactly one operation in ordinary Stop intent");
  const auto& target = operations.front();
  RequireExactObjectSize(target, 3U, "Stop operation target");
  return core::StopIntentDTO{
      DecodeRequestContext(Member(value, "request_context")),
      {core::StopOperationTargetDTO{
          core::OperationId(Text(target, "operation_id")),
          core::OperationRevision(Counter(target, "expected_operation_revision", 1U)),
          core::StopGeneration(Counter(target, "expected_stop_generation", 0U))}},
      OptionalBoundedText(value, "reason", kMaximumReasonBytes),
      attribution};
}

[[nodiscard]] core::RuntimeScaleRequestDTO DecodeRuntimeScaleRequest(
    const CanonicalValue& value, const core::RequestAttributionDTO& attribution) {
  RequireExactObjectSize(value, 5U, "runtime-scale request");
  const double scale = Number(value, "scale");
  Require(scale >= 0.0 && scale <= 1.0, "runtime scale must be in the closed interval [0, 1]");
  return core::RuntimeScaleRequestDTO{
      DecodeRequestContext(Member(value, "request_context")),
      core::OperationId(Text(value, "operation_id")),
      core::OperationRevision(Counter(value, "expected_operation_revision", 1U)),
      core::RuntimeScaleGeneration(Counter(value, "expected_runtime_scale_generation", 0U)),
      scale,
      attribution};
}

enum class CommandKind {
  kStatus,
  kEvents,
  kSubmit,
  kRead,
  kPause,
  kResume,
  kReset,
  kAcknowledgeCompleted,
  kStop,
  kSetRuntimeScale,
  kGetRuntimeScale,
};

struct Command final {
  CommandKind kind;
  std::optional<core::OperationIntentDTO> operation;
  std::optional<core::OperationId> operation_id;
  std::optional<core::OperationControlRequestDTO> control;
  std::optional<core::StopIntentDTO> stop;
  std::optional<core::RuntimeScaleRequestDTO> runtime_scale;
  std::optional<core::RequestOwnerEpoch> event_owner_epoch{};
  std::uint64_t after_sequence{};
  std::size_t event_limit{};
};

[[nodiscard]] Command DecodeCommand(std::string_view bytes) {
  Require(!bytes.empty() && bytes.size() <= kMaximumPacketBytes,
          "execution command packet is empty or oversized");
  CanonicalValue root;
  try {
    root = deployment::parse_strict_json_model_yaml(bytes).root();
  } catch (const std::exception& error) {
    Fail("execution command is not strict JSON: " + std::string(error.what()));
  }
  Require(content_identity::canonical_json(root) == bytes,
          "execution command must use canonical JSON encoding");
  Require(root.is_object(), "execution command root must be an object");
  Require(Text(root, "api_version") == kCommandApiVersion,
          "execution command API version is unsupported");
  const auto command = Text(root, "command");
  if (command == "status") {
    RequireExactObjectSize(root, 2U, "status command");
    return {CommandKind::kStatus, std::nullopt, std::nullopt,
            std::nullopt,         std::nullopt, std::nullopt};
  }
  if (command == "events") {
    RequireExactObjectSize(root, 5U, "events command");
    const auto limit = Counter(root, "limit", 1U);
    Require(limit <= kMaximumEventPage, "events command exceeds its page bound");
    return {CommandKind::kEvents,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            core::RequestOwnerEpoch(Text(root, "owner_epoch")),
            Counter(root, "after_sequence", 0U),
            static_cast<std::size_t>(limit)};
  }
  if (command == "read") {
    RequireExactObjectSize(root, 3U, "read command");
    return {CommandKind::kRead, std::nullopt, core::OperationId(Text(root, "operation_id")),
            std::nullopt,       std::nullopt, std::nullopt};
  }
  if (command == "get_runtime_scale") {
    RequireExactObjectSize(root, 3U, "runtime-scale read command");
    return {CommandKind::kGetRuntimeScale,
            std::nullopt,
            core::OperationId(Text(root, "operation_id")),
            std::nullopt,
            std::nullopt,
            std::nullopt};
  }
  if (command == "submit") {
    RequireExactObjectSize(root, 4U, "submit command");
    const auto attribution = DecodeRequestAttribution(Member(root, "attribution"));
    return {CommandKind::kSubmit, DecodeOperation(Member(root, "operation"), attribution),
            std::nullopt,         std::nullopt,
            std::nullopt,         std::nullopt};
  }
  if (command == "stop") {
    RequireExactObjectSize(root, 4U, "Stop command");
    const auto attribution = DecodeRequestAttribution(Member(root, "attribution"));
    return {CommandKind::kStop,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            DecodeStopIntent(Member(root, "request"), attribution),
            std::nullopt};
  }
  if (command == "set_runtime_scale") {
    RequireExactObjectSize(root, 4U, "runtime-scale command");
    const auto attribution = DecodeRequestAttribution(Member(root, "attribution"));
    return {CommandKind::kSetRuntimeScale,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            DecodeRuntimeScaleRequest(Member(root, "request"), attribution)};
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
  const auto attribution = DecodeRequestAttribution(Member(root, "attribution"));
  return {kind,         std::nullopt,
          std::nullopt, DecodeOperationControlRequest(Member(root, "request"), attribution),
          std::nullopt, std::nullopt};
}

[[nodiscard]] CanonicalValue ErrorValue(const core::ErrorDTO& error) {
  CanonicalValue::Object details;
  details.reserve(error.details.size());
  for (const auto& [key, value] : error.details) {
    details.emplace_back(key, CanonicalValue(value));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue(error.code)},
      {"details", CanonicalValue(std::move(details))},
      {"message", CanonicalValue(error.message)},
  });
}

[[nodiscard]] CanonicalValue ErrorResponse(std::string code, std::string message) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
      {"error", ErrorValue(core::ErrorDTO{std::move(code), std::move(message), {}})},
      {"ok", CanonicalValue(false)},
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

[[nodiscard]] CanonicalValue AcceptedValue(const core::OperationAcceptedDTO& value) {
  CanonicalValue::Array assignments;
  assignments.reserve(value.participant_assignments.size());
  for (const auto& assignment : value.participant_assignments) {
    assignments.emplace_back(CanonicalValue::Object{
        {"input_index", CanonicalValue(static_cast<double>(assignment.input_index))},
        {"participant_id", CanonicalValue(assignment.participant_id.value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", CanonicalValue(static_cast<double>(value.accepted_at_ns))},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision", CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participant_assignments", CanonicalValue(std::move(assignments))},
      {"replay_status", CanonicalValue(std::string(core::to_string(value.replay_status)))},
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

[[nodiscard]] CanonicalValue ControlAcceptedValue(const core::OperationControlAcceptedDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_against_revision",
       CanonicalValue(static_cast<double>(value.accepted_against_revision.value()))},
      {"accepted_at_ns", CanonicalValue(static_cast<double>(value.accepted_at_ns))},
      {"control", CanonicalValue(std::string(ControlName(value.control)))},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision_after_acceptance",
       CanonicalValue(static_cast<double>(value.operation_revision_after_acceptance.value()))},
      {"replay_status", CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
  });
}

[[nodiscard]] CanonicalValue StopAcceptedValue(const core::StopAcceptedDTO& value) {
  CanonicalValue::Array operations;
  operations.reserve(value.operations.size());
  for (const auto& operation : value.operations) {
    operations.emplace_back(operation.value());
  }
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", CanonicalValue(static_cast<double>(value.accepted_at_ns))},
      {"operation_revision_after_acceptance",
       CanonicalValue(static_cast<double>(value.operation_revision_after_acceptance.value()))},
      {"operations", CanonicalValue(std::move(operations))},
      {"replay_status", CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
      {"stop_generation", CanonicalValue(static_cast<double>(value.stop_generation.value()))},
      {"stop_id", CanonicalValue(value.stop_id.value())},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleAcceptedValue(const core::RuntimeScaleAcceptedDTO& value) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", CanonicalValue(static_cast<double>(value.accepted_at_ns))},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision", CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"replay_status", CanonicalValue(std::string(core::to_string(value.replay_status)))},
      {"request_id", CanonicalValue(value.request_id.value())},
      {"requested_scale", CanonicalValue(value.requested_scale)},
      {"runtime_scale_generation",
       CanonicalValue(static_cast<double>(value.runtime_scale_generation.value()))},
  });
}

[[nodiscard]] CanonicalValue RuntimeScaleStatusValue(const core::RuntimeScaleStatusDTO& value) {
  CanonicalValue::Array participants;
  participants.reserve(value.participants.size());
  for (const auto& participant : value.participants) {
    CanonicalValue::Array resources;
    resources.reserve(participant.resources.size());
    for (const auto& resource : participant.resources) {
      resources.emplace_back(resource.value());
    }
    participants.emplace_back(CanonicalValue::Object{
        {"accepted_generation",
         CanonicalValue(static_cast<double>(participant.accepted_generation.value()))},
        {"acknowledgement_sequence",
         CanonicalValue(static_cast<double>(participant.acknowledgement_sequence))},
        {"acknowledgement_source_epoch",
         CanonicalValue(participant.acknowledgement_source_epoch.value())},
        {"acknowledgement_timestamp_ns",
         CanonicalValue(static_cast<double>(participant.acknowledgement_timestamp_ns))},
        {"active_control_configuration_generation",
         CanonicalValue(
             static_cast<double>(participant.active_control_configuration_generation.value()))},
        {"active_control_configuration_id",
         CanonicalValue(participant.active_control_configuration_id.value())},
        {"applied_scale", CanonicalValue(participant.applied_scale)},
        {"capability_id", CanonicalValue(participant.capability_id.value())},
        {"commanded_scale", CanonicalValue(participant.commanded_scale)},
        {"command_target_id", CanonicalValue(participant.command_target_id.value())},
        {"controllable_component_id",
         CanonicalValue(participant.controllable_component_id.value())},
        {"control_binding_id", CanonicalValue(participant.control_binding_id.value())},
        {"control_binding_revision", CanonicalValue(participant.control_binding_revision.value())},
        {"control_domain_id", CanonicalValue(participant.control_domain_id.value())},
        {"control_domain_revision", CanonicalValue(participant.control_domain_revision.value())},
        {"control_route_id", CanonicalValue(participant.control_route_id.value())},
        {"controller_goal_generation",
         CanonicalValue(static_cast<double>(participant.controller_goal_generation.value()))},
        {"controller_goal_id", CanonicalValue(participant.controller_goal_id.value())},
        {"controller_topology_id", CanonicalValue(participant.controller_topology_id.value())},
        {"coordinated_barrier_id", participant.coordinated_barrier_id.has_value()
                                       ? CanonicalValue(participant.coordinated_barrier_id->value())
                                       : CanonicalValue(nullptr)},
        {"error",
         participant.error.has_value() ? ErrorValue(*participant.error) : CanonicalValue(nullptr)},
        {"participant_id", CanonicalValue(participant.participant_id.value())},
        {"resources", CanonicalValue(std::move(resources))},
        {"scaling_domain_id", CanonicalValue(participant.scaling_domain_id.value())},
        {"state", CanonicalValue(std::string(core::to_string(participant.state)))},
        {"supported", CanonicalValue(participant.supported)},
        {"topology_revision", CanonicalValue(participant.topology_revision.value())},
        {"virtual_trajectory_time_s", CanonicalValue(participant.virtual_trajectory_time_s)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"effective_scale", CanonicalValue(value.effective_scale)},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision", CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participants", CanonicalValue(std::move(participants))},
      {"requested_scale", CanonicalValue(value.requested_scale)},
      {"runtime_scale_generation",
       CanonicalValue(static_cast<double>(value.runtime_scale_generation.value()))},
      {"state", CanonicalValue(std::string(core::to_string(value.state)))},
      {"updated_at_ns", CanonicalValue(static_cast<double>(value.updated_at_ns))},
  });
}

[[nodiscard]] CanonicalValue OperationStateValue(const core::OperationStateDTO& value) {
  CanonicalValue::Array participants;
  participants.reserve(value.participants.size());
  for (const auto& participant : value.participants) {
    CanonicalValue::Array resources;
    resources.reserve(participant.resources.size());
    for (const auto& resource : participant.resources) {
      resources.emplace_back(resource.value());
    }
    participants.emplace_back(CanonicalValue::Object{
        {"capability_id", CanonicalValue(participant.capability_id.value())},
        {"capability_provider_binding_id",
         participant.capability_provider_binding_id.has_value()
             ? CanonicalValue(participant.capability_provider_binding_id->value())
             : CanonicalValue(nullptr)},
        {"capability_provider_binding_revision",
         participant.capability_provider_binding_revision.has_value()
             ? CanonicalValue(participant.capability_provider_binding_revision->value())
             : CanonicalValue(nullptr)},
        {"command_target_id", CanonicalValue(participant.command_target_id.value())},
        {"controllable_component_id",
         CanonicalValue(participant.controllable_component_id.value())},
        {"control_binding_id", CanonicalValue(participant.control_binding_id.value())},
        {"control_binding_revision", CanonicalValue(participant.control_binding_revision.value())},
        {"control_domain_id", CanonicalValue(participant.control_domain_id.value())},
        {"control_domain_revision", CanonicalValue(participant.control_domain_revision.value())},
        {"controller_goal_generation",
         CanonicalValue(static_cast<double>(participant.controller_goal_generation.value()))},
        {"controller_goal_id", CanonicalValue(participant.controller_goal_id.value())},
        {"participant_id", CanonicalValue(participant.participant_id.value())},
        {"payload_kind",
         CanonicalValue(participant.payload_kind == core::OperationParticipantPayloadKind::kMotion
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
           ? CanonicalValue(static_cast<double>(value.active_stop_generation->value()))
           : CanonicalValue(nullptr)},
      {"active_stop_id", value.active_stop_id.has_value()
                             ? CanonicalValue(value.active_stop_id->value())
                             : CanonicalValue(nullptr)},
      {"coordination_policy", CanonicalValue("independent")},
      {"operation_id", CanonicalValue(value.operation_id.value())},
      {"operation_revision", CanonicalValue(static_cast<double>(value.operation_revision.value()))},
      {"participants", CanonicalValue(std::move(participants))},
      {"program_binding_id", CanonicalValue(value.program_binding_id.value())},
      {"program_run_id", CanonicalValue(value.program_run_id.value())},
      {"runtime_scale_generation",
       value.runtime_scale_generation.has_value()
           ? CanonicalValue(static_cast<double>(value.runtime_scale_generation->value()))
           : CanonicalValue(nullptr)},
      {"state", CanonicalValue(std::string(StateName(value.state)))},
      {"updated_at_ns", CanonicalValue(static_cast<double>(value.updated_at_ns))},
  });
}

[[nodiscard]] CanonicalValue OperationEventPageValue(
    const core::OperationJournalEventPageDTO& page) {
  CanonicalValue::Array events;
  events.reserve(page.events.size());
  for (const auto& event : page.events) {
    CanonicalValue payload;
    try {
      payload = deployment::parse_strict_json_model_yaml(event.payload_json).root();
    } catch (const std::exception& error) {
      Fail("durable operation event payload is not strict JSON: " + std::string(error.what()));
    }
    Require(content_identity::canonical_json(payload) == event.payload_json,
            "durable operation event payload is not canonical JSON");
    events.emplace_back(CanonicalValue::Object{
        {"effect_pending", CanonicalValue(event.effect_pending)},
        {"occurred_at_ns", CanonicalValue(static_cast<double>(event.occurred_at_ns))},
        {"occurred_at_wall", CanonicalValue(event.occurred_at_wall)},
        {"operation_id", CanonicalValue(event.operation_id.value())},
        {"operation_revision",
         CanonicalValue(static_cast<double>(event.operation_revision.value()))},
        {"owner_epoch", CanonicalValue(event.owner_epoch.value())},
        {"payload", std::move(payload)},
        {"payload_digest", CanonicalValue(event.payload_digest)},
        {"record_kind", CanonicalValue(event.record_kind)},
        {"request_id", event.request_id.has_value() ? CanonicalValue(event.request_id->value())
                                                    : CanonicalValue(nullptr)},
        {"sequence", CanonicalValue(static_cast<double>(event.sequence))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"events", CanonicalValue(std::move(events))},
      {"has_more", CanonicalValue(page.has_more)},
      {"next_after_sequence", CanonicalValue(static_cast<double>(page.next_after_sequence))},
      {"owner_epoch", CanonicalValue(page.owner_epoch.value())},
      {"schema_version", CanonicalValue("manyforge.execution_event_page.v1")},
  });
}

[[nodiscard]] CanonicalValue CoordinatePositionValue(const core::CoordinatePositionDTO& position) {
  if (const auto* angular = std::get_if<core::AngularPositionDTO>(&position)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("angular_rad")},
        {"value_rad", CanonicalValue(angular->value_rad)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("linear_m")},
      {"value_m", CanonicalValue(std::get<core::LinearPositionDTO>(position).value_m)},
  });
}

[[nodiscard]] CanonicalValue CoordinateVelocityValue(const core::CoordinateVelocityDTO& velocity) {
  if (const auto* angular = std::get_if<core::AngularVelocityDTO>(&velocity)) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("angular_rad_s")},
        {"value_rad_s", CanonicalValue(angular->value_rad_s)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("linear_m_s")},
      {"value_m_s", CanonicalValue(std::get<core::LinearVelocityDTO>(velocity).value_m_s)},
  });
}

[[nodiscard]] CanonicalValue ObservedTargetValue(const core::TargetRoleId& target_role_id,
                                                 const core::ObservedStateSnapshotDTO& state) {
  CanonicalValue::Array resources;
  resources.reserve(state.resources.size());
  for (const auto& resource : state.resources) {
    resources.emplace_back(resource.value());
  }
  CanonicalValue::Array joint_names;
  joint_names.reserve(state.joint_names.size());
  for (const auto& joint_name : state.joint_names) {
    joint_names.emplace_back(joint_name);
  }
  CanonicalValue::Array positions;
  positions.reserve(state.positions.size());
  for (const auto& position : state.positions) {
    positions.push_back(CoordinatePositionValue(position));
  }
  CanonicalValue velocities(nullptr);
  if (state.velocities.has_value()) {
    CanonicalValue::Array values;
    values.reserve(state.velocities->size());
    for (const auto& velocity : *state.velocities) {
      values.push_back(CoordinateVelocityValue(velocity));
    }
    velocities = CanonicalValue(std::move(values));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active_control_configuration_generation",
       CanonicalValue(static_cast<double>(state.active_control_configuration_generation.value()))},
      {"active_control_configuration_id",
       CanonicalValue(state.active_control_configuration_id.value())},
      {"command_target_id", CanonicalValue(state.command_target_id.value())},
      {"complete", CanonicalValue(state.complete)},
      {"controllable_component_id", CanonicalValue(state.controllable_component_id.value())},
      {"control_binding_id", CanonicalValue(state.control_binding_id.value())},
      {"control_binding_revision", CanonicalValue(state.control_binding_revision.value())},
      {"control_domain_id", CanonicalValue(state.control_domain_id.value())},
      {"control_domain_revision", CanonicalValue(state.control_domain_revision.value())},
      {"control_route_id", CanonicalValue(state.control_route_id.value())},
      {"joint_names", CanonicalValue(std::move(joint_names))},
      {"positions", CanonicalValue(std::move(positions))},
      {"received_timestamp_ns", CanonicalValue(static_cast<double>(state.received_timestamp_ns))},
      {"resources", CanonicalValue(std::move(resources))},
      {"sequence", CanonicalValue(static_cast<double>(state.sequence))},
      {"source_epoch", CanonicalValue(state.source_epoch.value())},
      {"source_id", CanonicalValue(state.source_id)},
      {"source_kind", CanonicalValue(std::string(core::to_string(state.source_kind)))},
      {"source_timestamp_ns", CanonicalValue(static_cast<double>(state.source_timestamp_ns))},
      {"state_authority_class",
       CanonicalValue(std::string(core::to_string(state.state_authority_class)))},
      {"state_authority_id", CanonicalValue(state.state_authority_id.value())},
      {"target_role_id", CanonicalValue(target_role_id.value())},
      {"velocities", std::move(velocities)},
  });
}

class AbsoluteOnlyReferenceResolver final : public core::IReferencePoseResolver {
 public:
  [[nodiscard]] core::Result<core::Pose3DTO> resolve_reference_pose(
      const core::ReferenceDTO&, const std::string&,
      const core::ProjectionLeaseBindingDTO&) const override {
    return core::Result<core::Pose3DTO>::Err(
        core::ErrorDTO{std::string(core::to_string(core::ErrorCode::kMissingReference)),
                       "N3 native execution host has no logical-scene reference resolver",
                       {{"reason", "n3_reference_resolution_unavailable"}}});
  }
};

[[nodiscard]] RoboPlanCandidateBindingDTO CandidateBinding(
    const core::RuntimeParticipantBinding& runtime) {
  const auto& value = runtime.value();
  Require(value.model_groups.size() == 1U, "motion binding has no sole retained model group");
  const auto& group = value.model_groups.front();
  const auto model = std::find_if(value.model_bindings.begin(), value.model_bindings.end(),
                                  [&](const auto& candidate) {
                                    return candidate.model_instance_id == group.model_instance_id;
                                  });
  Require(model != value.model_bindings.end(), "motion model group has no retained model revision");
  std::vector<core::ResourceId> resources;
  std::vector<std::string> compiled_joint_names;
  resources.reserve(value.resources.size());
  compiled_joint_names.reserve(value.resources.size());
  for (const auto& resource : value.resources) {
    resources.push_back(resource.resource_id);
    compiled_joint_names.push_back(resource.compiled_resource_name);
  }
  std::vector<core::ResolvedFrameDTO> frames;
  frames.reserve(value.frame_bindings.size());
  for (const auto& frame : value.frame_bindings) {
    frames.emplace_back(core::ResolvedComponentModelFrameDTO{
        value.controllable_component_id, frame.model_instance_id, frame.frame_role_id,
        frame.compiled_frame_name, frame.model_revision});
  }
  return RoboPlanCandidateBindingDTO{value.controllable_component_id, value.command_target_id,
                                     group.model_instance_id,         model->model_revision,
                                     group.compiled_group_name,       std::move(resources),
                                     std::move(compiled_joint_names), std::move(frames)};
}

[[nodiscard]] core::LogicalSceneSnapshotDTO EmptyScene(const core::CellId& cell_id,
                                                       const core::SceneRevision& revision) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {1U, std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  core::LogicalSceneSnapshotDTO snapshot{revision, cell_id, 0U, 0U, 0U, std::move(policy), {}, {}};
  Require(core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot) ==
                  revision.content_hash &&
              snapshot.locally_valid(),
          "active scene revision is not the exact canonical empty scene; "
          "live scene consumption starts at N5");
  return snapshot;
}

[[nodiscard]] deployment::ActiveRuntimeParticipantFactsDTO ActiveFacts(
    const deployment::RuntimeProgramTargetBinding& primary,
    std::span<const deployment::RuntimeProgramTargetBinding> supporting,
    const core::RuntimeEpoch& runtime_epoch,
    core::ActiveControlConfigurationId active_configuration_id) {
  Require(primary.desired_route().has_value(), "runtime binding has no desired I6 route");
  std::set<core::RuntimeUnitId> runtime_units(primary.component().runtime_unit_ids.begin(),
                                              primary.component().runtime_unit_ids.end());
  if (primary.control_domain().has_value()) {
    runtime_units.insert(primary.control_domain()->runtime_unit_ids.begin(),
                         primary.control_domain()->runtime_unit_ids.end());
  }
  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_epochs;
  runtime_epochs.reserve(runtime_units.size());
  for (const auto& runtime_unit : runtime_units) {
    runtime_epochs.push_back({runtime_unit, runtime_epoch});
  }
  std::set<core::ProviderInstanceId> providers;
  if (primary.control_binding().has_value()) {
    providers.insert(primary.control_binding()->provider_instance_id);
  }
  if (primary.capability_provider_binding().has_value()) {
    providers.insert(primary.capability_provider_binding()->provider_instance_id);
  }
  for (const auto& support : supporting) {
    if (support.capability_provider_binding().has_value()) {
      providers.insert(support.capability_provider_binding()->provider_instance_id);
    }
  }
  std::vector<deployment::RuntimeProviderEpochFactDTO> provider_epochs;
  provider_epochs.reserve(providers.size());
  for (const auto& provider : providers) {
    provider_epochs.push_back({provider, runtime_epoch});
  }
  return deployment::ActiveRuntimeParticipantFactsDTO{
      *primary.desired_route(),
      std::move(active_configuration_id),
      core::ActiveControlConfigurationGeneration(1U),
      std::move(runtime_epochs),
      std::move(provider_epochs)};
}

struct Composition final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::shared_ptr<const deployment::ProgramBindingContract> program;
  std::shared_ptr<const deployment::ProgramBinding> program_binding;
  RetainedPlanningModelStage stage;
  std::shared_ptr<RetainedNativeRuntime> runtime;
  core::ProjectionRevisionDTO projection_revision;
  core::ActiveBundlePublicationGeneration publication_generation;

  Composition(std::shared_ptr<const deployment::RetainedBundle> selected_bundle,
              std::shared_ptr<const deployment::ProgramBindingContract> selected_program,
              std::shared_ptr<const deployment::ProgramBinding> selected_binding,
              RetainedPlanningModelStage selected_stage,
              std::shared_ptr<RetainedNativeRuntime> selected_runtime,
              core::ProjectionRevisionDTO selected_projection,
              core::ActiveBundlePublicationGeneration selected_generation)
      : bundle(std::move(selected_bundle)),
        program(std::move(selected_program)),
        program_binding(std::move(selected_binding)),
        stage(std::move(selected_stage)),
        runtime(std::move(selected_runtime)),
        projection_revision(std::move(selected_projection)),
        publication_generation(selected_generation) {}
};

[[nodiscard]] std::unique_ptr<Composition> BuildComposition(
    const HostConfig& config, const core::RequestOwnerEpoch& owner_epoch,
    const core::RuntimeEpoch& runtime_epoch) {
  component_manager::SqliteActiveBundlePublicationStore publication_store(
      config.manager_state_root /
          std::string(component_manager::kActiveBundlePublicationStoreFilename),
      core::CellId(config.cell_id));
  const auto integrity = publication_store.VerifyIntegrity();
  Require(integrity.current_generation.nonzero(), "active retained-bundle publication is absent");
  const auto publication =
      publication_store.ReadActiveBundlePublication(integrity.current_generation);
  Require(publication.ok() && publication.value().valid(),
          "active retained-bundle publication cannot be verified");
  const auto receipt = publication.value();
  publication_store.Close();

  const deployment::FilesystemVerifiedRetainedBundleSource bundle_source(config.package_root);
  auto bundle = bundle_source.get_verified(receipt.request.candidate_retained_bundle_id);
  Require(
      bundle != nullptr && bundle->value().cell_graph.cell_id == core::CellId(config.cell_id) &&
          bundle->value().cell_graph.graph_revision == receipt.request.candidate_graph_revision &&
          bundle->retained_bundle_id() ==
              receipt.request.candidate_projection_revision.retained_bundle_id &&
          bundle->value().cell_graph.graph_revision ==
              receipt.request.candidate_projection_revision.graph_revision,
      "active publication and retained bundle disagree");
  const auto& products = bundle->value().compiled_products;
  Require(products.symbol_role_maps.size() == 1U,
          "N3 native host requires one exact retained model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(config.package_root);
  auto model_map = map_source.get_verified(deployment::RetainedModelMapQuery{
      bundle->retained_bundle_id(), products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(), "active retained model map is absent or invalid");

  const auto program_bytes = ReadBoundedFile(config.program_path, kMaximumProgramBytes);
  auto program = deployment::verify_program_binding_contract(program_bytes, *bundle);
  const deployment::FilesystemProgramBindingRepository binding_repository(config.package_root);
  auto program_binding = binding_repository.get_verified(
      core::ProgramBindingId(config.program_binding_id), *bundle, *model_map);
  Require(program_binding != nullptr &&
              program_binding->value().program_id == program->value().program_id &&
              program_binding->value().program_revision == program->value().program_revision,
          "Program and exact retained Program Binding disagree");

  const core::TargetRoleId motion_target_role(config.motion_target_role_id);
  const core::TargetRoleId gripper_target_role(config.gripper_target_role_id);
  const auto motion_target = deployment::resolve_runtime_program_target(
      *program_binding, motion_target_role, core::CapabilityId("execution.trajectory.execute"),
      *bundle, *model_map);
  const auto planner_target = deployment::resolve_runtime_program_target(
      *program_binding, motion_target_role, core::CapabilityId("planning.motion.joint_goal"),
      *bundle, *model_map);
  const auto gripper_target = deployment::resolve_runtime_program_target(
      *program_binding, gripper_target_role, core::CapabilityId("tool.gripper.command"), *bundle,
      *model_map);
  const std::array<deployment::RuntimeProgramTargetBinding, 1U> support{planner_target};
  auto motion_binding_dto = deployment::build_runtime_participant_binding_dto(
      motion_target,
      ActiveFacts(motion_target, support, runtime_epoch,
                  core::ActiveControlConfigurationId("active.i6.native.arm")),
      support);
  auto motion_binding = core::verify_runtime_participant_binding(std::move(motion_binding_dto));
  if (!motion_binding.ok()) {
    Fail("active motion participant binding failed verification: " +
         motion_binding.error().message);
  }
  auto gripper_binding_dto = deployment::build_runtime_participant_binding_dto(
      gripper_target,
      ActiveFacts(gripper_target, std::span<const deployment::RuntimeProgramTargetBinding>{},
                  runtime_epoch,
                  core::ActiveControlConfigurationId("active.i6.native.gripper")));
  auto gripper_binding = core::verify_runtime_participant_binding(std::move(gripper_binding_dto));
  if (!gripper_binding.ok()) {
    Fail("active gripper participant binding failed verification: " +
         gripper_binding.error().message);
  }

  const deployment::FilesystemVerifiedRetainedPlanningModelSource planning_source(
      config.package_root);
  auto retained_model = planning_source.get_verified(deployment::RetainedPlanningModelQuery{
      bundle->retained_bundle_id(), products.planning_collision_model.product_id});
  Require(retained_model.has_value(), "active retained planning model is absent or invalid");
  auto stage = RetainedPlanningModelStage::create_verified(*retained_model, config.staging_root);
  auto scene = std::make_shared<roboplan::Scene>(
      "manyforge_i6_native_execution_host", std::string(stage.loader_urdf_bytes()),
      std::string(stage.srdf_bytes()), stage.package_paths());
  auto scene_mutex = std::make_shared<std::mutex>();
  auto state_projector = std::make_shared<RetainedKinematicStateProjector>(
      RetainedKinematicStateProjector::create_verified(
          scene, scene_mutex, *bundle, *model_map, *retained_model,
          std::vector<core::RuntimeParticipantBinding>{motion_binding.value(),
                                                       gripper_binding.value()},
          0));

  const auto& group = motion_binding.value().value().model_groups.front();
  auto collision_model = collision::ManyForgeCollisionEngine::load_shared_model(
      "manyforge_i6_native_execution_host", stage.loader_urdf_path(), stage.srdf_path(),
      stage.package_paths(), group.compiled_group_name);
  auto collision_projector = RetainedCollisionWorldProjector::create_verified(
      collision_model, *bundle, *model_map, *retained_model);
  const auto logical_scene =
      EmptyScene(bundle->value().cell_graph.cell_id,
                 receipt.request.candidate_projection_revision.scene_revision);
  auto publisher_result = RetainedCollisionWorldPublisher::create(
      std::move(collision_projector), "i6-native-execution-host", logical_scene,
      RetainedCollisionWorldPublicationContext{core::CollisionWorldMutationSource::kDeploymentApply,
                                               {},
                                               WallClock{}.now(),
                                               {},
                                               "i6_native_execution_host_startup",
                                               std::nullopt});
  if (!publisher_result.ok()) {
    Fail("retained collision publication failed: " + publisher_result.error().message);
  }
  auto publisher = std::move(publisher_result.value());
  const auto inventory = publisher->current_scene_snapshot_lease_inventory();
  Require(inventory.has_value(), "retained collision publisher has no initial lease");

  auto candidate_binding = CandidateBinding(motion_binding.value());
  auto candidate_client = std::make_unique<ProcessContainedRoboPlanCandidateClient>(
      RoboPlanWorkerModelDTO{"manyforge_i6_native_execution_worker",
                             stage.loader_urdf_path(),
                             stage.srdf_path(),
                             stage.package_paths(),
                             std::nullopt,
                             {},
                             {}},
      candidate_binding, RoboPlanCandidateComputerConfig{.rng_seed = 7U},
      ProcessContainedCandidateClientConfig{.worker_executable = config.candidate_worker});
  auto planner = std::make_unique<RoboPlanPlannerBackend>(
      inventory->projection_binding, candidate_binding, std::move(candidate_client));
  auto runtime = create_retained_native_runtime(
      RetainedNativeRuntimeInput{motion_binding.value(), gripper_binding.value(), scene,
                                 scene_mutex, state_projector, publisher, std::move(planner),
                                 std::make_unique<AbsoluteOnlyReferenceResolver>()},
      RetainedNativeRuntimeConfig{
          owner_epoch, core::CellId(config.cell_id),
          config.execution_state_root / std::string(core::kOperationJournalFilename), 1024U,
          core::CacheConfig{}, core::ExecutionKernelConfig{}, RoboPlanExecutionBackendConfig{},
          SimpleSchedulerConfig::from_environment(), 10'000'000});
  return std::make_unique<Composition>(
      std::move(bundle), std::move(program), std::move(program_binding), std::move(stage),
      std::move(runtime), receipt.request.candidate_projection_revision,
      receipt.publication_generation);
}

class NativeExecutionHost final {
 public:
  explicit NativeExecutionHost(HostConfig config)
      : config_(std::move(config)),
        owner_epoch_(RandomToken("execution-host-")),
        runtime_epoch_(RandomToken("native-runtime-")),
        host_lock_(config_.execution_state_root),
        composition_(BuildComposition(config_, owner_epoch_, runtime_epoch_)),
        command_socket_(config_.execution_state_root) {}

  int Run() {
    while (g_stop_requested == 0) {
      const auto reconciled =
          composition_->runtime->addressed_kernel->reconcile_subordinate_state();
      if (!reconciled.ok()) {
        last_reconcile_error_ = reconciled.error();
      }
      pollfd descriptor{command_socket_.get(), POLLIN, 0};
      int ready = -1;
      do {
        ready = ::poll(&descriptor, 1, kReconcilePeriodMilliseconds);
      } while (ready < 0 && errno == EINTR && g_stop_requested == 0);
      if (g_stop_requested != 0) {
        break;
      }
      if (ready < 0) {
        FailErrno("poll execution command socket");
      }
      if ((descriptor.revents & POLLIN) != 0) {
        HandleOneClient();
      }
      if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        Fail("execution command socket became unavailable");
      }
    }
    return 0;
  }

 private:
  [[nodiscard]] CanonicalValue StatusResponse() const {
    const auto& bundle = *composition_->bundle;
    const auto& program = composition_->program->value();
    const auto motion_state = composition_->runtime->motion_state_channel->current_state(
        composition_->runtime->clock->now());
    if (!motion_state.ok()) {
      Fail("selected motion observed state is unavailable: " + motion_state.error().message);
    }
    Require(composition_->runtime->gripper_state_channel != nullptr,
            "selected aggregate gripper observed-state channel is absent");
    Require(composition_->runtime->gripper_binding.has_value() &&
                composition_->runtime->gripper_binding->value().command_schema.has_value(),
            "selected aggregate gripper command schema is absent");
    const auto& gripper_schema = *composition_->runtime->gripper_binding->value().command_schema;
    const auto gripper_state = composition_->runtime->gripper_state_channel->current_state(
        composition_->runtime->clock->now());
    if (!gripper_state.ok()) {
      Fail("selected gripper observed state is unavailable: " + gripper_state.error().message);
    }
    CanonicalValue::Array observed_targets;
    observed_targets.reserve(2U);
    observed_targets.push_back(ObservedTargetValue(
        core::TargetRoleId(config_.motion_target_role_id), motion_state.value()));
    observed_targets.push_back(ObservedTargetValue(
        core::TargetRoleId(config_.gripper_target_role_id), gripper_state.value()));
    return CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
        {"command", CanonicalValue("status")},
        {"ok", CanonicalValue(true)},
        {"status",
         CanonicalValue(CanonicalValue::Object{
             {"active_publication_generation",
              CanonicalValue(static_cast<double>(composition_->publication_generation.value()))},
             {"cell_id", CanonicalValue(bundle.value().cell_graph.cell_id.value())},
             {"graph_revision", CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
             {"gripper_command_schema_id",
              CanonicalValue(gripper_schema.command_schema_id.value())},
             {"gripper_command_schema_revision",
              CanonicalValue(gripper_schema.command_schema_revision.value())},
             {"gripper_target_role_id", CanonicalValue(config_.gripper_target_role_id)},
             {"motion_target_role_id", CanonicalValue(config_.motion_target_role_id)},
             {"observed_targets", CanonicalValue(std::move(observed_targets))},
             {"owner_epoch", CanonicalValue(owner_epoch_.value())},
             {"program_binding_id",
              CanonicalValue(composition_->program_binding->program_binding_id().value())},
             {"program_id", CanonicalValue(program.program_id.value())},
             {"program_revision", CanonicalValue(program.program_revision.value())},
             {"retained_bundle_id", CanonicalValue(bundle.retained_bundle_id().value())},
             {"scene_revision",
              CanonicalValue(CanonicalValue::Object{
                  {"content_hash", CanonicalValue(composition_->projection_revision.scene_revision
                                                      .content_hash.value())},
                  {"revision_id",
                   CanonicalValue(static_cast<double>(
                       composition_->projection_revision.scene_revision.revision_id))},
                  {"scene_id", CanonicalValue(composition_->projection_revision.scene_revision
                                                  .scene_id.value())},
              })},
         })},
    });
  }

  [[nodiscard]] CanonicalValue Execute(const Command& command) {
    if (command.kind == CommandKind::kStatus) {
      return StatusResponse();
    }
    if (command.kind == CommandKind::kEvents) {
      const auto page = composition_->runtime->operation_journal->ReadEvents(
          *command.event_owner_epoch, command.after_sequence, command.event_limit);
      if (!page.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(page.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("events")},
          {"event_page", OperationEventPageValue(page.value())},
          {"ok", CanonicalValue(true)},
      });
    }
    if (last_reconcile_error_.has_value()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(*last_reconcile_error_)},
          {"ok", CanonicalValue(false)},
      });
    }
    if (command.kind == CommandKind::kSubmit) {
      const auto result = composition_->runtime->addressed_kernel->execute(*command.operation);
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
    const auto reconciled = composition_->runtime->addressed_kernel->reconcile_subordinate_state();
    if (!reconciled.ok()) {
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"error", ErrorValue(reconciled.error())},
          {"ok", CanonicalValue(false)},
      });
    }
    if (command.kind == CommandKind::kRead) {
      const auto state =
          composition_->runtime->addressed_kernel->get_operation_state(*command.operation_id);
      if (!state.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(state.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("read")},
          {"ok", CanonicalValue(true)},
          {"operation", OperationStateValue(state.value())},
      });
    }
    if (command.kind == CommandKind::kGetRuntimeScale) {
      const auto status =
          composition_->runtime->addressed_kernel->get_runtime_scale(*command.operation_id);
      if (!status.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(status.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("get_runtime_scale")},
          {"ok", CanonicalValue(true)},
          {"runtime_scale", RuntimeScaleStatusValue(status.value())},
      });
    }
    if (command.kind == CommandKind::kStop) {
      const auto result = composition_->runtime->addressed_kernel->stop(*command.stop);
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
          {"command", CanonicalValue("stop")},
          {"ok", CanonicalValue(true)},
      });
    }
    if (command.kind == CommandKind::kSetRuntimeScale) {
      const auto result =
          composition_->runtime->addressed_kernel->set_runtime_scale(*command.runtime_scale);
      if (!result.ok()) {
        return CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
            {"error", ErrorValue(result.error())},
            {"ok", CanonicalValue(false)},
        });
      }
      return CanonicalValue(CanonicalValue::Object{
          {"accepted", RuntimeScaleAcceptedValue(result.value())},
          {"api_version", CanonicalValue(std::string(kResponseApiVersion))},
          {"command", CanonicalValue("set_runtime_scale")},
          {"ok", CanonicalValue(true)},
      });
    }
    const auto control_result = [&]() -> core::Result<core::OperationControlAcceptedDTO> {
      switch (command.kind) {
        case CommandKind::kPause:
          return composition_->runtime->addressed_kernel->pause(*command.control);
        case CommandKind::kResume:
          return composition_->runtime->addressed_kernel->resume(*command.control);
        case CommandKind::kReset:
          return composition_->runtime->addressed_kernel->reset(*command.control);
        case CommandKind::kAcknowledgeCompleted:
          return composition_->runtime->addressed_kernel->acknowledge_completed(*command.control);
        case CommandKind::kStatus:
        case CommandKind::kEvents:
        case CommandKind::kSubmit:
        case CommandKind::kRead:
        case CommandKind::kStop:
        case CommandKind::kSetRuntimeScale:
        case CommandKind::kGetRuntimeScale:
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
        {"command", CanonicalValue(std::string(ControlName(control_result.value().control)))},
        {"ok", CanonicalValue(true)},
    });
  }

  void HandleOneClient() noexcept {
    try {
      FileDescriptor client(
          ::accept4(command_socket_.get(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (!client) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
          return;
        }
        FailErrno("accept execution command client");
      }
      ucred peer{};
      socklen_t peer_size = sizeof(peer);
      Require(::getsockopt(client.get(), SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) == 0 &&
                  peer_size == sizeof(peer) && peer.uid == ::geteuid(),
              "execution command client is not the same user");
      pollfd inbound{client.get(), POLLIN, 0};
      int packet_ready = -1;
      do {
        packet_ready = ::poll(&inbound, 1, kClientPacketDeadlineMilliseconds);
      } while (packet_ready < 0 && errno == EINTR && g_stop_requested == 0);
      Require(packet_ready > 0 && (inbound.revents & POLLIN) != 0,
              "execution command client did not send a packet in time");
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
      if (count <= 0 || (message.msg_flags & MSG_TRUNC) != 0) {
        response = ErrorResponse("execution_command_invalid",
                                 "execution command packet is empty or oversized");
      } else {
        try {
          response = Execute(
              DecodeCommand(std::string_view(buffer.data(), static_cast<std::size_t>(count))));
        } catch (const std::exception& error) {
          response = ErrorResponse("execution_command_invalid", error.what());
        }
      }
      const auto bytes = content_identity::canonical_json(response);
      Require(!bytes.empty() && bytes.size() <= kMaximumPacketBytes,
              "execution command response is empty or oversized");
      pollfd outbound{client.get(), POLLOUT, 0};
      int response_ready = -1;
      do {
        response_ready = ::poll(&outbound, 1, kClientPacketDeadlineMilliseconds);
      } while (response_ready < 0 && errno == EINTR && g_stop_requested == 0);
      Require(response_ready > 0 && (outbound.revents & POLLOUT) != 0,
              "execution command client did not accept a response in time");
      ssize_t sent = -1;
      do {
        sent = ::send(client.get(), bytes.data(), bytes.size(), MSG_NOSIGNAL);
      } while (sent < 0 && errno == EINTR);
      Require(sent == static_cast<ssize_t>(bytes.size()),
              "execution command response was not sent atomically");
    } catch (...) {
      // A malformed/disappearing private client cannot retire the sole owner.
    }
  }

  HostConfig config_;
  core::RequestOwnerEpoch owner_epoch_;
  // This complete pre-N5c host is itself one process incarnation. Keep its
  // runtime identity independent from the operation-owner epoch. The
  // Manager-launched mode receives its epoch from the direct-child adapter.
  core::RuntimeEpoch runtime_epoch_;
  HostLock host_lock_;
  std::unique_ptr<Composition> composition_;
  CommandSocket command_socket_;
  std::optional<core::ErrorDTO> last_reconcile_error_;
};

[[nodiscard]] HostConfig ParseArguments(int argc, char** argv) {
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
    } else if (option == "--staging-root") {
      config.staging_root = value;
    } else if (option == "--program") {
      config.program_path = value;
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
  Require(seen.size() == 10U,
          "package, Manager state, execution state, staging, Program, "
          "Program Binding, motion/gripper target roles, candidate worker, and "
          "cell options "
          "are required");
  config.package_root = ExistingDirectory(std::move(config.package_root), "package root");
  config.manager_state_root =
      ExistingDirectory(std::move(config.manager_state_root), "Manager state root");
  config.execution_state_root =
      ExistingDirectory(std::move(config.execution_state_root), "execution state root");
  config.staging_root = ExistingDirectory(std::move(config.staging_root), "staging root");
  config.program_path = ExistingRegularFile(std::move(config.program_path), "Program", false);
  config.candidate_worker =
      ExistingRegularFile(std::move(config.candidate_worker), "candidate worker", true);
  Require(!config.cell_id.empty() && config.cell_id.size() <= kMaximumIdentityBytes &&
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
  if (::sigaction(SIGINT, &action, nullptr) != 0 || ::sigaction(SIGTERM, &action, nullptr) != 0) {
    FailErrno("install execution-host signal handlers");
  }
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  if (::sigaction(SIGPIPE, &ignore, nullptr) != 0) {
    FailErrno("ignore execution-host SIGPIPE");
  }
}

}  // namespace
}  // namespace manyforge::roboplan_backend

int main(int argc, char** argv) {
  try {
    manyforge::roboplan_backend::InstallSignalHandlers();
    if (argc > 1 &&
        std::string_view(argv[1]) ==
            "--manyforge-managed-runtime-channel-fd") {
      return manyforge::roboplan_backend::RunManagedRuntimeChild(
          manyforge::roboplan_backend::ParseManagedRuntimeArguments(argc,
                                                                    argv));
    }
    manyforge::roboplan_backend::NativeExecutionHost host(
        manyforge::roboplan_backend::ParseArguments(argc, argv));
    return host.Run();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 70;
  } catch (...) {
    std::cerr << "Native execution host failed with an unknown error\n";
    return 71;
  }
}
