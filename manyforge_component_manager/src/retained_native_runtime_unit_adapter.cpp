#include "manyforge_component_manager/retained_native_runtime_unit_adapter.hpp"

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
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <map>
#include <mutex>
#include <optional>
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

constexpr int kChildChannelFd = 3;
constexpr int kChildExecErrorFd = 4;
constexpr int kChildExecutableFd = 5;
constexpr std::size_t kMaximumControlPacketBytes = 4096U;
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
  std::array<char, kMaximumControlPacketBytes + 1U> bytes{};
  ssize_t count = -1;
  do {
    count = ::recv(fd, bytes.data(), bytes.size(), MSG_TRUNC);
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
  return std::string(bytes.data(), static_cast<std::size_t>(count));
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

struct RetainedNativeRuntimeUnitAdapter::Impl final {
  struct Child final {
    pid_t pid;
    RuntimeEpoch runtime_epoch;
    UniqueFd channel;
    std::string product_identity;
    std::string configuration_identity;
    std::string implementation_revision;

    Child(pid_t child_pid, RuntimeEpoch child_epoch, UniqueFd child_channel,
          std::string selected_product_identity,
          std::string selected_configuration_identity,
          std::string selected_implementation_revision)
        : pid(child_pid), runtime_epoch(std::move(child_epoch)),
          channel(std::move(child_channel)),
          product_identity(std::move(selected_product_identity)),
          configuration_identity(std::move(selected_configuration_identity)),
          implementation_revision(std::move(selected_implementation_revision)) {
    }

    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    Child(Child &&) noexcept = default;
    Child &operator=(Child &&) noexcept = default;
  };

  Impl(std::filesystem::path trusted_installed_target_root,
       const manyforge::deployment::IVerifiedRetainedBundleSource
           &selected_bundle_source,
       const manyforge::deployment::IVerifiedRetainedProductArtifactSource
           &selected_artifact_source,
       std::string selected_operating_system,
       std::string selected_launch_environment_contract)
      : target_root_path(std::move(trusted_installed_target_root)),
        bundle_source(selected_bundle_source),
        artifact_source(selected_artifact_source),
        operating_system(std::move(selected_operating_system)),
        launch_environment_contract(
            std::move(selected_launch_environment_contract)),
        observer_instance_id("native-observer-" + RandomHex()) {
    if (!target_root_path.is_absolute() || operating_system.empty() ||
        launch_environment_contract.empty()) {
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
  LaunchChild(const NativeExecutionRuntimeUnitAdmission &admission) {
    auto target = OpenInstalledTarget();
    auto channels = MakePacketPair();
    auto exec_pipe = MakePipe();
    SetNonBlocking(channels[0].get());
    const RuntimeEpoch runtime_epoch("native-runtime-" + RandomHex());
    const auto launch_arguments =
        admission.launch_arguments(kChildChannelFd, runtime_epoch);
    std::vector<std::string> arguments;
    arguments.reserve(launch_arguments.size() + 1U);
    arguments.emplace_back(contract::kInstalledTargetId);
    arguments.insert(arguments.end(), launch_arguments.begin(),
                     launch_arguments.end());
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
    return Child(pid, runtime_epoch, std::move(channels[0]),
                 admission.product().product_identity().value(),
                 admission.product().configuration_identity().value(),
                 admission.implementation_revision());
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
    static_cast<void>(bundle);
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
        return Presence(query, ProcessPresenceState::kPresent,
                        found->second.runtime_epoch.value());
      } else {
        children.erase(found);
      }
    }
    auto [inserted, unique] =
        children.emplace(runtime_id, LaunchChild(admission));
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

  std::filesystem::path target_root_path;
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

RetainedNativeRuntimeUnitAdapter::RetainedNativeRuntimeUnitAdapter(
    std::filesystem::path trusted_installed_target_root,
    const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string operating_system, std::string launch_environment_contract)
    : impl_(std::make_unique<Impl>(std::move(trusted_installed_target_root),
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

} // namespace manyforge::component_manager
