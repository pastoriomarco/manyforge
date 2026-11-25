#pragma once

// The Program runner's command and status sockets (OD-380 D5), served by the
// core inside the runner and used by Headless through the same core.
//
// Both sockets live in one private socket root (the Manager run root), are
// same-user SEQPACKET sockets of mode 0600 and carry UTF-8 JSON text. The
// JSON members are written and read by the Python side (the runner's and
// Headless's own encoders, so numbers and strings keep today's exact form);
// the core moves bytes, bounds them and composes only its own small frames.
//
// Every message on both sockets is one or more packets of at most a quarter
// of the sending connection's atomic send size (so a poll that reports room
// always has room for one). Every packet of a message but its last starts
// with a NUL byte, which is not part of the message; the last packet is the
// message's remaining bytes as they are. JSON text never holds a NUL byte,
// so a message that fits one packet is that packet unchanged.
//
// program-command.sock: one request per connection.
//   client -> runner  {"api_version": "manyforge.program_command.v1",
//                      "command": <name>, "request": {...}}
//   runner -> client  {"api_version": "manyforge.program_command_response.v1",
//                      "ok": true, "value": ...}  or
//                     {..., "ok": false, "error": {code, message, details,
//                      producer}}
//   The request is at most 1 MiB, the reply at most 32 MiB. A command's
//   request has no time limit: the reply comes at the command's acceptance
//   (OD-214); a runner exit ends the wait, and a reply the runner's end cut
//   short is the runner unavailable. A read may be given a whole-request
//   limit (Headless's former HTTP limit of its non-command reads).
//
// program-status.sock: a kept subscription,
//   {"api_version": "manyforge.program_status_stream.v1", "type": "frame",
//    "frame": {"event_class", "program_cycle", "schema_version":
//    "manyforge.program_control_state.v4", "server_sequence"}}
//   or {"api_version", "type": "error", "error": {code, details, message}},
//   after which the subscription ends. The first frame (the newest published
//   cycle) is sent at once; then newest wins; `server_sequence` counts this
//   connection's frames from "1"; a frame is `critical` when it is the first
//   one or any cycle published since this connection's last frame was. A
//   withdrawn value (the state was unreadable) ends the subscriptions it
//   reaches with its error; a subscriber that connects while the value is
//   withdrawn asks the runner to read the state again and gets that read's
//   outcome (each new connection read the state at once before).
//
// Each socket has one server: a runner holds `.<socket name>.lock` in the
// root (flock) for the socket's life, and a second one is refused with
// GatewayError(program_runner_socket_unavailable).
//
// Threads: the command listener and the status acceptor and writers are C++
// threads; they never take the GIL. The listener serves every connection
// whose request is still arriving in one poll, so a slow or silent client
// delays no other. Python pulls requests with `Next` and
// answers with `Reply`; the runner's status pump waits in
// `WaitReadRequest`. Every lock is a leaf lock held for a memory copy; no
// lock is held across socket I/O or an eventfd write.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace manyforge::headless::gateway {

class PinnedRunRoot;

inline constexpr std::string_view kProgramCommandSocketName =
    "program-command.sock";
inline constexpr std::string_view kProgramStatusSocketName =
    "program-status.sock";
inline constexpr std::string_view kProgramCommandApiVersion =
    "manyforge.program_command.v1";
inline constexpr std::string_view kProgramCommandResponseApiVersion =
    "manyforge.program_command_response.v1";
inline constexpr std::string_view kProgramStatusStreamApiVersion =
    "manyforge.program_status_stream.v1";
inline constexpr std::string_view kProgramControlStateSchema =
    "manyforge.program_control_state.v4";

/// The runner's request bound (its former HTTP body bound).
inline constexpr std::size_t kMaximumProgramCommandRequestBytes = 1024U * 1024U;
/// Headless's reply bound (its former HTTP client bound).
inline constexpr std::size_t kMaximumProgramCommandReplyBytes =
    32U * 1024U * 1024U;
/// The largest status message (its former WebSocket bound, plus the stream
/// envelope).
inline constexpr std::size_t kMaximumProgramStatusMessageBytes =
    16U * 1024U * 1024U + 1024U;
/// Received requests not yet taken by `Next`, and connections whose request
/// is still arriving; one more of either is refused at once.
inline constexpr std::size_t kMaximumQueuedProgramCommands = 64U;
inline constexpr std::size_t kMaximumProgramStatusSubscribers = 4U;

/// Serves program-command.sock in the runner.
class ProgramCommandServer {
 public:
  struct Request {
    std::uint64_t token{};
    std::string bytes;
  };

  explicit ProgramCommandServer(std::filesystem::path socket_root);
  ~ProgramCommandServer();
  ProgramCommandServer(const ProgramCommandServer &) = delete;
  ProgramCommandServer &operator=(const ProgramCommandServer &) = delete;

  /// Takes the socket's lock, binds the socket (a stale socket of this user
  /// is replaced) and starts the listener thread. Throws
  /// GatewayError(program_runner_socket_unavailable) when another runner
  /// serves the socket, the root is unsafe or the socket cannot be bound.
  void Start();
  /// Stops the listener, closes every connection not yet answered (its
  /// client sees the runner gone) and removes the socket. Idempotent.
  void Close();

  /// The next received request, or nullopt after `timeout` or once closed.
  [[nodiscard]] std::optional<Request> Next(std::chrono::milliseconds timeout);
  /// Sends `reply` on the request's connection and closes it. Waits only for
  /// socket room (a stop ends the wait). Throws std::invalid_argument for a
  /// token that is not waiting for its reply.
  void Reply(std::uint64_t token, std::string_view reply);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

/// Serves program-status.sock in the runner.
class ProgramStatusPublisher {
 public:
  explicit ProgramStatusPublisher(std::filesystem::path socket_root);
  ~ProgramStatusPublisher();
  ProgramStatusPublisher(const ProgramStatusPublisher &) = delete;
  ProgramStatusPublisher &operator=(const ProgramStatusPublisher &) = delete;

  /// As ProgramCommandServer::Start.
  void Start();
  /// Ends every subscription and removes the socket. Idempotent.
  void Close();

  /// `program_cycle` is the cycle's JSON text; it becomes the newest value.
  void Publish(std::string program_cycle, bool critical);
  /// The newest value becomes an error: every subscriber connected now gets
  /// the error frame and its subscription ends. A subscriber that connects
  /// while the value is withdrawn makes a read request and waits for the
  /// next Publish or Withdraw.
  void Withdraw(std::string_view code, std::string_view message);

  /// Asks the runner's status pump to read the state (any thread).
  void RequestRead();
  /// Waits for a read request (none: no limit) and takes it. False after
  /// `timeout` or once closed.
  [[nodiscard]] bool WaitReadRequest(
      std::optional<std::chrono::milliseconds> timeout);

 private:
  struct State;
  std::shared_ptr<State> state_;
};

/// Headless's side of both sockets.
class ProgramRunnerLink {
 public:
  /// One status subscription. `Next` and `Close` may be called from
  /// different threads; `Close` ends a waiting `Next`.
  class Subscription {
   public:
    explicit Subscription(int fd);
    ~Subscription();
    Subscription(const Subscription &) = delete;
    Subscription &operator=(const Subscription &) = delete;
    /// The next stream message's bytes, or nullopt after `timeout` (a
    /// message whose packets are still arriving is kept for the next call).
    /// Throws GatewayError(program_runner_unavailable) when the runner closed
    /// the subscription or it was closed here. One caller at a time.
    [[nodiscard]] std::optional<std::string> Next(
        std::chrono::milliseconds timeout);
    void Close() noexcept;

   private:
    int fd_{-1};
    int wake_{-1};
    /// The packets received of the message in progress.
    std::string partial_;
    bool over_{false};
  };

  ProgramRunnerLink(std::filesystem::path socket_root,
                    std::chrono::milliseconds connect_limit);
  ~ProgramRunnerLink();
  ProgramRunnerLink(const ProgramRunnerLink &) = delete;
  ProgramRunnerLink &operator=(const ProgramRunnerLink &) = delete;

  /// Sends one request message and returns the reply's bytes. Without
  /// `limit` only the connect is limited (by the connect limit); with it the
  /// whole request (connect, send, reply) ends within `limit`. Throws
  /// GatewayError (producer `program_runner`): program_runner_unavailable
  /// (no connection, the runner closed it without a whole reply, or the
  /// limit passed: details.transport_error names which),
  /// program_runner_response_oversized.
  [[nodiscard]] std::string Call(
      std::string_view request,
      std::optional<std::chrono::milliseconds> limit = std::nullopt) const;
  [[nodiscard]] std::unique_ptr<Subscription> SubscribeStatus() const;

 private:
  std::unique_ptr<PinnedRunRoot> root_;
  std::chrono::milliseconds connect_limit_;
};

}  // namespace manyforge::headless::gateway
