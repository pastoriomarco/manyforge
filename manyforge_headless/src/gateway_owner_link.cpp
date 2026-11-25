// The owner link of the gateway core: the pinned run root and the one
// request/reply exchange (canonical JSON over same-user Unix SEQPACKET, one
// request per connection). The behaviour is the one of the Python clients it
// replaces (`private_transport.PinnedRunRoot`, `execution._exchange_on`,
// `execution._decode_execution_response`, `manager._exchange`,
// `manager_scene_runtime._exchange` for the peer check).

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "manyforge_headless/gateway_core.hpp"

namespace manyforge::headless::gateway {

namespace ci = manyforge::content_identity;

GatewayError::GatewayError(std::string code, std::string message,
                           CanonicalValue::Object details,
                           std::string producer)
    : std::runtime_error(std::move(message)),
      code_(std::move(code)),
      details_(std::move(details)),
      producer_(std::move(producer)) {}

namespace {

/// Closes one descriptor at scope end.
class UniqueFd {
 public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
  ~UniqueFd() {
    if (fd_ >= 0) ::close(fd_);
  }
  UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd &operator=(UniqueFd &&) = delete;
  [[nodiscard]] int get() const noexcept { return fd_; }
  [[nodiscard]] int release() noexcept { return std::exchange(fd_, -1); }

 private:
  int fd_;
};

std::string ErrnoText(std::string_view operation, int error) {
  return std::string(operation) + ": [Errno " + std::to_string(error) + "] " +
         std::strerror(error);
}

bool SafeDirectory(const struct stat &metadata) {
  constexpr mode_t kRequired = S_IRUSR | S_IWUSR | S_IXUSR;
  return S_ISDIR(metadata.st_mode) && metadata.st_uid == ::geteuid() &&
         (metadata.st_mode & (S_IWGRP | S_IWOTH)) == 0 &&
         (metadata.st_mode & kRequired) == kRequired;
}

bool SameIdentity(const struct stat &metadata, std::uint64_t device,
                  std::uint64_t inode) {
  return static_cast<std::uint64_t>(metadata.st_dev) == device &&
         static_cast<std::uint64_t>(metadata.st_ino) == inode;
}

/// `Path(path)` of the Python client: no trailing separator.
std::string PlainPath(const std::filesystem::path &path) {
  std::string text = path.lexically_normal().string();
  while (text.size() > 1U && text.back() == '/') text.pop_back();
  return text;
}

struct OpenedRoot {
  UniqueFd descriptor;
  std::uint64_t device{0};
  std::uint64_t inode{0};
};

/// One checked open of the run root (`PinnedRunRoot._open`). `expected`
/// (device, inode) is the pinned identity, or null at pinning.
OpenedRoot OpenRoot(const std::string &path, const std::string &code,
                    const std::pair<std::uint64_t, std::uint64_t> *expected) {
  struct stat before {};
  if (::lstat(path.c_str(), &before) != 0)
    throw GatewayError(code, "private run root is unavailable: " +
                                 ErrnoText("lstat", errno));
  std::unique_ptr<char, decltype(&std::free)> canonical(
      ::realpath(path.c_str(), nullptr), &std::free);
  if (!canonical)
    throw GatewayError(code, "private run root is unavailable: " +
                                 ErrnoText("realpath", errno));
  OpenedRoot opened{UniqueFd(::open(
      path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW))};
  if (opened.descriptor.get() < 0)
    throw GatewayError(code, "private run root is unavailable: " +
                                 ErrnoText("open", errno));
  struct stat descriptor_metadata {};
  struct stat after {};
  if (::fstat(opened.descriptor.get(), &descriptor_metadata) != 0 ||
      ::lstat(path.c_str(), &after) != 0)
    throw GatewayError(code, "private run root changed while opening: " +
                                 ErrnoText("stat", errno));
  opened.device = static_cast<std::uint64_t>(before.st_dev);
  opened.inode = static_cast<std::uint64_t>(before.st_ino);
  const bool safe =
      !path.empty() && path.front() == '/' && path == canonical.get() &&
      SafeDirectory(before) && SafeDirectory(descriptor_metadata) &&
      SafeDirectory(after) &&
      SameIdentity(descriptor_metadata, opened.device, opened.inode) &&
      SameIdentity(after, opened.device, opened.inode) &&
      (expected == nullptr ||
       (expected->first == opened.device && expected->second == opened.inode));
  if (!safe)
    throw GatewayError(code, "private run root is unsafe or was replaced");
  return opened;
}

timeval ToTimeval(std::chrono::nanoseconds remaining) {
  if (remaining < std::chrono::microseconds(1))
    remaining = std::chrono::microseconds(1);
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(remaining);
  const auto micros =
      std::chrono::duration_cast<std::chrono::microseconds>(remaining - seconds);
  return timeval{static_cast<time_t>(seconds.count()),
                 static_cast<suseconds_t>(micros.count())};
}

struct ProtocolFacts {
  std::size_t maximum_packet;
  std::size_t maximum_response;
  int receive_buffer;
  const char *invalid;
  const char *oversized;
  const char *transport_failure;
  const char *unavailable;
  const char *response_invalid;
  const char *label;  ///< message prefix of a reply problem
};

const ProtocolFacts &Facts(OwnerProtocol protocol) {
  static const ProtocolFacts host{
      kMaximumExecutionPacketBytes,
      kMaximumExecutionResponseBytes,
      static_cast<int>(kMaximumExecutionResponseBytes),
      "execution_command_invalid",
      "execution_command_oversized",
      "execution_command_transport_failure",
      "execution_owner_unavailable",
      "execution_command_response_invalid",
      "execution response"};
  static const ProtocolFacts manager{
      kMaximumManagerPacketBytes,
      kMaximumManagerResponseBytes,
      static_cast<int>(kMaximumManagerResponseBytes),
      "manager_command_invalid",
      "manager_command_oversized",
      "manager_command_transport_failure",
      "manager_command_unavailable",
      "manager_command_response_invalid",
      "Manager command response"};
  return protocol == OwnerProtocol::kExecutionHost ? host : manager;
}

}  // namespace

PinnedRunRoot::PinnedRunRoot(std::filesystem::path path,
                             std::string unavailable_code)
    : path_(std::move(path)), unavailable_code_(std::move(unavailable_code)) {
  const auto opened = OpenRoot(PlainPath(path_), unavailable_code_, nullptr);
  device_ = opened.device;
  inode_ = opened.inode;
}

int PinnedRunRoot::Connect(std::string_view leaf,
                           std::chrono::milliseconds connect_limit,
                           int receive_buffer_bytes) const {
  if (leaf.empty() || leaf == "." || leaf == ".." ||
      leaf.find('/') != std::string_view::npos ||
      leaf.find('\\') != std::string_view::npos ||
      leaf.find('\0') != std::string_view::npos)
    throw GatewayError(unavailable_code_, "private socket filename is invalid");
  const std::string leaf_text(leaf);
  const std::pair<std::uint64_t, std::uint64_t> identity{device_, inode_};
  const std::string root_path = PlainPath(path_);
  const auto root = OpenRoot(root_path, unavailable_code_, &identity);

  struct stat metadata {};
  if (::fstatat(root.descriptor.get(), leaf_text.c_str(), &metadata,
                AT_SYMLINK_NOFOLLOW) != 0)
    throw GatewayError(unavailable_code_, "private socket is unavailable: " +
                                              ErrnoText("stat", errno));
  if (!S_ISSOCK(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
      metadata.st_nlink != 1 || (metadata.st_mode & 07777) != 0600)
    throw GatewayError(unavailable_code_,
                       "private endpoint is not the exact same-user socket");

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const std::string target = "/proc/self/fd/" +
                             std::to_string(root.descriptor.get()) + "/" +
                             leaf_text;
  if (target.size() >= sizeof(address.sun_path))
    throw GatewayError(unavailable_code_, "private socket filename is invalid");
  std::memcpy(address.sun_path, target.c_str(), target.size() + 1U);

  const auto failed = [this](std::string_view operation, int error) {
    return GatewayError(unavailable_code_,
                        "native transport failed: " + ErrnoText(operation, error));
  };
  UniqueFd client(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
  if (client.get() < 0) throw failed("socket", errno);
  if (receive_buffer_bytes > 0 &&
      ::setsockopt(client.get(), SOL_SOCKET, SO_RCVBUF, &receive_buffer_bytes,
                   sizeof(receive_buffer_bytes)) != 0)
    throw failed("setsockopt", errno);

  // Only the connect is limited: a blocking Unix connect waits for room in
  // the listener's backlog for at most SO_SNDTIMEO. Send and receive then
  // have no limit (OD-214).
  const auto deadline = std::chrono::steady_clock::now() + connect_limit;
  for (;;) {
    const timeval limit = ToTimeval(deadline - std::chrono::steady_clock::now());
    if (::setsockopt(client.get(), SOL_SOCKET, SO_SNDTIMEO, &limit,
                     sizeof(limit)) != 0)
      throw failed("setsockopt", errno);
    if (::connect(client.get(), reinterpret_cast<const sockaddr *>(&address),
                  sizeof(address)) == 0 ||
        errno == EISCONN)
      break;
    if (errno == EINTR && std::chrono::steady_clock::now() < deadline) continue;
    throw failed("connect", errno == EINTR ? ETIMEDOUT : errno);
  }
  const timeval unlimited{0, 0};
  if (::setsockopt(client.get(), SOL_SOCKET, SO_SNDTIMEO, &unlimited,
                   sizeof(unlimited)) != 0)
    throw failed("setsockopt", errno);

  // The directory was used for the connect only; it must still be the
  // pinned one (`PinnedRunRoot._open` after use).
  struct stat final_metadata {};
  if (::lstat(root_path.c_str(), &final_metadata) != 0 ||
      !SafeDirectory(final_metadata) ||
      !SameIdentity(final_metadata, device_, inode_))
    throw GatewayError(unavailable_code_, "private run root changed during use");

  ucred credentials{};
  socklen_t length = sizeof(credentials);
  if (::getsockopt(client.get(), SOL_SOCKET, SO_PEERCRED, &credentials,
                   &length) != 0)
    throw failed("getsockopt(SO_PEERCRED)", errno);
  if (credentials.uid != ::geteuid())
    throw GatewayError(unavailable_code_,
                       "native transport failed: peer is not owned by the "
                       "current user");
  return client.release();
}

CanonicalValue Exchange(const PinnedRunRoot &root, std::string_view leaf,
                        OwnerProtocol protocol,
                        std::string_view response_version,
                        const CanonicalValue &command,
                        std::chrono::milliseconds connect_limit,
                        const std::optional<std::chrono::milliseconds> limit) {
  const auto &facts = Facts(protocol);
  const bool host = protocol == OwnerProtocol::kExecutionHost;
  const std::string encoded = ci::canonical_json(command);
  if (encoded.size() > facts.maximum_packet)
    throw GatewayError(facts.oversized,
                       host ? "execution command exceeds its byte bound"
                            : "Manager command exceeds its byte bound");

  UniqueFd client(root.Connect(
      leaf, limit.has_value() ? std::min(connect_limit, *limit) : connect_limit,
      facts.receive_buffer));
  const auto failed = [&](std::string_view operation, int error) {
    return GatewayError(
        facts.unavailable,
        std::string(host ? "native execution transport failed: "
                         : "Manager command transport failed: ") +
            ErrnoText(operation, error));
  };
  // A send or receive ended by `limit` (EAGAIN) names it in its details
  // (`reason` `limit_expired`), for a caller whose expiry has its own
  // outcome.
  const auto ended = [&](std::string_view operation, int error) {
    auto cause = failed(operation, error);
    if (!limit.has_value() || (error != EAGAIN && error != EWOULDBLOCK))
      return cause;
    return GatewayError(
        cause.code(), cause.what(),
        {{"reason", CanonicalValue(std::string("limit_expired"))}});
  };
  if (limit.has_value()) {
    // The send and the receive each end within `limit` (EAGAIN).
    const timeval each = ToTimeval(*limit);
    if (::setsockopt(client.get(), SOL_SOCKET, SO_SNDTIMEO, &each,
                     sizeof(each)) != 0 ||
        ::setsockopt(client.get(), SOL_SOCKET, SO_RCVTIMEO, &each,
                     sizeof(each)) != 0)
      throw failed("setsockopt", errno);
  }

  ssize_t sent = -1;
  do {
    sent = ::send(client.get(), encoded.data(), encoded.size(), MSG_NOSIGNAL);
  } while (sent < 0 && errno == EINTR);
  if (sent < 0) throw ended("send", errno);
  if (static_cast<std::size_t>(sent) != encoded.size())
    throw GatewayError(facts.transport_failure,
                       host ? "execution command packet was truncated"
                            : "Manager command packet was truncated");

  // No time limit unless `limit`: the reply, or the owner closing the
  // connection, ends it.
  const auto buffer =
      std::make_unique_for_overwrite<char[]>(facts.maximum_response);
  iovec part{buffer.get(), facts.maximum_response};
  msghdr message{};
  message.msg_iov = &part;
  message.msg_iovlen = 1;
  ssize_t received = -1;
  do {
    message.msg_flags = 0;
    received = ::recvmsg(client.get(), &message, 0);
  } while (received < 0 && errno == EINTR);
  if (received < 0) throw ended("recvmsg", errno);
  const bool truncated = (message.msg_flags & MSG_TRUNC) != 0;
  if (host) {
    if (truncated || received == 0)
      throw GatewayError(facts.response_invalid,
                         "execution response is empty or oversized");
  } else {
    if (received == 0)
      // The Manager closed the connection before answering (it exited or is
      // stopping); the command's outcome is unknown.
      throw GatewayError(facts.unavailable,
                         "Manager closed the connection before answering");
    if (truncated)
      throw GatewayError(facts.response_invalid,
                         "Manager command response is oversized");
  }
  const std::string label(facts.label);
  CanonicalValue response;
  try {
    ci::CanonicalJsonParseLimits limits;
    limits.maximum_bytes = facts.maximum_response;
    limits.maximum_depth = ci::kMaximumCanonicalJsonDepth;
    response = ci::parse_canonical_json(
        std::string_view(buffer.get(), static_cast<std::size_t>(received)),
        limits);
  } catch (const ci::CanonicalValueError &error) {
    throw GatewayError(facts.response_invalid,
                       label + " is not canonical JSON: " + error.what());
  }
  if (!response.is_object())
    throw GatewayError(facts.response_invalid, label + " root must be an object");
  const auto *version = response.find_object_member("api_version");
  if (version == nullptr || !version->is_string() ||
      version->as_string() != response_version)
    throw GatewayError(facts.response_invalid,
                       label + " version is unsupported");
  const auto *ok = response.find_object_member("ok");
  if (ok == nullptr || !ok->is_bool())
    throw GatewayError(facts.response_invalid,
                       label + " has no boolean result");
  return response;
}

}  // namespace manyforge::headless::gateway
