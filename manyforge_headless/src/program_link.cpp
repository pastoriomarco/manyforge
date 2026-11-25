// The Program runner's command and status sockets (OD-380 D5); see
// program_link.hpp for the protocol.

#include "manyforge_headless/program_link.hpp"

#include "manager_response_transport_internal.hpp"
#include "manyforge_headless/gateway_core.hpp"

#include <manyforge_private_stream/private_stream_server.hpp>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iterator>
#include <limits>
#include <list>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>
#include <utility>

namespace manyforge::headless::gateway {
namespace {

using private_stream::Fd;
using Clock = std::chrono::steady_clock;
/// The end of a wait; none: no time limit.
using Deadline = std::optional<Clock::time_point>;

constexpr std::string_view kRunnerProducer = "program_runner";
constexpr std::string_view kSocketUnavailable = "program_runner_socket_unavailable";
/// The first byte of every packet of a message but its last (see
/// program_link.hpp).
constexpr char kContinued = '\0';
/// A status reader that takes no frame for this long is dropped (the
/// WebSocket's former ping timeout); a closed reader ends its writer at once.
constexpr std::chrono::milliseconds kStatusSendDeadline{20000};

void Signal(const int wake) noexcept {
  const std::uint64_t one = 1U;
  // Fails only when the counter is already non-zero: woken either way.
  [[maybe_unused]] const auto written = ::write(wake, &one, sizeof(one));
}

[[nodiscard]] Fd NewEventFd() {
  Fd wake(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK));
  if (wake.get() < 0)
    throw std::runtime_error(std::string("eventfd failed: ") +
                             std::strerror(errno));
  return wake;
}

[[nodiscard]] std::string JsonText(const std::string_view text) {
  return content_identity::canonical_json(
      content_identity::CanonicalValue(std::string(text)));
}

/// A listening socket bound in a private root through its directory
/// descriptor (the root is checked like the runner's liveness root: a
/// directory of this user, not writable by group or others), held under the
/// socket's single-server lock `.<leaf>.lock` (flock, as the Manager and the
/// execution host hold their roots); removed again when it is still the one
/// bound here.
class BoundSocket final {
 public:
  BoundSocket(const std::filesystem::path &root, const std::string_view leaf)
      : leaf_(leaf) {
    directory_ = std::make_unique<Fd>(::open(
        root.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (directory_->get() < 0)
      Fail("open the Program socket root");
    struct stat status {};
    if (::fstat(directory_->get(), &status) != 0)
      Fail("inspect the Program socket root");
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
        (status.st_mode & S_IRWXU) != S_IRWXU)
      Refuse("the Program socket root is not a private directory of this user");
    const std::string lock_name = "." + leaf_ + ".lock";
    lock_ = std::make_unique<Fd>(::openat(
        directory_->get(), lock_name.c_str(),
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
    if (lock_->get() < 0)
      Fail("open the Program socket lock");
    struct stat lock_status {};
    if (::fstat(lock_->get(), &lock_status) != 0)
      Fail("inspect the Program socket lock");
    if (!S_ISREG(lock_status.st_mode))
      Refuse("the Program socket lock is not a regular file");
    if (::flock(lock_->get(), LOCK_EX | LOCK_NB) != 0) {
      if (errno == EWOULDBLOCK)
        Refuse("another Program runner already serves " + leaf_ + " in this root");
      Fail("acquire the Program socket lock");
    }
    struct stat existing {};
    if (::fstatat(directory_->get(), leaf_.c_str(), &existing,
                  AT_SYMLINK_NOFOLLOW) == 0) {
      // Under the lock: a socket left by an ended runner of this user is
      // replaced.
      if (!S_ISSOCK(existing.st_mode) || existing.st_uid != ::geteuid())
        Refuse("the Program socket path is not a socket");
      if (::unlinkat(directory_->get(), leaf_.c_str(), 0) != 0)
        Fail("remove a stale Program socket");
    } else if (errno != ENOENT) {
      Fail("inspect the Program socket path");
    }
    const std::string address_text = "/proc/self/fd/" +
                                      std::to_string(directory_->get()) + "/" +
                                      leaf_;
    sockaddr_un address{};
    if (address_text.size() >= sizeof(address.sun_path))
      Refuse("the Program socket path exceeds the AF_UNIX bound");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, address_text.c_str(), address_text.size() + 1U);
    listener_ = std::make_unique<Fd>(
        ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (listener_->get() < 0)
      Fail("create the Program socket");
    if (::bind(listener_->get(), reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != 0)
      Fail("bind the Program socket");
    struct stat bound {};
    if (::fstatat(directory_->get(), leaf_.c_str(), &bound,
                  AT_SYMLINK_NOFOLLOW) != 0)
      Fail("inspect the bound Program socket");
    device_ = bound.st_dev;
    inode_ = bound.st_ino;
    bound_ = true;
    if (::fchmodat(directory_->get(), leaf_.c_str(), S_IRUSR | S_IWUSR, 0) != 0)
      Fail("restrict the Program socket");
    struct stat current {};
    if (::fstatat(directory_->get(), leaf_.c_str(), &current,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        current.st_dev != device_ || current.st_ino != inode_ ||
        !S_ISSOCK(current.st_mode) || current.st_uid != ::geteuid() ||
        current.st_nlink != 1 || (current.st_mode & 07777) != 0600)
      Refuse("the Program socket changed while binding");
    if (::listen(listener_->get(), 64) != 0)
      Fail("listen on the Program socket");
  }

  /// Removes the socket, then (members) releases the lock.
  ~BoundSocket() {
    struct stat current {};
    if (bound_ &&
        ::fstatat(directory_->get(), leaf_.c_str(), &current,
                  AT_SYMLINK_NOFOLLOW) == 0 &&
        current.st_dev == device_ && current.st_ino == inode_)
      static_cast<void>(::unlinkat(directory_->get(), leaf_.c_str(), 0));
  }

  BoundSocket(const BoundSocket &) = delete;
  BoundSocket &operator=(const BoundSocket &) = delete;

  [[nodiscard]] int get() const noexcept { return listener_->get(); }

 private:
  [[noreturn]] static void Refuse(const std::string &message) {
    throw GatewayError(std::string(kSocketUnavailable), message, {},
                       std::string(kRunnerProducer));
  }
  [[noreturn]] static void Fail(const std::string_view what) {
    Refuse(std::string("cannot ") + std::string(what) + ": " +
           std::strerror(errno));
  }

  std::string leaf_;
  std::unique_ptr<Fd> directory_;
  /// Declared before the listener: released after the socket is removed.
  std::unique_ptr<Fd> lock_;
  std::unique_ptr<Fd> listener_;
  dev_t device_{};
  ino_t inode_{};
  bool bound_{false};
};

/// poll()'s timeout for `deadline`: -1 without one, else the milliseconds
/// left, rounded up (0 once passed).
[[nodiscard]] int PollTimeout(const Deadline &deadline) noexcept {
  if (!deadline.has_value())
    return -1;
  const auto left = std::chrono::ceil<std::chrono::milliseconds>(
                        *deadline - Clock::now())
                        .count();
  return static_cast<int>(std::clamp<long long>(
      left, 0, std::numeric_limits<int>::max()));
}

/// The packet size for a connection of `capacity` atomic send bytes.
[[nodiscard]] std::size_t PacketSize(const std::size_t capacity) noexcept {
  return std::max<std::size_t>(capacity / 4U, 2U);
}

/// Calls `send` with each packet of the non-empty message `bytes` (see
/// program_link.hpp), each at most `packet_size` bytes; stops at the first
/// false.
template <typename Send>
[[nodiscard]] bool ForEachPacket(std::string_view bytes,
                                 const std::size_t packet_size, Send &&send) {
  std::string packet;
  while (bytes.size() > packet_size) {
    packet.assign(1U, kContinued);
    packet.append(bytes.substr(0U, packet_size - 1U));
    if (!send(std::string_view(packet)))
      return false;
    bytes.remove_prefix(packet_size - 1U);
  }
  return send(bytes);
}

/// kMore: a packet of the message was read and more follow; kLast: the
/// message's last packet was read; kEnd: the peer ended its sending (or
/// closed) first; kTruncated: it did so in the middle of a message.
enum class Read { kMore, kLast, kEnd, kTruncated, kFailed, kStopped, kTimedOut };

/// Waits until `deadline` for the next packet of `fd`, or for `wake` (-1:
/// none). Appends its message bytes to `out` unless they would pass `limit`
/// in all; a packet over the limit is dropped and `over` set.
[[nodiscard]] Read ReadPacket(const int fd, const int wake, std::string &out,
                              const std::size_t limit, bool &over,
                              const Deadline &deadline) {
  for (;;) {
    pollfd ready[2]{{fd, POLLIN, 0}, {wake, POLLIN, 0}};
    const int count = ::poll(ready, wake >= 0 ? 2 : 1, PollTimeout(deadline));
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0)
      return Read::kFailed;
    if (wake >= 0 && (ready[1].revents & POLLIN) != 0)
      return Read::kStopped;
    if (count == 0)
      return Read::kTimedOut;
    if (ready[0].revents == 0)
      continue;
    // SEQPACKET: the next packet's real length (0 at the peer's end) and
    // its first byte.
    char first = 0;
    const auto size =
        ::recv(fd, &first, sizeof(first), MSG_PEEK | MSG_TRUNC | MSG_DONTWAIT);
    if (size < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      continue;
    if (size < 0)
      return Read::kFailed;
    if (size == 0) {
      static_cast<void>(::recv(fd, &first, sizeof(first), MSG_DONTWAIT));
      return Read::kEnd;
    }
    const bool continued = first == kContinued;
    const auto length = static_cast<std::size_t>(size);
    const auto bytes = length - (continued ? 1U : 0U);
    if (over || bytes > limit - std::min(limit, out.size())) {
      over = true;
      // A one-byte read with MSG_TRUNC dequeues the whole packet.
      if (::recv(fd, &first, sizeof(first), MSG_TRUNC | MSG_DONTWAIT) < 0)
        return Read::kFailed;
      return continued ? Read::kMore : Read::kLast;
    }
    const auto offset = out.size();
    out.resize(offset + length);
    const auto received = ::recv(fd, out.data() + offset, length, MSG_DONTWAIT);
    if (received != size) {
      out.resize(offset);
      return Read::kFailed;
    }
    if (continued)
      out.erase(offset, 1U);
    return continued ? Read::kMore : Read::kLast;
  }
}

/// Reads one whole message into `out` (kLast).
[[nodiscard]] Read ReadMessage(const int fd, const int wake, std::string &out,
                               const std::size_t limit, bool &over,
                               const Deadline &deadline) {
  for (bool started = false;; started = true) {
    const auto result = ReadPacket(fd, wake, out, limit, over, deadline);
    if (result == Read::kEnd && started)
      return Read::kTruncated;
    if (result != Read::kMore)
      return result;
  }
}

enum class Sent { kSent, kFailed, kStopped, kTimedOut };

/// Sends the message `bytes` as its packets (see program_link.hpp). Waits for
/// room until `deadline`; `wake` (-1: none) ends the wait.
[[nodiscard]] Sent SendMessage(const int fd, const int wake,
                               const std::string_view bytes,
                               const std::size_t maximum,
                               const Deadline &deadline) {
  const auto capacity = detail::AtomicResponseCapacity(fd, maximum);
  if (capacity == 0U || bytes.empty())
    return Sent::kFailed;
  Sent result = Sent::kSent;
  static_cast<void>(ForEachPacket(
      bytes, PacketSize(capacity), [&](const std::string_view packet) {
        for (;;) {
          const auto sent = ::send(fd, packet.data(), packet.size(),
                                   MSG_NOSIGNAL | MSG_DONTWAIT);
          if (sent == static_cast<ssize_t>(packet.size()))
            return true;
          if (sent >= 0 ||
              (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            result = Sent::kFailed;
            return false;
          }
          pollfd ready[2]{{fd, POLLOUT, 0}, {wake, POLLIN, 0}};
          const int count =
              ::poll(ready, wake >= 0 ? 2 : 1, PollTimeout(deadline));
          if (count < 0 && errno != EINTR) {
            result = Sent::kFailed;
            return false;
          }
          if (wake >= 0 && (ready[1].revents & POLLIN) != 0) {
            result = Sent::kStopped;
            return false;
          }
          if (count == 0) {
            result = Sent::kTimedOut;
            return false;
          }
        }
      }));
  return result;
}

/// Sends one status message as its packets; each packet waits for room at
/// most kStatusSendDeadline (a reader that takes nothing for that long is
/// dropped) and `stop` ends the wait.
[[nodiscard]] bool SendStatusMessage(const int client,
                                     const std::string_view bytes,
                                     const std::size_t capacity,
                                     const std::stop_token &stop) {
  return ForEachPacket(bytes, PacketSize(capacity),
                       [&](const std::string_view packet) {
                         return private_stream::SendFrameBytes(
                             client, packet, capacity, kStatusSendDeadline,
                             stop);
                       });
}

/// A reply the core itself sends (canonical JSON).
[[nodiscard]] std::string ErrorReply(const std::string_view code,
                                     const std::string_view message) {
  using Value = content_identity::CanonicalValue;
  return content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kProgramCommandResponseApiVersion))},
      {"error", Value(Value::Object{
                    {"code", Value(std::string(code))},
                    {"details", Value(Value::Object{})},
                    {"message", Value(std::string(message))},
                    {"producer", Value(std::string(kRunnerProducer))}})},
      {"ok", Value(false)}}));
}

[[nodiscard]] GatewayError RunnerError(const std::string_view code,
                                       const std::string_view message,
                                       const std::string_view transport_error = {}) {
  content_identity::CanonicalValue::Object details;
  if (!transport_error.empty())
    details.emplace_back(
        "transport_error",
        content_identity::CanonicalValue(std::string(transport_error)));
  return GatewayError(std::string(code), std::string(message), std::move(details),
                      std::string(kRunnerProducer));
}

[[nodiscard]] GatewayError Unavailable(const std::string_view transport_error) {
  return RunnerError("program_runner_unavailable",
                     "independent Program runner is unavailable",
                     transport_error);
}

}  // namespace

// ---------------------------------------------------------------------------
// ProgramCommandServer
// ---------------------------------------------------------------------------

struct ProgramCommandServer::State {
  struct Pending {
    std::uint64_t token;
    Fd connection;
    std::string bytes;
  };

  explicit State(std::filesystem::path path)
      : root(std::move(path)), stop(NewEventFd()) {}

  /// A connection whose request is still arriving.
  struct Incoming {
    Fd connection;
    std::string bytes;
    bool over{false};
    bool started{false};
  };

  /// One poll serves the listening socket and every connection whose
  /// request is still arriving, so a slow or silent client holds only its
  /// own place in the set.
  void Listen(const std::stop_token &token) {
    std::list<Incoming> incoming;
    std::vector<pollfd> ready;
    while (!token.stop_requested()) {
      ready.assign({pollfd{socket->get(), POLLIN, 0}, pollfd{stop.get(), POLLIN, 0}});
      for (const auto &client : incoming)
        ready.push_back(pollfd{client.connection.get(), POLLIN, 0});
      const int count = ::poll(ready.data(), ready.size(), -1);
      if (count < 0 && errno == EINTR)
        continue;
      if (count < 0 || (ready[1].revents & POLLIN) != 0 ||
          (ready[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        return;
      std::size_t index = 2U;
      for (auto client = incoming.begin(); client != incoming.end(); ++index)
        client = ready[index].revents != 0 && Receive(*client)
                     ? incoming.erase(client)
                     : std::next(client);
      if ((ready[0].revents & POLLIN) != 0)
        Accept(incoming);
    }
  }

  /// Accepts every waiting connection; one past the bound of arriving
  /// requests is refused at once.
  void Accept(std::list<Incoming> &incoming) {
    for (;;) {
      Fd client(::accept4(socket->get(), nullptr, nullptr,
                          SOCK_CLOEXEC | SOCK_NONBLOCK));
      if (client.get() < 0) {
        if (errno == EINTR || errno == ECONNABORTED)
          continue;
        return;  // EAGAIN: none left (or a transient accept failure).
      }
      if (!private_stream::PeerIsSameUser(client.get()))
        continue;
      if (incoming.size() >= kMaximumQueuedProgramCommands) {
        Busy(client);
        continue;
      }
      incoming.push_back(Incoming{std::move(client), {}, false, false});
    }
  }

  /// Reads the packets `client` has sent so far (never waits). True when the
  /// connection leaves the set: its request was queued or answered, or it
  /// ended or failed (dropped).
  bool Receive(Incoming &client) {
    for (;;) {
      const auto read = ReadPacket(client.connection.get(), -1, client.bytes,
                                   kMaximumProgramCommandRequestBytes,
                                   client.over, Clock::now());
      if (read == Read::kMore) {
        client.started = true;
        continue;
      }
      if (read == Read::kTimedOut)
        return false;
      if (read == Read::kLast)
        Complete(client);
      return true;
    }
  }

  /// A whole request: queued for `Next`, or answered with its refusal.
  void Complete(Incoming &client) {
    if (client.over) {
      static_cast<void>(SendMessage(
          client.connection.get(), stop.get(),
          ErrorReply("program_runner_request_body_oversized",
                     "Program-runner request body exceeds its byte bound"),
          kMaximumProgramCommandReplyBytes, std::nullopt));
      return;
    }
    bool full = false;
    {
      const std::lock_guard lock(mutex);
      if (closed)
        return;
      full = queue.size() >= kMaximumQueuedProgramCommands;
      if (!full)
        queue.push_back(Pending{next_token++, std::move(client.connection),
                                std::move(client.bytes)});
    }
    if (full) {
      Busy(client.connection);
      return;
    }
    ready_cv.notify_one();
  }

  void Busy(const Fd &client) {
    static_cast<void>(SendMessage(
        client.get(), stop.get(),
        ErrorReply("execution_control_ingress_busy",
                   "the Program runner's command intake is at its limit"),
        kMaximumProgramCommandReplyBytes, std::nullopt));
  }

  std::filesystem::path root;
  Fd stop;
  std::unique_ptr<BoundSocket> socket;
  std::jthread listener;
  std::mutex mutex;
  std::condition_variable ready_cv;
  std::deque<Pending> queue;
  std::unordered_map<std::uint64_t, Fd> taken;
  std::uint64_t next_token{1};
  bool started{false};
  bool closed{false};
};

ProgramCommandServer::ProgramCommandServer(std::filesystem::path socket_root)
    : state_(std::make_shared<State>(std::move(socket_root))) {}

ProgramCommandServer::~ProgramCommandServer() { Close(); }

void ProgramCommandServer::Start() {
  {
    const std::lock_guard lock(state_->mutex);
    if (state_->started || state_->closed)
      throw std::runtime_error("the Program command server starts once");
    state_->started = true;
  }
  state_->socket =
      std::make_unique<BoundSocket>(state_->root, kProgramCommandSocketName);
  state_->listener = std::jthread(
      [state = state_](const std::stop_token token) { state->Listen(token); });
}

void ProgramCommandServer::Close() {
  {
    const std::lock_guard lock(state_->mutex);
    if (state_->closed)
      return;
    state_->closed = true;
  }
  state_->ready_cv.notify_all();
  Signal(state_->stop.get());
  if (state_->listener.joinable()) {
    state_->listener.request_stop();
    state_->listener.join();
  }
  std::deque<State::Pending> queued;
  std::unordered_map<std::uint64_t, Fd> taken;
  {
    const std::lock_guard lock(state_->mutex);
    queued.swap(state_->queue);
    taken.swap(state_->taken);
  }
  // The connections close here, outside the lock; their clients see the
  // runner gone.
  queued.clear();
  taken.clear();
  state_->socket.reset();
}

std::optional<ProgramCommandServer::Request>
ProgramCommandServer::Next(const std::chrono::milliseconds timeout) {
  std::unique_lock lock(state_->mutex);
  state_->ready_cv.wait_for(lock, timeout, [this] {
    return state_->closed || !state_->queue.empty();
  });
  if (state_->closed || state_->queue.empty())
    return std::nullopt;
  auto pending = std::move(state_->queue.front());
  state_->queue.pop_front();
  state_->taken.emplace(pending.token, std::move(pending.connection));
  return Request{pending.token, std::move(pending.bytes)};
}

void ProgramCommandServer::Reply(const std::uint64_t token,
                                 const std::string_view reply) {
  std::unordered_map<std::uint64_t, Fd>::node_type connection;
  {
    const std::lock_guard lock(state_->mutex);
    if (state_->closed)
      return;
    connection = state_->taken.extract(token);
  }
  if (connection.empty())
    throw std::invalid_argument("the Program command token is not awaiting a reply");
  // A client that went away ends the send; the reply is then dropped, as an
  // HTTP response to a closed connection was.
  static_cast<void>(SendMessage(connection.mapped().get(), state_->stop.get(),
                                reply, kMaximumProgramCommandReplyBytes,
                                std::nullopt));
}

// ---------------------------------------------------------------------------
// ProgramStatusPublisher
// ---------------------------------------------------------------------------

struct ProgramStatusPublisher::State {
  struct Withdrawn {
    std::string code;
    std::string message;
  };
  struct Newest {
    std::uint64_t generation{};
    std::uint64_t critical_generation{};
    std::shared_ptr<const std::string> cycle;
    std::shared_ptr<const Withdrawn> withdrawn;
  };

  explicit State(std::filesystem::path path) : root(std::move(path)) {}

  /// Wakes every waiting writer. Takes the lock only to copy the writers'
  /// descriptors; each stays open while its copy is held.
  void WakeWriters() {
    std::vector<std::shared_ptr<const Fd>> wakes;
    {
      const std::lock_guard lock(mutex);
      wakes = writer_wakes;
    }
    for (const auto &wake : wakes)
      Signal(wake->get());
  }

  void RequestRead() {
    {
      const std::lock_guard lock(mutex);
      read_requested = true;
    }
    read_cv.notify_all();
  }

  /// Serves one subscriber on its own writer thread. Holds the lock only to
  /// copy the newest value; never while it sends or waits. While waiting it
  /// also watches the connection, so a subscriber that went away frees its
  /// place at once, also when nothing is published.
  void Serve(const int client, const std::stop_token &stop) noexcept {
    try {
      const auto capacity = detail::AtomicResponseCapacity(
          client, kMaximumProgramStatusMessageBytes);
      if (capacity == 0U)
        return;
      const auto wake = std::make_shared<const Fd>(NewEventFd());
      std::uint64_t sent = 0U;
      bool read_wanted = false;
      // Registered for this writer's life.
      struct Registration {
        State &state;
        std::shared_ptr<const Fd> fd;
        Registration(State &owner, std::shared_ptr<const Fd> descriptor,
                     std::uint64_t &sent, bool &read_wanted)
            : state(owner), fd(std::move(descriptor)) {
          const std::lock_guard lock(state.mutex);
          state.writer_wakes.push_back(fd);
          if (state.newest.withdrawn != nullptr) {
            // Not sent again: this subscriber waits for a fresh read of
            // the state, as each new connection read it before.
            sent = state.newest.generation;
            read_wanted = true;
          }
        }
        ~Registration() {
          const std::lock_guard lock(state.mutex);
          std::erase(state.writer_wakes, fd);
        }
        Registration(const Registration &) = delete;
        Registration &operator=(const Registration &) = delete;
      } const registration(*this, wake, sent, read_wanted);
      if (read_wanted)
        RequestRead();
      const std::stop_callback wake_on_stop(stop, [&wake] { Signal(wake->get()); });
      std::uint64_t sequence = 0U;
      for (;;) {
        Newest value;
        bool ready = false;
        {
          const std::lock_guard lock(mutex);
          if (closed || stop.stop_requested()) {
            break;
          }
          ready = newest.generation > sent;
          if (ready)
            value = newest;
        }
        if (!ready) {
          // The subscriber sends nothing: input or a hangup ends it.
          pollfd events[2]{{client, POLLIN | POLLRDHUP, 0}, {wake->get(), POLLIN, 0}};
          const int count = ::poll(events, 2, -1);
          if (count < 0 && errno != EINTR)
            break;
          if (events[0].revents != 0)
            break;
          if ((events[1].revents & POLLIN) != 0) {
            std::uint64_t drained = 0U;
            // Fails only when already drained (EAGAIN): nothing to do.
            [[maybe_unused]] const auto read =
                ::read(wake->get(), &drained, sizeof(drained));
          }
          continue;
        }
        if (value.withdrawn != nullptr) {
          static_cast<void>(private_stream::SendFrame(
              client,
              private_stream::ErrorFrame(kProgramStatusStreamApiVersion,
                                         value.withdrawn->code,
                                         value.withdrawn->message),
              capacity, kStatusSendDeadline, stop));
          break;
        }
        const bool critical = sequence == 0U || value.critical_generation > sent;
        ++sequence;
        std::string message = "{\"api_version\":";
        message += JsonText(kProgramStatusStreamApiVersion);
        message += ",\"frame\":{\"event_class\":";
        message += critical ? "\"critical\"" : "\"node_progress\"";
        message += ",\"program_cycle\":";
        message += *value.cycle;
        message += ",\"schema_version\":";
        message += JsonText(kProgramControlStateSchema);
        message += ",\"server_sequence\":\"";
        message += std::to_string(sequence);
        message += "\"},\"type\":\"frame\"}";
        if (message.size() > kMaximumProgramStatusMessageBytes) {
          static_cast<void>(private_stream::SendFrame(
              client,
              private_stream::ErrorFrame(
                  kProgramStatusStreamApiVersion,
                  "program_runner_response_oversized",
                  "the Program status frame exceeds its byte bound"),
              capacity, kStatusSendDeadline, stop));
          break;
        }
        if (!SendStatusMessage(client, message, capacity, stop))
          break;
        sent = value.generation;
      }
    } catch (...) {
      // Composing a frame fails only on memory; the subscription ends and
      // the reader reconnects.
    }
  }

  std::filesystem::path root;
  std::unique_ptr<BoundSocket> socket;
  std::jthread acceptor;
  std::mutex mutex;
  /// The writers' wake descriptors.
  std::vector<std::shared_ptr<const Fd>> writer_wakes;
  Newest newest;
  /// The status pump's read request (RequestRead / WaitReadRequest).
  std::condition_variable read_cv;
  bool read_requested{false};
  bool started{false};
  bool closed{false};
};

ProgramStatusPublisher::ProgramStatusPublisher(std::filesystem::path socket_root)
    : state_(std::make_shared<State>(std::move(socket_root))) {}

ProgramStatusPublisher::~ProgramStatusPublisher() { Close(); }

void ProgramStatusPublisher::Start() {
  {
    const std::lock_guard lock(state_->mutex);
    if (state_->started || state_->closed)
      throw std::runtime_error("the Program status publisher starts once");
    state_->started = true;
  }
  state_->socket =
      std::make_unique<BoundSocket>(state_->root, kProgramStatusSocketName);
  state_->acceptor = std::jthread([state = state_](const std::stop_token stop) {
    private_stream::AcceptSubscribers(
        state->socket->get(),
        private_stream::SubscriberPoolOptions{
            kMaximumProgramStatusSubscribers,
            std::string(kProgramStatusStreamApiVersion),
            "program_runner_unavailable",
            "the Program runner serves its bounded number of status "
            "subscriptions",
            kMaximumProgramStatusMessageBytes, kStatusSendDeadline},
        [state](const int client, const std::stop_token writer_stop) {
          state->Serve(client, writer_stop);
        },
        stop);
  });
}

void ProgramStatusPublisher::Close() {
  {
    const std::lock_guard lock(state_->mutex);
    if (state_->closed)
      return;
    state_->closed = true;
  }
  state_->read_cv.notify_all();
  state_->WakeWriters();
  if (state_->acceptor.joinable()) {
    // Stops the acceptor; it ends every writer before it returns.
    state_->acceptor.request_stop();
    state_->acceptor.join();
  }
  state_->socket.reset();
}

void ProgramStatusPublisher::Publish(std::string program_cycle,
                                     const bool critical) {
  auto cycle = std::make_shared<const std::string>(std::move(program_cycle));
  {
    const std::lock_guard lock(state_->mutex);
    auto &newest = state_->newest;
    ++newest.generation;
    if (critical)
      newest.critical_generation = newest.generation;
    newest.cycle = std::move(cycle);
    newest.withdrawn.reset();
  }
  state_->WakeWriters();
}

void ProgramStatusPublisher::Withdraw(const std::string_view code,
                                      const std::string_view message) {
  auto withdrawn = std::make_shared<const State::Withdrawn>(
      State::Withdrawn{std::string(code), std::string(message)});
  {
    const std::lock_guard lock(state_->mutex);
    auto &newest = state_->newest;
    ++newest.generation;
    newest.cycle.reset();
    newest.withdrawn = std::move(withdrawn);
  }
  state_->WakeWriters();
}

void ProgramStatusPublisher::RequestRead() { state_->RequestRead(); }

bool ProgramStatusPublisher::WaitReadRequest(
    const std::optional<std::chrono::milliseconds> timeout) {
  std::unique_lock lock(state_->mutex);
  const auto ready = [this] {
    return state_->closed || state_->read_requested;
  };
  if (timeout.has_value())
    state_->read_cv.wait_for(lock, *timeout, ready);
  else
    state_->read_cv.wait(lock, ready);
  if (state_->closed || !state_->read_requested)
    return false;
  state_->read_requested = false;
  return true;
}

// ---------------------------------------------------------------------------
// ProgramRunnerLink
// ---------------------------------------------------------------------------

ProgramRunnerLink::Subscription::Subscription(const int fd)
    : fd_(fd), wake_(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK)) {
  if (wake_ < 0) {
    ::close(fd_);
    throw std::runtime_error(std::string("eventfd failed: ") +
                             std::strerror(errno));
  }
}

ProgramRunnerLink::Subscription::~Subscription() {
  ::close(fd_);
  ::close(wake_);
}

void ProgramRunnerLink::Subscription::Close() noexcept { Signal(wake_); }

std::optional<std::string>
ProgramRunnerLink::Subscription::Next(const std::chrono::milliseconds timeout) {
  const Deadline deadline = Clock::now() + timeout;
  for (;;) {
    switch (ReadPacket(fd_, wake_, partial_, kMaximumProgramStatusMessageBytes,
                       over_, deadline)) {
      case Read::kMore:
        continue;
      case Read::kLast: {
        if (over_)
          throw RunnerError("program_runner_response_oversized",
                            "Program-runner response exceeds its byte bound");
        std::string message;
        message.swap(partial_);
        return message;
      }
      case Read::kTimedOut:
        return std::nullopt;
      case Read::kEnd:
      case Read::kTruncated:
        throw Unavailable("closed_by_runner");
      case Read::kStopped:
        throw Unavailable("subscription_closed");
      case Read::kFailed:
        break;
    }
    throw Unavailable("receive_failed");
  }
}

ProgramRunnerLink::ProgramRunnerLink(std::filesystem::path socket_root,
                                     const std::chrono::milliseconds connect_limit)
    : root_(std::make_unique<PinnedRunRoot>(std::move(socket_root),
                                            "program_runner_unavailable")),
      connect_limit_(connect_limit) {}

ProgramRunnerLink::~ProgramRunnerLink() = default;

std::string ProgramRunnerLink::Call(
    const std::string_view request,
    const std::optional<std::chrono::milliseconds> limit) const {
  const Deadline deadline =
      limit.has_value() ? Deadline(Clock::now() + *limit) : std::nullopt;
  int raw = -1;
  try {
    raw = root_->Connect(kProgramCommandSocketName,
                         limit.has_value() ? std::min(connect_limit_, *limit)
                                           : connect_limit_);
  } catch (const std::exception &) {
    throw Unavailable("connect_failed");
  }
  const Fd connection(raw);
  // A send the runner ended early (it answered a refusal and closed) still
  // leaves its reply to read.
  if (SendMessage(connection.get(), -1, request,
                  kMaximumProgramCommandRequestBytes,
                  deadline) == Sent::kTimedOut)
    throw Unavailable("write_timeout");
  std::string reply;
  bool over = false;
  switch (ReadMessage(connection.get(), -1, reply,
                      kMaximumProgramCommandReplyBytes, over, deadline)) {
    case Read::kLast:
      break;
    case Read::kEnd:
      throw Unavailable("closed_without_reply");
    case Read::kTruncated:
      // As an HTTP response cut short: the runner is unavailable.
      throw Unavailable("closed_mid_reply");
    case Read::kTimedOut:
      // The former HTTP read timeout of a non-command read.
      throw Unavailable("read_timeout");
    case Read::kMore:
    case Read::kStopped:
    case Read::kFailed:
      throw Unavailable("receive_failed");
  }
  if (over)
    throw RunnerError("program_runner_response_oversized",
                      "Program-runner response exceeds its byte bound");
  return reply;
}

std::unique_ptr<ProgramRunnerLink::Subscription>
ProgramRunnerLink::SubscribeStatus() const {
  int raw = -1;
  try {
    raw = root_->Connect(kProgramStatusSocketName, connect_limit_);
  } catch (const std::exception &) {
    throw Unavailable("connect_failed");
  }
  return std::make_unique<Subscription>(raw);
}

}  // namespace manyforge::headless::gateway
