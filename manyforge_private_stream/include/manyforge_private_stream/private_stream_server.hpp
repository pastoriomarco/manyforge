#pragma once

// Server side shared by ManyForge's private SEQPACKET streams (the execution
// host's event stream; the Manager's status stream).
//
// One frame per packet, RFC 8785 canonical JSON. Each admitted subscriber is
// served by its own writer thread, which holds nothing but its own
// connection: a slow or dead reader only holds its own writer. A send that
// finds no room waits for `POLLOUT` in 100 ms slices (so a stop is seen) up to
// the caller's deadline, then gives up; the caller ends that subscription.
//
// This is transport, not core logic: deadlines are kernel `poll()` timeouts on
// the real steady clock. What each stream accepts, reads and writes (its
// socket, its subscribe packet, its writer body, its codes and version) stays
// with that stream; this header holds only the parts both share.

#include <manyforge_content_identity/canonical_value.hpp>

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace manyforge::private_stream {

/// Owns one descriptor; closes it on destruction. Move-only.
class Fd final {
public:
  explicit Fd(const int value = -1) noexcept : value_(value) {}
  ~Fd() {
    if (value_ >= 0)
      ::close(value_);
  }
  Fd(Fd &&other) noexcept : value_(std::exchange(other.value_, -1)) {}
  Fd(const Fd &) = delete;
  Fd &operator=(const Fd &) = delete;
  Fd &operator=(Fd &&) = delete;
  [[nodiscard]] int get() const noexcept { return value_; }

private:
  int value_;
};

/// Sends `bytes` (one already encoded frame) as one packet, or returns false:
/// the frame is empty or above `max_bytes`, the peer is gone, `stop` was
/// requested, or the socket had no room for `send_deadline`.
[[nodiscard]] inline bool
SendFrameBytes(const int fd, const std::string_view bytes,
               const std::size_t max_bytes,
               const std::chrono::milliseconds send_deadline,
               const std::stop_token stop) noexcept {
  if (bytes.empty() || bytes.size() > max_bytes)
    return false;
  const auto deadline = std::chrono::steady_clock::now() + send_deadline;
  for (;;) {
    const auto sent = ::send(fd, bytes.data(), bytes.size(),
                             MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent == static_cast<ssize_t>(bytes.size()))
      return true;
    if (sent >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      return false;
    if (stop.stop_requested() || std::chrono::steady_clock::now() >= deadline)
      return false;
    // A full socket buffer: wait for room in short slices so a stop is
    // seen promptly; nothing else waits on this writer.
    pollfd outbound{fd, POLLOUT, 0};
    static_cast<void>(::poll(&outbound, 1, 100));
  }
}

/// Sends `message` as one packet, or returns false: the encoded frame is
/// empty or above `max_bytes`, the peer is gone, `stop` was requested, or
/// the socket had no room for `send_deadline`.
[[nodiscard]] inline bool
SendFrame(const int fd, const content_identity::CanonicalValue &message,
          const std::size_t max_bytes,
          const std::chrono::milliseconds send_deadline,
          const std::stop_token stop) noexcept {
  try {
    return SendFrameBytes(fd, content_identity::canonical_json(message),
                          max_bytes, send_deadline, stop);
  } catch (...) {
    return false;
  }
}

/// The `error` frame of a stream: `{api_version, error: {code, details: {},
/// message}, type: "error"}`, the message cut to 1024 characters. The
/// subscription ends after it.
[[nodiscard]] inline content_identity::CanonicalValue
ErrorFrame(const std::string_view api_version, const std::string_view code,
           const std::string_view message) {
  using Value = content_identity::CanonicalValue;
  return Value(Value::Object{
      {"api_version", Value(std::string(api_version))},
      {"error", Value(Value::Object{
                    {"code", Value(std::string(code))},
                    {"details", Value(Value::Object{})},
                    {"message", Value(std::string(message.substr(0U, 1024U)))}})},
      {"type", Value("error")}});
}

/// True when the connected peer runs as this process's effective user.
[[nodiscard]] inline bool PeerIsSameUser(const int fd) noexcept {
  ucred peer{};
  socklen_t peer_size = sizeof(peer);
  return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) == 0 &&
         peer_size == sizeof(peer) && peer.uid == ::geteuid();
}

/// The bounds and texts of one stream's subscriber pool.
struct SubscriberPoolOptions final {
  /// Concurrent subscribers; one more is refused with the "full" error.
  std::size_t max_subscribers{};
  /// The stream's frame version, carried by the "full" error frame.
  std::string api_version;
  std::string full_code;
  std::string full_message;
  /// The bound and send deadline used for the "full" error frame.
  std::size_t max_frame_bytes{};
  std::chrono::milliseconds send_deadline{};
};

/// The subscribers of one stream: at most `max_subscribers` writer threads,
/// one per connection. `Admit` and the destructor are called by one thread
/// only (the stream's acceptor or adopter); thread creation and the reaping
/// of finished writers happen there.
class SubscriberPool final {
public:
  /// Serves one subscriber until it fails, disconnects or `stop`; must not
  /// throw. The connection closes when it returns.
  using Serve = std::function<void(int client, std::stop_token stop)>;

  explicit SubscriberPool(SubscriberPoolOptions options)
      : options_(std::move(options)) {}

  SubscriberPool(const SubscriberPool &) = delete;
  SubscriberPool &operator=(const SubscriberPool &) = delete;

  /// Stops every writer (each sees it within one send slice or at once in
  /// its wait) and joins it; each connection closes with its writer.
  ~SubscriberPool() { writers_.clear(); }

  /// Reaps finished writers, then either starts `serve` on its own thread
  /// for `client` or, at the cap, sends the "full" error (`stop` ends that
  /// send early) and closes `client`.
  void Admit(Fd client, Serve serve, const std::stop_token stop) {
    writers_.remove_if([](const Writer &writer) { return writer.done->load(); });
    if (writers_.size() >= options_.max_subscribers) {
      static_cast<void>(SendFrame(
          client.get(),
          ErrorFrame(options_.api_version, options_.full_code,
                     options_.full_message),
          options_.max_frame_bytes, options_.send_deadline, stop));
      return;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    writers_.push_back(Writer{
        done,
        std::jthread([done, serve = std::move(serve),
                      fd = std::make_shared<Fd>(std::move(client))](
                         const std::stop_token writer_stop) {
          serve(fd->get(), writer_stop);
          done->store(true);
        })});
  }

private:
  struct Writer final {
    std::shared_ptr<std::atomic<bool>> done;
    std::jthread thread;
  };

  SubscriberPoolOptions options_;
  std::list<Writer> writers_;
};

/// Accepts subscribers on the listening `socket` (its own thread, the
/// stream's acceptor) until `stop` or a socket failure, admitting each peer
/// of this process's user into one pool; then ends every subscription (each
/// writer sees the stop within one send slice) and returns once they are
/// closed. `serve` runs on each subscriber's writer thread.
inline void AcceptSubscribers(const int socket, SubscriberPoolOptions options,
                              const SubscriberPool::Serve &serve,
                              const std::stop_token stop) noexcept {
  Fd wake(::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK));
  if (wake.get() < 0)
    return;
  const std::stop_callback wake_on_stop(stop, [&wake] {
    const std::uint64_t one = 1U;
    // An eventfd write only fails when the counter is already non-zero
    // (EAGAIN): the poll loop is woken either way.
    [[maybe_unused]] const auto written =
        ::write(wake.get(), &one, sizeof(one));
  });
  // Declared after the stop wake: destroyed first, it stops and joins
  // every writer; each socket closes with its writer.
  SubscriberPool writers(std::move(options));
  while (!stop.stop_requested()) {
    pollfd ready[2]{{socket, POLLIN, 0}, {wake.get(), POLLIN, 0}};
    const int count = ::poll(ready, 2, -1);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0 || stop.stop_requested() ||
        (ready[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
      break;
    if ((ready[0].revents & POLLIN) == 0)
      continue;
    Fd client(::accept4(socket, nullptr, nullptr,
                        SOCK_CLOEXEC | SOCK_NONBLOCK));
    if (client.get() < 0 || !PeerIsSameUser(client.get()))
      continue;
    writers.Admit(std::move(client), serve, stop);
  }
}

} // namespace manyforge::private_stream
