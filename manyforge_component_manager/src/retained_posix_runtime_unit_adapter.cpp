#include "manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <manyforge_component_manager/reference_runtime_unit_admission.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
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

namespace manyforge::component_manager {
namespace {

using manyforge::core::RuntimeEpoch;
using manyforge::deployment::RetainedBundle;

constexpr std::string_view kStateFormat = "manyforge.retained-posix-process.v2";
constexpr std::string_view kControlSocketName = "control.sock";
constexpr std::size_t kMaximumStateBytes = 4096U;
constexpr std::size_t kMaximumStatusLineBytes = 2048U;
constexpr std::size_t kMaximumControlPacketBytes = 4096U;
constexpr auto kSlotLockTimeout = std::chrono::seconds(2);
constexpr auto kStatusWait = std::chrono::milliseconds(500);
constexpr auto kBootstrapWait = std::chrono::seconds(2);
constexpr auto kRuntimeStopWait = std::chrono::seconds(2);
constexpr auto kRelayPollInterval = std::chrono::milliseconds(50);

[[noreturn]] void Fail(const std::string &detail) {
  throw RetainedPosixRuntimeUnitAdapterError(detail);
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
  [[nodiscard]] int Release() noexcept { return std::exchange(fd_, -1); }

  void Reset(const int replacement = -1) noexcept {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
    fd_ = replacement;
  }

private:
  int fd_{-1};
};

[[nodiscard]] std::filesystem::path
NormalizedTrustedDirectory(std::filesystem::path path,
                           const std::string_view label) {
  if (!path.is_absolute()) {
    Fail(std::string(label) + " must be an absolute directory capability");
  }
  if (path.lexically_normal() != path) {
    Fail(std::string(label) + " must use its canonical lexical spelling");
  }
  std::error_code error;
  const auto status = std::filesystem::symlink_status(path, error);
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

[[nodiscard]] UniqueFd OpenTrustedDirectory(
    const std::filesystem::path &path, const std::string_view label) {
  struct stat before {};
  if (::lstat(path.c_str(), &before) != 0 || !S_ISDIR(before.st_mode) ||
      S_ISLNK(before.st_mode)) {
    Fail(std::string(label) + " is not one plain directory");
  }
  UniqueFd descriptor(
      ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  struct stat opened {};
  struct stat after {};
  if (!descriptor || ::fstat(descriptor.get(), &opened) != 0 ||
      ::lstat(path.c_str(), &after) != 0 ||
      opened.st_dev != before.st_dev || opened.st_ino != before.st_ino ||
      after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
      !S_ISDIR(opened.st_mode) || !S_ISDIR(after.st_mode) ||
      opened.st_uid != ::geteuid() || after.st_uid != ::geteuid() ||
      (opened.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      (after.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      (opened.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) !=
          (S_IRUSR | S_IWUSR | S_IXUSR) ||
      (after.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) !=
          (S_IRUSR | S_IWUSR | S_IXUSR)) {
    Fail(std::string(label) + " identity, owner, or mode is unsafe");
  }
  return descriptor;
}

void RequireDisjointDirectoryTrees(const std::filesystem::path &left,
                                   const std::filesystem::path &right) {
  const auto mismatch = std::mismatch(left.begin(), left.end(), right.begin(),
                                      right.end());
  if (mismatch.first == left.end() || mismatch.second == right.end()) {
    Fail("retained POSIX state and run capabilities must be disjoint trees");
  }
}

void SetNonBlocking(const int fd) {
  const int flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    Fail(ErrnoDetail("could not set nonblocking I/O", errno));
  }
}

[[nodiscard]] std::array<UniqueFd, 2U> MakePipe() {
  int descriptors[2]{-1, -1};
  if (::pipe2(descriptors, O_CLOEXEC) != 0) {
    Fail(ErrnoDetail("could not create runtime adapter pipe", errno));
  }
  return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}

[[nodiscard]] std::array<UniqueFd, 2U> MakeSocketPair() {
  int descriptors[2]{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, descriptors) != 0) {
    Fail(ErrnoDetail("could not create runtime adapter socket pair", errno));
  }
  return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}

[[nodiscard]] std::array<UniqueFd, 2U> MakeSeqPacketPair() {
  int descriptors[2]{-1, -1};
  if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, descriptors) !=
      0) {
    Fail(ErrnoDetail("could not create runtime adapter packet socket pair",
                     errno));
  }
  return {UniqueFd(descriptors[0]), UniqueFd(descriptors[1])};
}

void WriteAll(const int fd, const std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto written =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      Fail(ErrnoDetail("runtime adapter write failed", errno));
    }
    if (written == 0) {
      Fail("runtime adapter write made no progress");
    }
    offset += static_cast<std::size_t>(written);
  }
}

void SendAllNoSignal(const int fd, const std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto written =
        ::send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      Fail(ErrnoDetail("runtime adapter command write failed", errno));
    }
    if (written == 0) {
      Fail("runtime adapter command write made no progress");
    }
    offset += static_cast<std::size_t>(written);
  }
}

void SendPacketNoSignal(const int fd, const std::string_view bytes) {
  for (;;) {
    const auto written = ::send(fd, bytes.data(), bytes.size(), MSG_NOSIGNAL);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written < 0) {
      Fail(ErrnoDetail("runtime adapter packet write failed", errno));
    }
    if (static_cast<std::size_t>(written) != bytes.size()) {
      Fail("runtime adapter packet write was incomplete");
    }
    return;
  }
}

[[nodiscard]] std::optional<std::string>
ReceivePacket(const int fd, const std::size_t maximum_bytes,
              const std::chrono::milliseconds timeout) {
  pollfd descriptor{fd, POLLIN | POLLHUP, 0};
  int poll_result = 0;
  do {
    poll_result = ::poll(&descriptor, 1U, static_cast<int>(timeout.count()));
  } while (poll_result < 0 && errno == EINTR);
  if (poll_result < 0) {
    Fail(ErrnoDetail("runtime adapter packet poll failed", errno));
  }
  if (poll_result == 0) {
    return std::nullopt;
  }
  std::vector<char> bytes(maximum_bytes + 1U);
  ssize_t received = -1;
  do {
    received = ::recv(fd, bytes.data(), bytes.size(), MSG_TRUNC);
  } while (received < 0 && errno == EINTR);
  if (received < 0) {
    Fail(ErrnoDetail("runtime adapter packet read failed", errno));
  }
  if (received == 0) {
    Fail("runtime adapter packet peer closed without a record");
  }
  if (static_cast<std::size_t>(received) > maximum_bytes) {
    Fail("runtime adapter packet exceeds its byte bound");
  }
  return std::string(bytes.data(), static_cast<std::size_t>(received));
}

[[nodiscard]] std::string RandomHex() {
  std::array<unsigned char, 16U> bytes{};
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto received = ::getrandom(bytes.data() + offset,
                                      bytes.size() - offset, GRND_NONBLOCK);
    if (received < 0) {
      if (errno == EINTR) {
        continue;
      }
      Fail(ErrnoDetail("could not mint runtime adapter identity", errno));
    }
    if (received == 0) {
      Fail("runtime adapter identity source returned no bytes");
    }
    offset += static_cast<std::size_t>(received);
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

[[nodiscard]] std::string ReadSmallFile(const std::filesystem::path &path,
                                        const std::size_t maximum_bytes) {
  const UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (!fd) {
    Fail(ErrnoDetail("could not open " + path.string(), errno));
  }
  std::string result;
  std::array<char, 1024U> buffer{};
  for (;;) {
    const auto count = ::read(fd.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      Fail(ErrnoDetail("could not read " + path.string(), errno));
    }
    if (count == 0) {
      break;
    }
    result.append(buffer.data(), static_cast<std::size_t>(count));
    if (result.size() > maximum_bytes) {
      Fail(path.string() + " exceeds its byte bound");
    }
  }
  return result;
}

[[nodiscard]] std::string HostBootId() {
  std::string value =
      ReadSmallFile("/proc/sys/kernel/random/boot_id", kMaximumStateBytes);
  if (!value.empty() && value.back() == '\n') {
    value.pop_back();
  }
  if (value.size() != 36U ||
      !std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') || character == '-';
      })) {
    Fail("host boot identity is malformed");
  }
  return value;
}

[[nodiscard]] std::uint64_t
ParseCanonicalUnsigned(const std::string_view text,
                       const std::string_view field) {
  if (text.empty() || (text.size() > 1U && text.front() == '0')) {
    Fail(std::string(field) + " is not a canonical unsigned integer");
  }
  std::uint64_t value = 0U;
  for (const char character : text) {
    if (character < '0' || character > '9') {
      Fail(std::string(field) + " is not a canonical unsigned integer");
    }
    const auto digit = static_cast<std::uint64_t>(character - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
      Fail(std::string(field) + " overflows uint64");
    }
    value = value * 10U + digit;
  }
  return value;
}

[[nodiscard]] std::string
DigestHex(const manyforge::core::ContentDigest &digest) {
  const auto &value = digest.value();
  if (value.size() != 71U || !value.starts_with("sha256:") ||
      !std::all_of(value.begin() + 7, value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
      })) {
    Fail("runtime adapter query carries an invalid digest");
  }
  return value.substr(7U);
}

[[nodiscard]] std::string
SlotName(const RuntimeUnitConvergenceQueryDTO &query) {
  constexpr std::string_view kReferenceProductPrefix =
      "runtime_unit_configuration_";
  const auto &product_id = query.plan.product_id.value();
  if (!product_id.starts_with(kReferenceProductPrefix) ||
      product_id.size() != kReferenceProductPrefix.size() + 64U ||
      !std::all_of(product_id.begin() + static_cast<std::ptrdiff_t>(
                                            kReferenceProductPrefix.size()),
                   product_id.end(), [](const char character) {
                     return (character >= '0' && character <= '9') ||
                            (character >= 'a' && character <= 'f');
                   })) {
    Fail("runtime adapter query carries an invalid reference product subject");
  }
  return "u_" + product_id.substr(kReferenceProductPrefix.size()) + "_p_" +
         DigestHex(query.plan.product_identity) + "_c_" +
         DigestHex(query.plan.configuration_identity);
}

struct ProcessState final {
  std::string runtime_unit_id;
  std::string product_identity;
  std::string configuration_identity;
  std::string process_adapter_revision;
  std::string boot_id;
  pid_t controller_pid{};
  std::uint64_t controller_start_ticks{};
  pid_t pid{};
  std::uint64_t start_ticks{};
  std::uint64_t control_socket_device{};
  std::uint64_t control_socket_inode{};
  RuntimeEpoch runtime_epoch;
};

[[nodiscard]] std::string EncodeState(const ProcessState &state) {
  return "format=" + std::string(kStateFormat) + "\n" +
         "runtime_unit_id=" + state.runtime_unit_id + "\n" +
         "product_identity=" + state.product_identity + "\n" +
         "configuration_identity=" + state.configuration_identity + "\n" +
         "process_adapter_revision=" + state.process_adapter_revision + "\n" +
         "boot_id=" + state.boot_id + "\n" +
         "controller_pid=" + std::to_string(state.controller_pid) + "\n" +
         "controller_start_ticks=" +
         std::to_string(state.controller_start_ticks) + "\n" +
         "pid=" + std::to_string(state.pid) + "\n" +
         "start_ticks=" + std::to_string(state.start_ticks) + "\n" +
         "control_socket_device=" +
         std::to_string(state.control_socket_device) + "\n" +
         "control_socket_inode=" + std::to_string(state.control_socket_inode) +
         "\n" + "runtime_epoch=" + state.runtime_epoch.value() + "\n";
}

[[nodiscard]] std::vector<std::string> SplitLines(const std::string &bytes) {
  if (bytes.empty() || bytes.back() != '\n') {
    Fail("runtime process state is not newline terminated");
  }
  std::vector<std::string> lines;
  std::size_t begin = 0U;
  while (begin < bytes.size()) {
    const auto end = bytes.find('\n', begin);
    if (end == std::string::npos) {
      Fail("runtime process state line framing is malformed");
    }
    lines.push_back(bytes.substr(begin, end - begin));
    begin = end + 1U;
  }
  return lines;
}

[[nodiscard]] std::string StateValue(const std::string &line,
                                     const std::string_view prefix) {
  if (!line.starts_with(prefix)) {
    Fail("runtime process state fields are missing or reordered");
  }
  std::string value = line.substr(prefix.size());
  if (value.empty() ||
      !std::all_of(value.begin(), value.end(), [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 0x21U && byte <= 0x7eU;
      })) {
    Fail("runtime process state contains an invalid value");
  }
  return value;
}

[[nodiscard]] ProcessState DecodeState(const std::string &bytes) {
  const auto lines = SplitLines(bytes);
  if (lines.size() != 13U ||
      lines.front() != "format=" + std::string(kStateFormat)) {
    Fail("runtime process state has the wrong closed schema");
  }
  const auto controller_pid_text = StateValue(lines[6], "controller_pid=");
  const auto controller_pid_value =
      ParseCanonicalUnsigned(controller_pid_text, "runtime controller pid");
  if (controller_pid_value == 0U ||
      controller_pid_value >
          static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
    Fail("runtime controller pid is out of range");
  }
  const auto controller_start_ticks =
      ParseCanonicalUnsigned(StateValue(lines[7], "controller_start_ticks="),
                             "runtime controller start time");
  if (controller_start_ticks == 0U) {
    Fail("runtime controller start time is zero");
  }
  const auto pid_text = StateValue(lines[8], "pid=");
  const auto pid_value =
      ParseCanonicalUnsigned(pid_text, "runtime process pid");
  if (pid_value == 0U || pid_value > static_cast<std::uint64_t>(
                                         std::numeric_limits<pid_t>::max())) {
    Fail("runtime process pid is out of range");
  }
  const auto start_ticks_text = StateValue(lines[9], "start_ticks=");
  const auto start_ticks =
      ParseCanonicalUnsigned(start_ticks_text, "runtime process start time");
  if (start_ticks == 0U) {
    Fail("runtime process start time is zero");
  }
  ProcessState result{
      StateValue(lines[1], "runtime_unit_id="),
      StateValue(lines[2], "product_identity="),
      StateValue(lines[3], "configuration_identity="),
      StateValue(lines[4], "process_adapter_revision="),
      StateValue(lines[5], "boot_id="),
      static_cast<pid_t>(controller_pid_value),
      controller_start_ticks,
      static_cast<pid_t>(pid_value),
      start_ticks,
      ParseCanonicalUnsigned(StateValue(lines[10], "control_socket_device="),
                             "runtime control socket device"),
      ParseCanonicalUnsigned(StateValue(lines[11], "control_socket_inode="),
                             "runtime control socket inode"),
      RuntimeEpoch(StateValue(lines[12], "runtime_epoch=")),
  };
  if (result.control_socket_inode == 0U) {
    Fail("runtime control socket inode is zero");
  }
  return result;
}

struct ProcObservation final {
  std::uint64_t start_ticks{};
  char state{};
};

[[nodiscard]] std::optional<ProcObservation>
ReadProcObservation(const pid_t pid) {
  const auto path =
      std::filesystem::path("/proc") / std::to_string(pid) / "stat";
  const int raw_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (raw_fd < 0) {
    if (errno == ENOENT || errno == ESRCH) {
      return std::nullopt;
    }
    Fail(ErrnoDetail("could not inspect runtime process", errno));
  }
  const UniqueFd fd(raw_fd);
  std::array<char, 4096U> buffer{};
  std::size_t size = 0U;
  for (;;) {
    const auto count =
        ::read(fd.get(), buffer.data() + size, buffer.size() - size);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      Fail(ErrnoDetail("could not read runtime process identity", errno));
    }
    if (count == 0) {
      break;
    }
    size += static_cast<std::size_t>(count);
    if (size == buffer.size()) {
      Fail("runtime process identity exceeds its byte bound");
    }
  }
  const std::string text(buffer.data(), size);
  if (text.empty()) {
    return std::nullopt;
  }
  const auto close = text.rfind(')');
  if (close == std::string::npos || close + 2U >= text.size() ||
      text[close + 1U] != ' ') {
    Fail("runtime process identity is malformed");
  }
  std::istringstream fields(text.substr(close + 2U));
  std::vector<std::string> tokens;
  std::string token;
  while (fields >> token) {
    tokens.push_back(std::move(token));
  }
  if (tokens.size() < 20U || tokens.front().size() != 1U) {
    Fail("runtime process identity lacks required fields");
  }
  return ProcObservation{
      ParseCanonicalUnsigned(tokens[19U], "Linux process start time"),
      tokens.front().front()};
}

[[nodiscard]] bool ProcessIsLive(const pid_t pid,
                                 const std::uint64_t start_ticks) {
  const auto observed = ReadProcObservation(pid);
  return observed.has_value() && observed->start_ticks == start_ticks &&
         observed->state != 'Z' && observed->state != 'X';
}

[[nodiscard]] bool RuntimeProcessIsLive(const ProcessState &state,
                                        const std::string_view boot_id) {
  return state.boot_id == boot_id &&
         ProcessIsLive(state.pid, state.start_ticks);
}

[[nodiscard]] bool ControllerProcessIsLive(const ProcessState &state,
                                           const std::string_view boot_id) {
  return state.boot_id == boot_id &&
         ProcessIsLive(state.controller_pid, state.controller_start_ticks);
}

void VerifyStateMatchesQuery(const ProcessState &state,
                             const RuntimeUnitConvergenceQueryDTO &query) {
  if (state.runtime_unit_id != query.plan.runtime_unit_id.value() ||
      state.product_identity != query.plan.product_identity.value() ||
      state.configuration_identity !=
          query.plan.configuration_identity.value() ||
      state.process_adapter_revision !=
          query.plan.process_adapter_contract_revision ||
      state.runtime_epoch.empty()) {
    Fail("runtime process state conflicts with the exact convergence query");
  }
}

class SlotLock final {
public:
  SlotLock(UniqueFd directory, UniqueFd lock)
      : directory_(std::move(directory)), lock_(std::move(lock)) {}
  ~SlotLock() {
    if (lock_) {
      static_cast<void>(::flock(lock_.get(), LOCK_UN));
    }
  }

  SlotLock(const SlotLock &) = delete;
  SlotLock &operator=(const SlotLock &) = delete;
  SlotLock(SlotLock &&) noexcept = default;
  SlotLock &operator=(SlotLock &&) noexcept = default;

  [[nodiscard]] int directory_fd() const noexcept { return directory_.get(); }
  [[nodiscard]] int lock_fd() const noexcept { return lock_.get(); }

private:
  UniqueFd directory_;
  UniqueFd lock_;
};

[[nodiscard]] std::optional<UniqueFd>
OpenSlotDirectory(const int root_fd, const std::string &slot_name,
                  const bool create) {
  bool created = false;
  int directory_fd = ::openat(root_fd, slot_name.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory_fd < 0 && errno == ENOENT && create) {
    if (::mkdirat(root_fd, slot_name.c_str(), 0700) == 0) {
      created = true;
    } else if (errno != EEXIST) {
      Fail(ErrnoDetail("could not create runtime adapter slot", errno));
    }
    directory_fd = ::openat(root_fd, slot_name.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  }
  if (directory_fd < 0 && errno == ENOENT) {
    return std::nullopt;
  }
  if (directory_fd < 0) {
    Fail(ErrnoDetail("could not open runtime adapter slot", errno));
  }
  UniqueFd directory(directory_fd);
  struct stat status {};
  if (::fstat(directory.get(), &status) != 0 || !S_ISDIR(status.st_mode) ||
      status.st_uid != ::geteuid() ||
      (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      (status.st_mode & (S_IRUSR | S_IWUSR | S_IXUSR)) !=
          (S_IRUSR | S_IWUSR | S_IXUSR)) {
    Fail("runtime adapter slot type, owner, or mode is unsafe");
  }
  if (created && ::fsync(root_fd) != 0) {
    Fail(ErrnoDetail("could not retain runtime adapter slot", errno));
  }
  return std::optional<UniqueFd>(std::move(directory));
}

[[nodiscard]] SlotLock AcquireSlot(const int root_fd,
                                   const std::string &slot_name,
                                   const bool create) {
  auto directory = OpenSlotDirectory(root_fd, slot_name, create);
  if (!directory.has_value()) {
    Fail("runtime adapter slot is absent");
  }
  const int lock_flags =
      O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0);
  const int lock_fd =
      ::openat(directory->get(), "lock", lock_flags, 0600);
  if (lock_fd < 0) {
    Fail(ErrnoDetail("could not open runtime adapter slot lock", errno));
  }
  UniqueFd lock(lock_fd);
  struct stat lock_status {};
  if (::fstat(lock.get(), &lock_status) != 0 ||
      !S_ISREG(lock_status.st_mode) || lock_status.st_uid != ::geteuid() ||
      lock_status.st_nlink != 1 || (lock_status.st_mode & 0777) != 0600) {
    Fail("runtime adapter slot lock type, owner, or mode is unsafe");
  }
  const auto deadline = std::chrono::steady_clock::now() + kSlotLockTimeout;
  for (;;) {
    if (::flock(lock.get(), LOCK_EX | LOCK_NB) == 0) {
      break;
    }
    if (errno != EWOULDBLOCK && errno != EAGAIN) {
      Fail(ErrnoDetail("could not lock runtime adapter slot", errno));
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      Fail("runtime adapter slot lock timed out");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return SlotLock(std::move(*directory), std::move(lock));
}

[[nodiscard]] std::optional<std::string>
ReadFileAt(const int directory_fd, const std::string_view name,
           const std::size_t maximum_bytes, const mode_t expected_mode) {
  const int raw_fd = ::openat(directory_fd, std::string(name).c_str(),
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (raw_fd < 0) {
    if (errno == ENOENT) {
      return std::nullopt;
    }
    Fail(ErrnoDetail("could not open runtime adapter state", errno));
  }
  const UniqueFd fd(raw_fd);
  struct stat status {};
  if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != ::geteuid() || status.st_nlink != 1 ||
      (status.st_mode & 0777) != expected_mode || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > maximum_bytes) {
    Fail("runtime adapter file type, owner, mode, or size is invalid");
  }
  std::string result;
  std::array<char, 1024U> buffer{};
  for (;;) {
    const auto count = ::read(fd.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      Fail(ErrnoDetail("could not read runtime adapter state", errno));
    }
    if (count == 0) {
      break;
    }
    result.append(buffer.data(), static_cast<std::size_t>(count));
    if (result.size() > maximum_bytes) {
      Fail("runtime adapter state exceeds its byte bound");
    }
  }
  struct stat after_fd {};
  struct stat after_path {};
  if (::fstat(fd.get(), &after_fd) != 0 ||
      ::fstatat(directory_fd, std::string(name).c_str(), &after_path,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      after_fd.st_dev != status.st_dev || after_fd.st_ino != status.st_ino ||
      after_path.st_dev != status.st_dev ||
      after_path.st_ino != status.st_ino ||
      after_fd.st_size != status.st_size || after_path.st_size != status.st_size ||
      !S_ISREG(after_path.st_mode) || after_path.st_uid != ::geteuid() ||
      after_path.st_nlink != 1 ||
      (after_path.st_mode & 0777) != expected_mode) {
    Fail("runtime adapter file changed while reading");
  }
  return result;
}

void SyncDirectory(const int directory_fd) {
  if (::fsync(directory_fd) != 0) {
    Fail(ErrnoDetail("could not sync runtime adapter directory", errno));
  }
}

void RemoveStaleTemporaryAt(const int directory_fd,
                            const std::string &temporary) {
  const int raw_fd = ::openat(directory_fd, temporary.c_str(),
                              O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (raw_fd < 0) {
    if (errno == ENOENT) {
      return;
    }
    Fail(
        ErrnoDetail("could not inspect runtime adapter temporary file", errno));
  }
  const UniqueFd fd(raw_fd);
  struct stat status {};
  if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_nlink != 1) {
    Fail("runtime adapter temporary path is not one private regular file");
  }
  if (::unlinkat(directory_fd, temporary.c_str(), 0) != 0) {
    Fail(ErrnoDetail("could not remove stale runtime adapter temporary file",
                     errno));
  }
}

void AtomicWriteAt(const int directory_fd, const std::string_view final_name,
                   const std::string_view bytes, const mode_t mode) {
  std::string temporary;
  temporary.reserve(final_name.size() + 5U);
  temporary.push_back('.');
  temporary.append(final_name);
  temporary.append(".tmp");
  RemoveStaleTemporaryAt(directory_fd, temporary);
  const int raw_fd =
      ::openat(directory_fd, temporary.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, mode);
  if (raw_fd < 0) {
    Fail(ErrnoDetail("could not create runtime adapter temporary file", errno));
  }
  UniqueFd fd(raw_fd);
  try {
    WriteAll(fd.get(), bytes);
    if (::fchmod(fd.get(), mode) != 0 || ::fsync(fd.get()) != 0) {
      Fail(ErrnoDetail("could not persist runtime adapter temporary file",
                       errno));
    }
    fd.Reset();
    if (::renameat(directory_fd, temporary.c_str(), directory_fd,
                   std::string(final_name).c_str()) != 0) {
      Fail(ErrnoDetail("could not publish runtime adapter file", errno));
    }
    SyncDirectory(directory_fd);
  } catch (...) {
    static_cast<void>(::unlinkat(directory_fd, temporary.c_str(), 0));
    throw;
  }
}

[[nodiscard]] UniqueFd OpenVerifiedLaunchable(
    const int directory_fd,
    const ReferenceRuntimeUnitAdmission &admission) {
  const int raw_fd =
      ::openat(directory_fd, "launchable", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (raw_fd < 0) {
    Fail(ErrnoDetail("could not open materialized runtime launchable", errno));
  }
  UniqueFd fd(raw_fd);
  struct stat before {};
  if (::fstat(fd.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
      before.st_uid != ::geteuid() || before.st_nlink != 1 ||
      (before.st_mode & 0777) != 0500 || before.st_size < 0 ||
      static_cast<std::uint64_t>(before.st_size) !=
          admission.launchable_bytes().size()) {
    Fail("materialized runtime launchable has invalid identity or permissions");
  }
  std::string bytes;
  bytes.reserve(admission.launchable_bytes().size());
  std::array<char, 1024U> buffer{};
  for (;;) {
    const auto count = ::read(fd.get(), buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      Fail(ErrnoDetail("could not read materialized runtime launchable", errno));
    }
    if (count == 0) {
      break;
    }
    bytes.append(buffer.data(), static_cast<std::size_t>(count));
    if (bytes.size() > admission.launchable_bytes().size()) {
      Fail("materialized runtime launchable exceeds its verified byte bound");
    }
  }
  struct stat after_fd {};
  struct stat after_path {};
  if (bytes != admission.launchable_bytes() ||
      ::fstat(fd.get(), &after_fd) != 0 ||
      ::fstatat(directory_fd, "launchable", &after_path,
                AT_SYMLINK_NOFOLLOW) != 0 ||
      after_fd.st_dev != before.st_dev || after_fd.st_ino != before.st_ino ||
      after_path.st_dev != before.st_dev ||
      after_path.st_ino != before.st_ino ||
      after_fd.st_size != before.st_size ||
      after_path.st_size != before.st_size ||
      !S_ISREG(after_path.st_mode) || after_path.st_uid != ::geteuid() ||
      after_path.st_nlink != 1 || (after_path.st_mode & 0777) != 0500) {
    Fail("materialized runtime launchable changed while being pinned");
  }
  return fd;
}

[[nodiscard]] UniqueFd MaterializeLaunchable(
    const int directory_fd,
    const ReferenceRuntimeUnitAdmission &admission) {
  const auto existing = ReadFileAt(directory_fd, "launchable",
                                   admission.launchable_bytes().size(), 0500);
  if (existing.has_value()) {
    if (*existing != admission.launchable_bytes()) {
      Fail("materialized runtime launchable conflicts with verified retained "
           "bytes");
    }
  } else {
    AtomicWriteAt(directory_fd, "launchable", admission.launchable_bytes(),
                  0500);
  }
  return OpenVerifiedLaunchable(directory_fd, admission);
}

[[nodiscard]] std::optional<ProcessState> ReadState(const int directory_fd) {
  const auto bytes =
      ReadFileAt(directory_fd, "process.state", kMaximumStateBytes, 0600);
  if (!bytes.has_value()) {
    return std::nullopt;
  }
  return DecodeState(*bytes);
}

void RemoveState(const int directory_fd) {
  if (::unlinkat(directory_fd, "process.state", 0) != 0 && errno != ENOENT) {
    Fail(ErrnoDetail("could not remove stale runtime process state", errno));
  }
  SyncDirectory(directory_fd);
}

[[nodiscard]] std::string ControlSocketPath(const int directory_fd) {
  return "/proc/self/fd/" + std::to_string(directory_fd) + "/" +
         std::string(kControlSocketName);
}

[[nodiscard]] sockaddr_un ControlSocketAddress(const int directory_fd,
                                               socklen_t &length) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = ControlSocketPath(directory_fd);
  if (path.size() >= sizeof(address.sun_path)) {
    Fail("runtime control socket path exceeds the Linux AF_UNIX bound");
  }
  std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
  length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                  path.size() + 1U);
  return address;
}

struct ControlSocketIdentity final {
  std::uint64_t device{};
  std::uint64_t inode{};
};

[[nodiscard]] std::optional<ControlSocketIdentity>
ReadControlSocketIdentity(const int directory_fd) {
  struct stat status {};
  if (::fstatat(directory_fd, std::string(kControlSocketName).c_str(), &status,
                AT_SYMLINK_NOFOLLOW) != 0) {
    if (errno == ENOENT) {
      return std::nullopt;
    }
    Fail(ErrnoDetail("could not inspect runtime control socket", errno));
  }
  if (!S_ISSOCK(status.st_mode) || status.st_uid != ::geteuid() ||
      (status.st_mode & 0777) != 0600 || status.st_nlink != 1) {
    Fail("runtime control socket has invalid type, owner, or permissions");
  }
  return ControlSocketIdentity{static_cast<std::uint64_t>(status.st_dev),
                               static_cast<std::uint64_t>(status.st_ino)};
}

void RemoveControlSocket(const int directory_fd) {
  const auto identity = ReadControlSocketIdentity(directory_fd);
  if (!identity.has_value()) {
    return;
  }
  if (::unlinkat(directory_fd, std::string(kControlSocketName).c_str(), 0) !=
      0) {
    Fail(ErrnoDetail("could not remove stale runtime control socket", errno));
  }
  SyncDirectory(directory_fd);
}

struct ControlListener final {
  UniqueFd fd;
  ControlSocketIdentity identity;
};

[[nodiscard]] ControlListener CreateControlListener(const int directory_fd) {
  RemoveControlSocket(directory_fd);
  UniqueFd listener(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  if (!listener) {
    Fail(ErrnoDetail("could not create runtime control socket", errno));
  }
  socklen_t address_length = 0;
  const auto address = ControlSocketAddress(directory_fd, address_length);
  if (::bind(listener.get(), reinterpret_cast<const sockaddr *>(&address),
             address_length) != 0) {
    Fail(ErrnoDetail("could not bind runtime control socket", errno));
  }
  if (::fchmodat(directory_fd, std::string(kControlSocketName).c_str(), 0600,
                 0) != 0) {
    Fail(ErrnoDetail("could not restrict runtime control socket", errno));
  }
  if (::listen(listener.get(), 4) != 0) {
    Fail(ErrnoDetail("could not listen on runtime control socket", errno));
  }
  const auto identity = ReadControlSocketIdentity(directory_fd);
  if (!identity.has_value()) {
    Fail("runtime control socket disappeared after bind");
  }
  return ControlListener{std::move(listener), *identity};
}

[[nodiscard]] bool ControlSocketMatches(const int directory_fd,
                                        const ProcessState &state) {
  const auto identity = ReadControlSocketIdentity(directory_fd);
  return identity.has_value() &&
         identity->device == state.control_socket_device &&
         identity->inode == state.control_socket_inode;
}

[[nodiscard]] UniqueFd ConnectControlSocket(const int directory_fd,
                                            const ProcessState &state) {
  if (!ControlSocketMatches(directory_fd, state)) {
    Fail("runtime control socket identity conflicts with durable state");
  }
  UniqueFd connection(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  if (!connection) {
    Fail(ErrnoDetail("could not create runtime control connection", errno));
  }
  socklen_t address_length = 0;
  const auto address = ControlSocketAddress(directory_fd, address_length);
  if (::connect(connection.get(), reinterpret_cast<const sockaddr *>(&address),
                address_length) != 0) {
    Fail(ErrnoDetail("could not connect to runtime controller", errno));
  }
  return connection;
}

void WaitForProcessExit(const pid_t pid,
                        const std::chrono::steady_clock::time_point deadline) {
  for (;;) {
    const auto observed = ReadProcObservation(pid);
    if (!observed.has_value() || observed->state == 'Z' ||
        observed->state == 'X') {
      return;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
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
    Fail("runtime adapter candidate bundle lacks the queried runtime unit");
  }
  return *found;
}

void VerifyAdmissionMatchesQuery(const ReferenceRuntimeUnitAdmission &admission,
                                 const RetainedBundle &bundle,
                                 const RuntimeUnitConvergenceQueryDTO &query) {
  const auto &plan = query.plan;
  const auto &runtime_unit = FindRuntimeUnit(bundle, plan.runtime_unit_id);
  const auto requirement = std::find_if(
      bundle.value().runtime_contract_requirements.begin(),
      bundle.value().runtime_contract_requirements.end(),
      [&](const auto &candidate) {
        return candidate.runtime_unit_id == plan.runtime_unit_id;
      });
  const auto *contract =
      manyforge::deployment::runtime_contract_declaration_registry().find(
          runtime_unit.runtime_profile_id, runtime_unit.runtime_profile_revision,
          plan.product_contract_id, plan.product_contract_revision);
  std::vector<manyforge::core::RuntimeRoleId> roles{
      manyforge::core::RuntimeRoleId(admission.runtime_role_id())};
  if (bundle.retained_bundle_id() != query.candidate_retained_bundle_id ||
      bundle.value().cell_graph.graph_revision !=
          query.candidate_graph_revision ||
      runtime_unit.runtime_profile_id != plan.runtime_profile_id ||
      runtime_unit.runtime_profile_revision != plan.runtime_profile_revision ||
      runtime_unit.dependency_runtime_unit_ids !=
          plan.dependency_runtime_unit_ids ||
      plan.lifecycle_control !=
          manyforge::core::RuntimeLifecycleControl::kManagerManaged ||
      admission.runtime_unit_id() != plan.runtime_unit_id.value() ||
      admission.retained_bundle_id() != query.candidate_retained_bundle_id ||
      admission.product_id() != plan.product_id ||
      admission.product_contract_id() != plan.product_contract_id ||
      admission.product_contract_revision() != plan.product_contract_revision ||
      admission.product_identity() != plan.product_identity.value() ||
      admission.configuration_identity() !=
          plan.configuration_identity.value() ||
      roles != plan.required_runtime_role_ids ||
      admission.status_endpoint_role_id() !=
          plan.status_endpoint_role_id.value() ||
      admission.status_endpoint_contract_id() !=
          plan.status_endpoint_contract_id ||
      admission.status_endpoint_contract_revision() !=
          plan.status_endpoint_contract_revision ||
      admission.status_transport() != plan.status_transport ||
      admission.process_adapter_contract_id() !=
          plan.process_adapter_contract_id ||
      admission.process_adapter_contract_revision() !=
          plan.process_adapter_contract_revision ||
      admission.retrieval_contract_id() != plan.retrieval_contract_id ||
      admission.retrieval_contract_revision() !=
          plan.retrieval_contract_revision ||
      (!plan.runtime_contract_offer_digest.empty() &&
       (requirement == bundle.value().runtime_contract_requirements.end() ||
        plan.runtime_contract_requirement != requirement->requirement ||
        contract == nullptr ||
        plan.runtime_contract_offer_digest != contract->offer().offer_digest ||
        !manyforge::core::CheckRuntimeContractCompatibility(
             contract->offer(), requirement->requirement)
             .ok())) ||
      !plan.launchable_content_digest.has_value() ||
      admission.launchable_content_digest() !=
          plan.launchable_content_digest->value() ||
      admission.launchable_bytes().size() != plan.launchable_byte_size ||
      admission.operating_system() != plan.operating_system ||
      admission.shell_contract() != plan.shell_contract) {
    Fail("verified retained runtime admission differs from the exact "
         "convergence query");
  }
}

struct RuntimeChannel final {
  UniqueFd standard_input;
  UniqueFd standard_output;
  std::string output_buffer;
  bool status_request_outstanding{true};
};

[[nodiscard]] std::optional<std::string>
ReadStatusLine(RuntimeChannel &channel) {
  auto take_line = [&]() -> std::optional<std::string> {
    const auto newline = channel.output_buffer.find('\n');
    if (newline == std::string::npos) {
      return std::nullopt;
    }
    if (newline + 1U != channel.output_buffer.size()) {
      Fail("runtime emitted unsolicited bytes beyond one status record");
    }
    std::string line = channel.output_buffer.substr(0U, newline);
    channel.output_buffer.clear();
    return line;
  };
  if (auto line = take_line(); line.has_value()) {
    return line;
  }

  pollfd descriptor{channel.standard_output.get(), POLLIN | POLLHUP, 0};
  int poll_result = 0;
  do {
    poll_result =
        ::poll(&descriptor, 1U, static_cast<int>(kStatusWait.count()));
  } while (poll_result < 0 && errno == EINTR);
  if (poll_result < 0) {
    Fail(ErrnoDetail("runtime status poll failed", errno));
  }
  if (poll_result == 0) {
    return std::nullopt;
  }

  std::array<char, 1024U> buffer{};
  for (;;) {
    const auto count =
        ::read(channel.standard_output.get(), buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      Fail(ErrnoDetail("runtime status read failed", errno));
    }
    if (count == 0) {
      channel.standard_output.Reset();
      break;
    }
    channel.output_buffer.append(buffer.data(),
                                 static_cast<std::size_t>(count));
    if (channel.output_buffer.size() > kMaximumStatusLineBytes) {
      Fail("runtime status line exceeds its byte bound");
    }
  }
  return take_line();
}

[[nodiscard]] std::string EncodeControllerBootstrap(const pid_t pid,
                                                    const std::uint64_t ticks) {
  return std::to_string(pid) + ":" + std::to_string(ticks);
}

[[nodiscard]] std::pair<pid_t, std::uint64_t>
DecodeControllerBootstrap(const std::string_view record) {
  const auto separator = record.find(':');
  if (separator == std::string_view::npos ||
      record.find(':', separator + 1U) != std::string_view::npos) {
    Fail("runtime controller bootstrap record is malformed");
  }
  const auto pid_value = ParseCanonicalUnsigned(record.substr(0U, separator),
                                                "runtime controller child pid");
  const auto ticks = ParseCanonicalUnsigned(
      record.substr(separator + 1U), "runtime controller child start time");
  if (pid_value == 0U ||
      pid_value >
          static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()) ||
      ticks == 0U) {
    Fail("runtime controller bootstrap identity is out of range");
  }
  return {static_cast<pid_t>(pid_value), ticks};
}

void VerifyControlPeer(const int connection) {
  struct ucred credentials {};
  socklen_t size = sizeof(credentials);
  if (::getsockopt(connection, SOL_SOCKET, SO_PEERCRED, &credentials, &size) !=
          0 ||
      size != sizeof(credentials) || credentials.uid != ::geteuid()) {
    Fail("runtime control peer does not have the Manager uid");
  }
}

[[nodiscard]] bool ReapChildIfExited(const pid_t child, int &status) {
  for (;;) {
    const pid_t result = ::waitpid(child, &status, WNOHANG);
    if (result == child) {
      return true;
    }
    if (result == 0) {
      return false;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    return result < 0 && errno == ECHILD;
  }
}

void ReapOwnedChildIfExited(const pid_t child) noexcept {
  int status = 0;
  for (;;) {
    const pid_t result = ::waitpid(child, &status, WNOHANG);
    if (result >= 0 || errno != EINTR) {
      return;
    }
  }
}

[[noreturn]] void RunController(const int listener_fd, const int bootstrap_fd,
                                const int bootstrap_lock_fd,
                                const int launchable_fd,
                                std::vector<std::string> arguments,
                                const std::string status_command,
                                const std::string shutdown_command) {
  try {
    UniqueFd listener(listener_fd);
    UniqueFd bootstrap(bootstrap_fd);
    // This is the inherited slot-lock open-file description. Keep the
    // descriptor in both controller and gated child so that, if Manager death
    // prevents its normal explicit unlock, a replacement cannot observe a
    // state-less slot while either unreleased process still exists.
    UniqueFd bootstrap_lock(bootstrap_lock_fd);
    UniqueFd launchable(launchable_fd);
    auto standard_input = MakePipe();
    auto standard_output = MakePipe();
    auto gate = MakeSocketPair();
    SetNonBlocking(standard_output[0].get());
    const UniqueFd null_error(::open("/dev/null", O_WRONLY | O_CLOEXEC));
    if (!null_error) {
      Fail(ErrnoDetail("could not open fixed runtime stderr sink", errno));
    }

    std::vector<char *> argv;
    argv.reserve(arguments.size() + 1U);
    for (auto &argument : arguments) {
      argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    std::array<char, 19U> path_environment{"PATH=/usr/bin:/bin"};
    std::array<char, 7U> lang_environment{"LANG=C"};
    std::array<char, 9U> locale_environment{"LC_ALL=C"};
    std::array<char *, 4U> environment{path_environment.data(),
                                       lang_environment.data(),
                                       locale_environment.data(), nullptr};

    const pid_t child = ::fork();
    if (child < 0) {
      Fail(ErrnoDetail("could not fork retained runtime", errno));
    }
    if (child == 0) {
      listener.Reset();
      bootstrap.Reset();
      standard_input[1].Reset();
      standard_output[0].Reset();
      gate[1].Reset();
      char release = 0;
      ssize_t received = -1;
      do {
        received = ::read(gate[0].get(), &release, 1U);
      } while (received < 0 && errno == EINTR);
      if (received != 1 || release != 'R' ||
          ::dup2(standard_input[0].get(), STDIN_FILENO) < 0 ||
          ::dup2(standard_output[1].get(), STDOUT_FILENO) < 0 ||
          ::dup2(null_error.get(), STDERR_FILENO) < 0) {
        ::_exit(126);
      }
      standard_input[0].Reset();
      standard_output[1].Reset();
      gate[0].Reset();
      bootstrap_lock.Reset();
      const int result = ::fexecve(launchable.get(), argv.data(),
                                   environment.data());
      (void)result;
      ::_exit(127);
    }

    standard_input[0].Reset();
    standard_output[1].Reset();
    gate[0].Reset();
    launchable.Reset();
    const auto observed = ReadProcObservation(child);
    if (!observed.has_value() || observed->state == 'Z' ||
        observed->state == 'X' || observed->start_ticks == 0U) {
      Fail("forked runtime disappeared before controller bootstrap");
    }
    SendPacketNoSignal(bootstrap.get(),
                       EncodeControllerBootstrap(child, observed->start_ticks));
    const auto release = ReceivePacket(
        bootstrap.get(), 16U,
        std::chrono::duration_cast<std::chrono::milliseconds>(kBootstrapWait));
    if (!release.has_value() || *release != "release") {
      Fail("runtime controller was not durably released by Manager");
    }
    SendAllNoSignal(gate[1].get(), "R");
    gate[1].Reset();
    bootstrap.Reset();
    bootstrap_lock.Reset();

    RuntimeChannel channel{
        std::move(standard_input[1]), std::move(standard_output[0]), {}, true};
    bool shutdown_requested = false;
    for (;;) {
      int child_status = 0;
      if (ReapChildIfExited(child, child_status)) {
        const bool clean =
            WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0;
        ::_exit(clean ? 0 : 124);
      }

      pollfd descriptor{listener.get(), POLLIN, 0};
      int poll_result = 0;
      do {
        poll_result = ::poll(&descriptor, 1U,
                             static_cast<int>(kRelayPollInterval.count()));
      } while (poll_result < 0 && errno == EINTR);
      if (poll_result < 0) {
        Fail(ErrnoDetail("runtime controller accept poll failed", errno));
      }
      if (poll_result == 0 || shutdown_requested) {
        continue;
      }

      UniqueFd connection(
          ::accept4(listener.get(), nullptr, nullptr, SOCK_CLOEXEC));
      if (!connection) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
          continue;
        }
        Fail(ErrnoDetail("runtime controller accept failed", errno));
      }
      try {
        VerifyControlPeer(connection.get());
        const auto command = ReceivePacket(
            connection.get(), 64U,
            std::chrono::duration_cast<std::chrono::milliseconds>(kStatusWait));
        if (!command.has_value()) {
          SendPacketNoSignal(connection.get(), "pending");
          continue;
        }
        if (*command == "status") {
          if (!channel.status_request_outstanding) {
            WriteAll(channel.standard_input.get(), status_command);
            channel.status_request_outstanding = true;
          }
          const auto line = ReadStatusLine(channel);
          if (!line.has_value()) {
            SendPacketNoSignal(connection.get(), "pending");
            continue;
          }
          channel.status_request_outstanding = false;
          SendPacketNoSignal(connection.get(), *line);
          continue;
        }
        if (*command == "shutdown") {
          WriteAll(channel.standard_input.get(), shutdown_command);
          shutdown_requested = true;
          SendPacketNoSignal(connection.get(), "shutdown-accepted");
          continue;
        }
        SendPacketNoSignal(connection.get(), "invalid-command");
      } catch (const std::exception &) {
        // A disappearing Manager client loses only its response. The
        // controller and runtime stay alive for exact reconnect/recovery.
      }
    }
  } catch (...) {
    ::_exit(125);
  }
}

[[noreturn]] void EnterControllerProcess(
    const int listener_fd, const int bootstrap_fd, const int bootstrap_lock_fd,
    const int launchable_fd,
    std::vector<std::string> arguments, std::string status_command,
    std::string shutdown_command) {
  const int listener_copy = ::fcntl(listener_fd, F_DUPFD_CLOEXEC, 10);
  const int bootstrap_copy = ::fcntl(bootstrap_fd, F_DUPFD_CLOEXEC, 10);
  const int bootstrap_lock_copy =
      ::fcntl(bootstrap_lock_fd, F_DUPFD_CLOEXEC, 10);
  const int launchable_copy =
      ::fcntl(launchable_fd, F_DUPFD_CLOEXEC, 10);
  if (listener_copy < 0 || bootstrap_copy < 0 || bootstrap_lock_copy < 0 ||
      launchable_copy < 0) {
    ::_exit(125);
  }
  const int null_io = ::open("/dev/null", O_RDWR | O_CLOEXEC);
  if (null_io < 0 || ::dup2(null_io, STDIN_FILENO) < 0 ||
      ::dup2(null_io, STDOUT_FILENO) < 0 ||
      ::dup2(null_io, STDERR_FILENO) < 0 || ::dup2(listener_copy, 3) < 0 ||
      ::dup2(bootstrap_copy, 4) < 0 || ::dup2(bootstrap_lock_copy, 5) < 0 ||
      ::dup2(launchable_copy, 6) < 0) {
    ::_exit(125);
  }
#ifdef SYS_close_range
  const long close_range_result = ::syscall(SYS_close_range, 7U, UINT_MAX, 0U);
  if (close_range_result != 0 && errno != ENOSYS) {
    ::_exit(125);
  }
  if (close_range_result != 0) {
#endif
    const long maximum = ::sysconf(_SC_OPEN_MAX);
    const int upper =
        maximum > 0 && maximum < INT_MAX ? static_cast<int>(maximum) : 65536;
    for (int descriptor = 7; descriptor < upper; ++descriptor) {
      static_cast<void>(::close(descriptor));
    }
#ifdef SYS_close_range
  }
#endif
  RunController(3, 4, 5, 6, std::move(arguments),
                std::move(status_command), std::move(shutdown_command));
}

[[nodiscard]] std::uint64_t
SourceSequenceFromStatusLine(const std::string &line,
                             const ReferenceRuntimeUnitAdmission &admission,
                             const RuntimeEpoch &runtime_epoch) {
  std::istringstream stream(line);
  std::vector<std::string> fields;
  std::string field;
  while (stream >> field) {
    fields.push_back(std::move(field));
  }
  if (fields.size() != 12U) {
    Fail("retained runtime emitted a malformed status field count");
  }
  const auto sequence =
      ParseCanonicalUnsigned(fields[3], "runtime source-report sequence");
  if (sequence == 0U ||
      line != admission.expected_status_line(runtime_epoch, sequence)) {
    Fail("retained runtime emitted a malformed or foreign status record");
  }
  return sequence;
}

} // namespace

struct RetainedPosixRuntimeUnitAdapter::Impl final {
  Impl(
      std::filesystem::path trusted_state_root,
      std::filesystem::path trusted_run_root,
      const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      std::string operating_system, std::string shell_contract)
      : state_root_path(NormalizedTrustedDirectory(
            std::move(trusted_state_root), "retained POSIX state root")),
        run_root_path(NormalizedTrustedDirectory(
            std::move(trusted_run_root), "retained POSIX run root")),
        bundle_source(bundle_source), artifact_source(artifact_source),
        operating_system(std::move(operating_system)),
        shell_contract(std::move(shell_contract)), boot_id(HostBootId()),
        observer_instance_id("posix-observer-" + RandomHex()) {
    if (this->operating_system.empty() || this->shell_contract.empty()) {
      throw std::invalid_argument(
          "retained POSIX adapter construction input is invalid");
    }
    RequireDisjointDirectoryTrees(state_root_path, run_root_path);
    state_root_fd =
        OpenTrustedDirectory(state_root_path, "retained POSIX state root");
    run_root_fd =
        OpenTrustedDirectory(run_root_path, "retained POSIX run root");
  }

  ~Impl() = default;

  [[nodiscard]] std::pair<std::shared_ptr<const RetainedBundle>,
                          ReferenceRuntimeUnitAdmission>
  Admission(const RuntimeUnitConvergenceQueryDTO &query) const {
    if (!query.valid()) {
      Fail("retained POSIX adapter received an invalid convergence query");
    }
    auto bundle =
        bundle_source.get_verified(query.candidate_retained_bundle_id);
    if (bundle == nullptr) {
      Fail("retained POSIX adapter could not reload the candidate bundle");
    }
    const auto admission = VerifyReferenceRuntimeUnitForHost(
        *bundle, query.plan.runtime_unit_id, artifact_source, operating_system,
        shell_contract);
    if (!admission.has_value()) {
      Fail("retained POSIX adapter query has no qualified reference product");
    }
    VerifyAdmissionMatchesQuery(*admission, *bundle, query);
    return {std::move(bundle), *admission};
  }

  [[nodiscard]] std::uint64_t NextObservationSequence() {
    if (observation_sequence == std::numeric_limits<std::uint64_t>::max()) {
      Fail("runtime process observation sequence is exhausted");
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

  [[nodiscard]] std::optional<ProcessState>
  CurrentState(const int state_directory_fd,
               const RuntimeUnitConvergenceQueryDTO &query) {
    auto state = ReadState(state_directory_fd);
    if (!state.has_value()) {
      return std::nullopt;
    }
    VerifyStateMatchesQuery(*state, query);
    return state;
  }

  [[nodiscard]] ProcessPresenceState
  DurablePresence(const ProcessState &state,
                  const int run_directory_fd) const {
    if (state.boot_id != boot_id) {
      return ProcessPresenceState::kAbsent;
    }
    const bool runtime_live = RuntimeProcessIsLive(state, boot_id);
    const bool controller_live = ControllerProcessIsLive(state, boot_id);
    if (!runtime_live && !controller_live) {
      return ProcessPresenceState::kAbsent;
    }
    if (runtime_live && controller_live && run_directory_fd >= 0 &&
        ControlSocketMatches(run_directory_fd, state)) {
      return ProcessPresenceState::kPresent;
    }
    return ProcessPresenceState::kUnknown;
  }

  [[nodiscard]] ProcessPresenceObservation
  ObserveLocked(const RuntimeUnitConvergenceQueryDTO &query,
                const int state_directory_fd,
                const int run_directory_fd) {
    const auto state = CurrentState(state_directory_fd, query);
    if (!state.has_value()) {
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    const auto presence = DurablePresence(*state, run_directory_fd);
    return Presence(query, presence,
                    presence == ProcessPresenceState::kPresent
                        ? state->runtime_epoch.value()
                        : std::string_view{});
  }

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) {
    static_cast<void>(Admission(query));
    const std::string slot_name = SlotName(query);
    std::lock_guard lock(mutex);
    auto state_slot =
        OpenSlotDirectory(state_root_fd.get(), slot_name, false);
    auto run_probe = OpenSlotDirectory(run_root_fd.get(), slot_name, false);
    if (!run_probe.has_value()) {
      return state_slot.has_value()
                 ? ObserveLocked(query, state_slot->get(), -1)
                 : Presence(query, ProcessPresenceState::kAbsent);
    }
    const int lock_probe =
        ::openat(run_probe->get(), "lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (lock_probe < 0 && errno == ENOENT) {
      return state_slot.has_value()
                 ? ObserveLocked(query, state_slot->get(), -1)
                 : Presence(query, ProcessPresenceState::kUnknown);
    }
    if (lock_probe < 0) {
      Fail(ErrnoDetail("could not inspect runtime adapter slot lock", errno));
    }
    UniqueFd probe_lock(lock_probe);
    probe_lock.Reset();
    run_probe.reset();
    auto run_slot = AcquireSlot(run_root_fd.get(), slot_name, false);
    if (!state_slot.has_value()) {
      state_slot =
          OpenSlotDirectory(state_root_fd.get(), slot_name, false);
    }
    if (!state_slot.has_value()) {
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    return ObserveLocked(query, state_slot->get(), run_slot.directory_fd());
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) {
    auto [bundle, admission] = Admission(query);
    (void)bundle;
    const std::string slot_name = SlotName(query);
    std::lock_guard lock(mutex);
    auto run_slot = AcquireSlot(run_root_fd.get(), slot_name, true);
    auto state_slot = OpenSlotDirectory(state_root_fd.get(), slot_name, true);
    if (!state_slot.has_value()) {
      Fail("runtime adapter durable state slot could not be created");
    }
    RemoveStaleTemporaryAt(state_slot->get(), ".process.state.tmp");
    auto launchable =
        MaterializeLaunchable(run_slot.directory_fd(), admission);
    auto current = CurrentState(state_slot->get(), query);
    if (current.has_value()) {
      const auto presence =
          DurablePresence(*current, run_slot.directory_fd());
      if (presence == ProcessPresenceState::kPresent) {
        return Presence(query, ProcessPresenceState::kPresent,
                        current->runtime_epoch.value());
      }
      if (presence == ProcessPresenceState::kUnknown) {
        Fail("runtime controller/process state is not safely absent");
      }
      ReapOwnedChildIfExited(current->controller_pid);
      RemoveState(state_slot->get());
    }
    RemoveControlSocket(run_slot.directory_fd());

    const RuntimeEpoch runtime_epoch("runtime-" + RandomHex());
    const auto launch_arguments = admission.launch_arguments(runtime_epoch);
    const std::filesystem::path launch_path =
        run_root_path / slot_name / "launchable";
    std::vector<std::string> arguments;
    arguments.reserve(launch_arguments.size() + 1U);
    arguments.push_back(launch_path.string());
    arguments.insert(arguments.end(), launch_arguments.begin(),
                     launch_arguments.end());
    auto listener = CreateControlListener(run_slot.directory_fd());
    auto bootstrap = MakeSeqPacketPair();
    const pid_t controller = ::fork();
    if (controller < 0) {
      const int fork_error = errno;
      listener.fd.Reset();
      RemoveControlSocket(run_slot.directory_fd());
      Fail(ErrnoDetail("could not fork retained runtime controller",
                       fork_error));
    }
    if (controller == 0) {
      EnterControllerProcess(listener.fd.get(), bootstrap[0].get(),
                             run_slot.lock_fd(), launchable.get(),
                             std::move(arguments),
                             std::string(admission.status_command()),
                             std::string(admission.shutdown_command()));
    }

    bootstrap[0].Reset();
    listener.fd.Reset();
    bool released = false;
    bool state_written = false;
    try {
      const auto controller_observed = ReadProcObservation(controller);
      if (!controller_observed.has_value() ||
          controller_observed->state == 'Z' ||
          controller_observed->state == 'X' ||
          controller_observed->start_ticks == 0U) {
        Fail("runtime controller disappeared before durable admission");
      }
      const auto bootstrap_record =
          ReceivePacket(bootstrap[1].get(), 128U,
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            kBootstrapWait));
      if (!bootstrap_record.has_value()) {
        Fail("runtime controller bootstrap timed out");
      }
      const auto [child, child_start_ticks] =
          DecodeControllerBootstrap(*bootstrap_record);
      if (!ProcessIsLive(child, child_start_ticks)) {
        Fail("forked runtime disappeared before durable process admission");
      }
      ProcessState state{
          query.plan.runtime_unit_id.value(),
          query.plan.product_identity.value(),
          query.plan.configuration_identity.value(),
          query.plan.process_adapter_contract_revision,
          boot_id,
          controller,
          controller_observed->start_ticks,
          child,
          child_start_ticks,
          listener.identity.device,
          listener.identity.inode,
          runtime_epoch,
      };
      AtomicWriteAt(state_slot->get(), "process.state", EncodeState(state),
                    0600);
      state_written = true;
      SendPacketNoSignal(bootstrap[1].get(), "release");
      released = true;
      bootstrap[1].Reset();
      return Presence(query, ProcessPresenceState::kPresent,
                      runtime_epoch.value());
    } catch (...) {
      bootstrap[1].Reset();
      WaitForProcessExit(controller,
                         std::chrono::steady_clock::now() + kRuntimeStopWait);
      ReapOwnedChildIfExited(controller);
      if (state_written && !released) {
        try {
          RemoveState(state_slot->get());
        } catch (...) {
        }
      }
      if (!released) {
        try {
          RemoveControlSocket(run_slot.directory_fd());
        } catch (...) {
        }
      }
      throw;
    }
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) {
    static_cast<void>(Admission(query));
    const std::string slot_name = SlotName(query);
    std::lock_guard lock(mutex);
    auto state_slot =
        OpenSlotDirectory(state_root_fd.get(), slot_name, false);
    auto run_probe = OpenSlotDirectory(run_root_fd.get(), slot_name, false);
    if (!run_probe.has_value()) {
      if (!state_slot.has_value()) {
        return Presence(query, ProcessPresenceState::kAbsent);
      }
      const auto state = CurrentState(state_slot->get(), query);
      if (!state.has_value()) {
        return Presence(query, ProcessPresenceState::kAbsent);
      }
      if (DurablePresence(*state, -1) != ProcessPresenceState::kAbsent) {
        Fail("runtime controller/process is live without its run capability");
      }
      ReapOwnedChildIfExited(state->controller_pid);
      RemoveState(state_slot->get());
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    run_probe.reset();
    auto run_slot = AcquireSlot(run_root_fd.get(), slot_name, false);
    if (!state_slot.has_value()) {
      RemoveControlSocket(run_slot.directory_fd());
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    const auto state = CurrentState(state_slot->get(), query);
    if (!state.has_value()) {
      RemoveControlSocket(run_slot.directory_fd());
      return Presence(query, ProcessPresenceState::kAbsent);
    }

    auto runtime_live = RuntimeProcessIsLive(*state, boot_id);
    auto controller_live = ControllerProcessIsLive(*state, boot_id);
    if (runtime_live && controller_live &&
        ControlSocketMatches(run_slot.directory_fd(), *state)) {
      bool request_uncertain = false;
      try {
        auto connection =
            ConnectControlSocket(run_slot.directory_fd(), *state);
        SendPacketNoSignal(connection.get(), "shutdown");
        const auto acknowledgement = ReceivePacket(
            connection.get(), 64U,
            std::chrono::duration_cast<std::chrono::milliseconds>(kStatusWait));
        request_uncertain = !acknowledgement.has_value() ||
                            *acknowledgement != "shutdown-accepted";
      } catch (const std::exception &) {
        request_uncertain = true;
      }

      const auto deadline = std::chrono::steady_clock::now() + kRuntimeStopWait;
      do {
        runtime_live = RuntimeProcessIsLive(*state, boot_id);
        controller_live = ControllerProcessIsLive(*state, boot_id);
        if (!runtime_live && !controller_live) {
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      } while (std::chrono::steady_clock::now() < deadline);
      if ((runtime_live || controller_live) && request_uncertain) {
        Fail("runtime shutdown outcome is uncertain");
      }
    }

    runtime_live = RuntimeProcessIsLive(*state, boot_id);
    controller_live = ControllerProcessIsLive(*state, boot_id);
    if (runtime_live || controller_live) {
      Fail("runtime controller/process is not safely stopped");
    }
    ReapOwnedChildIfExited(state->controller_pid);
    RemoveState(state_slot->get());
    RemoveControlSocket(run_slot.directory_fd());
    return Presence(query, ProcessPresenceState::kAbsent);
  }

  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const RuntimeEpoch &runtime_epoch) {
    auto [bundle, admission] = Admission(query);
    (void)bundle;
    if (runtime_epoch.empty()) {
      Fail("retained POSIX adapter received an empty runtime epoch");
    }
    const std::string slot_name = SlotName(query);
    std::lock_guard lock(mutex);
    auto state_slot =
        OpenSlotDirectory(state_root_fd.get(), slot_name, false);
    auto run_probe = OpenSlotDirectory(run_root_fd.get(), slot_name, false);
    if (!state_slot.has_value() || !run_probe.has_value()) {
      return std::nullopt;
    }
    run_probe.reset();
    auto run_slot = AcquireSlot(run_root_fd.get(), slot_name, false);
    const auto state = CurrentState(state_slot->get(), query);
    if (!state.has_value() || state->runtime_epoch != runtime_epoch ||
        DurablePresence(*state, run_slot.directory_fd()) !=
            ProcessPresenceState::kPresent) {
      return std::nullopt;
    }
    auto connection = ConnectControlSocket(run_slot.directory_fd(), *state);
    SendPacketNoSignal(connection.get(), "status");
    const auto line = ReceivePacket(
        connection.get(), kMaximumControlPacketBytes,
        std::chrono::duration_cast<std::chrono::milliseconds>(kStatusWait));
    if (!line.has_value() || *line == "pending") {
      return std::nullopt;
    }
    const auto sequence =
        SourceSequenceFromStatusLine(*line, admission, runtime_epoch);
    return RuntimeUnitSourceReport(
        query.plan.runtime_unit_id.value(), runtime_epoch.value(), sequence, 1U,
        RuntimeSourcePhase::kServing, RuntimeSourceHealth::kHealthy,
        query.plan.product_identity.value(),
        query.plan.configuration_identity.value(),
        {admission.runtime_role_id()},
        ManagedCommandSurfaceState::kClosedNoActiveEffect, {},
        admission.runtime_contract_offer_digest());
  }

  std::filesystem::path state_root_path;
  std::filesystem::path run_root_path;
  const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source;
  const manyforge::deployment::IVerifiedRetainedProductArtifactSource
      &artifact_source;
  std::string operating_system;
  std::string shell_contract;
  std::string boot_id;
  std::string observer_instance_id;
  UniqueFd state_root_fd;
  UniqueFd run_root_fd;
  std::mutex mutex;
  std::uint64_t observation_sequence{};
};

RetainedPosixRuntimeUnitAdapter::RetainedPosixRuntimeUnitAdapter(
    std::filesystem::path trusted_state_root,
    std::filesystem::path trusted_run_root,
    const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string operating_system, std::string shell_contract)
    : impl_(std::make_unique<Impl>(
          std::move(trusted_state_root), std::move(trusted_run_root),
          bundle_source, artifact_source, std::move(operating_system),
          std::move(shell_contract))) {}

RetainedPosixRuntimeUnitAdapter::~RetainedPosixRuntimeUnitAdapter() = default;

ProcessPresenceObservation RetainedPosixRuntimeUnitAdapter::ObserveProcess(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->ObserveProcess(query);
}

ProcessPresenceObservation RetainedPosixRuntimeUnitAdapter::EnsureRunning(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->EnsureRunning(query);
}

ProcessPresenceObservation RetainedPosixRuntimeUnitAdapter::EnsureStopped(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->EnsureStopped(query);
}

std::optional<RuntimeUnitSourceReport>
RetainedPosixRuntimeUnitAdapter::ObserveProgress(
    const RuntimeUnitConvergenceQueryDTO &query,
    const RuntimeEpoch &runtime_epoch) {
  return impl_->ObserveProgress(query, runtime_epoch);
}

} // namespace manyforge::component_manager
