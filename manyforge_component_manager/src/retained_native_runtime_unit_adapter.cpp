#include "manyforge_component_manager/retained_native_runtime_unit_adapter.hpp"

#include "kernel_apply_fence_rpc_internal.hpp"
#include "native_projection_rpc_internal.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <limits>
#include <manyforge_component_manager/native_execution_runtime_unit_admission.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

extern char **environ;

namespace manyforge::component_manager {
namespace {

namespace contract = manyforge::deployment::native_execution_runtime_contract;

using manyforge::core::RuntimeEpoch;
using manyforge::deployment::RetainedBundle;

using ProjectionBindingKey =
    std::pair<manyforge::core::CapabilityProviderBindingId,
              manyforge::core::CapabilityProviderBindingRevision>;

constexpr int kChildChannelFd = 3;
constexpr int kChildExecErrorFd = 4;
constexpr int kChildExecutableFd = 5;
constexpr std::size_t kMaximumControlPacketBytes =
    detail::kMaximumNativeProjectionRpcPacketBytes;
constexpr auto kExecTimeout = std::chrono::seconds(10);
constexpr auto kReadyTimeout = std::chrono::seconds(10);
constexpr auto kStatusTimeout = std::chrono::milliseconds(750);
constexpr auto kGracefulStopTimeout = std::chrono::seconds(2);
constexpr auto kTermStopTimeout = std::chrono::seconds(1);

[[noreturn]] void Fail(const std::string &detail) {
  throw RetainedNativeRuntimeUnitAdapterError(detail);
}

[[nodiscard]] std::string ErrnoDetail(const std::string_view operation,
                                      const int error) {
  return std::string(operation) + ": " + std::strerror(error);
}

[[nodiscard]] std::filesystem::path
NormalizedPlainDirectory(std::filesystem::path path,
                         const std::string_view label) {
  if (path.empty()) {
    Fail(std::string(label) + " is empty");
  }
  std::error_code error;
  path = std::filesystem::absolute(path, error).lexically_normal();
  if (error) {
    Fail(std::string(label) + " could not be resolved: " + error.message());
  }
  const auto status = std::filesystem::symlink_status(path, error);
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    Fail(std::string(label) + " is not one existing plain directory");
  }
  return path;
}

class UniqueFd final {
public:
  UniqueFd() = default;
  explicit UniqueFd(const int fd) : fd_(fd) {}
  ~UniqueFd() { Reset(); }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      Reset(std::exchange(other.fd_, -1));
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }
  void Reset(const int replacement = -1) noexcept {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
    fd_ = replacement;
  }

private:
  int fd_{-1};
};

[[nodiscard]] std::array<UniqueFd, 2U> MakePacketPair() {
  int descriptors[2]{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors) !=
      0) {
    Fail(ErrnoDetail("could not create native runtime private channel", errno));
  }
  return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}

[[nodiscard]] std::array<UniqueFd, 2U> MakePipe() {
  int descriptors[2]{-1, -1};
  if (::pipe2(descriptors, O_CLOEXEC) != 0) {
    Fail(ErrnoDetail("could not create native runtime exec pipe", errno));
  }
  return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}

void SetNonBlocking(const int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    Fail(ErrnoDetail("could not make native runtime channel nonblocking",
                     errno));
  }
}

[[nodiscard]] std::string RandomHex() {
  std::array<unsigned char, 16U> bytes{};
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      Fail(ErrnoDetail("could not mint native runtime identity", errno));
    }
    offset += static_cast<std::size_t>(count);
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(kHex[(byte >> 4U) & 0x0fU]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

[[nodiscard]] bool WaitForEvent(const int fd, const short events,
                                const std::chrono::milliseconds timeout) {
  pollfd descriptor{fd, static_cast<short>(events | POLLHUP), 0};
  int result = -1;
  do {
    result = ::poll(&descriptor, 1U, static_cast<int>(timeout.count()));
  } while (result < 0 && errno == EINTR);
  if (result < 0) {
    Fail(ErrnoDetail("native runtime channel poll failed", errno));
  }
  if (result == 0) {
    return false;
  }
  if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
    Fail("native runtime private channel became invalid");
  }
  return (descriptor.revents & events) != 0 ||
         (descriptor.revents & POLLHUP) != 0;
}

void SendPacket(const int fd, const std::string_view bytes,
                const std::chrono::milliseconds timeout) {
  if (bytes.empty() || bytes.size() > kMaximumControlPacketBytes) {
    Fail("native runtime control packet is empty or oversized");
  }
  if (!WaitForEvent(fd, POLLOUT, timeout)) {
    Fail("native runtime control write timed out");
  }
  ssize_t count = -1;
  do {
    count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
  } while (count < 0 && errno == EINTR);
  if (count < 0) {
    Fail(ErrnoDetail("native runtime control write failed", errno));
  }
  if (count != static_cast<ssize_t>(bytes.size())) {
    Fail("native runtime control packet was not sent atomically");
  }
}

[[nodiscard]] std::optional<std::string>
ReceivePacket(const int fd, const std::chrono::milliseconds timeout) {
  if (!WaitForEvent(fd, POLLIN, timeout)) {
    return std::nullopt;
  }
  char probe = '\0';
  ssize_t count = -1;
  do {
    count = ::recv(fd, &probe, 1U, MSG_PEEK | MSG_TRUNC);
  } while (count < 0 && errno == EINTR);
  if (count < 0) {
    Fail(ErrnoDetail("native runtime control read failed", errno));
  }
  if (count == 0) {
    Fail("native runtime child closed its private channel");
  }
  if (static_cast<std::size_t>(count) > kMaximumControlPacketBytes) {
    Fail("native runtime control response exceeds its byte bound");
  }
  std::string bytes(static_cast<std::size_t>(count), '\0');
  ssize_t received = -1;
  do {
    received = ::recv(fd, bytes.data(), bytes.size(), 0);
  } while (received < 0 && errno == EINTR);
  if (received != count) {
    if (received < 0) {
      Fail(ErrnoDetail("native runtime control read failed", errno));
    }
    Fail("native runtime control response changed while being received");
  }
  return bytes;
}

[[nodiscard]] std::uint64_t
ParseCanonicalUnsigned(const std::string_view value,
                       const std::string_view label) {
  if (value.empty() || (value.size() > 1U && value.front() == '0')) {
    Fail(std::string(label) + " is not a canonical unsigned integer");
  }
  std::uint64_t result = 0U;
  for (const char digit : value) {
    if (digit < '0' || digit > '9') {
      Fail(std::string(label) + " is not a canonical unsigned integer");
    }
    const auto next = static_cast<std::uint64_t>(digit - '0');
    if (result > (std::numeric_limits<std::uint64_t>::max() - next) / 10U) {
      Fail(std::string(label) + " overflows");
    }
    result = result * 10U + next;
  }
  return result;
}

[[nodiscard]] const manyforge::core::RuntimeUnitDTO &
FindRuntimeUnit(const RetainedBundle &bundle,
                const manyforge::core::RuntimeUnitId &runtime_unit_id) {
  const auto &units = bundle.value().cell_graph.runtime_units;
  const auto found =
      std::find_if(units.begin(), units.end(), [&](const auto &unit) {
        return unit.runtime_unit_id == runtime_unit_id;
      });
  if (found == units.end()) {
    Fail("native runtime candidate bundle lacks the queried runtime unit");
  }
  return *found;
}

[[nodiscard]] std::set<ProjectionBindingKey>
NativeProjectionBindingsForRuntime(
    const RetainedBundle &bundle,
    const manyforge::core::RuntimeUnitId &runtime_unit_id) {
  std::set<ProjectionBindingKey> result;
  for (const auto &binding :
       bundle.value().capability_provider_bindings) {
    if (!manyforge::core::RequiresNativeSceneProjection(
            binding.capability_id,
            binding.capability_definition_revision)) {
      continue;
    }
    std::set<manyforge::core::RuntimeUnitId> runtime_sources;
    for (const auto &endpoint : binding.endpoint_bindings) {
      if (endpoint.source_kind !=
          manyforge::core::CapabilityEndpointSourceKind::kRuntimeUnit) {
        continue;
      }
      const auto *source =
          std::get_if<manyforge::core::RuntimeUnitId>(&endpoint.source_id);
      if (source == nullptr || source->empty()) {
        Fail("native projection endpoint has a mismatched runtime source");
      }
      runtime_sources.insert(*source);
    }
    if (runtime_sources.size() != 1U) {
      Fail("native projection binding does not resolve one exact runtime");
    }
    if (*runtime_sources.begin() != runtime_unit_id) {
      continue;
    }
    if (!result
             .emplace(binding.capability_provider_binding_id,
                      binding.capability_provider_binding_revision)
             .second) {
      Fail("native projection binding identity is repeated for one runtime");
    }
  }
  return result;
}

void VerifyAdmissionMatchesQuery(
    const NativeExecutionRuntimeUnitAdmission &admission,
    const RetainedBundle &bundle, const RuntimeUnitConvergenceQueryDTO &query) {
  const auto &product = admission.product();
  const auto &plan = query.plan;
  const auto &runtime_unit = FindRuntimeUnit(bundle, plan.runtime_unit_id);
  const std::vector<manyforge::core::RuntimeRoleId> roles{
      manyforge::core::RuntimeRoleId(std::string(product.runtime_role_id()))};
  if (bundle.retained_bundle_id() != query.candidate_retained_bundle_id ||
      bundle.value().cell_graph.graph_revision !=
          query.candidate_graph_revision ||
      runtime_unit.runtime_profile_id != plan.runtime_profile_id ||
      runtime_unit.runtime_profile_revision != plan.runtime_profile_revision ||
      runtime_unit.dependency_runtime_unit_ids !=
          plan.dependency_runtime_unit_ids ||
      plan.lifecycle_control !=
          manyforge::core::RuntimeLifecycleControl::kManagerManaged ||
      product.runtime_unit_id() != plan.runtime_unit_id ||
      product.retained_bundle_id() != query.candidate_retained_bundle_id ||
      product.product_id() != plan.product_id ||
      product.product_contract_id() != plan.product_contract_id ||
      product.product_contract_revision() != plan.product_contract_revision ||
      product.product_identity() != plan.product_identity ||
      product.configuration_identity() != plan.configuration_identity ||
      roles != plan.required_runtime_role_ids ||
      product.status_endpoint_role_id() !=
          plan.status_endpoint_role_id.value() ||
      product.status_endpoint_contract_id() !=
          plan.status_endpoint_contract_id ||
      product.status_endpoint_contract_revision() !=
          plan.status_endpoint_contract_revision ||
      product.status_transport() != plan.status_transport ||
      product.process_adapter_contract_id() !=
          plan.process_adapter_contract_id ||
      product.process_adapter_contract_revision() !=
          plan.process_adapter_contract_revision ||
      product.retrieval_contract_id() != plan.retrieval_contract_id ||
      product.retrieval_contract_revision() !=
          plan.retrieval_contract_revision ||
      !plan.launchable_content_digest.has_value() ||
      product.launchable_artifact_ref().content_digest !=
          *plan.launchable_content_digest ||
      product.launchable_artifact_ref().byte_size !=
          plan.launchable_byte_size ||
      product.operating_system() != plan.operating_system ||
      product.launch_environment_contract() != plan.shell_contract ||
      admission.installed_target_id() != contract::kInstalledTargetId) {
    Fail("verified native runtime admission differs from the exact "
         "convergence query");
  }
}

[[nodiscard]] bool
SameChildProduct(const NativeExecutionRuntimeUnitAdmission &admission,
                 const std::string &product_identity,
                 const std::string &configuration_identity,
                 const std::string &implementation_revision) {
  return admission.product().product_identity().value() == product_identity &&
         admission.product().configuration_identity().value() ==
             configuration_identity &&
         admission.implementation_revision() == implementation_revision;
}

[[noreturn]] void ChildExit(const int error_fd, const int error) noexcept {
  const int saved = error;
  const auto *bytes = reinterpret_cast<const char *>(&saved);
  std::size_t offset = 0U;
  while (offset < sizeof(saved)) {
    const auto count =
        ::write(error_fd, bytes + offset, sizeof(saved) - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      break;
    }
    offset += static_cast<std::size_t>(count);
  }
  ::_exit(126);
}

[[noreturn]] void EnterChild(const pid_t expected_parent, const int channel_fd,
                             const int error_fd, const int executable_fd,
                             char *const argv[]) noexcept {
  if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
    ChildExit(error_fd, errno);
  }
  if (::getppid() != expected_parent) {
    ChildExit(error_fd, ECHILD);
  }
  const int channel_copy =
      ::fcntl(channel_fd, F_DUPFD_CLOEXEC, kChildExecutableFd + 1);
  const int error_copy =
      ::fcntl(error_fd, F_DUPFD_CLOEXEC, kChildExecutableFd + 1);
  const int executable_copy =
      ::fcntl(executable_fd, F_DUPFD_CLOEXEC, kChildExecutableFd + 1);
  if (channel_copy < 0 || error_copy < 0 || executable_copy < 0 ||
      ::dup3(channel_copy, kChildChannelFd, 0) < 0 ||
      ::dup3(error_copy, kChildExecErrorFd, O_CLOEXEC) < 0 ||
      ::dup3(executable_copy, kChildExecutableFd, O_CLOEXEC) < 0) {
    ChildExit(error_fd, errno);
  }
#ifdef SYS_close_range
  const long closed =
      ::syscall(SYS_close_range, kChildExecutableFd + 1U, UINT_MAX, 0U);
  if (closed != 0 && errno != ENOSYS) {
    ChildExit(kChildExecErrorFd, errno);
  }
  if (closed != 0) {
#endif
    const long maximum = ::sysconf(_SC_OPEN_MAX);
    const int upper =
        maximum > 0 && maximum < INT_MAX ? static_cast<int>(maximum) : 65536;
    for (int descriptor = kChildExecutableFd + 1; descriptor < upper;
         ++descriptor) {
      static_cast<void>(::close(descriptor));
    }
#ifdef SYS_close_range
  }
#endif
  ::fexecve(kChildExecutableFd, argv, environ);
  ChildExit(kChildExecErrorFd, errno);
}

void KillAndReap(const pid_t pid) noexcept {
  if (pid <= 0) {
    return;
  }
  int status = 0;
  pid_t observed = -1;
  do {
    observed = ::waitpid(pid, &status, WNOHANG);
  } while (observed < 0 && errno == EINTR);
  if (observed == pid || (observed < 0 && errno == ECHILD)) {
    return;
  }
  if (observed < 0) {
    return;
  }
  static_cast<void>(::kill(pid, SIGKILL));
  while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
}

void ConfirmExecOrThrow(const pid_t pid, const int error_fd) {
  pollfd descriptor{error_fd, POLLIN | POLLHUP, 0};
  int result = -1;
  do {
    result = ::poll(
        &descriptor, 1U,
        static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(kExecTimeout)
                .count()));
  } while (result < 0 && errno == EINTR);
  if (result <= 0) {
    KillAndReap(pid);
    if (result == 0) {
      Fail("native execution target exec timed out");
    }
    Fail(ErrnoDetail("native execution target exec poll failed", errno));
  }
  int child_error = 0;
  ssize_t count = -1;
  do {
    count = ::read(error_fd, &child_error, sizeof(child_error));
  } while (count < 0 && errno == EINTR);
  if (count == 0) {
    return;
  }
  KillAndReap(pid);
  if (count == static_cast<ssize_t>(sizeof(child_error))) {
    Fail(ErrnoDetail("native execution target exec failed", child_error));
  }
  if (count < 0) {
    Fail(ErrnoDetail("native execution target exec status failed", errno));
  }
  Fail("native execution target returned a malformed exec status");
}

} // namespace

struct RetainedNativeRuntimeUnitAdapter::Impl final
    : public std::enable_shared_from_this<
          RetainedNativeRuntimeUnitAdapter::Impl> {
  class ProjectionClient;

  struct Child final {
    struct ProjectionPreparation final {
      manyforge::core::RetainedBundleId retained_bundle_id;
      manyforge::core::GraphRevision graph_revision;
      manyforge::core::RetainedProductDTO planning_collision_model;
    };

    pid_t pid;
    RuntimeEpoch runtime_epoch;
    UniqueFd channel;
    std::string product_identity;
    std::string configuration_identity;
    std::string implementation_revision;
    std::set<ProjectionBindingKey> admitted_projection_bindings;
    std::map<ProjectionBindingKey, ProjectionPreparation>
        prepared_projection_products;

    Child(pid_t child_pid, RuntimeEpoch child_epoch, UniqueFd child_channel,
          std::string selected_product_identity,
          std::string selected_configuration_identity,
          std::string selected_implementation_revision,
          std::set<ProjectionBindingKey> selected_projection_bindings)
        : pid(child_pid), runtime_epoch(std::move(child_epoch)),
          channel(std::move(child_channel)),
          product_identity(std::move(selected_product_identity)),
          configuration_identity(std::move(selected_configuration_identity)),
          implementation_revision(std::move(selected_implementation_revision)),
          admitted_projection_bindings(
              std::move(selected_projection_bindings)) {
    }

    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    Child(Child &&) noexcept = default;
    Child &operator=(Child &&) noexcept = default;
  };

  Impl(std::filesystem::path trusted_installed_target_root,
       std::filesystem::path trusted_package_root,
       std::filesystem::path manager_state_root,
       std::filesystem::path private_staging_root,
       const manyforge::deployment::IVerifiedRetainedBundleSource
           &selected_bundle_source,
       const manyforge::deployment::IVerifiedRetainedProductArtifactSource
           &selected_artifact_source,
       std::string selected_operating_system,
       std::string selected_launch_environment_contract)
      : target_root_path(NormalizedPlainDirectory(
            std::move(trusted_installed_target_root),
            "trusted installed-target root")),
        package_root_path(NormalizedPlainDirectory(
            std::move(trusted_package_root), "trusted retained-package root")),
        manager_state_root_path(NormalizedPlainDirectory(
            std::move(manager_state_root), "Manager state root")),
        staging_root_path(NormalizedPlainDirectory(
            std::move(private_staging_root), "private native staging root")),
        bundle_source(selected_bundle_source),
        artifact_source(selected_artifact_source),
        operating_system(std::move(selected_operating_system)),
        launch_environment_contract(
            std::move(selected_launch_environment_contract)),
        observer_instance_id("native-observer-" + RandomHex()) {
    if (operating_system.empty() || launch_environment_contract.empty()) {
      throw std::invalid_argument(
          "retained native adapter construction input is invalid");
    }
    const int fd = ::open(target_root_path.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      Fail(ErrnoDetail("could not open trusted installed-target root", errno));
    }
    target_root_fd.Reset(fd);
    struct stat status {};
    if (::fstat(target_root_fd.get(), &status) != 0 ||
        !S_ISDIR(status.st_mode)) {
      Fail("trusted installed-target root is not a directory");
    }
  }

  ~Impl() {
    std::lock_guard lock(mutex);
    for (auto &[runtime_id, child] : children) {
      static_cast<void>(runtime_id);
      StopChildNoThrow(child);
    }
  }

  [[nodiscard]] std::pair<std::shared_ptr<const RetainedBundle>,
                          NativeExecutionRuntimeUnitAdmission>
  Admission(const RuntimeUnitConvergenceQueryDTO &query) const {
    if (!query.valid()) {
      Fail("retained native adapter received an invalid convergence query");
    }
    auto bundle =
        bundle_source.get_verified(query.candidate_retained_bundle_id);
    if (bundle == nullptr) {
      Fail("retained native adapter could not reload the candidate bundle");
    }
    auto admission = VerifyNativeExecutionRuntimeUnitForHost(
        *bundle, query.plan.runtime_unit_id, artifact_source, operating_system,
        launch_environment_contract);
    if (!admission.has_value()) {
      Fail("retained native adapter query has no qualified native product");
    }
    VerifyAdmissionMatchesQuery(*admission, *bundle, query);
    return {std::move(bundle), std::move(*admission)};
  }

  [[nodiscard]] UniqueFd OpenInstalledTarget() const {
    const int fd = ::openat(target_root_fd.get(),
                            std::string(contract::kInstalledTargetId).c_str(),
                            O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      Fail(ErrnoDetail("could not open fixed native execution target", errno));
    }
    UniqueFd target(fd);
    struct stat status {};
    if (::fstat(target.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
      Fail("fixed native execution target is not one protected regular file");
    }
    return target;
  }

  [[nodiscard]] std::uint64_t NextObservationSequence() {
    if (observation_sequence == std::numeric_limits<std::uint64_t>::max()) {
      Fail("native runtime process observation sequence is exhausted");
    }
    return ++observation_sequence;
  }

  [[nodiscard]] ProcessPresenceObservation
  Presence(const RuntimeUnitConvergenceQueryDTO &query,
           const ProcessPresenceState state,
           const std::string_view runtime_epoch = {}) {
    return ProcessPresenceObservation(
        query.plan.runtime_unit_id.value(), observer_instance_id,
        NextObservationSequence(), state, std::string(runtime_epoch));
  }

  [[nodiscard]] bool ChildIsLive(Child &child) {
    int status = 0;
    pid_t result = -1;
    do {
      result = ::waitpid(child.pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
      return true;
    }
    if (result == child.pid) {
      child.channel.Reset();
      return false;
    }
    if (result < 0 && errno == ECHILD) {
      Fail("tracked native runtime is no longer a direct Manager child");
    }
    if (result < 0) {
      Fail(ErrnoDetail("could not observe native runtime child", errno));
    }
    Fail("native runtime child observation returned a foreign PID");
  }

  [[nodiscard]] detail::NativeProjectionRpcResponse ExchangeProjectionRaw(
      Child &child, const detail::NativeProjectionRpcRequest &request) {
    const auto encoded = detail::EncodeNativeProjectionRpcRequest(request);
    SendPacket(child.channel.get(), encoded, kStatusTimeout);
    const auto packet = ReceivePacket(child.channel.get(), kStatusTimeout);
    if (!packet.has_value()) {
      Fail("native projection response timed out");
    }
    auto response = detail::DecodeNativeProjectionRpcResponse(*packet);
    return response;
  }

  [[nodiscard]] detail::NativeProjectionRpcResponse ExchangeProjection(
      Child &child, const detail::NativeProjectionRpcRequest &request) {
    auto response = ExchangeProjectionRaw(child, request);
    if (const auto *failure =
            std::get_if<detail::NativeProjectionRpcFailure>(&response)) {
      Fail("native projection request failed [" + failure->error.code +
           "]: " + failure->error.message);
    }
    return response;
  }

  template <typename Response>
  [[nodiscard]] Response ProjectionCall(
      const manyforge::core::ProjectionNativeRuntimeBindingDTO &binding,
      detail::NativeProjectionRpcRequest request) {
    if (!binding.valid()) {
      Fail("native projection request binding is invalid");
    }
    std::lock_guard lock(mutex);
    const auto found = children.find(binding.runtime_unit_id.value());
    if (found == children.end() ||
        found->second.runtime_epoch != binding.runtime_epoch) {
      Fail("native projection client no longer names the current child epoch");
    }
    const ProjectionBindingKey key{binding.provider_binding_id,
                                   binding.provider_binding_revision};
    if (!found->second.admitted_projection_bindings.contains(key)) {
      Fail("native projection client binding is no longer admitted");
    }
    if (!ChildIsLive(found->second)) {
      children.erase(found);
      Fail("native projection child exited before its response");
    }
    auto response = ExchangeProjection(found->second, request);
    const auto *typed = std::get_if<Response>(&response);
    if (typed == nullptr) {
      Fail("native projection child returned the wrong response kind");
    }
    return *typed;
  }

  template <typename Response>
  [[nodiscard]] manyforge::core::Result<Response> ProjectionResultCall(
      const manyforge::core::ProjectionNativeRuntimeBindingDTO &binding,
      detail::NativeProjectionRpcRequest request) {
    if (!binding.valid()) {
      Fail("native projection request binding is invalid");
    }
    std::lock_guard lock(mutex);
    const auto found = children.find(binding.runtime_unit_id.value());
    if (found == children.end() ||
        found->second.runtime_epoch != binding.runtime_epoch) {
      Fail("native projection client no longer names the current child epoch");
    }
    const ProjectionBindingKey key{binding.provider_binding_id,
                                   binding.provider_binding_revision};
    if (!found->second.admitted_projection_bindings.contains(key)) {
      Fail("native projection client binding is no longer admitted");
    }
    if (!ChildIsLive(found->second)) {
      children.erase(found);
      Fail("native projection child exited before its response");
    }
    auto response = ExchangeProjectionRaw(found->second, request);
    if (const auto *failure =
            std::get_if<detail::NativeProjectionRpcFailure>(&response)) {
      return manyforge::core::Result<Response>::Err(failure->error);
    }
    const auto *typed = std::get_if<Response>(&response);
    if (typed == nullptr) {
      Fail("native projection child returned the wrong response kind");
    }
    return manyforge::core::Result<Response>::Ok(*typed);
  }

  template <typename Response, typename Request>
  [[nodiscard]] Response KernelFenceCall(const Request &request) {
    std::lock_guard lock(mutex);
    if (children.size() != 1U) {
      Fail("Kernel Apply-fence request requires the sole managed native "
           "execution child");
    }
    auto found = children.begin();
    if (!ChildIsLive(found->second)) {
      children.erase(found);
      Fail("managed Kernel child exited before its fence response");
    }
    const auto encoded = detail::EncodeKernelApplyFenceRpcRequest(
        detail::KernelApplyFenceRpcRequest(request));
    SendPacket(found->second.channel.get(), encoded, kStatusTimeout);
    const auto packet =
        ReceivePacket(found->second.channel.get(), kStatusTimeout);
    if (!packet.has_value()) {
      Fail("managed Kernel Apply-fence response timed out");
    }
    auto response = detail::DecodeKernelApplyFenceRpcResponse(*packet);
    if (const auto *failure =
            std::get_if<detail::KernelApplyFenceRpcFailure>(&response)) {
      Fail("managed Kernel Apply-fence request failed [" + failure->error.code +
           "]: " + failure->error.message);
    }
    const auto *typed = std::get_if<Response>(&response);
    if (typed == nullptr) {
      Fail("managed Kernel child returned the wrong fence response kind");
    }
    return *typed;
  }

  void PrepareChildProjectionBindings(
      Child &child, const RetainedBundle &bundle,
      const manyforge::core::RuntimeUnitId &runtime_unit_id) {
    const auto bindings =
        NativeProjectionBindingsForRuntime(bundle, runtime_unit_id);
    for (const auto &[provider_binding_id, provider_binding_revision] :
         bindings) {
      const ProjectionBindingKey key{provider_binding_id,
                                     provider_binding_revision};
      detail::NativeProjectionPrepareRequest request{
          {provider_binding_id, provider_binding_revision, runtime_unit_id,
           child.runtime_epoch},
          bundle.retained_bundle_id(),
          bundle.value().cell_graph.graph_revision,
          bundle.value().compiled_products.planning_collision_model,
      };
      if (!request.valid()) {
        Fail("native projection preparation request is invalid");
      }
      const auto previous = child.prepared_projection_products.find(key);
      if (previous != child.prepared_projection_products.end() &&
          previous->second.retained_bundle_id ==
              request.candidate_retained_bundle_id &&
          previous->second.graph_revision ==
              request.candidate_graph_revision &&
          previous->second.planning_collision_model ==
              request.planning_collision_model) {
        continue;
      }
      auto response = ExchangeProjection(child, request);
      const auto *prepared =
          std::get_if<detail::NativeProjectionPrepareResult>(&response);
      if (prepared == nullptr ||
          prepared->request.native_runtime_binding !=
              request.native_runtime_binding ||
          prepared->request.candidate_retained_bundle_id !=
              request.candidate_retained_bundle_id ||
          prepared->request.candidate_graph_revision !=
              request.candidate_graph_revision ||
          prepared->request.planning_collision_model !=
              request.planning_collision_model) {
        Fail("native projection child did not acknowledge the exact prepared "
             "binding and product");
      }
      child.admitted_projection_bindings.emplace(provider_binding_id,
                                                  provider_binding_revision);
      child.prepared_projection_products.insert_or_assign(
          key, Child::ProjectionPreparation{
                   request.candidate_retained_bundle_id,
                   request.candidate_graph_revision,
                   request.planning_collision_model});
    }
  }

  [[nodiscard]] bool
  WaitForChildExit(Child &child,
                   const std::chrono::steady_clock::time_point deadline) {
    for (;;) {
      if (!ChildIsLive(child)) {
        return true;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }

  void StopChildNoThrow(Child &child) noexcept {
    try {
      StopChild(child);
    } catch (...) {
      KillAndReap(child.pid);
    }
    child.channel.Reset();
  }

  void StopChild(Child &child) {
    if (!ChildIsLive(child)) {
      child.channel.Reset();
      return;
    }
    try {
      SendPacket(child.channel.get(), "shutdown",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     kStatusTimeout));
    } catch (...) {
      // A failed graceful request does not weaken the direct-child stop
      // guarantee; continue through the bounded signal path below.
    }
    if (WaitForChildExit(child, std::chrono::steady_clock::now() +
                                    kGracefulStopTimeout)) {
      child.channel.Reset();
      return;
    }
    if (::kill(child.pid, SIGTERM) != 0 && errno != ESRCH) {
      Fail(ErrnoDetail("could not terminate native runtime child", errno));
    }
    if (WaitForChildExit(child,
                         std::chrono::steady_clock::now() + kTermStopTimeout)) {
      child.channel.Reset();
      return;
    }
    if (::kill(child.pid, SIGKILL) != 0 && errno != ESRCH) {
      Fail(ErrnoDetail("could not kill native runtime child", errno));
    }
    int status = 0;
    pid_t result = -1;
    do {
      result = ::waitpid(child.pid, &status, 0);
    } while (result < 0 && errno == EINTR);
    if (result != child.pid) {
      if (result < 0) {
        Fail(ErrnoDetail("could not reap native runtime child", errno));
      }
      Fail("native runtime child reap returned a foreign PID");
    }
    child.channel.Reset();
  }

  [[nodiscard]] Child
  LaunchChild(const NativeExecutionRuntimeUnitAdmission &admission,
              const RetainedBundle &bundle) {
    auto target = OpenInstalledTarget();
    auto channels = MakePacketPair();
    auto exec_pipe = MakePipe();
    SetNonBlocking(channels[0].get());
    const RuntimeEpoch runtime_epoch("native-runtime-" + RandomHex());
    const auto launch_arguments =
        admission.launch_arguments(kChildChannelFd, runtime_epoch);
    std::vector<std::string> arguments;
    arguments.reserve(launch_arguments.size() + 9U);
    arguments.emplace_back(contract::kInstalledTargetId);
    arguments.insert(arguments.end(), launch_arguments.begin(),
                     launch_arguments.end());
    arguments.emplace_back("--manyforge-managed-package-root");
    arguments.push_back(package_root_path.string());
    arguments.emplace_back("--manyforge-managed-manager-state-root");
    arguments.push_back(manager_state_root_path.string());
    arguments.emplace_back("--manyforge-managed-staging-root");
    arguments.push_back(staging_root_path.string());
    arguments.emplace_back("--manyforge-managed-cell-id");
    arguments.push_back(bundle.value().cell_graph.cell_id.value());
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (auto &argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
      Fail(ErrnoDetail("could not fork native execution target", errno));
    }
    if (pid == 0) {
      EnterChild(parent, channels[1].get(), exec_pipe[1].get(), target.get(),
                 argv.data());
    }

    channels[1].Reset();
    exec_pipe[1].Reset();
    target.Reset();
    try {
      ConfirmExecOrThrow(pid, exec_pipe[0].get());
      exec_pipe[0].Reset();
      SendPacket(
          channels[0].get(), "hello",
          std::chrono::duration_cast<std::chrono::milliseconds>(kReadyTimeout));
      const auto ready = ReceivePacket(
          channels[0].get(),
          std::chrono::duration_cast<std::chrono::milliseconds>(kReadyTimeout));
      if (!ready.has_value() ||
          *ready != admission.expected_ready_packet(runtime_epoch)) {
        Fail("native execution child returned malformed or foreign readiness");
      }
    } catch (...) {
      channels[0].Reset();
      KillAndReap(pid);
      throw;
    }
    Child child(pid, runtime_epoch, std::move(channels[0]),
                admission.product().product_identity().value(),
                admission.product().configuration_identity().value(),
                admission.implementation_revision(), {});
    try {
      PrepareChildProjectionBindings(child, bundle,
                                     admission.product().runtime_unit_id());
    } catch (...) {
      StopChildNoThrow(child);
      throw;
    }
    return child;
  }

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) {
    auto [bundle, admission] = Admission(query);
    static_cast<void>(bundle);
    std::lock_guard lock(mutex);
    const auto found = children.find(query.plan.runtime_unit_id.value());
    if (found == children.end()) {
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    if (!SameChildProduct(admission, found->second.product_identity,
                          found->second.configuration_identity,
                          found->second.implementation_revision)) {
      if (ChildIsLive(found->second)) {
        Fail("live native runtime child differs from the selected product");
      }
      children.erase(found);
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    if (!ChildIsLive(found->second)) {
      children.erase(found);
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    return Presence(query, ProcessPresenceState::kPresent,
                    found->second.runtime_epoch.value());
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) {
    auto [bundle, admission] = Admission(query);
    std::lock_guard lock(mutex);
    const auto runtime_id = query.plan.runtime_unit_id.value();
    auto found = children.find(runtime_id);
    if (found != children.end()) {
      if (!SameChildProduct(admission, found->second.product_identity,
                            found->second.configuration_identity,
                            found->second.implementation_revision)) {
        if (ChildIsLive(found->second)) {
          Fail("cannot replace a live native child with another product");
        }
        children.erase(found);
      } else if (ChildIsLive(found->second)) {
        PrepareChildProjectionBindings(found->second, *bundle,
                                       query.plan.runtime_unit_id);
        return Presence(query, ProcessPresenceState::kPresent,
                        found->second.runtime_epoch.value());
      } else {
        children.erase(found);
      }
    }
    auto [inserted, unique] =
        children.emplace(runtime_id, LaunchChild(admission, *bundle));
    if (!unique) {
      Fail("native runtime child registry insertion conflicted");
    }
    return Presence(query, ProcessPresenceState::kPresent,
                    inserted->second.runtime_epoch.value());
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) {
    auto [bundle, admission] = Admission(query);
    static_cast<void>(bundle);
    std::lock_guard lock(mutex);
    const auto found = children.find(query.plan.runtime_unit_id.value());
    if (found == children.end()) {
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    if (!SameChildProduct(admission, found->second.product_identity,
                          found->second.configuration_identity,
                          found->second.implementation_revision)) {
      Fail("native runtime stop query differs from the tracked child");
    }
    StopChild(found->second);
    children.erase(found);
    return Presence(query, ProcessPresenceState::kAbsent);
  }

  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const RuntimeEpoch &runtime_epoch) {
    auto [bundle, admission] = Admission(query);
    static_cast<void>(bundle);
    if (runtime_epoch.empty()) {
      Fail("retained native adapter received an empty runtime epoch");
    }
    std::lock_guard lock(mutex);
    const auto found = children.find(query.plan.runtime_unit_id.value());
    if (found == children.end() ||
        found->second.runtime_epoch != runtime_epoch) {
      return std::nullopt;
    }
    if (!SameChildProduct(admission, found->second.product_identity,
                          found->second.configuration_identity,
                          found->second.implementation_revision)) {
      Fail("native runtime progress query differs from the tracked child");
    }
    if (!ChildIsLive(found->second)) {
      children.erase(found);
      return std::nullopt;
    }
    SendPacket(found->second.channel.get(), "status", kStatusTimeout);
    const auto packet =
        ReceivePacket(found->second.channel.get(), kStatusTimeout);
    if (!packet.has_value()) {
      return std::nullopt;
    }
    std::istringstream stream(*packet);
    std::vector<std::string> fields;
    std::string field;
    while (stream >> field) {
      fields.push_back(std::move(field));
    }
    if (fields.size() != 12U) {
      Fail("native execution child emitted a malformed status field count");
    }
    const auto sequence =
        ParseCanonicalUnsigned(fields[3], "native source-report sequence");
    if (sequence == 0U ||
        *packet != admission.expected_status_packet(runtime_epoch, sequence)) {
      Fail("native execution child emitted a malformed or foreign status");
    }
    return RuntimeUnitSourceReport(
        query.plan.runtime_unit_id.value(), runtime_epoch.value(), sequence, 1U,
        RuntimeSourcePhase::kServing, RuntimeSourceHealth::kHealthy,
        query.plan.product_identity.value(),
        query.plan.configuration_identity.value(),
        {std::string(contract::kRuntimeRoleId)},
        ManagedCommandSurfaceState::kClosedNoActiveEffect);
  }

  [[nodiscard]] manyforge::core::Result<
      std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
  ResolveProjection(
      const manyforge::core::ProjectionNativeRuntimeBindingDTO &binding);

  std::filesystem::path target_root_path;
  std::filesystem::path package_root_path;
  std::filesystem::path manager_state_root_path;
  std::filesystem::path staging_root_path;
  const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source;
  const manyforge::deployment::IVerifiedRetainedProductArtifactSource
      &artifact_source;
  std::string operating_system;
  std::string launch_environment_contract;
  std::string observer_instance_id;
  UniqueFd target_root_fd;
  std::mutex mutex;
  std::uint64_t observation_sequence{};
  std::map<std::string, Child> children;
};

class RetainedNativeRuntimeUnitAdapter::Impl::ProjectionClient final
    : public manyforge::core::IPlannerSceneProjection {
public:
  ProjectionClient(
      std::shared_ptr<Impl> adapter,
      manyforge::core::ProjectionNativeRuntimeBindingDTO binding)
      : adapter_(std::move(adapter)), binding_(std::move(binding)) {}

  [[noreturn]] static void Unavailable(const std::string_view operation) {
    Fail("native projection " + std::string(operation) +
         " is unavailable before the durable transition checkpoint");
  }

  manyforge::core::ProjectionCommitResultDTO ApplyTransaction(
      const manyforge::core::SceneTransactionDTO &transaction) override {
    return adapter_->ProjectionCall<manyforge::core::ProjectionCommitResultDTO>(
        binding_, detail::NativeProjectionTransactionRequest{binding_,
                                                              transaction});
  }

  manyforge::core::Result<manyforge::core::CollisionWorldLeaseInventory>
  RetainProjection(const manyforge::core::ProjectionRevisionDTO &,
                   manyforge::core::ProjectionLeaseId) override {
    Unavailable("lease retention");
  }

  manyforge::core::Result<manyforge::core::ProjectionLeaseBindingDTO>
  RetainProjectionBinding(
      const manyforge::core::ProjectionRevisionDTO &revision,
      manyforge::core::ProjectionLeaseId lease_id) override {
    auto result = adapter_->ProjectionResultCall<
        detail::NativeProjectionRetainResult>(
        binding_, detail::NativeProjectionRetainRequest{
                      binding_, revision, std::move(lease_id)});
    if (!result.ok()) {
      return manyforge::core::Result<
          manyforge::core::ProjectionLeaseBindingDTO>::Err(result.error());
    }
    return manyforge::core::Result<
        manyforge::core::ProjectionLeaseBindingDTO>::Ok(
        std::move(result.value().projection_binding));
  }

  void ReleaseProjection(
      const manyforge::core::ProjectionLeaseId &lease_id) override {
    const auto released = adapter_->ProjectionCall<
        detail::NativeProjectionReleaseResult>(
        binding_, detail::NativeProjectionReleaseRequest{binding_, lease_id});
    if (released.projection_lease_id != lease_id) {
      Fail("native projection released a different lease");
    }
  }

  manyforge::core::ProjectionHealthDTO ProjectionHealth() const override {
    return adapter_->ProjectionCall<manyforge::core::ProjectionHealthDTO>(
        binding_, detail::NativeProjectionHealthQuery{binding_});
  }

  manyforge::core::ProjectionCommitResultDTO RebuildProjection(
      const manyforge::core::NativeProjectionRebuildDTO &request) override {
    return adapter_->ProjectionCall<manyforge::core::ProjectionCommitResultDTO>(
        binding_, request);
  }

  manyforge::core::NativeProjectionBootstrapRebuildResultDTO
  RebuildBootstrapProjection(
      const manyforge::core::NativeProjectionBootstrapRebuildDTO &request)
      override {
    return adapter_->ProjectionCall<
        manyforge::core::NativeProjectionBootstrapRebuildResultDTO>(binding_,
                                                                    request);
  }

  manyforge::core::NativeProjectionBootstrapStatusDTO BootstrapProjectionStatus(
      const manyforge::core::NativeProjectionBootstrapStatusQueryDTO &request) const
      override {
    return adapter_->ProjectionCall<
        manyforge::core::NativeProjectionBootstrapStatusDTO>(binding_,
                                                              request);
  }

  manyforge::core::NativeProjectionStageResultDTO StageProjection(
      const manyforge::core::NativeProjectionStageDTO &request) override {
    return adapter_->ProjectionCall<
        manyforge::core::NativeProjectionStageResultDTO>(binding_, request);
  }

  manyforge::core::NativeProjectionActivationResultDTO
  ActivateStagedProjection(
      const manyforge::core::NativeProjectionActivationDTO &request) override {
    return adapter_->ProjectionCall<
        manyforge::core::NativeProjectionActivationResultDTO>(binding_,
                                                               request);
  }

  manyforge::core::NativeProjectionRetireResultDTO RetireProjection(
      const manyforge::core::NativeProjectionRetireDTO &request) override {
    return adapter_->ProjectionCall<
        manyforge::core::NativeProjectionRetireResultDTO>(binding_, request);
  }

  manyforge::core::NativeProjectionRetireStatusDTO ProjectionRetireStatus(
      const manyforge::core::NativeProjectionRetireStatusQueryDTO &request) const
      override {
    return adapter_->ProjectionCall<
        manyforge::core::NativeProjectionRetireStatusDTO>(binding_, request);
  }

  manyforge::core::NativeProjectionDiscardResultDTO DiscardStagedProjection(
      const manyforge::core::NativeProjectionDiscardDTO &request) override {
    return adapter_->ProjectionCall<
        manyforge::core::NativeProjectionDiscardResultDTO>(binding_, request);
  }

  manyforge::core::NativeProjectionStatusDTO ProjectionStageStatus(
      const manyforge::core::NativeProjectionStatusQueryDTO &request) const override {
    return adapter_->ProjectionCall<manyforge::core::NativeProjectionStatusDTO>(
        binding_, request);
  }

  manyforge::core::Result<manyforge::core::SceneFrameResolutionReceiptDTO>
  ResolveSceneFrame(
      const manyforge::core::SceneFrameResolutionQueryDTO &query) const override {
    return adapter_->ProjectionResultCall<
        manyforge::core::SceneFrameResolutionReceiptDTO>(
        binding_, detail::NativeProjectionSceneFrameRequest{binding_, query});
  }

  manyforge::core::Result<manyforge::core::CollisionPolicySnapshot>
  ProjectEffectiveCollisionPolicy(
      const manyforge::core::LogicalSceneSnapshotDTO &logical_scene,
      const std::uint64_t effective_policy_revision_id) const override {
    return adapter_->ProjectionResultCall<
        manyforge::core::CollisionPolicySnapshot>(
        binding_, detail::NativeProjectionEffectivePolicyRequest{
                      binding_, logical_scene,
                      effective_policy_revision_id});
  }

private:
  std::shared_ptr<Impl> adapter_;
  manyforge::core::ProjectionNativeRuntimeBindingDTO binding_;
};

manyforge::core::Result<
    std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
RetainedNativeRuntimeUnitAdapter::Impl::ResolveProjection(
    const manyforge::core::ProjectionNativeRuntimeBindingDTO &binding) {
  const auto missing = [](std::string message) {
    return manyforge::core::Result<
        std::shared_ptr<manyforge::core::IPlannerSceneProjection>>::Err(
        manyforge::core::ErrorDTO{"missing_reference", std::move(message), {}});
  };
  if (!binding.valid()) {
    return missing("native projection binding is invalid");
  }
  {
    std::lock_guard lock(mutex);
    const auto found = children.find(binding.runtime_unit_id.value());
    if (found == children.end() ||
        found->second.runtime_epoch != binding.runtime_epoch) {
      return missing("no exact Manager child owns the native runtime epoch");
    }
    if (!ChildIsLive(found->second)) {
      children.erase(found);
      return missing("the exact Manager child is no longer live");
    }
    const ProjectionBindingKey key{binding.provider_binding_id,
                                   binding.provider_binding_revision};
    if (!found->second.admitted_projection_bindings.contains(key)) {
      return missing(
          "the verified child product does not own the projection binding");
    }
  }
  return manyforge::core::Result<
      std::shared_ptr<manyforge::core::IPlannerSceneProjection>>::Ok(
      std::make_shared<ProjectionClient>(shared_from_this(), binding));
}

RetainedNativeRuntimeUnitAdapter::RetainedNativeRuntimeUnitAdapter(
    std::filesystem::path trusted_installed_target_root,
    std::filesystem::path trusted_package_root,
    std::filesystem::path manager_state_root,
    std::filesystem::path private_staging_root,
    const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string operating_system, std::string launch_environment_contract)
    : impl_(std::make_shared<Impl>(std::move(trusted_installed_target_root),
                                   std::move(trusted_package_root),
                                   std::move(manager_state_root),
                                   std::move(private_staging_root),
                                   bundle_source, artifact_source,
                                   std::move(operating_system),
                                   std::move(launch_environment_contract))) {}

RetainedNativeRuntimeUnitAdapter::~RetainedNativeRuntimeUnitAdapter() = default;

ProcessPresenceObservation RetainedNativeRuntimeUnitAdapter::ObserveProcess(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->ObserveProcess(query);
}

ProcessPresenceObservation RetainedNativeRuntimeUnitAdapter::EnsureRunning(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->EnsureRunning(query);
}

ProcessPresenceObservation RetainedNativeRuntimeUnitAdapter::EnsureStopped(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->EnsureStopped(query);
}

std::optional<RuntimeUnitSourceReport>
RetainedNativeRuntimeUnitAdapter::ObserveProgress(
    const RuntimeUnitConvergenceQueryDTO &query,
    const RuntimeEpoch &runtime_epoch) {
  return impl_->ObserveProgress(query, runtime_epoch);
}

manyforge::core::ConfigurationFenceAdmissionSnapshotDTO
RetainedNativeRuntimeUnitAdapter::ObserveAdmission(
    const manyforge::core::ConfigurationFenceAdmissionQueryDTO &query) const {
  return impl_->KernelFenceCall<
      manyforge::core::ConfigurationFenceAdmissionSnapshotDTO>(query);
}

manyforge::core::ConfigurationFenceAcceptedDTO
RetainedNativeRuntimeUnitAdapter::Acquire(
    const manyforge::core::ApplyConfigurationFenceIntentDTO &intent) {
  return impl_->KernelFenceCall<manyforge::core::ConfigurationFenceAcceptedDTO>(
      intent);
}

manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO
RetainedNativeRuntimeUnitAdapter::ReconcileAcquireAttempt(
    const manyforge::core::ApplyFenceAcquireAttemptQueryDTO &query) const {
  return impl_->KernelFenceCall<
      manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO>(query);
}

manyforge::core::ApplyFenceReconcileResultDTO
RetainedNativeRuntimeUnitAdapter::Rehydrate(
    const manyforge::core::ApplyFenceRehydrateDTO &request) {
  return impl_->KernelFenceCall<manyforge::core::ApplyFenceReconcileResultDTO>(
      request);
}

manyforge::core::ConfigurationFenceStatusDTO
RetainedNativeRuntimeUnitAdapter::QueryStatus(
    const manyforge::core::ConfigurationFenceStatusQueryDTO &query) const {
  return impl_->KernelFenceCall<manyforge::core::ConfigurationFenceStatusDTO>(
      query);
}

manyforge::core::ConfigurationFenceReleasedDTO
RetainedNativeRuntimeUnitAdapter::Release(
    const manyforge::core::ConfigurationFenceReleaseDTO &request) {
  return impl_->KernelFenceCall<manyforge::core::ConfigurationFenceReleasedDTO>(
      request);
}

manyforge::core::Result<
    std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
RetainedNativeRuntimeUnitAdapter::ResolveProjection(
    const manyforge::core::ProjectionNativeRuntimeBindingDTO
        &native_runtime_binding) const {
  return impl_->ResolveProjection(native_runtime_binding);
}

} // namespace manyforge::component_manager
