#include "manyforge_component_manager/retained_native_runtime_unit_adapter.hpp"

#include "kernel_apply_fence_rpc_internal.hpp"
#include "manager_liveness_rpc_internal.hpp"
#include "manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp"
#include "native_projection_rpc_internal.hpp"
#include "selected_runtime_composition_rpc_internal.hpp"
#include <manyforge_deployment/apply_affected_scope.hpp>

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
#include <charconv>
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
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

extern char **environ;

namespace manyforge::component_manager {
namespace {

using manyforge::core::RuntimeEpoch;
using manyforge::deployment::RetainedBundle;

using ProjectionBindingKey =
    std::pair<manyforge::core::CapabilityProviderBindingId,
              manyforge::core::CapabilityProviderBindingRevision>;

constexpr int kChildChannelFd = 3;
constexpr int kChildLivenessChannelFd = 4;
constexpr int kChildExecErrorFd = 5;
constexpr int kChildPackageRootFd = 6;
constexpr int kChildExecutableFd = 7;
constexpr int kChildHighestReservedFd = kChildExecutableFd;
constexpr std::size_t kMaximumControlPacketBytes =
    detail::kMaximumManagedRuntimeRpcPacketBytes;
static_assert(detail::kMaximumNativeProjectionRpcPacketBytes <=
              kMaximumControlPacketBytes);
static_assert(detail::kMaximumManagerLivenessRpcPacketBytes <=
              kMaximumControlPacketBytes);
constexpr auto kExecTimeout = std::chrono::seconds(10);
constexpr auto kReadyTimeout = std::chrono::seconds(10);
constexpr auto kStatusTimeout = std::chrono::milliseconds(750);
constexpr auto kManagerLivenessTimeout = std::chrono::milliseconds(750);
// Cold projection preparation and the first candidate Stage may materialize
// the complete immutable planning/collision product. Keep those effects
// bounded independently from the short steady-state status, fence,
// composition, and projection-query budget.
constexpr auto kProjectionPreparationTimeout = std::chrono::seconds(10);
constexpr auto kGracefulStopTimeout = std::chrono::seconds(2);
constexpr auto kTermStopTimeout = std::chrono::seconds(1);

[[noreturn]] void Fail(const std::string &detail) {
  throw RetainedNativeRuntimeUnitAdapterError(detail);
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

void RequirePathOutsideDirectory(const std::filesystem::path &path,
                                 const std::filesystem::path &directory) {
  if (path.empty()) {
    return;
  }
  std::error_code error;
  const auto canonical_parent =
      std::filesystem::canonical(path.parent_path(), error);
  if (error) {
    Fail("native durable path parent is unavailable");
  }
  const auto canonical_path = canonical_parent / path.filename();
  const auto canonical_directory = std::filesystem::canonical(directory, error);
  if (error) {
    Fail("native disposable run capability is unavailable");
  }
  const auto mismatch =
      std::mismatch(canonical_directory.begin(), canonical_directory.end(),
                    canonical_path.begin(), canonical_path.end());
  if (mismatch.first == canonical_directory.end()) {
    Fail("native durable paths must be outside disposable run storage");
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
    Fail(ErrnoDetail("could not create native runtime private channel", errno));
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
      Fail(ErrnoDetail("could not configure native runtime packet buffers",
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
      Fail("native runtime packet buffers are below the atomic ceiling");
    }
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
  std::string bytes(kMaximumControlPacketBytes, '\0');
  iovec vector{bytes.data(), bytes.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  ssize_t count = -1;
  do {
    count = ::recvmsg(fd, &message, 0);
  } while (count < 0 && errno == EINTR);
  if (count < 0) {
    Fail(ErrnoDetail("native runtime control read failed", errno));
  }
  if (count == 0) {
    Fail("native runtime child closed its private channel");
  }
  if ((message.msg_flags & MSG_TRUNC) != 0) {
    Fail("native runtime control response exceeds its byte bound");
  }
  bytes.resize(static_cast<std::size_t>(count));
  return bytes;
}

[[nodiscard]] std::string ReceiveReadyPacket(const int fd) {
  std::string bytes(kMaximumControlPacketBytes, '\0');
  iovec vector{bytes.data(), bytes.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  ssize_t count = -1;
  do {
    count = ::recvmsg(fd, &message, MSG_DONTWAIT);
  } while (count < 0 && errno == EINTR);
  if (count < 0) {
    Fail(ErrnoDetail("native runtime control read failed", errno));
  }
  if (count == 0) {
    Fail("native runtime child closed its private channel");
  }
  if ((message.msg_flags & MSG_TRUNC) != 0) {
    Fail("native runtime control response exceeds its byte bound");
  }
  bytes.resize(static_cast<std::size_t>(count));
  return bytes;
}

void WaitForAll(std::vector<pollfd> &descriptors, const short event,
                const std::chrono::steady_clock::time_point deadline) {
  std::vector<bool> ready(descriptors.size(), false);
  std::size_t remaining = descriptors.size();
  while (remaining != 0U) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      Fail("native runtime liveness batch timed out");
    }
    const auto timeout = std::max<std::int64_t>(
        1, std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
               .count());
    for (std::size_t index = 0U; index < descriptors.size(); ++index) {
      descriptors[index].events =
          ready[index] ? 0 : static_cast<short>(event | POLLHUP);
      descriptors[index].revents = 0;
    }
    int result = -1;
    do {
      result =
          ::poll(descriptors.data(), descriptors.size(),
                 static_cast<int>(std::min<std::int64_t>(timeout, INT_MAX)));
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
      Fail(ErrnoDetail("native runtime liveness batch poll failed", errno));
    }
    if (result == 0) {
      Fail("native runtime liveness batch timed out");
    }
    for (std::size_t index = 0U; index < descriptors.size(); ++index) {
      const auto observed = descriptors[index].revents;
      if ((observed & (POLLERR | POLLNVAL)) != 0) {
        Fail("native runtime liveness batch channel became invalid");
      }
      if (!ready[index] && (observed & (event | POLLHUP)) != 0) {
        ready[index] = true;
        --remaining;
      }
    }
  }
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
    const NativeExecutionRuntimeUnitAdmission &admission,
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
SameChildProduct(const NativeExecutionRuntimeUnitAdmission &admission,
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
                             char *const argv[]) noexcept {
  if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0) {
    ChildExit(error_fd, errno);
  }
  if (::getppid() != expected_parent) {
    ChildExit(error_fd, ECHILD);
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
  if (channel_copy < 0 || liveness_channel_copy < 0 || error_copy < 0 ||
      executable_copy < 0 || package_root_copy < 0 ||
      ::dup3(channel_copy, kChildChannelFd, 0) < 0 ||
      ::dup3(liveness_channel_copy, kChildLivenessChannelFd, 0) < 0 ||
      ::dup3(error_copy, kChildExecErrorFd, O_CLOEXEC) < 0 ||
      ::dup3(executable_copy, kChildExecutableFd, O_CLOEXEC) < 0 ||
      ::dup3(package_root_copy, kChildPackageRootFd, 0) < 0) {
    ChildExit(error_fd, errno);
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
    std::string runtime_contract_offer_digest;
    std::set<ProjectionBindingKey> admitted_projection_bindings;
    std::map<ProjectionBindingKey, ProjectionPreparation>
        prepared_projection_products;
    std::optional<manyforge::core::ManagedRuntimeWithdrawalDTO>
        predecessor_withdrawal;

    Child(pid_t child_pid, RuntimeEpoch child_epoch, UniqueFd child_channel,
          std::string selected_product_identity,
          std::string selected_configuration_identity,
          std::string selected_runtime_contract_offer_digest,
          std::set<ProjectionBindingKey> selected_projection_bindings)
        : pid(child_pid), runtime_epoch(std::move(child_epoch)),
          channel(std::move(child_channel)),
          product_identity(std::move(selected_product_identity)),
          configuration_identity(std::move(selected_configuration_identity)),
          runtime_contract_offer_digest(
              std::move(selected_runtime_contract_offer_digest)),
          admitted_projection_bindings(
              std::move(selected_projection_bindings)) {}

    Child(const Child &) = delete;
    Child &operator=(const Child &) = delete;
    Child(Child &&) noexcept = default;
    Child &operator=(Child &&) noexcept = default;
  };

  struct LivenessEndpoint final {
    pid_t pid;
    RuntimeEpoch runtime_epoch;
    UniqueFd channel;

    LivenessEndpoint(pid_t child_pid, RuntimeEpoch child_epoch,
                     UniqueFd child_channel)
        : pid(child_pid), runtime_epoch(std::move(child_epoch)),
          channel(std::move(child_channel)) {}

    LivenessEndpoint(const LivenessEndpoint &) = delete;
    LivenessEndpoint &operator=(const LivenessEndpoint &) = delete;
    LivenessEndpoint(LivenessEndpoint &&) noexcept = default;
    LivenessEndpoint &operator=(LivenessEndpoint &&) noexcept = default;
  };

  struct AdmissionCacheEntry final {
    RuntimeUnitConvergenceQueryDTO query;
    std::shared_ptr<const RetainedBundle> bundle;
    NativeExecutionRuntimeUnitAdmission admission;
  };

  Impl(std::filesystem::path trusted_installed_target_root,
       std::filesystem::path trusted_package_root,
       std::filesystem::path manager_state_root,
       std::filesystem::path manager_run_root,
       std::filesystem::path execution_state_root,
       std::filesystem::path execution_run_root,
       const manyforge::deployment::IVerifiedRetainedBundleSource
           &selected_bundle_source,
       const manyforge::deployment::IVerifiedRetainedProductArtifactSource
           &selected_artifact_source,
       std::string selected_operating_system,
       std::vector<std::string> selected_launch_environment_contracts,
       std::filesystem::path logical_scene_database,
       std::filesystem::path program_cycle_journal,
       std::filesystem::path authoring_transaction_journal,
       std::filesystem::path candidate_package_root)
      : target_root_path(
            NormalizedPlainDirectory(std::move(trusted_installed_target_root),
                                     "trusted installed-target root")),
        package_root_path(NormalizedPlainDirectory(
            std::move(trusted_package_root), "trusted retained-package root")),
        package_root_fd(OpenPinnedPackageRoot(package_root_path)),
        candidate_package_root_path(
            candidate_package_root.empty()
                ? std::filesystem::path{}
                : NormalizedPlainDirectory(
                      std::move(candidate_package_root),
                      "candidate retained-package root")),
        manager_state_root_path(NormalizedPlainDirectory(
            std::move(manager_state_root), "Manager state root")),
        manager_run_root_path(NormalizedPlainDirectory(
            std::move(manager_run_root), "Manager run root")),
        execution_state_root_path(NormalizedPlainDirectory(
            std::move(execution_state_root), "native execution state root")),
        execution_run_root_path(NormalizedPlainDirectory(
            std::move(execution_run_root), "native execution run root")),
        logical_scene_database_path(std::move(logical_scene_database)),
        program_cycle_journal_path(std::move(program_cycle_journal)),
        authoring_transaction_journal_path(
            std::move(authoring_transaction_journal)),
        bundle_source(selected_bundle_source),
        artifact_source(selected_artifact_source),
        operating_system(std::move(selected_operating_system)),
        launch_environment_contracts(
            std::move(selected_launch_environment_contracts)),
        observer_instance_id("native-observer-" + RandomHex()) {
    const std::array<const std::filesystem::path *, 4U> typed_roots{
        &manager_state_root_path, &manager_run_root_path,
        &execution_state_root_path, &execution_run_root_path};
    RequirePrivateDirectory(manager_state_root_path, "Manager state root");
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
      if (!candidate_package_root_path.empty()) {
        RequireDisjointDirectories(candidate_package_root_path, *typed_root);
      }
    }
    if (!candidate_package_root_path.empty()) {
      RequireDisjointDirectories(package_root_path,
                                 candidate_package_root_path);
      candidate_package_root_fd =
          OpenPinnedPackageRoot(candidate_package_root_path);
    }
    const std::array<const std::filesystem::path *, 3U> durable_paths{
        &logical_scene_database_path, &program_cycle_journal_path,
        &authoring_transaction_journal_path};
    const std::array<const std::filesystem::path *, 2U> run_roots{
        &manager_run_root_path, &execution_run_root_path};
    for (const auto *durable_path : durable_paths) {
      for (const auto *run_root : run_roots) {
        RequirePathOutsideDirectory(*durable_path, *run_root);
      }
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
    struct stat status {};
    if (::fstat(target_root_fd.get(), &status) != 0 ||
        !S_ISDIR(status.st_mode)) {
      Fail("trusted installed-target root is not a directory");
    }
  }

  ~Impl() {
    std::lock_guard lock(mutex);
    for (auto &[runtime_id, child] : children) {
      EraseLivenessEndpoint(runtime_id);
      StopChildNoThrow(child);
    }
  }

  void EraseLivenessEndpoint(const std::string &runtime_id) {
    std::lock_guard lock(liveness_mutex);
    liveness_endpoints.erase(runtime_id);
  }

  void EraseChild(const std::map<std::string, Child>::iterator child) {
    EraseLivenessEndpoint(child->first);
    children.erase(child);
  }

  [[nodiscard]] std::pair<std::shared_ptr<const RetainedBundle>,
                          NativeExecutionRuntimeUnitAdmission>
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
    auto admission = VerifyNativeExecutionRuntimeUnitForHost(
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
      const NativeExecutionRuntimeUnitAdmission &admission) const {
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

  [[nodiscard]] bool
  ApprovedPredecessor(const RuntimeUnitConvergenceQueryDTO &query,
                      const RetainedBundle &candidate, const Child &child) {
    if (!query.replacement)
      return false;
    const auto predecessor = bundle_source.get_verified(
        query.replacement->expected_old_retained_bundle_id);
    if (!predecessor ||
        predecessor->value().cell_graph.graph_revision !=
            query.replacement->expected_old_graph_revision ||
        manyforge::deployment::derive_apply_configuration_fence_scope(
            *predecessor, candidate) != query.replacement->persisted_scope)
      Fail("native replacement does not carry the exact whole-Apply scope");
    const auto old = VerifyNativeExecutionRuntimeUnitForHost(
        *predecessor, query.plan.runtime_unit_id, artifact_source,
        operating_system, launch_environment_contracts);
    return old &&
           old->product().product_contract_id() ==
               query.plan.product_contract_id &&
           old->product().product_contract_revision() ==
               query.plan.product_contract_revision &&
           old->runtime_contract_offer_digest() ==
               query.plan.runtime_contract_offer_digest.value() &&
           SameChildProduct(*old, child.product_identity,
                            child.configuration_identity,
                            child.runtime_contract_offer_digest);
  }

  void RequireHeldReplacementFence(
      Child &child,
      const manyforge::core::ApplyFenceRehydrateDTO &replacement) {
    const auto held =
        ExchangeKernelFence<manyforge::core::ApplyFenceReconcileResultDTO>(
            child, replacement);
    if (!held.matches(replacement) || held.admission_fenced || held.error ||
        held.execution_host_epoch.runtime_epoch != child.runtime_epoch ||
        held.fence_results.size() != 1U ||
        held.fence_results.front().state !=
            manyforge::core::ApplyFenceReconcileState::kMatched)
      Fail("native replacement requires the current exact held Apply fence");
    const manyforge::core::ConfigurationFenceAdmissionQueryDTO query{
        replacement.persisted_scope, replacement.manager_runtime_epoch};
    const auto idle = ExchangeKernelFence<
        manyforge::core::ConfigurationFenceAdmissionSnapshotDTO>(child, query);
    if (!idle.matches(query) ||
        idle.execution_host_epoch.runtime_epoch != child.runtime_epoch ||
        idle.affected_active_operation_count != 0U ||
        idle.affected_queued_operation_count != 0U)
      Fail("native replacement requires the current exact held idle Apply "
           "fence");
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
      Child &child, const detail::NativeProjectionRpcRequest &request,
      const std::chrono::milliseconds timeout = kStatusTimeout) {
    const auto encoded = detail::EncodeNativeProjectionRpcRequest(request);
    SendPacket(child.channel.get(), encoded, timeout);
    const auto packet = ReceivePacket(child.channel.get(), timeout);
    if (!packet.has_value()) {
      // A late response on this untagged request/reply channel could otherwise
      // be consumed as the reply to a later command.  Once a request times
      // out, the stream is no longer usable: stop this exact child and let
      // normal convergence create a fresh epoch.
      child.channel.Reset();
      StopChildNoThrow(child);
      Fail("native projection response timed out");
    }
    auto response = detail::DecodeNativeProjectionRpcResponse(*packet);
    return response;
  }

  [[nodiscard]] detail::NativeProjectionRpcResponse
  ExchangeProjection(Child &child,
                     const detail::NativeProjectionRpcRequest &request,
                     const std::chrono::milliseconds timeout = kStatusTimeout) {
    auto response = ExchangeProjectionRaw(child, request, timeout);
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
      detail::NativeProjectionRpcRequest request,
      const std::chrono::milliseconds timeout = kStatusTimeout) {
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
      EraseChild(found);
      Fail("native projection child exited before its response");
    }
    auto response = ExchangeProjection(found->second, request, timeout);
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
      EraseChild(found);
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
      EraseChild(found);
      Fail("managed Kernel child exited before its fence response");
    }
    return ExchangeKernelFence<Response>(found->second, request);
  }

  template <typename Response, typename Request>
  [[nodiscard]] Response ExchangeKernelFence(Child &child,
                                             const Request &request) {
    const auto encoded = detail::EncodeKernelApplyFenceRpcRequest(
        detail::KernelApplyFenceRpcRequest(request));
    SendPacket(child.channel.get(), encoded, kStatusTimeout);
    const auto packet = ReceivePacket(child.channel.get(), kStatusTimeout);
    if (!packet.has_value()) {
      child.channel.Reset();
      StopChildNoThrow(child);
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

  template <typename Response>
  [[nodiscard]] manyforge::core::Result<Response>
  KernelCellAdmissionCall(const detail::KernelCellAdmissionRpcPacket &request) {
    std::lock_guard lock(mutex);
    if (children.size() != 1U) {
      return manyforge::core::Result<Response>::Err(
          {"kernel_owner_unavailable",
           "cell-admission request requires the sole managed native execution "
           "child",
           {{"reason", "native_execution_child_unavailable"}}});
    }
    auto found = children.begin();
    if (!ChildIsLive(found->second)) {
      EraseChild(found);
      return manyforge::core::Result<Response>::Err(
          {"kernel_owner_unavailable",
           "managed Kernel child exited before its cell-admission response",
           {{"reason", "native_execution_child_exited"}}});
    }
    const auto encoded = detail::EncodeKernelCellAdmissionRpcRequest(request);
    SendPacket(found->second.channel.get(), encoded, kStatusTimeout);
    const auto packet =
        ReceivePacket(found->second.channel.get(), kStatusTimeout);
    if (!packet.has_value()) {
      found->second.channel.Reset();
      StopChildNoThrow(found->second);
      Fail("managed Kernel cell-admission response timed out");
    }
    const auto response = detail::DecodeKernelCellAdmissionRpcResponse(*packet);
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

  [[nodiscard]] ManagerSelectedRuntimeCompositionResultDTO
  InstallOrObserveRuntimeComposition(
      const ManagerSelectedRuntimeCompositionRequestDTO &request) {
    if (!request.valid()) {
      Fail("selected runtime-composition request is invalid");
    }
    std::lock_guard lock(mutex);
    const auto found = children.find(request.runtime_unit_id.value());
    if (found == children.end() ||
        found->second.runtime_epoch != request.runtime_epoch) {
      Fail("selected runtime composition no longer names the current child "
           "epoch");
    }
    if (!ChildIsLive(found->second)) {
      EraseChild(found);
      Fail("managed runtime child exited before its composition response");
    }
    const auto encoded =
        detail::EncodeSelectedRuntimeCompositionRpcRequest(request);
    // On first convergence the child may be completing the bounded retained
    // CCLC recovery before it can service the ordinary command channel.  This
    // is cold composition work, not a steady-state status exchange.
    SendPacket(found->second.channel.get(), encoded,
               std::chrono::duration_cast<std::chrono::milliseconds>(
                   kProjectionPreparationTimeout));
    const auto packet =
        ReceivePacket(found->second.channel.get(),
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          kProjectionPreparationTimeout));
    if (!packet.has_value()) {
      found->second.channel.Reset();
      StopChildNoThrow(found->second);
      Fail("managed runtime-composition response timed out");
    }
    auto response =
        detail::DecodeSelectedRuntimeCompositionRpcResponse(*packet);
    if (const auto *failure =
            std::get_if<detail::SelectedRuntimeCompositionRpcFailure>(
                &response)) {
      Fail("managed runtime-composition request failed [" +
           failure->error.code + "]: " + failure->error.message);
    }
    const auto *result =
        std::get_if<ManagerSelectedRuntimeCompositionResultDTO>(&response);
    if (result == nullptr || !result->valid() || result->request != request) {
      Fail("managed runtime child returned foreign composition evidence");
    }
    return *result;
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
    const auto publication_generation =
        requests.front().active_publication_generation;
    const auto graph_revision = requests.front().active_graph_revision;
    const auto public_pointer_digest =
        requests.front().active_public_pointer_digest;
    std::set<std::string> requested_receivers;
    std::vector<std::string> encoded;
    encoded.reserve(requests.size());
    for (const auto &request : requests) {
      if (!manyforge::core::ValidateManagerLivenessRequiredSet(
               request.required_set)
               .ok() ||
          request.required_set != required_set ||
          request.heartbeat != heartbeat || request.policy != policy ||
          request.active_publication_generation != publication_generation ||
          request.active_graph_revision != graph_revision ||
          request.active_public_pointer_digest != public_pointer_digest ||
          !requested_receivers.insert(request.receiver_runtime_unit_id.value())
               .second) {
        Fail("Manager-liveness batch is not one exact required-set renewal");
      }
      encoded.push_back(detail::EncodeManagerLivenessRpcRequest(request));
    }
    // The required set may also contain same-host external receivers (for
    // example Program Runner) renewed by another strict transport in the same
    // Manager generation.  This adapter must atomically cover every requested
    // native child, while preserving the complete set in every native packet.

    if (!manyforge::core::ValidateManagerLivenessRequiredSet(required_set)
             .ok()) {
      Fail("Manager-liveness request has an invalid required set");
    }
    std::lock_guard lock(liveness_mutex);
    // The response budget starts only once this batch owns the dedicated
    // liveness channels. Ordinary control and Apply traffic cannot consume it.
    const detail::ManagerLivenessBatchDeadline deadline(
        std::chrono::steady_clock::now(), kManagerLivenessTimeout);
    std::vector<LivenessEndpoint *> targets;
    std::vector<pollfd> descriptors;
    targets.reserve(requests.size());
    descriptors.reserve(requests.size());
    for (const auto &request : requests) {
      const auto expected = std::find_if(
          required_set.required_receivers.begin(),
          required_set.required_receivers.end(), [&](const auto &row) {
            return row.runtime_unit_id == request.receiver_runtime_unit_id;
          });
      auto found =
          liveness_endpoints.find(request.receiver_runtime_unit_id.value());
      if (expected == required_set.required_receivers.end() ||
          found == liveness_endpoints.end() ||
          found->second.runtime_epoch != expected->runtime_epoch) {
        Fail(
            "Manager-liveness request no longer names the current child epoch");
      }
      targets.push_back(&found->second);
      descriptors.push_back({found->second.channel.get(), 0, 0});
    }

    WaitForAll(descriptors, POLLOUT, deadline.value());
    for (std::size_t index = 0U; index < targets.size(); ++index) {
      const auto count =
          ::send(targets[index]->channel.get(), encoded[index].data(),
                 encoded[index].size(), MSG_NOSIGNAL | MSG_DONTWAIT);
      if (count != static_cast<ssize_t>(encoded[index].size())) {
        Fail(count < 0
                 ? ErrnoDetail("native runtime liveness batch write failed",
                               errno)
                 : "native runtime liveness batch packet was not sent "
                   "atomically");
      }
    }
    WaitForAll(descriptors, POLLIN, deadline.value());
    std::vector<std::string> packets;
    packets.reserve(targets.size());
    for (const auto *target : targets) {
      packets.push_back(ReceiveReadyPacket(target->channel.get()));
    }
    std::vector<manyforge::core::ManagerLivenessObservationDTO> responses;
    responses.reserve(targets.size());
    std::size_t superseded_count = 0U;
    std::exception_ptr first_other_failure;
    for (const auto &packet : packets) {
      try {
        responses.push_back(detail::DecodeManagerLivenessRpcResponse(packet));
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
    if (superseded_count == packets.size()) {
      throw ManagerLivenessPublicationSuperseded();
    }
    if (superseded_count != 0U || first_other_failure != nullptr ||
        responses.size() != packets.size()) {
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
    // legitimately take longer than the Manager lease renewal interval.
    std::lock_guard lock(liveness_mutex);
    if (liveness_endpoints.size() != 1U) {
      return manyforge::core::Result<Response>::Err(
          {"kernel_owner_unavailable",
           "Manager liveness/readiness requires the sole managed native "
           "execution child",
           {{"reason", "native_execution_child_unavailable"}}});
    }
    auto &endpoint = liveness_endpoints.begin()->second;
    const auto encoded = detail::EncodeKernelCellAdmissionRpcRequest(request);
    SendPacket(endpoint.channel.get(), encoded, kManagerLivenessTimeout);
    const auto packet =
        ReceivePacket(endpoint.channel.get(), kManagerLivenessTimeout);
    if (!packet.has_value()) {
      Fail("managed Kernel liveness/readiness response timed out");
    }
    const auto response = detail::DecodeKernelCellAdmissionRpcResponse(*packet);
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
          previous->second.graph_revision == request.candidate_graph_revision &&
          previous->second.planning_collision_model ==
              request.planning_collision_model) {
        continue;
      }
      auto response = ExchangeProjection(
          child, request,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              kProjectionPreparationTimeout));
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
          key,
          Child::ProjectionPreparation{request.candidate_retained_bundle_id,
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
    auto target = OpenInstalledTarget(admission);
    auto channels = MakePacketPair();
    auto liveness_channels = MakePacketPair();
    auto exec_pipe = MakePipe();
    SetNonBlocking(channels[0].get());
    SetNonBlocking(liveness_channels[0].get());
    const RuntimeEpoch runtime_epoch("native-runtime-" + RandomHex());
    const auto launch_arguments =
        admission.launch_arguments(kChildChannelFd, runtime_epoch);
    std::vector<std::string> arguments;
    arguments.reserve(launch_arguments.size() + 25U);
    arguments.push_back(admission.installed_target_id());
    arguments.insert(arguments.end(), launch_arguments.begin(),
                     launch_arguments.end());
    arguments.emplace_back("--manyforge-managed-package-root-fd");
    arguments.push_back(std::to_string(kChildPackageRootFd));
    arguments.emplace_back("--manyforge-managed-liveness-channel-fd");
    arguments.push_back(std::to_string(kChildLivenessChannelFd));
    arguments.emplace_back("--manyforge-managed-manager-state-root");
    arguments.push_back(manager_state_root_path.string());
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
    if (!logical_scene_database_path.empty()) {
      arguments.emplace_back("--manyforge-managed-logical-scene-database");
      arguments.push_back(logical_scene_database_path.string());
    }
    if (!program_cycle_journal_path.empty()) {
      arguments.emplace_back("--manyforge-managed-program-cycle-journal");
      arguments.push_back(program_cycle_journal_path.string());
    }
    if (!authoring_transaction_journal_path.empty()) {
      arguments.emplace_back(
          "--manyforge-managed-authoring-transaction-journal");
      arguments.push_back(authoring_transaction_journal_path.string());
    }
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
      EnterChild(parent, channels[1].get(), liveness_channels[1].get(),
                 exec_pipe[1].get(), target.get(),
                 PackageRootFd(bundle.retained_bundle_id()),
                 argv.data());
    }

    channels[1].Reset();
    liveness_channels[1].Reset();
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
      liveness_channels[0].Reset();
      KillAndReap(pid);
      throw;
    }
    Child child(pid, runtime_epoch, std::move(channels[0]),
                admission.product().product_identity().value(),
                admission.product().configuration_identity().value(),
                admission.runtime_contract_offer_digest(), {});
    {
      std::lock_guard lock(liveness_mutex);
      const auto [_, inserted] = liveness_endpoints.emplace(
          admission.product().runtime_unit_id().value(),
          LivenessEndpoint(pid, runtime_epoch,
                           std::move(liveness_channels[0])));
      if (!inserted) {
        StopChildNoThrow(child);
        Fail("native runtime liveness endpoint insertion conflicted");
      }
    }
    try {
      PrepareChildProjectionBindings(child, bundle,
                                     admission.product().runtime_unit_id());
    } catch (...) {
      EraseLivenessEndpoint(admission.product().runtime_unit_id().value());
      StopChildNoThrow(child);
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
      const auto path = root / "retained-bundles" / "sha256" /
                        (digest.substr(7U) + ".json");
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

    // Returning to an installed revision still needs the authored package's
    // retained predecessor closure during fenced replacement recovery.
    // The child independently verifies the exact bundle in the selected root.
    if (!candidate_package_root_path.empty() &&
        contains_bundle(candidate_package_root_path, "authored")) {
      return candidate_package_root_fd.get();
    }
    return package_root_fd.get();
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
                          found->second.runtime_contract_offer_digest)) {
      if (ChildIsLive(found->second)) {
        if (!ApprovedPredecessor(query, *bundle, found->second))
          Fail("live native runtime child differs from the selected product: " +
               query.plan.runtime_unit_id.value() + " at " +
               found->second.runtime_epoch.value() + "; expected product=" +
               admission.product().product_identity().value() +
               ", configuration=" +
               admission.product().configuration_identity().value() +
               ", offer=" + admission.runtime_contract_offer_digest() +
               "; observed product=" + found->second.product_identity +
               ", configuration=" + found->second.configuration_identity +
               ", offer=" + found->second.runtime_contract_offer_digest);
        // Presence is not candidate readiness. EnsureRunning must replace
        // this exact predecessor before a candidate source report is usable.
        return Presence(query, ProcessPresenceState::kPresent,
                        found->second.runtime_epoch.value());
      }
      EraseChild(found);
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    if (!ChildIsLive(found->second)) {
      EraseChild(found);
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
    std::optional<manyforge::core::ManagedRuntimeWithdrawalDTO> withdrawal;
    auto found = children.find(runtime_id);
    if (found != children.end()) {
      if (!SameChildProduct(admission, found->second.product_identity,
                            found->second.configuration_identity,
                            found->second.runtime_contract_offer_digest)) {
        if (ChildIsLive(found->second)) {
          if (!ApprovedPredecessor(query, *bundle, found->second))
            Fail("cannot replace a live native child with another product");
          RequireHeldReplacementFence(found->second, *query.replacement);
          const auto &owner =
              std::get<RuntimeUnitConvergenceApplyOwnerDTO>(query.owner);
          withdrawal = manyforge::core::ManagedRuntimeWithdrawalDTO{
              owner.apply_id,
              owner.apply_generation,
              query.runtime_convergence_intent_digest,
              query.plan.runtime_unit_id,
              found->second.runtime_epoch,
              manyforge::core::ContentDigest(found->second.product_identity),
              manyforge::core::ContentDigest(
                  found->second.configuration_identity),
              observer_instance_id,
              0U};
          StopChild(found->second);
          withdrawal->process_observation_sequence = NextObservationSequence();
        }
        EraseChild(found);
      } else if (ChildIsLive(found->second)) {
        PrepareChildProjectionBindings(found->second, *bundle,
                                       query.plan.runtime_unit_id);
        return Presence(query, ProcessPresenceState::kPresent,
                        found->second.runtime_epoch.value());
      } else {
        EraseChild(found);
      }
    }
    auto launched = LaunchChild(admission, *bundle);
    // One receipt travels with the replacement, not an accumulating registry.
    // A failed launch or a crash before the journal records it stays unknown.
    launched.predecessor_withdrawal = std::move(withdrawal);
    auto [inserted, unique] =
        children.try_emplace(runtime_id, std::move(launched));
    if (!unique) {
      EraseLivenessEndpoint(runtime_id);
      StopChildNoThrow(launched);
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
                          found->second.runtime_contract_offer_digest)) {
      Fail("native runtime stop query differs from the tracked child");
    }
    StopChild(found->second);
    EraseChild(found);
    return Presence(query, ProcessPresenceState::kAbsent);
  }

  [[nodiscard]] std::optional<manyforge::core::ManagedRuntimeWithdrawalDTO>
  ReadPredecessorWithdrawal(const RuntimeUnitConvergenceQueryDTO &query) {
    auto [bundle, admission] = Admission(query);
    static_cast<void>(bundle);
    std::lock_guard lock(mutex);
    const auto found = children.find(query.plan.runtime_unit_id.value());
    const auto *owner =
        std::get_if<RuntimeUnitConvergenceApplyOwnerDTO>(&query.owner);
    if (owner == nullptr || found == children.end() ||
        !SameChildProduct(admission, found->second.product_identity,
                          found->second.configuration_identity,
                          found->second.runtime_contract_offer_digest))
      return std::nullopt;
    const auto &receipt = found->second.predecessor_withdrawal;
    if (!receipt || !receipt->valid() || receipt->apply_id != owner->apply_id ||
        receipt->apply_generation != owner->apply_generation ||
        receipt->runtime_convergence_intent_digest !=
            query.runtime_convergence_intent_digest)
      return std::nullopt;
    return receipt;
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
                          found->second.runtime_contract_offer_digest)) {
      Fail("native runtime progress query differs from the tracked child");
    }
    if (!ChildIsLive(found->second)) {
      EraseChild(found);
      return std::nullopt;
    }
    SendPacket(found->second.channel.get(), "status", kStatusTimeout);
    const auto packet =
        ReceivePacket(found->second.channel.get(),
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          kProjectionPreparationTimeout));
    if (!packet.has_value()) {
      found->second.channel.Reset();
      StopChildNoThrow(found->second);
      EraseChild(found);
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
        std::vector<std::string>{
            std::string(admission.product().runtime_role_id())},
        ManagedCommandSurfaceState::kClosedNoActiveEffect, {},
        admission.runtime_contract_offer_digest());
  }

  [[nodiscard]] manyforge::core::Result<
      std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
  ResolveProjection(
      const manyforge::core::ProjectionNativeRuntimeBindingDTO &binding);

  std::filesystem::path target_root_path;
  std::filesystem::path package_root_path;
  UniqueFd package_root_fd;
  std::filesystem::path candidate_package_root_path;
  UniqueFd candidate_package_root_fd;
  std::filesystem::path manager_state_root_path;
  std::filesystem::path manager_run_root_path;
  std::filesystem::path execution_state_root_path;
  std::filesystem::path execution_run_root_path;
  std::filesystem::path logical_scene_database_path;
  std::filesystem::path program_cycle_journal_path;
  std::filesystem::path authoring_transaction_journal_path;
  const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source;
  const manyforge::deployment::IVerifiedRetainedProductArtifactSource
      &artifact_source;
  std::string operating_system;
  std::vector<std::string> launch_environment_contracts;
  std::string observer_instance_id;
  UniqueFd target_root_fd;
  std::mutex admission_mutex;
  std::mutex mutex;
  std::mutex liveness_mutex;
  std::uint64_t observation_sequence{};
  std::map<std::string, AdmissionCacheEntry> admission_cache;
  std::map<std::string, Child> children;
  std::map<std::string, LivenessEndpoint> liveness_endpoints;
};

class RetainedNativeRuntimeUnitAdapter::Impl::ProjectionClient final
    : public manyforge::core::IPlannerSceneProjection {
public:
  ProjectionClient(std::shared_ptr<Impl> adapter,
                   manyforge::core::ProjectionNativeRuntimeBindingDTO binding)
      : adapter_(std::move(adapter)), binding_(std::move(binding)) {}

  [[noreturn]] static void Unavailable(const std::string_view operation) {
    Fail("native projection " + std::string(operation) +
         " is unavailable before the durable transition checkpoint");
  }

  manyforge::core::ProjectionCommitResultDTO ApplyTransaction(
      const manyforge::core::SceneTransactionDTO &transaction) override {
    return adapter_->ProjectionCall<manyforge::core::ProjectionCommitResultDTO>(
        binding_,
        detail::NativeProjectionTransactionRequest{binding_, transaction});
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
      const manyforge::core::NativeProjectionBootstrapStatusQueryDTO &request)
      const override {
    return adapter_
        ->ProjectionCall<manyforge::core::NativeProjectionBootstrapStatusDTO>(
            binding_, request);
  }

  manyforge::core::NativeProjectionStageResultDTO StageProjection(
      const manyforge::core::NativeProjectionStageDTO &request) override {
    return adapter_
        ->ProjectionCall<manyforge::core::NativeProjectionStageResultDTO>(
            binding_, request,
            std::chrono::duration_cast<std::chrono::milliseconds>(
                kProjectionPreparationTimeout));
  }

  manyforge::core::NativeProjectionActivationResultDTO ActivateStagedProjection(
      const manyforge::core::NativeProjectionActivationDTO &request) override {
    return adapter_
        ->ProjectionCall<manyforge::core::NativeProjectionActivationResultDTO>(
            binding_, request);
  }

  manyforge::core::NativeProjectionRetireResultDTO RetireProjection(
      const manyforge::core::NativeProjectionRetireDTO &request) override {
    return adapter_
        ->ProjectionCall<manyforge::core::NativeProjectionRetireResultDTO>(
            binding_, request);
  }

  manyforge::core::NativeProjectionRetireStatusDTO ProjectionRetireStatus(
      const manyforge::core::NativeProjectionRetireStatusQueryDTO &request)
      const override {
    return adapter_
        ->ProjectionCall<manyforge::core::NativeProjectionRetireStatusDTO>(
            binding_, request);
  }

  manyforge::core::NativeProjectionDiscardResultDTO DiscardStagedProjection(
      const manyforge::core::NativeProjectionDiscardDTO &request) override {
    return adapter_
        ->ProjectionCall<manyforge::core::NativeProjectionDiscardResultDTO>(
            binding_, request);
  }

  manyforge::core::NativeProjectionStatusDTO ProjectionStageStatus(
      const manyforge::core::NativeProjectionStatusQueryDTO &request)
      const override {
    return adapter_->ProjectionCall<manyforge::core::NativeProjectionStatusDTO>(
        binding_, request);
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

private:
  std::shared_ptr<Impl> adapter_;
  manyforge::core::ProjectionNativeRuntimeBindingDTO binding_;
};

manyforge::core::Result<
    std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
RetainedNativeRuntimeUnitAdapter::Impl::ResolveProjection(
    const manyforge::core::ProjectionNativeRuntimeBindingDTO &binding) {
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
    const auto found = children.find(binding.runtime_unit_id.value());
    if (found == children.end() ||
        found->second.runtime_epoch != binding.runtime_epoch) {
      return missing("no exact Manager child owns the native runtime epoch");
    }
    if (!ChildIsLive(found->second)) {
      EraseChild(found);
      return missing("the exact Manager child is no longer live");
    }
    const ProjectionBindingKey key{binding.provider_binding_id,
                                   binding.provider_binding_revision};
    if (!found->second.admitted_projection_bindings.contains(key)) {
      return missing(
          "the verified child product does not own the projection binding");
    }
  }
  return manyforge::core::
      Result<std::shared_ptr<manyforge::core::IPlannerSceneProjection>>::Ok(
          std::make_shared<ProjectionClient>(shared_from_this(), binding));
}

RetainedNativeRuntimeUnitAdapter::RetainedNativeRuntimeUnitAdapter(
    std::filesystem::path trusted_installed_target_root,
    std::filesystem::path trusted_package_root,
    std::filesystem::path manager_state_root,
    std::filesystem::path manager_run_root,
    std::filesystem::path execution_state_root,
    std::filesystem::path execution_run_root,
    const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string operating_system, std::string launch_environment_contract,
    std::filesystem::path logical_scene_database,
    std::filesystem::path program_cycle_journal,
    std::filesystem::path authoring_transaction_journal,
    std::filesystem::path candidate_package_root)
    : RetainedNativeRuntimeUnitAdapter(
          std::move(trusted_installed_target_root),
          std::move(trusted_package_root), std::move(manager_state_root),
          std::move(manager_run_root), std::move(execution_state_root),
          std::move(execution_run_root), bundle_source, artifact_source,
          std::move(operating_system),
          std::vector<std::string>{std::move(launch_environment_contract)},
          std::move(logical_scene_database), std::move(program_cycle_journal),
          std::move(authoring_transaction_journal),
          std::move(candidate_package_root)) {}

RetainedNativeRuntimeUnitAdapter::RetainedNativeRuntimeUnitAdapter(
    std::filesystem::path trusted_installed_target_root,
    std::filesystem::path trusted_package_root,
    std::filesystem::path manager_state_root,
    std::filesystem::path manager_run_root,
    std::filesystem::path execution_state_root,
    std::filesystem::path execution_run_root,
    const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string operating_system,
    std::vector<std::string> launch_environment_contracts,
    std::filesystem::path logical_scene_database,
    std::filesystem::path program_cycle_journal,
    std::filesystem::path authoring_transaction_journal,
    std::filesystem::path candidate_package_root)
    : impl_(std::make_shared<Impl>(
          std::move(trusted_installed_target_root),
          std::move(trusted_package_root), std::move(manager_state_root),
          std::move(manager_run_root), std::move(execution_state_root),
          std::move(execution_run_root), bundle_source, artifact_source,
          std::move(operating_system), std::move(launch_environment_contracts),
          std::move(logical_scene_database), std::move(program_cycle_journal),
          std::move(authoring_transaction_journal),
          std::move(candidate_package_root))) {}

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

std::optional<manyforge::core::ManagedRuntimeWithdrawalDTO>
RetainedNativeRuntimeUnitAdapter::ReadPredecessorWithdrawal(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->ReadPredecessorWithdrawal(query);
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

manyforge::core::AuthoringAdmissionDTO
RetainedNativeRuntimeUnitAdapter::GetAuthoringAdmission() const {
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
RetainedNativeRuntimeUnitAdapter::UpdateCellAdmissionReadiness(
    const manyforge::core::CellAdmissionReadinessSnapshotDTO &snapshot) {
  return impl_->KernelLivenessLaneCall<manyforge::core::AuthoringAdmissionDTO>(
      {"cell_admission_update_readiness", snapshot});
}

manyforge::core::Result<void> RetainedNativeRuntimeUnitAdapter::Publish(
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
RetainedNativeRuntimeUnitAdapter::AcquireCellAdmissionClaim(
    const manyforge::core::CellAdmissionClaimIntentDTO &intent) {
  return impl_->KernelCellAdmissionCall<manyforge::core::CellAdmissionClaimDTO>(
      {"cell_admission_acquire", intent});
}

manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
RetainedNativeRuntimeUnitAdapter::ReleaseCellAdmissionClaim(
    const manyforge::core::CellAdmissionClaimReleaseDTO &release) {
  return impl_->KernelCellAdmissionCall<manyforge::core::AuthoringAdmissionDTO>(
      {"cell_admission_release", release});
}

manyforge::core::Result<manyforge::core::AuthoringAdmissionDTO>
RetainedNativeRuntimeUnitAdapter::ReconcileCellAdmissionClaims(
    const manyforge::core::CellAdmissionReconcileDTO &reconcile) {
  return impl_->KernelCellAdmissionCall<manyforge::core::AuthoringAdmissionDTO>(
      {"cell_admission_reconcile", reconcile});
}

manyforge::core::Result<manyforge::core::CellAdmissionRecoveryReceiptDTO>
RetainedNativeRuntimeUnitAdapter::GetCellAdmissionRecoveryReceipt(
    const manyforge::core::CellAdmissionRecoveryReceiptQueryDTO &query) const {
  return impl_->KernelCellAdmissionCall<
      manyforge::core::CellAdmissionRecoveryReceiptDTO>(
      {"cell_admission_recovery_receipt", query});
}

manyforge::core::Result<void>
RetainedNativeRuntimeUnitAdapter::ValidateProgramResetSceneAuthorization(
    const manyforge::core::ProgramResetSceneAuthorizationDTO
        &authorization) const {
  const auto result = impl_->KernelCellAdmissionCall<std::monostate>(
      {"cell_admission_validate_program_reset_scene", authorization});
  return result.ok() ? manyforge::core::Result<void>::Ok()
                     : manyforge::core::Result<void>::Err(result.error());
}

manyforge::core::Result<
    std::shared_ptr<manyforge::core::IPlannerSceneProjection>>
RetainedNativeRuntimeUnitAdapter::ResolveProjection(
    const manyforge::core::ProjectionNativeRuntimeBindingDTO
        &native_runtime_binding) const {
  return impl_->ResolveProjection(native_runtime_binding);
}

ManagerSelectedRuntimeCompositionResultDTO
RetainedNativeRuntimeUnitAdapter::InstallOrObserve(
    const ManagerSelectedRuntimeCompositionRequestDTO &request) {
  return impl_->InstallOrObserveRuntimeComposition(request);
}

manyforge::core::ManagerLivenessObservationDTO
RetainedNativeRuntimeUnitAdapter::RenewManagerLiveness(
    const ManagerLivenessTransportRequestDTO &request) {
  return impl_->RenewManagerLiveness(request);
}

std::vector<manyforge::core::ManagerLivenessObservationDTO>
RetainedNativeRuntimeUnitAdapter::RenewManagerLivenessBatch(
    const std::vector<ManagerLivenessTransportRequestDTO> &requests) {
  return impl_->RenewManagerLivenessBatch(requests);
}

} // namespace manyforge::component_manager
