#include "manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <manyforge_component_manager/reference_runtime_unit_admission.hpp>
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

constexpr std::string_view kStateFormat = "manyforge.retained-posix-process.v1";
constexpr std::size_t kMaximumStateBytes = 4096U;
constexpr std::size_t kMaximumStatusLineBytes = 2048U;
constexpr auto kSlotLockTimeout = std::chrono::seconds(2);
constexpr auto kStatusWait = std::chrono::milliseconds(500);
constexpr auto kChildExitWait = std::chrono::milliseconds(500);

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
  pid_t pid{};
  std::uint64_t start_ticks{};
  RuntimeEpoch runtime_epoch;
};

[[nodiscard]] std::string EncodeState(const ProcessState &state) {
  return "format=" + std::string(kStateFormat) + "\n" +
         "runtime_unit_id=" + state.runtime_unit_id + "\n" +
         "product_identity=" + state.product_identity + "\n" +
         "configuration_identity=" + state.configuration_identity + "\n" +
         "process_adapter_revision=" + state.process_adapter_revision + "\n" +
         "boot_id=" + state.boot_id + "\n" +
         "pid=" + std::to_string(state.pid) + "\n" +
         "start_ticks=" + std::to_string(state.start_ticks) + "\n" +
         "runtime_epoch=" + state.runtime_epoch.value() + "\n";
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
  if (lines.size() != 9U ||
      lines.front() != "format=" + std::string(kStateFormat)) {
    Fail("runtime process state has the wrong closed schema");
  }
  const auto pid_text = StateValue(lines[6], "pid=");
  const auto pid_value =
      ParseCanonicalUnsigned(pid_text, "runtime process pid");
  if (pid_value == 0U || pid_value > static_cast<std::uint64_t>(
                                         std::numeric_limits<pid_t>::max())) {
    Fail("runtime process pid is out of range");
  }
  const auto start_ticks_text = StateValue(lines[7], "start_ticks=");
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
      static_cast<pid_t>(pid_value),
      start_ticks,
      RuntimeEpoch(StateValue(lines[8], "runtime_epoch=")),
  };
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

[[nodiscard]] bool ProcessIsLive(const ProcessState &state,
                                 const std::string_view boot_id) {
  if (state.boot_id != boot_id) {
    return false;
  }
  const auto observed = ReadProcObservation(state.pid);
  return observed.has_value() && observed->start_ticks == state.start_ticks &&
         observed->state != 'Z' && observed->state != 'X';
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

private:
  UniqueFd directory_;
  UniqueFd lock_;
};

[[nodiscard]] SlotLock AcquireSlot(const int root_fd,
                                   const std::string &slot_name,
                                   const bool create) {
  int directory_fd = ::openat(root_fd, slot_name.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (directory_fd < 0 && errno == ENOENT && create) {
    if (::mkdirat(root_fd, slot_name.c_str(), 0700) != 0 && errno != EEXIST) {
      Fail(ErrnoDetail("could not create runtime adapter slot", errno));
    }
    directory_fd = ::openat(root_fd, slot_name.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  }
  if (directory_fd < 0) {
    Fail(ErrnoDetail("could not open runtime adapter slot", errno));
  }
  UniqueFd directory(directory_fd);
  const int lock_flags =
      O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0);
  const int lock_fd = ::openat(directory.get(), "lock", lock_flags, 0600);
  if (lock_fd < 0) {
    Fail(ErrnoDetail("could not open runtime adapter slot lock", errno));
  }
  UniqueFd lock(lock_fd);
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
  return SlotLock(std::move(directory), std::move(lock));
}

[[nodiscard]] std::optional<std::string>
ReadFileAt(const int directory_fd, const std::string_view name,
           const std::size_t maximum_bytes) {
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
  if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode)) {
    Fail("runtime adapter state is not a regular file");
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

void MaterializeLaunchable(const int directory_fd,
                           const ReferenceRuntimeUnitAdmission &admission) {
  const auto existing = ReadFileAt(directory_fd, "launchable",
                                   admission.launchable_bytes().size());
  if (existing.has_value()) {
    if (*existing != admission.launchable_bytes()) {
      Fail("materialized runtime launchable conflicts with verified retained "
           "bytes");
    }
    const int raw_fd =
        ::openat(directory_fd, "launchable", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (raw_fd < 0) {
      Fail(ErrnoDetail("could not reopen materialized runtime launchable",
                       errno));
    }
    const UniqueFd fd(raw_fd);
    struct stat status {};
    if (::fstat(fd.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
        (status.st_mode & 0777) != 0500) {
      Fail("materialized runtime launchable has invalid type or permissions");
    }
    return;
  }
  AtomicWriteAt(directory_fd, "launchable", admission.launchable_bytes(), 0500);
}

[[nodiscard]] std::optional<ProcessState> ReadState(const int directory_fd) {
  const auto bytes =
      ReadFileAt(directory_fd, "process.state", kMaximumStateBytes);
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

void WaitForChildExit(const pid_t pid) noexcept {
  const auto deadline = std::chrono::steady_clock::now() + kChildExitWait;
  for (;;) {
    int status = 0;
    const pid_t result = ::waitpid(pid, &status, WNOHANG);
    if (result == pid || (result < 0 && errno == ECHILD)) {
      return;
    }
    if (result < 0 && errno != EINTR) {
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

struct Channel final {
  pid_t pid{};
  std::uint64_t start_ticks{};
  RuntimeEpoch runtime_epoch;
  UniqueFd standard_input;
  UniqueFd standard_output;
  std::string output_buffer;
  std::uint64_t next_source_sequence{1U};
  bool status_request_outstanding{true};
};

[[nodiscard]] std::optional<std::string> ReadStatusLine(Channel &channel) {
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

} // namespace

struct RetainedPosixRuntimeUnitAdapter::Impl final {
  Impl(
      std::filesystem::path trusted_runtime_root,
      const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
      const manyforge::deployment::IVerifiedRetainedProductArtifactSource
          &artifact_source,
      std::string operating_system, std::string shell_contract)
      : root_path(std::move(trusted_runtime_root)),
        bundle_source(bundle_source), artifact_source(artifact_source),
        operating_system(std::move(operating_system)),
        shell_contract(std::move(shell_contract)), boot_id(HostBootId()),
        observer_instance_id("posix-observer-" + RandomHex()) {
    if (!root_path.is_absolute() || this->operating_system.empty() ||
        this->shell_contract.empty()) {
      throw std::invalid_argument(
          "retained POSIX adapter construction input is invalid");
    }
    const int fd = ::open(root_path.c_str(),
                          O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      Fail(ErrnoDetail("could not open trusted runtime root", errno));
    }
    root_fd.Reset(fd);
    struct stat status {};
    if (::fstat(root_fd.get(), &status) != 0 || !S_ISDIR(status.st_mode)) {
      Fail("trusted runtime root is not a directory");
    }
  }

  ~Impl() {
    std::vector<pid_t> children;
    {
      std::lock_guard lock(mutex);
      children.reserve(channels.size());
      for (auto &[unused, channel] : channels) {
        (void)unused;
        children.push_back(channel.pid);
        channel.standard_input.Reset();
      }
      channels.clear();
    }
    for (const auto child : children) {
      WaitForChildExit(child);
    }
  }

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
  CurrentState(const int directory_fd,
               const RuntimeUnitConvergenceQueryDTO &query) {
    auto state = ReadState(directory_fd);
    if (!state.has_value()) {
      return std::nullopt;
    }
    VerifyStateMatchesQuery(*state, query);
    return state;
  }

  void DropLocalChannel(const std::string &slot_name,
                        const std::optional<ProcessState> &state) {
    const auto found = channels.find(slot_name);
    if (found == channels.end()) {
      return;
    }
    if (!state.has_value() || found->second.pid != state->pid ||
        found->second.start_ticks != state->start_ticks ||
        found->second.runtime_epoch != state->runtime_epoch) {
      const pid_t child = found->second.pid;
      channels.erase(found);
      WaitForChildExit(child);
    }
  }

  [[nodiscard]] ProcessPresenceObservation
  ObserveLocked(const RuntimeUnitConvergenceQueryDTO &query,
                const std::string &slot_name, const int directory_fd) {
    const auto state = CurrentState(directory_fd, query);
    if (!state.has_value() || !ProcessIsLive(*state, boot_id)) {
      DropLocalChannel(slot_name, std::nullopt);
      return Presence(query, ProcessPresenceState::kAbsent);
    }
    DropLocalChannel(slot_name, state);
    return Presence(query, ProcessPresenceState::kPresent,
                    state->runtime_epoch.value());
  }

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) {
    static_cast<void>(Admission(query));
    const std::string slot_name = SlotName(query);
    std::lock_guard lock(mutex);
    const int probe = ::openat(root_fd.get(), slot_name.c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (probe < 0) {
      if (errno == ENOENT) {
        return Presence(query, ProcessPresenceState::kAbsent);
      }
      Fail(ErrnoDetail("could not inspect runtime adapter slot", errno));
    }
    UniqueFd probe_fd(probe);
    const int lock_probe =
        ::openat(probe_fd.get(), "lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (lock_probe < 0 && errno == ENOENT) {
      return Presence(query, ProcessPresenceState::kUnknown);
    }
    if (lock_probe < 0) {
      Fail(ErrnoDetail("could not inspect runtime adapter slot lock", errno));
    }
    UniqueFd probe_lock(lock_probe);
    probe_lock.Reset();
    probe_fd.Reset();
    auto slot = AcquireSlot(root_fd.get(), slot_name, false);
    return ObserveLocked(query, slot_name, slot.directory_fd());
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) {
    auto [bundle, admission] = Admission(query);
    (void)bundle;
    const std::string slot_name = SlotName(query);
    std::lock_guard lock(mutex);
    auto slot = AcquireSlot(root_fd.get(), slot_name, true);
    auto current = CurrentState(slot.directory_fd(), query);
    if (current.has_value() && ProcessIsLive(*current, boot_id)) {
      DropLocalChannel(slot_name, current);
      return Presence(query, ProcessPresenceState::kPresent,
                      current->runtime_epoch.value());
    }
    DropLocalChannel(slot_name, std::nullopt);
    if (current.has_value()) {
      RemoveState(slot.directory_fd());
    }
    MaterializeLaunchable(slot.directory_fd(), admission);

    const RuntimeEpoch runtime_epoch("runtime-" + RandomHex());
    const auto launch_arguments = admission.launch_arguments(runtime_epoch);
    const std::filesystem::path launch_path =
        root_path / slot_name / "launchable";
    std::vector<std::string> arguments;
    arguments.reserve(launch_arguments.size() + 1U);
    arguments.push_back(launch_path.string());
    arguments.insert(arguments.end(), launch_arguments.begin(),
                     launch_arguments.end());
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

    auto standard_input = MakeSocketPair();
    auto standard_output = MakePipe();
    auto gate = MakeSocketPair();
    SetNonBlocking(standard_output[0].get());
    const UniqueFd null_error(::open("/dev/null", O_WRONLY | O_CLOEXEC));
    if (!null_error) {
      Fail(ErrnoDetail("could not open fixed runtime stderr sink", errno));
    }

    const pid_t child = ::fork();
    if (child < 0) {
      Fail(ErrnoDetail("could not fork retained runtime", errno));
    }
    if (child == 0) {
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
      const int result =
          ::execve(launch_path.c_str(), argv.data(), environment.data());
      (void)result;
      ::_exit(127);
    }

    standard_input[0].Reset();
    standard_output[1].Reset();
    gate[0].Reset();
    bool released = false;
    bool state_written = false;
    try {
      const auto observed = ReadProcObservation(child);
      if (!observed.has_value() || observed->state == 'Z' ||
          observed->state == 'X' || observed->start_ticks == 0U) {
        Fail("forked runtime disappeared before durable process admission");
      }
      ProcessState state{
          query.plan.runtime_unit_id.value(),
          query.plan.product_identity.value(),
          query.plan.configuration_identity.value(),
          query.plan.process_adapter_contract_revision,
          boot_id,
          child,
          observed->start_ticks,
          runtime_epoch,
      };
      AtomicWriteAt(slot.directory_fd(), "process.state", EncodeState(state),
                    0600);
      state_written = true;
      Channel channel{child,
                      observed->start_ticks,
                      runtime_epoch,
                      std::move(standard_input[1]),
                      std::move(standard_output[0]),
                      {},
                      1U,
                      true};
      channels.insert_or_assign(slot_name, std::move(channel));
      SendAllNoSignal(gate[1].get(), "R");
      released = true;
      gate[1].Reset();
      return Presence(query, ProcessPresenceState::kPresent,
                      runtime_epoch.value());
    } catch (...) {
      gate[1].Reset();
      channels.erase(slot_name);
      WaitForChildExit(child);
      if (state_written && !released) {
        try {
          RemoveState(slot.directory_fd());
        } catch (...) {
        }
      }
      throw;
    }
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
    const int probe = ::openat(root_fd.get(), slot_name.c_str(),
                               O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (probe < 0) {
      if (errno == ENOENT) {
        return std::nullopt;
      }
      Fail(ErrnoDetail("could not inspect runtime progress slot", errno));
    }
    UniqueFd probe_fd(probe);
    probe_fd.Reset();
    auto slot = AcquireSlot(root_fd.get(), slot_name, false);
    const auto state = CurrentState(slot.directory_fd(), query);
    if (!state.has_value() || state->runtime_epoch != runtime_epoch ||
        !ProcessIsLive(*state, boot_id)) {
      DropLocalChannel(slot_name, std::nullopt);
      return std::nullopt;
    }
    const auto found = channels.find(slot_name);
    if (found == channels.end() || found->second.pid != state->pid ||
        found->second.start_ticks != state->start_ticks ||
        found->second.runtime_epoch != state->runtime_epoch) {
      return std::nullopt;
    }
    Channel &channel = found->second;
    if (!channel.standard_input || !channel.standard_output) {
      return std::nullopt;
    }
    if (!channel.status_request_outstanding) {
      try {
        SendAllNoSignal(channel.standard_input.get(),
                        admission.status_command());
        channel.status_request_outstanding = true;
      } catch (const RetainedPosixRuntimeUnitAdapterError &) {
        channel.standard_input.Reset();
        return std::nullopt;
      }
    }
    const auto line = ReadStatusLine(channel);
    if (!line.has_value()) {
      return std::nullopt;
    }
    const auto sequence = channel.next_source_sequence;
    if (*line != admission.expected_status_line(runtime_epoch, sequence)) {
      Fail("retained runtime emitted a malformed or foreign status record");
    }
    if (sequence == std::numeric_limits<std::uint64_t>::max()) {
      Fail("retained runtime source-report sequence is exhausted");
    }
    ++channel.next_source_sequence;
    channel.status_request_outstanding = false;
    return RuntimeUnitSourceReport(
        query.plan.runtime_unit_id.value(), runtime_epoch.value(), sequence, 1U,
        RuntimeSourcePhase::kServing, RuntimeSourceHealth::kHealthy,
        query.plan.product_identity.value(),
        query.plan.configuration_identity.value(),
        {admission.runtime_role_id()},
        ManagedCommandSurfaceState::kClosedNoActiveEffect);
  }

  std::filesystem::path root_path;
  const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source;
  const manyforge::deployment::IVerifiedRetainedProductArtifactSource
      &artifact_source;
  std::string operating_system;
  std::string shell_contract;
  std::string boot_id;
  std::string observer_instance_id;
  UniqueFd root_fd;
  std::mutex mutex;
  std::uint64_t observation_sequence{};
  std::map<std::string, Channel> channels;
};

RetainedPosixRuntimeUnitAdapter::RetainedPosixRuntimeUnitAdapter(
    std::filesystem::path trusted_runtime_root,
    const manyforge::deployment::IVerifiedRetainedBundleSource &bundle_source,
    const manyforge::deployment::IVerifiedRetainedProductArtifactSource
        &artifact_source,
    std::string operating_system, std::string shell_contract)
    : impl_(std::make_unique<Impl>(
          std::move(trusted_runtime_root), bundle_source, artifact_source,
          std::move(operating_system), std::move(shell_contract))) {}

RetainedPosixRuntimeUnitAdapter::~RetainedPosixRuntimeUnitAdapter() = default;

ProcessPresenceObservation RetainedPosixRuntimeUnitAdapter::ObserveProcess(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->ObserveProcess(query);
}

ProcessPresenceObservation RetainedPosixRuntimeUnitAdapter::EnsureRunning(
    const RuntimeUnitConvergenceQueryDTO &query) {
  return impl_->EnsureRunning(query);
}

std::optional<RuntimeUnitSourceReport>
RetainedPosixRuntimeUnitAdapter::ObserveProgress(
    const RuntimeUnitConvergenceQueryDTO &query,
    const RuntimeEpoch &runtime_epoch) {
  return impl_->ObserveProgress(query, runtime_epoch);
}

} // namespace manyforge::component_manager
