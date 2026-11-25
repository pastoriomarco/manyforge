#include "manyforge_component_manager/retained_execution_host_unit_adapter.hpp"

#include "kernel_apply_fence_rpc_internal.hpp"
#include "manager_liveness_rpc_internal.hpp"
#include "manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp"
#include "native_projection_rpc_internal.hpp"
#include "selected_runtime_composition_rpc_internal.hpp"
#include "manyforge_component_manager/execution_host_exit.hpp"
#include "manyforge_component_manager/site_settings.hpp"
#include <manyforge_core/process_diagnostics.hpp>
#include <manyforge_core/types/error_catalog.generated.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/apply_affected_scope.hpp>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <functional>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <limits>
#include <manyforge_component_manager/execution_host_unit_admission.hpp>
#include <manyforge_core/types/native_projection_capability.hpp>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

extern char **environ;

// Preserve native-named unit maps, launch, epochs, inventory, liveness and Stop plumbing.
namespace manyforge::component_manager {
namespace {

using manyforge::core::LogLine;
using manyforge::core::LogSeverity;
using manyforge::core::LogText;
using manyforge::core::RuntimeEpoch;
using manyforge::deployment::RetainedBundle;

using ProjectionBindingKey =
    std::pair<manyforge::core::CapabilityProviderBindingId,
              manyforge::core::CapabilityProviderBindingRevision>;

[[nodiscard]] bool SameProjectionBindingIdentity(
    const manyforge::core::ProjectionExecutionHostBindingDTO &lhs,
    const manyforge::core::ProjectionExecutionHostBindingDTO &rhs) noexcept {
  return lhs.provider_binding_id == rhs.provider_binding_id &&
         lhs.provider_binding_revision == rhs.provider_binding_revision &&
         lhs.runtime_unit_id == rhs.runtime_unit_id &&
         lhs.runtime_epoch == rhs.runtime_epoch;
}

constexpr int kChildChannelFd = 3;
constexpr int kChildLivenessChannelFd = 4;
constexpr int kChildExecErrorFd = 5;
constexpr int kChildPackageRootFd = 6;
constexpr int kChildExecutableFd = 7;
constexpr int kChildProgramArtifactRootFd = 8;
// A duplicate of the activation root: scene meshes are read from its
// raw-artifact store by content digest.
constexpr int kChildSceneContentRootFd = 9;
constexpr int kChildHighestReservedFd = kChildSceneContentRootFd;
constexpr std::size_t kMaximumControlPacketBytes =
    detail::kMaximumManagedRuntimeRpcPacketBytes;
static_assert(detail::kMaximumNativeProjectionRpcPacketBytes <=
              kMaximumControlPacketBytes);
static_assert(detail::kMaximumManagerLivenessRpcPacketBytes <=
              kMaximumControlPacketBytes);
static_assert(detail::kMaximumSelectedRuntimeCompositionRpcPacketBytes <=
              kMaximumControlPacketBytes);
// The pause before StopChildNoThrow retries a stop that failed (a defect);
// not a limit on anything.
constexpr auto kStuckStopRetryPause = std::chrono::seconds(2);

[[noreturn]] void Fail(const std::string &detail) {
  throw RetainedExecutionHostUnitAdapterError(detail);
}

[[nodiscard]] std::string ErrnoDetail(const std::string_view operation,
                                      const int error) {
  return std::string(operation) + ": " + std::strerror(error);
}

[[nodiscard]] std::optional<int>
DescriptorMintedRootFd(const std::filesystem::path &path) {
  const auto text = path.generic_string();
  constexpr std::string_view prefix = "/proc/self/fd/";
  if (!text.starts_with(prefix) || text.size() == prefix.size() ||
      (text[prefix.size()] == '0' && text.size() != prefix.size() + 1U)) {
    return std::nullopt;
  }
  unsigned int value = 0U;
  const auto parsed = std::from_chars(text.data() + prefix.size(),
                                      text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value == 0U || value > static_cast<unsigned int>(INT_MAX)) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

[[nodiscard]] std::filesystem::path
NormalizedPlainDirectory(std::filesystem::path path,
                         const std::string_view label) {
  if (!path.is_absolute()) {
    Fail(std::string(label) + " must be an absolute directory capability");
  }
  if (path.lexically_normal() != path) {
    Fail(std::string(label) + " must use its canonical lexical spelling");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
  if (!error) {
    struct stat descriptor_status {};
    if (const auto descriptor = DescriptorMintedRootFd(path);
        descriptor.has_value()) {
      if (::fstat(*descriptor, &descriptor_status) == 0 &&
          S_ISDIR(descriptor_status.st_mode)) {
        return path;
      }
    }
  }
  if (error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    Fail(std::string(label) + " is not one existing plain directory");
  }
  const auto canonical = std::filesystem::canonical(path, error);
  if (error || canonical != path) {
    Fail(std::string(label) + " is not one canonical directory capability");
  }
  return path;
}

void RequirePrivateDirectory(const std::filesystem::path &path,
                             const std::string_view label) {
  struct stat before {};
  if (::lstat(path.c_str(), &before) != 0 || !S_ISDIR(before.st_mode) ||
      S_ISLNK(before.st_mode)) {
    Fail(std::string(label) + " is not one plain directory");
  }
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
  if (!valid) {
    Fail(std::string(label) + " identity, owner, or mode is unsafe");
  }
}

void RequireDisjointDirectories(const std::filesystem::path &left,
                                const std::filesystem::path &right) {
  std::error_code error;
  const auto canonical_left = std::filesystem::canonical(left, error);
  if (error) {
    Fail("native execution state/run capability is unavailable");
  }
  const auto canonical_right = std::filesystem::canonical(right, error);
  if (error) {
    Fail("native execution state/run capability is unavailable");
  }
  const auto mismatch =
      std::mismatch(canonical_left.begin(), canonical_left.end(),
                    canonical_right.begin(), canonical_right.end());
  if (mismatch.first == canonical_left.end() ||
      mismatch.second == canonical_right.end()) {
    Fail("native execution state/run capabilities must be disjoint directory "
         "trees");
  }
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

[[nodiscard]] UniqueFd
OpenPinnedPackageRoot(const std::filesystem::path &path) {
  if (const auto descriptor = DescriptorMintedRootFd(path);
      descriptor.has_value()) {
    struct stat status {};
    if (::fstat(*descriptor, &status) != 0 || !S_ISDIR(status.st_mode)) {
      Fail("trusted retained-package descriptor is not a directory");
    }
    const int duplicate =
        ::fcntl(*descriptor, F_DUPFD_CLOEXEC, kChildPackageRootFd + 1);
    if (duplicate < 0) {
      Fail(ErrnoDetail("could not duplicate trusted retained-package root",
                       errno));
    }
    return UniqueFd(duplicate);
  }
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    Fail(ErrnoDetail("could not open trusted retained-package root", errno));
  }
  return UniqueFd(descriptor);
}

[[nodiscard]] std::array<UniqueFd, 2U> MakePacketPair() {
  int descriptors[2]{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors) !=
      0) {
    Fail(ErrnoDetail("could not create execution host private channel", errno));
  }
  const int packet_bytes =
      static_cast<int>(detail::kMaximumManagedRuntimeRpcPacketBytes);
  for (const auto descriptor : descriptors) {
    if (::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &packet_bytes,
                     sizeof(packet_bytes)) != 0 ||
        ::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &packet_bytes,
                     sizeof(packet_bytes)) != 0) {
      const auto error = errno;
      ::close(descriptors[0]);
      ::close(descriptors[1]);
      Fail(ErrnoDetail("could not configure execution host packet buffers",
                       error));
    }
    int send_bytes = 0;
    int receive_bytes = 0;
    socklen_t option_size = sizeof(int);
    if (::getsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &send_bytes,
                     &option_size) != 0 ||
        option_size != sizeof(int) ||
        ::getsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &receive_bytes,
                     &option_size) != 0 ||
        option_size != sizeof(int) || send_bytes < packet_bytes ||
        receive_bytes < packet_bytes) {
      ::close(descriptors[0]);
      ::close(descriptors[1]);
      Fail("execution host packet buffers are below the atomic ceiling");
    }
  }
  return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}

[[nodiscard]] std::array<UniqueFd, 2U> MakePipe() {
  int descriptors[2]{-1, -1};
  if (::pipe2(descriptors, O_CLOEXEC) != 0) {
    Fail(ErrnoDetail("could not create execution host exec pipe", errno));
  }
  return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}

void SetNonBlocking(const int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    Fail(ErrnoDetail("could not make execution host channel nonblocking",
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
      Fail(ErrnoDetail("could not mint execution host identity", errno));
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

/// Writes the Manager-validated site document (`manyforge.site.v2`) into one
/// fresh private run-root snapshot. It is retained with the run's evidence and
/// never rewritten, so a launched child always reads the bytes it was given.
[[nodiscard]] std::filesystem::path
CaptureSiteSettings(const std::filesystem::path &run_root,
                    const std::string_view document) {
  const auto path = run_root / ("site-settings-" + RandomHex() + ".json");
  UniqueFd file(::open(path.c_str(),
                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                       S_IRUSR | S_IWUSR));
  if (!file) {
    Fail(ErrnoDetail("could not create site settings snapshot " + path.string(),
                     errno));
  }
  std::size_t offset = 0U;
  while (offset < document.size()) {
    const auto count = ::write(file.get(), document.data() + offset,
                               document.size() - offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      Fail(ErrnoDetail("could not write site settings snapshot " + path.string(),
                       count == 0 ? EIO : errno));
    }
    offset += static_cast<std::size_t>(count);
  }
  return path;
}

// Sends one packet when the channel takes it now; false when it is full.
// Never waits (OD-290 (a)).
[[nodiscard]] bool TrySendPacket(const int fd, const std::string_view bytes) {
  if (bytes.empty() || bytes.size() > kMaximumControlPacketBytes) {
    Fail("execution host control packet is empty or oversized");
  }
  ssize_t count = -1;
  do {
    count = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
  } while (count < 0 && errno == EINTR);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return false;
  }
  if (count < 0) {
    Fail(ErrnoDetail("execution host control write failed", errno));
  }
  if (count != static_cast<ssize_t>(bytes.size())) {
    Fail("execution host control packet was not sent atomically");
  }
  return true;
}

// Receives one packet when one is waiting; never waits. A closed channel
// fails.
[[nodiscard]] std::optional<std::string> TryReceivePacket(const int fd) {
  std::string bytes(kMaximumControlPacketBytes, '\0');
  iovec vector{bytes.data(), bytes.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  ssize_t count = -1;
  do {
    count = ::recvmsg(fd, &message, MSG_DONTWAIT);
  } while (count < 0 && errno == EINTR);
  if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return std::nullopt;
  }
  if (count < 0) {
    Fail(ErrnoDetail("execution host control read failed", errno));
  }
  if (count == 0) {
    Fail("execution host child closed its private channel");
  }
  if ((message.msg_flags & MSG_TRUNC) != 0) {
    Fail("execution host control response exceeds its byte bound");
  }
  bytes.resize(static_cast<std::size_t>(count));
  return bytes;
}

// How one wait on the execution host ended.
enum class HostWaitEnd { kReady, kExited, kAbandoned };

// OD-290 (a): waits with no time limit until `fd` shows `events` (or hangs
// up), the host exits (`exit_fd`, its pidfd) or the application stops
// (`abandon_fd`). A negative `fd` waits for the exit or the stop only.
[[nodiscard]] HostWaitEnd WaitOnHost(const int fd, const short events,
                                     const int exit_fd, const int abandon_fd) {
  std::array<pollfd, 3U> descriptors{{{fd, events, 0},
                                      {exit_fd, POLLIN, 0},
                                      {abandon_fd, POLLIN, 0}}};
  for (;;) {
    int ready = -1;
    do {
      ready = ::poll(descriptors.data(), descriptors.size(), -1);
    } while (ready < 0 && errno == EINTR);
    if (ready < 0) Fail(ErrnoDetail("execution host wait failed", errno));
    if ((descriptors[0].revents & POLLNVAL) != 0)
      Fail("execution host private channel became invalid");
    if ((descriptors[0].revents & (events | POLLHUP | POLLERR)) != 0)
      return HostWaitEnd::kReady;
    if ((descriptors[1].revents & POLLIN) != 0) return HostWaitEnd::kExited;
    if ((descriptors[2].revents & POLLIN) != 0) return HostWaitEnd::kAbandoned;
  }
}

// Sends one packet, waiting for room with no time limit. A failed channel
// does not end the wait: only the host's exit or the stop does then, as in
// ReceiveOnHost.
[[nodiscard]] HostWaitEnd SendOnHost(const int fd, const std::string_view bytes,
                                     const int exit_fd, const int abandon_fd) {
  for (;;) {
    bool sent = false;
    try {
      sent = TrySendPacket(fd, bytes);
    } catch (const RetainedExecutionHostUnitAdapterError &) {
      if (bytes.empty() || bytes.size() > kMaximumControlPacketBytes) throw;
      return WaitOnHost(-1, 0, exit_fd, abandon_fd);
    }
    if (sent) return HostWaitEnd::kReady;
    const auto end = WaitOnHost(fd, POLLOUT, exit_fd, abandon_fd);
    if (end != HostWaitEnd::kReady) return end;
  }
}

// Receives one packet, waiting with no time limit. A closed channel does not
// end the wait: only the host's exit or the stop does then.
[[nodiscard]] HostWaitEnd ReceiveOnHost(const int fd, const int exit_fd,
                                        const int abandon_fd,
                                        std::string &packet) {
  int channel = fd;
  for (;;) {
    const auto end = WaitOnHost(channel, POLLIN, exit_fd, abandon_fd);
    if (end != HostWaitEnd::kReady) return end;
    try {
      if (auto received = TryReceivePacket(fd)) {
        packet = std::move(*received);
        return HostWaitEnd::kReady;
      }
    } catch (const RetainedExecutionHostUnitAdapterError &) {
      channel = -1;
    }
  }
}

// Diagnostic identity of one exchange (log text only; never on a Stop path).
[[nodiscard]] std::pair<std::string_view, std::string> AdmissionIdentity(
    const detail::KernelCellAdmissionRpcPacket &packet) {
  using manyforge::core::RequestKeyText;
  if (const auto *intent = std::get_if<manyforge::core::CellAdmissionClaimIntentDTO>(&packet.request))
    return {"request_key", RequestKeyText(intent->request_key)};
  if (const auto *release = std::get_if<manyforge::core::CellAdmissionClaimReleaseDTO>(&packet.request))
    return {"request_key", RequestKeyText(release->request_key)};
  if (const auto *apply = std::get_if<detail::ManagerApplyClaimAcquireRequest>(&packet.request))
    return {"request_key", RequestKeyText(apply->intent.request_key)};
  if (const auto *apply = std::get_if<detail::ManagerApplyClaimReleaseRequest>(&packet.request))
    return {"request_key", RequestKeyText(apply->release.request_key)};
  if (const auto *query = std::get_if<manyforge::core::CellAdmissionRecoveryReceiptQueryDTO>(&packet.request))
    return {"request_key", RequestKeyText(query->intent.request_key)};
  if (const auto *reset = std::get_if<manyforge::core::ProgramResetSceneAuthorizationDTO>(&packet.request)) {
    if (reset->reset_claim.has_value())
      return {"request_key", RequestKeyText(reset->reset_claim->request_key)};
    return {"program_run_id", std::to_string(reset->program_run_id.value())};
  }
  return {"none", {}};
}

template <typename Request>
[[nodiscard]] std::pair<std::string_view, std::string>
FenceIdentity(const Request &request) {
  using namespace manyforge::core;
  if constexpr (std::is_same_v<Request, ApplyConfigurationFenceIntentDTO>)
    return {"apply_id", std::to_string(request.owner_transaction.apply_id.value())};
  else if constexpr (std::is_same_v<Request, ApplyFenceAcquireAttemptQueryDTO>)
    return {"apply_id", std::to_string(request.persisted_intent.owner_transaction.apply_id.value())};
  else if constexpr (std::is_same_v<Request, ConfigurationFenceStatusQueryDTO>)
    return {"apply_id", std::to_string(request.owner_transaction.apply_id.value())};
  else if constexpr (std::is_same_v<Request, ConfigurationFenceReleaseDTO>)
    return {"apply_id", std::to_string(request.reconciliation_evidence.owner_transaction.apply_id.value())};
  else
    return {"none", {}};
}

// Rate limit slow-exchange records independently per command.
[[nodiscard]] bool AllowSlowKernelLog(const std::string_view command,
                                      const std::chrono::steady_clock::time_point now) noexcept {
  static constexpr std::array<std::string_view, 14> kinds{
      "cell_admission_get", "cell_admission_update_readiness",
      "cell_admission_publish_liveness", "cell_admission_acquire",
      "cell_admission_release",
      "cell_admission_recovery_receipt", "cell_admission_validate_program_reset_scene",
      "fence_observe_admission", "fence_acquire", "fence_reconcile_attempt",
      "fence_query_status", "fence_release",
      "status", "host_apply"};
  static std::array<std::atomic<std::int64_t>, kinds.size() + 1U> last{};
  const auto found = std::find(kinds.begin(), kinds.end(), command);
  const auto index = static_cast<std::size_t>(found - kinds.begin());
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
  auto previous = last[index].load(std::memory_order_relaxed);
  while (previous == 0 || ns - previous >= 1000000000LL) {
    if (last[index].compare_exchange_weak(previous, ns, std::memory_order_relaxed))
      return true;
  }
  return false;
}

// The management exchanges with the execution host: the projection
// operations in request order, then the fence, cell-admission, Host Apply,
// status, process-inspection and liveness-lane exchanges by their names.
constexpr std::array<std::string_view, 27U> kExchangeKinds{
    "projection_prepare", "projection_health", "projection_transaction",
    "projection_rebuild", "projection_bootstrap_rebuild",
    "projection_bootstrap_status", "projection_retain", "projection_release",
    "projection_scene_frame", "projection_effective_policy",
    "projection_commit_preflight", "fence_observe_admission", "fence_acquire", "fence_reconcile_attempt",
    "fence_query_status", "fence_release",
    "cell_admission_get", "cell_admission_acquire", "cell_admission_release",
    "cell_admission_recovery_receipt",
    "cell_admission_validate_program_reset_scene", "host_apply", "status",
    "component_process_inspection", "cell_admission_update_readiness",
    "cell_admission_publish_liveness", "manager_liveness_renewal"};
static_assert(std::variant_size_v<detail::NativeProjectionRpcRequest> == 11U);
constexpr std::uint8_t kStatusKind = 22U;
static_assert(kExchangeKinds[kStatusKind] == "status");
constexpr std::uint8_t kInspectionKind = 23U;
static_assert(kExchangeKinds[kInspectionKind] ==
              "component_process_inspection");
constexpr std::uint8_t kLivenessRenewalKind = 26U;
static_assert(kExchangeKinds[kLivenessRenewalKind] ==
              "manager_liveness_renewal");

[[nodiscard]] std::uint8_t ExchangeKind(const std::string_view name) {
  const auto found =
      std::find(kExchangeKinds.begin(), kExchangeKinds.end(), name);
  if (found == kExchangeKinds.end())
    Fail("management exchange kind is unknown");
  return static_cast<std::uint8_t>(found - kExchangeKinds.begin());
}

// One slow exchange's diagnostic record (log text only). These protocols do
// not have a universal per-request ID.
void ReportSlowExchange(const std::string_view command, const pid_t pid,
                        const std::string_view id_type,
                        const std::string_view identity,
                        const std::chrono::steady_clock::time_point start) {
  const auto now = std::chrono::steady_clock::now();
  const auto elapsed = now - start;
  if (elapsed <= std::chrono::milliseconds(200) || !AllowSlowKernelLog(command, now))
    return;
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
  char id[129]{};
  const auto length = std::min(identity.size(), sizeof(id) - 1U);
  for (std::size_t i = 0; i < length; ++i) {
    const auto c = static_cast<unsigned char>(identity[i]);
    id[i] = c >= 33U && c <= 126U && c != '=' ? static_cast<char>(c) : '_';
  }
  const auto start_us = std::chrono::duration_cast<std::chrono::microseconds>(start.time_since_epoch()).count();
  const auto end_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
  LogLine(LogSeverity::kWarning, LogText("kernel rpc timing: command=", command, " pid=",
                      static_cast<long>(pid), " id_type=", id_type, " id=",
                      length ? id : "none", " start_us=",
                      static_cast<long long>(start_us), " end_us=",
                      static_cast<long long>(end_us), " elapsed_us=",
                      static_cast<long long>(us)));
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
    Fail("execution host candidate bundle lacks the queried runtime unit");
  }
  return *found;
}

[[nodiscard]] std::set<ProjectionBindingKey> NativeProjectionBindingsForRuntime(
    const RetainedBundle &bundle,
    const manyforge::core::RuntimeUnitId &runtime_unit_id) {
  std::set<ProjectionBindingKey> result;
  for (const auto &binding : bundle.value().capability_provider_bindings) {
    if (!manyforge::core::RequiresNativeSceneProjection(
            binding.capability_id, binding.capability_definition_revision)) {
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
    const ExecutionHostUnitAdmission &admission,
    const RetainedBundle &bundle, const RuntimeUnitConvergenceQueryDTO &query) {
  const auto &product = admission.product();
  const auto &plan = query.plan;
  const auto &runtime_unit = FindRuntimeUnit(bundle, plan.runtime_unit_id);
  const auto requirement =
      std::find_if(bundle.value().runtime_contract_requirements.begin(),
                   bundle.value().runtime_contract_requirements.end(),
                   [&](const auto &candidate) {
                     return candidate.runtime_unit_id == plan.runtime_unit_id;
                   });
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
      (!plan.runtime_contract_offer_digest.empty() &&
       (requirement == bundle.value().runtime_contract_requirements.end() ||
        plan.runtime_contract_requirement != requirement->requirement)) ||
      (!plan.runtime_contract_offer_digest.empty() &&
       admission.runtime_contract_offer_digest() !=
           plan.runtime_contract_offer_digest.value()) ||
      !plan.launchable_content_digest.has_value() ||
      product.launchable_artifact_ref().content_digest !=
          *plan.launchable_content_digest ||
      product.launchable_artifact_ref().byte_size !=
          plan.launchable_byte_size ||
      product.operating_system() != plan.operating_system ||
      product.launch_environment_contract() != plan.shell_contract) {
    Fail("verified execution runtime admission differs from the exact "
         "convergence query");
  }
}

[[nodiscard]] bool
SameChildProduct(const ExecutionHostUnitAdmission &admission,
                 const std::string &product_identity,
                 const std::string &configuration_identity,
                 const std::string &runtime_contract_offer_digest) {
  return admission.product().product_identity().value() == product_identity &&
         admission.product().configuration_identity().value() ==
             configuration_identity &&
         admission.runtime_contract_offer_digest() ==
             runtime_contract_offer_digest;
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
                             const int liveness_channel_fd, const int error_fd,
                             const int executable_fd, const int package_root_fd,
                             const int program_artifact_root_fd,
                             const int scene_content_root_fd,
                             const std::uint64_t core_dump_limit_bytes,
                             char *const argv[]) noexcept {
  if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
    ChildExit(error_fd, errno);
  }
  if (::getppid() != expected_parent) {
    ChildExit(error_fd, ECHILD);
  }
  // The core-file size limit's soft value is the site's (0, core dumps off,
  // by default), at most the hard limit, which is kept.
  struct rlimit core_limit {};
  if (::getrlimit(RLIMIT_CORE, &core_limit) != 0) {
    ChildExit(error_fd, errno);
  }
  core_limit.rlim_cur =
      std::min(static_cast<rlim_t>(core_dump_limit_bytes), core_limit.rlim_max);
  if (::setrlimit(RLIMIT_CORE, &core_limit) != 0) {
    ChildExit(error_fd, errno);
  }
  const int channel_copy =
      ::fcntl(channel_fd, F_DUPFD_CLOEXEC, kChildHighestReservedFd + 1);
  const int liveness_channel_copy = ::fcntl(
      liveness_channel_fd, F_DUPFD_CLOEXEC, kChildHighestReservedFd + 1);
  const int error_copy =
      ::fcntl(error_fd, F_DUPFD_CLOEXEC, kChildHighestReservedFd + 1);
  const int executable_copy =
      ::fcntl(executable_fd, F_DUPFD_CLOEXEC, kChildHighestReservedFd + 1);
  const int package_root_copy =
      ::fcntl(package_root_fd, F_DUPFD_CLOEXEC, kChildHighestReservedFd + 1);
  const int program_artifact_root_copy = ::fcntl(
      program_artifact_root_fd, F_DUPFD_CLOEXEC, kChildHighestReservedFd + 1);
  // No activation root: FD 9 stays closed (close_range below).
  const int scene_content_root_copy =
      scene_content_root_fd < 0
          ? -1
          : ::fcntl(scene_content_root_fd, F_DUPFD_CLOEXEC,
                    kChildHighestReservedFd + 1);
  if (channel_copy < 0 || liveness_channel_copy < 0 || error_copy < 0 ||
      executable_copy < 0 || package_root_copy < 0 ||
      program_artifact_root_copy < 0 ||
      (scene_content_root_fd >= 0 && scene_content_root_copy < 0) ||
      ::dup3(channel_copy, kChildChannelFd, 0) < 0 ||
      ::dup3(liveness_channel_copy, kChildLivenessChannelFd, 0) < 0 ||
      ::dup3(error_copy, kChildExecErrorFd, O_CLOEXEC) < 0 ||
      ::dup3(executable_copy, kChildExecutableFd, O_CLOEXEC) < 0 ||
      ::dup3(package_root_copy, kChildPackageRootFd, 0) < 0 ||
      ::dup3(program_artifact_root_copy, kChildProgramArtifactRootFd, 0) < 0 ||
      (scene_content_root_copy >= 0 &&
       ::dup3(scene_content_root_copy, kChildSceneContentRootFd, 0) < 0)) {
    ChildExit(error_fd, errno);
  }
  if (scene_content_root_copy < 0) {
    static_cast<void>(::close(kChildSceneContentRootFd));
  }
#ifdef SYS_close_range
  const long closed =
      ::syscall(SYS_close_range, kChildHighestReservedFd + 1U, UINT_MAX, 0U);
  if (closed != 0 && errno != ENOSYS) {
    ChildExit(kChildExecErrorFd, errno);
  }
  if (closed != 0) {
#endif
    const long maximum = ::sysconf(_SC_OPEN_MAX);
    const int upper =
        maximum > 0 && maximum < INT_MAX ? static_cast<int>(maximum) : 65536;
    for (int descriptor = kChildHighestReservedFd + 1; descriptor < upper;
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

// OD-290 (a): the exec pipe closes when the child execs (close-on-exec) or
// exits, or carries the child's exec errno; no time limit. The application's
// stop ends the wait (`application_stopping`).
void ConfirmExecOrThrow(const pid_t pid, const int error_fd,
                        const int abandon_fd) {
  if (WaitOnHost(error_fd, POLLIN, -1, abandon_fd) ==
      HostWaitEnd::kAbandoned) {
    KillAndReap(pid);
    throw manyforge::core::TypedError(
        manyforge::core::ErrorDTO{
            std::string(kApplicationStoppingCode),
            "the application is stopping; ManyForge stopped the execution "
            "host's launch",
            {{"exchange", "launch"}}},
        "component_manager");
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

struct RetainedExecutionHostUnitAdapter::Impl final
    : public std::enable_shared_from_this<
          RetainedExecutionHostUnitAdapter::Impl> {
  class ProjectionClient;

  // True when a packet read later is this exchange's reply (what it echoes,
  // L-1).
  using EchoMatch = std::function<bool(const std::string &)>;

  // A host channel's one outstanding exchange: a request whose reply has not
  // been read. Nothing new is sent on the channel until its reply is
  // consumed (OD-121 (4), OD-295 (2)(a)), except a request sent behind an
  // unanswered status (OD-290 (a)). Nothing closes the channel or stops the
  // host because a reply is outstanding.
  struct PendingExchange final {
    EchoMatch match;
    // Index into kExchangeKinds.
    std::uint8_t kind{};
    // When the request was sent, and its number (a new number is a new
    // outstanding exchange).
    std::chrono::steady_clock::time_point since{};
    std::uint64_t sequence{};
    // A caller waits for this reply with no adapter lock held; nothing
    // else reads the channel meanwhile.
    bool awaited{};
    // A consumed malformed or foreign reply, or a failed channel, cannot
    // restore correlation; the exchange then stays unsettled until the
    // host's owned shutdown.
    bool invalid_response{};
  };

  // OD-297: one request/reply channel to the execution host (its management
  // or its liveness channel) with its one outstanding exchange. The state is
  // guarded by the channel's lock: `mutex` for the management channel,
  // `liveness_mutex` for the liveness channel (its own lane, L-5). `fd` is
  // fixed and stays open while anything holds the child.
  struct HostChannel final {
    explicit HostChannel(UniqueFd channel_fd) : fd(std::move(channel_fd)) {}
    UniqueFd fd;
    std::optional<PendingExchange> pending;
    // A status reply read before the Manager's observation consumed it, and
    // the number of the status request it answers (management channel).
    std::optional<std::string> status_reply;
    std::uint64_t status_reply_sequence{};
  };

  // The execution host child, held by shared_ptr: an exchange keeps it, and
  // so its descriptors, for its wait. Fields other than the channels are
  // guarded by `mutex`.
  struct Child final {
    pid_t pid;
    // The host's pidfd, opened once at launch: every wait on either channel
    // ends on the host's exit through it (a closed channel alone does not
    // make the exit waitable yet).
    UniqueFd exit_fd;
    // Set by whichever stop or observation path consumed this child's exit
    // status. A reaped PID is no longer this adapter's child and may already
    // name an unrelated process, so it is never waited on again; the stale
    // registry row is observed as absent and erased by its next observer.
    bool reaped{};
    std::optional<int> exit_status;
    bool shutdown_requested{};
    // OD-187 (1): ended by SIGKILL after a second stop request.
    bool force_killed{};
    bool shutdown_acknowledged{};
    bool shutdown_progress_pending{};
    RuntimeEpoch runtime_epoch;
    HostChannel management;
    HostChannel liveness;
    std::string product_identity;
    std::string configuration_identity;
    std::string runtime_contract_offer_digest;
    std::set<ProjectionBindingKey> admitted_projection_bindings;
    bool shutdown_sent{};

    Child(pid_t child_pid, UniqueFd child_exit_fd, RuntimeEpoch child_epoch,
          UniqueFd management_channel, UniqueFd liveness_channel,
          std::string selected_product_identity,
          std::string selected_configuration_identity,
          std::string selected_runtime_contract_offer_digest)
        : pid(child_pid), exit_fd(std::move(child_exit_fd)),
          runtime_epoch(std::move(child_epoch)),
          management(std::move(management_channel)),
          liveness(std::move(liveness_channel)),
          product_identity(std::move(selected_product_identity)),
          configuration_identity(std::move(selected_configuration_identity)),
          runtime_contract_offer_digest(
              std::move(selected_runtime_contract_offer_digest)) {}

    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
  };

  struct AdmissionCacheEntry final {
    RuntimeUnitConvergenceQueryDTO query;
    std::shared_ptr<const RetainedBundle> bundle;
    ExecutionHostUnitAdmission admission;
  };

  Impl(const manyforge::core::ApplicationStartId application_start_id,
       std::filesystem::path trusted_installed_target_root,
       std::filesystem::path trusted_package_root,
       std::filesystem::path manager_run_root,
       std::filesystem::path execution_state_root,
       std::filesystem::path execution_run_root,
       const manyforge::deployment::IVerifiedRetainedBundleSource
           &selected_bundle_source,
       const manyforge::deployment::IVerifiedRetainedProductArtifactSource
           &selected_artifact_source,
       std::string selected_operating_system,
       std::vector<std::string> selected_launch_environment_contracts,
       std::filesystem::path activation_root,
       std::optional<std::string> site_settings_document)
      : target_root_path(
            NormalizedPlainDirectory(std::move(trusted_installed_target_root),
                                     "trusted installed-target root")),
        package_root_path(NormalizedPlainDirectory(
            std::move(trusted_package_root), "trusted retained-package root")),
        package_root_fd(OpenPinnedPackageRoot(package_root_path)),
        activation_root_path(
            activation_root.empty()
                ? std::filesystem::path{}
                : NormalizedPlainDirectory(std::move(activation_root),
                                           "candidate retained-package root")),
        manager_run_root_path(NormalizedPlainDirectory(
            std::move(manager_run_root), "Manager run root")),
        execution_state_root_path(NormalizedPlainDirectory(
            std::move(execution_state_root), "native execution state root")),
        execution_run_root_path(NormalizedPlainDirectory(
            std::move(execution_run_root), "native execution run root")),
        bundle_source(selected_bundle_source),
        artifact_source(selected_artifact_source),
        operating_system(std::move(selected_operating_system)),
        launch_environment_contracts(
            std::move(selected_launch_environment_contracts)),
        site_settings_document(std::move(site_settings_document)),
        core_dump_limit_bytes(
            CoreDumpLimitBytesOfDocument(this->site_settings_document)),
        observer_instance_id("native-observer-" + RandomHex()),
        start_epoch([&] {
          const auto text = FormatApplicationStartId(application_start_id);
          return RuntimeEpoch(std::string(text.data(), text.size()));
        }()) {
    if (this->site_settings_document.has_value() &&
        this->site_settings_document->find('\0') != std::string::npos) {
      Fail("site settings document cannot contain NUL");
    }
    const std::array<const std::filesystem::path *, 3U> typed_roots{
        &manager_run_root_path, &execution_state_root_path,
        &execution_run_root_path};
    RequirePrivateDirectory(manager_run_root_path, "Manager run root");
    RequirePrivateDirectory(execution_state_root_path,
                            "native execution state root");
    RequirePrivateDirectory(execution_run_root_path,
                            "native execution run root");
    for (std::size_t left = 0U; left < typed_roots.size(); ++left) {
      for (std::size_t right = left + 1U; right < typed_roots.size(); ++right) {
        RequireDisjointDirectories(*typed_roots[left], *typed_roots[right]);
      }
    }
    for (const auto *typed_root : typed_roots) {
      RequireDisjointDirectories(target_root_path, *typed_root);
      RequireDisjointDirectories(package_root_path, *typed_root);
      if (!activation_root_path.empty()) {
        RequireDisjointDirectories(activation_root_path, *typed_root);
      }
    }
    if (!activation_root_path.empty()) {
      RequireDisjointDirectories(package_root_path,
                                 activation_root_path);
      activation_root_fd =
          OpenPinnedPackageRoot(activation_root_path);
    }
    std::sort(launch_environment_contracts.begin(),
              launch_environment_contracts.end());
    if (operating_system.empty() || launch_environment_contracts.empty() ||
        std::any_of(launch_environment_contracts.begin(),
                    launch_environment_contracts.end(),
                    [](const auto &value) { return value.empty(); }) ||
        std::adjacent_find(launch_environment_contracts.begin(),
                           launch_environment_contracts.end()) !=
            launch_environment_contracts.end()) {
      throw std::invalid_argument(
          "retained execution adapter construction input is invalid");
    }
    const int fd = ::open(target_root_path.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      Fail(ErrnoDetail("could not open trusted installed-target root", errno));
    }
    target_root_fd.Reset(fd);
    if (!abandon_fd) {
      Fail("could not create the execution host exchange abandon signal");
    }
    struct stat status {};
    if (::fstat(target_root_fd.get(), &status) != 0 ||
        !S_ISDIR(status.st_mode)) {
      Fail("trusted installed-target root is not a directory");
    }
    if (this->site_settings_document.has_value()) {
      site_settings_snapshot = CaptureSiteSettings(
          manager_run_root_path, *this->site_settings_document);
    }
  }

  // OD-308 (a): the hosts are stopped with no lock held; each unit is busy
  // `stopping` meanwhile, and erased afterwards.
  ~Impl() {
    std::map<std::string, std::shared_ptr<Child>> stopping;
    {
      std::lock_guard lock(mutex);
      for (const auto &[runtime_id, child] : children)
        busy_units.insert_or_assign(runtime_id, "stopping");
      stopping = children;
    }
    for (const auto &[runtime_id, child] : stopping) {
      StopChildNoThrow(*child);
      EraseLivenessEndpoint(runtime_id);
    }
    std::lock_guard lock(mutex);
    for (const auto &[runtime_id, child] : stopping) {
      children.erase(runtime_id);
      busy_units.erase(runtime_id);
    }
  }

  void EraseLivenessEndpoint(const std::string &runtime_id) {
    std::lock_guard lock(liveness_mutex);
    liveness_endpoints.erase(runtime_id);
  }

  void EraseChild(
      const std::map<std::string, std::shared_ptr<Child>>::iterator child) {
    EraseLivenessEndpoint(child->first);
    children.erase(child);
  }

  [[nodiscard]] static manyforge::core::ErrorDTO
  BusyError(const std::string &runtime_unit_id, const std::string_view phase) {
    const std::string_view activity =
        phase == "launching"    ? "being launched"
        : phase == "preparing"  ? "preparing its projections"
        : phase == "inspecting" ? "reading its component processes"
        : phase == "stopping"   ? "stopping"
                                : "rebuilding a projection";
    return {std::string(kRuntimeUnitLaunchInProgressCode),
            "the execution host is " + std::string(activity) +
                "; retry the call",
            {{"phase", std::string(phase)},
             {"runtime_unit_id", runtime_unit_id}}};
  }

  // Under `mutex`: the busy answer for one unit, if it is launching or
  // preparing.
  [[nodiscard]] std::optional<manyforge::core::ErrorDTO>
  BusyLocked(const std::string &runtime_unit_id) const {
    const auto found = busy_units.find(runtime_unit_id);
    if (found == busy_units.end()) return std::nullopt;
    return BusyError(found->first, found->second);
  }

  // Under `mutex`: the busy answer for calls addressed to the sole host.
  [[nodiscard]] std::optional<manyforge::core::ErrorDTO> AnyBusyLocked() const {
    if (busy_units.empty()) return std::nullopt;
    return BusyError(busy_units.begin()->first, busy_units.begin()->second);
  }

  // Under `mutex`: the unit's host is being stopped (OD-308 (a)). Its child
  // is the stopper's alone until the stop erases it, and still present.
  [[nodiscard]] bool IsStoppingLocked(const std::string &runtime_unit_id) const {
    const auto found = busy_units.find(runtime_unit_id);
    return found != busy_units.end() && found->second == "stopping";
  }

  void ThrowIfBusyLocked(const std::string &runtime_unit_id) const {
    if (auto busy = BusyLocked(runtime_unit_id))
      throw manyforge::core::TypedError(*busy, "component_manager");
  }

  // Ends one unit's busy phase; the stop waiting for it is woken.
  void EndBusyPhase(const std::string &runtime_unit_id) {
    {
      std::lock_guard lock(mutex);
      busy_units.erase(runtime_unit_id);
    }
    busy_changed.notify_all();
  }

  // Ends the busy phase begun under `mutex` when the exchange returns or
  // throws.
  class BusyPhase final {
  public:
    BusyPhase(Impl &adapter, std::string runtime_unit_id)
        : adapter_(adapter), runtime_unit_id_(std::move(runtime_unit_id)) {}
    ~BusyPhase() { adapter_.EndBusyPhase(runtime_unit_id_); }
    BusyPhase(const BusyPhase &) = delete;
    BusyPhase &operator=(const BusyPhase &) = delete;

  private:
    Impl &adapter_;
    std::string runtime_unit_id_;
  };

  [[nodiscard]] std::pair<std::shared_ptr<const RetainedBundle>,
                          ExecutionHostUnitAdmission>
  Admission(const RuntimeUnitConvergenceQueryDTO &query) {
    if (!query.valid()) {
      Fail("retained native adapter received an invalid convergence query");
    }
    std::lock_guard cache_lock(admission_mutex);
    const auto runtime_id = query.plan.runtime_unit_id.value();
    const auto cached = admission_cache.find(runtime_id);
    if (cached != admission_cache.end() && cached->second.query == query) {
      return {cached->second.bundle, cached->second.admission};
    }
    auto bundle =
        bundle_source.get_verified(query.candidate_retained_bundle_id);
    if (bundle == nullptr) {
      Fail("retained native adapter could not reload the candidate bundle");
    }
    auto admission = VerifyExecutionHostUnitForHost(
        *bundle, query.plan.runtime_unit_id, artifact_source, operating_system,
        launch_environment_contracts);
    if (!admission.has_value()) {
      Fail("retained native adapter query has no qualified native product");
    }
    VerifyAdmissionMatchesQuery(*admission, *bundle, query);
    auto [stored, inserted] = admission_cache.insert_or_assign(
        runtime_id,
        AdmissionCacheEntry{query, std::move(bundle), std::move(*admission)});
    static_cast<void>(inserted);
    return {stored->second.bundle, stored->second.admission};
  }

  [[nodiscard]] UniqueFd OpenInstalledTarget(
      const ExecutionHostUnitAdmission &admission) const {
    const int fd =
        ::openat(target_root_fd.get(), admission.installed_target_id().c_str(),
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
      Fail("execution host process observation sequence is exhausted");
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

  // A reaped child's channels stay open until the child is released: an
  // exchange may still hold them for its wait.
  [[nodiscard]] bool ChildIsLive(Child &child) {
    if (child.reaped) return false;
    int status = 0;
    pid_t result = -1;
    do {
      result = ::waitpid(child.pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
      return true;
    }
    if (result == child.pid) {
      host_exit_pid.store(child.pid, std::memory_order_relaxed);
      if (WIFEXITED(status)) {
        child.exit_status = WEXITSTATUS(status);
        host_exit_value.store(WEXITSTATUS(status), std::memory_order_relaxed);
        host_exited.store(true, std::memory_order_release);
        std::cerr << "execution host child exited: pid=" << child.pid
                  << " status=" << WEXITSTATUS(status) << '\n';
      } else if (WIFSIGNALED(status)) {
        host_exit_signaled.store(true, std::memory_order_relaxed);
        host_exit_value.store(WTERMSIG(status), std::memory_order_relaxed);
        host_exit_core_dumped.store(WCOREDUMP(status) != 0,
                                    std::memory_order_relaxed);
        host_exited.store(true, std::memory_order_release);
        std::cerr << "execution host child terminated: pid=" << child.pid
                  << " signal=" << WTERMSIG(status) << '\n';
      } else {
        std::cerr << "execution host child stopped unexpectedly: pid="
                  << child.pid << '\n';
      }
      child.reaped = true;
      return false;
    }
    if (result < 0 && errno == ECHILD) {
      Fail("tracked execution host is no longer a direct Manager child");
    }
    if (result < 0) {
      Fail(ErrnoDetail("could not observe execution host child", errno));
    }
    Fail("execution host child observation returned a foreign PID");
  }

  // The answer of a call that meets an outstanding exchange on the channel
  // (OD-290 (a)): nothing was sent, so it is refused before any effect
  // (`effect: none`, which the Apply journal reads as a known before-effect
  // failure). Nothing is halted and the host does not count as faulted.
  [[nodiscard]] static manyforge::core::ErrorDTO
  OutstandingError(const std::string_view exchange,
                   const std::chrono::steady_clock::time_point since) {
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - since);
    return {std::string(kExecutionHostLateCode),
            "the execution host has not answered an earlier " +
                std::string(exchange) +
                " request yet; this request was refused before it was sent",
            {{"exchange", std::string(exchange)},
             {"waited_ms", std::to_string(waited.count())},
             {"effect", "none"}}};
  }

  [[nodiscard]] static manyforge::core::ErrorDTO
  ExitedError(const std::string_view exchange) {
    return {std::string(kExecutionHostExitedCode),
            "the execution host exited during its " + std::string(exchange) +
                " exchange; ManyForge stops the application",
            {{"exchange", std::string(exchange)}}};
  }

  // The host's exit seen before a request was sent: the same typed
  // `execution_host_exited`, with `effect: none`.
  [[nodiscard]] static manyforge::core::TypedError
  ExitedBeforeSend(const std::string_view exchange) {
    return manyforge::core::TypedError(
        manyforge::core::ErrorDTO{
            std::string(kExecutionHostExitedCode),
            "the execution host exited before this " + std::string(exchange) +
                " request was sent; ManyForge stops the application",
            {{"exchange", std::string(exchange)}, {"effect", "none"}}},
        "component_manager");
  }

  // Under the channel's lock: records `channel`'s one outstanding exchange,
  // numbered and timed now. A status is numbered under `mutex`, the lock
  // ProgressRequestMark reads the number under (OD-295 (1)(a)).
  void MarkPending(HostChannel &channel, EchoMatch match,
                   const std::uint8_t kind, const bool awaited) {
    channel.pending.emplace(PendingExchange{
        std::move(match), kind, std::chrono::steady_clock::now(),
        exchange_sequence.fetch_add(1U, std::memory_order_relaxed) + 1U,
        awaited, false});
  }

  // Under the channel's lock: settle `host`'s `channel`'s one outstanding
  // exchange that no caller waits for (a status, or one the application's
  // stop abandoned) with a non-blocking receive; true when the channel is
  // free. A status reply is kept for the Manager's observation; another is
  // discarded.
  [[nodiscard]] static bool SettlePending(const Child &host,
                                          HostChannel &channel) {
    if (!channel.pending) return true;
    auto &pending = *channel.pending;
    if (pending.awaited || pending.invalid_response) return false;
    std::optional<std::string> packet;
    try {
      packet = TryReceivePacket(channel.fd.get());
      if (!packet) return false;
      if (!pending.match(*packet)) {
        pending.invalid_response = true;
        return false;
      }
    } catch (...) {
      pending.invalid_response = true;
      return false;
    }
    if (pending.kind == kStatusKind) {
      channel.status_reply = std::move(*packet);
      channel.status_reply_sequence = pending.sequence;
    } else if (pending.kind == kInspectionKind) {
      LogLine(LogSeverity::kInfo, LogText("component inspection late response settled: pid=",
                          host.pid, " epoch=", host.runtime_epoch.value()));
    } else {
      LogLine(LogSeverity::kInfo, LogText(&channel == &host.liveness ? "abandoned liveness"
                                                     : "abandoned exchange",
                          " reply settled: pid=", host.pid,
                          " exchange=", kExchangeKinds[pending.kind]));
    }
    channel.pending.reset();
    return true;
  }

  // The typed answer of a call that meets the channel's unsettled exchange:
  // the outstanding reply, or the application's stop (OD-270; nothing is
  // sent, `effect: none`).
  [[nodiscard]] static manyforge::core::ErrorDTO
  UnsettledError(const PendingExchange &pending, const bool stopping) {
    if (!stopping)
      return OutstandingError(kExchangeKinds[pending.kind], pending.since);
    auto error = StoppingError(kExchangeKinds[pending.kind]);
    error.details.emplace("effect", "none");
    return error;
  }

  // One poll of the abandon eventfd until it is seen written; then a memory
  // read (the eventfd is never read empty).
  [[nodiscard]] bool ExchangesAbandoned() const {
    if (abandoned.load(std::memory_order_acquire)) return true;
    pollfd descriptor{abandon_fd.get(), POLLIN, 0};
    int result = -1;
    do {
      result = ::poll(&descriptor, 1U, 0);
    } while (result < 0 && errno == EINTR);
    if (result < 0) Fail(ErrnoDetail("abandon signal poll failed", errno));
    if (result == 0) return false;
    abandoned.store(true, std::memory_order_release);
    return true;
  }

  [[nodiscard]] static manyforge::core::ErrorDTO
  StoppingError(const std::string_view exchange) {
    return {std::string(kApplicationStoppingCode),
            "the application is stopping; ManyForge stopped waiting for the "
            "execution host's " + std::string(exchange) +
                " answer, whose outcome is unknown",
            {{"exchange", std::string(exchange)}}};
  }

  // One request of an exchange on a host channel (OD-297).
  struct ChannelExchange final {
    // Held across the wait: the channel and the host's pidfd stay open.
    std::shared_ptr<Child> host;
    // `host`'s management or liveness channel.
    HostChannel *channel{};
    std::string_view request;
    EchoMatch match;
    std::uint8_t kind{};
    // Filled by ExchangeAll.
    bool behind_status{};
    std::uint64_t status_sequence{};
    std::chrono::steady_clock::time_point started{};
    bool sent{};
    bool received{};
    std::string status_reply{};
    std::string reply{};
  };

  // OD-297: the one exchange mechanism of both host channels, with no time
  // limit (OD-290 (a)). `lock` holds the channels' lock on entry (`mutex`
  // for the management channel, `liveness_mutex` for the liveness channel)
  // and is released on return. Each channel carries one exchange
  // (OD-295 (2)(a)): a call that meets an outstanding exchange is refused at
  // once with the typed `execution_host_late` (`application_stopping` once
  // the application stops), `effect: none`, and nothing is sent; a request
  // may be sent behind an unanswered status, whose reply is read first and
  // kept for the Manager's observation. Every request is then the awaited
  // exchange of its channel, so no other caller reads the channel, and the
  // wait holds no lock: all requests are sent, then all replies read. The
  // wait ends on the replies (each channel free again); on the host's exit
  // through its pidfd (reaped, typed `execution_host_exited`); or on
  // AbandonExchanges (typed `application_stopping`). A request sent without
  // its reply read stays its channel's pending exchange, so its late reply is
  // still recognised and settled; a failed channel leaves it unsettled.
  void ExchangeAll(std::unique_lock<std::mutex> &lock,
                   const std::span<ChannelExchange> exchanges) {
    const bool stopping = ExchangesAbandoned();
    for (auto &exchange : exchanges) {
      if (SettlePending(*exchange.host, *exchange.channel)) continue;
      const auto &earlier = *exchange.channel->pending;
      exchange.behind_status = earlier.kind == kStatusKind &&
                               !earlier.awaited &&
                               !earlier.invalid_response && !stopping;
      exchange.status_sequence = earlier.sequence;
      if (!exchange.behind_status)
        throw manyforge::core::TypedError(UnsettledError(earlier, stopping),
                                          "component_manager");
    }
    const auto name = kExchangeKinds[exchanges.front().kind];
    if (stopping) {
      auto error = StoppingError(name);
      error.details.emplace("effect", "none");
      throw manyforge::core::TypedError(std::move(error), "component_manager");
    }
    for (auto &exchange : exchanges) {
      MarkPending(*exchange.channel, std::move(exchange.match), exchange.kind,
                  true);
      exchange.started = exchange.channel->pending->since;
    }
    lock.unlock();
    auto end = HostWaitEnd::kReady;
    std::exception_ptr failure;
    try {
      for (auto &exchange : exchanges) {
        if (end != HostWaitEnd::kReady) break;
        end = SendOnHost(exchange.channel->fd.get(), exchange.request,
                         exchange.host->exit_fd.get(), abandon_fd.get());
        exchange.sent = end == HostWaitEnd::kReady;
      }
      for (auto &exchange : exchanges) {
        if (end != HostWaitEnd::kReady) break;
        const int channel = exchange.channel->fd.get();
        const int exit_fd = exchange.host->exit_fd.get();
        if (exchange.behind_status)
          end = ReceiveOnHost(channel, exit_fd, abandon_fd.get(),
                              exchange.status_reply);
        if (end == HostWaitEnd::kReady) {
          end = ReceiveOnHost(channel, exit_fd, abandon_fd.get(),
                              exchange.reply);
          exchange.received = end == HostWaitEnd::kReady;
        }
      }
    } catch (...) {
      failure = std::current_exception();
    }
    lock.lock();
    for (auto &exchange : exchanges) {
      auto &channel = *exchange.channel;
      channel.pending->awaited = false;
      if (!exchange.status_reply.empty()) {
        channel.status_reply = std::move(exchange.status_reply);
        channel.status_reply_sequence = exchange.status_sequence;
      }
      if (failure)
        channel.pending->invalid_response = true;
      else if (exchange.received || !exchange.sent)
        channel.pending.reset();
    }
    lock.unlock();
    busy_changed.notify_all();
    if (failure) std::rethrow_exception(failure);
    if (end == HostWaitEnd::kExited) {
      {
        // HostExit is set when the exchange is answered. A host not yet
        // registered is its launcher's, which reaps it as the launch fails;
        // a stopping host is its stopper's (OD-308 (a)).
        std::lock_guard reap(mutex);
        for (const auto &[runtime_id, child] : children)
          for (const auto &exchange : exchanges)
            if (!exchange.received && child == exchange.host &&
                !IsStoppingLocked(runtime_id))
              static_cast<void>(ChildIsLive(*child));
      }
      throw manyforge::core::TypedError(ExitedError(name),
                                        "component_manager");
    }
    if (end == HostWaitEnd::kAbandoned)
      throw manyforge::core::TypedError(StoppingError(name),
                                        "component_manager");
  }

  // One exchange on `host`'s `channel` (ExchangeAll); returns its reply.
  [[nodiscard]] std::string
  Exchange(std::unique_lock<std::mutex> &lock, std::shared_ptr<Child> host,
           HostChannel &channel, const std::string_view request,
           EchoMatch match, const std::uint8_t kind,
           const std::string_view id_type = "none",
           const std::string_view identity = {}) {
    ChannelExchange exchange{std::move(host), &channel, request,
                             std::move(match), kind};
    ExchangeAll(lock, std::span(&exchange, 1U));
    ReportSlowExchange(kExchangeKinds[kind], exchange.host->pid, id_type,
                       identity, exchange.started);
    return std::move(exchange.reply);
  }

  // OD-270: a management exchange of the owner of the unit's busy phase, or
  // of a child not yet registered (projection preparation, process
  // inspection, projection rebuild). The phase keeps every other caller off
  // the child.
  [[nodiscard]] std::string
  PhaseExchange(const std::shared_ptr<Child> &host,
                const std::string_view request, EchoMatch match,
                const std::uint8_t kind) {
    std::unique_lock lock(mutex);
    return Exchange(lock, host, host->management, request, std::move(match),
                    kind);
  }

  // A projection reply decodes as a projection response; Prepare echoes its
  // exact request (L-1).
  [[nodiscard]] static EchoMatch
  ProjectionEcho(const detail::NativeProjectionRpcRequest &request) {
    std::optional<detail::NativeProjectionPrepareRequest> prepare;
    if (const auto *selected =
            std::get_if<detail::NativeProjectionPrepareRequest>(&request))
      prepare = *selected;
    return [prepare = std::move(prepare)](const std::string &packet) {
      const auto response = detail::DecodeNativeProjectionRpcResponse(packet);
      if (!prepare ||
          std::holds_alternative<detail::NativeProjectionRpcFailure>(response))
        return true;
      const auto *prepared =
          std::get_if<detail::NativeProjectionPrepareResult>(&response);
      return prepared != nullptr &&
             prepared->request.execution_host_binding ==
                 prepare->execution_host_binding &&
             prepared->request.candidate_retained_bundle_id ==
                 prepare->candidate_retained_bundle_id &&
             prepared->request.candidate_graph_revision ==
                 prepare->candidate_graph_revision &&
             prepared->request.planning_collision_model ==
                 prepare->planning_collision_model;
    };
  }

  // A cell-admission reply echoes its command.
  [[nodiscard]] static EchoMatch CellAdmissionEcho(std::string command) {
    return [command = std::move(command)](const std::string &later) {
      return detail::DecodeKernelCellAdmissionRpcResponse(later).command ==
             command;
    };
  }

  // A projection exchange; `lock` holds `mutex` on entry and is released.
  [[nodiscard]] detail::NativeProjectionRpcResponse
  ExchangeProjection(std::unique_lock<std::mutex> &lock,
                     const std::shared_ptr<Child> &host,
                     const detail::NativeProjectionRpcRequest &request) {
    return detail::DecodeNativeProjectionRpcResponse(Exchange(
        lock, host, host->management,
        detail::EncodeNativeProjectionRpcRequest(request),
        ProjectionEcho(request), static_cast<std::uint8_t>(request.index())));
  }

  // Under `mutex`: the live child that owns `binding`.
  [[nodiscard]] std::shared_ptr<Child> ProjectionChildLocked(
      const manyforge::core::ProjectionExecutionHostBindingDTO &binding) {
    if (!binding.valid()) {
      Fail("native projection request binding is invalid");
    }
    ThrowIfBusyLocked(binding.runtime_unit_id.value());
    const auto found = children.find(binding.runtime_unit_id.value());
    if (found == children.end()) {
      Fail("native projection client no longer names the current child epoch");
    }
    const ProjectionBindingKey key{binding.provider_binding_id,
                                   binding.provider_binding_revision};
    if (!found->second->admitted_projection_bindings.contains(key)) {
      Fail("native projection client binding is no longer admitted");
    }
    if (!ChildIsLive(*found->second)) {
      EraseChild(found);
      throw ExitedBeforeSend("projection");
    }
    return found->second;
  }

  template <typename Response>
  [[nodiscard]] static Response
  RequireProjectionResponse(detail::NativeProjectionRpcResponse response) {
    if (const auto *failure =
            std::get_if<detail::NativeProjectionRpcFailure>(&response)) {
      Fail("native projection request failed [" + failure->error.code +
           "]: " + failure->error.message);
    }
    auto *typed = std::get_if<Response>(&response);
    if (typed == nullptr) {
      Fail("native projection child returned the wrong response kind");
    }
    return std::move(*typed);
  }

  template <typename Response>
  [[nodiscard]] Response ProjectionCall(
      const manyforge::core::ProjectionExecutionHostBindingDTO &binding,
      detail::NativeProjectionRpcRequest request) {
    std::unique_lock lock(mutex);
    const auto child = ProjectionChildLocked(binding);
    return RequireProjectionResponse<Response>(
        ExchangeProjection(lock, child, request));
  }

  // A rebuild: no time limit and no adapter lock across the exchange; the
  // unit's `rebuilding` phase answers concurrent calls busy (OD-270).
  template <typename Response>
  [[nodiscard]] Response ProjectionRebuildCall(
      const manyforge::core::ProjectionExecutionHostBindingDTO &binding,
      detail::NativeProjectionRpcRequest request) {
    std::shared_ptr<Child> child;
    {
      std::lock_guard lock(mutex);
      child = ProjectionChildLocked(binding);
      busy_units.emplace(binding.runtime_unit_id.value(), "rebuilding");
    }
    const BusyPhase phase(*this, binding.runtime_unit_id.value());
    return RequireProjectionResponse<Response>(
        detail::DecodeNativeProjectionRpcResponse(PhaseExchange(
            child, detail::EncodeNativeProjectionRpcRequest(request),
            ProjectionEcho(request),
            static_cast<std::uint8_t>(request.index()))));
  }

  template <typename Response>
  [[nodiscard]] manyforge::core::Result<Response> ProjectionResultCall(
      const manyforge::core::ProjectionExecutionHostBindingDTO &binding,
      detail::NativeProjectionRpcRequest request) {
    if (!binding.valid()) {
      Fail("native projection request binding is invalid");
    }
    // A request beyond a codec bound is refused before it is sent.
    std::string encoded;
    try {
      encoded = detail::EncodeNativeProjectionRpcRequest(request);
    } catch (const detail::NativeProjectionRpcBoundExceeded &error) {
      return manyforge::core::Result<Response>::Err(manyforge::core::ErrorDTO{
          std::string(
              manyforge::core::error_codes::kNativeProjectionRpcBoundExceeded),
          error.what(),
          {}});
    }
    std::unique_lock lock(mutex);
    const auto child = ProjectionChildLocked(binding);
    auto response = detail::DecodeNativeProjectionRpcResponse(
        Exchange(lock, child, child->management, encoded,
                 ProjectionEcho(request),
                 static_cast<std::uint8_t>(request.index())));
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
    // Read from the Manager's own plan before the channel lock is taken.
    detail::KernelApplyFenceRpcPacket packet{request, std::nullopt};
    if constexpr (!std::is_same_v<Request, manyforge::core::ConfigurationFenceReleaseDTO>)
      packet.active_publication = active_publication();
    std::unique_lock lock(mutex);
    if (auto busy = AnyBusyLocked())
      throw manyforge::core::TypedError(*busy, "component_manager");
    if (children.size() != 1U) {
      Fail("Kernel Apply-fence request requires the sole managed native "
           "execution child");
    }
    auto found = children.begin();
    if (!ChildIsLive(*found->second)) {
      EraseChild(found);
      throw ExitedBeforeSend("fence");
    }
    return ExchangeKernelFence<Response>(lock, found->second, request, packet);
  }

  template <typename Response, typename Request>
  [[nodiscard]] Response ExchangeKernelFence(std::unique_lock<std::mutex> &lock,
                                             const std::shared_ptr<Child> &child,
                                             const Request &request,
                                             const detail::KernelApplyFenceRpcPacket &packet) {
    const auto &kind = packet.request;
    const auto encoded = detail::EncodeKernelApplyFenceRpcPacket(packet);
    static constexpr std::array<std::string_view, 5> names{
        "fence_observe_admission", "fence_acquire", "fence_reconcile_attempt",
        "fence_query_status", "fence_release"};
    static_assert(names.size() == std::variant_size_v<detail::KernelApplyFenceRpcRequest>);
    const auto [id_type, id] = FenceIdentity(request);
    // A fence reply carries its request's identity.
    EchoMatch echo = [request](const std::string &later) {
      const auto response = detail::DecodeKernelApplyFenceRpcResponse(later);
      if (std::holds_alternative<detail::KernelApplyFenceRpcFailure>(response))
        return true;
      const auto *typed = std::get_if<Response>(&response);
      if (typed == nullptr) return false;
      if constexpr (requires(const Response &reply, const Request &query) {
                      reply.matches(query);
                    })
        return typed->matches(request);
      else
        return true;
    };
    const auto fence_reply =
        Exchange(lock, child, child->management, encoded, std::move(echo),
                 ExchangeKind(names[kind.index()]), id_type, id);
    auto response = detail::DecodeKernelApplyFenceRpcResponse(fence_reply);
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

  template <typename Response>
  [[nodiscard]] manyforge::core::Result<Response>
  KernelCellAdmissionCall(const detail::KernelCellAdmissionRpcPacket &request) {
    std::unique_lock lock(mutex);
    if (auto busy = AnyBusyLocked())
      return manyforge::core::Result<Response>::Err(std::move(*busy));
    if (children.size() != 1U) {
      return manyforge::core::Result<Response>::Err(
          {"kernel_owner_unavailable",
           "cell-admission request requires the sole managed native execution "
           "child",
           {{"reason", "execution_host_child_unavailable"}}});
    }
    auto found = children.begin();
    if (!ChildIsLive(*found->second)) {
      const auto exit_reason =
          execution_host_child_exit_reason(found->second->exit_status);
      EraseChild(found);
      return manyforge::core::Result<Response>::Err(
          {"kernel_owner_unavailable",
           "managed Kernel child exited before its cell-admission response",
           {{"reason", std::string(exit_reason)}}});
    }
    const auto encoded = detail::EncodeKernelCellAdmissionRpcRequest(request);
    const auto [id_type, id] = AdmissionIdentity(request);
    std::string packet;
    try {
      packet = Exchange(lock, found->second, found->second->management,
                        encoded, CellAdmissionEcho(request.command),
                        ExchangeKind(request.command), id_type, id);
    } catch (const manyforge::core::TypedError &error) {
      return manyforge::core::Result<Response>::Err(error.error());
    }
    const auto response = detail::DecodeKernelCellAdmissionRpcResponse(packet);
    if (response.command != request.command) {
      Fail("managed Kernel child returned a foreign cell-admission command");
    }
    if (const auto *error =
            std::get_if<manyforge::core::ErrorDTO>(&response.response)) {
      return manyforge::core::Result<Response>::Err(*error);
    }
    const auto *typed = std::get_if<Response>(&response.response);
    if (typed == nullptr) {
      Fail("managed Kernel child returned the wrong cell-admission response "
           "kind");
    }
    return manyforge::core::Result<Response>::Ok(*typed);
  }

  [[nodiscard]] manyforge::core::Result<
      ManagerComponentProcessInventoryResultDTO>
  InspectInstalledComponentProcesses(
      const ManagerComponentProcessInventoryRequestDTO &request) {
    using Result =
        manyforge::core::Result<ManagerComponentProcessInventoryResultDTO>;
    if (!request.valid())
      Fail("installed process inventory request is invalid");
    const auto encoded =
        detail::EncodeSelectedRuntimeCompositionRpcRequest(request);
    const auto runtime_id = request.runtime_unit_id.value();
    std::shared_ptr<Child> child;
    {
      std::lock_guard lock(mutex);
      if (auto busy = BusyLocked(runtime_id))
        return Result::Err(std::move(*busy));
      const auto found = children.find(runtime_id);
      if (found == children.end())
        return Result::Err({"installed_process_inspection_unavailable",
                            "installed process inspection has no managed child",
                            {}});
      if (!ChildIsLive(*found->second)) {
        EraseChild(found);
        return Result::Err(
            {"installed_process_inspection_unavailable",
             "managed child exited before installed process inspection",
             {}});
      }
      child = found->second;
      // No adapter lock across the exchange: the unit's `inspecting` phase
      // answers concurrent calls busy (OD-270).
      busy_units.emplace(runtime_id, "inspecting");
    }
    const BusyPhase phase(*this, runtime_id);
    // No time limit (OD-270): the read ends on its reply, the host's exit or
    // the application's stop. The inventory echoes its request.
    std::string packet;
    try {
      packet = PhaseExchange(
          child, encoded,
          [request](const std::string &later) {
            const auto response =
                detail::DecodeSelectedRuntimeCompositionRpcResponse(later);
            const auto *result =
                std::get_if<ManagerComponentProcessInventoryResultDTO>(
                    &response);
            return std::holds_alternative<
                       detail::SelectedRuntimeCompositionRpcFailure>(
                       response) ||
                   (result && result->valid() && result->request == request);
          },
          kInspectionKind);
    } catch (const manyforge::core::TypedError &error) {
      return Result::Err(error.error());
    }
    const auto response =
        detail::DecodeSelectedRuntimeCompositionRpcResponse(packet);
    if (const auto *failure =
            std::get_if<detail::SelectedRuntimeCompositionRpcFailure>(
                &response))
      return Result::Err(failure->error);
    const auto *result =
        std::get_if<ManagerComponentProcessInventoryResultDTO>(&response);
    if (!result || !result->valid() || result->request != request)
      Fail("managed child returned foreign installed process inventory");
    return Result::Ok(*result);
  }

  [[nodiscard]] manyforge::core::Result<HostApplyResult>
  ApplyOnHost(const manyforge::core::RuntimeUnitId &runtime_unit_id,
              const HostApplyRequest &request) {
    if (!request.valid()) Fail("host Apply request is invalid");
    std::unique_lock lock(mutex);
    if (auto busy = BusyLocked(runtime_unit_id.value()))
      return manyforge::core::Result<HostApplyResult>::Err(std::move(*busy));
    const auto found = children.find(runtime_unit_id.value());
    if (found == children.end()) {
      Fail("host Apply names no current execution host");
    }
    if (!ChildIsLive(*found->second)) {
      EraseChild(found);
      return manyforge::core::Result<HostApplyResult>::Err(
          ExitedBeforeSend("host_apply").error());
    }
    const auto encoded = detail::EncodeSelectedRuntimeCompositionRpcRequest(request);
    // No time limit (OD-290 (a)): the host installs every added or replaced
    // component inside this one exchange. The host Apply reply carries no
    // Apply id; the one pending exchange identifies it (L-1).
    std::string packet;
    try {
      packet = Exchange(
          lock, found->second, found->second->management, encoded,
          [](const std::string &later) {
            const auto response =
                detail::DecodeSelectedRuntimeCompositionRpcResponse(later);
            const auto *result = std::get_if<HostApplyResult>(&response);
            return std::holds_alternative<
                       detail::SelectedRuntimeCompositionRpcFailure>(
                       response) ||
                   (result != nullptr && result->valid());
          },
          ExchangeKind("host_apply"), "apply_id",
          std::to_string(request.apply.apply_id.value()));
    } catch (const manyforge::core::TypedError &error) {
      return manyforge::core::Result<HostApplyResult>::Err(error.error());
    }
    const auto response = detail::DecodeSelectedRuntimeCompositionRpcResponse(packet);
    if (const auto *failure =
            std::get_if<detail::SelectedRuntimeCompositionRpcFailure>(&response))
      return manyforge::core::Result<HostApplyResult>::Err(failure->error);
    const auto *result = std::get_if<HostApplyResult>(&response);
    if (result == nullptr || !result->valid()) {
      Fail("execution host returned a foreign Apply response");
    }
    return manyforge::core::Result<HostApplyResult>::Ok(*result);
  }

  [[nodiscard]] manyforge::core::ManagerLivenessObservationDTO
  RenewManagerLiveness(const ManagerLivenessTransportRequestDTO &request) {
    auto responses = RenewManagerLivenessBatch({request});
    return std::move(responses.front());
  }

  [[nodiscard]] std::vector<manyforge::core::ManagerLivenessObservationDTO>
  RenewManagerLivenessBatch(
      const std::vector<ManagerLivenessTransportRequestDTO> &requests) {
    if (requests.empty() ||
        requests.size() > manyforge::core::kMaximumManagerLivenessReceivers) {
      Fail("Manager-liveness batch has an invalid receiver count");
    }
    const auto &required_set = requests.front().required_set;
    const auto &heartbeat = requests.front().heartbeat;
    const auto &policy = requests.front().policy;
    const auto &publication = requests.front().active_publication;
    std::set<std::string> requested_receivers;
    for (const auto &request : requests) {
      if (!manyforge::core::ValidateManagerLivenessRequiredSet(
               request.required_set)
               .ok() ||
          request.required_set != required_set ||
          request.heartbeat != heartbeat || request.policy != policy ||
          request.active_publication != publication ||
          !requested_receivers.insert(request.receiver_runtime_unit_id.value())
               .second) {
        Fail("Manager-liveness batch is not one exact required-set renewal");
      }
    }
    // The required set may also contain same-host external receivers (for
    // example Program Runner) renewed by another strict transport in the same
    // Manager generation.  This adapter must atomically cover every requested
    // native child, while preserving the complete set in every native packet.

    if (!manyforge::core::ValidateManagerLivenessRequiredSet(required_set)
             .ok()) {
      Fail("Manager-liveness request has an invalid required set");
    }
    std::unique_lock lock(liveness_mutex);

    std::vector<std::string> encoded;
    encoded.reserve(requests.size());
    std::vector<std::shared_ptr<Child>> hosts;
    hosts.reserve(requests.size());
    for (const auto &request : requests) {
      const auto required = std::find_if(
          required_set.required_receivers.begin(),
          required_set.required_receivers.end(), [&](const auto &row) {
            return row.runtime_unit_id == request.receiver_runtime_unit_id;
          });
      const auto endpoint =
          liveness_endpoints.find(request.receiver_runtime_unit_id.value());
      if (required == required_set.required_receivers.end() ||
          endpoint == liveness_endpoints.end()) {
        Fail("Manager-liveness request names an unavailable native receiver");
      }
      encoded.push_back(detail::EncodeManagerLivenessRpcRequest(request));
      hosts.push_back(endpoint->second);
    }

    // OD-290 (a): the renewals are sent once every channel is free and wait
    // for their replies with no time limit and no adapter lock held
    // (ExchangeAll). A refused renewal is still that renewal's reply.
    std::vector<ChannelExchange> exchanges;
    exchanges.reserve(hosts.size());
    for (std::size_t index = 0U; index < hosts.size(); ++index) {
      auto &host = hosts[index];
      exchanges.push_back(ChannelExchange{
          host, &host->liveness, encoded[index],
          [](const std::string &later) {
            try {
              static_cast<void>(detail::DecodeManagerLivenessRpcResponse(later));
            } catch (const detail::ManagerLivenessRpcRejection &) {
            }
            return true;
          },
          kLivenessRenewalKind});
    }
    ExchangeAll(lock, exchanges);
    std::vector<manyforge::core::ManagerLivenessObservationDTO> responses;
    responses.reserve(exchanges.size());
    std::size_t superseded_count = 0U;
    std::exception_ptr first_other_failure;
    for (const auto &exchange : exchanges) {
      try {
        responses.push_back(
            detail::DecodeManagerLivenessRpcResponse(exchange.reply));
      } catch (const detail::ManagerLivenessRpcRejection &rejection) {
        if (rejection.reason() == "superseded_active_publication") {
          ++superseded_count;
        } else if (first_other_failure == nullptr) {
          first_other_failure = std::current_exception();
        }
      } catch (...) {
        if (first_other_failure == nullptr) {
          first_other_failure = std::current_exception();
        }
      }
    }
    if (superseded_count == exchanges.size()) {
      throw ManagerLivenessPublicationSuperseded();
    }
    if (superseded_count != 0U || first_other_failure != nullptr ||
        responses.size() != exchanges.size()) {
      if (first_other_failure != nullptr && superseded_count == 0U) {
        std::rethrow_exception(first_other_failure);
      }
      Fail("Manager-liveness batch returned mixed renewal outcomes");
    }
    return responses;
  }

  template <typename Response>
  [[nodiscard]] manyforge::core::Result<Response>
  KernelLivenessLaneCall(const detail::KernelCellAdmissionRpcPacket &request) {
    // Readiness and liveness form one ordered fail-closed control stream.
    // Keep it independent from projection and authoring calls: those may
    // legitimately take longer than the Manager report interval.
    std::unique_lock lock(liveness_mutex);
    if (liveness_endpoints.size() != 1U) {
      return manyforge::core::Result<Response>::Err(
          {"kernel_owner_unavailable",
           "Manager liveness/readiness requires the sole managed native "
           "execution child",
           {{"reason", "execution_host_child_unavailable"}}});
    }
    const auto host = liveness_endpoints.begin()->second;
    const auto encoded = detail::EncodeKernelCellAdmissionRpcRequest(request);
    const auto [id_type, id] = AdmissionIdentity(request);
    // OD-290 (a): no time limit and no adapter lock across the wait.
    std::string packet;
    try {
      packet = Exchange(lock, host, host->liveness, encoded,
                        CellAdmissionEcho(request.command),
                        ExchangeKind(request.command), id_type, id);
    } catch (const manyforge::core::TypedError &error) {
      return manyforge::core::Result<Response>::Err(error.error());
    }
    const auto response = detail::DecodeKernelCellAdmissionRpcResponse(packet);
    if (response.command != request.command) {
      Fail("managed Kernel child returned a foreign liveness/readiness "
           "command");
    }
    if (const auto *error =
            std::get_if<manyforge::core::ErrorDTO>(&response.response)) {
      return manyforge::core::Result<Response>::Err(*error);
    }
    const auto *typed = std::get_if<Response>(&response.response);
    if (typed == nullptr) {
      Fail("managed Kernel child returned the wrong liveness/readiness "
           "response kind");
    }
    return manyforge::core::Result<Response>::Ok(*typed);
  }

  void PrepareChildProjectionBindings(
      const std::shared_ptr<Child> &host, const RetainedBundle &bundle,
      const manyforge::core::RuntimeUnitId &runtime_unit_id) {
    auto &child = *host;
    const auto bindings =
        NativeProjectionBindingsForRuntime(bundle, runtime_unit_id);
    for (const auto &[provider_binding_id, provider_binding_revision] :
         bindings) {
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
      // The child owns projection residency and bounded retirement. Historical
      // preparation is not proof that a removed provider still exists; its
      // exact Prepare replay already avoids rebuilding a resident product.
      // No time limit (OD-270): it ends on its reply, the host's exit or the
      // application's stop; its start and duration are logged.
      const std::string binding_text = provider_binding_id.value();
      LogLine(LogSeverity::kInfo, LogText("native projection preparation started: pid=",
                          child.pid, " binding=", binding_text));
      const auto started = std::chrono::steady_clock::now();
      const auto elapsed_ms = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - started)
            .count();
      };
      const auto response = [&] {
        try {
          std::unique_lock lock(mutex);
          return ExchangeProjection(lock, host, request);
        } catch (...) {
          LogLine(LogSeverity::kWarning, LogText("native projection preparation ended without "
                              "its reply: pid=",
                              child.pid, " binding=", binding_text,
                              " elapsed_ms=", elapsed_ms()));
          throw;
        }
      }();
      LogLine(LogSeverity::kInfo, LogText("native projection preparation answered: pid=",
                          child.pid, " binding=", binding_text,
                          " elapsed_ms=", elapsed_ms()));
      if (const auto *failure =
              std::get_if<detail::NativeProjectionRpcFailure>(&response)) {
        Fail("native projection request failed [" + failure->error.code +
             "]: " + failure->error.message);
      }
      const auto *prepared =
          std::get_if<detail::NativeProjectionPrepareResult>(&response);
      if (prepared == nullptr ||
          prepared->request.execution_host_binding !=
              request.execution_host_binding ||
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
    }
  }

  // One step of the stop's progress reading, never waiting (OD-290 (a)):
  // the stop loop calls it each slice. Empty while an answer is not there
  // yet.
  [[nodiscard]] std::optional<detail::NativeShutdownProgress>
  ReadShutdownProgress(Child &child) {
    // StopChild owns the management channel: the unit's `stopping` phase
    // keeps every other caller off it (OD-308 (a)). Heartbeat renewals keep
    // their independent existing endpoint, stream and mutex throughout host
    // drain.
    // An exchange abandoned or outstanding before the stop is answered
    // first; its reply is settled here, never read as the shutdown's
    // (OD-270).
    if (!SettlePending(child, child.management)) return std::nullopt;
    if (!child.shutdown_acknowledged) {
      const auto acknowledgement = TryReceivePacket(child.management.fd.get());
      if (!acknowledgement) return std::nullopt;
      if (*acknowledgement != "shutdown-accepted")
        Fail("shutdown acknowledgement belongs to another request");
      child.shutdown_acknowledged = true;
    }
    // One progress request at a time: only its actual reply clears this
    // transport state. Never queue retries.
    if (!child.shutdown_progress_pending) {
      if (!TrySendPacket(child.management.fd.get(),
                         detail::EncodeNativeShutdownRpcRequest(
                             child.runtime_epoch.value())))
        return std::nullopt;
      child.shutdown_progress_pending = true;
    }
    const auto packet = TryReceivePacket(child.management.fd.get());
    if (!packet) return std::nullopt;
    child.shutdown_progress_pending = false;
    return detail::DecodeNativeShutdownRpcResponse(*packet);
  }

  void StopChildNoThrow(Child &child) noexcept {
    for (;;) {
      try {
        StopChild(child);
        return;
      } catch (const std::exception &error) {
        // Unknown direct-child ownership is not permission to target a reused
        // PID: this waits until the Manager process itself is ended (only a
        // defect reaches here; a stop request does not end this wait).
        LogLine(LogSeverity::kError, LogText("shutdown stuck at child-exit-observation: ",
                            error.what()));
      } catch (...) {
        LogLine(LogSeverity::kError, "shutdown stuck at child-exit-observation");
      }
      std::this_thread::sleep_for(kStuckStopRetryPause);
    }
  }

  void StopChild(Child &child) {
    if (!ChildIsLive(child))
      return;
    if (!child.shutdown_requested) {
      child.shutdown_requested =
          true; // one request, even if delivery is unknown
      LogLine(LogSeverity::kInfo, LogText("execution host child shutdown requested: pid=",
                          child.pid, " epoch=", child.runtime_epoch.value()));
    }
    // No time limit (OD-187 (1)): the host's exit (waitpid) ends the wait;
    // each change of its published phase is logged, so the last line names
    // where it waits. A second stop request ends the host with SIGKILL; the
    // Manager logs the kill (HostForceKilledPid). No adapter lock is held
    // here (OD-308 (a)): the caller keeps the child busy `stopping` or holds
    // it alone (a launch that failed).
    std::optional<detail::NativeShutdownProgress> last_progress;
    std::string last_observation_error;
    while (ChildIsLive(child)) {
      if (const auto force = shutdown_force.load(std::memory_order_acquire);
          !child.force_killed && force != nullptr && force()) {
        child.force_killed = true;
        if (::kill(child.pid, SIGKILL) == 0)
          host_force_killed_pid.store(child.pid, std::memory_order_release);
        continue;
      }
      // The shutdown request is sent when the channel takes it; a full
      // channel is retried at the next slice, never waited on (OD-290 (a)).
      if (!child.shutdown_sent) {
        try {
          child.shutdown_sent = TrySendPacket(child.management.fd.get(), "shutdown");
        } catch (const std::exception &error) {
          child.shutdown_sent = true;
          LogLine(LogSeverity::kWarning, LogText("native shutdown delivery failed: ", error.what(),
                              "; requesting existing SIGTERM handler once: pid=",
                              child.pid));
          // Existing handler requests the same host teardown. No automatic
          // total wait/escalation and no signal on a merely delayed reply.
          if (::kill(child.pid, SIGTERM) != 0 && errno != ESRCH)
            LogLine(LogSeverity::kError, LogText("native shutdown SIGTERM request failed: errno=",
                                errno));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        continue;
      }
      try {
        const auto read = ReadShutdownProgress(child);
        if (!read) {
          std::this_thread::sleep_for(std::chrono::milliseconds(20));
          continue;
        }
        const auto &progress = *read;
        if (!last_progress || progress != *last_progress) {
          last_progress = progress;
          LogLine(LogSeverity::kInfo,
                  LogText("native shutdown entering/waiting at ",
                          detail::NativeShutdownPhaseName(progress.phase),
                          " pid=", child.pid,
                          " epoch=", progress.runtime_epoch,
                          " workers_joined=", progress.completed_workers,
                          " groups_pending=", progress.pending_groups, '/',
                          progress.total_groups,
                          " signalled=", progress.signalled_groups,
                          " escalated=", progress.escalated_groups,
                          " reaped=", progress.reaped_groups,
                          " drained=", progress.drained_groups,
                          " unknown=", progress.unknown_groups,
                          " pending_pid=", progress.pending_pid));
        }
        last_observation_error.clear();
      } catch (const std::exception &error) {
        // Responsiveness/transport loss never proves EXIT or authorizes force.
        if (last_observation_error != error.what()) {
          last_observation_error = error.what();
          LogLine(LogSeverity::kWarning, LogText("native shutdown progress unavailable: ",
                              error.what()));
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }

  [[nodiscard]] std::shared_ptr<Child>
  LaunchChild(const ExecutionHostUnitAdmission &admission,
              const RetainedBundle &bundle) {
    auto target = OpenInstalledTarget(admission);
    auto channels = MakePacketPair();
    auto liveness_channels = MakePacketPair();
    auto exec_pipe = MakePipe();
    SetNonBlocking(channels[0].get());
    SetNonBlocking(liveness_channels[0].get());
    const RuntimeEpoch runtime_epoch = start_epoch;
    const auto launch_arguments =
        admission.launch_arguments(kChildChannelFd, runtime_epoch);
    std::vector<std::string> arguments;
    arguments.reserve(launch_arguments.size() + 25U);
    arguments.push_back(admission.installed_target_id());
    arguments.insert(arguments.end(), launch_arguments.begin(),
                     launch_arguments.end());
    arguments.emplace_back("--manyforge-managed-package-root-fd");
    arguments.push_back(std::to_string(kChildPackageRootFd));
    arguments.emplace_back("--manyforge-managed-program-artifact-root-fd");
    arguments.push_back(std::to_string(kChildProgramArtifactRootFd));
    arguments.emplace_back("--manyforge-managed-liveness-channel-fd");
    arguments.push_back(std::to_string(kChildLivenessChannelFd));
    arguments.emplace_back("--manyforge-managed-manager-run-root");
    arguments.push_back(manager_run_root_path.string());
    arguments.emplace_back("--manyforge-managed-execution-state-root");
    arguments.push_back(execution_state_root_path.string());
    arguments.emplace_back("--manyforge-managed-execution-run-root");
    arguments.push_back(execution_run_root_path.string());
    arguments.emplace_back("--manyforge-managed-candidate-worker");
    arguments.push_back(
        (target_root_path / admission.candidate_worker_installed_target_id())
            .string());
    arguments.emplace_back("--manyforge-managed-cell-id");
    arguments.push_back(bundle.value().cell_graph.cell_id.value());
    arguments.emplace_back("--manyforge-managed-retained-bundle-id");
    arguments.push_back(bundle.retained_bundle_id().value());
    arguments.emplace_back("--manyforge-managed-graph-revision");
    arguments.push_back(bundle.value().cell_graph.graph_revision.value());
    // Scene meshes are read by content digest from the activation root's
    // raw-artifact store; without an activation root they fail closed.
    if (activation_root_fd) {
      arguments.emplace_back("--manyforge-managed-scene-content-root-fd");
      arguments.push_back(std::to_string(kChildSceneContentRootFd));
    }
    if (site_settings_snapshot.has_value()) {
      arguments.emplace_back("--manyforge-managed-site-settings");
      arguments.push_back(site_settings_snapshot->string());
    }
    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (auto &argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    const int selected_package_root_fd =
        PackageRootFd(bundle.retained_bundle_id());
    // The managed child needs both verified package roots: the selected bundle
    // can be authored while the active predecessor is still installed (or vice
    // versa). Keep the selected root first and pass the other root as its exact
    // read-only fallback for publication/fence reconciliation.
    const int selected_program_artifact_root_fd =
        activation_root_path.empty()
            ? selected_package_root_fd
            : (selected_package_root_fd == activation_root_fd.get()
                   ? package_root_fd.get()
                   : activation_root_fd.get());
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
      Fail(ErrnoDetail("could not fork native execution target", errno));
    }
    if (pid == 0) {
      EnterChild(parent, channels[1].get(), liveness_channels[1].get(),
                 exec_pipe[1].get(), target.get(), selected_package_root_fd,
                 selected_program_artifact_root_fd, activation_root_fd.get(),
                 core_dump_limit_bytes, argv.data());
    }

    channels[1].Reset();
    liveness_channels[1].Reset();
    exec_pipe[1].Reset();
    target.Reset();
    // The host's pidfd, kept by the child for every later wait (OD-297).
    UniqueFd exit_fd;
    try {
      // OD-290 (a): no time limit. The exec confirmation ends when the exec
      // pipe closes or reports; the handshake on the child's readiness, its
      // exit (pidfd) or the application's stop. The host logs its start-up
      // steps.
      ConfirmExecOrThrow(pid, exec_pipe[0].get(), abandon_fd.get());
      exec_pipe[0].Reset();
      LogLine(LogSeverity::kInfo, LogText("execution host launched: pid=", pid,
                          "; waiting for its readiness"));
      exit_fd.Reset(static_cast<int>(::syscall(SYS_pidfd_open, pid, 0U)));
      if (!exit_fd)
        Fail(ErrnoDetail("could not observe execution host exit", errno));
      std::string ready;
      auto end = SendOnHost(channels[0].get(), "hello", exit_fd.get(),
                            abandon_fd.get());
      if (end == HostWaitEnd::kReady)
        end = ReceiveOnHost(channels[0].get(), exit_fd.get(), abandon_fd.get(),
                            ready);
      if (end == HostWaitEnd::kAbandoned)
        throw manyforge::core::TypedError(StoppingError("launch"),
                                          "component_manager");
      if (end == HostWaitEnd::kExited)
        throw manyforge::core::TypedError(ExitedError("launch"),
                                          "component_manager");
      if (ready != admission.expected_ready_packet(runtime_epoch)) {
        Fail("native execution child returned malformed or foreign readiness");
      }
      LogLine(LogSeverity::kInfo, LogText("execution host ready: pid=", pid));
    } catch (...) {
      channels[0].Reset();
      liveness_channels[0].Reset();
      KillAndReap(pid);
      throw;
    }
    auto child = std::make_shared<Child>(
        pid, std::move(exit_fd), runtime_epoch, std::move(channels[0]),
        std::move(liveness_channels[0]),
        admission.product().product_identity().value(),
        admission.product().configuration_identity().value(),
        admission.runtime_contract_offer_digest());
    bool inserted = false;
    {
      std::lock_guard lock(liveness_mutex);
      inserted = liveness_endpoints
                     .emplace(admission.product().runtime_unit_id().value(),
                              child)
                     .second;
    }
    if (!inserted) {
      // OD-308 (a): stopped with no lock held. The child is registered
      // nowhere and the unit stays busy `launching` until the launch fails.
      StopChildNoThrow(*child);
      Fail("execution host liveness endpoint insertion conflicted");
    }
    try {
      PrepareChildProjectionBindings(child, bundle,
                                     admission.product().runtime_unit_id());
    } catch (...) {
      EraseLivenessEndpoint(admission.product().runtime_unit_id().value());
      StopChildNoThrow(*child);
      throw;
    }
    return child;
  }

  [[nodiscard]] int PackageRootFd(
      const manyforge::core::RetainedBundleId &retained_bundle_id) const {
    const auto &digest = retained_bundle_id.value();
    if (digest.size() != 71U || !digest.starts_with("sha256:")) {
      return package_root_fd.get();
    }
    const auto contains_bundle = [&](const std::filesystem::path &root,
                                     std::string_view label) {
      const auto path =
          root / "retained-bundles" / "sha256" / (digest.substr(7U) + ".json");
      std::error_code error;
      const auto status = std::filesystem::symlink_status(path, error);
      if (error == std::errc::no_such_file_or_directory ||
          status.type() == std::filesystem::file_type::not_found) {
        return false;
      }
      if (error) {
        Fail("could not inspect " + std::string(label) +
             " retained bundle: " + error.message());
      }
      return true;
    };

    // An installed immutable bundle owns its complete package closure.  An
    // authored cache is considered only for identities absent from the
    // installation; a partial duplicate must never shadow installed bytes.
    if (contains_bundle(package_root_path, "installed")) {
      return package_root_fd.get();
    }
    if (!activation_root_path.empty() &&
        contains_bundle(activation_root_path, "authored")) {
      return activation_root_fd.get();
    }
    return package_root_fd.get();
  }

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) {
    auto [bundle, admission] = Admission(query);
    static_cast<void>(bundle);
    static_cast<void>(admission);
    std::lock_guard lock(mutex);
    ThrowIfBusyLocked(query.plan.runtime_unit_id.value());
    const auto found = children.find(query.plan.runtime_unit_id.value());
    if (found == children.end()) {
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    if (!ChildIsLive(*found->second)) {
      EraseChild(found);
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    return Presence(query, ProcessPresenceState::kPresent,
                    found->second->runtime_epoch.value());
  }

  // The host is launched once per application start (Q1). A live host is
  // only prepared for the selected bundle's projection bindings; an exited
  // host is never relaunched: its exit ends the application (OD-122 (1)).
  // The launch, its handshake and the preparation run with no lock held;
  // the unit's visible phase answers concurrent calls (B13, contract 1.3).
  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) {
    auto [bundle, admission] = Admission(query);
    const auto runtime_id = query.plan.runtime_unit_id.value();
    std::shared_ptr<Child> live;
    {
      std::lock_guard lock(mutex);
      ThrowIfBusyLocked(runtime_id);
      // An exited host is never relaunched; its exit ends the application.
      if (host_exited.load(std::memory_order_acquire))
        throw ExitedBeforeSend("launch");
      const auto found = children.find(runtime_id);
      if (found != children.end()) {
        if (!ChildIsLive(*found->second)) {
          EraseChild(found);
          throw ExitedBeforeSend("launch");
        }
        live = found->second;
        busy_units.emplace(runtime_id, "preparing");
      } else {
        busy_units.emplace(runtime_id, "launching");
      }
    }
    const auto clear_phase = [&] { EndBusyPhase(runtime_id); };
    if (live != nullptr) {
      // The phase keeps every other caller off this child and its channel.
      try {
        PrepareChildProjectionBindings(live, *bundle,
                                       query.plan.runtime_unit_id);
      } catch (...) {
        clear_phase();
        throw;
      }
      std::lock_guard lock(mutex);
      busy_units.erase(runtime_id);
      busy_changed.notify_all();
      return Presence(query, ProcessPresenceState::kPresent,
                      live->runtime_epoch.value());
    }
    std::shared_ptr<Child> launched;
    try {
      launched = LaunchChild(admission, *bundle);
    } catch (...) {
      clear_phase();
      throw;
    }
    std::lock_guard lock(mutex);
    busy_units.erase(runtime_id);
    busy_changed.notify_all();
    const auto inserted =
        children.emplace(runtime_id, std::move(launched)).first;
    return Presence(query, ProcessPresenceState::kPresent,
                    inserted->second->runtime_epoch.value());
  }

  // The host's drain runs with no lock held (OD-308 (a)): under `mutex` the
  // unit is marked busy `stopping` and its child kept; StopChild then runs
  // unlocked, and the child is erased under `mutex` afterwards. A stopping
  // unit still counts as present (HasHostChild); concurrent calls get the
  // typed busy answer at once.
  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) {
    auto [bundle, admission] = Admission(query);
    static_cast<void>(bundle);
    const auto &runtime_id = query.plan.runtime_unit_id.value();
    std::shared_ptr<Child> stopping;
    {
      std::unique_lock lock(mutex);
      // The application's stop (OD-270, OD-290 (a)): its abandon signal ends
      // every exchange wait at once, so the unit's busy phase and any
      // awaited exchange end promptly; the stop then proceeds instead of
      // answering busy. A unit already stopping is answered busy at once.
      if (ExchangesAbandoned())
        busy_changed.wait(lock, [&] {
          const auto child = children.find(runtime_id);
          return IsStoppingLocked(runtime_id) ||
                 (!busy_units.contains(runtime_id) &&
                  (child == children.end() ||
                   !child->second->management.pending ||
                   !child->second->management.pending->awaited));
        });
      ThrowIfBusyLocked(runtime_id);
      const auto found = children.find(runtime_id);
      if (found == children.end()) {
        return Presence(query, ProcessPresenceState::kAbsent);
      }
      if (!SameChildProduct(admission, found->second->product_identity,
                            found->second->configuration_identity,
                            found->second->runtime_contract_offer_digest)) {
        Fail("execution host stop query differs from the tracked child");
      }
      stopping = found->second;
      busy_units.emplace(runtime_id, "stopping");
    }
    LogLine(LogSeverity::kInfo, LogText("stopping execution host child: runtime_unit=",
                        runtime_id, " epoch=", stopping->runtime_epoch.value()));
    try {
      StopChild(*stopping);
    } catch (...) {
      EndBusyPhase(runtime_id);
      throw;
    }
    {
      std::lock_guard lock(mutex);
      EraseChild(children.find(runtime_id));
      busy_units.erase(runtime_id);
    }
    busy_changed.notify_all();
    return Presence(query, ProcessPresenceState::kAbsent);
  }

  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const RuntimeEpoch &runtime_epoch,
                  const std::uint64_t answering_after) {
    auto [bundle, admission] = Admission(query);
    static_cast<void>(bundle);
    if (runtime_epoch.empty()) {
      Fail("retained native adapter received an empty runtime epoch");
    }
    std::lock_guard lock(mutex);
    ThrowIfBusyLocked(query.plan.runtime_unit_id.value());
    const auto found = children.find(query.plan.runtime_unit_id.value());
    if (found == children.end()) {
      return std::nullopt;
    }
    if (!SameChildProduct(admission, found->second->product_identity,
                          found->second->configuration_identity,
                          found->second->runtime_contract_offer_digest)) {
      Fail("execution host progress query differs from the tracked child");
    }
    if (!ChildIsLive(*found->second)) {
      EraseChild(found);
      return std::nullopt;
    }
    // OD-290 (a): the status exchange never waits. The reply of the status
    // sent by an earlier observation is consumed here (or kept by a call
    // that settled the channel first), and the next status is sent when the
    // channel takes it. An unsettled exchange sends nothing; an observation
    // without a consumed reply reports no new source evidence, as does a
    // reply to a status sent at or before `answering_after` (OD-295 (1)(a)).
    auto &child = *found->second;
    auto &channel = child.management;
    if (!SettlePending(child, channel)) return std::nullopt;
    std::optional<RuntimeUnitSourceReport> report;
    if (channel.status_reply) {
      const auto packet = std::move(*channel.status_reply);
      channel.status_reply.reset();
      if (channel.status_reply_sequence > answering_after) {
        try {
          report = admission.parse_status_packet(runtime_epoch, packet);
        } catch (const ExecutionHostUnitAdmissionError &error) {
          Fail(error.what());
        }
      }
    }
    try {
      if (TrySendPacket(channel.fd.get(), "status"))
        MarkPending(
            channel,
            [status_admission = admission,
             epoch = runtime_epoch](const std::string &later) mutable {
              static_cast<void>(
                  status_admission.parse_status_packet(epoch, later));
              return true;
            },
            kStatusKind, false);
    } catch (const RetainedExecutionHostUnitAdapterError &error) {
      // A failed channel with the host alive: no reply can settle it, so
      // every call is answered at once until the host is stopped; the
      // channel stays open so the host is not ended by its close. An exited
      // host is reaped here or at the next observation.
      LogLine(LogSeverity::kError, LogText("execution host child private channel failed: pid=",
                          child.pid, ": ", error.what()));
      if (!ChildIsLive(child)) {
        EraseChild(found);
        return std::nullopt;
      }
      MarkPending(channel, [](const std::string &) { return false; },
                  kStatusKind, false);
      channel.pending->invalid_response = true;
    }
    return report;
  }

  // OD-290 (a): the oldest management exchange whose reply has not arrived
  // (both channels). Replies that arrived for an exchange nobody waits for
  // are settled first (non-blocking), so a status reply waiting to be
  // consumed is not outstanding. Units in a busy phase are left to their
  // owner.
  [[nodiscard]] std::optional<ExecutionHostReplyOutstandingDTO>
  ReplyOutstanding() {
    std::optional<std::tuple<std::chrono::steady_clock::time_point,
                             std::uint64_t, std::string>>
        oldest;
    const auto consider = [&](const std::chrono::steady_clock::time_point since,
                              const std::uint64_t sequence,
                              const std::string_view exchange) {
      if (!oldest || since < std::get<0>(*oldest))
        oldest.emplace(since, sequence, std::string(exchange));
    };
    {
      std::lock_guard lock(mutex);
      for (auto &[runtime_id, child] : children) {
        if (child->reaped || busy_units.contains(runtime_id)) continue;
        if (!SettlePending(*child, child->management))
          consider(child->management.pending->since,
                   child->management.pending->sequence,
                   kExchangeKinds[child->management.pending->kind]);
      }
    }
    {
      std::lock_guard lock(liveness_mutex);
      for (auto &[runtime_id, host] : liveness_endpoints)
        if (!SettlePending(*host, host->liveness))
          consider(host->liveness.pending->since,
                   host->liveness.pending->sequence,
                   kExchangeKinds[host->liveness.pending->kind]);
    }
    if (!oldest) return std::nullopt;
    return ExecutionHostReplyOutstandingDTO{
        std::get<1>(*oldest), std::move(std::get<2>(*oldest)),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - std::get<0>(*oldest))};
  }

  [[nodiscard]] manyforge::core::Result<
      std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
  ResolveProjection(
      const manyforge::core::ProjectionExecutionHostBindingDTO &binding);

  std::filesystem::path target_root_path;
  std::filesystem::path package_root_path;
  UniqueFd package_root_fd;
  std::filesystem::path activation_root_path;
  UniqueFd activation_root_fd;
  std::filesystem::path manager_run_root_path;
  std::filesystem::path execution_state_root_path;
  std::filesystem::path execution_run_root_path;
  const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source;
  const manyforge::deployment::IVerifiedRetainedProductArtifactSource
      &artifact_source;
  std::string operating_system;
  std::vector<std::string> launch_environment_contracts;
  std::optional<std::string> site_settings_document;
  // The soft core-file size limit of every host child, from the site
  // document, read once here.
  std::uint64_t core_dump_limit_bytes{};
  std::optional<std::filesystem::path> site_settings_snapshot;
  std::string observer_instance_id;
  // The 16-hex text of the application-start identity: the one host
  // occurrence of this application start carries it (contract 1.1).
  RuntimeEpoch start_epoch;
  UniqueFd target_root_fd;
  std::function<manyforge::core::ActiveBundlePublicationReceiptDTO()>
      active_publication;
  std::mutex admission_mutex;
  std::mutex mutex;
  // Notified when a unit's busy phase ends; the stop waits on it (OD-270).
  std::condition_variable busy_changed;
  std::mutex liveness_mutex;
  std::uint64_t observation_sequence{};
  std::map<std::string, AdmissionCacheEntry> admission_cache;
  // Guarded by `mutex`: the registered host children.
  std::map<std::string, std::shared_ptr<Child>> children;
  // Guarded by `mutex`. A unit being launched, prepared, inspected, rebuilt
  // or stopped, with its phase ("launching", "preparing", "inspecting",
  // "rebuilding" or "stopping"). The fork, the handshake, these exchanges and
  // the host's drain run with no lock; a call that meets the unit here gets
  // the typed busy answer instead of waiting (B13, contract 1.3; OD-270,
  // OD-308).
  std::map<std::string, std::string_view> busy_units;
  // OD-270: written once by AbandonExchanges or the Manager's stop signal
  // handler (the application is stopping) and never read empty, so every
  // later poll sees it.
  UniqueFd abandon_fd{::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)};
  // Set once a poll of `abandon_fd` (or AbandonExchanges) saw it written;
  // later checks then read memory only.
  mutable std::atomic<bool> abandoned{false};
  // Set once when the host child's exit is first observed; read lock-free by
  // the Manager's observation tick (OD-122 (1)).
  std::atomic<bool> host_exited{false};
  std::atomic<bool> host_exit_signaled{false};
  std::atomic<int> host_exit_value{0};
  std::atomic<pid_t> host_exit_pid{0};
  std::atomic<bool> host_exit_core_dumped{false};
  // HostReplyOutstanding's last printed failure line (once per episode);
  // used only by its one caller thread, the Manager's main thread.
  std::optional<std::string> reply_observation_last_failure;
  // OD-187 (1): the Manager's "a second stop request came" (set once before
  // any stop), and the host PID StopChild killed for it (0: none).
  std::atomic<bool (*)() noexcept> shutdown_force{nullptr};
  std::atomic<pid_t> host_force_killed_pid{0};
  // Numbers each sent exchange on either channel (PendingExchange::sequence).
  std::atomic<std::uint64_t> exchange_sequence{0U};
  // Guarded by `liveness_mutex`: the children the liveness lane reaches, by
  // runtime unit (a child is here from its launch's readiness on).
  std::map<std::string, std::shared_ptr<Child>> liveness_endpoints;
};

class RetainedExecutionHostUnitAdapter::Impl::ProjectionClient final
    : public manyforge::core::IPlannerSceneProjection {
public:
  ProjectionClient(std::shared_ptr<Impl> adapter,
                   manyforge::core::ProjectionExecutionHostBindingDTO binding)
      : adapter_(std::move(adapter)), binding_(std::move(binding)) {}

  [[noreturn]] static void Unavailable(const std::string_view operation) {
    Fail("native projection " + std::string(operation) +
         " is unavailable before the durable transition checkpoint");
  }

  // A transaction that passed CommitPreflight cannot exceed the codec bound
  // in practice: the preflight already encoded the same transaction. A breach
  // here therefore stays the existing uncertain-projection failure.
  manyforge::core::ProjectionCommitResultDTO ApplyTransaction(
      const manyforge::core::SceneTransactionDTO &transaction) override {
    auto result = adapter_->ProjectionCall<
        manyforge::core::ProjectionCommitResultDTO>(
        binding_, detail::NativeProjectionTransactionRequest{binding_, transaction});
    RequireHealth(result.resulting_health);
    return result;
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
    auto result =
        adapter_->ProjectionResultCall<detail::NativeProjectionRetainResult>(
            binding_, detail::NativeProjectionRetainRequest{
                          binding_, revision, std::move(lease_id)});
    if (!result.ok()) {
      return manyforge::core::Result<
          manyforge::core::ProjectionLeaseBindingDTO>::Err(result.error());
    }
    return manyforge::core::Result<manyforge::core::ProjectionLeaseBindingDTO>::
        Ok(std::move(result.value().projection_binding));
  }

  void ReleaseProjection(
      const manyforge::core::ProjectionLeaseId &lease_id) override {
    const auto released =
        adapter_->ProjectionCall<detail::NativeProjectionReleaseResult>(
            binding_,
            detail::NativeProjectionReleaseRequest{binding_, lease_id});
    if (released.projection_lease_id != lease_id) {
      Fail("native projection released a different lease");
    }
  }

  manyforge::core::ProjectionHealthDTO ProjectionHealth() const override {
    auto health = adapter_->ProjectionCall<
        manyforge::core::ProjectionHealthDTO>(
        binding_, detail::NativeProjectionHealthQuery{binding_});
    RequireHealth(health);
    return health;
  }

  manyforge::core::ProjectionHealthDTO ProjectionHealthForRevision(
      const manyforge::core::ProjectionRevisionDTO &selected_revision)
      const override {
    if (!selected_revision.valid()) {
      Fail("native projection health selector is invalid");
    }
    auto health =
        adapter_->ProjectionCall<manyforge::core::ProjectionHealthDTO>(
            binding_,
            detail::NativeProjectionHealthQuery{binding_, selected_revision});
    RequireSelectedHealth(health, selected_revision);
    return health;
  }

  manyforge::core::ProjectionCommitResultDTO RebuildProjection(
      const manyforge::core::NativeProjectionRebuildDTO &request) override {
    RequireBinding(request.expected_execution_host_binding,
                   "rebuild request");
    auto result = adapter_->ProjectionRebuildCall<
        manyforge::core::ProjectionCommitResultDTO>(binding_, request);
    RequireHealth(result.resulting_health);
    return result;
  }

  manyforge::core::NativeProjectionBootstrapRebuildResultDTO
  RebuildBootstrapProjection(
      const manyforge::core::NativeProjectionBootstrapRebuildDTO &request)
      override {
    RequireBinding(request.execution_host_binding,
                   "bootstrap rebuild request");
    auto result = adapter_->ProjectionRebuildCall<
        manyforge::core::NativeProjectionBootstrapRebuildResultDTO>(binding_,
                                                                    request);
    RequireBinding(result.execution_host_binding,
                   "bootstrap rebuild response");
    RequireHealth(result.resulting_health);
    return result;
  }

  manyforge::core::NativeProjectionBootstrapStatusDTO BootstrapProjectionStatus(
      const manyforge::core::NativeProjectionBootstrapStatusQueryDTO &request)
      const override {
    RequireBinding(request.execution_host_binding,
                   "bootstrap status request");
    auto result = adapter_
        ->ProjectionCall<manyforge::core::NativeProjectionBootstrapStatusDTO>(
            binding_, request);
    RequireBinding(result.execution_host_binding,
                   "bootstrap status response");
    RequireHealth(result.resulting_health);
    return result;
  }

  manyforge::core::Result<manyforge::core::SceneFrameResolutionReceiptDTO>
  ResolveSceneFrame(const manyforge::core::SceneFrameResolutionQueryDTO &query)
      const override {
    return adapter_
        ->ProjectionResultCall<manyforge::core::SceneFrameResolutionReceiptDTO>(
            binding_,
            detail::NativeProjectionSceneFrameRequest{binding_, query});
  }

  manyforge::core::Result<manyforge::core::CollisionPolicySnapshot>
  ProjectEffectiveCollisionPolicy(
      const manyforge::core::LogicalSceneSnapshotDTO &logical_scene,
      const std::uint64_t effective_policy_revision_id) const override {
    return adapter_
        ->ProjectionResultCall<manyforge::core::CollisionPolicySnapshot>(
            binding_,
            detail::NativeProjectionEffectivePolicyRequest{
                binding_, logical_scene, effective_policy_revision_id});
  }

  // One exchange; the candidate stays here, the host applies the delta.
  // A refusal of the call itself (a codec bound breach on encode or a typed
  // host-side failure) is returned as the error.
  manyforge::core::Result<manyforge::core::SceneCommitPreflightDTO>
  CommitPreflight(
      const manyforge::core::ProjectionRevisionDTO &selected_revision,
      const manyforge::core::SceneTransactionDTO *transaction,
      const manyforge::core::LogicalSceneSnapshotDTO *) const override {
    if (!selected_revision.valid()) {
      Fail("native projection health selector is invalid");
    }
    detail::NativeProjectionCommitPreflightRequest request{binding_,
                                                           selected_revision};
    if (transaction != nullptr) {
      request.transaction = *transaction;
    }
    auto called = adapter_->ProjectionResultCall<
        detail::NativeProjectionCommitPreflightResult>(binding_,
                                                       std::move(request));
    if (!called.ok()) {
      return manyforge::core::Result<
          manyforge::core::SceneCommitPreflightDTO>::Err(called.error());
    }
    auto &result = called.value();
    RequireSelectedHealth(result.health, selected_revision);
    manyforge::core::SceneCommitPreflightDTO preflight{std::move(result.health),
                                                       std::nullopt};
    if (result.effective_policy.has_value()) {
      using PolicyResult =
          manyforge::core::Result<manyforge::core::CollisionPolicySnapshot>;
      auto &policy = *result.effective_policy;
      preflight.effective_policy =
          std::holds_alternative<manyforge::core::CollisionPolicySnapshot>(
              policy)
              ? PolicyResult::Ok(std::get<manyforge::core::CollisionPolicySnapshot>(
                    std::move(policy)))
              : PolicyResult::Err(
                    std::get<manyforge::core::ErrorDTO>(std::move(policy)));
    }
    return manyforge::core::Result<manyforge::core::SceneCommitPreflightDTO>::
        Ok(std::move(preflight));
  }

private:
  void RequireBinding(
      const manyforge::core::ProjectionExecutionHostBindingDTO &binding,
      const std::string_view surface) const {
    if (!SameProjectionBindingIdentity(binding, binding_)) {
      Fail("native projection " + std::string(surface) +
           " names a different provider, runtime unit, or runtime epoch");
    }
  }

  void RequireHealth(
      const manyforge::core::ProjectionHealthDTO &health) const {
    RequireBinding(health.execution_host_binding, "health response");
  }

  void RequireSelectedHealth(
      const manyforge::core::ProjectionHealthDTO &health,
      const manyforge::core::ProjectionRevisionDTO &selected_revision) const {
    RequireHealth(health);
    if (health.active_projection_revision.has_value() &&
        (health.active_projection_revision->retained_bundle_id !=
             selected_revision.retained_bundle_id ||
         health.active_projection_revision->graph_revision !=
             selected_revision.graph_revision)) {
      Fail(
          "native projection health response names a different retained model");
    }
  }

  std::shared_ptr<Impl> adapter_;
  manyforge::core::ProjectionExecutionHostBindingDTO binding_;
};

manyforge::core::Result<
    std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
RetainedExecutionHostUnitAdapter::Impl::ResolveProjection(
    const manyforge::core::ProjectionExecutionHostBindingDTO &binding) {
  const auto missing = [](std::string message) {
    return manyforge::core::
        Result<std::shared_ptr<manyforge::core::IPlannerSceneProjection>>::Err(
            manyforge::core::ErrorDTO{
                "missing_reference", std::move(message), {}});
  };
  if (!binding.valid()) {
    return missing("native projection binding is invalid");
  }
  {
    std::lock_guard lock(mutex);
    if (auto busy = BusyLocked(binding.runtime_unit_id.value()))
      return manyforge::core::
          Result<std::shared_ptr<manyforge::core::IPlannerSceneProjection>>::Err(
              std::move(*busy));
    const auto found = children.find(binding.runtime_unit_id.value());
    if (found == children.end()) {
      return missing("no Manager child owns the execution host unit");
    }
    if (!ChildIsLive(*found->second)) {
      EraseChild(found);
      return missing("the Manager child is no longer live");
    }
    const ProjectionBindingKey key{binding.provider_binding_id,
                                   binding.provider_binding_revision};
    if (!found->second->admitted_projection_bindings.contains(key)) {
      return missing(
          "the verified child product does not own the projection binding");
    }
    return manyforge::core::
        Result<std::shared_ptr<manyforge::core::IPlannerSceneProjection>>::Ok(
            std::make_shared<ProjectionClient>(shared_from_this(), binding));
  }
}

RetainedExecutionHostUnitAdapter::RetainedExecutionHostUnitAdapter(
    const manyforge::core::ApplicationStartId application_start_id,
    std::filesystem::path trusted_installed_target_root,
    std::filesystem::path trusted_package_root,
    std::filesystem::path manager_run_root,
    std::filesystem::path execution_state_root,
    std::filesystem::path execution_run_root,
    const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string operating_system, std::string launch_environment_contract,
    std::filesystem::path activation_root,
    std::optional<std::string> site_settings_document,
    std::function<manyforge::core::ActiveBundlePublicationReceiptDTO()>
        active_publication)
    : RetainedExecutionHostUnitAdapter(
          application_start_id, std::move(trusted_installed_target_root),
          std::move(trusted_package_root),
          std::move(manager_run_root), std::move(execution_state_root),
          std::move(execution_run_root), bundle_source, artifact_source,
          std::move(operating_system),
          std::vector<std::string>{std::move(launch_environment_contract)},
          std::move(activation_root),
          std::move(site_settings_document), std::move(active_publication)) {}

RetainedExecutionHostUnitAdapter::RetainedExecutionHostUnitAdapter(
    const manyforge::core::ApplicationStartId application_start_id,
    std::filesystem::path trusted_installed_target_root,
    std::filesystem::path trusted_package_root,
    std::filesystem::path manager_run_root,
    std::filesystem::path execution_state_root,
    std::filesystem::path execution_run_root,
    const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string operating_system,
    std::vector<std::string> launch_environment_contracts,
    std::filesystem::path activation_root,
    std::optional<std::string> site_settings_document,
    std::function<manyforge::core::ActiveBundlePublicationReceiptDTO()>
        active_publication)
    : impl_(std::make_shared<Impl>(
          application_start_id, std::move(trusted_installed_target_root),
          std::move(trusted_package_root),
          std::move(manager_run_root), std::move(execution_state_root),
          std::move(execution_run_root), bundle_source, artifact_source,
          std::move(operating_system), std::move(launch_environment_contracts),
          std::move(activation_root),
          std::move(site_settings_document))) {
  impl_->active_publication = std::move(active_publication);
}

RetainedExecutionHostUnitAdapter::~RetainedExecutionHostUnitAdapter() = default;

std::optional<ExecutionHostExitDTO>
RetainedExecutionHostUnitAdapter::HostExit() const noexcept {
  if (!impl_->host_exited.load(std::memory_order_acquire)) return std::nullopt;
  return ExecutionHostExitDTO{
      impl_->host_exit_signaled.load(std::memory_order_relaxed),
      impl_->host_exit_value.load(std::memory_order_relaxed),
      impl_->host_exit_pid.load(std::memory_order_relaxed),
      impl_->host_exit_core_dumped.load(std::memory_order_relaxed)};
}

void RetainedExecutionHostUnitAdapter::SetShutdownForce(
    bool (*const force_requested)() noexcept) noexcept {
  impl_->shutdown_force.store(force_requested, std::memory_order_release);
}

void RetainedExecutionHostUnitAdapter::AbandonExchanges() noexcept {
  impl_->abandoned.store(true, std::memory_order_release);
  const std::uint64_t one = 1U;
  ssize_t written = -1;
  do {
    written = ::write(impl_->abandon_fd.get(), &one, sizeof(one));
  } while (written < 0 && errno == EINTR);
  LogLine(LogSeverity::kInfo, "execution host exchanges abandoned: the application is "
                    "stopping");
}

int RetainedExecutionHostUnitAdapter::AbandonSignalFd() const noexcept {
  return impl_->abandon_fd.get();
}

bool RetainedExecutionHostUnitAdapter::HasHostChild() const {
  std::lock_guard lock(impl_->mutex);
  // A stopping host is present until its stop erases it (OD-308 (a)); its
  // child is the stopper's to read.
  return std::any_of(impl_->children.begin(), impl_->children.end(),
                     [&](const auto &entry) {
                       return impl_->IsStoppingLocked(entry.first) ||
                              !entry.second->reaped;
                     });
}

std::optional<pid_t>
RetainedExecutionHostUnitAdapter::HostForceKilledPid() const noexcept {
  const auto pid = impl_->host_force_killed_pid.load(std::memory_order_acquire);
  if (pid == 0) return std::nullopt;
  return pid;
}

std::optional<ExecutionHostReplyOutstandingDTO>
RetainedExecutionHostUnitAdapter::HostReplyOutstanding() const noexcept {
  // Printed once per episode: a failure is printed only when its line differs
  // from the last one printed; the next success ends the episode.
  try {
    auto outstanding = impl_->ReplyOutstanding();
    impl_->reply_observation_last_failure.reset();
    return outstanding;
  } catch (const std::exception &error) {
    std::string text =
        LogText("execution host reply observation failed: ", error.what());
    if (impl_->reply_observation_last_failure != text) {
      LogLine(LogSeverity::kError, text);
      impl_->reply_observation_last_failure = std::move(text);
    }
    return std::nullopt;
  }
}

ProcessPresenceObservation RetainedExecutionHostUnitAdapter::ObserveProcess(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->ObserveProcess(query);
}

ProcessPresenceObservation RetainedExecutionHostUnitAdapter::EnsureRunning(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->EnsureRunning(query);
}

ProcessPresenceObservation RetainedExecutionHostUnitAdapter::EnsureStopped(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->EnsureStopped(query);
}

std::optional<RuntimeUnitSourceReport>
RetainedExecutionHostUnitAdapter::ObserveProgress(
    const RuntimeUnitConvergenceQueryDTO &query,
    const RuntimeEpoch &runtime_epoch, const std::uint64_t answering_after) {
  return impl_->ObserveProgress(query, runtime_epoch, answering_after);
}

std::uint64_t RetainedExecutionHostUnitAdapter::ProgressRequestMark() {
  // Under `mutex`: a status is sent and numbered under it (ObserveProgress),
  // so no status sent before this call is numbered after it.
  std::lock_guard lock(impl_->mutex);
  return impl_->exchange_sequence.load(std::memory_order_relaxed);
}

manyforge::core::ConfigurationFenceAdmissionSnapshotDTO
RetainedExecutionHostUnitAdapter::ObserveAdmission(
    const manyforge::core::ConfigurationFenceAdmissionQueryDTO &query) const {
  return impl_->KernelFenceCall<
      manyforge::core::ConfigurationFenceAdmissionSnapshotDTO>(query);
}

manyforge::core::ConfigurationFenceAcceptedDTO
RetainedExecutionHostUnitAdapter::Acquire(
    const manyforge::core::ApplyConfigurationFenceIntentDTO &intent) {
  return impl_->KernelFenceCall<manyforge::core::ConfigurationFenceAcceptedDTO>(
      intent);
}

manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO
RetainedExecutionHostUnitAdapter::ReconcileAcquireAttempt(
    const manyforge::core::ApplyFenceAcquireAttemptQueryDTO &query) const {
  return impl_->KernelFenceCall<
      manyforge::core::ApplyFenceAcquireAttemptReconcileResultDTO>(query);
}

manyforge::core::ConfigurationFenceStatusDTO
RetainedExecutionHostUnitAdapter::QueryStatus(
    const manyforge::core::ConfigurationFenceStatusQueryDTO &query) const {
  return impl_->KernelFenceCall<manyforge::core::ConfigurationFenceStatusDTO>(
      query);
}

manyforge::core::ConfigurationFenceReleasedDTO
RetainedExecutionHostUnitAdapter::Release(
    const manyforge::core::ConfigurationFenceReleaseDTO &request) {
  return impl_->KernelFenceCall<manyforge::core::ConfigurationFenceReleasedDTO>(
      request);
}

manyforge::core::AuthoringAdmissionDTO
RetainedExecutionHostUnitAdapter::GetAuthoringAdmission() const {
  auto result =
      impl_->KernelCellAdmissionCall<manyforge::core::AuthoringAdmissionDTO>(
          {"cell_admission_get", std::monostate{}});
  if (!result.ok()) {
    Fail("managed Kernel cell-admission get failed [" + result.error().code +
         "]: " + result.error().message);
  }
  return result.value();
}

manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
RetainedExecutionHostUnitAdapter::UpdateCellAdmissionReadiness(
    const manyforge::core::CellAdmissionReadinessSnapshotDTO &snapshot,
    std::shared_ptr<manyforge::core::IIoSignalService>* actual_retired_service) {
  // A process-local source pin is consumed by the receiving host, never RPC.
  if (actual_retired_service) actual_retired_service->reset();
  return impl_->KernelLivenessLaneCall<manyforge::core::AuthoringAdmissionDTO>(
      {"cell_admission_update_readiness", snapshot});
}

manyforge::core::Result<void> RetainedExecutionHostUnitAdapter::Publish(
    const manyforge::core::ManagerLivenessObservationDTO &observation) {
  if (!manyforge::core::ValidateManagerLivenessObservation(observation).ok()) {
    return manyforge::core::Result<void>::Err(
        {"invalid_request", "Manager-liveness observation is invalid", {}});
  }
  const auto result = impl_->KernelLivenessLaneCall<std::monostate>(
      {"cell_admission_publish_liveness", observation});
  return result.ok() ? manyforge::core::Result<void>::Ok()
                     : manyforge::core::Result<void>::Err(result.error());
}

manyforge::core::Result<manyforge::core::CellAdmissionClaimDTO>
RetainedExecutionHostUnitAdapter::AcquireCellAdmissionClaim(
    const manyforge::core::CellAdmissionClaimIntentDTO &intent) {
  return impl_->KernelCellAdmissionCall<manyforge::core::CellAdmissionClaimDTO>(
      {"cell_admission_acquire", intent});
}

manyforge::core::Result<manyforge::core::CellAdmissionClaimDTO>
RetainedExecutionHostUnitAdapter::AcquireManagerApplyClaim(
    const manyforge::core::CellAdmissionClaimIntentDTO &intent,
    const manyforge::core::RetainedBundleId &candidate_retained_bundle_id) {
  return impl_->KernelCellAdmissionCall<manyforge::core::CellAdmissionClaimDTO>(
      {"cell_admission_acquire",
       detail::ManagerApplyClaimAcquireRequest{intent, candidate_retained_bundle_id}});
}

manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
RetainedExecutionHostUnitAdapter::ReleaseCellAdmissionClaim(
    const manyforge::core::CellAdmissionClaimReleaseDTO &release) {
  return impl_->KernelCellAdmissionCall<manyforge::core::AuthoringAdmissionDTO>(
      {"cell_admission_release", release});
}

manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
RetainedExecutionHostUnitAdapter::ReleaseManagerApplyClaim(
    const manyforge::core::CellAdmissionClaimReleaseDTO &release,
    const manyforge::core::ManagerApplyReleaseOutcomeDTO &outcome) {
  return impl_->KernelCellAdmissionCall<manyforge::core::AuthoringAdmissionDTO>(
      {"cell_admission_release",
       detail::ManagerApplyClaimReleaseRequest{release, outcome}});
}

manyforge::core::Result<manyforge::core::CellAdmissionRecoveryReceiptDTO>
RetainedExecutionHostUnitAdapter::GetCellAdmissionRecoveryReceipt(
    const manyforge::core::CellAdmissionRecoveryReceiptQueryDTO &query) const {
  return impl_->KernelCellAdmissionCall<
      manyforge::core::CellAdmissionRecoveryReceiptDTO>(
      {"cell_admission_recovery_receipt", query});
}

manyforge::core::Result<void>
RetainedExecutionHostUnitAdapter::ValidateProgramResetSceneAuthorization(
    const manyforge::core::ProgramResetSceneAuthorizationDTO &authorization)
    const {
  const auto result = impl_->KernelCellAdmissionCall<std::monostate>(
      {"cell_admission_validate_program_reset_scene", authorization});
  return result.ok() ? manyforge::core::Result<void>::Ok()
                     : manyforge::core::Result<void>::Err(result.error());
}

manyforge::core::Result<
    std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
RetainedExecutionHostUnitAdapter::ResolveProjection(
    const manyforge::core::ProjectionExecutionHostBindingDTO
        &execution_host_binding) const {
  return impl_->ResolveProjection(execution_host_binding);
}

manyforge::core::Result<HostApplyResult>
RetainedExecutionHostUnitAdapter::ApplyOnHost(
    const manyforge::core::RuntimeUnitId &runtime_unit_id,
    const HostApplyRequest &request) {
  return impl_->ApplyOnHost(runtime_unit_id, request);
}

manyforge::core::Result<ManagerComponentProcessInventoryResultDTO>
RetainedExecutionHostUnitAdapter::InspectInstalledComponentProcesses(
    const ManagerComponentProcessInventoryRequestDTO &request) {
  return impl_->InspectInstalledComponentProcesses(request);
}


manyforge::core::ManagerLivenessObservationDTO
RetainedExecutionHostUnitAdapter::RenewManagerLiveness(
    const ManagerLivenessTransportRequestDTO &request) {
  return impl_->RenewManagerLiveness(request);
}

std::vector<manyforge::core::ManagerLivenessObservationDTO>
RetainedExecutionHostUnitAdapter::RenewManagerLivenessBatch(
    const std::vector<ManagerLivenessTransportRequestDTO> &requests) {
  return impl_->RenewManagerLivenessBatch(requests);
}

} // namespace manyforge::component_manager
